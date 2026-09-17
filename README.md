# Quidra

**Quidra — Programming with maximum meaning per token.**

Quidra is a statically typed, native general-purpose programming language designed for both humans and language models.

The name **Quidra** is derived from *quid*.

Its goal is not to minimize characters. Its goal is **semantic compression**: a small amount of syntax should communicate a large amount of reliable intent.

A Quidra program should make the important facts visible:

- what is a value and what is storage,
- who is allowed to write,
- what may be uninitialized,
- which operations can fail,
- when a conversion changes representation,
- which alternatives a value may contain,
- and which effects a call can have on existing state.

The compiler uses those facts aggressively. Ambiguity and hidden behavior are treated as costs, even when another language would consider them convenient.

```text
Quidra source
    → AST
    → module resolution
    → generic specialization
    → static checking, effect analysis, and call resolution
    → typed Quidra IR
    → LLVM IR
    → native machine code
```

A central implementation rule is that later stages do not rediscover meaning from source spelling. Module resolution and generic specialization establish concrete declarations; the checker resolves calls, conversions, storage authority, initialization facts, and observable effects; typed Quidra IR carries those decisions explicitly into LLVM lowering.

The compiler is implemented in C++20. Native code is produced through LLVM IR and Clang; normal execution does not transpile Quidra to another source language.

The supported desktop targets are Linux, macOS, and Windows. The compiler emits platform-neutral LLVM IR and uses the host Clang toolchain for native code generation; platform-specific executable discovery, process launching, runtime packaging, and filesystem replacement are isolated behind host implementations.

## The language in one example

```quidra
class Counter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

int | error parse_count(string text)
    return int.parse(text)

Counter counter = Counter()
counter.reset()
counter.increment()

int | error parsed = parse_count("41")
match parsed
    int count
        int &alias = &count
        alias = alias + counter.value
        print("count: {count}")
    error problem
        print(problem)
```

Several core ideas appear here:

- `Counter()` may initially be partial; reading an uninitialized field is rejected.
- `reset()` is understood by the checker as initializing `value`.
- ordinary assignment is value-oriented;
- `&` explicitly introduces observable access to storage, while `const` removes write authority from an access path;
- `int.parse` may fail, so failure appears in the type;
- `try` propagates only `error`;
- string interpolation exposes intent directly instead of requiring formatting boilerplate.

These are not independent features. They follow from a common semantic model.

## Core design laws

### 1. Values are the default; storage is explicit

Ordinary `=` means independent value semantics.

```quidra
int[] a = [1, 2, 3]
int[] b = a

b[0] = 9

print(a[0]) // 1
print(b[0]) // 9
```

The implementation may avoid unnecessary physical copies through immutable sharing, copy-on-write, moves, reference counting, or copy elision, but only when the difference is not observable.

Observable aliasing is explicit:

```quidra
int x = 1
int &writer = &x
const int &view = &x

writer = 5
print(view) // 5
```

`&x` is a safe abstract storage address, not a numeric pointer. `T &` is a read/write path to that storage; `const T &` is a live read-only path. `const T` is an immutable value binding. A const reference can observe changes performed through another writable path, but it cannot write, rebind, or recover write authority. Quidra does not expose pointer arithmetic, address-to-integer conversion, an explicit `*` dereference operator, or a general object-identity operator.

This same model applies to bindings, class fields, array elements, and bytes elements.

### 2. Authority is part of the call

A reference parameter makes caller-owned storage access explicit. The parameter determines whether that path is writable.

```quidra
void inspect(const int &value)
    print(value)

void initialize(int &value)
    value = 7

int value = 1
inspect(&value)
initialize(&value)
```

`&` means explicit storage access. `const` removes write authority from that access path. The same storage may be passed through multiple reference parameters, including a mixture of readonly and writable paths; readonly paths may observe writes made through another path.

The checker tracks initialization requirements and guarantees of reference parameters. A writable parameter may safely initialize previously uninitialized storage. A readonly reference always requires initialized storage. Authority can be reduced (`T &` to `const T &`) but cannot be recovered through the weaker reference.

### 3. State facts are tracked, not guessed

Uninitialized does not mean zero, `none`, or a hidden default.

```quidra
int x
print(x) // compile-time error
```

The checker follows initialization through branches, loops, references, classes, nested fields, calls, and returns.

Classes can be intentionally partial:

