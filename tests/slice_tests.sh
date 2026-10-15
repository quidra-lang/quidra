#!/usr/bin/env bash
# Slices of arrays, strings, bins and tensors (L5 slices, L6 slice
# assignment). A brute-force
# oracle written here, independently of the compiler's slice plan, gives the
# index set of every form pair, step and endpoint over lengths 0-5; the
# compiler must select those indices for run-time operands, accept every
# valid slice with constant operands, and reject every invalid one with
# constant operands over a fixed array at compile time. Then the examples of
# the decision, element kinds, initialization, assignment, the failures at
# run time, and the formatter and patch round trips.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# expect NAME EXPECTED < PROGRAM: the program prints exactly EXPECTED.
expect() {
    local name=$1 expected=$2
    cat > "$TMP/$name.qui"
    local out rc=0
    out="$("$QUIDRA" "$TMP/$name.qui" 2> "$TMP/$name.err")" || rc=$?
    if [[ $rc -ne 0 ]]; then fail "$name: status $rc"; cat "$TMP/$name.err"; return; fi
    if [[ "$out" != "$expected" ]]; then
        fail "$name: output"; printf 'expected:\n%s\ngot:\n%s\n' "$expected" "$out"
    fi
}

# fails NAME CODE MESSAGE < PROGRAM: the program prints "before", then stops
# with status 101 and the runtime error CODE whose message is MESSAGE.
fails() {
    local name=$1 code=$2 message=$3
    cat > "$TMP/$name.qui"
    local out rc=0
    out="$("$QUIDRA" "$TMP/$name.qui" 2> "$TMP/$name.err")" || rc=$?
    if [[ $rc -ne 101 ]]; then fail "$name: status $rc, expected 101"; cat "$TMP/$name.err"; return; fi
    [[ "$out" == "before" ]] || fail "$name: output '$out'"
    grep -Fq "runtime error[$code]" "$TMP/$name.err" || { fail "$name: no $code"; cat "$TMP/$name.err"; }
    grep -Fq "$message" "$TMP/$name.err" || { fail "$name: message"; cat "$TMP/$name.err"; }
}

# rejects NAME CODE MESSAGE < PROGRAM: quidra check reports CODE with a
# message that contains MESSAGE.
rejects() {
    local name=$1 code=$2 message=$3
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json" 2>&1; then
        fail "$name: accepted"; return
    fi
    python3 - "$TMP/$name.json" "$code" "$message" <<'PY' || fail "$name: diagnostics"
import json, sys
report = json.load(open(sys.argv[1]))
found = [d for d in report["diagnostics"] if d["code"] == sys.argv[2] and sys.argv[3] in d["message"]]
if not found:
    print(json.dumps(report["diagnostics"], indent=1))
    sys.exit(1)
PY
}

# A materialized indices() result uses the outer axis and nat elements.
expect indices-dynamic "true" <<'QUI'
int[] values = [10, 20, 30, 40]
nat[] expected = [0, 1, 2, 3]
print(values.indices() == expected)
QUI
expect indices-nested "true" <<'QUI'
int[][] values = [[1, 2], [3, 4], [5, 6]]
nat[] expected = [0, 1, 2]
print(values.indices() == expected)
QUI
expect indices-empty "true" <<'QUI'
int[] values = []
nat[] expected = []
print(values.indices() == expected)
QUI
rejects indices-args ARGUMENT_MISMATCH "array.indices() takes no arguments" <<'QUI'
int[] values = [1, 2]
print(values.indices(1))
QUI

# --- The brute-force oracle ------------------------------------------------
python3 - "$TMP" <<'PY'
import pathlib, sys
tmp = pathlib.Path(sys.argv[1])
STARTS = ["incl", "asc", "desc", "omit"]
ENDS = ["incl", "asc", "desc", "omit"]

def oracle(length, sf, s0, ef, e0, k0, strict_step=True):
    """The index set of the slice rules, or 'step' / 'bounds'."""
    sm = {"asc": 1, "desc": -1}.get(sf, 0)
    em = {"asc": 1, "desc": -1}.get(ef, 0)
    if sm and em and sm != em:
        return "step"
    d = sm or em
    if k0 is not None:
        if k0 == 0:
            return "step"
        sign = -1 if k0 < 0 else 1
        if d and d != sign:
            return "step"
        d, k = sign, k0
    else:
        if not d:
            d = -1 if sf != "omit" and ef != "omit" and s0 > e0 else 1
        k = d
    s_omit, e_omit = sf == "omit", ef == "omit"
    s = (0 if d > 0 else length - 1) if s_omit else s0
    e = (length if d > 0 else -1) if e_omit else e0
    s_excl, e_excl = sm != 0, e_omit or em != 0
    if strict_step and k0 is not None and not s_omit and not e_omit and (s > e if d > 0 else s < e):
        return "bounds"
    indices, j = [], 1 if s_excl else 0
    while True:
        i = s + j * k
        if not (d * i < d * e if e_excl else d * i <= d * e):
            break
        indices.append(i)
        j += 1
    empty = not indices
    if not s_omit and s_excl and not -1 <= s <= length: return "bounds"
    if not e_omit and e_excl and not -1 <= e <= length: return "bounds"
    if not e_omit and not e_excl and not (0 <= e < length or (e == length and empty)): return "bounds"
    if not s_omit and not s_excl and not (0 <= s < length or (s == length and empty)): return "bounds"
    if (s > e if d > 0 else s < e) and not s_excl and not e_excl: return "bounds"
    if any(i < 0 or i >= length for i in indices): return "bounds"
    return indices

