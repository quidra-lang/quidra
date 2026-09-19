#!/usr/bin/env python3
"""
Score the MEASURED Standard metrics from the micro timing run.

Scores only what was actually measured. The Safety/Robustness category (25% of
the Standard Overall Score) and the Ecosystem rubrics (20%) were Not Executed,
so **no Standard Overall Score is produced** -- renormalising the remaining
categories to 100% would silently redefine the metric, which spec section 26
permits for a genuinely inapplicable metric but not for one that was not run.
See NOT_EXECUTED.md items 3 and 4.

Quidra's two execution modes are combined by the rule frozen in
00_cross_language_constraints.md C-10, before normalization.
"""

from __future__ import annotations

import json
import os
import statistics
import sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import score as S  # noqa: E402

RUN = os.path.dirname(D)
TIMING = os.path.join(RUN, "micro", "raw", "micro_timing.json")
OUT = os.path.join(RUN, "standard", "scores")

COL = {"quidra_native": "Quidra", "quidra_interpreter": "Quidra", "python": "Python",
       "cpp": "C++", "rust": "Rust", "go": "Go", "java": "Java",
       "typescript": "TypeScript", "kotlin": "Kotlin", "swift": "Swift", "zig": "Zig"}
ORDER = ["Quidra", "Python", "C++", "Rust", "Go", "Java", "TypeScript", "Kotlin", "Swift", "Zig"]
WL = [f"mb{n:02d}" for n in range(1, 12)]


def load():
    d = json.load(open(TIMING))
    cells = {}
    for r in d["results"]:
        if r.get("status") != "ok":
            continue
        cells.setdefault(r["config"], {})[r["workload"]] = r
    return cells


def combine_quidra(cells, field, mode_rule):
    """C-10: Native only, Interpreter only, or the mean of the two, per metric."""
    n = cells.get("quidra_native", {})
    i = cells.get("quidra_interpreter", {})
    out = {}
    for w in WL:
        vn = field(n.get(w)) if w in n else None
        vi = field(i.get(w)) if w in i else None
        if mode_rule == "native":
            out[w] = vn
        elif mode_rule == "interpreter":
            out[w] = vi
        else:  # mean of both modes, at the raw level, per workload
            vals = [v for v in (vn, vi) if v is not None]
            out[w] = (sum(vals) / len(vals)) if vals else None
    return out


def per_language(cells, field, mode_rule):
    langs = {}
    for cfg, per_w in cells.items():
        col = COL[cfg]
        if col == "Quidra":
            continue
        langs[col] = {w: (field(per_w[w]) if w in per_w else None) for w in WL}
    langs["Quidra"] = combine_quidra(cells, field, mode_rule)
    return langs


def aggregate(per_lang_per_w, how="mean"):
    """Aggregate across workloads. Spec 25.2: unweighted arithmetic mean across
    the fixed workload set, after per-workload normalization."""
    return {l: (statistics.fmean([v for v in d.values() if v is not None])
                if any(v is not None for v in d.values()) else None)
            for l, d in per_lang_per_w.items()}


def normalize_per_workload_then_mean(per_lang_per_w, family="C"):
    """Normalize each workload separately (so a slow workload cannot dominate),
    then take the unweighted mean of the per-workload scores."""
    scores_by_lang = {l: [] for l in per_lang_per_w}
    reports = {}
    for w in WL:
        raw = {l: per_lang_per_w[l].get(w) for l in per_lang_per_w}
        if not any(v is not None for v in raw.values()):
            continue
        sc, rep = (S.family_c(raw) if family == "C" else S.family_d(raw))
        reports[w] = rep
        for l, v in sc.items():
            if v is not S.NA:
                scores_by_lang[l].append(v)
    return ({l: (statistics.fmean(v) if v else S.NA) for l, v in scores_by_lang.items()},
            reports)


def main():
    if not os.path.exists(TIMING):
        print("no timing data", file=sys.stderr)
        return 1
    cells = load()

    wall = lambda r: r["timing"]["representative"]["wall_seconds"]      # noqa: E731
    rss = lambda r: r["timing"]["representative"]["peak_rss_bytes"]     # noqa: E731
    comp = lambda r: (r.get("build") or {}).get("wall_seconds")         # noqa: E731
    art = lambda r: r.get("artifact_size_bytes")                        # noqa: E731
    first = lambda r: r["timing"]["first_run_wall_seconds"]             # noqa: E731

    # C-10 mode rules
    METRICS = [
        ("native_performance", "Native Execution Performance", wall, "native", "C"),
        ("long_running_performance", "Long-running Performance", wall, "mean", "C"),
        ("memory_efficiency", "Memory Efficiency", rss, "mean", "C"),
        ("compile_build_performance", "Compile / Build Performance", comp, "native", "C"),
        ("binary_artifact_size", "Binary / Artifact Size", art, "native", "C"),
        ("interactive_performance", "Interactive / Interpreter Performance", wall, "interpreter", "C"),
    ]

    table, raws, reports = {}, {}, {}
    for key, name, field, mode, fam in METRICS:
        per = per_language(cells, field, mode)
        if not any(any(v is not None for v in d.values()) for d in per.values()):
            continue
        sc, rep = normalize_per_workload_then_mean(per, fam)
        table[key] = sc
        raws[key] = {"name": name, "mode_rule": mode, "per_workload": per,
                     "aggregate_raw": aggregate(per)}
        reports[key] = rep

    os.makedirs(OUT, exist_ok=True)
    result = {
        "_scope": ("MEASURED Standard metrics only. No Standard Overall Score: the Safety/Robustness "
                   "category (25%) and the Ecosystem rubrics (20%) were Not Executed, so 45% of the "
                   "Standard weight has no evidence. See NOT_EXECUTED.md."),
        "quidra_mode_rules": {k: v[3] for k, v in
                              {m[0]: m for m in METRICS}.items()},
        "table": table, "raw": raws,
    }
    json.dump(result, open(os.path.join(OUT, "standard_measured_scores.json"), "w"), indent=1)

    print("Standard — 実測済み指標のみ（総合スコアは非公表: 重みの45%が未実行）")
    print()
    hdr = f"{'metric':34s}" + "".join(f"{l[:9]:>10s}" for l in ORDER)
    print(hdr); print("-" * len(hdr))
    for key, name, *_ in METRICS:
        if key not in table:
            continue
        row = f"{name[:33]:34s}"
        for l in ORDER:
            v = table[key].get(l)
            row += f"{S.fmt(v):>10s}"
        print(row)
    print()
    print("生値（合計/平均, 自然単位）")
    for key, name, *_ in METRICS:
        if key not in raws:
            continue
        agg = raws[key]["aggregate_raw"]
        unit = {"memory_efficiency": "bytes", "binary_artifact_size": "bytes"}.get(key, "s")
        print(f"  {name} [{raws[key]['mode_rule']}] ({unit})")
        for l in ORDER:
            v = agg.get(l)
            if v is None:
                continue
            print(f"    {l:12s} {v:>14,.3f}" if unit == "s" else f"    {l:12s} {v:>14,.0f}")
    print()
    print(f"wrote {OUT}/standard_measured_scores.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
