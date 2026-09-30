# Quidra

**Maximum Meaning Per Token**

Quidra is a statically typed, native general-purpose programming language built
around **semantic compression**: express as much reliable intent as possible
with as little ceremony as possible, without hiding distinctions that affect
correctness.

That is the primary design target. Syntax is kept when it carries meaning about
value, storage, authority, representation, failure, state, shape, device
placement, or effects. Syntax is removed when the compiler can recover the same
fact unambiguously.

> **Fewer meaningless tokens, not fewer meaningful distinctions.**

- Website: <https://quidra-lang.com>
- Playground: <https://quidra-lang.github.io/playground/>
- Playground source: <https://github.com/quidra-lang/playground>

The name **Quidra** is inspired by the Latin *quidditas* — the “whatness” or
essence of a thing.

## Maximum Meaning Per Token

Quidra optimizes for semantic density rather than character count:

```text
semantic information
────────────────────
       tokens
```

Shorter is better only when meaning stays explicit, stable, and mechanically
checkable. The compiler should infer facts it can prove; source code should state
facts that cannot be inferred without changing meaning.

That gives Quidra two rules:

1. **Remove ceremony that carries little semantic information.**
2. **Keep syntax that distinguishes behavior, authority, failure, state,
   representation, shape, or placement.**

Common forms therefore have deliberately narrow jobs:

| Form | Meaning carried |
| --- | --- |
| `=` | independent value-oriented assignment |
| `&x` | explicit access to existing storage |
| `T &` | writable path to caller-visible storage |
| `const T &` | live read-only path to storage |
| `T \| none` | normal absence is part of the type |
| `T \| error` | failure is part of the type |
| `auto \| error` | retain an otherwise fail-fast failure channel |
| `try` | propagate `error` |
| `T(value)` | explicit representation conversion |
| `tensor<T><3, _, _>` | element type plus exact rank/shape constraints |
| `and / or / not` | boolean logic |
| `AND / OR / XOR / NOT / << / >>` | fixed-width integer bit operations |
| `match` | exhaustive handling of alternatives |

The same rule applies beyond individual tokens: visible names cannot be
shadowed, numeric values do not silently change representation, ordinary
assignment does not create observable aliasing, and CPU/GPU movement is never
inferred from a later operation.

## A program as compressed semantics

A small Quidra program can state several important facts without surrounding
ceremony:

```quidra
void increment(int &value)
    value += 1

float32 first_sample(const tensor<float32><3, _, _> &pixels)
    return pixels[0, 0, 0].item()

int count = 7
increment(&count)

tensor<float32><3, _, _> pixels = tensor.zeros([3, 224, 224])
float32 sample = first_sample(&pixels)

uint8 flags = 240
uint8 selected = flags AND 15
```

The surviving tokens carry concrete semantics:

- `int &value` says the function may write caller-visible storage, while
  `&count` makes that authority visible at the call site.
- `const tensor<float32><3, _, _> &pixels` says the function observes existing
  storage without write authority, with `float32`, rank 3, and first extent 3
  fixed in the type.
- `AND` cannot be confused with boolean `and`, storage `&`, or union `|`.
- Ordinary `=` still means value semantics; none of these forms invents hidden
  aliasing or implicit conversion.

This is semantic compression: remove repetition, not distinctions.

## Design laws

### Values first; storage and authority are explicit

Ordinary assignment creates an independent value. Observable access to existing
storage is written with `&`, and writable authority is visible in both the
parameter and the call.

```quidra
int[] original = [1, 2, 3]
int[] copy = original
copy[0] = 9

void reset(int &value)
    value = 0

int count = 5
reset(&count)
```

The implementation may use moves, copy-on-write, reference counting, or copy
elision only when the optimization cannot change those source semantics.

### The compiler proves state; source states intent

Uninitialized storage is not secretly zero, `none`, or another default. The
checker tracks initialization through branches, calls, fields, arrays, tensors,
and references. Shape constraints, receiver effects, and reference effects are
likewise checked rather than guessed.

The source does not repeat proofs the compiler already has. It does state the
intent that cannot be inferred safely.

### Representation changes are explicit

A typed numeric value never changes representation merely because a destination
could hold it. Conversions use the destination type, such as `float32(value)`
or `int8(value)`. Float-to-integer conversion requires the rounding choice to
be named with `math.trunc`, `math.round`, `math.floor`, or `math.ceil`.

