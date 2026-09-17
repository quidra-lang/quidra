# Quidra LLM Guide

## Canonical generation rules

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
11. Named arguments use `name = value`, follow positional arguments, and occur at most once per parameter. Functions and methods are callable declarations, not first-class values; do not assign a bare function name to a binding.
12. `&storage` means the safe address of existing storage. It is not a numeric pointer. Do not invent `*`, pointer arithmetic, address-to-int conversion, or `is`.
13. Storage access is explicit at function boundaries: declare `int[] &values` for read/write access or `const int[] &values` for read-only access, and call either form with `process(&data)` or a named `&values = &data`. The callee's parameter decides whether that access path may write.
14. Create local aliases with `T &alias = &storage` for read/write access or `const T &view = &storage` for a live read-only view. A const reference may observe changes made through another writable path, but it cannot write, rebind, or be used to recover a writable reference.
15. `const T value = expression` creates an immutable value binding; `const auto` may infer `T`, including `const auto &view = &storage`. const value bindings and const references require initialization. On by-value function parameters, const is local to the callee; on reference parameters, `T &` versus `const T &` is part of the caller-visible authority contract. Taking an address does not itself read the stored value. Do not create addresses of temporaries. Do not store references in class fields or arrays.
16. A reference to a field or array element keeps that captured substorage alive even if the containing binding is later replaced. Reference aliasing is allowed, including passing the same storage to multiple reference parameters; const constrains only the access path carrying it.
17. Ordinary arguments have value semantics. Reference arguments share the addressed storage. `T &` carries read/write authority; `const T &` carries read authority only. `&` is therefore storage access, while `const` removes write authority from that path.
18. Iterate using `for value in values`, `for &value in values`, or `for i in range(start, stop, step = 2)` as appropriate. Use `continue` to advance the nearest loop and `break` to leave it; both are invalid outside loops.
19. Put required parameters before defaulted parameters. Type every default parameter. Defaults are fresh per omitted call and cannot reference parameters/caller locals. Reference parameters have no defaults.
20. Conditions are booleans. Combine them with `and`, `or`, and `not`; do not invent `&&`, `||`, `!`, or truthiness coercions. Numeric operands have matching types. Built-in numeric types are `int8`, `int16`, `int32`, `int`/`int64`, `uint8`, `uint16`, `uint32`, `uint64`, `float32`, and `float`/`float64`; there is no `char`.
21. Allow implicit numeric conversion only when every source-type value is exactly representable by the destination. Explicit `T(value)` conversion is practical: integer narrowing is range-checked, integer-to-float and float-to-float may deterministically round, and float-to-integer is not a generic cast. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` when converting floating-point values to integer. Never use a cast to request wrap or clamp.
22. Parse text with a numeric type method such as `int.parse(text)` and convert scalar values to text with `value.string()`. Parsing and casting are different operations. Float text is shortest-round-trip decimal while retaining `.0` when an otherwise integral-looking representation needs to remain visibly floating-point.
23. Use `print(value)` for output with a newline, `write(value)` for output without one, and `input()` for `string | none | error`. Use string interpolation such as `"answer: {value}"`. Numeric interpolation may use only `int=N`, `frac=N`, `sig=N`, and `zero`, for example `"{value:int=4,frac=2,zero}"`. Do not combine `frac` with `sig`, and do not use `zero` without `int`.
24. Quidra has no backslash escapes. Write backslashes literally and use `enter`, `tab`, `home`, `quote`, `backspace`, `page`, `vtab`, and `bell` for LF, HT, CR, quote, BS, FF, VT, and BEL.
25. Reserved identifiers are absolute in user code: never reuse one as a binding, parameter, function, class, field, method, generic parameter, loop/match binder, CLI field, or import alias. Qualification is not an exception, so a user-defined `object.math` is invalid when `math` is reserved. For non-reserved names, never shadow a declaration that is currently visible; reuse is allowed only in disjoint scopes. Prefer adding standard APIs beneath existing reserved namespaces or as value methods rather than creating new bare global built-ins.
26. Define user types with `class`. Construction may initialize any subset of fields by name. Uninitialized fields may exist but must not be read.
27. A class field may declare a default, such as `int retries = 3`. An explicit construction argument overrides it; omitted defaults are fresh for each construction.
28. Ordinary `=` has independent value semantics. A later write to one copy cannot change another. The implementation may share storage or use copy-on-write only when the sharing is unobservable; explicit `&` is the observable alias mechanism. Class copies preserve per-field initialization state.
29. Methods access fields directly; do not invent `self` or `this`. Field read/write requirements are inferred from the method body.
30. Use `class Child : Parent` for single inheritance and `override` for inherited method replacement. Parent and child remain distinct types; use a union when either type is possible.
31. Inside a subclass method, use `super.method(...)` to call the statically resolved parent implementation. Do not treat `super` as a value.
32. Class, array, and `bytes` `==` / `!=` are value comparisons, not identity comparisons. Class equality requires all compared fields to be definitely initialized.
33. Put imports at top level. Standard namespaces are already visible and must not be imported. Use `import module = "./file.qui"` for importer-relative files, `import module = "@/file.qui"` for command-root files, and `import plot = plotting` for an installed package. Access imported declarations through the alias.
34. Imported module files contain declarations only. Do not put executable top-level statements in an imported file, do not create import cycles, and do not rely on a nested import being re-exported.
35. Declare generics with `class Box<T>`, `T first<T>(T[] values)`, or `T method<T>(T value)`. Generic class construction remains explicit (`Box<int>`). For generic functions and methods, omit type arguments when every generic parameter is uniquely determined by the call arguments (`first(values)`, `object.method(value)`); otherwise provide them explicitly.
36. Generic bodies are monomorphized and checked after substitution. Do not assume an operation is valid for every `T`; it must be valid for each concrete instantiation actually used.
37. Generic classes may inherit instantiated generic parents. Use `super.method<T>(...)` when an overriding generic method needs the direct parent implementation.
38. Top-level statements are the entrypoint. Choose descriptive function names for reusable computations.
39. Format one-line lists without a trailing comma and multiline lists with one.
40. Use `bytes()`, `bytes(n)`, or `bytes(n, fill = value)` for mutable raw binary data. A bytes element is `uint8`; indexing, `&data[i]`, value/writable iteration, `len`, and value equality are supported.
41. Treat `string` as immutable UTF-8 text and `bytes` as mutable binary data. `len(string)`, `text[index]`, `find`, and `slice` use Unicode code-point positions; string indexing returns a one-code-point `string`, not a byte or `char`. String methods include `contains`, `starts_with`, `ends_with`, `find`, `slice`, `trim`, `split`, `utf8`, and `codepoints`. Use `text.utf8()` when byte-oriented processing is intended and `text.codepoints()` when Unicode scalar values are intended. A `string[]` can use `values.join(separator)` for efficient assembly. Do not invent a `char` or `byte` scalar alias; use `uint8` for one byte.
42. Use `while condition` for condition-controlled repetition. The condition must be `bool`; `break` and `continue` target the nearest loop. Prefer `for` when iterating an existing range or collection and `while` when progress is state- or convergence-driven.
43. For a command-line program, declare exactly one top-level `cli name` block; the `cli` namespace is already available and is not imported. Inside the block, use typed fields initialized with `argument()`, `option(default = value)`, or `flag()`; read parsed values as fields of that binding. The binding is not implicitly visible inside functions because functions do not capture top-level bindings; pass the needed CLI-derived values explicitly.
44. Method selection is static. Inheritance provides code reuse and explicit `override`, not dynamic dispatch through a parent-typed value. When a value may be one of several concrete types, use a union and exhaustive `match`.
45. Standard namespaces are always visible and cannot be imported or aliased: `math`, `cli`, `file`, `environment`, `test`, `time`, `random`, `process`, `map`, `set`, `json`, `http`, `tensor`, `stats`, `linear`, `signal`, `vision`, and `neural`. Unquoted non-standard imports are installed packages and never fall back to local files. Use `file.is_directory(path)` to distinguish directories from files. Use `file.list(path)` for deterministic direct-child enumeration or `file.list(path, recursive = true)` for deterministic recursive enumeration; both list forms return sorted paths as `string[] | error`.

46. Use `array(n)` only with an explicit array type context when intentionally creating uninitialized elements. Use `array(n, fill = value)` for a fully initialized array; `auto x = array(n)` is invalid because the element type is unknown.
47. Use `tensor<T>(shape)` for intentionally uninitialized dense numeric tensor storage, and `tensor.zeros<T>(shape)` or `tensor.ones<T>(shape)` for initialized storage. Tensor indexing always returns `tensor<T>`; call `.item()` only on a 0-D tensor to obtain a scalar.
48. Tensor-to-tensor implicit broadcasting requires equal rank and per-axis equality or singleton expansion. Do not rely on NumPy-style rank prepending. Use explicit `.reshape(...)` when rank must change.
49. Tensor slices may be internal views, but value semantics are preserved with copy-on-write. `.reshape(...)` requires contiguous storage and never hides a materializing copy; use `.contiguous()` explicitly. Tensor `.cast<T>()` follows explicit cast semantics: integer narrowing is range-checked, integer-to-float and float-to-float may deterministically reduce precision, and float-to-integer requires an explicit rounding operation.
50. Use `stats.mean(value)` for the current tensor mean operation. It returns `float` and requires every element to be initialized; an empty tensor is a runtime error.
51. Use `linear.dot(a, b)` for same-length rank-1 numeric tensors and `linear.matmul(a, b)` for compatible rank-2 tensors instead of manually expanding those loops unless the algorithm itself is the task. Both require matching dtypes; integer multiplication and accumulation remain overflow-checked.
52. Read images with `vision.read<uint8>(path)` and write them with `vision.write(path, image, quality = 95)`. Image tensors are CHW `tensor<uint8>`: grayscale is `[1,H,W]`, RGB is `[3,H,W]`, RGBA is `[4,H,W]`. Do not invent BGR ordering, automatic normalization, implicit dtype conversion, or silent alpha removal. PNG/JPEG/BMP/TIFF/WebP are supported; JPEG rejects RGBA.
53. `signal` is a reserved standard namespace but has no public callable API in language version 0.1; do not invent FFT or signal operations.
54. Declare C FFI only with top-level `extern` declarations. Results may be only `void` or ABI-stable scalar values. Parameters may additionally use `string` or `bytes` as call-scoped read-only borrows; each lowers to an adjacent C `(data pointer, uint64 byte length)` pair. Do not assume a pointer-only C string, implicit NUL-terminated foreign contract, encoding conversion, ownership transfer, pointer retention, or mutation. Use an explicit C wrapper when the foreign API has a different signature, and represent foreign failure explicitly with a scalar status/result rather than inferring `errno` or null semantics.

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

The current core includes fixed-width numeric types, strict implicit conversions and practical explicit casts, numeric `Type.parse`, scalar `.string()`, `print`/`write`/`input`, compact mutable bytes with explicit `file.read_bytes` / `file.write_bytes` binary I/O, initialized/uninitialized arrays, dense tensors with slicing/COW/broadcasting, `stats.mean`, rank-2 `linear.matmul`, `vision` image I/O, file modules with explicit namespaces, monomorphized generics with unambiguous function/method type inference, partially initialized user-defined classes, field defaults, field-level definite initialization, static `super` calls, class/array/bytes value equality, safe storage addresses, rebindable explicit references, fields and methods, unions, and single inheritance between distinct class types.

## Output and tensor initialization

- `print` and `write` accept scalar/text values, not whole arrays or tensors. Print elements explicitly or format them intentionally.
- `tensor<T>(shape)` creates uninitialized elements; initialize scalar elements with indexed assignment, or use `tensor.zeros<T>(shape)` / `tensor.ones<T>(shape)` when a fully initialized tensor is intended.


### Writable arguments and explicit conversion

For writable named arguments write `&name = &value`; positional writable arguments remain `&value`.

Implicit numeric conversion is lossless-only. Explicit integer-to-float and float-to-float conversion may round; integer narrowing is range checked. Do not generically cast float to integer: choose `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

