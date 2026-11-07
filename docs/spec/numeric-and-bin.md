# Numeric Types and Bin

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int64`, `int`
- `nat8`, `nat16`, `nat32`, `nat64`, `nat`
- `real16`, `real16b`, `real32`, `real64`
- `real`
- `com16`, `com16b`, `com32`, `com64`, `com`

`int8` to `int64` are the signed fixed-width integers and `nat8` to `nat64` the unsigned ones. `int` is an exact arbitrary-precision integer and `nat` an exact arbitrary-precision natural number: their arithmetic never overflows, and a `nat` result below zero is an error. Their representation is internal: they have no bit operations, no C ABI and no tensors of them. `real64` is IEEE-754 binary64. `real32` is IEEE-754 binary32. `real16` is IEEE-754 binary16 and `real16b` is bfloat16 (8 significand bits, the exponent range of `real32`); each operation on them computes the exact operands in binary32 and rounds the result once to the type, to nearest with ties to even, which is the correctly rounded result. Neither has a C mapping. The complex types `com16`, `com16b`, `com32`, `com64` and `com` hold a real part and an imaginary part of `real16`, `real16b`, `real32`, `real64` and `real` ([Complex numbers](#complex-numbers)). `real` is an exact mathematical-real value: finite decimals are exact rationals, while exact constants and irrational results may remain symbolic rather than being rounded to an IEEE representation.

There is no `char`, `bit`, `byte`, or `bytes` source type. Text uses `string`; a one-byte numeric value uses `nat8`; arbitrary raw bit sequences use `bin`. A quoted literal is always `string`.

## Numeric literal families

Numeric literals take a compatible concrete type from a resolved context. Without one, integer literals default to `int`, real literals to exact `real`, and imaginary or complex literals to exact `com`. A natural type (`nat` or a fixed-width `nat*`) requires context.

```quidra
a := 5 + 6          // int
int32 b := 5 + 6    // int32
c := b + 1          // int32
nat d := 5          // nat
ratio := 0.1        // exact real
z := 1.0 + 2.0i     // com
```

Contextual materialization is not a conversion: the literal was never a default-typed value first. Binding, assignment, field, compatible parameter, return, tuple and array element contexts may determine literal types. When a numeric destination is outside the literal category, the literal may first materialize in its own category and then undergo checked conversion to the destination type. A context with several compatible numeric union alternatives remains ambiguous; defaulting does not choose an alternative of such a union.

### Literal categories

| Category | Formed by | Directly materializes into | Default |
|---|---|---|---|
| non-negative integer | `10` | `nat*`, `int*` | `int` |
| negative integer | unary `-` on an integer literal (`-10`, `-0`) | `int*` | `int` |
| real | `10.0`, `0.5`, `-10.0`, `1.0e8` | `real*` | `real` |
| imaginary | a real literal followed by `i` (`2.0i`) | `com*` | `com` |
| complex | real and imaginary literals joined by `+` or `-` (`1.0 + 2.0i`) | `com*` | `com` |

Direct materialization preserves categories. A numeric destination can instead use materialization in the literal's own category followed by checked conversion: `real32 x := 10` and `int y := 3.0` are valid; `int y := 3.5` fails the integrality check, and `nat8 n := -1` fails the destination range check. In a mixed numeric operator, a literal may first materialize in its own category and then participate in the common-type conversion below: `real32 y := 1.0; y + 2` has type `real32`. The integer form `2i` is rejected; write `2.0i`.

A compound literal expression computes in its resolved context type with checked arithmetic: `nat8 x := 3 - 5` and `int8 y := 100 + 100` are compile-time errors. Already-typed numeric values also convert at a numeric destination: `nat32 n := 10` followed by `int32 x := n` is valid, subject to the destination range check.

A real-family literal materialized as an IEEE type is rounded once to nearest, ties to even, with subnormals included, and must stay within the finite destination range. It is not rounded through `real64` first. `real32 x := 0.1` therefore deterministically rounds to binary32. In exact `real`, the decimal spelling is an exact rational. Explicit integer-to-real conversion such as `real32(16777217)` uses destination rounding too.

Integer literal syntax has no implementation-width ceiling in `int` or `nat`, including the default `int` context:

```quidra
exact := 1234567890123456789012345678901234567890 // int
```

Fixed-width contexts enforce their normal ranges. Integer literals are decimal only; `0x`, `0b` and `0o` prefixes are not source syntax. Exponent notation requires a decimal point: `1e8` is an error, while `1.0e8` and `1.0e-8` are real-family literals.

## Common numeric operator types

Scalar numeric binary operators, including arithmetic, comparisons and fixed-width bitwise operators, resolve their common operand type statically. Supported elementwise array and tensor numeric operators use the same rule for their element/scalar types. Whole-array `==` and `!=` retain their matching-container-type value equality contract.

For numeric operators:

1. Select the highest family present: **Natural < Integer < Real < Complex**.
2. Among operands in that family only, select the widest type. The arbitrary-precision/exact type of that family wins over its fixed-width types. Lower-family widths never participate.
3. `real16` and `real16b` join at `real32`; `com16` and `com16b` join at `com32`, since neither half format covers the other's precision and range.
4. Convert both operand values to the selected type **before** applying the operator. Check each conversion and the result; do not widen from runtime values or after an overflow. Real and complex conversions may use the destination's normal rounding. Signed infinity and NaN retain the [existing real arithmetic policy](#signed-infinity-without-nan).

| Operand types | Common type |
|---|---|
| `nat64`, `int32` | `int32` |
| `nat`, `int32` | `int32` |
| `int`, `nat64` | `int` |
| `int32`, `real16` | `real16` |
| `real64`, `com16` | `com16` |
| `real16`, `real16b` | `real32` |
| `com16`, `com16b` | `com32` |
| `real`, `com32` | `com32` |

For example, `nat64` plus `int32` uses `int32` even if the natural operand currently exceeds its range. That operand's conversion fails before addition; cancellation cannot rescue it. Similarly, an integer combined with `real16` is rounded to `real16` before arithmetic, and a finite out-of-range conversion fails. The original variables retain their declared types. Complex precision comes only from complex operands, and complex values still have no ordering.

Numeric values implicitly convert to the resolved numeric destination in initialized declarations, assignments, parameter defaults, returns, and value arguments to functions, methods and constructors. The checks match explicit `T(value)`: destination range, ordinary real/complex rounding, and integral-only real-to-integer conversion. Supported whole-array/tensor conversions preserve structure and shape. A failed implicit conversion stops before the destination write, return or callee body; source variable types stay unchanged. Writable `T &` arguments require the exact storage type and never convert. `bool` is separate from numeric types and never participates in these implicit conversions.

For example, `fahrenheit := celsius * 1.8 + 32` uses exact `real` arithmetic when `celsius` is an integer. The decimal `1.8` is exactly nine fifths, and both integer operands convert at their respective operators. Celsius `22` therefore gives `71.6`.

The [matrix multiplication operator](language.md#matrix-multiplication-operator) uses Math's `matmul` overload resolution and argument conversions.

## Type inference in declarations

An inferred declaration uses the initializer's type, including the numeric defaults:

```quidra
int32 source := 3
copy := source       // int32
x := 3               // int
y := 1.5             // real
values := [1, 2]     // int[] with inferred length
text := "hello"      // string
flag := true         // bool
```

If the inferred type is an unnamed union containing `error`, an inferred declaration removes only `error` from the binding type and fails fast on error. An explicit union destination retains that failure channel.

A compatible concrete function parameter supplies literal context; a cross-category numeric argument instead materializes in its own category and converts to the formal type under the destination conversion rule above: `identity32(3)` materializes `3` as `int32` when that parameter is `int32`. Without a concrete generic context, the numeric default supplies a type for inference: `identity(3)` can infer `T = int` for `T identity<T>(T value)`. Printable literals use the same defaults, so `print(1)` is valid with `int`; display and interpolation do not create a separate numeric default policy.

## Exponentiation

`^` is Quidra's basic exponentiation operator. It is right-associative and binds
more tightly than multiplication and division:

```quidra
base := 2
exponent := 10
value := base ^ exponent

