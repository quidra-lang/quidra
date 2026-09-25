# Quidra LLM Guide

## Canonical generation rules

Core typing rule: ordinary value arguments never specialize a call's static return type from their values. Values may determine runtime behavior and compile-time facts used for diagnostics or optimization, while overload/generic resolution may depend on argument static types. An expected type may explicitly constrain a result. Do not turn a known argument value into a narrower source-visible return type.

1. Use four spaces for each block level. Dedentation closes a block. Use spaces exclusively.
2. Write explicit return and parameter types: `int add(int a, int b = 1)`.
3. Use `auto x = expression` when inference is clear. Otherwise put the complete type before the name: `int[2][3] matrix`.
4. An initializer can be omitted from a typed declaration, but every read needs definite initialization on all continuing paths.
5. Array indices refer to existing elements. `[]` is an initialized empty array; indexing it at zero fails unless it has first been replaced by a non-empty value. Constant out-of-bounds indices are compile-time errors when the array type has a statically known length; dynamic arrays remain runtime checked.
6. Create a runtime-sized filled array with `array(n, fill = 0)`. Fixed sizes are type contracts; respect all dimensions. Grow an array with `values = values.append(value)` or combine arrays with `values.concat(other)`; these return new values rather than resizing referenced storage in place. For numeric, bool, or string arrays of either fixed or runtime size, use `values.sorted()` for a non-mutating runtime-sized sorted copy instead of hand-writing a sort unless the algorithm itself is the task.
7. Use `return` for explicit control flow. Use `elif` for a same-level conditional chain instead of nesting `else` + `if`. Compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`) evaluates its target exactly once; prefer it when the read-modify-write operation is the intended meaning. A `void` function can use bare `return` or reach the end of its body.
8. Use `T | error` for failure, `T | none` for normal absence, and `T | none | error` when both are possible. Return the actual value directly.
9. `try expression` propagates only `error` from a compatible function and removes it from the expression's type.
10. Cover every alternative in `match` using `T`, `T name`, or `none`. When matching a variable, its name has the selected type inside that case. A class value must be fully definitely initialized before it enters a union, so a matched class alternative is fully initialized.
11. Named arguments use `name = value`, follow positional arguments, and occur at most once per parameter. For capture-free function values, write an explicit type such as `fn<int>(int) operation = twice`. Do not infer one with `auto`. The declaration must match the `fn` result/parameter types exactly, may not use reference parameters, and an `extern` function or bound method is not a function value. Call a named function-value binding with positional value arguments.
12. `&storage` means the safe address of existing storage. It is not a numeric pointer. Do not invent `*`, pointer arithmetic, address-to-int conversion, or `is`.
13. Storage access is explicit at function boundaries: declare `int[] &values` for read/write access or `const int[] &values` for read-only access, and call either form with `process(&data)` or a named `&values = &data`. The callee's parameter decides whether that access path may write.
14. Create local aliases with `T &alias = &storage` for read/write access or `const T &view = &storage` for a live read-only view. A const reference may observe changes made through another writable path, but it cannot write, rebind, or be used to recover a writable reference.
15. `const T value = expression` creates an immutable value binding; `const auto` may infer `T`, including `const auto &view = &storage`. const value bindings and const references require initialization. On by-value function parameters, const is local to the callee; on reference parameters, `T &` versus `const T &` is part of the caller-visible authority contract. Taking an address does not itself read the stored value. Do not create addresses of temporaries. Do not store references in class fields or arrays.
16. A reference to a field or array element keeps that captured substorage alive even if the containing binding is later replaced. Reference aliasing is allowed, including passing the same storage to multiple reference parameters; const constrains only the access path carrying it.
17. Ordinary arguments have value semantics. Reference arguments share the addressed storage. `T &` carries read/write authority; `const T &` carries read authority only. `&` is therefore storage access, while `const` removes write authority from that path.
18. Iterate using `for value in values`, `for &value in values`, or `for i in range(start, stop, step = 2)` as appropriate. Use `continue` to advance the nearest loop and `break` to leave it; both are invalid outside loops.
19. Put required parameters before defaulted parameters. Type every default parameter. Defaults are fresh per omitted call and cannot reference parameters/caller locals. Reference parameters have no defaults.
20. Conditions are booleans. Combine them with `and`, `or`, and `not`; do not invent `&&`, `||`, `!`, or truthiness coercions. Numeric operands have matching types. Built-in numeric types are `int8`, `int16`, `int32`, `int`/`int64`, `uint8`, `uint16`, `uint32`, `uint64`, `bigint`, `float32`, `float`/`float64`, and `bigreal`; there is no `char`. For fixed-width integer bit operations use uppercase `AND`, `OR`, `XOR`, unary `NOT`, `<<`, and `>>`; never apply them to bool, floats, exact numerics, bin, tensor, or neural values.
21. Never change an already-typed numeric value's representation implicitly. Numeric literals may take a contextual numeric type when representable. Explicit `T(value)` conversion is practical: integer narrowing is range-checked, integer-to-float and float-to-float may deterministically round, and float-to-integer is not a generic cast. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` when converting floating-point values to integer. Never use a cast to request wrap or clamp. Mathematical functions are namespaced only: write `math.abs`, `math.sqrt`, `math.min`, and `math.max`; the bare spellings do not exist.
22. Parse text with a numeric type method such as `int.parse(text)` and convert scalar values to text with `value.string()`. Parsing and casting are different operations. Float text is shortest-round-trip decimal while retaining `.0` when an otherwise integral-looking representation needs to remain visibly floating-point.
23. Use `print(value)` for output with a newline and `write(value)` for output without one; both return `void | error` and fail fast when written as statements. Read input with `scan`: `scan(&n)` reads one value into `n`, and `scan("{&n} {&m}")` or `scan("{&name},{&age}")` reads a line against a format whose literal text must match the input, parsing each `{&target}` by the target's numeric or `string` type. A `string` target takes the whole rest of the line. `scan` returns `void | error`; as a statement it fails fast and initializes its targets, and `void | error read = scan(...)` keeps the error, in which case the targets must already be initialized. There is no `input()`. Use string interpolation such as `"answer: {value}"`. Numeric interpolation may use only `int=N`, `frac=N`, `sig=N`, and `zero`, for example `"{value:int=4,frac=2,zero}"`. Do not combine `frac` with `sig`, and do not use `zero` without `int`.
24. Quidra has no backslash escapes. Write backslashes literally and use `ENTER`, `TAB`, `HOME`, `QUOTE`, `BACKSPACE`, `PAGE`, `VTAB`, and `BELL` for LF, HT, CR, quote, BS, FF, VT, and BEL. These eight are the only built-in values spelled in capitals; their lowercase forms are rejected.
25. Reserved identifiers are absolute in user code: never reuse one as a binding, parameter, function, class, field, method, generic parameter, loop/match binder, CLI field, or import alias. Qualification is not an exception, so a user-defined `object.math` is invalid when `math` is reserved. For non-reserved names, never shadow a declaration that is currently visible; reuse is allowed only in disjoint scopes. Prefer adding standard APIs beneath existing reserved namespaces or as value methods rather than creating new bare global built-ins.
26. Define user types with `class`. Members are public by default. Prefix a field, method, or constructor with `private` when it is class-local; private access is allowed only from methods and constructors declared by the owning class. Create a value either by declaring it without an initializer (`Point point`, then `point.x = 1.0`) or by calling a constructor the class declares: `construct(float px, float py)` is called as `Point(1.0, 2.0)` or `Point(px = 1.0, py = 2.0)`. `T(...)` is always a constructor call, `Point()` needs an explicit `construct()`, and there is no `Point(x = 1.0)` field-initializer form. A parameter cannot share a field's name. A class may declare several constructors that differ in their parameter types; an ambiguous call is an error, so cast bare literals when constructors differ only by numeric type. A constructor that can fail is `Point | error construct(...)` and must initialize every field before completing; `Point p = Point(...)` fails fast, `try Point(...)` propagates. A `const` field without a default is assigned exactly once, directly in the constructor body. Uninitialized fields may exist but must not be read.
27. A class field may declare a default, such as `int retries = 3`. Defaults are applied before a constructor body runs and when a value is declared without an initializer, fresh for each value; a constructor may then overwrite them.
28. Ordinary `=` has independent value semantics. A later write to one copy cannot change another. The implementation may share storage or use copy-on-write only when the sharing is unobservable; explicit `&` is the observable alias mechanism. Class copies preserve per-field initialization state.
29. Methods access fields directly; do not invent `self` or `this`. Field read/write requirements are inferred from the method body.
30. Classes contain only their declared fields and methods. Reuse behavior and state through explicit composition; use an explicit union and exhaustive `match` when several concrete class types are possible.
31. Class, array, and `bin` `==` / `!=` are value comparisons, not identity comparisons. Class equality requires all compared fields to be definitely initialized.
32. Put imports at top level. Standard namespaces are already visible and must not be imported. Use `import module = "./file.qui"` for importer-relative files, `import module = "@/file.qui"` for command-root files, and `import plot = plotting` for an installed package. Access imported declarations through the alias.
33. Imported module files contain declarations only. Do not put executable top-level statements in an imported file, do not create import cycles, and do not rely on a nested import being re-exported.
34. Declare generics with `class Box<T>`, `T first<T>(T[] values)`, or `T method<T>(T value)`. Generic class construction remains explicit (`Box<int>(7)` for a class with `construct(T initial)`, or `Box<int> box` followed by field assignment). For generic functions and methods, omit type arguments when every generic parameter is uniquely determined by the call arguments (`first(values)`, `object.method(value)`); otherwise provide them explicitly.
35. Generic bodies are monomorphized and checked after substitution. Do not assume an operation is valid for every `T`; it must be valid for each concrete instantiation actually used.
36. Top-level statements are the entrypoint. Choose descriptive function names for reusable computations.
37. Format one-line lists without a trailing comma and multiline lists with one.
38. Use `bin.fill(n, bit)` for allocated raw bits and `bin.parse("0101")` for written bit patterns. `bin.parse` has static type `bin | error` even for a valid literal; handle the union with `match` or `try`. A known-invalid literal may be rejected early and a known-valid literal may be optimized internally without changing that type. `bin(value)` is reserved for explicit conversion. `len(bin)` counts bits; indexing returns a one-bit `bin`, slicing returns `bin`, and value/writable iteration operates one bit at a time.
39. Treat `string` as immutable UTF-8 text and `bin` as packed raw bits. Repeat one Unicode code point with `string.repeat(value, n)` rather than overloading `string(...)`. `len(string)`, `text[index]`, `find`, and `slice` use Unicode code-point positions; string indexing returns a one-code-point `string`. `text.utf8()` returns `bin`, while `text.codepoints()` returns Unicode scalar values as `int[]`. Do not invent `char`, `bit`, `byte`, or `bytes` source types.
40. Use `while condition` for condition-controlled repetition. The condition must be `bool`; `break` and `continue` target the nearest loop. Prefer `for` when iterating an existing range or collection and `while` when progress is state- or convergence-driven.
41. For a command-line program, declare exactly one top-level `cli name` block; the `cli` namespace is already available and is not imported. Inside the block, use `argument()` for required positional fields, `argument(default = value)` for trailing optional positional fields, `option(default = value)` for named options, and `flag()` for boolean flags. Read parsed values as fields of that binding. The binding is not implicitly visible inside functions because functions do not capture top-level bindings; pass the needed CLI-derived values explicitly.
42. Method selection is static and local to the receiver's concrete class declaration. Do not invent dynamic dispatch or hidden subtype relationships.
43. Standard namespaces are always visible and cannot be imported or aliased: `math`, `io`, `cli`, `file`, `environment`, `test`, `time`, `gpu`, `task`, `atomic`, `ref`, `random`, `process`, `map`, `set`, `json`, `http`, `stats`, `linear`, `signal`, `image`, `video`, `tensor`, and `neural`. Unquoted non-standard imports are installed packages and never fall back to local files. Use `file.is_directory(path)` to distinguish directories from files. Use `file.list(path)` for deterministic direct-child enumeration or `file.list(path, recursive = true)` for deterministic recursive enumeration; both list forms return sorted paths as `string[] | error`. Use `file.open/create/append` when incremental Handle-based I/O is required; `read_line()` returns `string | none | error`.