A required result type is a constraint, not permission to convert.

### Failure is lightweight by default, explicit when retained

Fallible operations can flow directly into a success-only context; an actual
`error` then fails fast at that boundary. Explicit `T | error`,
`auto | error`, `try`, and `match` are for code that deliberately keeps,
propagates, or handles the failure channel.

This keeps ordinary examples and ordinary application code focused on the main
flow without removing typed failure from the language.

### Names are monotonic

Reserved names are never reusable, and a visible user-defined name cannot be
shadowed. Adding nearby code therefore cannot silently redirect an earlier bare
reference. Standard-library growth normally happens behind namespaces or value
methods instead of consuming new global names.

### Movement and differentiation are visible

Tensor device placement is explicit. A later GPU use is not permission to move
earlier work. Floating tensors are untracked by default; `.track()`,
`.untrack()`, and `.retrack()` visibly control autograd provenance.

The same principle applies to mutation in NN/DNN training code: the model being
changed is named at the gradient and optimizer operations.

### Machine-readable by design

LLM-friendliness is not only syntax. The compiler exposes structured checking,
formatting, inspection, and revision-validated patching:

```bash
quidra check program.qui --json
quidra fmt program.qui
quidra inspect program.qui --no-source --no-effects --kind call --depth 3
quidra patch program.qui change.json --write
```

Source modification can therefore be checked against compiler-known structure
instead of relying on blind text replacement.

## Language tour: semantics carried by the surface

The following syntax is not intended as an inventory of unrelated features. Each form exists to expose a semantic distinction that matters to humans, language models, or static checking while leaving mechanically provable facts to the compiler.

### Bindings

```quidra
int initialized = 5
int later
int source = 10
auto inferred = source
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

Exact numeric values:

```text
bigint
bigreal
```

`int` is signed 64-bit. Fixed-width signed integers have a defined two's-complement bit representation. `float` is IEEE-754 binary64. `float32` is IEEE-754 binary32. `bigint` is an exact arbitrary-precision integer; `bigreal` represents exact rational and symbolic real values rather than a configurable floating-point precision.

Fixed-width integers use explicit bitwise syntax:

```quidra
uint8 flags = 240
uint8 mask = 15
uint8 selected = flags AND mask
uint8 toggled = flags XOR mask
uint8 inverted = NOT flags
uint8 shifted = flags << 2
```

Uppercase bitwise words are intentionally distinct from boolean `and` / `or` / `not`, safe storage `&`, and union `|`. Signed operations use the same defined N-bit two's-complement representation used by explicit binary conversion; signed `>>` preserves the sign bit. This is a small example of Quidra preferring one stable semantic role per spelling over familiar overloads.

There is no `char` type:

- text is `string`,
- one byte as a number is `uint8`,
- raw binary sequences are `bin`.

Numeric parsing and standard text conversion use methods:

```quidra
int value = int.parse("123")

int number = 123
string text = number.string()
```

### Arrays and bin

```quidra
int[] values = [1, 2, 3]
int[3] fixed = [4, 5, 6]

int[] pending = array(100)
pending[0] = 7