real64 x := 4.0
root := x ^ 0.5
```

Integer exponentiation requires a non-negative integer exponent. Fixed-width results retain the language's checked-overflow semantics.
Floating and `real` operands support real exponents. Both operands use the
common numeric type selected by the rules above before exponentiation.

Powers without a value are `POWER_DOMAIN` failures in every numeric family:
`0 ^ 0`, a negative integer exponent,
and, for `real32`, `real64` and `real`, a negative base with an exponent that is not an
integer (`real64 a := -2.0; a ^ 0.5`; there is no promotion to a complex
result). With literal operands these are compile-time errors; otherwise the
program stops at the operation.

`real` powers are exact. An integer exponent multiplies exactly. A rational
exponent `p/q` of a positive rational base gives a rational when the base has
a rational `q`-th root (`real(4) ^ real(0.5)` is `2.0`) and otherwise stays a
radical, which Core encloses through integer roots, so it compares and
converts like any other value (`real(2) ^ real(0.5)` prints `√2` and converts
to the real64 `1.4142135623730951`); a symbolic base must be provably
positive. The integer part of a rational base's exponent moves into the
coefficient (`real(2) ^ real(1.5)` is `2√2`, `real(1) / √2` is `√2/2`), and
a radical is not simplified further (`real(8) ^ real(0.5)` stays `√8`; Math
may canonicalize radicals). Any other exponent (`real(2) ^ math.pi`) gives a
power that only a provider's refinement can decide.

Bitwise XOR is deliberately separate and continues to use the uppercase `XOR`
operator.

## Factorial

Postfix factorial is implemented on `develop`.

Postfix `n!` computes the factorial of a non-negative integer scalar. The
result is `1` for `0!` and the product of `1` through `n` otherwise. It keeps
the operand's `int` or `nat` type, including fixed-width forms. A literal
uses compatible integer context and defaults to `int`, so `5!` has value
`120` and type `int` in an inferred declaration.

Factorial applies after calls, member access, and indexing. It binds more
tightly than exponentiation and unary minus, so `2 ^ 3!` is `2 ^ (3!)` and
`-3!` is `-(3!)`. Parenthesize a factorial to use it with another postfix
suffix. For example, `(3!)!` applies factorial twice.

The operand must be an integer scalar. A negative operand such as `(-3)`
fails with `FACTORIAL_DOMAIN`. A fixed-width result outside its type's range
fails with `INTEGER_OVERFLOW`. Statically evaluated domain or range failures
are compile-time errors. With a runtime operand, the program stops at the
operation on either failure.

## Signed infinity without NaN

The real types (`real16`, `real16b`, `real32`, `real64` and exact `real`)
admit signed infinity, written `inf` and `-inf`. Exact `real` displays these
values as `∞` and `-∞`; fixed-width types display `inf` and `-inf`. The same
spellings are used by printing, interpolation and `string(value)`.

Real overflow, nonzero real division by zero and zero to a negative real
power may produce infinity. NaN is never observable: an indeterminate
operation such as `inf - inf`, `0 * inf` or `inf / inf` fails with
`NONFINITE_RESULT`, while `0.0 / 0.0` fails with `DIVISION_BY_ZERO`.
Undefined powers, including `0 ^ 0` and a negative real base with a
fractional exponent, retain `POWER_DOMAIN`.

A constant expression follows the same rules in its concrete type:

```quidra
real64 big := 1.0e308 * 10.0 // inf: arithmetic overflow is allowed
real64 invalid := 0.0 / 0.0 // compile-time error: indeterminate division
```

Finite out-of-range literals and finite narrowing conversions still fail;
an already infinite real value preserves its sign in real-family conversions.
Infinity cannot convert to an integer. Parsing accepts `inf` and `-inf`,
rejects NaN, and retains range checks for finite text. Exact `real(text)`
also accepts `∞` and `-∞`. Signed zero and subnormal behavior is unchanged;
fixed-width `-0.0` prints with its sign and compares equal to `0.0`.

Arrays, tensor elements, delivered gradients and native package outputs
obey this policy. CPU tensor checks report the first failing logical element;
GPU checks stay on the device and report at the next synchronization point.
Merged checks must report the same first indeterminate operation, at its
own location, as unmerged checks. An intermediate NaN cannot be hidden by a
later operation such as raising it to zero.

## Complex numbers

`com16`, `com16b`, `com32` and `com64` hold a real part and an imaginary part
of `real16`, `real16b`, `real32` and `real64`; `com` holds two exact `real`
parts. An imaginary or complex literal materializes as any of them, and a
real literal inside a complex literal is its real part:

```quidra
com64 z := 1.0 + 2.0i
com32 w := 1.5 - 0.25i
com exact := 0.5i
re := z.real
im := z.imag
print("{z} {w} {exact}")
```

prints `1.0+2.0i 1.5-0.25i 0.0+0.5i`: each part as its real type prints it,
the imaginary part's sign joining them, then `i`; interpolation formats apply
to each part. `com64("1.5-0.25i")` reads that form, each part rounded
once to the part type; `com(text)` accepts the parts `real(text)` reads.

The operators are `+`, `-`, `*`, `/`, unary `-`, `==`, `!=` and, for the
fixed-width types, `^`. Complex values have no ordering. Each part operation
of a fixed-width type rounds once to the part type: `*` is
`(ac - bd) + (ad + bc)i` with no fused operation, and `/` is Smith's
algorithm. A NaN result, or a NaN intermediate value of `/`, stops the
program (`NONFINITE_RESULT`, `com32 multiplication produced NaN`);
infinite parts are valid. A divisor `0.0 + 0.0i` remains
`DIVISION_BY_ZERO` (`com64 division by zero`). `z ^ w` is the principal value `exp(w Log z)`,
computed in binary64 with the platform's math library and rounded once per
part: it is deterministic on one platform but not correctly rounded. `0 ^ w`
is 0 when the real part of `w` is positive and `POWER_DOMAIN` otherwise.
`com` arithmetic is exact; `^` of `com` values is not available.

Mixed numeric operators convert real/integer operands to the selected complex
type with imaginary part zero: `real64 + com16` uses `com16`, so the
real operand is checked and rounded to `real16` before the operation.
Numeric destinations, including bindings, assignments, returns and value parameters, use checked conversion. A real
or integer value, or a literal, converts to complex with imaginary part 0,
as its real part converts
(`com32(1.0)`, `com64(n)` for `int32 n`); complex types convert part by part
(`com32(z)` for `com64 z` is `com32 | error`). A complex value has no real
value: use `z.real`, `z.imag` or `math.abs(z)`.

```quidra
com32 a := 1.0 + 1.0i
real32 x := 2.0
b := a * x // com32; x stays real32
same := a == b
```

`tensor<com16>`, `tensor<com16b>`, `tensor<com32>` and `tensor<com64>` hold
complex elements on CPU and device backends: creation (`tensor.ones` gives `1.0 + 0.0i`),
casts from numeric and complex tensors, element-wise `+`, `-`, `*`, `/`
with the scalar operations' rounding and checks, `==` and `!=`, unary `-`,
and `.item()`. They are not differentiable. Metal supports `com64` storage,
creation, gather, negation and equality; its arithmetic and casts that require
FP64 report the backend capability error, as `real64` operations do.
Complex types satisfy the generic constraint `equatable`, not `numeric` or
`ordered`, and no complex type crosses the C boundary.

## Fixed-width bitwise operations

Bitwise operations are defined only for the fixed-width integer types `nat8` to `nat64` and `int8` to `int64`; `int` and `nat` have no bit representation.

```quidra
nat8 flags := 240
nat8 mask := 204

