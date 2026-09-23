#!/usr/bin/env python3
"""Production Quidra benchmark driver used by the manual GitHub Actions run."""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
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


_LOG_LOCK = threading.Lock()


def log(message: str) -> None:
    """One timestamped progress line on stdout, visible in the workflow log.

    The first paid run was silent for four hours and then reported only that it
    had run out of time. Every unit start and outcome is worth a line.
    """
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%H:%M:%S")
    with _LOG_LOCK:
        print(f"[{stamp}] {message}", flush=True)


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


def write_policy(
    root: Path,
    output: Path,
    model: str | None = None,
    evaluation: str | None = None,
    units: list[str] | None = None,
) -> dict[str, Any]:
    benchmark.assert_template_integrity(root)
    manifest_path = root / "work" / "root" / "manifest.json"
    if not manifest_path.is_file():
        raise ProductionRunError("manifest missing; run deterministic prepare first")
    manifest = json_load(manifest_path)
    isolation = benchmark.worker_isolation_config(root)
    guard = isolation.get("task_spend_guard") or {}
    floor_usd = float(guard.get("floor_usd", 0) or 0)
    multiplier = float(guard.get("envelope_multiplier", 0) or 0)
    gateway = benchmark.gateway_config(root)
    pricing = (gateway.get("anthropic_pricing") or {}).get(model or "", {}) if model else {}
    input_price = float(pricing.get("input_usd_per_million_tokens", 0) or 0)
    output_price = float(pricing.get("output_usd_per_million_tokens", 0) or 0)

    ledger_path = root / "work" / "root" / "ledger.json"
    ledger = json_load(ledger_path) if ledger_path.is_file() else {"units": {}}
    cache_status_path = root / "results" / "cache_status.json"
    cache_status = (
        json_load(cache_status_path)
        if cache_status_path.is_file()
        else {"hits": {}, "misses": {}}
    )

    tasks: dict[str, str] = {}
    budgets: dict[str, float] = {}
    efforts: dict[str, str] = {}
    evaluation_effort = dict(
        (gateway.get("anthropic_decoding") or {}).get("evaluation_effort") or {}
    )
    skipped_complete: list[str] = []
    selected = set(units or [])
    for unit in manifest.get("work_units", []):
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        if unit.get("execution_kind", "agent") != "agent":
            continue
        uid = str(unit.get("id") or "")
        # A unit-scoped rehearsal names the only tasks the gateway may serve;
        # every other agent is refused before it can spend anything.
        if selected and uid not in selected:
            continue
        if (ledger.get("units", {}).get(uid, {}) or {}).get("status") == "COMPLETE":
            skipped_complete.append(uid)
            continue
        agent_id = str(unit.get("assigned_agent_id") or "")
        if not agent_id:
            raise ProductionRunError(f"agent work unit has no assigned agent: {unit.get('id')}")
        policy = "allowed" if bool(unit.get("network_allowed")) else "disabled"
        if agent_id in tasks and tasks[agent_id] != policy:
            raise ProductionRunError(f"conflicting network policy for {agent_id}")
        tasks[agent_id] = policy
        # An evaluation's frozen depth override, pinned per task so the gateway
        # applies it without the sandbox naming it.
        depth = evaluation_effort.get(str(unit.get("evaluation") or ""))
        if depth:
            efforts[agent_id] = str(depth)
        # A per-task soft ceiling the gateway enforces: the unit's planned token
        # envelope at frozen prices, with headroom for the agent's own turns,
        # never below the floor. Its job is to stop one runaway unit from
        # spending the run's budget by itself, not to price the unit exactly.
        if floor_usd > 0 or multiplier > 0:
            calls = int(unit.get("max_llm_calls", 0) or 0)
            per_call = (
                int(unit.get("estimated_input_tokens_per_call", 0) or 0) * input_price
                + int(unit.get("max_output_tokens_per_call", 0) or 0) * output_price
            ) / 1_000_000.0
            budgets[agent_id] = round(max(floor_usd, multiplier * calls * per_call), 4)

    payload = {
        "schema_version": 1,
        "manifest_sha256": benchmark.sha256_file(manifest_path),
        "tasks": tasks,
        "efforts": efforts,
        "cache": {
            "hit_count": len(cache_status.get("hits", {})),
            "miss_count": len(cache_status.get("misses", {})),
            "units_skipped_as_complete": sorted(skipped_complete),
            "paid_task_count": len(tasks),
        },
    }
    if selected:
        payload["units"] = sorted(selected)
    if budgets:
        payload["budgets"] = budgets
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return payload


def manifest_units(root: Path) -> dict[str, dict[str, Any]]:
    manifest = json_load(root / "work" / "root" / "manifest.json")
    return {str(unit["id"]): unit for unit in manifest.get("work_units", [])}


def ledger_state(root: Path, work_unit_id: str) -> dict[str, Any]:
    return json_load(root / "work" / "root" / "ledger.json")["units"][work_unit_id]


#: Provider answers that no retry and no other unit can get past. Continuing
#: would turn every remaining unit into three instant failures and a ledger
#: full of blockers that say nothing about the work.
FATAL_PROVIDER_TOKENS = (
    "soft api budget exhausted",
    "credit balance",
    "provider http 401",
    "provider http 403",
    "authentication_error",
    "permission_error",
)

#: Failures that repeat deterministically for the same request. Re-dispatching
#: the unit buys the same failure again at the same price.
NON_RETRYABLE_TOKENS = (
    "spend ceiling",
    "max_tokens",
    "output limit",
    "gateway refused",
)


def classify_failure(detail: str) -> str:
    lowered = detail.lower()
    if "spend ceiling" in lowered or "gateway refused" in lowered:
        return "budget-plan-defect"
    if any(token in lowered for token in (
        "gateway", "provider", "transport", "connection", "timed out", "timeout",
        "rate limit", "http 429", "http 5",
    )):
        return "infrastructure"
    return "ordinary-incomplete"


def is_retryable(detail: str) -> bool:
    lowered = detail.lower()
    return not any(token in lowered for token in NON_RETRYABLE_TOKENS)


