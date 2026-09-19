#!/usr/bin/env python3
"""
Frozen normalization and scoring library.

Implements exactly the normalization families fixed in spec section 25.1, the
clipping/precision rules of 25.3, the N/A policy of section 26, and the
Semantic Compression harmonic mean of 6.1.6.

Every normalized score produced here obeys the universal direction rule:
100 = best, 0 = worst, higher is always better.

This module deliberately contains NO metric-specific special cases and no
post-result formula selection. A caller picks the family for a metric from the
metric's type, which is frozen in the methodology before results are observed.
"""

from __future__ import annotations

import math

NA = None  # a metric that is genuinely not applicable

# Spec 25.1.F — fixed ordered defect-detection stage scores.
STAGE_SCORES = {
    "compile_time_detection": 100,
    "static_checking_before_execution": 90,
    "runtime_safe_detection": 75,
    "test_failure": 55,
    "output_verification": 40,
    "crash": 20,
    "undefined_behavior": 5,
    "silent_bug": 0,
}


def clip(x, lo=0.0, hi=100.0):
    return max(lo, min(hi, x))


def clamp01(x):
    return max(0.0, min(1.0, x))


# --------------------------------------------------------------------------
# Family A — rates / probabilities / bounded success fractions, 1.0 ideal
# --------------------------------------------------------------------------
def family_a(raw_fraction):
    """Score = 100 * clamp(raw_fraction, 0, 1)."""
    if raw_fraction is NA:
        return NA
    return clip(100.0 * clamp01(float(raw_fraction)))


# --------------------------------------------------------------------------
# Family B — bounded error/failure fractions, 0.0 ideal
# --------------------------------------------------------------------------
def family_b(raw_fraction):
    """Score = 100 * (1 - clamp(raw_fraction, 0, 1))."""
    if raw_fraction is NA:
        return NA
    return clip(100.0 * (1.0 - clamp01(float(raw_fraction))))


# --------------------------------------------------------------------------
# Family C — positive lower-is-better quantities
# --------------------------------------------------------------------------
def family_c(raw_by_lang, epsilon=None):
    """Score_i = 100 * best_positive_raw / raw_i  (lower raw is better).

    With `epsilon` (the predeclared shifted form, for metrics where a legitimate
    exact zero is possible): Score_i = 100 * (best_raw + eps) / (raw_i + eps).

    Returns (scores, report) where `report` carries the spec-mandated
    transparency payload: raw values, ratio to best, and whether the applicable
    raw values span a factor of 100 or more. When they do, spec 25.1.C REQUIRES
    publishing raws and ratios beside the compressed score and stating that the
    ratios, not the scores, carry the comparison between non-leading languages.
    """
    applicable = {k: float(v) for k, v in raw_by_lang.items() if v is not NA}

    if epsilon is None:
        positives = {k: v for k, v in applicable.items() if v > 0}
        if not positives:
            return ({k: (NA if raw_by_lang.get(k) is NA else 0.0) for k in raw_by_lang},
                    {"note": "no positive raw values; scores not computable", "raw": raw_by_lang})
        best = min(positives.values())
        scores = {}
        for k in raw_by_lang:
            v = raw_by_lang[k]
            if v is NA:
                scores[k] = NA
            elif float(v) <= 0:
                # A non-positive raw on a metric with no predeclared epsilon is a
                # measurement defect, not a perfect score. Surface it.
                scores[k] = NA
            else:
                scores[k] = clip(100.0 * best / float(v))
        span_vals = list(positives.values())
    else:
        eps = float(epsilon)
        if not applicable:
            return ({k: NA for k in raw_by_lang}, {"note": "no applicable raw values"})
        best = min(applicable.values())
        scores = {k: (NA if raw_by_lang[k] is NA
                      else clip(100.0 * (best + eps) / (float(raw_by_lang[k]) + eps)))
                  for k in raw_by_lang}
        span_vals = list(applicable.values())

    lo = min(v for v in span_vals if v > 0) if any(v > 0 for v in span_vals) else None
    hi = max(span_vals) if span_vals else None
    span = (hi / lo) if (lo and hi and lo > 0) else None
    report = {
        "family": "C",
        "direction": "lower raw is better",
        "epsilon": epsilon,
        "best_raw": lo,
        "raw": dict(raw_by_lang),
        "ratio_to_best": {k: (None if raw_by_lang[k] is NA or lo in (None, 0)
                              else float(raw_by_lang[k]) / lo) for k in raw_by_lang},
        "span_factor": span,
        "compression_disclosure_required": bool(span is not None and span >= 100),
    }
    if report["compression_disclosure_required"]:
        report["compression_note"] = (
            "Applicable raw values span a factor of {:.1f}. Family C is a hyperbola, so the "
            "normalized scores for this metric are compressed toward zero for every non-leading "
            "language. The raw values and the ratio_to_best column, not the normalized scores, "
            "carry the comparison between the non-leading languages.".format(span)
        )
    return scores, report