both := flags AND mask
either := flags OR mask
changed := flags XOR mask
inverted := NOT flags
left := flags << 1
right := flags >> 2
```

`AND`, `OR`, `XOR`, and `NOT` are uppercase deliberately. Lowercase `and`, `or`, and `not` are bool-only logical operations; `&` remains explicit storage access and `|` remains union syntax. The language therefore does not reuse one spelling for unrelated meanings.

Bitwise operations bind more tightly than comparison, equality, and lowercase boolean logic. Therefore `flags AND mask == expected` means `(flags AND mask) == expected`, avoiding a comparison-first interpretation.

Binary bitwise operands use the common numeric type, which must be a fixed-width integer type. Eligibility is checked after common-type selection; a selected arbitrary-precision `int` or `nat` is not allowed. A bare integer-family literal may materialize from that operator context in the normal way. `int`, `nat`, `real32`, `real64`, `real`, `bool`, `bin`, and tensor values do not accept these operators.

Signed fixed-width integers use a two's-complement bit representation; unsigned integers use the ordinary modulo-`2^N` N-bit representation. `NOT` flips every bit of that fixed-width representation. `AND`, `OR`, and `XOR` operate on it directly. `<<` shifts the N-bit representation left and discards bits shifted beyond the width; it is a representation operation and does not raise arithmetic overflow. `>>` is arithmetic with sign extension for signed integer types and logical with zero fill for unsigned integer types. Shift counts must be nonnegative and smaller than the common operand width; a provably invalid constant count is a compile-time `SHIFT_COUNT` error and a dynamic invalid count is a deterministic runtime `SHIFT_COUNT` failure.

After checked common-type conversion, these operators work on the selected fixed-width representation. They leave original variable types unchanged; shifts retain their representation semantics rather than checked arithmetic overflow.

## Exact integers and reals

`int` and `nat` arithmetic `+`, `-`, `*`, integer `/`, and `%` is exact and does not overflow because storage grows with the value. The source type never changes as the value grows. A `nat` subtraction whose result would be negative fails with `INTEGER_OVERFLOW` ("nat subtraction result is negative"); a negative integer literal never materializes as `nat`. Division and remainder by zero fail with `DIVISION_BY_ZERO`, as for fixed-width integers.

`real` is not a configurable-precision floating type. Exact decimals are rationals; values such as `math.pi`, `math.e`, and `math.sqrt(real(2))` may remain symbolic. Algebraic simplification is valid only when it preserves the represented mathematical value. For example, an implementation may prove `math.sqrt(real(2)) * math.sqrt(real(8)) == 4.0` without approximating either square root.

Equality and ordering of exact symbolic values must not silently fall back to rounded IEEE guesses. Core decides them by certified refinement: it encloses the difference of the two values in intervals with dyadic bounds, rounded outward, that narrow as the precision doubles from 64 to 4096 bits, using the enclosures that the providers of the atoms and operations supply, until the interval excludes zero. Rationals are compared exactly. If a result cannot be established within this deterministic budget (precision, node visits and nesting, never time), the program stops with `EXACT_UNPROVEN` instead of returning an unproved Boolean, the same way on every run and platform. Decimal formatting is an observation of the exact stored value and does not mutate it.

A contextual constant is declared with `const literal real` or `const literal com`: its initializer is a constant exact expression (literals, arithmetic, other contextual constants, and `real.atom`, `real.unary` and `real.op` with their literal provider and opcode), and every use is a literal of its declared real or complex category. `math.pi` and `math.e` are such constants, and a package or program may declare its own:

```quidra
import math