def handle_worker_failure(root: Path, unit: dict[str, Any], detail: str) -> None:
    lowered = detail.lower()
    uid = str(unit["id"])
    state = ledger_state(root, uid)
    if state.get("status") != "RUNNING":
        return
    attempts = int(state.get("attempts", 0) or 0)

    fatal = next((token for token in FATAL_PROVIDER_TOKENS if token in lowered), None)
    if fatal:
        # A provider-wide failure is not a verdict on this work unit. Leaving it
        # RUNNING strands the next continuation behind the lease timeout, because
        # the worker process is already gone but reclaim-stale quite correctly
        # refuses a fresh heartbeat. Archive the attempt and return the unit to
        # PENDING before stopping the paid driver. The next continuation can then
        # hydrate any COMPLETE siblings immediately and retry only this unfinished
        # unit once the provider/budget problem is actually gone.
        safe_detail = " ".join(detail.strip().split())[:1200] or "provider failure"
        benchmark.archive_attempt(
            root, unit, attempts, "fatal-provider-pause", reset=True,
            detail=safe_detail,
        )
        run_cli(
            root, "ledger-update", "--id", uid, "--status", "PENDING",
            "--validation-result", "FAIL",
        )
        raise ProductionRunError(
            f"the provider cannot serve this run any further ({fatal}); stopping "
            "before another paid request with the interrupted unit returned to PENDING"
        )

    max_attempts = int(state.get("max_attempts", 3) or 3)
    safe_detail = " ".join(detail.strip().split())[:1200] or "worker process failed"
    blocker_class = classify_failure(safe_detail)

    if attempts < max_attempts and is_retryable(safe_detail):
        benchmark.archive_attempt(
            root, unit, attempts, "worker-process-failed-retry", reset=True,
            detail=safe_detail,
        )
        run_cli(
            root, "ledger-update", "--id", uid, "--status", "PENDING",
            "--validation-result", "FAIL",
        )
        log(f"retry {uid} (attempt {attempts}/{max_attempts}): {safe_detail[:300]}")
        return

    run_cli(
        root, "ledger-update", "--id", uid, "--status", "BLOCKED",
        "--validation-result", "FAIL",
        "--blocker", f"worker process failed after {attempts} attempt(s): {safe_detail}",
        "--blocker-class", blocker_class,
    )
    log(f"blocked {uid} ({blocker_class}): {safe_detail[:300]}")


def dispatch_one(root: Path, task: dict[str, Any], units: dict[str, dict[str, Any]]) -> None:
    uid = str(task["work_unit_id"])
    unit = units[uid]
    agent_id = str(task["agent_id"])
    run_cli(root, "task-start", "--id", uid)
    started = time.monotonic()
    log(f"start {uid} [{task.get('worker_mode')}]")

    frozen_default = int(
        benchmark.worker_isolation_config(root).get("default_max_output_tokens", 8192)
    )
    ceiling = int(benchmark.gateway_config(root).get("max_output_tokens_ceiling", 65536))
    if task.get("worker_mode") == "packet-only":
        # One shot, no repair loop, and adaptive thinking is billed inside the
        # same cap. At the frozen default every semantic-compression packet of
        # the first paid run spent all 8192 tokens reasoning and returned no
        # text; at a 32768 ceiling the third run's two-metric packets were still
        # truncated. The ceiling is the cap, and the gateway streams the answer
        # so a long generation is not dropped by an idle connection.
        max_output = ceiling
    else:
        max_output = int(unit.get("max_output_tokens_per_call", 0) or frozen_default)
    max_output = max(1, min(max_output, ceiling))
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
        _log_outcome(root, uid, started)
        return

    finish = run_cli(root, "task-finish", "--id", uid, check=False)
    if finish.returncode != 0 and ledger_state(root, uid).get("status") == "RUNNING":
        handle_worker_failure(root, unit, finish.stderr or finish.stdout)
    _log_outcome(root, uid, started)


def _log_outcome(root: Path, uid: str, started: float) -> None:
    state = ledger_state(root, uid)
    status = state.get("status")
    line = (
        f"done {uid} -> {status}/{state.get('validation_result')} "
        f"in {time.monotonic() - started:.0f}s"
    )
    if status == "BLOCKED":
        line += f": {str(state.get('blocker') or '')[:300]}"
    log(line)


def ledger_summary(root: Path, evaluation: str | None) -> str:
    units = manifest_units(root)
    ledger = json_load(root / "work" / "root" / "ledger.json")["units"]
    counts: dict[str, int] = {}
    for uid, unit in units.items():
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        status = str(ledger.get(uid, {}).get("status", "PENDING"))
        counts[status] = counts.get(status, 0) + 1
    return ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))


def dispatch_batch(
    root: Path,
    queue: list[dict[str, Any]],
    units: dict[str, dict[str, Any]],
    concurrency: int,
    *,
    may_start=lambda: True,
    dispatch=None,
) -> int:
    """Run one dispatch queue with up to `concurrency` workers in flight.

    Every unit in a queue is dependency-ready, and each one owns its own agent
    directory; the only shared state is the ledger, whose updates are serialized
    in `benchmark.py`. The first paid run dispatched strictly one unit at a
    time and used the whole GitHub Actions window on 182 attempts, most of them
    a single ninety-second provider call; the runner spent that time waiting.

    `may_start` is consulted before each submission so a wall-clock stop lets
    the in-flight units finish instead of abandoning them mid-request. A fatal
    provider error raised by one worker is re-raised after the batch has
    drained, so no in-flight unit is left RUNNING without a verdict.
    """
    dispatch = dispatch or dispatch_one
    started = 0
    fatal: BaseException | None = None
    workers = max(1, int(concurrency))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = []
        for task in queue:
            if not may_start():
                break
            futures.append(pool.submit(dispatch, root, task, units))
            started += 1
        for future in concurrent.futures.as_completed(futures):
            try:
                future.result()
            except ProductionRunError as exc:
                fatal = fatal or exc
            except Exception as exc:  # noqa: BLE001 - surface, never swallow, a worker crash
                fatal = fatal or ProductionRunError(f"dispatch worker crashed: {exc!r}")
    if fatal is not None:
        raise fatal
    return started