int[] zeros = array(100, fill = 0)
values = values.append(4)
```

Fixed array lengths are part of the type. Runtime-sized arrays may be created
uninitialized with `array(n)` or fully initialized with
`array(n, fill = value)`; initialization is tracked per element. Arrays have
value semantics, including nested arrays. `append`, `concat`, and `sorted`
return new array values rather than hiding mutation of another value.

`bin` is mutable packed raw binary data:

```quidra
bin data = bin.fill(8, 0)
data[0] = bin.fill(1, 1)
bin first = data[0]
bin parsed = bin.parse("0101")
```

`len(data)` is the number of bits, indexing returns one-bit `bin`, and slicing
returns another `bin`. Binary-to-numeric interpretation is explicit, for
example `uint8(bits)`, and the bit length must match the destination width.

### Strings

Strings are immutable values. Repetition uses `string.repeat(value, n)`, where `value` is exactly one Unicode code point and `n` is the repeat count.

```quidra
string name = "Quidra"
string repeated = string.repeat("a", 6)
print("Hello, {name}")
print(NL)
print("first{NL}second")
print(NL)
```

Backslash is literal rather than an escape introducer. The eight two-letter uppercase immutable values `NL`, `HT`, `CR`, `DQ`, `BS`, `FF`, `VT`, and `BL` represent the built-in control characters.

Immutable backing storage may be shared internally because that sharing cannot change observable value semantics. For the same reason, `text = text + piece` in a loop is linear overall rather than quadratic: when the target is the sole owner of its storage, the append reuses it with geometric growth instead of copying the accumulated prefix each time.

Strings are immutable UTF-8 text. `len(text)` counts Unicode code points, `text[index]` returns a one-code-point string, and text supports `contains`, `starts_with`, `ends_with`, `find`, `slice`, `trim`, and `split`. `text.utf8()` explicitly exposes the UTF-8 encoding as `bin`; `string.from_utf8(data)` explicitly validates byte-aligned binary data and returns `string | error`; `text.codepoints()` explicitly exposes Unicode scalar values; and `string[]` uses `join(separator)` for efficient assembly.

### Functions and calls

```quidra
int add(int a, int b = 1)
    return a + b

print(add(41)) // output: 42
print(NL)
print(add(a = 40, b = 2)) // output: 42
print(NL)
```

Positional arguments come before named arguments. Parameters can have defaults; defaults are evaluated afresh when omitted. Ordinary function overloading is not supported: one function name has one ordinary definition, including argument-count and return-type variants. Optional call forms use default parameters.

### Classes

```quidra
class Point
    float x
    float y = 0.0

    construct(float px, float py)
        x = px
        y = py

    float length_squared()
        return x * x + y * y

Point point = Point(3.0, 4.0)
print(point.length_squared())
print(NL)

Point origin
origin.x = 0.0
```

Fields and methods use one `class` construct. Methods access fields directly; there is no `self` or `this` syntax. A class may declare at most one `construct` member. Constructor call variants use default parameters rather than constructor overloads; `T(...)` runs that single constructor. A declaration without an initializer creates the value with its field defaults so fields can be assigned one at a time. A constructor that can fail is spelled `Point | error construct(...)`; `Point p = Point(...)` then fails fast on error and `try Point(...)` propagates it. Members are public by default; prefix a field, method, or constructor with `private` to restrict access to methods of that class.

```quidra
class Counter
    private int value = 0

    private void increment_raw()
        value = value + 1

    void increment()
        increment_raw()
```

Private fields cannot be read, written, or addressed outside their declaring class; a constructor initializes hidden state. Private methods cannot be called outside their declaring class.

Fields may remain uninitialized when no assignment or default supplies them, and the checker tracks that state field by field.

Class equality is value equality and requires compared fields to be definitely initialized.

### Modules

Core standard namespaces are always visible and cannot be imported or aliased:

```quidra
auto home_path = environment.get("HOME")
```

Generic mathematical semantics live in the installed `math` package and require
an explicit import:

```quidra
import math
print(math.sqrt(float(16.0)))
print(NL)
```

Local source modules use explicit quoted paths, while unquoted non-standard imports denote installed packages:

```quidra
import geometry = "./geometry.qui"
import shared = "@/shared.qui"
import plot = plotting

geometry.Point point = geometry.Point(2, 3)
```

Relative quoted paths resolve from the importing file. `@/` resolves from the command working directory. Installed packages never silently fall back to a same-named file in the working directory. A module's own imports stay private unless it writes `public import mode = "./mode.qui"`, which re-exports the target as a nested namespace such as `nn.mode.fast()`.

Imported modules may export declarations and immutable compile-time `const` bindings. Other executable top-level statements belong to the root program. Import cycles are rejected.

### Generics

```quidra
class Box<T>
    T value

    construct(T initial)
        value = initial

    T get()
        return value

T first<T>(T[] values)
    return values[0]

Box<int> box = Box<int>(7)
print(first<int>([4, 5]))
print(NL)
```

Generic class type arguments are explicit. Generic function and method type arguments are inferred when every generic parameter is uniquely determined by the call arguments; otherwise they must be written explicitly. Concrete instances are deterministically monomorphized before static checking and native code generation.

Repeated function or method names are permitted only for a generic specialization family. Family members keep the same call shape: the same argument count, names, reference/const forms, default-argument positions, and generic type pattern. A concrete exact-type member may specialize that family. Generic members may differ by constraints such as `numeric` and the narrower `floating`; any number of generic parameters such as `<T1, T2>` is allowed. Exact concrete matches win over generic matches, narrower statically ordered constraints win over broader ones, and an overlap with no unique most-specific member is a compile error.

### Control flow

```quidra
bool condition = false

