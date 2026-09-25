#!/usr/bin/env python3
"""Prove the scored benchmark sandbox cannot reach a provider credential.

The claim this file has to defend is narrow and concrete:

    trusted gateway (has credentials)
        -> credential-less Unix socket IPC
            -> scored worker (no credentials, restricted local tools)

so the tests run the real gateway in a subprocess that holds a secret, run the
real workers in subprocesses whose environment has been stripped, and check what
actually happens rather than what the configuration says should happen.

Everything here is offline: the gateway uses its deterministic fake provider, so
CI needs no provider account and no network.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "benchmark" / "template"
SCRIPTS = TEMPLATE / "scripts"

FAKE_PROVIDER_SECRET = "sk-test-quidra-benchmark-secret-000000000000"
FAKE_HOST_PATH = "/Users/example-benchmark-operator/checkout"

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def load(path: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


benchmark = load(SCRIPTS / "benchmark.py", "isolation_tests_benchmark")
gateway_client = load(SCRIPTS / "gateway_client.py", "isolation_tests_gateway_client")
inference_gateway = load(SCRIPTS / "inference_gateway.py", "isolation_tests_gateway")
sandbox_launcher = load(SCRIPTS / "sandbox_launcher.py", "isolation_tests_launcher")
sandbox_agent = load(SCRIPTS / "sandbox_agent.py", "isolation_tests_sandbox_agent")


# --------------------------------------------------------------------------
# Harness
# --------------------------------------------------------------------------


CREDENTIAL_ENV_NAMES = json.loads(
    (TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")
)["sandbox_forbidden_credential_environment_names"]


def sandbox_side_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    """The environment a scored process is allowed to see.

    Built by subtraction from the real environment so that a credential the test
    machine happens to export cannot accidentally make a test pass.
    """
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": "/nonexistent-sandbox-home",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    env.update(extra or {})
    for name in CREDENTIAL_ENV_NAMES:
        env.pop(name, None)
    return env


class Gateway:
    """A real gateway subprocess that holds a secret the sandbox must never see."""

    def __init__(self, socket_dir: Path, script: dict[str, Any] | None = None,
                 network_policy: str = "disabled",
                 task_policy: dict[str, Any] | None = None) -> None:
        self.socket_path = socket_dir / "inference.sock"
        self.script_path = socket_dir / "fake_script.json"
        if script is not None:
            self.script_path.write_text(json.dumps(script), encoding="utf-8")
        self.log_path = socket_dir / "audit.jsonl"
        argv = [
            sys.executable, str(SCRIPTS / "inference_gateway.py"), "serve",
            "--socket", str(self.socket_path),
            "--template", str(TEMPLATE),
            "--provider", "fake",
            "--network-policy", network_policy,
            "--log", str(self.log_path),
        ]
        if script is not None:
            argv += ["--fake-script", str(self.script_path)]
        if task_policy is not None:
            self.policy_path = socket_dir / "task_policy.json"
            self.policy_path.write_text(json.dumps(task_policy), encoding="utf-8")
            argv += ["--task-policy", str(self.policy_path)]
        self.env = {**os.environ, "ANTHROPIC_API_KEY": FAKE_PROVIDER_SECRET}
        self.process = subprocess.Popen(
            argv, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )

    def wait_ready(self, timeout: float = 20.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                return False
            if self.socket_path.is_socket():
                try:
                    raw_request(
                        self.socket_path,
                        {"schema_version": 1, "kind": "health.request"},
                        timeout=2.0,
                    )
                    return True
                except OSError:
                    pass
            time.sleep(0.05)
        return False

    def close(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def __enter__(self) -> "Gateway":
        if not self.wait_ready():
            out, err = self.process.communicate(timeout=5)
            raise AssertionError(f"gateway did not start: {err or out}")
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


def raw_request(socket_path: Path, payload: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(socket_path))
        client.sendall(json.dumps(payload).encode("utf-8") + b"\n")
        chunks: list[bytes] = []
        while not chunks or not chunks[-1].endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    return json.loads(b"".join(chunks).decode("utf-8"))


def make_workspace(tmp: Path) -> Path:
    """A synthetic benchmark workspace good enough for Task Packet work."""
    root = tmp / "workspace"
    for directory in (
        "work/root", "work/agents", "work/attempts", "raw", "results",
        "prompts/by-hash", "prompts/components/by-hash", "prompts/manifests",
        "home", "tmp", "gateway", "repo/docs",
    ):
        (root / directory).mkdir(parents=True, exist_ok=True)

    shutil.copytree(
        TEMPLATE,
        root / "template",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )
    (root / "repo" / "docs" / "allowed.md").write_text(
        "# Readable documentation\n\nThis file is inside a declared read path.\n",
        encoding="utf-8",
    )
    (root / "repo" / "unlisted.md").write_text(
        "This file is outside every declared read path.\n", encoding="utf-8"
    )

    master = (ROOT / "benchmark" / "master_prompt.md").read_bytes()
    master_hash = benchmark.sha256_bytes(master)
    (root / "prompts" / "by-hash" / f"{master_hash}.md").write_bytes(master)
    (root / "run.json").write_text(
        json.dumps({
            "schema_version": 1,
            "run_id": "isolation-tests",
            "workspace_root": "/quidra-benchmark",
            "sandbox_mode": "container",
            "evaluated": {"commit_sha": "synthetic"},
            "master_prompt_sha256": master_hash,
            "primary_config_sha256": benchmark.sha256_file(
                root / "template" / "config" / "primary.json"
            ),
            "template_tree_sha256": benchmark.sha256_tree(root / "template"),
        }, indent=2) + "\n",
        encoding="utf-8",
    )
    return root


def create_task(root: Path, agent_id: str, worker_mode: str) -> Path:
    import argparse
    import contextlib
    import io

    # cmd_task_create prints the whole packet manifest; it is noise here.
    with contextlib.redirect_stdout(io.StringIO()):
        benchmark.cmd_task_create(argparse.Namespace(
            workspace=str(root),
            id=agent_id,
            parent=None,
            evaluation=None,
            goal="isolation test task",
            read=[str(root / "repo" / "docs")],
            write=None,
            output=[str(root / "work" / "agents" / agent_id / "result.json")],
            validate="true",
            network=False,
            depth=0,
            section=[],
            requirement_id=[],
            language=[],
            worker_mode=worker_mode,
        ))
    return root / "work" / "agents" / agent_id


# --------------------------------------------------------------------------
# 1. The gateway is a pure inference broker
# --------------------------------------------------------------------------


def test_gateway_brokers_inference_and_nothing_else() -> None:
    with tempfile.TemporaryDirectory() as td:
        socket_dir = Path(td)
        with Gateway(socket_dir) as gw:
            health = raw_request(gw.socket_path, {"schema_version": 1, "kind": "health.request"})
            check(health.get("ok") is True, f"health handshake failed: {health}")
            check(
                health.get("gateway") == "quidra-inference-gateway-v1",
                "gateway did not identify its protocol",
            )
            check(
                health.get("credential_less_client") is True,
                "gateway did not advertise credential-less clients",
            )
            check(
                health.get("host_tools_exposed") is False
                and health.get("exposed_tool_surface") == []
                and health.get("sandbox_selectable_tools") == [],
                "gateway advertised a host tool surface",
            )

            completion = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "t1",
                "messages": [{"role": "user", "content": "hello"}],
            })
            check(
                completion.get("kind") == "inference.response" and completion.get("ok"),
                f"plain inference request was not served: {completion}",
            )

            # A broker that merely claims to have no tools proves nothing. These
            # refusals are the actual evidence.
            with_tools = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "t2",
                "messages": [{"role": "user", "content": "hello"}],
                "tools": [{"name": "bash"}],
            })
            check(
                with_tools.get("kind") == "inference.error"
                and with_tools["error"]["class"] == "policy",
                f"gateway accepted a request carrying a tool definition: {with_tools}",
            )

            for field in ("mcp_servers", "files", "command", "cwd", "env", "api_key", "base_url"):
                refused = raw_request(gw.socket_path, {
                    "schema_version": 1,
                    "kind": "inference.request",
                    "request_id": "t3",
                    "messages": [{"role": "user", "content": "hello"}],
                    field: "anything",
                })
                check(
                    refused.get("kind") == "inference.error"
                    and refused["error"]["class"] == "policy",
                    f"gateway accepted a request carrying {field!r}: {refused}",
                )

            wrong_kind = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "shell.exec",
                "request_id": "t4",
                "argv": ["id"],
            })
            check(
                wrong_kind.get("kind") == "inference.error"
                and wrong_kind["error"]["class"] == "policy",
                f"gateway accepted a non-inference request kind: {wrong_kind}",
            )

            # network_allowed may only ever narrow the trusted side's policy.
            escalation = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "t5",
                "messages": [{"role": "user", "content": "hello"}],
                "network_allowed": True,
            })
            check(
                escalation.get("kind") == "inference.error"
                and escalation["error"]["class"] == "policy",
                f"sandbox escalated its own network policy: {escalation}",
            )

        elevated_dir = socket_dir / "elevated"
        elevated_dir.mkdir()
        with Gateway(elevated_dir, network_policy="allowed") as gw:
            allowed = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "t6",
                "messages": [{"role": "user", "content": "hello"}],
                "network_allowed": True,
            })
            check(
                allowed.get("kind") == "inference.response",
                f"an explicitly granted network ceiling was still refused: {allowed}",
            )



def test_gateway_binds_each_task_to_one_live_worker_process() -> None:
    """A child process cannot spend through a task already owned by its worker."""
    with tempfile.TemporaryDirectory() as td:
        with Gateway(Path(td)) as gw:
            first = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "peer-parent",
                "task_id": "peer-bound-task",
                "messages": [{"role": "user", "content": "bind this task"}],
            })
            check(
                first.get("kind") == "inference.response" and first.get("ok"),
                f"the legitimate worker could not bind its task: {first}",
            )
            code = """
import json, socket, sys
path = sys.argv[1]
payload = {
    "schema_version": 1,
    "kind": "inference.request",
    "request_id": "peer-child",
    "task_id": "peer-bound-task",
    "messages": [{"role": "user", "content": "child bypass"}],
}
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.connect(path)
    client.sendall(json.dumps(payload).encode("utf-8") + b"\\n")
    data = b""
    while not data.endswith(b"\\n"):
        chunk = client.recv(65536)
        if not chunk:
            break
        data += chunk
