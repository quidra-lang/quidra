# Language design candidates

These are proposed language changes, not current specification.

> Candidates are temporary tracking notes. Remove each entry once the underlying issue or design question is resolved.

## Uniform elementwise array and tensor comparisons

**Status:** Candidate

- Comparison operators `==`, `!=`, `<`, `<=`, `>`, and `>=` use one elementwise rule for arrays and tensors. This follows the same general container model as elementwise arithmetic: applying a scalar comparison to a container lifts that comparison over corresponding elements instead of changing the operator into a whole-value predicate.
- Scalar comparisons still return one `bool`. Array comparisons return a boolean array with the same recursive shape, and tensor comparisons return a same-shaped boolean tensor.
- Array comparison is recursive through every nesting level. Corresponding leaves are compared only when the two arrays have exactly the same recursive shape: every corresponding array level must have the same length. Equal total leaf counts are not enough, and comparisons never flatten, truncate, pad, or broadcast nested arrays.
- Tensor comparison likewise requires exactly equal rank and extents. Comparison operators do not broadcast tensor operands. A statically provable shape mismatch is rejected during checking; a mismatch that depends on runtime extents fails deterministically at runtime.
- `!=` is the elementwise logical negation of `==` at each corresponding leaf. It is not a whole-container inequality predicate.
- Whole-container questions are explicit reductions over the boolean result:
  - `(a == b).all()` means every corresponding element is equal.
  - `(a != b).any()` means at least one corresponding element differs.
  - `(a < b).all()` means every corresponding element is less.
  - `(a < b).any()` means at least one corresponding element is less.
- `all()` and `any()` recurse through every boolean leaf of nested arrays rather than reducing only one level. The same conceptual reduction applies to every tensor element.
- Empty reductions use the logical identity values: `empty.all()` is `true` and `empty.any()` is `false`. Therefore an entirely leaf-empty nested boolean array also reduces to `true` for `all()` and `false` for `any()`.
- This keeps the semantic boundary simple: operators perform elementwise work; reductions answer questions about the container as a whole. No separate `===`, `.==`, or `eq/ne/lt/le/gt/ge` comparison surface is needed.

```quidra
int[][] a = [[1, 2], [3]]
int[][] b = [[1, 5], [3]]

bool[][] mask = a == b
// [[true, false], [true]]

bool same = (a == b).all()        // false
bool different = (a != b).any()   // true
```

The following arrays are not comparable because their recursive shapes differ even though both contain three integer leaves:

```quidra
int[][] a = [[1, 2], [3]]
int[][] b = [[1], [2, 3]]

auto mask = a == b // shape mismatch
```

## Uniform fallible-operation error channel with default fail-fast

**Status:** Candidate

- Treat failure as a language-wide operation property rather than something users must remember only for selected APIs. A source-level operation that can fail because of runtime values or external conditions exposes an `error` channel by default. This includes ordinary fallible APIs and should extend consistently to fallible conversions and other runtime-checked operations instead of giving them a separate termination-only model.
- The default user experience remains lightweight: when a context consumes only the successful value, Quidra automatically handles the `error` alternative by reporting it and terminating at that consumption site. Users do not need to add `try` merely because an operation is fallible.

```quidra
int8 narrowed = int8(value)       // success -> int8; error -> fail-fast
file.Handle handle = file.open(path) // success -> handle; error -> fail-fast
```

- A caller explicitly preserves the failure channel by requesting a type that includes `error`, or by matching the operation directly.

```quidra
int8 | error narrowed = int8(value)
file.Handle | error opened = file.open(path)

match int8(value)
    int8 narrowed
        print(narrowed)
    error problem
        print(problem)
```

- `try expression` means specifically: do not fail fast here; if the operation produces `error`, propagate that error out of the current compatible function. It is not a mandatory marker on every fallible call.
- A compatible `T | error` destination or return context may preserve the error directly; `try` is needed when the surrounding expression wants the successful value while errors must leave the current function early.
- `auto` infers only the normal-success type of a fallible operation. It does not silently turn ordinary code into error-holding code. Any unhandled `error` therefore follows the default fail-fast rule.

```quidra
auto narrowed = int8(value)       // int8; error fails fast
auto handle = file.open(path)     // file.Handle; error fails fast
```

