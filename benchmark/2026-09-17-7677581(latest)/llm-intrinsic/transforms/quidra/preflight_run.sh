#!/bin/zsh
# Regenerates every piece of pre-flight evidence for the I1/quidra unit.
# Usage: ./preflight_run.sh  (writes preflight_evidence.txt, exits non-zero on any failure)
set -u
cd "$(dirname "$0")"
QUIDRA_BIN=${QUIDRA_BIN:-/Users/koba/Desktop/Quidra/quidra/build/quidra}
export QUIDRA_BIN
rc=0
run() { echo; echo "\$ $*"; "$@" || rc=1; }

echo "=== toolchain ==="
run $QUIDRA_BIN --version

echo
echo "=== determinism of the mapping (two separate processes) ==="
cp mapping.json build/map_a.json
run python3 genmap.py
cp mapping.json build/map_b.json
run cmp build/map_a.json build/map_b.json
run shasum -a 256 mapping.json wordlists/en_common.txt wordlists/prog_terms.txt wordlists/reserved_union.txt

echo
echo "=== filter self-test (every acceptance filter can reject) ==="
run python3 filtertest.py

echo
echo "=== PF-a1: oracle produced by execution ==="
run $QUIDRA_BIN build oracle_real.qui -o build/oracle_real
./build/oracle_real > build/oracle_stdout.txt; echo "exit=$?"
run cmp build/oracle_stdout.txt expected_output.txt

echo
echo "=== PF-a2: the pack's worked fixture builds, runs, output matches the pack ==="
run python3 packcheck.py

echo
echo "=== PF-b: harness conventions (positive: pack-only submission) ==="
run python3 harness_sim.py sim/raw_submission.md expected_output.txt

echo
echo "=== PF-b negative: submission that ignores the transformation must be rejected ==="
echo "\$ python3 harness_sim.py sim/raw_submission_ignores_rules.md expected_output.txt"
if python3 harness_sim.py sim/raw_submission_ignores_rules.md expected_output.txt; then
  echo "UNEXPECTED: the gate accepted real source"; rc=1
else
  echo "rejected as required"
fi

echo
echo "=== PF-c positive 1: fixture round trip ==="
run python3 forward.py fixture_real.qui build/fixture_anon_check.qui
run cmp build/fixture_anon_check.qui fixture_anon.qui
run python3 validate.py fixture_real.qui fixture_anon.qui fixture_expected.txt

echo
echo "=== PF-c positive 2: task solution round trip ==="
run python3 forward.py oracle_real.qui build/oracle_anon.qui
run python3 validate.py oracle_real.qui build/oracle_anon.qui expected_output.txt

echo
echo "=== PF-c positive 3: stress round trip (literals, comments, member names) ==="
run python3 forward.py build/stress_real.qui build/stress_anon.qui
run python3 inverse.py build/stress_anon.qui build/stress_rt.qui
run cmp build/stress_real.qui build/stress_rt.qui

echo
echo "=== PF-c negatives: every corrupted input must be REJECTED ==="
for n in N1_real_keyword N2_swapped_map N3_typo N4_ignores_transformation N5_literal; do
  echo
  echo "\$ python3 validate.py fixture_real.qui negatives/$n.qui fixture_expected.txt"
  if python3 validate.py fixture_real.qui negatives/$n.qui fixture_expected.txt; then
    echo "UNEXPECTED: validator accepted $n"; rc=1
  else
    echo "rejected as required"
  fi
done

echo
echo "=== identity-leak scan of the Reference Pack ==="
run python3 leakscan.py reference_pack.md

echo
echo "=== OVERALL: $([ $rc -eq 0 ] && echo ALL_PASS || echo FAILURES_PRESENT) ==="
exit $rc