print(data.decode("utf-8"))
"""
            child = subprocess.run(
                [sys.executable, "-c", code, str(gw.socket_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10,
            )
            try:
                response = json.loads(child.stdout)
            except json.JSONDecodeError:
                response = {"decode_error": child.stdout, "stderr": child.stderr}
            check(
                response.get("kind") == "inference.error"
                and response.get("error", {}).get("class") == "policy",
                f"a child process reused its parent's task budget: {response}",
            )


def test_sandbox_subprocess_trace_rejects_undeclared_workspace_access() -> None:
    class Policy:
        root = Path("/quidra-benchmark")
        agent_dir = Path("/quidra-benchmark/work/agents/worker-a")
        read_roots = [
            Path("/quidra-benchmark/repo/docs"),
            Path("/quidra-benchmark/work/agents/worker-a"),
        ]

    policy = Policy()
    allowed = (
        '1 openat(AT_FDCWD, "/quidra-benchmark/repo/docs/allowed.md", O_RDONLY) = 3\\n'
        '1 openat(AT_FDCWD, "/quidra-benchmark/work/agents/worker-a/result.json", O_WRONLY) = 4\\n'
    )
    check(
        sandbox_agent._sandbox_subprocess_access_problem(allowed, policy) is None,
        "declared subprocess workspace access was rejected",
    )

    # Real compilers walk toward the filesystem root looking for optional
    # project files. Missing probes (go.work, tsconfig.json, etc.) reveal no
    # contents and must not make every language fail before its scored trials.
    missing_project_files = (
        '1 openat(AT_FDCWD, "/quidra-benchmark/work/agents/go.work", O_RDONLY) = -1 ENOENT (No such file or directory)\n'
        '1 statx(AT_FDCWD, "/quidra-benchmark/work/agents/tsconfig.json", 0, STATX_ALL, 0x0) = -1 ENOENT (No such file or directory)\n'
    )
    check(
        sandbox_agent._sandbox_subprocess_access_problem(
            missing_project_files, policy
        ) is None,
        "missing toolchain project-file probes were rejected",
    )

    ancestor_metadata = (
        '1 newfstatat(AT_FDCWD, "/quidra-benchmark", {st_mode=S_IFDIR|0755}, 0) = 0\n'
        '1 statx(AT_FDCWD, "/quidra-benchmark/work/agents", 0, STATX_ALL, {stx_mode=S_IFDIR|0755}) = 0\n'
    )
    check(
        sandbox_agent._sandbox_subprocess_access_problem(
            ancestor_metadata, policy
        ) is None,
        "metadata-only ancestor probes were rejected",
    )

    ancestor_open = (
        '1 openat(AT_FDCWD, "/quidra-benchmark", O_RDONLY|O_DIRECTORY) = 3\n'
    )
    problem = sandbox_agent._sandbox_subprocess_access_problem(ancestor_open, policy)
    check(
        problem is not None and "undeclared workspace access" in problem,
        f"workspace-root directory open escaped the subprocess audit: {problem}",
    )

    validator_open = (
        '1 openat(AT_FDCWD, "/quidra-benchmark/template/scripts/benchmark.py", O_RDONLY) = 3\\n'
    )
    problem = sandbox_agent._sandbox_subprocess_access_problem(validator_open, policy)
    check(
        problem is not None and "benchmark.py" in problem,
        f"runner-owned validator source became readable to a worker: {problem}",
    )

    sibling = (
        '1 openat(AT_FDCWD, "/quidra-benchmark/work/agents/worker-b/result.json", O_RDONLY) = 3\\n'
    )
    problem = sandbox_agent._sandbox_subprocess_access_problem(sibling, policy)
    check(
        problem is not None and "undeclared workspace access" in problem,
        f"sibling-worker access escaped the subprocess audit: {problem}",
    )
    gateway = (
        '1 connect(3, {sa_family=AF_UNIX, sun_path="/quidra-benchmark/gateway/inference.sock"}, 110) = 0\\n'
    )
    problem = sandbox_agent._sandbox_subprocess_access_problem(gateway, policy)
    check(
        problem is not None and "gateway/inference.sock" in problem,
        f"direct gateway-socket access escaped the subprocess audit: {problem}",
    )


def test_gateway_never_returns_credentials_or_host_paths() -> None:
    script = {
        "schema_version": 1,
        "default": (
            "here is a leaked key "
            + FAKE_PROVIDER_SECRET
            + " and a host path "
            + FAKE_HOST_PATH
            + "/file.txt"
        ),
    }
    with tempfile.TemporaryDirectory() as td:
        with Gateway(Path(td), script=script) as gw:
            response = raw_request(gw.socket_path, {
                "schema_version": 1,
                "kind": "inference.request",
                "request_id": "leak",
                "messages": [{"role": "user", "content": "anything"}],
            })
    content = response.get("content", "")
    check(
        FAKE_PROVIDER_SECRET not in content,
        "a credential-shaped string reached the sandbox through a completion",
    )
    check(
        FAKE_HOST_PATH not in content,
        "a host path reached the sandbox through a completion",
    )

    scrubbed = inference_gateway.scrub_outbound(
        f"provider said {FAKE_PROVIDER_SECRET} about {FAKE_HOST_PATH}", [FAKE_PROVIDER_SECRET]
    )
    check(
        FAKE_PROVIDER_SECRET not in scrubbed and FAKE_HOST_PATH not in scrubbed,
        "scrub_outbound left a secret or host path in an outbound message",
    )


def test_client_holds_no_credentials() -> None:
    source = (SCRIPTS / "gateway_client.py").read_text(encoding="utf-8")
    for name in ("ANTHROPIC_API_KEY", "CLAUDE_CODE_MESSAGING_TOKEN", "api_key", "Authorization"):
        check(
            name not in source,
            f"the sandbox-side client references {name}; it must be credential-free",
        )

    with tempfile.TemporaryDirectory() as td:
        with Gateway(Path(td)) as gw:
            completed = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "gateway_client.py"), "complete",
                    "--socket", str(gw.socket_path), "--prompt", "credential-less check",
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
    check(
        completed.returncode == 0 and "fake-provider" in completed.stdout,
        f"a fully credential-stripped client could not reach the gateway: {completed.stderr}",
    )


# --------------------------------------------------------------------------
# 2. preflight verifies reality, not declarations
# --------------------------------------------------------------------------


HARDENED_OBSERVATIONS = {
    "platform": "linux",
    "euid": 1000,
    "cwd": "/quidra-benchmark",
    "network_interfaces": ["lo"],
    "mounts": [
        {"target": "/", "options": "ro", "source": "/dev/sda1"},
        {"target": "/quidra-benchmark", "options": "rw", "source": "/staging"},
        {"target": "/quidra-benchmark/repo", "options": "ro", "source": "/staging/repo"},
        {"target": "/quidra-benchmark/gateway", "options": "rw", "source": "/volume"},
        {"target": "/proc/bus", "options": "ro", "source": "proc"},
    ],
    "no_new_privileges": "1",
    "capability_bounding_set": "0000000000000000",
    "writable": {"repo": False, "template": False, "work": True},
}


def test_isolation_problems_reject_unhardened_processes() -> None:
    root = Path("/quidra-benchmark")
    check(
        benchmark.isolation_problems(root, dict(HARDENED_OBSERVATIONS)) == [],
        "a fully hardened observation set was rejected",
    )

    cases = {
        "sandbox_process_runs_as_root": {"euid": 0},
        "sandbox_network_not_isolated:eth0": {"network_interfaces": ["eth0", "lo"]},
        "network_isolation_unverifiable": {"network_interfaces": None},
        "mount_visibility_unverifiable": {"mounts": None},
        "no_new_privileges_not_set": {"no_new_privileges": "0"},
        "capabilities_not_dropped:00000000a80425fb": {
            "capability_bounding_set": "00000000a80425fb"
        },
        "evaluated_repo_snapshot_must_be_mounted_read_only": {
            "writable": {"repo": True, "template": False, "work": True}
        },
        "frozen_template_must_be_mounted_read_only": {
            "writable": {"repo": False, "template": True, "work": True}
        },
    }
    for expected, override in cases.items():
        observations = {**HARDENED_OBSERVATIONS, **override}
        problems = benchmark.isolation_problems(root, observations)
        check(expected in problems, f"{override} did not produce {expected}: {problems}")

    leaky = {
        **HARDENED_OBSERVATIONS,
        "mounts": HARDENED_OBSERVATIONS["mounts"] + [
            {"target": "/home/operator", "options": "rw", "source": "/home/operator"},
        ],
    }
    problems = benchmark.isolation_problems(root, leaky)
    check(
        any(p.startswith("unexpected_mount_visible_in_sandbox:/home/operator") for p in problems)
        and any(p.startswith("host_home_path_mounted:") for p in problems),
        f"a mounted host home directory was not reported: {problems}",
    )


def test_launcher_contract_must_match_reality() -> None:
    uid = HARDENED_OBSERVATIONS["euid"]
    contract = sandbox_launcher.launcher_contract(uid, uid, "image:tag", "disabled")
    env = {
        **contract["environment"],
        "QUIDRA_BENCHMARK_LAUNCHER_CONTRACT": json.dumps(contract, sort_keys=True),
    }

    _, problems = benchmark.launcher_contract_problems(
        json.dumps(contract, sort_keys=True), HARDENED_OBSERVATIONS, env
    )
    check(problems == [], f"the launcher's own contract was rejected: {problems}")

    _, problems = benchmark.launcher_contract_problems(None, HARDENED_OBSERVATIONS, env)
    check(problems == ["launcher_contract_missing"], f"a missing contract passed: {problems}")

    forged = {**contract, "uid": uid + 1}
    _, problems = benchmark.launcher_contract_problems(
        json.dumps(forged, sort_keys=True), HARDENED_OBSERVATIONS, env
    )
    check(
        "launcher_contract_uid_does_not_match_running_process" in problems,
        f"a contract claiming a different uid passed: {problems}",
    )

    # Claiming --network none while the environment says otherwise must fail.
    lying = {**contract, "network": "bridge"}
    _, problems = benchmark.launcher_contract_problems(
        json.dumps(lying, sort_keys=True), HARDENED_OBSERVATIONS, env
    )
    check(
        "launcher_contract_network_must_be_none" in problems,
        f"a contract admitting a network namespace passed: {problems}",
    )

    stripped_env = {k: v for k, v in env.items() if k != "HOME"}
    _, problems = benchmark.launcher_contract_problems(
        json.dumps(contract, sort_keys=True), HARDENED_OBSERVATIONS, stripped_env
    )
    check(
        "launcher_contract_environment_mismatch:HOME" in problems,
        f"an environment that disagrees with the contract passed: {problems}",
    )


def test_credentials_must_be_absent_from_the_sandbox() -> None:
    config = json.loads(
        (TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")
    )
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "home").mkdir()
        clean_env = {"HOME": str(root / "home")}
        check(
            benchmark.credential_exposure_problems(root, config, clean_env) == [],
            "a clean sandbox environment was reported as exposing credentials",
        )

        for name in ("ANTHROPIC_API_KEY", "CLAUDE_CODE_MESSAGING_TOKEN", "AWS_SECRET_ACCESS_KEY"):
            problems = benchmark.credential_exposure_problems(
                root, config, {**clean_env, name: "value"}
            )
            check(
                f"provider_credential_in_sandbox_environment:{name}" in problems,
                f"{name} in the sandbox environment was not rejected: {problems}",
            )

        problems = benchmark.credential_exposure_problems(
            root, config, {**clean_env, "SOME_HARMLESS_NAME": FAKE_PROVIDER_SECRET}
        )
        check(
            "credential_shaped_environment_value:SOME_HARMLESS_NAME" in problems,
            f"a credential-shaped value under a harmless name passed: {problems}",
        )

        (root / "home" / ".claude").mkdir()
        problems = benchmark.credential_exposure_problems(root, config, clean_env)
        check(
            "provider_credential_visible_in_sandbox:.claude" in problems,
            f"a mounted Claude configuration directory was not rejected: {problems}",
        )

        agent = root / "agent.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(agent))
        try:
            problems = benchmark.credential_exposure_problems(
                root, config, {**clean_env, "SSH_AUTH_SOCK": str(agent)}
            )
            check(
                "ssh_agent_socket_forwarded_into_sandbox" in problems,
                f"a forwarded SSH agent socket was not rejected: {problems}",
            )
        finally:
            server.close()


def test_gateway_attestation_requires_a_live_socket() -> None:
    config = json.loads(
        (TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")
    )
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "gateway").mkdir()
        local_config = {**config, "socket_path": str(root / "gateway" / "inference.sock")}

        info, problems = benchmark.gateway_attestation(root, local_config, timeout=5.0)
        check(
            "inference_gateway_socket_missing" in problems,
            f"a missing gateway socket passed attestation: {problems}",
        )

        os.environ["QUIDRA_BENCHMARK_INFERENCE_SOCKET"] = str(
            root / "gateway" / "inference.sock"
        )
        try:
            with Gateway(root / "gateway"):
                info, problems = benchmark.gateway_attestation(root, local_config, timeout=10.0)
        finally:
            os.environ.pop("QUIDRA_BENCHMARK_INFERENCE_SOCKET", None)
        check(problems == [], f"a live compliant gateway failed attestation: {problems}")
        check(
            info.get("policy_probes") == {"tool_field": True, "unsupported_kind": True},
            f"the negative policy probes did not run: {info}",
        )


# --------------------------------------------------------------------------
# 3. The launcher contract really is what gets launched
# --------------------------------------------------------------------------


def test_scored_container_command_is_hardened() -> None:
    contract = sandbox_launcher.launcher_contract(1000, 1000, "image:tag", "disabled")
    argv = sandbox_launcher.scored_container_argv(
        "docker",
        image="image:tag",
        staging=Path("/staging/.quidra-benchmark"),
        gateway_mount="gateway-volume",
        name="scored",
        uid=1000,
        gid=1000,
        contract=contract,
        interactive=False,
        argv=["python3", "/quidra-benchmark/template/scripts/benchmark.py", "preflight"],
    )
    joined = " ".join(argv)

    for expected in (
        "--network none",
        "--user 1000:1000",
        "--security-opt no-new-privileges",
        "--cap-drop ALL",
        "--read-only",
    ):
        check(expected in joined, f"the scored container is missing {expected}")

    for expected in (
        "/staging/.quidra-benchmark:/quidra-benchmark:rw",
        "/staging/.quidra-benchmark/repo:/quidra-benchmark/repo:ro",
        "/staging/.quidra-benchmark/template:/quidra-benchmark/template:ro",
        "/staging/.quidra-benchmark/cache:/quidra-benchmark/cache:ro",
        "gateway-volume:/quidra-benchmark/gateway:rw",
    ):
        check(expected in argv, f"the scored container is missing the mount {expected}")

    check(
        argv[argv.index("--workdir") + 1] == "/quidra-benchmark",
        "the scored container does not start in the canonical workspace",
    )

    env_values = [argv[i + 1] for i, item in enumerate(argv) if item == "--env"]
    env_keys = {value.split("=", 1)[0] for value in env_values}
    for forbidden in CREDENTIAL_ENV_NAMES:
        check(forbidden not in env_keys, f"the launcher passes {forbidden} into scored work")
    for expected_key, expected_value in (
        ("HOME", "/quidra-benchmark/home"),
        ("TMPDIR", "/quidra-benchmark/tmp"),
        ("PWD", "/quidra-benchmark"),
        ("QUIDRA_BENCHMARK_INFERENCE_SOCKET", "/quidra-benchmark/gateway/inference.sock"),
        ("QUIDRA_BENCHMARK_SANDBOX_ATTESTED", "container"),
    ):
        check(
            f"{expected_key}={expected_value}" in env_values,
            f"the scored container is missing {expected_key}={expected_value}",
        )

    check(
        "QUIDRA_BENCHMARK_LAUNCHER_CONTRACT" in env_keys,
        "the scored container carries no launcher contract to verify",
    )
    # The environment the contract declares must be exactly what is passed, or
    # preflight's cross-check would be comparing against fiction.
    for key, value in contract["environment"].items():
        check(
            f"{key}={value}" in env_values,
            f"the contract declares {key}={value} but the command line does not pass it",
        )


def test_gateway_container_keeps_credentials_on_the_trusted_side() -> None:
    argv = sandbox_launcher.gateway_container_argv(
        "docker",
        image="image:tag",
        staging=Path("/staging/.quidra-benchmark"),
        gateway_mount="gateway-volume",
        name="gw",
        uid=1000,
        gid=1000,
        provider="anthropic-messages",
        model="test-model",
        fake_script=None,
        exec_command=None,
        network_policy="disabled",
        task_policy=None,
        credential_env={"ANTHROPIC_API_KEY": FAKE_PROVIDER_SECRET},
        extra_mounts=[f"{Path.home()}/.claude:/home/agent/.claude:ro"],
    )
    joined = " ".join(argv)
    check(
        f"ANTHROPIC_API_KEY={FAKE_PROVIDER_SECRET}" in argv,
        "the gateway container did not receive the provider credential",
    )
    check(
        "--network none" not in joined,
        "the gateway container must keep provider egress",
    )
    check(
        f"/staging/.quidra-benchmark:{'/quidra-benchmark'}" not in joined,
        "the gateway container must not mount the scored workspace",
    )
    check(
        "gateway-volume:/gateway:rw" in argv,
        "the gateway container does not share the socket volume",
    )
    check(
        any(".claude:/home/agent/.claude:ro" in item for item in argv),
        "the gateway container did not receive its trusted-side agent session",
    )

    # The decisive property: a trusted-side mount must not be reachable from the
    # scored side. The scored command line is built from the contract alone, so
    # there is no path by which --gateway-mount could widen it.
    contract = sandbox_launcher.launcher_contract(1000, 1000, "image:tag", "disabled")
    scored = sandbox_launcher.scored_container_argv(
        "docker",
        image="image:tag",
        staging=Path("/staging/.quidra-benchmark"),
        gateway_mount="gateway-volume",
        name="scored",
        uid=1000,
        gid=1000,
        contract=contract,
        interactive=False,
        argv=["true"],
    )
    scored_mounts = [scored[i + 1] for i, item in enumerate(scored) if item == "--volume"]
    for mount in scored_mounts:
        check(
            ".claude" not in mount and "/home/" not in mount.split(":")[0],
            f"a trusted-side path reached the scored container: {mount}",
        )
    check(
        len(scored_mounts) == 5,
        f"the scored container's mounts are not the five fixed ones: {scored_mounts}",
    )


# --------------------------------------------------------------------------
# 4. End to end: packet-only and sandbox-agent through the same gateway
# --------------------------------------------------------------------------


def test_packet_only_end_to_end_through_the_gateway() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        root = make_workspace(tmp)
        agent_id = "worker-packet-only"
        agent_dir = create_task(root, agent_id, "packet-only")

        task = json.loads((agent_dir / "task.json").read_text(encoding="utf-8"))
        task["evaluation"] = "semantic_compression"
        (agent_dir / "task.json").write_text(
            json.dumps(task, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        packet = benchmark.render_prompt_components(
            task["prompt_components"], task["prompt_sha256"]
        ).decode("utf-8")
        check(
            "Readable documentation" in packet,
            "the packet-only Task Packet did not embed its permitted input",
        )

        payload = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {"gate.example": True},
            "evidence": {"source": "isolation test"},
        }
        worker_response = {
            "schema_version": 1,
            "task_id": agent_id,
            "files": [{"path": "result.json", "content": json.dumps(payload) + "\n"}],
        }
        script = {
            "schema_version": 1,
            "default": "```json\n" + json.dumps(worker_response) + "\n```",
        }

        task_policy = {
            "schema_version": 1,
            "tasks": {agent_id: "disabled"},
            "efforts": {agent_id: "medium"},
        }
        with Gateway(root / "gateway", script=script, task_policy=task_policy) as gw:
            completed = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "benchmark.py"), "task-infer",
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path),
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

        check(
            completed.returncode == 0,
            f"packet-only task-infer failed: {completed.stderr or completed.stdout}",
        )
        result = agent_dir / "result.json"
        check(result.is_file(), "task-infer did not materialize the worker's result.json")
        if result.is_file():
            check(
                json.loads(result.read_text(encoding="utf-8")) == payload,
                "task-infer materialized different content than the worker returned",
            )
        receipt = json.loads((agent_dir / "worker_response.json").read_text(encoding="utf-8"))
        check(
            receipt["inference"]["gateway"] == "quidra-inference-gateway-v1",
            "the receipt does not record which gateway produced the response",
        )
        check(
            receipt["inference"]["network_allowed"] is False,
            "a network-disabled packet recorded network access",
        )
        check(
            receipt["inference"]["sampling"]["effort"] == "medium"
            and receipt["inference"]["sampling"]["effort_source"] == "evaluation_effort",
            f"the receipt did not record the evaluation-specific frozen depth: {receipt}",
        )
        check(
            receipt["inference"]["effective_decoding"] == "medium",
            f"the receipt did not record the gateway's effective decoding depth: {receipt}",
        )


def sandbox_agent_script(root: Path, agent_id: str) -> dict[str, Any]:
    """Drive the agent through both permitted and forbidden actions, in order."""
    agent_dir = root / "work" / "agents" / agent_id
    payload = {
        "schema_version": 1,
        "evaluation": "semantic_compression",
        "requirements": {"gate.example": True},
        "evidence": {"source": "sandbox agent isolation test"},
    }
    return {
        "schema_version": 1,
        "sequence": [
            json.dumps({"action": "list_dir", "path": "../../../repo/docs"}),
            json.dumps({"action": "read_file", "path": "../../../repo/docs/allowed.md"}),
            json.dumps({"action": "read_file", "path": "../../../repo/unlisted.md"}),
            json.dumps({"action": "read_file", "path": "/etc/passwd"}),
            json.dumps({"action": "write_file", "path": "../../escape.json", "content": "no"}),
            json.dumps({"action": "write_file", "path": "escape-link/x.json", "content": "no"}),
            json.dumps({"action": "run", "argv": ["rm", "-rf", "/"]}),
            json.dumps({"action": "run", "argv": ["/bin/sh", "-c", "id"]}),
            json.dumps({"action": "run", "argv": ["python3", "--version"], "cwd": "../../.."}),
            json.dumps({"action": "run", "argv": ["python3", "--version"]}),
            json.dumps({
                "action": "write_file",
                "path": "result.json",
                "content": json.dumps(payload) + "\n",
            }),
            json.dumps({"action": "final", "summary": "wrote result.json"}),
        ],
        "_agent_dir": str(agent_dir),
    }


def test_sandbox_agent_enforces_its_permissions_in_code() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        root = make_workspace(tmp)
        agent_id = "worker-sandbox-agent"
        agent_dir = create_task(root, agent_id, "sandbox-agent")

        # A symlink planted inside the writable directory must not become a way out.
        (agent_dir / "escape-link").symlink_to(root / "work" / "root")

        script = sandbox_agent_script(root, agent_id)
        with Gateway(root / "gateway", script=script) as gw:
            completed = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path),
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

        check(
            completed.returncode == 0,
            f"the sandbox agent did not complete: {completed.stderr or completed.stdout}",
        )
        trace_path = agent_dir / "agent_trace.json"
        check(trace_path.is_file(), "the sandbox agent wrote no audit trace")
        if not trace_path.is_file():
            return
        trace = json.loads(trace_path.read_text(encoding="utf-8"))

        check(trace["stop_reason"] == "final", f"unexpected stop reason: {trace['stop_reason']}")
        check(trace["missing_outputs"] == [], f"expected outputs missing: {trace}")
        check(
            (agent_dir / "result.json").is_file(),
            "the sandbox agent's permitted write did not land",
        )
        check(
            not (root / "work" / "escape.json").exists()
            and not (root / "work" / "root" / "x.json").exists(),
            "the sandbox agent escaped its writable directory",
        )

        by_turn = {entry["turn"]: entry for entry in trace["trace"]}
        check(by_turn[1]["observation"]["ok"] is True, "a permitted list_dir was denied")
        check(by_turn[2]["observation"]["ok"] is True, "a permitted read was denied")

        denials = {entry["turn"]: entry["reason"] for entry in trace["denied_actions"]}
        expectations = {
            3: "outside the Task Packet's declared read paths",
            4: "escapes the sandbox workspace",
            5: "writes are limited to this worker's own directory",
            6: "writes are limited to this worker's own directory",
            7: "not in the frozen sandbox-agent executable allowlist",
            8: "not in the frozen allowlist",
            9: "working directory must stay inside",
        }
        for turn, fragment in expectations.items():
            check(turn in denials, f"turn {turn} was not denied at all: {trace['denied_actions']}")
            if turn in denials:
                check(
                    fragment in denials[turn],
                    f"turn {turn} denied for the wrong reason: {denials[turn]}",
                )

        check(by_turn[10]["observation"]["ok"] is True, "a permitted subprocess was denied")
        check(
            trace["gateway"]["provider"] == "fake"
            and trace["gateway"]["credential_less_client"] is True,
            f"the agent did not record a credential-less gateway: {trace['gateway']}",
        )


def test_retained_artifacts_carry_no_secret_or_host_path() -> None:
    import argparse

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        root = make_workspace(tmp)
        agent_id = "worker-hygiene"
        agent_dir = create_task(root, agent_id, "sandbox-agent")

        script = {
            "schema_version": 1,
            "sequence": [
                json.dumps({
                    "action": "write_file",
                    "path": "result.json",
                    "content": json.dumps({"schema_version": 1, "note": "clean"}) + "\n",
                }),
                json.dumps({"action": "final", "summary": "done"}),
            ],
        }
        with Gateway(root / "gateway", script=script) as gw:
            subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path),
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

        retained = []
        for relative in benchmark.RETAINED_RUN_PATHS:
            target = root / relative
            if target.is_file():
                retained.append(target)
            elif target.is_dir():
                retained.extend(p for p in target.rglob("*") if p.is_file())
        check(bool(retained), "no retained artifacts were produced to scan")
        for path in retained:
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            check(
                FAKE_PROVIDER_SECRET not in text,
                f"a provider secret reached a retained artifact: {path.name}",
            )
            check(
                "ANTHROPIC_API_KEY=" not in text,
                f"a credential assignment reached a retained artifact: {path.name}",
            )

        task = json.loads((agent_dir / "task.json").read_text(encoding="utf-8"))
        packet = benchmark.render_prompt_components(
            task["prompt_components"], task["prompt_sha256"]
        ).decode("utf-8")
        check(
            str(tmp) not in packet,
            "the synthetic host workspace path leaked into a rendered Task Packet",
        )

        # Every stored prompt component, not just the assembled packet: these are
        # exactly the bytes a model is sent, and the physical workspace path must
        # never appear in them regardless of where the workspace happens to live.
        physical = str(root.resolve())
        for component in (root / "prompts" / "components" / "by-hash").rglob("*"):
            if component.is_file():
                check(
                    physical not in component.read_text(encoding="utf-8"),
                    f"the physical workspace path leaked into {component.name}",
                )

        import contextlib
        import io

        os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = benchmark.cmd_privacy_check(argparse.Namespace(workspace=str(root)))
        finally:
            os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
        findings = json.loads((root / "results" / "privacy_check.json").read_text(encoding="utf-8"))
        check(rc == 0, f"the privacy gate rejected a clean run: {findings}")


def test_declared_tool_surface_matches_what_the_provider_will_do() -> None:
    """The handshake has to describe the provider that is actually running.

    A network-enabled task may reach a provider-side retrieval tool the trusted
    side froze in advance. That is allowed; claiming an empty surface while
    attaching one is not, because preflight records the claim as evidence.
    """
    config = json.loads(
        (TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")
    )
    os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
    try:
        provider = inference_gateway.AnthropicMessagesProvider(
            "claude-sonnet-5",
            timeout=30,
            pricing=config["anthropic_pricing"]["claude-sonnet-5"],
            web_search=config["anthropic_web_search"],
        )
    finally:
        os.environ.pop("ANTHROPIC_API_KEY", None)

    surface = provider.provider_tool_policy()
    check(len(surface) == 1, f"the web-search policy was not declared: {surface}")
    entry = surface[0]
    check(
        entry["type"] == config["anthropic_web_search"]["tool_type"],
        f"declared tool type disagrees with the frozen config: {entry}",
    )
    check(
        entry["selectable_by_sandbox"] is False and entry["grants_host_access"] is False,
        f"the declared tool is not constrained: {entry}",
    )

    # A gateway carrying no web-search policy must declare nothing, and must
    # refuse a network-enabled task rather than silently dropping the tool.
    os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
    try:
        bare = inference_gateway.AnthropicMessagesProvider("claude-sonnet-5", timeout=30)
    finally:
        os.environ.pop("ANTHROPIC_API_KEY", None)
    check(bare.provider_tool_policy() == [], "a provider with no policy declared a tool")

    problems, recorded = benchmark.tool_surface_problems({
        "sandbox_selectable_tools": [],
        "exposed_tool_surface": surface,
    })
    check(problems == [], f"a properly declared provider tool was rejected: {problems}")
    check(
        recorded and recorded[0]["name"] == "web_search",
        f"preflight did not record the declared tool for audit: {recorded}",
    )

    for label, health, expected in (
        (
            "sandbox-selectable",
            {"sandbox_selectable_tools": ["bash"], "exposed_tool_surface": []},
            "inference_gateway_lets_the_sandbox_select_tools",
        ),
        (
            "undeclared",
            {"sandbox_selectable_tools": []},
            "inference_gateway_does_not_declare_its_tool_surface",
        ),
        (
            "host access",
            {
                "sandbox_selectable_tools": [],
                "exposed_tool_surface": [{
                    "name": "shell", "scope": "provider-side",
                    "selectable_by_sandbox": False, "grants_host_access": True,
                }],
            },
            "inference_gateway_tool_grants_host_access:shell",
        ),
        (
            "not provider-side",
            {
                "sandbox_selectable_tools": [],
                "exposed_tool_surface": [{
                    "name": "editor", "scope": "local",
                    "selectable_by_sandbox": False, "grants_host_access": False,
                }],
            },
            "inference_gateway_exposes_a_non_provider_tool:editor",
        ),
    ):
        problems, _ = benchmark.tool_surface_problems(health)
        check(expected in problems, f"{label} surface was accepted: {problems}")


def test_model_completions_are_parsed_by_content_not_packaging() -> None:
    """A formatting habit must not cost a work unit.

    Both worker protocols require exactly one JSON object, and both consume a
    paid call per attempt before a unit is retried and finally blocked. Real
    models routinely wrap that object in a preamble or follow it with a closing
    remark; rejecting those spends the call and moves the unit toward being
    blocked for packaging rather than for a wrong answer. Genuine ambiguity -
    no object, or more than one - still has to be refused, because then which
    answer was meant is unknown.
    """
    accepted = {
        "bare object": '{"action":"final"}',
        "fenced with a language tag": '```json\n{"action":"final"}\n```',
        "fenced without one": '```\n{"action":"final"}\n```',
        "preamble then a fence": 'Here is the response:\n```json\n{"action":"final"}\n```',
        "fence then a closing remark": '```json\n{"action":"final"}\n```\nLet me know.',
        "prose then a bare object": 'Sure.\n{"action":"final"}',
        "object then prose": '{"action":"final"}\nDone.',
        "braces and quotes inside strings": '{"a":"} not a brace","b":"say \\"hi\\""}',
    }
    for label, completion in accepted.items():
        try:
            gateway_client.parse_model_json(completion)
        except gateway_client.GatewayClientError as exc:
            check(False, f"a well-formed answer was rejected for its packaging ({label}): {exc}")

    refused = {
        "no object at all": "I'll help with that! Let me think first.",
        "two bare objects": '{"a":1}\n{"b":2}',
        "two fenced objects": '```json\n{"a":1}\n```\n```json\n{"b":2}\n```',
        "unbalanced": '{"a":1',
        "an array, not an object": "[1,2,3]",
        "invalid JSON": '{"a": unquoted}',
    }
    for label, completion in refused.items():
        try:
            gateway_client.parse_model_json(completion)
        except gateway_client.GatewayClientError:
            continue
        check(False, f"an ambiguous or malformed completion was accepted ({label})")


def test_a_truncated_or_declined_completion_says_so() -> None:
    """Name the failure the provider reported instead of the one parsing invents.

    A completion cut off at the output limit is a truncated JSON object, which
    the parser can only describe as "no JSON object". The driver then retries an
    identical request three times and blocks the unit with a diagnosis pointing
    at the model's formatting rather than at the cap that actually stopped it.
    Adaptive thinking bills into the same output budget, so this is the ordinary
    way a large answer fails, not an edge case.
    """
    check(
        gateway_client.completion_problem({"stop_reason": "end_turn"}) is None,
        "a normal completion was reported as a problem",
    )
    for reason in ("max_tokens", "refusal"):
        message = gateway_client.completion_problem({"stop_reason": reason})
        check(bool(message) and message.startswith(reason),
              f"{reason} was not surfaced as its own failure: {message}")
    check(
        "output cap" in (gateway_client.completion_problem({"stop_reason": "max_tokens"}) or ""),
        "the truncation message does not say what to change",
    )

    # The parse failure it would otherwise be mistaken for.
    truncated = '{"schema_version":1,"files":[{"path":"result.json","content":"abc'
    try:
        gateway_client.parse_model_json(truncated)
    except gateway_client.GatewayClientError as exc:
        check(
            "JSON" in str(exc),
            f"a truncated object produced an unexpected error: {exc}",
        )
    else:
        check(False, "a truncated object was accepted")


# --------------------------------------------------------------------------
# 5. The exec provider keeps a local agent session on the trusted side
# --------------------------------------------------------------------------


def test_exec_provider_contract() -> None:
    """A trusted local command is reachable as a provider, and only from there.

    The exec provider is how an already-authenticated local agent CLI is reused
    without letting scored work anywhere near its session. The stub stands in for
    that CLI so the contract itself - conversation on stdin, assistant text on
    stdout, credentials only in the gateway process - is tested offline.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        stub = tmp / "stub_cli.py"
        stub.write_text(
            "#!/usr/bin/env python3\n"
            "import os, sys\n"
            "conversation = sys.stdin.read()\n"
            "# A real CLI answers from its own authenticated session; the stub\n"
            "# reports what it received and whether it holds the credential.\n"
            "sys.stdout.write(\n"
            "    'turns=%d\\n' % conversation.count('<<<')\n"
            "    + 'saw_packet=%s\\n' % ('yes' if 'TASK BODY' in conversation else 'no')\n"
            "    + 'credential_present=%s\\n'\n"
            "      % ('yes' if os.environ.get('ANTHROPIC_API_KEY') else 'no')\n"
            ")\n",
            encoding="utf-8",
        )

        provider = inference_gateway.ExecProvider(
            [sys.executable, str(stub)], timeout=30, model="stub-cli"
        )
        check(provider.describe()["id"] == "exec", "exec provider misidentifies itself")
        check(
            provider.secrets() == [],
            "the exec provider must not surface a credential of its own",
        )

        result = provider.complete({
            "messages": [
                {"role": "system", "content": "system rules"},
                {"role": "user", "content": "TASK BODY"},
            ],
            "max_output_tokens": 256,
            "temperature": 0.0,
            "stop": None,
            "network_allowed": False,
        })
        check("turns=2" in result["content"], f"stub did not receive the conversation: {result}")
        check("saw_packet=yes" in result["content"], f"packet body was not passed: {result}")

        # The credential the gateway holds is the local CLI's environment, which
        # is exactly the trusted side. What matters is that scored work never has
        # it - covered by the packet-only and sandbox-agent end-to-end tests.
        os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
        try:
            trusted = provider.complete({
                "messages": [{"role": "user", "content": "TASK BODY"}],
                "max_output_tokens": 256, "temperature": None, "stop": None,
                "network_allowed": False,
            })
        finally:
            os.environ.pop("ANTHROPIC_API_KEY", None)
        check(
            "credential_present=yes" in trusted["content"],
            "the exec provider did not run on the credential-holding side",
        )

        failing = tmp / "failing_cli.py"
        failing.write_text(
            "import sys\nsys.stderr.write('session expired\\n')\nsys.exit(3)\n",
            encoding="utf-8",
        )
        broken = inference_gateway.ExecProvider([sys.executable, str(failing)], timeout=30)
        try:
            broken.complete({
                "messages": [{"role": "user", "content": "x"}],
                "max_output_tokens": 16, "temperature": None, "stop": None,
                "network_allowed": False,
            })
        except inference_gateway.GatewayError as exc:
            check(
                "session expired" in str(exc),
                f"a failing local CLI did not surface its own error: {exc}",
            )
        else:
            check(False, "a failing local CLI was reported as a successful completion")


