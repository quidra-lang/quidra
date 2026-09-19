#!/bin/sh
# Reproduces every pre-flight check for the I1 'rust' column. Exit 0 iff all pass.
set -u
W=$(cd "$(dirname "$0")" && pwd)
cd "$W"
fail=0
say() { printf '%s\n' "$*"; }
chk() { # chk <expected-exit> <label> <cmd...>
  want=$1; label=$2; shift 2
  "$@" >/dev/null 2>&1; got=$?
  if [ "$got" -eq "$want" ]; then say "PASS  $label"; else say "FAIL  $label (exit $got, wanted $want)"; fail=1; fi
}
say "--- determinism of the mapping ---"
python3 lexicalize.py --out /tmp/_m.json 2>/dev/null
chk 0 "mapping.json regenerates byte-identically" cmp /tmp/_m.json mapping.json
say "--- forward transform is stable ---"
python3 forward.py --in fixture_real.rs --out /tmp/_a.rs --posmap /tmp/_p.json 2>/dev/null
chk 0 "fixture_anon.rs regenerates byte-identically" cmp /tmp/_a.rs fixture_anon.rs
say "--- validator positive ---"
chk 0 "correct anonymized fixture is ACCEPTED" python3 validate.py --source fixture_anon.rs --posmap fixture_anon.posmap.json
say "--- validator negatives ---"
for f in N1_real_source_ignoring_transformation.rs N2_wrong_pseudoword.rs \
         N3_unmapped_word.rs N4_builds_but_wrong_output.rs N6_identifier_is_a_pseudoword.rs; do
  chk 1 "$f is REJECTED" python3 validate.py --source "preflight_evidence/negatives/$f" --no-r1
done
chk 1 "N5_mapping_not_one_to_one.json is REJECTED" python3 validate.py \
    --source fixture_anon.rs --posmap fixture_anon.posmap.json \
    --mapping preflight_evidence/negatives/N5_mapping_not_one_to_one.json --no-r1
say "--- harness convention sufficiency ---"
chk 0 "bare-body submission passes the full pipeline" python3 harness.py --submission preflight_evidence/bare_body_submission.txt
chk 1 "real source ignoring the transformation is gated out" python3 harness.py --submission preflight_evidence/negative_submission_real_source.txt
say ""
if [ "$fail" -eq 0 ]; then say "all_pass: true"; else say "all_pass: false"; fi
exit $fail
