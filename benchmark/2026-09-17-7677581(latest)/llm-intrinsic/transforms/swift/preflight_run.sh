#!/bin/bash
# Pre-flight evidence producer for I1 / language_id = swift.
# Every line of preflight.md that claims a result is produced by this script.
set -u
cd "$(dirname "$0")"
D=$(pwd)
EV="$D/preflight_evidence"
rm -rf "$EV"; mkdir -p "$EV"

say() { printf '\n===== %s =====\n' "$1"; }

say "PF-A0  toolchain identity"
swiftc --version
python3 --version

say "PF-A1  the real fixture builds with the frozen intrinsic-track recipe"
set -x
swiftc -O "$D/fixture_real.swift" -o "$EV/fixture_real.bin"
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

say "PF-A5  the pack's worked example is byte-identical to fixture_anon.swift"
python3 - <<'PY'
import re, sys
pack = open("reference_pack.md", encoding="utf-8").read()
anon = open("fixture_anon.swift", encoding="utf-8").read()
blocks = re.findall(r"```\n(.*?)```", pack, re.S)
hit = [b for b in blocks if b.startswith("bukuda state: derozi = 7\n")]
assert len(hit) == 1, "expected exactly one full program in the pack, found %d" % len(hit)
print("pack example == fixture_anon.swift :", hit[0] == anon)
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
# The submission contains EVERY element the pack documents and NOTHING the pack
# withholds (no file name, no build command, no output path).  It arrives wrapped
# in a fenced block, as a model would emit it.
{ echo 'Here is the program.'; echo; echo '```'; cat "$D/fixture_anon.swift"; echo '```'; } > "$EV/model_output.txt"
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
python3 inverse.py "$EV/extracted.txt" --report-ambiguous -o "$EV/fixup_H1_solution.swift"
echo "--- fixup H1: the HARNESS supplies the entry file name; the trial never states it ---"
mkdir -p "$EV/build"; cp "$EV/fixup_H1_solution.swift" "$EV/build/solution.swift"
echo "harness wrote: $EV/build/solution.swift"
echo "--- fixups applied: H1 (file name) and H6 (output path). Pack-documented fixups: 0 ---"
echo "--- frozen build recipe, supplied by the harness, not by the trial ---"
( set -x; swiftc -O "$EV/build/solution.swift" -o "$EV/build/BIN" )
echo "build exit=$?"
"$EV/build/BIN" > "$EV/build/out.txt"; echo "run exit=$?"
cat "$EV/build/out.txt"
cmp "$EV/build/out.txt" "$D/expected_output.txt" && echo "ORACLE: PASS (byte-identical, exit 0)"

say "PF-B2 an INDEPENDENT solution, written only from the pack, also passes"
# Not derived from the worked example: different identifier names, the run walked
# from 1 to 50 rather than 0 to 49, `<=` and `!=` in place of `<` and `>`, and the
# generator step written without its parentheses so that the pack's precedence
# table is actually relied on.
cat > "$EV/independent_submission.txt" <<'SUB'
Here is my solution.

