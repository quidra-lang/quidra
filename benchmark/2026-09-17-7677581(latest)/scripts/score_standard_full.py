#!/usr/bin/env python3
"""Standard Overall Score: all five categories, fixed weights from spec 8.1."""
import json, glob, os, sys, collections
D = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, D)
import score as S
RUN = os.path.dirname(D)
ORDER = ["Quidra","Python","C++","Rust","Go","Java","TypeScript","Kotlin","Swift","Zig"]
DISP = {"quidra":"Quidra","python":"Python","cpp":"C++","rust":"Rust","go":"Go","java":"Java",
        "typescript":"TypeScript","kotlin":"Kotlin","swift":"Swift","zig":"Zig"}

# --- measured performance/resource metrics ---
meas = json.load(open(f"{RUN}/standard/scores/standard_measured_scores.json"))["table"]

# --- rubrics ---
rub = {}
for f in glob.glob(f"{RUN}/standard/raw/rubrics/*.json"):
    try: d = json.load(open(f))
    except Exception: continue
    sc = d.get("scores") or {}
    v = {k: (x.get("score") if isinstance(x, dict) else x) for k, x in sc.items()}
    v = {k: x for k, x in v.items() if isinstance(x, (int, float))}
    if v and d.get("metric"): rub[d["metric"]] = v

# --- adversarial -> Early Error Detection + the safety family ---
per = collections.defaultdict(list); sb = collections.defaultdict(list)
for f in glob.glob(f"{RUN}/standard/raw/adversarial/*/chunk_*.json"):
    lang = os.path.basename(os.path.dirname(f))
    if lang not in DISP: continue
    try: d = json.load(open(f))
    except Exception: continue
    for o in d.get("observations", []):
        k = str(o.get("earliest_observable_stage","")).strip().lower().replace(" ","_").replace("-","_")
        if k in S.STAGE_SCORES:
            per[DISP[lang]].append(S.STAGE_SCORES[k])
            sb[DISP[lang]].append(1 if k == "silent_bug" else 0)
eed = {l: (sum(v)/len(v) if v else None) for l, v in per.items()}
sbr = {l: S.family_b(sum(v)/len(v)) if v else None for l, v in sb.items()}

CATS = {
 "Performance": [meas.get("native_performance"), meas.get("interactive_performance"),
                 meas.get("long_running_performance"), meas.get("compile_build_performance")],
 "Resource": [meas.get("memory_efficiency"), meas.get("binary_artifact_size")],
 "LanguageDev": [rub.get("Readability"), rub.get("Functionality / Expressiveness"),
                 rub.get("Diagnostics"), rub.get("Dependency Simplicity"),
                 rub.get("Portability"), rub.get("FFI / Interoperability"), rub.get("Concurrency")],
 "Safety": [eed, sbr],
 "Ecosystem": [rub.get("Ecosystem Breadth"), rub.get("Tooling"), rub.get("Library Availability"),
               rub.get("Package / Dependency Management"), rub.get("IDE / Editor Support"),
               rub.get("Debugger / Profiler Support"), rub.get("Build / Test Integration"),
               rub.get("Production Adoption / Deployment Evidence"),
               rub.get("Documentation / Community"),
               rub.get("Toolchain Stability / Release Maturity")],
}
W = {"Performance":0.20,"Resource":0.15,"LanguageDev":0.20,"Safety":0.25,"Ecosystem":0.20}

cat = {}
for name, metrics in CATS.items():
    cat[name] = {}
    for l in ORDER:
        vals = [m.get(l) for m in metrics if isinstance(m, dict) and m.get(l) is not None]
        cat[name][l] = (sum(vals)/len(vals)) if vals else None

overall = {}
for l in ORDER:
    sc = {k: cat[k].get(l) for k in W}
    v, _ = S.weighted(sc, W, strict=False)
    overall[l] = v

print("Standard 総合スコア（仕様8.1の固定重み）")
hdr = f"{'言語':12s}" + "".join(f"{k[:9]:>11s}" for k in W) + f"{'総合':>9s}"
print(hdr); print("-"*len(hdr))
for l in sorted(ORDER, key=lambda x: -(overall[x] or 0)):
    row = f"{l:12s}" + "".join(f"{S.fmt(cat[k].get(l)):>11s}" for k in W)
    print(row + f"{S.fmt(overall[l]):>9s}")
print()
print("Standard ランキング")
for r in S.rank(overall):
    print(f"  {r['rank']:2d}. {r['language']:12s} {S.fmt(r['score'])}")
json.dump({"categories":cat,"weights":W,"overall":overall,
           "early_error_detection":eed,"silent_bug_resistance":sbr},
          open(f"{RUN}/standard/scores/standard_overall.json","w"), indent=1)
