#!/usr/bin/env python3
"""
Authoritative re-tokenization pass over every semantic-compression probe.

Required by CORRECTIONS D-4. Probe annotations were produced by agents that
invoked the tokenizer at different times, and the interpolation-hole defect was
fixed mid-run. Token counts recorded inline in annotations are therefore not
mutually comparable.

This pass re-tokenizes EVERY probe file in ONE run with ONE version of the
tokenizer, so that every language's Semantic Density denominator is produced by
the same instrument. Counts written here are the values used for scoring; any
count recorded inline in an annotation is superseded.
"""

from __future__ import annotations

import json
import os
import re
import sys

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import tokenize_probe as tk  # noqa: E402

RUN = os.path.dirname(D)
PROBES = os.path.join(RUN, "semantic-compression", "probes")
OUT = os.path.join(RUN, "semantic-compression", "raw", "tokens")

# probe-directory name -> tokenizer profile key
LANG_DIRS = {
    "quidra": "quidra", "python": "python", "cpp": "cpp", "rust": "rust",
    "go": "go", "java": "java", "typescript": "typescript", "kotlin": "kotlin",
    "swift": "swift", "zig": "zig",
}

DISPLAY = {
    "quidra": "Quidra", "python": "Python", "cpp": "C++", "rust": "Rust", "go": "Go",
    "java": "Java", "typescript": "TypeScript", "kotlin": "Kotlin", "swift": "Swift",
    "zig": "Zig",
}

# Only a language's own source files are probes. Agents legitimately create
# fixture files (a data.txt for a file-I/O probe); those are inputs, not probes,
# and tokenizing them would be a category error.
SRC_EXT = {
    "quidra": ".qui", "python": ".py", "cpp": ".cpp", "rust": ".rs", "go": ".go",
    "java": ".java", "typescript": ".ts", "kotlin": ".kt", "swift": ".swift",
    "zig": ".zig",
}


def main():
    if not os.path.isdir(PROBES):
        print(f"no probe directory at {PROBES}", file=sys.stderr)
        return 1

    results, failures, skipped = {}, [], []
    script_sha = tk._sha256(os.path.join(D, "tokenize_probe.py"))
    profile_sha = tk._sha256(tk.PROFILE_PATH)

    for ldir in sorted(os.listdir(PROBES)):
        src_dir = os.path.join(PROBES, ldir)
        if not os.path.isdir(src_dir) or ldir not in LANG_DIRS:
            continue
        lang = LANG_DIRS[ldir]
        os.makedirs(os.path.join(OUT, ldir), exist_ok=True)
        per_lang = {}

        for fn in sorted(os.listdir(src_dir)):
            path = os.path.join(src_dir, fn)
            if not os.path.isfile(path):
                continue
            if not fn.endswith(SRC_EXT[lang]):
                skipped.append({"language": ldir, "file": fn,
                                "reason": "not a source file for this language (fixture/input)"})
                continue
            stem_only = os.path.splitext(fn)[0]
            # A probe file is named <PROBE_ID>.<ext>. Anything else is a helper /
            # second translation unit for a multi-unit probe (families 14, 18, 20),
            # whose MEASURED region lives in the probe file itself. Helpers are
            # recorded, never silently dropped -- and a file that DOES carry a probe
            # id must still have markers, so a malformed probe still fails loudly.
            if not re.fullmatch(r"F\d{2}\.P\d+", stem_only):
                skipped.append({"language": ldir, "file": fn,
                                "reason": "helper / second translation unit, not a probe file"})
                continue
            probe_id = os.path.splitext(fn)[0]
            try:
                res = tk.count_file(lang, path, probe_id=None, use_markers=True)
            except Exception as e:  # noqa: BLE001
                failures.append({"language": ldir, "probe": probe_id, "file": path,
                                 "error": str(e)})
                continue
            res["probe_id"] = res.get("probe_id") or probe_id
            res["source_file"] = path
            res["retokenized_by"] = "retokenize_all.py (CORRECTIONS D-4)"
            with open(os.path.join(OUT, ldir, f"{res['probe_id']}.json"), "w") as f:
                json.dump(res, f, indent=1)
            per_lang[res["probe_id"]] = res["count"]

        results[ldir] = per_lang

    summary = {
        "pass": "authoritative re-tokenization (CORRECTIONS D-4)",
        "tokenizer_script_sha256": script_sha,
        "token_profiles_sha256": profile_sha,
        "languages": {k: {"probes_tokenized": len(v), "total_tokens": sum(v.values()),
                          "counts": v} for k, v in results.items()},
        "failures": failures,
        "skipped_non_source_files": skipped,
    }
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "_retokenization_summary.json"), "w") as f:
        json.dump(summary, f, indent=1)

    print(f"tokenizer sha256 : {script_sha[:16]}…")
    print(f"profiles sha256  : {profile_sha[:16]}…")
    print()
    print(f"{'language':12s} {'probes':>7s} {'tokens':>8s} {'mean':>7s}")
    for ldir, v in sorted(results.items()):
        n, tot = len(v), sum(v.values())
        print(f"{DISPLAY.get(ldir, ldir):12s} {n:>7d} {tot:>8d} {(tot / n if n else 0):>7.1f}")
    if failures:
        print()
        print(f"FAILURES ({len(failures)}) — these must be resolved before scoring:")
        for f in failures[:20]:
            print(f"  {f['language']}/{f['probe']}: {f['error'][:150]}")
    else:
        print("\nno tokenizer failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
