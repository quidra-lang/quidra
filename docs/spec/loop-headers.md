# Range and collection headers

`for:` declares a range variable or every collection index written in the header.
Plain `for` reuses those variables. Independently, `:=` declares a collection
element binding and `=` assigns an existing element binding. Ordinary declarations
outside loops continue to use `:=`.

## Range variables

```quidra
for: i < 10
    println(i)
for: 0 <= i < 10
    println(i)
for: 10 >= i > 0
    println(i)
for: 10 > i
    println(i)
```

A new range variable is local to the loop. An already visible or reserved name
is an error; adding an outer declaration never changes a declaration into reuse.
Use a plain header to reuse a mutable value binding:

```quidra
i := 99
for i < 3
    println(i)
println(i) // 2
```

The reused variable retains its last produced value. An empty range leaves its
previous value and initialization state unchanged. Range variables cannot be
assigned or passed by writable reference while their loop is active.

Omit the type when the bounds determine it. `for: i < 10` uses `int`,
`for: i < len(values)` uses `nat`, and an `int32` bound supplies `int32`.
Typed bounds and a non-`int` typed step must agree on one ordered numeric type;
untyped numeric literal expressions use that context. With only literal bounds
and steps, the ordinary `int` or exact `real` default applies. An integer step
of type `int` can remain signed for a natural range. Explicit contracts remain
available as `for: int32 i < 10` or `for: 0 <= nat i < len(values)`.
A typed declaration without the colon is invalid; plain `for` reuses a binding.

The four ascending combinations of `<` and `<=`, and the four descending
combinations of `>` and `>=`, independently include or exclude each end.
An excluded start advances by one step before the first value. Bounds and
an explicit `step` are evaluated once, from left to right. Integer steps
default to 1 or -1 according to the header's direction. A final value at a
fixed-width limit does not cause an extra overflowing increment.

The inclusive zero end may be omitted. `for: i <= 5` includes 0 through 5;
`for: 5 >= i` includes 5 through 0. `<` selects the left identifier as the
variable in an ascending shorthand, and `>` selects the right identifier in
a descending shorthand. A full three-operand range has priority. These forms
do not omit the other end: `for i > 5` and `for 0 < i` are invalid.

Real headers require an explicit step, including omitted-zero forms. The
omitted zero has the resolved loop type. Counts use the exact unrounded
lattice; each produced IEEE value is rounded once, never accumulated from
the previous rounded value. An exact `real` range preserves the lattice.
A rounded value can equal an excluded boundary without changing the count.

## Elements and indices

```quidra
for value := values[]
    println(value)
for: value := values[index]
    println(index)
for: &value := &values[index]
    value += int(index)
```

`:=` declares an element binding, inferring its exact source element type unless
a type is supplied. `=` reuses an existing mutable value binding of that exact
type. A reference loop uses `&` on both sides and declares its reference with
`:=`. Value loops use independent element values; reference loops alias live
element or bit storage.

An index has exactly type `nat` and is read-only during iteration. `for:`
declares each written index as a loop-local name; plain `for` requires an
existing mutable `nat` value binding for each index. A reused index is updated
before the body and retains its last position after the loop, even if the body
never reads it. An empty collection leaves it unchanged and uninitialized
storage remains uninitialized. Reference bindings and const values cannot be
reused as index storage. Indices start at zero relative to the iterated source,
including a slice. `values[]` omits the index; `_` is not an index name.

| Header | Element variable | Index variable |
| --- | --- | --- |
| `for: value := values[index]` | New | New |
| `for: value = values[index]` | Existing | New |
| `for value := values[index]` | New | Existing |
| `for value = values[index]` | Existing | Existing |

Each `[]` iterates one level. A value loop preserves the collection's entry
value when later header evaluation or body writes could change it. A reference
loop requires storage after `&`; calls and slices are independent values.
Replacing or resizing live iterated storage or a prefix path is rejected
directly, or checked after an aliasing call with `FOR_ITERATION`. A bit place
cannot be taken as an ordinary address.

## Multiple arrays

Several array sources zip by position; they do not form a Cartesian product.
Any source may provide an index, and a source without one uses `[]`:

```quidra
for: p := prices[i], int q := quantities[j]
    println("{i}: {p}, {q}")
for: p := prices[], q := quantities[j]
    println("{j}: {p}, {q}")
```

`for:` independently declares every written index, so both `i` and `j` in
the first header are new `nat` bindings and contain the same zip position.
Without the colon, every written index is reused. Each element's `:=` or `=`
still independently controls declaration or reuse. All element and index names
in one header must be distinct; writing the same new index twice is an error.

Sources are evaluated once, left to right, with each snapshot taken immediately
before evaluating the next source. All lengths are checked before publishing
any element or index. Unequal lengths fail with `FOR_LENGTH` before the body;
equal empty sources run zero iterations and preserve reused storage. Body writes
cannot change entry snapshots. Only Array value sources are supported in a zip:
reference sources, patterns, `bin`, structural traversals and ranges are not
combined with multiple arrays. Nested loops remain available for Cartesian work.

## Discard and parallel iteration

`for _ = values[]` repeats by the entry length without reading elements or
creating an element snapshot. It may have an index, for example
`for: _ = values[i]`. `_` binds nothing and takes no type or `&`.
A discarded range uses plain `for` and computes only its trip count. A typed
bound or a non-`int` typed step supplies its type; all-integer-literal discard
ranges use an internal exact counter.

Parallel ranges follow the same declaration rule: write `parallel for: i < n`
for a new range variable. Parallel collection elements still use `:=`.
The initial parallel implementation requires newly declared scalar value
bindings and does not accept index bindings, reference sources or patterns.

`range` and `in` are ordinary identifiers. The old `range(...)` builtin and
`for ... in ...` forms are unavailable.