def select_queue(
    queue: list[dict[str, Any]],
    units: dict[str, dict[str, Any]],
    evaluation: str | None,
    selected: set[str] | None,
) -> list[dict[str, Any]]:
    """The dispatchable tasks inside the run's scope: one evaluation, or named units."""
    chosen = []
    for task in queue:
        uid = str(task["work_unit_id"])
        if evaluation is not None and units[uid].get("evaluation") != evaluation:
            continue
        if selected and uid not in selected:
            continue
        chosen.append(task)
    return chosen


def scope_states(
    ledger: dict[str, Any],
    units: dict[str, dict[str, Any]],
    evaluation: str | None,
    selected: set[str] | None,
) -> set[str]:
    """Ledger statuses of every unit inside the run's scope."""
    states: set[str] = set()
    for uid, unit in units.items():
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        if selected and uid not in selected:
            continue
        states.add(str(ledger["units"].get(uid, {}).get("status", "PENDING")))
    return states


def run_production(
    root: Path,
    max_iterations: int,
    max_wall_seconds: float = 0.0,
    evaluation: str | None = None,
    concurrency: int = 1,
    units: list[str] | None = None,
) -> dict[str, Any]:
    """Drive the prepared run. Naming `units` makes it a rehearsal.

    A rehearsal dispatches exactly the named work units through the real
    image, gateway and provider - the same frozen packets and caps a full run
    would use - and stops when they are terminal, without finalizing. It is
    how a change to the paid path is tried on one packet or one agent unit for
    under a dollar before a run that would spend a hundred; what it completes
    is certified and checkpointed like any other unit, so nothing it spends is
    lost.
    """
    if evaluation is not None and evaluation not in benchmark.PRIMARY_NAMES:
        raise ProductionRunError(f"unknown Primary evaluation: {evaluation}")
    selected: set[str] = {str(u) for u in (units or []) if str(u)}

    scope_args: tuple[str, ...] = (
        ("--evaluation", evaluation) if evaluation is not None else ()
    )
    run_cli(root, "preflight")
    run_cli(root, "prepare", *scope_args)
    if selected:
        known = manifest_units(root)
        unknown = sorted(uid for uid in selected if uid not in known)
        if unknown:
            raise ProductionRunError(f"unknown work unit(s): {', '.join(unknown)}")
        not_agent = sorted(
            uid for uid in selected if known[uid].get("execution_kind", "agent") != "agent"
        )
        if not_agent:
            raise ProductionRunError(
                f"only agent work units can be rehearsed: {', '.join(not_agent)}"
            )

    started = time.monotonic()
    dispatched = 0
    log(
        f"production run: scope={evaluation or 'all'}"
        + (f" units={','.join(sorted(selected))}" if selected else "")
        + f" concurrency={max(1, int(concurrency))} "
        f"wall_budget={int(max_wall_seconds) or 'none'}s; ledger {ledger_summary(root, evaluation)}"
    )

    def wall_budget_left() -> bool:
        return max_wall_seconds <= 0 or (time.monotonic() - started) < max_wall_seconds

    def wall_checkpoint_if_due() -> None:
        if wall_budget_left():
            return
        elapsed = time.monotonic() - started
        payload = {
            "schema_version": 1,
            "ok": True,
            "reason": "wall-clock-checkpoint",
            "evaluation": evaluation or "all",
            "elapsed_seconds": round(elapsed, 3),
            "dispatched_agent_attempts": dispatched,
        }
        benchmark.json_dump(root / "results" / "production_checkpoint.json", payload)
        raise ProductionRunError(
            "production wall-clock checkpoint reached; stopping before the GitHub "
            "Actions hard timeout so COMPLETE+PASS units can be certified"
        )

    for _ in range(max_iterations):
        wall_checkpoint_if_due()
        run_cli(root, "advance", *scope_args)
        queue = json_load(root / "results" / "dispatch_queue.json").get("tasks", [])
        units = manifest_units(root)
        queue = select_queue(queue, units, evaluation, selected)
        ledger = json_load(root / "work" / "root" / "ledger.json")
        states = scope_states(ledger, units, evaluation, selected)
        if states <= {"COMPLETE", "BLOCKED", "INVALID"}:
            break
        if not queue:
            raise ProductionRunError(
                f"dispatch queue is empty while selected ledger scope is nonterminal: "
                f"{sorted(states)}"
            )

        log(f"batch of {len(queue)} ready unit(s); ledger {ledger_summary(root, evaluation)}")
        dispatched += dispatch_batch(
            root, queue, units, concurrency, may_start=wall_budget_left
        )
        wall_checkpoint_if_due()
    else:
        raise ProductionRunError(
            f"production benchmark exceeded {max_iterations} runner iterations"
        )

    run_cli(root, "advance", *scope_args)
    finalized = False
    finalization: str | None = None
    if evaluation is None and not selected:
        run_cli(root, "finalize")
        finalized = True
        finalization = str(root / "results" / "finalization.json")

    payload = {
        "schema_version": 1,
        "ok": True,
        "evaluation": evaluation or "all",
        "units": sorted(selected),
        "scope_terminal": True,
        "finalized": finalized,
        "dispatched_agent_attempts": dispatched,
        "finalization": finalization,
    }
    if selected:
        ledger = json_load(root / "work" / "root" / "ledger.json")
        payload["unit_outcomes"] = {
            uid: {
                "status": ledger["units"].get(uid, {}).get("status"),
                "validation_result": ledger["units"].get(uid, {}).get("validation_result"),
                "blocker": ledger["units"].get(uid, {}).get("blocker"),
            }
            for uid in sorted(selected)
        }
    benchmark.json_dump(root / "results" / "production_run.json", payload)
    return payload




