#!/usr/bin/env python3
"""Real sandbox launcher for scored benchmark work.

This is the trusted host-side component that turns `<source-repo>/.quidra-benchmark`
into the canonical `/quidra-benchmark` sandbox. It is the only thing allowed to
emit the `QUIDRA_BENCHMARK_*_ATTESTED` variables, and it emits them only after
actually applying the corresponding restriction, so an attestation is a record of
what was enforced rather than a claim anyone can type.

Topology
--------

    trusted side                         scored side
    ------------                         -----------
    gateway container                    scored container
      provider credentials                 no credentials
      network: bridge                      network: none
      /gateway  (shared volume)  <──socket──>  /quidra-benchmark/gateway
                                           /quidra-benchmark        rw
                                           /quidra-benchmark/repo     ro
                                           /quidra-benchmark/template ro

The two containers share one Unix domain socket on a container-engine volume.
A volume rather than a host bind mount is what makes this work identically on
Linux and on macOS through Colima or Docker Desktop, where a socket created by a
macOS process inside a shared folder is not usable from inside the VM.

Because the socket is the scored container's only channel to anything outside
itself, the scored container runs with `--network none`: provider egress belongs
to the gateway alone.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
TEMPLATE_DIR = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import benchmark  # noqa: E402  (sibling module inside the frozen template)

CANONICAL_WORKSPACE = benchmark.CANONICAL_WORKSPACE
CANONICAL_ROOT = CANONICAL_WORKSPACE.as_posix()
GATEWAY_MOUNTPOINT = f"{CANONICAL_ROOT}/gateway"
GATEWAY_SOCKET = f"{GATEWAY_MOUNTPOINT}/inference.sock"
CONTRACT_VERSION = "quidra-sandbox-launcher-v1"


class LauncherError(RuntimeError):
    pass


def load_runtime_manifest() -> dict[str, Any]:
    path = TEMPLATE_DIR / "runtime" / "toolchains.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1:
        raise LauncherError(f"unsupported runtime manifest schema: {path}")
    return manifest


def default_image() -> str:
    manifest = load_runtime_manifest()
    return f"{manifest['image']['repository']}:{manifest['image']['tag']}"


# --------------------------------------------------------------------------
# The contract: one definition, used both to launch and to describe
# --------------------------------------------------------------------------


def scored_environment() -> dict[str, str]:
    """Exactly the environment the scored container receives.

    Nothing from the launcher's own environment is inherited. There is no
    passthrough switch here on purpose: a passthrough is how `ANTHROPIC_API_KEY`
    or `SSH_AUTH_SOCK` reaches scored work by accident.
    """
    return {
        "HOME": f"{CANONICAL_ROOT}/home",
        "TMPDIR": f"{CANONICAL_ROOT}/tmp",
        "TMP": f"{CANONICAL_ROOT}/tmp",
        "TEMP": f"{CANONICAL_ROOT}/tmp",
        "PWD": CANONICAL_ROOT,
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONNOUSERSITE": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "QUIDRA_BENCHMARK_SANDBOX_ATTESTED": "container",
        "QUIDRA_BENCHMARK_SANDBOX_ROOT": CANONICAL_ROOT,
        "QUIDRA_BENCHMARK_WORKER_GATEWAY_ATTESTED": "packet-gateway-v1",
        "QUIDRA_BENCHMARK_PACKET_WORKER_LOCAL_TOOLS": "disabled",
        "QUIDRA_BENCHMARK_SANDBOX_AGENT_LAUNCHER_ATTESTED": "inside-sandbox-v1",
        "QUIDRA_BENCHMARK_INFERENCE_SOCKET": GATEWAY_SOCKET,
    }


def launcher_contract(uid: int, gid: int, image: str, network_policy: str) -> dict[str, Any]:
    """Machine-readable statement of what the launcher enforced.

    `preflight` re-derives every one of these claims from the running process and
    refuses when a claim and the observed reality disagree. The contract can
    therefore only ever fail a run, never pass one that reality would not.
    """
    return {
        "schema_version": 1,
        "contract": CONTRACT_VERSION,
        "sandbox_mode": "container",
        "workspace_root": CANONICAL_ROOT,
        "run_as_root": False,
        "uid": uid,
        "gid": gid,
        "image": image,
        "network": "none",
        "no_new_privileges": True,
        "capabilities_dropped": "ALL",
        "read_only_root_filesystem": True,
        "read_only_paths": [f"{CANONICAL_ROOT}/repo", f"{CANONICAL_ROOT}/template"],
        "writable_paths": [
            f"{CANONICAL_ROOT}/work",
            f"{CANONICAL_ROOT}/results",
            f"{CANONICAL_ROOT}/raw",
            f"{CANONICAL_ROOT}/prompts",
            f"{CANONICAL_ROOT}/home",
            f"{CANONICAL_ROOT}/tmp",
        ],
        "inference_socket": GATEWAY_SOCKET,
        "inference_network_policy": network_policy,
        "host_home_mounted": False,
        "ssh_agent_forwarded": False,
        "provider_credentials_in_sandbox": False,
        "claude_configuration_mounted": False,
        "environment": scored_environment(),
    }


def scored_container_argv(
    engine: str,
    *,
    image: str,
    staging: Path,
    gateway_mount: str,
    name: str,
    uid: int,
    gid: int,
    contract: dict[str, Any],
    interactive: bool,
    argv: list[str],
) -> list[str]:
    """Build the scored container command line from the contract."""
    command = [
        engine, "run", "--rm",
        "--name", name,
        "--network", "none",
        "--user", f"{uid}:{gid}",
        "--security-opt", "no-new-privileges",
        "--cap-drop", "ALL",
        "--read-only",
        "--tmpfs", "/tmp:rw,nosuid,nodev,exec,size=256m",
        "--pids-limit", "1024",
        "--workdir", CANONICAL_ROOT,
        "--volume", f"{staging}:{CANONICAL_ROOT}:rw",
        "--volume", f"{staging / 'repo'}:{CANONICAL_ROOT}/repo:ro",
        "--volume", f"{staging / 'template'}:{CANONICAL_ROOT}/template:ro",
        "--volume", f"{gateway_mount}:{GATEWAY_MOUNTPOINT}:rw",
    ]
    if interactive:
        command.append("--interactive")
    for key, value in contract["environment"].items():
        command += ["--env", f"{key}={value}"]
    command += [
        "--env",
        "QUIDRA_BENCHMARK_LAUNCHER_CONTRACT=" + json.dumps(contract, sort_keys=True),
    ]
    command.append(image)
    command += argv
    return command


def gateway_container_argv(
    engine: str,
    *,
    image: str,
    staging: Path,
    gateway_mount: str,
    name: str,
    uid: int,
    gid: int,
    provider: str,
    model: str | None,
    fake_script: Path | None,
    exec_command: list[str] | None,
    network_policy: str,
    task_policy: Path | None,
    credential_env: dict[str, str],
    extra_mounts: list[str] | None = None,
) -> list[str]:
    """Build the trusted gateway sidecar command line.

    This container is the credential side of the boundary: it keeps provider
    egress and any API key, and exposes only the socket on the shared volume.
    """
    command = [
        engine, "run", "--rm", "--detach",
        "--name", name,
        "--user", f"{uid}:{gid}",
        "--security-opt", "no-new-privileges",
        "--cap-drop", "ALL",
        "--read-only",
        "--tmpfs", "/tmp:rw,nosuid,nodev,size=64m",
        "--workdir", "/",
        "--volume", f"{staging / 'template'}:/template:ro",
        "--volume", f"{gateway_mount}:/gateway:rw",
        "--env", "HOME=/tmp",
        "--env", "PYTHONDONTWRITEBYTECODE=1",
    ]
    if fake_script is not None:
        command += ["--volume", f"{fake_script.parent}:/fake:ro"]
    if task_policy is not None:
        command += ["--volume", f"{task_policy}:/policy/task_policy.json:ro"]
    # Only ever applied to this container. An authenticated local agent CLI and
    # its session directory belong on the credential side of the boundary; the
    # scored container's mounts are fixed by the contract and cannot be extended.
    for mount in extra_mounts or []:
        command += ["--volume", mount]
    for key, value in credential_env.items():
        command += ["--env", f"{key}={value}"]
    command.append(image)
    command += [
        "python3", "/template/scripts/inference_gateway.py", "serve",
        "--socket", "/gateway/inference.sock",
        "--template", "/template",
        "--provider", provider,
        "--network-policy", network_policy,
        "--log", "/gateway/gateway-audit.jsonl",
        "--ready-file", "/gateway/gateway-ready.json",
    ]
    if model:
        command += ["--model", model]
    if fake_script is not None:
        command += ["--fake-script", f"/fake/{fake_script.name}"]
    if task_policy is not None:
        command += ["--task-policy", "/policy/task_policy.json"]
    if exec_command:
        command += ["--exec-command", *exec_command]
    return command


# --------------------------------------------------------------------------
# Engine helpers
# --------------------------------------------------------------------------


def run_engine(argv: list[str], *, check: bool = True, capture: bool = True) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        argv,
        shell=False,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
    )
    if check and completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise LauncherError(f"{' '.join(argv[:3])} failed: {detail}")
    return completed


def require_engine(engine: str) -> str:
    resolved = shutil.which(engine)
    if not resolved:
        raise LauncherError(
            f"container engine {engine!r} was not found. Scored work must not start "
            "without a real isolation boundary; install Docker, Podman or Colima, or "
            "provide another trusted sandbox that can present the staging directory "
            f"as {CANONICAL_ROOT}."
        )
    probe = run_engine([resolved, "info", "--format", "{{.ServerVersion}}"], check=False)
    if probe.returncode != 0:
        raise LauncherError(
            f"{engine} is installed but its daemon is not reachable: "
            f"{(probe.stderr or probe.stdout).strip()}"
        )
    return resolved


def sandbox_user() -> tuple[int, int]:
    """Run the container as the invoking non-root user where that is meaningful.

    Matching the host uid keeps the bind-mounted staging directory writable on
    Linux. When the launcher itself runs as root the container still must not, so
    fall back to the image's unprivileged account.
    """
    uid = os.getuid()
    gid = os.getgid()
    if uid == 0:
        return 1000, 1000
    return uid, gid


def collect_credential_env(provider: str) -> dict[str, str]:
    """Credentials for the trusted gateway only. Never part of the scored contract."""
    if provider != "anthropic-messages":
        return {}
    env = {}
    for name in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_BASE_URL"):
        value = os.environ.get(name)
        if value:
            env[name] = value
    if not env:
        raise LauncherError(
            "provider anthropic-messages needs ANTHROPIC_API_KEY in the launcher "
            "environment; it is passed to the gateway container only"
        )
    return env


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------


def cmd_contract(args: argparse.Namespace) -> int:
    uid, gid = sandbox_user()
    contract = launcher_contract(uid, gid, args.image or default_image(), args.network_policy)
    print(json.dumps(contract, indent=2, sort_keys=True))
    return 0


def cmd_doctor(args: argparse.Namespace) -> int:
    manifest = load_runtime_manifest()
    image = args.image or default_image()
    report: dict[str, Any] = {
        "schema_version": 1,
        "engine": args.engine,
        "engine_path": shutil.which(args.engine),
        "image": image,
        "runtime_manifest": manifest["image"],
        "uid_gid": list(sandbox_user()),
    }
    if report["engine_path"]:
        info = run_engine([report["engine_path"], "info", "--format", "{{.ServerVersion}}"],
                          check=False)
        report["engine_available"] = info.returncode == 0
        report["engine_version"] = (info.stdout or "").strip() or None
        report["engine_error"] = None if info.returncode == 0 else (info.stderr or "").strip()
        if report["engine_available"]:
            images = run_engine(
                [report["engine_path"], "image", "inspect", image, "--format", "{{.Id}}"],
                check=False,
            )
            report["image_present"] = images.returncode == 0
    else:
        report["engine_available"] = False
        report["image_present"] = False
    report["ok"] = bool(report.get("engine_available") and report.get("image_present"))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 2


def cmd_build_image(args: argparse.Namespace) -> int:
    engine = require_engine(args.engine)
    manifest = load_runtime_manifest()
    image = args.image or default_image()
    context = TEMPLATE_DIR / "runtime"
    command = [engine, "build", "--tag", image, "--file", str(context / "Dockerfile")]
    if args.target:
        command += ["--target", args.target]
    for key, value in manifest["toolchains"].items():
        command += ["--build-arg", f"{key}={value}"]
    command.append(str(context))
    run_engine(command, capture=False)
    print(json.dumps({"ok": True, "image": image, "target": args.target}, indent=2))
    return 0


def wait_for_gateway(engine: str, container: str, volume_reader: list[str], timeout: float) -> None:
    """Block until the sidecar has bound its socket, or explain why it never did."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        probe = run_engine([*volume_reader, "test", "-S", "/gateway/inference.sock"], check=False)
        if probe.returncode == 0:
            return
        alive = run_engine(
            [engine, "inspect", "--format", "{{.State.Running}}", container], check=False
        )
        if (alive.stdout or "").strip() != "true":
            logs = run_engine([engine, "logs", container], check=False)
            raise LauncherError(
                "the trusted gateway container exited before binding its socket: "
                + ((logs.stderr or logs.stdout or "").strip()[-2000:] or "no output")
            )
        time.sleep(0.3)
    raise LauncherError(f"the trusted gateway socket did not appear within {timeout}s")


