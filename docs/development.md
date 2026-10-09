# Development and release workflow

This document is the canonical workflow for developing and releasing Quidra.
It is the single authority for the release procedure: `RELEASING.md` at the
repository root points here and deliberately restates none of it.

It is intentionally operational: an engineer or LLM given an instruction to
release Quidra should execute the complete applicable procedure below rather
than only describing it. Do not stop after writing a plan, opening a pull
request, merging a branch, or creating a tag; continue through release
verification and post-release `develop` preparation. Ask only when a real
merge conflict or an ambiguous destructive choice cannot be resolved from the
repositories. Always fetch the current remote refs first: never release from a
stale local SHA, never reset away newer remote work, and never move an
existing release tag.

## Release command scope

An unqualified release request is a coordinated Quidra release, not a Core-only
release. Instructions such as "release Quidra", "release", "リリースして", or
equivalent wording that does not name a repository put all eight official
repositories in scope:

1. Quidra Core (`quidra-lang/quidra`)
2. Quidra Math (`quidra-lang/math`)
3. Quidra NN (`quidra-lang/nn`)
4. Quidra Vision (`quidra-lang/vision`)
5. Quidra Video (`quidra-lang/video`)
6. Quidra DNN (`quidra-lang/dnn`)
7. Quidra Playground (`quidra-lang/playground`)
8. Quidra Website (`quidra-lang/website`)

Do not silently stop after releasing Core when the request is unqualified.
A repository-specific instruction such as "release Core", "release Math",
"release NN", "release DNN", "release Vision", "release Video",
"release Playground", or "release Website" narrows the scope to that repository.

For a coordinated release, use the canonical order shown later in this document:
Core -> Math -> NN -> Vision -> Video -> DNN -> Playground -> Website. Math is
released immediately after Core; the three Layer-3 packages then establish their
released states before the Layer-4 DNN package is released. The fixed sibling
order keeps the procedure deterministic for both humans and automation.

## Permanent branch roles

The normal repository has two permanent branches:

- `main` is the latest published stable release once the repository has published at least one release. Before a newly created first-party repository's first release, `main` may contain bootstrap history only; that state is not a release or installation identity.
- `develop` is the integration branch for the next release.

Routine development is performed directly on `develop`. Do not create new
work branches for ordinary changes, and never use any non-`main`/`develop`
ref as a release or installation identity.

Do not delete and recreate `develop` after a release. It remains the
long-lived development branch.

## Versioning

Quidra uses Semantic Versioning. While the language is below 1.0:

- `0.X.0` is appropriate for new language features, standard-library
  capabilities, or material semantic changes.
- `0.X.Y` is appropriate for compatible fixes and maintenance of the same
  minor line.
- `1.0.0` is reserved for the first explicitly stable language contract.

Core and all first-party computational packages (Math, NN, Vision, Video, and
DNN) use one synchronized release-train version. Their `project.toml` package
versions must exactly equal Core's `[project].version` on `develop`, and every
coordinated release publishes the same immutable `vX.Y.Z` across those six
repositories. Package versions never advance independently.

A version number in a development branch is not itself a release. Only an
immutable `vX.Y.Z` tag on `main` is a released version.

Versions are plain `MAJOR.MINOR.PATCH`. A `-dev` or `-rc` suffix is rejected by
the semantic-version parser and by CMake, so a development branch carries an
ordinary version number, not a decorated one.

## Single source of truth for project metadata

`project.toml` at the repository root is the single source of truth for project
metadata. It owns the project name, CLI name, source extension, tagline and
version; the ABI, IR and package-schema numbers; the
repository URLs; the backend id/display/kind table; the supported operating
systems and targets; and the toolchain minimums. Nothing else in this
repository may declare one of those values independently.

Everything else is derived from it:

- `python3 scripts/sync_metadata.py` regenerates `quidra.manifest.json` and the
  Quidra version printed in the README REPL transcript.
- CMake reads the version out of `project.toml` before `project()` and
  generates `include/quidra/project.hpp` at configure time.

Run `python3 scripts/sync_metadata.py` after every edit to `project.toml`.
`python3 scripts/sync_metadata.py --check` writes nothing and exits non-zero
when a derived file disagrees.

