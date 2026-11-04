#!/usr/bin/env bash
# The exact type `real` end to end, with the exactprobe probe providers
# (tests/golden/probes/packages/exactprobe) so that no case depends on Math:
# the provider interface (version 2 and version 1), real.op and Core's
# constructors. Each case is a program read from stdin; a runtime failure
# is checked field by field through tests/runtime_report.py.
set -euo pipefail

QUIDRA="$1"
ROOT="$2"
HELPER="$ROOT/tests/runtime_report.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only
mkdir -p "$TMP/packages"
cp -R "$ROOT/tests/golden/probes/packages/exactprobe" "$TMP/packages/exactprobe"
export QUIDRA_PACKAGE_PATH="$TMP/packages"

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# ok NAME EXPECTED < PROGRAM: the program runs and prints EXPECTED.
ok() {
    local name=$1 expected=$2
    cat > "$TMP/$name.qui"
    local rc=0
    (cd "$TMP" && "$QUIDRA" "$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        fail "$name: exit status $rc"; cat "$TMP/$name.err"; return
    fi
    if [[ "$(cat "$TMP/$name.out")" != "$expected" ]]; then
        fail "$name: printed"; cat "$TMP/$name.out"; echo "expected"; echo "$expected"; return
    fi
    echo "ok   $name"
}

# stops NAME LINE COLUMN MESSAGE code=CODE [field=value ...] < PROGRAM: the
# program stops with status 101 and the report has these fields.
stops() {
    local name=$1 line=$2 column=$3 message=$4
    shift 4
    cat > "$TMP/$name.qui"
    local rc=0
    (cd "$TMP" && "$QUIDRA" "$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err") || rc=$?
    if [[ "$rc" -ne 101 ]]; then
        fail "$name: exit status $rc, expected 101"; cat "$TMP/$name.err"; return
    fi
    if ! python3 "$HELPER" "$TMP/$name.err" "file=$name.qui" "line=$line" "column=$column" \
            "message=$message" "$@"; then
        fail "$name"; return
    fi
    echo "ok   $name"
}

# reason NAME REASON: the program of an earlier `stops` case stops the same
# way when run again, and its JSON report names REASON.
reason() {
    local name=$1 expected=$2
    local rc=0
    (cd "$TMP" && "$QUIDRA" "$name.qui" > /dev/null 2> "$TMP/$name.again") || rc=$?
    if ! cmp -s "$TMP/$name.err" "$TMP/$name.again"; then
        fail "$name: a second run reported differently"; cat "$TMP/$name.again"; return
    fi
    rc=0
    (cd "$TMP" && QUIDRA_ERROR_FORMAT=json "$QUIDRA" "$name.qui" > /dev/null 2> "$TMP/$name.json") || rc=$?
    if ! python3 -c 'import json,sys; r=json.loads(open(sys.argv[1]).read().splitlines()[-1]); sys.exit(0 if r["details"].get("reason")==sys.argv[2] else 1)' \
            "$TMP/$name.json" "$expected"; then
        fail "$name: expected reason $expected"; cat "$TMP/$name.json"; return
    fi
    echo "ok   $name (reason $expected)"
}

# rejects NAME CODE < PROGRAM: the program does not compile, and the first
# diagnostic has CODE.
rejects() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    local output
    if output="$("$QUIDRA" check "$TMP/$name.qui" --json 2>&1)"; then
        fail "$name: compiled"; return
    fi
    if ! python3 -c 'import json,sys; d=json.loads(sys.argv[1])["diagnostics"]; sys.exit(0 if d and d[0]["code"]==sys.argv[2] else 1)' \
            "$output" "$code"; then
        fail "$name: expected $code"; echo "$output"; return
    fi
    echo "ok   $name"
}

