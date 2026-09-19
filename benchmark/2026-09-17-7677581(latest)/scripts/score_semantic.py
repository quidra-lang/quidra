#!/usr/bin/env python3
"""
Score Primary Evaluation 1 — Semantic Compression.

Reads the frozen probe annotations and the AUTHORITATIVE token counts (from
retokenize_all.py, per CORRECTIONS D-4 -- token counts recorded inline in an
annotation are superseded), then applies the frozen formulas:

    Q       = 0.20*Density + 0.25*Determinacy + 0.20*Locality
              + 0.20*HiddenCost + 0.15*CapabilityEfficiency
    C       = 100 * supported_points / total_points
    Overall = 2QC / (Q + C)                      (harmonic mean, spec 6.1.6)

Metrics B, C, D and E are computed on the COMMON BASIS (probes every language
supports) as the primary figure, with the all-fragments basis published beside
it, per the post-remediation rule in methodology 01/03/04. Reporting both is
required: a language must not gain by having fewer measurable probes.
"""

from __future__ import annotations

import glob
import json
import math
import os
import sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import score as S  # noqa: E402

RUN = os.path.dirname(D)
ANN = os.path.join(RUN, "semantic-compression", "raw", "annotations")
TOK = os.path.join(RUN, "semantic-compression", "raw", "tokens")
OUT = os.path.join(RUN, "semantic-compression", "scores")

DISPLAY = {"quidra": "Quidra", "python": "Python", "cpp": "C++", "rust": "Rust",
           "go": "Go", "java": "Java", "typescript": "TypeScript",
           "kotlin": "Kotlin", "swift": "Swift", "zig": "Zig"}
ORDER = ["Quidra", "Python", "C++", "Rust", "Go", "Java", "TypeScript",
         "Kotlin", "Swift", "Zig"]


def load_annotations():
    langs = {}
    for ldir in sorted(os.listdir(ANN)):
        d = os.path.join(ANN, ldir)
        if not os.path.isdir(d) or ldir not in DISPLAY:
            continue
        probes = {}
        for f in sorted(glob.glob(os.path.join(d, "chunk_*.json"))):
            try:
                doc = json.load(open(f))
            except Exception as e:  # noqa: BLE001
                print(f"  WARNING: unreadable {f}: {e}", file=sys.stderr)
                continue
            for p in doc.get("probes", []):
                if p.get("probe_id"):
                    probes[p["probe_id"]] = p
        if probes:
            langs[ldir] = probes
    return langs


def load_tokens():
    """Authoritative counts from the single re-tokenization pass."""
    toks = {}
    for ldir in sorted(os.listdir(TOK)) if os.path.isdir(TOK) else []:
        d = os.path.join(TOK, ldir)
        if not os.path.isdir(d) or ldir not in DISPLAY:
            continue
        per = {}
        for f in glob.glob(os.path.join(d, "*.json")):
            try:
                doc = json.load(open(f))
            except Exception:
                continue
            if doc.get("probe_id"):
                per[doc["probe_id"]] = doc.get("count")
        toks[ldir] = per
    return toks