`python3 tests/metadata_ssot_tests.py` runs that check and additionally proves
that the files which cannot be generated still agree with `project.toml`: the
CMake and Debian toolchain minimums, the README build-requirement prose, the
backend display names compiled into `src/device_backend.cpp`, the backend ids
documented in the manifest, and the repository the installer script downloads
from. It also fails if the version appears anywhere it would be a second
definition rather than an example. It runs as the `quidra_metadata_ssot` test,
as a no-build `metadata` job on every push and pull request, and again before a
release tag is created.

The README build-requirement list restates the `[dependencies]` minimums in
prose, because build requirements have to be readable without running a script.
That is the only permitted restatement, and the check above verifies it rather
than trusting it.

Benchmark metadata is deliberately outside this. `benchmark/template/` is
copied whole into the measurement sandbox and must stay immutable during a
frozen measurement window, so it owns its own metadata under
`benchmark/template/config/`.

## Environment variables

Core reads the process environment only through `src/platform/environment.hpp`:
`quidra::platform::environment_value(name)` (no value when unset) and
`quidra::platform::environment_has(name)`. They read through `_dupenv_s` on
Windows, where the CRT deprecates `std::getenv` (C4996, an error under `/WX`),
and through `std::getenv` elsewhere. They only read: what an unset, empty or
malformed value means is decided where the variable is used, and the table
below records that decision for every variable. "Once" means that the first use
reads the variable and the process keeps the result.

The "Run cache key" column says how the key of a cached run
(`quidra FILE.qui`, `quidra run`) covers each variable, as
`src/toolchain/compile_environment.hpp` classifies it: `value` (the key
holds the variable's value, set or not), `effect` (the variable only steers
a search; the key holds what the search found, and a cached run repeats the
search) or `no` (it never changes what is built). The table and the header
must agree (`check_cache_key_environment`), so a variable cannot be added to
either without a class.

`tests/metadata_ssot_tests.py` fails when C, C++ or Objective-C++ code under
`src/`, `include/` or `tests/`, outside that header, names an environment
reader: `getenv` and its `secure_`, `_w` and `_s` forms, `_dupenv_s`,
`GetEnvironmentVariable`, `GetEnvironmentStrings`, an `environ` array,
`_NSGetEnviron` or `NSProcessInfo`'s `environment`. A name counts whether it is
called or not (`&std::getenv` does); comments, string literals and longer names
such as `fegetenv` do not. The test also fails when this table and the
`QUIDRA_*` names quoted in `src/` and `include/` disagree.