```
bukuda seed: derozi = 7
bukuda acc: derozi = 0
bukuda peak: derozi = 0
bukuda even_count: derozi = 0
bukuda head: radipi = ""
vumodu k piripo 1..<51 {
    seed = seed * 48271 % 2147483647
    tutovo v: derozi = seed % 1000
    acc = acc + v
    bigoze peak < v {
        peak = v
    }
    bigoze v % 2 != 1 {
        even_count = even_count + 1
    }
    bigoze k <= 5 {
        bigoze k != 1 {
            head = head + "-"
        }
        head = head + "\(v)"
    }
}
print("SUM \(acc)")
print("MAX \(peak)")
print("EVENS \(even_count)")
print("JOINED \(head)")
```
SUB
python3 - <<'PY'
import re
raw = open("preflight_evidence/independent_submission.txt", encoding="utf-8").read()
b = re.findall(r"```[a-zA-Z0-9_+-]*\n(.*?)```", raw, re.S)[-1]
open("preflight_evidence/independent_extracted.txt", "w", encoding="utf-8").write(b)
print("extracted %d bytes" % len(b))
PY
python3 validate.py gate "$EV/independent_extracted.txt"
python3 inverse.py "$EV/independent_extracted.txt" -o "$EV/build2_solution.swift"
mkdir -p "$EV/build2"; cp "$EV/build2_solution.swift" "$EV/build2/solution.swift"
( set -x; swiftc -O "$EV/build2/solution.swift" -o "$EV/build2/BIN" ); echo "build exit=$?"
"$EV/build2/BIN" > "$EV/build2/out.txt"; echo "run exit=$?"
cat "$EV/build2/out.txt"
cmp "$EV/build2/out.txt" "$D/expected_output.txt" && echo "ORACLE: PASS (independent solution, byte-identical, exit 0)"

say "PF-B3 code extraction with ZERO, ONE and SEVERAL fenced blocks (section 9.2)"
python3 - <<'PY'
import re
def extract(raw):
    blocks = re.findall(r"```[a-zA-Z0-9_+-]*\n(.*?)```", raw, re.S)
    return blocks[-1] if blocks else raw
body = open("fixture_anon.swift", encoding="utf-8").read()
cases = {
    "zero blocks": body,
    "one block": "text\n```\n" + body + "```\n",
    "three blocks": ("```\nnot this\n```\ntext\n```swift\nnor this\n```\nmore\n```\n"
                     + body + "```\n"),
}
for name, raw in cases.items():
    got = extract(raw)
    print("%-14s -> %s" % (name, "the program" if got == body else "WRONG: %r" % got[:40]))
PY

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
src = open("fixture_real.swift", encoding="utf-8").read()
print("no pseudo-word collides with any identifier or literal in the fixture :",
      not any(w in src for w in pw))
PY

say "PF-F  round-trip on the shipped fixture, checks R1-R5"
python3 validate.py roundtrip

say "PF-G  the language properties the pack states are the language's REAL behaviour"
# Each of these is a claim the pack makes.  A pack claim that is false would teach
# every trial in this language something wrong, so each is executed here.
probe() {  # probe "<label>" "<source>" "<expect-build:ok|fail>"
  printf '%s\n' "$2" > "$EV/probe.swift"
  if swiftc -O "$EV/probe.swift" -o "$EV/probe.bin" > "$EV/probe.log" 2>&1; then
    r="build-ok"; "$EV/probe.bin" > "$EV/probe.out" 2>&1; r="$r run-exit=$?"
    r="$r out=$(tr '\n' '|' < "$EV/probe.out")"
  else
    r="build-FAIL: $(head -1 "$EV/probe.log" | cut -c1-70)"
  fi
  printf '  %-52s %s   [pack says: %s]\n' "$1" "$r" "$3"
}
probe "symmetric spacing a + b" 'var n = 0
n = n + 1
print("\(n)")' "builds"
probe "asymmetric spacing a +b" 'var n = 0
n = n +1
print("\(n)")' "must NOT build"
probe "0..<3 excludes the upper end" 'var s = 0
for k in 0..<3 { s = s + k }
print("\(s)")' "prints 3 (0+1+2)"
probe "assigning to a fixed binding" 'let a: Int = 1
a = 2
print("\(a)")' "must NOT build"
probe "an unread binding is only a warning" 'let unused: Int = 1
print("ok")' "builds"
probe "whole-number arithmetic is checked" 'var a: Int = 9223372036854775807
a = a + 1
print("\(a)")' "stops the program"
probe "brace on the next line" 'var n = 1
if n > 0
{
    n = 2
}
print("\(n)")' "builds; the pack says either line is fine"
probe "loop brace on the next line" 'var s = 0
for k in 0..<3
{
    s = s + k
}
print("\(s)")' "builds; the pack says either line is fine"

say "DONE"