54. Neural scalar arithmetic uses the neural element dtype. Numeric literals may be used directly only when the literal value is exactly representable in that dtype; otherwise cast explicitly to the intended floating type. Use `neural.training` and `neural.inference` for neural execution mode. Their static types are distinct (`neural.Training` / `neural.Inference`), allowing generic forward code to specialize statically. Do not invent a hidden model-wide train/eval flag.

55. Use `neural.Conv2D(input = ..., output = ..., kernel = ..., stride = 1, padding = 0)` for core NCHW convolution. It supports tensor inference and neural training, including backward for input, weight, and bias. Do not assume groups, dilation, or model-zoo APIs in the standard core; those belong in extensions.

56. Construct optimizers without a model reference: `neural.SGD(rate = ...)` or `neural.Adam(rate = ...)`. Update only with `neural.step(&model, &optimizer, gradients)`; do not invent `.grad`, `zero_grad()`, or hidden Parameter registration. A zero-Parameter model with empty Gradients is a no-op and does not advance optimizer State; non-empty foreign Gradients still fail. Foreign/stale Gradients and existing Adam moment structure are validated before any update, so a failing step does not partially mutate model or optimizer State; Adam's global call counter is committed only after successful Parameter updates.

57. Save neural state with `neural.save(model, path = "x.quistate")` or include an optimizer as the second object. Restore only into existing matching storage with `neural.load(&model = &model, ...)`; add `&optimizer = &optimizer` when the file includes optimizer state. Do not invent a checkpoint API.
58. `.quistate` loading is exact and prevalidated before mutation: schema, field order/path/type, tensor dtype and shape, format version, bounds/trailing data, and checksum must agree. A rejected load does not partially restore earlier fields; never assume partial or name-only restoration.

