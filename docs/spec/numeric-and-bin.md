# Numeric Types and Bin

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int64`, `int`
- `nat8`, `nat16`, `nat32`, `nat64`, `nat`
- `real32`, `real64`
- `real`

`int8` to `int64` are the signed fixed-width integers and `nat8` to `nat64` the unsigned ones. `int` is an exact arbitrary-precision integer and `nat` an exact arbitrary-precision natural number: their arithmetic never overflows, and a `nat` result below zero is an error. Their representation is internal: they have no bit operations, no C ABI and no tensors of them. `real64` is IEEE-754 binary64. `real32` is IEEE-754 binary32. `real` is an exact mathematical-real value: finite decimals are exact rationals, while exact constants and irrational results may remain symbolic rather than being rounded to an IEEE representation.

There is no `char`, `bit`, `byte`, or `bytes` source type. Text uses `string`; a one-byte numeric value uses `nat8`; arbitrary raw bit sequences use `bin`. A quoted literal is always `string`.

## Numeric literal families

A numeric literal does not have a default concrete numeric type.

- `3` is an **integer-family literal**.
- `3.0` is a **real-family literal**.

The integer family can materialize as a fixed-width integer type, `int` or `nat`. The real family can materialize as `real32`, `real64`, or `real`.

A literal becomes concrete only when surrounding source code determines exactly one type in the same family:

```quidra
int32 count = 3
nat8 channel = 3
real32 ratio = 3.0
real64 precise = 3.0
```

This contextual materialization is not an implicit cast. The literal was never an `int` or `real64` value first.

Literal materialization never crosses families:

```quidra
real64 x = 3      // error
int y = 3.0      // error
```

Use an explicit conversion when a representation change is intended:

```quidra
real64 x = real64(3)
```

When a real-family literal materializes as `real32` or `real64`, destination rounding is allowed, but the literal must remain inside the finite range of that IEEE type after rounding. The exact decimal value is rounded once to the nearest value of the destination, ties to even, subnormal values included; a `real32` literal is not rounded to `real64` first. Thus `real32 x = 0.1` is valid and deterministically rounds to binary32. An integer-family literal converted explicitly to an IEEE type, as in `real32(16777217)`, rounds the same way. When the same literal materializes as `real`, its decimal spelling becomes an exact rational value; it is not routed through `real64` first.

Integer literal syntax has no implementation-width ceiling when the unique context is `int` or `nat`:

```quidra
int exact = 1234567890123456789012345678901234567890
```

Fixed-width integer contexts still enforce their normal ranges.

Integer literals use decimal notation only. Base-prefixed forms such as `0x`, `0b`, and `0o` are not source syntax. Floating exponent notation requires an explicit decimal point:

```quidra
1e8      // error
1.0e8    // real-family literal
1.0e-8   // real-family literal
```

## Literal categories

A numeric literal has a category, not a type, and materializes only into the
types of its category:

| Category | Formed by | Materializes into |
|---|---|---|
| non-negative integer | `10` | `nat*`, `int*` |
| negative integer | unary `-` applied to an integer literal (`-10`, `-0`) | `int*` |
| real | `10.0`, `0.5`, `-10.0`, `1.0e8` | `real*` |
| imaginary | a real literal followed by `i` (`2.0i`) | complex types |
| complex | a real and an imaginary literal joined by `+` or `-` (`1.0 + 2.0i`) | complex types |

Categories never cross and none has a default type: `real32 x = 10` is an
error (write `10.0` or `real32(10)`), `nat8 n = -1` is an error, and
`auto x = 10` has no type to infer. Materialization is not a conversion:
`nat32 n = 10; int32 x = n` is an error. A literal operand materializes into
the other operand's type when its category allows it (`x + 2` with `int32 x`;
`y + 2` with `real32 y` is an error), and the literal argument of an explicit
conversion converts from its exact value (`real32(10)`). A compound literal
expression materializes every literal into the context type and computes in
it with checked arithmetic, so `nat8 x = 3 - 5` and `int8 y = 100 + 100` are
compile-time errors. The integer form `2i` is rejected: write `2.0i`.

## `auto`

`auto` infers the type that the initializer already determines independently and never chooses a concrete numeric representation. If that inferred type is an unnamed union containing `error`, bare `auto` removes only `error` from the binding type and makes the initialization fail fast on error; `auto | error` retains the failure channel explicitly.

```quidra
int32 source = 3
auto copy = source       // int32
auto text = "hello"      // string
auto flag = true         // bool
auto result = function() // the declared return type of function()
```

A bare numeric-family expression has no concrete type for `auto` to propagate:

```quidra
auto x = 3           // error
auto y = 1.5         // error
auto values = [1, 2] // error
```

A function parameter can provide the required context:

```quidra
int32 identity32(int32 value)
    return value

auto value = identity32(3) // int32
```

Generic inference does not choose a concrete numeric type for a bare literal. `identity<T>(3)` therefore cannot infer `T` unless some independent concrete context determines it.

The same rule applies to built-ins. A built-in such as `print` accepts already-typed printable values but does not choose a numeric representation:

```quidra
print(1)       // error
print(int(1))  // valid
```

There are no display-only, interpolation-only, or other special exceptions for numeric-family literals.

## Exponentiation

`^` is Quidra's basic exponentiation operator. It is right-associative and binds
more tightly than multiplication and division:

```quidra
int base = 2
int exponent = 10
int value = base ^ exponent