def probe_staging_visibility(
    engine: str, image: str, staging: Path, uid: int, gid: int
) -> None:
    """Confirm the container really sees the staging directory.

    On macOS the engine runs in a VM that only shares configured host paths.
    Colima shares $HOME by default, so a checkout elsewhere bind-mounts as an
    empty directory and the run fails much later with a confusing error. Asking
    the container what it can see is cheaper and more reliable than trying to
    infer the VM's mount configuration.
    """
    probe = run_engine(
        [
            engine, "run", "--rm", "--network", "none",
            "--user", f"{uid}:{gid}",
            "--volume", f"{staging}:{CANONICAL_ROOT}:ro",
            image,
            "test", "-f", f"{CANONICAL_ROOT}/run.json",
        ],
        check=False,
    )
    if probe.returncode != 0:
        raise LauncherError(
            f"the container cannot see {staging}. The engine is running in a VM "
            "that does not share this path. On Colima, either move the checkout "
            "under your home directory or restart with the path shared, for "
            f"example: colima start --mount '{staging.parent}:w'"
        )


def cmd_selftest(args: argparse.Namespace) -> int:
    """Prove a --network none container can reach the gateway over the shared volume.

    This is the one assumption the whole topology rests on and the one most
    likely to differ between engines: a Unix socket created by the gateway
    container has to be usable from a scored container that has no network at
    all. It takes seconds and needs no staged workspace, so it is the first
    thing to run on a new machine.
    """
    engine = require_engine(args.engine)
    image = args.image or default_image()
    token = uuid.uuid4().hex[:12]
    volume = f"quidra-benchmark-selftest-{token}"
    gateway_name = f"quidra-benchmark-selftest-gateway-{token}"
    uid, gid = sandbox_user()
    steps: list[dict[str, Any]] = []

    def record(name: str, ok: bool, detail: str = "") -> None:
        steps.append({"step": name, "ok": ok, "detail": detail})

    started = False
    try:
        run_engine([engine, "volume", "create", volume])
        run_engine([
            engine, "run", "--rm", "--network", "none", "--user", "0:0",
            "--volume", f"{volume}:/gateway", image,
            "chown", f"{uid}:{gid}", "/gateway",
        ])
        record("shared volume created and owned by the sandbox account", True)

        run_engine([
            engine, "run", "--rm", "--detach", "--name", gateway_name,
            "--user", f"{uid}:{gid}",
            "--security-opt", "no-new-privileges", "--cap-drop", "ALL",
            "--volume", f"{TEMPLATE_DIR}:/template:ro",
            "--volume", f"{volume}:/gateway:rw",
            "--env", "HOME=/tmp",
            image,
            "python3", "/template/scripts/inference_gateway.py", "serve",
            "--socket", "/gateway/inference.sock",
            "--template", "/template",
            "--provider", "fake",
            "--network-policy", "disabled",
        ])
        started = True
        reader = [
            engine, "run", "--rm", "--network", "none", "--user", f"{uid}:{gid}",
            "--volume", f"{volume}:/gateway:ro", image,
        ]
        wait_for_gateway(engine, gateway_name, reader, float(args.timeout))
        record("trusted gateway bound its socket on the shared volume", True)

        # The decisive check: no network namespace at all, yet the handshake works.
        handshake = run_engine(
            [
                engine, "run", "--rm", "--network", "none",
                "--user", f"{uid}:{gid}",
                "--security-opt", "no-new-privileges", "--cap-drop", "ALL",
                "--read-only", "--tmpfs", "/tmp",
                "--volume", f"{TEMPLATE_DIR}:/template:ro",
                "--volume", f"{volume}:{GATEWAY_MOUNTPOINT}:rw",
                "--env", "HOME=/tmp",
                image,
                "python3", "/template/scripts/gateway_client.py", "health",
                "--socket", GATEWAY_SOCKET,
            ],
            check=False,
        )
        ok = handshake.returncode == 0 and "quidra-inference-gateway-v1" in handshake.stdout
        record(
            "a --network none container completed the credential-less handshake",
            ok,
            "" if ok else (handshake.stderr or handshake.stdout).strip()[-800:],
        )
    except LauncherError as exc:
        record("selftest aborted", False, str(exc))
    finally:
        if started:
            run_engine([engine, "stop", "--time", "5", gateway_name], check=False)
            run_engine([engine, "rm", "--force", gateway_name], check=False)
        run_engine([engine, "volume", "rm", "--force", volume], check=False)

    passed = all(step["ok"] for step in steps) and len(steps) == 3
    payload = {
        "schema_version": 1,
        "ok": passed,
        "engine": args.engine,
        "image": image,
        "platform": sys.platform,
        "steps": steps,
    }
    if not passed:
        payload["note"] = (
            "The scored sandbox reaches the model only through this socket. Until "
            "this passes on the machine that will run the benchmark, scored work "
            "must not start."
        )
    print(json.dumps(payload, indent=2))
    return 0 if passed else 2