59. `neural` means `neural<float32>`. Use `neural.track(tensor)` to enter the dynamic graph and `.untrack()` to return to ordinary tensor storage. Do not invent hidden `.grad`, gradient accumulation, `zero_grad()`, or a Module base class.

60. Softmax requires a non-empty last axis. MSE and BCE mean reductions require at least one element. Cross entropy accepts logits `[N,C]` with `N > 0`, `C > 0`, and a `tensor<int>` target of exact shape `[N]`; do not flatten or reshape other target ranks implicitly. Binary cross entropy accepts probabilities, not logits. Keep prediction and target values finite and within `[0,1]`; do not rely on implicit clipping of invalid values.

61. `neural.BatchNorm` uses axis 1 as feature/channel, so it applies naturally to both `[N,F]` and NCHW `[N,C,H,W]`. Training updates running State; inference is read-only. `neural.Dropout` owns local RNG State and inference does not advance it.

62. A `.quistate` file matches exact nominal root model/optimizer types as well as the complete structural leaf schema, tensor dtype/shape, version, and checksum.

63. Do not assign to or index-write `Parameter.value`. Treat Parameter storage as persistent learnable identity; update it only through `neural.step` or restore it through `neural.load`.

64. Do not take a writable reference to `Parameter.value`; Parameter storage is not user-writable even through an alias.
65. Use only `float32` or `float` (`float64`) as the element type of `neural.Parameter<T>`, `neural.Linear<T>`, `neural.Conv2D<T>`, and `neural.BatchNorm<T>`. `neural.State<T>` is intentionally generic and may hold non-floating persistent state.

66. For `extern` C declarations, pass numeric/bool scalars by value. Expose text or binary input only as `const string &` / `const bytes &` and call it with `&storage`; each such borrow lowers to a read-only no-capture data pointer plus an explicit `uint64` byte length for that call. Do not invent pointer-only C strings, implicit encoding conversion, foreign retention/mutation, hidden ownership transfer, or inferred `errno`/null failure.