- `auto | error` is the explicit counterpart when the caller wants to infer the normal-success type but preserve the failure channel. It is a dedicated failure-channel form, not a general rule that allows `auto` to participate in arbitrary unions.

```quidra
auto | error narrowed = int8(value)   // int8 | error
auto | error handle = file.open(path) // file.Handle | error
```

- `auto | error` preserves all ordinary successful alternatives inferred from the expression and adds the operation's `error` channel. For example, if the successful result is `string | none`, then `auto | error line = handle.read_line()` has type `string | none | error`.
- Forms such as `auto | none` are not introduced by this proposal. `none` already belongs to the ordinary success type and is inferred by plain `auto`; the special syntax exists only to opt into retaining the failure channel without spelling the successful type.

- Normal absence is not failure. `none` remains part of the ordinary value type and therefore remains visible through `auto`. For an operation conceptually producing `T | none | error`, `auto` infers `T | none` and the `error` channel fails fast unless explicitly preserved, matched, or propagated.

```quidra
auto line = handle.read_line()    // string | none; error fails fast
```

- Fallible explicit conversions follow the same rule as every other fallible operation. For example, narrowing `int8(value)` can expose `int8 | error` when the caller wants to recover from an out-of-range value, while ordinary `int8 value = int8(source)` remains concise and fail-fast.
- Operations proven impossible at compile time should still be rejected statically where appropriate. Operations proven safe may have their runtime failure path optimized away without changing the source-level contract.
- The intended semantic boundary is: successful values flow normally; `none` represents normal absence; `error` is the recoverable failure channel; leaving that channel unhandled deliberately selects fail-fast behavior. No separate `must` syntax is required.

## Root-only `if main` guard for direct execution

**Status:** Candidate

- Reserve `main` as a built-in compile-time root condition rather than introducing a standalone `main` block. It may appear only in the exact top-level guard form `if main`; it is not an ordinary `bool` value, cannot be called as `main()`, cannot be assigned or passed around, and cannot be shadowed by a user-defined name.
- `main` is true only when its containing source file is the file directly executed by the Quidra CLI. When that file is imported as a module, the `if main` body is excluded from execution.
- `if main` executes at its written source position as part of ordinary root top-level execution. It is not deferred to the end of the file and it does not cause later declarations or bindings to be hoisted.
- Ordinary root top-level statements may appear before and after `if main`. When the file is executed directly, those statements continue to run in source order and the guarded body runs exactly where it appears.

```quidra
print("start")

if main
    print("direct-only")

print("finished")
```

Direct execution prints `start`, then `direct-only`, then `finished`.

- Normal source-order visibility rules still apply inside `if main`: the guarded body may use declarations and bindings that are visible above it, but it receives no special forward visibility into later bindings. This keeps values such as a `cli` binding ordinary and predictable.

```quidra
cli args
    string source = argument()

if main
    print(args.source) // valid: args is already visible
```

The reverse order does not make `args` visible merely because the condition is `main`.

- Importing a file never executes its `if main` body. A declaration-oriented file can therefore expose reusable module values, classes, and functions while keeping lightweight direct-execution checks beside them.

```quidra
int x = 10

int add(int a, int b)
    return a + b

if main
    test.equal(add(1, 2), 3)
```

Another file may import this module and access values such as `lib.x` or call `lib.add(...)` without running the guarded body.
- The existing module rule remains separate: imported files cannot gain arbitrary import-time executable top-level side effects merely because `if main` exists. A file that needs ordinary root-only executable statements before or after `if main` is a directly executed root program; a reusable imported module remains declaration-oriented, with `if main` serving only as its optional direct-execution/self-test guard.
- Primary use case: express the role of Python's direct-execution guard without a magic module-name comparison. In Quidra, `if main` means: execute this body only when this source file itself is the directly executed root, and execute it at this source position.

## Explicit top-level prototype declarations

**Status:** Candidate

- Keep Quidra's ordinary source-order visibility rule: a user-defined name is visible only after it has been declared. Do not implicitly hoist later function or class declarations.
- Add explicit top-level prototype declarations as the opt-in mechanism for forward visibility. A prototype makes only that declared name and its declared interface visible from that source position onward; it does not make unrelated later declarations visible.
- A function prototype is exactly a function signature without a body. No extra `prototype`, `declare`, or similar keyword is introduced.

