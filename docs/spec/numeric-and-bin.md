# Numeric Types and Bin

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float32`, `float`, `float64`
- `bigint`
- `bigreal`

`int` and `int64` are the same signed 64-bit type. `float` and `float64` are the same IEEE-754 binary64 type. `float32` is IEEE-754 binary32. `bigint` is an exact arbitrary-precision integer. `bigreal` is an exact mathematical-real value: finite decimals are exact rationals, while exact constants and irrational results may remain symbolic rather than being rounded to an IEEE representation.

There is no `char`, `bit`, `byte`, or `bytes` source type. Text uses `string`; a one-byte numeric value uses `uint8`; arbitrary raw bit sequences use `bin`. A quoted literal is always `string`.

## Numeric literal families

A numeric literal does not have a default concrete numeric type.

- `3` is an **integer-family literal**.
- `3.0` is a **real-family literal**.

The integer family can materialize as a fixed-width integer type or `bigint`. The real family can materialize as `float32`, `float`, or `bigreal`.

A literal becomes concrete only when surrounding source code determines exactly one type in the same family:

```quidra
int32 count = 3
uint8 channel = 3
float32 ratio = 3.0
float precise = 3.0
```

This contextual materialization is not an implicit cast. The literal was never an `int` or `float` value first.

Literal materialization never crosses families:

```quidra
float x = 3      // error
int y = 3.0      // error
```

Use an explicit conversion when a representation change is intended:

```quidra
float x = float(3)
```

When a real-family literal materializes as `float32` or `float`, destination rounding is allowed, but the literal must remain inside the finite range of that IEEE type. Thus `float32 x = 0.1` is valid and deterministically rounds to binary32. When the same literal materializes as `bigreal`, its decimal spelling becomes an exact rational value; it is not routed through `float` first.

Integer literal syntax has no implementation-width ceiling when the unique context is `bigint`:

```quidra
bigint exact = 1234567890123456789012345678901234567890
```

Fixed-width integer contexts still enforce their normal ranges.

Integer literals use decimal notation only. Base-prefixed forms such as `0x`, `0b`, and `0o` are not source syntax. Floating exponent notation requires an explicit decimal point:

```quidra
1e8      // error
1.0e8    // real-family literal
1.0e-8   // real-family literal
```

## `auto`

`auto` propagates a type that the initializer already determines independently. It never chooses a concrete numeric type.

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

## Fixed-width bitwise operations

Bitwise operations are defined only for the fixed-width integer types `int8`, `int16`, `int32`, `int`/`int64`, `uint8`, `uint16`, `uint32`, and `uint64`.

```quidra
uint8 flags = 240
uint8 mask = 204

uint8 both = flags AND mask
uint8 either = flags OR mask
uint8 changed = flags XOR mask
uint8 inverted = NOT flags
uint8 left = flags << 1
uint8 right = flags >> 2
```

`AND`, `OR`, `XOR`, and `NOT` are uppercase deliberately. Lowercase `and`, `or`, and `not` are bool-only logical operations; `&` remains explicit storage access and `|` remains union syntax. The language therefore does not reuse one spelling for unrelated meanings.

Binary bitwise operands have the same concrete fixed-width integer type. A bare integer-family literal may materialize from that operator context in the normal way. `float32`, `float`, `bigint`, `bigreal`, `bool`, `bin`, tensor, and neural values do not accept these operators.

Signed fixed-width integers use a two's-complement bit representation; unsigned integers use the ordinary modulo-`2^N` N-bit representation. `NOT` flips every bit of that fixed-width representation. `AND`, `OR`, and `XOR` operate on it directly. `<<` shifts the N-bit representation left and discards bits shifted beyond the width; it is a representation operation and does not raise arithmetic overflow. `>>` is arithmetic with sign extension for signed integer types and logical with zero fill for unsigned integer types. Shift counts must be nonnegative and smaller than the operand width; a provably invalid constant count is a compile-time `SHIFT_COUNT` error and a dynamic invalid count is a deterministic runtime `SHIFT_COUNT` failure.

These operations are representation operations rather than arithmetic conversions. They never change the operand type and never imply a cast.

## Exact integers and reals

`bigint` arithmetic `+`, `-`, `*`, integer `/`, and `%` is exact and does not overflow because storage grows with the value. The source type never changes as the value grows.

`bigreal` is not a configurable-precision floating type. Exact decimals are rationals; values such as `math.pi`, `math.e`, and `math.sqrt(2.0)` may remain symbolic. Algebraic simplification is valid only when it preserves the represented mathematical value. For example, an implementation may prove `math.sqrt(2.0) * math.sqrt(8.0) == 4.0` without approximating either square root.

Equality and ordering of exact symbolic values must not silently fall back to rounded IEEE guesses. If a result cannot be established within the runtime's finite proof budget, evaluation fails deterministically instead of returning an unproved Boolean. Decimal formatting is an observation of the exact stored value and does not mutate it.

`math.pi` and `math.e` are real-family constants whose concrete representation comes from context:

```quidra
float fast_pi = math.pi
bigreal exact_pi = math.pi
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