```quidra
class Point
    int x
    int y

Point point = Point()
point.x = 10
// point.y is still uninitialized
```

Methods are summarized by their observable receiver effects. The checker records facts such as:

- fields required before a call,
- fields definitely initialized after a call,
- fields written by the method,
- fields whose previous initialization state may be invalidated,
- initialization guarantees of returned class values,
- initialization requirements and guarantees of reference parameters, including read-only `const T &` paths.

Those summaries compose across method calls and control flow. The goal is to make mutation analyzable without forcing programmers to manually annotate every effect.

### 4. Implicit operations must preserve meaning

Quidra distinguishes **representation change** from **value change**.

An implicit numeric conversion is allowed only when every source value is exactly representable by the destination type.

```text
int8   → int16    allowed
uint8  → int16    allowed
uint32 → int      allowed
float32 → float   allowed

int8   → uint8    not implicit
uint64 → int      not implicit
int32  → float32  not implicit
int    → float    not implicit
```

Explicit casts use the destination type:

```quidra
int value = 100
int8 small = int8(value)
```

Explicit casts are practical rather than exact-only. Integer narrowing is range checked and never wraps. Integer-to-floating-point and floating-point-to-floating-point casts use deterministic destination IEEE-754 rounding, so precision may be reduced when the programmer explicitly requests that destination type.

Floating-point to integer is intentionally not a generic cast because the rounding meaning is ambiguous. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` to state that intent explicitly. Casts never request wrapping or clamping.

The general rule is:

> **Implicit behavior may remove boilerplate, but it may not silently change meaning.**

### 5. Absence, failure, completion, and impossibility are different

Quidra keeps several concepts separate:

- `void` — successful completion with no data,
- `none` — normal absence,
- `error` — a failed operation represented as data,
- `never` — no normal continuation.

```quidra
int | none | error lookup(int id)
    if id < 0
        return error("invalid id")
    if id == 0
        return none
    return id
```

These meanings are not collapsed into null, exceptions, sentinel integers, or implicit process termination.

`try` propagates `error` while preserving other alternatives:

```quidra
int | none | error doubled(int id)
    auto value = try lookup(id)

    match value
        int
            return value * 2
        none
            return none
```

A `match` must cover every alternative exactly once.

### 6. Alternatives are explicit; identity is not invented

Unions describe alternatives directly:

```quidra
int | string value
```

Inheritance is code and member reuse, not an implicit runtime subtype relation.

```quidra
class Parent
    int value

class Child : Parent
    int extra
```

`Parent` and `Child` remain distinct static types. There is no implicit upcast, object slicing, or hidden dynamic dispatch. If a value may be either type, write the alternative:

```quidra
Parent | Child value
```

Overrides are explicit and signature-checked. `super.method(...)` is statically resolved.

Generics follow the same preference for explicit structure: generic classes keep explicit type arguments, while generic functions and methods infer them only when every generic parameter is uniquely determined by the call arguments. Concrete instances are monomorphized before ordinary checking and native lowering.

### 7. Syntax should expose semantic roles

Quidra prefers familiar words and punctuation when they carry a stable meaning.

Examples:

```quidra
int x = 5
int &alias = &x

print(x)
int number = 10
string text = number.string()

int[] values = [1, 2, 3]
for &item in values
    item = item + 1
```

The language avoids syntax whose main purpose is ceremony. At the same time, it does not remove tokens that distinguish important semantics.

This is what **Maximum meaning per token** means in practice: fewer meaningless tokens, not fewer meaningful distinctions.

### 8. Name resolution is monotonic

Reserved identifiers are absolute: user code cannot redefine a reserved name as a binding, parameter, function, class, field, method, generic parameter, loop/match binder, CLI field, or import alias. Qualification does not create an exception; for example, because `math` and `array` are reserved, user-defined `object.math` and `object.array()` are invalid as well.

For ordinary user-defined names, shadowing is prohibited only while the earlier name is visible. The same spelling may be reused in genuinely disjoint scopes:

```quidra
int local_value()
    int x = 5
    return x