void work()
    print("work")
    print(NL)

if condition
    print("yes")
    print(NL)
else
    print("no")
    print(NL)

for i in range(0, 10)
    print(i)
    print(NL)

while condition
    work()
```

Blocks use four-space indentation. Conditions are `bool`; numeric truthiness is not implicit.

Use `elif` for flat conditional chains:

```quidra
int score = 85
if score >= 90
    print("A")
    print(NL)
elif score >= 80
    print("B")
    print(NL)
else
    print("C")
    print(NL)
```

### Basic I/O

```quidra
print("line")
print(NL)
print("prompt: ")
flush()

string line
scan(&line)

int n
int m
scan("{&n} {&m}")
```

`print` writes exactly the supplied value and never appends a newline; output a separate `NL` when line termination is wanted. `flush()` explicitly flushes standard output. Both `print` and `flush` return `void | error`, so an output failure fails fast when the call is a statement. There is no console `write` API and no `io` namespace. `scan` reads one line: `scan(&x)` reads a single value into `x`, and a format such as `"{&name},{&age}"` splits the line at its literal text and parses each `{&target}` by the target's type; a `string` target takes the text as it is. End of input, invalid text, and leftover input are `error`, which fails fast for a statement and can be handled through `void | error read = scan(...)`.

## Tensors and explicit devices

Dense numeric tensors use `tensor<T>`. The element type is static; an optional
second angle group states an exact-rank shape pattern. Each written axis exists,
`_` leaves its extent unrestricted, and an integer expression constrains that
extent.

```quidra
int batch = 3
tensor<float32><batch * 2, 224> contextual = tensor.zeros()
batch = 8 // the captured extent remains 6

tensor<float32> matrix = tensor.zeros([2, 3])
tensor<float32><3, _, _> pixels = tensor.zeros([3, 224, 224])
tensor<float32><1, _, _> bias = tensor.ones([1, 224, 224])
tensor<float32><3, _, _> result = pixels + bias

auto crop = result[:, 10:20, 30:40]
float32 value = result[0, 10, 20].item()
```

`tensor<T>(shape)` creates uninitialized tensor storage; `tensor.zeros` and
`tensor.ones` create initialized storage. Expected tensor types may supply an
otherwise unambiguous element type or exact shape, so the source need not repeat
facts the compiler already knows.

Device movement is always visible:

```quidra
tensor<float32> cpu = tensor.zeros([1024])
tensor<float32> gpu0 = tensor.zeros([1024], gpu = 0)

tensor<float32> copied = cpu.gpu(0)
tensor<float32> host = copied.cpu()
```

CPU is the default. Quidra never inserts CPU↔GPU or GPU↔GPU transfers and never
falls back to CPU when a requested device operation is unavailable. A later
`.gpu(0)` therefore means an explicit copy, not permission to relocate earlier
computation.

GPU work is host-asynchronous where supported. Host-visible reads such as
`.cpu()` and `.item()` synchronize as needed; `gpu.sync(index)` is the
explicit synchronization boundary. `time.now(sync = true)` and
`time.since(start, sync = true)` make synchronization explicit when measuring
GPU completion time.

Tensor operands must use compatible devices. Broadcasting is deliberately
strict: ranks match, and each axis must either match or be singleton on one
side. `.transpose()` is a metadata view; `.reshape()` never hides a copy, so
call `.contiguous()` explicitly when a contiguous representation is required.

Numeric representation changes use the same `T(value)` spelling as scalars and
arrays. A tracked tensor must be explicitly disconnected before changing dtype,
for example `float(x.untrack())`; representation change never silently cuts an
autograd graph.

## Tensor autograd

Autograd is part of the ordinary `tensor<T>` type. Core does not define a
Parameter, model, layer, optimizer, or optimizer state abstraction. Floating
tensors are untracked by default; `.track()` starts a dynamic graph,
`.untrack()` disconnects it, and `.retrack()` cuts prior provenance and starts
a new tracked root. Tracking is runtime metadata and never changes the source
type.

`loss.backward(&target, ...)` performs reverse-mode autodiff and writes only to
the explicitly listed gradient destinations. Repeated calls accumulate by
default; clear a destination explicitly before starting a new accumulation
window. Within one backward call, naming the same underlying gradient
destination more than once still writes it once. A tensor can be a destination directly and exposes its local gradient
through `.grad`, `.has_grad()`, and `.clear_grad()`. Ordinary tensor copies
have independent gradient state even when their immutable/COW value storage is
shared internally. `autograd.Target` is Core's generic explicit destination
handle for abstractions that need a stable gradient identity without turning the
tensor value itself into shared mutable state. Optional static shape contracts
remain ordinary tensor contracts such as `tensor<float32><3, _, _>`.

The official `nn` package owns architecture-independent learnable parameters,
layers, losses, optimizers, and training semantics as ordinary
Quidra source abstractions. The upper `dnn` package composes those mechanisms
into concrete deep-neural-network model families.

```quidra
import nn
import math