Explicit conversion may lose precision when the destination representation requires deterministic rounding. It must not silently leave the destination's representable finite range. Integer narrowing never wraps or clamps, and finite floating narrowing must not silently become infinity.

IEEE floating-point to an integer-family type is not a generic cast because a rounding policy is required; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

Exact conversions follow these rules:

- fixed-width integer -> `bigint`: exact;
- integer-family value -> `bigreal`: exact;
- `float32`/`float` -> `bigreal`: exact conversion of the stored IEEE value, not reinterpretation of the original decimal spelling;
- `bigint` -> fixed-width integer: explicit and range-checked;
- `bigreal` -> integer-family type: accepted only when the mathematical value is provably integral, then range-checked when the destination is fixed-width;
- `bigint`/`bigreal` -> IEEE float: explicit, with finite destination-range checking.

In short: **an explicit conversion may discard precision when the destination representation requires it, but it may not discard range or invent an integer rounding policy**.

## Parsing and text conversion

Numeric parsing is distinct from conversion:

```quidra
int | error count = int.parse("123")
float32 | error ratio = float32.parse("1.5")
bigint | error huge = bigint.parse("123456789012345678901234567890")
bigreal | error exact = bigreal.parse("0.1")
```

Scalar values provide `.string()` for their standard textual representation. `bigint.parse` and `bigreal.parse` consume the input text directly rather than passing through a fixed-width integer or IEEE float.

JSON number parsing is likewise lossless at the syntax boundary: a valid JSON number token is retained even when it exceeds `float` range. `json.Value.number()` performs IEEE-range conversion, while `json.Value.bigint()` and `json.Value.bigreal()` consume the preserved numeric token directly.

## Bin

`bin` is a mutable packed raw bit sequence with value semantics.

```quidra
bin zeros = bin.fill(8, 0)
bin ones = bin.fill(5, 1)
bin pattern = bin.parse("01010000")
```

`bin.fill(n, bit)` allocates exactly `n` bits, with `bit` restricted to `0` or `1`. `bin(value)` is reserved for explicit conversion. `len(value)` returns the bit count. Negative lengths, fill values other than 0 or 1, and invalid allocation sizes fail deterministically.

A written bit pattern is parsed rather than given a separate binary literal grammar:

```quidra
bin value = bin.parse("01010000")
```

A statically known valid string produces `bin` directly. A runtime string produces `bin | error`. Only `0` and `1` are accepted.

Indexing and slicing operate in bits:

```quidra
bin first = value[0]
bin high = value[0:4]
value[0] = bin.parse("1")
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
bin a = bin.parse("001")
bin b = a
b[0] = bin.parse("1")

print(a) // 001
print(b) // 101
```

The implementation stores payload bits packed into bytes, but that packing is not a source-level element model. Observable aliasing of whole storage still uses the language's explicit reference mechanisms.