# --- The installed header is C (the native ABI is C, additive only).
if command -v cc > /dev/null; then
    printf '#include <quidra/native_extension.h>\nint main(void) { return (int)sizeof(qcore_exact_provider_v2) > 0 ? 0 : 1; }\n' \
        > "$TMP/header.c"
    if cc -std=c99 -Wall -Wextra -Werror -fsyntax-only -I "$ROOT/include" "$TMP/header.c"; then
        echo "ok   header-is-c"
    else
        fail "header-is-c"
    fi
fi

# --- Version-2 providers: atoms, operations of any arity through real.op,
# and the provider's canonicalization (flip(flip(x)) is x).
ok v2-construct "true
true
true" <<'QUI'
import exactprobe
real root := exactprobe.root2
real flipped := exactprobe.flip(exactprobe.flip(root))
print("{flipped == root}")
print(NL)
real picked := exactprobe.first(exactprobe.pi, root)
print("{picked == exactprobe.first(exactprobe.pi, root)}")
print(NL)
real two := real.op("exactprobe", 102, real(2), exactprobe.omega)
real again := real.op("exactprobe", 102, real(2), exactprobe.omega)
print("{two == again}")
print(NL)
QUI
stops v2-arity 2 1 "exact-real provider operation is unavailable" code=EXACT_NUMERIC <<'QUI'
import exactprobe
real bad := real.op("exactprobe", 101, real(1), real(2))
QUI
stops v2-unknown-atom 2 1 "exact-real provider or atom opcode is unavailable" code=EXACT_NUMERIC <<'QUI'
import exactprobe
real bad := real.atom("exactprobe", 9)
QUI

# --- Version-1 providers still register and decide through their decimal
# atoms.
ok v1-provider "true true
true" <<'QUI'
import exactprobe
real root := exactprobe.root2_v1
print("{root > real(1.41)} {root < real(1.42)}")
print(NL)
print("{exactprobe.flip_v1(root) == exactprobe.flip_v1(root)}")
print(NL)
QUI

# --- Refinement decides comparisons and conversions of symbolic values
# from the providers' certified enclosures.
ok refine-decides "true true true true true
1.4142135623730951 1.4142135 3.141592653589793" <<'QUI'
import exactprobe
real root := exactprobe.root2
real pi := exactprobe.pi
print("{root > real(1.41)} {root < real(1.42)} {exactprobe.flip(root) < real(0)} {pi > root} {exactprobe.omega > real(0.5)}")
print(NL)
real64 a := real64(root)
real32 b := real32(root)
real64 c := real64(pi)
print("{a} {b} {c}")
print(NL)
QUI
ok refine-declines "numeric conversion failed: the exact value could not be decided for real64
numeric conversion failed: value is not an integer and cannot be represented as int
numeric conversion failed: the exact value could not be decided for int
numeric conversion out of range: value cannot be represented as real64" <<'QUI'
import exactprobe
real wide := exactprobe.omega
real root := exactprobe.root2
real64 | error a := real64(wide)
match a
    real64 value
        print("{value}")
    error problem
        print("{problem}")
print(NL)
int | error b := int(root)
match b
    int value
        print("{value}")
    error problem
        print("{problem}")
print(NL)
int | error c := int(wide)
match c
    int value
        print("{value}")
    error problem
        print("{problem}")
print(NL)
real64 | error d := real64(real(10) ^ real(400))
match d
    real64 value
        print("{value}")
    error problem
        print("{problem}")
print(NL)
QUI
# Rationals round to nearest, ties to even.
ok round-ties "1.0 1.0000000000000004 1.6777216e+07" <<'QUI'
real ulp := real(1) / real(9007199254740992)
real64 a := real64(real(1) + ulp)
real64 b := real64(real(1) + ulp * real(3))
real32 c := real32(real(16777217))
print("{a} {b} {c}")
QUI
# What refinement cannot decide stops with EXACT_UNPROVEN and its reason:
# an enclosure that never leaves zero, an operation without a refinement (a
# version-1 provider's), and an expression beyond the visit budget; the
# same way on every run.
stops unproven-zero 3 7 "exact comparison could not be proven within the maximum precision" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
real root := exactprobe.root2
print(root * root == real(2))
QUI
reason unproven-zero ZERO_UNDECIDED
stops unproven-wide 2 7 "exact comparison could not be proven within the maximum precision" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
print(exactprobe.omega < real(1.5))
QUI
reason unproven-wide ZERO_UNDECIDED
stops unproven-v1 3 7 "exact comparison could not be proven: a value has no certified refinement" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
real flipped := exactprobe.flip_v1(exactprobe.root2_v1)
print(flipped < real(0))
QUI
reason unproven-v1 NO_REFINEMENT
stops unproven-budget 5 7 "exact comparison could not be proven: the refinement budget ran out" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
real value := exactprobe.root2
for: 0 <= nat i < 12000
    value = exactprobe.first(value, exactprobe.pi)