class Scale
    nn.Parameter<float32> value

Scale model
model.value = nn.Parameter<float32>(value = tensor.ones([1]))
nn.Adam optimizer = nn.Adam()
tensor<float32> prediction = model.value.track() * float32(2)
tensor<float32> loss = math.mean(prediction * prediction)

optimizer.zero_grad(&model)
loss.backward(&model)
optimizer.step(&model)
```

`nn.Parameter<T>` is implemented by the package with ordinary tensor values
plus a private Core `autograd.Target`; its gradient state is therefore NN-owned
rather than hidden in the Parameter value tensor. `nn.State<T>` has no gradient
destination. Neither abstraction is a compiler-special type. NN optimizer state,
update equations, reusable layer kernels, and NN execution policy likewise live
in the NN package. DNN may depend on NN but not the reverse.


## Safety model

The static checker rejects, among other things:

- reads before definite initialization,
- invalid field initialization paths,
- attempts to write, rebind, or regain write authority through const access paths,
- reference type mismatches,
- implicit representation-changing numeric conversions,
- incomplete union matches,
- use of partially initialized class values where fully initialized values are required,
- import cycles and namespace collisions,
- invalid concrete generic instantiations.

The native runtime checks invariant and safety failures that depend on runtime values, including:

- integer overflow at each supported integer width,
- integer division and remainder by zero,
- array and bin bounds,
- invalid allocation sizes,
- zero range steps.

These runtime safety failures terminate deterministically with status `101`. A range-checked numeric cast is different: an out-of-range value produces the cast's typed `error` alternative. Arrays and tensors use the whole converted container as the success alternative and expose no partial conversion when one leaf fails. The cast terminates only when a success-only context deliberately consumes that alternative via the ordinary fail-fast rule.

Floating-point arithmetic follows IEEE-754 behavior for its width.

## Value equality

`==` and `!=` compare values, not storage identity.

- scalars compare their values,
- strings compare text,
- bin compares bit contents,
- arrays compare lengths and elements recursively,
- classes compare fields recursively when those fields are definitely initialized.

Internal allocation identity is intentionally not part of the source-language model.

## Meaning preserved through native compilation

Semantic compression is a source-language goal, not a request for a lightweight or interpreted implementation. Native execution serves the semantic model rather than defining it: Quidra first makes meaning explicit and statically resolved, then preserves those decisions through a typed native compilation pipeline:

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

Later stages do not rediscover meaning from source spelling. Module resolution and generic specialization establish concrete declarations; the checker resolves calls, conversions, storage authority, initialization facts, shape constraints, and observable effects; typed Quidra IR carries those decisions explicitly into lowering.

The compiler is implemented in C++20. Native code is produced through LLVM IR and Clang; normal execution does not transpile Quidra to another source language. The supported desktop targets are Linux, macOS, and Windows. Platform-specific executable discovery, process launching, runtime packaging, and filesystem replacement are isolated behind host implementations.

The current implementation includes:

- indentation-aware lexing/parsing with a nesting safety budget,
- typed AST checking and resolved call semantics,
- definite-initialization analysis for bindings, fields, arrays, and tensors,
- receiver and reference-parameter effect summaries, including read-only `const T &` paths,
- unions, exhaustive matching, and explicit `error` propagation,
- fixed-width numeric checking with no implicit representation-changing conversion, range-checked explicit integer casts, and explicit fixed-width bitwise semantics,
- partially initialized classes, explicit composition, and value equality,
- explicit safe storage references with pinned substorage lifetime,
- monotonic bare-name resolution and always-visible standard namespaces,
- local modules, installed-package resolution, explicit generics, and monomorphization,
- dense tensors with views, copy-on-write, strict broadcasting, explicit numeric casting, transpose views, and autograd/device substrate operations,
- deterministic `file.Handle` resources with automatic lifetime-bound close independent of GC timing, value-semantic copies, incremental read/write/seek/flush, and optional explicit early `close()`,
- typed Quidra IR followed by direct LLVM IR/native lowering,
- Linux, macOS, and Windows native execution/packaging,
- structured diagnostics, source inspection, and revision/hash-validated node-level patching.

The source extension is `.qui`.

## Playground

The official Playground is a separate product and repository:
<https://github.com/quidra-lang/playground>. Core does not own its UI,
deployment, runner policy, or release lifecycle.

For compiler development only, Core keeps a small loopback test UI under
`tools/compiler-ui/`. It is a developer harness, not a second Playground
product:

```bash
python3 tools/compiler-ui/server.py --quidra ./build/quidra
```

The harness shells out to the real `quidra` binary and exposes **Run**,
**Check**, **Format**, **Quidra IR**, and **LLVM IR** locally. It is deliberately
loopback-only and is not a multi-tenant security boundary.

### Public Playground (WebAssembly + native runner)

The public playground at **<https://quidra-lang.github.io/playground/>**
([source](https://github.com/quidra-lang/playground)) keeps frontend tooling in
the browser. It loads `quidra_core` compiled to WebAssembly, so **Check**,
**Format**, **Quidra IR**, **Inspect** and **Patch** run locally in the visitor's
tab.

**Build** and **Run** use a separate native sandbox runner because LLVM linking,
the native runtime and OS process facilities are not part of the WebAssembly
frontend. Source is sent to that runner only when Build or Run is requested, and
the page enables those operations only when the runner reports the same
language version and exact same Core commit as the loaded WebAssembly frontend. The runner invokes the real
`quidra build` / `quidra run` commands rather than implementing a second
interpreter.

The playground is not a reimplementation. Its browser frontend has no parser,
checker, formatter or IR of its own; it calls the same `quidra::check`,
`quidra::format_source`, `quidra::ir::lower`,
`quidra::inspect_source_json` and `quidra::apply_source_patch` entry points
this repository already exposes.

Build the frontend bridge with the Emscripten toolchain:

```bash
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target quidra_wasm
node tests/wasm_api_tests.mjs build-wasm
```

`QUIDRA_BUILD_WASM_FRONTEND` (implied by Emscripten) restricts the build to
`quidra_core` and `src/wasm_api.cpp`. That configuration needs no CURL, GPU
backend, native runtime, or package-owned native dependencies because those are
outside the browser frontend.

## Build

Requirements:

- CMake 3.20+
- C++20 compiler
- LLVM 15+ with `lli` for ORC JIT execution in the REPL
- Clang 15+ for AOT native code generation via `quidra FILE.qui`, `quidra run`, and `quidra build` (the current distribution intentionally does not bundle the backend toolchain)
- libcurl development files (for the `http` standard module and native linking)
- Python 3 for documentation verification
- Bash for the full Unix test suite

Core's runtime archive links the dependencies required by Core standard facilities such as `http`. Image/video codecs and other domain-native dependencies belong to their packages and are not Core build requirements.

Linux and macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/quidra run examples/hello.qui
```