| Variable | Read by | Values | Run cache key |
|---|---|---|---|
| `QUIDRA_PACKAGE_PATH` | Compiler: each installed-package import | Package roots separated by `:` (`;` on Windows), searched in order before the user package store; empty entries are skipped. Unset: the store only. | `effect` |
| `HOME` (`USERPROFILE` on Windows) | Compiler: installed-package imports; package commands; the user cache directory (`HOME` only) | The user package store is `<home>/<store_relative>` (`store_relative` in `project.toml`). Imports skip the store when the variable is unset; when it is empty they look for `<store_relative>` relative to the current directory. Package commands fail ("cannot determine user home") when it is unset or empty. The user cache directory on macOS and Linux lies below `HOME` (see `QUIDRA_CACHE_DIR`); there is none unless `HOME` is an absolute path. | `effect` |
| `QUIDRA_ALLOW_FILE_PACKAGE_ASSETS` | Package commands: release asset URLs | `1` also accepts `file://` asset URLs. Anything else or unset: HTTPS only. | `no` |
| `QUIDRA_CLANGXX`, then `QUIDRA_CLANG` | Native code generation and linking | The first non-empty one is the C++ driver, used as given. Neither: the first of `clang++-20` ... `clang++-15`, `clang++` found on `PATH` (Windows: `clang++.exe`, `clang++-20.exe` ... `clang++-17.exe`). | `value` |
| `QUIDRA_NVCC` | Package `.cu` sources | Non-empty: the CUDA compiler; it must be executable or on `PATH`. Unset or empty: `nvcc` on `PATH`. | `value` |
| `QUIDRA_CUDA_HOME`, `CUDA_HOME`, `CUDA_PATH` | CUDA runtime library lookup | The first non-empty one, in this order, is the toolkit root and must be a directory. None: derived from the CUDA compiler's location. | `value` |
| `QUIDRA_LLI` | REPL JIT | Non-empty: the `lli` runner, on `PATH` or as a file path. Unset or empty: the platform search. | `no` |
| `QUIDRA_LLVM_LIBRARY` | In-process ORC JIT | Non-empty: tried first as the LLVM shared library, before the platform names. | `no` |
| `QUIDRA_ORC_RUNTIME` | REPL JIT (macOS) | Non-empty: the ORC runtime archive; it must be a file. Unset or empty: searched beside the LLVM installation. | `no` |
| `QUIDRA_COMPILER_RT_BUILTINS` | REPL JIT (macOS) | Non-empty: the compiler-rt builtins archive; it must be a file. Unset or empty: searched beside the LLVM installation. | `no` |
| `QUIDRA_DEBUGGER` | `quidra debug` | Non-empty: the debugger; it must be executable or on `PATH`. Unset or empty: `lldb`, then `gdb` on `PATH`. | `no` |
| `QUIDRA_RUNTIME_LIBRARY` | Native builds and `quidra run` | Non-empty: the runtime archive; it must be a file. Unset or empty: beside the `quidra` executable, then the installed `lib/quidra` layouts. | `value` |
| `QUIDRA_JIT_RUNTIME_LIBRARY` | REPL JIT | Non-empty: the shared JIT runtime; it must be a file. Unset or empty: beside the executable, then the installed layouts. | `no` |
| `QUIDRA_NATIVE_INCLUDE_DIR` | Package native sources | Non-empty: a directory that must contain `quidra/native_extension.h`. Unset or empty: the installation layout. | `value` |
| `QUIDRA_PKG_CONFIG` | Package native dependencies | Non-empty: the `pkg-config` program. Unset or empty: `pkg-config`. | `value` |
| `QUIDRA_AR` | `quidra build --lib` on Linux and Windows | Non-empty: the static archiver (`ar`, which reads MRI scripts, or `lib.exe`/`llvm-lib` on Windows). Unset or empty: `ar`, `lib.exe` on Windows. | `no` |
| `QUIDRA_LIBTOOL` | `quidra build --lib` on Apple platforms | Non-empty: the `libtool` program that merges the archive. Unset or empty: `libtool`. | `no` |
| `QUIDRA_CACHE_DIR` | User cache directory (`src/platform/user_cache_directory.hpp`): the run cache of `quidra FILE.qui` and `quidra run`, and `quidra cache clean` | Non-empty: the cache directory, made absolute against the current directory when it is relative. Unset or empty: the platform's user cache directory: `$HOME/Library/Caches/Quidra` on macOS, `$XDG_CACHE_HOME/quidra` or `$HOME/.cache/quidra` on Linux and other POSIX systems, `<Local AppData>\Quidra\Cache` (the known folder) on Windows. | `no` |
| `XDG_CACHE_HOME` | User cache directory on POSIX systems other than macOS | An absolute path: the cache directory is `$XDG_CACHE_HOME/quidra`. Unset, empty or relative: `$HOME/.cache/quidra`. macOS and Windows ignore it. | `no` |
| `PATH` | Executable lookup on POSIX | Searched in order for names without a directory; an empty entry means `.`. Unset: nothing is found. Windows uses `SearchPathW` instead. | `effect` |
| `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`, `OBJC_INCLUDE_PATH`, `OBJCPLUS_INCLUDE_PATH`, `LIBRARY_PATH`, `COMPILER_PATH`, `CCC_OVERRIDE_OPTIONS`, `PKG_CONFIG_PATH`, `PKG_CONFIG_LIBDIR`, `PKG_CONFIG_SYSROOT_DIR`, `PKG_CONFIG_ALLOW_SYSTEM_CFLAGS`, `PKG_CONFIG_ALLOW_SYSTEM_LIBS`, `NVCC_PREPEND_FLAGS`, `NVCC_APPEND_FLAGS`, `SDKROOT`, `DEVELOPER_DIR`, `MACOSX_DEPLOYMENT_TARGET`, `ZERO_AR_DATE`, `INCLUDE`, `LIB`, `LIBPATH`, `VCINSTALLDIR`, `VCToolsInstallDir`, `WindowsSdkDir`, `WindowsSDKVersion`, `UniversalCRTSdkDir`, `UCRTVersion` | Run cache key only: the native toolchain (clang, the linker, pkg-config, nvcc) reads them, Core never acts on them | Keyed by value on every platform, whether set or not, also those only one platform's tools read. | `value` |
| any name | `environment.get(name)` and `environment.has(name)`, each call | `get` yields `none` when the variable is unset; a value that is not valid UTF-8 text is a runtime text failure. `has` is true for every set variable, the empty string included. | `no` |
| `QUIDRA_ERROR_FORMAT` | Runtime: each failure report | `json`: each runtime failure and test assertion report is one line of JSON on stderr in place of the text (`docs/spec/diagnostics.md`, "Machine-readable runtime reports"). Anything else or unset: the text. | `no` |
| `QUIDRA_CPU_THREADS` | Runtime: `qcore_parallel_for`, once | An integer from 1 to 256 bounds the threads of each call and is reduced to the hardware thread count. Unset or empty: the hardware thread count. Anything else stops the program with `Quidra runtime error[CPU_THREADS]` (exit status 101). | `no` |
| `QUIDRA_BROADCAST` | Runtime: GPU broadcast and strided views, once | `strided` (also unset or empty): Metal and the test backend index strided views in the kernel. `gather`: host-built index maps. Anything else: a warning on stderr, then `gather`. | `no` |
| `QUIDRA_SAVED_TENSORS` | Runtime: tensors autograd keeps for backward, once | `cow` (also unset or empty): storage shared copy-on-write. `copy`: copied. Anything else: a warning on stderr, then `copy`. | `no` |
| `QUIDRA_AUTOGRAD_PRUNE` | Runtime: `backward`, once | `on` (also unset or empty): no gradient formula runs for values that reach no target. `off`: every node runs. Anything else: a warning on stderr, then `off`. | `no` |
| `QUIDRA_TEST_AUTOGRAD_STATS` | Runtime: `backward`, once | `1`: each backward prints `autograd stats: formulas N pruned M` on stderr. | `no` |
| `QUIDRA_COUNTERS` | Runtime counters, once at program start | Non-empty: per-step counter records are written to this file; `-` or `stderr` selects stderr. Unset or empty: off. | `no` |
| `QUIDRA_COUNTERS_STEP` | Runtime counters, once, only with `QUIDRA_COUNTERS` | `none`: a backward does not end a step. Anything else or unset: each backward ends a step. | `no` |
| `QUIDRA_UNIFIED_MEMORY` | Runtime: `.gpu(n)` and `.cpu()` on unified memory, once | `0`, `off` or `false`: every transfer copies. `upload`: only `.gpu(n)` may become a view. Anything else or unset: both may become views. | `no` |
| `QUIDRA_UNIFIED_MEMORY_STATS` | Runtime, once | Non-empty and not `0`, `off` or `false`: transfer counters are printed on stderr at exit. | `no` |
| `QUIDRA_GPU_ZERO_FILL` | Runtime: write-only GPU outputs, once | `always`: they are zero-filled. Anything else or unset: Metal and the test backend skip the fill. | `no` |
| `QUIDRA_METAL_STREAM` | Metal command stream, once | `0`: every operation commits its own command buffer. Anything else or unset: the stream batches work. | `no` |
| `QUIDRA_METAL_MAX_OPS` | Metal command stream, once | Decimal operation count after which the open command buffer is committed, at least 1. Default 100. | `no` |
| `QUIDRA_METAL_MAX_BYTES` | Metal command stream, once | Decimal byte count after which the open command buffer is committed, at least 1. Default 41943040 (40 MiB). | `no` |
| `QUIDRA_METAL_IDLE_OPS` | Metal command stream, once | Decimal operation count after which work is committed while the GPU is idle. Default 8. | `no` |
| `QUIDRA_METAL_IDLE_NS` | Metal command stream, once | Decimal nanoseconds after which work is committed while the GPU is idle. Default 50000. | `no` |
| `QUIDRA_METAL_POOL` | Metal buffer pool, once | `0`: no pool. Anything else or unset: freed buffers are recycled. | `no` |
| `QUIDRA_METAL_POOL_BYTES` | Metal buffer pool, once | Decimal byte capacity of each device's pool. Default 268435456 (256 MiB). | `no` |
| `QUIDRA_METAL_FAST_MATH` | Metal kernel compilation, once | `1`: fast math, whatever `QUIDRA_METAL_SAFE_MATH` says. Any other value or unset: `QUIDRA_METAL_SAFE_MATH` decides. | `no` |
| `QUIDRA_METAL_SAFE_MATH` | Metal kernel compilation, once | `0`: fast math. Any other non-empty value: safe math with precise functions. Unset or empty: the mode `metal_safe_math_default` in `src/device_compute.inc` selects. | `no` |
| `QUIDRA_METAL_UPLOAD` | Metal uploads, fills and copies, once | `blit`: blit encoders. Anything else or unset: host writes to idle buffers, compute kernels otherwise. Either way Core runs no transfer inside a package encode scope (a protocol violation, see `docs/packages.md`). | `no` |
| `QUIDRA_METAL_HOST_FILL_MAX` | Metal fills and copies, once | Decimal byte count up to which the host fills or copies an idle buffer itself instead of encoding a kernel. Default 65536. | `no` |
| `QUIDRA_TEST_POOL_POISON` | Metal buffer allocation, once (test hook) | An integer in C notation (decimal, `0x` hex or leading-`0` octal): every Metal buffer handed out is filled with its low byte first. No leading number: off. | `no` |
| `QUIDRA_TEST_METAL_FAIL_COMMAND` | Metal, test-GPU builds only, once | Decimal serial of a command buffer that is reported as failed. Unset: none. | `no` |
| `QUIDRA_TEST_FAKE_GPU_COUNT` | Test-GPU builds only: device enumeration, once | Decimal 0 to 16: that many fake GPUs replace the real devices. Any other set value: no GPU at all. Unset: the real devices. | `no` |
| `QUIDRA_TEST_FAKE_GPU_SYNC_FAIL` | Test-GPU builds only: each fake GPU synchronization | Set to anything, the empty string included: every synchronization fails. | `no` |
| `QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX` | Test-GPU builds only: each fake GPU synchronization | Decimal index of the GPU whose synchronizations fail. | `no` |
| `QUIDRA_TEST_FAKE_GPU_UNIFIED` | Test-GPU builds only, once | `1`: fake GPUs behave as unified memory, so transfers may become views. | `no` |

