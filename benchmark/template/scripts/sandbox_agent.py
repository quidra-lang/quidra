#!/usr/bin/env python3
"""Scored sandbox-agent runtime.

This is the agent loop for `worker_mode=sandbox-agent`. It runs *inside* the
attested `/quidra-benchmark` sandbox and is the only tool-capable scored worker
the benchmark recognises. A host-side general-purpose coding agent is not a valid
substitute: it would carry host filesystem, shell and credential reach that the
scored population must not have.

Two properties matter and both are enforced here in code, never by asking the
model to behave:

  * the agent holds no provider credential - every model turn goes through the
    credential-less gateway socket;
  * reads are confined to the Task Packet's declared read paths, writes and
    subprocess working directories to `/quidra-benchmark/work/agents/<agent-id>/`,
    and subprocesses run with `shell=False` and an allowlisted argv[0].

Every refusal is returned to the model as an observation *and* recorded in
`agent_trace.json`, so a run that repeatedly tried to escape its sandbox is
visible to an auditor rather than silently retried.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
TEMPLATE_DIR = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import benchmark  # noqa: E402  (sibling module inside the frozen template)
from gateway_client import (  # noqa: E402
    GatewayClientError,
    GatewayRefusal,
    InferenceGatewayClient,
    parse_model_json,
)

CANONICAL_WORKSPACE = benchmark.CANONICAL_WORKSPACE


class AgentDenied(RuntimeError):
    """A requested action violated the frozen sandbox-agent permissions."""


class AgentFailure(RuntimeError):
    """The run cannot continue (transport, contract or workspace failure)."""


def load_runtime_config(root: Path) -> dict[str, Any]:
    path = root / "template" / "config" / "sandbox_agent.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    if config.get("schema_version") != 1:
        raise AgentFailure(f"unsupported sandbox agent config schema: {path}")
    return config


# --------------------------------------------------------------------------
# Permission enforcement
# --------------------------------------------------------------------------


def _real(path: Path) -> Path:
    return Path(os.path.realpath(os.fspath(path)))


def _is_within(candidate: Path, container: Path) -> bool:
    """True when `candidate` is `container` or lies beneath it, symlinks resolved.

    Both the lexical and the resolved form must agree, so a symlink planted inside
    the writable agent directory cannot be used to reach the rest of the workspace.
    """
    try:
        benchmark.lexical_absolute(candidate).relative_to(benchmark.lexical_absolute(container))
    except ValueError:
        return False
    try:
        _real(candidate).relative_to(_real(container))
    except ValueError:
        return False
    return True


class Permissions:
    """Frozen read/write/exec authority derived from one Task Packet."""

    def __init__(self, root: Path, agent_dir: Path, task: dict[str, Any],
                 config: dict[str, Any]) -> None:
        self.root = benchmark.lexical_absolute(root)
        self.agent_dir = benchmark.lexical_absolute(agent_dir)
        self.config = config
        self.read_roots = [Path(p) for p in task.get("read_paths", [])]
        # A worker may always re-read what it has produced itself.
        self.read_roots.append(self.agent_dir)
        self.reserved_names = {
            "task.json", "validation.json", "worker_response.json", "agent_trace.json"
        }
        self.total_written = 0

    def _absolute(self, raw: str, *, base: Path) -> Path:
        if not isinstance(raw, str) or not raw:
            raise AgentDenied("path must be a non-empty string")
        if "\x00" in raw:
            raise AgentDenied("path may not contain NUL")
        candidate = Path(raw)
        if not candidate.is_absolute():
            candidate = base / candidate
        absolute = benchmark.lexical_absolute(candidate)
        if not _is_within(absolute, self.root):
            raise AgentDenied(
                f"path escapes the sandbox workspace {self.root.as_posix()}: {raw}"
            )
        return absolute

    def resolve_read(self, raw: str) -> Path:
        target = self._absolute(raw, base=self.agent_dir)
        for allowed in self.read_roots:
            if _is_within(target, allowed):
                return target
        raise AgentDenied(
            f"read is outside the Task Packet's declared read paths: {target.as_posix()}"
        )

    def resolve_write(self, raw: str) -> Path:
        target = self._absolute(raw, base=self.agent_dir)
        if not _is_within(target, self.agent_dir):
            raise AgentDenied(
                "writes are limited to this worker's own directory "
                f"{self.agent_dir.as_posix()}: {target.as_posix()}"
            )
        relative = target.relative_to(self.agent_dir).as_posix()
        if relative in self.reserved_names:
            raise AgentDenied(f"{relative} is runner-owned and may not be written by the worker")
        return target

    def resolve_cwd(self, raw: str | None) -> Path:
        if not raw:
            return self.agent_dir
        target = self._absolute(raw, base=self.agent_dir)
        if not _is_within(target, self.agent_dir):
            raise AgentDenied(
                "a subprocess working directory must stay inside "
                f"{self.agent_dir.as_posix()}: {target.as_posix()}"
            )
        if not target.is_dir():
            raise AgentDenied(f"working directory does not exist: {target.as_posix()}")
        return target

    def resolve_argv(self, argv: Any, env_path: str) -> list[str]:
        if not isinstance(argv, list) or not argv:
            raise AgentDenied("argv must be a non-empty array of strings")
        if not all(isinstance(item, str) for item in argv):
            raise AgentDenied("every argv entry must be a string")
        if any("\x00" in item for item in argv):
            raise AgentDenied("argv may not contain NUL")

        program = argv[0]
        allowlist = set(self.config["exec_allowlist"])
        prefixes = list(self.config.get("exec_allowed_absolute_prefixes", []))

        if "/" in program:
            absolute = benchmark.lexical_absolute(Path(program))
            if not any(absolute.as_posix().startswith(prefix) for prefix in prefixes):
                raise AgentDenied(
                    f"executable path is not in the frozen allowlist: {program}"
                )
            if not _is_within(absolute, self.root):
                raise AgentDenied(f"executable escapes the sandbox workspace: {program}")
            if not os.access(absolute, os.X_OK):
                raise AgentDenied(f"executable is missing or not executable: {program}")
            return [str(absolute), *argv[1:]]

        if program not in allowlist:
            raise AgentDenied(
                f"{program!r} is not in the frozen sandbox-agent executable allowlist"
            )
        resolved = shutil.which(program, path=env_path)
        if not resolved:
            raise AgentDenied(f"{program!r} is not installed in this sandbox image")
        return [resolved, *argv[1:]]

    def account_write(self, size: int) -> None:
        limit = int(self.config["max_write_bytes"])
        total_limit = int(self.config["max_total_write_bytes"])
        if size > limit:
            raise AgentDenied(f"single write exceeds the frozen limit {limit} bytes")
        if self.total_written + size > total_limit:
            raise AgentDenied(f"total writes exceed the frozen limit {total_limit} bytes")
        self.total_written += size


# --------------------------------------------------------------------------
# Actions
# --------------------------------------------------------------------------


def truncate(text: str, limit: int) -> tuple[str, bool]:
    encoded = text.encode("utf-8")
    if len(encoded) <= limit:
        return text, False
    return encoded[:limit].decode("utf-8", "ignore"), True


def act_list_dir(action: dict[str, Any], perms: Permissions) -> dict[str, Any]:
    target = perms.resolve_read(str(action.get("path", "")))
    if not target.is_dir():
        raise AgentDenied(f"not a directory: {target.as_posix()}")
    limit = int(perms.config["max_list_entries"])
    entries = []
    for child in sorted(target.iterdir()):
        if len(entries) >= limit:
            break
        entries.append({
            "name": child.name,
            "kind": "dir" if child.is_dir() else ("symlink" if child.is_symlink() else "file"),
            "bytes": child.stat().st_size if child.is_file() and not child.is_symlink() else None,
        })
    return {"ok": True, "path": target.as_posix(), "entries": entries}


def act_read_file(action: dict[str, Any], perms: Permissions) -> dict[str, Any]:
    target = perms.resolve_read(str(action.get("path", "")))
    if target.is_symlink():
        raise AgentDenied(f"refusing to read through a symlink: {target.as_posix()}")
    if not target.is_file():
        raise AgentDenied(f"not a readable file: {target.as_posix()}")
    limit = min(
        int(perms.config["max_read_bytes"]),
        int(action.get("max_bytes") or perms.config["max_read_bytes"]),
    )
    with target.open("rb") as handle:
        data = handle.read(limit + 1)
    truncated = len(data) > limit
    data = data[:limit]
    if b"\x00" in data:
        raise AgentDenied(f"file is binary and cannot be read as text: {target.as_posix()}")
    return {
        "ok": True,
        "path": target.as_posix(),
        "truncated": truncated,
        "content": data.decode("utf-8", "replace"),
    }


def act_write_file(action: dict[str, Any], perms: Permissions) -> dict[str, Any]:
    target = perms.resolve_write(str(action.get("path", "")))
    content = action.get("content")
    if not isinstance(content, str):
        raise AgentDenied("write_file content must be a UTF-8 string")
    data = content.encode("utf-8")
    perms.account_write(len(data))
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        raise AgentDenied(f"refusing to write through a symlink: {target.as_posix()}")
    target.write_bytes(data)
    return {
        "ok": True,
        "path": target.as_posix(),
        "bytes": len(data),
        "sha256": benchmark.sha256_bytes(data),
    }


def act_run(action: dict[str, Any], perms: Permissions) -> dict[str, Any]:
    cwd = perms.resolve_cwd(action.get("cwd"))
    env = benchmark.sanitized_subprocess_env(perms.root, cwd)
    argv = perms.resolve_argv(action.get("argv"), env["PATH"])
    timeout = int(perms.config["exec_timeout_seconds"])
    try:
        completed = subprocess.run(
            argv,
            shell=False,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "argv": argv,
            "timed_out": True,
            "timeout_seconds": timeout,
        }
    except OSError as exc:
        raise AgentDenied(f"subprocess could not start: {exc}") from exc
    cap = int(perms.config["exec_output_bytes"])
    stdout, stdout_truncated = truncate(completed.stdout, cap)
    stderr, stderr_truncated = truncate(completed.stderr, cap)
    return {
        "ok": completed.returncode == 0,
        "argv": argv,
        "cwd": cwd.as_posix(),
        "exit_code": completed.returncode,
        "stdout": stdout,
        "stderr": stderr,
        "truncated": stdout_truncated or stderr_truncated,
    }


ACTIONS = {
    "list_dir": act_list_dir,
    "read_file": act_read_file,
    "write_file": act_write_file,
    "run": act_run,
}


# --------------------------------------------------------------------------
# Prompting
# --------------------------------------------------------------------------


def runtime_contract_prompt(perms: Permissions, task: dict[str, Any],
                            config: dict[str, Any]) -> str:
    read_lines = "\n".join(f"- {Path(p).as_posix()}" for p in task.get("read_paths", []))
    outputs = task.get("expected_outputs") or []
    output_lines = "\n".join(f"- {Path(p).as_posix()}" for p in outputs) or "- defined by this task"
    allowlist = ", ".join(config["exec_allowlist"])
    return f"""# Sandbox agent runtime contract