Windows (PowerShell, with a CMake-visible libcurl installation; the project CI uses vcpkg):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\quidra.exe run examples\hello.qui
```

Packaging is platform-specific: Linux produces DEB/TGZ, macOS produces TGZ, and Windows produces ZIP through CPack.

Execution modes are intentionally split:

- `quidra` → interactive REPL + LLVM ORC JIT
- `quidra main.qui [ARGS...]` → AOT compile/link to a unique hidden native artifact beside `main.qui`, execute it, then delete it
- `quidra build main.qui` → AOT compilation/linking and a persistent native executable artifact

Direct file execution rebuilds on every invocation and never copies the source tree. Temporary direct-run artifacts are named `.quidra-run-*` in the source file's directory, so executable-relative behavior stays aligned with an ordinary local build. Normal completion removes the artifact immediately. Quidra also keeps a per-user lease registry and checks it on every later Quidra startup: stale unlocked artifacts left by a killed compiler process are removed from any project directory, while artifacts belonging to another still-running invocation are left untouched. On POSIX the execution child inherits the lease lock, so killing the parent compiler cannot make an active executable look stale; Windows additionally prevents deletion of the running executable itself.


## Interactive REPL

Run `quidra` with no arguments from a terminal to start the native REPL. REPL submissions are lowered to LLVM IR and executed by LLVM ORC JIT; they are not interpreted and do not invoke Clang/linker for each executable submission:

```text
$ quidra
Quidra 0.5.0
>>> int(1) + 2
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