def item(sf, s, ef, e, k):
    text = "" if sf == "omit" else str(s)
    text += {"asc": "<", "desc": ">"}.get(sf, "") + ":" + {"asc": "<", "desc": ">"}.get(ef, "")
    text += "" if ef == "omit" else str(e)
    return text + ("" if k is None else ":" + str(k))

def cases(lengths, steps):
    for sf in STARTS:
        for ef in ENDS:
            for stepped in (False, True):
                rows = []
                for length in lengths:
                    for s in ([0] if sf == "omit" else range(-2, length + 2)):
                        for e in ([0] if ef == "omit" else range(-2, length + 2)):
                            for k in (steps if stepped else [None]):
                                rows.append((length, s, e, k, oracle(length, sf, s, ef, e, k)))
                yield sf, ef, stepped, rows

# The first code points of a string with one-, two-, four- and three-byte
# UTF-8 sequences.
LETTERS = ["a", "\u00e9", "\U0001F600", "b", "\u20ac", "c"]

HELPERS = """string letters(nat n)
    string all = "a\u00e9\U0001F600b\u20acc"
    string out = ""
    nat i = 0
    while i < n
        out = out + all[i]
        i = i + 1
    return out

int[] ramp(nat n)
    int[] out = array(n, fill = 0)
    nat i = 0
    while i < n
        out[i] = int(i)
        i = i + 1
    return out

void show(int[] xs)
    print("[")
    nat i = 0
    while i < len(xs)
        if i > 0
            print(" ")
        print(xs[i])
        i = i + 1
    print("]")
    print(NL)

"""

# Run-time operands: one slice site per form pair and step presence, run
# over every valid case of that pair.
program, expected, total = [HELPERS], [], 0
for n, (sf, ef, stepped, rows) in enumerate(cases(range(0, 6), [1, -1, 2, -2, 3, -3])):
    valid = [row for row in rows if isinstance(row[4], list)]
    if not valid:
        continue
    total += len(valid)
    name = f"c{n}"
    program.append(f"nat[] {name}len = [{', '.join(str(r[0]) for r in valid)}]\n")
    program.append(f"int[] {name}s = [{', '.join(str(r[1]) for r in valid)}]\n")
    program.append(f"int[] {name}e = [{', '.join(str(r[2]) for r in valid)}]\n")
    if stepped:
        program.append(f"int[] {name}k = [{', '.join(str(r[3]) for r in valid)}]\n")
    start = "" if sf == "omit" else f"{name}s[{name}i]"
    end = "" if ef == "omit" else f"{name}e[{name}i]"
    text = start + {"asc": "<", "desc": ">"}.get(sf, "") + ":" + {"asc": "<", "desc": ">"}.get(ef, "") + end
    if stepped:
        text += f":{name}k[{name}i]"
    program.append(f"nat {name}i = 0\nwhile {name}i < len({name}len)\n"
                   f"    int[] values = ramp({name}len[{name}i])\n"
                   f"    show(values[{text}])\n"
                   f"    string word = letters({name}len[{name}i])\n"
                   f"    print(\"<\" + word[{text}] + \">\" + NL)\n"
                   f"    {name}i = {name}i + 1\n")
    for row in valid:
        expected.append("[" + " ".join(str(i) for i in row[4]) + "]")
        expected.append("<" + "".join(LETTERS[i] for i in row[4]) + ">")
(tmp / "oracle.qui").write_text("".join(program), encoding="utf-8")
(tmp / "oracle.expected").write_text("\n".join(expected) + "\n", encoding="utf-8")

# Constant operands over fixed arrays: every valid case runs, every invalid
# one is a compile error (INDEX_BOUNDS or SLICE_STEP).
valid_program, valid_expected = [HELPERS], []
invalid_program, bounds, steps = [], 0, 0
for length in range(1, 5):
    literal = "[" + ", ".join(str(i) for i in range(length)) + "]"
    valid_program.append(f"int[{length}] f{length} = {literal}\n")
    invalid_program.append(f"int[{length}] f{length} = {literal}\n")