real64 x = 4.0
real64 root = x ^ 0.5
```

Integer exponentiation requires a non-negative integer exponent. Fixed-width results retain the language's checked-overflow semantics.
Floating and `real` operands support real exponents. The operands have the
same concrete numeric type; `^` does not introduce implicit numeric conversion.

Powers without a value are `POWER_DOMAIN` failures in every numeric family:
`0 ^ 0`, a zero base with a negative exponent, a negative integer exponent,
and, for the fixed-width reals `real32` and `real64`, a negative base with an exponent that is not an
integer (`real64 a = -2.0; a ^ 0.5`; there is no promotion to a complex
result). With literal operands these are compile-time errors; otherwise the
program stops at the operation.

Bitwise XOR is deliberately separate and continues to use the uppercase `XOR`
operator.

## Fixed-width bitwise operations

Bitwise operations are defined only for the fixed-width integer types `nat8` to `nat64` and `int8` to `int64`; `int` and `nat` have no bit representation.

```quidra
nat8 flags = 240
nat8 mask = 204

nat8 both = flags AND mask
nat8 either = flags OR mask
nat8 changed = flags XOR mask
nat8 inverted = NOT flags
nat8 left = flags << 1
nat8 right = flags >> 2
```

`AND`, `OR`, `XOR`, and `NOT` are uppercase deliberately. Lowercase `and`, `or`, and `not` are bool-only logical operations; `&` remains explicit storage access and `|` remains union syntax. The language therefore does not reuse one spelling for unrelated meanings.

Bitwise operations bind more tightly than comparison, equality, and lowercase boolean logic. Therefore `flags AND mask == expected` means `(flags AND mask) == expected`, avoiding a comparison-first interpretation.

Binary bitwise operands have the same concrete fixed-width integer type. A bare integer-family literal may materialize from that operator context in the normal way. `int`, `nat`, `real32`, `real64`, `real`, `bool`, `bin`, and tensor values do not accept these operators.

Signed fixed-width integers use a two's-complement bit representation; unsigned integers use the ordinary modulo-`2^N` N-bit representation. `NOT` flips every bit of that fixed-width representation. `AND`, `OR`, and `XOR` operate on it directly. `<<` shifts the N-bit representation left and discards bits shifted beyond the width; it is a representation operation and does not raise arithmetic overflow. `>>` is arithmetic with sign extension for signed integer types and logical with zero fill for unsigned integer types. Shift counts must be nonnegative and smaller than the operand width; a provably invalid constant count is a compile-time `SHIFT_COUNT` error and a dynamic invalid count is a deterministic runtime `SHIFT_COUNT` failure.

These operations are representation operations rather than arithmetic conversions. They never change the operand type and never imply a cast.

## Exact integers and reals

`int` and `nat` arithmetic `+`, `-`, `*`, integer `/`, and `%` is exact and does not overflow because storage grows with the value. The source type never changes as the value grows. A `nat` subtraction whose result would be negative fails with `INTEGER_OVERFLOW` ("nat subtraction result is negative"); a negative integer literal never materializes as `nat`. Division and remainder by zero fail with `DIVISION_BY_ZERO`, as for fixed-width integers.

`real` is not a configurable-precision floating type. Exact decimals are rationals; values such as `math.pi`, `math.e`, and `math.sqrt(real(2))` may remain symbolic. Algebraic simplification is valid only when it preserves the represented mathematical value. For example, an implementation may prove `math.sqrt(real(2)) * math.sqrt(real(8)) == 4.0` without approximating either square root.

Equality and ordering of exact symbolic values must not silently fall back to rounded IEEE guesses. If a result cannot be established within the runtime's finite proof budget, evaluation fails deterministically instead of returning an unproved Boolean. Decimal formatting is an observation of the exact stored value and does not mutate it.

`math.pi` and `math.e` are real-family constants whose concrete representation comes from context:

```quidra
import math

real64 fast_pi = math.pi
real exact_pi = math.pi
```

Without a unique real-family context, these constants are ambiguous for the same reason as a bare real-family literal.

## Conversion

Already-typed values never change concrete type implicitly.

```quidra
int32 small = 3
int wider = small // error
```

Explicit conversion uses the destination type as a call:

```quidra
int8 small = 10
int16 wider = int16(small)