def cmd_run(args: argparse.Namespace) -> int:
    engine = require_engine(args.engine)
    image = args.image or default_image()
    source = Path(args.source_repo).resolve()
    staging = benchmark.host_workspace(source)
    for required in ("run.json", "repo", "template"):
        if not (staging / required).exists():
            raise LauncherError(
                f"{staging / required} is missing; run `benchmark.py init --source-repo "
                f"{source}` before launching the sandbox"
            )

    uid, gid = sandbox_user()
    run_token = args.run_token or uuid.uuid4().hex[:12]
    scored_name = f"quidra-benchmark-{run_token}"
    gateway_name = f"quidra-benchmark-gateway-{run_token}"
    volume = args.gateway_socket_dir or f"quidra-benchmark-gateway-{run_token}"
    owns_volume = args.gateway_socket_dir is None
    fake_script = Path(args.fake_script).resolve() if args.fake_script else None

    contract = launcher_contract(uid, gid, image, args.network_policy)
    argv = list(args.argv or [])
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        argv = ["python3", f"{CANONICAL_ROOT}/template/scripts/benchmark.py", "preflight"]

    started_gateway = False
    try:
        if owns_volume:
            run_engine([engine, "volume", "create", volume])
            # A fresh engine volume belongs to root. Both containers run
            # unprivileged, so neither could create the socket in it until the
            # mountpoint is handed to the account they share.
            run_engine([
                engine, "run", "--rm", "--network", "none", "--user", "0:0",
                "--volume", f"{volume}:/gateway", image,
                "chown", f"{uid}:{gid}", "/gateway",
            ])

        if args.gateway == "sidecar":
            command = gateway_container_argv(
                engine,
                image=image,
                staging=staging,
                gateway_mount=volume,
                name=gateway_name,
                uid=uid,
                gid=gid,
                provider=args.provider,
                model=args.model,
                fake_script=fake_script,
                exec_command=shlex.split(args.exec_command) if args.exec_command else None,
                network_policy=args.network_policy,
                task_policy=Path(args.task_policy).resolve() if args.task_policy else None,
                credential_env=collect_credential_env(args.provider),
                extra_mounts=list(args.gateway_mount or []),
            )
            run_engine(command)
            started_gateway = True
            volume_reader = [
                engine, "run", "--rm", "--network", "none",
                "--user", f"{uid}:{gid}",
                "--volume", f"{volume}:/gateway:ro",
                image,
            ]
            wait_for_gateway(engine, gateway_name, volume_reader, float(args.gateway_timeout))

        probe_staging_visibility(engine, image, staging, uid, gid)

        scored = scored_container_argv(
            engine,
            image=image,
            staging=staging,
            gateway_mount=volume,
            name=scored_name,
            uid=uid,
            gid=gid,
            contract=contract,
            interactive=args.interactive,
            argv=argv,
        )
        if args.print_command:
            print(json.dumps({"scored_container_argv": scored}, indent=2))
            return 0
        completed = subprocess.run(scored, shell=False)
        return completed.returncode
    finally:
        if started_gateway:
            if args.gateway_log_out:
                out = Path(args.gateway_log_out).resolve()
                out.parent.mkdir(parents=True, exist_ok=True)
                run_engine(
                    [engine, "cp", f"{gateway_name}:/gateway/gateway-audit.jsonl", str(out)],
                    check=False,
                )
            run_engine([engine, "stop", "--time", "5", gateway_name], check=False)
            run_engine([engine, "rm", "--force", gateway_name], check=False)
        if owns_volume and not args.keep:
            run_engine([engine, "volume", "rm", "--force", volume], check=False)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Launch the scored Quidra benchmark sandbox with a credential-less "
                    "inference gateway"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    contract = sub.add_parser(
        "contract", help="print the exact isolation contract this launcher enforces"
    )
    contract.add_argument("--image")
    contract.add_argument("--network-policy", default="disabled", choices=("disabled", "allowed"))
    contract.set_defaults(func=cmd_contract)

    doctor = sub.add_parser("doctor", help="report container engine and image readiness")
    doctor.add_argument("--engine", default="docker")
    doctor.add_argument("--image")
    doctor.set_defaults(func=cmd_doctor)

    build = sub.add_parser("build-image", help="build the pinned ten-language runtime image")
    build.add_argument("--engine", default="docker")
    build.add_argument("--image")
    build.add_argument("--target", choices=("base", "toolchains"))
    build.set_defaults(func=cmd_build_image)

    selftest = sub.add_parser(
        "selftest",
        help="verify a --network none container can reach the gateway socket on this engine",
    )
    selftest.add_argument("--engine", default="docker")
    selftest.add_argument("--image")
    selftest.add_argument("--timeout", type=float, default=60.0)
    selftest.set_defaults(func=cmd_selftest)

    run = sub.add_parser("run", help="run a command inside the scored sandbox")
    run.add_argument("--source-repo", required=True)
    run.add_argument("--engine", default="docker")
    run.add_argument("--image")
    run.add_argument("--gateway", default="sidecar", choices=("sidecar", "external"))
    run.add_argument(
        "--gateway-socket-dir",
        help="use this existing engine volume or host directory instead of creating one; "
             "required with --gateway external",
    )
    run.add_argument("--gateway-timeout", type=float, default=60.0)
    run.add_argument("--gateway-log-out", help="copy the gateway audit log here on teardown")
    run.add_argument(
        "--provider", default="fake", choices=("fake", "exec", "anthropic-messages")
    )
    run.add_argument("--fake-script")
    run.add_argument("--model")
    run.add_argument(
        "--exec-command",
        help="quoted command for --provider exec, run on the trusted side only, "
             "e.g. \"claude -p --output-format text --disallowed-tools '*'\"",
    )
    run.add_argument(
        "--gateway-mount",
        action="append",
        metavar="HOST:CONTAINER[:ro]",
        help="extra read-only mount for the GATEWAY container only, for a local "
             "agent CLI and its session; never applied to the scored container",
    )
    run.add_argument("--network-policy", default="disabled", choices=("disabled", "allowed"))
    run.add_argument(
        "--task-policy",
        help="trusted-side JSON mapping agent ids to network ceilings, derived from "
             "the frozen manifest rather than from anything the sandbox claims",
    )
    run.add_argument("--run-token")
    run.add_argument("--interactive", action="store_true")
    run.add_argument("--keep", action="store_true", help="keep the gateway volume after teardown")
    run.add_argument(
        "--print-command",
        action="store_true",
        help="print the scored container command instead of running it",
    )
    run.add_argument("argv", nargs=argparse.REMAINDER)
    run.set_defaults(func=cmd_run)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    if getattr(args, "gateway", None) == "external" and not args.gateway_socket_dir:
        print("sandbox launcher error: --gateway external requires --gateway-socket-dir",
              file=sys.stderr)
        return 2
    try:
        return int(args.func(args))
    except LauncherError as exc:
        print(f"sandbox launcher error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