for sf, ef, stepped, rows in cases(range(1, 5), [1, -1, 2, -3, 0]):
    for length, s, e, k, outcome in rows:
        text = item(sf, s, ef, e, k)
        if isinstance(outcome, list):
            valid_program.append(f"show(f{length}[{text}])\n")
            valid_expected.append("[" + " ".join(str(i) for i in outcome) + "]")
        else:
            invalid_program.append(f"int[] x{bounds + steps} = f{length}[{text}]\n")
            if outcome == "bounds":
                bounds += 1
            else:
                steps += 1
(tmp / "constant.qui").write_text("".join(valid_program), encoding="utf-8")
(tmp / "constant.expected").write_text("\n".join(valid_expected) + "\n")
(tmp / "invalid.qui").write_text("".join(invalid_program), encoding="utf-8")
(tmp / "invalid.counts").write_text(f"{bounds} {steps}\n")
print(f"oracle: {total} run-time cases, {len(valid_expected)} constant cases, "
      f"{bounds} + {steps} constant errors")
# The same independent index oracle checks every bin form without a step,
# and ascending tensor forms with a positive or omitted step.
OTHER_HELPERS = '''bin bits(nat n)
    string all = "010101"
    string text = ""
    nat i = 0
    while i < n
        text = text + all[i]
        i = i + 1
    return bin.parse(text)

tensor<real32> tensor_ramp(nat n)
    tensor<real32> result = tensor.zeros<real32>([n])
    nat i = 0
    while i < n
        result[i] = real32(i)
        i = i + 1
    return result

void show_tensor(tensor<real32> values)
    print("[")
    nat i = 0
    while i < values.shape()[0]
        if i > 0
            print(" ")
        print(values[i].item())
        i = i + 1
    print("]" + NL)

'''
for collection in ("bin", "tensor"):
    other_program, other_expected, other_total = [OTHER_HELPERS], [], 0
    for n, (sf, ef, stepped, rows) in enumerate(cases(range(0, 6), [1, 2, 3])):
        if collection == "bin" and stepped:
            continue
        if collection == "tensor" and (sf == "desc" or ef == "desc"):
            continue
        if collection == "tensor":
            rows = [(n, s, e, k, oracle(n, sf, s, ef, e, 1 if k is None else k, strict_step=False))
                    for n, s, e, k, _ in rows]
        valid = [row for row in rows if isinstance(row[4], list)]
        if not valid:
            continue
        other_total += len(valid)
        name = f"d{n}"
        other_program.append(f"nat[] {name}len = [{', '.join(str(r[0]) for r in valid)}]\n")
        other_program.append(f"int[] {name}s = [{', '.join(str(r[1]) for r in valid)}]\n")
        other_program.append(f"int[] {name}e = [{', '.join(str(r[2]) for r in valid)}]\n")
        if stepped:
            other_program.append(f"int[] {name}k = [{', '.join(str(r[3]) for r in valid)}]\n")
        start = "" if sf == "omit" else f"{name}s[{name}i]"
        end = "" if ef == "omit" else f"{name}e[{name}i]"
        text = start + {"asc": "<", "desc": ">"}.get(sf, "") + ":" + {"asc": "<", "desc": ">"}.get(ef, "") + end
        if stepped:
            text += f":{name}k[{name}i]"
        if collection == "bin":
            body = (f"    bin values = bits({name}len[{name}i])\n"
                    f"    print(\"<\" + values[{text}].string() + \">\" + NL)\n")
        else:
            body = (f"    tensor<real32> values = tensor_ramp({name}len[{name}i])\n"
                    f"    show_tensor(values[{text}])\n")
        other_program.append(f"nat {name}i = 0\nwhile {name}i < len({name}len)\n" + body +
                             f"    {name}i = {name}i + 1\n")
        for row in valid:
            if collection == "bin":
                other_expected.append("<" + "".join("010101"[i] for i in row[4]) + ">")
            else:
                other_expected.append("[" + " ".join(f"{i}.0" for i in row[4]) + "]")
    (tmp / f"{collection}-oracle.qui").write_text("".join(other_program), encoding="utf-8")
    (tmp / f"{collection}-oracle.expected").write_text("\n".join(other_expected) + "\n")
    print(f"{collection} oracle: {other_total} run-time cases")

PY

