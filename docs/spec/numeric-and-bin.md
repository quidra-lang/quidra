# Numeric Types and Bin

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float32`, `float`, `float64`

`int` and `int64` are the same signed 64-bit type. `float` and `float64` are the same IEEE-754 binary64 type. `float32` is IEEE-754 binary32.

There is no `char`, `bit`, `byte`, or `bytes` source type. Text uses `string`; a one-byte numeric value uses `uint8`; arbitrary raw bit sequences use `bin`.

Integer literal magnitudes are accepted through the full `uint64` range. They are ordinary `int` literals unless an expected numeric type can represent the literal exactly. Floating literals are ordinary `float` literals unless an expected floating type can represent the literal exactly.

## Conversion

Already-typed values never change concrete type implicitly. Numeric literals may still be contextually typed when the expected numeric type can represent the literal.

Explicit conversion uses the destination type as a call:

```quidra
int8 small = 10
int16 wider = int16(small)

int value = 100
int8 checked = int8(value)
```

Integer narrowing succeeds only when the runtime value is inside the destination range. It never wraps or clamps. Floating-point to integer conversion is not a generic cast because a rounding policy is required; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

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

All conversions between `bin` and other concrete types are explicit.

```quidra
int8 number = -1
bin raw = bin(number)                 // 11111111
int8 again = int8(raw)                // -1
uint8 unsigned_value = uint8(raw)     // 255
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