The Metal decimal variables (the `QUIDRA_METAL_*` counts, byte sizes and
nanoseconds, and `QUIDRA_TEST_METAL_FAIL_COMMAND`) are parsed with `strtoull`
in base 10: an empty value, or one with characters after the number, keeps the
default. As `strtoull` does, leading white space and a sign are accepted, a
negative value wraps modulo 2^64 (`-1` is 2^64 - 1) and a value above
2^64 - 1 becomes 2^64 - 1. `QUIDRA_METAL_MAX_OPS` (after its at-least-1 bound)
and `QUIDRA_METAL_IDLE_OPS` then keep only the low 32 bits: 2^32 gives 0 and
`-1` gives 2^32 - 1. With 0, the open command buffer is committed every time
work is added to it (`MAX_OPS`) or whenever work is added while the GPU is idle
(`IDLE_OPS`).

## The run cache in tests

Direct runs (`quidra FILE.qui`, `quidra run`) reuse executables from the
run cache, so every test suite or harness that runs programs that way sets
`QUIDRA_CACHE_DIR` to a fresh directory of its own: shell suites export
`QUIDRA_CACHE_DIR="$TMP/quidra-cache"` next to their `TMP`, Python suites
point it at a temporary directory. Each suite run then starts from an empty
cache, and its sequence of builds and hits is the same on every run; the
user's cache is never read or written. A test that must not use the cache
at all passes `--no-cache`, or sets a directory that cannot be created
(`QUIDRA_CACHE_DIR=/dev/null/x` runs uncached, silently).
`tests/metadata_ssot_tests.py` (`check_test_cache_isolation`) fails when a
suite that runs programs does not set the variable. `tests/run_cache_tests.sh`
checks the cache itself (`--full` adds every `value` variable of the table
above), and `quidra cache clean` empties a cache by hand.

