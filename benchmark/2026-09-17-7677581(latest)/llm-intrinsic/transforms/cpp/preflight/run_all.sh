#!/bin/sh
# Full pre-flight for the I1 C++ column. Every command is executed verbatim here.
set -u
cd "$(dirname "$0")/.."
echo "### toolchain"
clang++ --version | head -2
python3 --version
echo
echo "### PF-08a determinism: regenerate the mapping in two separate processes"
python3 gen_mapping.py > /dev/null; shasum -a 256 mapping.json
python3 gen_mapping.py > /dev/null; shasum -a 256 mapping.json
echo
echo "### PF-08b mapping invariants"
python3 - <<'PY'
import json,re
m=json.load(open('mapping.json'))
f=m['forward']; i=m['inverse']
ws=sorted(i)
assert len(f)==len(i)==9, "not one-to-one"
assert all(re.fullmatch(r'[a-z]{6}',w) for w in ws), "shape"
assert all(re.fullmatch(r'([bdfgklmnprstvz][aeiou]){3}',w) for w in ws), "CVCVCV"
pref=[(a,b) for a in ws for b in ws if a!=b and (a.startswith(b) or b.startswith(a))]
assert not pref, pref
assert all(f[i[w]]==w for w in ws), "round-trip of the map itself"
print("one-to-one:9  shape:^[a-z]{6}$ CVCVCV  prefix-free:yes  char-len: all 6  seed:",m['seed'])
PY
echo
echo "### PF-08c rejection filters can fire (self-test with planted candidates)"
python3 preflight/filter_selftest.py

echo
echo "### PF-01 fixture builds, runs, output exact"
clang++ -std=c++20 -O2 fixture_real.cpp -o preflight/artifacts/fixture_real.bin; echo "build exit=$?"
./preflight/artifacts/fixture_real.bin > preflight/artifacts/fixture_actual.txt 2> preflight/artifacts/fixture_actual.err; echo "run exit=$?"
diff fixture_expected.txt preflight/artifacts/fixture_actual.txt && echo "fixture stdout: byte-identical"
echo "fixture stderr bytes: $(wc -c < preflight/artifacts/fixture_actual.err)"
echo "pack-claimed fixture output vs actual:"
python3 - <<'PY'
s=open('reference_pack.md').read()
claimed=s.rsplit('```\n',3)[-2]
actual=open('fixture_expected.txt').read()
print("  identical" if claimed==actual else "  MISMATCH\n  claimed=%r\n  actual =%r"%(claimed,actual))
PY
echo
echo "### PF-01b reference solution builds, runs, produces the oracle"
clang++ -std=c++20 -O2 reference_solution.cpp -o preflight/artifacts/reference_solution.bin; echo "build exit=$?"
./preflight/artifacts/reference_solution.bin > preflight/artifacts/oracle_actual.txt 2> preflight/artifacts/oracle_actual.err; echo "run exit=$?"
diff expected_output.txt preflight/artifacts/oracle_actual.txt && echo "oracle stdout: byte-identical"
python3 preflight/artifacts/oracle_crosscheck.py > preflight/artifacts/oracle_crosscheck.txt
diff expected_output.txt preflight/artifacts/oracle_crosscheck.txt && echo "independent cross-check: identical"
echo
echo "### PF-02/PF-04 round-trip R1/R4/R5 on both real sources"
python3 validate.py roundtrip fixture_real.cpp
python3 validate.py roundtrip reference_solution.cpp
python3 forward.py fixture_real.cpp preflight/artifacts/rt_fixture_anon.cpp
python3 inverse.py preflight/artifacts/rt_fixture_anon.cpp preflight/artifacts/rt_fixture_back.cpp
cmp fixture_real.cpp preflight/artifacts/rt_fixture_back.cpp && echo "R1 fixture: byte-identical"
python3 forward.py reference_solution.cpp preflight/artifacts/rt_sol_anon.cpp
python3 inverse.py preflight/artifacts/rt_sol_anon.cpp preflight/artifacts/rt_sol_back.cpp
cmp reference_solution.cpp preflight/artifacts/rt_sol_back.cpp && echo "R1 solution: byte-identical"
echo "R2/R3: inverse(forward(F)) builds and reproduces F's output"
python3 validate.py build preflight/artifacts/rt_fixture_anon.cpp fixture_expected.txt
python3 validate.py build preflight/artifacts/rt_sol_anon.cpp expected_output.txt
echo
echo "### PF-05 validators: positive must PASS"
python3 validate.py gate fixture_anon.cpp
python3 validate.py gate reference_solution_anon.cpp
python3 validate.py all reference_solution_anon.cpp expected_output.txt
echo
echo "### PF-05 validators: negatives must REJECT"
for n in neg_real_ignores_transform neg_corrupt_letter neg_swap_roles neg_wrong_output; do
  python3 validate.py all preflight/negatives/$n.cpp expected_output.txt; echo "   exit=$?"
done
echo "  INVERSE_AMBIGUOUS negative (a pseudo-word used as a variable name):"
sed 's/litasa litasa total = 0;/litasa litasa kelugo = 0;/; s/\btotal\b/kelugo/g' reference_solution_anon.cpp > preflight/negatives/neg_ambiguous.cpp
python3 validate.py all preflight/negatives/neg_ambiguous.cpp expected_output.txt; echo "   exit=$?"
echo
echo "### PF-13 harness-convention sufficiency: bare-body submission, full pipeline"
python3 pipeline.py preflight/bare_body_submission.txt expected_output.txt preflight/artifacts/pf13; echo "exit=$?"
echo "### PF-05b conformance gate negative through the same pipeline"
python3 pipeline.py preflight/neg_real_submission.txt expected_output.txt preflight/artifacts/pf05neg; echo "exit=$?"
echo
echo "### PF-07 identity-leak scan of the Reference Pack"
python3 - <<'PY'
import re,json
s=open('reference_pack.md').read()
prose=re.sub(r'```.*?```','',s,flags=re.S); prose=re.sub(r'`[^`]*`','',prose)
m=json.load(open('mapping.json'))
bad={k:len(re.findall(r'(?<![A-Za-z0-9_])'+k+r'(?![A-Za-z0-9_])',prose)) for k in m['forward']}
bad={k:v for k,v in bad.items() if v}
print("  mapped real keywords in prose:", bad or "NONE")
allw=set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*',s))
print("  mapped real keywords anywhere in the pack:", sorted(set(m['forward'])&allw) or "NONE")
names=['C++','cpp','clang','gcc','g++','llvm','STL','libstdc','Stroustrup','cplusplus','.cpp',
       'Python','Rust','Java','Kotlin','Swift','Zig','Go','TypeScript','Quidra','compiler','toolchain']
print("  language/tool identity terms:", [n for n in names if re.search(r'(?<![A-Za-z0-9_+])'+re.escape(n)+r'(?![A-Za-z0-9_+])',s,re.I)] or "NONE")
print("  UNTRANSFORMED library surface present by design (I1 residual):",
      sorted(t for t in ['std','cout','endl','iostream','string','to_string'] if t in allw))
print("  lines:",s.count('\n')+(0 if s.endswith('\n') else 1)," chars:",len(s))
PY
echo
echo "### file hashes"
shasum -a 256 mapping.json forward.py inverse.py lex_cpp.py validate.py pipeline.py gen_mapping.py \
  fixture_real.cpp fixture_anon.cpp fixture_expected.txt reference_solution.cpp \
  reference_solution_anon.cpp expected_output.txt reference_pack.md \
  wordlists/en_common.txt wordlists/prog_terms.txt wordlists/reserved_union.txt