const literal planck := 6.62607015e-34

real64 fast_pi := math.pi
real exact_pi := math.pi
real32 single_pi := math.pi
real64 h := planck
```

A use materializes into the type its context expects, as a real literal does: in `real` it is the exact value; in `real32` or `real64` it is the exact value rounded once, correctly (a lone literal at compile time; any other initializer once per constant and type, before the first statement, by certified refinement, so every use is a load). `real64(math.pi)` is that same one rounding, an operand takes the other operand's type (`x * math.pi` with a `real32 x` is `real32`), and a compound expression evaluates in the materialized type. Without a resolved context a contextual real constant defaults to exact `real` (`x := math.pi` is `real`); a context with multiple compatible numeric union alternatives remains ambiguous. An integer destination instead materializes the real category first and attempts checked integral-only conversion. A constant whose rounding refinement cannot certify stops the program before its first statement with `EXACT_UNPROVEN` at its first use. `real.atom(...)` itself has type `real`: in a fixed-width context write `real64(real.atom(...))` or declare a contextual constant. Any other typed constant, imported ones included, keeps its declared type at every use (`const real32 eps := 1.0e-5` is a `real32` everywhere); numeric destinations convert that stored value with the ordinary checked rules, while numeric operators convert it to their static common type.

## Conversion

Numeric destinations use checked implicit conversion for initialized declarations,
assignments, parameter defaults, returns and value arguments. For example,
`int32 small := 3` followed by `int wider := small` is valid; the destination
has its declared type and the source stays `int32`. Invalid direct literal
materialization is diagnosed at compile time. A failed runtime implicit conversion
stops with `NUMERIC_CONVERSION` before the value is committed. Use explicit
`T(value)` when its typed conversion-error result must be retained or handled.

`bool` is not numeric. Conditions, logical operands and `bool` value arguments
require `bool`; write `n != 0` to test an integer. There is no implicit conversion
in either direction between boolean and numeric values. Reference bindings and
writable arguments retain their exact storage type.

Explicit conversion uses the destination type as a call:

```quidra
int8 small := 10
wider := int16(small)

