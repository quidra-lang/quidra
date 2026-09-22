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
import http.client
import urllib.error
import urllib.request

TEMPLATE_DIR = Path(__file__).resolve().parent.parent
CONFIG_RELATIVE = "config/inference_gateway.json"
PROTOCOL = "quidra-inference-gateway-v1"
CANONICAL_SOCKET_PATH = "/quidra-benchmark/gateway/inference.sock"


class GatewayError(RuntimeError):
    """Operator-facing gateway failure (never sent verbatim to the sandbox)."""


class EventStreamError(RuntimeError):
    """A streamed provider response ended early or carried an error event.

    Raised inside the transport retry loop, where it is treated like a dropped
    connection: the identical request is sent again.
    """


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

    def provider_tool_policy(self) -> list[dict[str, Any]]:
        """Provider-side tools this adapter may enable, as frozen on the trusted side.

        The handshake reports this verbatim. A provider that quietly attaches a
        tool while the gateway advertises none would make `preflight` record
        evidence that is not true, which is the failure this whole boundary
        exists to prevent.
        """
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
        self._sequence_served = 0
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

        # A script may price its completions so the spend guards can be tested
        # without a paid provider. Zero by default: the fake costs nothing.
        usage = {"input_tokens": len(transcript) // 4, "output_tokens": 0}
        priced = self.script.get("usage_cost_usd_per_call")
        if priced is not None:
            usage["estimated_cost_usd"] = float(priced)

        def scripted(entry: Any) -> dict[str, Any]:
            # A scripted completion is a string, or an object that also names
            # the stop reason the provider reported, so a test can hand the
            # sandbox a truncated turn without a paid provider.
            if isinstance(entry, dict):
                return {
                    "content": str(entry.get("content", "")),
                    "stop_reason": str(entry.get("stop_reason") or "end_turn"),
                    "usage": {**usage, **(entry.get("usage") or {})},
                }
            return {"content": str(entry), "stop_reason": "end_turn", "usage": dict(usage)}

        # Per-task turn scripts come first: they let one gateway drive many
        # independent work units deterministically, each with its own turn counter.
        turns = (self.script.get("tasks") or {}).get(task_id)
        if turns and task_index <= len(turns):
            return scripted(turns[task_index - 1])

        for rule in self.script.get("rules", []):
            needle = rule.get("contains")
            if needle and needle in last_user:
                return scripted(rule["content"])

        # The sequence answers the calls that nothing above answered, in order.
        # Counting every call here instead would let a scripted trial completion
        # (served by a rule) silently consume the agent's next scripted action.
        sequence = self.script.get("sequence")
        if sequence:
            with self._lock:
                position = self._sequence_served
                if position < len(sequence):
                    self._sequence_served += 1
            if position < len(sequence):
                return scripted(sequence[position])

        default = self.script.get("default")
        if default is not None:
            return scripted(default)

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
        decoding: dict[str, Any] | None = None,
        caching: dict[str, Any] | None = None,
        transport: dict[str, Any] | None = None,
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
        self.decoding = dict(decoding or {})
        if self.decoding.get("send_sampling_parameters"):
            raise GatewayError(
                "this adapter refuses to send sampling parameters: the evaluated "
                "model family rejects temperature/top_p/top_k with HTTP 400"
            )
        self.effort = self.decoding.get("effort")
        # Sandbox-agent action turns are never scored. They are decoded at their
        # own frozen depth because adaptive thinking is billed inside max_tokens:
        # at the scored depth the first action turn of a unit regularly spent its
        # whole cap on reasoning and returned no text, which ended the unit.
        self.orchestration_effort = self.decoding.get("orchestration_effort") or self.effort
        # Even at the shallow depth, an action turn sometimes spends its whole
        # answer inside the thinking block and ends the turn with no text at all;
        # the runtime counts that as a protocol error and a wasted turn. Turning
        # thinking off for these never-scored turns removes that failure mode.
        # Scored requests keep the provider default, exactly as the frozen
        # sampling declaration records.
        self.orchestration_thinking = self.decoding.get("orchestration_thinking")
        if self.orchestration_thinking not in (None, "disabled", "provider-default"):
            raise GatewayError(
                "anthropic_decoding.orchestration_thinking must be 'disabled' or "
                "'provider-default'"
            )
        # A completion with no text and an ordinary end_turn is a provider-side
        # accident, not an answer. It is re-requested once, identically, before
        # the sandbox ever sees it.
        self.empty_completion_retries = max(
            0, int(self.decoding.get("empty_completion_retries", 1) or 0)
        )
        self.caching = dict(caching or {})
        # Streaming is a transport decision on the trusted side: it changes how
        # the bytes of an answer arrive, never what the model sees or produces.
        # It is on unless the frozen config switches it off.
        self.streaming = bool((transport or {}).get("streaming", True))
        self._estimated_cost_usd = 0.0
        self._lock = threading.Lock()

    def effort_for(self, purpose: str | None) -> str | None:
        return self.orchestration_effort if purpose == "orchestration" else self.effort

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "model": self.model,
            "offline": False,
            "soft_budget_usd": self.budget_usd,
            "estimated_cost_usd": round(self._estimated_cost_usd, 6),
            "sampling_parameters": "omitted",
            "effort": self.effort,
            "orchestration_effort": self.orchestration_effort,
            "orchestration_thinking": self.orchestration_thinking or "provider-default",
            "empty_completion_retries": self.empty_completion_retries,
            "prompt_caching": bool(self.caching.get("enabled")),
            "streaming": "tool-less requests" if self.streaming else "off",
        }

    def secrets(self) -> list[str]:
        return [self._api_key]

    def provider_tool_policy(self) -> list[dict[str, Any]]:
        if not self.web_search:
            return []
        return [{
            "type": str(self.web_search["tool_type"]),
            "name": "web_search",
            "scope": "provider-side",
            "enabled_for": "tasks whose frozen trusted policy grants network_allowed",
            "max_uses_per_request": int(self.web_search["max_uses_per_request"]),
            "selectable_by_sandbox": False,
            "grants_host_access": False,
        }]

    def _request_cost(self, usage: dict[str, Any]) -> float:
        if not self.pricing:
            return 0.0
        input_price = float(self.pricing["input_usd_per_million_tokens"])
        # The provider reports the uncached remainder as input_tokens and the
        # cached prefix separately; each part has its own rate, and pricing the
        # cached part at full rate would overstate spend by the whole prefix.
        cache_write_price = float(
            self.pricing.get("cache_write_usd_per_million_tokens", input_price * 1.25)
        )
        cache_read_price = float(
            self.pricing.get("cache_read_usd_per_million_tokens", input_price * 0.1)
        )
        return (
            float(usage.get("input_tokens", 0) or 0) * input_price / 1_000_000.0
            + float(usage.get("cache_creation_input_tokens", 0) or 0)
            * cache_write_price
            / 1_000_000.0
            + float(usage.get("cache_read_input_tokens", 0) or 0)
            * cache_read_price
            / 1_000_000.0
            + float(usage.get("output_tokens", 0) or 0)
            * float(self.pricing["output_usd_per_million_tokens"])
            / 1_000_000.0
            + float(usage.get("web_search_requests", 0) or 0)
            * float(self.pricing.get("web_search_usd_per_request", 0.0))
        )

    def _open(
        self, http_request: urllib.request.Request, *, stream: bool = False
    ) -> dict[str, Any]:
        for attempt in range(6):
            try:
                with urllib.request.urlopen(http_request, timeout=self.timeout) as response:
                    if stream:
                        return self._read_event_stream(response)
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
            except (
                urllib.error.URLError, TimeoutError, OSError, http.client.HTTPException,
                json.JSONDecodeError, EventStreamError,
            ) as exc:
                if attempt == 5:
                    raise GatewayError(f"provider transport failure: {exc}") from exc
                time.sleep(min(2 ** attempt, 30))
        raise GatewayError("provider retry loop exhausted")

    @staticmethod
    def _read_event_stream(response: Any) -> dict[str, Any]:
        """Assemble one Messages response from its server-sent event stream.

        A non-streaming request is silent on the wire for as long as the model
        generates. On the GitHub-hosted runner that silence is fatal: the
        outbound connection is dropped after roughly four idle minutes, and the
        third paid run lost every packet call that generated for longer than
        that as "Remote end closed connection without response", then paid for
        the same generation again on each transport retry. A streamed response
        carries deltas and keep-alive pings for the whole generation, so the
        connection is never idle.

        The stream is folded back into the exact shape the non-streaming
        endpoint returns - content blocks, stop_reason and usage - so nothing
        after this point knows which transport carried the answer.
        """
        blocks: dict[int, dict[str, Any]] = {}
        usage: dict[str, Any] = {}
        stop_reason: str | None = None
        finished = False

        def apply(event: str, payload: dict[str, Any]) -> None:
            nonlocal stop_reason, finished
            if event == "message_start":
                usage.update((payload.get("message") or {}).get("usage") or {})
            elif event == "content_block_start":
                index = int(payload["index"])
                block = dict(payload.get("content_block") or {})
                if block.get("type") == "text":
                    block.setdefault("text", "")
                elif block.get("type") == "thinking":
                    block.setdefault("thinking", "")
                elif block.get("type") in {"tool_use", "server_tool_use"}:
                    block["_partial_json"] = ""
                blocks[index] = block
            elif event == "content_block_delta":
                index = int(payload["index"])
                block = blocks.setdefault(index, {"type": "text", "text": ""})
                delta = payload.get("delta") or {}
                kind = delta.get("type")
                if kind == "text_delta":
                    block["text"] = block.get("text", "") + str(delta.get("text", ""))
                elif kind == "thinking_delta":
                    block["thinking"] = block.get("thinking", "") + str(delta.get("thinking", ""))
                elif kind == "signature_delta":
                    block["signature"] = str(delta.get("signature", ""))
                elif kind == "input_json_delta":
                    block["_partial_json"] = (
                        block.get("_partial_json", "") + str(delta.get("partial_json", ""))
                    )
            elif event == "content_block_stop":
                block = blocks.get(int(payload["index"]))
                if block is not None and "_partial_json" in block:
                    raw = block.pop("_partial_json")
                    block["input"] = json.loads(raw) if raw.strip() else {}
            elif event == "message_delta":
                delta = payload.get("delta") or {}
                if delta.get("stop_reason"):
                    stop_reason = str(delta["stop_reason"])
                for key, value in (payload.get("usage") or {}).items():
                    if value is not None:
                        usage[key] = value
            elif event == "message_stop":
                finished = True
            elif event == "error":
                raise EventStreamError(f"provider stream error: {payload.get('error')}")

        event_name: str | None = None
        data_lines: list[str] = []
        for raw_line in response:
            line = raw_line.decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                if data_lines:
                    payload = json.loads("\n".join(data_lines))
                    apply(event_name or str(payload.get("type") or ""), payload)
                event_name = None
                data_lines = []
                continue
            if line.startswith(":"):
                continue
            field, _, value = line.partition(":")
            if value.startswith(" "):
                value = value[1:]
            if field == "event":
                event_name = value
            elif field == "data":
                data_lines.append(value)
        if data_lines:
            payload = json.loads("\n".join(data_lines))
            apply(event_name or str(payload.get("type") or ""), payload)
        if not finished:
            raise EventStreamError("provider event stream ended before message_stop")
        content = [blocks[index] for index in sorted(blocks)]
        return {"content": content, "stop_reason": stop_reason or "end_turn", "usage": usage}

    #: What a model that asks for a tool it cannot have is told. The gateway is a
    #: pure inference broker: the only tool it ever attaches is the frozen
    #: server-side web search, and once that tool's per-request budget is spent
    #: the model has nothing left to call. Answering the request with an error
    #: result keeps the turn alive so the model can finish from what it has,
    #: instead of the whole paid turn ending on a stop reason nobody consumes.
    TOOL_USE_REFUSAL = (
        "This tool is not available. The trusted gateway executes no client tools, "
        "and the frozen server-side web-search budget for this request may already "
        "be spent. Do not request tools again; finish the task now from the "
        "evidence you already have and reply with the required output."
    )

    def _reserve_budget(self) -> None:
        with self._lock:
            if self.budget_usd is not None and self._estimated_cost_usd >= self.budget_usd:
                raise GatewayError(
                    "soft API budget exhausted: "
                    f"estimated {self._estimated_cost_usd:.4f} USD >= {self.budget_usd:.4f} USD"
                )

    def _account(self, step: dict[str, Any]) -> None:
        with self._lock:
            self._estimated_cost_usd += self._request_cost(step)

    @staticmethod
    def _zero_usage() -> dict[str, int]:
        return {
            "input_tokens": 0,
            "cache_creation_input_tokens": 0,
            "cache_read_input_tokens": 0,
            "output_tokens": 0,
            "web_search_requests": 0,
        }

    def _complete_once(
        self, payload: dict[str, Any], turns: list[dict[str, Any]]
    ) -> dict[str, Any]:
        """One provider turn, with the continuations that keep it alive.

        Only budget checks and cost accounting take the provider lock, so several
        workers can have requests in flight at once; the HTTP round trip itself
        runs unlocked.
        """
        max_continuations = int(self.web_search.get("max_turn_continuations", 8) or 0)
        texts: list[str] = []
        usage = self._zero_usage()
        stop_reason = "end_turn"
        history: list[dict[str, Any]] = list(turns)
        # A paused turn is resumed by sending the assistant content produced so
        # far back verbatim; it accumulates across pauses within one turn.
        assistant_blocks: list[dict[str, Any]] = []
        continuations = 0
        tool_use_refusals = 0
        for continuation in range(max_continuations + 1):
            self._reserve_budget()
            payload["messages"] = (
                [*history, {"role": "assistant", "content": assistant_blocks}]
                if assistant_blocks
                else history
            )
            # A request that carries no tool streams: its answer can take many
            # minutes to generate and the connection must not sit idle. A
            # server-tool request keeps the buffered response so the pause_turn
            # and tool_use continuations below can send its content blocks back
            # verbatim; those turns are short and were never the ones dropped.
            stream = bool(self.streaming) and "tools" not in payload
            wire = {**payload, "stream": True} if stream else payload
            http_request = urllib.request.Request(
                f"{self.base_url}/v1/messages",
                data=json.dumps(wire).encode("utf-8"),
                headers={
                    "content-type": "application/json",
                    "anthropic-version": "2023-06-01",
                    "x-api-key": self._api_key,
                    "accept": "text/event-stream" if stream else "application/json",
                },
                method="POST",
            )
            body = self._open(http_request, stream=stream)
            content = body.get("content", []) or []
            texts.extend(
                block.get("text", "") for block in content if block.get("type") == "text"
            )
            raw_usage = body.get("usage", {}) or {}
            server_tool_use = raw_usage.get("server_tool_use", {}) or {}
            step = {
                "input_tokens": int(raw_usage.get("input_tokens", 0) or 0),
                "cache_creation_input_tokens": int(
                    raw_usage.get("cache_creation_input_tokens", 0) or 0
                ),
                "cache_read_input_tokens": int(
                    raw_usage.get("cache_read_input_tokens", 0) or 0
                ),
                "output_tokens": int(raw_usage.get("output_tokens", 0) or 0),
                "web_search_requests": int(server_tool_use.get("web_search_requests", 0) or 0),
            }
            for key, value in step.items():
                usage[key] += value
            self._account(step)
            stop_reason = body.get("stop_reason", "end_turn")
            if continuation == max_continuations:
                break
            # A server-side tool loop hands the turn back paused. Sending the
            # response back verbatim lets it continue; the sandbox never sees
            # the partial state and never has to know a tool ran.
            if stop_reason == "pause_turn":
                assistant_blocks = [*assistant_blocks, *content]
                continuations += 1
                continue
            # The model asked for a client tool. There is none: answer every
            # request with an error result and let the model finish its turn.
            if stop_reason == "tool_use":
                requested = [block for block in content if block.get("type") == "tool_use"]
                if not requested:
                    break
                refusals = [
                    {
                        "type": "tool_result",
                        "tool_use_id": str(block.get("id")),
                        "is_error": True,
                        "content": self.TOOL_USE_REFUSAL,
                    }
                    for block in requested
                ]
                history = [
                    *history,
                    {"role": "assistant", "content": [*assistant_blocks, *content]},
                    {"role": "user", "content": refusals},
                ]
                assistant_blocks = []
                tool_use_refusals += len(requested)
                continuations += 1
                continue
            break
        return {
            "content": "".join(texts),
            "stop_reason": stop_reason,
            "usage": usage,
            "continuations": continuations,
            "tool_use_refusals": tool_use_refusals,
        }

    def complete(self, request: dict[str, Any]) -> dict[str, Any]:
        system_chunks = [m["content"] for m in request["messages"] if m["role"] == "system"]
        turns: list[dict[str, Any]] = [
            {"role": m["role"], "content": m["content"]}
            for m in request["messages"]
            if m["role"] in {"user", "assistant"}
        ]
        purpose = request.get("purpose")
        effort = self.effort_for(purpose)
        cache_marker = {"type": "ephemeral"}
        caching = bool(self.caching.get("enabled"))
        if caching and str(self.caching.get("ttl", "5m")) == "1h":
            cache_marker["ttl"] = "1h"
        payload: dict[str, Any] = {
            "model": self.model,
            "max_tokens": int(request["max_output_tokens"]),
            "messages": turns,
        }
        if system_chunks:
            system_text = "\n\n".join(system_chunks)
            if caching:
                payload["system"] = [
                    {"type": "text", "text": system_text, "cache_control": dict(cache_marker)}
                ]
            else:
                payload["system"] = system_text
        # Prompt caching is a trusted-side pricing decision the sandbox cannot see
        # or select. A sandbox-agent conversation grows by appending, so every
        # turn re-sends the same prefix; a breakpoint on the last user block lets
        # each turn read that prefix at the cache rate instead of full price. A
        # server-side web-search loop re-sends the Task Packet on every
        # continuation and benefits the same way. What the model sees and what it
        # produces are unchanged.
        if caching:
            for index in range(len(turns) - 1, -1, -1):
                if turns[index]["role"] == "user":
                    turns[index] = {
                        "role": "user",
                        "content": [{
                            "type": "text",
                            "text": turns[index]["content"],
                            "cache_control": dict(cache_marker),
                        }],
                    }
                    break
        # No temperature/top_p/top_k: this model family removed them and rejects a
        # request carrying one with HTTP 400. Depth is pinned with effort, which is
        # the control it does expose. Scored requests leave thinking at the
        # provider default; never-scored orchestration turns may switch it off.
        if effort:
            payload["output_config"] = {"effort": str(effort)}
        if purpose == "orchestration" and self.orchestration_thinking == "disabled":
            payload["thinking"] = {"type": "disabled"}
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

        total = self._zero_usage()
        outcome: dict[str, Any] = {}
        empty_retries = 0
        for attempt in range(self.empty_completion_retries + 1):
            outcome = self._complete_once(payload, turns)
            for key in total:
                total[key] += int(outcome["usage"].get(key, 0) or 0)
            if outcome["content"].strip() or outcome["stop_reason"] != "end_turn":
                break
            if attempt < self.empty_completion_retries:
                empty_retries += 1
        usage: dict[str, Any] = dict(total)
        usage["estimated_cost_usd"] = round(self._request_cost(total), 6)
        with self._lock:
            usage["cumulative_estimated_cost_usd"] = round(self._estimated_cost_usd, 6)

        return {
            "content": outcome["content"],
            "stop_reason": outcome["stop_reason"],
            "usage": usage,
            "effort": effort,
            "continuations": int(outcome.get("continuations", 0)),
            "tool_use_refusals": int(outcome.get("tool_use_refusals", 0)),
            "empty_completion_retries": empty_retries,
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
            decoding=config.get("anthropic_decoding"),
            caching=config.get("prompt_caching"),
            transport=config.get("anthropic_transport"),
        )
    raise GatewayError(f"unknown provider: {args.provider}")


