#!/usr/bin/env python3
"""Trusted, credential-less-to-the-client model inference gateway.

Threat model
------------
Scored benchmark work must never hold provider credentials. This process is the
only place an API key, OAuth token or existing Claude Code session is allowed to
exist. It listens on a Unix domain socket that is shared with the scored sandbox
and answers exactly two request kinds: a health handshake and a model inference
request.

The gateway is a pure inference broker. It deliberately has no notion of tools,
files, commands, repositories or agent platforms, and it rejects any request that
carries a field which could describe one. A compromised or misbehaving sandbox can
therefore obtain model completions and nothing else: no host filesystem, no shell,
no GitHub, no Claude Code tool surface and no provider credential.

Providers are pluggable and provider-agnostic at the protocol level:

  fake                deterministic, offline, scriptable - used by CI
  exec                a trusted local command that maps stdin prompt to stdout text
                      (this is how an existing local agent CLI session is reused)
  anthropic-messages  direct provider HTTP call using gateway-only credentials

Only the provider implementation knows provider specifics. The wire protocol seen
by the sandbox is identical for all of them.
"""

from __future__ import annotations

import argparse
import datetime as dt
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import signal
import socketserver
import subprocess
import sys
import threading
import time
from typing import Any
import urllib.error
import urllib.request

TEMPLATE_DIR = Path(__file__).resolve().parent.parent
CONFIG_RELATIVE = "config/inference_gateway.json"
PROTOCOL = "quidra-inference-gateway-v1"
CANONICAL_SOCKET_PATH = "/quidra-benchmark/gateway/inference.sock"


class GatewayError(RuntimeError):
    """Operator-facing gateway failure (never sent verbatim to the sandbox)."""


class PolicyError(RuntimeError):
    """Request refused because it asked for something outside the broker contract."""


class ProtocolError(RuntimeError):
    """Request was malformed."""


def load_config(template_dir: Path | None = None) -> dict[str, Any]:
    path = (template_dir or TEMPLATE_DIR) / CONFIG_RELATIVE
    config = json.loads(path.read_text(encoding="utf-8"))
    if config.get("schema_version") != 1:
        raise GatewayError(f"unsupported inference gateway config schema: {path}")
    if config.get("protocol") != PROTOCOL:
        raise GatewayError(f"unsupported inference gateway protocol: {path}")
    return config


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


# --------------------------------------------------------------------------
# Response hygiene
# --------------------------------------------------------------------------

_HOST_PATH_PATTERNS = (
    re.compile(r"(?<![A-Za-z0-9_])/(?:Users|home)/[^/\s\"']+"),
    re.compile(r"(?<![A-Za-z0-9:])/(?:private/var/folders|var/folders)/[^\s\"']+"),
)
_SECRET_PATTERNS = (
    re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
    re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})\b"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
)


def scrub_outbound(text: str, extra_secrets: list[str]) -> str:
    """Strip host paths and credential-shaped text from anything sent to the sandbox.

    The gateway sits on the credential side of the boundary. Provider errors and
    completions occasionally echo a host path or a key fragment; the sandbox must
    never receive either, so redaction happens here rather than being trusted to
    the provider.
    """
    for secret in extra_secrets:
        if secret and len(secret) >= 8:
            text = text.replace(secret, "[redacted-credential]")
    for pattern in _SECRET_PATTERNS:
        text = pattern.sub("[redacted-credential]", text)
    for pattern in _HOST_PATH_PATTERNS:
        text = pattern.sub("[redacted-host-path]", text)
    return text


# --------------------------------------------------------------------------
# Providers
# --------------------------------------------------------------------------


class Provider:
    """Provider-agnostic inference contract.

    `complete` receives an already-validated, tool-free request and returns
    plain assistant text plus optional usage. It must never be given, and never
    asks for, anything the sandbox supplied beyond messages and sampling limits.
    """

    id = "abstract"

    def describe(self) -> dict[str, Any]:
        return {"id": self.id}

    def secrets(self) -> list[str]:
        return []

    def complete(self, request: dict[str, Any]) -> dict[str, Any]:
        raise NotImplementedError