int value = 100
int8 checked = int8(value)
```

Explicit conversion may lose precision when the destination representation requires deterministic rounding. It must not silently leave the destination's representable finite range. A failed conversion's error says what was converted, the destination type and why: `numeric conversion out of range: value cannot be represented as int8`, `numeric conversion out of range: array element cannot be represented as int8`, `numeric conversion failed: non-finite value cannot be represented as real32` (±infinity narrowed to `real32`), `numeric conversion failed: value is not an integer and cannot be represented as bigint` (a non-integral exact `real`), `numeric conversion failed: the exact value could not be decided for int64` (a symbolic `real` the proof budget cannot decide). Any conversion that can fail because a runtime value is outside that range exposes `converted-type | error`: for a scalar the success type is `T`; for an array or tensor the success type is the whole converted container with its structure, rank, and shape facts preserved. Container conversion is atomic: if any numeric leaf is out of range, no partial converted container is exposed. Bare `auto` infers the success type and fails fast on error, `auto | error` keeps the error alternative, a success-only destination also fails fast, `try` propagates it, and `match` may recover from an explicitly preserved result locally. Integer narrowing never wraps or clamps, and finite floating narrowing must not silently become infinity. Integer widening whose source range is fully contained in the destination is infallible.

IEEE floating-point to an integer-family type is not a generic cast because a rounding policy is required; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

Exact conversions follow these rules:

- fixed-width integer -> `int`: exact; unsigned fixed-width integer -> `nat`: exact; `nat` -> `int`: exact;
- `int` or a signed fixed-width integer -> `nat`: explicit and checked to be non-negative;
- integer-family value -> `real`: exact;
- `real32`/`real64` -> `real`: exact conversion of the stored IEEE value, not reinterpretation of the original decimal spelling;
- `int`/`nat` -> fixed-width integer: explicit and range-checked;
- `real` -> integer-family type: accepted only when the mathematical value is provably integral, then range-checked when the destination is fixed-width or `nat`;
- `int`/`nat`/`real` -> `real32`/`real64`: explicit, with finite destination-range checking through the same whole-value `converted-type | error` conversion channel.

In short: **an explicit conversion may discard precision when the destination representation requires it, but it may not discard range or invent an integer rounding policy**.

## Parsing and text conversion

Numeric parsing is distinct from conversion:

```quidra
int | error count = int.parse("123")
real32 | error ratio = real32.parse("1.5")
int | error huge = int.parse("123456789012345678901234567890")
real | error exact = real.parse("0.1")
```

Scalar values provide `.string()` for their standard textual representation. `int.parse` and `real.parse` consume the input text directly rather than passing through a fixed-width integer or an IEEE type. `real32.parse` and `real64.parse` round the exact decimal value of the text once to the nearest value of their type, ties to even, subnormal values included, as literals do, so the text of any finite value parses back to the same value; text whose value lies beyond the finite range, or is not zero but rounds to zero, is an error.

JSON number parsing is likewise lossless at the syntax boundary: a valid JSON number token is retained even when it exceeds `real64` range. `json.Value.number()` performs IEEE-range conversion with the same rounding, while `json.Value.integer()` and `json.Value.real()` consume the preserved numeric token directly.

## Bin

`bin` is a mutable packed raw bit sequence with value semantics.

```quidra
bin zeros = bin.fill(8, 0)
bin ones = bin.fill(5, 1)
bin | error pattern = bin.parse("01010000")
```

`bin.fill(n, bit)` allocates exactly `n` bits, with `bit` restricted to `0` or `1`. `bin(value)` is reserved for explicit conversion. `len(value)` returns the bit count. Negative lengths, fill values other than 0 or 1, and invalid allocation sizes fail deterministically.

A written bit pattern is parsed rather than given a separate binary literal grammar:

```quidra
bin | error value = bin.parse("01010000")
```

`bin.parse` always has static type `bin | error`, for literals and runtime strings alike. Only `0` and `1` are accepted. A statically known invalid literal may be diagnosed at compile time; a statically known valid literal may be marked internally as success-proven so the backend can remove the impossible error branch without changing the source-visible union type.

Indexing and slicing operate in bits:

```quidra
bin bits = bin.fill(8, 0)
bin first = bits[0]
bin high = bits[0:4]
bits[0] = bin.fill(1, 1)
```

An index result is a one-bit `bin`. Bin elements do not expose addressable references. `print(value)` and `value.string()` display the exact 0/1 sequence.

## Bin conversion

All conversions between `bin` and other concrete types are explicit. The source must already have a concrete width:

```quidra
bin raw = bin(3567446)          // error: integer-family literal has no width
bin raw32 = bin(int32(3567446)) // 32-bit representation
bin raw64 = bin(int(3567446))   // 64-bit representation
```

`bin(integer)` preserves the integer type's fixed-width representation, including the defined two's-complement representation of signed fixed-width integers. `intN(bin)` and `uintN(bin)` require an exact bit-length match. `bin(bool)` produces one bit; `bool(bin)` requires exactly one bit.

Flat integer and bool arrays use the same rule:

```quidra
int8[] values = [80, -1]
bin raw = bin(values)
int8[] restored = int8[](raw)
```

`T[](bin)` requires `len(bin)` to be exactly divisible by the destination element width. No conversion pads, truncates, wraps, or silently changes bit count.

## Value semantics

Ordinary `bin` assignment is independent:

```quidra
bin a = bin.fill(3, 0)
a[2] = bin.fill(1, 1)
bin b = a
b[0] = bin.fill(1, 1)

print(a) // 001
print(b) // 101
```

The implementation stores payload bits packed into bytes, but that packing is not a source-level element model. Observable aliasing of whole storage still uses the language's explicit reference mechanisms.