def test_exec_provider_errors_reach_the_sandbox_scrubbed() -> None:
    """A provider failure is an infrastructure error, with nothing sensitive in it."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        leaky = tmp / "leaky_cli.py"
        leaky.write_text(
            "import sys\n"
            "sys.stderr.write('auth failed for key %s at %s\\n' %\n"
            "                 (sys.argv[1], sys.argv[2]))\n"
            "sys.exit(1)\n",
            encoding="utf-8",
        )
        provider = inference_gateway.ExecProvider(
            [sys.executable, str(leaky), FAKE_PROVIDER_SECRET, FAKE_HOST_PATH], timeout=30
        )
        try:
            provider.complete({
                "messages": [{"role": "user", "content": "x"}],
                "max_output_tokens": 16, "temperature": None, "stop": None,
                "network_allowed": False,
            })
        except inference_gateway.GatewayError as exc:
            scrubbed = inference_gateway.scrub_outbound(str(exc), provider.secrets())
            check(
                FAKE_PROVIDER_SECRET not in scrubbed and FAKE_HOST_PATH not in scrubbed,
                "a failing local CLI leaked its key or a host path toward the sandbox",
            )
        else:
            check(False, "the leaky stub was expected to fail")


def test_scored_paths_send_no_sampling_parameters() -> None:
    """Scored requests must carry no decoding parameters at all.

    This replaces an earlier guard that asserted a frozen temperature reached the
    gateway. That guard was wrong about the model: Claude Sonnet 5 removed
    temperature, top_p and top_k and rejects a request carrying one with HTTP 400,
    so sending a fixed value would have failed every paid request. What the
    benchmark can freeze is that the decoding state is identical for all languages
    and recorded, which is what section 6.2 actually requires.
    """
    frozen = json.loads((TEMPLATE / "config" / "primary.json").read_text(encoding="utf-8"))
    sampling = frozen["sampling"]
    check(
        sampling["sampling_parameters"] == "omitted"
        and sampling["decoding_state"] == "provider-controlled",
        f"the frozen sampling declaration is not provider-controlled: {sampling}",
    )

    def brokered(log_path: Path) -> list[dict[str, Any]]:
        if not log_path.is_file():
            return []
        return [
            json.loads(line)
            for line in log_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and json.loads(line).get("event") == "inference"
        ]

    for mode, argv_for in (
        ("packet-only", lambda root, agent, sock: [
            sys.executable, str(SCRIPTS / "benchmark.py"), "task-infer",
            "--workspace", str(root), "--id", agent, "--socket", str(sock),
        ]),
        ("sandbox-agent", lambda root, agent, sock: [
            sys.executable, str(SCRIPTS / "sandbox_agent.py"),
            "--workspace", str(root), "--id", agent, "--socket", str(sock),
        ]),
    ):
        with tempfile.TemporaryDirectory() as td:
            root = make_workspace(Path(td))
            agent_id = f"worker-sampling-{mode}"
            create_task(root, agent_id, mode)
            if mode == "packet-only":
                script = {"schema_version": 1, "default": json.dumps({
                    "schema_version": 1, "task_id": agent_id,
                    "files": [{"path": "result.json", "content": '{"schema_version":1}\n'}],
                })}
            else:
                script = {"schema_version": 1, "sequence": [
                    json.dumps({"action": "write_file", "path": "result.json",
                                "content": '{"schema_version":1}\n'}),
                    json.dumps({"action": "final", "summary": "done"}),
                ]}
            with Gateway(root / "gateway", script=script) as gw:
                completed = subprocess.run(
                    argv_for(root, agent_id, gw.socket_path),
                    env=sandbox_side_env(),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                )
                check(
                    completed.returncode == 0,
                    f"{mode} worker failed: {completed.stderr or completed.stdout}",
                )
                records = brokered(gw.log_path)
            check(records, f"{mode} brokered no requests")
            check(
                all(r.get("sampling_parameters") == "omitted" for r in records),
                f"{mode} did not record omitted sampling: {records}",
            )
            check(
                all("temperature" not in r for r in records),
                f"{mode} sent a decoding parameter the model rejects: {records}",
            )


def test_gateway_refuses_sandbox_supplied_decoding_parameters() -> None:
    """Decoding is a trusted-side decision, so the sandbox may not set it."""
    with tempfile.TemporaryDirectory() as td:
        with Gateway(Path(td)) as gw:
            for field in ("temperature", "top_p", "top_k", "thinking", "output_config"):
                response = raw_request(gw.socket_path, {
                    "schema_version": 1,
                    "kind": "inference.request",
                    "request_id": "decoding-probe",
                    "messages": [{"role": "user", "content": "x"}],
                    field: 0.0 if field in {"temperature", "top_p"} else 1,
                })
                check(
                    response.get("kind") == "inference.error"
                    and response["error"]["class"] == "policy",
                    f"the gateway accepted a sandbox-supplied {field}: {response}",
                )


def test_preflight_rejects_an_unfrozen_sampling_declaration() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        config_path = root / "template" / "config" / "primary.json"
        good = json.loads(config_path.read_text(encoding="utf-8"))
        declared = benchmark.sampling_config(root)
        check(
            declared["sampling_parameters"] == "omitted"
            and declared["decoding_state"] == "provider-controlled"
            and declared["effort"] in {"low", "medium", "high", "xhigh", "max"},
            f"the frozen declaration was not read back: {declared}",
        )
        for label, sampling in (
            ("missing", None),
            ("a fixed temperature", {"temperature": 0.0}),
            ("an unrecorded state", {"sampling_parameters": "omitted"}),
            ("an unknown effort", {
                "sampling_parameters": "omitted",
                "decoding_state": "provider-controlled",
                "effort": "turbo",
            }),
        ):
            broken = dict(good)
            if sampling is None:
                broken.pop("sampling", None)
            else:
                broken["sampling"] = sampling
            config_path.write_text(json.dumps(broken, indent=2) + "\n", encoding="utf-8")
            try:
                benchmark.sampling_config(root)
            except benchmark.BenchmarkError:
                pass
            else:
                check(False, f"a sampling section with {label} was accepted")
        config_path.write_text(json.dumps(good, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------
# 5. The exec provider keeps a local agent session on the trusted side
# --------------------------------------------------------------------------


# --------------------------------------------------------------------------
# 6. Configuration stays consistent with the code
# --------------------------------------------------------------------------


def test_frozen_configuration_agrees_with_the_implementation() -> None:
    primary = json.loads((TEMPLATE / "config" / "primary.json").read_text(encoding="utf-8"))
    gateway = json.loads(
        (TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")
    )
    isolation = primary["worker_isolation"]

    check(
        isolation["gateway_protocol"] == gateway["protocol"] == inference_gateway.PROTOCOL,
        "primary.json, the gateway config and the gateway implementation disagree on the protocol",
    )
    check(
        isolation["gateway_socket_path"] == gateway["socket_path"]
        == sandbox_launcher.GATEWAY_SOCKET,
        "the frozen socket path disagrees with what the launcher mounts",
    )
    check(
        isolation["sandbox_provider_credentials"] == "forbidden",
        "primary.json does not forbid provider credentials in the sandbox",
    )
    decoding = gateway["anthropic_decoding"]
    check(
        primary["sampling"]["effort"] == decoding["effort"]
        and primary["sampling"]["orchestration_effort"] == decoding["orchestration_effort"],
        "the declared decoding depths disagree with what the gateway adapter sends",
    )
    for model, pricing in gateway["anthropic_pricing"].items():
        check(
            {"cache_write_usd_per_million_tokens", "cache_read_usd_per_million_tokens"}
            <= set(pricing),
            f"pricing for {model} has no cache rates; cached prefixes would be priced wrongly",
        )
    check(
        gateway["prompt_caching"]["enabled"] is True,
        "prompt caching is off; every sandbox-agent turn would re-buy its whole prefix",
    )
    check(
        gateway["prompt_caching"].get("ttl") == "1h",
        "benchmark prompt caching must survive long Semantic Compression turns",
    )
    for model, pricing in gateway["anthropic_pricing"].items():
        check(
            abs(
                float(pricing["cache_write_usd_per_million_tokens"])
                - 2.0 * float(pricing["input_usd_per_million_tokens"])
            ) < 1e-12,
            f"1h cache writes for {model} are not priced at the provider's 2x input rate",
        )
    check(
        "purpose" not in gateway["forbidden_request_fields"]
        and "effort" in gateway["forbidden_request_fields"],
        "purpose must be a permitted label while effort itself stays refused",
    )

    manifest = json.loads((TEMPLATE / "runtime" / "toolchains.json").read_text(encoding="utf-8"))
    dockerfile = (TEMPLATE / "runtime" / "Dockerfile").read_text(encoding="utf-8")
    check(
        manifest["image"]["base"].split(":")[1] in dockerfile,
        "the Dockerfile base image disagrees with runtime/toolchains.json",
    )
    check(
        "quidra" not in manifest["toolchains"] and "QUIDRA_PIN" not in manifest["toolchains"],
        "the runtime image must not pin a Quidra compiler; it is built from the snapshot",
    )
    languages = json.loads(
        (TEMPLATE / "config" / "benchmark_metadata.json").read_text(encoding="utf-8")
    )["languages"]
    verify = (TEMPLATE / "runtime" / "verify_toolchains.py").read_text(encoding="utf-8")
    for language in languages:
        if language == "Quidra":
            continue
        check(
            f'"{language}"' in verify,
            f"the image build does not verify the pinned {language} toolchain",
        )


# --------------------------------------------------------------------------
# 7. Trials are runtime-owned, batched, and recorded on disk
# --------------------------------------------------------------------------


def test_trials_are_runtime_owned_fresh_sessions() -> None:
    """A trial is a fresh session the runtime records verbatim, in batches too.

    The first paid run spent most of its budget on one action turn per trial:
    every trial start, every write and every compile was its own model turn on
    an ever-growing conversation. The batch form and the on-disk record exist so
    the agent can run a whole cell in one turn and process it with a script.
    """
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        agent_id = "worker-trials"
        agent_dir = create_task(root, agent_id, "sandbox-agent")
        # The trial budget comes from the frozen manifest, never from the agent.
        (root / "work" / "root" / "manifest.json").write_text(json.dumps({
            "schema_version": 1,
            "work_units": [{
                "id": "trials-unit", "assigned_agent_id": agent_id,
                "evaluation": "synthetic_trials", "max_llm_calls": 4,
            }],
        }), encoding="utf-8")
        task_path = agent_dir / "task.json"
        task = json.loads(task_path.read_text(encoding="utf-8"))
        task["evaluation"] = "synthetic_trials"
        task_path.write_text(json.dumps(task), encoding="utf-8")

        actions = [
            # A whole batch is refused before any call when it exceeds the budget.
            {"action": "trial_start", "trials": [
                {"trial_id": f"c-t{n}", "prompt": f"TRIAL-PROMPT {n}"} for n in range(5)
            ]},
            {"action": "trial_start", "trials": [
                {"trial_id": "c-t1", "prompt": "TRIAL-PROMPT one"},
                {"trial_id": "c-t2", "prompt": "TRIAL-PROMPT two"},
            ]},
            # The record is runtime-owned even though it sits in the agent's directory.
            {"action": "write_file", "path": "trials/c-t1/completion_01.txt", "content": "forged"},
            {"action": "read_file", "path": "trials/c-t1/completion_01.txt"},
            {"action": "trial_continue", "trial_id": "c-t1", "message": "TRIAL-PROMPT repair"},
            {"action": "trial_start", "trial_id": "c-t1", "prompt": "TRIAL-PROMPT again"},
            {"action": "trial_start", "trial_id": "c-t3", "prompt": "TRIAL-PROMPT three"},
            # Budget is 4: two in the batch, one repair, one single start. This is the fifth.
            {"action": "trial_start", "trial_id": "c-t4", "prompt": "TRIAL-PROMPT four"},
            {"action": "write_file", "path": "result.json", "content": '{"schema_version":1}\n'},
            {"action": "final", "summary": "done"},
        ]
        script = {
            "schema_version": 1,
            # Trial prompts are answered by content; the agent's own turns by order.
            "rules": [{"contains": "TRIAL-PROMPT", "content": "fn main() {}"}],
            "sequence": [json.dumps(a) for a in actions],
        }
        with Gateway(root / "gateway", script=script) as gw:
            completed = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path), "--max-output-tokens", "4000",
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            audit = [
                json.loads(line) for line in gw.log_path.read_text(encoding="utf-8").splitlines()
                if line.strip() and json.loads(line).get("event") == "inference"
            ]
        check(completed.returncode == 0, f"the trial agent failed: {completed.stderr or completed.stdout}")
        trace = json.loads((agent_dir / "agent_trace.json").read_text(encoding="utf-8"))
        by_turn = {entry["turn"]: entry for entry in trace["trace"]}

        check("exceeds the remaining trial budget" in str(by_turn[1]["observation"].get("denied")),
              f"an oversized batch was not refused up front: {by_turn[1]}")
        batch = by_turn[2]["observation"]
        check(batch.get("ok") is True and len(batch.get("trials", [])) == 2
              and all("completion" not in t for t in batch["trials"])
              and all(t.get("completion_path", "").startswith("trials/") for t in batch["trials"]),
              f"the batch observation is not the on-disk summary it should be: {batch}")
        check("runtime-owned" in str(by_turn[3]["observation"].get("denied")),
              f"the worker could write into the trial record: {by_turn[3]}")
        # The trace elides file contents; the read itself succeeded on the record.
        check(by_turn[4]["observation"].get("ok") is True
              and str(by_turn[4]["observation"].get("content", "")).startswith("<12 chars"),
              f"the worker could not read the trial record: {by_turn[4]}")
        repair = by_turn[5]["observation"]
        check(repair.get("ok") is True and repair.get("repairs_used") == 1
              and repair.get("completion_chars") == len("fn main() {}")
              and repair.get("completion_path") == "trials/c-t1/completion_02.txt",
              f"a single repair turn did not behave: {repair}")
        check("already exists" in str(by_turn[6]["observation"].get("denied")),
              f"a trial was allowed to start twice: {by_turn[6]}")
        check(by_turn[7]["observation"].get("ok") is True
              and by_turn[7]["observation"].get("calls_remaining") == 0,
              f"the fourth trial call was not accounted: {by_turn[7]}")
        check("budget exhausted" in str(by_turn[8]["observation"].get("denied")),
              f"a fifth trial call was allowed past the frozen budget: {by_turn[8]}")

        record = agent_dir / "trials" / "c-t1"
        check((record / "prompt_01.txt").read_text(encoding="utf-8") == "TRIAL-PROMPT one"
              and (record / "completion_01.txt").read_text(encoding="utf-8") == "fn main() {}"
              and (record / "prompt_02.txt").read_text(encoding="utf-8") == "TRIAL-PROMPT repair"
              and (record / "session.json").is_file(),
              "trial prompts and completions were not recorded verbatim on disk")
        summary = trace["trials"]
        check(summary["budget"] == 4 and summary["used"] == 4
              and set(summary["trials"]) == {"c-t1", "c-t2", "c-t3"},
              f"the trace does not account the trials: {summary}")

        purposes = [r.get("purpose") for r in audit]
        check(purposes.count("scored") == 4 and purposes.count("orchestration") == len(actions),
              f"the audit log does not separate scored trial calls from action turns: {purposes}")
        # Trial sessions are fresh: the scored requests never carry the agent's conversation.
        check(all("stop_reason" in r for r in audit), "the audit log does not record stop reasons")


def test_paid_trial_calls_resume_from_trusted_checkpoint_after_runtime_loss() -> None:
    """Trusted call checkpoints survive loss of worker-side trial bookkeeping."""
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        agent_id = "worker-trial-resume"
        agent_dir = create_task(root, agent_id, "sandbox-agent")
        benchmark.json_dump(root / "work" / "root" / "manifest.json", {
            "schema_version": 1,
            "work_units": [{
                "id": "trial-resume-unit",
                "assigned_agent_id": agent_id,
                "evaluation": "synthetic_trials",
                "max_llm_calls": 2,
            }],
        })
        task_path = agent_dir / "task.json"
        task = json.loads(task_path.read_text(encoding="utf-8"))
        task["evaluation"] = "synthetic_trials"
        task_path.write_text(json.dumps(task), encoding="utf-8")

        first_actions = [
            {"action": "trial_start", "trials": [
                {"trial_id": "r-t1", "prompt": "RESUME-TRIAL one"},
                {"trial_id": "r-t2", "prompt": "RESUME-TRIAL two"},
            ]},
            {"action": "write_file", "path": "result.json",
             "content": '{"schema_version":1}\n'},
            {"action": "final", "summary": "first pass"},
        ]
        first_script = {
            "schema_version": 1,
            "rules": [{"contains": "RESUME-TRIAL", "content": "paid completion"}],
            "sequence": [json.dumps(a) for a in first_actions],
        }
        with Gateway(root / "gateway", script=first_script) as gw:
            first = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path), "--max-output-tokens", "4000",
                ],
                env=sandbox_side_env(), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True,
            )
        check(first.returncode == 0, f"journal setup run failed: {first.stderr or first.stdout}")
        journal = json.loads(
            (agent_dir / "trial_call_journal.json").read_text(encoding="utf-8")
        )
        check(
            [(row["trial_id"], row["call"]) for row in journal.get("calls", [])]
            == [("r-t1", 1), ("r-t2", 1)],
            f"paid calls were not journaled individually: {journal}",
        )

        trusted_calls = root / "work/root/trial-checkpoints" / agent_id
        check(
            (trusted_calls / "r-t1/call_01.json").is_file()
            and (trusted_calls / "r-t2/call_01.json").is_file(),
            "paid calls were not committed to trusted checkpoint storage",
        )

        # Model a crash after the trusted provider-reply commit but before any
        # worker-side trial bookkeeping can be relied on. Keep only trusted call
        # checkpoints plus the empty prior orchestration trace.
        previous = json.loads((agent_dir / "agent_trace.json").read_text(encoding="utf-8"))
        previous["trace"] = []
        previous["trials"] = {"trials": {}}
        benchmark.json_dump(agent_dir / "resume_trace.json", previous)
        shutil.rmtree(agent_dir / "trials")
        (agent_dir / "trial_call_journal.json").unlink()
        (agent_dir / "agent_trace.json").unlink()
        (agent_dir / "result.json").unlink()

        second_actions = [
            {"action": "write_file", "path": "result.json",
             "content": '{"schema_version":1}\n'},
            {"action": "final", "summary": "resumed without buying trials again"},
        ]
        retry_gateway = root / "gateway-retry"
        retry_gateway.mkdir()
        with Gateway(
            retry_gateway,
            script={"schema_version": 1, "sequence": [json.dumps(a) for a in second_actions]},
        ) as gw:
            second = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path), "--max-output-tokens", "4000",
                ],
                env=sandbox_side_env(), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True,
            )
            second_audit = [
                json.loads(line)
                for line in gw.log_path.read_text(encoding="utf-8").splitlines()
                if line.strip() and json.loads(line).get("event") == "inference"
            ]

        check(
            second.returncode == 0,
            f"journal-backed retry failed: {second.stderr or second.stdout}",
        )
        final_trace = json.loads(
            (agent_dir / "agent_trace.json").read_text(encoding="utf-8")
        )
        recovered = [
            row for row in final_trace.get("trace", [])
            if row.get("runtime_recovery") == "paid-call-journal-v1"
        ]
        check(
            len(recovered) == 2
            and {row.get("observation", {}).get("trial_id") for row in recovered}
            == {"r-t1", "r-t2"},
            f"journaled calls were not reconstructed into the audit trace: {recovered}",
        )
        rebuilt_journal = json.loads(
            (agent_dir / "trial_call_journal.json").read_text(encoding="utf-8")
        )
        check(
            [(row["trial_id"], row["call"]) for row in rebuilt_journal.get("calls", [])]
            == [("r-t1", 1), ("r-t2", 1)],
            f"trusted checkpoints did not rebuild the paid-call journal: {rebuilt_journal}",
        )
        check(
            (agent_dir / "trials/r-t1/session.json").is_file()
            and (agent_dir / "trials/r-t2/session.json").is_file(),
            "trusted checkpoints did not rebuild trial sessions",
        )
        check(
            final_trace.get("trials", {}).get("used") == 2,
            f"restored calls did not consume the frozen call budget: {final_trace.get('trials')}",
        )
        check(
            not any(row.get("purpose") == "scored" for row in second_audit),
            f"the retry repurchased a scored call: {second_audit}",
        )


def test_gateway_maps_purpose_to_a_frozen_depth_and_refuses_the_rest() -> None:
    with tempfile.TemporaryDirectory() as td:
        with Gateway(Path(td)) as gw:
            for purpose, ok in (("scored", True), ("orchestration", True), ("turbo", False)):
                response = raw_request(gw.socket_path, {
                    "schema_version": 1, "kind": "inference.request",
                    "request_id": f"purpose-{purpose}", "purpose": purpose,
                    "messages": [{"role": "user", "content": "x"}],
                })
                check(
                    (response.get("kind") == "inference.response") is ok,
                    f"purpose {purpose!r} handled wrongly: {response}",
                )
                if not ok:
                    check(response.get("error", {}).get("class") == "protocol", response)
            records = [
                json.loads(line) for line in gw.log_path.read_text(encoding="utf-8").splitlines()
                if line.strip() and json.loads(line).get("event") == "inference"
            ]
        check(
            [r.get("purpose") for r in records] == ["scored", "orchestration"],
            f"the audit log does not record request purposes: {records}",
        )

    config = json.loads((TEMPLATE / "config" / "inference_gateway.json").read_text("utf-8"))
    os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
    try:
        provider = inference_gateway.AnthropicMessagesProvider(
            "claude-sonnet-5", timeout=30,
            pricing=config["anthropic_pricing"]["claude-sonnet-5"],
            decoding=config["anthropic_decoding"],
            caching=config["prompt_caching"],
        )
    finally:
        os.environ.pop("ANTHROPIC_API_KEY", None)
    check(
        provider.effort_for("scored") == config["anthropic_decoding"]["effort"]
        and provider.effort_for("orchestration") == config["anthropic_decoding"]["orchestration_effort"]
        and provider.effort_for(None) == config["anthropic_decoding"]["effort"],
        "the adapter does not map purposes to the frozen depths",
    )
    # Cached prefixes are priced at their own rates, not as fresh input.
    pricing = config["anthropic_pricing"]["claude-sonnet-5"]
    cost = provider._request_cost({
        "input_tokens": 1_000_000, "cache_creation_input_tokens": 1_000_000,
        "cache_read_input_tokens": 1_000_000, "output_tokens": 0,
    })
    expected = (
        pricing["input_usd_per_million_tokens"]
        + pricing["cache_write_usd_per_million_tokens"]
        + pricing["cache_read_usd_per_million_tokens"]
    )
    check(abs(cost - expected) < 1e-9, f"cache tokens are mispriced: {cost} != {expected}")


def test_gateway_enforces_per_task_spend_ceilings() -> None:
    """One unit must not be able to spend the run's budget by itself."""
    with tempfile.TemporaryDirectory() as td:
        script = {"schema_version": 1, "default": "ok", "usage_cost_usd_per_call": 1.0}
        policy = {
            "schema_version": 1,
            "tasks": {"capped": "disabled", "free": "disabled"},
            "budgets": {"capped": 2.5},
        }
        with Gateway(Path(td), script=script, task_policy=policy) as gw:
            outcomes = []
            for _ in range(4):
                response = raw_request(gw.socket_path, {
                    "schema_version": 1, "kind": "inference.request",
                    "request_id": "spend", "task_id": "capped",
                    "messages": [{"role": "user", "content": "x"}],
                })
                outcomes.append(response.get("kind"))
            # 1.0 + 1.0 + 1.0 = 3.0 >= 2.5 after three calls; the fourth is refused.
            check(
                outcomes == ["inference.response"] * 3 + ["inference.error"],
                f"the spend ceiling did not stop the fourth call: {outcomes}",
            )
            refused = raw_request(gw.socket_path, {
                "schema_version": 1, "kind": "inference.request",
                "request_id": "spend", "task_id": "capped",
                "messages": [{"role": "user", "content": "x"}],
            })
            check(
                refused.get("error", {}).get("class") == "policy"
                and "spend ceiling" in refused["error"]["message"],
                f"the refusal is not a policy refusal naming the ceiling: {refused}",
            )
            other = raw_request(gw.socket_path, {
                "schema_version": 1, "kind": "inference.request",
                "request_id": "spend", "task_id": "free",
                "messages": [{"role": "user", "content": "x"}],
            })
            check(other.get("kind") == "inference.response",
                  f"a ceiling on one task leaked onto another: {other}")

    production_run = load(SCRIPTS / "production_run.py", "isolation_tests_production_run")
    check(
        production_run.classify_failure("inference gateway refused this worker's request: task spend ceiling reached")
        == "budget-plan-defect"
        and not production_run.is_retryable("task spend ceiling reached")
        and not production_run.is_retryable("packet-only worker response is incomplete: max_tokens: ..."),
        "a deterministic failure would be retried at full price",
    )
    check(
        not production_run.is_retryable("sandbox agent error: protocol_contract_violated")
        and not production_run.is_retryable("trial integrity: stopped after 0 repairs"),
        "exhausted protocol repair or integrity failure bought another whole-unit retry",
    )
    check(
        production_run.is_retryable("sandbox agent error: inference transport failure: timed out"),
        "a transient failure would not be retried",
    )


