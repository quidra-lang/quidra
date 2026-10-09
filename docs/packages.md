# Package management

Quidra packages are source packages with a `main.qui` entrypoint. Released
packages additionally carry a `quidra.package` manifest and are installed from
immutable release tags.

## First-party dependency layers

Quidra's official computational packages follow a strict dependency-layer model.
A higher layer may depend on lower layers; a lower layer must not depend on a
higher layer. Sibling repositories in the same layer are independent by
default and must not import one another merely to reuse domain semantics.

Core, Math, NN, Vision, Video, and DNN also use one synchronized release-train
version. On `develop`, every first-party package's `package.version` must equal
Core's `project.version`. A coordinated release publishes the same immutable
`vX.Y.Z` across all six repositories; package versions do not advance
independently. Dependency ranges may remain compatibility ranges, but they must
admit that same shared version, and release validation uses same-version
dependency tags.

```text
Layer 4:  DNN
          |
Layer 3:  NN    Vision    Video
           \      |      /
Layer 2:         Math
                  |
Layer 1:         Core
```

The layers define semantic ownership, not implementation technology:

| Layer | Repository | Ownership |
| --- | --- | --- |
| 1 | Core | Language/compiler, tensor and autograd substrate, devices, execution, package/native-extension mechanisms, and domain-neutral optimization infrastructure. Core provides mechanisms, not package semantics. |
| 2 | Math | Generic mathematical semantics, including numerical operations, exact mathematical semantics, native implementations, autograd rules, and Math-owned compiler optimizations. |
| 3 | NN | Architecture-independent neural-network mechanisms: modules/layers, losses, optimizers, parameter/training semantics, and NN-owned rewrites or fused kernels such as Conv + BatchNorm + ReLU. |
| 3 | Vision | Image and computer-vision semantics such as resize, crop, color/image transforms, image codecs, and Vision-owned kernels or optimizations. |
| 3 | Video | Video/temporal media semantics such as frame/video transforms, sampling, codecs, and Video-owned kernels or optimizations. |
| 4 | DNN | Concrete deep-neural-network model compositions such as ResNet, ViT, UNet, YOLO, and multimodal/video models. DNN may compose NN, Vision, and Video without forcing those Layer-3 packages to depend on one another. |

The allowed dependency direction for these packages is:

```text
Layer 4 packages -> Layer 3 packages / Math / Core as needed
Layer 3 packages -> Math / Core
Math             -> Core
Core             -> no higher first-party layer
```

This is an upper bound, not a declaration that every allowed edge exists.
The current DNN manifest depends on NN and Math (with Core as the substrate);
it does not depend on Vision or Video.

Layer-3 sibling dependencies are forbidden by default. For example, NN does
not import Vision, Vision does not import NN, and Video does not import either
sibling just to reuse domain semantics. Cross-domain composition belongs in
Layer 4 or another explicitly higher-level package.

Optimization follows the same ownership boundary. Core may provide typed IR,
pattern/rewrite registration, graph analysis, dispatch, native-kernel hooks,
and other generic compiler mechanisms. The package that owns the semantics
owns the actual rewrite rules, numerical validity conditions, native/CUDA/Metal
implementation, and backend policy. Thus Conv + BatchNorm + ReLU fusion belongs
to NN, while a model-specific ResNet rewrite belongs to DNN and mathematical
kernels or rewrites belong to Math. Being performance-critical,
compiler-visible, differentiable, or GPU-backed is never by itself a reason to
move semantics into Core.

## Commands

```text
quidra install quidra-dnn
quidra install quidra-dnn@0.5.0
quidra install owner/repository
quidra install https://github.com/owner/repository.git@1.2.3
quidra install ./local-package
quidra remove quidra-dnn
quidra list
quidra package-info quidra-dnn
quidra package-info quidra-dnn --json
quidra package sync ./my-package
quidra package sync ./my-package --check
quidra package validate ./my-package
quidra package-path
quidra lock program.qui
quidra lock program.qui --check
```

