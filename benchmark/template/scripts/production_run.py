#!/usr/bin/env python3
"""Production Quidra benchmark driver used by the manual GitHub Actions run."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

import benchmark

SCRIPTS_DIR = Path(__file__).resolve().parent


def load_module(path: Path, name: str) -> Any:
    import importlib.util

    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CLI = SCRIPTS_DIR / "benchmark.py"
AGENT = SCRIPTS_DIR / "sandbox_agent.py"


class ProductionRunError(RuntimeError):
    pass


def json_load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def run_cli(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [sys.executable, str(CLI), *args, "--workspace", str(root)],
        cwd=root,
        env=dict(os.environ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and completed.returncode != 0:
        raise ProductionRunError(
            f"benchmark.py {' '.join(args)} failed: "
            f"{(completed.stderr or completed.stdout).strip()}"
        )
    return completed


def write_policy(root: Path, output: Path) -> dict[str, Any]:
    benchmark.assert_template_integrity(root)
    manifest_path = root / "work" / "root" / "manifest.json"
    if not manifest_path.is_file():
        raise ProductionRunError("manifest missing; run deterministic prepare first")
    manifest = json_load(manifest_path)
    tasks: dict[str, str] = {}
    for unit in manifest.get("work_units", []):
        if unit.get("execution_kind", "agent") != "agent":
            continue
        agent_id = str(unit.get("assigned_agent_id") or "")
        if not agent_id:
            raise ProductionRunError(f"agent work unit has no assigned agent: {unit.get('id')}")
        policy = "allowed" if bool(unit.get("network_allowed")) else "disabled"
        if agent_id in tasks and tasks[agent_id] != policy:
            raise ProductionRunError(f"conflicting network policy for {agent_id}")
        tasks[agent_id] = policy

    payload = {
        "schema_version": 1,
        "manifest_sha256": benchmark.sha256_file(manifest_path),
        "tasks": tasks,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return payload


def manifest_units(root: Path) -> dict[str, dict[str, Any]]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    return {str(unit["id"]): unit for unit in manifest.get("work_units", [])}


def ledger_state(root: Path, work_unit_id: str) -> dict[str, Any]:
    return json_load(root / "work" / "root" / "ledger.json")["units"][work_unit_id]


def classify_failure(detail: str) -> str:
    lowered = detail.lower()
    if any(token in lowered for token in (
        "gateway", "provider", "transport", "connection", "timed out", "timeout",
        "rate limit", "http 429", "http 5",
    )):
        return "infrastructure"
    return "ordinary-incomplete"


def handle_worker_failure(root: Path, unit: dict[str, Any], detail: str) -> None:
    if "soft api budget exhausted" in detail.lower():
        raise ProductionRunError(
            "soft API budget exhausted; stopping before another paid request"
        )

    uid = str(unit["id"])
    state = ledger_state(root, uid)
    if state.get("status") != "RUNNING":
        return
    attempts = int(state.get("attempts", 0) or 0)
    max_attempts = int(state.get("max_attempts", 3) or 3)
    safe_detail = " ".join(detail.strip().split())[:1200] or "worker process failed"
    blocker_class = classify_failure(safe_detail)

    if attempts < max_attempts:
        benchmark.archive_attempt(
            root, unit, attempts, "worker-process-failed-retry", reset=True
        )
        run_cli(
            root, "ledger-update", "--id", uid, "--status", "PENDING",
            "--validation-result", "FAIL",
        )
        return

    run_cli(
        root, "ledger-update", "--id", uid, "--status", "BLOCKED",
        "--validation-result", "FAIL",
        "--blocker", f"worker process failed after {attempts} attempt(s): {safe_detail}",
        "--blocker-class", blocker_class,
    )


def dispatch_one(root: Path, task: dict[str, Any], units: dict[str, dict[str, Any]]) -> None:
    uid = str(task["work_unit_id"])
    unit = units[uid]
    agent_id = str(task["agent_id"])
    run_cli(root, "task-start", "--id", uid)

    max_output = int(unit.get("max_output_tokens_per_call", 0) or 8192)
    max_output = max(1, min(max_output, 32768))
    if task.get("worker_mode") == "packet-only":
        argv = [
            sys.executable, str(CLI), "task-infer",
            "--workspace", str(root), "--id", agent_id,
            "--max-output-tokens", str(max_output),
        ]
    elif task.get("worker_mode") == "sandbox-agent":
        argv = [
            sys.executable, str(AGENT),
            "--workspace", str(root), "--id", agent_id,
            "--max-output-tokens", str(max_output),
        ]
    else:
        raise ProductionRunError(
            f"unsupported worker mode for {uid}: {task.get('worker_mode')!r}"
        )

    worker = subprocess.run(
        argv,
        cwd=root,
        env=dict(os.environ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if worker.returncode != 0:
        handle_worker_failure(root, unit, worker.stderr or worker.stdout)
        return

    finish = run_cli(root, "task-finish", "--id", uid, check=False)
    if finish.returncode != 0 and ledger_state(root, uid).get("status") == "RUNNING":
        handle_worker_failure(root, unit, finish.stderr or finish.stdout)


def run_production(root: Path, max_iterations: int) -> dict[str, Any]:
    run_cli(root, "preflight")
    run_cli(root, "prepare")

    dispatched = 0
    for _ in range(max_iterations):
        run_cli(root, "advance")
        queue = json_load(root / "results" / "dispatch_queue.json").get("tasks", [])
        if not queue:
            ledger = json_load(root / "work" / "root" / "ledger.json")
            states = {str(v.get("status", "PENDING")) for v in ledger["units"].values()}
            if states <= {"COMPLETE", "BLOCKED", "INVALID"}:
                break
            raise ProductionRunError(
                f"dispatch queue is empty while ledger is nonterminal: {sorted(states)}"
            )

        units = manifest_units(root)
        for task in queue:
            dispatch_one(root, task, units)
            dispatched += 1
    else:
        raise ProductionRunError(
            f"production benchmark exceeded {max_iterations} runner iterations"
        )

    run_cli(root, "advance")
    run_cli(root, "finalize")
    payload = {
        "schema_version": 1,
        "ok": True,
        "dispatched_agent_attempts": dispatched,
        "finalization": str(root / "results" / "finalization.json"),
    }
    benchmark.json_dump(root / "results" / "production_run.json", payload)
    return payload


def provider_smoke(
    model: str, template: Path, budget_usd: float, timeout: float
) -> dict[str, Any]:
    """Spend a few cents proving the paid path works before spending the rest.

    Everything up to this point has run against the deterministic fake provider,
    so the one thing never exercised is the request this benchmark will actually
    send: the frozen decoding state, the frozen web-search tool, and the response
    shape the evaluated model returns. A malformed request fails identically on
    call 1 and call 1160, and finding out on call 1 costs a fraction of a cent
    instead of an image build, a planning pass and a partial run.

    The checks that need no provider call run first and for free.
    """
    import shutil
    import tempfile
    import time
    import uuid

    gateway_client = load_module(SCRIPTS_DIR / "gateway_client.py", "smoke_gateway_client")

    workdir = Path(tempfile.mkdtemp(prefix="quidra-smoke-"))
    sock = workdir / "s.sock"
    policy_path = workdir / "task-policy.json"
    log_path = workdir / "audit.jsonl"
    granted, refused = "smoke-network-task", "smoke-unknown-task"
    policy_path.write_text(
        json.dumps({"schema_version": 1, "tasks": {granted: "allowed"}}) + "\n",
        encoding="utf-8",
    )

    process = subprocess.Popen(
        [
            sys.executable, str(SCRIPTS_DIR / "inference_gateway.py"), "serve",
            "--socket", str(sock),
            "--template", str(template),
            "--provider", "anthropic-messages",
            "--model", model,
            "--budget-usd", str(budget_usd),
            "--network-policy", "disabled",
            "--task-policy", str(policy_path),
            "--log", str(log_path),
        ],
        env=dict(os.environ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    checks: list[dict[str, Any]] = []

    def record(name: str, ok: bool, detail: Any = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not sock.is_socket():
            if process.poll() is not None:
                out, err = process.communicate(timeout=5)
                raise ProductionRunError(
                    f"the gateway could not start with the paid provider: {err or out}"
                )
            time.sleep(0.05)
        if not sock.is_socket():
            raise ProductionRunError("the gateway did not bind its socket")

        client = gateway_client.InferenceGatewayClient(sock, timeout=timeout)
        health = client.health()
        record(
            "handshake reports the paid provider",
            health.get("provider", {}).get("id") == "anthropic-messages",
            health.get("provider"),
        )
        frozen_tool = (
            json.loads((template / "config" / "inference_gateway.json").read_text("utf-8"))
            .get("anthropic_web_search", {})
            .get("tool_type")
        )
        declared = [entry.get("type") for entry in health.get("exposed_tool_surface", [])]
        record(
            "the declared tool surface matches the frozen policy",
            declared == [frozen_tool],
            {"declared": declared, "frozen": frozen_tool},
        )

        # Free checks: both are refused before any provider call.
        for name, payload in (
            ("a task absent from the frozen policy is refused", {
                "schema_version": 1, "kind": "inference.request",
                "request_id": "smoke-unknown", "task_id": refused,
                "messages": [{"role": "user", "content": "x"}],
            }),
            ("a sandbox-supplied temperature is refused", {
                "schema_version": 1, "kind": "inference.request",
                "request_id": "smoke-temp", "task_id": granted,
                "messages": [{"role": "user", "content": "x"}], "temperature": 0.0,
            }),
        ):
            response = client.raw_exchange(payload)
            record(
                name,
                response.get("kind") == "inference.error"
                and response.get("error", {}).get("class") == "policy",
                response.get("error", {}).get("message"),
            )

        # The first real call. Tiny prompt, tiny output cap.
        plain = client.complete(
            [{"role": "user", "content": "Reply with the single word: ready"}],
            task_id="smoke-plain",
            max_output_tokens=16,
            request_id=uuid.uuid4().hex,
        )
        record("the frozen request shape is accepted by the provider", bool(plain.get("content")))
        record(
            "the response carries usable text",
            isinstance(plain.get("content"), str) and plain["content"].strip() != "",
            plain.get("content", "")[:120],
        )
        record(
            "usage is reported for cost accounting",
            int(plain.get("usage", {}).get("input_tokens", 0)) > 0,
            plain.get("usage"),
        )

        # The second real call exercises the network-enabled payload shape, which
        # carries the frozen server-side tool and is otherwise never sent.
        searched = client.complete(
            [{"role": "user", "content": "Reply with the single word: ready"}],
            task_id=granted,
            max_output_tokens=16,
            network_allowed=True,
            request_id=uuid.uuid4().hex,
        )
        record(
            "the network-enabled request shape is accepted by the provider",
            bool(searched.get("content")),
            searched.get("usage"),
        )
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()

    cost = build_cost_report(log_path)
    shutil.rmtree(workdir, ignore_errors=True)

    failures = [c["check"] for c in checks if not c["ok"]]
    payload = {
        "schema_version": 1,
        "ok": not failures,
        "model": model,
        "checks": checks,
        "spend": cost,
    }
    if failures:
        payload["failed_checks"] = failures
    return payload


def build_cost_report(log_path: Path) -> dict[str, Any]:
    calls = 0
    input_tokens = 0
    output_tokens = 0
    searches = 0
    estimated_cost = 0.0
    if log_path.is_file():
        for line in log_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("event") != "inference":
                continue
            calls += 1
            usage = record.get("usage", {}) or {}
            input_tokens += int(usage.get("input_tokens", 0) or 0)
            output_tokens += int(usage.get("output_tokens", 0) or 0)
            searches += int(usage.get("web_search_requests", 0) or 0)
            estimated_cost += float(usage.get("estimated_cost_usd", 0.0) or 0.0)
    return {
        "schema_version": 1,
        "paid_inference_calls": calls,
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "web_search_requests": searches,
        "estimated_cost_usd": round(estimated_cost, 6),
        "note": "Estimated from the frozen per-token and web-search prices in the benchmark template.",
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    policy = sub.add_parser("policy", help="freeze trusted per-task network policy")
    policy.add_argument("--workspace", required=True)
    policy.add_argument("--output", required=True)

    run = sub.add_parser("run", help="drive the prepared scored run to finalization")
    run.add_argument("--workspace", default="/quidra-benchmark")
    run.add_argument("--max-iterations", type=int, default=200)

    smoke = sub.add_parser(
        "provider-smoke",
        help="spend a few cents proving the paid request shape works before the run",
    )
    smoke.add_argument("--model", required=True)
    smoke.add_argument("--template", default=str(SCRIPTS_DIR.parent))
    smoke.add_argument("--budget-usd", type=float, default=0.50)
    smoke.add_argument("--timeout", type=float, default=180.0)
    smoke.add_argument("--output")

    cost = sub.add_parser("cost-report", help="summarize the trusted gateway audit log")
    cost.add_argument("--log", required=True)
    cost.add_argument("--output")

    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "policy":
        payload = write_policy(Path(args.workspace).resolve(), Path(args.output).resolve())
    elif args.command == "provider-smoke":
        payload = provider_smoke(
            args.model, Path(args.template).resolve(),
            float(args.budget_usd), float(args.timeout),
        )
        if args.output:
            Path(args.output).write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
        if not payload["ok"]:
            print(json.dumps(payload, indent=2, sort_keys=True))
            raise ProductionRunError(
                "the paid request shape failed before the run: "
                + ", ".join(payload["failed_checks"])
            )
    elif args.command == "run":
        payload = run_production(Path(args.workspace).resolve(), int(args.max_iterations))
    else:
        payload = build_cost_report(Path(args.log).resolve())
        if args.output:
            Path(args.output).write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ProductionRunError, benchmark.BenchmarkError, OSError, ValueError, KeyError) as exc:
        print(f"production benchmark error: {exc}", file=sys.stderr)
        raise SystemExit(2)