def _num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def main():
    ann = load_annotations()
    toks = load_tokens()
    if not ann:
        print("no annotations found", file=sys.stderr)
        return 1

    universe = json.load(open(os.path.join(RUN, "methodology",
                                           "01_capability_universe_and_probes.json")))
    total_points = _num(universe.get("total_fixed_capability_points"), 88.0)
    all_probe_ids = [p["probe_id"] for p in universe.get("probes", [])]

    # COMMON BASIS: probes that every present language actually measured
    # (support != NONE and a token count exists). Metrics B/C/D/E use this.
    present = sorted(ann.keys())
    def measurable(l, pid):
        p = ann[l].get(pid)
        return bool(p) and p.get("support") != "NONE" and toks.get(l, {}).get(pid) is not None
    common = [pid for pid in all_probe_ids if all(measurable(l, pid) for l in present)]

    raw = {}
    for l in present:
        name = DISPLAY[l]
        probes = ann[l]

        sup_pts = sum(_num(p.get("capability_points_awarded"), 0.0) or 0.0
                      for p in probes.values())

        def over(ids, key, transform=None):
            vals = []
            for pid in ids:
                p = probes.get(pid)
                if not p or p.get("support") == "NONE":
                    continue
                v = _num(p.get(key))
                if v is None:
                    continue
                vals.append(transform(v) if transform else v)
            return vals

        # A. Density = facts / tokens, on the common basis (and all-fragments)
        def density(ids):
            f = sum(_num(probes[pid].get("fact_count"), 0.0) or 0.0
                    for pid in ids if pid in probes and probes[pid].get("support") != "NONE")
            t = sum(_num(toks.get(l, {}).get(pid), 0.0) or 0.0 for pid in ids
                    if pid in probes and probes[pid].get("support") != "NONE")
            return (f / t) if t else None

        # B. Determinacy = mean(log2(B_i)); lower is better
        b_common = over(common, "determinacy_B", lambda v: math.log2(max(v, 1.0)))
        b_all = over(all_probe_ids, "determinacy_B", lambda v: math.log2(max(v, 1.0)))

        raw[name] = {
            "supported_points": sup_pts,
            "density_common": density(common),
            "density_all": density(all_probe_ids),
            "determinacy_common": (sum(b_common) / len(b_common)) if b_common else None,
            "determinacy_all": (sum(b_all) / len(b_all)) if b_all else None,
            "locality_common": (lambda v: sum(v) / len(v) if v else None)(over(common, "locality_lookups")),
            "locality_all": (lambda v: sum(v) / len(v) if v else None)(over(all_probe_ids, "locality_lookups")),
            "hidden_common": (lambda v: sum(v) / len(v) if v else None)(over(common, "hidden_count")),
            "hidden_all": (lambda v: sum(v) / len(v) if v else None)(over(all_probe_ids, "hidden_count")),
            "complexity_units_common": sum(over(common, "complexity_units")),
            "complexity_units_all": sum(over(all_probe_ids, "complexity_units")),
            "probes_measured_common": len(common),
            "probes_measured_all": sum(1 for pid in all_probe_ids
                                       if pid in probes and probes[pid].get("support") != "NONE"),
            "support_counts": {
                k: sum(1 for p in probes.values() if p.get("support") == k)
                for k in ("FULL", "PARTIAL", "NONE")
            },
        }
        # E. Capability efficiency = complexity units per supported capability point
        raw[name]["capeff_common"] = (raw[name]["complexity_units_common"] / sup_pts) if sup_pts else None
        raw[name]["capeff_all"] = (raw[name]["complexity_units_all"] / sup_pts) if sup_pts else None

    langs = [DISPLAY[l] for l in present]

    def fam_d(key):
        return S.family_d({n: raw[n][key] for n in langs})

    def fam_c(key):
        return S.family_c({n: raw[n][key] for n in langs})

    results, reports = {}, {}
    sc_density, reports["Semantic Density"] = fam_d("density_common")
    sc_det, reports["Semantic Determinacy"] = fam_c("determinacy_common")
    sc_loc, reports["Semantic Locality"] = fam_c("locality_common")
    sc_hid, reports["Hidden Semantic Cost"] = fam_c("hidden_common")
    sc_cap, reports["Capability Efficiency"] = fam_c("capeff_common")

    table = {"semantic_density": sc_density, "semantic_determinacy": sc_det,
             "semantic_locality": sc_loc, "hidden_semantic_cost": sc_hid,
             "capability_efficiency": sc_cap}

    q, c, overall = {}, {}, {}
    for n in langs:
        qq, _ = S.semantic_quality({
            "semantic_density": sc_density.get(n),
            "semantic_determinacy": sc_det.get(n),
            "semantic_locality": sc_loc.get(n),
            "hidden_semantic_cost": sc_hid.get(n),
            "capability_efficiency": sc_cap.get(n),
        })
        cc = S.capability_coverage(raw[n]["supported_points"], total_points)
        q[n], c[n] = qq, cc
        overall[n] = S.semantic_compression_overall(qq, cc)

    table["quality_Q"], table["coverage_C"], table["overall"] = q, c, overall
    results = {"table": table, "raw": raw, "family_c_reports": reports,
               "common_basis": common, "common_basis_size": len(common),
               "total_capability_points": total_points,
               "languages_present": langs,
               "languages_missing": [DISPLAY[k] for k in DISPLAY if k not in present]}

    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "semantic_compression_scores.json"), "w") as f:
        json.dump(results, f, indent=1)

    # ---- console report -------------------------------------------------
    print(f"COMMON BASIS: {len(common)} of {len(all_probe_ids)} probes "
          f"(measurable in all {len(langs)} languages present)")
    if results["languages_missing"]:
        print(f"MISSING LANGUAGES (not yet annotated): {results['languages_missing']}")
    print()
    hdr = (f"{'language':12s} {'FULL':>4s} {'PART':>4s} {'NONE':>4s} {'pts':>6s} "
           f"{'Dens':>6s} {'Det':>6s} {'Loc':>6s} {'Hid':>6s} {'CapEf':>6s} "
           f"{'Q':>6s} {'C':>6s} {'OVERALL':>8s}")
    print(hdr); print("-" * len(hdr))
    for n in sorted(langs, key=lambda x: -(overall[x] or 0)):
        r = raw[n]; sc = r["support_counts"]
        print(f"{n:12s} {sc['FULL']:>4d} {sc['PARTIAL']:>4d} {sc['NONE']:>4d} "
              f"{r['supported_points']:>6.1f} "
              f"{S.fmt(sc_density.get(n)):>6s} {S.fmt(sc_det.get(n)):>6s} "
              f"{S.fmt(sc_loc.get(n)):>6s} {S.fmt(sc_hid.get(n)):>6s} "
              f"{S.fmt(sc_cap.get(n)):>6s} "
              f"{S.fmt(q[n]):>6s} {S.fmt(c[n]):>6s} {S.fmt(overall[n]):>8s}")
    print()
    print("RAW (common basis, natural units):")
    print(f"{'language':12s} {'facts/token':>12s} {'mean log2(B)':>13s} "
          f"{'lookups':>8s} {'hidden':>7s} {'SCU/point':>10s}")
    for n in ORDER:
        if n not in raw: continue
        r = raw[n]
        def g(k):
            v = r.get(k)
            return f"{v:.3f}" if isinstance(v, float) else "N/A"
        print(f"{n:12s} {g('density_common'):>12s} {g('determinacy_common'):>13s} "
              f"{g('locality_common'):>8s} {g('hidden_common'):>7s} {g('capeff_common'):>10s}")

    for name, rep in reports.items():
        if rep.get("compression_disclosure_required"):
            print()
            print(f"NOTE [{name}]: raw values span {rep['span_factor']:.1f}x — family C compresses; "
                  "ratios, not scores, carry the comparison between non-leading languages.")
    print()
    print(f"wrote {OUT}/semantic_compression_scores.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
