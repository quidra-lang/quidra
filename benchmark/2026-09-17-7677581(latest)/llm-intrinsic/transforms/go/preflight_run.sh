#!/bin/bash
# Pre-flight evidence producer for I1 / language_id = go.
# Every line of preflight.md that claims a result is produced by this script.
set -u
cd "$(dirname "$0")"
D=$(pwd)
EV="$D/preflight_evidence"
rm -rf "$EV"; mkdir -p "$EV"

say() { printf '\n===== %s =====\n' "$1"; }

say "PF-A0  toolchain identity"
go version
python3 --version

say "PF-A1  the real fixture builds with the frozen recipe"
set -x
go build -o "$EV/fixture_real.bin" "$D/fixture_real.go"
echo "build exit=$?"
set +x

say "PF-A2  the real fixture runs, exit status and stdout"
"$EV/fixture_real.bin" > "$EV/fixture_real.out"; echo "run exit=$?"
cat "$EV/fixture_real.out"

say "PF-A3  stdout is byte-identical to expected_output.txt (the oracle)"
cmp "$EV/fixture_real.out" "$D/expected_output.txt" && echo "cmp: IDENTICAL"

say "PF-A4  the oracle was produced by execution, and an independent implementation agrees"
python3 - <<'PY' > "$EV/independent.out"
s = 7
terms = []
for _ in range(50):
    s = (s * 48271) % 2147483647
    terms.append(s % 1000)
print("SUM " + str(sum(terms)))
print("MAX " + str(max(terms)))
print("EVENS " + str(sum(1 for t in terms if t % 2 == 0)))
print("JOINED " + "-".join(str(t) for t in terms[:5]))
PY
cat "$EV/independent.out"
cmp "$EV/independent.out" "$D/expected_output.txt" && echo "cmp: IDENTICAL (second, independent implementation)"

say "PF-A5  the pack's worked example is byte-identical to fixture_anon.go"
python3 - <<'PY'
import re, io, sys
pack = open("reference_pack.md", encoding="utf-8").read()
anon = open("fixture_anon.go", encoding="utf-8").read()
blocks = re.findall(r"```\n(.*?)```", pack, re.S)
hit = [b for b in blocks if b.startswith("nemetu lemuma\n\npitudo")]
assert len(hit) == 1, "expected exactly one full program in the pack, found %d" % len(hit)
print("pack example == fixture_anon.go :", hit[0] == anon)
sys.exit(0 if hit[0] == anon else 1)
PY

say "PF-A6  the four lines the pack CLAIMS are byte-identical to what the program PRINTS"
python3 - <<'PY'
import re, sys
pack = open("reference_pack.md", encoding="utf-8").read()
blocks = re.findall(r"```\n(.*?)```", pack, re.S)
claimed = [b for b in blocks if b.startswith("SUM ")]
assert len(claimed) == 1, "expected exactly one claimed-output block"
actual = open("expected_output.txt", encoding="utf-8").read()
print("claimed:\n" + claimed[0] + "actual:\n" + actual)
print("claimed == actual :", claimed[0] == actual)
sys.exit(0 if claimed[0] == actual else 1)
PY

say "PF-B  harness-convention sufficiency (bare-body submission through the real pipeline)"
# The submission contains EVERY element the pack documents (unit line, module lines,
# entry point) and NOTHING the pack withholds (no file name, no build command, no
# output directory).  It arrives wrapped in a fenced block, as a model would emit it.
{ echo 'Here is the program.'; echo; echo '```'; cat "$D/fixture_anon.go"; echo '```'; } > "$EV/model_output.txt"
python3 - <<'PY'
# section 9.2 code extraction: content of the LAST fenced block, language tag stripped
import re
raw = open("preflight_evidence/model_output.txt", encoding="utf-8").read()
blocks = re.findall(r"```[a-zA-Z0-9_+-]*\n(.*?)```", raw, re.S)
open("preflight_evidence/extracted.txt", "w", encoding="utf-8").write(blocks[-1])
print("extracted %d bytes from the last of %d fenced block(s)" % (len(blocks[-1]), len(blocks)))
PY
echo "--- section 4.2a conformance gate on the raw pre-inverse submission ---"
python3 validate.py gate "$EV/extracted.txt"
echo "--- inverse mapping (no position map: this is model output) ---"
python3 inverse.py "$EV/extracted.txt" -o "$EV/fixup_H1_solution.go"
echo "--- fixup H1: the HARNESS supplies the entry file name; the trial never states it ---"
mkdir -p "$EV/build"; cp "$EV/fixup_H1_solution.go" "$EV/build/solution.go"
echo "harness wrote: $EV/build/solution.go"
echo "--- fixups applied: H1 (file name) and H6 (output path). Pack-documented fixups: 0 ---"
echo "--- frozen build recipe, supplied by the harness, not by the trial ---"
( set -x; go build -o "$EV/build/BIN" "$EV/build/solution.go" )
echo "build exit=$?"
"$EV/build/BIN" > "$EV/build/out.txt"; echo "run exit=$?"
cat "$EV/build/out.txt"
cmp "$EV/build/out.txt" "$D/expected_output.txt" && echo "ORACLE: PASS (byte-identical, exit 0)"