Official first-party distributions use the `quidra-` namespace. A bare name
such as `quidra-dnn` resolves to `https://github.com/quidra-lang/dnn.git`;
the suffix is the import/repository identifier. The old short spelling `dnn`
remains accepted as a compatibility alias. Third-party packages use
`owner/repository` or an explicit Git URL until a registry is introduced.

Remote installation requires `git`. Quidra never installs from `main`,
`develop`, `feature`, or another moving branch. It enumerates exact stable
release tags of the form `vMAJOR.MINOR.PATCH`, checks each tag's package
manifest, and clones only the selected tag. No package install script is
executed.

A released package may attach a platform asset to the immutable source release:

```text
asset.linux-x86_64 = https://github.com/owner/package/releases/download/v1.2.3/package-linux-x86_64.tar.xz
asset.windows-x86_64 = https://github.com/owner/package/releases/download/v1.2.3/package-windows-x86_64.zip
```

The installer downloads only HTTPS assets, inspects the archive before
extraction, rejects absolute/traversing paths, links, and attempts to replace
`main.qui`, `quidra.package`, `.git`, or `quidra.lock`, then merges the
regular files into the package tree before publishing the install atomically.
`asset.default` is an optional fallback. Asset-backed installs additionally
require `curl` and `tar`; official Linux packages declare those runtime
dependencies.

When no package version is requested, Quidra selects the newest released tag
whose `requires.quidra` range accepts the running compiler. An explicit
version must exist as an exact release tag and must be compatible; otherwise
installation fails instead of silently selecting another version.

## Package manifest

Released packages contain `quidra.package` at the repository root:

```text
name = dnn
version = 0.5.0
repository = https://github.com/quidra-lang/dnn
description = Deep neural network models for Quidra
license = MIT
homepage = https://github.com/quidra-lang/dnn
requires.quidra = >=0.5.0 <0.6.0
requires.math = >=0.5.0 <0.6.0
requires.nn = >=0.5.0 <0.6.0
```

Package-to-package requirements use the same `requires.<import>` form. The
DNN example above follows the current layer graph: DNN depends on Math and NN,
not on sibling Layer-3 packages such as Vision or Video.

Versions use exact `MAJOR.MINOR.PATCH` Semantic Versioning. Requirement terms
are conjunctive and support `=`, `<`, `<=`, `>`, and `>=`. A released
package must declare `requires.quidra`. Its manifest version must exactly match
the release tag.

Official first-party computational packages are versioned in lockstep with
Core. Math, NN, Vision, Video, and DNN must carry the exact same
`MAJOR.MINOR.PATCH` value as the corresponding Core release, including when a
repository has no code change in that release. Their release workflows check
out first-party dependencies at that same version tag and reject divergence.

`description`, `license`, and `homepage` are optional descriptive metadata. `asset.<platform>` entries are optional immutable release payload URLs and are selected only for the running platform (or `asset.default` when present).
They do not participate in dependency resolution or execute any behavior.
`quidra package-info NAME` reads only the already-installed package; `--json`
provides stable machine-readable metadata without network access.

For a dependency other than Quidra, the current installer requires an already
installed compatible package and reports the required range when it is missing
or incompatible. Dependency resolution can become more automatic later without
changing the manifest format.

### Package-owned native sources

A package may keep native implementation beside its Quidra source. The manifest
uses named `native.source.*` entries and optional `native.pkg.*` pkg-config
dependencies. Sources may be common to every host or scoped to one canonical
host platform:

```toml
[native.source]
cpu = "native/cpu.cpp"

[native.source.macos-arm64]
metal = "native/metal.mm"

[native.source.linux-x86_64]
cuda = "native/kernels.cu"

[native.pkg]
codec = "libexample"
```

The generated `quidra.package` flattens platform sources as
`native.source.<platform>.<name> = path`. Only sources matching the current
host platform are compiled; common `native.source.*` inputs are always used.
Core treats these as build inputs, not as domain semantics. C, C++, and
`.s`/`.S` assembly sources use the host Clang toolchain; Objective-C++ `.mm`
is accepted on Apple hosts, and `.cu` sources use NVIDIA `nvcc`. `QUIDRA_NVCC` can select the CUDA compiler explicitly.
The CUDA toolkit root is resolved from `QUIDRA_CUDA_HOME`, `CUDA_HOME`,
`CUDA_PATH`, or the selected `nvcc` location so the package-owned object can
link the CUDA runtime. AOT and REPL/JIT use the same package source declarations.