"$QUIDRA" "$TMP/oracle.qui" > "$TMP/oracle.out" 2> "$TMP/oracle.err" || { fail "oracle: run"; cat "$TMP/oracle.err"; }
cmp -s "$TMP/oracle.expected" "$TMP/oracle.out" || { fail "oracle: run-time slices differ"; diff "$TMP/oracle.expected" "$TMP/oracle.out" | head -20; }
"$QUIDRA" "$TMP/constant.qui" > "$TMP/constant.out" 2> "$TMP/constant.err" || { fail "oracle: constant run"; cat "$TMP/constant.err"; }
cmp -s "$TMP/constant.expected" "$TMP/constant.out" || { fail "oracle: constant slices differ"; diff "$TMP/constant.expected" "$TMP/constant.out" | head -20; }
"$QUIDRA" check "$TMP/invalid.qui" --json --max-errors 100000 > "$TMP/invalid.json" 2>&1 || true
python3 - "$TMP/invalid.json" "$TMP/invalid.counts" <<'PY' || fail "oracle: constant errors"
import collections, json, sys
report = json.load(open(sys.argv[1]))
bounds, steps = map(int, open(sys.argv[2]).read().split())
codes = collections.Counter(d["code"] for d in report["diagnostics"])
lines = collections.Counter(d["span"]["start"]["line"] for d in report["diagnostics"])
assert codes == {"INDEX_BOUNDS": bounds, "SLICE_STEP": steps}, (codes, bounds, steps)
assert all(count == 1 for count in lines.values()), "one diagnostic per slice"
PY

for collection in bin tensor; do
    "$QUIDRA" "$TMP/$collection-oracle.qui" > "$TMP/$collection-oracle.out" 2> "$TMP/$collection-oracle.err" || { fail "$collection oracle: run"; cat "$TMP/$collection-oracle.err"; }
    cmp -s "$TMP/$collection-oracle.expected" "$TMP/$collection-oracle.out" || { fail "$collection oracle: slices differ"; diff "$TMP/$collection-oracle.expected" "$TMP/$collection-oracle.out" | head -20 || true; }
done

# --- The decision's examples (L5/L6 and the exclusive start) ----------
expect examples "$(printf '%s\n' '[0 1 2 3 4 5]' '[0 2 4]' '[5 4 3 2 1 0]' '[4 2 0]' \
    '[10 8 6 4 2]' '[8 6 4 2 0]' '[1 2 3 4 5]' '[2 4 6]' '[3 5]' '[]' '[7]' \
    '[10 9 8 7 6 5 4 3 2 1 0]' '[4 2 0]' '[3 1]' '[4 1]' '[0 1 2]' '[2 1 0]' '[]' '[]' '[]' '[]')" <<'QUI'
int[] ramp(nat n)
    int[] out = array(n, fill = 0)
    nat i = 0
    while i < n
        out[i] = int(i)
        i = i + 1
    return out

void show(int[] xs)
    print("[")
    nat i = 0
    while i < len(xs)
        if i > 0
            print(" ")
        print(xs[i])
        i = i + 1
    print("]")
    print(NL)

int[] a = ramp(11)
int[] six = ramp(6)
nat N = len(six)
show(six[0:<N])
show(six[0:<N:2])
show(six[N>:0])
show(six[N>:0:-2])
show(a[10:>0:-2])
show(a[10>:0:-2])
show(six[0<:5])
show(a[0<:6:2])
show(a[1<:6:2])
int[] empty = []
show(empty[len(empty)>:0])
int[] one = [7]
show(one[len(one)>:0])
show(a[len(a)>:0])
int[] five = ramp(5)
show(five[::-2])
show(five[5>::-2])
show(five[::-3])
show(five[-1<:2])
show(five[2:>-1])
show(five[3>:3])
show(five[5:>5])
show(five[5:<3])
show(five[5:])
QUI

# --- Strings: code points, every form; start:<end past its end is empty ------
expect strings "$(printf '%s\n' '<é😀>' '<b😀éa>' '<b😀éa>' '<>' '<😀>' '<é😀b>' '<>' '<aé>' '<ba>')" <<'QUI'
string word = "aé😀b"
print("<" + word[1:2] + ">" + NL)
print("<" + word[::-1] + ">" + NL)
print("<" + word[len(word)>:0] + ">" + NL)
print("<" + word[4:<3] + ">" + NL)
print("<" + word[0<:3:2] + ">" + NL)
print("<" + word[1:] + ">" + NL)
int stop = 1
print("<" + word[3:<stop] + ">" + NL)
print("<" + word[:<2] + ">" + NL)
print("<" + "abc"[2>:0:-1] + ">" + NL)
QUI
fails string_inclusive INDEX_BOUNDS "slice 1:9 out of bounds for length 4" <<'QUI'
string word = "aé😀b"
int stop = 9
print("before" + NL)
string part = word[1:stop]
QUI
fails string_exclusive INDEX_BOUNDS "slice 5:<1 out of bounds for length 4" <<'QUI'
string word = "aé😀b"
int start = 5
print("before" + NL)
string part = word[start:<1]
QUI
fails string_step SLICE_STEP "slice step 0 is invalid" <<'QUI'
string word = "aé😀b"
int step = 0
print("before" + NL)
string part = word[::step]
QUI
rejects string_literal INDEX_BOUNDS "Slice 1:5 out of bounds for length 3." <<'QUI'
string part = "abc"[1:5]
QUI
rejects string_arity INDEX_ARITY "Arrays and strings take one index or one slice." <<'QUI'
string text = "abc"
string part = text[0:1, 1:2]
QUI