say "PF-B2 an INDEPENDENT solution, written only from the pack, also passes"
# Not derived from the worked example: different identifier names, a different
# accumulation order, and `!=` / `<=` in place of `>` / `<`.  If the pack were
# under-specified this is where it would show.
cat > "$EV/independent_submission.txt" <<'SUB'
```
nemetu lemuma

pitudo "fmt"
pitudo "strconv"

mizufi lemuma () {
	vuzuvi seed nebala = 7
	vuzuvi acc nebala = 0
	vuzuvi peak nebala = 0
	vuzuvi even_count nebala = 0
	vuzuvi head sopona = ""
	vuzuvi k nebala = 0
	lunuso k = 1; k <= 50; k = k + 1 {
		seed = seed * 48271 % 2147483647
		vuzuvi v nebala = seed % 1000
		acc = acc + v
		feguzi peak < v {
			peak = v
		}
		feguzi v % 2 != 1 {
			even_count = even_count + 1
		}
		feguzi k <= 5 {
			feguzi k != 1 {
				head = head + "-"
			}
			head = head + strconv.FormatInt(v, 10)
		}
	}
	fmt.Println("SUM " + strconv.FormatInt(acc, 10))
	fmt.Println("MAX " + strconv.FormatInt(peak, 10))
	fmt.Println("EVENS " + strconv.FormatInt(even_count, 10))
	fmt.Println("JOINED " + head)
}
```
SUB
python3 - <<'PY'
import re
raw = open("preflight_evidence/independent_submission.txt", encoding="utf-8").read()
b = re.findall(r"```[a-zA-Z0-9_+-]*\n(.*?)```", raw, re.S)[-1]
open("preflight_evidence/independent_extracted.txt", "w", encoding="utf-8").write(b)
PY
python3 validate.py gate "$EV/independent_extracted.txt"
python3 inverse.py "$EV/independent_extracted.txt" -o "$EV/build2_solution.go"
mkdir -p "$EV/build2"; cp "$EV/build2_solution.go" "$EV/build2/solution.go"
( set -x; go build -o "$EV/build2/BIN" "$EV/build2/solution.go" ); echo "build exit=$?"
"$EV/build2/BIN" > "$EV/build2/out.txt"; echo "run exit=$?"
cat "$EV/build2/out.txt"
cmp "$EV/build2/out.txt" "$D/expected_output.txt" && echo "ORACLE: PASS (independent solution, byte-identical, exit 0)"

say "PF-C  validator: positive AND negative, every check"
python3 validate.py selftest
echo "selftest exit=$?"

say "PF-D  identity-leak scan of the Reference Pack"
python3 leakscan.py

say "PF-E  mapping invariants"
python3 - <<'PY'
import json
m = json.load(open("mapping.json"))
pw = [e["pseudo"] for e in m["mapping"].values()]
real = [e["real"] for e in m["mapping"].values()]
print("seed recorded in mapping.json :", m["seed"])
print("tokens mapped                 :", len(pw))
print("one-to-one                    :", len(set(pw)) == len(pw) == len(set(real)))
print("all exactly 6 ASCII lowercase :", all(len(w) == 6 and w.isalpha() and w.islower() for w in pw))
print("comparable length (min,max)   :", (min(map(len, pw)), max(map(len, pw))))
print("no pseudo-word is a prefix of another :",
      not any(a != b and (a.startswith(b) or b.startswith(a)) for a in pw for b in pw))
src = open("fixture_real.go", encoding="utf-8").read()
print("no pseudo-word collides with any identifier or literal in the fixture :",
      not any(w in src for w in pw))
PY

say "PF-F  round-trip on the shipped fixture, checks R1-R5"
python3 validate.py roundtrip

say "DONE"
