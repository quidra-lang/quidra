#!/usr/bin/env python3
"""Template-maintenance check: every restatement of the benchmark identity facts agrees.

`config/benchmark_metadata.json` owns the five Primary evaluation IDs, their display
names and the fixed evaluated-language set. Everything else in the template either
derives from that file or is checked here against it. Exits non-zero on any drift.
"""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import re
import sys
from typing import Any

TEMPLATE = Path(__file__).resolve().parent.parent
METADATA_PATH = TEMPLATE / "config" / "benchmark_metadata.json"
MASTER_PROMPT = TEMPLATE.parent / "master_prompt.md"


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def load_module(path: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def numbered_list(text: str, heading: str) -> list[str]:
    """Ordered entries of the numbered list under `heading`, up to the next heading."""
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == heading)
    except StopIteration:
        return []
    items: list[str] = []
    for line in lines[start + 1:]:
        if line.startswith("#"):
            break
        match = re.match(r"^\s*(\d+)\.\s+(.+?)\s*$", line)
        if match:
            index = int(match.group(1))
            if index != len(items) + 1:
                return []
            items.append(match.group(2))
        elif items and line.strip():
            break
    return items


def check() -> list[str]:
    problems: list[str] = []

    def fail(message: str) -> None:
        problems.append(message)

    metadata = read_json(METADATA_PATH)
    if metadata.get("schema_version") != 1:
        fail("benchmark_metadata.json: unsupported schema_version")
        return problems

    evaluations = metadata["evaluations"]
    ids = [entry["id"] for entry in evaluations]
    display_names = [entry["display_name"] for entry in evaluations]
    languages = list(metadata["languages"])
    target = metadata["evaluated_target_language"]

    if len(set(ids)) != len(ids):
        fail("benchmark_metadata.json: duplicate evaluation ids")
    if len(set(display_names)) != len(display_names):
        fail("benchmark_metadata.json: duplicate evaluation display names")
    if len(set(languages)) != len(languages):
        fail("benchmark_metadata.json: duplicate languages")
    if target not in languages:
        fail(f"benchmark_metadata.json: evaluated_target_language {target!r} is not in languages")
    for entry in evaluations:
        if not re.fullmatch(r"[a-z][a-z0-9_]*", entry["id"]):
            fail(f"benchmark_metadata.json: evaluation id is not a snake_case slug: {entry['id']!r}")
    if problems:
        return problems

    comparison_languages = [lang for lang in languages if lang != target]

    primary = read_json(TEMPLATE / "config" / "primary.json")
    for dead_key in ("languages", "primary_evaluations"):
        if dead_key in primary:
            fail(
                f"config/primary.json: {dead_key!r} is owned by config/benchmark_metadata.json "
                "and must not be restated here"
            )
    reusable = primary["language_quality"]["comparison_languages_reusable_by_default"]
    if list(reusable) != comparison_languages:
        fail(
            "config/primary.json: language_quality.comparison_languages_reusable_by_default "
            f"must be {comparison_languages}, got {list(reusable)}"
        )

    aggregation = read_json(TEMPLATE / "config" / "aggregation.json")
    if "languages" in aggregation:
        fail(
            "config/aggregation.json: 'languages' is owned by config/benchmark_metadata.json "
            "and must not be restated here"
        )
    if list(aggregation["evaluations"]) != ids:
        fail(
            "config/aggregation.json: evaluations must be exactly "
            f"{ids} in order, got {list(aggregation['evaluations'])}"
        )

    requirements = read_json(TEMPLATE / "config" / "evaluation_requirements.json")
    if list(requirements["evaluations"]) != ids:
        fail(
            "config/evaluation_requirements.json: evaluations must be exactly "
            f"{ids} in order, got {list(requirements['evaluations'])}"
        )

    work_plans = read_json(TEMPLATE / "config" / "work_plan_templates.json")
    if list(work_plans["evaluations"]) != ids:
        fail(
            "config/work_plan_templates.json: evaluations must be exactly "
            f"{ids} in order, got {list(work_plans['evaluations'])}"
        )

    for position, (eid, display) in enumerate(zip(ids, display_names), start=1):
        spec_path = TEMPLATE / "methodology" / f"{eid}.md"
        if not spec_path.is_file():
            fail(f"methodology/{eid}.md is missing")
            continue
        spec_text = read_text(spec_path)
        first_line = spec_text.splitlines()[0].strip()
        if first_line != f"# {display} Specification":
            fail(
                f"methodology/{eid}.md: first heading must be '# {display} Specification', "
                f"got {first_line!r}"
            )
        banner = f"**Primary Evaluation {position} — {display}**"
        if banner not in spec_text:
            fail(f"methodology/{eid}.md: must identify itself as {banner}")

    common_languages = numbered_list(
        read_text(TEMPLATE / "methodology" / "common.md"), "# 3. Fixed Comparison Languages"
    )
    if common_languages != languages:
        fail(
            "methodology/common.md section 3 must list exactly "
            f"{languages} in order, got {common_languages}"
        )

    readme = read_text(TEMPLATE / "README.md")
    brace = re.search(r"methodology/\{([^}]*)\}\.md", readme)
    if not brace:
        fail("README.md: no 'methodology/{...}.md' evaluation-specification list found")
    elif [part.strip() for part in brace.group(1).split(",")] != ids:
        fail(
            f"README.md: the methodology/{{...}}.md list must be {ids} in order, "
            f"got {[part.strip() for part in brace.group(1).split(',')]}"
        )
    if "config/benchmark_metadata.json" not in readme:
        fail("README.md: config/benchmark_metadata.json is missing from the layout list")

    if not MASTER_PROMPT.is_file():
        fail(f"{MASTER_PROMPT} is missing")
    else:
        prompt_names = numbered_list(
            read_text(MASTER_PROMPT), "## 2. Five independent Primary evaluations"
        )
        if prompt_names != display_names:
            fail(
                "master_prompt.md section 2 must list exactly "
                f"{display_names} in order, got {prompt_names}"
            )

    cli = load_module(TEMPLATE / "scripts" / "benchmark.py", "benchmark_cli_metadata_check")
    if list(cli.PRIMARY_NAMES) != ids:
        fail(f"scripts/benchmark.py: PRIMARY_NAMES is {list(cli.PRIMARY_NAMES)}, expected {ids}")
    if cli.PRIMARY_DISPLAY_NAMES != dict(zip(ids, display_names)):
        fail("scripts/benchmark.py: PRIMARY_DISPLAY_NAMES disagrees with benchmark_metadata.json")
    if cli.EVALUATION_SPEC_FILES != {eid: f"{eid}.md" for eid in ids}:
        fail("scripts/benchmark.py: EVALUATION_SPEC_FILES disagrees with benchmark_metadata.json")
    if cli.metadata_languages(TEMPLATE.parent) != languages:
        fail("scripts/benchmark.py: metadata_languages() disagrees with benchmark_metadata.json")

    micro = load_module(TEMPLATE / "scripts" / "micro_measure.py", "micro_measure_metadata_check")
    if list(micro.LANGUAGES) != languages:
        fail(f"scripts/micro_measure.py: LANGUAGES is {list(micro.LANGUAGES)}, expected {languages}")
    if list(micro.CONFIGS) != languages:
        fail(
            f"scripts/micro_measure.py: CONFIGS keys are {list(micro.CONFIGS)}, expected {languages}"
        )

    return problems


def main() -> int:
    problems = check()
    if problems:
        print("benchmark metadata drift:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 2
    print("benchmark metadata consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