The current REPL still recompiles accumulated accepted source, but the growing root source is compiled from an in-memory overlay instead of being written and read back through a temporary source file on every submission. Its stable virtual source path still drives relative imports, lock checking, and diagnostics. It does not silently replay observable effects. During reconstruction, prior `print` operations are suppressed, including output reached through user-function calls. If a submission may execute external or nondeterministic operations such as file/environment access, input, time, random, process, HTTP or package-native I/O, CLI reads, the session arms a conservative replay barrier before native execution. Further submissions are rejected with `REPL_REPLAY_UNSAFE` until `:reset`, even when the effectful submission later fails at runtime, because the external effect may already have happened.

`:help` lists REPL commands, `:type expression` prints the statically checked type, `:reset` clears accepted session state and the replay barrier, and `:quit` or `:exit` exits. Ctrl-D exits normally; Ctrl-C cancels the current input and keeps the session.

When standard input is not a TTY, bare `quidra` does not implicitly consume it as a REPL. `quidra repl` explicitly starts the same REPL and is useful for automated tests.

## C interoperability

The core C FFI is intentionally narrow and explicit:

```quidra
extern int c_abs(int value) = "llabs"
print(c_abs(-42))
print(NL)
```

Results are limited to ABI-stable scalar values or `void`. Scalar/bool parameters cross by value. Capture-free `fn` values can cross as explicit callback pointers when their signature uses only the ABI-stable callback scalar subset (`int32`, `uint32`, `int`, `uint64`, `float32`, `float`) and `void` where applicable; there is no hidden closure environment or callback allocation. Managed text/binary input uses explicit storage borrows: `const string &` and `const bin &` are read-only, while `bin &` is an explicit mutable byte borrow. Tensor input uses the same explicit authority model: `const tensor<T> &` is a read-only opaque tensor borrow and `tensor<T> &` is a mutable opaque tensor borrow. String/bin borrows lower to a `(data pointer, uint64 byte length)` pair; tensor borrows lower to one opaque handle defined by `quidra/native_extension.h`. Native code must not retain a borrowed pointer or tensor handle after the call. Mutation is permitted only through a mutable borrow and the corresponding native-extension API; tracked tensors deliberately reject raw mutable CPU access so native code cannot bypass autograd silently. A borrowed `bin` must be byte-aligned (`len(value) % 8 == 0`); otherwise the call fails deterministically. Quidra does not expose a pointer-only C-string contract, infer ownership transfer, or infer foreign failure from `errno`/null. Each external C symbol may be bound by only one `extern` declaration in a compilation. `main`, the compiler-owned `n_*` mangling namespace, and the implementation-owned `quidra_*` / `__quidra_*` C symbol namespaces are reserved.

Packages may own native implementation components instead of moving performance-sensitive domain code into Core. Package metadata can declare package-owned native sources and a platform-selected native library; AOT/direct/run build paths consume them automatically, while REPL/JIT loads package native libraries and compiles declared native sources to temporary objects before resolving package externs. Native package code uses the installed `quidra/native_extension.h` API and treats Quidra tensor handles as opaque. Core-private runtime/device structs are not a package ABI.

## CLI