# --- Bins: every form but a step; start:<end past its end is empty ---------
expect bins "$(printf '%s\n' '<110>' '<11>' '<10>' '<10110>' '<101>' '<>' '<01>' '<>')" <<'QUI'
auto data = bin.parse("01101")
print("<" + data[1:3].string() + ">" + NL)
print("<" + data[1:<3].string() + ">" + NL)
print("<" + data[1<:3].string() + ">" + NL)
print("<" + data[len(data)>:0].string() + ">" + NL)
print("<" + data[4:>1].string() + ">" + NL)
int at = 3
print("<" + data[at:<1].string() + ">" + NL)
print("<" + data[:<2].string() + ">" + NL)
print("<" + data[5:].string() + ">" + NL)
QUI
fails bin_inclusive INDEX_BOUNDS "slice 2:9 out of bounds for length 5" <<'QUI'
auto data = bin.parse("01101")
int at = 9
print("before" + NL)
bin part = data[2:at]
QUI
fails bin_exclusive INDEX_BOUNDS "slice 6:<1 out of bounds for length 5" <<'QUI'
auto data = bin.parse("01101")
int at = 6
print("before" + NL)
bin part = data[at:<1]
QUI

# --- Tensors: ascending forms with a positive step --------------------------
expect tensors "$(printf '%s\n' '3' '2' '0' '3' '1' '3' '4.0' '1.0')" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5]) + real32(1)
print(t[0:2].shape()[0])
print(NL)
print(t[0<:2].shape()[0])
print(NL)
print(t[1:<1].shape()[0])
print(NL)
print(t[0:4:2].shape()[0])
print(NL)
print(t[0<:<4:2].shape()[0])
print(NL)
print(t[2:].shape()[0])
print(NL)
tensor<real32> grid = tensor.zeros<real32>([3, 3])
grid[2, 2] = real32(4)
print(grid[1<:2, 0<:2][0, 1].item())
print(NL)
print(t[:<1][0].item())
print(NL)
QUI
fails tensor_inclusive INDEX_BOUNDS "slice 0:5 out of bounds for axis 0 with length 5" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int at = 5
print("before" + NL)
tensor<real32> part = t[0:at]
QUI
rejects tensor_descending SLICE_STEP "Tensor slices ascend" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
auto part = t[4:>0]
QUI

# --- Element kinds: copies are independent values ---------------------------
expect elements "$(printf '%s\n' 'c b' 'true' '2.5' 'x' '1' '9 2' '9 7' 'a 4' '30' '30 20 10')" <<'QUI'
class Pair this(local string name_initial, local int count_initial)
    string name
    int count


    this.name = name_initial
    this.count = count_initial

string[] words = ["a", "b", "c", "d"]
string[] picked = words[2:>0]
print(picked[0] + " " + picked[1])
print(NL)
bool[] flags = [false, true]
print(flags[1:][0])
print(NL)
real64[] reals = [0.5, 1.5, 2.5]
print(reals[2:<3][0])
print(NL)
string text = "x"
string[] single = [text][0:0]
print(single[0])
print(NL)
int[][] rows = [[1, 2], [3, 4]]
int[][] copied = rows[0:<1]
print(copied[0][0])
print(NL)
copied[0][0] = 9
print(copied[0][0])
print(" ")
print(rows[0][1])
print(NL)
rows[1][0] = 7
print(copied[0][0])
print(" ")
print(rows[1][0])
print(NL)
Pair first = Pair("a", 4)
Pair[] pairs = [first, Pair("b", 5)]
Pair[] front = pairs[:<1]
print(front[0].name + " ")
print(front[0].count)
print(NL)
int[3] fixed = [10, 20, 30]
int[] tail = fixed[2:]
print(tail[0])
print(NL)
int[] reversed = fixed[len(fixed)>:0]
print(reversed[0])
print(" ")
print(reversed[1])
print(" ")
print(reversed[2])
print(NL)
QUI

expect nested_fixed "$(printf '%s\n' '4 5 6' '1 2 3' '9' '4')" <<'QUI'
int[2][3] matrix = [[1, 2, 3], [4, 5, 6]]
int[][3] lower = matrix[1:]
print(lower[0][0])
print(" ")
print(lower[0][1])
print(" ")
print(lower[0][2])
print(NL)
int[][3] both = matrix[1>:0]
print(both[0][0])
print(" ")
print(both[0][1])
print(" ")
print(both[0][2])
print(NL)
lower[0][0] = 9
print(lower[0][0])
print(NL)
print(matrix[1][0])
print(NL)
QUI