int x = 7
```

This yields one simple rule: **reserved names are never reusable; visible names are never shadowable; otherwise names may be reused.** Standard-library member names such as `zeros`, `mean`, or `matmul` are not automatically global reserved identifiers; `tensor.zeros(...)` works because `tensor` is a language-owned reserved namespace and `zeros` is selected inside that namespace.

The rule makes local edits safer for both humans and language models: adding code cannot make an earlier reference start resolving to a different declaration. Adding a new reserved global name is therefore compatibility-sensitive, so standard-library growth should normally happen behind existing namespaces or value methods rather than by adding bare built-ins.

### 9. The compiler should be useful to machines as well as humans

LLM-friendliness is not only surface syntax.

Quidra exposes compiler operations intended for structured tooling:

```bash
quidra check program.qui --json
quidra fmt program.qui
quidra fmt program.qui --check
quidra lsp
quidra inspect program.qui
quidra inspect program.qui --no-source --no-effects --kind call --depth 3
quidra patch program.qui change.json --write
```

`fmt` canonicalizes unambiguous token spacing while preserving literal contents, comments, and syntax whose token role is context-dependent; `quidra lsp` provides compiler-backed diagnostics and formatting over standard LSP stdio framing; `--check` performs a non-writing canonical-form check. `inspect` exposes source structure with spans, node identifiers, source hashes, inferred semantic information, and deterministic storage-effect summaries for method receivers and reference parameters. For token-efficient tooling, `--no-source` omits repeated source fragments, `--no-effects` omits effect summaries, `--kind KIND` keeps only one node kind, and `--depth N` bounds structural depth. Every inspected node includes an explicit `parent_id` and `depth`.

A patch identifies the source revision and the exact node/hash it expects to replace. Stale revisions, unknown nodes, hash mismatches, and overlapping edits are rejected. The resulting source is accepted only after it passes the compiler validation path.

This makes source modification closer to a checked transaction than blind text replacement.

The same principle appears throughout the language:

> **Make intent machine-readable, then validate it before execution.**

## Surface model

### Bindings

```quidra
int initialized = 5
int later
auto inferred = 10
```

`auto` requires an initializer. Visible names cannot be shadowed. Disjoint sibling scopes may reuse a name when neither binding is visible from the other.

### Numeric types

Signed integers:

```text
int8
int16
int32
int / int64
```

Unsigned integers:

```text
uint8
uint16
uint32
uint64
```

Floating-point:

```text
float32
float / float64
```

`int` is signed 64-bit. `float` is IEEE-754 binary64. `float32` is IEEE-754 binary32.

There is no `char` type:

- text is `string`,
- one byte as a number is `uint8`,
- binary sequences are `bytes`.

Numeric parsing and standard text conversion use methods:

```quidra
int | error value = int.parse("123")

int number = 123
string text = number.string()
```

### Arrays and bytes

```quidra
int[] dynamic = [1, 2, 3]
int[3] fixed = [4, 5, 6]
int[] zeros = array(5, fill = 0)

bytes data = bytes(4, fill = 0)
data[0] = 255
uint8 first = data[0]
```

Fixed array lengths are part of the type. Arrays have value semantics, including nested arrays.

`bytes` is mutable raw binary data with compact contiguous `uint8` elements. It supports indexing, mutation, length, value equality, iteration, writable iteration, and safe element references.

### Strings

Strings are immutable values.

```quidra
string name = "Quidra"
print("Hello, {name}")
print("first{enter}second")
```

Backslash is literal rather than an escape introducer. Named immutable values such as `enter`, `tab`, `home`, and `quote` represent control characters.

Immutable backing storage may be shared internally because that sharing cannot change observable value semantics. For the same reason, `text = text + piece` in a loop is linear overall rather than quadratic: when the target is the sole owner of its storage, the append reuses it with geometric growth instead of copying the accumulated prefix each time.

### Functions and calls

```quidra
int add(int a, int b = 1)
    return a + b

print(add(41)) // output: 42
print(add(a = 40, b = 2)) // output: 42
```

Positional arguments come before named arguments. Parameters can have defaults; defaults are evaluated afresh when omitted.

### Classes

```quidra
class Point
    float x
    float y = 0.0

    float length_squared()
        return x * x + y * y

Point point = Point(x = 3.0)
print(point.length_squared())
```

Fields and methods use one `class` construct. Methods access fields directly; there is no `self` or `this` syntax.

Construction is named by field. Fields may remain uninitialized when no value/default is supplied, and the checker tracks that state field by field.

Class equality is value equality and requires compared fields to be definitely initialized.

### Modules

Standard namespaces are always visible and cannot be imported or aliased:

```quidra
print(math.sqrt(16.0))
auto home_path = environment.get("HOME")
```

Local source modules use explicit quoted paths, while unquoted non-standard imports denote installed packages:

```text
import geometry = "./geometry.qui"
import shared = "@/shared.qui"
import plot = plotting

