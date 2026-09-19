#!/usr/bin/env python3
"""
Final report renderer.

Produces exactly the tables spec sections 27, 28, 29 and 34 require, from a
results JSON. Every table follows the universal direction rule: 100 = best,
0 = worst, higher is always better. `N/A` stays `N/A` and is never rendered as
zero.

Hard constraint enforced here, from spec sections 6, 32 and 35: there is NO
cross-evaluation weighted overall score and NO combined ranking. The four
primary evaluations are reported separately and never merged. render_all()
refuses to emit any table named or keyed as a combined total.
"""

from __future__ import annotations

import json
import os
import sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import score as S  # noqa: E402

LANGS = ["Quidra", "Python", "C++", "Rust", "Go", "Java",
         "TypeScript", "Kotlin", "Swift", "Zig"]

SEMANTIC_ROWS = [
    ("semantic_density", "Semantic Density"),
    ("semantic_determinacy", "Semantic Determinacy"),
    ("semantic_locality", "Semantic Locality"),
    ("hidden_semantic_cost", "Hidden Semantic Cost"),
    ("capability_efficiency", "Capability Efficiency"),
    ("quality_Q", "Raw Semantic Compression Quality `Q`"),
    ("coverage_C", "Capability Coverage `C`"),
    ("overall", "**Semantic Compression Overall Score**"),
]

STANDARD_ROWS = [
    ("native_performance", "Native Performance"),
    ("interactive_performance", "Interactive Performance"),
    ("long_running_performance", "Long-running Performance"),
    ("compile_build_performance", "Compile / Build Performance"),
    ("startup_repl_latency", "Startup / REPL Latency"),
    ("memory_efficiency", "Memory Efficiency"),
    ("source_code_size", "Source Code Size"),
    ("binary_artifact_size", "Binary / Artifact Size"),
    ("deployment_footprint", "Deployment Footprint"),
    ("runtime_overhead", "Runtime Overhead"),
    ("code_efficiency", "Code Efficiency / Conciseness"),
    ("readability", "Readability"),
    ("functionality", "Functionality / Expressiveness"),
    ("diagnostics", "Diagnostics"),
    ("dependency_simplicity", "Dependency Simplicity"),
    ("portability", "Portability"),
    ("interoperability", "Interoperability"),
    ("concurrency", "Concurrency"),
    ("type_safety", "Type Safety"),
    ("memory_safety", "Memory Safety"),
    ("runtime_safety", "Runtime Safety"),
    ("boundary_value_safety", "Boundary Value Safety"),
    ("adversarial_robustness", "Adversarial Input Robustness"),
    ("early_error_detection", "Early Error Detection"),
    ("debuggability", "Debuggability"),
    ("silent_bug_resistance", "Silent Bug Resistance"),
    ("implementation_robustness", "Implementation Robustness"),
    ("ecosystem_breadth", "Ecosystem Breadth"),
    ("tooling", "Tooling"),
    ("library_availability", "Library Availability"),
    ("package_management", "Package / Dependency Management"),
    ("ide_support", "IDE / Editor Support"),
    ("debugger_profiler", "Debugger / Profiler Support"),
    ("build_test_integration", "Build / Test Integration"),
    ("production_adoption", "Production Adoption / Deployment Evidence"),
    ("documentation_community", "Documentation / Community"),
    ("toolchain_stability", "Toolchain Stability / Release Maturity"),
    ("overall", "**Standard Overall Score**"),
]

INTRINSIC_ROWS = [
    ("I1", "I1 Keyword Anonymization"),
    ("I2", "I2 Vocabulary Anonymization"),
    ("I3", "I3 Structural Surface Perturbation"),
    ("I4", "I4 Novel-rule Generalization"),
    ("I5", "I5 Held-out Rule Composition"),
    ("I6", "I6 Prior-conflict Resistance"),
    ("I1_drop", "I1 Familiarity Drop, raw diagnostic"),
    ("I2_drop", "I2 Familiarity Drop, raw diagnostic"),
    ("overall", "**LLM Intrinsic Learnability Score**"),
]

PRACTICAL_ROWS = [
    ("generation_success", "LLM Generation Success"),
    ("compile_success", "LLM Compile Success"),
    ("correct_at_1", "LLM Correct@1"),
    ("repair_success", "LLM Repair Success"),
    ("repair_efficiency", "LLM Repair Efficiency"),
    ("diagnosis_efficiency", "LLM Diagnosis Efficiency"),
    ("token_efficiency", "LLM Token Efficiency"),
    ("prompt_robustness", "LLM Prompt Robustness"),
    ("unseen_generalization", "LLM Unseen-case Generalization"),
    ("hallucination_resistance", "LLM Hallucination Resistance"),
    ("silent_bug_resistance", "LLM Silent Bug Resistance"),
    ("generated_code_performance", "LLM Generated Code Performance"),
    ("overall", "**LLM Practical Effectiveness Score**"),
]

BANNED = ("combined", "overall_ranking", "cross_evaluation", "weighted_total",
          "grand_total", "final_overall")


def _cell(v):
    if v is None:
        return "N/A"
    if isinstance(v, str):
        return v
    return S.fmt(v)


def table(rows, data, title, note=None):
    """data: {row_key: {language: score}}"""
    out = [f"### {title}", ""]
    out.append("| Metric | " + " | ".join(LANGS) + " |")
    out.append("|---" + "|---:" * len(LANGS) + "|")
    for key, label in rows:
        vals = data.get(key, {})
        out.append(f"| {label} | " + " | ".join(_cell(vals.get(l)) for l in LANGS) + " |")
    out.append("")
    if note:
        out += [note, ""]
    return "\n".join(out)