expect bigint "$(printf '%s\n' '100000000000000000000000' '-5')" <<'QUI'
int[] big = [5, 100000000000000000000000, -5]
int[] picked = big[1:2]
print(picked[0])
print(NL)
print(picked[1])
print(NL)
QUI

# --- Initialization: the selected elements are read ------------------------
expect initialized_part "$(printf '%s\n' '1 2')" <<'QUI'
int[] values = array(5)
values[0] = 1
values[1] = 2
int[] part = values[0:1]
print(part[0])
print(" ")
print(part[1])
print(NL)
QUI
fails uninitialized UNINITIALIZED "value is uninitialized" <<'QUI'
int[] values = array(5)
values[0] = 1
print("before" + NL)
int[] part = values[0:2]
QUI

# --- Run-time failures --------------------------------------------------------
fails past_end INDEX_BOUNDS "slice 1:9 out of bounds for length 6" <<'QUI'
int[] values = [1, 2, 3, 4, 5, 6]
int stop = int(len(values)) + 3
print("before" + NL)
int[] part = values[1:stop]
QUI
fails reversed_inclusive INDEX_BOUNDS "slice 4:2:1 out of bounds for length 6" <<'QUI'
int[] values = [1, 2, 3, 4, 5, 6]
int stop = 2
print("before" + NL)
int[] part = values[4:stop:1]
QUI
fails excluded_far INDEX_BOUNDS "slice -2<:2 out of bounds for length 6" <<'QUI'
int[] values = [1, 2, 3, 4, 5, 6]
int at = -2
print("before" + NL)
int[] part = values[at<:2]
QUI
fails empty_inclusive INDEX_BOUNDS "slice -1<:-1 out of bounds for length 5" <<'QUI'
int[] values = [1, 2, 3, 4, 5]
int at = -1
print("before" + NL)
int[] part = values[at<:at]
QUI
fails strided_anchor INDEX_BOUNDS "slice 7>:0:-2 out of bounds for length 6" <<'QUI'
int[] values = [1, 2, 3, 4, 5, 6]
int at = int(len(values)) + 1
print("before" + NL)
int[] part = values[at>:0:-2]
QUI
fails step_zero SLICE_STEP "slice step 0 is invalid" <<'QUI'
int[] values = [1, 2, 3]
int step = 0
print("before" + NL)
int[] part = values[0:2:step]
QUI
fails step_against_end SLICE_STEP "slice step -2 cannot be used with ':<'" <<'QUI'
int[] values = [1, 2, 3]
int step = -2
print("before" + NL)
int[] part = values[0:<2:step]
QUI
fails step_against_start SLICE_STEP "slice step 2 cannot be used with '>:'" <<'QUI'
int[] values = [1, 2, 3]
int step = 2
print("before" + NL)
int[] part = values[3>:0:step]
QUI
expect runtime_step "$(printf '%s\n' '[2 1 0]' '[0 1 2]')" <<'QUI'
void show(int[] xs)
    print("[")
    nat i = 0
    while i < len(xs)
        if i > 0
            print(" ")
        print(xs[i])
        i = i + 1
    print("]")
    print(NL)
int[] values = [0, 1, 2]
int down = -1
int up = 1
show(values[2:0:down])
show(values[0:2:up])
QUI

# --- Compile-time errors -----------------------------------------------------
rejects marker_conflict SLICE_STEP "Slice markers point one way" <<'QUI'
int[] values = [1, 2, 3]
int[] part = values[0<:>2]
QUI
rejects constant_step SLICE_STEP "Slice step -1 cannot be used with ':<'." <<'QUI'
int[] values = [1, 2, 3]
int[] part = values[0:<2:-1]
QUI
rejects fixed_bounds INDEX_BOUNDS "Slice 1:5 out of bounds for length 3." <<'QUI'
int[3] values = [1, 2, 3]
int[] part = values[1:5]
QUI
rejects two_items INDEX_ARITY "Arrays and strings take one index or one slice." <<'QUI'
int[] values = [1, 2, 3]
int[] part = values[0:1, 1:2]
QUI
rejects not_storage INDEX_ARITY "exactly one integer index" <<'QUI'
void touch(int[] &values)
    values[0] = 1
int[] values = [1, 2, 3]
touch(&values[0:1])
QUI