geometry.Point point = geometry.Point(x = 2, y = 3)
```

Relative quoted paths resolve from the importing file. `@/` resolves from the command working directory. Installed packages never silently fall back to a same-named file in the working directory.

Imported modules contain declarations only. Executable top-level statements belong to the root program. Import cycles are rejected.

### Generics

```quidra
class Box<T>
    T value

    T get()
        return value

T first<T>(T[] values)
    return values[0]

Box<int> box = Box<int>(value = 7)
print(first<int>([4, 5]))
```

Generic class type arguments are explicit. Generic function and method type arguments are inferred when every generic parameter is uniquely determined by the call arguments; otherwise they must be written explicitly. Concrete instances are deterministically monomorphized before static checking and native code generation.

### Control flow

```quidra
bool condition = false

void work()
    print("work")

if condition
    print("yes")
else
    print("no")

for i in range(0, 10)
    print(i)

while condition
    work()
```

Blocks use four-space indentation. Conditions are `bool`; numeric truthiness is not implicit.

### Basic I/O

```quidra
print("line")
write("prompt: ")

string | none | error line = input()
```

`print` appends a newline. `write` does not. `input()` returns a line, `none` at EOF, or `error` for an input failure.

## Text, arrays, and conditional chains

Strings are immutable UTF-8 text. `len(text)` counts Unicode code points, `text[index]` returns a one-code-point string, and text supports `contains`, `starts_with`, `ends_with`, `find`, `slice`, `trim`, and `split`. `text.utf8()` explicitly exposes encoded bytes, `text.codepoints()` explicitly exposes Unicode scalar values, and `string[]` uses `join(separator)` for efficient assembly.

Runtime-sized arrays can be fully initialized or explicitly created with uninitialized elements:

```quidra
int[] pending = array(100)
pending[0] = 7

int[] zeros = array(100, fill = 0)

int[] values = [1, 2]
values = values.append(3)
values = values.concat([4, 5])
int[] ordered = values.sorted()
```

`array(n)` requires an array type context and tracks initialization per element. Reading an uninitialized element fails deterministically rather than exposing arbitrary memory. `array(n, fill = value)` is fully initialized, may infer the element type, evaluates `value` once, and gives each element independent value semantics.

`append`, `concat`, and `sorted` return new array values. `sorted()` is available for numeric, `bool`, and `string` arrays of either fixed or runtime size and returns a runtime-sized sorted copy; it is deterministic and non-mutating. The implementation may use copy-on-write or spare capacity only when that optimization is unobservable, so source-level value semantics remain unchanged. For fully initialized local arrays, typed IR can carry that proof into LLVM and omit redundant per-element initialization checks. Forming a read/write whole-array reference restores the check automatically; forming a `const T &` read-only reference preserves the proof. Control-flow joins that cannot preserve the proof also restore the check, and bounds safety is unaffected.

Dense numeric tensors use the dedicated `tensor<T>` type:

```quidra
tensor<float32> pixels = tensor.zeros<float32>([3, 224, 224])
tensor<float32> bias = tensor.ones<float32>([1, 224, 224])
tensor<float32> result = pixels + bias

tensor<float32> manual = tensor<float32>([2, 2])
manual[0, 0] = 1.0
manual[0, 1] = 2.0
manual[1, 0] = 3.0
manual[1, 1] = 4.0

auto crop = result[:, 10:20, 30:40]
float32 value = result[0, 10, 20].item()
```

`tensor<T>(shape)` creates storage whose elements are initially uninitialized; individual scalar elements can be initialized with `tensor[i, j, ...] = value`. `tensor.zeros<T>(shape)` and `tensor.ones<T>(shape)` create fully initialized tensors. Reading an element that is not definitely initialized remains a deterministic safety failure.

Tensor-to-tensor broadcasting is intentionally strict: ranks must match and each axis must match or be singleton on one side. Scalars broadcast to tensors. Slices may use internal views, but mutation preserves value semantics through copy-on-write. `.reshape(shape)` never hides a copy; call `.contiguous()` explicitly first when needed. `.cast<T>()` follows the explicit numeric-conversion policy: integer narrowing is range-checked, integer-to-float and float-to-float may deterministically reduce precision, and float-to-integer requires an explicit rounding operation.

Compound assignment supports `+=`, `-=`, `*=`, `/=`, and `%=`. Its target is evaluated exactly once, avoiding duplicated side effects in indexed or member targets.

Use `elif` for flat conditional chains:

```quidra
int score = 85
if score >= 90
    print("A")
