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
    and subprocesses run with `shell=False` and an argv[0] that is allowlisted
    or an executable the worker built inside its own directory.

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
import re
import shutil
import time
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
    completion_problem,
    parse_model_json_batch,
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
            "task.json", "validation.json", "worker_response.json", "agent_trace.json",
            "agent_trace.partial.json", "resume_trace.json", "trial_call_journal.json",
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
        if relative == "trials" or relative.startswith("trials/"):
            raise AgentDenied(
                "trials/ is runtime-owned: trial prompts and completions are recorded "
                "there verbatim by the runtime and may not be written by the worker"
            )
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
            # A relative program path names something the worker built in its
            # own directory, exactly as write_file and cwd are resolved there.
            candidate = Path(program)
            absolute = benchmark.lexical_absolute(
                candidate if candidate.is_absolute() else self.agent_dir / candidate
            )
            in_prefix = any(absolute.as_posix().startswith(prefix) for prefix in prefixes)
            # An executable the worker compiled inside its own directory may be
            # run directly. Refusing it only forced the model through a python
            # wrapper that spawned the same binary, and hid the language's real
            # toolchain invocations from the trace that certifies them.
            in_own_dir = _is_within(absolute, self.agent_dir)
            if not (in_prefix or in_own_dir):
                raise AgentDenied(
                    f"executable path is not in the frozen allowlist: {program}"
                )
            if not _is_within(absolute, self.root):
                raise AgentDenied(f"executable escapes the sandbox workspace: {program}")
            if absolute.is_symlink():
                raise AgentDenied(f"refusing to run through a symlink: {program}")
            if not absolute.is_file() or not os.access(absolute, os.X_OK):
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
    if target.is_dir():
        raise AgentDenied(
            f"not a readable file: {target.as_posix()} is a directory; use list_dir on it"
        )
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


_WORKSPACE_PATH_RE = re.compile(r"/quidra-benchmark(?:/[^\\\"'\s,)]*)?")


def _sandbox_subprocess_allowed_roots(perms: Permissions) -> list[Path]:
    roots = [benchmark.lexical_absolute(path) for path in perms.read_roots]
    target_build = perms.root / "work" / "root" / "target-build"
    if target_build.exists():
        roots.append(benchmark.lexical_absolute(target_build))
    return roots


def _sandbox_subprocess_access_problem(
    trace_text: str, perms: Permissions
) -> str | None:
    """Reject a subprocess that crossed the Task Packet filesystem boundary.

    argv/cwd validation alone is not a filesystem sandbox: an allowed
    interpreter could otherwise read a sibling worker or the gateway socket.
    Production therefore traces file syscalls and accepts the action only when
    every benchmark-workspace path actually touched is inside the same declared
    read roots enforced by read_file plus the worker's own directory.
    """
    allowed = _sandbox_subprocess_allowed_roots(perms)
    for line in trace_text.splitlines():
        # A procfs-root alias still contains the canonical workspace suffix,
        # so the same matcher catches /proc/.../root/quidra-benchmark/... .
        for raw in _WORKSPACE_PATH_RE.findall(line):
            candidate = raw.rstrip(".,:;")
            try:
                path = benchmark.lexical_absolute(Path(candidate))
            except Exception:
                return f"unparseable workspace path in subprocess trace: {candidate}"
            if any(_is_within(path, root) for root in allowed):
                continue
            return f"undeclared workspace access: {candidate}"
    return None


def _runtime_state_hashes(perms: Permissions) -> dict[str, str | None]:
    protected = (
        "task.json",
        "trial_call_journal.json",
        "resume_trace.json",
    )
    out: dict[str, str | None] = {}
    for name in protected:
        path = perms.agent_dir / name
        out[name] = benchmark.sha256_file(path) if path.is_file() else None
    return out