#: Request purposes the broker recognises. This is a label the trusted side maps
#: to a frozen decoding depth; it is not a decoding parameter, which the sandbox
#: may not supply.
REQUEST_PURPOSES = ("scored", "orchestration")


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

    stop = request.get("stop")
    if stop is not None:
        if not isinstance(stop, list) or not all(isinstance(s, str) for s in stop):
            raise ProtocolError("stop must be an array of strings")
        stop = stop[:8]

    requested_network = request.get("network_allowed", False)
    if not isinstance(requested_network, bool):
        raise ProtocolError("network_allowed must be a boolean")

    purpose = request.get("purpose", "scored")
    if purpose not in REQUEST_PURPOSES:
        raise ProtocolError(
            "purpose must be one of " + ", ".join(REQUEST_PURPOSES)
            + "; it names which frozen depth applies, it does not set one"
        )
    # The sandbox may only ever narrow the run's policy. A task that claims more
    # network than the trusted side granted is refused rather than downgraded, so
    # a mis-scoped Task Packet is a visible failure instead of a silent one.
    # Fail closed. When the trusted side has frozen a per-task policy, a task it
    # does not name is unknown work, not work that inherits the run-wide default.
    # Inheriting was the one place this contract opened rather than closed.
    if task_policy:
        if not task_id:
            raise PolicyError(
                "a frozen task network policy is in force, so every request must "
                "identify its task"
            )
        if task_id not in task_policy:
            raise PolicyError(
                f"task {task_id!r} is not named in the frozen network policy; "
                "refusing rather than falling back to the run-wide default"
            )
        ceiling = task_policy[task_id]
    else:
        ceiling = network_policy
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
        "stop": stop,
        "network_allowed": bool(requested_network),
        "purpose": str(purpose),
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
        task_budgets: dict[str, float] | None = None,
    ) -> None:
        self.config = config
        self.provider = provider
        self.network_policy = network_policy
        self.task_policy = task_policy
        # Per-task soft ceilings from the frozen policy, enforced here so they
        # hold for every provider. A unit that keeps spending past its plan is
        # refused its next request rather than draining the run-wide budget.
        self.task_budgets = dict(task_budgets or {})
        self.task_spend: dict[str, float] = {}
        self.log_path = log_path
        self.max_requests = max_requests
        self.started_at = utc_now()
        self.lock = threading.Lock()
        self.counters = {"health": 0, "inference": 0, "policy_refusals": 0, "errors": 0}
        self.usage = {
            "input_tokens": 0,
            "cache_creation_input_tokens": 0,
            "cache_read_input_tokens": 0,
            "output_tokens": 0,
            "web_search_requests": 0,
            "estimated_cost_usd": 0.0,
        }
        self.shutdown_event = threading.Event()

    def check_task_ceiling(self, task_id: str | None) -> None:
        if not task_id or task_id not in self.task_budgets:
            return
        with self.lock:
            spent = self.task_spend.get(task_id, 0.0)
            ceiling = self.task_budgets[task_id]
        if spent >= ceiling:
            raise PolicyError(
                f"task spend ceiling reached: {task_id} has an estimated "
                f"{spent:.4f} USD against a frozen ceiling of {ceiling:.4f} USD; "
                "the unit is stopped here so it cannot consume the run's budget alone"
            )

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
            # What the provider may enable on the trusted side, and what the
            # sandbox may ask for. The second list is empty by construction:
            # every tool-shaped request field is refused before a provider is
            # ever reached.
            "exposed_tool_surface": self.provider.provider_tool_policy(),
            "sandbox_selectable_tools": [],
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
            state.check_task_ceiling(validated["task_id"])
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
            request_cost = float(usage.get("estimated_cost_usd", 0.0) or 0.0)
            with state.lock:
                state.counters["inference"] += 1
                for key in (
                    "input_tokens", "cache_creation_input_tokens",
                    "cache_read_input_tokens", "output_tokens", "web_search_requests",
                ):
                    state.usage[key] += int(usage.get(key, 0) or 0)
                state.usage["estimated_cost_usd"] = round(
                    float(state.usage["estimated_cost_usd"]) + request_cost, 6
                )
                if validated["task_id"]:
                    state.task_spend[validated["task_id"]] = (
                        state.task_spend.get(validated["task_id"], 0.0) + request_cost
                    )
                served = state.counters["inference"]
            describe = state.provider.describe()
            state.log({
                "event": "inference",
                "request_id": validated["request_id"],
                "task_id": validated["task_id"],
                "provider": describe.get("id"),
                "network_allowed": validated["network_allowed"],
                "purpose": validated["purpose"],
                "decoding": result.get("effort", describe.get("effort")),
                "sampling_parameters": "omitted",
                "stop_reason": result.get("stop_reason", "end_turn"),
                "continuations": int(result.get("continuations", 0) or 0),
                "tool_use_refusals": int(result.get("tool_use_refusals", 0) or 0),
                "empty_completion_retries": int(result.get("empty_completion_retries", 0) or 0),
                "elapsed_seconds": round(elapsed, 3),
                "response_sha256": sha256_text(content),
                "response_chars": len(content),
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
                    "cache_creation_input_tokens": int(
                        usage.get("cache_creation_input_tokens", 0) or 0
                    ),
                    "cache_read_input_tokens": int(
                        usage.get("cache_read_input_tokens", 0) or 0
                    ),
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
    task_budgets: dict[str, float] = {}
    if args.task_policy:
        raw = json.loads(Path(args.task_policy).read_text(encoding="utf-8"))
        if raw.get("schema_version") != 1:
            raise GatewayError("task policy schema_version must be 1")
        for task_id, policy in raw.get("tasks", {}).items():
            if policy not in config["network_policies"]:
                raise GatewayError(f"unknown network policy for {task_id}: {policy}")
            task_policy[task_id] = policy
        for task_id, ceiling in (raw.get("budgets") or {}).items():
            if task_id not in task_policy:
                raise GatewayError(f"spend ceiling names a task the policy does not: {task_id}")
            try:
                value = float(ceiling)
            except (TypeError, ValueError) as exc:
                raise GatewayError(f"spend ceiling for {task_id} is not a number") from exc
            if value <= 0:
                raise GatewayError(f"spend ceiling for {task_id} must be positive")
            task_budgets[task_id] = value

    state = GatewayState(
        config=config,
        provider=provider,
        network_policy=args.network_policy,
        task_policy=task_policy,
        log_path=Path(args.log).resolve() if args.log else None,
        max_requests=int(args.max_requests) if args.max_requests else None,
        task_budgets=task_budgets,
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