This mechanism is intentionally domain-neutral: NN, DNN, Vision, Video,
scientific, and other packages own their kernels and third-party backend policy. Core only
compiles, links, and loads the declared native inputs. A direct run (`quidra FILE.qui`,
`quidra run`) caches the whole executable built from them, keyed by their contents among
everything else the build depends on; the inputs themselves are compiled again whenever a
build runs.

Package kernels that need to share Core's same-device asynchronous ordering may
query the borrowed backend-native queue/stream with
`qcore_device_queue_handle(device)`. Metal exposes Core's
`MTLCommandQueue`; CUDA/HIP use their backend default stream, represented by
native handle 0. The execution handle is mechanism only: packages still own
operation semantics, kernels, and backend policy.

On Metal, Core keeps one open command buffer per device and waits only where
the host reads device data (`.item()`, `.cpu()`, readbacks, `gpu.sync`,
synchronized time samples, program exit). Package kernels can append to it
instead of committing and waiting on command buffers of their own; the calls
are declared and specified in `include/quidra/native_extension.h`:

- Package encodes follow one ownership protocol: take every handle and
  status slot, open the scope, set state, dispatch, close.
  `qcore_device_encode_begin(device)` holds Core's stream for the calling
  thread (it returns 0 when the device has no Core stream: CUDA, HIP, the
  fake backend, `QUIDRA_METAL_STREAM=0`; the package then submits as before).
  While the stream is held Core commits nothing, so tensor handles and
  status slots taken now do not split Core's batch, and the work Core may
  need for them (a copy-on-write detach, an upload, new output storage, a
  status page clear) is encoded in program order on Core's own encoders.
  `qcore_device_compute_encoder(device)` then opens the encoder scope and
  returns Core's open compute encoder (set the pipeline state and every
  binding the kernel uses, dispatch, never end it), or
  `qcore_metal_command_buffer(device)` returns Core's open command buffer
  for encoders of the package's own (end them before closing). Exactly one
  `qcore_metal_note_work(device, dispatches, bytes)` closes the scope and
  the hold, also after a failed encode and when no scope was opened. Defer
  status slots and attach autograd after it. Without
  `qcore_device_encode_begin`, opening a scope takes the hold itself, and
  handles taken before it were lent outside a hold, which commits Core's
  batch at each lend.
- Inside the scope Core performs no encoding, upload, copy, fill, blit or
  copy-on-write detach on that stream: a Metal encoder cannot save and
  restore its pipeline state and bindings, and an encoder Core ended would
  be released under the package. Only lookups that need no Core work are
  served there (tensor metadata and offsets, a const handle of a tensor
  that is not a still-shared unified-memory view, a mutable or output handle
  of a dense tensor at offset 0 that covers the storage it alone owns (a
  view of part of its storage needs a copy), the queue and native device
  handles, scratch buffers, deferring or releasing a status slot, completion
  callbacks, counters). A handle that needs Core work, a status slot, a
  custom autograd attach involving the device, and a nested scope or hold
  are protocol violations. During the whole hold Core neither commits nor
  waits on the stream, so a flush, a wait, a status wait and a mutable CPU
  pointer (`qcore_tensor_cpu_data`) of a unified-memory view that device
  work may still read are violations too; take mutable CPU pointers before
  `qcore_device_encode_begin`. A hold covers one device: a call that needs
  another device's stream (its tensors' handles, its queue, scratch, status
  slots, completion callbacks, flush or wait, an attach involving it) is a
  violation, so a thread never waits for a second stream while it keeps
  one locked. Whether a call is refused depends only on the program's
  state, never on the buffer pool, the upload mode or GPU timing.