```bash
quidra
quidra repl
quidra lsp
quidra install quidra-dnn
quidra install quidra-dnn@<release-version>
quidra install ./my-package
quidra remove quidra-dnn
quidra list
quidra package-info quidra-dnn
quidra package-info quidra-dnn --json
quidra package sync ./my-package
quidra package validate ./my-package
quidra lock program.qui
quidra lock program.qui --check
quidra package-path
quidra gpu
quidra info
quidra --version
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

Released packages are installed from immutable `vMAJOR.MINOR.PATCH` tags, never
from `main`, `develop`, or another moving branch. A canonical first-party
distribution name such as `quidra-dnn` resolves to `quidra-lang/dnn`; with no
version written, Quidra chooses the newest released tag compatible with the
running compiler. The legacy short spelling `dnn` remains accepted as a
compatibility alias.
`quidra.package` records the package version and its
`requires.quidra`/package dependency ranges. Local directory installation and
`QUIDRA_PACKAGE_PATH` remain available for development.

`quidra lock FILE.qui` records each resolved package's version and SHA-256 in
`quidra.lock`; normal compilation verifies both. See
[Package management](docs/packages.md) for the complete contract.

### Debugging

`quidra build --debug` asks the native Clang driver for an unoptimized, frame-pointer-preserving debug build and disables link-time dead stripping for that build. `quidra debug FILE.qui` launches LLDB or GDB (or `QUIDRA_DEBUGGER`) against that executable. Debug builds emit Quidra-aware DWARF for source files, functions, statement locations, scalar parameters, and scalar locals; release builds omit that debug metadata.

## Documentation

- [Development and release workflow](docs/development.md)
- [Package management](docs/packages.md)
- [Language semantics](docs/spec/language.md)
- [Numeric types and bin](docs/spec/numeric-and-bin.md)
- [Grammar](docs/spec/grammar.ebnf)
- [LLM guide](docs/spec/llm-guide.md)
- [Architecture](docs/spec/architecture.md)
- [Diagnostics](docs/spec/diagnostics.md)
- [Source patch schema](docs/spec/patch-schema.md)

## Standard namespaces

Standard namespaces keep common capabilities discoverable without consuming
unstable global names. They are reserved, always visible, and are not imported.
Source-file imports stay explicit: quoted targets are source modules and
unquoted non-standard targets are installed packages.

The reserved standard namespaces are `cli`, `file`, `environment`, `test`, `time`, `gpu`, `task`, `atomic`, `autograd`, `ref`, `reflect`, `random`, `process`, `map`, `set`, `json`, `http`, `tensor`, `exact`. Generic mathematical semantics are provided by the explicit `math` package and therefore require `import math`.

Only referenced standard implementations are linked into a program. The
namespaces are grouped by semantic role rather than exposed as unrelated global
functions:

| Area | Namespaces |
| --- | --- |
| Tensor execution | `tensor`, `autograd`, `gpu` |
| Exact numeric extension | `exact` |
| Structured data | `map`, `set`, `json`, `ref` |
| I/O and host interaction | `file`, `environment`, `process`, `http` |
| Explicit state and coordination | `random`, `time`, `task`, `atomic` |
| Tooling and program structure | `cli`, `test`, `reflect` |

The same semantic-compression rule applies here: stateful or effectful behavior
is named by its owning namespace, representation changes stay explicit, and
fallible calls can use the ordinary fail-fast success context unless the program
deliberately retains the `error` channel.

Core intentionally stops below domain frameworks and general numerical
libraries:

```text
Quidra Core
├── tensor / autograd / device   storage, layout, generic execution mechanics, gradients
└── standard foundations

Official source packages
├── math                mathematical functions, reductions, linear algebra, and numerical algorithms
├── nn                  architecture-independent neural-network mechanisms, layers, losses, optimizers, and training semantics
├── vision              image codecs, image processing, computer vision
├── video               video decoding and video processing
└── dnn                 concrete deep-neural-network model compositions built on NN and Math
```

Core, Math, NN, Vision, Video, and DNN use one lockstep `MAJOR.MINOR.PATCH`
version. A first-party package release never advances independently of Core.

These packages use the ordinary package/native-extension system and receive no
compiler-specific name handling. Their C++/CUDA/vendor-backend implementations
remain package-owned.

For the complete standard-library contracts and examples, see
[Language semantics](docs/spec/language.md).

## License

MIT