```quidra
int calculate(int value)

if main
    print(calculate(10))

int calculate(int value)
    return value * 2
```

- A function prototype and its later definition must describe the same function exactly. Return type, parameter types, parameter names, reference/const qualifiers, generic parameters and constraints, and any other call-signature contract must agree. Parameter names are part of the contract because Quidra supports named arguments.
- If a function prototype exists, it is the single source of truth for default arguments. Defaults are written on the prototype only; the later definition repeats the parameter types and names but omits default expressions. This prevents duplicated call-interface metadata from drifting.

```quidra
int add(int a, int b = 1)

print(add(10))

int add(int a, int b)
    return a + b
```

- Repeating a default on the later definition is invalid even when the expression is identical, and using a different default is likewise invalid. When no prototype exists, defaults remain written normally on the full function definition.
- A prototype may appear at most once, and exactly one matching definition must exist in the same source file. A prototype is unnecessary when the full definition is already visible above the use site.
- Generic functions may be prototyped using the same signature syntax as their later definitions.

```quidra
T first<T>(T[] values)

T first<T>(T[] values)
    return values[0]
```

- A class prototype is the class header with no body, for example `class User` or `class Box<T>`. It makes the class type name available for later type references while leaving fields and methods to the single later full definition.

```quidra
class User

User create_user(string name)

class User
    string name

User create_user(string name)
    User user
    user.name = name
    return user
```

- A class prototype does not duplicate field or method declarations. The full class body remains the single source of truth for class structure.
- Generic classes may likewise be forward-declared, for example `class Box<T>`, and the later class definition must use the same generic parameter contract.
- Once a class name has been explicitly prototyped, the checker may resolve its later complete definition when validating uses of that type. This explicit forward declaration does not permit otherwise-invalid layouts or ownership structures; impossible by-value recursive layouts and other existing type errors remain compile-time errors.
- Method prototypes inside a class are intentionally not added by this proposal. Methods continue to be defined normally inside the class body. The forward-declaration surface is limited to top-level functions and top-level classes.
- A full class body is analyzed as one member namespace. All fields and methods declared in that class are visible to every method body regardless of their textual order, so methods may call later methods and access fields written later in the class without method-level prototypes.

```quidra
class Counter
    void reset()
        value = 0
        report()

    int value

    void report()
        print(value)
```

- This class-member visibility is deliberately narrower than general hoisting. Local bindings inside a method still follow ordinary source-order visibility: a local name cannot be used before its declaration. Names outside the class also remain governed by the top-level source-order/prototype rules.
- A function's own name is visible inside its body as soon as that definition begins, so ordinary self-recursion does not require a prototype. Calling a different function that is defined later still requires that later function to have been prototyped above the call site. Mutual recursion therefore uses explicit top-level prototypes only for the names that must be visible before their definitions.

```quidra
int factorial(int n)
    if n <= 1
        return 1
    return n * factorial(n - 1)

bool odd(int n)

bool even(int n)
    if n == 0
        return true
    return odd(n - 1)

bool odd(int n)
    if n == 0
        return false
    return even(n - 1)
```

- Constructors do not get a separate prototype form. Once a class name has been explicitly prototyped, a use such as `Point(...)` may resolve against the `construct` members in that class's later complete definition, including overload selection, parameter defaults, visibility such as `private`, and fallible-constructor behavior. Without a prior class declaration, the type name and therefore its constructor call remain unavailable.

```quidra
class Point

Point make()
    return Point(1.0, 2.0)

class Point
    float x
    float y

    construct(float px, float py = 0.0)
        x = px
        y = py
```

