# Numeric Types and Bin

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float32`, `float`, `float64`

`int` and `int64` are the same signed 64-bit type. `float` and `float64` are the same IEEE-754 binary64 type. `float32` is IEEE-754 binary32.

There is no `char`, `bit`, `byte`, or `bytes` source type. Text uses `string`; a one-byte numeric value uses `uint8`; arbitrary raw bit sequences use `bin`. A quoted literal is always `string`.

## Numeric literal families

A numeric literal does not have a default concrete numeric type.

- `3` is an **integer-family literal**.
- `3.0` is a **floating-family literal**.

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

Within the floating family, destination rounding is allowed while materializing a literal, but the literal must remain inside the finite range of the destination type. Thus `float32 x = 0.1` is valid and deterministically rounds to binary32, while a finite literal larger than the maximum finite binary32 value is rejected.

Integer literals use decimal notation only. Base-prefixed forms such as `0x`, `0b`, and `0o` are not source syntax. Floating exponent notation requires an explicit decimal point:

```quidra
1e8      // error
1.0e8    // floating-family literal
1.0e-8   // floating-family literal
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

Floating-point to integer conversion is not a generic cast because a rounding policy is required; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

In short: **an explicit conversion may discard precision, but it may not discard range**.

## Parsing and text conversion

Numeric parsing is distinct from conversion:

```quidra
int | error count = int.parse("123")
float32 | error ratio = float32.parse("1.5")
```

Scalar values provide `.string()` for their standard textual representation.

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

`bin(integer)` preserves the integer type's fixed-width representation. `intN(bin)` and `uintN(bin)` require an exact bit-length match. `bin(bool)` produces one bit; `bool(bin)` requires exactly one bit.

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