# --------------------------------------------------------------------------
# 8. What the first paid run exposed: the fixes stay fixed
# --------------------------------------------------------------------------


def test_sanitized_environment_keeps_toolchain_homes_and_the_built_compiler() -> None:
    """Scored subprocesses see the image's toolchain locations and the target build.

    rustc in the runtime image is a rustup proxy; without RUSTUP_HOME it looks
    under the sandbox home, finds nothing, and tries to download a toolchain
    offline. The first paid run measured Rust with a compiler that never ran.
    """
    with tempfile.TemporaryDirectory() as td:
        root = Path(td).resolve() / "ws"
        for d in ("work/root/target-build", "home", "tmp"):
            (root / d).mkdir(parents=True)
        planted = {
            "RUSTUP_HOME": "/opt/rust-test", "CARGO_HOME": "/opt/rust-test",
            "JAVA_HOME": "/opt/java-test", "ANTHROPIC_API_KEY": FAKE_PROVIDER_SECRET,
            "SSH_AUTH_SOCK": "/tmp/agent.sock",
        }
        saved = {k: os.environ.get(k) for k in planted}
        os.environ.update(planted)
        try:
            env = benchmark.sanitized_subprocess_env(root, root)
            check(
                env.get("RUSTUP_HOME") == "/opt/rust-test"
                and env.get("CARGO_HOME") == "/opt/rust-test"
                and env.get("JAVA_HOME") == "/opt/java-test",
                f"toolchain locations were stripped from the scored environment: {env}",
            )
            check(
                "ANTHROPIC_API_KEY" not in env and "SSH_AUTH_SOCK" not in env,
                f"a credential survived environment sanitization: {sorted(env)}",
            )
            target = root / "work" / "root" / "target-build"
            check(
                str(target) not in env["PATH"],
                "an absent target build was put on PATH",
            )
            compiler = target / "quidra"
            compiler.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            compiler.chmod(0o755)
            env = benchmark.sanitized_subprocess_env(root, root)
            check(
                env["PATH"].startswith(f"{target}:"),
                f"the built compiler's directory does not lead PATH: {env['PATH']}",
            )
            check(
                shutil.which("quidra", path=env["PATH"]) == str(compiler),
                "`quidra` on the scored PATH does not resolve to the target build",
            )
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value


def test_ledger_updates_are_serialized_across_processes() -> None:
    """Concurrent workers must not lose each other's ledger transitions."""
    with tempfile.TemporaryDirectory() as td:
        root = Path(td).resolve() / "ws"
        (root / "work" / "root").mkdir(parents=True)
        ids = [f"unit-{n:02d}" for n in range(24)]
        manifest = {"schema_version": 1, "work_units": [{"id": uid} for uid in ids]}
        manifest_path = root / "work" / "root" / "manifest.json"
        benchmark.json_dump(manifest_path, manifest)
        benchmark.json_dump(root / "work" / "root" / "ledger.json", {
            "schema_version": 1,
            "manifest_sha256": benchmark.sha256_file(manifest_path),
            "units": {
                uid: {"status": "PENDING", "attempts": 0, "max_attempts": 3, "attempt_history": []}
                for uid in ids
            },
        })

        def transition(uid: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "benchmark.py"), "ledger-update",
                    "--workspace", str(root), "--id", uid, "--status", "RUNNING",
                ],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )

        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(ids)) as pool:
            results = list(pool.map(transition, ids))
        failed = [r.stderr for r in results if r.returncode != 0]
        check(not failed, f"ledger updates failed: {failed[:3]}")
        ledger = json.loads((root / "work" / "root" / "ledger.json").read_text(encoding="utf-8"))
        running = [uid for uid in ids if ledger["units"][uid]["status"] == "RUNNING"]
        check(
            len(running) == len(ids),
            f"concurrent ledger updates were lost: {len(running)}/{len(ids)} survived",
        )
        check(
            (root / "work" / "root" / "ledger.lock").exists(),
            "the ledger lock file was never created",
        )


class _ScriptedHTTP:
    """Stand-in for the provider's HTTP round trip: canned bodies, captured payloads."""

    def __init__(self, bodies: list[dict[str, Any]]) -> None:
        self.bodies = list(bodies)
        self.payloads: list[dict[str, Any]] = []
        self.streamed: list[bool] = []
        self.headers: list[dict[str, str]] = []

    def __call__(self, http_request: Any, *, stream: bool = False) -> dict[str, Any]:
        self.payloads.append(json.loads(http_request.data.decode("utf-8")))
        self.streamed.append(stream)
        self.headers.append({k.lower(): v for k, v in http_request.header_items()})
        if not self.bodies:
            raise AssertionError("the provider made more requests than the script allows")
        return self.bodies.pop(0)


def _anthropic_provider() -> Any:
    config = json.loads((TEMPLATE / "config" / "inference_gateway.json").read_text("utf-8"))
    os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
    try:
        return inference_gateway.AnthropicMessagesProvider(
            "claude-sonnet-5", timeout=30,
            pricing=config["anthropic_pricing"]["claude-sonnet-5"],
            web_search=config["anthropic_web_search"],
            decoding=config["anthropic_decoding"],
            caching=config["prompt_caching"],
        )
    finally:
        os.environ.pop("ANTHROPIC_API_KEY", None)


def _validated_request(**overrides: Any) -> dict[str, Any]:
    request = {
        "request_id": "r", "task_id": "t",
        "messages": [{"role": "user", "content": "Reply with ready"}],
        "max_output_tokens": 64, "stop": None, "network_allowed": False, "purpose": "scored",
    }
    request.update(overrides)
    return request