## Normal development

1. Fetch the current remote `develop` HEAD. Never start from a remembered SHA.
2. Implement and test ordinary changes directly on `develop`.
3. Keep source, language specification, tests, examples, manifest data, and
   packaging behavior consistent.
4. Do not merge ordinary unfinished development into `main`.
5. Do not create release tags from `develop` or any other unreleased ref.

## Compiler source layout

`docs/spec/architecture.md` ("Compiler source layout") maps the compiler's
directories to its pipeline, lists the instruction domains and the
reserved terms, and states the include layering. New code goes to the
directory and the domain it belongs to: a new instruction is added to its
domain's IR header, emitter and lowering unit; a code or layout that the
compiler and the runtime share goes to `include/quidra/abi/`; a runtime
entry point is declared in `include/quidra/abi/runtime_entry_points.hpp`,
gets an entry in its family in `src/llvm_backend/runtime_abi.hpp`, through
which generated code calls it, and a `declare()` item in the prelude's list
in `src/llvm_backend/runtime_prelude.cpp` (`tests/metadata_ssot_tests.py`
checks that every entry has exactly one); host and toolchain services go to
`src/platform/` and `src/toolchain/`.
`scripts/refactor/check_headers.sh` checks the layering, and
`tests/golden/README.md` describes the equivalence gates that a change of
structure, without a change of output, is held to.