44. Use `array(n)` only with an explicit array type context when intentionally creating uninitialized elements. Use `array(n, fill = value)` for a fully initialized array; `auto x = array(n)` is invalid because the element type is unknown.
45. Use `tensor<T>(shape)` for intentionally uninitialized dense numeric tensor storage, and `tensor.zeros<T>(shape)` or `tensor.ones<T>(shape)` for initialized storage. An expected tensor type may supply the element type even when the shape is explicit, so prefer `tensor<float32> x = tensor.zeros([2, 3])` over repeating `float32` on both sides. `auto x = tensor.zeros([2, 3])` is invalid because no element type is available. With a fully known expected exact shape, contextual `tensor.zeros()` / `tensor.ones()` may derive both dtype and extents; any `_` axis or unconstrained rank still requires an explicit shape argument. Tensor indexing always returns `tensor<T>`; call `.item()` only on a 0-D tensor to obtain a scalar.
46. Tensor-to-tensor implicit broadcasting requires equal rank and per-axis equality or singleton expansion. Do not rely on NumPy-style rank prepending. Use explicit `.reshape(...)` when rank must change.
47. Tensor slices may be internal views, but value semantics are preserved with copy-on-write. `.reshape(...)` requires contiguous storage and never hides a materializing copy; use `.contiguous()` explicitly. Write exact-rank shape contracts as `tensor<T><D0, D1, ...>`, with `_` for an existing axis whose extent is unrestricted. A shape slot or array dimension may be an integer expression such as `n * 2 + 1`; if it is not constant, evaluate it once at binding creation and treat the result as a captured constraint, never as a live reference to `n`. `float[][n * m]` therefore has a runtime-sized outer array and a captured inner extent. Use the scalar destination type as the only numeric cast syntax: `float(array)` recursively preserves array structure, `float(tensor)` preserves tensor rank/shape facts, and floating `float32(neural_value)` / `float(neural_value)` keeps the autograd graph while changing dtype. Do not invent a container-specific cast method.
48. Use `stats.mean(value)` for the current tensor mean operation. It returns `float` and requires every element to be initialized; an empty tensor is a runtime error.
49. Use `linear.dot(a, b)` for same-length rank-1 numeric tensors and `linear.matmul(a, b)` for vector-matrix, matrix-vector, or matrix-matrix multiplication instead of manually expanding those loops unless the algorithm itself is the task. Both require matching dtypes; integer multiplication and accumulation remain overflow-checked.
50. Read images with `image.read(path)` and write them with `image.write(path, image, quality = 95)`. Image tensors use CHW layout and have compiler-known rank 3: grayscale `[1,H,W]`, RGB `[3,H,W]`, RGBA `[4,H,W]`. An expected type such as `tensor<uint8><3, _, _> | error` is an acceptance constraint: it requires rank-3 uint8 RGB input/output and never authorizes conversion. Use `channel = value` only when channel conversion is intended; the value must evaluate to 1, 3, or 4. Use `type = float32` (or another numeric built-in type) only when sample-representation conversion is intended. These option values affect behavior but do not narrow an `auto` result type; write an expected tensor union explicitly when a fixed result type is required. Channel conversion may explicitly discard/add alpha; RGB-to-gray is fixed as `0.299R + 0.587G + 0.114B`. Dtype conversion does not normalize sample ranges. Do not invent implicit dtype conversion, normalization, or BGR ordering.
51. `neural<D0, D1, ...>` means `neural<float32><D0, D1, ...>`; `neural<T><D0, D1, ...>` uses an explicit floating dtype plus the same exact-rank shape pattern as tensor. Do not interpret a missing neural dtype as an unknown dtype.
52. `signal` is a reserved standard namespace but has no public callable API in language version 0.2; do not invent FFT or signal operations.
53. Declare C FFI only with top-level `extern` declarations. Results may be only `void` or ABI-stable scalar values. Parameters may additionally use `string` or `bin` as call-scoped read-only borrows; each lowers to an adjacent C `(data pointer, uint64 byte length)` pair. Do not assume a pointer-only C string, implicit NUL-terminated foreign contract, encoding conversion, ownership transfer, pointer retention, or mutation. Use an explicit C wrapper when the foreign API has a different signature, and represent foreign failure explicitly with a scalar status/result rather than inferring `errno` or null semantics.