elif score >= 80
    print("B")
else
    print("C")
```

## Neural computation

The `neural` namespace is the Define-by-Run/autograd foundation. A model is an
ordinary Quidra class containing `neural.Parameter<T>` and `neural.State<T>`
fields.

```quidra
class Scale
    neural.Parameter<float32> value

Scale model = Scale(
    value = neural.Parameter<float32>(value = tensor.ones<float32>([1]))
)
neural<float32> prediction = model.value.track() * float32(2)
neural<float32> loss = neural.mean(prediction * prediction)
neural.Gradients gradients = neural.grad(loss)
neural.update(&model, gradients, rate = 0.01)
```

`neural` means `neural<float32>`. `neural.track(tensor)` enters the dynamic
graph, `.untrack()` returns ordinary tensor storage, and `neural.grad(loss)`
returns an explicit `neural.Gradients` value. There is no hidden gradient
accumulation or parameter registry. Operand-level primitives such as `affine`,
`convolve2d`, `normalize`, reductions, and safe update operations allow ordinary
packages to build differentiable libraries.

The official `dnn` package provides layers, activations, losses, and optimizers
through a normal package import:

```text
import dnn

dnn.LinearLayer layer = dnn.Linear(features_in = 2, features_out = 1)
dnn.AdamOptimizer optimizer = dnn.Adam()
```

Model and training state use one typed, non-executable `.quistate` format.
Saving uses atomic replacement. Loading requires exact nominal root types,
structural schema, tensor dtype/shape, version, and checksum, validates the
complete payload before replaying writes, and rejects mismatches without
partially restoring earlier fields.

## Safety model

The static checker rejects, among other things:

- reads before definite initialization,
- invalid field initialization paths,
- attempts to write, rebind, or regain write authority through const access paths,
- reference type mismatches,
- implicit lossy numeric conversions,
- incomplete union matches,
- use of partial class values where complete values are required,
- invalid override signatures,
- import cycles and namespace collisions,
- invalid concrete generic instantiations.

The native runtime checks cases that depend on runtime values, including:

- integer overflow at each supported integer width,
- integer division and remainder by zero,
- array and bytes bounds,
- invalid allocation sizes,
- zero range steps,
- out-of-range explicit integer casts.

Runtime safety failures terminate deterministically with status `101`.

Floating-point arithmetic follows IEEE-754 behavior for its width.

## Value equality

`==` and `!=` compare values, not storage identity.

- scalars compare their values,
- strings compare text,
- bytes compare contents,
- arrays compare lengths and elements recursively,
- classes compare fields recursively when those fields are definitely initialized.

Internal allocation identity is intentionally not part of the source-language model.

## Native implementation

The current implementation includes:

- indentation-aware lexing/parsing with a nesting safety budget,
- typed AST checking and resolved call semantics,
- definite-initialization analysis for bindings, fields, arrays, and tensors,
- receiver and reference-parameter effect summaries, including read-only `const T &` paths,
- unions, exhaustive matching, and explicit `error` propagation,
- fixed-width numeric checking with strict implicit conversion and range-checked explicit integer casts,
- partial classes, explicit static inheritance, and value equality,
- explicit safe storage references with pinned substorage lifetime,
- monotonic bare-name resolution and always-visible standard namespaces,
- local modules, installed-package resolution, explicit generics, and monomorphization,
- dense tensors with views, copy-on-write, strict broadcasting, explicit numeric casting, statistics, and rank-2 matrix multiplication,
- PNG/JPEG/BMP/TIFF/WebP image I/O through `image`,
- typed Quidra IR followed by direct LLVM IR/native lowering,
- Linux, macOS, and Windows native execution/packaging,
- structured diagnostics, source inspection, and revision/hash-validated node-level patching.

The source extension is `.qui`.

## Build

Requirements:

- CMake 3.20+
- C++20 compiler
- Clang 15+ for native code generation (Quidra emits LLVM IR and invokes Clang; the current distribution intentionally does not bundle a backend toolchain)
- libcurl development files (for the `http` standard module and native linking)
- libpng, libjpeg, libtiff, and libwebp development files (for `image`)
- Python 3 for documentation verification
- Bash for the full Unix test suite

The runtime archive is built with these native dependencies, while generated programs link HTTP or image libraries only when their generated LLVM IR actually calls those runtimes.

Linux and macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/quidra run examples/hello.qui
```