- Core does none of the work a violation asks for: the call fails (0, NULL,
  nonzero, or -6 from an autograd attach), and `qcore_metal_note_work`
  discards the hold's command buffer (nothing of it runs) and stops the
  program with `Quidra runtime error[GPU_SCOPE] at FILE:L:C` (status 101)
  where Core called the package code: at the user's statement that made
  the extern call, or at the `backward()` call whose custom autograd
  backward callback broke the protocol. In a completion callback or a
  warm-up, which run outside any Quidra statement, the report names the
  program's root file without a line and the process ends at once (`std::_Exit`, exit handlers skipped) with
  the stream still held, so the program never runs on without the
  discarded work. In a `task.all` task the report keeps its location, and
  the process ends at once the same way, so the other tasks never run on
  without that work either. A hold or scope must end before the package
  code that began it returns (the extern call, or a backward, completion
  or warm-up callback). Core checks where control comes back: at that
  thread's next Quidra statement, at the start of a `task.all` call and
  the end of each of its tasks, when a backward callback returns, after
  each completion callback and warm-up, and at exit. A hold found open
  there is ended the same way and the program stops with `GPU_SCOPE` at
  the statement that was running, at the `task.all` call (whose tasks
  would otherwise wait for the held stream), at the `backward()` call, or
  naming the root file after a completion callback, a warm-up or at exit (ending at
  once there and on a `task.all` task's thread). Core work the thread asks
  for on the held stream before that point, for example later in the same
  statement, is refused where the hold refuses it (any work inside the
  scope, a commit or wait during the hold) and stops the program with
  `GPU_SCOPE` right there; other threads' work on the stream waits until
  the hold has been ended. The runtime counters `package_encode_holds`,
  `package_encode_scopes` and `package_scope_violations` count holds,
  scopes and violations.
- Inside a hold never wait, flush, synchronize or commit a command buffer
  of your own, and bind buffer handles taken in it only in that hold's
  scope (Core records them as used by its open command buffer, so the host
  can read them without a device synchronization once it completed). A
  queue handle taken in a hold makes the hold's end commit Core's open work,
  so the package's own command buffers on it run after the hold's work.
  Only a handle taken in the hold does that: after a hold that took none,
  Core's command buffer stays open, and a command buffer the package
  commits on a queue handle taken before `qcore_device_encode_begin` (or
  cached from an earlier call) runs before the hold's work and reads stale
  data. Call `qcore_device_queue_handle` again during the hold (a lookup,
  also allowed in the scope) before committing command buffers of your own
  that use the hold's results.
- `qcore_device_flush`, `qcore_device_wait` (before a host read of
  package-owned device memory) and `qcore_device_on_complete` (for example to
  release scratch once all queued work, including the package's own command
  buffers, has completed; registered in a hold, the callback joins the
  hold's command buffer and runs after its work, never at once). Completion
  callbacks run one at a time on a
  Core-owned queue, not inside Metal's completion handlers, and a
  synchronization returns only after the callbacks of the work it waited for
  have run. A callback may use the stream calls listed here but must not wait
  for another callback. Completion handlers a package adds to its own Metal
  command buffers must not call them.
- Status words for checked kernels: `qcore_device_status_slot` hands out a
  zeroed 32-bit word to bind (take it before the encoder scope opens).
  After encoding (or committing) the kernel, the
  package either defers the check with `qcore_device_defer_status`, which
  reports the message and the queuing statement at the next synchronization
  point without a host wait now, reads it at once with
  `qcore_device_status_wait`, or discards it with
  `qcore_device_status_release`.
- Scratch from `qcore_device_buffer_allocate` holds unspecified bytes and no
  queued GPU work uses it when it is returned, so on Metal the package may
  write it through shared storage at once. A tensor's storage may still be in
  use by queued Core work when its handle is lent: host access to it needs
  `qcore_device_wait` first.
- Write-only outputs: for a kernel that stores every element, take the output
  with `qcore_tensor_output_handle` (no fill, no per-element bookkeeping; reads
  fail as uninitialized until written) and call `qcore_tensor_mark_written`
  after a successful encode.
- Warm-up: `qcore_register_warmup(fn)` (for example from a static
  initializer of the package's native code) makes Core call `fn(device)` once
  per GPU device the program uses, on a Core background thread, when the
  program first allocates on it. Compile the package's kernels there, for the
  device `qcore_device_native_device(device)` returns; the first GPU step then
  finds them compiled. The kernel cache must be thread-safe, and `fn` must
  not encode or wait for device work.

Packages that still commit their own command buffers on
`qcore_device_queue_handle` keep working: lending a native handle outside a
hold commits Core's open work first, and every later synchronization covers
the package's command buffers. `QUIDRA_COUNTERS=<file>` reports per program
step how many command buffers, waits (with their source sites), fills,
uploads, allocations and package queue borrows a program pays.

Package autograd callbacks are attached with the
`qcore_tensor_attach_custom_autograd*` family in
`include/quidra/native_extension.h`. Saved tensors are value snapshots, but
Core may keep a dense saved tensor by sharing its storage copy-on-write
instead of copying it. Writes through Core detach a shared storage first,
including `qcore_tensor_cpu_data` and `qcore_tensor_device_handle` called
after the attach. A pointer or handle obtained before the attach call is not
detached, so finish every write through it before attaching, or save a
separate tensor.

A callback attached with `qcore_tensor_attach_custom_autograd_masked`
receives, besides the gradient tensors, a `needed` byte per input: Core marks
the inputs through which a selected backward target is reachable, passes
`NULL` for every other gradient, and does not call the callback at all when no
input needs one. Callbacks attached with the older functions keep receiving
every gradient, zero-filled. At attach time a masked callback may declare, per
input, that it writes the whole gradient whenever it is requested
(`full_writes`). A declared gradient is a write-only output: its contents are
unspecified when the callback starts (Metal and the fake test GPU skip its
zero fill; `QUIDRA_GPU_ZERO_FILL=always` restores it), the callback must store
every element, and it must report `fully_written[i] = 1` after each successful
call, or the backward fails. Undeclared gradients always arrive zero-filled.

### CPU parallelism in package kernels

CPU kernels that want more than one core use Core's
`qcore_parallel_for(begin, end, grain, body, context)` instead of creating
threads or calling Grand Central Dispatch themselves. Core schedules the work
on GCD on Apple platforms and on one persistent thread pool elsewhere.

The call is deterministic by construction:

- `[begin, end)` is cut into the fixed chunks
  `[begin + k*grain, min(begin + (k+1)*grain, end))`. The chunks depend only on
  the three arguments, never on the thread count, and `body` runs exactly once
  per chunk.
- A body may write only the outputs of its own range and must compute each one
  exactly as a serial loop would. Partition independent outputs (output
  channels, rows, elements), never the terms of one sum. A reduction may use a
  fixed number of chunks whose partial results the caller combines in a fixed
  order afterwards. Under this rule the results are bitwise identical for
  every thread count.
- A call nested inside a body, a call made inside a `task.all` operation, a
  single-chunk range and a thread count of 1 all run the chunks in ascending
  order on the calling thread.
- A body returns 0 or a positive package error code. Once a chunk fails, later
  chunks that have not started are skipped, and the call returns the code of
  the failing chunk with the lowest index, which does not depend on
  scheduling. An escaping C++ exception is caught and reported as
  `QCORE_PARALLEL_CALLBACK_EXCEPTION`; invalid arguments return
  `QCORE_PARALLEL_INVALID_ARGUMENT`. Negative results are reserved for Core: a
  body that returns one fails its chunk with
  `QCORE_PARALLEL_INVALID_BODY_RESULT`, so a body result never looks like
  another Core status.
- Bodies may call only the `qcore_*` functions that read data without
  changing Core state: a nested `qcore_parallel_for`,
  `qcore_parallel_thread_count`, `qcore_native_abi_version`,
  `qcore_execution_policy_get`, `qcore_execution_is_deterministic`, and the
  tensor metadata queries `qcore_tensor_dtype`, `qcore_tensor_device`,
  `qcore_tensor_rank`, `qcore_tensor_extent`, `qcore_tensor_element_count`,
  `qcore_tensor_is_contiguous`, `qcore_tensor_backend`,
  `qcore_tensor_backend_device_index` and `qcore_tensor_device_offset_bytes`.
  Any other `qcore_*` call from a body, including `qcore_tensor_cpu_data` and
  `qcore_tensor_device_handle`, stops the program with
  `Quidra runtime error[PARALLEL_BODY]` (exit status 101) at every thread
  count, before it touches Core state. Take data pointers and handles before
  the call and pass them through `context`.
- Bodies on other threads run under the caller's floating-point environment
  (rounding mode, and flush-to-zero/denormal controls where the platform's
  `fenv_t` carries them, as on arm64 and x86-64), and exception flags they
  raise are raised on the caller before the call returns. A body that changes
  the environment must restore it before returning.
- Bodies may run on threads with a smaller stack than the caller's (512 KiB
  for Grand Central Dispatch workers). Keep large scratch buffers off the
  stack: allocate them before the call and pass them through `context`.
- Test every parallel kernel both with `QUIDRA_CPU_THREADS=1` and with more
  than one thread (for example with the variable unset). With one thread every
  chunk runs on the caller, so a body that overflows a helper's stack can pass
  there and fail only with more threads.

```cpp
struct Scale { const float* in; float* out; float factor; };

static int scale_elements(uint64_t begin, uint64_t end, void* raw) {
    const auto& job = *static_cast<const Scale*>(raw);
    for (uint64_t i = begin; i < end; ++i) job.out[i] = job.in[i] * job.factor;
    return 0;
}

// ...
Scale job{input_data, output_data, 3.0F};
int status = qcore_parallel_for(0, count, 16384, scale_elements, &job);
```

`QUIDRA_CPU_THREADS=N` (an integer from 1 to 256) limits each
`qcore_parallel_for` call to `N` threads including the caller; a value above
the hardware thread count is reduced to it. `QUIDRA_CPU_THREADS=1` makes every
call fully serial, with no thread hand-off. Unset or empty means the hardware
thread count. The variable is read and validated by the first
`qcore_parallel_for` or `qcore_parallel_thread_count` call in the process,
wherever that call is made (at top level, nested, or inside `task.all`); any
other value stops the program there with `Quidra runtime error[CPU_THREADS]`
(exit status 101). A program that must not stop part way, such as a benchmark
harness before a timed run, reads `qcore_parallel_thread_count()` first (see
below) so that the value is checked before any output or timed work. The
variable does not limit `task.all`, which starts its own threads, and package
kernels called inside a `task.all` operation run serially anyway.
Core's own CPU tensor kernels stay single-threaded.

`qcore_parallel_thread_count()` returns the number of threads a call from the
current context may use: the effective `QUIDRA_CPU_THREADS` value, or 1 inside
a body or a `task.all` operation. It is an upper bound; a loaded system may run
a call on fewer threads, which never changes its results. Quidra code, such as
a benchmark harness that records the setting and wants it validated up front,
can read it through an `extern` binding:

```quidra
extern nat64 cpu_threads() = "qcore_parallel_thread_count"

print(cpu_threads())
```

### Package `project.toml`

`quidra.package` recognizes a closed set of keys, and an unknown key is an error
rather than a warning, so a package that added one could not be read by an
already-released compiler. Metadata beyond that set therefore lives in an
optional `project.toml` next to it, which older compilers simply never open:

```toml
[package]
name = "quidra-dnn"
import = "dnn"
display_name = "Quidra DNN"
version = "0.5.0"
repository = "https://github.com/quidra-lang/dnn"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
```

It separates four identities that `quidra.package`'s single `name` cannot carry
at once:

| Field | Meaning |
| --- | --- |
| `name` | distribution/package-manager identity, e.g. `quidra-dnn` |
| `import` | the identifier `import NAME` binds, and the installed directory name |
| `display_name` | human-facing title, e.g. `Quidra DNN` |
| `repository` | where releases come from |

The distribution name is used by package-manager commands such as install,
list, remove and package-info. The import name is the constrained one: it must
be a valid Quidra identifier, so it cannot contain a hyphen, and it equals
`quidra.package`'s legacy `name` plus the installed directory name. Keeping
that legacy field as the import identifier lets older compilers continue to
read new package releases.

`requires.abi` cannot appear in `quidra.package`, because every `requires.<dep>`
value there is parsed as a version range. It lives here instead, and it is an
exact match against the compiler's ABI version rather than a range: the ABI
number changes only when the contract itself changes, so a package either speaks
it or does not.

When `project.toml` is present the compiler reads it alongside `quidra.package`
and rejects the package if the two disagree on the import name or the version,
which is what makes `quidra.package` safe to generate from it.

`quidra package sync [DIR]` is the canonical generator for the compatibility
manifest. It derives `quidra.package` from `project.toml`, including package
identity, release asset URLs, native sources/pkg-config dependencies and package
requirements. `--check` verifies the generated file without writing;
`quidra package validate [DIR]` is the equivalent validation-only command.
Package repositories therefore do not need a second TOML parser merely to keep
these two metadata files synchronized.

The optional `[compiler.extension]` table maps a package-owned logical extension
name to a package-relative declarative descriptor. Core validates that each
descriptor stays inside the package and exists as a regular file. A descriptor
starts with `[extension]`, `version = 1`, and the generic
`phase = "tensor-region"`. Core snapshots descriptor contents during import,
carries them through specialization/checking into typed IR, and routes candidate
tensor regions by the source package that owns each function. Import aliases do
not affect ownership. Regions also expose whether they reach backward and
whether they may require higher-order backward, allowing package policy to
conservatively avoid a first-order-only implementation. Descriptors are data,
not in-process native compiler plugins; domain patterns and backend policy remain
package-owned. Core validates only the descriptor structure required by this
generic substrate: every `[operation.*]` table has a non-empty function target,
and every `[fusion.*]` sequence references declared operation IDs without empty
entries. A fusion may optionally name `replacement = "operation_id"`; that id
must name another declared operation in the same extension. Core may apply such
a replacement only through a domain-neutral call contract. The initial rewrite
requires a same-basic-block pure tensor chain, unary tail operations, no
observable use of eliminated intermediate SSA values, a replacement parameter
contract identical to the first operation, and a result type identical to the
last operation. If any condition is not proven, the fusion remains analysis
metadata only. Domain meaning, the replacement implementation, backend policy,
and numerical validity stay with the owning package.

Local directory installation remains available for package development:

```text
quidra install ./my-package
quidra install ./legacy-package --name legacy_package
```

A local package with a manifest is compatibility-checked. A legacy local package
without a manifest can still be installed explicitly and is treated as
unversioned. Existing packages are replaced by local installation only with
`--force`. Development checkouts can also be exposed through
`QUIDRA_PACKAGE_PATH` without copying them into the package store.

## Installed store and lockfile

The default installed source store is:

```text
~/.quidra/packages/<import-name>/
```

`quidra list` prints package versions when a manifest is present.
`quidra package-path` prints the default store location.

`quidra lock FILE.qui` writes `quidra.lock` version 3. Every direct or
transitive package reached by the program's import graph records both identities:

```text
quidra-lock-v3
quidra-dnn dnn 0.5.0 <sha256>
quidra-vision vision 0.5.0 <sha256>
```

The columns are distribution name, import name, version and content hash.
An unversioned local development package uses `-` in the version column.
Version-1 and version-2 lockfiles remain readable for compatibility; rewriting
the lockfile upgrades it to version 3. The
SHA-256 covers the package's regular-file tree while deliberately excluding
`.git` metadata. Compilation checks both the recorded version and content
hash, so a lockfile identifies the exact package contents rather than merely a
compatible release.

`quidra lock FILE.qui --check` is the non-writing CI form.

## Release identity

A package release is identified by all of the following agreeing:

- `quidra.package` version `X.Y.Z`;
- immutable Git tag `vX.Y.Z`;
- the source tree referenced by that tag;
- the package's declared Quidra and package dependency ranges.

Branches are development locations, never install identities.