## Complete example

```quidra
int | error value(bool valid)
    if valid
        return 21
    return error("invalid value")

int | error doubled(bool valid)
    auto x = try value(valid)
    return x * 2

void scale(int[] &values, int factor = 2)
    for &item in values
        item = item * factor

int[] values = [1, 2, 3]
scale(&values = &values)

int pending
int &alias = &pending
alias = 7

match doubled(true)
    int answer
        print(answer)
    error problem
        print(problem)
```

## Verification

Run `quidra check source.qui --json` for diagnostics, then `quidra run source.qui` to check native behavior. `quidra inspect` exposes source structure; prefer `--no-source`, `--no-effects`, `--kind KIND`, and `--depth N` when a task needs only a compact subset. Nodes include `parent_id` and `depth`, so callers do not need to reconstruct hierarchy from spans. `quidra patch` applies revision/hash-checked changes using the schema in `patch-schema.md`. Check both intended results and error paths. Preserve the explicit write contracts and fixed shapes when modifying code.

The current core includes fixed-width numeric types, no implicit representation-changing numeric conversion and practical explicit casts, numeric `Type.parse`, scalar `.string()`, `print`/`write`/`input`, packed mutable `bin` with explicit `file.read_bin` / `file.write_bin` binary I/O, initialized/uninitialized arrays, dense tensors with slicing/COW/broadcasting, `stats.mean`, vector/matrix `linear.matmul`, `image` image I/O, file modules with explicit namespaces, monomorphized generics with unambiguous function/method type inference, partially initialized user-defined classes, field defaults, field-level definite initialization, class/array/bin value equality, safe storage addresses, rebindable explicit references, fields and methods, unions, and explicit class composition.

