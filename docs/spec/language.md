# Quidra Language Specification

The [factorial](numeric-and-bin.md#factorial) and [matrix multiplication operator](#matrix-multiplication-operator) sections record the approved `develop` contracts and identify the features awaiting implementation.

## Source structure

Top-level statements execute in source order. Functions have explicit return and parameter types and are declared at top level. User-defined top-level names are downward-visible by default: a function or class becomes visible at its declaration point rather than being implicitly hoisted.

A top-level function may opt into forward visibility with a same-file prototype: write its complete signature without a body and terminate that declaration with `;`, for example `int calculate(int value);`. A class prototype repeats the complete generation header and ends with `;`, for example `class User();` or `class Scaled scaled(real64 x, local real64 scale := 2.0);`. A function prototype must have exactly one matching later definition in the same source file; its return type, generic contract, parameter types/names/reference/const forms and its `private` prefix must match exactly, and a class prototype and its definition agree on `private`, the generic contract, the optional instance name, the ordered parameter names/types/reference/const/local forms, and the generation error contract as well. Defaults, when present, belong on the prototype only and are omitted from the later definition. A class prototype exposes only the nominal type until its matching later definition: construction, fields, methods, and by-value layout-dependent uses require the complete class. Prototypes are not `extern` declarations, do not cross module boundaries, and do not create overload sets. Enum and class-member prototype forms do not exist. The semicolon is reserved for top-level function/class prototypes; ordinary statements and complete definitions do not accept it.

Every source file also has one compiler-owned direct-execution flag named `main`. User source can observe it only with an exact top-level `if main` guard. The directly executed root file treats its own guard as true; every imported file treats its own guard as false. Imported `if main` bodies are still parsed and type-checked but emit no runtime module-initialization code. The guard has no `elif` or `else`, does not hoist declarations, and multiple guards execute at their source positions when the file is run directly. User code cannot declare, assign, shadow, store, pass, return, or take the address of `main`. REPL submissions do not receive a synthetic per-submission `main`.

```quidra
int calculate(int value);

if main
    println(calculate(10))

int calculate(int value)
    return value * 2
```

Blocks use four spaces per indentation level; tabs are invalid as indentation. A dedent closes a block. Blank and comment-only lines do not affect nesting. Parentheses and square brackets permit line continuation. A trailing comma is optional in a list; canonical formatting omits it on one line and includes it in multiline lists. Strings may contain literal newlines and literal tab characters. Line comments start with `//`.

## Types

Numeric built-ins are `int8`, `int16`, `int32`, `int64`, `int`, `nat8`, `nat16`, `nat32`, `nat64`, `nat`, `real16`, `real16b`, `real32`, `real64`, `real`, `com16`, `com16b`, `com32`, `com64`, and `com`. `int8` to `int64` are the signed and `nat8` to `nat64` the unsigned fixed-width integers; `real64` is IEEE-754 binary64; `real32` is IEEE-754 binary32; `real16` is IEEE-754 binary16 and `real16b` bfloat16, both computed in binary32 and rounded once per operation. `int` is an exact arbitrary-precision integer and `nat` an exact arbitrary-precision natural number (a `nat` result below zero is an error); neither overflows, and neither has bit operations, a C ABI or tensors. `real` represents exact rationals and exact real expressions constructed from `real` arithmetic, such as `real(1) / real(3)`. The complex types hold a real and an imaginary part (`z.real`, `z.imag`) of `real16`, `real16b`, `real32`, `real64` and `real`; they have `+`, `-`, `*`, `/`, `==`, `!=` and, fixed-width, `^`, but no ordering; mixed numeric operators use a common complex type, and numeric destinations use checked implicit conversion; explicit `com32(x)` remains available. Imaginary (`2.0i`) and complex (`1.0 + 2.0i`) literals materialize as them (docs/spec/numeric-and-bin.md). Numeric literals use compatible resolved context; without one, integer, real, and imaginary/complex literals default to `int`, exact `real`, and exact `com`. Natural types require context. Integer-family literals may materialize as fixed integers, `int` or `nat`, while real-family literals may materialize as `real16`, `real16b`, `real32`, `real64` or `real`. Multiple compatible alternatives in a numeric union context remain ambiguous. An `int` or `nat` context admits decimal integer literals beyond `nat64`; fixed-width contexts retain their normal range limits. `bool`, `string`, `bin`, and `error` hold booleans, immutable text, packed raw binary data, and error information. There is no `char`: text uses `string`, a one-byte numeric value uses `nat8`, and arbitrary raw bit sequences use `bin`. `void` denotes normal completion without data. Process termination is control flow rather than a source-visible value type. An initialized binding can omit its type when the initializer determines it. If the initializer's unnamed union contains `error`, the inferred declaration keeps the complete non-`error` success side and fails fast on `error`. Declare the full union explicitly to retain the failure alternative.

`T | U` is an untagged surface description of a runtime tagged union: exactly one alternative is active. Union types flatten nested alternatives, remove duplicates, and have deterministic canonical representation independent of spelling order. A union with one distinct member is that member. Values and smaller unions can flow to compatible larger unions without wrapper calls; the runtime discriminator is adjusted to the destination type.

`none` is the absence alternative of a union. `string | none` can hold text or normal absence; `string | none | error` additionally admits failure. Standalone `none` declarations, function returns typed solely `none`, and `x := none` are invalid. `void` is allowed as a function result and as an alternative in a union. Non-continuing control flow is compiler-internal and is neither storable nor source-spellable.

Omitting the type is the standard style for a `:=` declaration when its initializer determines the intended static type. This applies to values, constants, initialized fields, defaulted parameters and collection loop bindings. For example, given `class Point(real64 x, real64 y)`:

```quidra
point := Point(1.0, 2.0)
image := tensor.zeros<real32>([3, 224, 224])
```

Write an explicit type when it supplies a needed context or states a stronger contract. For example, `int count := 0` chooses the numeric type, `string[] names := []` sets the element type, and an explicit union containing `error` keeps the failure alternative.

## Arrays and shapes

Array type information precedes the binding name:

```quidra
values := [1, 2, 3]
int[3] fixed := [4, 7, 8]
int[2][3] matrix := [[1, 2, 3], [4, 5, 6]]
```

`T[]` has runtime length. `T[expr]` accepts a `nat` extent expression; a signed integer expression requires an explicit conversion such as `nat(n)`. A compile-time expression is folded; otherwise it is evaluated once when that array binding is created and the resulting nonnegative extent is captured for that binding. Later mutation of variables used by the expression does not change the captured contract. Dimensions read from the outside inward: `int[5][6]` contains five arrays of six integers, while `real64[][nat(n * m)]` has a runtime-sized outer dimension and a captured inner extent. Unconstrained `T[]` dimensions may still be ragged.

An array's storage and nested elements obey value semantics. Copying a nested array cannot make a later write to one copy modify another.

`array(count)` creates runtime-sized storage whose elements are initially uninitialized. It therefore requires an explicit array type context:

```quidra
int[] values := array(100)
values[0] = 7
println(values[0])
```

`values := array(100)` is invalid because there is no element value from which to infer the type. Reading an element that no path initializes is a compile-time error (`UNINITIALIZED`, "'values[1]' is read before it is initialized on every path."): a constant index into a local array created without element values (`T[n] values`, `T[] values := array(k)`) whose element no store has reached, any index into one that nothing has written, or an equality over one with such an element. Otherwise reading an element before it has been initialized is a deterministic runtime safety error. The implementation tracks initialization independently from stored bits, using compact per-element state rather than treating zero or null as uninitialized.

`array(count, fill = value)` creates a fully initialized array and may infer its element type. The fill expression is evaluated once and every element receives an independent value-semantic copy. Negative counts and invalid allocation sizes produce runtime failure 101. Count zero is valid.

A fixed declaration such as `int[10] values` allocates its fixed storage immediately with all elements initially uninitialized. Individual elements may be written before the whole array is read. Fixed contiguous dimensions remain eligible for flattened native storage.

`[]` requires enough contextual element-type information. An inferred binding initialized directly from an array literal infers runtime-sized `T[]`, preserving the normal appendable-array behavior. When an inferred binding receives an array-valued expression whose static type is already fixed, such as `.shape()` on a tensor whose rank is compiler-known, that fixed dimension is preserved rather than erased. A trailing comma does not add an element.

Array element indices accept every integer kind. Nonnegative indices count from zero. A negative index `i` selects position `a.len() + i`, so `a[-1]` is the last element and `a[-int(a.len())]` is the first element of a nonempty array. For an array of length `n`, the valid indices satisfy `-n <= i < n`. Reads, writes and element references use the same rule for both `T[]` and `T[n]`. A constant out-of-bounds index into a statically sized array is a compile-time error. Other out-of-bounds indices fail at runtime. Slices use the separate bound rules below.

Assignment to an element does not append or resize an array. `a.len()` returns the outer length of a fixed or runtime-sized array as `nat`, without reading its elements. Lengths, counts, shapes, sizes and the index values APIs return are `nat`, so `a.len() - 1` is a checked `nat` subtraction that fails on an empty array, while the signed `int(a.len()) - 1` is `-1`. Write `int(a.len()) - int(b.len())` for a signed difference of lengths. The built-in `.len()` method takes no arguments and is defined on arrays, strings and `bin`. The bare name `len` is an ordinary identifier.

### Array selection, elementwise operators and ranges

The six arithmetic operators `+`, `-`, `*`, `/`, `%` and `^` apply
elementwise to ordinary numeric arrays. Array-array arithmetic requires
identical shape at every nested axis, including ragged dimensions. Array-scalar
arithmetic applies the scalar to each leaf. Elements and scalars convert to
their common numeric type before each operation. Array-array broadcasting
does not occur. Exponentiation is `array ^ scalar`
only. All operations produce independent array values and preserve fixed
lengths where statically known. Errors in individual operations, such as
integer overflow or divide by zero, use the scalar numeric contract.

The four ordering comparisons `<`, `<=`, `>` and `>=` also work
elementwise on ordered numeric arrays and return a same-shaped `bool` array.
Array `==` and `!=` remain whole-array comparisons requiring matching container types, not mask producers;
array comparison chains are not permitted.

```quidra
values := [-2, 3, 0, 4]
positive := values > 0
selected := values[positive]   // [3, 4]
squared := values ^ 2         // [4, 9, 0, 16]
indices := [3, 1, 3]
gathered := values[indices]   // [4, 3, 4]
locations := values.indices()  // [0, 1, 2, 3]
```

A `bool[]` mask must match the input array's outer length; it selects complete
outer-axis elements and preserves their order. A `nat[]` or `int[]`
index array gathers outer-axis elements in its specified order, including
repeated positions. Negative indices in an `int[]` count from the end, just as
with a scalar index. The result is a new independent array, and invalid index
values are errors. Multi-element mask/index selection is read-only in this
version: use individual indexed assignments for writes. The `.indices()`
method always creates a `nat[]` for the outermost dimension, including an
empty result for an empty array.

A bounded colon range can also appear as an independent expression: it
materializes a normal integer array, never a lazy iterator. Both endpoints
are inclusive by default, and the same exclusion markers and step form
used by slices are available. An omitted start is zero. An omitted end is
only meaningful when indexing an actual array, not in an independent range.

```quidra
a := 0:3       // [0, 1, 2, 3]
b := :3        // [0, 1, 2, 3]
c := 0:<3      // [0, 1, 2]
d := 5:0:-2    // [5, 3, 1]
e := 3:0       // [3, 2, 1, 0]
```

`int[] invalid := 3:` is rejected at compile time because a standalone range must have an end.

Without directional exclusion markers, an omitted step follows the endpoint
order; equal endpoints allow either nonzero explicit step. Directional markers
take precedence, as in slices. A marker pointing away from the endpoint produces
an empty array when no step is supplied. An explicit step must agree with both
the markers and the order of unequal endpoints. Zero steps are always errors.
Direction and length arithmetic use exact integers, including steps beyond
machine integer limits; element and allocation limits are still checked.

Signed endpoints and steps need no parentheses (`-3:0`, `0:-3`, `3:0:-1`).
Parenthesize compound endpoint expressions, for example `start:(stop + 1)`.
Arithmetic following the range applies to the generated array: `(:9 ^ 2)`
produces the squares from zero through nine.

A colon within brackets, such as `values[:3]`, remains a direct slice and
does not first construct an index array. The stand-alone range must fit its
integer element type and the runtime allocation limit.

### Slices

A slice `a[start:end:step]` selects elements of an array by position and gives them as a new runtime-sized array: a slice of `T[]` or `T[n]` is a `T[]` whose elements are independent copies, so writing the slice never changes `a`, and writing `a` never changes the slice. Start, end and step accept every integer kind, as an index does. A slice takes one item: `a[1:3]`, never `a[1:3, 0]`.

Each side of a slice is inclusive, exclusive, or omitted. A side without a marker is inclusive. A marker written directly next to the first colon, with no whitespace between, makes its side exclusive and gives the slice its direction:

| Form | Meaning |
| --- | --- |
| `s<:` | ascending; the start `s` itself is not selected |
| `s>:` | descending; the start `s` itself is not selected |
| `:<e` | ascending; the end `e` is not selected |
| `:>e` | descending; the end `e` is not selected |

Anywhere else, `<` and `>` are comparisons: `a[x < y]` is an index, `a[f(x < 3):]` and `a[(x < 3):5]` compare inside the call or the parentheses, and `a[s < :e]` and `a[s: <e]` are errors, because the marker is separated from its colon. `a[s<: e]` is a valid exclusive start (only the marker has to touch the colon), `<=` and `>=` are never markers, and no marker belongs to the second colon (the step). A marker needs its value: `a[s:<]`, `a[<:e]` are errors.

The direction follows an explicit step's sign, then any marker (`<` ascending, `>` descending). With neither, two explicit endpoints choose ascending when start <= end and descending when start > end; if either endpoint is omitted, the default is ascending. The comparison uses the evaluated values, so dynamic endpoints can change direction. Two markers that point different ways (`a[s<:>e]`) and a marker against the step's sign (`a[0:<5:-1]`) are `SLICE_STEP` errors, at compile time for a constant step and at run time otherwise, and so is a step of 0. The default step is `1` ascending and `-1` descending, so a marker alone makes a descending slice: `a[a.len()>:0]` steps by -1 and reverses the whole array.

The start anchors the progression `s`, `s + step`, `s + 2·step`, ...: an exclusive start drops `s` itself, so `a[0<:6:2]` selects 2, 4, 6 while `a[1:6:2]` selects 1, 3, 5. The progression continues while it has not passed the end (an inclusive end is still a bound when the step skips it), and stops before an exclusive end. These are the rules of `for` range headers, and the forms map onto them:

| Slice | Selected positions | As a range |
| --- | --- | --- |
| `a[1:4]` | 1, 2, 3, 4 | `1 <= i <= 4` |
| `a[4:1]` | 4, 3, 2, 1 | `4 >= i >= 1` |
| `a[4:0]` | 4, 3, 2, 1, 0 | `4 >= i` |
| `a[0:<N]` | 0, 1, ..., N - 1 | `0 <= i < N` |
| `a[0:<N:2]` | 0, 2, 4, ... below N | `0 <= i < N` step 2 |
| `a[N>:0]` | N - 1, ..., 1, 0 | `N > i >= 0` |
| `a[N>:0:-2]` | N - 2, N - 4, ..., at least 0 | `N > i >= 0` step -2 |
| `a[10:>0:-2]` | 10, 8, 6, 4, 2 | `10 >= i > 0` step -2 |
| `a[10>:0:-2]` | 8, 6, 4, 2, 0 | `10 > i >= 0` step -2 |
| `a[0<:5]` | 1, 2, 3, 4, 5 | `0 < i <= 5` |

When direction must stay fixed, write `a[start:end:1]` or `a[start:end:-1]`. If two explicit endpoints oppose that step's sign, the slice fails with `INDEX_BOUNDS`; it never reverses automatically or silently returns an empty slice, even when a side is exclusive. The same constant/runtime checking rules apply.

An omitted start is the first element in the slice's direction (index 0 ascending, the last index descending), included; an omitted end runs through the last element in that direction. So `a[:]` is the whole array, `a[s:]` runs to the end and is empty when `s` is the length, and `a[::-1]` reverses (an empty array gives an empty slice). The omitted descending start is an inclusive anchor, unlike the exclusive start `a.len()>:`: both reverse with step -1, but over five elements `a[::-2]` selects 4, 2, 0 and `a[5>::-2]` selects 3, 1.

Omitted slice endpoints denote the sequence's beginning or end in the selected direction. They do not use the zero default of a `for` range's omitted endpoint: `a[4:]` runs to the sequence's end, whereas `for: 4 >= int i` runs to zero.

Slices are checked strictly, with no clamping. For a value of length `len`:

1. every selected position lies in `[0, len)`;
2. an excluded endpoint lies in `[-1, len]`, at most one position outside: `a[-1<:2]` is `a[0:2]`, and `a[2:>-1]` is `a[2:0:-1]`;
3. an inclusive end is a valid position, or `len` when the slice is empty;
4. so is an inclusive start;
5. without an explicit step, the start may pass an explicitly marked direction's end only when a side is exclusive, and the slice is then empty; an explicit step with two explicit endpoints rejects reversed endpoints. A slice inclusive on both sides is never empty.

| Form | Bounds | Empty when |
| --- | --- | --- |
| `a[s:e]`, `s <= e` | `0 <= s <= e < len` | never |
| `a[s:e]`, `s > e` | `len > s > e >= 0` | never |
| `a[s:<e]` | `0 <= s <= len`, `-1 <= e <= len` | `s >= e` |
| `a[s<:e]` | `-1 <= s <= len`; `0 <= e < len`, or `e = len` when empty | `s + step > e` |
| `a[s<:<e]` | `-1 <= s <= len`, `-1 <= e <= len` | `s + step >= e` |
| `a[s:e:k]`, `k < 0` (descending) | `len > s >= e >= 0` | never |
| `a[s:>e]` | `0 <= s < len`, or `s = len` when empty; `-1 <= e <= len` | `s <= e` |
| `a[s>:e]` | `-1 <= s <= len`; `0 <= e < len`, or `e = len` when empty | `s + step < e` |
| `a[s>:>e]` | `-1 <= s <= len`, `-1 <= e <= len` | `s + step <= e` |

Over ten elements `a[5:3]` selects 5, 4, 3, while `a[5:<3]` and `a[3>:5]` are empty. `a[5:3:1]`, `a[5:<3:1]`, `a[3:5:-1]` and `a[12:<3]` fail. A slice outside these bounds fails with `INDEX_BOUNDS`, which shows the slice as written with its operands' values (`slice 2:<7 out of bounds for length 5`), at compile time when the operands are constant and the length is known (a fixed array), at run time otherwise. Reading a slice reads the selected elements, so each must be initialized.

```quidra
values := [10, 20, 30, 40, 50]
middle := values[1:3]
reversed := values[values.len()>:0]
evens := values[0:<values.len():2]
println(middle[0] + reversed[0] + evens[2])
// output: 120
```

Strings take the same slices over Unicode code points, and a slice of a `string` is a `string`: `text[1:3]`, `text[:<n]`, and `text[text.len()>:0]` or `text[::-1]` for the reversed text. A string slice with constant operands over a string literal is checked at compile time.

A slice of an array is also an assignment target. `a[1:3] = b` stores the elements of `b` into the selected elements, in the order of the slice, which may be strided, descending or exclusive (`a[a.len()>:0] = b` stores `b` reversed). The value must be an array whose element type equals the target's exactly (no conversion and no broadcast) and must have exactly as many elements as the slice selects: `SLICE_ASSIGNMENT` (`slice assignment needs 3 values, got 2`), at compile time when both counts are known. The slice's operands are evaluated first, then the value completely, then the elements are written; the value is independent of the target, so `a[1:<4] = a[0:<3]` is safe. A slice assignment initializes the elements it writes. A slice is a value, not storage: `&a[1:3]` is an error, and a string slice is never assigned (`INVALID_ASSIGNMENT`): strings are immutable.

## Bin

`bin` is a mutable packed raw bit sequence. It is not an array type and has no separate `bit`, `byte`, or `bytes` element type.

```quidra
zeros := bin.fill(8, 0)
ones := bin.fill(5, 1)
bin | error pattern := bin("01010000")

data := bin.fill(8, 0)
data[0] = bin.fill(1, 1)
first := data[0]
nibble := data[0:<4]
```

`bin.fill(n, bit)` allocates exactly `n` bits (a `nat`) and accepts only `0` or `1` for `bit`. Length zero is valid. Invalid allocation sizes and fill values other than 0 or 1 are rejected. `data.len()` returns the bit count as `nat`. Indexing is zero-based and returns a one-bit `bin`; slicing takes the forms of array slices without a step (`data[0:<4]` excludes bit 4, `data[0:3]` includes bit 3, `data[data.len()>:0]` reverses the bits) and returns `bin`; a start past an excluded end gives an empty `bin`. `print(data)` and `string(data)` expose the exact 0/1 sequence.

Written bit patterns use parsing rather than a separate literal grammar. `bin(text)` accepts only `0` and `1` and always has static type `bin | error`, whether `text` is a literal or a runtime value. A statically known invalid literal may be rejected at compile time. A statically known valid literal may carry an internal success fact for optimization, but that fact does not remove the `error` alternative from the source-visible type.

Conversions between `bin` and other concrete types are always explicit. `bin(integer)` preserves the integer type's fixed-width bit representation, and `intN(bin)` / `natN(bin)` require the bit length to equal the destination width exactly. `bin(bool)` produces one bit and `bool(bin)` requires exactly one bit. Flat integer/bool arrays convert explicitly with `bin(values)`; the reverse uses `T[](bits)` and requires the bit length to be exactly divisible by the element width. No conversion pads, truncates, wraps, or silently changes bit count.

Ordinary `bin` assignment has independent value semantics. A later write to one copy cannot change another copy. The implementation stores the sequence packed into bytes internally, but storage packing is not a source-level element model.


## Bindings and definite initialization

An initialized declaration uses `T name := expression`. When the
initializer determines the type, write `name := expression`. The
declaration creates a fresh binding and fixes its type. Use `=` to
assign to an existing binding, including its first assignment after a
typed declaration without an initializer. The former `auto` keyword
and `T name = expression` declaration syntax are rejected.

`local` explicitly declares a binding in the current scope: `local int count := 0`
or `local const string label := "ready"`. It is visible after its declaration in
that scope and its inner control blocks, and does not leak into an adjacent or
outer block. Functions and classes do not implicitly capture it. A local
binding never creates a member or an exported module constant. Inside a
function, `if`, `for`, `while`, `match` or `parallel` block, `local` is optional:
the same declaration without it has the same storage, initialization and
lifetime rules. A function parameter may be written `local T name`; in an
ordinary function this has exactly the type and calling convention of `T name`.
Qualifiers precede the type in the order `local const T name`.
A destructuring declaration can qualify its whole pattern: `local (int count, string label) := (7, "ready")`. The qualifier does not turn an assignment to existing places into a declaration.

```quidra
int x
if true
    x = 1
else
    x = 2
println(x)
```

A declaration without an initializer leaves a scalar or ordinary aggregate binding uninitialized. A fixed array's storage exists immediately, while initialization is tracked per element. A class-typed declaration such as `Point point` declares only an uninitialized binding: no usable instance exists until whole-value initialization or assignment. Before that, member reads, member writes, method calls and instance borrowing are prohibited. Successful explicit generation executes the class generation body and member initialization expressions; copying an existing initialized instance does not repeat them. Standard-library value types that user code cannot construct, such as file handles and random generators, stay uninitialized until the library supplies a value. Runtime-sized `array(n)` values use the same per-element model. Reading an element that has not been initialized is rejected at runtime; whole-array operations require the participating elements to be initialized. Whole-value assignment establishes the destination value.

Every read of a binding has one of three initialization states, decided from the paths that reach it; branches that return, break or continue do not contribute to a merge. A read that is initialized on every path has no check: both branches of an `if` assigning, or a straight-line assignment before the read. A read that is uninitialized on every path is a compile-time error, `UNINITIALIZED` with "'x' is read before it is initialized on every path.". A read that is initialized on some paths and not on others, such as after an `if` whose other branch does not assign, after a loop that may run zero times, or after a `match` with a case that does not assign, compiles and is checked at run time: if the binding is uninitialized when the read happens, the program stops with the deterministic runtime failure `UNINITIALIZED`, "'x' is uninitialized", at the read. Past such a read the binding is initialized. A loop whose body runs at least once, over a non-empty array literal, a fixed array of length one or more, or a range header with constant bounds and step that produces at least one value, and that is left only at the end of its body (no `break` or `continue` of its own), initializes after it what its body initializes on every path, with no check.

```quidra
int choose(bool ready)
    int x
    if ready
        x = 1
    return x

println(choose(true))
```

The cost is one hidden flag per binding that has such a read, set where the binding is initialized, and a branch at each such read; a program whose reads are all initialized on every path has neither. A read through a reference uses the state of the binding the reference designates; when the target is not known after a join, a read that is not initialized on every path stays a compile-time error ("may be uninitialized through a reference whose target is not known"). A binding that a call may write without initializing it on every path, through a `&` argument or a receiver, is not tracked by a flag either, so a read of it that is not initialized on every path also stays a compile-time error ("may be uninitialized").

`x := expression` infers a static storable success type. When the expression has type `T | error` (or `T | none | error`), the inferred binding is `T` (or `T | none`) and an actual `error` fails fast at that initialization site. Use an explicit type such as `T | error x := expression` to retain the failure alternative. Assignments preserve the declared or inferred type, applying checked conversion when both source and destination are numeric. Numeric arithmetic supports compound assignments `+=`, `-=`, `*=`, `/=`, and `%=` when the corresponding operation is valid. They compute in the ordinary numeric common type and then apply checked conversion of the completed result back to the storage type. String concatenation supports `+=`. For arrays, the target must be writable whole-array storage: a binding, field, or array-valued indexed place; a slice assignment needs an explicit array result. The assignment target is evaluated exactly once, so an expression such as `values[next(&index)] += 1` does not repeat the index computation or its side effects. `a[i] op= e` is `a[i] = a[i] op e` with the target's subexpressions evaluated once, applied to the target as it is after `e`: when `e` replaces, grows, or shrinks the array or object that the target belongs to, the result is stored at the same index or field of the current one, and an index that is no longer in bounds fails with `INDEX_BOUNDS` at the target. A plain assignment `a[i] = e` follows the same order without the read: the target's index operands first, then `e`, then the store into the target as it is after `e` (see "Evaluation order, execution order and numeric determinism").

Ordinary value arguments never specialize a call's static return type based on their values. Values determine behavior and compile-time facts; static types determine static types. An explicitly supplied expected type may constrain a result, and generic specialization resolution may select a return type from the static types of arguments. Compile-time-known values may still prove invalid input, prove an expected-type contradiction, remove unreachable checks, or enable other optimization; those facts remain internal and do not silently narrow the source-visible return type. In short: **values determine behavior and facts; types determine types; write the type when the result type must be fixed.**

Quidra uses reservation plus monotonic visibility. A language-reserved identifier cannot be introduced as a root/unqualified user binding, parameter, function, class, field, method, generic parameter, loop/match binder, CLI field, or import alias. A name the language removes (for example the old type spellings `float`, `uint8` to `uint64`, `bigreal` and the `exact` namespace) is released: it is an ordinary identifier again, and code that still uses it gets the ordinary diagnostic, such as `UNKNOWN_TYPE` or `UNKNOWN_NAME`. `_` is not a reserved word but the discard name, which names nothing anywhere (below). Names that begin with `__quidra_` belong to the compiler, which gives them to the instances of generic declarations and to its own helpers, so no declaration in any module may use one (`SHADOWING`). Imported module exports are the deliberate namespace boundary: an imported module may export a top-level declaration whose spelling is otherwise reserved, because callers can reach it only through the import alias (for example `vision.write`). That declaration never becomes a bare binding, and import aliases themselves remain reserved. Standard-library internal declarations remain language-owned; their members never conflict with user declarations.

For other user names, a declaration cannot shadow a name that is already visible in its lexical environment. The lexical environment of code in an imported module is that module: its locals (those of its `if main` guard included) and the parameters, generic type parameters, fields, and methods of its declarations are checked against the module's own declarations and imports, never against the importing program's names, and a generic body instantiated for an importer is checked the same way. A package's `save<M>(M model, ...)` therefore coexists with an importer's `int M()` or `class model`. Bare names in that code resolve in the same environment: a function value or call names the module's own function, a field read inside a method names the class's field, and the importing program's top-level functions are never reachable from it, so an importer's `int storage()` cannot replace a package function value or hide a package field named `storage`. The same spelling may be reused only in disjoint scopes where the earlier declaration is not visible. In short: **reserved bare names are not reusable; imported namespace members are qualified; visible names are never shadowable.**

### Range constraints

Range syntax in a declaration or guarded assignment constrains one ordered numeric
scalar, without changing its storage type:

```quidra
0 < x < 5 := 3
candidate := 4
0 < x < 5 = candidate
```

`:=` declares the subject; `=` guards a store to an existing subject. An annotation
may identify its type (`0 <= int32 count <= 10 := value`); otherwise the initializer
or parameter default determines it before the bounds are checked. Bounds use
ordinary numeric common-type conversion and cannot be boolean or complex values.
A two-bound range chooses its central name. In a single comparison, an explicitly
typed name is the subject; otherwise two untyped names such as `x < y` choose the
left name, while a literal on the left such as `0 < x` chooses the right name.
Malformed or ambiguous ranges are diagnosed. Outside these contextual forms,
`0 < x < 5` remains an ordinary boolean comparison.

Evaluate the initializer or assignment RHS and each bound at most once. Bounds
run in their written order, short-circuiting at the first failed comparison,
including descending chains. Convert
the candidate to its destination type first, then validate that converted value,
then commit the write. A failed guard does not store the candidate. The
compiler rejects violations it can prove from constant facts; otherwise the
program fails fast with `RANGE_CONSTRAINT` before the guarded store. Release
builds retain every check that is not proven redundant.

Functions and methods may constrain parameters, for example
`void use(3 < int n < 7, limit < 9 := 8)`. Required parameters need an explicit
type; a default may supply one. Entry checks apply to supplied and defaulted
arguments, including named calls and calls through function values, before the
body runs. Class-header ranges use the same syntax and check generation inputs
before the ordered class body. They are generation preconditions rather than
permanent member invariants: a later guarded assignment states its own check.
[Tensor-axis ranges](#shapes-in-braces-and-shape-variables) additionally preserve
axis equality or capture the incoming dimension.

Shape ranges are supported on direct tensor bindings, parameters, results and
match cases. A tensor range nested inside an array, tuple or union is diagnosed;
function types own separate shape scopes and retain their tensor contracts.

### The discard name `_`

`_` is the discard name: it stands for a value that is not kept, so it binds nothing and is never a name. `_` cannot be declared as a binding, parameter, field, function, method, class, enum, variant, generic type parameter, loop or match binder, or import alias, in the root program or in an imported module, and it can never be read: `print(_)`, `f(&_)`, `"{_}"` and `_ = f()` are compile-time errors. `_` takes no type either, so `int _ := f()` is reported as "'_' takes no type; its type is inferred." A value that is not needed is dropped with an expression statement, `f()`. These errors are `DISCARD`. In a tensor shape pattern, `_` keeps its meaning of an axis of any extent (`tensor<real32>{3, _}`) and names no extent there either.

## Functions, calls, and defaults

```quidra
int add(int a, b := 1)
    return a + b

println(add(41))
println(add(a = 40, b = 2))

string greet(label := "hello")
    return label

println(greet())
println(greet(label = "hi"))
```

Function results and required parameters use explicit types. A default parameter uses
`:=` in its declaration. Omit the parameter type when the default expression
determines one, as with `label := "hello"`. The resulting type is fixed for all
calls, including those that supply an argument. Named arguments use the declared parameter name followed by `=`. Positional arguments precede named arguments. Unknown names, duplicate supply, missing required arguments, and positional arguments following named arguments are errors. Ordinary function overloading is not supported: a function name has one ordinary definition, so argument-count variants and return-type-only variants are errors. Optional call forms belong in default parameters. The only repeated-name exception is a generic specialization family described below.

A parameter without `&` receives a value: the callee sees the argument as the call evaluated it, even when the call writes the same storage while it runs, through another `&` argument (`f(a, &a)`), through the receiver of a method that writes it (`box.keep(box.items)`), or through an argument evaluated after it. The compiler passes such an argument without copying when the call cannot write its storage, and a copy of it otherwise. A `const` parameter has storage of its own, so the address of any part of it never equals the address of the caller's storage: in `void f(const int[] b, int[] &a)`, `&b[0] == &a[0]` is false for `f(x, &x)`.

Numeric values implicitly convert to the resolved numeric destination in initialized declarations, assignments, parameter defaults, returns, and value arguments to functions, methods and constructors. The checks match explicit `T(value)`: destination range, ordinary real/complex rounding, and integral-only real-to-integer conversion. Supported whole-array/tensor conversions preserve structure and shape. A failed implicit conversion stops before the destination write, return or callee body; source variable types stay unchanged. Writable `T &` arguments require the exact storage type and never convert. `bool` is separate from numeric types and never participates in these implicit conversions.

A resolved `bool` value parameter requires a boolean argument, positional or named.
Use `n != 0` to test an integer before passing, initializing, assigning or returning
a boolean value. A `bool &` parameter requires exact `bool` storage.

For example, if `int8 take(int8 value)` returns `value`, then `int x := 100` may be passed as `take(x)` with the same conversion as `take(int8(x))`. `real64 y := 3.0` may be passed to `take(y)`, but `3.5` fails integral-only conversion. A `real32` parameter may receive an integer argument with normal rounding; a complex parameter may receive a real or integer argument with imaginary part zero. A complex argument never implicitly converts to a real/integer parameter; select `.real`, `.imag`, or an explicit package operation. Resolving generic type arguments remains a separate step from conversion to the resolved formal type.

Required parameters precede default parameters. A numeric default supplies its ordinary default type when no annotation is given: `limit := 100` gives an `int` parameter; `int32 limit := 100` provides a narrower context.
A default is evaluated for each call that omits that argument; it is not evaluated when an explicit argument is supplied. Defaults are evaluated in parameter order and cannot reference parameters or caller-local bindings. They may call declared functions. Mutable array defaults are independent values on different calls. Reference parameters cannot have defaults.

A `void` function can fall through or execute bare `return`. A non-void result must be produced on every normally continuing path. `return void` can explicitly select a void alternative of a union. `return` itself carries control-flow information.

When a function or method returns a class value, the checker records the field paths that are definitely initialized on every normal return. Multiple returns contribute the intersection of those paths, including nested paths such as `data.ys`. Callers receive that summary, so a factory whose returned `Point` has only `x` assigned makes `x` readable while `y` remains statically uninitialized. A constructor is summarized the same way: the fields it definitely initializes on every completion are the fields a caller may read. These summaries are solved interprocedurally to a fixed point, so declaration order does not change the result.

### Callable classes

A class may define a method named `operator()` to make its instances callable.
The operator name is followed by its parameter list: `Result operator()()` has
no arguments, and `Result operator()(T value)` has one. `value(args)` calls a
read-only operator; `.value(args)` calls a method declared
`Result .operator()(args)`. Both follow ordinary method and mutation authority
rules. Bare `operator` remains an ordinary identifier, not a callable alias.

```quidra
class Counter(int value)
    int .operator()()
        value += 1
        return value

counter := Counter(0)
print(.counter())
```

### Class index operators

A class may define `Result operator[](parameters)` for `value[arguments]`.
At least one parameter is required. String keys, multiple indices, default
parameters and generic classes use the ordinary rules for positional value
arguments, including checked numeric conversions. Named arguments and writable
reference arguments are not part of the index-list syntax. The receiver and arguments are evaluated once,
left to right. A mutating index method is declared
`Result .operator[](parameters)` and called as `.value[arguments]`.

```quidra
class Values(int[] data)
    int operator()()
        return data.sum()
    int operator[](int index)
        return data[index]

values := Values([10, 20, 30])
println(values())   // 60
println(values[1])  // 20
```

Each operator has one ordinary definition per class, so `operator()` and
`operator[]` can coexist without creating an overload set. Access control,
value copying, typed errors and mutation effects are those of ordinary methods.
Index results are values, never assignment targets or writable references.
The methods can also be called explicitly as `value.operator()(args)` and
`value.operator[](keys)`, using ordinary method-call argument syntax.
Slice syntax is not passed to a class index operator; empty `[]` remains the
existing collection-loop suffix. Built-in indexing is unchanged.

### Standard iterators

`Iterator<T>` is a standard generic class for a borrowed runtime-sized `T[]`.
`Iterator(&values)` infers `T`; type positions require explicit arguments,
including the result of `Iterator<int> iterator()`. The iterator has
`bool .operator()(T &value)`: `.it(&value)` advances its independent cursor,
initializes or replaces the output on success, and returns false without
changing the output at exhaustion. Further exhausted calls remain false.
A successful boolean condition may declare its output inline, for example
`while .it(Point &point)`; no default element value is required.

```quidra
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)

numbers := Numbers([10, 20, 30])
for value := numbers[]
    println(value)
```

A single user collection value loop evaluates its source and calls `iterator()`
once, then calls the returned standard iterator until false. Elements retain
exact static types; declarations, reuse with `=`, tuple patterns, discard,
`break`, `continue` and nested loops follow their ordinary value-binding rules.
Each factory invocation creates a fresh cursor. Index operators remain separate:
`operator[](key)` serves `object[key]`, not iteration. This first iterator
contract does not provide reference elements, indexed headers, zip or parallel
iteration. Existing array, bin, range and structural loops keep their contracts.

The source is borrowed, while the `T &value` argument is an output destination
receiving an independent element value. Element writes to the borrowed source
remain visible. Replacing or resizing its storage invalidates iteration and is
checked on advance (exit status 101). Iterator results may not escape a local
source array, including through a local iterator binding; use a reference
parameter or receiver-owned storage for a returning factory.

## Length shorthand

`#value` is the prefix shorthand for the ordinary zero-argument call
`value.len()`. It is available for every type on which that call resolves,
including user-defined classes, generic classes and visible extension methods.
Defining an applicable `len` method automatically enables `#`; no operator
declaration or type registration is needed. `.len()` remains available and
`len` remains an ordinary identifier.

The shorthand inherits the method's return type, accessibility, omitted-argument
defaults, initialization requirements, failure behavior and mutation-marker
rules. In particular, it cannot silently invoke a mutating `len` or omit a
required argument. An explicit `.#value` marks the same call chain as
`.value.len()` when a mutation marker is required. User-defined methods keep their declared result types;
built-in array, string and `bin` lengths return `nat`. They count outer array
elements, Unicode code points and bits respectively. An array's length does
not read its elements, so it can be queried before those elements are initialized.

```quidra
class Group(string[] members)
    nat len()
        return #members

group := Group(["Aki", "Yui", "Haru"])
println(#group)      // 3, exactly as group.len()
matrix := [[1, 2, 3], [4, 5, 6]]
println(#matrix)     // 2
println(#matrix[0])  // 3
```

The receiver is evaluated once. Postfix calls, indexing and member access bind
inside the receiver, while exponentiation and arithmetic bind outside it:
`#matrix[0]` means `matrix[0].len()` and `#values ^ 2` means
`values.len() ^ 2`. To select or call a member on the length result, write
`(#value).member`. A following range uses the length as its bound, as in
`0:<#values` or `#values>:0`; parenthesize a range or another compound receiver
to count its result, as in `#(0:3)` or `#(left + right)`. Nested `##value`
means `value.len().len()` and requires both calls to resolve normally.

## Evaluation order, execution order and numeric determinism

```quidra
int note(string label, int value)
    print(label)
    return value

values := [0, 0, 0]
values[note("index ", 1)] = note("value", 5)
println()
println(values[1])
```

Source evaluation order is left to right everywhere. A call evaluates its arguments in source order, named arguments included, then the defaults of omitted parameters in parameter order; a method call evaluates its receiver before its arguments. Binary operands, the parts of a concatenation, the elements of an array literal, the segments of an interpolation and the operands of an index list are evaluated left to right. A class generation call evaluates its arguments and omitted header defaults, initializes header members, then runs direct statements/member initializers in source order. The program above prints `index value` and then `5`.

An assignment changes storage only after both of its sides are evaluated. First the target is evaluated left to right and its place is fixed: the root binding, followed by the values of the index operands on its path, each evaluated once. Then the right-hand side is evaluated completely. Then the write happens. `a[i] = bump(&i)` therefore writes at the index `i` had before `bump` ran, and `a[b[j]] = f()` with `b[j]` out of bounds fails before `f` runs. The fixed place is logical: the write goes to the current storage of the root binding at the fixed indices and is validated then, so a right-hand side that replaces, grows or shrinks the array or object the target belongs to never causes a write into released storage; an index that is no longer in bounds fails with `INDEX_BOUNDS` at the target. A compound assignment `a[i] op= e` reads the current element after fixing the place and before evaluating `e`, because the element is the operator's left operand; `a[k] += e` with `k` out of bounds therefore fails before `e` runs, while `a[k] = e` evaluates `e` and fails at the write.

Execution order is free while the observable behavior holds: the implementation may reorder, fuse, overlap or defer operations when the outputs and their order, the identity and location of the first error, and the final state stay the same. Two operations that may fail are reordered only when at most one of them can fail or the reported error stays the first in source order. A device operation may report its failure at a later synchronization point (see "Tensor device placement"), but the error it reports is still the first failing operation in source order.

Numeric determinism is a third, separate concept. Evaluation order fixes which operations a program performs and in what order their effects are observed; a determinism policy, such as a package's choice between deterministic and faster nondeterministic kernels, fixes how a numeric operation computes its result. Changing such a policy is an explicit state change made by the program, never a side effect of evaluation order.

## Extension methods

A top-level function may mark its first parameter with `extend` to permit
both the ordinary function call and a static method call. The definition and
the implementation remain one function; there is no dynamic dispatch, implicit
receiver mutation, trait object or access to private class members.

```quidra
int squared(extend int value)
    return value * value

result := 5.squared()  // squared(5)
```

An imported, public extension function is available as a method only while
that library is imported. An ordinary `import` does not re-export extensions;
`public import` explicitly re-exports an imported namespace. Where multiple
visible extensions match the same call or a built-in/class method conflicts,
the compiler reports ambiguity instead of silently preferring a candidate.

A writable extension receiver must be marked at the method call site. Its
ordinary function form continues to use the explicit `&` argument.

```quidra
void scale(extend int[] &values, int factor)
    for &value := &values[]
        value *= factor

values := [1, 2, 3]
.values.scale(2)    // scale(&values, 2)
```

`extend` is permitted only on the first parameter, never on class methods
or an external C declaration. Extension receivers do not have defaults,
are evaluated once and use normal value, const, reference, evaluation-order
and error propagation rules.

## Text and dynamic array operations

`string` is immutable UTF-8 text. The expression `text * count` repeats the entire string and returns a `string`. The count can be any integer type. For example, `"ab" * 3` gives `"ababab"`. A negative count is an error. Zero repetitions and repetitions of empty text give `""`. Size overflow or allocation failure stops the program with a runtime error. The function `string.repeat(value, n)` repeats one Unicode code point with a `nat` count.

`text.len()` counts Unicode code points rather than UTF-8 bytes and returns a `nat`. `text[index]` returns a one-code-point `string`, using the same code-point indexing model and deterministic bounds failure as other indexed values. `text.find(needle)` returns the code-point index as `nat`, or `none`; a slice `text[start:<end]` takes the code points from `start` up to `end`, with every slice form of arrays over code points (see "Slices"): `text[text.len()>:0]` reverses the text, and a start past the excluded end gives `""`. `trim()` removes Unicode whitespace at both ends. `lower_ascii()` changes ASCII A-Z to a-z and `upper_ascii()` changes ASCII a-z to A-Z while retaining all other UTF-8 characters unchanged; these are non-mutating string operations, not Unicode case folding. `split(separator)` preserves empty fields and requires a nonempty separator. `contains`, `starts_with`, and `ends_with` perform exact text matching. `text.utf8()` explicitly returns the UTF-8 encoding as `bin`; `string.from_utf8(data) -> string | error` performs the inverse only for byte-aligned `bin` containing valid UTF-8 without NUL, and reports representation failure as `error`; `text.codepoints()` explicitly returns Unicode scalar values as `int[]`. These conversions keep byte-oriented and text-oriented operations distinct rather than introducing a `char` type.

Runtime-sized `T[]` arrays support `append(value) -> T[]` and `concat(other) -> T[]`. Fixed-width numeric, `bool`, and `string` arrays of either fixed or runtime size support `sorted() -> T[]`; sorting is non-mutating and returns an independent runtime-sized value. Real sorting places values in numeric order, preserves equal-value order and orders `-0.0` before `0.0` (no real value is NaN). `sorted(descending = true)` sorts in the reverse of that order (`0.0` before `-0.0`, strings in descending code-point order), and is stable too: equal elements keep their relative order, so the result is not a reversed ascending sort. Without `by`, `descending` is a named `bool` and may be a run-time value; `sorted(descending = false)` is `sorted()`. A `sorted(by = keys)` call orders its receiver by a key array of the same length, including receiver values without their own ordering. `by` may be a tuple of ordered numeric, bool or string key arrays (heterogeneous key types are supported), with the leftmost key taking priority. `descending` may be one bool applied to every key, or a tuple of bool flags matching the number of keys. For example, `names.sorted(by = (scores, names), descending = (true, false))` ranks by highest score, breaking ties by name ascending. Sorting is stable: when all keys compare equal, the original order is preserved. Fixed-length mismatches are rejected by the compiler; other length mismatches fail at run time before sorting. These methods never change the source or key arrays. String sorting uses deterministic Unicode-code-point-compatible UTF-8 lexical order. Any string array additionally supports `join(separator) -> string`, which constructs the result in one operation. Repeated `text = text + piece` is linear overall rather than quadratic: `string` is an immutable value, so when the assignment target is the sole owner of its storage the implementation may append into that storage with geometric growth, and neither the reuse nor the spare capacity is observable. `append`, `concat`, and `sorted` return new array values. They do not resize the receiver's backing storage in place, so an existing safe address such as `&values[i]` is never invalidated by the operation itself. A later implementation may use capacity, moves, or copy-on-write internally only when that optimization is unobservable.

Loops support `break` and `continue`. In writable iteration, changes to the current element are committed before either advancing with `continue` or leaving with `break`.

Conditional chains use `elif`:

```quidra
score := 85
if score >= 90
    println("A")
elif score >= 80
    println("B")
else
    println("C")
```

## Modules

Imports are top-level namespace bindings. Imported files contribute declarations to the compilation and may export immutable top-level `const` bindings whose initializers are side-effect-free scalar constant expressions. Qualified uses clone those expressions at the use site, so ordinary constant initializers execute no module initialization code. An imported range-constrained constant additionally retains its declared range check, evaluated once at program startup before importer code; repeated qualified uses do not repeat the check. A contextual constant (`const literal real pi := real.atom("math", 1)`, see numeric-and-bin.md) is a literal of the real category at every use, local or imported: it takes the real type its context expects. Every other typed constant keeps its declared type at every use: with `const real32 eps := 1.0e-5` in module `mod`, `mod.eps` is a `real32` wherever it is used. `real64 x := mod.eps` applies checked destination conversion to that stored `real32` value; it does not rematerialize the original decimal spelling. `real64(mod.eps)` requests the same numeric conversion explicitly. An inferred constant, `const name := initializer`, takes the initializer's type. Other executable top-level statements remain forbidden outside `if main` guards.

```quidra
import geometry = "./geometry.qui"
import root = "@/shared/root.qui"
import plot = plotting
```

A quoted local path that does not begin with `@/` resolves from the importing file's directory. A quoted path beginning with `@/` resolves from the command working directory. The special meaning applies only to that leading `@/` prefix: `"./@/util.qui"` addresses a real `@` directory beside the importer, and `"@/@/util.qui"` addresses a real `@` directory under the command root. Local paths must end in `.qui`; project-root imports cannot escape the command working directory.

Standard namespaces are always visible and cannot be imported or aliased. For example, use `file.read(...)` or `tensor.zeros<T>(...)` directly. The standard namespaces are reserved so later declarations cannot change what those qualified references mean. Mathematical semantics are not a Core standard namespace: the first-party `math` package is imported explicitly with `import math` (or an ordinary package alias).

An unquoted non-standard target denotes an installed package, for example `import plotting` or `import plot = plotting`. Package resolution never falls back to a same-named source file in the working directory. Local source modules use quoted paths only. Imported declarations are accessed through their alias, for example `geometry.Point`. A module's own imports are private implementation namespaces rather than automatic re-exports. A module re-exports one deliberately with `public import mode = "./mode.qui"`: an importer of that module then reaches the target's declarations as a nested namespace, `nn.mode.fast()`, and the namespace itself is not a value.

Import aliases cannot collide with another import alias or a class/function declared in the same file. Import cycles are compile-time errors. Nested imports are supported and retain independent namespaces.

A top-level function, `extern` declaration, class, enum or `const` binding is public by default. `private` before it makes it module-private: it is usable only inside the module (source file) that declares it, and an importer that names it, `geometry.helper()`, gets the compile error `PRIVATE_DECLARATION` ("'helper' is private to module 'geometry'"). `public import` never re-exports a private declaration. A generic function or class instantiated for an importer still resolves names in its declaring module, so its body may use that module's private helpers. Since an importer cannot name a private type, no public declaration may expose one: a private class or enum cannot appear in the parameters or result of a public function, a public field of a public class, a payload of a public enum, or the type of a public constant. `private` applies only to declarations: `private import` (imports are private unless `public import`), `private cli` and `private` before a statement are errors, and so is `private export "C"`, because an exported C function is public. In the REPL, `private` is accepted and has no effect.

```quidra
private int clamp_index(int value, int limit)
    if value < 0
        return 0
    return if value < limit then value else limit - 1

int pick(int[] values, int index)
    return values[clamp_index(index, int(values.len()))]
```

A class may place itself directly under an exported namespace by qualifying its
top-level declaration name, for example `class model.ResNet50()`. This does not
create a nested lexical class or a separate module: the declaration remains in
the same source file and can therefore use the file's ordinary declarations,
while importers access it through the declared namespace path, such as
`dnn.model.ResNet50`. Qualified class names are useful for grouping families
of public types without duplicating implementation modules or introducing
import cycles.

## Generics

Classes, functions, and methods may declare explicit type parameters:

```quidra
class Box<T>()
    T value

T first<T>(T[] values)
    return values[0]

class Convert()
    T identity<T>(T value)
        return value
```

Class type positions require explicit type arguments. Class generation calls, function calls and method calls may omit type arguments when every generic parameter is uniquely determined from typed call arguments:

```quidra
box := Box<int>()
box.value = 7
values := [1, 2, 3]
first_value := first(values)
convert := Convert()
sample := 5
same := convert.identity(sample)
```

Explicit function or method type arguments remain valid when inference would be ambiguous or when the caller wants to state them explicitly.

Generic type arguments may themselves be arrays, unions, or instantiated generic classes. Generic classes, functions, and methods are all monomorphized without introducing a runtime trait, vtable, or subtype system.

Quidra implements generics by monomorphization. Each used concrete type-argument tuple produces one internal concrete class, function, or method before ordinary static checking and IR lowering. Repeated uses of the same tuple reuse the same instance. Generic declarations that are never instantiated do not emit native code.

Generic class, function and method calls infer type arguments only when every generic parameter is uniquely determined by typed arguments. `Box(seven)` for `class Box<T>(T value)` and an `int seven` constructs `Box<int>`. Without another concrete context, numeric literal defaults participate in inference: `Box(7)` constructs `Box<int>`. Write `Box<int32>(7)` to supply a narrower context. Type positions always supply the class type arguments. Use an ordinary generic factory when initialization needs extra type parameters while preserving the class type identity. Supplying arguments to a non-generic target is an error.

A generic parameter may carry one minimal built-in compile-time constraint:

```quidra
T maximum<T: ordered>(T a, T b)
    if a > b
        return a
    return b

tensor<T> normalize<T: floating>(tensor<T> value)
    return value
```

The built-in constraints are `numeric`, `integer`, `floating`, `ordered`, and `equatable`. They add no runtime representation, vtable, dynamic dispatch, or implicit conversion. `integer` accepts the fixed-width integers, `int` and `nat`; `floating` accepts `real16`, `real16b`, `real32` and `real64`; `numeric` accepts integer, floating, exact `real`, and every complex type; `ordered` accepts integer, floating, and exact `real`, excluding complex values; and `equatable` accepts values with ordinary structural/scalar equality. Constraint failure is diagnosed before a concrete generic body is emitted. User-defined traits/interfaces are intentionally not part of this constraint system.

A repeated function or method name is legal only when the declarations form one generic specialization family within the same scope. Family members keep the same call shape: the same argument count, parameter names, reference/const forms, extension-receiver markers, and default-argument positions. Generic members also keep the same generic type pattern and relationships among their type parameters, except that array, tensor, and constrained scalar parameter patterns may coexist when their domains are statically disjoint. A scalar type parameter constrained by `numeric`, `ordered`, `integer`, or `floating` cannot match an array or tensor; `equatable` and unconstrained parameters provide no such exclusion. The number of generic parameters is unrestricted; for example, a family may use `<T1, T2>`. A concrete exact-type member may specialize the family, and generic members may narrow the same type variables with constraints.

Every parameter in a prototype or specialization family requires an explicit type, including
defaulted parameters whose literal would otherwise determine it. Write
`int radius := 1` in those headers; omitted default-parameter types apply to
function and method definitions outside a repeated-name family.

```quidra
T1 combine<T1: numeric, T2: integer>(T1 left, T2 right)
    return left

T1 combine<T1: floating, T2: integer>(T1 left, T2 right)
    return left
```

A concrete member normally instantiates that generic type pattern. A narrow exception permits a different concrete signature only when known primitive argument types and supported built-in constraints prove that every generic member excludes that signature. For example, an `int` argument cannot match a `floating` type parameter. A mismatch involving a structural or nominal type, shape, reference/const form, value, or unknown type is not a primitive-disjoint proof. The exception does not change the family's argument count, parameter names, default positions, mutation marker, visibility, or ordinary shape checks. It concerns which declarations form a valid specialization family; value arguments still follow the ordinary checked numeric call-conversion rule, and return-type-only overloading remains forbidden. Within each container domain, generic members still share one type-variable pattern, and a concrete signature that is not proved excluded from every generic member remains an error.

Candidate selection first considers exact concrete matches and matches obtained by generic inference. An exact concrete match is preferred to generic members. Only when neither an exact concrete nor an inferred generic candidate matches are concrete candidates requiring numeric value-argument conversion considered, with the ordinary ambiguity checks. Implicit convertibility does not change generic constraints or the declaration-family disjointness proof. Between generic matches, a uniquely narrower supported constraint wins, such as `floating` over `numeric`. Domains such as `ordered` and `equatable` that overlap without a unique specificity relation are rejected as ambiguous; the compiler never chooses an arbitrary member. Changing the type-variable relationship between generic members, such as pairing `(T, T)` in one declaration with `(T, U)` in another, is an ordinary overload rather than specialization and is rejected.

## Named enums and variants

Named enums represent semantic alternatives without overloading ordinary structural unions:

```quidra
enum Token
    Number(real64)
    Name(string)
    Plus
    End

token := Token.Number(3.0)
end := Token.End
```

The enum name is the static type. Variants are always qualified with the enum name. A variant carries either one explicitly typed payload or no payload; payload-free variants are values and are written without parentheses. Different variants remain distinct even when they carry the same payload type.

Enum matching is exhaustive:

```quidra
enum Token
    Number(real64)
    Name(string)
    Plus
    End

token := Token.End

match token
    Token.Number(value)
        println(value)
    Token.Name(name)
        println(name)
    Token.Plus
        void
    Token.End
        void
```

A payload binder exists only inside its branch. A payload may be ignored by matching the qualified variant without a binder. Enum values are nominal: they do not implicitly convert to or from an ordinary union with the same payload types. Internally, enum storage reuses Quidra's checked tagged-union representation; there is no dynamic dispatch or hidden subtype relation.

## Classes

A class definition has a type name, an optional instance name, a mandatory argument list, and an optional `| error` generation contract:

```quidra
class Point point(real64 x, real64 y, local real64 scale := 2.0)
    x *= scale
    y *= scale
    length_squared := x * x + y * y

    real64 sum()
        return x + y

    Point copy()
        return point

complete := Point(3.0, 4.0)
larger := Point(x = 3.0, y = 4.0, scale = 10.0)
```

Ordinary header parameters declare members initialized from the supplied arguments. A `local` parameter is a temporary available only during initialization; `scale` is not a member. Header members are initialized first, then direct statements and member initialization expressions execute once, sequentially in written order. Methods define operations and do not execute during generation. Empty classes still use `class Empty()`. There is no special `construct` declaration: that name may name an ordinary function or method.

`T(...)` runs this ordered initialization. Arguments follow ordinary positional, named and default rules. The header has one parameter list; express alternate call forms with defaults or free factory functions. Generic parameters belong to the class header or an ordinary function/method, so a generic factory may construct a non-generic class without changing its type identity.

A typed declaration without an initializer, such as `Point pending`, leaves the binding uninitialized and creates no usable instance. It permits subsequent whole-value assignment, such as `pending = Point(3.0, 4.0)`, but no member access, method call or instance borrowing before initialization. `point := Point(3.0, 4.0)` is an initialized declaration and follows ordinary constructor call rules for argument evaluation, mutation markers and errors. Failed generation cannot initialize the destination with an unfinished instance. Standard `map` and `distinct` bare bindings retain their initialized empty-storage contract.

```quidra
Point pending
pending = Point(3.0, 4.0, scale = 1.0)
```

A normal declaration directly inside a class creates a member. A field initializer uses `:=`. Omit the field type when the expression determines it, as in `label := "hello"`. Write the type when the initializer needs it or the field has a stronger type contract. A field without an
initializer needs an explicit type. A default header parameter may likewise infer
its type, as in `class Greeting(text := "hello")`. `local` keeps an initialization temporary; declarations inside control blocks are local with or without the qualifier. A name is visible only from its declaration through its block and nested control blocks. It cannot implicitly cross a function or class definition boundary. A method therefore cannot capture an initialization local. In ordinary function bodies and parameters `local` is optional and has the ordinary declaration or parameter semantics.

Members use their bare names inside the class, including assignments, references, interpolation, loop sources and method shape expressions. Methods may be called bare or through an explicit object. An optional instance name denotes the fixed current instance and is neither an extra member nor a copy. Value use follows ordinary copy/borrow rules; `&point` explicitly passes storage access. The fixed instance cannot be rebound or replaced. `this` has no implicit meaning: it names the instance only when the header explicitly declares it, for example `class Point this(real64 x, real64 y)`.

The whole instance cannot escape while initialization is in progress, including through a method that publishes it. Member access remains available during initialization. Header locals, members, instance names and method/block declarations may not shadow one another. An ordinary header member and its generated member are one declaration.

```quidra
class Document document(local string path) | error
    stream := try file.read(path)

Document | error result := Document("notes.txt")
```

Generation with `| error` returns `Document | error`, while the class type and instance name remain `Document`. A bare `return` completes initialization; `return error(...)` is allowed with that contract. A success-only destination fails fast, an explicit `Document | error` binding retains the failure alternative, and `try` propagates it. Successful union construction requires complete member initialization. Failure releases initialized members and local values; it does not roll back external effects. Classes add no special user destructor: saving or closing a resource remains an explicit operation.

A member initialization expression such as `int count := 0` executes at its written position in the generation body. Earlier members are available there. Each generation gets independent mutable values. A field without an initializer remains uninitialized until assigned; it does not become zero, false, empty or none. Const members are initialized exactly once at the permitted initialization point. Parameter defaults retain ordinary default rules and cannot depend on other parameters or caller locals.

Members are public by default. Prefix a member with `private` for class-local access:

```quidra
class Counter()
    private value := 0

    private void .increment_raw()
        value += 1

    void .increment()
        .increment_raw()

counter := Counter()
.counter.increment()
```

Private members are accessible only from initialization or methods of their declaring class, including through another instance of that class. Method summaries retain receiver read/write and definite initialization requirements. A method that reads `value` before writing requires it to be initialized at its call; a method that establishes it can initialize partial storage. Nested field replacement invalidates facts not guaranteed by the replacement, and a later definite write can restore them.

Initialization is tracked per field and through nested class fields. A field of partial local storage, or of the receiver during initialization, that some paths assign and others do not is checked at run time when read: the class keeps a hidden initialization bit and the read stops with `UNINITIALIZED` on a path that left it unset. The bit travels with value copies and does not participate in equality or display. Untracked nested fields or writes known only through a callee/reference still require definite proof. Equality, union conversion, array insertion and by-value arguments retain their complete-value requirements.

Ordinary class bindings have deep value semantics. Copying a class value produces independent nested class, array, and union storage and preserves field-initialization state:

```quidra
class CopyPoint()
    real64 x
    real64 y

a := CopyPoint()
a.x = 1.0
b := a
b.y = 2.0
```

Here both copies begin with initialized `x` and uninitialized `y`; initializing `b.y` does not initialize `a.y`. Strings remain immutable values.

Classes do not have a parent-class form. Reuse is explicit composition: place the reusable value in a field and access it through that field. A class contains only the fields and methods written in its own declaration, so member lookup stays local and statically obvious. When a value may be one of several concrete class types, use an explicit union and exhaustive `match`.

Reserved identifiers cannot be reused by user declarations. `this` and `construct` are ordinary identifiers. Members, the optional instance name, header locals and method/block declarations cannot conflict with one another; qualification does not bypass reserved-name or visibility rules. Standard-library implementation declarations are language-owned and are the only internal reserved-name exception.

Class `==` and `!=` compare values rather than allocation identity. The operands must have the same static class type, and all recursively compared fields must be definitely initialized. Nested fields participate in the comparison. Comparing partially initialized class values is a compile-time error because uninitialized state is not a value.

Array `==` and `!=` compare lengths and then elements recursively. Equality is available only when the element type has defined value equality. A class value must have every field definitely initialized before it can be stored as an array element (including literals, `array(..., fill = ...)`, `.append(...)`, and indexed assignment), so recursive array equality never observes conceptual uninitialized class fields. Strings compare contents. Quidra does not expose object or storage identity through an `is` operator.

## Addresses, references, and const authority

Quidra uses `&` consistently for safe storage addresses. For addressable storage `x`, `x` denotes its value and `&x` denotes its storage address. Address expressions are observation-only values: `print(&x)` exposes the current raw machine address using the platform pointer representation, while `&x == &y` and `&x != &y` test whether two address expressions designate the same current storage. An address expression cannot be stored, converted to an integer, used in arithmetic or ordering, or dereferenced with `*`. Its printed representation is diagnostic and may change across executions, builds, platforms, or optimization choices. Using a reference name still accesses the referenced storage directly.

For example:

```quidra
x := 1
&alias := &x
other := 1

println(&x)             // current raw address of x storage
println(&alias)         // the same address
println(&x == &alias)   // true
println(&x == &other)   // false
```

For managed values the address still means the address of the language-level storage being addressed. In particular, `&text` for a `string text` observes the binding/storage slot, not the separate UTF-8 payload pointer used internally or at an FFI borrow boundary.

A value `T` owns its value storage. A reference declaration `T &name` gives read/write access to existing storage; it may be a local, a parameter or a class field. `&` is not part of a type: array elements, tuple elements, generic type arguments, union alternatives, `func` parameter and result types, and function results cannot themselves be references. They may contain class values that have reference fields. `const T` is an immutable value: `const T x := y` and a `const T` parameter hold the value `y` had, and a later write to `y` does not change them. `const value := expression` infers `T`. There is no read-only reference: `const T &` is the compile-time error `CONST_REFERENCE`, and a `const T` value takes its place. The compiler passes a `const` value without copying it wherever nothing can change its source while it is live, so the value form costs nothing over a view.

```quidra
a := 1
&b := &a

b = 5
```

After the assignment, `a` and `b` both read as `5`. A reference binding may point at a binding, field, or array element. A `const` binding is a value of its own:

```quidra
value := 5
const view := value

value = 7
println(view) // 5
```

`view` keeps the value `value` had when it was bound. `view = 8`, `&view = &other` and `int &writer := &view` are compile-time errors: a const value is never written, and no writable path to it can be formed. It also has its own identity, as any copy does: `&view == &value` is false.

A reference binding may point at a binding, field, or array element:

```quidra
values := [1, 2, 3]
&first := &values[0]

point := Point(1.0, 0.0)
&x := &point.x
```

Taking a scalar address does not read the stored value, so uninitialized scalar storage may be referenced by a writable reference that can initialize it. Class instance borrowing requires a successfully initialized instance:

```quidra
int value
&alias := &value
alias = 7
println(value)
```

Reading `alias` before the write would be an uninitialized-read error. A `const T` value reads its source when it is formed, so that source must be initialized. Temporary values such as `&5` or `&Point(1.0, 2.0)` are not valid address targets.

A local reference may start unbound (`int &alias`). Bind it before use with `&alias = &value`. The compiler reports `UNBOUND_REFERENCE` when it knows the reference has never been bound; where binding depends on a branch, it checks at the access and reports the same code at run time if still unbound. Each loop iteration begins with fresh binding state. Reading or writing through the reference, taking its target address, and comparing that address all require a binding. `&alias := &storage` declares a reference with the type inferred from its target. A parameter is already bound by its caller and cannot be rebound.

A reference field is a transparent alias to its target:

```quidra
class Holder this(local int &source)
    private int &total


    &total = &source

    int read()
        return this.total

    void .replace(int &source)
        &total = &source

    void .add(int amount)
        this.total += amount

value := 4
first := Holder(&value)
second := first
.second.add(3)
println(first.read()) // 7: both holders refer to value
```

Reference fields have no `const` qualifier. An uninitialized reference field starts unbound; its direct initialization may explicitly bind it with `&`. Only the declaring class binds or rebinds them, with the direct form `&total = &source` in initialization or a method. Access elsewhere still denotes the target: `holder.total` reads or writes it and `&holder.total` observes its address, subject to field privacy. `&holder.total = &source`, and a longer place such as `&self.inner.total = &source`, cannot rebind it; call a method of the declaring class. Binding in a constructor initializes the new holder and needs no mutation marker. Rebinding in a method writes the existing holder, so that method is declared and called with `.`. Writes through a reference field change shared state, and require `.` too unless an explicit `&` argument exposes that target. A `const` holder grants no write or rebinding authority through its reference fields. `cli` fields hold command-line values and cannot be references.

A stored target must have stable storage: a whole variable, a by-value parameter's storage, or a field path consisting only of class fields beneath such a root. A reference parameter may be stored only if all its callers supply such a target; that requirement propagates through calls. Array elements, tuple elements, union payloads, map or distinct entries, temporaries and unresolved targets are `REFERENCE_TARGET`. Ordinary local references and non-storing reference parameters keep their existing substorage rules. A stored target must be bound and initialized when it is stored, and it must remain initialized when subsequently replaced: a replacement that could leave the target partly uninitialized is rejected.

Copying a holder copies its reference fields and keeps the same targets alive. Ordinary value fields remain independent. Copying or growing a container of holders does not retarget those references. A stored reference to a whole variable sees later replacements of that variable; a reference to a field keeps the captured field storage if a containing value is replaced. It then refers to detached storage, rather than to the new field. Refer to the whole variable, or update its fields in place, when the reference must follow the current value. A reference may escape its original call; the implementation preserves the necessary storage until the last reference releases it.

A value cannot hold a reference whose target lies inside that same value, at any depth. `&alias = &value`, `model.optimizer = Optimizer(&model)`, and `holder = Holder(&holder)` are `REFERENCE_TARGET`. Keep a model and its bound optimizer in separate variables. The compiler rejects proven interior self-reference and checks unresolved alias cases at the binding or store. Separate variables may refer to each other, including cycles; those pins keep their targets alive and cycles may retain storage. A copied holder continues to refer to the original target, so this rule prevents a copy from silently updating its original model.

Class equality compares the values reached through reference fields, never their target addresses. Equality and REPL display require every visited reference field to be bound and its target initialized. Statically known unbound fields are compile errors; fields encountered in containers or other untracked storage are checked at run time. Copying a holder preserves any unbound fields.

A reference can be rebound explicitly:

```quidra
a := 1
c := 2
&b := &a

b = 5
&b = &c
b = 8
```

`b = c` assigns a value through `b`; `&b = &c` changes the address held by `b`. Consequently the example leaves `a == 5` and `c == 8`. Forms such as `b = &c` and `&b = c` are invalid. Taking `&` of an existing reference yields its current target address rather than a pointer-to-reference layer, so `print(&b)` observes the same raw address as `print(&c)` after the rebind and `&b == &c` is true.

Reference target identity is flow-sensitive. After an `if` or exhaustive `match`, the checker keeps a concrete target only when every continuing path resolves the reference to the same storage. If different targets remain possible, the runtime reference is still valid, but storage-specific initialization and non-alias proofs are discarded; a write through that reference initializes the reference access path without claiming which concrete root was initialized. Because a loop may execute zero or many times, a reference rebound inside a loop is treated conservatively after the loop. An explicit rebind after the control-flow join restores a precise target.

Reference parameters use the same model and remain explicit at both declaration and call sites:

```quidra
void inspect(const int value)
    println(value)

void initialize(int &value)
    value = 7

data := 1
inspect(data)
initialize(&data)
println(data)
```

A `const` parameter takes a value, `inspect(data)`, and a reference parameter takes `&storage`, `initialize(&data)`; writing `&` for a const parameter is `WRITE_CAPABILITY` ("Const parameters take values; remove '&' from '&data'."). The checker infers read-before-write requirements for writable parameters. A write-only writable parameter can initialize previously uninitialized storage; a const parameter's argument is a read, so its storage must be initialized. Reference markers cannot be omitted or supplied to ordinary parameters. Reference contracts preserve exact types, including fixed array shapes and class identity.

Reference aliasing is allowed: the same storage may be passed to several reference parameters, `f(&data, &data)`. A const parameter keeps the value its argument had at the call even when the call changes that storage through another argument: with `void f(const int b, int &c)`, `f(a, &a)` gives `b` the value `a` had at the call while `c` changes `a`. The compiler passes a const argument without a copy where nothing in the call can change it, and copies it into a temporary where something may: another `&` argument of the call, a write the callee makes through other paths, or an argument evaluated later. Taking the address of a const parameter, or of a part of one, gives it storage of its own, so `&b` never equals the caller's `&a`.

Addresses are rooted in existing storage: a binding, a receiver field, or substorage beneath one. Quidra does not create references into temporary values such as `&array(1, fill = 0)[0]`; bind the value first when its storage must outlive the expression.

A reference to a field or array element captures that substorage at address-formation time. Replacing the containing value does not retarget an existing substorage reference:

```quidra
values := [1, 2, 3]
&first := &values[0]
values = [4, 5]

println(first)      // 1
println(values[0])  // 4
```

The implementation keeps the old backing storage alive as long as such a reference needs it. `print(&first)` may observe the current machine address of that pinned substorage, but the numeric-looking text is diagnostic only and is not guaranteed to remain the same across separate executions, builds, platforms, or unrelated storage relocations allowed before an address becomes observable.

A reference to a whole binding is different: `&alias := &point` addresses the binding's storage slot, so replacing `point` is visible through `alias`.

Quidra intentionally has no Python-style `is` identity operator. Ordinary `==` compares values according to the value type, while `&left == &right` explicitly compares storage identity. Reference fields share only the storage they explicitly designate.

## Mutation markers

Every change of existing state is visible where a call is written. A target passed as `&x` is visible already. Any other change of state that existed before the call needs the mutation marker `.`, both on the definition and at every call:

```quidra
class Counter()
    value := 0

    void .increment()
        value += 1

    int read()
        return value

counter := Counter()
.counter.increment()
println(counter.read())
```

The marker is written before a function's or method's name (`void .update()`), before an extern's name (`extern void .set(...)`). At a call it is one `.` before the whole postfix chain: `.update()`, `.counter.increment()`, `.nn.mode.fast()`, `.self.inner.bump()`. It covers the calls of that chain, the call itself and the calls in its receiver positions, not the calls nested in its arguments, which carry their own. A marked call is not a postfix: `(.gen.batch()).size()` parenthesizes it before a further member. The readings:

| Call | May change |
| --- | --- |
| `f()` | nothing that existed |
| `f(&x)` | `x` |
| `.f()` | hidden state |
| `.f(&x)` | `x` and hidden state |
| `obj.f()` | nothing that existed |
| `.obj.f()` | hidden state, not necessarily `obj` |
| `T(...)`, `T(&r)` | nothing beyond the new value, `r` |
| `.T(...)` | hidden state |

Existing state is the receiver's fields, the shared state reachable from the receiver or from a by-value argument (the targets of reference fields or a file handle's position), and runtime state behind `.` externs. It is not the function's own locals, the value a constructor creates, the targets passed with `&`, or external I/O: `print`, `scan`, `flush`, file system paths, `process`, `http`, `environment` and `time` never need the marker, so adding a debug print never changes a signature.

The compiler derives what each function changes from its body and the summaries of what it calls. A function whose body may change existing state that no `&` parameter exposes must be declared `.`; otherwise `MUTATION_MARKER` names the cause (`'increment' may change existing state (field 'value'); declare it as '.increment'.`). The rule is transitive: a function that calls a `.` function is itself `.` unless the changed state is exposed with `&` (`void bump(Counter &c)` calling `.c.increment()` is not `.`) or is its own fresh value (a local it created). A declaration may carry the marker without changing anything, as a promise its callers rely on. Calling a `.` callee without the marker is `MUTATION_MARKER` with the corrected spelling, and so is a marker on a chain that reaches no `.` callee. An extern's marker is its declaration, which the compiler trusts: `extern void .set(...)` changes runtime state, and an extern without it changes only its `&` parameters.

The standard library follows the rule. `map`'s `set` and `remove`, `distinct`'s `add` and `remove`, `random.Generator`'s `int`, `real64` and `bool`, the `file.read`/`file.write` call operators and file writer/appender `.line()` methods, `clear_grad` on tensors and `autograd.Target` are called with the marker (`.counts.set("a", 1)`, `.rng.int(1, 6)`, `.reader(&line)`, `.writer(text)`). Queries such as `get`, `has`, `keys`, `values` and `size` are not, nor are `track()`, `retrack()` and `untrack()`, which return a new value.

Class generation that changes state it does not create is called `.T(...)` or `.module.T(...)`. Its header has no definition marker: the compiler infers the requirement from the ordered initialization body. Passing the changed target with `&` needs no marker (`Widget(&registry)`). A bare class binding declaration `T x` executes no generation body and has no usable instance until whole-value initialization. A member initialization expression participates in that body and may call marked operations; ordinary parameter defaults cannot change existing state.

On a `const` value, a `.` call is an error only when it may change that value or the state reachable from it (`WRITE_CAPABILITY`: `'.c.update()' may change const 'c' (field 'total').`); a `.` method that changes only other hidden state may be called. A `.` function cannot become an `func` value (`FUNCTION_REFERENCE_SIGNATURE`). The members of a generic specialization family agree on the marker, as they agree on `&` parameters: a specialization may change the implementation, never whether the operation changes existing state. A prototype and its definition agree on it too. An `export "C"` function may be declared `.`; its C symbol is unaffected.

## Conditions and iteration

The conditions of `if`, `elif`, `while`, if-expressions, and array generation
filters require `bool`. The operands of `and`, `or`, and `not` also require
`bool`; their result is `bool`, and `and` and `or` short-circuit. Integers do
not implicitly become booleans: write `n != 0` for a nonzero test. Boolean values
also do not implicitly become numbers. An `if` statement can have an `else`
block. A `while` loop repeats while its condition is true.

Comparisons chain: `a OP1 b OP2 c` means `a OP1 b and b OP2 c`, so `0 <= i < values.len()` tests both bounds. Each operand is evaluated at most once, left to right, and the chain stops at the first link that is false, as `and` does: in `f() < g() < h()`, `h()` runs only when `f() < g()` holds, and `g()` runs once. A chain keeps one direction: its operators are all from `<` and `<=`, all from `>` and `>=`, or all `==` (`a == b == c` tests that all three are equal, also for `bool` operands). `a < b > c` mixes directions and is the compile error `CHAINED_COMPARISON`; so is any chain with `!=`, and a relational comparison written next to `==` or `!=` without parentheses: to compare the result of a comparison, write `(a < b) == c`. Every link follows the rules of a single comparison (numeric operands use their static common type; ordered numbers for `<`, `<=`, `>` and `>=`, equatable values for `==`), and the result is `bool`. Tensor comparisons give masks, which a chain cannot join; combine them explicitly.

```quidra
int64 x := 5
if 0 <= x < 10
    println("in range")
```

```quidra
for: 2 <= nat i < 10 step 2
    println(i)

values := [1, 2, 3]
for &value := &values[]
    value = value * 2
```

Range headers independently include or exclude both ends: `for: 0 <= nat i < stop`, `for: stop > nat i >= 0`, or plain `for` with an existing variable. `for:` declares a new loop-local range variable; its type can be omitted when the bounds determine it. Integer-only literal ranges default to `int`, `for: i < values.len()` infers `nat`, and an `int32` bound supplies `int32`. Typed bounds and a non-`int` typed step must agree on one ordered numeric type. An explicit type is written after the marker, as in `for: int32 i < 10`. Plain `for` reuses a mutable value binding; new names cannot shadow visible names. An explicit `step` follows the last bound; integer steps default to 1 or -1. The inclusive zero end may be omitted, as in `for: nat i < stop` or `for: stop > nat i`. Bounds and the step are captured once, left to right. The body cannot assign the range variable. An existing variable retains the last produced value, and is unchanged when the range is empty.

Real ranges require an explicit step. Fixed-width operands first materialize in the loop type. The count tests exact unrounded candidates `S + j * step`; each produced value is rounded once. The loop never corrects its count from rounded values. Exact `real` ranges use an exact step lattice, with certified comparisons at entry for non-rational bounds.

A header may zip several arrays by position:

```quidra
prices := [10, 20]
quantities := [2, 3]
total := 0
for price := prices[], quantity := quantities[]
    total += price * quantity
print(total) // 80
```

Each source is evaluated once, left to right, and its value snapshot is taken
immediately before evaluating the next source. All lengths are compared before
the body starts. Unequal lengths fail with `FOR_LENGTH` and run the body zero
times; equal empty arrays run zero times. One iteration selects the same
position from every snapshot, rather than forming a Cartesian product. Later
source evaluation or body writes cannot change an earlier snapshot. Bindings
are ordinary mutable value copies; inferred bindings, existing value variables and `_`
follow the single-source binding rules. Initial zip headers accept Array values
only: references, ranges, structural traversal, patterns and `bin` cannot be mixed into them. Any array may provide a position binding: `for: p := prices[i], int q := quantities[j]` independently declares both `i` and `j` as read-only loop-local `nat` values containing the same zip position. A source may omit its position with `[]`, including the first source. Without the colon every written index is reused. Each element's `:=` or `=` independently controls declaration or reuse. All element and index names in a header must be distinct. Nested single-source loops remain
available for Cartesian iteration.

`for value := values[]` iterates an array or `bin` by value: later replacement or element writes do not alter the entry value it visits. `for &value := &values[]` aliases the live element or bit place. Replacing or resizing its storage through an alias or a call fails with `FOR_ITERATION` before the loop advances. `for: value := values[i]` declares the element and index independently; `for: value = values[i]` reuses the element and declares the index. Plain `for value := values[i]` or `for value = values[i]` reuses an existing mutable `nat` index. Indices are read-only during iteration and start at zero relative to the source, including a slice. A reused index retains the last position even if the body never reads it; an empty source preserves its old value and initialization state. `for:` without a range variable or a written index has no declaration target and is invalid. Each `[]` iterates one level.

`for _ = values[]` evaluates the source and fixes its length at entry, then runs the body that many times without reading any elements or creating an element snapshot. `_` binds nothing and takes neither a type nor `&`. A range with `_` similarly computes only the count. Its type comes from typed bounds or a non-`int` typed step; a range made only of integer literals uses an internal exact integer count.

`range` and `in` are ordinary identifiers. The former `range(...)` builtin and `for ... in ...` syntax are unavailable. See [Range and collection headers](loop-headers.md) for the full forms.

### If expressions

`if C then A else B` is an expression. It evaluates the condition `C`, which must be `bool`, and then exactly one of `A` and `B`. `elif` continues the chain, and `else` is mandatory, because every path must give a value:

```quidra
score := 85
grade := if score >= 90 then "A" elif score >= 80 then "B" else "C"
println(grade)
```

`else if` is not a synonym for `elif` in an if-expression; it is an error with the hint to write `elif`. There is no `?:` operator.

Where a whole expression stands, an if-expression needs no parentheses: an initializer, an assignment's right-hand side, `return`, a call argument, an array element, an index, a default, an interpolation, or inside parentheses. Everywhere else it is written in parentheses: as an operand of an operator, `1 + (if c then 2 else 3)`; nested in a condition or a branch, `if a then (if b then x else y) else z`; and as the condition of an `if`, `elif` or `while` statement, the subject of a `match`, or the iterable of a `for`. A branch extends as far as an operator expression does, so `if c then 2 else 3 + 1` is `if c then 2 else (3 + 1)`. A statement that begins with `if` is always an if statement, which has no `then`. Outside brackets an if-expression ends with its line; across lines it is written in parentheses, where newlines are insignificant.

With a contextual type (the declared type of a binding or field, the target of an assignment, a parameter's type, the function's result at `return`, the element type of a typed array literal, or the other operand of an operator), every branch is checked against that type as an initializer is, and numeric branches use ordinary checked destination conversion when direct literal materialization is unavailable: `real32 x := if ready then 1.0 else 2.0` and `int | error result := if ready then 1 else error("bad")` are valid. Without one (an inferred declaration or an interpolation), the branches that are not numeric literals must have the identical type, and a numeric literal branch materializes into that type: with `int64 n`, `x := if ready then n else 0` is an `int64`. This is literal materialization, not an operator conversion: differing branch categories still require an explicit common context or conversion. `x := if ready then 1 else 2` uses the integer default `int`, as `x := 10` does.

An if-expression gives a value, never a reference: `&` cannot precede it, it cannot initialize a reference binding, and its branches cannot be addresses. Its branches cannot be `void`. Initialization flows through it as through an if statement: a binding read in a branch is read only on that branch's path, and storage that every continuing branch initializes is initialized afterwards.

The compiler may evaluate an if-expression with branches or, when both values are cheap and cannot fail, with a select; a branch that can fail runs only when it is chosen, so `if d != 0 then n / d else 0` never divides by zero. `then` is a reserved word.

## Error propagation and match

```quidra
int | error calculate(bool valid)
    if valid
        return 21
    return error("invalid input")

int | error doubled(bool valid)
    value := try calculate(valid)
    return value * 2

match doubled(true)
    int value
        println(value)
    error problem
        println(problem)
```

A fallible union is preserved when the destination explicitly includes the failure channel, as in `T | error result := operation()`. In `result := operation()`, the inferred type contains the complete non-`error` success side and the initialization site fails fast on `error`. In particular, `T | none | error` infers `T | none`.

When a value whose unnamed union contains `error` is consumed where the
expected type accepts every non-`error` alternative but excludes `error`,
Quidra inserts fail-fast handling at that consumption site. Success supplies the
non-error value; an actual `error` is reported and terminates the program at
that site. This rule removes only the distinguished `error` alternative:
ordinary unions are never implicitly narrowed, so `int | real64` cannot flow
to `real64` without explicit handling.

An expression statement discards its value. An error is not a value to
discard: when the statement's expression can be `error`, the same fail-fast
handling applies, so `file.remove(path)` or `scan(&n)` written as a
statement terminates the program on failure. This also applies to an
expression whose type is exactly `error`, such as a bare `error("invalid input")`
guard or an error-valued name: its own code and message are reported.
(A standalone expression
typed into an interactive session is displayed rather than discarded; see
[Interactive evaluation](#interactive-evaluation).) Binding the result, as in
`void | error removed := file.remove(path)`, keeps the error for explicit
handling.

An `error` has a code, the details its code defines, and an optional cause;
apart from these it keeps its origin, where it was made. `error("text")` makes a
user error with the code `ERROR`; `code = "CODE"` gives it a code of its own (a
literal of words of capital letters and digits joined by single underscores,
starting with a letter, that is not a Core code; package code starts its codes
with the package's prefix), and `cause = e` wraps another error (an `error`,
not an `error | none`):

```quidra
int | error read_port(string text)
    match int(text)
        int port
            return port
        error problem
            return error("invalid port", cause = problem, code = "CONFIG_PORT")
```

The parts are members: `e.code` (`string`, `"ERROR"` without a code),
`e.message` (`string`, the rendering of the code and the details, which
`print(e)`, `"{e}"` and `string(e)` also give), `e.cause` (`error | none`),
and the details by name (`e.path`, `e.reason`, `e.type`, ... as
`string | none`, `e.offset` as `nat | none`; see the error-value table of
`diagnostics.md`), `none` where the error's code has no such detail. None can
be assigned. A detail is read through `match`, as the cause is:

```quidra
bool | error has_config(string path)
    match file.read(path)
        file.read reader
            return true
        error problem
            match problem.reason
                string reason
                    if reason == "NOT_FOUND"
                        return false
                    return problem
                none
                    return problem
```

`==` compares two errors' codes, details and causes (along the chain), never
where they were made or propagated.

An error remembers the calls it propagated through, for its report: a hop is
recorded where it leaves a call of a user function, method or constructor, or
a call through an `func` value, at `try CALL`, at a fail-fast stop of `CALL` and
in the error case of `match CALL` that binds it. Storing an error records
nothing, and the success path of a call is unchanged.

`try` requires an expression whose union includes `error`, inside a function
that can return that error. On error it returns the same error from the current
function. Otherwise its type is the input union with `error` removed,
collapsing a one-member residual type. Normal absence is not propagated by
`try`.

Process termination itself is not a source type. `process.exit(status)` and
other compiler-known non-continuing operations are tracked as control-flow
facts. The compiler may propagate that fact through a user function when it can
prove the function has no normal return. A loop is not assumed to be infinite
merely from its syntax.

A `match` evaluates its subject once. Each case selects one actual alternative using `T`, `T name`, or `none`. A binding is local to its case. Matching a named variable automatically narrows that name within each branch. This refinement cannot permit writes or aliases that corrupt the union representation or invalidate the checked branch type.

Cases must cover every alternative exactly once; missing, duplicate, and impossible cases are compile-time errors. Branch initialization information merges using only continuing branches.

## Strings and arithmetic

Strings support `{expression}` interpolation, concatenation with `+`, content equality, literal newlines, and literal tab characters. Backslash has no escape semantics: `\n`, `\t`, `\r`, `\b`, `\f`, `\v`, `\a`, `\xNN`, and Unicode-style backslash sequences are ordinary source characters. A backslash is written as itself. Because `"` still delimits a string, a quote character inside string data is written through the `DQ` built-in value rather than by escaping it.

Numeric interpolation uses compact decimal formats:

| Form | Meaning | Example output |
| --- | --- | --- |
| `"{value:3}"` | Three significant digits across the whole number | `0.012345` becomes `0.0123` |
| `"{value:3.}"` | Minimum integer-part width three, padded with spaces | `7` becomes `  7` |
| `"{value:03.}"` | Minimum integer-part width three, padded with zeros | `7` becomes `007` |
| `"{value:.2}"` | Exactly two fractional digits | `1.2` becomes `1.20` |
| `"{value:03.2}"` | Zero-padded integer width three and two fractional digits | `1.2` becomes `001.20` |
| `"{value::3}"` | ASCII commas every three integer digits from the right | `1234567` becomes `1,234,567` |

A dot distinguishes integer width from whole-number significant precision. Integer width is a minimum: `12345` stays `12345` with `:3.` or `:03.`; no integer digits are removed. Omitting fractional digits after a dot leaves the fraction unchanged, so a `real64` value `7.125` with `:03.` becomes `007.125`. A fractional count fixes decimal places, not significant digits: `0.001234` with `:.2` becomes `0.00`. Leading zeros in the fractional count are accepted (`:.02` means `:.2`, and `:03.02` means `:03.2`); prefer the shorter spelling. Zero padding requires an integer width and a dot: `:03` is invalid.

A final `:G` composes grouping with either significant precision or integer/fraction layout: `"{value:3:3}"`, `"{value:.2:3}"`, and `"{value:03.2:3}"`. Integer widths, significant-digit counts and group widths are integer literals in 1..1000; fractional counts are in 0..1000 (`:.0` rounds to an integer). A bare dot, empty count or repeated grouping suffix is invalid. The former named decimal options are not aliases. Grouping is applied after rounding and integer padding; it affects the leading decimal integer portion, after any sign, but not the fraction or exponent. Sign characters do not count toward integer width. A formatted interpolation must be numeric. Ordinary `{value}` interpolation keeps canonical scalar formatting.

For an exact `real`, `:.N` and `:N` give correctly rounded digits, ties to even as for fixed-width reals, certified by refinement (`EXACT_UNPROVEN` when it cannot). Integer padding and grouping on exact values require decimal precision, because canonical exact text need not be decimal: use `:03.2` or `:.2:3`, rather than width-only `:03.` or grouping-only `::3`. Complex values do not support grouping. Integer radix conversion remains separate: `"{value:base=16,upper}"` selects base 2..36 and optional uppercase digits, and cannot be combined with decimal precision, padding or grouping.

The immutable built-in `string` values `NL`, `HT`, `CR`, `DQ`, `BS`, `FF`, `VT`, and `BL` denote LF, HT, CR, `"`, BS, FF, VT, and BEL respectively. They are ordinary expressions, so both `"A{HT}B"` and `separator := HT` use the same value. All eight names are two-letter uppercase abbreviations, and they are the only built-in values spelled entirely in capitals: the capitals mark a symbol the language provides for a character that cannot be written directly, so a reader never has to ask whether `HT` is a user binding. `true`, `none`, `print` and every other built-in keep their ordinary spelling. Lowercase spellings such as `nl`, `ht`, and `bs` are ordinary identifiers, not aliases for these built-ins; they work only when the program defines them. Literal `{{` and `}}` continue to represent braces in an interpolated string. NUL remains forbidden in source strings.

Numeric binary operators choose a static common type: select Natural < Integer < Real < Complex, then the widest operand type only within the highest family; arbitrary precision/exact wins within that family. `real16` with `real16b` selects `real32`, and `com16` with `com16b` selects `com32`. Convert both operands before the operation with checked range and normal real/complex rounding; do not widen based on values or alter operand storage types. Thus `nat64 + int32` uses `int32`, and `int32 + real16` uses `real16`. Complex values have no ordering. Numeric values implicitly convert to the resolved numeric destination in initialized declarations, assignments, parameter defaults, returns, and value arguments to functions, methods and constructors. The checks match explicit `T(value)`: destination range, ordinary real/complex rounding, and integral-only real-to-integer conversion. Supported whole-array/tensor conversions preserve structure and shape. A failed implicit conversion stops before the destination write, return or callee body; source variable types stay unchanged. Writable `T &` arguments require the exact storage type and never convert. `bool` is separate from numeric types and never participates in these implicit conversions.

Fixed-width integers additionally support `AND`, `OR`, `XOR`, unary `NOT`, `<<`, and `>>`. The uppercase words are deliberately distinct from bool-only `and`, `or`, and `not`; `&` remains storage access and `|` remains union syntax. Bitwise operations do not apply to `bool`, floating-point values, `int`, `nat`, `real`, `bin`, tensor, or tensor values. Signed fixed-width integers have a defined two's-complement bit representation. `<<` operates on the N-bit representation and discards shifted-out bits without turning the operation into checked arithmetic; signed `>>` is arithmetic with sign extension and unsigned `>>` is logical with zero fill. Shift counts outside `[0, width)` are rejected statically when proven and otherwise fail deterministically at runtime.

Explicit numeric casts use the destination type directly: `int8(value)`, `nat32(value)`, or `real32(value)`. A range-checked cast whose runtime value may not fit has source-visible type `converted-type | error`. For scalars the success type is the destination scalar type; for arrays and tensors it is the entire converted container, never a container of per-element unions. Container conversion is atomic: one out-of-range leaf makes the whole conversion `error`. An inferred declaration or explicit success-only destination fails fast on the error, an explicit union destination preserves it, `try` propagates the error, and `match` may handle an explicitly preserved result locally. Integer narrowing never wraps or clamps. Integer-to-real and real-to-real casts are explicit practical conversions and may use the destination IEEE-754 rounding. Real-to-integer casts accept only mathematically integral values in the destination range; an invalid conversion produces `T | error` (including `NOT_INTEGRAL` for fractional values). Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` when rounding is intended.

With a `string` operand, `T(text)` parses text as the numeric type `T` and has static type `T | error`, including when the text is a literal. With a numeric operand, the same call form uses the numeric cast rules above. `string(value)` takes one positional value and returns its standard text for a numeric value, `bool`, `bin`, `error`, or a printable union. An existing `string` is returned as the same text. For a printable union the active alternative determines the text, including the error message for `error` and `"none"` for absence.

All ten integer types (`int`, `nat`, `int8`/`int16`/`int32`/`int64`, and `nat8`/`nat16`/`nat32`/`nat64`) provide `.b2` through `.b36` and `.B2` through `.B36` members returning `string`, without parentheses. For radix `N`, `.bN` uses lowercase `a`–`z` and `.BN` uses uppercase `A`–`Z`. Both preserve the exact integer value, write zero as `"0"`, retain a single minus for negatives, and add no prefix or padding. Thus `255.b16` is `"ff"`, `255.B16` is `"FF"`, and `(-255).b16` is `"-ff"`. Ordinary postfix precedence means `-255.b16` tries to negate a string and is invalid. Only canonical bases `2` through `36` are accepted on integers; `.b1`, `.b37`, and `.b02` are invalid. Other built-in types do not expose these members, but user-defined class member names remain unrestricted. Use `"{value:base=16,upper}"` to choose the radix in interpolation, or `"{value.B16}"` to interpolate the member's string directly. See [numeric text conversion](numeric-and-bin.md#parsing-and-text-conversion).

Mathematical functions live in the first-party `math` package namespace after an explicit `import math`: `math.abs` accepts numeric values, `math.sqrt` accepts floating-point values, and `math.min`/`math.max` require two values of the same numeric type. There are no Core bare spellings of these names, so `abs`, `sqrt`, `min`, and `max` remain ordinary identifiers.

`print(value)` writes a scalar without adding a newline. Prefer `println(value)` for a terminated line or `println()` for an empty line; `NL` interpolation remains available for embedded line breaks. `print` and `flush()` return `void | error`: an output failure such as a closed pipe is reported as `error`, and because the usual spelling is a statement, that error fails fast. Core has no console `write` builtin and no `io` namespace; `write` remains an ordinary user or package identifier.

`scan(...)` is the one input operation. It reads one line of standard input, without the trailing LF, and stores what it reads into the targets named by its format; its result is `void | error`. `scan(&n)` reads a single value and is the short form of `scan("{&n}")`. `scan(...)` reads one complete line, with `void | error` for success or failure.
`scan(&n)` is shorthand for `scan("{&n}")`. In an input format, `{&n}`
writes an existing target and `{T &n}` declares a new typed target. `T`
may be `string`, `bool`, or a numeric scalar. An inline declaration is
allowed in a standalone, fail-fast `scan` statement, where the new binding
is initialized and visible in the containing scope after success. Input
placeholders are not ordinary output interpolation: each one consumes the
text before the next literal separator, and every written separator must
match exactly. For example, `scan("{&n} {int &m}")` reads two numbers
separated by exactly one space. The `bool` format accepts only `true`
or `false`. A string target retains its field exactly; numeric fields
use the parsing rules of `T(field)`, including range checks. Adjacent placeholders
without literal separators and trailing unmatched text are errors.

**Input format updates are atomic:** all fields and the complete line
must validate before any destination changes. If conversion, a literal
separator, or the final remainder is invalid, all targets retain their
previous values. With a retained `void | error` result, every existing
target must therefore already be initialized; the inline declaration
form is reserved for fail-fast statements and successful condition branches.
EOF on standard input is `INPUT_END` (an `error`), while a file reader
uses `false` for EOF. Runtime strings always contain valid UTF-8 and
no embedded NUL; use `bin` for arbitrary bytes.

Integer division or remainder whose divisor is statically known to be zero is a compile-time error, and so is a real constant expression that produces NaN, including `0.0 / 0.0`. Otherwise integer overflow at every integer width, dynamically determined integer division/remainder by zero, index and slice bounds failures of arrays, strings, bin and tensors (`INDEX_BOUNDS`, which names the index and the length: `index 5 out of bounds for length 5`, at the index operand), invalid allocation sizes, zero range steps, and exceeding the native call-depth safety limit (`CALL_DEPTH_LIMIT`: `maximum recursion depth exceeded (limit 4096)`) are deterministic runtime errors with exit status 101. Range-checked numeric casts, including whole-array and whole-tensor conversions, instead produce their source-visible `error` alternative when any converted value is outside the destination range, with a message that names what was converted, the destination type and the reason (`numeric conversion out of range: value cannot be represented as int8`); status 101 occurs only if that error reaches a contextual fail-fast consumption site. The call-depth guard fails before host stack exhaustion rather than allowing a segmentation fault. Signed infinities are valid real values, but NaN is never observable. Real overflow, nonzero real division by zero, and zero to a negative real power may produce infinity. An indeterminate operation stops the program (`NONFINITE_RESULT`: `real64 multiplication produced NaN`, `tensor<real32> subtraction produced NaN`; zero divided by zero is `DIVISION_BY_ZERO`: `real32 division by zero`). Constant expressions enforce the same rules at compile time; finite out-of-range literals and range-checked casts still fail. A tensor operation tests every element it computes: on the CPU the first failing element in logical order stops the program at the operation; on a GPU the test stays on the device and fails at the next synchronization point, like the other device-resident checks, naming the operation and the statement that queued it. A delivered gradient and a real tensor a package function writes are likewise validated against NaN (`NONFINITE_RESULT`); infinity is allowed. A package may waive Core's output scan only by declaring that it validates NaN itself or preserves NaN-free inputs, including infinite inputs.

An exact `real` prints exactly: a finite decimal when it has one (`0.5`, `4.0`), otherwise an exact fraction (`1/3`), and a symbolic value in its canonical text, with `√` for square roots, the symbols its providers give their constants and operations (`π`, `e`, `sin(1)`), juxtaposed coefficients (`2√3`, `3π`, `π/2`, `√2/2`) and other roots as powers (`2^(1/3)`). `print`, interpolation, REPL display and `string(value)` share that text. `real(text)` reads decimals, fractions (`1/3`, `-2/5`) and signed infinity (`inf`, `-inf`, `∞`, `-∞`), not symbolic text. Exact infinities print as `∞` and `-∞`; fixed-width infinities print as `inf` and `-inf`.

Float text uses the shortest decimal representation that round-trips to the same binary floating-point value. If that shortest representation would look integral, at least one fractional digit is retained, so `0.6` stays `0.6`, `1.0 / 3.0` is `0.3333333333333333`, and `4.0` remains `4.0`. The same canonical form is used by `print`, interpolation, REPL display, and `string(value)`. Fractional literals continue to require a leading zero; `.5` is invalid and `0.5` is the canonical form.

## Interactive evaluation

When attached to a terminal, `quidra` starts an interactive session. `quidra repl` starts the same session explicitly. Accepted top-level declarations and statements accumulate in session order and are rechecked with the normal language rules for each submission.

A standalone expression is an interactive submission result. Its checked value is displayed automatically unless its type is `void`; a process-terminating expression never reaches display. This is a REPL presentation rule, not an implicit source-level call to `print`; the expression retains the same parsing, typing, arithmetic, reference, class, generic, equality, and error semantics as the same expression in a source file. The one difference is the result itself: a displayed value is consumed by the display, not discarded, so the fail-fast rule for discarded errors does not apply to it. A standalone `int("x")` or `error("message")` displays the error value and the session continues; inside a larger expression or as a statement in a block, the usual rules apply.

The accumulated-source implementation compiles the root source from an in-memory overlay while retaining a stable virtual source path for relative imports, package-lock checks, and diagnostics; it does not write and re-read the growing root source on every submission. It still rechecks and recompiles the accumulated program. The already accepted prefix is marked explicitly in typed IR, and console output from that replay prefix is suppressed. External or nondeterministic operations are not silently replayed: before native execution of a candidate that may reach such an operation, the session arms a conservative safety barrier. A later submission is rejected with `REPL_REPLAY_UNSAFE` until `:reset`; the barrier remains armed if native execution fails because an external effect may already have occurred. `:reset` clears all accepted REPL state.

A rejected compile-time submission does not become part of the session. EOF exits successfully. Ctrl-C cancels the current incomplete submission without discarding previously accepted state.

## Tensor

`tensor<T>` is a first-class dense numeric N-dimensional value type. `T` must be numeric and is always static; Quidra does not have a tensor type with an unknown element type. Without a shape pattern, rank and known extents are compiler-inferred flow facts obtained from construction, tensor arithmetic (by its broadcasting rule), reshape/index operations, control flow, and ordinary checked function contracts.

An optional brace group constrains tensor rank and axis sizes. Without `...`, it is an **exact-rank shape pattern**:

```quidra
tensor<real32> any_rank
tensor<real32>{3} vector_of_three
tensor<real32>{3, _} rank_two_first_axis_three
tensor<real32>{3, 224, 224} chw_224
tensor<real32>{_, _, _} any_rank_three
```

Without an ellipsis, the number of shape entries is the required rank. Each extent is a `nat` value, a shape variable or `_`. A constant expression is folded by the compiler; a runtime expression is evaluated once and captured. `_` requires the axis to exist and leaves its extent unrestricted. Thus `tensor<real32>{3, _, _}` accepts `[3,H,W]` but rejects `[3,H]`, `[3,H,W,D]`, and `[1,H,W]`. The angle group names the mandatory element type; braces name the shape. Empty slots and trailing commas are invalid.

Known rank or extent conflicts are rejected statically. A tensor expected type may supply the numeric element type when an explicit shape argument is present, so `tensor<real32> x := tensor.zeros([2, 3])` is valid and avoids repeating `real32`. Without either an explicit tensor type argument or an expected tensor element type, as in `x := tensor.zeros([2, 3])`, the element type is ambiguous and the call is rejected. If a source tensor's relevant rank or extent is not statically known, assignment or parameter passing to a constrained destination performs the corresponding runtime constraint check instead of silently assuming the shape. Declared captured constraints remain fixed for that binding across reassignment, while inferred flow facts may weaken after reassignment or control-flow joins. APIs that produce runtime data may additionally validate an expected pattern through their normal result model. Shape constraints and inferred rank/shape facts do not change TensorStorage or the LLVM ABI.

```quidra
a := tensor<real32>([3, 224, 224])
a[0, 0, 0] = 1.0

tensor<real32> contextual := tensor.zeros([2, 3])
tensor<real32>{3, _, _} z := tensor.zeros<real32>([3, 224, 224])
tensor<real32>{1, _, _} o := tensor.ones<real32>([1, 224, 224])
```

The direct `tensor<T>(shape)` form creates uninitialized tensor storage. Scalar indexed assignment initializes an element. `tensor.zeros<T>` and `tensor.ones<T>` create fully initialized tensors. When the expected tensor type supplies the element type and every exact extent, `tensor.zeros()` and `tensor.ones()` may use that context to allocate; an unconstrained rank or `_` extent still requires an explicit shape argument. Initialization is tracked independently from shape knowledge and numeric contents; reading an uninitialized element is a deterministic safety failure.

A shape may contain one `...`, standing for zero or more axes. `_` always stands for exactly one axis. Constraints before `...` align from the start; those after it align from the end, without overlap. The minimum rank is the number of explicitly written axes. A statically known rank or extent mismatch is a compile-time error; otherwise the constraint is checked at runtime before an axis is read.

| Shape | Meaning |
| --- | --- |
| `{...}` | Any rank, including rank zero; equivalent to `tensor<T>` (prefer the latter) |
| `{N, ...}` | First axis is N |
| `{_, N, ...}` | Second axis is N |
| `{..., N}` | Last axis is N |
| `{N, ..., M}` | First axis is N and last is M; rank is at least two |
| `{N, M}` | Exactly two axes, N and M |

The same rules apply to bindings, parameters, results and match cases. Each shape permits at most one ellipsis. Shape variables retain their existing declaration and reference syntax: `tensor<real32>{..., nat C} x := value` binds C to the last axis, while `{nat N, ..., N}` requires equal first and last axes. A later assignment may have a different rank if the captured head and tail constraints still hold. An omitted shape in `tensor.zeros()` or `tensor.ones()` needs an exact-rank context; an ellipsis cannot supply the missing axes.

### Shapes in braces and shape variables

A shape is written in braces, `tensor<ELEM>{AXIS, ...}`, which gives each bracket kind one role: `<...>` the element type, `{...}` the shape, `[...]` an index or a slice, `(...)` a call. Outside string literals, `{` and `}` appear only in shapes (`Braces appear only in tensor shapes.`), and a tensor type always names its element type (`tensor{2, 3}` is `TENSOR_TYPE`). Each axis is one of:

- `nat N`: declares a new shape variable `N`, whose value is the axis's extent;
- `N := EXPR`: declares a new shape variable `N` equal to `EXPR`, and the axis must equal it;
- `N`, a bare name: an existing shape variable or numeric value convertible to `nat`, which the axis must equal;
- `0 < nat N <= limit` or `nat N > 0`: captures the actual axis extent in a new read-only `nat N` and checks its ordered range;
- `0 < x < 5` or `x < limit`, with an existing `x`: requires the axis to equal the checked `nat(x)` and also checks the range of `x`;
- `_`: an extent of any length that binds nothing (`{_, _}` is two independent axes);
- any other numeric expression (`3`, `rows * cols`, `nat(n)`, `(n)`, `v.len()`): the axis must equal its checked conversion to `nat`.

Write `nat N` to capture a dimension or `N := EXPR` to define it from an expression. The explicit form `nat N := EXPR` is also accepted. Only a lone identifier is a bare name: `(n)` is an expression. Every captured extent is `nat`: evaluate the expression once at its original static type, then convert its result with the checks of explicit `nat(result)`. Operands and intermediate arithmetic keep their types. Thus `batch := 3` permits `tensor<real32>{batch * 2, 224} image := tensor.zeros()` without repeating `nat(...)`. Integral real results may convert; negative, fractional or unsupported results fail. A boolean expression is never a numeric extent. `{N := 32}` requires exactly extent 32; it is not a default. In a single range with two untyped names, `{x < y}` selects `x` as the subject and `y` as its bound. A bare name that names nothing remains a `SHAPE_VARIABLE` error; an existing numeric name does not declare a shape variable. A shape variable is a new name: declaring a visible name is `SHADOWING`, and declaring one twice in a shape is an error, so a square shape is `{nat N, N}`. A shape is one scope unit: an axis may use any variable of its shape whatever the textual order, `N := EXPR` axes are evaluated in dependency order, and a cycle is an error.

```quidra
tensor<real32>{2, 3, 5} cube
tensor<real32>{nat R, nat W} d := tensor.zeros<real32>([2, 3])
print("{R} x {W}")
// output: 2 x 3
```

```quidra
nat n := 3
tensor<real32>{n, _} rows := tensor.zeros<real32>([3, 7])
tensor<real32>{nat K, K} square := tensor.zeros<real32>([2, 2])
nat height := 4
nat width := 5
tensor<real32>{H := height * width, nat W} image := tensor.zeros<real32>([20, 3])
tensor<real32>{M := 2 * N, nat N} pair := tensor.zeros<real32>([4, 2])
print(H + M)
// output: 24
```

Shape variables are read-only `nat` values (`Shape variable 'N' is read-only.`), visible from their declaration onward like other bindings; one declared in a loop body is bound anew on each iteration, and a later shape reuses one by its bare name (`tensor<real32>{B, C, H, W} y := conv(x)` after `x`'s `{nat B, nat C, nat H, nat W}`). Their values come from the initializer: a declaration without one takes literals, expressions, `N := EXPR` and bare existing values, but no `nat N` (`'B' has no value: ...`). The expressions that use no variable of their own shape are evaluated before the initializer, left to right, the rest after it; the shape is then fixed for the binding, and every later assignment is checked against it. An axis that a shape variable fixes is checked when the extent is known at compile time (`TENSOR_SHAPE`) and otherwise when the value is stored, naming the variable and the binding (`SHAPE_MISMATCH`: `shape variable 'K' is 2, but axis 1 of 'sq' is 3`).

A match case may declare shape variables for its body and use every axis form: `tensor<real32>{3, nat W} rgb`. A case is taken when the value has its rank and every axis constraint of the case holds; a range-predicate mismatch proceeds to the next case instead of raising `RANGE_CONSTRAINT`; a case whose selection depends on such a run-time test (an equality axis, a non-constant expression) covers nothing, so a later case of the same type takes the rest. A signature is one scope unit containing its result and every parameter shape. Parameter axes bind the shape variables, derived axes are evaluated in dependency order, and equality constraints are checked in parameter order. Result extents are computed last from those bindings. A result may write `{N + M}` or `{L := N + M}`; a bare `nat L` in a result cannot infer a value. Every ordinary parameter is visible to an axis expression: `f(nat N, tensor<real32>{N} a)` constrains the axis to its argument N.

For example, `tensor<real32>{N, M} matmul2(tensor<real32>{nat N, nat K} a, tensor<real32>{K, nat M} b)` takes shapes `[2, 3]` and `[3, 5]` to `[2, 5]`. Different known K extents are a compile-time `TENSOR_SHAPE` error; unknown extents are checked at entry before the body. `tensor<real32>{L := N + M, C} concat_rows(tensor<real32>{nat N, nat C} a, tensor<real32>{nat M, C} b)` computes its result extent from both parameter bindings. A signature may refer to a variable declared later in its text. Class generation and methods use the same shape rules. A method may refer to a member by its bare name or through the explicitly declared instance name; header shapes cannot read members before initialization.

An `extern` caller binds and checks shaped arguments before the foreign call. Every shaped mutable tensor borrow is checked again afterwards, even when the pre-call constraints were proven. A read-only tensor value is not checked again. A shaped `func<R(P...)>` type is also one scope unit; its shape variables are bound afresh on each call. A function value must match that unit up to renaming, including the identities of captured values. A square-only function does not match a type with two independent axes, and a function whose axis refers to its ordinary parameter has no matching `func` type.

A proven direct call may enter the internal checked body. Other direct calls, calls through function values, REPL execution and exports use the checking entry. Both paths capture shape expressions once.

### Tensor device placement

Device placement is explicit runtime/compiler metadata and is not part of the nominal `tensor<T>{...}` type. CPU is the default. A tensor constructor may instead name a zero-based GPU index with the ordinary named-argument syntax `gpu = n`:

```quidra
cpu := tensor.zeros<real32>([1024])
gpu0 := tensor.zeros<real32>([1024], gpu = 0)
tensor<real32>{1024} gpu1 := tensor.ones(gpu = 1)
```

The `gpu` argument is optional but, when present, must be named and is a `nat`. There is no public negative GPU sentinel: `gpu = -1` is invalid. Omission means CPU. The same placement rule applies to uninitialized `tensor<T>(shape)`, zeros, and ones.

Transfers are explicit value operations. `value.gpu(index)` copies a tensor to the requested GPU and requires exactly one `nat` index. `value.gpu()` is invalid. `value.cpu()` copies a tensor to CPU and takes no arguments. An explicit transfer remains an observable semantic boundary for optimization: `tensor.zeros<T>(shape).gpu(0)` means CPU allocation followed by CPU-to-GPU transfer and may not be rewritten as direct GPU allocation. Likewise a later `.gpu(n)` cannot relocate an earlier computation to that GPU.

GPU work is host-asynchronous by default. Quidra queues supported kernels, uploads, same-device copies, accelerator-library calls, and collectives in backend order without waiting after each operation. The runtime preserves same-device dependencies through the backend default stream/command queue. Runtime checks that belong to tensor-to-tensor GPU operations, such as checked integer overflow, the finiteness of real results and the domain of real powers, cast-range validation, logarithm-domain validation, and square-root-domain validation, also remain device-resident: their status is consumed at the next synchronization boundary rather than forcing a host wait after the operation. A host-visible read is a synchronization boundary: `.cpu()`, `.item()`, and any operation that must expose completed GPU bytes wait as required. `gpu.sync(index)` explicitly waits for all previously queued Quidra work on one non-negative GPU index and returns `void`; an unavailable index is a runtime-safety failure. Time sampling does not synchronize GPU work by default: `time.now()` and `time.since(start)` sample the monotonic clock without waiting, so progress reporting, logging, and ordinary timing do not serialize asynchronous GPU execution. Both accept an optional named `sync = bool` argument whose default is `false`. `time.now(sync = true)` first waits for outstanding Quidra GPU work and only then samples the start clock, excluding earlier GPU workload from the interval. `time.since(start, sync = true)` waits for outstanding Quidra GPU work before sampling the end clock, so completion of GPU work submitted during the measured interval is included in the elapsed duration. Resource destruction must not add an ordinary per-operation host wait: backend storage may be pooled or retired until a safe lifetime boundary. Program/runtime teardown waits for outstanding work and reports any deferred checked-GPU failure before destroying backend resources.

Quidra never performs an implicit CPU/GPU or GPU/GPU transfer. Tensor-to-tensor operations require compatible operands to be on the same device; a mismatch fails rather than copying either input. Results remain on the input device. Scalar literals and scalar variables are not tensor placements and may be passed as scalar kernel arguments to an operation on the tensor's device.

A requested GPU that does not exist or whose backend is unavailable is a runtime error, for example `error: gpu(0) is not available`. CPU fallback is forbidden. If an operation has no implementation for the tensor's current GPU backend, it fails explicitly with a diagnostic such as `operation is not supported on gpu(0)`; executing the operation over hidden CPU storage is not a valid implementation.

The public placement semantics are independent of OS and vendor. There are three device backends, each with a canonical id and a separate display name: `cuda` (NVIDIA), `hip` (AMD), and `metal` (Metal). The id is the machine-readable spelling; the display name is what `quidra gpu` prints. There is no `cpu` device backend — CPU execution is ordinary native LLVM code, not a device — so `cpu` never appears as a tensor placement target. The backend table in `project.toml` does list `cpu`, with kind `native` rather than `gpu`, for exactly that distinction.

NVIDIA systems use the `cuda` backend through the CUDA Driver API rather than a user `nvcc`, `CUDA_HOME`, or `/usr/local/cuda` selection. Apple Silicon uses `metal`. AMD uses `hip`, through the HIP runtime and runtime-compiled HIP kernels, when a compatible ROCm/HIP runtime is present, without changing Quidra source syntax. Backend kernel availability may still be element-type-specific; an unsupported backend/element-type combination is an explicit runtime error and never permission for CPU fallback. Unified-memory hardware may allow a backend to elide a physical copy, but the explicit logical device transition remains part of the program semantics. `quidra gpu` reports Quidra's zero-based device enumeration and active backend information.

Tensor storage is row-major, with the last dimension contiguous. Integer indices remove axes, slices retain axes, and omitted trailing dimensions mean full slices. Tensor slices take the forms of array slices that ascend, as views: `t[a:b]` includes `b`, `t[a:<b]` excludes it and `t[a<:b]` excludes `a`, with an optional positive step; a descending marker (`>:`, `:>`) is a `SLICE_STEP` error, and a start past an excluded end is an empty view. Inferred rank and known shape facts are projected accordingly. Fully indexing a known rank-N tensor with N integer indices yields an internal rank-0 tensor, not a scalar; `.item()` is the explicit scalar extraction operation. On a GPU tensor, `.item()` performs only the required one-element device-to-host read. This scalar extraction is an explicit semantic boundary and is not permission to move or evaluate the surrounding tensor operation on the CPU.

```quidra
z := tensor.zeros<real32>([3, 224, 224])
channel := z[1]
crop := z[:, 10:<20, 30:<40]
value := z[0, 10, 20].item()
```

Slices may share internal storage, but source semantics remain value-oriented. Mutating a copied tensor or slice triggers copy-on-write when needed. Slice assignment and writable `&` references to tensor elements are intentionally not exposed.

`.transpose(axis0, axis1)` swaps two axes (`nat` values) as a metadata-only view: shape and strides are permuted while storage, offset, and device placement are preserved. Constant axis values may be used to reject an invalid axis at compile time, but they do not permute or specialize the source-visible static shape type. `.reshape(shape)` requires contiguous storage and never performs a hidden copy. Use `.contiguous()` explicitly before reshaping a non-contiguous view. The static length of the shape array establishes result rank; its extent values may be used for diagnostics or expected-type checks but do not refine the source-visible result extents. `.shape()` returns `nat[N]` when rank N is inferred at that program point and `nat[]` when rank is unknown; creation and reshape shape parameters are `nat[]` as well. Numeric shape arguments of other element types use the ordinary checked value-argument conversion to `nat[]`. `.is_contiguous()` reports layout state.

Tensor `+`, `-`, `*`, `/`, and integer `%` are elementwise and use the common numeric element/scalar type with checked conversions before evaluation. `^` is also a Core basic operator: tensor exponentiation is deliberately one-way as `tensor ^ scalar`, with tensor elements and the scalar converted to their static common numeric type before evaluation; `tensor ^ tensor` and `scalar ^ tensor` are not implicit operations. Integer tensor exponents must be non-negative and use checked integer arithmetic. Tensor-to-tensor implicit broadcasting requires identical rank and, on every axis, equal extents or an extent of 1 on one side; the result axis takes the common extent, or the other side's extent where one side is 1. 0 is not special: `[0, 3]` with `[1, 3]`, in either order, gives `[0, 3]`, while `[0, 3]` with `[2, 3]` is a runtime error. Rank-changing broadcasting is not implicit: `[3]` with `[0, 3]`, `[1, 3]` or `[2, 3]` is an error, reported at compile time when both ranks are statically known and at runtime otherwise; reshape explicitly, for example to `[1, 3]`, first. Scalars are the one exception and broadcast to any tensor rank. Every tensor arithmetic result remains on the tensor's device. A backend/element-type combination without a native implementation fails explicitly; tensor arithmetic never gains permission to migrate the surrounding operation to CPU implicitly.

Tensor `==`, `!=`, `<`, `<=`, `>`, and `>=` are elementwise and return a same-shaped `tensor<bool>` mask. Tensor-to-tensor comparison uses the common numeric element type with checked operand conversions and requires identical rank and extents; it never performs tensor-to-tensor broadcasting. A numeric scalar is the comparison-broadcast exception: it and the elements convert to their common type before comparison with every element. `!=` is the elementwise logical negation of `==`. Whole-tensor questions are explicit: `mask.all()` requires every mask element to be true and `mask.any()` requires at least one; empty masks use the logical identities `true` for `.all()` and `false` for `.any()`. `tensor<bool>` supports ordinary tensor storage, indexing, slicing, copying, and explicit device transfer, but numeric tensor arithmetic and numeric kernels reject it unless an API explicitly accepts masks. Results remain on the tensor's device.

Boolean tensor masking with `values[mask]` accepts a `tensor<bool>` that
has exactly the input's shape, including all axes. The result is a new
rank-one tensor containing the true-selected elements in logical row-major
order, with an empty rank-one tensor if no position is true. It keeps the
source's CPU/GPU placement and supports backward propagation through its
gather graph, accumulating gradients at the selected positions. Output
length is dynamic; when the mask is stored on a GPU, the implementation
transfers the mask to the host once to determine the selected indices, then
performs the gather on the source's original device. This host synchronization
is currently part of the operation's performance profile and may be
optimized without changing source semantics. Tensor integer-array indexing
and masked assignment are separate features and are not implied by this form.

Explicit numeric casts use the destination scalar type as the operation: `real64(value)`, `real32(value)`, `int8(value)`, and so on. Applied to a tensor, the cast preserves rank, shape constraints, known extents, and value semantics while converting every initialized numeric element. A cast between floating tensor element types (`real16`, `real16b`, `real32`, `real64`) preserves tracking. Its forward uses the ordinary numeric conversion, with exact widening and range-checked, nearest-even narrowing; its backward converts the incoming gradient to the source element type with the same conversion rule. Gradient contributions accumulate in each source destination’s own type. Casting a tracked tensor to an integer, bool, or complex type is rejected; call `.untrack()` explicitly when the graph is intentionally cut. A scalar conversion whose runtime value may be outside the destination range has source-visible type `T | error`: an inferred declaration or explicit success-only destination fails fast on error, an explicit union destination preserves it, `try` propagates the error, and `match` handles an explicitly preserved result locally. Integer widening whose complete source range fits the destination is infallible. Integer-to-real and in-range real-to-real conversion may use destination IEEE-754 rounding. Real-to-integer casts accept a mathematically integral value and then check the destination range; they return `T | error` for a fractional or out-of-range value. The conversion preserves the stored IEEE value exactly while checking integrality, so it never rounds a fraction. Container-specific cast methods are not part of the surface language.

## Implementation scope

The native core supports fixed-width numeric types, explicit numeric conversion, initialized/uninitialized arrays, first-class dense tensors, basic tensor operators and structural operations, generic autograd/package-extension mechanisms, tensor storage/layout/device operations, console and file I/O, explicit package/local-module resolution, monomorphized generics, user-defined classes with explicit composition, capture-free typed function values, and bounded structured concurrency. Domain semantics such as linear algebra, DNN operations, image/video processing, and signal processing belong to packages rather than Core. The browser-facing WebAssembly frontend shares the ordinary Quidra frontend rather than defining a separate language implementation.


## Standard namespaces and imports

Standard namespaces are always visible; importing them is an error. Current reserved namespaces are `cli`, `file`, `environment`, `test`, `time`, `gpu`, `autograd`, `reflect`, `random`, `process`, `map`, `distinct`, `json`, `http`, `tensor`. `math` is intentionally absent: it is an installed package namespace, not a Core standard namespace.

A source-file import always uses a quoted path:

```quidra
import local_file = "./local.qui"
import shared = "@/shared.qui"
```

An unquoted non-standard import is an installed package:

```quidra
import plotting
import plot = plotting
```

Package resolution never silently falls back to the current directory. This distinction is semantic: adding or removing a local file cannot change the meaning of an unquoted import.

### math

The first-party Math package supplies mathematical functions, constants, linear algebra, and numerical algorithms. Import it explicitly. Math defines the semantics, native kernels, autograd rules, and compiler-extension policy for its operations.

```quidra
import math

root := math.sqrt(real64(16.0))
real exact_pi := math.pi
left := tensor.ones<real32>([1, 1])
right := tensor.ones<real32>([1, 1])
product := math.matmul(left, right)
```

#### Matrix multiplication operator

> Specified for `develop`. Compiler support is pending.

`a @ b` calls the first-party Math `matmul` overload for `a` and `b`, as in
`a.matmul(b)` after `import math`. The Math package must be in scope. The
operator inherits Math's supported operand types, shape checks, result shape,
device placement, and autograd behavior. Both operands are evaluated once,
left to right. `@` shares the precedence of `*`, `/`, and `%`, and groups
left to right with them. Tensor `*` is elementwise.

### Numerical reductions

Core provides `sum()`, `mean()`, `var()`, `std()`, `min()`, and `max()`
on numeric arrays and tensors without an import.

```quidra
values := [2, 4, 6]
average := values.mean()
sample_variance := values.var(ddof = 1)

matrix := tensor.ones<real32>([2, 3])
rows := matrix.sum(dim = 1)
```

| Method | Array result | Tensor element types |
| --- | --- | --- |
| `sum()` | Element type for integers, reals, and complex numbers | Integers and floating-point reals |
| `mean()` | Exact `real` for integers, element type for reals and complex numbers | Floating-point reals |
| `var()`, `std()` | Exact `real` for integers, element type for reals | Floating-point reals |
| `min()`, `max()` | Element type for integers and reals | Integers and floating-point reals |

An array returns a scalar. An array sum starts from the element type's zero
and visits elements in index order with the scalar addition rules. Integer
means and variances accumulate in exact `real`. Extrema retain the first
occurrence of a tie.

A tensor reduction preserves the element type and device. Without `dim` it
returns a rank-0 tensor. With a zero-based `dim` it reduces each slice on
that dimension and removes it, keeping the order of all other dimensions.
For example, `t.max(dim = 1)` reduces `[2, 3, 4]` to `[2, 4]`. Tensor sums
combine adjacent pairs in logical order at each level and carry any odd last
value to the next level. Tensor extrema select the first occurrence in each
slice. Floating-point tensor reductions retain the input's autograd graph.
Tensor extrema use one host snapshot to select indices and gather the winners
from the original tensor. Tracked standard deviation uses a host snapshot of
the reduced variance to assign zero gradients to zero-variance slices. CUDA
supports the square root and reciprocal square root used for standard
deviation and its first derivative.

Variance centers the values at their mean, sums the squared deviations, and
divides by `N - ddof`. `N` is the number of elements in the array, the tensor,
or the selected tensor dimension. `ddof` is a `nat` with default `0`, and
`std()` returns the square root of that variance. `ddof >= N` raises
`REDUCTION_DDOF`.

An empty sum gives the element type's zero. An empty mean or extremum and a
dimension outside the tensor rank raise `MATH_SHAPE`. Math's `sum_last()`,
`min_last()`, and `max_last()` helpers broadcast a last-dimension reduction
back to the original tensor shape.

### Output flushing

`flush()` explicitly flushes standard output. It is useful when output must be observable before a long-running process exits or is externally stopped; `print` does not imply a flush.

### cli

`cli` enables one declarative command-line binding in the root source file:

```quidra
cli args
    string source = argument()
    int count = option(default = 1)
    bool verbose = flag()

println(args.source)
println(args.count)
println(args.verbose)
```

`argument()` declares a required positional value. `argument(default = value)` declares a trailing optional positional value; optional positional fields must come after every required positional field so omission is unambiguous. `option(default = value)` declares a named `--field value` option whose type is inferred from the field declaration and checked against its default. `flag()` declares a boolean `--field` flag. Field names are the command-line names, so they are not repeated as string literals. CLI scalar values support `string`, `bool`, and every `nat*`, `int*`, and `real*` kind; `flag()` is `bool` only. Exact numeric CLI text is parsed directly into `nat`/`int`/`real` without an intermediate fixed-width integer or IEEE real64. Missing required values, invalid typed values, non-UTF-8 text values, duplicate or unknown arguments, and misplaced arguments terminate with CLI status 2.

The generated CLI binding is a root top-level value. Function bodies do not implicitly capture top-level bindings, including the CLI binding; pass CLI-derived values or a configuration value explicitly to reusable functions.

Direct execution passes arguments after the source path. With the explicit `run` command, `--` separates compiler options from program arguments:

```text
quidra app.qui input.png --count 5 --verbose
quidra run app.qui -- input.png --count 5 --verbose
```

### file

The `file` namespace provides three distinct nominal stream types, `file.read`,
`file.write` and `file.append`. Each type has a same-named factory function:
`file.read(path)` returns `file.read | error`, `file.write(path)`
creates or truncates a file and returns `file.write | error`, and
`file.append(path)` opens a file for appending, creating it if needed, and
returns `file.append | error`. These three types are not interchangeable;
in particular, a `file.append` value is not assignable to `file.write`.

The qualified type and function have separate syntactic roles. A stream may be
declared and initialized with `file.read f := file.read(path)`, or after a definite-init declaration
(`file.read f` followed by `f = file.read(path)`). The direct form
performs the same fallible factory call and fails fast when the destination
requires a success value. In a `T | error` destination, the error is preserved
instead. An uninitialized stream cannot be used before assignment.

```quidra
f := file.read("input.txt")
while .f(string &line)
    println("{line}")

numbers := file.read("numbers.txt")
while .numbers(int &value)
    println("{value}")

pairs := file.read("pairs.txt")
first := 0
while .pairs("{&first} {int &second}")
    println("{first} + {second} = {first + second}")

file.write output
output = file.write("output.txt")
.output("Hello{NL}")
.output(42)
.output("{NL}")
.output(true)

file.append log
log = file.append("output.txt")
.log("{NL}Appended")
```

File stream instances support the mutable call operator, written
`.stream(...)`. The `file.read` operator takes a writable `T &value` where `T` is
`string`, `bool`, or a numeric scalar. It always consumes one complete
UTF-8 line without the terminating LF and returns `bool | error`:
`true` for a successfully decoded value, `false` only for EOF, and
`error` for I/O or conversion failure. For `string`, the entire line
is returned unchanged; for numeric types, the complete line is parsed
with the corresponding `T(text)` rules, including range checks; for
`bool`, only `true` and `false` are accepted. A final line lacking
LF is returned, and an empty line is valid for `string` but fails
conversion to a numeric type or `bool`. A line such as `10 20` is
not two integer reads: it is a conversion error for `int`.
Alternatively, `.stream("{&first} {int &second}")` uses precisely
the same complete-line input format and atomic update algorithm as `scan`.
The separator is literal, the first target must be initialized before
the call when failure or EOF can continue, and the new `second` binding
is available only in the successful `if` / `while` body. On EOF the
format is not evaluated and none of its targets change. Invalid fields
or unmatched delimiters return `error`, never `false`. Neither a
single-target nor a formatted read modifies an output on EOF or error.
`file.write` and `file.append`
operators accept a `string`, `bool`, or numeric scalar and return
`void | error`. Numeric and boolean values are rendered using their
standard `string(value)` representation (such as `42`, `2.5`, or
`true`), then written as UTF-8 text. Context-free numeric literals use the general `int`, exact `real`, and exact `com` defaults here as elsewhere. The writer uses normal string interpolation, including numeric
format options and control-character expressions, exactly like `print`.
It does not add spaces, separators, or newlines: `.output(10)`
followed by `.output(20)` writes `1020` unless a delimiter is supplied. The two writing
modes share their call contract but remain separate types. Text validates UTF-8 and
rejects embedded NUL as ordinary Quidra strings do. Streams close
deterministically at the end of their owned lifetime. Copies preserve
the opened resource identity and maintain independent logical positions.

As with other fallible expressions, a stream error fails fast when consumed
in a success-only condition, assignment or expression statement. In
`while .f(string &line)` or `while .f(int &number)`, EOF ends
normally; I/O or parsing error does not masquerade as EOF. An inline `T &name` introduces fresh writable storage in a boolean
`if` / `while` call condition. The binding is visible only on the
true branch / in the loop body, and is initialized only if every true-result
path is proven to write it. The reader operator also provides this guarantee for every typed
placeholder of a formatted read; a user-defined boolean function may
prove the same contract.

The `file` namespace also provides `exists`, `is_directory`, `remove`,
`copy`, `move`, `mkdir`, `list`, and byte-oriented whole-file
`read_bin` / `write_bin`. The binary functions preserve raw byte-aligned
`bin` data; arbitrary bytes never become `string` implicitly.
`file.list(path)` is nonrecursive, deterministic and lexicographically
sorted. Operational failures return typed errors. Bare `file` as a type,
`file.Reader`, `file.Writer`, `file.Handle`, `file.open()`,
`read_line()`, `next()`, `lines()`, `.f[]`, whole-text
`file.read(path)` and two-argument `file.write(path, text)` are removed.

### environment

`environment.get(name)` returns `string | none`; an unset variable is normal absence and yields `none`. `environment.has(name)` returns `bool`. Retrieved strings are copied into runtime-owned immutable text so an already returned value cannot change if the host environment later changes. Because this API has no error case, a host value that is not valid UTF-8 text causes a deterministic runtime text failure instead of creating an invalid `string`.

### test

`test.check(condition)` takes a `bool` value argument, including an integer converted under the ordinary call rule. `test.equal(actual, expected)` requires identical static types and follows the same equality availability and class definite-initialization requirements as ordinary `==`.

A failed assertion exits with status 1. This is a test failure, not a runtime-safety violation, so it does not use the runtime-safety status 101.

### time

`time` represents elapsed-time concepts with opaque value types rather than unitless integers:

```quidra
start := time.now()
pause := time.seconds(0.01)
time.sleep(pause)
elapsed := time.since(start)
println(elapsed.seconds())
```

`time.now()` returns a monotonic `time.Instant`. By default, `time.now()` and `time.since(start)` only sample that clock and do not synchronize GPU work. Both accept the optional named `sync = bool` argument, whose default is `false`. `time.now(sync = true)` waits for outstanding Quidra GPU work before taking the start sample, so earlier GPU work is excluded from the interval. `time.since(start, sync = true)` waits for outstanding Quidra GPU work before taking the end sample, so completion of asynchronous GPU work submitted during the interval is included. `time.seconds(value)` constructs a duration from any finite integer or real kind, including a negative value. It explicitly converts the value internally to `real64`, using the ordinary range and certified-rounding checks; failure terminates with the ordinary numeric-conversion reason. Complex values are rejected because complex-to-scalar conversion is forbidden; `time.sleep(duration)` requires a nonnegative duration and otherwise terminates as a runtime-safety failure with status 101. `Duration.seconds()` exposes the duration as `real64` when numeric computation is explicitly desired. Standard-library time values cannot be constructed through their class names, and their internal representation is not source-visible.

### random

`random` has no implicit process-global generator. Randomness is explicit state:

```quidra
rng := random.generator(seed = 42)
n := .rng.int(1, 10)
x := .rng.real64()
b := .rng.bool()
```

`random.generator(seed)` accepts an `int` seed and deterministically creates a generator. `Generator.int(start, end)` accepts arbitrary-precision `int` bounds and returns an `int` sampled uniformly from the half-open integer range `[start, end)` using rejection sampling; `start >= end` is a runtime-safety failure with status 101. `Generator.real64()` yields a `real64` in `[0.0, 1.0)`; `Generator.bool()` yields a boolean. Calls advance only that generator's state. Ordinary assignment copies generator state according to normal class value semantics, so later advancement of one copy cannot mutate another. Standard-library generator values cannot be directly constructed and their state representation is not source-visible.

### process

`process.run` launches an executable directly with an explicit string-array argument vector:

```quidra
result := process.run("git", ["status", "--short"])
println(result.started)
println(result.status)
println(result.output)
println(result.error)
```

No shell is inserted between the program and its arguments. `process.shell(command)` is the explicit alternative when shell syntax such as pipelines, redirections, or shell expansion is required. It invokes `/bin/sh -c` on POSIX systems and `cmd.exe /S /C` on Windows, returning the same `process.Result`. The command is interpreted by that shell, so untrusted values should be passed as structured arguments to `process.run` rather than concatenated into a shell command. Quidra string interpolation is processed first; literal braces in a shell command are therefore written as `{{` and `}}`. `Result.started` is false when the executable or shell could not be launched; in that case `status` is -1 and `error` describes the launch failure. If the process starts, `status` is its exit code, or `128 + signal` when it terminates by signal on POSIX systems. `output` and `error` capture stdout and stderr independently. They are text fields, so captured streams must be valid UTF-8 and contain no embedded NUL; arbitrary binary child output is not silently coerced into a `string` and causes a deterministic runtime text failure. This separates launch failure from an ordinary nonzero child exit without treating child exit codes as Quidra runtime failures. `process.Result` is compiler-provided and cannot be directly constructed. `process.exit(status)` terminates the current Quidra program with the supplied `int` status. It is deliberately namespaced so adding process functionality does not consume another bare user identifier.

### map

`map<K, V>` is an explicit generic value container. A bare declaration creates an empty map without a constructor call:

```quidra
map<string, int> counts
.counts.set("apple", 2)
.counts.set("banana", 1)
.counts.set("apple", 3)

println(counts.has("apple"))
value := counts.get("apple")
keys := counts.keys()
```

`increment(key, by = 1)` is available when a Map's value type is an integer or natural type. It adds to an existing value or inserts `by` when the key is absent; it is invoked as `.counts.increment(key)` or `.counts.increment(key, by = amount)`. The operation preserves insertion order and the existing type's checked arithmetic. It does not exist on Maps with non-integer values and is not an atomic concurrent increment.

`set(key, value)` inserts or replaces without changing an existing key's position. `remove(key)` removes the key and returns whether it was present; reinserting a removed key places it at the end of insertion order. `get(key)` returns `V | none`; `has(key)` returns `bool`; `size()` returns `nat`; `keys()` and `values()` return value arrays in stable insertion order. Keys are restricted to integer types (fixed-width, `int` and `nat`), `bool`, `string`, and recursively nested tuples of these types. IEEE floating-point keys are intentionally excluded because NaN would make equality and hashing ambiguous; `real` keys are excluded because symbolic equality may require bounded proof. The native implementation uses deterministic open addressing for average O(1) `has`, `get`, and `set` lookup while retaining separate insertion-order arrays for `keys()` and `values()`. Hash buckets and capacity are implementation details and are not observable language semantics. The explicit `map<K, V>()` constructor is also available. The legacy `map.Map<K, V>` spelling is rejected; `map<K, V>` is the public map type.

### Output lines

`print(value)` outputs without an implicit line terminator; `println(value)`
outputs the same value followed by one `NL` (LF), and `println()` outputs
only `NL`. No flushing is implicit. Both print forms return `void | error` and
perform fail-fast output error handling when used as statements. The newline
is part of one output operation for error checking and REPL replay suppression.
`println` has the same printable types and contextual `T | error` consumption
as `print`. On writable `file.write` and `file.append` streams,
`.stream.line(value)` uses the same scalar conversion as `.stream(value)` and
then writes one LF; `.stream.line()` writes one LF. Its return type is
`void | error`; the leading mutation marker remains mandatory, and existing
write/append positions and contents are preserved. `file.read` has no `.line`.
`NL` interpolation and `print(NL)` remain valid and unchanged.

### distinct

`distinct<T>` is the corresponding unique-value container. A bare declaration creates an empty set:

```quidra
distinct<string> tags
.tags.add("compiler")
.tags.add("ai")
.tags.add("compiler")
println(tags.size())
println(tags.has("ai"))
```

`add(value)` preserves the first insertion position. `remove(value)` removes the value and returns whether it was present; adding it again places it at the end of insertion order. `has(value)` tests membership, `size()` returns the number of unique values as `nat`, and `values()` returns insertion order. `union(other)`, `intersection(other)`, `difference(other)`, and `symmetric_difference(other)` return independent sets without changing either input. Results retain the left operand's insertion order, followed by newly admitted right-side values in the right operand's insertion order for union and symmetric difference. Elements use the same key-domain restriction as `map`. The implementation uses the same deterministic open-addressing index, giving average O(1) membership and insertion while preserving stable insertion order. Map/distinct internals are compiler-generated and are not source-visible. Assignment follows ordinary independent value semantics rather than sharing mutable container identity. The explicit `distinct<T>()` constructor is also available. The old `set<T>` and `set.Set<T>` spellings are no longer supported; `set` is no longer reserved, so a user-defined `set(...)` function may coexist with `distinct<T>` and the map method `.counts.set(...)`.


### json

`json.parse(text)` returns `json.Value | error`. `json.Value` is an immutable standard value whose runtime representation is not source-visible.

The value methods are:

- `kind() -> string`: one of `"null"`, `"bool"`, `"number"`, `"string"`, `"array"`, or `"object"`.
- `size() -> nat | error`: array length or object member count; other kinds return `error`.
- `get(key) -> json.Value | none | error`: object lookup. Missing keys are `none`; calling it on a non-object returns `error`.
- `at(index) -> json.Value | none | error`: array lookup with a `nat` index. Out-of-range is `none`; calling it on a non-array returns `error`.
- `text() -> string | error`, `integer() -> int | error`, `number() -> real64 | error`, `real() -> real | error`, and `boolean() -> bool | error`: explicit typed extraction.
- `encode() -> string`: compact JSON serialization preserving object insertion order and the original JSON number spelling.
- `equal(other) -> bool`: recursive structural equality. Object member order does not affect equality. JSON numbers compare by parsed numeric value.

JSON `null` is not Quidra `none`. A present JSON null is a `json.Value` with kind `"null"`; `none` is reserved for missing lookup results. Duplicate object keys are rejected during parsing rather than silently overwritten. Parsing rejects malformed numbers, malformed escapes, unmatched surrogate escapes, trailing content, and nesting deeper than 256 levels. Integer extraction accepts JSON numbers written in integer syntax as arbitrary-precision `int`. For exponent or fractional spellings, use `number()` for a range-checked `real64` interpretation or `real()` for the exact numeric value.

Ordinary source-level `==` is intentionally not defined for `json.Value`; use `equal` so JSON comparison semantics remain explicit.


### http

`http.get(url)` returns `http.Response | error`.

`http.Response` exposes:

- `status: int`: the HTTP response status code.
- `body: bin`: the complete response body as byte-aligned raw binary data.
- `header(name) -> string | none`: the first matching response header, using ASCII case-insensitive field-name comparison. Absence is `none`. Header values are exposed as Quidra text, so a returned value must be valid UTF-8 without embedded NUL; malformed host text fails deterministically rather than entering `string` storage.

HTTP status is not transport success. Any response received through HTTP, including 4xx and 5xx, is represented as `Response`. DNS resolution failure, connection failure, TLS verification failure, unsupported protocol, redirect failure, or timeout returns `error`.

The native implementation uses libcurl directly in the runtime. It permits only HTTP and HTTPS URLs and redirects, follows at most ten redirects, enables content decoding supported by libcurl, uses a 5-second connection timeout and a 30-second total timeout, and leaves normal TLS certificate and hostname verification enabled. The runtime buffers the complete response body in memory. The body is deliberately `bin`; text decoding is not implicit.

`http.Response` contains immutable internal header metadata which is not source-visible. Ordinary assignment preserves independent observable value behavior; header metadata may be shared because it is immutable. Source-level `==` is not defined for `http.Response`.


### Numerical packages

Core owns the language, tensor storage and layout, device execution, basic operators, [numerical reductions](#numerical-reductions), autograd graph machinery, and generic package extension mechanisms.

The first-party `math` package owns tensor unary functions such as `math.sqrt(value)`, shape-preserving row reductions, and linear algebra such as `math.dot(a, b)` and `math.matmul(a, b)`. The [matrix multiplication operator](#matrix-multiplication-operator) is Core syntax for the Math operation. Math owns its numerical semantics, autograd, native and backend implementations, and compiler optimization policy. Core supplies the package-neutral mechanisms those implementations use.

### First-party domain packages

Collections, text processing, binary encoding, neural-network, image, and video
semantics beyond the Core foundations are provided by explicit packages, not
Core standard namespaces. These packages use the ordinary package and, where
needed, native-extension mechanisms. The first-party dependency layers are:
Layer 1 Core; Layer 2 Math / Collections / Text / Encoding; Layer 3 NN / Vision /
Video; and Layer 4 DNN. Layer numbers constrain dependency
direction; they do not imply that every higher-layer package depends on every
lower-layer package. DNN currently depends on Core, Math, and NN, not on Vision
or Video.

- `import collections` provides reusable priority queues and frequency counting,
  complementing the built-in `map<K, V>` and `distinct<T>` containers.
- `import text` provides text operations such as ASCII case conversion,
  tokenization, CSV parsing, and URL query decoding.
- `import encoding` provides binary-to-text transformations such as Base64;
  it is separate from the Core `bin` and `string` representations.
- `import nn` owns architecture-independent neural-network mechanisms such as
  parameters/state, reusable layers, losses, optimizers, schedulers, training
  semantics, NN-native kernels, and NN-owned compiler rewrites.
- `import vision` provides image decoding/writing and image-processing
  operations. Vision owns its codec dependencies, native C++ implementation,
  autograd composition where applicable, and backend/compiler optimizations.
- `import video` provides video opening/decoding through `video.open` and
  `video.Reader`. Video owns FFmpeg integration and all video-specific native
  implementation.
- `import dnn` is the upper composition layer for concrete deep-neural-network
  model families. Its current manifest composes NN and Math on Core; a future
  model may additionally use Vision or Video only after DNN explicitly declares
  that dependency.
- Future signal-processing semantics likewise belong in a package rather than
  becoming a Core namespace.

Core does not link NN vendor backends, image codecs, or FFmpeg on behalf of
these packages and does not acquire their domain semantics merely because they
operate on tensors or use GPU/native acceleration.

## Function values and union payload initialization

Quidra has explicit capture-free typed function values. In `func<R(P...)>`, the angle brackets enclose the result type `R` and the parenthesized parameter types. Write `func<R()>` for a function with no parameters.

The compiler accepts the previous `func<R>(P...)` spelling and the legacy `fn` alias for source compatibility. Type displays use `func<R(P...)>`.

```quidra
int twice(int value)
    return value * 2

int apply(func<int(int)> operation, int value)
    return operation(value)

func<int(int)> operation := twice
println(apply(operation, 21))
```

A function declaration becomes a value when its context supplies a complete
`func<R(P...)>` signature. At a generic call with a declared `func` parameter,
the signature of a named non-generic function can also determine the call's
type arguments. For example, `apply(twice, 21)` can pass the function directly
when `apply` takes a `func<R(T)>`. A standalone `operation := twice`
still has no function-value context. Conversion requires an exact result and
parameter match and represents only by-value parameters. Extern C declarations
and bound methods are excluded. Function-value calls use positional by-value
arguments only.

The value contains only the statically known code target: it captures no local, receiver, top-level, or hidden environment. Ordinary assignment/pass/return therefore does not create a closure allocation or hidden lifetime. Function identity is not part of source semantics, so `==` and `!=` are not defined for function values. Calling an uninitialized function binding is a definite-initialization error. A function value stored behind an explicit reference still follows the ordinary reference read-before-use rules.

A function value stored in a class member is read by its direct member name.
To call it, first copy it into an explicitly typed local `func` binding, then call
that binding. Direct field-call syntax is rejected with `NOT_CALLABLE`, including
calls through the fixed instance name; it never selects a header argument or an
unrelated function with the same spelling. Ordinary method calls are unchanged.

### Compile-time field collection

`reflect.collect<T>(value) -> T[]` recursively walks the statically known public
class fields of a class value and returns copies of every field whose exact type is
`T`. Traversal is resolved at compile time; there is no runtime type metadata
lookup, string-based field access, or access to private representation. Recursive
class cycles are cut by nominal type while descending.

Only storage paths that can actually lead to a collected `T` are read requirements.
Unrelated fields may remain uninitialized. A collected class value itself must be
fully initialized, including its private representation, because the result is an
ordinary value copy. Inside a function or method taking a reference parameter,
these requirements become part of that reference's normal storage-effect contract
and are checked at the call site.

This is a general structural facility rather than a model- or framework-specific
hook. Collecting a value copies it; its reference fields keep referring to the same
targets.

`reflect.paths<T>(value) -> string[]` performs the same public-field traversal
and produces one structural path for every value that `reflect.collect<T>(value)`
would return, in exactly the same order. Class fields use dot-separated names.
Array traversal includes the runtime index from the ordinary generated traversal,
so matching elements report paths such as `blocks[0].weight` and
`blocks[1].weight`. The index is produced by compiler-generated code from the
statically known array traversal; this does not introduce runtime RTTI or
string-based field lookup.

`reflect.type_name(value) -> string` returns the statically resolved nominal
class type name. Both metadata operations are generated from compile-time class
structure; they do not add runtime type metadata and never expose private fields.

### Structural traversal in place

`reflect.each<T>(&root)` is a for-header source: write
`for: T &value := &reflect.each<T>(&root)[i]`. The value form,
`for value := reflect.each<T>(root)[]`, reads the entry value. The root is
evaluated once. The index is a `nat` counter over all visits. `continue` moves
to the next visit and `break` leaves the entire traversal.

The static root type determines one visit plan. A value whose exact type is T
is visited without entering it. Classes use field declaration order, depth
first; arrays use increasing indices; tuples use increasing positions.
Reference fields, compiler fields beginning with `$`, and standard-library
classes are leaves. A standard-library leaf of type T is still visited. The
origin of a class, rather than its name, determines this rule. A union,
map/distinct or recursive value structure that may contain T is `TRAVERSAL`, because
it has no finite supported visit plan. An unrelated structure yields no visits.

Private fields are included. A user walk may enter its own module's private
fields; a private field containing T in another module is `TRAVERSAL`. Use that
module's traversal function. Package walks and backward may enter private
fields. A const field that may contain T is `TRAVERSAL` for a writable walk and
for `reflect.each_paths`; the value form may read it. Reference fields are never
entered.

The body may write the current visit. It may not directly replace the root or
any strict prefix of a visit: this is `WRITE_CAPABILITY`. A possible replacement
through a call gets a `FOR_ITERATION` check at that statement. The active site
stays pinned until its address is checked again; array levels also keep their
entry identity and length. A root reached through a reference stays at its
entry target even if that reference is rebound. A value walk uses the entry
snapshot when its body may write the source.

`reflect.each_paths<T>(root) -> string[]` lists the writable plan's visits in
exactly the same order. The root path is `""`; fields append `.name` (without an
initial dot), arrays append `[k]`, and tuple elements append `.k`. Private field
names are their declared names. This format is stable for checkpoint keys.
`reflect.collect` and `reflect.paths` keep their public-field collection rule.

A traversal is not a runtime value. It cannot be stored, placed in an array or
tuple, passed as an argument, exported to C, or converted to a function value.
A function may name a traversal with a complete `reflect.Each<E>` result:

```quidra
reflect.Each<Parameter<T>> parameters<T, M>(M &model)
    return reflect.each<Parameter<T>>(&model)
```

Its body is exactly one return of a traversal, and calls among such functions
cannot recurse. The body is checked once per instance in its declaring module,
with ordinary name hygiene. At a for header, arguments are evaluated once,
left to right, and bound to hidden locals or references. The checked returned
traversal is then lowered over those bindings. No function or runtime traversal
object is emitted. Uninferred type arguments can follow from the loop's declared
element type; with an inferred binding, an existing variable or a pattern they must be written.

Backward uses the same plan with `autograd.Target` as its target type. Its
ordinary destination list retains the model's own Targets; it never substitutes
cloned handles. Thus backward and a writable traversal share the same order,
private-field rules and supported structures.

### Parallel blocks

Each direct statement of a `parallel` suite is one node. A compound statement,
including a `for` loop, is one sequential node. Earlier writes establish a
compiler-built dependency when a later node reads that storage. The block joins
all started nodes before continuing; bindings declared inside the block are
visible to later nodes there and are out of scope after the block.

```quidra
int first
int second
parallel
    first = 20
    second = 22
println(first + second)
```

Unordered nodes may not make overlapping writes or read storage another node
writes. Shared-state aliases also conflict. Standard output, standard input and
standard error are single writable resources: two unordered calls that use the
same one are `PARALLEL_CONFLICT`. Resource accesses never introduce dependency
edges; put operations that need ordered I/O in one compound node or outside the
block. Two uses of the same file Handle conflict. Distinct handles may run
concurrently, including handles for one file; the runtime serializes each write
but does not fix their order.

Path-based file reads, queries and opens are shared; file mutations are exclusive.
A handle read is shared against a path-based access, and a handle write is
exclusive. Process execution is exclusive for process, files, network and stdin.
HTTP requests are exclusive network accesses. Clock readings are exclusive;
sleeps are shared. Environment queries are shared. `time.seconds` and
`Duration.seconds` have no resource effect. Foreign I/O performed by an `extern`
function is invisible to the checker. That and the distinct-handle rule are
limits of the worker-count guarantee.

Independent nodes all finish, even after another node fails. Dependents of a
failed node never start. The representative failure or process exit is the
first failing node in source order, independent of completion order and
`QUIDRA_CPU_THREADS`. The report includes the worker's call path, its node body
boundary and the joining caller's path. A node may use `try` when the enclosing
function can return an error; the joined error propagates through that function.
`return`, `break` and `continue` cannot leave a node (`PARALLEL_CONTROL`).

Nodes and nested blocks share the same persistent worker pool with package
`qcore_parallel_for` calls. The joining thread helps execute ready work, so
nested blocks do not add threads or deadlock waiting for their own pool.
`QUIDRA_CPU_THREADS` limits the shared pool; one runs serially. A node runs on a
worker only when its captures are proven safe for thread-local ownership:
scalar storage or managed values accessed solely through borrows, with no
retain, release, pin, unpin or COW operation reaching captured managed storage.
Other nodes run on the joining thread in deterministic topological order.

### Parallel iterations

`parallel for` makes each iteration an independent parallel task. It reuses
threads from the shared CPU pool rather than creating a thread for every value.
The iteration order is unspecified; all iterations join before the following
statement runs. A plain `for` inside a `parallel` suite remains one sequential
node, which differs from a `parallel for`.

```quidra
int[5] a
parallel for: x < 5
    a[x] = x
for value := a[]
    println(value)
```

Unlike an ordinary untyped range header, which uses an existing binding,
`parallel for: x < 5` declares a fresh loop-local `int x`. An already visible
binding named `x` is rejected; an explicit type may instead declare the range
variable.

Integer range bounds and the step are evaluated once, left to right, before
scheduling. Ascending and descending forms, independent inclusive/exclusive
ends, explicit starts and integer steps have the ordinary `for` value sequence.
Only the exact iteration count is checked into the scheduler's signed 64-bit
limit; arbitrary-precision range values are not truncated. An excessive count
fails before the body starts. Empty ranges run no body. Real ranges are initially
rejected with `PARALLEL_FOR` because the iteration mapping needs a rounding proof.

Array value headers, including multiple-array zip headers, evaluate each source
once from left to right and immediately take its value snapshot. All lengths
must match before scheduling (`FOR_LENGTH` on mismatch); equal empty arrays run
zero iterations. The initial form requires newly declared scalar value bindings
without reference, pattern, structural, or index bindings.

A shared write must target a direct array variable at `a[loop_variable]` in an
integer range. Each output has one syntactic write per iteration and may not also
be read, aliased or updated by a compound assignment. The parent prepares its
independent array storage before workers start; workers write disjoint elements
and synchronize initialization marks, including marks sharing one bitmap byte.
When the output range crosses zero, negative and nonnegative indices can
resolve to the same element, so the iterations run in range order on the
joining thread.
An output holds a fixed primitive scalar, or the unchanged inline `int`/`nat`
iteration value. Previously boxed integer output elements are explicitly
rejected. Iteration-local scalar calculation and proven pure scalar calls are
supported; unknown effects, shared aliases, managed aggregates and resource calls
are rejected with `PARALLEL_FOR` rather than silently becoming an ordinary loop.

As in explicit parallel blocks, ownership determines worker eligibility. Inline
integer and fixed scalar calculation with prepared array storage may run on pool
workers. Boxed integer mapping, arbitrary-precision arithmetic, inner ordinary
loops and pure function calls run as independent nodes on the joining owner
thread so retained values remain in their owning thread. This preserves task,
failure and cleanup semantics; it does not promise simultaneous execution of
ownership-restricted iterations.

`continue` ends the current iteration. `break` and `return` may not leave the
parallel group (`PARALLEL_FOR`); `break` inside an inner ordinary loop is allowed.
Every independent iteration finishes even if another fails, including deferred
checks and owned-value cleanup. The representative failure or exit has the
lowest logical iteration ordinal, independent of completion order or worker
count. Shared writes performed before a failure remain visible; the group is not
a transaction. Scheduler storage is bounded by the configured worker count,
not by the number of iterations.

`task` and `atomic` are ordinary identifiers. Their former namespaces and APIs
are unavailable; an undeclared use receives `UNKNOWN_NAME`.

Class values preserve field-level definite initialization while they remain class-typed. Converting a class value into a union closes that hidden initialization state: the class must be fully definitely initialized first. Therefore a class alternative selected by exhaustive `match` is known to be fully initialized, including when the union crossed a function boundary.

For arrays whose type has a statically known length, a constant index outside `[0, length)` is rejected at compile time (`Index 4 out of bounds for length 4.`). Dynamic arrays remain bounds-checked at runtime.

## Native execution cost model

Generated native code is optimized with Clang `-O3` without fast-math. Runtime safety remains explicit: checked arithmetic, bounds checks, ownership, and recursion protection are not disabled for speed.

Direct calls remain statically resolved. Function-value calls lower through an explicit indirect-call IR node whose signature was already checked. The backend identifies direct recursive cycles and also treats any function whose address becomes a function value as a possible indirect recursion target; only those functions receive dynamic call-depth bookkeeping. Ordinary acyclic direct calls do not pay that fixed overhead.

An `int` or `nat` whose value lies within [-2^62, 2^62 - 1] is held inline in one word, and its arithmetic, comparisons and conversions run without allocation; a larger value is boxed and shared. Where the compiler proves the operands and result inline (loop counters within a length, literals, values of narrow fixed-width types), the word arithmetic needs no test at all. An exact `real` whose value is a rational with a 64-bit numerator and denominator is held inline in two words, in lowest terms, and its arithmetic, comparisons and conversions from fixed-width integers run without allocation; a larger rational or a symbolic value is a runtime node, interned by structure and shared. The representation is internal: values, text and failures do not depend on it.

Managed ownership bookkeeping is local to the executing thread in the current runtime. The language does not currently expose cross-thread sharing of managed values, so retain/release does not require a process-wide mutex. If a future concurrency model introduces shared managed values, that ownership contract must be revisited explicitly rather than silently changing this assumption.


Writable named arguments expose both sides of the reference contract:

```text
update(&state = &state)
```

The left `&state` identifies the writable parameter and the right `&state` forms the storage address. `state = &state` is invalid.

## Explicit numeric conversion

Use `T(value)` to explicitly convert between numeric types, with `T` as the destination scalar numeric type:

- for a scalar, it converts that scalar;
- for a numeric array of any nesting, it preserves every fixed/dynamic array dimension and recursively converts numeric leaves;
- for a tensor, it preserves rank/shape facts and converts the element type.

For example, `real64(values)` maps `int[][]` to `real64[][]`, while `real32(image)` maps `tensor<nat8>{3, _, _}` to `tensor<real32>{3, _, _}`. A container cast is a whole-value operation: every source leaf is read, so an uninitialized element fails deterministically, and no partial converted value is observable.

The admissibility rule is exactly the scalar rule applied to every leaf: integer-to-integer is allowed when the runtime value is in range and never wraps; integer-to-floating point is allowed with destination IEEE-754 rounding; floating-point to floating-point may reduce precision deterministically; real-to-integer conversion is allowed only for a mathematically integral value in the destination range. A fractional leaf returns `NOT_INTEGRAL`, a value beyond the destination range returns `OUT_OF_RANGE`, and an exact value whose integrality cannot be certified returns `UNDECIDED`. Every leaf is validated before the converted container becomes visible. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` when an integer rounding operation is intended. They reject non-finite and out-of-range results deterministically. `math.round` rounds halfway cases away from zero.

Container syntax is not duplicated at the cast site. Numeric representation conversion always uses the scalar destination form `T(value)`; tensor type syntax remains reserved for tensor storage construction.


## Tensor autograd and training state

Quidra has one numeric tensor value type: `tensor<T>`. Floating tensors are
untracked by default. `.track()` starts a dynamic computation graph,
`.untrack()` disconnects the value from its graph, and `.retrack()` cuts the
previous graph and starts a new tracked root. Tracking is runtime metadata and
does not change the source type or its optional shape contract.

Tracked and untracked tensors may participate in the same operation. If any
operand is tracked, the result is tracked; untracked operands are constants for
that graph. Core does not expose mathematical tensor methods such as `.abs()`,
`.exp()`, `.log()`, or `.sqrt()`. Import the first-party `math` package and
use `math.abs(value)`, `math.exp(value)`, `math.log(value)`, or
`math.sqrt(value)`; Math owns their semantics and differentiation rules.

A tensor operation must never silently discard tracking metadata. `.reshape()`
and `.transpose()` are graph-preserving tensor views and participate in
autograd. Operations that currently have no autograd transform for their
structural/device change (`.contiguous()`, tensor indexing/slicing, and
`.gpu()`/`.cpu()`) reject tracked inputs; call `.untrack()` explicitly when
the graph is intentionally cut. Assignment through a tracked tensor or tracked
alias is likewise rejected.

`loss.backward(&target, ...)` performs reverse-mode autodiff and accumulates
only into the writable gradient destinations named at that call site. At least
one destination is required. A direct floating tensor target uses that tensor's
logical autograd identity, not its physical/COW storage identity; ordinary
tensor assignment therefore never makes another tensor's `.grad`,
`.has_grad()`, or `.clear_grad()` state change. Repeated backward calls add to
existing destination gradients. Clear them explicitly when a fresh accumulation
window is required. A single backward call treats repeated references to the
same underlying gradient destination as one destination rather than multiplying
that gradient. A final `track = true` option, for example
`loss.backward(&x, track = true)`, keeps the backward computation tracked where
the primitive supports higher-order differentiation. Floating casts between real32 and real64 preserve that backward graph. A graph containing real16 or real16b does not support `track = true`, including when a cast widens its final loss to real32 or real64.

`autograd.Target` is the generic Core mechanism for an abstraction to own a
stable explicit gradient destination. Copying a Target makes a fresh destination
with a fresh graph identity, the same dtype, and a value copy of any pending
gradient. Later accumulation or clearing on either copy leaves the other
unchanged. Leaves tracked before the copy still name the original destination;
use `&` to share that destination. `tensor.track(&target)` binds a new graph
leaf to that destination without making ordinary tensor copies share gradient
state. Core has no Parameter, State, model, layer, optimizer, training-mode, or
optimizer-state type. The official `nn` package defines `nn.Parameter<T>` as
an ordinary source class whose private `autograd.Target` owns its gradient state;
a copied Parameter owns an independent destination and pending gradient;
`nn.State<T>` has no such target. NN owns parameter traversal, reusable layers,
losses, optimizers, schedulers, update equations, optimizer state, and generic
neural-network training semantics. The upper `dnn` package currently composes
NN and Math into concrete deep-neural-network model families. Vision and Video
remain independent Layer-3 packages unless a concrete higher-layer model
explicitly declares them as dependencies.



### Package management

Unquoted non-standard imports resolve installed source packages from configured
`QUIDRA_PACKAGE_PATH` roots and then the default per-user store
`~/.quidra/packages/<name>/main.qui`.

Released packages contain `quidra.package`, including an exact package version
and a `requires.quidra` range. `quidra install PACKAGE` examines only exact
stable release tags named `vMAJOR.MINOR.PATCH`; branches are never install
identities. With no version specified, the newest released tag compatible with
the running Quidra compiler is selected. `quidra install PACKAGE@X.Y.Z`
requires that exact release and rejects it when its Quidra compatibility range
does not match. Remote package installation uses Git but never executes package
install scripts.

A local source directory can be installed with `quidra install DIR`; legacy
local packages without a manifest remain usable for development. Existing local
installs require explicit `--force` replacement. `quidra remove`,
`quidra list`, and `quidra package-path` manage the default store.

`quidra lock FILE.qui` resolves the direct and transitive installed-package
import graph and writes sorted `quidra-lock-v3` entries containing distribution
name, import name, package version (or `-` for an unversioned local package),
and a SHA-256 over the complete regular-file tree excluding `.git` metadata. When the
lockfile exists, normal compilation requires the same package graph, declared
version, and content hash. `--check` verifies the current resolution without
rewriting the lockfile.

The full package/release metadata contract is documented in
`docs/packages.md`.

### C FFI

A deliberately narrow C ABI boundary is available for functions with scalar/`void` results and optional borrowed text/binary inputs:

```quidra
extern int64 c_abs(int64 value) = "llabs"
println(c_abs(-42))
```

The declaration is top-level only and binds a Quidra function name to an explicit C symbol. Results are restricted to `void` or ABI-stable by-value scalars: all fixed-width signed/unsigned integer types, `real32`, `real64`, and `bool`; the arbitrary-precision `int` and `nat` are rejected (`FFI_TYPE`), so a C `int64_t` is spelled `int64`. Scalar and bool parameters are also by value. Capture-free Quidra function values may cross as explicit C callback pointers when the `func` signature uses only `int32`, `nat32`, `int64`, `nat64`, `real32`, and `real64` value parameters and the same scalar set or `void` as its result. The narrower callback scalar set deliberately excludes 8/16-bit integers and `bool` so callback ABI extension rules cannot become platform-dependent hidden behavior. A callback is only a code pointer: it has no capture environment or bound receiver and therefore no hidden allocation or captured-object lifetime. Managed buffers cross the boundary only through explicit call-scoped borrows: `const string name` is a read-only UTF-8 span, `const bin name` is a read-only byte span, and `bin &name` is a mutable byte span. A `const` borrow is a value parameter like any other `const` parameter: the call passes a value, `c_text(name)` or `c_text(prefix + name)`, and the compiler lends C the storage of a variable, field or array element directly and holds any other value for the duration of the call. `bin &name` is passed as `&storage`. Mutable `string &` is forbidden so foreign code cannot violate Quidra's UTF-8 invariant. Each borrowed parameter expands at the C ABI boundary to two adjacent C parameters: a non-null data pointer followed by a `nat64` byte length. For `string`, the bytes are the existing validated UTF-8 representation; for `bin`, only the payload after Quidra's private length header is exposed, and only when its bit length is byte-aligned. The adjacent length is the payload byte count, so foreign code cannot resize the Quidra value or access its ownership metadata. No encoding conversion is performed and the foreign contract does not depend on NUL termination. A `const` borrow may not be mutated; a mutable `bin &` borrow may mutate only bytes inside the supplied length. Neither form may retain the pointer after the call. Quidra `bin` has independent value storage, so ordinary value copies remain independent even when one copy is later passed mutably to C. This deliberately means a one-pointer C-string API such as `puts(const char*)` is not directly compatible with a Quidra `string` parameter; use a small C wrapper with an explicit pointer+length signature instead. LLVM marks borrowed pointers `nocapture nonnull`; read-only `string`/`bin` borrows additionally carry `readonly`. Integer ABI extension contracts are explicit: signed 8/16-bit values use `signext`, unsigned 8/16-bit values use `zeroext`, and `bool` uses `zeroext`, on both declarations and call sites. Managed buffers by value without `const`, scalar references, default arguments, generics, managed-value results, other array forms, tensor values, unions, and classes remain rejected. Foreign failure is never inferred from `errno`, a null pointer, or ownership convention: expose an explicit scalar status/result and handle it in Quidra. A `real32` or `real64` result is a value C code made, so it is tested where it enters: NaN stops the program at the call with `NONFINITE_RESULT` (`extern 'f' produced a NaN real64 value`); signed infinities cross unchanged. C symbols must be ordinary C identifiers. The generated entrypoint `main`, the compiler-owned `n_*` function-mangling namespace, the implementation-owned `quidra_*` / `__quidra_*` symbol namespaces, and symbols already owned by the generated runtime prelude cannot be rebound through `extern`; use a distinct C wrapper symbol instead. A C symbol may be bound by only one source `extern` declaration per compilation; duplicate aliases are rejected during checking rather than producing conflicting LLVM declarations. External calls are treated as potentially effectful by the REPL, so accumulated-source replay never silently re-executes them. The core declaration does not load libraries or run foreign initialization code. Foreign object, archive, shared-library, or import-library files are linked only when explicitly named with repeatable `--link FILE` options on `quidra build`, `quidra run`, or `quidra debug`; each path must name an existing regular file. There is no implicit package linker script, library search, or build-time network access.

### C ABI export

`export "C"` is the outbound direction of the C boundary, as `extern` is the inbound one: it marks a top-level function as callable from C under its own name.

```quidra
export "C" int32 add2(int32 a, int32 b)
    return a + b

println(add2(2, 3))
// output: 5
```

The prefix applies only to a top-level, non-generic free function, written before its prototype or definition; methods, constructors, generic functions and declarations inside blocks cannot be exported (`FFI_EXPORT`; a generic function is exported through a concrete wrapper). When the function has a prototype, the prototype and the definition both carry the prefix. The ABI string is `"C"`. `export` is contextual: it begins a declaration only at top level and when a string literal follows it, and remains an ordinary identifier everywhere else. An exported function is an ordinary Quidra function too: Quidra code calls it, and it can be a function value.

Exported values cross the boundary by value and only as fixed-width scalars with a defined C mapping: parameters are `int8`, `int16`, `int32`, `int64`, `nat8`, `nat16`, `nat32`, `nat64`, `real32` or `real64`, which C sees as `int8_t`..`int64_t`, `uint8_t`..`uint64_t`, `float` and `double`, and the result is one of them or `void`. The arbitrary-precision `int` and `nat` have no fixed C representation and are rejected (write `int64` or `nat64`), as are `real`, `bool`, `string`, `bin`, arrays, tensors, classes, enums, unions (`error` and `none` included) and `func` values (`FFI_TYPE`). Parameters may be `const` values; reference parameters and default arguments are rejected (`FFI_REFERENCE`, `FFI_DEFAULT`). A `real32` or `real64` argument from C may be infinite but not NaN: the entry point checks before the function runs, and NaN stops the program with `NUMERIC_CONVERSION` (`numeric conversion failed: non-finite value cannot be represented as real64`) at the function's declaration.

The C symbol is the function's declared name, without the module namespace of an imported module, so exports are unique per compilation. It may not be a C keyword or a reserved C identifier (`_X...`, `__...`; `FFI_SYMBOL`), a name the compiler and runtime own (`main`, `n_*`, `quidra_*`, `__quidra_*` and the runtime prelude's C library functions), a symbol another module exports, or a symbol an `extern` declaration binds (`FFI_SYMBOL_CONFLICT`). Clashes with other C library names are not checked. The REPL cannot export functions.

The compiler gives each exported function a C entry point under its C symbol, which calls the Quidra function; Quidra code calls the function itself and never passes through the entry point, and the optimizer inlines a small body into it. Every build emits the entry points, so C code linked into a program with `--link` on `quidra build`, `quidra run` or `quidra debug` can call exported functions, also while the program calls C (exported function, `extern` C function, exported function again). Narrow integer results reach C extended to a full register, as C compilers expect.

No Quidra failure crosses into C. The exported types include no `error`, so no error value is returned. A runtime failure inside an exported function ends the process where it happens, exactly as in an executable: the canonical report on stderr, naming the Quidra file, line and column, then status 101 through `exit`, so the host's `atexit` handlers run; nothing unwinds through C frames. An exported function that may leave device work behind (tensor or autograd work, a `parallel` block, or a call to C) waits for the deferred device checks still pending before it returns, so a failing check is reported at the boundary rather than later in unrelated host code, and a package encode left open is reported as `GPU_SCOPE`; other exported functions return with no extra work. Host threads may call exported functions concurrently, with the guarantees of concurrently running parallel nodes: exported functions share no Quidra state, the call-depth limit counts per thread, and the runtime initializes its services on first use.

`quidra build FILE.qui --lib` builds a static library for a C program instead of an executable: `-o` names the archive (default `libFILE.a`, `FILE.lib` on Windows), which holds the compiled module, the package native code the executable link would compile, and the Quidra runtime, so the host needs only system libraries. `--print-link-flags` prints them on stdout, from the same options as the executable link (on macOS `-lc++ -lm -pthread` and the Foundation, Metal and MetalPerformanceShaders frameworks, on Linux `-lstdc++ -lm -pthread -ldl`, plus the libraries of pkg-config, CUDA and HTTP where the program uses them). The root of a library is checked like an imported module: it holds declarations, immutable `const` bindings and `if main` guards, which never run, since a library has no entry point; other top-level statements and a `cli` declaration are rejected (`LIBRARY_TOP_LEVEL`), and a library must export at least one function (`FFI_EXPORT`). The archive is merged by `libtool -static` on macOS, `ar` with an MRI script on Linux and `lib.exe` on Windows (`QUIDRA_LIBTOOL` and `QUIDRA_AR` name other programs). Only one Quidra library can be linked into a host program: two would define the same runtime symbols. Shared libraries are not built yet.

```c
#include <stdint.h>
#include <stdio.h>

int32_t add2(int32_t a, int32_t b);

int main(void) {
    printf("%d\n", add2(2, 3));
    return 0;
}
```

```sh
quidra build add.qui --lib -o libadd.a --print-link-flags > flags.txt
cc main.c libadd.a $(cat flags.txt) -o main
```

The compiler may relate inferred tensor extents across values, including the
broadcast of unknown extents. These are internal value facts, rather than source
types: distinct inferred symbols do not imply different sizes. Shape checks
still validate those values at runtime. Declaration shape variables name the
related extents in source.

## Tuples

A tuple is a fixed product of at least two value types. `(int, real64) pair := (10, 0.5)` materializes each literal in its element type. `(T)` is a grouped type and `(x)` a grouped expression; `()` and trailing commas are errors. `pair := (count, score)` takes the types of already typed elements. Numeric tuple elements without a resolved context use the ordinary `int`, `real`, or `com` defaults.

Tuple assignment copies its elements with their ordinary value semantics. Tuples may occur in arrays, fields, generic arguments, function parameters and results, and union alternatives. They contain values, including tensors and classes with reference fields, but their element types never contain references. Tuples have no positional indexing or members, no ordering, and cannot cross the C boundary or serve as tensor elements.

A destructuring pattern either declares every bound name or assigns existing
places. It can nest, and `_` discards a value without declaring a name:
`(count, _) := measure()` or `((a, b), c) := nested()`, inferring each element type.
`(a, b) = (b, a)` evaluates both input values before writing either place.
A standalone existing-place `=` assignment can omit the outer parentheses
around its target pattern, its right-hand list of values, or both. For example,
`a, b = b, a % b` uses the same evaluation order as `(a, b) = (b, a % b)`.
Parentheses group any nested tuple or pattern.
All target index operands are captured once, left to right, before the source;
the writes then resolve each target's current storage in pattern order.
`(&a, &b) := &pair` binds the elements of initialized writable storage and infers their exact target types.
Tuple types themselves always hold values, never references.

A declaration pattern names each element, e.g. `(count, score) := measure()`, and `_` discards a position. Nested patterns destructure nested tuples; a tuple type followed by a name binds that whole element. Existing-place patterns assign after the entire right side is evaluated: `(a, b) = (b, a)` swaps. Target indices and slice bounds are captured left to right before the right side; each write resolves the current target storage. A declaration pattern cannot mix new names and existing places.

In a destructuring declaration, an outer pattern made of inferred value names and `_` can omit its parentheses, as in `count, score := measure()`. The outer parentheses around a comma-separated initializer can also be omitted, as in `n, steps, largest := start, 0, start`. The initializer is evaluated left to right before any of the new names enter scope. An element with an explicit type, `const`, or `&` requires a parenthesized pattern, as does a nested pattern.

`(&a, &b) := &pair` declares inferred true references to initialized tuple storage. Every bound element has `&`; `_` needs none. Calls, literals and slices cannot provide reference storage. Array element references keep the array's existing pin and shape rules.

Collection headers accept the same patterns: `for: (count, score) := pairs[i]`, `for (a, b) = pairs[]`, and `for (&count, &score) := &pairs[]`. Each iteration binds only the selected elements; reference patterns check that the element is initialized. An index is a new read-only `nat` binding with `for:`; plain `for` reuses an existing mutable `nat` binding.

Tuple equality compares corresponding elements and stops at the first difference. Every element must support bool-valued equality. Printable tuples contain printable scalar values or nested tuples. Strings inside their text are quoted, including strings reached through a union alternative: `(10, "a, b")`. Quotes use `{DQ}`, braces are doubled, and named controls use `{NL}`, `{HT}`, `{CR}`, `{BS}`, `{FF}`, `{VT}` and `{BL}`. Backslashes and non-ASCII characters remain themselves. Printing a string alone keeps its ordinary unquoted behavior. Tuple formatting suffixes are rejected.

`map` keys and `distinct` elements accept recursively nested tuples composed of integer, natural, bool and string values. Hashing combines the existing element hashes in order with `h = (h * 1000003 + child_hash) % 2147483647`, without building tuple text.

A tuple pattern in `match` selects one exact tuple alternative of a union. Typed leaves select the alternative; `_` discards a position. Ambiguous and impossible patterns are errors. Bindings are local to the case, may be `const`, and never use references. `(int count, string name)` is a pattern; `(int, string) pair` is a whole-type case. A parenthesized group that scans as a type and is followed by a name, array suffix, `|`, or the line end is a type case, including `(int, real64)[3] values`. Enum payloads are still bound whole with `Variant(name)`.

A mutable local reference may start unbound: `int &r`. Bind it with
`&r = &value` before reading or writing its target, or using `&r`.
A definitely unbound access is `UNBOUND_REFERENCE`; an access after a
conditional binding checks the address at runtime unless every path binds it.
An unbound declaration resets the reference each time it executes, including
in a loop body. An inferred reference, `&r := &storage`, requires an initializer to determine its type.
Reference parameters are lent by the caller and cannot be rebound.

### Array generation expressions

`[for x := values[] if x > 0 then x * x]` creates a new Array of the selected values. The `if` condition is optional; `then` is required. The bracketed expression may span multiple lines. Declare one element binding with its exact source element type or omit the type to infer it.

The source Array is evaluated once and held as a value snapshot. Elements are visited in their original order. Each condition is evaluated once per element, and the value following `then` is evaluated once only when its condition is true. Without a condition, every element contributes one value. Execution is sequential.

The binding exists only inside the expression. Like an ordinary Array value loop, it is an element copy that a writable call argument may change; changing that copy does not change the source element. Ordinary calls, mutation markers and `try` retain their usual rules. Source, selection and value errors stop evaluation and use ordinary owned-value cleanup.

The result element type is checked even when the source is empty. Numeric literal results use a compatible concrete context or the ordinary `int`, `real`, or `com` default. An empty input produces an empty Array of the checked result element type.

This form accepts one Array source and value iteration. Reference bindings, range sources, index bindings, structural traversals and multiple sources are rejected explicitly. Nested generation expressions remain ordinary nested evaluation.

### Native tensor array borrows

Native tensor array borrows use only one dynamic dimension:
`const tensor<T>[] values` or `tensor<T>[] &values`, with no element shape
contract in the extern signature. They cross as one
call-scoped opaque view pointer. Packages use `qcore_tensor_array_count`,
`qcore_tensor_array_element_const` and `qcore_tensor_array_element`; array
headers, element storage and ownership metadata are never public ABI. A view
and the element handles must not be retained or queried from a parallel-for
worker. The compiler rejects nested/fixed/non-tensor array extern declarations.
Core checks initialization, dtype, bounds and writable authority before exposing
an element, refuses tracked mutable tensors, and detaches shared outer storage
using the ordinary typed copy. A tensor's mutable data/device accessor preserves
its normal COW and tracking rules. The view is invalidated at return. Core
validates every writable floating tensor after an unflagged extern, and skips
this fallback only for a registered self-validating or finite-preserving op.

