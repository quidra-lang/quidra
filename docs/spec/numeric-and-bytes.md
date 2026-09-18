# Numeric Types and Bytes

## Built-in scalar types

Quidra provides these numeric scalar types:

- `int8`, `int16`, `int32`, `int`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float32`, `float`, `float64`

`int` and `int64` are the same signed 64-bit type. `float` and `float64` are the same IEEE-754 binary64 type. `float32` is IEEE-754 binary32.

There is no `char` type. Text uses `string`; a one-byte numeric value uses `uint8`; raw binary sequences use `bytes`.

Integer literal magnitudes are accepted through the full `uint64` range. They are ordinary `int` literals unless an expected numeric type can represent the literal exactly; therefore a magnitude above signed 64-bit maximum requires an explicit `uint64` context. Values above `18446744073709551615` are rejected. Floating literals are ordinary `float` literals unless an expected floating type can represent the literal exactly.

## Numeric conversion

Already-typed numeric values never change representation implicitly. This includes widening changes such as `int8 -> int16` and `float32 -> float`. Numeric literals may still be contextually typed when the literal value is representable by the expected numeric type.

Explicit numeric conversion uses the destination type as a call:

```quidra
int large = 16777217
float32 rounded = float32(large)

float wide = 1.75
float32 narrow = float32(wide)

int value = 100
int8 checked = int8(value)
```

Explicit casts are practical rather than exact-only. Integer-to-floating-point and floating-point-to-floating-point casts use deterministic destination IEEE-754 rounding, so the programmer may explicitly request a representation with less precision. Integer narrowing is permitted only when the runtime value is inside the destination range; it never wraps or clamps.

Floating-point to integer conversion is deliberately not a generic cast because it requires an explicit rounding choice. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil`; these operations reject non-finite and out-of-range results deterministically. `math.round` rounds halfway cases away from zero.

`T(value)` applies the same policy to scalar values and numeric containers. For nested arrays it preserves every fixed/dynamic dimension and converts numeric leaves recursively; for tensors it preserves rank and shape facts while changing the element dtype. Integer narrowing is range checked, integer-to-floating-point and floating-point precision reduction may round, and floating-point containers cannot be generically cast to integer containers. `tensor.cast<T>()` is not a source-language operation.

## Parsing and text conversion

Numeric types provide a type method:

```quidra
int | error count = int.parse("123")
float32 | error ratio = float32.parse("1.5")
uint8 | error byte = uint8.parse("255")
```

Parsing interprets text and therefore is distinct from a cast.

Scalar values provide `.string()` for their standard textual representation:

```quidra
int value = 123
string text = value.string()
```

## Bytes

`bytes` is mutable raw binary data with value semantics. Each element is exactly one byte and has type `uint8`.

Construction is:

```quidra
bytes empty = bytes()
bytes zeros = bytes(1024)
bytes filled = bytes(1024, fill = 255)
```

The length is an `int`. Negative lengths are rejected when statically known and invalid allocation sizes fail at runtime. `len(data)` returns the byte count.

Bytes support indexing, mutation, safe element addresses, value iteration, writable iteration, and value equality:

```quidra
bytes data = bytes(4, fill = 0)
data[0] = 255

uint8 first = data[0]
uint8 &second = &data[1]
second = 7

for value in data
    print(value)

for &value in data
    value = 0
```

The runtime representation uses a length followed by contiguous one-byte elements. A byte is not boxed or widened to the normal `int` width in byte storage.

## Value semantics and storage sharing

Ordinary assignment is defined by observable value semantics:

```quidra
bytes a = bytes(3, fill = 1)
bytes b = a
b[0] = 9

print(a[0]) // 1
print(b[0]) // 9
```

The same independence rule applies to arrays and classes. The implementation may share backing storage, use copy-on-write, elide copies, move values, or use reference counting when those choices are not observable.

Observable aliasing is introduced explicitly with `&`:

```quidra
bytes &alias = &a
alias[0] = 9
print(a[0]) // 9
```

`string` is immutable, so multiple string values may share the same underlying text storage without copy-on-write. Reassigning one string binding never mutates another string value.

The language does not expose allocation identity, physical addresses, or an identity-comparison operator.