## Output and tensor initialization

- `print` and `write` accept scalar/text values, not whole arrays or tensors. Print elements explicitly or format them intentionally.
- `tensor<T>(shape)` creates uninitialized elements; initialize scalar elements with indexed assignment, or use `tensor.zeros<T>(shape)` / `tensor.ones<T>(shape)` when a fully initialized tensor is intended.


### Writable arguments and explicit conversion

For writable named arguments write `&name = &value`; positional writable arguments remain `&value`.

Already-typed numeric values never change representation implicitly, even when the conversion would be lossless. Numeric literals may materialize directly in a unique compatible numeric context. Explicit integer-to-float and float-to-float conversion may round; integer narrowing is range checked. Do not generically cast float to integer: choose `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

54. Neural scalar arithmetic uses the neural element dtype. Numeric literals may be used directly only when the literal value is exactly representable in that dtype; otherwise cast explicitly to the intended floating type.

55. The built-in `neural` namespace is autodiff infrastructure: `neural<T>`, `track`/`untrack`, `Parameter`, `State`, `Gradients`, `grad`, persistence, operand-level math, affine/convolution/normalization/masking primitives, and safe update primitives. Use `import dnn` for layers, activations, losses, and optimizers.

56. Keep models as ordinary classes containing `neural.Parameter<T>` and `neural.State<T>`. Do not invent `.grad`, `zero_grad()`, hidden Parameter registration, or a Module base class.

57. Save neural state with `neural.save(model, path = "x.quistate")` or include an optimizer as the second object. Restore only into existing matching storage with `neural.load(&model = &model, ...)`; add `&optimizer = &optimizer` when the file includes optimizer state. Do not invent a checkpoint API.
58. `.quistate` loading is exact and prevalidated before mutation: schema, field order/path/type, tensor dtype and shape, format version, bounds/trailing data, and checksum must agree. A rejected load does not partially restore earlier fields; never assume partial or name-only restoration.

59. `neural` means `neural<float32>`. Use `neural.track(tensor)` to enter the dynamic graph and `.untrack()` to return to ordinary tensor storage. Do not invent hidden `.grad`, gradient accumulation, `zero_grad()`, or a Module base class.

60. `neural.mean`, `neural.sum_last`, and `neural.max_last` are differentiable reductions. Last-axis reductions retain shape by broadcasting the reduced value across that axis so ordinary packages can compose stable normalization expressions.

61. `neural.normalize` takes Parameter and State operands explicitly and updates running State; `neural.normalize_inference` is read-only. `neural.random_mask` takes an explicit `State<uint64>` rather than hidden global RNG state.

62. A `.quistate` file matches exact nominal root model/optimizer types as well as the complete structural leaf schema, tensor dtype/shape, version, and checksum.

63. Do not assign to or index-write `Parameter.value`. Treat Parameter storage as persistent learnable identity; update it only through `neural.update`, `neural.moment_update`, or restore it through `neural.load`.

64. Do not take a writable reference to `Parameter.value`; Parameter storage is not user-writable even through an alias.
65. Use only `float32` or `float` (`float64`) as the element type of `neural.Parameter<T>`. `neural.State<T>` is intentionally generic and may hold non-floating persistent state.

66. For `extern` C declarations, pass numeric/bool scalars by value. Expose text or binary input only as `const string &` / `const bin &` and call it with `&storage`; each such borrow lowers to a read-only no-capture data pointer plus an explicit `uint64` byte length for that call. Do not invent pointer-only C strings, implicit encoding conversion, foreign retention/mutation, hidden ownership transfer, or inferred `errno`/null failure.
