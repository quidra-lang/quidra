#!/bin/sh
# Mandatory pre-flight validation, methodology 10 section 10.4 / 10.1.
# Language: zig   Condition: I1   Seed: 20260918
#
# Every command below is executed verbatim and its output captured into
# preflight_evidence/.  Nothing here summarises; it runs.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
EV="$HERE/preflight_evidence"
LOG="$HERE/preflight_evidence_log.txt"
rm -rf "$EV" "$HERE/.work"
mkdir -p "$EV"
: > "$LOG"

run() {
  echo ""                                        | tee -a "$LOG"
  echo "\$ $*"                                   | tee -a "$LOG"
  ( cd "$HERE" && "$@" ) 2>&1                    | tee -a "$LOG"
  rc=${?}
  echo "[exit $rc]"                              | tee -a "$LOG"
  return $rc
}

FAIL=0

echo "=== environment ===" | tee -a "$LOG"
run zig version                       || FAIL=1
run python3 --version                 || FAIL=1
run shasum -a 256 "$HERE/wordlists/en_common.txt" "$HERE/wordlists/prog_terms.txt" \
                  "$HERE/wordlists/reserved_union.txt" "$HERE/wordlists/reserved_zig.txt" \
                  "$HERE/mapping.json" "$HERE/fixture_real.zig" "$HERE/fixture_anon.zig" \
                  "$HERE/expected_output.txt" "$HERE/reference_pack.md" || FAIL=1

echo "" | tee -a "$LOG"
echo "=== PF-01 / 10.4(a): the fixture builds, runs, output matches ===" | tee -a "$LOG"
mkdir -p "$HERE/.work/pf01"
cp "$HERE/fixture_real.zig" "$HERE/.work/pf01/solution.zig"
run sh -c "cd '$HERE/.work/pf01' && zig build-exe solution.zig -O ReleaseSafe -femit-bin=solution" || FAIL=1
run sh -c "cd '$HERE/.work/pf01' && ./solution > stdout.txt; echo exit=\$?; cat stdout.txt" || FAIL=1
run sh -c "diff -u '$HERE/expected_output.txt' '$HERE/.work/pf01/stdout.txt' && echo 'stdout byte-identical to expected_output.txt'" || FAIL=1
run python3 "$HERE/pack_check.py" || FAIL=1

echo "" | tee -a "$LOG"
echo "=== PF-07: identity-leak scan of the Reference Pack ===" | tee -a "$LOG"
run python3 "$HERE/leakscan.py" || FAIL=1

echo "" | tee -a "$LOG"
echo "=== PF-02 / PF-04 / PF-05 / PF-08: transformers, validators, lexicalizer ===" | tee -a "$LOG"
run python3 "$HERE/lex10.py" "$HERE/fixture_real.zig" "$HERE/fixture_anon.zig" || FAIL=1
run python3 "$HERE/validate.py" --evidence preflight_evidence --work "$HERE/.work" || FAIL=1

echo "" | tee -a "$LOG"
echo "=== 10.4(b) / PF-13: harness conventions, extraction, bare-body submission ===" | tee -a "$LOG"
run python3 "$HERE/harness.py" --extractor --pf13 --evidence preflight_evidence --work "$HERE/.work" || FAIL=1

echo "" | tee -a "$LOG"
if [ "$FAIL" -eq 0 ]; then
  echo "PRE-FLIGHT: all_pass = true"  | tee -a "$LOG"
else
  echo "PRE-FLIGHT: all_pass = FALSE -- the trial is BLOCKED" | tee -a "$LOG"
fi
exit "$FAIL"