def test_a_frozen_task_depth_replaces_the_run_wide_effort() -> None:
    """The trusted side may pin one task's scored depth below the run-wide one.

    Semantic Compression packets carry about 180k tokens of frozen matrix, and
    at high effort the model spent its whole 65,536-token cap reasoning before
    writing a score. The frozen policy now names a depth per task; the adapter
    sends it as output_config.effort, and the sandbox still cannot set it.
    """
    provider = _anthropic_provider()
    http = _ScriptedHTTP([
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn",
         "usage": {"input_tokens": 10, "output_tokens": 3}},
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn",
         "usage": {"input_tokens": 10, "output_tokens": 3}},
    ])
    provider._open = http
    provider.complete(_validated_request(frozen_effort="medium"))
    provider.complete(_validated_request())
    check(
        http.payloads[0]["output_config"] == {"effort": "medium"}
        and http.payloads[1]["output_config"] == {"effort": provider.effort_for("scored")},
        f"the frozen task depth was not sent: {[p.get('output_config') for p in http.payloads]}",
    )
    validated = inference_gateway.validate_inference_request(
        {"schema_version": 1, "kind": "inference.request", "request_id": "x", "task_id": "t",
         "messages": [{"role": "user", "content": "x"}], "frozen_effort": "low"},
        json.loads((TEMPLATE / "config" / "inference_gateway.json").read_text(encoding="utf-8")),
        "disabled", {},
    )
    check("frozen_effort" not in validated, "a sandbox-supplied frozen_effort survived validation")


def test_provider_recovers_empty_completions_and_refused_tool_calls() -> None:
    """The three provider-side accidents of the first paid run, each handled once.

    A normal end_turn with no text is re-requested once; a turn that stops on a
    client tool call is answered with an error result so the model can finish;
    a turn cut off by max_tokens is not retried, because the same request would
    be cut off again at the same price.
    """
    provider = _anthropic_provider()

    # 1. Empty end_turn -> one identical retry, usage summed across both.
    http = _ScriptedHTTP([
        {"content": [{"type": "thinking", "thinking": ""}], "stop_reason": "end_turn",
         "usage": {"input_tokens": 10, "output_tokens": 300}},
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn",
         "usage": {"input_tokens": 10, "output_tokens": 3}},
    ])
    provider._open = http
    result = provider.complete(_validated_request())
    check(result["content"] == "ready", f"the retried completion was not returned: {result}")
    check(result["empty_completion_retries"] == 1, f"the empty retry was not counted: {result}")
    check(result["usage"]["output_tokens"] == 303, f"retry usage was not summed: {result['usage']}")
    check(
        len(http.payloads) == 2 and http.payloads[0]["messages"] == http.payloads[1]["messages"],
        "the retry did not re-send the identical request",
    )

    # 2. tool_use -> refused with an error tool_result, turn continued to the answer.
    http = _ScriptedHTTP([
        {"content": [
            {"type": "text", "text": "partial "},
            {"type": "tool_use", "id": "tu_1", "name": "web_search", "input": {"query": "x"}},
         ], "stop_reason": "tool_use", "usage": {"output_tokens": 20}},
        {"content": [{"type": "text", "text": '{"ok":true}'}], "stop_reason": "end_turn",
         "usage": {"output_tokens": 5}},
    ])
    provider._open = http
    result = provider.complete(_validated_request(network_allowed=True))
    check(result["content"] == 'partial {"ok":true}', f"the continued turn lost text: {result}")
    check(
        result["tool_use_refusals"] == 1 and result["continuations"] == 1
        and result["stop_reason"] == "end_turn",
        f"the tool_use stop was not refused-and-continued: {result}",
    )
    tail = http.payloads[1]["messages"][-2:]
    check(
        tail[0]["role"] == "assistant"
        and any(block.get("type") == "tool_use" for block in tail[0]["content"])
        and tail[1]["role"] == "user"
        and tail[1]["content"][0]["type"] == "tool_result"
        and tail[1]["content"][0]["tool_use_id"] == "tu_1"
        and tail[1]["content"][0]["is_error"] is True,
        f"the continuation did not answer the tool call with an error result: {tail}",
    )
    check("tools" in http.payloads[0], "a network-enabled request carried no web-search tool")

    # 3. max_tokens with no text -> reported as-is, not retried.
    http = _ScriptedHTTP([
        {"content": [], "stop_reason": "max_tokens", "usage": {"output_tokens": 64}},
    ])
    provider._open = http
    result = provider.complete(_validated_request())
    check(
        result["stop_reason"] == "max_tokens" and result["empty_completion_retries"] == 0
        and len(http.payloads) == 1,
        f"a max_tokens cut-off was retried at full price: {result}",
    )
    check(
        gateway_client.completion_problem({"stop_reason": "tool_use"}) is not None,
        "a lingering tool_use stop is not classified as incomplete",
    )


def test_orchestration_turns_run_without_thinking_and_scored_turns_do_not_change() -> None:
    provider = _anthropic_provider()
    for purpose, expect_thinking in (("orchestration", True), ("scored", False), (None, False)):
        http = _ScriptedHTTP([
            {"content": [{"type": "text", "text": "{}"}], "stop_reason": "end_turn", "usage": {}},
        ])
        provider._open = http
        provider.complete(_validated_request(purpose=purpose))
        payload = http.payloads[0]
        has_thinking = payload.get("thinking") == {"type": "disabled"}
        check(
            has_thinking is expect_thinking,
            f"purpose {purpose!r} sent thinking={payload.get('thinking')!r}",
        )
        check(
            payload["output_config"]["effort"] == provider.effort_for(purpose),
            f"purpose {purpose!r} was decoded at the wrong depth: {payload.get('output_config')}",
        )
        check(
            all(key not in payload for key in ("temperature", "top_p", "top_k")),
            f"purpose {purpose!r} sent a sampling parameter: {sorted(payload)}",
        )
    described = provider.describe()
    check(
        described.get("orchestration_thinking") == "disabled"
        and described.get("empty_completion_retries") == 1,
        f"the handshake does not describe the frozen orchestration decoding: {described}",
    )


def test_cache_checkpoint_skips_units_it_cannot_certify() -> None:
    """One uncertifiable unit costs one record, not the whole checkpoint."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td).resolve()
        root = make_workspace(tmp)
        run = json.loads((root / "run.json").read_text(encoding="utf-8"))
        run["inference_identity"] = {"provider": "anthropic-messages", "model": "claude-sonnet-5"}
        run["created_at_utc"] = "2026-09-22T05:00:00+00:00"
        (root / "run.json").write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
        benchmark.json_dump(root / "results" / "toolchains.json", {
            "schema_version": 1,
            "toolchains": {"Python": {"canonical": "3.12.3"}, "Rust": {"canonical": "1.95.0"}},
            "missing": [], "ok": True,
        })

        def unit(uid: str, evaluation: str, language: str, mode: str, requirement: str) -> dict[str, Any]:
            return {
                "id": uid, "evaluation": evaluation, "assigned_languages": [language],
                "execution_kind": "agent", "result_kind": "requirements", "phase": "measurement",
                "requirement_ids": [requirement], "input_hashes": {}, "validator_command": "true",
                "worker_mode": mode, "network_allowed": False, "assigned_agent_id": f"worker-{uid}",
                "dependencies": [],
            }

        units = [
            unit("eco-python", "ecosystem", "Python", "packet-only", "metric.x"),
            unit("learn-rust", "llm_learnability", "Rust", "sandbox-agent", "condition.i1"),
        ]
        manifest_path = root / "work" / "root" / "manifest.json"
        benchmark.json_dump(manifest_path, {"schema_version": 1, "work_units": units})
        benchmark.json_dump(root / "work" / "root" / "ledger.json", {
            "schema_version": 1, "manifest_sha256": benchmark.sha256_file(manifest_path),
            "units": {u["id"]: {"status": "COMPLETE", "validation_result": "PASS"} for u in units},
        })
        for u in units:
            agent_dir = root / "work" / "agents" / u["assigned_agent_id"]
            agent_dir.mkdir(parents=True)
            benchmark.json_dump(agent_dir / "task.json", {"prompt_sha256": "a" * 64, "read_paths": []})
            benchmark.json_dump(agent_dir / "result.json", {"schema_version": 1, "requirements": {}})
        # learn-rust has no agent_trace.json, so its certification cannot succeed.

        source = tmp / "source"
        source.mkdir()
        promotion = benchmark.promote_certified_cache(source, root)
        check(
            promotion["promoted"] == 1
            and [r["work_unit_id"] for r in promotion["records"]] == ["eco-python"],
            f"the certifiable unit was not promoted: {promotion}",
        )
        check(
            [s["work_unit_id"] for s in promotion.get("skipped", [])] == ["learn-rust"]
            and "agent_trace.json" in promotion["skipped"][0]["reason"],
            f"the uncertifiable unit was not skipped with its reason: {promotion}",
        )
        written = list((source / "benchmark" / "cache").rglob("*.json"))
        check(len(written) == 1, f"expected exactly one cache record on disk: {written}")


def test_semantic_compression_metrics_are_recomputed_from_the_evidence() -> None:
    """A shard's invented 0-100 transform must not reach the ranking.

    Methodology 6.1.4 freezes each metric's raw value and its direction, not
    the scale, and a shard sees one language. In the first full run that left
    one language reporting min(100, raw*1000) = 82 beside another's raw*100 =
    11.77 for the same metric, which the harmonic combination would have mixed
    into a ranking had the comparability gate not stopped it.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td).resolve()
        root = make_workspace(tmp)
        config = json.loads(
            (root / "template" / "config" / "aggregation.json").read_text(encoding="utf-8")
        )["evaluations"]["semantic_compression"]
        languages = benchmark.metadata_languages(root)
        probes = [f"F{index:02d}.P1" for index in range(1, 21)]
        matrix = benchmark.json_load(
            root / "template" / "methodology-assets" / "semantic_compression"
            / "semantic_site_matrix.json"
        )
        support_probes = [str(probe["probe_id"]) for probe in matrix["probes"]]
        support_owner_id = config["support_level_owner"]["requirement_id"]

        def write_shards(invented: dict[str, float]) -> None:
            units = []
            for position, language in enumerate(languages):
                # Current SC derives support/capability points from one
                # canonical probe owner per language × probe. This test is
                # about runner-side normalization, so give every synthetic
                # probe an unambiguous FULL owner rather than relying on the
                # retired language-wide coverage owner as fragment authority.
                for probe_id in support_probes:
                    probe_slug = probe_id.lower().replace(".", "-")
                    owner_probe_uid = (
                        f"sc-canonical--{benchmark.slug_id(language)}--{probe_slug}"
                    )
                    owner_probe_agent = f"worker-{owner_probe_uid}"
                    units.append({
                        "id": owner_probe_uid,
                        "evaluation": "semantic_compression",
                        "assigned_languages": [language],
                        "assigned_agent_id": owner_probe_agent,
                        "execution_kind": "agent",
                        "result_kind": "requirements",
                        "phase": "measurement",
                        "requirement_ids": [
                            benchmark.CANONICAL_FRAGMENT_PREFIX + probe_slug
                        ],
                        "input_hashes": {},
                        "validator_command": "true",
                        "worker_mode": "packet-only",
                        "network_allowed": False,
                        "dependencies": [],
                        "canonical_fragment_owner": True,
                        "canonical_probe_id": probe_id,
                    })
                    probe_dir = root / "work" / "agents" / owner_probe_agent
                    probe_dir.mkdir(parents=True, exist_ok=True)
                    benchmark.json_dump(probe_dir / "result.json", {
                        "schema_version": 1,
                        "evaluation": "semantic_compression",
                        "requirements": {},
                        "evidence": {
                            "canonical_fragments": {
                                probe_id: {
                                    "level": "FULL",
                                    "fragment": f"synthetic_{probe_slug}()",
                                    "partial_reasons": [],
                                    "none_reason": None,
                                    "justification": (
                                        "synthetic FULL support for runner normalization"
                                    ),
                                    "citation": "synthetic isolation fixture",
                                }
                            }
                        },
                    })

                owner_uid = f"sc-capability-coverage--{language.lower()}"
                owner_agent = f"worker-{owner_uid}"
                units.append({
                    "id": owner_uid, "evaluation": "semantic_compression",
                    "assigned_languages": [language], "assigned_agent_id": owner_agent,
                    "execution_kind": "agent", "result_kind": "requirements",
                    "phase": "measurement", "requirement_ids": [support_owner_id],
                    "input_hashes": {}, "validator_command": "true",
                    "worker_mode": "packet-only", "network_allowed": False,
                    "dependencies": [],
                })
                owner_dir = root / "work" / "agents" / owner_agent
                owner_dir.mkdir(parents=True, exist_ok=True)
                benchmark.json_dump(owner_dir / "result.json", {
                    "schema_version": 1, "evaluation": "semantic_compression",
                    "requirements": {support_owner_id: {language: 100.0}},
                    "evidence": {support_owner_id: {"per_probe": [
                        {"probe_id": probe_id, "support": "FULL"}
                        for probe_id in support_probes
                    ]}},
                })

                sites, tokens = 4 + position, 40
                for metric in config["recompute_from_evidence"]["metrics"]:
                    uid = f"sc-{metric.rsplit('.', 1)[-1]}--{language.lower()}"
                    agent = f"worker-{uid}"
                    units.append({
                        "id": uid, "evaluation": "semantic_compression",
                        "assigned_languages": [language], "assigned_agent_id": agent,
                        "execution_kind": "agent", "result_kind": "requirements",
                        "phase": "measurement", "requirement_ids": [metric],
                        "input_hashes": {}, "validator_command": "true",
                        "worker_mode": "packet-only", "network_allowed": False,
                        "dependencies": [],
                    })
                    if metric == "metric.capability_efficiency":
                        evidence = {
                            "total_semantic_complexity_units": 1.0 + position / 10
                        }
                    else:
                        field = {
                            "metric.semantic_density": "explicit_local_facts",
                            "metric.semantic_determinacy": "B",
                            "metric.semantic_locality": "lookups",
                            "metric.hidden_semantic_cost": "count",
                        }[metric]
                        evidence = {"per_probe": [
                            {"probe_id": probe, field: sites, "tokens": tokens}
                            for probe in probes
                        ]}
                    agent_dir = root / "work" / "agents" / agent
                    agent_dir.mkdir(parents=True, exist_ok=True)
                    benchmark.json_dump(agent_dir / "result.json", {
                        "schema_version": 1, "evaluation": "semantic_compression",
                        # what the shard invented for itself, which must not matter
                        "requirements": {metric: {language: invented[language]}},
                        "evidence": {metric: evidence},
                    })
            benchmark.json_dump(
                root / "work" / "root" / "manifest.json",
                {"schema_version": 1, "work_units": units},
            )

        honest = {language: 10.0 for language in languages}
        write_shards(honest)
        first = benchmark.sc_raw_values(root, config, languages)

        # The same measurements, reported through wildly different transforms.
        invented = dict(honest)
        invented[languages[0]] = 82.0
        invented[languages[1]] = 39.0
        write_shards(invented)
        second = benchmark.sc_raw_values(root, config, languages)
        check(first == second, "a shard's own 0-100 transform changed the raw values")

        density = benchmark.sc_normalize(
            first["metric.semantic_density"], "higher_is_better"
        )
        check(
            density[languages[-1]] == 100.0 and density[languages[0]] == 0.0,
            f"density was not normalized across the cohort: {density}",
        )
        efficiency = benchmark.sc_normalize(
            first["metric.capability_efficiency"], "lower_is_better"
        )
        check(
            efficiency[languages[0]] == 100.0 and efficiency[languages[-1]] == 0.0,
            f"a lower-is-better metric was not inverted: {efficiency}",
        )
        check(
            len(first["metric.semantic_density"]) == len(languages),
            "the recomputation skipped a language",
        )