value := 100
checked := int8(value)
```

Explicit conversion may lose precision when the destination representation requires deterministic rounding. It must not silently leave the destination's representable finite range. A failed conversion's error says what was converted, the destination type and why: `numeric conversion out of range: value cannot be represented as int8`, `numeric conversion out of range: array element cannot be represented as int8`, `numeric conversion failed: non-finite value cannot be represented as real32` (a finite value whose conversion to `real32` would round to infinity), `numeric conversion failed: value is not an integer and cannot be represented as int` (a non-integral exact `real`), `numeric conversion failed: the exact value could not be decided for int64` (a symbolic `real` the proof budget cannot decide). An explicit conversion that can fail because a runtime value is outside that range exposes `converted-type | error`: for a scalar the success type is `T`; for an array or tensor the success type is the whole converted container with its structure, rank, and shape facts preserved. Container conversion is atomic: if any numeric leaf is out of range, no partial converted container is exposed. An inferred declaration infers the success type and fails fast on error, an explicit union destination keeps the error alternative, a success-only destination also fails fast, `try` propagates it, and `match` may recover from an explicitly preserved result locally. Integer narrowing never wraps or clamps, and finite floating narrowing must not silently become infinity. Already infinite values retain their sign in real-family conversions; they cannot convert to an integer. Integer widening whose source range is fully contained in the destination is infallible.

A real (`real16`, `real16b`, `real32`, `real64` or exact `real`) converts to an integer-family type only when its mathematical value is integral and fits the destination range. An explicit cast has type `T | error`: `NOT_INTEGRAL` for a fractional value, `OUT_OF_RANGE` for an integral value outside the range, and `UNDECIDED` when an exact proof cannot establish integrality. Signed zero converts to integer zero. No integer rounding is performed; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` to select one.