Windows (PowerShell, with CMake-visible libcurl/libpng/libjpeg/libtiff/libwebp installations; the project CI uses vcpkg):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\quidra.exe run examples\hello.qui
```

Packaging is platform-specific: Linux produces DEB/TGZ, macOS produces TGZ, and Windows produces ZIP through CPack.

## Interactive REPL

Run `quidra` with no arguments from a terminal to start the native REPL:

```text
$ quidra
Quidra 0.2.0
>>> 1 + 2
3
>>> int x = 5
>>> x
5
>>> x = 8
>>> x
8
>>> float y = 4.0
>>> y
4.0
>>> int square(int value)
...     return value * value
...
>>> square(6)
36
```

Accepted declarations, bindings, functions, classes, generic declarations, and imports remain available for later submissions. Each candidate submission is parsed, specialized, checked, lowered to typed Quidra IR and LLVM IR, compiled natively, and executed. A compile error rejects only that candidate; the previously accepted session remains intact. A standalone expression uses a dedicated typed REPL-display IR operation rather than a source rewrite to `print(...)`.

The current REPL still recompiles accumulated accepted source, but it does not silently replay observable effects. During reconstruction, prior `print` / `write` operations are suppressed, including output reached through user-function calls. If a submission may execute external or nondeterministic operations such as file/environment access, input, time, random, process, HTTP/image I/O, CLI reads, or neural state save/load, the session arms a conservative replay barrier before native execution. Further submissions are rejected with `REPL_REPLAY_UNSAFE` until `:reset`, even when the effectful submission later fails at runtime, because the external effect may already have happened.

`:help` lists REPL commands, `:type expression` prints the statically checked type, `:reset` clears accepted session state and the replay barrier, and `:quit` or `:exit` exits. Ctrl-D exits normally; Ctrl-C cancels the current input and keeps the session.

When standard input is not a TTY, bare `quidra` does not implicitly consume it as a REPL. `quidra repl` explicitly starts the same REPL and is useful for automated tests.

## C interoperability

The core C FFI is intentionally narrow and explicit:

```quidra
extern int c_abs(int value) = "llabs"
print(c_abs(-42))
```

Results are limited to ABI-stable scalar values or `void`. Scalar/bool parameters cross by value. Managed text/binary input must instead be an explicit read-only storage borrow, written `const string &` or `const bytes &` and passed with `&storage`. Each borrow lowers to a `(data pointer, uint64 byte length)` C ABI pair only for the duration of the call. Quidra does not expose a pointer-only C-string contract, infer ownership transfer, or infer foreign failure from `errno`/null. Foreign code must not mutate or retain a borrowed pointer; APIs with a different contract need an explicit C wrapper. Each external C symbol may be bound by only one `extern` declaration in a compilation. `main`, the compiler-owned `n_*` mangling namespace, and the implementation-owned `quidra_*` / `__quidra_*` C symbol namespaces are reserved.

## CLI

```bash
quidra
quidra repl
quidra lsp
quidra package list
quidra package install ./my-package --name my_package
quidra program.qui
quidra run program.qui
quidra check program.qui
quidra check program.qui --json --max-errors 20
quidra fmt program.qui
quidra fmt program.qui --check
quidra ir program.qui
quidra llvm program.qui
quidra inspect program.qui
quidra patch program.qui change.json --write
quidra build program.qui -o program
quidra build program.qui --debug -o program-debug
quidra debug program.qui -- arg1 arg2
quidra describe
```


Installed source packages use the existing deterministic package location `~/.quidra/packages/<name>/main.qui` and can also be discovered through `QUIDRA_PACKAGE_PATH`. The built-in `quidra package` commands manage only the default local store: `install` validates `main.qui` through the ordinary frontend before an atomic directory replacement, rejects symbolic links, and refuses replacement unless `--force` is explicit; `remove`, `list`, and `path` provide the corresponding local operations. Network fetching is intentionally outside this core command, so installing a package never executes a package script or silently contacts a registry. For reproducible projects, `quidra package lock FILE.qui` writes `quidra.lock` with the SHA-256 of every direct and transitive installed package actually reached by the import graph. Once present, the compiler enforces those hashes; `quidra package lock FILE.qui --check` is the non-writing CI check.

### Debugging

`quidra build --debug` asks the native Clang driver for an unoptimized, frame-pointer-preserving debug build and disables link-time dead stripping for that build. `quidra debug FILE.qui` launches LLDB or GDB (or `QUIDRA_DEBUGGER`) against that executable. Current integration guarantees native function/symbol visibility; fine-grained Quidra source-line DWARF mapping is not yet claimed.

## Documentation

- [Development and release workflow](docs/development.md)
- [Language semantics](docs/spec/language.md)
- [Numeric types and bytes](docs/spec/numeric-and-bytes.md)
- [Grammar](docs/spec/grammar.ebnf)
- [LLM guide](docs/spec/llm-guide.md)
- [Architecture](docs/spec/architecture.md)
- [Diagnostics](docs/spec/diagnostics.md)
- [Source patch schema](docs/spec/patch-schema.md)

## Standard namespaces

Standard namespaces are reserved and always visible. They are not imported. Source-file imports are always quoted, and unquoted non-standard imports name installed packages, so module resolution cannot silently change with files in the working directory.

```text
print(math.sqrt(16.0))
tensor<float32> x = tensor.zeros<float32>([2, 3])