You are running as a `sandbox-agent` worker inside the benchmark sandbox rooted at
`{CANONICAL_WORKSPACE.as_posix()}`. You have no provider credentials, no host
filesystem access, no shell, and no agent-platform tools. The only capabilities
that exist are the actions below, and the runtime enforces every limit in code.

Reply with exactly one JSON object per turn and nothing else. No prose, no
Markdown fence, no multiple objects.

Available actions:

```
{{"action":"list_dir","path":"<path>"}}
{{"action":"read_file","path":"<path>","max_bytes":65536}}
{{"action":"write_file","path":"<relative-path>","content":"<UTF-8 text>"}}
{{"action":"run","argv":["<program>","<arg>"],"cwd":"<relative-dir>"}}
{{"action":"final","summary":"<what you produced>"}}
```

Permissions:

- Readable paths (nothing else is readable):
{read_lines or "- (none beyond your own directory)"}
- Writable directory (the only writable location): `{perms.agent_dir.as_posix()}`
- `write_file` and `run` paths are resolved relative to that directory.
- `run` executes with `shell=False`. Permitted programs: {allowlist}.
- Network access: {"allowed through the inference gateway only" if task.get("network_allowed") else "disabled"}.
- Maximum turns: {config['max_turns']}.

Expected outputs before you send `final`:
{output_lines}

