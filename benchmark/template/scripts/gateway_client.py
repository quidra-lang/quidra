#!/usr/bin/env python3
"""Credential-less client for the trusted inference gateway.

This module is the only way scored sandbox code reaches a model. It holds no API
key, reads no provider configuration and has no fallback path: if the gateway
socket is absent or refuses a request, inference simply fails. That absence of a
fallback is deliberate, because a fallback is exactly how a credential ends up
inside scored work.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import sys
import uuid
from typing import Any

TEMPLATE_DIR = Path(__file__).resolve().parent.parent
PROTOCOL = "quidra-inference-gateway-v1"
SOCKET_ENV = "QUIDRA_BENCHMARK_INFERENCE_SOCKET"


class GatewayClientError(RuntimeError):
    pass


class GatewayRefusal(GatewayClientError):
    """The gateway refused the request on policy grounds."""


def load_config(template_dir: Path | None = None) -> dict[str, Any]:
    path = (template_dir or TEMPLATE_DIR) / "config" / "inference_gateway.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    if config.get("schema_version") != 1 or config.get("protocol") != PROTOCOL:
        raise GatewayClientError(f"unsupported inference gateway config: {path}")
    return config


def resolve_socket_path(
    explicit: str | None = None, config: dict[str, Any] | None = None
) -> Path:
    """Socket location, in the order a sandboxed process should trust it."""
    if explicit:
        return Path(explicit)
    from_env = os.environ.get(SOCKET_ENV)
    if from_env:
        return Path(from_env)
    return Path((config or load_config())["socket_path"])


class InferenceGatewayClient:
    def __init__(
        self,
        socket_path: Path | str | None = None,
        timeout: float = 900.0,
        template_dir: Path | None = None,
    ) -> None:
        self.config = load_config(template_dir)
        self.socket_path = Path(socket_path) if socket_path else resolve_socket_path(
            None, self.config
        )
        self.timeout = timeout

    # -- transport ---------------------------------------------------------

    def _exchange(self, payload: dict[str, Any]) -> dict[str, Any]:
        data = json.dumps(payload, sort_keys=True).encode("utf-8")
        if len(data) + 1 > int(self.config["max_request_bytes"]):
            raise GatewayClientError(
                f"request exceeds the frozen limit {self.config['max_request_bytes']} bytes"
            )
        if len(str(self.socket_path).encode("utf-8")) > 100:
            raise GatewayClientError(
                f"socket path is too long for AF_UNIX: {self.socket_path}"
            )
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(self.timeout)
                client.connect(str(self.socket_path))
                client.sendall(data + b"\n")
                chunks: list[bytes] = []
                total = 0
                while True:
                    chunk = client.recv(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    total += len(chunk)
                    if total > int(self.config["max_response_bytes"]) * 2:
                        raise GatewayClientError("gateway response exceeded the transport limit")
                    if chunks[-1].endswith(b"\n"):
                        break
        except FileNotFoundError as exc:
            raise GatewayClientError(
                f"inference gateway socket is not present at {self.socket_path}; "
                "scored work has no provider credentials and no fallback path"
            ) from exc
        except (ConnectionError, socket.timeout, OSError) as exc:
            raise GatewayClientError(f"inference gateway transport failure: {exc}") from exc

        raw = b"".join(chunks).strip()
        if not raw:
            raise GatewayClientError("inference gateway closed the connection without a response")
        try:
            response = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise GatewayClientError(f"inference gateway response is not valid JSON: {exc}") from exc
        if not isinstance(response, dict) or response.get("schema_version") != 1:
            raise GatewayClientError("inference gateway response has an unsupported schema")
        return response

    # -- operations --------------------------------------------------------

    def raw_exchange(self, payload: dict[str, Any]) -> dict[str, Any]:
        """Send an arbitrary payload and return the gateway's raw reply.

        `preflight` uses this to probe the broker with requests it must refuse -
        a tool field, an unsupported kind - because a refusal that actually
        happens is evidence, while a promise in a config file is not.
        """
        return self._exchange(payload)

    def health(self) -> dict[str, Any]:
        response = self._exchange({"schema_version": 1, "kind": "health.request"})
        if response.get("kind") != "health.response" or not response.get("ok"):
            raise GatewayClientError(f"inference gateway handshake failed: {response}")
        if response.get("gateway") != PROTOCOL:
            raise GatewayClientError(
                f"unexpected gateway protocol: {response.get('gateway')!r}"
            )
        return response

    def complete(
        self,
        messages: list[dict[str, str]],
        *,
        task_id: str | None = None,
        max_output_tokens: int = 8192,
        temperature: float | None = None,
        stop: list[str] | None = None,
        network_allowed: bool = False,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "schema_version": 1,
            "kind": "inference.request",
            "request_id": request_id or uuid.uuid4().hex,
            "messages": messages,
            "max_output_tokens": int(max_output_tokens),
            "network_allowed": bool(network_allowed),
        }
        if task_id:
            payload["task_id"] = task_id
        if temperature is not None:
            payload["temperature"] = float(temperature)
        if stop:
            payload["stop"] = list(stop)

        response = self._exchange(payload)
        if response.get("kind") == "inference.error":
            error = response.get("error", {})
            message = str(error.get("message", "unspecified gateway error"))
            if error.get("class") == "policy":
                raise GatewayRefusal(message)
            raise GatewayClientError(f"inference gateway error ({error.get('class')}): {message}")
        if response.get("kind") != "inference.response" or not response.get("ok"):
            raise GatewayClientError(f"unexpected gateway response: {response}")
        if not isinstance(response.get("content"), str):
            raise GatewayClientError("gateway response carried no text completion")
        return response


def parse_model_json(text: str) -> dict:
    """Parse one JSON object out of a model completion.

    Models reliably wrap JSON in a Markdown fence even when told not to, and a
    rejected turn costs a scored model call. Tolerating exactly one fence - and
    nothing looser - keeps the contract strict without burning retries on
    formatting.
    """
    stripped = text.strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if len(lines) >= 2 and lines[-1].strip() == "```":
            stripped = "\n".join(lines[1:-1]).strip()
    if not stripped.startswith("{"):
        raise GatewayClientError(
            "model completion is not a single JSON object as required by the contract"
        )
    try:
        value = json.loads(stripped)
    except json.JSONDecodeError as exc:
        raise GatewayClientError(f"model completion is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise GatewayClientError("model completion must be a JSON object")
    return value


def cmd_health(args: argparse.Namespace) -> int:
    client = InferenceGatewayClient(args.socket, timeout=args.timeout)
    print(json.dumps(client.health(), indent=2, sort_keys=True))
    return 0


def cmd_complete(args: argparse.Namespace) -> int:
    prompt = sys.stdin.read() if args.prompt == "-" else args.prompt
    client = InferenceGatewayClient(args.socket, timeout=args.timeout)
    response = client.complete(
        [{"role": "user", "content": prompt}],
        task_id=args.task_id,
        max_output_tokens=args.max_output_tokens,
        network_allowed=args.network,
    )
    sys.stdout.write(response["content"])
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Credential-less inference client used inside the scored sandbox"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    health = sub.add_parser("health", help="handshake with the trusted gateway")
    health.add_argument("--socket")
    health.add_argument("--timeout", type=float, default=30.0)
    health.set_defaults(func=cmd_health)

    complete = sub.add_parser("complete", help="request one completion")
    complete.add_argument("--socket")
    complete.add_argument("--timeout", type=float, default=900.0)
    complete.add_argument("--task-id")
    complete.add_argument("--max-output-tokens", type=int, default=8192)
    complete.add_argument("--network", action="store_true")
    complete.add_argument("--prompt", default="-")
    complete.set_defaults(func=cmd_complete)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return int(args.func(args))
    except GatewayClientError as exc:
        print(f"inference gateway client error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