def ranking(scores_by_lang, title):
    out = [f"### {title}", "", "| Rank | Language | Score |", "|---:|---|---:|"]
    for r in S.rank(scores_by_lang):
        if r.get("score") is None:
            out.append(f"| — | {r['language']} | N/A |")
        else:
            out.append(f"| {r['rank']} | {r['language']} | {S.fmt(r['score'])} |")
    out.append("")
    return "\n".join(out)


def raw_table(title, headers, rows, note=None):
    """Raw evidence table in natural units -- NOT converted to 100-is-best."""
    out = [f"### {title}", ""]
    out.append("| " + " | ".join(headers) + " |")
    out.append("|" + "|".join(["---"] + ["---:"] * (len(headers) - 1)) + "|")
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    out.append("")
    if note:
        out += [note, ""]
    return "\n".join(out)


def family_c_disclosure(name, report):
    """Spec 25.1.C: when applicable raw values span >= 100x, publish raws,
    ratios and an explicit statement that the ratios carry the comparison."""
    if not report or not report.get("compression_disclosure_required"):
        return ""
    out = [f"#### {name} — compressed-scale disclosure", "",
           report.get("compression_note", ""), "",
           "| Language | Raw value | Ratio to best |", "|---|---:|---:|"]
    raw = report.get("raw", {})
    ratio = report.get("ratio_to_best", {})
    for l in LANGS:
        rv = raw.get(l)
        rt = ratio.get(l)
        out.append(f"| {l} | {'N/A' if rv is None else rv} | "
                   f"{'N/A' if rt is None else f'{rt:.2f}x'} |")
    out.append("")
    return "\n".join(out)


def render_all(results, out_path):
    for k in results:
        if any(b in str(k).lower() for b in BANNED):
            raise SystemExit(
                f"refusing to render '{k}': spec sections 6, 32 and 35 prohibit any "
                "cross-evaluation combined score or overall ranking")

    meta = results.get("meta", {})
    doc = [
        "# Quidra Comprehensive Benchmark — Results",
        "",
        f"**Run id:** `{meta.get('run_id', '')}`  ",
        f"**Evaluated Quidra HEAD:** `{meta.get('quidra_head', '')}` "
        f"(branch `{meta.get('branch', 'develop')}`)  ",
        f"**Date:** {meta.get('date', '')}  ",
        f"**Host:** {meta.get('host', '')}",
        "",
        "Every normalized score below follows the universal direction rule: "
        "**100 = best, 0 = worst, higher is always better.** `N/A` means the metric was genuinely "
        "not applicable and is never rendered as zero.",
        "",
        "> **No combined score is reported.** The four primary evaluations answer different questions "
        "and are deliberately never averaged, weighted or merged into one ranking "
        "(spec sections 6, 32, 35).",
        "",
        "---",
        "",
        "## Primary Evaluation 1 — Semantic Compression",
        "",
    ]
    sc = results.get("semantic_compression", {})
    doc.append(table(SEMANTIC_ROWS, sc.get("table", {}), "Semantic Compression Final Comparison"))
    doc.append(ranking(sc.get("table", {}).get("overall", {}), "Semantic Compression Ranking"))
    for name, rep in (sc.get("family_c_reports") or {}).items():
        d = family_c_disclosure(name, rep)
        if d:
            doc.append(d)

    doc += ["---", "", "## Primary Evaluation 2 — Standard", ""]
    st = results.get("standard", {})
    doc.append(table(STANDARD_ROWS, st.get("table", {}), "Standard Final Comparison"))
    doc.append(ranking(st.get("table", {}).get("overall", {}), "Standard Ranking"))
    for name, rep in (st.get("family_c_reports") or {}).items():
        d = family_c_disclosure(name, rep)
        if d:
            doc.append(d)

    doc += ["---", "",
            "## Primary Evaluation 3 — LLM Intrinsic / Unknown-Language Learnability", ""]
    it = results.get("llm_intrinsic", {})
    doc.append(table(INTRINSIC_ROWS, it.get("table", {}), "LLM Intrinsic Learnability"))
    doc.append(ranking(it.get("table", {}).get("overall", {}), "LLM Intrinsic Learnability Ranking"))

    doc += ["---", "",
            "## Primary Evaluation 4 — LLM Standard / Knowledge-Dependent Performance", ""]
    pt = results.get("llm_practical", {})
    doc.append(table(PRACTICAL_ROWS, pt.get("table", {}), "LLM Practical Effectiveness"))
    doc.append(ranking(pt.get("table", {}).get("overall", {}), "LLM Practical Effectiveness Ranking"))

    for title, block in (results.get("raw_tables") or {}).items():
        doc.append(raw_table(title, block["headers"], block["rows"], block.get("note")))

    na = results.get("not_executed") or []
    if na:
        doc += ["---", "", "## N/A and Not Executed", "",
                "Recorded explicitly rather than estimated or fabricated "
                "(spec sections 26 and 33).", "",
                "| Item | Status | Reason |", "|---|---|---|"]
        for r in na:
            doc.append(f"| {r.get('item')} | {r.get('status')} | {r.get('reason')} |")
        doc.append("")

    for title, body in (results.get("appendices") or {}).items():
        doc += ["---", "", f"## {title}", "", body, ""]

    text = "\n".join(doc)
    with open(out_path, "w") as f:
        f.write(text)
    return text


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: report.py RESULTS.json OUT.md", file=sys.stderr)
        sys.exit(2)
    res = json.load(open(sys.argv[1]))
    render_all(res, sys.argv[2])
    print(f"wrote {sys.argv[2]}")