# --- Slice assignment (L6) ------------------------------------------------------
expect assign "$(printf '%s\n' '[0 9 9 3 4 5]' '[0 7 2 8 4 9]' '[5 4 3 2 1 0]' '[0 0 1 2 4 5]' \
    '[0 1 9 9 4]' '[3 2 1]' '[1 2 3]' '[a b z]' '[1 2]')" <<'QUI'
int[] ramp(nat n)
    int[] out = array(n, fill = 0)
    nat i = 0
    while i < n
        out[i] = int(i)
        i = i + 1
    return out

void show(int[] xs)
    print("[")
    nat i = 0
    while i < len(xs)
        if i > 0
            print(" ")
        print(xs[i])
        i = i + 1
    print("]")
    print(NL)

int[] a = ramp(6)
a[1:2] = [9, 9]
show(a)
int[] b = ramp(6)
b[1:<6:2] = [7, 8, 9]
show(b)
int[] c = ramp(6)
c[len(c)>:0] = ramp(6)
show(c)
int[] d = ramp(6)
d[1:<4] = d[0:<3]
show(d)
int[] e = ramp(5)
e[1<:3] = [9, 9]
show(e)
int[3] f = [0, 0, 0]
f[::-1] = [1, 2, 3]
show(f)
int[] g = array(3)
g[:] = [1, 2, 3]
show(g)
string[] words = ["a", "b", "c"]
words[2:] = ["z"]
print("[" + words[0] + " " + words[1] + " " + words[2] + "]")
print(NL)
int[2] source = [1, 2]
int[] h = [0, 0]
h[0:1] = source
show(h)
QUI
fails assign_length SLICE_ASSIGNMENT "slice assignment needs 3 values, got 2" <<'QUI'
int[] values = [1, 2, 3, 4]
int[] two = [5, 6]
print("before" + NL)
values[0:2] = two
QUI
fails assign_bounds INDEX_BOUNDS "slice 2:4 out of bounds for length 4" <<'QUI'
int[] values = [1, 2, 3, 4]
int stop = 4
print("before" + NL)
values[2:stop] = [7, 8, 9]
QUI
rejects assign_static SLICE_ASSIGNMENT "Slice assignment needs 3 values, got 2." <<'QUI'
int[] values = [1, 2, 3, 4]
int[2] two = [5, 6]
values[0:2] = two
QUI
rejects assign_scalar TYPE_MISMATCH "Slice assignment needs an array." <<'QUI'
int[] values = [1, 2, 3, 4]
values[0:2] = 5
QUI
rejects assign_elements TYPE_MISMATCH "int64[]" <<'QUI'
int64[] values = [int64(1), int64(2)]
int[] other = [3, 4]
values[0:1] = other
QUI
rejects assign_string INVALID_ASSIGNMENT "A string slice cannot be assigned" <<'QUI'
string text = "abc"
text[0:1] = "xy"
QUI

# --- quidra fmt and quidra patch keep each marker next to its colon -----------
cat > "$TMP/format.qui" <<'QUI'
int[] values = [0, 1, 2, 3, 4, 5]
int n = 5
int[] a = values[0<:5]
int[] b = values[n>:0]
int[] c = values[0:<n]
int[] d = values[4:>0: -2]
int[] e = values[0<:<n:2]
int[] f = values[n>:> -1]
int[] g = values[0 <:5]
int[] h = values[0<: 5]
print(len(a) + len(b) + len(c) + len(d) + len(e) + len(f) + len(g) + len(h))
print(NL)
QUI
cp "$TMP/format.qui" "$TMP/format.original"
"$QUIDRA" fmt "$TMP/format.qui"
cmp -s "$TMP/format.original" "$TMP/format.qui" || { fail "fmt changed slice markers"; diff "$TMP/format.original" "$TMP/format.qui"; }
"$QUIDRA" inspect "$TMP/format.qui" > "$TMP/format-inspect.json"
python3 - "$TMP/format-inspect.json" "$TMP/format-patch.json" <<'PY'
import json, sys
x = json.load(open(sys.argv[1]))
node = next(n for n in x["nodes"] if n["kind"] == "integer" and n["source"] == "5"
            and n["span"]["start"]["line"] == 2)
json.dump({"schema_version": 3, "base_revision": x["revision"], "operations": [{
    "op": "replace_node", "node_id": node["node_id"], "expected_hash": node["source_hash"],
    "expected_kind": "integer", "replacement_node": {"kind": "integer", "source": "4"}}]},
    open(sys.argv[2], "w"))
PY
"$QUIDRA" patch "$TMP/format.qui" "$TMP/format-patch.json" --write > /dev/null
sed 's/^int n = 5$/int n = 4/' "$TMP/format.original" > "$TMP/format.expected"
cmp -s "$TMP/format.expected" "$TMP/format.qui" || { fail "patch changed slice markers"; diff "$TMP/format.expected" "$TMP/format.qui"; }
[[ "$("$QUIDRA" "$TMP/format.qui")" == "30" ]] || fail "patched program output"