# --------------------------------------------------------------------------
# Family D — positive higher-is-better quantities without a natural 0-1 bound
# --------------------------------------------------------------------------
def family_d(raw_by_lang):
    """Score_i = 100 * raw_i / best_raw, best_raw = largest applicable value."""
    applicable = {k: float(v) for k, v in raw_by_lang.items() if v is not NA}
    if not applicable:
        return ({k: NA for k in raw_by_lang}, {"note": "no applicable raw values"})
    best = max(applicable.values())
    if best <= 0:
        return ({k: (NA if raw_by_lang[k] is NA else 0.0) for k in raw_by_lang},
                {"note": "best raw value is not positive"})
    scores = {k: (NA if raw_by_lang[k] is NA else clip(100.0 * float(raw_by_lang[k]) / best))
              for k in raw_by_lang}
    return scores, {"family": "D", "direction": "higher raw is better",
                    "best_raw": best, "raw": dict(raw_by_lang)}


# --------------------------------------------------------------------------
# Family E — repair-iteration count against the fixed three-repair budget
# --------------------------------------------------------------------------
def family_e(repair_turns):
    """Repair Efficiency = 100 * (1 - repair_turns/3), clipped to [0,100].

    A Correct@1 success has zero repair turns and scores 100. A trial still
    incorrect after the third repair scores 0 and is also a repair failure
    (Repair Success is a separate metric).
    """
    if repair_turns is NA:
        return NA
    return clip(100.0 * (1.0 - float(repair_turns) / 3.0))


def family_e_aggregate(trial_repair_turns):
    """Average trial-level Repair Efficiency scores (spec 25.1.E)."""
    vals = [family_e(t) for t in trial_repair_turns if t is not NA]
    return sum(vals) / len(vals) if vals else NA


# --------------------------------------------------------------------------
# Family F — ordered defect-detection stage
# --------------------------------------------------------------------------
def family_f(stages):
    """Average the fixed stage scores over the fixed adversarial case set."""
    vals = []
    for s in stages:
        if s is NA:
            continue
        key = str(s).strip().lower().replace(" ", "_").replace("-", "_")
        if key not in STAGE_SCORES:
            raise ValueError(f"unknown detection stage: {s!r}")
        vals.append(STAGE_SCORES[key])
    return (sum(vals) / len(vals)) if vals else NA


# --------------------------------------------------------------------------
# Family G — objective rubric / proxy metrics
# --------------------------------------------------------------------------
def family_g(level_score):
    """Rubric levels already express a 0-100 score; clip and pass through.

    Spec 25.1.G: unsupported capabilities intentionally covered by a rubric
    receive the rubric-defined LOW score, not N/A.
    """
    if level_score is NA:
        return NA
    return clip(float(level_score))


# --------------------------------------------------------------------------
# Weighted aggregation with the section 26 N/A policy
# --------------------------------------------------------------------------
def weighted(scores_by_metric, weights_by_metric, strict=True):
    """Weighted mean over metrics, excluding N/A metrics from the denominator
    and renormalizing the remaining weights (spec 26).

    Returns (score, detail). N/A is never silently converted to zero.
    """
    used, skipped = {}, []
    for metric, w in weights_by_metric.items():
        if metric not in scores_by_metric:
            if strict:
                raise KeyError(f"missing score for weighted metric {metric!r}")
            skipped.append((metric, "missing"))
            continue
        s = scores_by_metric[metric]
        if s is NA:
            skipped.append((metric, "N/A"))
            continue
        used[metric] = (float(s), float(w))

    total_w = sum(w for _, w in used.values())
    if total_w <= 0:
        return NA, {"note": "all metrics N/A", "skipped": skipped}
    score = sum(s * w for s, w in used.values()) / total_w
    return clip(score), {
        "applicable_weight_total": total_w,
        "renormalized": abs(total_w - sum(weights_by_metric.values())) > 1e-12,
        "used": {k: {"score": v[0], "weight": v[1]} for k, v in used.items()},
        "skipped": skipped,
    }


def mean_scores(values):
    """Unweighted arithmetic mean across a fixed workload set (spec 25.2),
    skipping N/A."""
    vals = [float(v) for v in values if v is not NA]
    return (sum(vals) / len(vals)) if vals else NA