## Releasing Quidra

When explicitly instructed to release the core repository, perform these steps
in order.

1. Fetch the latest remote `develop` and `main` HEADs immediately before
   release work. Inspect their actual difference. Never overwrite newer remote
   work with an older checkout or remembered SHA.
2. Confirm that all work intended for the release is already present in
   `develop`.
3. Determine the release version from the actual compatibility/feature delta.
   Set it in `project.toml`, which is the one place a version is authored, then
   run `python3 scripts/sync_metadata.py` and commit what it regenerates. CLI
   version output, manifest metadata and packaging metadata all follow from
   that single edit; do not set any of them by hand. Confirm that the tag
   `vX.Y.Z` does not already exist before continuing.
4. Run the complete test suite on `develop` and require green CI. Fix release
   blockers on `develop`; do not bypass failing checks.
5. Merge `develop` into `main` while preserving both branches' valid history.
   If `main` contains changes not already in `develop`, reconcile them
   explicitly; never replace one branch wholesale merely to make the histories
   match.
6. Fetch/verify the resulting remote `main` HEAD and run or wait for the
   `main` CI checks. The exact commit that passes is the release commit.
7. The `release` workflow runs on every push to `main`. It re-checks that the
   derived metadata still matches `project.toml`, reads the version from
   `project.toml`, refuses to continue if `vX.Y.Z` already exists, and then
   creates and pushes the immutable annotated tag on that commit itself. Do not
   create or push the tag by hand: the workflow aborts when the tag is already
   there. Nothing is ever tagged from `develop` or an unverified commit,
   because only a push to `main` starts this.
8. That workflow must finish successfully and create the GitHub Release and all
   supported platform artifacts. A release is not complete while it is failing
   or incomplete.
9. Verify that the GitHub Release tag, `main` version metadata, packaged
   `quidra --version`, and published assets all report the same `X.Y.Z`.
10. Bring any release-time `main` changes back into `develop` if necessary,
    but do not advance Core's version independently. Core, Math, NN, Vision,
    Video, and DNN must keep the same `MAJOR.MINOR.PATCH` value on `develop`.
11. Verify the CI of that post-release `develop` state.
12. Leave `main` at the released version and continue ordinary work on
    `develop`. Report the released tag, the exact `main` SHA, and the release
    and CI result. A next development version is selected only by the coordinated
    first-party version-advance procedure below.

The release tag is immutable. Never force-move, delete/recreate, or reuse a
published `vX.Y.Z` tag for different source. A published release that needs
correction is superseded by a new Semantic Versioning release, never by
retargeting the existing tag.

## Release checks

A core release is not complete until the applicable checks pass:

- GCC and Clang release builds;
- Linux, macOS, and Windows CI;
- sanitizer and warnings-as-errors builds;
- `ctest --output-on-failure`;
- parser/checker negative tests;
- native runtime smoke tests;
- standard-library and module/import tests;
- package/version/lockfile tests;
- REPL/CLI tests;
- packaging tests;
- `python3 scripts/sync_metadata.py --check`;
- `quidra --version` and `quidra describe` consistency;
- documented examples.

Benchmarks are not part of the release procedure. Do not start, rerun, or
regenerate benchmark measurements merely because a release is being performed,
even when semantics or performance-sensitive behavior changed. Run benchmarks
only when the release instruction explicitly requests benchmark execution.
An already completed formal benchmark result may be published or synchronized
during the release without starting a new benchmark run.