class FakeProvider(Provider):
    """Deterministic offline provider for CI and for protocol conformance tests.

    Without a script it answers with a stable digest of the conversation, which is
    enough to prove that a credential-less sandbox really reached a model broker.
    With a script it returns canned completions selected by substring match, which
    lets CI drive complete packet-only and sandbox-agent runs with no provider
    account and no network.
    """

    id = "fake"

    def __init__(self, script_path: Path | None = None) -> None:
        self.script_path = script_path
        self.script: dict[str, Any] = {}
        self._script_source: str | None = None
        self._calls = 0
        self._task_calls: dict[str, int] = {}
        self._lock = threading.Lock()
        self._reload()

    def _reload(self) -> None:
        """Re-read the script when it changes on disk.

        A benchmark run dispatches many different work units against one
        long-lived gateway. Reloading lets the harness script the next unit
        without restarting the gateway and losing the socket.
        """
        if self.script_path is None:
            return
        try:
            raw = self.script_path.read_text(encoding="utf-8")
        except OSError as exc:
            raise GatewayError(f"fake provider script is unreadable: {exc}") from exc
        # Re-read rather than stat: a harness often rewrites the script between
        # two dispatches in the same second, which a timestamp comparison misses.
        if raw == self._script_source:
            return
        script = json.loads(raw)
        if script.get("schema_version") != 1:
            raise GatewayError(f"unsupported fake provider script schema: {self.script_path}")
        self.script = script
        self._script_source = raw

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "model": self.script.get("model", "fake-deterministic-v1"),
            "scripted": self.script_path is not None,
            "offline": True,
        }

    def complete(self, request: dict[str, Any]) -> dict[str, Any]:
        task_id = request.get("task_id") or ""
        with self._lock:
            self._reload()
            self._calls += 1
            call_index = self._calls
            task_index = self._task_calls.get(task_id, 0) + 1
            self._task_calls[task_id] = task_index
        messages = request["messages"]
        last_user = ""
        for message in reversed(messages):
            if message["role"] == "user":
                last_user = message["content"]
                break
        transcript = "\n".join(f"{m['role']}:{m['content']}" for m in messages)

        # Per-task turn scripts come first: they let one gateway drive many
        # independent work units deterministically, each with its own turn counter.
        turns = (self.script.get("tasks") or {}).get(task_id)
        if turns and task_index <= len(turns):
            return {
                "content": str(turns[task_index - 1]),
                "stop_reason": "end_turn",
                "usage": {"input_tokens": len(transcript) // 4, "output_tokens": 0},
            }

        for rule in self.script.get("rules", []):
            needle = rule.get("contains")
            if needle and needle in last_user:
                return {
                    "content": str(rule["content"]),
                    "stop_reason": "end_turn",
                    "usage": {"input_tokens": len(transcript) // 4, "output_tokens": 0},
                }

        sequence = self.script.get("sequence")
        if sequence and call_index <= len(sequence):
            return {
                "content": str(sequence[call_index - 1]),
                "stop_reason": "end_turn",
                "usage": {"input_tokens": len(transcript) // 4, "output_tokens": 0},
            }

        default = self.script.get("default")
        if default is not None:
            return {
                "content": str(default),
                "stop_reason": "end_turn",
                "usage": {"input_tokens": len(transcript) // 4, "output_tokens": 0},
            }

        digest = sha256_text(transcript)
        return {
            "content": (
                "fake-provider deterministic completion\n"
                f"call_index={call_index}\n"
                f"conversation_sha256={digest}\n"
            ),
            "stop_reason": "end_turn",
            "usage": {"input_tokens": len(transcript) // 4, "output_tokens": 16},
        }


class ExecProvider(Provider):
    """Delegate inference to a trusted local command on the credential side.

    This is how an existing authenticated local agent CLI is reused without ever
    exposing its session to scored work: the command runs here, in the trusted
    gateway process, receives only the rendered conversation on stdin, and returns
    assistant text on stdout. The sandbox never sees the command, its arguments or
    its environment.
    """

    id = "exec"

    def __init__(self, argv: list[str], timeout: int, model: str | None = None) -> None:
        if not argv:
            raise GatewayError("exec provider requires a command")
        self.argv = argv
        self.timeout = timeout
        self.model = model or argv[0]

    def describe(self) -> dict[str, Any]:
        return {"id": self.id, "model": self.model, "offline": False}

    def complete(self, request: dict[str, Any]) -> dict[str, Any]:
        prompt = render_conversation(request["messages"])
        try:
            completed = subprocess.run(
                self.argv,
                shell=False,
                input=prompt,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=self.timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise GatewayError(f"exec provider timed out after {self.timeout}s") from exc
        except OSError as exc:
            raise GatewayError(f"exec provider could not start: {exc}") from exc
        if completed.returncode != 0:
            raise GatewayError(
                f"exec provider exited {completed.returncode}: "
                f"{(completed.stderr or completed.stdout).strip()[:2000]}"
            )
        return {
            "content": completed.stdout,
            "stop_reason": "end_turn",
            "usage": {"input_tokens": len(prompt) // 4, "output_tokens": len(completed.stdout) // 4},
        }


class AnthropicMessagesProvider(Provider):
    """Direct Anthropic Messages adapter with a gateway-side soft spend guard.

    The credential, pricing policy and optional server-side web search all live on
    the trusted side. The sandbox can only request the frozen network_allowed bit;
    it cannot supply tools, endpoints, credentials or provider options.
    """

    id = "anthropic-messages"

    def __init__(
        self,
        model: str,
        timeout: int,
        *,
        budget_usd: float | None = None,
        pricing: dict[str, Any] | None = None,
        web_search: dict[str, Any] | None = None,
        base_url: str | None = None,
    ) -> None:
        api_key = os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN")
        if not api_key:
            raise GatewayError(
                "anthropic-messages provider requires ANTHROPIC_API_KEY in the trusted "
                "gateway environment; it must never be present in the scored sandbox"
            )
        if budget_usd is not None and budget_usd <= 0:
            raise GatewayError("--budget-usd must be positive")
        if budget_usd is not None and not pricing:
            raise GatewayError(
                f"no frozen pricing is configured for {model!r}; refusing to run with a spend guard"
            )
        self._api_key = api_key
        self.model = model
        self.timeout = timeout
        self.base_url = (base_url or os.environ.get("ANTHROPIC_BASE_URL")
                         or "https://api.anthropic.com").rstrip("/")
        self.budget_usd = float(budget_usd) if budget_usd is not None else None
        self.pricing = dict(pricing or {})
        self.web_search = dict(web_search or {})
        self._estimated_cost_usd = 0.0
        self._lock = threading.Lock()

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "model": self.model,
            "offline": False,
            "soft_budget_usd": self.budget_usd,
            "estimated_cost_usd": round(self._estimated_cost_usd, 6),
        }

    def secrets(self) -> list[str]:
        return [self._api_key]

    def _request_cost(self, usage: dict[str, Any]) -> float:
        if not self.pricing:
            return 0.0
        return (
            float(usage.get("input_tokens", 0) or 0)
            * float(self.pricing["input_usd_per_million_tokens"])
            / 1_000_000.0
            + float(usage.get("output_tokens", 0) or 0)
            * float(self.pricing["output_usd_per_million_tokens"])
            / 1_000_000.0
            + float(usage.get("web_search_requests", 0) or 0)
            * float(self.pricing.get("web_search_usd_per_request", 0.0))
        )

    def _open(self, http_request: urllib.request.Request) -> dict[str, Any]:
        for attempt in range(6):
            try:
                with urllib.request.urlopen(http_request, timeout=self.timeout) as response:
                    return json.loads(response.read().decode("utf-8"))
            except urllib.error.HTTPError as exc:
                detail = exc.read().decode("utf-8", "replace")[:2000]
                if exc.code not in {429, 500, 502, 503, 529} or attempt == 5:
                    raise GatewayError(f"provider HTTP {exc.code}: {detail}") from exc
                retry_after = exc.headers.get("retry-after")
                try:
                    delay = float(retry_after) if retry_after else min(2 ** attempt, 30)
                except ValueError:
                    delay = min(2 ** attempt, 30)
                time.sleep(max(1.0, min(delay, 60.0)))
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                if attempt == 5:
                    raise GatewayError(f"provider transport failure: {exc}") from exc
                time.sleep(min(2 ** attempt, 30))
        raise GatewayError("provider retry loop exhausted")

    def complete(self, request: dict[str, Any]) -> dict[str, Any]:
        system_chunks = [m["content"] for m in request["messages"] if m["role"] == "system"]
        turns = [
            {"role": m["role"], "content": m["content"]}
            for m in request["messages"]
            if m["role"] in {"user", "assistant"}
        ]
        payload: dict[str, Any] = {
            "model": self.model,
            "max_tokens": int(request["max_output_tokens"]),
            "messages": turns,
        }
        if system_chunks:
            payload["system"] = "\n\n".join(system_chunks)
        if request.get("temperature") is not None:
            payload["temperature"] = float(request["temperature"])
        if request.get("stop"):
            payload["stop_sequences"] = list(request["stop"])

        if request.get("network_allowed"):
            if not self.web_search:
                raise GatewayError("network-enabled task has no frozen provider web-search policy")
            payload["tools"] = [{
                "type": str(self.web_search["tool_type"]),
                "name": "web_search",
                "max_uses": int(self.web_search["max_uses_per_request"]),
                "allowed_callers": list(self.web_search.get("allowed_callers", ["direct"])),
            }]

        http_request = urllib.request.Request(
            f"{self.base_url}/v1/messages",
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "content-type": "application/json",
                "anthropic-version": "2023-06-01",
                "x-api-key": self._api_key,
            },
            method="POST",
        )

        with self._lock:
            if (
                self.budget_usd is not None
                and self._estimated_cost_usd >= self.budget_usd
            ):
                raise GatewayError(
                    "soft API budget exhausted: "
                    f"estimated {self._estimated_cost_usd:.4f} USD >= {self.budget_usd:.4f} USD"
                )
            body = self._open(http_request)

            text = "".join(
                block.get("text", "")
                for block in body.get("content", [])
                if block.get("type") == "text"
            )
            raw_usage = body.get("usage", {}) or {}
            server_tool_use = raw_usage.get("server_tool_use", {}) or {}
            usage = {
                "input_tokens": int(raw_usage.get("input_tokens", 0) or 0),
                "output_tokens": int(raw_usage.get("output_tokens", 0) or 0),
                "web_search_requests": int(server_tool_use.get("web_search_requests", 0) or 0),
            }
            call_cost = self._request_cost(usage)
            self._estimated_cost_usd += call_cost
            usage["estimated_cost_usd"] = round(call_cost, 6)
            usage["cumulative_estimated_cost_usd"] = round(self._estimated_cost_usd, 6)

        return {
            "content": text,
            "stop_reason": body.get("stop_reason", "end_turn"),
            "usage": usage,
        }

def render_conversation(messages: list[dict[str, str]]) -> str:
    chunks = []
    for message in messages:
        chunks.append(f"<<<{message['role']}>>>\n{message['content']}")
    return "\n\n".join(chunks) + "\n"


def build_provider(args: argparse.Namespace, config: dict[str, Any]) -> Provider:
    if args.provider == "fake":
        script = Path(args.fake_script).resolve() if args.fake_script else None
        return FakeProvider(script)
    if args.provider == "exec":
        if not args.exec_command:
            raise GatewayError("--provider exec requires --exec-command")
        return ExecProvider(list(args.exec_command), int(args.provider_timeout), args.model)
    if args.provider == "anthropic-messages":
        if not args.model:
            raise GatewayError("--provider anthropic-messages requires --model")
        pricing = config.get("anthropic_pricing", {}).get(args.model)
        return AnthropicMessagesProvider(
            args.model,
            int(args.provider_timeout),
            budget_usd=float(args.budget_usd) if args.budget_usd is not None else None,
            pricing=pricing,
            web_search=config.get("anthropic_web_search"),
        )
    raise GatewayError(f"unknown provider: {args.provider}")


# --------------------------------------------------------------------------
# Request validation
# --------------------------------------------------------------------------


def validate_inference_request(
    request: dict[str, Any],
    config: dict[str, Any],
    network_policy: str,
    task_policy: dict[str, str],
) -> dict[str, Any]:
    """Reduce an untrusted request to the only shape the broker will act on.

    Everything the sandbox sends is untrusted. Rather than sanitizing in place,
    this returns a freshly built request containing only known-safe fields, so a
    future provider cannot accidentally forward an attacker-controlled key.
    """
    forbidden = [f for f in config["forbidden_request_fields"] if f in request]
    if forbidden:
        raise PolicyError(
            "the inference gateway is a pure model broker and refuses tool, file, "
            "command, endpoint and credential fields: " + ", ".join(sorted(forbidden))
        )

    request_id = request.get("request_id")
    if not isinstance(request_id, str) or not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", request_id):
        raise ProtocolError("request_id must be a short identifier string")

    task_id = request.get("task_id")
    if task_id is not None and (
        not isinstance(task_id, str) or not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", task_id)
    ):
        raise ProtocolError("task_id must be a short identifier string")

    raw_messages = request.get("messages")
    if not isinstance(raw_messages, list) or not raw_messages:
        raise ProtocolError("messages must be a non-empty array")
    if len(raw_messages) > int(config["max_messages"]):
        raise ProtocolError(f"messages exceed the frozen limit {config['max_messages']}")
    messages: list[dict[str, str]] = []
    for message in raw_messages:
        if not isinstance(message, dict):
            raise ProtocolError("each message must be an object")
        role = message.get("role")
        content = message.get("content")
        if role not in {"system", "user", "assistant"}:
            raise ProtocolError("message role must be system, user or assistant")
        if not isinstance(content, str):
            raise ProtocolError("message content must be a string")
        messages.append({"role": role, "content": content})

    max_output_tokens = request.get("max_output_tokens", 4096)
    if not isinstance(max_output_tokens, int) or max_output_tokens <= 0:
        raise ProtocolError("max_output_tokens must be a positive integer")
    max_output_tokens = min(max_output_tokens, int(config["max_output_tokens_ceiling"]))

    temperature = request.get("temperature")
    if temperature is not None and not isinstance(temperature, (int, float)):
        raise ProtocolError("temperature must be a number")

    stop = request.get("stop")
    if stop is not None:
        if not isinstance(stop, list) or not all(isinstance(s, str) for s in stop):
            raise ProtocolError("stop must be an array of strings")
        stop = stop[:8]

    requested_network = request.get("network_allowed", False)
    if not isinstance(requested_network, bool):
        raise ProtocolError("network_allowed must be a boolean")
    # The sandbox may only ever narrow the run's policy. A task that claims more
    # network than the trusted side granted is refused rather than downgraded, so
    # a mis-scoped Task Packet is a visible failure instead of a silent one.
    ceiling = task_policy.get(task_id or "", network_policy)
    if requested_network and ceiling != "allowed":
        raise PolicyError(
            "network_allowed=true is refused: the trusted gateway policy for this run "
            "is 'disabled', so no network-capable model tooling may be used"
        )

    return {
        "request_id": request_id,
        "task_id": task_id,
        "messages": messages,
        "max_output_tokens": max_output_tokens,
        "temperature": temperature,
        "stop": stop,
        "network_allowed": bool(requested_network),
    }


# --------------------------------------------------------------------------
# Server
# --------------------------------------------------------------------------


class GatewayState:
    def __init__(
        self,
        config: dict[str, Any],
        provider: Provider,
        network_policy: str,
        task_policy: dict[str, str],
        log_path: Path | None,
        max_requests: int | None,
    ) -> None:
        self.config = config
        self.provider = provider
        self.network_policy = network_policy
        self.task_policy = task_policy
        self.log_path = log_path
        self.max_requests = max_requests
        self.started_at = utc_now()
        self.lock = threading.Lock()
        self.counters = {"health": 0, "inference": 0, "policy_refusals": 0, "errors": 0}
        self.usage = {
            "input_tokens": 0,
            "output_tokens": 0,
            "web_search_requests": 0,
            "estimated_cost_usd": 0.0,
        }
        self.shutdown_event = threading.Event()

    def log(self, record: dict[str, Any]) -> None:
        if self.log_path is None:
            return
        line = json.dumps({"at_utc": utc_now(), **record}, sort_keys=True)
        with self.lock:
            with self.log_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")

    def health_payload(self) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "kind": "health.response",
            "ok": True,
            "gateway": PROTOCOL,
            "started_at_utc": self.started_at,
            "provider": self.provider.describe(),
            "network_policy": self.network_policy,
            "client_credentials_required": False,
            "credential_less_client": True,
            "host_tools_exposed": False,
            "exposed_tool_surface": [],
            "capabilities": list(self.config["allowed_request_kinds"]),
            "forbidden_request_fields": list(self.config["forbidden_request_fields"]),
            "max_request_bytes": int(self.config["max_request_bytes"]),
            "max_output_tokens_ceiling": int(self.config["max_output_tokens_ceiling"]),
        }


class GatewayHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:  # noqa: C901 - explicit protocol branches read better flat
        state: GatewayState = self.server.state  # type: ignore[attr-defined]
        config = state.config
        limit = int(config["max_request_bytes"])
        request_id = None
        try:
            raw = self.rfile.readline(limit + 1)
            if len(raw) > limit:
                raise ProtocolError(f"request exceeds the frozen limit {limit} bytes")
            if not raw.strip():
                return
            try:
                request = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise ProtocolError(f"request is not valid UTF-8 JSON: {exc}") from exc
            if not isinstance(request, dict):
                raise ProtocolError("request must be a JSON object")
            if request.get("schema_version") != 1:
                raise ProtocolError("request schema_version must be 1")
            kind = request.get("kind")
            if kind not in config["allowed_request_kinds"]:
                raise PolicyError(
                    f"unsupported request kind {kind!r}; this gateway brokers model "
                    "inference only"
                )
            request_id = request.get("request_id") if isinstance(request.get("request_id"), str) else None

            if kind == "health.request":
                with state.lock:
                    state.counters["health"] += 1
                self.respond(state.health_payload())
                return

            validated = validate_inference_request(
                request, config, state.network_policy, state.task_policy
            )
            request_id = validated["request_id"]
            started = time.perf_counter()
            result = state.provider.complete(validated)
            elapsed = time.perf_counter() - started

            content = result.get("content")
            if not isinstance(content, str):
                raise GatewayError("provider returned a non-text completion")
            content = scrub_outbound(content, state.provider.secrets())
            encoded = content.encode("utf-8")
            max_response = int(config["max_response_bytes"])
            if len(encoded) > max_response:
                raise GatewayError(
                    f"provider completion exceeds the frozen limit {max_response} bytes"
                )
            usage = result.get("usage") or {}
            with state.lock:
                state.counters["inference"] += 1
                state.usage["input_tokens"] += int(usage.get("input_tokens", 0) or 0)
                state.usage["output_tokens"] += int(usage.get("output_tokens", 0) or 0)
                state.usage["web_search_requests"] += int(
                    usage.get("web_search_requests", 0) or 0
                )
                state.usage["estimated_cost_usd"] = round(
                    float(state.usage["estimated_cost_usd"])
                    + float(usage.get("estimated_cost_usd", 0.0) or 0.0),
                    6,
                )
                served = state.counters["inference"]
            state.log({
                "event": "inference",
                "request_id": validated["request_id"],
                "task_id": validated["task_id"],
                "provider": state.provider.describe().get("id"),
                "network_allowed": validated["network_allowed"],
                "elapsed_seconds": round(elapsed, 3),
                "response_sha256": sha256_text(content),
                "usage": usage,
            })
            self.respond({
                "schema_version": 1,
                "kind": "inference.response",
                "ok": True,
                "request_id": validated["request_id"],
                "task_id": validated["task_id"],
                "content": content,
                "stop_reason": result.get("stop_reason", "end_turn"),
                "usage": {
                    "input_tokens": int(usage.get("input_tokens", 0) or 0),
                    "output_tokens": int(usage.get("output_tokens", 0) or 0),
                },
                "provider": {
                    **state.provider.describe(),
                    "response_sha256": sha256_text(content),
                },
            })
            if state.max_requests is not None and served >= state.max_requests:
                state.shutdown_event.set()
                threading.Thread(target=self.server.shutdown, daemon=True).start()
        except PolicyError as exc:
            with state.lock:
                state.counters["policy_refusals"] += 1
            state.log({"event": "policy_refusal", "request_id": request_id, "message": str(exc)})
            self.respond(self.error_payload(request_id, "policy", str(exc)))
        except ProtocolError as exc:
            with state.lock:
                state.counters["errors"] += 1
            state.log({"event": "protocol_error", "request_id": request_id, "message": str(exc)})
            self.respond(self.error_payload(request_id, "protocol", str(exc)))
        except GatewayError as exc:
            with state.lock:
                state.counters["errors"] += 1
            message = scrub_outbound(str(exc), state.provider.secrets())
            state.log({"event": "provider_error", "request_id": request_id, "message": message})
            self.respond(self.error_payload(request_id, "infrastructure", message))
        except Exception as exc:  # pragma: no cover - defensive
            with state.lock:
                state.counters["errors"] += 1
            message = scrub_outbound(f"{type(exc).__name__}: {exc}", state.provider.secrets())
            state.log({"event": "internal_error", "request_id": request_id, "message": message})
            self.respond(self.error_payload(request_id, "infrastructure", message))

    @staticmethod
    def error_payload(request_id: str | None, error_class: str, message: str) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "kind": "inference.error",
            "ok": False,
            "request_id": request_id,
            "error": {"class": error_class, "message": message},
        }

    def respond(self, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, sort_keys=True).encode("utf-8") + b"\n"
        try:
            self.wfile.write(data)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class GatewayServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, socket_path: str, state: GatewayState) -> None:
        self.state = state
        super().__init__(socket_path, GatewayHandler)


# sockaddr_un.sun_path is 104 bytes on macOS and 108 on Linux. The canonical
# /quidra-benchmark/gateway/inference.sock is far below that; a long synthetic
# path is not, and bind() then fails with an opaque OSError.
MAX_SOCKET_PATH_BYTES = 100


def prepare_socket_path(path: Path) -> None:
    if len(str(path).encode("utf-8")) > MAX_SOCKET_PATH_BYTES:
        raise GatewayError(
            f"socket path is too long for AF_UNIX ({len(str(path))} bytes, limit "
            f"{MAX_SOCKET_PATH_BYTES}): {path}. The canonical path is "
            f"{CANONICAL_SOCKET_PATH}; choose a shorter directory with --socket."
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        if not path.is_socket():
            raise GatewayError(f"refusing to replace a non-socket at {path}")
        try:
            path.unlink()
        except OSError as exc:
            if exc.errno != errno.ENOENT:
                raise


def cmd_serve(args: argparse.Namespace) -> int:
    config = load_config(Path(args.template).resolve() if args.template else None)
    socket_path = Path(args.socket or config["socket_path"])
    provider = build_provider(args, config)

    task_policy: dict[str, str] = {}
    if args.task_policy:
        raw = json.loads(Path(args.task_policy).read_text(encoding="utf-8"))
        if raw.get("schema_version") != 1:
            raise GatewayError("task policy schema_version must be 1")
        for task_id, policy in raw.get("tasks", {}).items():
            if policy not in config["network_policies"]:
                raise GatewayError(f"unknown network policy for {task_id}: {policy}")
            task_policy[task_id] = policy

    state = GatewayState(
        config=config,
        provider=provider,
        network_policy=args.network_policy,
        task_policy=task_policy,
        log_path=Path(args.log).resolve() if args.log else None,
        max_requests=int(args.max_requests) if args.max_requests else None,
    )

    prepare_socket_path(socket_path)
    server = GatewayServer(str(socket_path), state)
    os.chmod(socket_path, int(config["socket_mode"]))

    ready = {
        "schema_version": 1,
        "kind": "gateway.ready",
        "gateway": PROTOCOL,
        "socket_path": str(socket_path),
        "provider": provider.describe(),
        "network_policy": args.network_policy,
        "pid": os.getpid(),
    }
    print(json.dumps(ready, sort_keys=True), flush=True)
    if args.ready_file:
        Path(args.ready_file).write_text(json.dumps(ready, indent=2, sort_keys=True) + "\n",
                                         encoding="utf-8")

    # A supervisor stops this process with SIGTERM. Shutting down cleanly there
    # means the socket file is removed rather than left behind, where it would
    # look alive to the next gateway's readiness check.
    def stop(_signum: int, _frame: Any) -> None:
        state.shutdown_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    for received in (signal.SIGTERM, signal.SIGINT):
        signal.signal(received, stop)

    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        try:
            socket_path.unlink()
        except OSError:
            pass
        if args.ready_file:
            try:
                Path(args.ready_file).unlink()
            except OSError:
                pass
    summary = {
        "schema_version": 1,
        "kind": "gateway.summary",
        "counters": state.counters,
        "usage": state.usage,
    }
    print(json.dumps(summary, sort_keys=True), flush=True)
    return 0


def cmd_health(args: argparse.Namespace) -> int:
    config = load_config(Path(args.template).resolve() if args.template else None)
    socket_path = Path(args.socket or config["socket_path"])
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(float(args.timeout))
        client.connect(str(socket_path))
        client.sendall(json.dumps({"schema_version": 1, "kind": "health.request"}).encode() + b"\n")
        chunks = []
        while not chunks or not chunks[-1].endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    payload = json.loads(b"".join(chunks).decode("utf-8"))
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if payload.get("ok") else 2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Trusted credential-holding model inference gateway for the Quidra benchmark"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    serve = sub.add_parser("serve", help="listen on the shared Unix domain socket")
    serve.add_argument("--socket")
    serve.add_argument("--template")
    serve.add_argument(
        "--provider", default="fake", choices=("fake", "exec", "anthropic-messages")
    )
    serve.add_argument("--fake-script", help="deterministic scripted responses for CI")
    serve.add_argument(
        "--exec-command",
        nargs=argparse.REMAINDER,
        help="trusted local command; the conversation arrives on stdin, text leaves on stdout",
    )
    serve.add_argument("--model")
    serve.add_argument(
        "--budget-usd",
        type=float,
        help="trusted-side soft API spend limit; no new paid request starts after it is reached",
    )
    serve.add_argument("--provider-timeout", type=int, default=900)
    serve.add_argument("--network-policy", default="disabled", choices=("disabled", "allowed"))
    serve.add_argument("--task-policy", help="JSON file granting per-task network ceilings")
    serve.add_argument("--log", help="append a JSONL audit record per brokered request")
    serve.add_argument("--ready-file")
    serve.add_argument("--max-requests", type=int)
    serve.set_defaults(func=cmd_serve)

    health = sub.add_parser("health", help="perform the handshake against a running gateway")
    health.add_argument("--socket")
    health.add_argument("--template")
    health.add_argument("--timeout", type=float, default=15.0)
    health.set_defaults(func=cmd_health)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return int(args.func(args))
    except GatewayError as exc:
        print(f"inference gateway error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