import local = "./local.qui"
import shared = "@/shared.qui"
import plot = plotting
```

The reserved standard namespaces are `math`, `cli`, `file`, `environment`, `test`, `time`, `random`, `process`, `map`, `set`, `json`, `http`, `tensor`, `stats`, `linear`, `signal`, `image`, and `neural`. Only referenced standard implementations are linked into a program. Boolean logic is spelled `and`, `or`, and `not`.

`math` provides `pi`, `e`, `sin`, `cos`, `tan`, `log`, `exp`, `pow`, and namespaced access to `abs`, `sqrt`, `min`, and `max`.

Command-line interfaces are declarative and expose their values through one binding:

```quidra
cli args
    string source = argument()
    int count = option(default = 1)
    bool verbose = flag()

print(args.source)
print(args.count)
```

The field name is also the CLI name: `count` becomes `--count`, without repeating `"count"`. The CLI binding is a root top-level value and is not implicitly captured by functions; pass CLI-derived values explicitly when reusable code needs them. Direct execution accepts program arguments after the source path; `quidra run FILE.qui -- ARGS...` uses `--` as the compiler/program boundary.

`file` provides text `read` / `write`, binary `read_bytes` / `write_bytes`, `exists`, `is_directory`, `remove`, `copy`, `move`, `mkdir`, and deterministic `list`. `file.is_directory(path)` returns `bool | error` (a missing path is `false`). `file.list(path)` returns sorted direct child paths as `string[] | error`; `file.list(path, recursive = true)` returns the full sorted descendant list. Filesystem failures are represented with `error` unions rather than silent fallback.

`environment` treats an unset host variable as absence rather than failure:

```quidra
string | none home_path = environment.get("HOME")
bool configured = environment.has("HOME")
```

`test` reuses normal Quidra semantics:

```quidra
test.check(2 + 2 == 4)
test.equal("Quidra", "Quidra")
```

`test.equal` accepts exactly the types for which ordinary `==` is defined. A failed assertion exits with status 1.

`time` keeps units explicit through opaque values:

```quidra
time.Instant start = time.now()
time.sleep(time.seconds(0.01))
time.Duration elapsed = time.since(start)
print(elapsed.seconds())
```

`time.now()` uses a monotonic clock, so elapsed-time measurement is not affected by wall-clock adjustments.

`random` uses explicit generator state rather than hidden global randomness:

```quidra
random.Generator rng = random.generator(seed = 42)
int value = rng.int(1, 10)
float sample = rng.float()
bool bit = rng.bool()
```

The same seed produces the same sequence. Generator assignment follows ordinary Quidra value semantics, so a copied generator receives independent state. `Generator.int(start, end)` uses the half-open range `[start, end)`.

`process` executes programs without implicit shell parsing:

```quidra
process.Result result = process.run("git", ["status", "--short"])
print(result.started)
print(result.status)
print(result.output)
print(result.error)
```

`process.run(program, args)` keeps the executable and argument array separate. `started` distinguishes launch failure from a program that started and returned a nonzero status. Standard output and standard error are captured separately. `process.exit(status)` terminates the current Quidra program with the given integer status; process termination is namespaced rather than consuming the global name `exit`.

`map` and `set` are deterministic value containers:

```quidra
map.Map<string, int> counts = map.Map<string, int>()
counts.set("apple", 2)
counts.set("banana", 1)
auto apple = counts.get("apple")
match apple
    int value
        print(value)
    none
        print("missing")
