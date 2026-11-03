# Range and collection headers

Integer ranges have one direction throughout the header:

```quidra
for 0 <= nat i < len(values)
    print(values[i])
for len(values) > nat i >= 0
    print(values[i])
```

The four ascending combinations of `<` and `<=`, and the four descending
combinations of `>` and `>=`, independently include or exclude each end.
An excluded start advances by one step before the first value. Bounds and
an explicit `step` are evaluated once, from left to right. Integer steps
are signed and default to 1 or -1 according to the header's direction.
The body cannot assign or take a writable reference to a range variable.
The values lie on the exact step lattice; an empty range performs no
increment and a final value at a fixed-width limit does not overflow.

The inclusive zero end may be omitted:

```quidra
for nat i < len(values)       // 0 <= nat i < len(values)
    print(values[i])
for len(values) > nat i       // len(values) > nat i >= 0
    print(values[i])
```

`for int i <= 5` includes 0 through 5, and `for 5 >= int i` includes 5
through 0. In an ordinary `for`, an untyped variable uses an existing value binding,
for example `for i < limit` or `for limit > i`. In contrast,
`parallel for i < limit` declares a fresh loop-local `int i` and rejects an
already visible binding named `i`. Both omitted-zero forms accept `step`;
the implicit zero is always inclusive. A full three-operand range has
priority. With two untyped identifiers, `<` selects the left identifier as
the variable, and `>` selects the right one. These forms do not omit an
upper end: `for i > 5` and `for 0 < i` are errors. Real headers retain the
same bound spelling and require an explicit step.

Collection headers iterate exactly one level:

```quidra
for int value := values[]
    print(value)
for &value := &values[nat i]
    value = value + int(i)
```

Omitting the type before a new binding infers the element type. An explicit type must be the exact element
type. A value loop may also assign each element to an existing value
binding (`for value = values[]`); a zero-iteration loop preserves its old
value. `[nat i]` declares a new read-only natural index scoped
to the loop. A new element binding uses `:=`. The index is relative to the iterated collection, including a
slice. Write `values[]` to omit the index; an existing index variable and
`_` in the brackets are not index declarations.

A value loop observes the collection at entry. When a body may write that
storage, the loop preserves its entry value at the first possible write.
A reference loop requires storage after `&`: a binding, a field, or an
array element path. Calls and slices produce independent values and are
not reference-loop sources. Its variable aliases the live element: a
write through another path is immediately visible and no exit writes a
saved element back. A bin reference loop similarly aliases a bit place;
ordinary bit-reference bindings remain unavailable. Direct replacement of
the iterated storage or a prefix path is rejected. Potential replacement
through aliases or calls is checked after the statement and fails with
`FOR_ITERATION` before the loop advances.

`range` and `in` are ordinary identifiers. The old `range(...)` builtin and
`for ... in ...` forms are unavailable; ordinary unknown-name or header
syntax diagnostics apply.

Real headers require an explicit step, including omitted-zero forms.
Fixed-width operands materialize in their declared type, and the count is
computed on the exact unrounded lattice before each value is rounded once.
Exact `real` headers preserve that lattice exactly. A rounded value can
therefore equal an excluded boundary without changing the count.

`for _ = values[]` repeats the body by the entry length, without reading
any element or preserving a collection snapshot. `_` takes no type or
reference marker. Range discard loops compute only the trip count. A typed
bound determines their type; otherwise a non-`int` typed step does, while
all-integer-literal ranges use an internal exact counter.

The omitted zero has the resolved loop variable's numeric type. Real-family
headers use the same expansion: `for real64 x <= 1.0 step 0.1` starts at
`0.0`, and `for 1.0 >= real x step -0.1` ends at inclusive `0.0`. Every real
loop still requires an explicit step. A negative upper bound with an
ascending zero start gives an empty range; it does not change an existing
variable. Written bounds keep the ordinary numeric literal family rules.