print(value > real(1))
QUI
reason unproven-budget BUDGET_EXHAUSTED

# --- Generic normalization: equal values built in different orders are one
# value, like terms and equal factors combine, and a rational coefficient
# distributes over a sum, so these comparisons need no refinement.
ok normal-forms "true true true true true true" <<'QUI'
import exactprobe
real s := exactprobe.root2
real p := exactprobe.pi
real w := exactprobe.omega
bool a := (s + p) - (p + s) == real(0)
bool b := s + s == real(2) * s
bool c := (s * p) / s == p
bool d := real(2) * (w + real(1)) == real(2) * w + real(2)
bool e := (w - w) * p == real(0)
bool f := w * w * w == w * (w * w)
print("{a} {b} {c} {d} {e} {f}")
QUI

# --- Exact text: decimals when finite, fractions otherwise, and symbolic
# values in canonical text with the providers' symbols; print, "{r}" and
# .string() agree.
ok text-forms "0.5 1/3 -1/3 4.0 -0.75 1000000000000000000000.0
σ π/2 3π 2σ 1+σ -π σπ/2 1/2+π flip(σ) first(σ, π) σ^2 ω^2/3
real.atom(\"exactprobe_v1\", 1)
same same" <<'QUI'
import exactprobe
real s := exactprobe.root2
real p := exactprobe.pi
real half := real(1) / real(2)
real third := real(1) / real(3)
print("{half} {third} {-third} {real(4)} {real(-3) / real(4)} {real(10) ^ real(21)}")
print(NL)
print("{s} {p / real(2)} {real(3) * p} {s * real(2)} {s + real(1)} {-p} {s * p / real(2)} {half + p} {exactprobe.flip(s)} {exactprobe.first(s, p)} {s * s} {exactprobe.omega * exactprobe.omega / real(3)}")
print(NL)
real v1 := exactprobe.root2_v1
print(v1)
print(NL)
real value := s * p / real(2)
string printed := "{value}"
print(if printed == value.string() then "same" else "different")
print(" ")
print(if "{third}" == third.string() then "same" else "different")
print(NL)
QUI
# Interpolation formats give certified, correctly rounded digits, ties to
# even; int and zero pad that decimal text.
ok text-formats "3.14159 3.14 0.333 0.12 0.38 1.4142135623730950488
  0.33 000.33 -0.00 10 9.97 0.0000" <<'QUI'
import exactprobe
real third := real(1) / real(3)
real p := exactprobe.pi
real root := exactprobe.root2
print("{p:frac=5} {p:sig=3} {third:frac=3} {real(1) / real(8):frac=2} {real(3) / real(8):frac=2} {root:sig=20}")
print(NL)
real tiny := real(-1) / real(1000)
real near := real(9966) / real(1000)
print("{third:int=3,frac=2} {third:int=3,frac=2,zero} {tiny:frac=2} {near:sig=2} {near:sig=3} {real(0):sig=5}")
print(NL)
QUI
rejects format-int-real TYPE_MISMATCH <<'QUI'
real third := real(1) / real(3)
print("{third:int=3}")
QUI
rejects format-zero-real TYPE_MISMATCH <<'QUI'
real third := real(1) / real(3)
print("{third:int=4,zero}")
QUI
stops digits-unproven 3 1 "exact digits could not be proven within the maximum precision" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
real wide := exactprobe.omega
print("{wide:frac=2}")
QUI
reason digits-unproven ZERO_UNDECIDED
# real.parse reads what real prints for rationals, and no symbolic text.
ok parse-fractions "1/3 -0.5 0.5 error error" <<'QUI'
string describe(string text)
    match real.parse(text)
        real value
            return value.string()
        error problem
            return "error"
print(describe("1/3"))
print(" ")
print(describe("-2/4"))
print(" ")
print(describe("1/2"))
print(" ")
print(describe("√2"))
print(" ")
print(describe("1/0"))
QUI

# --- Real powers: rational exponents of positive rationals are exact when
# the root is rational and radicals otherwise, which Core refines; a
# negative base with a fractional exponent and 0 ^ 0 are domain errors.
ok powers "2.0 √2 2^(1/3) √8 √2/2 √(2/3) 2.0 √π 2^π
true true true 1.4142135623730951 1.2599210498948732" <<'QUI'
import exactprobe
real half := real(1) / real(2)
real third := real(1) / real(3)
real root := real(2) ^ half
print("{real(4) ^ half} {root} {real(2) ^ third} {real(8) ^ half} {real(1) / root} {(real(2) / real(3)) ^ half} {root * root} {exactprobe.pi ^ half} {real(2) ^ exactprobe.pi}")
print(NL)
real64 a := real64(root)
real64 b := real64(real(2) ^ third)
print("{root > real(1.41)} {root < real(1.42)} {root * root == real(2)} {a} {b}")
print(NL)
QUI
stops power-negative-base 1 7 "a negative base requires an integer exponent" code=POWER_DOMAIN <<'QUI'
print(real(-2) ^ (real(1) / real(2)))
QUI
stops power-zero-zero 2 7 "0 ^ 0 is undefined" code=POWER_DOMAIN <<'QUI'
real zero := real(0)
print(zero ^ zero)
QUI
stops power-unrefined 3 7 "exact comparison could not be proven: a value has no certified refinement" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
real value := real(2) ^ exactprobe.pi
print(value > real(1))
QUI
reason power-unrefined NO_REFINEMENT

# --- Contextual constants: every use of a `const literal real` constant is a
# literal of the real category. It materializes into the real type its
# context expects, rounded once (a lone literal at compile time, anything
# else certified before the first statement); it has no type of its own.
cat > "$TMP/lib.qui" <<'QUI'
const literal real planck := 6.62607015e-34
const literal real half_pi := real.atom("exactprobe", 2) / 2.0
QUI
ok contextual "π 3.1415927 3.141592653589793 6.2831855 6.283185307179586 true
0.33333334 0.3333333333333333 1/3 6.62607015e-34 1.5707963267948966 π/2" <<'QUI'
import exactprobe
import lib = "./lib.qui"
const literal real pi := real.atom("exactprobe", 2)
const literal real third := 1.0 / 3.0
real exact := pi
real32 single := pi
real64 double := pi
real32 x := real32(2.0)
real32 scaled := x * pi
real64 y := pi * 2.0
real64 direct := real64(pi)
print("{exact} {single} {double} {scaled} {y} {direct == double}")
print(NL)
real32 t32 := third
real64 t64 := third
real texact := third
real64 h := lib.planck
real64 q := lib.half_pi
real qexact := lib.half_pi
print("{t32} {t64} {texact} {h} {q} {qexact}")
print(NL)
QUI
# A contextual constant is a literal for the typing of if-expression
# branches as well.
ok contextual-branches "0.5 3.1415927" <<'QUI'
import exactprobe
const literal real pi := real.atom("exactprobe", 2)
bool small := true
real32 a := if small then 0.5 else pi
real32 b := if not small then 0.5 else pi
print("{a} {b}")
QUI
ok contextual-local "2.0 4.0" <<'QUI'
real64 twice(real64 value)
    const literal real two := 2.0
    real64 result := value * two
    return result
print("{twice(1.0)} {twice(2.0)}")
QUI
ok contextual-auto "true" <<'QUI'
const literal real c := 1.0 / 3.0
value := c
real typed := value
print(typed == 1.0 / 3.0)
QUI
rejects contextual-integer NUMERIC_FAMILY <<'QUI'
const literal real c := 1.0 / 3.0
int32 value := c
QUI
rejects contextual-type LITERAL_CONSTANT <<'QUI'
const literal real32 c := 1.5
QUI
rejects contextual-initializer LITERAL_CONSTANT <<'QUI'
const literal real c := real(len("ab"))
QUI
rejects atom-typed-real TYPE_MISMATCH <<'QUI'
import exactprobe
real32 value := real.atom("exactprobe", 2)
QUI
ok literal-identifier "3" <<'QUI'
int literal := 3
print(literal)
QUI
# A constant whose rounding cannot be certified stops the program before its
# first statement, at the constant's first use.
stops contextual-unproven 4 17 "exact rounding could not be proven within the maximum precision" code=EXACT_UNPROVEN <<'QUI'
import exactprobe
const literal real wide := real.atom("exactprobe", 3)
print("before")
real64 value := wide
QUI
reason contextual-unproven ZERO_UNDECIDED
if [[ -s "$TMP/contextual-unproven.out" ]]; then fail "contextual-unproven: a statement ran"; fi

# --- Typed constants keep their declared type at every use: another type
# needs an explicit conversion (which converts the declared value), and only
# a `const literal` constant re-materializes in its use-site type.
cat > "$TMP/typed.qui" <<'QUI'
const real32 eps := 1.0e-5
const int32 count := 7
const literal real tenth := 0.1
QUI
ok typed-constants "1.0e-05 1.0e-05 9.999999747378752e-06 7 7 0.1 0.1" <<'QUI'
import typed = "./typed.qui"
real32 a := typed.eps
real64 widened := real64(typed.eps)
int64 wide := int64(typed.count)
int32 same := typed.count
real32 t32 := typed.tenth
real64 t64 := typed.tenth
print("{a} {typed.eps} {widened} {wide} {same} {t32} {t64}")
QUI
rejects typed-real-retype TYPE_MISMATCH <<'QUI'
import typed = "./typed.qui"
real64 value := typed.eps
QUI
rejects typed-integer-retype TYPE_MISMATCH <<'QUI'
import typed = "./typed.qui"
int64 value := typed.count
QUI
printf 'import typed = "./typed.qui"\nreal64 value := typed.eps\n' > "$TMP/typed-message.qui"
if "$QUIDRA" check "$TMP/typed-message.qui" > "$TMP/typed-message.out" 2>&1; then
    fail "typed-message: compiled"
elif ! grep -qF "'typed.eps' has type real32; convert it explicitly (real64(typed.eps)) or declare it 'const literal real'." "$TMP/typed-message.out"; then
    fail "typed-message"; cat "$TMP/typed-message.out"
else
    echo "ok   typed-message"
fi

# --- real.op is checked like real.atom and real.unary.
rejects op-provider-literal EXACT_PROVIDER <<'QUI'
string provider := "exactprobe"
real value := real.op(provider, 102, real(1), real(2))
QUI
rejects op-opcode-literal EXACT_PROVIDER <<'QUI'
int opcode := 102
real value := real.op("exactprobe", opcode, real(1), real(2))
QUI
rejects op-no-value ARGUMENT_MISMATCH <<'QUI'
real value := real.op("exactprobe", 102)
QUI
ok op-value-type "true" <<'QUI'
import exactprobe
real64 x := 1.0
real value := real.op("exactprobe", 102, x, real(2))
real explicit_value := real.op("exactprobe", 102, real(x), real(2))
print(value == explicit_value)
QUI

if [[ "$failures" -ne 0 ]]; then
    echo "$failures failure(s)"
    exit 1
fi
echo "exact_real_cli: all cases passed"