string[] keys = counts.keys()
for key in keys
    print(key)

set.Set<string> tags = set.Set<string>()
tags.add("compiler")
tags.add("ai")
print(tags.has("compiler"))
```

Keys/elements are currently integer, `bool`, or `string` values. Insertion order is stable. Ordinary assignment copies container state independently, following the same value semantics as arrays and classes.

`json` exposes immutable parsed values without collapsing JSON `null` into Quidra `none`:

```quidra
string source = "{{" + quote + "name" + quote + ":" + quote + "Quidra" + quote + "," + quote + "items" + quote + ":[1,2]}}"
auto parsed = json.parse(source)
match parsed
    json.Value root
        auto name = root.get("name")
        match name
            json.Value value
                auto text = value.text()
                match text
                    string content
                        print(content)
                    error problem
                        print(problem)
            none
                print("missing")
            error problem
                print(problem)
        print(root.encode())
    error problem
        print(problem)
```

`none` means a missing object key or out-of-range array index. A JSON `null` remains a real `json.Value` whose `kind()` is `"null"`. Typed accessors return `error` on kind mismatch. Values are immutable; structural comparison is explicit with `equal`.


### HTTP

```quidra
auto result = http.get("https://example.com")
match result
    http.Response response
        print(response.status)
        print(len(response.body))
        auto content_type = response.header("content-type")
        match content_type
            string value
                print(value)
            none
                print("missing")
    error problem
        print(problem)
```

`http.Response.body` is `bytes`, not `string`, because an HTTP body is not necessarily text. Header lookup is ASCII case-insensitive and a missing header is `none`. HTTP status codes such as 404 and 500 still produce a `Response`; DNS, TLS, connection, redirect, timeout, and protocol failures produce `error`. The v0.1 runtime supports only `http://` and `https://`, follows at most 10 redirects, keeps TLS certificate verification enabled, and captures the complete response body in memory.

### Tensor numerics and image I/O

`stats.mean(value)` computes the arithmetic mean of a numeric tensor and returns `float`. Empty or partially uninitialized tensors fail deterministically rather than inventing missing values.

`linear.dot(a, b)` computes a scalar dot product for same-length rank-1 numeric tensors. `linear.matmul(a, b)` performs rank-2 matrix multiplication for compatible `[m, k]` / `[k, n]` tensors. Both require identical element types, preserve that numeric type, and keep integer multiplication and accumulation overflow-checked.

Image I/O is explicit and tensor-native:

```quidra
tensor<uint16> | error loaded = image.read("input.png")
match loaded
    tensor<uint16> pixels
        auto written = image.write("output.png", pixels)
        match written
            void
                print("saved")
            error problem
                print(problem)
    error problem
        print(problem)
```

Decoded images use CHW layout: grayscale `[1,H,W]`, RGB `[3,H,W]`, and RGBA `[4,H,W]`. `image.read` preserves every source sample dtype that Quidra and the codec can represent: PNG yields `uint8` or `uint16`, TIFF can yield any built-in numeric tensor dtype, and JPEG/BMP/WebP yield `uint8`. With no expected type, `auto loaded = image.read(path)` therefore has the union of all numeric tensor alternatives plus `error`; use exhaustive `match` when the dtype is genuinely unknown. When the expected union names one tensor dtype, such as `tensor<uint16> | error`, a file with a different dtype produces `error` rather than an implicit conversion. `image.write` likewise writes only when the target format can represent the tensor dtype exactly. There is no implicit normalization, BGR conversion, dtype conversion, or alpha discard; JPEG rejects RGBA input.

The library boundary is intentionally small:

```text
standard foundations
tensor
├── image       file I/O using tensor<uint8>
└── neural      autodiff, gradients, parameters, and training state

official source packages
├── dnn         layers, activations, losses, and optimizers
└── vision      tensor image processing and computer vision
```

`dnn` and `vision` are installed and imported through the ordinary package
system; neither package receives compiler-specific name handling.

## License

MIT