Exact conversions follow these rules:

- fixed-width integer -> `int`: exact; unsigned fixed-width integer -> `nat`: exact; `nat` -> `int`: exact;
- `int` or a signed fixed-width integer -> `nat`: checked to be non-negative;
- integer-family value -> `real`: exact;
- `real32`/`real64` -> `real`: exact conversion of the stored IEEE value, not reinterpretation of the original decimal spelling;
- `int`/`nat` -> fixed-width integer: range-checked;
- `real` -> integer-family type: accepted only when the mathematical value is provably integral, then range-checked when the destination is fixed-width or `nat`;
- `int`/`nat`/`real` -> `real32`/`real64`: correctly rounded (to nearest, ties to even, subnormals included), with finite destination-range checking (explicit casts expose the whole-value `converted-type | error` channel; implicit destinations fail fast). A symbolic `real` converts once refinement certifies the rounding, that is, once both ends of its enclosure round to the same value; otherwise the conversion fails as undecided. A symbolic `real` converts to an integer type when its provider proves it integral and refinement isolates the integer; refinement that places it strictly between two integers makes it not an integer.

A numeric conversion may discard precision through the destination representation's defined rounding, but it never discards range or invents an integer rounding policy. Complex-to-real or complex-to-integer conversion is unsupported, even when the imaginary part is zero; select `.real`, `.imag` or a package operation explicitly.

### Range constraints

