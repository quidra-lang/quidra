#!/bin/bash
# Full pre-flight evidence run for I1 / kotlin / seed 20260918.
# Every command it executes and every byte those commands print is captured under preflight/.
set -u
cd "$(dirname "$0")"
mkdir -p preflight
: > preflight/commands.txt
rc_total=0

run () {
  echo "\$ $*" | tee -a preflight/commands.txt
  "$@" > "preflight/$LOG.out" 2> "preflight/$LOG.err"
  rc=$?
  echo "exit=$rc" >> preflight/commands.txt
  cat "preflight/$LOG.out"
  [ -s "preflight/$LOG.err" ] && { echo "--- stderr ---"; cat "preflight/$LOG.err"; }
  rc_total=$((rc_total + rc))
  return 0
}

echo "== toolchain =="
LOG=toolchain run kotlinc -version
LOG=toolchain_rt run java -version

echo; echo "== lexer losslessness (concatenating every token reproduces the source) =="
LOG=pf_lex run python3 lextest.py

echo; echo "== PF-03 section 7.1a domain recomputed mechanically =="
LOG=pf_domain run python3 domaincheck.py

echo; echo "== (a) fixture builds, runs, output matches the pack exactly =="
LOG=pf_a run python3 packcheck.py

echo; echo "== (b) harness conventions: pack-only submission through the real pipeline =="
LOG=pf_b1 run python3 harness.py pipeline/bare_body_submission.md --expected expected_output.txt
echo "-- and a submission that ignores the transformation must NOT reach the toolchain --"
echo "\$ python3 harness.py pipeline/real_source_submission.md --expected expected_output.txt" | tee -a preflight/commands.txt
python3 harness.py pipeline/real_source_submission.md --expected expected_output.txt \
  > preflight/pf_b2.out 2> preflight/pf_b2.err
rc=$?
echo "exit=$rc (nonzero is the REQUIRED outcome here)" >> preflight/commands.txt
cat preflight/pf_b2.out
if [ "$rc" -eq 0 ]; then echo "PF-b2 FAILED: a real-source submission was accepted"; rc_total=$((rc_total+1));
else echo "PF-b2 OK: rejected with exit=$rc before any build"; fi

echo; echo "== (c) validator accepts a correct input AND rejects a corrupted one =="
LOG=pf_c run python3 validate.py

echo; echo "== leak scan of the Reference Pack =="
LOG=pf_leak run python3 leakscan.py

echo; echo "== lexicalizer determinism (separate process) =="
cp mapping.json preflight/mapping.before.json
LOG=pf_det run python3 lexicalize.py
if cmp -s preflight/mapping.before.json mapping.json; then
  echo "DETERMINISM: byte-identical across processes"
else
  echo "DETERMINISM: FAILED"; rc_total=$((rc_total+1))
fi

echo; echo "== artifact hashes =="
shasum -a 256 mapping.json fixture_real.kt fixture_anon.kt expected_output.txt \
  reference_pack.md forward.py inverse.py ktlex.py lexicalize.py validate.py harness.py \
  domaincheck.py packcheck.py leakscan.py lextest.py wordlists/*.txt | tee preflight/hashes.txt

echo
echo "PREFLIGHT_TOTAL_NONZERO_EXITS=$rc_total"
exit $rc_total