## Coordinated multi-repository release order

Core and all first-party computational packages use one lockstep release
version: Core, Math, NN, Vision, Video, and DNN must all carry the same
`MAJOR.MINOR.PATCH` value. They do not version independently. An unqualified
Quidra release covers Core, Math, NN, Vision, Video, DNN, Playground, and
Website and is performed in this order:

```text
1. Quidra Core
   -> complete Core release and verify the immutable Core tag/assets

2. Quidra Math
   -> require the same-version released Core tag
   -> complete the Math release at the same `vX.Y.Z` and verify its immutable package tag/assets

3. Quidra NN
   -> require the same-version released Core and Math tags
   -> complete the NN release at the same `vX.Y.Z` and verify its immutable package tag/assets

4. Quidra Vision
   -> require the same-version released Core and Math tags
   -> complete the Vision release at the same `vX.Y.Z` and verify its immutable package tag/assets

5. Quidra Video
   -> require the same-version released Core and Math tags
   -> complete the Video release at the same `vX.Y.Z` and verify its immutable package tag/assets

6. Quidra DNN
   -> require the same-version released Core, Math, and NN tags
   -> complete the DNN release at the same `vX.Y.Z` and verify its immutable package tag/assets

7. Quidra Playground
   -> publish Playground only after the preceding release states are stable
   -> fast-forward Playground `main` to its verified `develop`
   -> verify the production deployment uses the intended released Core/package state

8. Quidra Website
   -> update the site to the released Core/Math/NN/Vision/Video/DNN state and release date
   -> re-verify examples, release links, and package references
   -> synchronize an already completed formal benchmark result only when one is intended for publication; never trigger a benchmark run as part of release
   -> require Website `develop` CI to pass, publish the verified state to `main`, and deploy it
   -> verify https://quidra-lang.com serves the intended release and its key links/assets work
```

Do not begin a dependent step merely because an earlier repository was merged
to `main`; wait until that repository's release/deployment workflow has
finished successfully and verify its published state first.

## Coordinated first-party version advance

After the coordinated release is fully published and verified, do not bump only
one repository to the next development version. When a next release-train version
is selected, update Core, Math, NN, Vision, Video, and DNN to that exact same
`MAJOR.MINOR.PATCH` value as one coordinated metadata operation. Regenerate each
derived `quidra.package`/Core metadata artifact, update first-party compatibility
ranges when required, and finish only when all six remote `develop` branches
agree again and the Core first-party compatibility workflow is green. Temporary
cross-repository CI failures during that multi-repository transition are not a
valid final state.

Do not publish Math, NN, Vision, Video, or DNN until every declared first-party
dependency has an immutable tag with the exact same `vX.Y.Z`. NN, Vision, and
Video require same-version Core and Math tags; DNN requires same-version Core,
Math, and NN tags. Every first-party computational package uses the same release
version as Core. Its declared first-party compatibility ranges must admit that
shared version, while the release workflow independently requires and checks out
the exact same-version dependency tags.
This remains true even when a repository has no code changes for that release:
the shared version is a property of the Quidra first-party package set,
not an independent per-repository cadence. The package installer consumes
library release tags only; it never installs a library from `main` or
`develop`.

Playground follows the package releases. Its production `main` resolves Core
`main`, and its WebAssembly frontend and native runner must identify the same
Core commit. Follow the Playground deployment procedure in its `README.md`,
then verify the production page/runner version and package set match.

Website is deliberately last. Follow `quidra-lang/website`'s release-update
procedure: point the site at the released Core version/date, re-verify all
published Quidra examples with that immutable compiler, update
Math/NN/Vision/Video/DNN and benchmark references when applicable, require
Website CI to pass, publish the verified `develop` state to `main`, deploy
the production site, and verify the live `https://quidra-lang.com` content and
important release/download/docs/Playground links. A coordinated release is not
complete until this live-site verification passes.

For a repository-specific release request, run only the named repository's
procedure, while still respecting any prerequisite release dependency described
above.

## History invariant

```text
main       = latest released stable source (or bootstrap history before the first release)
develop    = next release development
vX.Y.Z     = immutable released source
```

Before the first release of a newly created repository, no stable source exists; the bootstrap-only `main` branch must not be treated as one.