def build_budget_plan(
    root: Path,
    model: str,
    *,
    available_usd: float | None = None,
    evaluation: str | None = None,
    selected: set[str] | None = None,
    safety_multiplier: float = 1.25,
    smoke_reserve_usd: float = 0.0,
) -> dict[str, Any]:
    """Freeze a no-provider execution plan for the currently unresolved scope.

    Besides the conservative spend bound, this classifies each leaf as already
    reused/revalidated, new paid execution, paid re-evaluation after explicit
    invalidation, deferred until dependencies make its fingerprint knowable, or
    machine-only. The plan is therefore an auditable answer to "what will be
    reused and what can still cost money?" before scored inference starts.
    """
    benchmark.assert_template_integrity(root)
    manifest = json_load(root / "work" / "root" / "manifest.json")
    ledger = json_load(root / "work" / "root" / "ledger.json")
    gateway = benchmark.gateway_config(root)
    pricing = (gateway.get("anthropic_pricing") or {}).get(model)
    if not isinstance(pricing, dict):
        raise ProductionRunError(f"no frozen pricing is configured for {model!r}")
    if safety_multiplier < 1.0:
        raise ProductionRunError("budget safety multiplier must be at least 1.0")
    if smoke_reserve_usd < 0:
        raise ProductionRunError("smoke reserve may not be negative")

    units = {str(unit["id"]): unit for unit in manifest.get("work_units", [])}
    selected = set(selected or set())
    unknown = sorted(selected - set(units))
    if unknown:
        raise ProductionRunError(
            "budget plan names unknown work units: " + ", ".join(unknown)
        )

    cache_status_path = root / "results/cache_status.json"
    cache_status = (
        json_load(cache_status_path)
        if cache_status_path.is_file()
        else {"hits": {}, "misses": {}, "invalidated": {}}
    )
    cache_hits = cache_status.get("hits", {}) or {}
    cache_misses = cache_status.get("misses", {}) or {}
    cache_invalidated = cache_status.get("invalidated", {}) or {}

    cache_impact_path = root / "results/cache_impact.json"
    cache_impact = (
        json_load(cache_impact_path)
        if cache_impact_path.is_file()
        else {"valid": 0, "invalid": []}
    )
    partial_path = root / "results/partial_paid_checkpoint_status.json"
    partial_status = (
        json_load(partial_path)
        if partial_path.is_file()
        else {"imported_units": [], "restored_paid_calls": 0}
    )
    partial_by_unit = {
        str(row.get("work_unit_id")): row
        for row in (partial_status.get("imported_units", []) or [])
        if row.get("work_unit_id")
    }

    input_price = float(pricing["input_usd_per_million_tokens"])
    output_price = float(pricing["output_usd_per_million_tokens"])
    search_price = float(pricing.get("web_search_usd_per_request", 0.0) or 0.0)
    search_uses = int(
        (gateway.get("anthropic_web_search") or {}).get(
            "max_uses_per_request", 0
        ) or 0
    )
    packet_output_ceiling = int(gateway.get("max_output_tokens_ceiling", 0) or 0)
    sandbox_cfg = benchmark.json_load(root / "template/config/sandbox_agent.json")
    worker_cfg = benchmark.worker_isolation_config(root)
    orchestration_output = int(
        worker_cfg.get("orchestration_max_output_tokens", 8192) or 8192
    )
    orchestration_turn_reserve = min(
        3, int(sandbox_cfg.get("max_turns", 3) or 3)
    )

    def task_tokens(unit: dict[str, Any]) -> int:
        agent_id = str(unit.get("assigned_agent_id") or "")
        task_path = root / "work" / "agents" / agent_id / "task.json"
        if not task_path.is_file():
            return 0
        task = json_load(task_path)
        rendered = int(task.get("rendered_bytes", 0) or 0)
        return max(0, (rendered + 3) // 4)

    decisions: dict[str, list[dict[str, Any]]] = {
        "reused_and_revalidated": [],
        "already_complete_not_from_cache": [],
        "new_paid_execution": [],
        "paid_reevaluation_after_invalidation": [],
        "deferred_cache_decision": [],
        "machine_only": [],
        "blocked": [],
        "partial_paid_resume": [],
    }
    by_eval: dict[str, dict[str, Any]] = {}
    rows: list[dict[str, Any]] = []
    blockers: list[dict[str, str]] = []
    total_upper = 0.0
    expected_calls_upper = 0
    pending_agent_units = 0
    complete_units = 0

    for uid, unit in units.items():
        if evaluation is not None and unit.get("evaluation") != evaluation:
            continue
        if selected and uid not in selected:
            continue
        ev = str(unit.get("evaluation") or "unknown")
        state = (ledger.get("units") or {}).get(uid, {}) or {}
        status = str(state.get("status") or "PENDING")
        base_decision = {
            "work_unit_id": uid,
            "evaluation": ev,
            "status": status,
            "assigned_languages": list(unit.get("assigned_languages", []) or []),
        }
        entry = by_eval.setdefault(
            ev,
            {
                "complete_units": 0,
                "pending_agent_units": 0,
                "estimated_uncached_usd": 0.0,
                "expected_paid_api_calls_upper_bound": 0,
                "hard_blockers": [],
            },
        )

        if status == "COMPLETE":
            complete_units += 1
            entry["complete_units"] += 1
            if uid in cache_hits:
                decisions["reused_and_revalidated"].append({
                    **base_decision,
                    "cache": cache_hits[uid],
                    "api_calls": 0,
                })
            else:
                decisions["already_complete_not_from_cache"].append({
                    **base_decision,
                    "api_calls": 0,
                })
            continue

        if status in {"BLOCKED", "INVALID"}:
            blocker = {
                "work_unit_id": uid,
                "status": status,
                "reason": str(state.get("blocker") or "terminal unresolved work unit"),
            }
            blockers.append(blocker)
            entry["hard_blockers"].append(blocker)
            decisions["blocked"].append({**base_decision, **blocker, "api_calls": 0})
            continue

        if unit.get("execution_kind", "agent") != "agent":
            decisions["machine_only"].append({
                **base_decision,
                "runner_action": unit.get("runner_action"),
                "api_calls": 0,
            })
            continue

        pending_agent_units += 1
        entry["pending_agent_units"] += 1
        worker_mode = str(unit.get("worker_mode") or "packet-only")
        planned_input = int(unit.get("estimated_input_tokens_per_call", 0) or 0)
        rendered_input = task_tokens(unit)
        input_tokens = max(planned_input, rendered_input, 1)
        if worker_mode == "packet-only":
            calls = 1
            declared_output = int(unit.get("max_output_tokens_per_call", 0) or 0)
            if declared_output > 0:
                output_tokens = min(declared_output, packet_output_ceiling)
            else:
                planning_default = {
                    "semantic_compression": 32768,
                    "ecosystem": 16384,
                    "language_quality": 8192,
                }.get(str(unit.get("evaluation") or ""), 16384)
                output_tokens = min(planning_default, packet_output_ceiling)
            output_tokens = max(output_tokens, 1)
        elif worker_mode == "sandbox-agent":
            restored_paid = int(
                (partial_by_unit.get(uid) or {}).get("restored_paid_calls", 0) or 0
            )
            scored_calls = max(
                0,
                int(unit.get("max_llm_calls", 0) or 0) - restored_paid,
            )
            calls = max(1, scored_calls + orchestration_turn_reserve)
            output_tokens = max(
                int(unit.get("max_output_tokens_per_call", 0) or 0),
                orchestration_output,
                1,
            )
        else:
            raise ProductionRunError(
                f"{uid}: unsupported worker mode in budget plan: {worker_mode!r}"
            )

        upper = calls * (
            input_tokens * input_price / 1_000_000.0
            + output_tokens * output_price / 1_000_000.0
            + (
                search_uses * search_price
                if bool(unit.get("network_allowed"))
                else 0.0
            )
        )
        total_upper += upper
        expected_calls_upper += calls
        entry["estimated_uncached_usd"] = round(
            float(entry["estimated_uncached_usd"]) + upper, 6
        )
        entry["expected_paid_api_calls_upper_bound"] += calls

        row = {
            **base_decision,
            "worker_mode": worker_mode,
            "restored_paid_calls": int(
                (partial_by_unit.get(uid) or {}).get("restored_paid_calls", 0) or 0
            ),
            "calls_upper_bound": calls,
            "input_tokens_per_call": input_tokens,
            "output_tokens_per_call": output_tokens,
            "network_allowed": bool(unit.get("network_allowed")),
            "estimated_uncached_usd": round(upper, 6),
        }
        rows.append(row)
        if uid in partial_by_unit:
            decisions["partial_paid_resume"].append({
                **row,
                "checkpoint": partial_by_unit[uid],
            })

        miss = cache_misses.get(uid)
        invalid = cache_invalidated.get(uid)
        if invalid is not None:
            decisions["paid_reevaluation_after_invalidation"].append({
                **row,
                "invalidation": invalid,
            })
        elif isinstance(miss, dict) and str(miss.get("reason") or "") == "no certified record":
            decisions["new_paid_execution"].append({
                **row,
                "cache_miss": miss,
            })
        elif isinstance(miss, dict):
            decisions["paid_reevaluation_after_invalidation"].append({
                **row,
                "invalidation": miss,
            })
        else:
            # A downstream fingerprint can depend on an upstream output that
            # does not exist yet. It is impossible to claim HIT/MISS honestly
            # before that dependency finishes, so budget conservatively while
            # recording that the cache decision is deferred rather than "new".
            decisions["deferred_cache_decision"].append({
                **row,
                "reason": "exact cache fingerprint waits for unresolved dependencies",
            })

    # A fully cache-satisfied run has no provider path to prove and must not
    # reserve or spend money on a smoke request. Smoke is a guard for actual
    # paid dispatch, not a tax on deterministic cache replay.
    effective_smoke_reserve = smoke_reserve_usd if expected_calls_upper > 0 else 0.0
    recommended = total_upper * safety_multiplier + effective_smoke_reserve
    available = float(available_usd) if available_usd is not None else None
    sufficient = (
        not blockers
        and (available is None or available + 1e-9 >= recommended)
    )
    return {
        "schema_version": 2,
        "kind": "benchmark_execution_plan",
        "model": model,
        "evaluation": evaluation or "all",
        "selected_units": sorted(selected),
        "complete_units": complete_units,
        "pending_agent_units": pending_agent_units,
        "cache_hits": len(cache_hits),
        "cache_misses": len(cache_misses),
        "cache_invalidated_units": len(cache_invalidated),
        "invalidated_cache_records": list(cache_impact.get("invalid", []) or []),
        "partial_paid_restored_calls": int(
            partial_status.get("restored_paid_calls", 0) or 0
        ),
        "partial_paid_resumed_units": sorted(partial_by_unit),
        "estimated_uncached_usd": round(total_upper, 6),
        "expected_paid_api_calls_upper_bound": expected_calls_upper,
        "safety_multiplier": safety_multiplier,
        "smoke_reserve_usd": round(effective_smoke_reserve, 6),
        "provider_smoke_required": expected_calls_upper > 0,
        "recommended_budget_usd": round(recommended, 6),
        "available_usd": available,
        "sufficient": sufficient,
        "hard_blockers": blockers,
        "by_evaluation": by_eval,
        "execution_decisions": decisions,
        "units": sorted(
            rows,
            key=lambda row: (
                -float(row["estimated_uncached_usd"]),
                str(row["work_unit_id"]),
            ),
        ),
        "note": (
            "Frozen pre-paid execution plan. COMPLETE certified-cache hits are "
            "revalidated and cost zero. Pending leaves with a known invalidation "
            "state the exact reason; leaves whose fingerprint depends on unfinished "
            "upstream evidence are explicitly DEFERRED and conservatively priced. "
            "Exact-fingerprint paid partial checkpoints reduce the remaining scored "
            "trial-call envelope before pricing. Prompt-cache discounts can only "
            "reduce actual provider spend further."
        ),
    }


def cmd_budget_plan(args: argparse.Namespace) -> dict[str, Any]:
    selected = set(args.units or [])
    payload = build_budget_plan(
        Path(args.workspace).resolve(),
        args.model,
        available_usd=args.available_usd,
        evaluation=args.evaluation,
        selected=selected,
        safety_multiplier=float(args.safety_multiplier),
        smoke_reserve_usd=float(args.smoke_reserve_usd),
    )
    if args.output:
        Path(args.output).write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return payload


def provider_smoke(
    model: str, template: Path, budget_usd: float, timeout: float,
    prefix_probe: Path | None = None,
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
    # Every task the smoke will actually send must be named, exactly as the
    # production policy names every agent it will dispatch. `unknown_task` is left
    # out on purpose: an unnamed task is what the refusal check needs.
    plain_task, network_task, unknown_task = (
        "smoke-plain", "smoke-network-task", "smoke-unknown-task"
    )
    policy_path.write_text(
        json.dumps({
            "schema_version": 1,
            "tasks": {plain_task: "disabled", network_task: "allowed"},
        }) + "\n",
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
    calls: list[dict[str, Any]] = []

    def record(name: str, ok: bool, detail: Any = "") -> None:
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    def record_call(label: str, response: dict[str, Any]) -> None:
        # Per call, not just the total. An aggregate cannot distinguish a fixed
        # per-request overhead from the cost of the frozen server-side tool, and
        # that distinction is what decides whether the run's budget is sound.
        calls.append({"call": label, "usage": response.get("usage", {})})

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
        gateway_cfg = json.loads(
            (template / "config" / "inference_gateway.json").read_text("utf-8")
        )
        frozen_tool = gateway_cfg.get("anthropic_web_search", {}).get("tool_type")
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
                "request_id": "smoke-unknown", "task_id": unknown_task,
                "messages": [{"role": "user", "content": "x"}],
            }),
            ("a sandbox-supplied temperature is refused", {
                "schema_version": 1, "kind": "inference.request",
                "request_id": "smoke-temp", "task_id": network_task,
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
            task_id=plain_task,
            max_output_tokens=16,
            request_id=uuid.uuid4().hex,
        )
        record_call("plain (no tools)", plain)
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

        # Packet-only workers send the frozen output ceiling as max_tokens on
        # every call, and the provider validates it against the model's own
        # limit: a ceiling one token too high fails every packet of a paid run
        # with HTTP 400 before any text. Prove the value on this key with a
        # one-word answer under the full ceiling.
        ceiling = int(gateway_cfg.get("max_output_tokens_ceiling", 0) or 0)
        ceiling_check = "the provider accepts the frozen output ceiling as max_tokens"
        try:
            capped = client.complete(
                [{"role": "user", "content": "Reply with the single word: ready"}],
                task_id=plain_task,
                max_output_tokens=ceiling,
                request_id=uuid.uuid4().hex,
            )
        except gateway_client.GatewayClientError as exc:
            record(ceiling_check, False, {"ceiling": ceiling, "error": str(exc)})
        else:
            record_call(f"plain under the frozen output ceiling ({ceiling} max_tokens)", capped)
            record(
                ceiling_check,
                isinstance(capped.get("content"), str) and capped["content"].strip() != "",
                {
                    "ceiling": ceiling,
                    "stop_reason": capped.get("stop_reason"),
                    "usage": capped.get("usage"),
                },
            )

        # The second real call exercises the network-enabled payload shape, which
        # carries the frozen server-side tool and is otherwise never sent.
        searched = client.complete(
            [{"role": "user", "content": "Reply with the single word: ready"}],
            task_id=network_task,
            max_output_tokens=16,
            network_allowed=True,
            request_id=uuid.uuid4().hex,
        )
        record_call("network-enabled (frozen web-search tool attached)", searched)
        record(
            "the network-enabled request shape is accepted by the provider",
            bool(searched.get("content")),
            searched.get("usage"),
        )

        # An orchestration-purpose turn is decoded at its own frozen depth. The
        # label has to be accepted by the provider path and recorded as such,
        # because every sandbox-agent action turn in the run will carry it.
        orchestration = client.complete(
            [{"role": "user", "content": "Reply with the single word: ready"}],
            task_id=plain_task,
            max_output_tokens=16,
            purpose="orchestration",
            request_id=uuid.uuid4().hex,
        )
        record_call("orchestration purpose (frozen shallow depth)", orchestration)
        record(
            "an orchestration-purpose request is accepted by the provider",
            isinstance(orchestration.get("content"), str)
            and orchestration["content"].strip() != "",
            orchestration.get("content", "")[:120],
        )

        # Prompt caching is what keeps a growing sandbox-agent conversation from
        # re-buying its whole prefix every turn. It is silent when it does not
        # work, so prove it on this key: the same request twice, and the second
        # must report a cache read. The prefix has to clear the model's minimum
        # cacheable size, hence the filler.
        filler = "\n".join(
            f"Line {index:04d}: the quick brown fox jumps over the lazy dog."
            for index in range(220)
        )
        cached_messages = [
            {"role": "system", "content": "You answer with one word.\n\n" + filler},
            {"role": "user", "content": "Reply with the single word: ready"},
        ]
        first = client.complete(
            cached_messages, task_id=plain_task, max_output_tokens=16,
            request_id=uuid.uuid4().hex,
        )
        second = client.complete(
            cached_messages, task_id=plain_task, max_output_tokens=16,
            request_id=uuid.uuid4().hex,
        )
        record_call("cache probe, first send", first)
        record_call("cache probe, identical second send", second)
        record(
            "the provider reports cache activity fields",
            "cache_read_input_tokens" in (first.get("usage") or {}),
            first.get("usage"),
        )
        record(
            "an identical second request is served from the prompt cache",
            int((second.get("usage") or {}).get("cache_read_input_tokens", 0) or 0) > 0,
            {"first": first.get("usage"), "second": second.get("usage")},
        )

        if prefix_probe is not None:
            # A packet in the shared-inputs-first layout is one user message:
            # the inputs every sibling unit shares, then a per-unit header. The
            # trusted adapter splits it at the header and marks the shared part
            # as the cache breakpoint. Two packets that share the inputs but
            # differ in their header must therefore write the prefix once and
            # read it the second time. This is the only way to know before a
            # paid run whether that saving is real; it costs one write of the
            # probe text plus one read, well under a dollar.
            shared = prefix_probe.read_text(encoding="utf-8")
            tail = "\n\n## Rules\n- Reply with the single word: ready\n"
            probe_a = client.complete(
                [{"role": "user", "content": shared + "\n# Task Packet: smoke-probe-a\n" + tail}],
                task_id=plain_task, max_output_tokens=16, request_id=uuid.uuid4().hex,
            )
            probe_b = client.complete(
                [{"role": "user", "content": shared + "\n# Task Packet: smoke-probe-b\n" + tail}],
                task_id=plain_task, max_output_tokens=16, request_id=uuid.uuid4().hex,
            )
            record_call("prefix probe, first packet (writes the shared inputs)", probe_a)
            record_call("prefix probe, sibling packet (should read them)", probe_b)
            written = int((probe_a.get("usage") or {}).get("cache_creation_input_tokens", 0) or 0)
            read = int((probe_b.get("usage") or {}).get("cache_read_input_tokens", 0) or 0)
            record(
                "a sibling packet reads the shared inputs from the prompt cache",
                written > 0 and read >= int(0.8 * written),
                {"first": probe_a.get("usage"), "second": probe_b.get("usage"),
                 "probe_text": str(prefix_probe)},
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
        "calls": calls,
        "spend": cost,
    }
    if len(calls) >= 2:
        plain_in = int(calls[0]["usage"].get("input_tokens", 0) or 0)
        tooled_in = int(calls[1]["usage"].get("input_tokens", 0) or 0)
        payload["input_token_breakdown"] = {
            "plain_request_input_tokens": plain_in,
            "network_request_input_tokens": tooled_in,
            "attributable_to_frozen_web_search_tool": tooled_in - plain_in,
            "note": (
                "The plain figure is the provider's floor for a near-empty prompt; "
                "the difference is what attaching the frozen server-side tool costs. "
                "Only the first applies to every scored request."
            ),
        }
    if failures:
        payload["failed_checks"] = failures
    return payload


def build_cost_report(log_path: Path) -> dict[str, Any]:
    totals = {
        "input_tokens": 0,
        "cache_creation_input_tokens": 0,
        "cache_read_input_tokens": 0,
        "output_tokens": 0,
        "web_search_requests": 0,
    }
    calls = 0
    empty = 0
    estimated_cost = 0.0
    errors: dict[str, int] = {}
    by_task: dict[str, dict[str, Any]] = {}
    if log_path.is_file():
        for line in log_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            record = json.loads(line)
            event = record.get("event")
            if event in {"provider_error", "policy_refusal", "protocol_error"}:
                errors[event] = errors.get(event, 0) + 1
                continue
            if event != "inference":
                continue
            calls += 1
            usage = record.get("usage", {}) or {}
            for key in totals:
                totals[key] += int(usage.get(key, 0) or 0)
            cost = float(usage.get("estimated_cost_usd", 0.0) or 0.0)
            estimated_cost += cost
            is_empty = int(record.get("response_chars", 1) or 0) == 0
            empty += is_empty
            task = str(record.get("task_id") or "")
            entry = by_task.setdefault(task, {"calls": 0, "empty_completions": 0,
                                              "estimated_cost_usd": 0.0})
            entry["calls"] += 1
            entry["empty_completions"] += is_empty
            entry["estimated_cost_usd"] = round(entry["estimated_cost_usd"] + cost, 6)
    return {
        "schema_version": 1,
        "paid_inference_calls": calls,
        **totals,
        "empty_completions": empty,
        "estimated_cost_usd": round(estimated_cost, 6),
        "gateway_errors": errors,
        # Sorted by spend so the first lines answer where the money went.
        "by_task": dict(sorted(
            by_task.items(), key=lambda kv: -kv[1]["estimated_cost_usd"]
        )),
        "note": (
            "Estimated from the frozen per-token, cache and web-search prices in the "
            "benchmark template. input_tokens is the uncached remainder; the cached "
            "prefix is reported and priced separately."
        ),
    }



def reconcile_execution_plan(
    root: Path,
    plan_path: Path,
    actual_path: Path,
    evaluation: str,
) -> dict[str, Any]:
    """Compare the frozen pre-paid plan with what the provider actually did."""
    plan = json_load(plan_path)
    actual = json_load(actual_path)
    ledger = json_load(root / "work" / "root" / "ledger.json")
    manifest = json_load(root / "work" / "root" / "manifest.json")
    eval_ids = {
        str(unit["id"])
        for unit in manifest.get("work_units", [])
        if str(unit.get("evaluation") or "") == evaluation
    }

    planned_calls = int(plan.get("expected_paid_api_calls_upper_bound", 0) or 0)
    actual_calls = int(actual.get("paid_inference_calls", 0) or 0)
    planned_cost = float(plan.get("estimated_uncached_usd", 0.0) or 0.0)
    actual_cost = float(actual.get("estimated_cost_usd", 0.0) or 0.0)

    retry_units = []
    final_status_counts: dict[str, int] = {}
    for uid in sorted(eval_ids):
        state = (ledger.get("units", {}).get(uid) or {})
        status = str(state.get("status") or "PENDING")
        final_status_counts[status] = final_status_counts.get(status, 0) + 1
        if int(state.get("attempts", 0) or 0) > 1:
            retry_units.append({
                "work_unit_id": uid,
                "attempts": int(state.get("attempts", 0) or 0),
            })

    cache_status_path = root / "results/cache_status.json"
    cache_status = (
        json_load(cache_status_path)
        if cache_status_path.is_file()
        else {"hits": {}, "misses": {}, "invalidated": {}}
    )
    final_hits = sorted(
        uid for uid in (cache_status.get("hits", {}) or {}) if uid in eval_ids
    )

    reasons: list[str] = []
    if actual_calls < planned_calls:
        reasons.append(
            "actual calls are below the conservative upper bound because unused "
            "repair/orchestration envelopes and dependency-unlocked cache hits are "
            "not charged"
        )
    elif actual_calls > planned_calls:
        reasons.append(
            "actual calls exceeded the frozen upper-bound estimate; inspect retries "
            "and gateway audit before accepting the run plan as accurate"
        )
    if retry_units:
        reasons.append("one or more work units required runner retries")
    errors = actual.get("gateway_errors") or {}
    if any(int(v or 0) for v in errors.values()):
        reasons.append("the provider/gateway audit recorded retryable or refused calls")
    if not reasons:
        reasons.append("actual dispatch matched the frozen plan without a material deviation")

    return {
        "schema_version": 1,
        "kind": "benchmark_execution_actual_vs_plan",
        "evaluation": evaluation,
        "planned_paid_api_calls_upper_bound": planned_calls,
        "actual_paid_api_calls": actual_calls,
        "paid_api_call_delta_actual_minus_upper_bound": actual_calls - planned_calls,
        "within_planned_call_upper_bound": actual_calls <= planned_calls,
        "planned_uncached_usd_upper_estimate": round(planned_cost, 6),
        "actual_estimated_cost_usd": round(actual_cost, 6),
        "cost_delta_actual_minus_plan": round(actual_cost - planned_cost, 6),
        "final_ledger_status_counts": final_status_counts,
        "final_cache_hit_units": final_hits,
        "retry_units": retry_units,
        "gateway_errors": errors,
        "difference_reasons": reasons,
    }


def cmd_reconcile_plan(args: argparse.Namespace) -> dict[str, Any]:
    root = Path(args.workspace).resolve()
    payload = reconcile_execution_plan(
        root,
        Path(args.plan).resolve(),
        Path(args.actual).resolve(),
        args.evaluation,
    )
    if args.output:
        Path(args.output).write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return payload


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    policy = sub.add_parser("policy", help="freeze trusted per-task network policy")
    policy.add_argument("--workspace", required=True)
    policy.add_argument("--output", required=True)
    policy.add_argument(
        "--model",
        help="model whose frozen prices size the per-task spend ceilings; without it "
             "every ceiling is the configured floor",
    )
    policy.add_argument("--evaluation", choices=benchmark.PRIMARY_NAMES)
    policy.add_argument(
        "--unit", action="append", dest="units", default=None,
        help="name a work unit to rehearse (repeatable); every other agent is refused",
    )

    run = sub.add_parser("run", help="drive the prepared scored run to finalization")
    run.add_argument("--workspace", default="/quidra-benchmark")
    run.add_argument("--max-iterations", type=int, default=200)
    run.add_argument("--evaluation", choices=benchmark.PRIMARY_NAMES)
    run.add_argument(
        "--unit", action="append", dest="units", default=None,
        help="rehearse exactly this work unit through the real provider (repeatable); "
             "the run stops when the named units are terminal and does not finalize",
    )
    run.add_argument(
        "--max-wall-seconds",
        type=float,
        default=0.0,
        help="stop cleanly before dispatching another unit after this wall-clock budget",
    )
    run.add_argument(
        "--concurrency",
        type=int,
        default=1,
        help="dependency-ready work units to keep in flight at once (each is one "
             "worker process and at most one provider request at a time)",
    )

    budget = sub.add_parser(
        "budget-plan",
        help="estimate unresolved paid work conservatively without calling a provider",
    )
    budget.add_argument("--workspace", required=True)
    budget.add_argument("--model", required=True)
    budget.add_argument("--evaluation", choices=benchmark.PRIMARY_NAMES)
    budget.add_argument("--unit", action="append", dest="units", default=None)
    budget.add_argument("--available-usd", type=float)
    budget.add_argument("--safety-multiplier", type=float, default=1.25)
    budget.add_argument("--smoke-reserve-usd", type=float, default=0.0)
    budget.add_argument("--output")

    smoke = sub.add_parser(
        "provider-smoke",
        help="spend a few cents proving the paid request shape works before the run",
    )
    smoke.add_argument("--model", required=True)
    smoke.add_argument("--template", default=str(SCRIPTS_DIR.parent))
    smoke.add_argument("--budget-usd", type=float, default=0.50)
    smoke.add_argument("--timeout", type=float, default=180.0)
    smoke.add_argument("--output")
    smoke.add_argument(
        "--prefix-probe-text",
        help="a large text file to send twice under different Task Packet headers, "
             "proving the shared-inputs-first cache breakpoint on the live provider",
    )

    cost = sub.add_parser("cost-report", help="summarize the trusted gateway audit log")
    cost.add_argument("--log", required=True)
    cost.add_argument("--output")

    reconcile = sub.add_parser(
        "reconcile-plan",
        help="persist the difference between the frozen execution plan and actual paid calls",
    )
    reconcile.add_argument("--workspace", required=True)
    reconcile.add_argument("--plan", required=True)
    reconcile.add_argument("--actual", required=True)
    reconcile.add_argument("--evaluation", required=True, choices=benchmark.PRIMARY_NAMES)
    reconcile.add_argument("--output")

    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "policy":
        payload = write_policy(
            Path(args.workspace).resolve(),
            Path(args.output).resolve(),
            args.model,
            args.evaluation,
            args.units,
        )
    elif args.command == "budget-plan":
        payload = cmd_budget_plan(args)
    elif args.command == "reconcile-plan":
        payload = cmd_reconcile_plan(args)
    elif args.command == "provider-smoke":
        payload = provider_smoke(
            args.model, Path(args.template).resolve(),
            float(args.budget_usd), float(args.timeout),
            Path(args.prefix_probe_text).resolve() if args.prefix_probe_text else None,
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
        payload = run_production(
            Path(args.workspace).resolve(),
            int(args.max_iterations),
            float(args.max_wall_seconds),
            args.evaluation,
            int(args.concurrency),
            args.units,
        )
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