- Constructor signatures and defaults remain written only in the complete class definition. A class prototype is only the forward declaration of the type name; it is not a second place to declare constructors, fields, or methods.
- Enums do not gain a prototype form. An enum's variant list is its type definition, so the complete enum declaration must be visible before the enum type is used.
- Prototypes are strictly same-file source-order tools. A function or class prototype must be completed by its matching definition later in the same source file; prototypes cannot be placed in a separate header/interface file and cannot be satisfied by a definition in another module.
- Module imports expose the resolved declarations of the imported source, not its internal prototypes. Whether a module used prototypes to order its own source is not part of its public API and does not change import/export visibility. Quidra therefore does not introduce C/C++-style header/source separation for ordinary modules.
- An external implementation is a separate concept from a prototype. If Quidra exposes an FFI or `extern` surface, such a declaration means that the implementation lives outside Quidra source and must have its own explicit ABI contract. A bodyless ordinary function prototype never means `extern` and must not silently fall back to linker resolution.
- Ordinary top-level functions are not overloadable: one user-defined function name denotes one function interface. Methods are likewise not overloadable within a class. Different operations use different names, while behavior that is intentionally shared across types should use Quidra's generic functions or generic methods instead of an overload set.
- Constructors remain the deliberate exception. A class may declare multiple `construct` members distinguished by their call signatures, because every such call has the single semantic purpose of constructing that class value. This constructor overloading does not generalize to ordinary functions or methods.
- Function prototypes therefore never form an overload set. Once a function name is introduced by a prototype or definition, another function declaration with that same name is not a second overload; only the single matching later definition of that prototype is permitted. The same one-name/one-meaning rule keeps named arguments, defaults, generics, and forward visibility deterministic.

```quidra
int parse_text(string value)
int parse_bin(bin value)

// Not an overload set:
// int parse(string value)
// int parse(bin value)
```

- The resulting visibility model is: top-level names are downward-visible unless explicitly prototyped; class members are mutually visible throughout their class body; method-local names are downward-visible only.
- The intended rule is therefore simple: **names are visible only downward by default; write a prototype when a later top-level function or class must be visible earlier.**

## Machine-oriented structural editing and repair protocol

**Status:** Candidate

- Keep Quidra source as the single human/LLM-facing programming language. Extend the existing `inspect`, `patch`, structured diagnostics, and machine-description interfaces rather than introducing a second source syntax.
- Add a versioned structural patch schema (for example, patch schema v3) that can replace or insert a typed AST node directly as structured data, in addition to the existing source-fragment operations. Structural edits must retain the current fail-closed checks for `base_revision`, `node_id`, `expected_hash`, and `expected_kind`, validate the replacement against the expected AST role, re-run the ordinary compiler validation path, and render the accepted result back to canonical Quidra source.
- Make compiler diagnostics node-addressable wherever a source node exists. Structured parse, type, initialization, authority/effect, module, generic, and backend diagnostics should carry the relevant `node_id` / node kind plus explicitly identified related nodes when useful, so a machine client can request only the failing structure instead of re-reading the full file.
- Map runtime failures and test failures back to source nodes. Preserve enough compiler/runtime provenance for structured failures such as bounds errors, failed assertions, and other attributable runtime faults to report the originating `node_id` together with compact failure-specific data. Do not invent a node association when the failure cannot be attributed unambiguously.
- Expose a versioned canonical AST protocol, including a machine-readable AST schema and a deterministic source -> AST -> canonical source path. Add round-trip tests that require structural/semantic equivalence after canonicalization; exact original whitespace or formatting need not survive when `fmt` would canonicalize it.
- Formalize the compiler grammar export for constrained generation. Provide stable machine-readable grammar output (for example EBNF and/or structured JSON) derived from the authoritative grammar so compatible model clients can constrain syntax during generation without maintaining a separate grammar copy. This must not add aliases, redundant syntax, or LLM-only surface forms to the language.
- Treat the full repair loop as a first-class tooling contract: inspect a revision, consume structured diagnostics, patch only the addressed nodes, validate the complete result, and re-inspect after a successful revision change. Extend `quidra describe llm` / related discovery commands so clients can discover the supported schema versions and workflow mechanically.

## Implementation candidates

### Indexed array element class-field initialization false positive

**Status:** Candidate bug

- Reading a class field through an indexed array element, for example `points[1].x`, has been reported to produce `UNINITIALIZED` even when that element's field is initialized.
- Reproduce the array -> index -> class -> field path, fix definite-initialization propagation if confirmed, and add a regression test.

### File I/O and numeric parsing performance

**Status:** Candidate performance investigation

- A benchmark audit reported a file-processing case around 12.49 s and roughly 1.6 GB peak memory.
- Profile allocation and lifetime behavior around line splitting and repeated `int.parse` before changing semantics or adding a specialized fast path.
- Preserve the benchmark workload contract and verify any optimization against the same output and error behavior.