def act_run(action: dict[str, Any], perms: Permissions) -> dict[str, Any]:
    cwd = perms.resolve_cwd(action.get("cwd"))
    env = benchmark.sanitized_subprocess_env(perms.root, cwd)
    # HOME/TMPDIR are private to this worker instead of shared by the whole
    # scored sandbox, preventing accidental cross-unit state.
    process_state = perms.agent_dir / ".subprocess"
    process_home = process_state / "home"
    process_tmp = process_state / "tmp"
    process_home.mkdir(parents=True, exist_ok=True)
    process_tmp.mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(process_home),
        "TMPDIR": str(process_tmp),
        "TMP": str(process_tmp),
        "TEMP": str(process_tmp),
    })
    argv = perms.resolve_argv(action.get("argv"), env["PATH"])
    timeout = int(perms.config["exec_timeout_seconds"])
    protected_before = _runtime_state_hashes(perms)

    command = argv
    trace_path: Path | None = None
    if benchmark.lexical_absolute(perms.root) == benchmark.lexical_absolute(CANONICAL_WORKSPACE):
        strace = Path("/usr/bin/strace")
        if not strace.is_file():
            raise AgentFailure(
                "sandbox filesystem audit is unavailable: /usr/bin/strace is missing"
            )
        audit_dir = (
            perms.root / "work" / "root" / "subprocess-audit" / perms.agent_dir.name
        )
        audit_dir.mkdir(parents=True, exist_ok=True)
        trace_path = audit_dir / f"trace-{os.getpid()}-{time.time_ns()}.log"
        command = [
            str(strace),
            "-f",
            "-qq",
            "-s",
            "4096",
            "-e",
            "trace=%file,connect",
            "-o",
            str(trace_path),
            "--",
            *argv,
        ]
    timed_out = False
    completed: subprocess.CompletedProcess[str] | None = None
    try:
        completed = subprocess.run(
            command,
            shell=False,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        timed_out = True
    except OSError as exc:
        raise AgentDenied(f"subprocess could not start: {exc}") from exc

    if trace_path is not None:
        if not trace_path.is_file():
            raise AgentFailure("sandbox filesystem audit produced no syscall trace")
        trace_text = trace_path.read_text(encoding="utf-8", errors="replace")
        problem = _sandbox_subprocess_access_problem(trace_text, perms)
        trace_path.unlink(missing_ok=True)
        if problem is not None:
            raise AgentFailure(f"sandbox filesystem policy violation: {problem}")

    protected_after = _runtime_state_hashes(perms)
    if protected_after != protected_before:
        changed = sorted(
            name
            for name in protected_before
            if protected_before.get(name) != protected_after.get(name)
        )
        raise AgentFailure(
            "sandbox filesystem policy violation: subprocess modified runner-owned "
            + ", ".join(changed)
        )

    if timed_out:
        return {
            "ok": False,
            "argv": argv,
            "timed_out": True,
            "timeout_seconds": timeout,
        }
    if completed is None:
        raise AgentFailure("subprocess ended without a completion record")

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


class Trials:
    """Fresh, isolated model sessions for a scored experiment, enforced in code.

    A replicated cell needs N independent trials, each with its own prompt chain
    and up to R repair turns, and nothing from this agent's conversation or from
    another trial may leak into it. The runtime, not the model, owns every
    trial's message list: the agent supplies prompts and repair messages, the
    runtime builds the session from its own records, sends it through the same
    credential-less gateway, and preserves every prompt and completion verbatim.
    """

    def __init__(self, root: Path, task: dict[str, Any], agent_id: str,
                 config: dict[str, Any], client: InferenceGatewayClient,
                 max_output_tokens: int) -> None:
        self.client = client
        self.root = root
        self.agent_id = agent_id
        self.agent_dir = root / "work" / "agents" / agent_id
        self.evaluation = str(task.get("evaluation") or "")
        assigned_languages = [
            str(value) for value in (task.get("assigned_languages") or [])
        ]
        self.proficiency_language: str | None = None
        if self.evaluation == "llm_proficiency":
            if len(assigned_languages) != 1:
                raise AgentFailure(
                    "LLM Proficiency runtime requires exactly one assigned language"
                )
            self.proficiency_language = assigned_languages[0]
        self.network_allowed = bool(task.get("network_allowed"))
        self.max_output_tokens = max_output_tokens
        self.budget = 0
        manifest_path = root / "work" / "root" / "manifest.json"
        if manifest_path.is_file():
            for unit in benchmark.json_load(manifest_path).get("work_units", []):
                if unit.get("assigned_agent_id") == agent_id:
                    self.budget = int(unit.get("max_llm_calls", 0) or 0)
                    break
        primary = benchmark.json_load(root / "template" / "config" / "primary.json")
        section = primary.get(str(task.get("evaluation") or ""), {}) or {}
        isolation = primary.get("worker_isolation", {}) or {}
        self.max_repairs = int(section.get("max_repair_turns", 3) or 0)
        multiplier = int(isolation.get("trial_turn_multiplier", 3) or 3)
        self.max_batch = max(1, int(isolation.get("max_trial_batch", 16) or 16))
        base_turns = int(config["max_turns"])
        self.max_turns = max(base_turns, self.budget * multiplier) if self.budget else base_turns
        self.used = 0
        self.sessions: dict[str, dict[str, Any]] = {}
        self.required_trial_ids = (
            benchmark.proficiency_required_trial_ids(root)
            if self.evaluation == "llm_proficiency"
            else []
        )
        self.required_trial_id_set = set(self.required_trial_ids)
        # Every prompt and completion is also written verbatim to disk, under a
        # directory the worker can read but not write. The agent processes trial
        # output in bulk with its own scripts instead of copying completions out
        # of observations one turn at a time, and an auditor reads the record
        # without going through the trace.
        self.records_dir = root / "work" / "agents" / agent_id / "trials"
        self.call_journal_path = self.agent_dir / "trial_call_journal.json"
        self.trusted_verification_dir = (
            root / "work" / "root" / "proficiency-verification" / agent_id
        )
        self.trusted_call_checkpoint_dir = (
            root / "work" / "root" / "trial-checkpoints" / agent_id
        )
        # The first durable write after a scored provider reply lives outside
        # worker authority. Recovery can rebuild all worker-side trial state from
        # this record without buying the same scored call again.
        self._recover_trusted_call_checkpoints()
        self._restore_sessions()

    def _trusted_call_checkpoint_path(self, trial_id: str, call: int) -> Path:
        return self.trusted_call_checkpoint_dir / trial_id / f"call_{call:02d}.json"

    def _write_trusted_call_checkpoint(
        self, trial_id: str, record: dict[str, Any]
    ) -> None:
        """Atomically persist one paid scored reply outside worker authority."""
        call = int(record.get("call", 0) or 0)
        if call <= 0:
            raise AgentFailure(
                f"refusing trusted checkpoint with invalid call number: {call}"
            )
        benchmark.json_dump(
            self._trusted_call_checkpoint_path(trial_id, call),
            {
                "schema_version": 1,
                "kind": "paid-trial-call-checkpoint-v1",
                "agent_id": self.agent_id,
                "evaluation": self.evaluation,
                "trial_id": trial_id,
                "call": call,
                "record": record,
            },
        )

    def _recover_trusted_call_checkpoints(self) -> None:
        """Reconstruct session/journal state from trusted paid-call records."""
        if not self.trusted_call_checkpoint_dir.is_dir():
            return

        recovered_by_trial: dict[str, list[dict[str, Any]]] = {}
        for trusted_trial_dir in sorted(
            p for p in self.trusted_call_checkpoint_dir.iterdir() if p.is_dir()
        ):
            trial_id = self._valid_id(trusted_trial_dir.name)
            self._require_allowed_trial_id(trial_id)
            checkpoint_records: list[dict[str, Any]] = []

            for checkpoint_path in sorted(trusted_trial_dir.glob("call_*.json")):
                try:
                    checkpoint = benchmark.json_load(checkpoint_path)
                except (OSError, json.JSONDecodeError) as exc:
                    raise AgentFailure(
                        f"trusted trial checkpoint is unreadable: {checkpoint_path}: {exc}"
                    ) from exc
                call = int(checkpoint.get("call", 0) or 0)
                record = checkpoint.get("record")
                if (
                    checkpoint.get("schema_version") != 1
                    or checkpoint.get("kind") != "paid-trial-call-checkpoint-v1"
                    or checkpoint.get("agent_id") != self.agent_id
                    or checkpoint.get("evaluation") != self.evaluation
                    or checkpoint.get("trial_id") != trial_id
                    or call <= 0
                    or checkpoint_path.name != f"call_{call:02d}.json"
                    or not isinstance(record, dict)
                    or int(record.get("call", 0) or 0) != call
                ):
                    raise AgentFailure(
                        f"trusted trial checkpoint metadata is invalid: {checkpoint_path}"
                    )
                record = dict(record)
                prompt = record.get("prompt")
                completion = record.get("completion")
                if not isinstance(prompt, str) or not isinstance(completion, str):
                    raise AgentFailure(
                        f"trusted trial checkpoint lacks verbatim text: {checkpoint_path}"
                    )
                if (
                    benchmark.sha256_bytes(prompt.encode("utf-8"))
                    != record.get("prompt_sha256")
                    or benchmark.sha256_bytes(completion.encode("utf-8"))
                    != record.get("completion_sha256")
                ):
                    raise AgentFailure(
                        f"trusted trial checkpoint text hash changed: {checkpoint_path}"
                    )

                expected_prompt_rel = (
                    Path("trials") / trial_id / f"prompt_{call:02d}.txt"
                ).as_posix()
                expected_completion_rel = (
                    Path("trials") / trial_id / f"completion_{call:02d}.txt"
                ).as_posix()
                if (
                    record.get("prompt_path") != expected_prompt_rel
                    or record.get("completion_path") != expected_completion_rel
                ):
                    raise AgentFailure(
                        f"trusted trial checkpoint path contract changed: {checkpoint_path}"
                    )

                # Recreate worker-readable verbatim evidence if the process died
                # before those secondary files were committed.
                for relative, value in (
                    (expected_prompt_rel, prompt),
                    (expected_completion_rel, completion),
                ):
                    target = self.agent_dir / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if target.exists():
                        if not target.is_file() or target.read_text(encoding="utf-8") != value:
                            raise AgentFailure(
                                f"trusted paid-call file conflicts with checkpoint: {target}"
                            )
                    else:
                        tmp = target.with_name(target.name + ".trusted-recovery.tmp")
                        tmp.write_text(value, encoding="utf-8")
                        os.replace(tmp, target)

                # Verification is free/deterministic. If a crash happened before
                # it was committed, replay it from the preserved paid completion.
                if self.evaluation == "llm_proficiency":
                    if self.proficiency_language is None:
                        raise AgentFailure(
                            "trusted Proficiency checkpoint has no assigned language"
                        )
                    verification = None
                    raw_verification_path = record.get("verification_path")
                    if isinstance(raw_verification_path, str) and raw_verification_path:
                        candidate = self.root / raw_verification_path
                        if (
                            _is_within(candidate, self.trusted_verification_dir)
                            and candidate.is_file()
                        ):
                            loaded = benchmark.json_load(candidate)
                            if (
                                benchmark.proficiency_verification_summary(loaded)
                                == record.get("verification")
                            ):
                                verification = loaded
                    if verification is None:
                        verify_dir = (
                            self.trusted_verification_dir
                            / trial_id
                            / f"call_{call:02d}"
                        )
                        try:
                            verification = benchmark.verify_proficiency_completion(
                                self.root,
                                self.proficiency_language,
                                trial_id,
                                completion,
                                verify_dir,
                            )
                        except benchmark.BenchmarkError as exc:
                            raise AgentFailure(
                                f"trusted Proficiency recovery verifier failed: {exc}"
                            ) from exc
                        record["verification_path"] = (
                            verify_dir / "verification.json"
                        ).relative_to(self.root).as_posix()
                        record["verification"] = (
                            benchmark.proficiency_verification_summary(verification)
                        )
                        self._write_trusted_call_checkpoint(trial_id, record)

                checkpoint_records.append(record)

            checkpoint_records.sort(key=lambda row: int(row["call"]))
            if len({int(row["call"]) for row in checkpoint_records}) != len(
                checkpoint_records
            ):
                raise AgentFailure(
                    f"trusted trial checkpoints duplicate a call for {trial_id!r}"
                )
            if checkpoint_records:
                recovered_by_trial[trial_id] = checkpoint_records

        # Merge trusted records into session.json. Existing legacy calls remain
        # valid only when they precede or exactly match the trusted records.
        for trial_id, checkpoint_records in sorted(recovered_by_trial.items()):
            trial_dir = self.records_dir / trial_id
            trial_dir.mkdir(parents=True, exist_ok=True)
            session_path = trial_dir / "session.json"
            session_records: list[dict[str, Any]] = []
            if session_path.is_file():
                try:
                    session = benchmark.json_load(session_path)
                except (OSError, json.JSONDecodeError) as exc:
                    raise AgentFailure(
                        f"trial session {trial_id!r} is unreadable: {exc}"
                    ) from exc
                if (
                    session.get("schema_version") != 1
                    or session.get("trial_id") != trial_id
                    or not isinstance(session.get("calls"), list)
                ):
                    raise AgentFailure(
                        f"trial session {trial_id!r} has invalid session metadata"
                    )
                session_records = list(session["calls"])

            for record in checkpoint_records:
                call = int(record["call"])
                if call <= len(session_records):
                    if session_records[call - 1] != record:
                        raise AgentFailure(
                            f"trial session disagrees with trusted checkpoint "
                            f"for {trial_id!r} call {call}"
                        )
                    continue
                if call != len(session_records) + 1:
                    raise AgentFailure(
                        f"trusted checkpoint for {trial_id!r} has a gap before call {call}"
                    )
                session_records.append(record)

            benchmark.json_dump(
                session_path,
                {
                    "schema_version": 1,
                    "trial_id": trial_id,
                    "calls": session_records,
                },
            )

        # Finally ratchet trusted records into the compact journal. Existing
        # journal-only legacy calls can precede new trusted records.
        journal_counts: dict[str, int] = {}
        for row in self._journal_rows():
            if not isinstance(row, dict):
                raise AgentFailure("trial call journal contains a non-object call")
            trial_id = self._valid_id(row.get("trial_id"))
            call = int(row.get("call", 0) or 0)
            expected = journal_counts.get(trial_id, 0) + 1
            if call != expected:
                raise AgentFailure(
                    f"trial call journal is non-sequential for {trial_id!r}: "
                    f"got {call}, expected {expected}"
                )
            journal_counts[trial_id] = call

        for trial_id, records in sorted(recovered_by_trial.items()):
            recorded = int(journal_counts.get(trial_id, 0) or 0)
            for record in records:
                call = int(record["call"])
                if call <= recorded:
                    continue
                if call != recorded + 1:
                    raise AgentFailure(
                        f"trusted checkpoint for {trial_id!r} cannot bridge "
                        f"journal call {recorded} to call {call}"
                    )
                self._checkpoint_call(trial_id, record)
                recorded = call
                journal_counts[trial_id] = call

    def _resumed_call_counts(self) -> dict[str, int]:
        """Accepted scored calls from the finest trusted runtime checkpoint.

        New runs journal each provider call atomically after its verbatim session
        record (and, for Proficiency, trusted verification) reaches disk. Older
        attempts have only the action-level audit trace, which remains a safe
        fallback. The journal therefore improves resume granularity without
        invalidating or weakening any previously certified result.
        """
        if self.call_journal_path.is_file():
            try:
                journal = benchmark.json_load(self.call_journal_path)
            except (OSError, json.JSONDecodeError) as exc:
                raise AgentFailure(f"trial_call_journal.json is unreadable: {exc}") from exc
            if journal.get("schema_version") != 1:
                raise AgentFailure("trial_call_journal.json has unsupported schema_version")
            counts: dict[str, int] = {}
            for row in journal.get("calls", []) or []:
                if not isinstance(row, dict):
                    raise AgentFailure("trial_call_journal.json contains a non-object call")
                trial_id = self._valid_id(row.get("trial_id"))
                call = int(row.get("call", 0) or 0)
                expected = counts.get(trial_id, 0) + 1
                if call != expected:
                    raise AgentFailure(
                        f"trial call journal is non-sequential for {trial_id!r}: "
                        f"got call {call}, expected {expected}"
                    )
                counts[trial_id] = call
            return counts

        path = self.agent_dir / "resume_trace.json"
        if not path.is_file():
            return {}
        try:
            payload = benchmark.json_load(path)
        except (OSError, json.JSONDecodeError) as exc:
            raise AgentFailure(f"resume_trace.json is unreadable: {exc}") from exc
        counts: dict[str, int] = {}
        for entry in payload.get("trace", []) or []:
            if entry.get("action") not in {"trial_start", "trial_continue"}:
                continue
            observation = entry.get("observation") or {}
            rows = observation.get("trials")
            if not isinstance(rows, list):
                rows = [observation]
            for row in rows:
                if not isinstance(row, dict) or row.get("denied"):
                    continue
                trial_id = row.get("trial_id")
                if isinstance(trial_id, str) and trial_id:
                    counts[trial_id] = counts.get(trial_id, 0) + 1
        return counts

    def _checkpoint_call(self, trial_id: str, record: dict[str, Any]) -> None:
        """Atomically attest one persisted paid call before control returns."""
        payload = {"schema_version": 1, "calls": []}
        if self.call_journal_path.is_file():
            try:
                payload = benchmark.json_load(self.call_journal_path)
            except (OSError, json.JSONDecodeError) as exc:
                raise AgentFailure(f"trial_call_journal.json is unreadable: {exc}") from exc
            if payload.get("schema_version") != 1 or not isinstance(payload.get("calls"), list):
                raise AgentFailure("trial_call_journal.json is malformed")
        calls = list(payload["calls"])
        call = int(record.get("call", 0) or 0)
        previous = sum(1 for row in calls if row.get("trial_id") == trial_id)
        if call != previous + 1:
            raise AgentFailure(
                f"refusing non-sequential journal checkpoint for {trial_id!r}: "
                f"call {call} after {previous} recorded call(s)"
            )
        calls.append({
            "trial_id": trial_id,
            "call": call,
            "action": "trial_start" if call == 1 else "trial_continue",
            "prompt_sha256": record.get("prompt_sha256"),
            "completion_sha256": record.get("completion_sha256"),
            "verification": record.get("verification"),
            "verification_path": record.get("verification_path"),
        })
        benchmark.json_dump(self.call_journal_path, {
            "schema_version": 1,
            "calls": calls,
        })

    def _journal_rows(self) -> list[dict[str, Any]]:
        if not self.call_journal_path.is_file():
            return []
        try:
            payload = benchmark.json_load(self.call_journal_path)
        except (OSError, json.JSONDecodeError) as exc:
            raise AgentFailure(f"trial_call_journal.json is unreadable: {exc}") from exc
        rows = payload.get("calls")
        if payload.get("schema_version") != 1 or not isinstance(rows, list):
            raise AgentFailure("trial_call_journal.json is malformed")
        return rows

    @staticmethod
    def _trace_call_counts(trace: list[dict[str, Any]]) -> dict[str, int]:
        counts: dict[str, int] = {}
        for entry in trace:
            if not isinstance(entry, dict) or entry.get("action") not in {
                "trial_start", "trial_continue"
            }:
                continue
            observation = entry.get("observation") or {}
            rows = observation.get("trials")
            if not isinstance(rows, list):
                rows = [observation]
            for row in rows:
                if not isinstance(row, dict) or row.get("denied"):
                    continue
                trial_id = row.get("trial_id")
                if isinstance(trial_id, str) and trial_id:
                    counts[trial_id] = counts.get(trial_id, 0) + 1
        return counts

    def resume_trace_entries(
        self, trace: list[dict[str, Any]]
    ) -> list[dict[str, Any]]:
        """Audit records for journaled calls whose enclosing batch never returned.

        A process can die after the provider reply and atomic call journal commit
        but before the outer trial_start/trial_continue action is appended to the
        orchestration trace. These runtime-authored recovery entries bridge only
        that gap; ordinary completed actions already in the trace are untouched.
        """
        journal = self._journal_rows()
        if not journal:
            return []
        represented = self._trace_call_counts(trace)
        seen: dict[str, int] = {}
        recovered: list[dict[str, Any]] = []
        next_turn = max(
            [int(row.get("turn", 0) or 0) for row in trace if isinstance(row, dict)]
            or [0]
        )
        for row in journal:
            if not isinstance(row, dict):
                raise AgentFailure("trial call journal contains a non-object call")
            trial_id = self._valid_id(row.get("trial_id"))
            call = int(row.get("call", 0) or 0)
            expected = seen.get(trial_id, 0) + 1
            if call != expected:
                raise AgentFailure(
                    f"trial call journal is non-sequential for {trial_id!r}: "
                    f"got {call}, expected {expected}"
                )
            seen[trial_id] = call
            if call <= represented.get(trial_id, 0):
                continue
            next_turn += 1
            recovered.append({
                "turn": next_turn,
                "action": str(row.get("action") or (
                    "trial_start" if call == 1 else "trial_continue"
                )),
                "observation": {
                    "ok": True,
                    "trial_id": trial_id,
                    "call": call,
                    "resumed_from_runtime_call_journal": True,
                },
                "runtime_recovery": "paid-call-journal-v1",
            })
        return recovered

    def _restore_sessions(self) -> None:
        """Restore paid calls only when runtime records and trusted checkpoints agree."""
        if not self.records_dir.is_dir():
            return
        traced = self._resumed_call_counts()
        if not traced:
            return

        for trial_dir in sorted(p for p in self.records_dir.iterdir() if p.is_dir()):
            trial_id = trial_dir.name
            try:
                self._valid_id(trial_id)
                self._require_allowed_trial_id(trial_id)
            except AgentDenied as exc:
                raise AgentFailure(f"invalid resumed trial directory {trial_id!r}: {exc}") from exc
            allowed = int(traced.get(trial_id, 0) or 0)
            if allowed <= 0:
                continue
            session_path = trial_dir / "session.json"
            if not session_path.is_file():
                raise AgentFailure(f"resumed trial {trial_id!r} has no session.json")
            try:
                payload = benchmark.json_load(session_path)
            except (OSError, json.JSONDecodeError) as exc:
                raise AgentFailure(f"resumed trial {trial_id!r} is unreadable: {exc}") from exc
            if payload.get("schema_version") != 1 or payload.get("trial_id") != trial_id:
                raise AgentFailure(f"resumed trial {trial_id!r} has invalid session metadata")
            original = payload.get("calls")
            if not isinstance(original, list) or len(original) < allowed:
                raise AgentFailure(f"resumed trial {trial_id!r} has fewer calls than its audit trace")
            records = list(original[:allowed])
            journal_rows = {
                (str(row.get("trial_id")), int(row.get("call", 0) or 0)): row
                for row in self._journal_rows()
                if isinstance(row, dict)
            }
            messages: list[dict[str, str]] = []
            trusted: list[dict[str, Any]] = []
            for expected_call, record in enumerate(records, start=1):
                if not isinstance(record, dict) or int(record.get("call", 0) or 0) != expected_call:
                    raise AgentFailure(f"resumed trial {trial_id!r} has non-sequential calls")
                prompt = record.get("prompt")
                completion = record.get("completion")
                if not isinstance(prompt, str) or not isinstance(completion, str):
                    raise AgentFailure(f"resumed trial {trial_id!r} lacks verbatim call text")
                if benchmark.sha256_bytes(prompt.encode("utf-8")) != record.get("prompt_sha256"):
                    raise AgentFailure(f"resumed trial {trial_id!r} prompt hash changed")
                if benchmark.sha256_bytes(completion.encode("utf-8")) != record.get("completion_sha256"):
                    raise AgentFailure(f"resumed trial {trial_id!r} completion hash changed")
                journal_row = journal_rows.get((trial_id, expected_call))
                if self.call_journal_path.is_file():
                    if journal_row is None:
                        raise AgentFailure(
                            f"resumed trial {trial_id!r} call {expected_call} is absent from the call journal"
                        )
                    if (
                        journal_row.get("prompt_sha256") != record.get("prompt_sha256")
                        or journal_row.get("completion_sha256") != record.get("completion_sha256")
                        or journal_row.get("verification") != record.get("verification")
                        or journal_row.get("verification_path") != record.get("verification_path")
                    ):
                        raise AgentFailure(
                            f"resumed trial {trial_id!r} call {expected_call} disagrees with the call journal"
                        )
                for kind, value in (("prompt", prompt), ("completion", completion)):
                    rel = record.get(f"{kind}_path")
                    if not isinstance(rel, str):
                        raise AgentFailure(f"resumed trial {trial_id!r} lacks {kind}_path")
                    disk = self.agent_dir / rel
                    if not _is_within(disk, self.agent_dir) or not disk.is_file():
                        raise AgentFailure(f"resumed trial {trial_id!r} {kind} file is missing")
                    if disk.read_text(encoding="utf-8") != value:
                        raise AgentFailure(f"resumed trial {trial_id!r} {kind} file changed")
                if self.evaluation == "llm_proficiency":
                    rel = record.get("verification_path")
                    if not isinstance(rel, str) or not rel:
                        raise AgentFailure(f"resumed Proficiency trial {trial_id!r} lacks verification")
                    verification_path = self.root / rel
                    if not _is_within(verification_path, self.trusted_verification_dir):
                        raise AgentFailure(f"resumed verification escapes trusted storage: {rel}")
                    if not verification_path.is_file():
                        raise AgentFailure(f"resumed verification is missing: {rel}")
                    verification = benchmark.json_load(verification_path)
                    if benchmark.proficiency_verification_summary(verification) != record.get("verification"):
                        raise AgentFailure(f"resumed verification summary changed for {trial_id!r}")
                    trusted.append(verification)
                messages.append({"role": "user", "content": prompt})
                messages.append({"role": "assistant", "content": completion})

            if self.evaluation == "llm_proficiency":
                if self.proficiency_language is None:
                    raise AgentFailure("resumed Proficiency trial has no assigned language")
                expected_prompt = benchmark.proficiency_expected_prompt(
                    self.root, self.proficiency_language, trial_id
                )
                if not records or records[0].get("prompt") != expected_prompt:
                    raise AgentFailure(f"resumed Proficiency trial {trial_id!r} initial prompt changed")

            if len(original) != allowed:
                benchmark.json_dump(session_path, {
                    "schema_version": 1, "trial_id": trial_id, "calls": records
                })
            self.sessions[trial_id] = {
                "messages": messages,
                "records": records,
                "trusted_verifications": trusted,
            }
            self.used += len(records)

        if self.used > self.budget:
            raise AgentFailure(
                f"resumed trial calls ({self.used}) exceed frozen unit budget ({self.budget})"
            )

    @staticmethod
    def _valid_id(value: Any) -> str:
        if not isinstance(value, str) or not value or len(value) > 96 \
                or not all(ch.isalnum() or ch in "-_." for ch in value):
            raise AgentDenied("trial_id must be a short identifier of letters, digits, '-', '_' or '.'")
        return value

    def _require_allowed_trial_id(self, trial_id: str) -> None:
        if self.required_trial_id_set and trial_id not in self.required_trial_id_set:
            raise AgentDenied(
                f"trial {trial_id!r} is not one of the frozen Primary trial IDs"
            )

    def _call(self, trial_id: str, session: dict[str, Any]) -> dict[str, Any]:
        if self.used >= self.budget:
            raise AgentDenied(
                f"trial budget exhausted: this unit is frozen at {self.budget} trial calls"
            )
        try:
            response = self.client.complete(
                session["messages"],
                task_id=self.agent_id,
                max_output_tokens=self.max_output_tokens,
                network_allowed=self.network_allowed,
                purpose="scored",
            )
        except GatewayRefusal as exc:
            raise AgentFailure(f"inference gateway refused a trial request: {exc}") from exc
        except GatewayClientError as exc:
            raise AgentFailure(f"inference transport failure during a trial: {exc}") from exc
        self.used += 1
        completion = response["content"]
        incomplete = completion_problem(response)
        if incomplete is None and not completion.strip():
            incomplete = "empty: the model returned no text for this trial call"
        prompt_text = session["messages"][-1]["content"]
        call = len(session["records"]) + 1
        trial_dir = self.records_dir / trial_id
        trial_dir.mkdir(parents=True, exist_ok=True)
        prompt_path = trial_dir / f"prompt_{call:02d}.txt"
        completion_path = trial_dir / f"completion_{call:02d}.txt"
        agent_dir = self.records_dir.parent
        record = {
            "call": call,
            "prompt": prompt_text,
            "prompt_sha256": benchmark.sha256_bytes(prompt_text.encode("utf-8")),
            "prompt_path": prompt_path.relative_to(agent_dir).as_posix(),
            "completion": completion,
            "completion_sha256": benchmark.sha256_bytes(completion.encode("utf-8")),
            "completion_path": completion_path.relative_to(agent_dir).as_posix(),
            "stop_reason": response.get("stop_reason"),
            "incomplete": incomplete,
            "usage": response.get("usage", {}),
            "verification": None,
            "verification_path": None,
        }
        # Earliest durable commit after the paid provider reply. This trusted
        # record is self-contained and cannot be written by the scored worker.
        self._write_trusted_call_checkpoint(trial_id, record)

        for path, value in ((prompt_path, prompt_text), (completion_path, completion)):
            tmp = path.with_name(path.name + ".paid-call.tmp")
            tmp.write_text(value, encoding="utf-8")
            os.replace(tmp, path)

        verification = None
        if self.evaluation == "llm_proficiency":
            if self.proficiency_language is None:
                raise AgentFailure("LLM Proficiency target language is unavailable")
            verify_dir = (
                self.trusted_verification_dir / trial_id / f"call_{call:02d}"
            )
            try:
                verification = benchmark.verify_proficiency_completion(
                    self.root,
                    self.proficiency_language,
                    trial_id,
                    completion,
                    verify_dir,
                )
            except benchmark.BenchmarkError as exc:
                raise AgentFailure(
                    f"trusted Proficiency verifier failed: {exc}"
                ) from exc
            record["verification_path"] = (
                verify_dir / "verification.json"
            ).relative_to(self.root).as_posix()
            record["verification"] = (
                benchmark.proficiency_verification_summary(verification)
            )
            session.setdefault("trusted_verifications", []).append(verification)
            # Ratchet the same paid call with free verifier evidence.
            self._write_trusted_call_checkpoint(trial_id, record)

        session["records"].append(record)
        benchmark.json_dump(trial_dir / "session.json", {
            "schema_version": 1,
            "trial_id": trial_id,
            "calls": session["records"],
        })
        # Secondary compact index for trace reconstruction and legacy recovery.
        self._checkpoint_call(trial_id, record)
        session["messages"].append({"role": "assistant", "content": completion})
        verification_view = None
        if isinstance(verification, dict):
            verification_view = benchmark.proficiency_model_visible_feedback(
                verification
            )
        return {
            "ok": incomplete is None,
            "trial_id": trial_id,
            "completion": completion,
            "completion_path": completion_path.relative_to(agent_dir).as_posix(),
            "completion_chars": len(completion),
            "stop_reason": response.get("stop_reason"),
            "incomplete": incomplete,
            "verification": verification_view,
            "repairs_used": len(session["records"]) - 1,
            "repairs_remaining": self.max_repairs - (len(session["records"]) - 1),
            "calls_used": self.used,
            "calls_remaining": self.budget - self.used,
        }

    def _start_one(self, trial_id: str, prompt: Any) -> dict[str, Any]:
        if self.evaluation == "llm_proficiency":
            if self.proficiency_language is None:
                raise AgentFailure("LLM Proficiency target language is unavailable")
            frozen = benchmark.proficiency_expected_prompt(
                self.root, self.proficiency_language, trial_id
            )
            if prompt not in (None, "", frozen):
                raise AgentDenied(
                    f"trial {trial_id!r} initial prompt is runtime-owned; "
                    "omit prompt instead of supplying a custom task"
                )
            prompt = frozen
        elif not isinstance(prompt, str) or not prompt.strip():
            raise AgentDenied(f"trial {trial_id!r} needs a non-empty prompt string")
        session = {
            "messages": [{"role": "user", "content": prompt}],
            "records": [],
            "trusted_verifications": [],
        }
        self.sessions[trial_id] = session
        return self._call(trial_id, session)

    def _resume_one(self, trial_id: str, message: Any) -> dict[str, Any]:
        session = self.sessions.get(trial_id)
        if session is None:
            raise AgentDenied(f"trial {trial_id!r} has not been started")
        if len(session["records"]) - 1 >= self.max_repairs:
            raise AgentDenied(
                f"trial {trial_id!r} has used all {self.max_repairs} repair turns"
            )
        if self.evaluation == "llm_proficiency":
            if message not in (None, ""):
                raise AgentDenied(
                    f"trial {trial_id!r} repair feedback is runtime-owned; "
                    "omit message instead of supplying custom guidance"
                )
            trusted = session.get("trusted_verifications") or []
            previous_verification = trusted[-1] if trusted else None
            try:
                message = benchmark.proficiency_repair_prompt(
                    previous_verification
                )
            except benchmark.BenchmarkError as exc:
                raise AgentDenied(str(exc)) from exc
        elif not isinstance(message, str) or not message.strip():
            raise AgentDenied(f"trial {trial_id!r} needs a non-empty message string")
        session["messages"].append({"role": "user", "content": message})
        return self._call(trial_id, session)

    def _batch(self, action: dict[str, Any], field: str) -> list[tuple[str, Any]]:
        """Validate a whole batch before any call, so a bad entry costs nothing."""
        raw = action.get("trials")
        if not isinstance(raw, list) or not raw:
            raise AgentDenied("trials must be a non-empty array")
        if len(raw) > self.max_batch:
            raise AgentDenied(
                f"a batch holds at most {self.max_batch} trials; this one has {len(raw)}"
            )
        if self.used + len(raw) > self.budget:
            raise AgentDenied(
                f"batch of {len(raw)} exceeds the remaining trial budget "
                f"({self.budget - self.used} of {self.budget} calls left)"
            )
        entries: list[tuple[str, Any]] = []
        seen: set[str] = set()
        for item in raw:
            if not isinstance(item, dict):
                raise AgentDenied("each batch entry must be an object")
            trial_id = self._valid_id(item.get("trial_id"))
            self._require_allowed_trial_id(trial_id)
            if trial_id in seen:
                raise AgentDenied(f"trial {trial_id!r} appears twice in one batch")
            seen.add(trial_id)
            entries.append((trial_id, item.get(field)))
        return entries

    @staticmethod
    def _batch_view(observation: dict[str, Any]) -> dict[str, Any]:
        # Completions are on disk and in the trace; repeating them inline for a
        # whole batch would only inflate every later turn of this conversation.
        return {k: v for k, v in observation.items() if k != "completion"}

    def start(self, action: dict[str, Any]) -> dict[str, Any]:
        if self.evaluation == "llm_learnability" and self.used == 0:
            problems = benchmark.validate_learnability_attestations(self.agent_dir)
            if problems:
                raise AgentDenied(
                    "Learnability scored trials are locked until preflight and leakage "
                    "attestations pass: " + "; ".join(problems)
                )
        if self.budget <= 0:
            raise AgentDenied("this unit has no trial budget; trial actions are not available")
        if "trials" in action:
            entries = self._batch(action, "prompt")
            for trial_id, _ in entries:
                if trial_id in self.sessions:
                    raise AgentDenied(
                        f"trial {trial_id!r} already exists; a trial is fresh exactly once"
                    )
            results = [self._batch_view(self._start_one(t, p)) for t, p in entries]
            return {
                "ok": all(r["ok"] for r in results),
                "trials": results,
                "calls_used": self.used,
                "calls_remaining": self.budget - self.used,
            }
        trial_id = self._valid_id(action.get("trial_id"))
        self._require_allowed_trial_id(trial_id)
        if trial_id in self.sessions:
            raise AgentDenied(f"trial {trial_id!r} already exists; a trial is fresh exactly once")
        if self.used >= self.budget:
            raise AgentDenied(
                f"trial budget exhausted: this unit is frozen at {self.budget} trial calls"
            )
        return self._start_one(trial_id, action.get("prompt"))

    def resume(self, action: dict[str, Any]) -> dict[str, Any]:
        if "trials" in action:
            entries = self._batch(action, "message")
            for trial_id, _ in entries:
                if trial_id not in self.sessions:
                    raise AgentDenied(f"trial {trial_id!r} has not been started")
            results = [self._batch_view(self._resume_one(t, m)) for t, m in entries]
            return {
                "ok": all(r["ok"] for r in results),
                "trials": results,
                "calls_used": self.used,
                "calls_remaining": self.budget - self.used,
            }
        trial_id = self._valid_id(action.get("trial_id"))
        if self.used >= self.budget:
            raise AgentDenied(
                f"trial budget exhausted: this unit is frozen at {self.budget} trial calls"
            )
        return self._resume_one(trial_id, action.get("message"))

    def unfinished_proficiency_trial_ids(self) -> list[str]:
        """Started Primary trials that still need a trusted repair turn."""
        if self.evaluation != "llm_proficiency":
            return []
        unfinished: list[str] = []
        for trial_id in self.required_trial_ids:
            session = self.sessions.get(trial_id)
            if session is None:
                continue
            records = session.get("records") or []
            if not records:
                unfinished.append(trial_id)
                continue
            trusted = session.get("trusted_verifications") or []
            verification = trusted[-1] if trusted else None
            if (
                isinstance(verification, dict)
                and benchmark.proficiency_public_repair_gate_passed(verification)
            ):
                continue
            repairs_used = max(0, len(records) - 1)
            if repairs_used < self.max_repairs:
                unfinished.append(trial_id)
        return unfinished

    def summary(self) -> dict[str, Any]:
        return {
            "budget": self.budget,
            "used": self.used,
            "max_repairs_per_trial": self.max_repairs,
            "max_batch": self.max_batch,
            "records_dir": "trials/",
            "required_trial_ids": self.required_trial_ids,
            "proficiency_prompt_set_sha256": (
                benchmark.proficiency_primary_prompt_set_sha256(
                    self.root, self.proficiency_language
                )
                if self.proficiency_language is not None
                else None
            ),
            "missing_required_trial_ids": sorted(
                self.required_trial_id_set - set(self.sessions)
            ),
            "unfinished_proficiency_trial_ids": self.unfinished_proficiency_trial_ids(),
            "trials": {
                trial_id: {"calls": session["records"], "repairs": len(session["records"]) - 1}
                for trial_id, session in self.sessions.items()
            },
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
                            config: dict[str, Any], trials: "Trials | None" = None,
                            max_turns: int | None = None,
                            orchestration_cap: int | None = None) -> str:
    read_lines = "\n".join(f"- {Path(p).as_posix()}" for p in task.get("read_paths", []))
    outputs = task.get("expected_outputs") or []
    output_lines = "\n".join(f"- {Path(p).as_posix()}" for p in outputs) or "- defined by this task"
    allowlist = ", ".join(config["exec_allowlist"])
    target_compiler = perms.root / "work" / "root" / "target-build" / "quidra"
    compiler_line = ""
    if target_compiler.is_file():
        compiler_line = (
            f"- `quidra` is the compiler built from the evaluated snapshot "
            f"(`{target_compiler.as_posix()}`); it is on PATH for `run`.\n"
        )
    feedback = benchmark.previous_attempt_feedback(perms.root, task.get("id", ""))
    feedback_block = ""
    if feedback:
        feedback_block = f"""
Your previous attempt at this exact task was rejected. The rejection was:

{feedback}

Correct that this time; the task, its permissions and its expected outputs are
unchanged.
"""
    if max_turns is None:
        max_turns = trials.max_turns if trials is not None else config["max_turns"]
    cap_line = ""
    if orchestration_cap is not None:
        cap_line = (
            f"- Each of your own turns is capped at {orchestration_cap} output tokens. A "
            "turn cut off at that cap executes nothing and is answered with a "
            "protocol_error; split large content across several write_file calls.\n"
        )
    trial_block = ""
    if trials is not None and trials.budget > 0:
        trial_block = f"""
Independent model trials (this unit's scored experiment):

```
{{"action":"trial_start","trial_id":"<cell>-t<n>","prompt":"<exact initial prompt for the model under test>"}}
{{"action":"trial_start","trials":[{{"trial_id":"<cell>-t1","prompt":"..."}}, {{"trial_id":"<cell>-t2","prompt":"..."}}]}}
{{"action":"trial_continue","trial_id":"<cell>-t<n>","message":"<repair prompt with the failure>"}}
{{"action":"trial_continue","trials":[{{"trial_id":"<cell>-t1","message":"..."}}]}}
```

Each `trial_start` opens a FRESH session for the model under test: it sees only the
prompt you give it, never this conversation, never another trial. `trial_continue`
is a repair turn inside that same trial; at most {trials.max_repairs} per trial.
The batch form runs up to {trials.max_batch} independent trials in one turn; use
it. Every trial prompt and completion is recorded verbatim by the runtime under
`trials/<trial_id>/` in your directory (`prompt_NN.txt`, `completion_NN.txt`,
`session.json`); you can read those files but not write them. Batch observations
report each completion's path and size rather than its text. When a trial
evaluation has no runtime-owned verifier, use scripts you `write_file` and
`run` to inspect its outputs. LLM Proficiency is different: the rules below
provide trusted compile/run verification automatically. Trial completions are capped at
{trials.max_output_tokens} output tokens; a trial whose reply hit that cap or came
back empty is reported with `ok:false` and counts as a failed attempt for that
trial. Budget for this unit: {trials.budget} trial calls in total ({trials.used}
used); a batch larger than the remaining budget is denied before any call.
"""
        if trials.sessions:
            restored = "\n".join(
                f"- {trial_id}: {len(session.get('records') or [])} paid call(s) already preserved"
                for trial_id, session in sorted(trials.sessions.items())
            )
            trial_block += f"""
This is a resumed worker attempt. The runtime has already restored the scored
sessions below from runtime-owned per-trial records and the prior audit trace.
Do not trial_start an existing ID again. Read its preserved files when needed;
use trial_continue only when that session still requires an allowed repair, and
spend new scored calls only on genuinely missing work.

{restored}
"""

        if str(task.get("evaluation") or "") == "llm_proficiency":
            required = "\n".join(f"- {trial_id}" for trial_id in trials.required_trial_ids)
            trial_block += f"""
For LLM Proficiency, both the Primary allocation and every initial scored
prompt are enforced by the trusted runtime. Start every ID below exactly once
before finalizing; any other trial ID is rejected before it can spend a scored
call. Do NOT author an initial prompt. Use
`{{"action":"trial_start","trial_id":"<id>"}}` (or batch entries containing only
`trial_id`). The runtime inserts the frozen workload/scenario prompt. A custom
prompt is rejected before inference.

After every initial or repair completion, the runtime writes the returned source,
performs the target language's frozen compile/parse step, then executes every
public/hidden external-oracle case for the frozen workload. The observation
includes only compact pass counts and safe diagnostics. Full oracle evidence is
retained in a trusted runner-only path outside your readable roots; hidden inputs,
expected answers, hidden case identities, and hidden-run output are never placed
in model-visible feedback. Do not repeat those commands merely to establish
correctness. Repair
feedback is runtime-owned: omit the message field on trial_continue. The runtime
sends verifier facts plus the frozen replacement-source instruction. Custom
repair guidance is rejected, a passing trial cannot be repaired, and a failing
trial must continue until it passes or its frozen repair budget is exhausted
before finalization is accepted.

{required}
"""
        languages = [str(x) for x in (task.get("assigned_languages") or [])]
        programs = sorted({
            program
            for language in languages
            for program in benchmark.LANGUAGE_TOOLCHAIN_PROGRAMS.get(language, ())
        })
        if languages:
            trial_block += f"""
Two rules the validator applies to this unit's result, stated here because a
retry that learns them one at a time costs a whole attempt each:

- Toolchain evidence: before the first trial_start, invoke the assigned
  language's own toolchain directly as the program of a `run` action and have
  it exit 0 (accepted programs: {", ".join(programs) or "the language's compiler or runtime"}).
  A compile or run performed inside a helper script does not count: the
  validator reads the trace's `run` actions, not what a script did. Executables
  you build inside your own directory may then be run directly by relative path.
- Assigned languages only: every requirement value in result.json carries exactly
  the assigned language set ({", ".join(languages)}) and no other language.
"""
        if str(task.get("evaluation") or "") == "llm_learnability":
            trial_block += """
Before the FIRST trial_start in this unit, you MUST perform and preserve the
Learnability infrastructure/leakage checks in your writable directory:

- learnability_preflight.json: schema_version=1, passed=true, every boolean
  fixtures_compile_and_run, harness_conventions_satisfied,
  validator_positive_control_passed, validator_negative_control_passed=true,
  plus a non-empty evidence array.
- learnability_leakage.json: schema_version=1, passed=true, every boolean
  exact_solution_absent, expected_output_not_leaked, isomorphic_example_absent,
  withheld_mapping_absent, validator_answer_absent, metadata_leak_absent,
  planted_leak_positive_control_passed=true, plus a non-empty evidence array.

The runtime checks these files and refuses trial_start until they pass. Perform
the real checks first; these files are attestations of evidence, not substitutes
for the checks.
"""
    return f"""# Sandbox agent runtime contract

You are running as a `sandbox-agent` worker inside the benchmark sandbox rooted at
`{CANONICAL_WORKSPACE.as_posix()}`. You have no provider credentials, no host
filesystem access, no shell, and no agent-platform tools. The only capabilities
that exist are the actions below, and the runtime enforces every limit in code.

Reply with JSON action objects and nothing else: no prose and no Markdown fence.
One object per turn is the norm; several objects in one turn are executed in the
order written and answered together, so independent steps (several `write_file`
calls, a `write_file` followed by its `run`) may share a turn. `final` ends the
unit: send it alone or as the last object, and only once every expected output
exists; a `final` sent earlier is refused and costs a turn.

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
{compiler_line}- Network access: {"allowed through the inference gateway only" if task.get("network_allowed") else "disabled"}.
- Maximum turns: {max_turns}.
{cap_line}{trial_block}{feedback_block}
Expected outputs before you send `final`:
{output_lines}

Each of your turns is answered with a JSON observation. A denied action returns
`{{"ok":false,"denied":"..."}}`; that is a permission boundary, not a hint to try
another path. Do the task within these permissions, then send `final`.
"""


# --------------------------------------------------------------------------
# Loop
# --------------------------------------------------------------------------


def missing_expected_outputs(task: dict[str, Any], agent_dir: Path) -> list[str]:
    """The Task Packet's expected outputs that do not exist yet, as sandbox paths."""
    missing = []
    for raw in task.get("expected_outputs", []):
        path = benchmark.require_under(Path(raw), agent_dir)
        if not path.is_file():
            missing.append(path.as_posix())
    return missing


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
    sampling = benchmark.sampling_config(
        root, str(task.get("evaluation") or "")
    )
    perms = Permissions(root, agent_dir, task, config)
    packet = benchmark.render_prompt_components(
        task["prompt_components"], task["prompt_sha256"]
    ).decode("utf-8")

    client = InferenceGatewayClient(args.socket, timeout=float(args.timeout))
    health = client.health()
    if health.get("host_tools_exposed"):
        raise AgentFailure("gateway reports an exposed host tool surface; refusing to run")

    trials = Trials(root, task, args.id, config, client, int(args.max_output_tokens))
    isolation = benchmark.worker_isolation_config(root)
    # The agent's own action turns are never scored, so they do not carry the
    # unit's scored output cap: that cap is sized for one trial completion, and
    # an action turn that has to reason about a whole packet first needs room the
    # scored cap does not give it.
    # The cap lives in the runtime configuration (sandbox_agent.json) rather
    # than in primary.json: primary.json is embedded in every Task Packet and
    # hashed into every certified-cache key, so raising the cap there would
    # have invalidated every cached measurement. The isolation block is only a
    # fallback for older workspaces.
    orchestration_cap = int(
        config.get("orchestration_max_output_tokens")
        or isolation.get("orchestration_max_output_tokens", args.max_output_tokens)
        or args.max_output_tokens
    )
    # Every turn adds two messages; the gateway refuses a conversation longer than
    # its frozen limit, so stop before that becomes the way a unit ends.
    max_turns = min(int(trials.max_turns), (int(client.config["max_messages"]) - 2) // 2)
    messages: list[dict[str, str]] = [
        {"role": "system", "content": runtime_contract_prompt(
            perms, task, config, trials, max_turns=max_turns,
            orchestration_cap=orchestration_cap,
        )},
        {"role": "user", "content": packet},
    ]

    resume_path = agent_dir / "resume_trace.json"
    resume_payload: dict[str, Any] = {}
    if resume_path.is_file():
        try:
            resume_payload = benchmark.json_load(resume_path)
        except (OSError, json.JSONDecodeError) as exc:
            raise AgentFailure(f"resume_trace.json is unreadable: {exc}") from exc
    trace: list[dict[str, Any]] = list(resume_payload.get("trace", []) or [])
    # Recover calls that reached the per-call commit point but whose enclosing
    # batch never returned far enough to append its aggregate action trace.
    trace.extend(trials.resume_trace_entries(trace))
    denials: list[dict[str, Any]] = list(resume_payload.get("denied_actions", []) or [])
    usage = {
        "input_tokens": 0,
        "cache_creation_input_tokens": 0,
        "cache_read_input_tokens": 0,
        "output_tokens": 0,
        "calls": 0,
    }
    for key in usage:
        usage[key] = int((resume_payload.get("usage") or {}).get(key, 0) or 0)
    turn_offset = max(
        [int(row.get("turn", 0) or 0) for row in trace if isinstance(row, dict)] or [0]
    )
    partial_path = agent_dir / "agent_trace.partial.json"

    def checkpoint_trace() -> None:
        benchmark.json_dump(partial_path, {
            "schema_version": 1,
            "agent_id": args.id,
            "worker_mode": "sandbox-agent",
            "prompt_sha256": task["prompt_sha256"],
            "denied_actions": denials,
            "usage": usage,
            "trials": trials.summary(),
            "trace": trace,
            "checkpointed_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        })

    protocol_errors = 0
    max_protocol_errors = int(config["max_consecutive_protocol_errors"])
    final_summary: str | None = None
    stop_reason = "max_turns_exhausted"
    checkpoint_trace()

    for local_turn in range(1, max_turns + 1):
        turn = turn_offset + local_turn
        try:
            response = client.complete(
                messages,
                task_id=args.id,
                max_output_tokens=orchestration_cap,
                network_allowed=bool(task.get("network_allowed")),
                purpose="orchestration",
            )
        except GatewayRefusal as exc:
            raise AgentFailure(f"inference gateway refused this worker's request: {exc}") from exc
        except GatewayClientError as exc:
            raise AgentFailure(f"inference transport failure: {exc}") from exc

        usage["calls"] += 1
        for key in (
            "input_tokens", "cache_creation_input_tokens",
            "cache_read_input_tokens", "output_tokens",
        ):
            usage[key] += int(response.get("usage", {}).get(key, 0) or 0)
        completion = response["content"]
        if not completion.strip():
            # An empty assistant turn is rejected by the provider on the next
            # request, so it must not enter the conversation at all. Record what
            # the provider reported: an empty turn that used the whole cap spent
            # it on reasoning, which is a depth problem, not a model refusal.
            protocol_errors += 1
            trace.append({
                "turn": turn,
                "action": None,
                "protocol_error": "empty completion",
                "stop_reason": response.get("stop_reason"),
                "output_tokens": int(response.get("usage", {}).get("output_tokens", 0) or 0),
            })
            checkpoint_trace()
            if protocol_errors >= max_protocol_errors:
                stop_reason = "protocol_contract_violated"
                break
            continue
        messages.append({"role": "assistant", "content": completion})

        incomplete = completion_problem(response)
        if incomplete:
            if incomplete.startswith("max_tokens"):
                # An action turn cut off at its cap is recoverable: the turn is
                # never scored, nothing in it is executed, and the model can
                # resend the same work in smaller pieces. The third paid run
                # ended a unit on the first such turn; only a run of consecutive
                # truncations ends one now.
                protocol_errors += 1
                trace.append({
                    "turn": turn,
                    "action": None,
                    "incomplete": incomplete,
                    "protocol_error": "truncated action turn",
                })
                checkpoint_trace()
                if protocol_errors >= max_protocol_errors:
                    stop_reason = "protocol_contract_violated"
                    break
                messages.append({"role": "user", "content": json.dumps({
                    "ok": False,
                    "protocol_error": (
                        f"your turn was cut off at the {orchestration_cap}-token turn cap "
                        "and nothing in it was executed; resend the work as smaller "
                        "actions (several write_file calls with shorter content, one "
                        "step at a time)"
                    ),
                })})
                continue
            # Refused, paused or asking for a tool: not a protocol error on the
            # model's part, and not worth another identical turn.
            stop_reason = f"incomplete_completion:{incomplete.split(':', 1)[0]}"
            trace.append({"turn": turn, "action": None, "incomplete": incomplete})
            checkpoint_trace()
            break

        try:
            actions = parse_model_json_batch(completion)
        except GatewayClientError as exc:
            protocol_errors += 1
            trace.append({
                "turn": turn,
                "action": None,
                "protocol_error": str(exc),
                "completion_sha256": benchmark.sha256_bytes(completion.encode("utf-8")),
            })
            checkpoint_trace()
            if protocol_errors >= max_protocol_errors:
                stop_reason = "protocol_contract_violated"
                break
            messages.append({
                "role": "user",
                "content": json.dumps({"ok": False, "protocol_error": str(exc)}),
            })
            continue

        # Every object in the turn is an action, executed in the order written.
        # `final` ends the unit once the expected outputs exist; anything the
        # model wrote after a `final` is never executed.
        observations: list[dict[str, Any]] = []
        turn_protocol_error = False
        finished = False
        for position, action in enumerate(actions, start=1):
            batch = {"batch": [position, len(actions)]} if len(actions) > 1 else {}
            name = action.get("action")
            if name == "final":
                missing_now = missing_expected_outputs(task, agent_dir)
                missing_trials = sorted(
                    trials.required_trial_id_set - set(trials.sessions)
                )
                unfinished_trials = trials.unfinished_proficiency_trial_ids()
                if missing_now or missing_trials or unfinished_trials:
                    protocol_errors += 1
                    turn_protocol_error = True
                    reasons = []
                    if missing_now:
                        reasons.append(
                            "expected outputs are still missing: " + ", ".join(missing_now)
                        )
                    if missing_trials:
                        reasons.append(
                            "required Primary trials are still missing: "
                            + ", ".join(missing_trials)
                        )
                    if unfinished_trials:
                        reasons.append(
                            "failed Proficiency trials still have frozen repair budget: "
                            + ", ".join(unfinished_trials)
                        )
                    observation = {
                        "ok": False,
                        "protocol_error": (
                            "final refused: " + "; ".join(reasons)
                            + "; complete them before sending final again"
                        ),
                    }
                    trace.append({
                        "turn": turn, "action": "final", "observation": observation,
                        "protocol_error": "final before required work completed", **batch,
                    })
                    checkpoint_trace()
                    observations.append(observation)
                    break
                final_summary = str(action.get("summary", ""))
                stop_reason = "final"
                trace.append({"turn": turn, "action": "final", **batch})
                checkpoint_trace()
                finished = True
                break

            if name in {"trial_start", "trial_continue"}:
                try:
                    observation = trials.start(action) if name == "trial_start" else trials.resume(action)
                except AgentDenied as exc:
                    observation = {"ok": False, "denied": str(exc)}
                    denials.append({"turn": turn, "action": name, "reason": str(exc)})
            elif name not in ACTIONS:
                observation = {
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
            if isinstance(recorded.get("completion"), str):
                recorded["completion"] = f"<{len(recorded['completion'])} chars, preserved under trials>"
            if isinstance(recorded.get("content"), str):
                recorded["content"] = f"<{len(recorded['content'])} chars elided from trace>"
            trace.append({"turn": turn, "action": name, "observation": recorded, **batch})
            checkpoint_trace()
            observations.append(observation)

        if finished:
            break
        if protocol_errors >= max_protocol_errors:
            stop_reason = "protocol_contract_violated"
            break
        if not turn_protocol_error:
            protocol_errors = 0

        if len(actions) == 1:
            reply: dict[str, Any] = observations[0]
        else:
            reply = {
                "ok": all(bool(item.get("ok")) for item in observations),
                "executed": len(observations),
                "of": len(actions),
                "results": observations,
            }
        rendered, _ = truncate(
            json.dumps(reply, sort_keys=True), int(config["max_observation_bytes"])
        )
        messages.append({"role": "user", "content": rendered})

    expected = [
        benchmark.require_under(Path(raw), agent_dir).as_posix()
        for raw in task.get("expected_outputs", [])
    ]
    missing = missing_expected_outputs(task, agent_dir)

    result = {
        "schema_version": 1,
        "agent_id": args.id,
        "worker_mode": "sandbox-agent",
        "runtime": config["runtime"],
        "prompt_sha256": task["prompt_sha256"],
        "gateway": {
            "protocol": health.get("gateway"),
            "provider": health.get("provider", {}).get("id"),
            "model": health.get("provider", {}).get("model"),
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
        "trials": trials.summary(),
        "trace": trace,
        "finished_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    benchmark.json_dump(agent_dir / "agent_trace.json", result)
    partial_path.unlink(missing_ok=True)
    resume_path.unlink(missing_ok=True)
    print(json.dumps({k: v for k, v in result.items() if k != "trace"}, indent=2))

    if stop_reason != "final" or missing:
        # The runner keeps the worker's stderr as the attempt's failure detail,
        # and the next attempt is told about it. One line that names the cause
        # is worth more there than the whole trace on stdout.
        protocol = [x for x in trace if x.get("protocol_error")]
        last_denials = "; ".join(str(d.get("reason")) for d in denials[-3:])
        print(
            "sandbox agent error: the run ended with stop_reason="
            f"{stop_reason!r} after {len(trace)} turn(s); "
            f"missing outputs: {missing or 'none'}; "
            f"protocol errors: {len(protocol)}"
            + (f" (last: {protocol[-1].get('protocol_error')})" if protocol else "")
            + (f"; last denials: {last_denials}" if last_denials else ""),
            file=sys.stderr,
        )
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