# --- The REPL ---------------------------------------------------------------
cat > "$TMP/repl.txt" <<'QUI'
int[] values = [1, 2, 3, 4]
values[len(values)>:0][0] * 10 + values[0<:2][1]
:exit
QUI
"$QUIDRA" repl < "$TMP/repl.txt" > "$TMP/repl.out" 2> "$TMP/repl.err" || fail "repl status"
grep -Eq '(^|> )43$' "$TMP/repl.out" || { fail "repl slice"; cat "$TMP/repl.out" "$TMP/repl.err"; }

# Slice bounds have the same integer-operand rule as element indices.
for integer_kind in nat nat8 nat16 nat32 nat64 int int8 int16 int32 int64; do
    expect "bounds_$integer_kind" "$(printf '%s\n' '1 3' 'é🙂' '110' '2')" <<QUI
$integer_kind first = 1
$integer_kind last = 3
$integer_kind stride = 2
int[] values = [0, 1, 2, 3, 4]
int[] picked = values[first:last:stride]
print(picked[0])
print(" ")
print(picked[1])
print(NL)
string text = "aé中🙂z"
print(text[first:last:stride] + NL)
auto bits = bin.parse("01101")
print(bits[first:last].string() + NL)
tensor<real32> t = tensor.zeros<real32>([5])
print(t[first:last:stride].shape()[0])
print(NL)
QUI
done

# The bounds run before the rhs; the store resolves the root after it.
expect assign_order "$(printf '%s\n' 'start end step rhs' '4 8 9 7' '6')" <<'QUI'
int start()
    print("start ")
    return 1
int stop()
    print("end ")
    return 2
int stride()
    print("step ")
    return 1
int[] replacement(int[] &values)
    print("rhs" + NL)
    values = [4, 5, 6, 7, 10, 11]
    return [8, 9]
int[] values = [0, 1, 2, 3]
values[start():stop():stride()] = replacement(&values)
print(values[0])
print(" ")
print(values[1])
print(" ")
print(values[2])
print(" ")
print(values[3])
print(NL)
print(len(values))
print(NL)
QUI

# A tensor step fails with the slice diagnostic, including its marker.
fails tensor_zero_step SLICE_STEP "slice step 0 is invalid" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int stride = 0
print("before" + NL)
auto part = t[::stride]
QUI
fails tensor_end_step SLICE_STEP "slice step -1 cannot be used with ':<'" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int stride = -1
print("before" + NL)
auto part = t[0:<3:stride]
QUI
fails tensor_start_step SLICE_STEP "slice step -1 cannot be used with '<:'" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int stride = -1
print("before" + NL)
auto part = t[0<:3:stride]
QUI
fails tensor_negative_step SLICE_STEP "tensor slices currently require a positive step" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int stride = -1
print("before" + NL)
auto part = t[::stride]
QUI

rejects tensor_constant_zero SLICE_STEP "Slice step 0 is invalid." <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
auto part = t[::0]
QUI
rejects tensor_constant_end_step SLICE_STEP "Slice step -1 cannot be used with ':<'." <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
auto part = t[0:<3:-1]
QUI
rejects tensor_constant_start_step SLICE_STEP "Slice step -1 cannot be used with '<:'." <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
auto part = t[0<:3:-1]
QUI

# A written extreme integer is never an omitted bound or step.
fails tensor_min_start INDEX_BOUNDS "slice -9223372036854775808<:3 out of bounds" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int64 first = -9223372036854775808
print("before" + NL)
auto part = t[first<:3]
QUI
fails tensor_min_end INDEX_BOUNDS "slice 0:<-9223372036854775808 out of bounds" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int64 last = -9223372036854775808
print("before" + NL)
auto part = t[0:<last]
QUI
fails tensor_min_step SLICE_STEP "slice step -9223372036854775808 cannot be used with ':<'" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int64 stride = -9223372036854775808
print("before" + NL)
auto part = t[0:<3:stride]
QUI

# Bounds are checked against the storage the right-hand side leaves behind.
fails assign_new_bounds INDEX_BOUNDS "slice 2:3 out of bounds for length 2" <<'QUI'
int[] replacement(int[] &values)
    values = [4, 5]
    return [8, 9]
int[] values = [0, 1, 2, 3]
print("before" + NL)
values[2:3] = replacement(&values)
QUI

expect tensor_excluded_minus_one "0" <<'QUI'
tensor<real32> t = tensor.zeros<real32>([5])
int last = -1
print(t[0:<last].shape()[0])
print(NL)
QUI

if [[ $failures -ne 0 ]]; then
    echo "slice tests: $failures failure(s)"
    exit 1
fi
echo "slice tests: ok"