def test_the_comparability_audit_reviews_blinded_annotations() -> None:
    """The audit packet carries the run's own annotations, with the languages hidden.

    The first full benchmark run dispatched this audit with nothing but the
    frozen matrix template, whose every site is UNMEASURED, and the worker
    correctly refused to certify a sample that did not exist. Semantic
    Compression lost its ranking to that.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td).resolve()
        root = make_workspace(tmp)
        matrix = benchmark.json_load(
            root / "template" / "methodology-assets" / "semantic_compression"
            / "semantic_site_matrix.json"
        )
        probes = matrix["probes"]
        languages = ["Go", "Quidra", "Rust"]

        def shard(language: str) -> dict[str, Any]:
            slug = language.lower()
            return {
                "id": f"sc-metrics-local--part-1--{slug}",
                "evaluation": "semantic_compression",
                "assigned_languages": [language],
                "assigned_agent_id": f"worker-sc-metrics-local--part-1--{slug}",
                "execution_kind": "agent", "result_kind": "requirements",
                "phase": "measurement", "requirement_ids": ["metric.semantic_density"],
                "input_hashes": {}, "validator_command": "true",
                "worker_mode": "packet-only", "network_allowed": False,
                "dependencies": [],
            }

        shards = [shard(language) for language in languages]
        audit = {
            "id": "sc-comparability", "evaluation": "semantic_compression",
            "assigned_languages": [], "assigned_agent_id": "worker-sc-comparability",
            "execution_kind": "agent", "result_kind": "requirements",
            "phase": "measurement", "requirement_ids": ["gate.comparability_audit"],
            "input_hashes": {}, "validator_command": "true",
            "worker_mode": "packet-only", "network_allowed": False,
            "dependencies": [unit["id"] for unit in shards],
        }
        manifest = {"schema_version": 1, "work_units": shards + [audit]}
        for unit in shards:
            language = unit["assigned_languages"][0]
            agent_dir = root / "work" / "agents" / unit["assigned_agent_id"]
            agent_dir.mkdir(parents=True, exist_ok=True)
            benchmark.json_dump(agent_dir / "result.json", {
                "schema_version": 1,
                "evaluation": "semantic_compression",
                "requirements": {"metric.semantic_density": 40.0},
                "evidence": {"metric.semantic_density": {"per_probe": [
                    {
                        "probe_id": probe["probe_id"],
                        "fragment": f"let n = 7 // {probe['probe_id']}",
                        "explicit_local_facts": 4,
                        "notes": f"{language} resolves this from the local form alone.",
                    }
                    for probe in probes
                ]}},
            })

        # The current contract gives sampled support one authoritative
        # cohort-level record. Metric shards keep their own measurement fields
        # under metric_annotations, while the canonical fragment/support record
        # is projected into the blinded entry by the runner.
        for probe in benchmark.comparability_sample_probes(root):
            probe_id = str(probe["probe_id"])
            rid = (
                benchmark.SUPPORT_ADJUDICATION_PREFIX
                + probe_id.lower().replace(".", "-")
            )
            agent_dir = (
                root / "work" / "agents"
                / f"worker-test-adjudication-{probe_id.lower().replace('.', '-')}"
            )
            agent_dir.mkdir(parents=True, exist_ok=True)
            benchmark.json_dump(agent_dir / "result.json", {
                "schema_version": 1,
                "evaluation": "semantic_compression",
                "requirements": {rid: {
                    language: {
                        "level": "FULL",
                        "fragment": f"let n = 7 // {probe_id}",
                        "partial_reasons": [],
                        "none_reason": None,
                        "justification": "synthetic cohort-consistent support record",
                        "citation": "synthetic frozen evidence",
                    }
                    for language in languages
                }},
            })

        sample_path = benchmark.build_comparability_sample(root, audit, manifest)
        sample = json.loads(sample_path.read_text(encoding="utf-8"))
        sampled = {probe["probe_id"] for probe in sample["probes"]}
        check(
            len(sampled) >= -(-len(probes) // 5),
            f"the audit sample is under the 20% the methodology predeclares: {len(sampled)}",
        )
        check(
            {probe["family"] for probe in sample["probes"]}
            == {probe["family"] for probe in probes},
            "the audit sample does not cover every capability family",
        )
        entries = [entry for probe in sample["probes"] for entry in probe["annotations"]]
        check(
            len(entries) == len(sampled) * len(languages),
            f"every sampled probe must carry one entry per language: {len(entries)}",
        )
        check(
            {entry["label"] for entry in entries} == {"A", "B", "C"},
            "the entries are not labelled by an opaque per-run permutation",
        )
        check(
            all(
                any(
                    annotation.get("explicit_local_facts") == 4
                    for annotation in (entry.get("metric_annotations") or {}).values()
                    if isinstance(annotation, dict)
                )
                for entry in entries
            ),
            "the sample dropped the annotations it exists to show",
        )
        raw = sample_path.read_text(encoding="utf-8")
        check(
            not any(language in raw for language in languages),
            "a language name survived into the blinded sample",
        )
        check(
            "let n = 7" not in raw,
            (
                "canonical fragments must not be duplicated into the blinded "
                "comparability packet; support provenance remains in trusted evidence"
            ),
        )
        check(
            all(
                entry.get("support") == "FULL"
                and (entry.get("support_adjudication") or {}).get("level") == "FULL"
                for entry in entries
            ),
            "the blinded sample lost the authoritative reconciled support decision",
        )
        blinding = benchmark.json_load(root / "work" / "root" / "comparability_blinding.json")
        check(
            sorted(blinding["labels"]) == sorted(languages)
            and sample_path.parent != (root / "work" / "root"),
            "the unblinding map must stay on the trusted side, away from the sample",
        )


def test_worker_responses_may_carry_json_objects_and_broken_json_is_named() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        payload = {"schema_version": 1, "evaluation": "ecosystem", "requirements": {"metric.x": {}}}

        agent_id = "worker-json-object"
        create_task(root, agent_id, "packet-only")
        raw = json.dumps({
            "schema_version": 1, "task_id": agent_id,
            "files": [{"path": "result.json", "json": payload}],
        }).encode("utf-8")
        benchmark.apply_worker_response(root, agent_id, raw)
        materialized = json.loads(
            (root / "work" / "agents" / agent_id / "result.json").read_text(encoding="utf-8")
        )
        check(materialized == payload, f"the json-object file form was not materialized: {materialized}")

        agent_id = "worker-broken-json"
        create_task(root, agent_id, "packet-only")
        raw = json.dumps({
            "schema_version": 1, "task_id": agent_id,
            "files": [{"path": "result.json", "content": '{"schema_version": 1, "requirements": {'}],
        }).encode("utf-8")
        try:
            benchmark.apply_worker_response(root, agent_id, raw)
        except benchmark.BenchmarkError as exc:
            check(
                "result.json is not valid JSON" in str(exc) and '"json"' in str(exc),
                f"the import error does not name the file or the object form: {exc}",
            )
        else:
            check(False, "a result.json that is not JSON was imported")


def test_result_check_names_invalid_json_instead_of_crashing() -> None:
    import argparse
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        agent_id = "worker-result-check"
        agent_dir = create_task(root, agent_id, "packet-only")
        (agent_dir / "result.json").write_text("{ broken", encoding="utf-8")
        try:
            benchmark.cmd_result_check(argparse.Namespace(workspace=str(root), id=agent_id))
        except benchmark.BenchmarkError as exc:
            check("not valid JSON" in str(exc), f"unexpected result-check error: {exc}")
        except json.JSONDecodeError:
            check(False, "result-check crashed with a traceback on invalid JSON")
        else:
            check(False, "result-check accepted a result.json that is not JSON")


def test_trial_units_need_real_trials_and_a_working_toolchain() -> None:
    """A learnability unit whose compiler never ran is not a measurement."""
    unit = {
        "id": "learnability-i1-i2--rust", "evaluation": "llm_learnability",
        "assigned_languages": ["Rust"], "requirement_ids": ["condition.i1"],
        "execution_kind": "agent",
    }
    check(benchmark.is_trial_unit(unit), "a learnability condition unit is not a trial unit")

    def attestation(fields: tuple[str, ...], **overrides: Any) -> dict[str, Any]:
        payload: dict[str, Any] = {"schema_version": 1, "passed": True, "evidence": ["checked"]}
        payload.update({field: True for field in fields})
        payload.update(overrides)
        return payload

    def run_entry(turn: int, program: str, exit_code: int) -> dict[str, Any]:
        return {"turn": turn, "action": "run",
                "observation": {"argv": [f"/usr/bin/{program}", "x.rs"], "exit_code": exit_code, "ok": exit_code == 0}}

    with tempfile.TemporaryDirectory() as td:
        agent_dir = Path(td)
        # 1. The toolchain could not run at all: an infrastructure blocker, not a retry.
        benchmark.json_dump(agent_dir / "learnability_preflight.json", attestation(
            benchmark.LEARNABILITY_PREFLIGHT_FIELDS, passed=False, fixtures_compile_and_run=False,
            evidence=["rustc needs network to fetch a toolchain; sandbox has none"],
        ))
        benchmark.json_dump(agent_dir / "learnability_leakage.json", attestation(benchmark.LEARNABILITY_LEAKAGE_FIELDS))
        benchmark.json_dump(agent_dir / "agent_trace.json", {
            "trace": [run_entry(1, "rustc", 1), {"turn": 2, "action": "final"}],
        })
        infrastructure, problems = benchmark.trial_unit_problems(unit, agent_dir)
        check(infrastructure, f"an unusable toolchain was not classed as infrastructure: {problems}")
        check(
            any("cannot compile" in p for p in problems) and any("trial_start" in p for p in problems),
            f"the problems do not say what was missing: {problems}",
        )

        # 2. Attested true, trial ran, compiler succeeded: clean.
        benchmark.json_dump(agent_dir / "learnability_preflight.json", attestation(benchmark.LEARNABILITY_PREFLIGHT_FIELDS))
        benchmark.json_dump(agent_dir / "agent_trace.json", {
            "trace": [run_entry(1, "rustc", 0), {"turn": 2, "action": "trial_start"}, {"turn": 3, "action": "final"}],
        })
        infrastructure, problems = benchmark.trial_unit_problems(unit, agent_dir)
        check(not infrastructure and problems == [], f"a sound unit was rejected: {problems}")

        # 3. Attested true but no successful compiler run anywhere: unsupported, retried with feedback.
        benchmark.json_dump(agent_dir / "agent_trace.json", {
            "trace": [run_entry(1, "rustc", 1), run_entry(2, "python3", 0),
                      {"turn": 3, "action": "trial_start"}, {"turn": 4, "action": "final"}],
        })
        infrastructure, problems = benchmark.trial_unit_problems(unit, agent_dir)
        check(
            not infrastructure and any("no successful Rust toolchain invocation" in p for p in problems),
            f"an unsupported attestation passed: infrastructure={infrastructure} problems={problems}",
        )


def test_proficiency_runtime_verifier_executes_generated_python() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        trial_id = next(
            trial_id
            for trial_id in benchmark.proficiency_required_trial_ids(root)
            if trial_id.startswith("lightgrad--specification-to-implementation--")
        )
        trusted_dir = (
            root / "work/root/proficiency-verification/test-worker"
            / trial_id / "call_01"
        )
        previous = os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
        try:
            valid_source = (
                "import sys\n"
                "x1, x2, x3 = map(float, sys.stdin.read().split())\n"
                "y = x1*x1*x1*x2*x2 + x1*x3\n"
                "g1 = 3*x1*x1*x2*x2 + x3\n"
                "g2 = 2*x1*x1*x1*x2\n"
                "g3 = x1\n"
                "print('LIGHTGRAD', format(y, '.17g'), format(g1, '.17g'), "
                "format(g2, '.17g'), format(g3, '.17g'))\n"
            )
            verification = benchmark.verify_proficiency_completion(
                root, "Python", trial_id, valid_source, trusted_dir
            )
            check(
                verification["compile_parse_ok"] is True
                and verification["test_passed"] is True
                and verification["oracle_passed_count"]
                == verification["oracle_test_count"],
                f"trusted Python hidden-oracle verifier rejected a valid fixture: {verification}",
            )

            hardcoded = benchmark.verify_proficiency_completion(
                root,
                "Python",
                trial_id,
                "print('LIGHTGRAD 82 113 48 2')\n",
                root / "work/root/proficiency-verification/test-worker"
                / trial_id / "hardcoded",
            )
            check(
                hardcoded["compile_parse_ok"] is True
                and hardcoded["test_passed"] is False
                and 0 < hardcoded["oracle_passed_count"] < hardcoded["oracle_test_count"],
                f"a hard-coded public answer escaped hidden-input verification: {hardcoded}",
            )
            feedback = benchmark.proficiency_model_visible_feedback(hardcoded)
            summary = benchmark.proficiency_verification_summary(hardcoded)
            serialized_visible = json.dumps(
                {"feedback": feedback, "summary": summary},
                sort_keys=True,
            )
            check(
                "hidden-signs" not in serialized_visible
                and "hidden-negative" not in serialized_visible
                and "hidden-fractional" not in serialized_visible
                and "expected_sha256" not in serialized_visible
                and "input_sha256" not in serialized_visible
                and "hidden_failure_count" not in serialized_visible
                and "oracle_test_count" not in serialized_visible
                and "oracle_passed_count" not in serialized_visible
                and "test_passed" not in serialized_visible,
                f"hidden-oracle verdict leaked into worker/model-visible data: {serialized_visible}",
            )

            # The hard-coded public answer passes the public example but fails
            # hidden cases. Hidden failure must not become a one-bit repair oracle.
            check(
                summary.get("public_repair_gate_passed") is True,
                f"public repair gate did not recognize a public-pass fixture: {summary}",
            )
            try:
                benchmark.proficiency_repair_prompt(hardcoded)
            except benchmark.BenchmarkError as exc:
                check(
                    "hidden score-only cases never authorize a repair" in str(exc),
                    f"hidden-only failure was rejected for the wrong reason: {exc}",
                )
            else:
                check(False, "a hidden-only failure was allowed to trigger repair")

            # The complete trusted record may retain hidden diagnostics, but it
            # lives under work/root/proficiency-verification, outside worker read
            # paths. The worker-readable projection carries only compile/public
            # repair state, never the full hidden verdict or its cardinality.
            hidden_rows = [
                row for row in hardcoded["oracle_tests"] if row.get("hidden") is True
            ]
            check(hidden_rows, "trusted verification lost hidden oracle evidence")
            check(
                "oracle_tests" not in summary
                and "compile_or_parse" not in summary
                and set(summary) <= {
                    "schema_version", "trial_id", "language", "workload",
                    "source_sha256", "workload_contract_sha256",
                    "compile_parse_ok", "public_repair_gate_passed", "synthetic_ci",
                },
                f"worker-readable verification summary is too broad: {summary}",
            )

            # Integrity must use the same public stopping gate as the worker,
            # even when score-only hidden cases fail and the visible projection
            # deliberately omits test_passed.
            call = {
                "completion_sha256": hardcoded["source_sha256"],
                "verification": summary,
                "verification_path": str((trusted_dir.parent / "hardcoded" / "verification.json").relative_to(root)),
            }
            trace = {"trials": {"trials": {trial_id: {"calls": [call]}}}}
            agent_dir = root / "work/agents/test-worker"
            agent_dir.mkdir(parents=True, exist_ok=True)
            metrics = benchmark.proficiency_runtime_metrics(root, trace)
            benchmark.json_dump(agent_dir / "result.json", {
                "requirements": {key: {"Python": value} for key, value in (metrics or {}).items()}
            })
            problems = benchmark.proficiency_runtime_verification_problems(
                root, {"id": "trial", "assigned_agent_id": "test-worker", "assigned_languages": ["Python"]},
                agent_dir, trace,
            )
            check(not any("stopped after" in p for p in problems), f"public-pass trial rejected by integrity: {problems}")
            benchmark.json_dump(agent_dir / "trials" / trial_id / "session.json", {"calls": [call]})
            planner = load(SCRIPTS / "production_run.py", "public_gate_budget_test")
            needed = planner.nominal_sandbox_scored_calls(
                root, {"evaluation": "llm_proficiency", "assigned_agent_id": "test-worker"}, 72, 1
            )
            check(needed == 17, f"budget charged a completed public-gate trial again: {needed}")

            extra_stdout = benchmark.verify_proficiency_completion(
                root,
                "Python",
                trial_id,
                valid_source + "print('EXTRA')\n",
                root / "work/root/proficiency-verification/test-worker"
                / trial_id / "extra-stdout",
            )
            check(
                extra_stdout["compile_parse_ok"] is True
                and extra_stdout["test_passed"] is False,
                f"extra stdout escaped the exact one-line oracle: {extra_stdout}",
            )

            broken = benchmark.verify_proficiency_completion(
                root,
                "Python",
                trial_id,
                "def broken(:\n",
                root / "work/root/proficiency-verification/test-worker"
                / trial_id / "broken",
            )
            check(
                broken["compile_parse_ok"] is False,
                f"trusted Python verifier accepted invalid syntax: {broken}",
            )
        finally:
            if previous is not None:
                os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = previous


def test_proficiency_toolchain_evidence_precedes_scored_trials() -> None:
    unit = {
        "id": "proficiency-trials--zig",
        "evaluation": "llm_proficiency",
        "assigned_languages": ["Zig"],
        "requirement_ids": ["metric.correct_at_1"],
        "execution_kind": "agent",
    }

    def run_entry(program: str, exit_code: int) -> dict[str, Any]:
        return {
            "action": "run",
            "observation": {
                "argv": [f"/usr/bin/{program}", "main.zig"],
                "exit_code": exit_code,
                "ok": exit_code == 0,
            },
        }

    accepted_trial = {"action": "trial_start", "observation": {"ok": True}}
    clean = {"trace": [run_entry("zig", 0), accepted_trial]}
    check(
        benchmark.trial_toolchain_evidence_problems(unit, clean) == [],
        "a successful pre-trial Zig invocation was rejected",
    )

    late = {"trace": [accepted_trial, run_entry("zig", 0)]}
    problems = benchmark.trial_toolchain_evidence_problems(unit, late)
    check(
        any("before the first scored trial_start" in problem for problem in problems),
        f"a post-trial toolchain probe was incorrectly accepted: {problems}",
    )

    failed = {"trace": [run_entry("zig", 1), accepted_trial]}
    problems = benchmark.trial_toolchain_evidence_problems(unit, failed)
    check(
        any("no successful Zig toolchain invocation" in problem for problem in problems),
        f"a failed Zig invocation was incorrectly accepted: {problems}",
    )


def test_proficiency_repair_feedback_is_runtime_owned() -> None:
    failed = {
        "compile_parse_ok": True,
        "test_passed": False,
        "compile_or_parse": {
            "label": "build", "argv": ["python3", "-m", "py_compile", "main.py"],
            "exit_code": 0, "stdout": "", "stderr": "",
        },
        "oracle_test_count": 3,
        "oracle_passed_count": 1,
        "oracle_tests": [
            {
                "id": "public",
                "hidden": False,
                "passed": False,
                "problem": "public output mismatch",
                "run": {
                    "label": "run:public",
                    "argv": ["python3", "main.py"],
                    "exit_code": 0,
                    "stdout": "PUBLIC-WRONG\n",
                    "stderr": "PUBLIC-DIAGNOSTIC\n",
                },
            },
            {
                "id": "hidden-secret-id",
                "hidden": True,
                "passed": False,
                "problem": "HIDDEN-PROBLEM-SECRET",
                "run": {
                    "label": "run:hidden-secret-id",
                    "argv": ["python3", "main.py"],
                    "exit_code": 0,
                    "stdout": "HIDDEN-STDOUT-SECRET\n",
                    "stderr": "HIDDEN-STDERR-SECRET\n",
                },
            },
        ],
        "run": {
            "label": "run:hidden-secret-id",
            "argv": ["python3", "main.py"],
            "exit_code": 0,
            "stdout": "HIDDEN-REPRESENTATIVE-SECRET\n",
            "stderr": "",
        },
    }
    first = benchmark.proficiency_repair_prompt(failed)
    second = benchmark.proficiency_repair_prompt(json.loads(json.dumps(failed)))
    check(first == second, "Proficiency repair feedback is not deterministic")
    check(
        "PUBLIC-WRONG" in first
        and "PUBLIC-DIAGNOSTIC" in first
        and "hidden_failure_count" not in first
        and "oracle_passed_count" not in first
        and "oracle_test_count" not in first
        and "\"test_passed\"" not in first
        and "Return only one complete replacement source program" in first,
        f"trusted repair prompt leaked hidden aggregate feedback: {first}",
    )
    for secret in (
        "hidden-secret-id",
        "HIDDEN-PROBLEM-SECRET",
        "HIDDEN-STDOUT-SECRET",
        "HIDDEN-STDERR-SECRET",
        "HIDDEN-REPRESENTATIVE-SECRET",
    ):
        check(secret not in first, f"hidden oracle detail leaked into repair prompt: {secret}")

    public_passed = json.loads(json.dumps(failed))
    public_passed["oracle_tests"][0]["passed"] = True
    public_passed["test_passed"] = False  # hidden case still fails
    check(
        benchmark.proficiency_public_repair_gate_passed(public_passed) is True,
        "hidden failure incorrectly kept the public repair gate open",
    )
    try:
        benchmark.proficiency_repair_prompt(public_passed)
    except benchmark.BenchmarkError as exc:
        check(
            "hidden score-only cases never authorize a repair" in str(exc),
            f"a public-pass/hidden-fail trial was rejected for the wrong reason: {exc}",
        )
    else:
        check(False, "a hidden-only failure was allowed to request a repair")

    runtime = (SCRIPTS / "sandbox_agent.py").read_text(encoding="utf-8")
    check(
        "repair feedback is runtime-owned" in runtime
        and "benchmark.proficiency_repair_prompt" in runtime
        and "omit message instead of supplying custom guidance" in runtime,
        "sandbox runtime does not enforce verifier-only Proficiency repairs",
    )

def test_proficiency_requires_the_complete_primary_trial_set() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        required = benchmark.proficiency_required_trial_ids(root)
        unit = {"assigned_languages": ["Python"]}
        check(len(required) == 18, f"unexpected Primary trial count: {required}")
        check(len(set(required)) == 18, f"Primary trial IDs are not unique: {required}")

        def session(trial_id: str) -> dict[str, Any]:
            prompt = benchmark.proficiency_expected_prompt(root, "Python", trial_id)
            return {"calls": [{
                "prompt": prompt,
                "prompt_sha256": benchmark.sha256_bytes(prompt.encode("utf-8")),
                "completion": "c",
            }]}

        trace = {
            "trials": {
                "trials": {
                    trial_id: session(trial_id)
                    for trial_id in required
                }
            }
        }
        check(
            benchmark.proficiency_trial_coverage_problems(root, unit, trace) == [],
            "the exact frozen Primary trial/prompt set was rejected",
        )
        missing = json.loads(json.dumps(trace))
        missing["trials"]["trials"].pop(required[-1])
        problems = benchmark.proficiency_trial_coverage_problems(root, unit, missing)
        check(
            any("missing required Primary trials" in p for p in problems)
            and any("expected exactly 18" in p for p in problems),
            f"an incomplete Proficiency run was not rejected: {problems}",
        )
        extra = json.loads(json.dumps(trace))
        extra["trials"]["trials"]["invented-cell-t1"] = {
            "calls": [{"prompt": "p", "prompt_sha256": "0" * 64, "completion": "c"}]
        }
        problems = benchmark.proficiency_trial_coverage_problems(root, unit, extra)
        check(
            any("unexpected Primary trial IDs" in p for p in problems),
            f"an invented Proficiency trial ID was not rejected: {problems}",
        )
        tampered = json.loads(json.dumps(trace))
        first = required[0]
        tampered["trials"]["trials"][first]["calls"][0]["prompt"] += "\nchanged"
        problems = benchmark.proficiency_trial_coverage_problems(root, unit, tampered)
        check(
            any("runtime-owned" in p or "prompt hash" in p for p in problems),
            f"a replaced Proficiency task prompt was accepted: {problems}",
        )


def test_proficiency_prompt_variants_and_runtime_metrics() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        manifest = benchmark.proficiency_trial_manifest(root)
        cfg = json.loads(
            (root / "template/config/primary.json").read_text(encoding="utf-8")
        )["llm_proficiency"]
        variants = list(cfg["primary_prompt_variants"])
        first_cell = [
            trial_id
            for trial_id, cell in manifest.items()
            if cell["workload"] == "SVM"
            and cell["scenario"] == "specification_to_implementation"
        ]
        check(len(first_cell) == 3, f"unexpected prompt-variant cell: {first_cell}")
        check(
            [manifest[trial_id]["prompt_variant"] for trial_id in first_cell] == variants,
            "Primary replications were not mapped to frozen prompt variants in order",
        )
        prompts = [
            benchmark.proficiency_expected_prompt(root, "Python", trial_id)
            for trial_id in first_cell
        ]
        check(
            len(set(prompts)) == 3,
            "the three frozen Proficiency prompt variants are byte-identical",
        )
        for prompt in prompts:
            check(
                "Frozen specification:" in prompt
                and "trusted runtime supplies additional hidden stdin cases" in prompt
                and "Target language: Python" in prompt,
                "a prompt variant dropped frozen semantic content",
            )

        trials = {}
        trusted_base = root / "work/root/proficiency-verification/test-agent"
        for trial_id, cell in manifest.items():
            variant = cell["prompt_variant"]
            passes = variant != "contract-first"
            oracle_cases = benchmark.proficiency_trusted_oracle_cases(
                root, cell["workload"]
            )
            rows = [
                {
                    "id": case["id"],
                    "hidden": bool(case["hidden"]),
                    "passed": passes,
                    "run": {
                        "exit_code": 0,
                        "stdout": "ok" if passes else "wrong",
                        "stderr": "",
                    },
                }
                for case in oracle_cases
            ]
            completion = "source"
            completion_sha = benchmark.sha256_bytes(completion.encode("utf-8"))
            trusted = {
                "schema_version": 2,
                "trial_id": trial_id,
                "language": "Python",
                "workload": cell["workload"],
                "source_sha256": completion_sha,
                "workload_contract_sha256": (
                    benchmark.proficiency_workload_contract_sha256(root)
                ),
                "compile_parse_ok": True,
                "test_passed": passes,
                "oracle_test_count": len(rows),
                "oracle_passed_count": len(rows) if passes else 0,
                "oracle_tests": rows,
            }
            verify_path = (
                trusted_base / trial_id / "call_01" / "verification.json"
            )
            benchmark.json_dump(verify_path, trusted)
            trials[trial_id] = {
                "calls": [{
                    "completion": completion,
                    "completion_sha256": completion_sha,
                    "incomplete": None,
                    "verification": benchmark.proficiency_verification_summary(trusted),
                    "verification_path": verify_path.relative_to(root).as_posix(),
                }]
            }
        metrics = benchmark.proficiency_runtime_metrics(
            root, {"trials": {"trials": trials}}
        )
        check(metrics is not None, "runtime-owned Proficiency metrics were not computed")
        if metrics is not None:
            expected = 200.0 / 3.0
            check(
                abs(metrics["metric.prompt_robustness"] - 0.0) < 1e-9,
                f"worst-variant Prompt Robustness is wrong: {metrics}",
            )
            check(
                abs(metrics["metric.unseen_case_generalization"] - expected) < 1e-6,
                f"hidden-only Unseen-case Generalization is wrong: {metrics}",
            )
            check(
                abs(metrics["metric.correct_at_1"] - expected) < 1e-6,
                f"Correct@1 did not use the variant trial set: {metrics}",
            )
            check(
                abs(metrics["metric.repair_success_rate"] - 0.0) < 1e-9,
                f"Repair Success was not derived from the failed first attempts: {metrics}",
            )
            check(
                abs(metrics["metric.repair_efficiency"] - expected) < 1e-6,
                f"Repair Efficiency did not preserve first-pass trials: {metrics}",
            )
            check(
                abs(metrics["metric.diagnosis_efficiency"] - 0.0) < 1e-9,
                f"Diagnosis Efficiency should be zero without a successful first repair: {metrics}",
            )
            check(
                abs(metrics["metric.silent_bug_resistance"] - expected) < 1e-6,
                f"Silent Bug Resistance did not detect exit-0 wrong outputs: {metrics}",
            )


def test_rejected_attempts_feed_back_into_the_next_one() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td).resolve())
        agent_id = "worker-feedback"
        create_task(root, agent_id, "packet-only")
        unit = {"id": "feedback-unit", "assigned_agent_id": agent_id}
        benchmark.json_dump(root / "work" / "root" / "manifest.json", {"schema_version": 1, "work_units": [unit]})
        check(
            benchmark.previous_attempt_feedback(root, agent_id) is None,
            "feedback was produced before any attempt failed",
        )
        benchmark.archive_attempt(
            root, unit, 1, "validation-failed-retry", reset=True,
            detail="benchmark error: result requirement mismatch; missing=['metric.x'], unknown=[]",
        )
        feedback = benchmark.previous_attempt_feedback(root, agent_id)
        check(
            feedback is not None and "requirement mismatch" in feedback,
            f"the rejected attempt's detail did not reach the next attempt: {feedback!r}",
        )
        archived = json.loads(
            (root / "work" / "attempts" / "feedback-unit" / "attempt-01" / "attempt.json").read_text("utf-8")
        )
        check(archived.get("detail", "").startswith("benchmark error"), f"attempt.json lost the detail: {archived}")
        check(
            (root / "work" / "agents" / agent_id / "task.json").is_file()
            and not (root / "work" / "agents" / agent_id / "result.json").exists(),
            "archive_attempt did not reset the agent directory to its packet",
        )


def test_dispatch_batches_run_units_concurrently_and_drain_before_a_fatal_error() -> None:
    production_run = load(SCRIPTS / "production_run.py", "isolation_tests_production_run_batch")
    import threading
    state = {"active": 0, "peak": 0, "done": []}
    gate = threading.Lock()

    def slow_dispatch(root: Path, task: dict[str, Any], units: dict[str, Any]) -> None:
        with gate:
            state["active"] += 1
            state["peak"] = max(state["peak"], state["active"])
        time.sleep(0.25)
        with gate:
            state["active"] -= 1
            state["done"].append(task["work_unit_id"])
        if task["work_unit_id"] == "fatal":
            raise production_run.ProductionRunError("the provider cannot serve this run any further")

    queue = [{"work_unit_id": f"u{n}"} for n in range(4)]
    started_at = time.monotonic()
    started = production_run.dispatch_batch(Path("."), queue, {}, 4, dispatch=slow_dispatch)
    elapsed = time.monotonic() - started_at
    check(started == 4 and len(state["done"]) == 4, f"not every unit was dispatched: {state}")
    check(state["peak"] >= 2, f"units did not overlap: peak concurrency {state['peak']}")
    check(elapsed < 0.9, f"four 0.25s units took {elapsed:.2f}s; the batch was not concurrent")

    # A wall-clock stop lets in-flight units finish and starts nothing new.
    state.update(active=0, peak=0, done=[])
    calls = {"n": 0}

    def may_start() -> bool:
        calls["n"] += 1
        return calls["n"] <= 2

    started = production_run.dispatch_batch(Path("."), queue, {}, 4, may_start=may_start, dispatch=slow_dispatch)
    check(started == 2 and len(state["done"]) == 2, f"the stop signal was not honoured: {started}, {state}")

    # A fatal provider error surfaces only after the batch has drained.
    state.update(active=0, peak=0, done=[])
    fatal_queue = [{"work_unit_id": "fatal"}, {"work_unit_id": "a"}, {"work_unit_id": "b"}]
    try:
        production_run.dispatch_batch(Path("."), fatal_queue, {}, 3, dispatch=slow_dispatch)
    except production_run.ProductionRunError as exc:
        check("cannot serve" in str(exc), f"the wrong error surfaced: {exc}")
    else:
        check(False, "a fatal provider error was swallowed by the batch")
    check(sorted(state["done"]) == ["a", "b", "fatal"], f"in-flight units were abandoned: {state}")


def test_tool_less_requests_stream_and_tool_requests_stay_buffered() -> None:
    """A long generation must keep bytes moving on the wire.

    The third paid run lost every packet call that generated for longer than
    about four minutes: a buffered request is silent while the model works and
    the runner's outbound connection was dropped as idle, then the identical
    generation was paid for again on each transport retry. Tool-less requests
    therefore stream. Requests carrying the frozen web-search tool stay
    buffered so their continuations can send content blocks back verbatim.
    """
    provider = _anthropic_provider()
    config = json.loads((TEMPLATE / "config" / "inference_gateway.json").read_text("utf-8"))
    check(
        config["anthropic_transport"]["streaming"] is True
        and provider.describe().get("streaming") == "tool-less requests",
        f"the frozen transport disagrees with what the adapter does: {provider.describe()}",
    )

    http = _ScriptedHTTP([
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn", "usage": {}},
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn", "usage": {}},
    ])
    provider._open = http
    provider.complete(_validated_request())
    provider.complete(_validated_request(network_allowed=True))
    check(http.streamed == [True, False], f"the wrong requests streamed: {http.streamed}")
    check(
        http.payloads[0].get("stream") is True and "tools" not in http.payloads[0],
        f"a tool-less request was not sent as a stream: {sorted(http.payloads[0])}",
    )
    check(
        "stream" not in http.payloads[1] and "tools" in http.payloads[1],
        f"a web-search request was not kept buffered: {sorted(http.payloads[1])}",
    )
    check(
        http.headers[0].get("accept") == "text/event-stream"
        and http.headers[1].get("accept") == "application/json",
        f"the accept headers do not match the transports: {http.headers}",
    )

    os.environ["ANTHROPIC_API_KEY"] = FAKE_PROVIDER_SECRET
    try:
        buffered = inference_gateway.AnthropicMessagesProvider(
            "claude-sonnet-5", timeout=30, transport={"streaming": False},
        )
    finally:
        os.environ.pop("ANTHROPIC_API_KEY", None)
    http = _ScriptedHTTP([
        {"content": [{"type": "text", "text": "ready"}], "stop_reason": "end_turn", "usage": {}},
    ])
    buffered._open = http
    buffered.complete(_validated_request())
    check(
        http.streamed == [False] and "stream" not in http.payloads[0]
        and buffered.describe().get("streaming") == "off",
        "a transport frozen to buffered responses still streamed",
    )


def _sse(events: list[tuple[str, dict[str, Any]]]) -> bytes:
    return b"".join(
        f"event: {name}\ndata: {json.dumps(payload)}\n\n".encode("utf-8")
        for name, payload in events
    )


_STREAMED_ANSWER: list[tuple[str, dict[str, Any]]] = [
    ("message_start", {"type": "message_start", "message": {
        "id": "msg_1", "role": "assistant", "content": [], "stop_reason": None,
        "usage": {"input_tokens": 40, "cache_creation_input_tokens": 0,
                  "cache_read_input_tokens": 30, "output_tokens": 1},
    }}),
    ("content_block_start", {"type": "content_block_start", "index": 0,
                             "content_block": {"type": "thinking", "thinking": ""}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 0,
                             "delta": {"type": "thinking_delta", "thinking": "hmm"}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 0,
                             "delta": {"type": "signature_delta", "signature": "sig"}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 0}),
    ("ping", {"type": "ping"}),
    ("content_block_start", {"type": "content_block_start", "index": 1,
                             "content_block": {"type": "text", "text": ""}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1,
                             "delta": {"type": "text_delta", "text": '{"ok":'}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1,
                             "delta": {"type": "text_delta", "text": "true}"}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 1}),
    ("message_delta", {"type": "message_delta",
                       "delta": {"stop_reason": "max_tokens", "stop_sequence": None},
                       "usage": {"output_tokens": 12}}),
    ("message_stop", {"type": "message_stop"}),
]


def test_event_stream_folds_back_into_the_buffered_response() -> None:
    """Nothing after the transport may know which transport carried the answer."""
    import http.server
    import threading

    folded = inference_gateway.AnthropicMessagesProvider._read_event_stream(
        _sse(_STREAMED_ANSWER).splitlines(keepends=True)
    )
    texts = [b["text"] for b in folded["content"] if b.get("type") == "text"]
    check(texts == ['{"ok":true}'], f"text deltas were not joined in order: {folded}")
    check(folded["stop_reason"] == "max_tokens", f"stop_reason was not taken from message_delta: {folded}")
    usage = folded["usage"]
    check(
        usage.get("input_tokens") == 40 and usage.get("cache_read_input_tokens") == 30
        and usage.get("output_tokens") == 12,
        f"usage was not merged from message_start and message_delta: {usage}",
    )
    thinking = [b for b in folded["content"] if b.get("type") == "thinking"]
    check(
        thinking and thinking[0].get("thinking") == "hmm" and thinking[0].get("signature") == "sig",
        f"the thinking block was not reassembled: {folded['content']}",
    )

    for label, lines in (
        ("a stream that ends before message_stop", _sse(_STREAMED_ANSWER[:-1]).splitlines(keepends=True)),
        ("an error event", _sse([("error", {"type": "error", "error": {"type": "overloaded_error"}})]).splitlines(keepends=True)),
    ):
        try:
            inference_gateway.AnthropicMessagesProvider._read_event_stream(lines)
        except inference_gateway.EventStreamError:
            continue
        check(False, f"{label} was accepted as a complete answer")

    # End to end through urllib against a local server: the first stream is cut
    # off mid-answer (what an idle-dropped connection looks like from here), the
    # transport retry gets the whole answer, and a buffered tool request still
    # receives plain JSON.
    served: list[dict[str, Any]] = []

    class Handler(http.server.BaseHTTPRequestHandler):
        cut_once = [True]

        def log_message(self, *args: Any) -> None:  # noqa: D401 - silence the server
            return

        def do_POST(self) -> None:
            body = json.loads(self.rfile.read(int(self.headers["content-length"])))
            served.append(body)
            if body.get("stream"):
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.end_headers()
                if self.cut_once[0]:
                    self.cut_once[0] = False
                    self.wfile.write(_sse(_STREAMED_ANSWER[:6]))
                    self.wfile.flush()
                    self.close_connection = True
                    return
                self.wfile.write(_sse(_STREAMED_ANSWER))
                return
            payload = json.dumps({
                "content": [{"type": "text", "text": "buffered"}],
                "stop_reason": "end_turn", "usage": {"output_tokens": 2},
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        provider = _anthropic_provider()
        provider.base_url = f"http://127.0.0.1:{server.server_address[1]}"
        result = provider.complete(_validated_request())
        check(
            result["content"] == '{"ok":true}' and result["stop_reason"] == "max_tokens",
            f"the streamed answer did not come through urllib intact: {result}",
        )
        check(
            len(served) == 2 and all(b.get("stream") for b in served),
            f"the cut-off stream was not retried as a stream: {[b.get('stream') for b in served]}",
        )
        check(
            result["usage"]["output_tokens"] == 12 and result["usage"]["input_tokens"] == 40,
            f"streamed usage was not carried into the result: {result['usage']}",
        )
        result = provider.complete(_validated_request(network_allowed=True))
        check(
            result["content"] == "buffered" and served[-1].get("stream") is None,
            f"a tool request did not use the buffered transport: {result}",
        )
    finally:
        server.shutdown()
        server.server_close()


def test_agent_action_turns_parse_batches_and_control_characters() -> None:
    """The action protocol accepts what real models send, and no more."""
    raw_newline = '{"action":"write_file","path":"a.txt","content":"line one\nline two"}'
    parsed = gateway_client.parse_model_json(raw_newline)
    check(
        parsed["content"] == "line one\nline two",
        "a raw newline inside a JSON string was not tolerated",
    )

    batch = gateway_client.parse_model_json_batch(
        'First:\n{"action":"write_file","path":"a","content":"1"}\n'
        'then\n{"action":"run","argv":["python3","--version"]}\n{"action":"final"}'
    )
    check(
        [item["action"] for item in batch] == ["write_file", "run", "final"],
        f"a batch of actions was not parsed in order: {batch}",
    )
    check(
        gateway_client.parse_model_json_batch('```json\n{"action":"final"}\n```') == [{"action": "final"}],
        "a single fenced action was not accepted by the batch parser",
    )
    check(
        gateway_client.parse_model_json_batch(
            '```json\n{"action":"run","argv":["true"]}\n{"action":"final"}\n```'
        ) == [{"action": "run", "argv": ["true"]}, {"action": "final"}],
        "JSONL in one code fence was rejected",
    )
    for label, completion in (
        ("no object", "I will start now."),
        ("one broken object among two", '{"action":"final"}\n{"action": broken}'),
        ("an array", "[1,2]"),
    ):
        try:
            gateway_client.parse_model_json_batch(completion)
        except gateway_client.GatewayClientError:
            continue
        check(False, f"the batch parser accepted {label}")
    # The packet-only contract is unchanged: one object, or it is ambiguous.
    try:
        gateway_client.parse_model_json('{"a":1}\n{"b":2}')
    except gateway_client.GatewayClientError:
        pass
    else:
        check(False, "the packet-only parser accepted two objects")


def test_sandbox_agent_batches_actions_and_recovers_from_truncation_and_early_final() -> None:
    """The three ways the third paid run lost learnability units, each survived.

    Several actions in one turn are executed in order instead of being refused;
    a turn cut off at the cap is answered with a protocol error instead of
    ending the unit; a `final` sent before the expected outputs exist is
    refused so the model can still write them.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        root = make_workspace(tmp)
        agent_id = "worker-batching-agent"
        agent_dir = create_task(root, agent_id, "sandbox-agent")
        payload = {
            "schema_version": 1,
            "evaluation": "semantic_compression",
            "requirements": {"gate.example": True},
            "evidence": {"source": "batch protocol test"},
        }
        script = {
            "schema_version": 1,
            "sequence": [
                # 1. two actions in one turn, executed in order
                json.dumps({"action": "write_file", "path": "a.txt", "content": "A"})
                + "\n"
                + json.dumps({"action": "write_file", "path": "b.txt", "content": "B"}),
                # 2. final before result.json exists
                json.dumps({"action": "final", "summary": "too early"}),
                # 3. a turn cut off at the cap: nothing in it may run
                {"content": '{"action":"write_file","path":"result.json","content":"{\\"schema',
                 "stop_reason": "max_tokens"},
                # 4. the output and final in one turn
                json.dumps({"action": "write_file", "path": "result.json",
                            "content": json.dumps(payload) + "\n"})
                + "\n"
                + json.dumps({"action": "final", "summary": "done"}),
            ],
        }
        with Gateway(root / "gateway", script=script) as gw:
            completed = subprocess.run(
                [
                    sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                    "--workspace", str(root), "--id", agent_id,
                    "--socket", str(gw.socket_path),
                ],
                env=sandbox_side_env(),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
        check(
            completed.returncode == 0,
            f"the agent did not finish: {completed.stderr or completed.stdout}",
        )
        trace_path = agent_dir / "agent_trace.json"
        if not trace_path.is_file():
            check(False, "the agent wrote no trace")
            return
        trace = json.loads(trace_path.read_text(encoding="utf-8"))
        check(trace["stop_reason"] == "final" and trace["missing_outputs"] == [],
              f"the unit did not end deliberately with its outputs: {trace['stop_reason']}")
        check(
            (agent_dir / "a.txt").read_text() == "A" and (agent_dir / "b.txt").read_text() == "B",
            "a batched turn did not execute both actions",
        )
        check(
            json.loads((agent_dir / "result.json").read_text())["evidence"]["source"]
            == "batch protocol test",
            "the truncated write must not have landed, and the whole one must have",
        )
        by_turn: dict[int, list[dict[str, Any]]] = {}
        for entry in trace["trace"]:
            by_turn.setdefault(int(entry["turn"]), []).append(entry)
        first = by_turn.get(1, [])
        check(
            [e.get("batch") for e in first] == [[1, 2], [2, 2]]
            and all(e["observation"]["ok"] for e in first),
            f"turn 1 was not recorded as an executed batch: {first}",
        )
        second = by_turn.get(2, [])
        check(
            len(second) == 1 and second[0].get("action") == "final"
            and (
                "expected outputs" in str(second[0].get("protocol_error"))
                or "expected outputs" in str(
                    (second[0].get("observation") or {}).get("protocol_error")
                )
            ),
            f"an early final was not refused: {second}",
        )
        third = by_turn.get(3, [])
        check(
            len(third) == 1 and third[0].get("protocol_error") == "truncated action turn"
            and third[0].get("action") is None,
            f"a truncated turn was not reported as recoverable: {third}",
        )
        fourth = by_turn.get(4, [])
        check(
            [e.get("action") for e in fourth] == ["write_file", "final"],
            f"the closing batch was not executed in order: {fourth}",
        )
        check(
            benchmark.sandbox_agent_trace_problems(agent_dir, agent_id) == [],
            f"task-finish would reject a trace with batched turns: "
            f"{benchmark.sandbox_agent_trace_problems(agent_dir, agent_id)}",
        )


def test_privacy_gate_reads_sandbox_authored_temp_paths_as_code_not_leaks() -> None:
    """A `/tmp/...` literal a worker wrote into its own script is not a host path.

    The scored sandbox mounts no host directory, so nothing it writes can carry
    one. The third paid run's finalize failed - and its completed Ecosystem
    ranking went unpublished - on `/tmp/_perfbin` inside a perf script one
    agent wrote. Home-directory paths and e-mail addresses stay findings
    everywhere, and temp paths stay findings outside sandbox-authored files.
    """
    import argparse
    import contextlib
    import io

    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        agent_dir = create_task(root, "worker-perf-agent", "sandbox-agent")
        (agent_dir / "perf_test.py").write_text(
            "subprocess.run(['c++', '-O2', src, '-o', '/tmp/_perfbin'])\n", encoding="utf-8"
        )
        (root / "raw").mkdir(exist_ok=True)
        (root / "raw" / "worker-x.response.json").write_text(
            '{"note": "wrote /workspace/out.txt"}\n', encoding="utf-8"
        )
        # The synthetic workspace itself lives under the host temp directory,
        # which the gate allows only in synthetic mode, exactly as CI runs it.
        os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = benchmark.cmd_privacy_check(argparse.Namespace(workspace=str(root)))
        finally:
            os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
        findings = json.loads((root / "results" / "privacy_check.json").read_text(encoding="utf-8"))
        check(rc == 0, f"a temp-path literal in sandbox-authored text was reported: {findings}")

        (agent_dir / "notes.md").write_text("see /Users/someone/secret.txt\n", encoding="utf-8")
        (root / "results" / "stray.json").write_text('{"p": "/tmp/tmpabc123/x"}\n', encoding="utf-8")
        os.environ["QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS"] = "1"
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = benchmark.cmd_privacy_check(argparse.Namespace(workspace=str(root)))
        finally:
            os.environ.pop("QUIDRA_BENCHMARK_SYNTHETIC_COMMANDS", None)
        findings = json.loads((root / "results" / "privacy_check.json").read_text(encoding="utf-8"))
        kinds = sorted((f["file"], f["kind"]) for f in findings["findings"])
        check(
            rc != 0 and kinds == [
                ("results/stray.json", "host_temp_path"),
                ("work/agents/worker-perf-agent/notes.md", "unix_home"),
            ],
            f"the privacy gate lost a real finding or kept a false one: {kinds}",
        )


def test_a_rehearsal_dispatches_only_its_named_units_and_stops_when_they_are_terminal() -> None:
    """A unit-scoped run is the same run on one packet or one agent unit.

    It is how a change to the paid path is tried for under a dollar. The
    runner must dispatch nothing outside the named units, must stop as soon as
    they are terminal even though the rest of the manifest is pending, and the
    frozen task policy must name only their agents so the gateway refuses
    every other request before it can spend anything.
    """
    production_run = load(SCRIPTS / "production_run.py", "isolation_tests_production_run_rehearsal")
    units = {
        "sc-a": {"evaluation": "semantic_compression", "execution_kind": "agent"},
        "sc-b": {"evaluation": "semantic_compression", "execution_kind": "agent"},
        "eco-a": {"evaluation": "ecosystem", "execution_kind": "agent"},
        "gate": {"evaluation": "ecosystem", "execution_kind": "command"},
    }
    queue = [{"work_unit_id": uid} for uid in ("sc-a", "sc-b", "eco-a")]
    chosen = production_run.select_queue(queue, units, None, {"sc-b"})
    check([t["work_unit_id"] for t in chosen] == ["sc-b"], f"a rehearsal dispatched outside its units: {chosen}")
    chosen = production_run.select_queue(queue, units, "semantic_compression", None)
    check([t["work_unit_id"] for t in chosen] == ["sc-a", "sc-b"], f"the evaluation scope changed: {chosen}")
    chosen = production_run.select_queue(queue, units, "ecosystem", {"sc-b"})
    check(chosen == [], "a unit outside the evaluation scope was dispatched")

    ledger = {"units": {"sc-b": {"status": "COMPLETE"}, "sc-a": {"status": "PENDING"},
                        "eco-a": {"status": "RUNNING"}, "gate": {"status": "PENDING"}}}
    check(
        production_run.scope_states(ledger, units, None, {"sc-b"}) == {"COMPLETE"},
        "a rehearsal's scope included units it did not name",
    )
    check(
        production_run.scope_states(ledger, units, None, None) == {"COMPLETE", "PENDING", "RUNNING"},
        "the full scope lost a state",
    )
    check(
        production_run.scope_states(ledger, units, "ecosystem", None) == {"RUNNING", "PENDING"},
        "the evaluation scope lost a state",
    )

    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        work_units = [
            {"id": uid, "evaluation": "ecosystem", "execution_kind": "agent",
             "assigned_agent_id": f"worker-{uid}", "network_allowed": True,
             "max_llm_calls": 1, "estimated_input_tokens_per_call": 1000,
             "max_output_tokens_per_call": 1000}
            for uid in ("eco-a", "eco-b")
        ]
        benchmark.json_dump(root / "work" / "root" / "manifest.json",
                            {"schema_version": 1, "work_units": work_units})
        benchmark.json_dump(root / "work" / "root" / "ledger.json", {"schema_version": 1, "units": {}})
        policy = production_run.write_policy(
            root, root / "results" / "task-policy.json", model="claude-sonnet-5", units=["eco-b"],
        )
        check(
            list(policy["tasks"]) == ["worker-eco-b"] and policy.get("units") == ["eco-b"],
            f"the rehearsal policy named more than its units: {policy['tasks']}",
        )
        policy = production_run.write_policy(
            root, root / "results" / "task-policy.json", model="claude-sonnet-5",
        )
        check(
            sorted(policy["tasks"]) == ["worker-eco-a", "worker-eco-b"] and "units" not in policy,
            f"the full policy changed: {policy['tasks']}",
        )
        # An evaluation's frozen depth override is pinned per task in the policy.
        work_units.append({
            "id": "sc-a", "evaluation": "semantic_compression", "execution_kind": "agent",
            "assigned_agent_id": "worker-sc-a", "network_allowed": False,
            "max_llm_calls": 1, "estimated_input_tokens_per_call": 1000,
            "max_output_tokens_per_call": 1000,
        })
        benchmark.json_dump(root / "work" / "root" / "manifest.json",
                            {"schema_version": 1, "work_units": work_units})
        policy = production_run.write_policy(
            root, root / "results" / "task-policy.json", model="claude-sonnet-5",
        )
        check(
            policy["efforts"] == {"worker-sc-a": "medium"},
            f"the Semantic Compression depth was not frozen per task: {policy.get('efforts')}",
        )


def test_a_worker_may_run_what_it_built_and_is_told_every_earlier_rejection() -> None:
    """Two things the second rehearsal showed a compiled-language unit needs.

    An executable the worker compiled inside its own directory must be
    runnable by relative path (a relative path used to resolve against the
    workspace root and be refused, forcing a python wrapper that hid the real
    toolchain invocation from the trace); anything outside that directory or
    reached through a symlink stays refused. And a retried worker must be told
    every earlier rejection, not only the last one, or it fixes them in turns.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        root = make_workspace(tmp)
        agent_id = "worker-builder-agent"
        agent_dir = create_task(root, agent_id, "sandbox-agent")
        (agent_dir / "fixtures").mkdir()
        tool = agent_dir / "fixtures" / "check"
        tool.write_text("#!/usr/bin/env python3\nprint('built tool ran')\n", encoding="utf-8")
        tool.chmod(0o755)
        (agent_dir / "escape-bin").symlink_to(root / "work" / "root")
        payload = {"schema_version": 1, "evaluation": "semantic_compression",
                   "requirements": {"gate.example": True}, "evidence": {"source": "builder test"}}
        script = {
            "schema_version": 1,
            "sequence": [
                json.dumps({"action": "run", "argv": ["fixtures/check"]}),
                json.dumps({"action": "run", "argv": ["./fixtures/check", "arg"]}),
                json.dumps({"action": "run", "argv": ["escape-bin/x"]}),
                json.dumps({"action": "run", "argv": ["../../root/manifest.json"]}),
                json.dumps({"action": "write_file", "path": "result.json",
                            "content": json.dumps(payload) + "\n"}),
                json.dumps({"action": "final", "summary": "done"}),
            ],
        }
        with Gateway(root / "gateway", script=script) as gw:
            completed = subprocess.run(
                [sys.executable, str(SCRIPTS / "sandbox_agent.py"),
                 "--workspace", str(root), "--id", agent_id, "--socket", str(gw.socket_path)],
                env=sandbox_side_env(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
        check(completed.returncode == 0, f"the agent did not finish: {completed.stderr or completed.stdout}")
        trace = json.loads((agent_dir / "agent_trace.json").read_text(encoding="utf-8"))
        by_turn = {int(e["turn"]): e for e in trace["trace"]}
        first = by_turn[1]["observation"]
        check(
            first.get("ok") is True and "built tool ran" in str(first.get("stdout", "")),
            f"a worker-built executable could not be run by relative path: {first}",
        )
        check(by_turn[2]["observation"].get("ok") is True, f"a ./-relative path was refused: {by_turn[2]}")
        check(
            "symlink" in str(by_turn[3]["observation"].get("denied", ""))
            or "not in the frozen allowlist" in str(by_turn[3]["observation"].get("denied", "")),
            f"a symlink out of the worker directory was not refused: {by_turn[3]}",
        )
        check(
            "not in the frozen allowlist" in str(by_turn[4]["observation"].get("denied", "")),
            f"a path outside the worker directory was not refused: {by_turn[4]}",
        )

        # Feedback carries every archived rejection, in order.
        unit_id = None
        for unit in json.loads((root / "work" / "root" / "manifest.json").read_text(encoding="utf-8")).get("work_units", []) if (root / "work" / "root" / "manifest.json").is_file() else []:
            if unit.get("assigned_agent_id") == agent_id:
                unit_id = unit["id"]
        if unit_id is None:
            unit_id = "builder-unit"
            benchmark.json_dump(root / "work" / "root" / "manifest.json", {
                "schema_version": 1,
                "work_units": [{"id": unit_id, "assigned_agent_id": agent_id}],
            })
        for n, detail in ((1, "no successful C++ toolchain invocation"), (2, "expected languages ['C++']")):
            benchmark.json_dump(root / "work" / "attempts" / unit_id / f"attempt-0{n}" / "attempt.json",
                                {"schema_version": 1, "attempt": n, "detail": detail, "reason": "validation-failed-retry"})
        feedback = benchmark.previous_attempt_feedback(root, agent_id) or ""
        check(
            "attempt 1: no successful C++ toolchain invocation" in feedback
            and "attempt 2: expected languages ['C++']" in feedback,
            f"the retry feedback lost an earlier rejection: {feedback!r}",
        )


def test_shared_inputs_first_packets_cache_their_shared_prefix() -> None:
    """Prompt caching must not mutate the certified result-cache identity.

    The historical shared-inputs-first byte order is frozen embedded inputs,
    unit-specific task inputs, the per-unit Task Packet header, then the
    assigned-requirements tail. The trusted Anthropic adapter may split that
    unchanged string into content blocks for pricing, but must put the cache
    breakpoint before unit-specific input rather than moving bytes around.
    """
    import argparse
    import contextlib
    import io

    with tempfile.TemporaryDirectory() as td:
        root = make_workspace(Path(td))
        docs = root / "repo" / "docs"
        docs.mkdir(parents=True, exist_ok=True)
        (docs / "allowed.md").write_text(
            "unit-specific input A\n" * 50, encoding="utf-8"
        )

        agent_id = "worker-layout-shared"
        with contextlib.redirect_stdout(io.StringIO()):
            benchmark.cmd_task_create(argparse.Namespace(
                workspace=str(root), id=agent_id, parent=None,
                evaluation="semantic_compression",
                goal="layout test", read=[str(docs / "allowed.md")], write=None,
                output=[str(root / "work" / "agents" / agent_id / "result.json")],
                validate="true", network=False, depth=0, section=[],
                requirement_id=["metric.semantic_density"],
                language=["Python"], worker_mode="packet-only",
                layout="shared-inputs-first",
            ))
        task = json.loads(
            (root / "work" / "agents" / agent_id / "task.json").read_text("utf-8")
        )
        components = task["prompt_components"]
        kinds = [component["kind"] for component in components]
        task_index = kinds.index("task")
        task_inputs = [
            index for index, kind in enumerate(kinds)
            if kind.startswith("task-input:")
        ]
        assigned_index = kinds.index("embedded:assigned_requirements.json")
        check(
            task_inputs
            and all(index < task_index for index in task_inputs)
            and task_index < assigned_index,
            (
                "shared-inputs-first byte order changed; moving unit-specific inputs "
                f"across the task header invalidates certified prompt hashes: {kinds}"
            ),
        )

        text = benchmark.render_prompt_components(
            components, task["prompt_sha256"]
        ).decode("utf-8")
        marker = {"type": "ephemeral"}
        blocks = inference_gateway.split_cached_user_content(text, marker)
        check(
            len(blocks) == 2
            and "cache_control" in blocks[0]
            and "cache_control" not in blocks[1]
            and blocks[0]["text"] + blocks[1]["text"] == text
            and blocks[1]["text"].startswith(inference_gateway.TASK_INPUT_MARKER),
            "the provider adapter did not split before the first unit-specific input",
        )
        check(
            "unit-specific input A" not in blocks[0]["text"]
            and "unit-specific input A" in blocks[1]["text"],
            "unit-specific evidence leaked into the supposedly shared cache prefix",
        )

        # A sibling with different task-local evidence must keep the exact same
        # cached prefix while changing only the uncached tail.
        (docs / "other.md").write_text(
            "unit-specific input B\n" * 50, encoding="utf-8"
        )
        sibling_id = "worker-layout-shared-sibling"
        with contextlib.redirect_stdout(io.StringIO()):
            benchmark.cmd_task_create(argparse.Namespace(
                workspace=str(root), id=sibling_id, parent=None,
                evaluation="semantic_compression",
                goal="layout test", read=[str(docs / "other.md")], write=None,
                output=[str(root / "work" / "agents" / sibling_id / "result.json")],
                validate="true", network=False, depth=0, section=[],
                requirement_id=["metric.semantic_density"],
                language=["Python"], worker_mode="packet-only",
                layout="shared-inputs-first",
            ))
        sibling = json.loads(
            (root / "work" / "agents" / sibling_id / "task.json").read_text("utf-8")
        )
        sibling_text = benchmark.render_prompt_components(
            sibling["prompt_components"], sibling["prompt_sha256"]
        ).decode("utf-8")
        sibling_blocks = inference_gateway.split_cached_user_content(
            sibling_text, marker
        )
        check(
            len(sibling_blocks) == 2
            and sibling_blocks[0]["text"] == blocks[0]["text"]
            and sibling_blocks[1]["text"] != blocks[1]["text"]
            and "unit-specific input B" in sibling_blocks[1]["text"],
            "sibling packets did not share only their frozen prefix",
        )

        # When there is no unit-specific embedded input, the legacy Task Packet
        # header remains the fallback breakpoint.
        fallback = (
            "shared frozen prefix"
            + inference_gateway.PACKET_HEADER_MARKER
            + "worker-x\nrest"
        )
        fallback_blocks = inference_gateway.split_cached_user_content(
            fallback, marker
        )
        check(
            len(fallback_blocks) == 2
            and fallback_blocks[0]["text"] == "shared frozen prefix"
            and fallback_blocks[1]["text"].startswith(
                inference_gateway.PACKET_HEADER_MARKER
            ),
            "header fallback breakpoint drifted",
        )
        plain = inference_gateway.split_cached_user_content(
            "# Task Packet: x\nheader first", marker
        )
        check(
            len(plain) == 1 and "cache_control" in plain[0],
            f"a header-first packet was unexpectedly split: {plain}",
        )

        # End to end through the adapter: content blocks may change shape, never
        # their concatenated model-visible bytes.
        provider = _anthropic_provider()
        http = _ScriptedHTTP([
            {"content": [{"type": "text", "text": "ready"}],
             "stop_reason": "end_turn", "usage": {}},
        ])
        provider._open = http
        provider.complete(
            _validated_request(messages=[{"role": "user", "content": text}])
        )
        content = http.payloads[0]["messages"][-1]["content"]
        check(
            isinstance(content, list)
            and len(content) == 2
            and "cache_control" in content[0]
            and content[0]["text"] + content[1]["text"] == text,
            "provider request changed model-visible Task Packet bytes",
        )


def main() -> int:
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    for test in tests:
        try:
            test()
        except Exception as exc:  # surface the failing test rather than aborting the file
            FAILURES.append(f"{test.__name__} raised {type(exc).__name__}: {exc}")

    if FAILURES:
        print("benchmark isolation failures:", file=sys.stderr)
        for failure in FAILURES:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"benchmark isolation checks passed ({len(tests)} groups)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