Each of your turns is answered with a JSON observation. A denied action returns
`{{"ok":false,"denied":"..."}}`; that is a permission boundary, not a hint to try
another path. Do the task within these permissions, then send `final`.
"""


# --------------------------------------------------------------------------
# Loop
# --------------------------------------------------------------------------


def run_agent(args: argparse.Namespace) -> int:
    root = benchmark.lexical_absolute(Path(args.workspace))
    agent_dir = benchmark.require_under(root / "work" / "agents" / args.id, root)
    task_path = agent_dir / "task.json"
    if not task_path.is_file():
        raise AgentFailure(f"Task Packet metadata is missing: {task_path}")
    task = benchmark.json_load(task_path)
    if task.get("worker_mode") != "sandbox-agent":
        raise AgentFailure(
            "sandbox_agent.py only runs sandbox-agent work units; "
            f"{args.id} is {task.get('worker_mode')!r}"
        )

    config = load_runtime_config(root)
    sampling = benchmark.sampling_config(root)
    perms = Permissions(root, agent_dir, task, config)
    packet = benchmark.render_prompt_components(
        task["prompt_components"], task["prompt_sha256"]
    ).decode("utf-8")

    client = InferenceGatewayClient(args.socket, timeout=float(args.timeout))
    health = client.health()
    if health.get("host_tools_exposed"):
        raise AgentFailure("gateway reports an exposed host tool surface; refusing to run")

    messages: list[dict[str, str]] = [
        {"role": "system", "content": runtime_contract_prompt(perms, task, config)},
        {"role": "user", "content": packet},
    ]

    trace: list[dict[str, Any]] = []
    denials: list[dict[str, Any]] = []
    usage = {"input_tokens": 0, "output_tokens": 0, "calls": 0}
    max_turns = int(config["max_turns"])
    protocol_errors = 0
    max_protocol_errors = int(config["max_consecutive_protocol_errors"])
    final_summary: str | None = None
    stop_reason = "max_turns_exhausted"

    for turn in range(1, max_turns + 1):
        try:
            response = client.complete(
                messages,
                task_id=args.id,
                max_output_tokens=int(args.max_output_tokens),
                temperature=sampling["temperature"],
                network_allowed=bool(task.get("network_allowed")),
            )
        except GatewayRefusal as exc:
            raise AgentFailure(f"inference gateway refused this worker's request: {exc}") from exc
        except GatewayClientError as exc:
            raise AgentFailure(f"inference transport failure: {exc}") from exc

        usage["calls"] += 1
        usage["input_tokens"] += int(response.get("usage", {}).get("input_tokens", 0) or 0)
        usage["output_tokens"] += int(response.get("usage", {}).get("output_tokens", 0) or 0)
        completion = response["content"]
        messages.append({"role": "assistant", "content": completion})

        try:
            action = parse_model_json(completion)
            protocol_errors = 0
        except GatewayClientError as exc:
            protocol_errors += 1
            trace.append({
                "turn": turn,
                "action": None,
                "protocol_error": str(exc),
                "completion_sha256": benchmark.sha256_bytes(completion.encode("utf-8")),
            })
            if protocol_errors >= max_protocol_errors:
                stop_reason = "protocol_contract_violated"
                break
            messages.append({
                "role": "user",
                "content": json.dumps({"ok": False, "protocol_error": str(exc)}),
            })
            continue

        name = action.get("action")
        if name == "final":
            final_summary = str(action.get("summary", ""))
            stop_reason = "final"
            trace.append({"turn": turn, "action": "final"})
            break

        if name not in ACTIONS:
            observation: dict[str, Any] = {
                "ok": False,
                "denied": f"unknown action {name!r}; permitted actions are "
                          + ", ".join([*ACTIONS, "final"]),
            }
            denials.append({"turn": turn, "action": name, "reason": observation["denied"]})
        else:
            try:
                observation = ACTIONS[name](action, perms)
            except AgentDenied as exc:
                observation = {"ok": False, "denied": str(exc)}
                denials.append({"turn": turn, "action": name, "reason": str(exc)})
            except OSError as exc:
                observation = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}

        recorded = dict(observation)
        if isinstance(recorded.get("content"), str):
            recorded["content"] = f"<{len(recorded['content'])} chars elided from trace>"
        trace.append({"turn": turn, "action": name, "observation": recorded})

        rendered, _ = truncate(
            json.dumps(observation, sort_keys=True), int(config["max_observation_bytes"])
        )
        messages.append({"role": "user", "content": rendered})

    expected = []
    missing = []
    for raw in task.get("expected_outputs", []):
        path = benchmark.require_under(Path(raw), agent_dir)
        expected.append(path.as_posix())
        if not path.is_file():
            missing.append(path.as_posix())

    result = {
        "schema_version": 1,
        "agent_id": args.id,
        "worker_mode": "sandbox-agent",
        "runtime": config["runtime"],
        "prompt_sha256": task["prompt_sha256"],
        "gateway": {
            "protocol": health.get("gateway"),
            "provider": health.get("provider", {}).get("id"),
            "credential_less_client": health.get("credential_less_client"),
            "host_tools_exposed": health.get("host_tools_exposed"),
            "network_policy": health.get("network_policy"),
        },
        "network_allowed": bool(task.get("network_allowed")),
        "sampling": sampling,
        "stop_reason": stop_reason,
        "turns": len(trace),
        "usage": usage,
        "final_summary": final_summary,
        "expected_outputs": expected,
        "missing_outputs": missing,
        "denied_actions": denials,
        "trace": trace,
        "finished_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    benchmark.json_dump(agent_dir / "agent_trace.json", result)
    print(json.dumps({k: v for k, v in result.items() if k != "trace"}, indent=2))

    if stop_reason != "final" or missing:
        return 2
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run one sandbox-agent work unit inside the benchmark sandbox"
    )
    parser.add_argument("--workspace", default=str(CANONICAL_WORKSPACE))
    parser.add_argument("--id", required=True, help="agent id under work/agents/")
    parser.add_argument("--socket", help="inference gateway socket (defaults to the frozen path)")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--max-output-tokens", type=int, default=8192)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return run_agent(args)
    except (AgentFailure, benchmark.BenchmarkError) as exc:
        print(f"sandbox agent error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