An ordered scalar candidate can carry a [range constraint](language.md#range-constraints):
`0 < x < 5 := value` declares a checked binding; `0 < x < 5 = value`
guards a store to an existing binding. Infer or annotate its storage type first,
convert the candidate into it, and compare with the bounds using ordinary numeric
common types. The candidate and each bound are evaluated at most once; validation
precedes the store. A proven violation is a compile error; an unresolved violation
stops with `RANGE_CONSTRAINT`, including in release builds.

The same ordered constraints apply to function, method and class-header parameters,
and to tensor axes. A new `nat N` captures the actual axis extent; a range around
an existing name also retains the axis-equality requirement. Boolean predicates
are not numeric extents. Class-header ranges are generation preconditions, and a
shape-match range mismatch selects the next case.

## Integer digit decomposition

> Specified for `develop`. Math implementation is pending.

After `import math`, `value.digits()` returns the decimal digits of an
integer scalar's magnitude as an `int[]`, most significant first.
The [Math digits specification](https://github.com/quidra-lang/math/blob/develop/README.md#integer-digit-decomposition)
defines the equivalent function form, supported integer types, and edge
cases.

## Parsing and text conversion

When its operand is a `string`, `T(text)` parses the text as the numeric
type `T` and has static type `T | error`, including for literal text.
With a numeric operand, the call instead casts the existing numeric value.

```quidra
int | error count := int("123")
real32 | error ratio := real32("1.5")
int | error huge := int("123456789012345678901234567890")
real | error exact := real("0.1")
```

An explicit `T | error` binding preserves either outcome. An inferred
binding or a success-only destination fails fast on error, and `try`
propagates the error from the enclosing function.

`int(text)` and `real(text)` consume the input text directly without a
fixed-width integer or IEEE intermediary. `real(text)` accepts a decimal
with an optional exponent, signed infinity, or a fraction `p/q` with an
optional sign (`1/3`, `-2/4`). It does not accept symbolic text such as
`√2` or `π`. Fixed-width real parsing rounds the exact decimal value of
the text once to the nearest value of the destination type, with ties to
even and subnormals included. Finite text outside the representable range,
or nonzero text that rounds to zero, is an error.

A failed parse is a `NUMERIC_PARSE` error. Its details are `type` (the
destination type) and `reason` (`INVALID_TEXT` or `OUT_OF_RANGE`). For
`int8`, the messages are `cannot parse numeric input as int8` and
`cannot parse numeric input as int8: value out of range`. The input is
not echoed.

`string(value)` takes one positional value and returns the standard text
of a numeric value, `bool`, `bin`, `error`, or a printable union. An
existing `string` is returned as the same text. For integers, the
standard text uses decimal digits.
All ten integer scalar types (`int`, `nat`, `int8`, `int16`, `int32`,
`int64`, `nat8`, `nat16`, `nat32`, and `nat64`) also expose `.b2` through
`.b36` and `.B2` through `.B36` as members returning `string`, without
parentheses. Lowercase `b` uses digits `a`–`z`; uppercase `B` uses
`A`–`Z`. Zero renders as `"0"`, signed negatives retain a single
leading minus, and no base prefix or automatic padding is added.
Conversion preserves the exact integer value, including arbitrary-precision
`int` and `nat`.

```quidra
binary := 255.b2       // "11111111"
hex := 255.b16         // "ff"
upper := 255.B16       // "FF"
signed := (-255).b16   // "-ff"
```

These members use ordinary postfix precedence: `-255.b16` attempts to negate the resulting string and is invalid; `(-255).b16` formats the negative integer. Only canonical base spellings `2` through `36` are accepted on integers (`.b1`, `.b37`, and `.b02` are invalid). Other built-in type families do not expose radix members. These names remain ordinary identifiers for user-defined class members.

String interpolation can select a radix with `"{value:base=16,upper}"`.
`base=N` accepts a decimal literal from 2 through 36, and the optional
`upper` flag selects uppercase digits. Radix interpolation cannot be
combined with decimal padding, precision, or grouping, and requires an
integer expression. A radix member can also be interpolated directly as
`"{value.B16}"`.

A finite fixed-width real prints as the shortest decimal that reads back to
the same value of its type (`real16 x := 0.1` prints `0.1`, its value being
0.0999755859375). The text of a `real` is exact: a finite decimal when the
value has one (`0.5`, `4.0`, `-0.75`), an exact fraction otherwise
(`1/3`, `-1/3`), or canonical text for a symbolic value. Core lays that
text out and a provider supplies only the symbols of its atoms and
operations: square roots take the radical sign (`√2`, `√(2/3)`), other
roots print as powers (`2^(1/3)`), coefficients are juxtaposed (`2√3`,
`3π`, `π/2`, `√2/2`, with `·` only between two digits, `3·2^(1/3)`), a
sum prints its constant first (`1+√2`), and an operation prints as its
symbol applied to its arguments (`sin(1)`). An atom or operation of a
version-1 provider, which has no symbol, prints in its intrinsic's source
form (`real.atom("math", 1)`).

Interpolation formats apply to `real` as well: `:.N` fixes fractional
digits and `:N` fixes whole-number significant digits, correctly rounded
with ties to even and certified by refinement (`EXACT_UNPROVEN` when
refinement cannot decide them). `:W.N` and `:0W.N` pad that decimal text
to a minimum integer width with spaces or zeros, without truncation; a
final `:G` adds integer grouping. Exact-value padding and grouping require
significant or fractional precision because canonical text need not be
decimal. Prefer the shorter fractional spelling `:.2` to `:.02`.

JSON number parsing is likewise lossless at the syntax boundary: a valid JSON number token is retained even when it exceeds `real64` range. `json.Value.number()` performs IEEE-range conversion with the same rounding, while `json.Value.integer()` and `json.Value.real()` consume the preserved numeric token directly.

## Bin

`bin` is a mutable packed raw bit sequence with value semantics.

```quidra
zeros := bin.fill(8, 0)
ones := bin.fill(5, 1)
bin | error pattern := bin("01010000")
```

`bin.fill(n, bit)` allocates exactly `n` bits, with `bit` restricted to `0` or `1`. `#value` (equivalently `value.len()`) returns the bit count as `nat`. Negative lengths, fill values other than 0 or 1, and invalid allocation sizes fail deterministically.

A written bit pattern is parsed rather than given a separate binary literal grammar:

```quidra
bin | error value := bin("01010000")
```

`bin(text)` with a `string` operand always has static type `bin | error`,
for literals and runtime strings alike. Only `0` and `1` are accepted;
other text is the error `BIN_PARSE`, `cannot parse bin input`. A
statically known invalid literal may be diagnosed at compile time; a
statically known valid literal may be marked internally as success-proven
so the backend can remove the impossible error branch without changing
the source-visible union type.

Indexing and slicing operate in bits:

```quidra
bits := bin.fill(8, 0)
first := bits[0]
high := bits[0:<4]
bits[0] = bin.fill(1, 1)
```

An index result is a one-bit `bin`. Bin elements do not expose addressable references. `print(value)` and `string(value)` use the exact 0/1 sequence as text.

## Bin conversion

All conversions between `bin` and other concrete types are explicit. With
a non-string operand, `bin(value)` converts a supported value's bit
representation. Integer sources must have a fixed-width bit representation;
arbitrary-precision `int` and `nat` have no fixed bit width and cannot be
converted directly to `bin`:

```quidra
bin raw := bin(3567446)          // error: default int has no fixed bit width
bin raw32 := bin(int32(3567446)) // 32-bit representation
bin raw64 := bin(int64(3567446)) // 64-bit representation
```

`bin(integer)` accepts only fixed-width `int8`..`int64` and `nat8`..`nat64` sources and preserves their exact bit patterns, including the defined two's-complement representation of signed integers. `intN(bin)` and `natN(bin)` require an exact match between the bit length and the destination width. `bin(bool)` produces one bit; `bool(bin)` requires exactly one bit.

Flat integer and bool arrays use the same rule:

```quidra
int8[] values := [80, -1]
raw := bin(values)
restored := int8[](raw)
```

`T[](bits)` requires `bits.len()` to be exactly divisible by the destination element width. No conversion pads, truncates, wraps, or silently changes bit count.

## Value semantics

Ordinary `bin` assignment is independent:

```quidra
a := bin.fill(3, 0)
a[2] = bin.fill(1, 1)
b := a
b[0] = bin.fill(1, 1)

print(a) // 001
print(b) // 101
```

The implementation stores payload bits packed into bytes, but that packing is not a source-level element model. Observable aliasing of whole storage still uses the language's explicit reference mechanisms.

## Numeric ranges and bin iteration

Range operands materialize in the variable's declared or existing numeric
type. Natural-family steps are `int`, so descending headers can use a
negative step. Real-family headers require an explicit step; an omitted
inclusive zero bound is the resolved type's zero and does not relax the
family rule for written literals. The start is the progression's anchor,
and an exclusive start skips its first candidate. Integer loops include
only lattice values in the range and never increment past their last value.

`for bin value := bits[]` reads one-bit copies. `for bin &value := &bits[]`
binds a live bit place: each assignment changes that bit immediately. A
bit place cannot be addressed or passed as an ordinary reference. Both
forms accept an index as `[index]`: `for:` declares it and plain `for` reuses an existing mutable `nat` value. It is read-only during iteration. Value loops may also
iterate a bin slice, whose positions are relative to the slice.

