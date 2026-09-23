#!/usr/bin/env python3
"""Drive a complete synthetic benchmark run through the real isolation plumbing.

This is the harness CI uses to prove the runtime actually works end to end. It is
not a scoring tool and produces no meaningful measurements: every model turn comes
from the gateway's deterministic fake provider. What it does prove is that the
paths a real run depends on are wired correctly -

  * packet-only units reach a model only through `benchmark.py task-infer`, which
    talks to the credential-less gateway socket and hands the reply to the same
    importer `task-apply` uses;
  * sandbox-agent units run through `scripts/sandbox_agent.py`, whose read, write
    and exec limits are enforced in code;
  * every worker subprocess is started with a stripped environment, so a run that
    only succeeds because a credential happened to be exported fails here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
CLI = SCRIPTS_DIR / "benchmark.py"
GATEWAY = SCRIPTS_DIR / "inference_gateway.py"
AGENT = SCRIPTS_DIR / "sandbox_agent.py"

CREDENTIAL_ENV_NAMES = json.loads(
    (SCRIPTS_DIR.parent / "config" / "inference_gateway.json").read_text(encoding="utf-8")
)["sandbox_forbidden_credential_environment_names"]

ATTESTATIONS = {
    "QUIDRA_BENCHMARK_WORKER_GATEWAY_ATTESTED": "packet-gateway-v1",
    "QUIDRA_BENCHMARK_PACKET_WORKER_LOCAL_TOOLS": "disabled",
    "QUIDRA_BENCHMARK_SANDBOX_AGENT_LAUNCHER_ATTESTED": "inside-sandbox-v1",
}


class RunError(RuntimeError):
    pass


def worker_env(socket_path: Path) -> dict[str, str]:
    """What a scored worker process is allowed to see."""
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": "/nonexistent-worker-home",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "TZ": "UTC",
        "PYTHONDONTWRITEBYTECODE": "1",
        "QUIDRA_BENCHMARK_INFERENCE_SOCKET": str(socket_path),
    }
    for name in CREDENTIAL_ENV_NAMES:
        env.pop(name, None)
    return env


def runner_env() -> dict[str, str]:
    return {**os.environ, **ATTESTATIONS}


def run_cli(root: Path, *args: str) -> None:
    completed = subprocess.run(
        [sys.executable, str(CLI), *args, "--workspace", str(root)],
        env=runner_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise RunError(
            f"benchmark.py {' '.join(args)} failed: {completed.stderr or completed.stdout}"
        )


def gateway_side_secret() -> str:
    return "sk-" + uuid.uuid4().hex + uuid.uuid4().hex


def write_task_policy(root: Path, destination: Path) -> Path:
    """Derive each unit's network ceiling from the frozen manifest.

    This has to happen on the trusted side. The gateway must never take a
    worker's word for how much network its own task is allowed, or the ceiling
    would be set by the thing it constrains.
    """
    manifest = json.loads(
        (root / "work" / "root" / "manifest.json").read_text(encoding="utf-8")
    )
    tasks = {}
    for unit in manifest.get("work_units", []):
        agent_id = unit.get("assigned_agent_id")
        if not agent_id:
            continue
        task_path = root / "work" / "agents" / agent_id / "task.json"
        allowed = bool(unit.get("network_allowed"))
        if task_path.is_file():
            allowed = bool(json.loads(task_path.read_text(encoding="utf-8")).get(
                "network_allowed"
            ))
        tasks[agent_id] = "allowed" if allowed else "disabled"
    destination.write_text(
        json.dumps({"schema_version": 1, "tasks": tasks}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return destination


def start_gateway(
    socket_path: Path, script_path: Path, log_path: Path, task_policy: Path
) -> subprocess.Popen:
    script_path.write_text(json.dumps({"schema_version": 1}) + "\n", encoding="utf-8")
    process = subprocess.Popen(
        [
            sys.executable, str(GATEWAY), "serve",
            "--socket", str(socket_path),
            "--template", str(SCRIPTS_DIR.parent),
            "--provider", "fake",
            "--fake-script", str(script_path),
            "--network-policy", "disabled",
            "--task-policy", str(task_policy),
            "--log", str(log_path),
        ],
        # The gateway is the trusted side: it is the only process in this harness
        # that would legitimately hold a provider credential. The value is minted
        # at runtime so this file never itself contains a credential-shaped
        # literal - the privacy gate scans the template and would rightly
        # object to one.
        env={**os.environ, "ANTHROPIC_API_KEY": gateway_side_secret()},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            out, err = process.communicate(timeout=5)
            raise RunError(f"inference gateway did not start: {err or out}")
        if socket_path.is_socket():
            return process
        time.sleep(0.05)
    process.kill()
    raise RunError("inference gateway did not bind its socket in time")


def synthetic_canonical_catalog(root: Path) -> dict[str, dict[str, Any]]:
    """A complete fake catalog matching the real canonical-fragment contract."""
    matrix = json.loads(
        (
            root
            / "template/methodology-assets/semantic_compression/semantic_site_matrix.json"
        ).read_text(encoding="utf-8")
    )
    return {
        str(probe["probe_id"]): {
            "level": "FULL",
            "fragment": "synthetic_fragment()",
            "partial_reasons": [],
            "none_reason": None,
            "citation": "Synthetic frozen documentation citation.",
            "justification": "Synthetic canonical fragment selected under R1-R10.",
        }
        for probe in matrix["probes"]
    }



def synthetic_canonical_verification(
    root: Path, catalog: dict[str, dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    """Structurally exercise V1 without pretending CI compiled fake fragments."""
    return {
        probe_id: {
            "entry_file": "main.txt",
            "fragment_files": ["main.txt"],
            "files": {
                "main.txt": (
                    "synthetic fixture prefix\n"
                    + str(record["fragment"])
                    + "\nsynthetic fixture suffix\n"
                )
            },
            "mode": "nm-add2" if probe_id == "F20.P2" else "run",
            "run_count": 0 if probe_id == "F20.P2" else (20 if probe_id == "F19.P2" else 1),
        }
        for probe_id, record in catalog.items()
        if str(record.get("level")).upper() in {"FULL", "PARTIAL"}
    }


def synthetic_coverage_score(
    root: Path, catalog: dict[str, dict[str, Any]]
) -> float:
    """Derive Capability Coverage exactly as the real owner validator does."""
    aggregation = json.loads(
        (root / "template/config/aggregation.json").read_text(encoding="utf-8")
    )
    owner = (
        aggregation.get("evaluations", {}).get("semantic_compression", {})
        .get("support_level_owner") or {}
    )
    factors = {
        str(name).upper(): float(value)
        for name, value in (owner.get("levels") or {}).items()
    }
    matrix = json.loads(
        (
            root
            / "template/methodology-assets/semantic_compression/semantic_site_matrix.json"
        ).read_text(encoding="utf-8")
    )
    total = 0.0
    awarded = 0.0
    for probe in matrix.get("probes", []):
        probe_id = str(probe["probe_id"])
        points = float(probe.get("capability_denominator") or 0)
        total += points
        awarded += points * factors.get(str(catalog[probe_id]["level"]).upper(), 0.0)
    if total <= 0:
        raise RunError("synthetic canonical catalog has no capability denominator")
    return round(100.0 * awarded / total, 6)


def synthetic_catalog_digest(
    root: Path,
    unit: dict[str, Any],
    units: dict[str, dict[str, Any]],
) -> str | None:
    """Reproduce canonical_fragment_input_for_unit's content hash for fake results."""
    source_requirement = str(
        unit.get("canonical_fragment_source_requirement") or ""
    )
    assigned = list(unit.get("assigned_languages") or [])
    if not source_requirement:
        return None
    if len(assigned) != 1:
        raise RunError(
            f"{unit.get('id')}: synthetic canonical consumer is not single-language"
        )
    language = str(assigned[0])
    candidates = [
        source
        for source in units.values()
        if source_requirement in (source.get("requirement_ids") or [])
        and list(source.get("assigned_languages") or []) == [language]
    ]
    if len(candidates) != 1:
        raise RunError(
            f"{unit.get('id')}: synthetic catalog owner count is {len(candidates)}"
        )
    source = candidates[0]
    payload = {
        "schema_version": 1,
        "language": language,
        "source_work_unit_id": source.get("id"),
        "source_requirement_id": source_requirement,
        "rule": (
            "Use exactly these fragments for every downstream Semantic "
            "Compression metric. FULL/PARTIAL entries must be measured verbatim; "
            "NONE entries have no fragment and must not receive a numeric "
            "per-probe A/B/C/D measurement."
        ),
        "canonical_fragments": synthetic_canonical_catalog(root),
    }
    encoded = (
        json.dumps(payload, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def unit_payload(
    root: Path,
    unit: dict[str, Any],
    languages: list[str],
    units: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    if unit.get("result_kind") == "audit":
        return {
            "schema_version": 1,
            "evaluation": unit["evaluation"],
            "audit_pass": True,
            "evidence": {"synthetic": "deterministic harness audit"},
        }
    assigned = unit.get("assigned_languages") or languages
    requirements: dict[str, Any] = {}
    evidence: dict[str, Any] = {"synthetic": "deterministic harness evidence"}
    for requirement_id in unit.get("requirement_ids", []):
        if requirement_id.startswith("gate.") or requirement_id.startswith("coverage."):
            requirements[requirement_id] = True
        elif requirement_id.startswith("annotation.support_adjudication--"):
            # A support adjudication answers for the whole cohort with the full
            # canonical record consumed by comparability.
            requirements[requirement_id] = {
                language: {
                    "level": "FULL",
                    "fragment": "synthetic_fragment()",
                    "partial_reasons": [],
                    "none_reason": None,
                    "citation": "Synthetic frozen documentation citation.",
                    "justification": "Synthetic cohort-consistent justification.",
                }
                for language in languages
            }
        else:
            requirements[requirement_id] = {
                language: float(90 - languages.index(language)) for language in assigned
            }
        if (
            unit.get("evaluation") == "semantic_compression"
            and requirement_id == "metric.capability_coverage"
        ):
            catalog = synthetic_canonical_catalog(root)
            requirements[requirement_id] = {
                language: synthetic_coverage_score(root, catalog)
                for language in assigned
            }
            evidence["canonical_fragments"] = catalog
            evidence["canonical_verification"] = synthetic_canonical_verification(
                root, catalog
            )

    digest = synthetic_catalog_digest(root, unit, units)
    if digest is not None:
        evidence["canonical_fragment_catalog_sha256"] = digest

    return {
        "schema_version": 1,
        "evaluation": unit["evaluation"],
        "requirements": requirements,
        "evidence": evidence,
    }


TOOLCHAIN_VERSION_PROBES = {
    "Python": ("python3", "--version"),
    "C++": ("c++", "--version"),
    "Rust": ("rustc", "--version"),
    "Go": ("go", "version"),
    "Java": ("javac", "-version"),
    "Kotlin": ("kotlinc", "-version"),
    "TypeScript": ("tsc", "--version"),
    "Swift": ("swift", "--version"),
    "Zig": ("zig", "version"),
    "Quidra": ("quidra", "--version"),
}


def sandbox_turns(unit: dict[str, Any], payload: str) -> list[str]:
    turns: list[str] = []
    if unit.get("evaluation") == "llm_learnability" and int(unit.get("max_llm_calls", 0) or 0) > 0:
        preflight = {
            "schema_version": 1,
            "passed": True,
            "fixtures_compile_and_run": True,
            "harness_conventions_satisfied": True,
            "validator_positive_control_passed": True,
            "validator_negative_control_passed": True,
            "evidence": ["synthetic preflight control"],
        }
        leakage = {
            "schema_version": 1,
            "passed": True,
            "exact_solution_absent": True,
            "expected_output_not_leaked": True,
            "isomorphic_example_absent": True,
            "withheld_mapping_absent": True,
            "validator_answer_absent": True,
            "metadata_leak_absent": True,
            "planted_leak_positive_control_passed": True,
            "evidence": ["synthetic planted-leak control"],
        }
        turns.extend([
            json.dumps({"action": "write_file", "path": "learnability_preflight.json",
                        "content": json.dumps(preflight, indent=2) + "\n"}),
            json.dumps({"action": "write_file", "path": "learnability_leakage.json",
                        "content": json.dumps(leakage, indent=2) + "\n"}),
        ])
    if unit.get("evaluation") == "llm_learnability" and int(unit.get("max_llm_calls", 0) or 0) > 0:
        # A trial unit must show its assigned toolchain actually ran: a real
        # agent compiles fixtures, the scripted one asks for the version. In
        # the sandbox this resolves through the same PATH a real agent gets
        # (the Quidra build included); on a host without the toolchain it is
        # denied, and the synthetic escape hatch waives the check there.
        for language in unit.get("assigned_languages") or []:
            probe = TOOLCHAIN_VERSION_PROBES.get(str(language))
            if probe:
                turns.append(json.dumps({"action": "run", "argv": list(probe)}))
    if int(unit.get("max_llm_calls", 0) or 0) > 0:
        turns.extend([
            json.dumps({"action": "trial_start", "trial_id": "synthetic-t1",
                        "prompt": "Return a short synthetic benchmark completion."}),
            "synthetic trial completion",
        ])
    turns.extend([
        json.dumps({"action": "write_file", "path": "result.json", "content": payload}),
        json.dumps({"action": "final", "summary": "wrote result.json"}),
    ])
    return turns


def script_for_queue(
    root: Path,
    queue: list[dict[str, Any]],
    units: dict[str, Any],
    languages: list[str],
) -> dict[str, Any]:
    """Turn-by-turn fake completions for every unit about to be dispatched."""
    tasks: dict[str, list[str]] = {}
    for task in queue:
        unit = units[task["work_unit_id"]]
        agent_id = task["agent_id"]
        payload = json.dumps(
            unit_payload(root, unit, languages, units), indent=2
        ) + "\n"
        if task["worker_mode"] == "packet-only":
            tasks[agent_id] = [json.dumps({
                "schema_version": 1,
                "task_id": agent_id,
                "files": [{"path": "result.json", "content": payload}],
            })]
        else:
            tasks[agent_id] = sandbox_turns(unit, payload)
    return {"schema_version": 1, "tasks": tasks}


def emit_fake_script(root: Path, output: Path) -> dict[str, Any]:
    """Script every unit in the frozen manifest up front, in one file.

    The per-queue scripting this harness normally does needs a driver that hands
    control back between batches. production_run.py does not: it owns its own
    loop, exactly as it will in a paid run. Writing the whole script in advance
    lets that driver be exercised end to end against a provider that costs
    nothing, which is otherwise the largest part of a run that never executes
    until the run that costs money.
    """
    languages = json.loads(
        (root / "template" / "config" / "benchmark_metadata.json").read_text(encoding="utf-8")
    )["languages"]
    manifest = json.loads(
        (root / "work" / "root" / "manifest.json").read_text(encoding="utf-8")
    )
    units = {str(unit["id"]): unit for unit in manifest.get("work_units", [])}
    tasks: dict[str, list[str]] = {}
    for unit in manifest.get("work_units", []):
        if unit.get("execution_kind", "agent") != "agent":
            continue
        agent_id = str(unit["assigned_agent_id"])
        payload = json.dumps(
            unit_payload(root, unit, languages, units), indent=2
        ) + "\n"
        if unit.get("worker_mode", "packet-only") == "packet-only":
            turns = [json.dumps({
                "schema_version": 1,
                "task_id": agent_id,
                "files": [{"path": "result.json", "content": payload}],
            })]
        else:
            turns = sandbox_turns(unit, payload)
        # A retry re-dispatches the same unit, so repeat each script enough times
        # that a retried unit is answered rather than falling through.
        tasks[agent_id] = turns * 4
    script = {"schema_version": 1, "tasks": tasks}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(script, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {"scripted_agents": len(tasks), "output": str(output)}


def dispatch(root: Path, task: dict[str, Any], socket_path: Path) -> None:
    agent_id = task["agent_id"]
    run_cli(root, "task-start", "--id", task["work_unit_id"])
    if task["worker_mode"] == "packet-only":
        argv = [
            sys.executable, str(CLI), "task-infer",
            "--workspace", str(root), "--id", agent_id,
            "--socket", str(socket_path),
        ]
    else:
        argv = [
            sys.executable, str(AGENT),
            "--workspace", str(root), "--id", agent_id,
            "--socket", str(socket_path),
        ]
    completed = subprocess.run(
        argv,
        env=worker_env(socket_path),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise RunError(
            f"{task['worker_mode']} worker {agent_id} failed: "
            f"{completed.stderr or completed.stdout}"
        )
    run_cli(root, "task-finish", "--id", task["work_unit_id"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", required=True)
    parser.add_argument(
        "--socket",
        help="gateway socket path; defaults to <workspace>/gateway/inference.sock. "
             "Override it when the workspace sits under a directory long enough to "
             "exceed the AF_UNIX path limit.",
    )
    parser.add_argument("--max-iterations", type=int, default=100)
    parser.add_argument(
        "--emit-fake-script",
        help="write a complete deterministic script for the frozen manifest and exit, "
             "for driving production_run.py without a paid provider",
    )
    args = parser.parse_args()

    root = Path(args.workspace).resolve()
    if args.emit_fake_script:
        print(json.dumps(emit_fake_script(root, Path(args.emit_fake_script)), indent=2))
        return 0
    languages = json.loads(
        (root / "template" / "config" / "benchmark_metadata.json").read_text(encoding="utf-8")
    )["languages"]

    socket_path = Path(args.socket) if args.socket else root / "gateway" / "inference.sock"
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    script_path = socket_path.parent / "fake_script.json"
    log_path = socket_path.parent / "audit.jsonl"

    # Task Packets are already frozen at this point, so their network ceilings can
    # be derived once and handed to the gateway before any worker starts.
    run_cli(root, "advance")
    task_policy = write_task_policy(root, socket_path.parent / "task_policy.json")
    gateway = start_gateway(socket_path, script_path, log_path, task_policy)
    counts = {"packet-only": 0, "sandbox-agent": 0}
    try:
        for _ in range(args.max_iterations):
            run_cli(root, "advance")
            queue = json.loads(
                (root / "results" / "dispatch_queue.json").read_text(encoding="utf-8")
            )["tasks"]
            if not queue:
                ledger = json.loads(
                    (root / "work" / "root" / "ledger.json").read_text(encoding="utf-8")
                )
                states = {unit["status"] for unit in ledger["units"].values()}
                if states <= {"COMPLETE", "BLOCKED", "INVALID"}:
                    break
                raise RunError(f"empty dispatch queue with nonterminal states: {states}")

            manifest = json.loads(
                (root / "work" / "root" / "manifest.json").read_text(encoding="utf-8")
            )
            units = {unit["id"]: unit for unit in manifest["work_units"]}
            script_path.write_text(
                json.dumps(
                    script_for_queue(root, queue, units, languages), indent=2
                ) + "\n",
                encoding="utf-8",
            )
            for task in queue:
                dispatch(root, task, socket_path)
                counts[task["worker_mode"]] += 1
        else:
            raise RunError("the synthetic run did not terminate")

        run_cli(root, "advance")
    finally:
        gateway.terminate()
        try:
            gateway.wait(timeout=10)
        except subprocess.TimeoutExpired:
            gateway.kill()

    audit_lines = (
        log_path.read_text(encoding="utf-8").splitlines() if log_path.is_file() else []
    )
    summary = {
        "ok": True,
        "dispatched": counts,
        "gateway_requests_brokered": len(audit_lines),
    }
    print(json.dumps(summary, indent=2))
    if not counts["packet-only"] or not counts["sandbox-agent"]:
        raise RunError(f"the run did not exercise both worker modes: {counts}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunError as exc:
        print(f"synthetic run error: {exc}", file=sys.stderr)
        raise SystemExit(2)
