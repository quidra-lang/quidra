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
                 network_policy: str = "disabled") -> None:
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
                and health.get("exposed_tool_surface") == [],
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

        with Gateway(root / "gateway", script=script) as gw:
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

        packet = benchmark.render_prompt_components(
            json.loads((agent_dir / "task.json").read_text(encoding="utf-8"))["prompt_components"],
            json.loads((agent_dir / "task.json").read_text(encoding="utf-8"))["prompt_sha256"],
        ).decode("utf-8")
        check(
            str(tmp) not in packet,
            "the synthetic host workspace path leaked into a rendered Task Packet",
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


# --------------------------------------------------------------------------
# 5. Configuration stays consistent with the code
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