def median(values):
    """Representative statistic for repeated timing/resource runs (spec 25.2)."""
    vals = sorted(float(v) for v in values if v is not NA)
    if not vals:
        return NA
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2.0


# --------------------------------------------------------------------------
# Semantic Compression composition (spec 6.1.6)
# --------------------------------------------------------------------------
SEMANTIC_QUALITY_WEIGHTS = {
    "semantic_density": 0.20,
    "semantic_determinacy": 0.25,
    "semantic_locality": 0.20,
    "hidden_semantic_cost": 0.20,
    "capability_efficiency": 0.15,
}


def semantic_quality(metric_scores):
    """Q = 0.20*Density + 0.25*Determinacy + 0.20*Locality + 0.20*HiddenCost
           + 0.15*CapabilityEfficiency"""
    q, detail = weighted(metric_scores, SEMANTIC_QUALITY_WEIGHTS)
    return q, detail


def semantic_compression_overall(q, c):
    """Harmonic mean of quality and coverage: 2QC/(Q+C); 0 when Q+C == 0.

    Fixed in advance so neither high quality with trivial coverage nor broad
    coverage with poor quality can dominate. Must not be replaced by a weighted
    arithmetic mean after results are known.
    """
    if q is NA or c is NA:
        return NA
    q, c = float(q), float(c)
    if q + c == 0:
        return 0.0
    return clip(2.0 * q * c / (q + c))


def capability_coverage(supported_points, total_points):
    """C = 100 * supported / total. The denominator is fixed before results and
    includes capabilities a language does not support (spec 6.1.5, 26)."""
    if not total_points:
        return NA
    return clip(100.0 * float(supported_points) / float(total_points))


# --------------------------------------------------------------------------
# Ranking and presentation
# --------------------------------------------------------------------------
def rank(scores_by_lang, ascending=False):
    """Rank languages by score. Ranking uses UNROUNDED values (spec 25.3);
    displayed rounding must not decide ties. N/A ranks last."""
    items = [(k, float(v)) for k, v in scores_by_lang.items() if v is not NA]
    items.sort(key=lambda kv: kv[1], reverse=not ascending)
    out = []
    prev_score = None
    prev_rank = 0
    for i, (k, v) in enumerate(items, start=1):
        # Ties share a rank. Comparison is on the UNROUNDED value (spec 25.3):
        # displayed two-decimal rounding must not decide a tie.
        if prev_score is not None and v == prev_score:
            r = prev_rank
        else:
            r = i
        out.append({"rank": r, "language": k, "score": v})
        prev_score, prev_rank = v, r
    for k, v in scores_by_lang.items():
        if v is NA:
            out.append({"rank": None, "language": k, "score": None, "note": "N/A"})
    return out


def fmt(score):
    """Two decimal places for display (spec 25.3); 'N/A' stays 'N/A'."""
    return "N/A" if score is NA else f"{float(score):.2f}"


if __name__ == "__main__":
    # Self-test the frozen formulas against hand-computed values.
    assert fmt(family_a(0.75)) == "75.00"
    assert fmt(family_a(1.4)) == "100.00"          # clamped
    assert fmt(family_b(0.25)) == "75.00"
    assert fmt(family_e(0)) == "100.00"
    assert fmt(family_e(1)) == "66.67"
    assert fmt(family_e(3)) == "0.00"
    assert fmt(family_f(["compile_time_detection", "silent_bug"])) == "50.00"

    sc, rep = family_c({"A": 1.0, "B": 2.0, "C": 10.0})
    assert fmt(sc["A"]) == "100.00" and fmt(sc["B"]) == "50.00" and fmt(sc["C"]) == "10.00"
    assert rep["compression_disclosure_required"] is False

    sc2, rep2 = family_c({"A": 1.0, "B": 500.0})
    assert rep2["compression_disclosure_required"] is True   # spans 500x

    sd, _ = family_d({"A": 2.0, "B": 4.0})
    assert fmt(sd["A"]) == "50.00" and fmt(sd["B"]) == "100.00"

    # N/A must not become zero; remaining weights renormalize.
    s, d = weighted({"x": 80.0, "y": NA}, {"x": 0.5, "y": 0.5})
    assert fmt(s) == "80.00" and d["renormalized"] is True

    # Harmonic mean behaviour
    assert fmt(semantic_compression_overall(100.0, 0.0)) == "0.00"
    assert fmt(semantic_compression_overall(80.0, 20.0)) == "32.00"
    assert fmt(semantic_compression_overall(0.0, 0.0)) == "0.00"

    print("score.py self-test: all frozen formulas OK")
