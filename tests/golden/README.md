# Golden equivalence harness

Tools that check that a change to the compiler keeps what it produces: the
language semantics, the typed IR, the generated LLVM text, runtime behavior
and compile time. Every check compares a head with a base: a commit with its
parent, or a branch with the commit it started from. This file is the
reference for the views, inputs, comparison rules and gates of the harness.

| File | Role |
|---|---|
| `golden_dump.cpp` | `quidra_golden_dump`: every observable compiler output of one input (views `ir`, `ir.full`, `ir.lowered`, `llvm`, `llvm.lowered[.debug]`, `llvm.debug`, `repl`, `diagnostics`, `inspect`, `deps`, `census`, `compile.memory`, `fixture`, `repl.session`, and `inputs` on request) |
| `ir_full.{hpp,cpp}` | field-complete typed-IR serialization (`ir.full`); `ir_full_tests.cpp` pins every struct's field count |
| `failure.{hpp,cpp}` | a compilation that throws: diagnostics, the exception class (also a derived one, `type=CLASS`) and the `index.tsv` status |
| `census.{hpp,cpp}`, `census_keys.{hpp,cpp}` | coverage evidence: alternatives, type-signature keys, optimizer effects; checks, loop operations, numeric kinds and operations; the listed sites of language changes (`census_tests.py` is their self-test) |
| `proxy.py` | performance proxies without timing: instructions per function in the emitted LLVM, in `clang -O3 -S`, and loops `opt -O3` vectorizes |
| `repl_session.{hpp,cpp}` | the REPL's compile path, with the REPL's own helpers (`src/repl_submission.cpp`) |
| `fixtures/backend_fixtures.hpp` (`quidra::backend_fixtures`), `fixtures/optimizer_probes.hpp` (`quidra::optimizer_probes`), `../optimizer_fixtures.hpp` (`quidra::optimizer_fixtures`, shared with `compiler_tests.cpp`) | hand-built modules no source program produces |
| `corpus.{py,toml}` | corpus A: materialize, pin, census |
| `capture.py`, `compare.py` | capture `index.tsv` per entry and view (and, with `--run`, run the listed programs); compare two captures |
| `expect.py`, `expect_tests.py` | the expected differences of a language change (`compare.py --expect`) and their self-test |
| `observe_shim.py` | corpus B: snapshots of what the test suites compile |
| `repl_session_check.py` | simulator vs `quidra repl` decisions |
| `coverage.py` | G1c (region and branch coverage of changed code) |
| `compile_time.py` | Gperf |
| `platform_cases.sh` | Gplat |
| `metal_source_log/` | Gobj for run-time Metal sources |
| `wasm_tool.sh` | Gw (the tool built for node) |
| `fetch_packages.sh` | CI: clones the pinned packages and writes `sources.conf` |
| `../ir_golden.sh` | the driver |
| `../../scripts/refactor/` | Gm `check_moves.py`, Gseq `check_sequencing.py`, Ghdr `check_headers.sh`, Gobj `check_objects.sh` |

## Views

`quidra ir` leaves out 70 of the 185 instruction kinds, parts of types (tensor
rank, shape prefixes, case names) and many fields (spans, proof flags, region
contents), and `quidra llvm` shows neither debug metadata nor the REPL and
`<memory>` compile paths. The tool therefore writes these views:

| View | Produced by |
|---|---|
| `ir`, `llvm` | `compile_file(path, {}, cwd)`: equal to `quidra ir` and `quidra llvm` (checked by `ir_golden.sh cli`) |
| `ir.full` | the field-complete serialization of the optimized module: every field of every instruction, block, function, region, class layout and compiler extension, and the full recursive `Type`; instructions by name (`Binary{…}`, never by variant index, so adding or removing an alternative changes no other line), enums as integers, optionals as `none`/value, pointers as `null`/pointee, maps in key order |
| `ir.lowered` | `ir.full` of `ir::lower(check_file(...))`: the Lowerer without the optimizer |
| `llvm.lowered`, `llvm.lowered.debug` | `emit_llvm` over the unoptimized module, without and with debug information |
| `llvm.debug` | `compile_file` with debug information (`quidra build --debug`) |
| `repl` | `compile_repl_file`: LLVM, `ir.full` and the displayed expression's type |
| `repl.session` | the session simulator over a `.session` file or teed REPL input: one record per submission (kind, status, barrier, type, `ir.full`, LLVM) |
| `compile.memory` | `quidra::compile(source)`, the `<memory>` path of `compiler_tests` |
| `fixture` | `ir.full` of `optimize(M)` and `emit_llvm(M)` with and without debug information, for a hand-built module M |
| `diagnostics` | code, message and span of the failure (`failure.cpp`) |
| `inspect` | `inspect_source_json` |
| `deps` | the resolved source files (the closure check of corpus B) |
| `inputs` | what the compilation reads from the file system, as `load_program_with_modules` records it (`quidra/compile_inputs.hpp`): files with their digests, absent paths, import and package resolutions, package tree digests. Written only on request (`--views inputs`), never by `capture.py`, so captures of compilers without the recorder stay comparable; the run cache's dependency checks (Gdeps) read it |
| `census`, `census.repl` | coverage evidence (below) |
| `run` | what a program does: the entries listed as `[[run]]` in `corpus.toml` are built with `quidra build` and run (`capture.py --run QUIDRA`, below) |

A view whose stage throws is written as `<view>.error`. `index.tsv` has one
line per entry and view: `<entry> <view> <sha256> <bytes> <status>`, with
status `ok`, `error:<code>`, `timeout`, `signal:<n>` or `exit:<n>`.

## Inputs

Corpus A is static and pinned. `corpus.toml` records every entry with the
expected status of its primary view and the sha256 of its source, the package
commits and the external programs; a status change is a mismatch even when
the text is equal. `corpus.py materialize` writes it under
`$QUIDRA_GOLDEN_HOME/corpus` (absolute paths reach the output, so compared
captures share the root), and `corpus.py pin` rewrites `corpus.toml` from a
capture whenever an entry is added or its expected status changes; it keeps
the `[[timeout]]` and `[[run]]` tables. Components:

- the Core examples and benchmark programs;
- the quoted heredocs of the test scripts (sibling modules land together);
- the inline programs of `compiler_tests.cpp`, captured by its
  `QUIDRA_CAPTURE_SOURCES` hook, with the status their helper expects;
- the hand-built optimizer and backend fixtures;
- the fenced Quidra blocks of the documents `documentation_examples.py` checks
  (`tests/doc_blocks.py`) and `docs/packages.md`;
- the package programs and test heredocs at the pinned package commits
  (`corpus/store`), and the probe packages under `probes/packages/`;
- REPL sessions (`probes/**/*.session` and the REPL heredocs);
- programs outside the repository, listed in `sources.conf` (Setup) and
  materialized under `external/`: an external training program
  (`training-integ`), its benchmark variant (`training-bench`) and its layers
  module (`layers`);
- probes (`probes/**/*.qui`) for the paths the other inputs miss;
- generated programs: a deep `and` chain, deeply nested statements, flat
  10,000- and 40,000-statement files, a long method chain.

The REPL session simulator (`repl_session.cpp`) replays `ReplSession::submit`
with the REPL's own code (`src/repl_submission.hpp`): redirected input without
a command line is one submission, otherwise `SubmissionAssembler` cuts the
lines; `declaration_only_submission` classifies a submission; it compiles as
accepted source plus submission with `replay_prefix_bytes`; the replay barrier
comes from `module_requires_replay_barrier`. It does not run the JIT.

Corpus B is observed: `observe_shim.py` stands in for the CLI in the Core and
package test suites and the Python drivers, snapshots every compiled program
(argv, cwd, the environment keys the compiler reads except `QUIDRA_CACHE_DIR`,
teed REPL input, the sources around the entry), then runs the real binary.
`ir_golden.sh observe` captures the snapshots with the base and head tools and
compares them.

The census writes these tables per entry (`census.hpp`), aggregated per
component by `corpus.py census`:

- `alt`: per-alternative counts in the lowered and the optimized module
  (`--require-all` fails unless every alternative occurs lowered and
  optimized);
- `sig`: type-signature keys per alternative (`--matrix ALT` prints one
  matrix);
- `event`: optimizer events derived from the lowered-to-optimized change;
- `count`, lowered and optimized: runtime checks by kind (`check.shape`,
  `check.extent`, `check.range-step`, `check.assert`; bounds checks of array
  accesses not proven in range, of element addresses, of `bin` and string
  indices; `check.initialized.array`, array accesses whose initialization is
  not proven; `check.slice`; `check.conversion`, range-checked and fallible
  conversions; `check.overflow`, fixed-width `+`, `-`, `*` not proven free of
  overflow; `check.divide`, integer `/` and `%`), and in loop blocks (blocks on
  a cycle of the control-flow graph) the overflow-checked operations
  (`loop.overflow-checked`), the overflow-checked induction steps
  (`loop.overflow-checked-step`: a local plus or minus a constant, stored back
  into it) and the `bigint` arithmetic (`loop.bigint-arithmetic`). `Clone`,
  `Retain` and `Release` are the `alt` rows of their alternatives; a check
  that is an alternative of its own (a later `FiniteCheck`) has its `alt`
  row too;
- `op`: numeric operations per kind (arithmetic, comparisons, unary
  operations, conversions `as.T`), lowered and optimized;
- `kind`: the numeric kinds of the checked expressions (`tensor.T` for
  tensor elements);
- `key` and `site`: the sites each key lists, as `FILE:LINE:COLUMN`
  (`--sites KEY` prints them for a capture). A key is the expected set of a
  language change, used as a census predicate (`key.NAME > 0`) in its
  expectation and as its review list. Every list over-approximates:

  | Key | Lists |
  |---|---|
  | `compound-store-across-call` | compound assignments to an element or a field whose right-hand side contains a call: in the lowered IR, a store to an element or field address of a value computed from a read of that place (the same address, or one resolved again with the same index) with a call between the read and the store |
  | `collection-loop-writes-iterable` | value collection loops whose body may write the iterated storage: the loops the lowering gives their own source (its hidden `$for.source` local) |
  | `assign-order`, `.tier1`, `.tier2` | plain assignments whose target subexpressions (the index and slice operands on the target's path) and right-hand side interact: tier 1 when either side may write a variable the other reads or writes (a `&` argument, a receiver whose `receiver_effect` writes, an implicit-receiver call against a field read, a call that receives a shared region against a read of one) or both contain a call; tier 2 when one side may fail (an index read not proven in range, a slice, checked integer arithmetic, a conversion, `try`, a call) and the other contains a call or may fail too |
  | `reference-loop-alias-call`, `.statements` | reference loops over a reference binding (a `&` parameter or a reference local), and their body statements that may replace or resize the iterated array: a call that may write storage of the array's type through another `&` argument or a writing receiver, an assignment to another reference binding whose type can hold the array (or storage reached through one), an assignment to the storage a reference local designates (any assignment when its initializer names no place) |
  | `reference-loop-current-element-alias` | reference loops whose body may reach the current element through another path: the iterated binding named in the body, another reference binding or a writable argument or writing receiver whose type can hold the element, an implicit-receiver call that writes when the iterable is a receiver field or a reference, a call receiving a shared region when the element holds one |
  | `argument-isolation-copy` | by-value arguments passed as a copy because the call may write their storage: storage of exactly the parameter's type, given to a borrowed parameter of a function or method, that the effect analysis isolates (the call lowering's decision) |
  | `typed-constant-retyped` | uses of an imported typed constant whose checked type differs from its declared type |
  | `autograd-target-copy` | in programs that touch gradient state (backward into a class, `track` into a Target, or a Target's `has_grad`, `gradient` or `clear_grad`), the copies of a value whose type holds an `autograd.Target`: bindings, plain assignments, by-value arguments of user calls, returns, array elements and value-loop variables |
  | `backward-unvisitable-target` | backward destinations whose type holds a Target under a union, inside a `map.Map` or `set.Set`, in a recursive structure or under a `const` field |

  The IR keys come from the lowered module, the others from the checked
  program (a REPL session or a hand-built module has no checked program, so
  its source keys are zero).

`proxy.py report --capture DIR` gives, per function of every entry's `llvm`
view, its LLVM instructions with those of cold blocks apart (the K3 blocks
of `expect.py`), and with `--clang` the machine instructions of `clang -O3
-S` (blocks that come from cold IR blocks, and split-off `F.cold.N`
fragments, apart) and with `--opt` the loops that `opt -O3` vectorizes for
the target `--triple` names (by default the one `--clang` targets);
`--keep DIR` keeps each entry's `clang -O3 -S -emit-llvm` output and
assembly for structural checks. `proxy.py compare BASE HEAD` lists the
functions whose hot instruction counts rose or whose vectorized loops fell,
and fails when there is one.

## Capture and comparison

- Captures run under a fixed environment (`env -i`, `HOME` under the golden
  home, `QUIDRA_PACKAGE_PATH` at the store, `LC_ALL=C`, `TZ=UTC`), one process
  per entry with a per-entry timeout (300 s; outliers in `corpus.toml`).
- Base and head are built with the same compiler, flags and standard library.
  Baselines are per platform and never compared across platforms: libc++,
  libstdc++, MSVC and wasm32 iterate unordered containers differently.
- `selfcheck` captures again with memory perturbation, reverse entry order and
  permuted views, and in one process per group (`--batch` forward and
  reverse); all captures must be equal. A nondeterministic entry is a bug to
  fix, not an entry to exclude.
- A change to the harness alone (`tests/`, `scripts/refactor/`, CI) is
  checked with `--harness`: the new tool on the base compiler must equal the
  old tool on every view that existed before; `walk --harness-auto` uses it
  for the commits that touch nothing else. Otherwise base and head run the
  same tool source.
- `compare --base-ref REF` builds REF in a worktree, captures it (cached by
  tree, platform, toolchain and corpus), compares every view and pinned
  status, and prints the first difference with a diff. `walk A..B` compares
  every commit of a range with its parent.
- Only the `index.tsv` of a baseline is archived
  (`$QUIDRA_GOLDEN_HOME/index/<key>.tsv`).

### The run view

The other views show what the compiler produces; `run` shows what the
program then does, for the file entries that `corpus.toml` lists as runnable:

```toml
[[run]]
entry = "examples/cli.qui"
args = ["input.txt", "--count", "3", "--verbose"]
stdin = ["first line", "second line"]
timeout = 120
```

`args`, `stdin` and `timeout` are optional: no arguments, empty standard
input, 60 seconds. `stdin` lists lines, each followed by a newline.

With `--run QUIDRA`, `capture.py` builds each listed entry with
`QUIDRA build` in the entry's directory, then runs the program with the
table's arguments and standard input, in an empty working directory, under
the capture environment and a `QUIDRA_CACHE_DIR` of the capture's own (the
rule of every suite that runs programs). The program and its working
directory live under `$HOME/run/<entry>`, one path for every capture under a
golden home, so a path the program prints is the same in base and head. The
view is

```
exit <status>          (signal <n> when the program was killed by one)
stdout <n> bytes
<stdout>
stderr <n> bytes
<stderr>
```

with status `ok`; a build that fails is `run.error` with the build's stderr
and status `error:<code>`, and a program that outlives its timeout has
status `timeout`. `ir_golden.sh` captures corpus A with `--run` on both
sides, each with its own CLI, so `compare` and `walk` also compare what the
listed programs print and their exit status. A runnable entry must behave
the same in every run: no timing, randomness or environment in its output.

### Expected differences

A language change alters outputs on purpose. `compare.py --expect FILE`
(repeatable; `expect.py` is the reference) states what may differ: every
(entry, view) outside the expectation must be identical, and inside it every
differing line must be explained by a declared rule; what no rule explains
fails like any difference. An expectation file is JSON and lives with the
change's review records, not in the repository. Its `rules` select pairs by
entry patterns, census predicates (`"Clone > 0"`, over the entry's `census`
view) and views, and explain lines by

- line classes `K1`-`K17`: diagnostic constants, runtime declarations,
  cold blocks, added immediates, new exported functions, diagnostics of
  named codes, new IR fields, new entries, the source table, location
  values, element index positions, added line/column fields, call-site
  debug metadata (stripped), error records, tail records and hop recording;
- token rules: renames of whole tokens, tag permutations, string-constant
  lengths, new `ir.full` fields at declared positions, and instruction-shape
  rules (a hunk whose removed and added lines match declared patterns);
- `any`: a declared list whose differences are all accepted and listed for
  review.

Status changes (`ok` to `error:CODE`, one code to another) are reported in a
section of their own, apart from content differences, and each must match a
`status` item of the expectation; without `--expect` none is expected. The
summary counts the explained lines per class and the pairs per rule
(`--list-explained` lists every pair), which a reviewer checks against the
classes the change declares. `added` and `removed` list entries the change
adds or removes.

A rewriter that changes sources (a respelling migration) shifts the columns
of everything after each edit, on every line it touches. It writes a column
map, one edit per line in the original's coordinates:

```
FILE<TAB>LINE<TAB>COLUMN<TAB>DELTA
```

FILE as named in the corpus (relative to its root) or by absolute path, a
positive DELTA inserting that many characters before COLUMN, a negative one
deleting -DELTA characters from COLUMN on. `compare.py --column-map FILE`
(or `column_maps` in an expectation, or `ir_golden.sh compare --column-map
FILE`) compares a base captured from the original sources with a head
captured from the rewritten ones at the same root: in every entry whose
sources the map names, a number that differs must be a column after its line
(`L:C`, `L, C`, `i64 L, i64 C`, `line: L, column: C`: the fields of `ir`,
`ir.full`, the location immediates and debug records of `llvm`) that the map
translates, and a `diagnostics` span's byte offsets must have moved by the
edits before it; the base revision of each mapped source (its sha256) is
renamed to the head's. `inspect` and `deps` hold the source text and its
hash, so they differ under any rewrite.

`ctest -R quidra_golden_expect` runs `expect_tests.py`: every rule kind on
synthetic captures, and a synthetic rewrite of two programs (characters
inserted and deleted) captured with `quidra_golden_dump`, which the rewrite's
column map must make equal.

## Gates

| Id | Gate | Tool |
|---|---|---|
| Gm | pure move or rename: the normalized removed lines equal the added lines up to glue and the declared rename map (lines mode), or the token streams (tokens mode) | `scripts/refactor/check_moves.py` |
| G1 | every view on every corpus A input, fixture, captured compiler-test program and REPL session, head against base, with pinned statuses | `ir_golden.sh compare`, `walk` |
| G1u | G1 under a language change's expected differences: every other pair identical, every difference inside them explained by a declared rule, every status change expected | `ir_golden.sh compare --expect FILE` |
| Gmig | a rewriter's migration: the base compiler on the original sources equals the head compiler on the rewritten ones once the rewrite's column map translates every column | `compare.py --column-map FILE` |
| G1c | G1, and every code region on a line changed since the base under `src/{llvm_backend,llvm_text,lowering,optimizer,ir}` executed (a probe or fixture reaches it); branch coverage of each touched function not below the base's | `ir_golden.sh coverage` |
| G2 | corpus B, base against head | `ir_golden.sh observe` |
| Gbatch | `selfcheck`: equal captures in any entry order, memory layout, view order and batching (run it for any change that adds a static or a cache) | `ir_golden.sh selfcheck` |
| Gt | Core ctest on the release build and on the fake-GPU test build | ctest |
| Gp | the package suites with the pinned package revisions, on the fake GPU and on Metal | `ir_golden.sh packages` |
| Gperf | compile time per stage (lower, optimize, emit): retired instructions on Linux, at most `--limit` percent above the base (0.5 by default); wall time on macOS, reported only | `compile_time.py`, `ir_golden.sh perf` |
| Gobj | runtime and device objects identical after stripping debug information, every section; on macOS the Metal kernel sources compiled at run time equal as multisets (the same sources on both sides; how often each is compiled can vary between two runs of one build, so a count-only difference runs both sides again, up to three attempts) | `check_objects.sh`, `ir_golden.sh objects`, `metal-sources` |
| Gprel | `runtime_prelude_text()` occurs exactly once, verbatim, in the LLVM module that `compile_file` (the path of `quidra llvm`) emits for `probes/minimal.qui`, emitted afresh on every run | `tests/llvm_backend_tests.cpp` |
| Gplat | toolchain discovery and failure paths: stderr, exit codes and resolved tools with each override bogus or shimmed, base equal to head per OS | `platform_cases.sh` |
| Ghdr | every header of the layered directories and the stages' public headers compiles on its own, and the include graph respects the layering below, directly and through other headers | `scripts/refactor/check_headers.sh` |
| Gseq | no call, parenthesized construction or unsequenced operator with two effectful operands | `scripts/refactor/check_sequencing.py` |
| Gsan | the corpus captured with an ASan+UBSan build runs clean and equals the release capture (Linux) | `ir_golden.sh sanitize` |
| Gwc, Gwin, Gw | Windows compile `/W4 /WX` of every commit; full Windows CI and the Windows golden subset against the base; the wasm build and golden tool against the base | CI |
| Gnest | the per-level stack cost of the guarded recursions, re-measured on a Linux Debug build, within `nesting_budget.hpp` | the method described in `src/nesting_budget.hpp` |

## Layering

Ghdr enforces, for headers and sources of these directories:

```
abi            std + quidra/native_extension.h        (anyone may include it)
platform       std + OS headers
toolchain      platform, abi
llvm_text      std only (no quidra/ header, no other layer)
ir             include/quidra/ir, src/ir, src/ir/analysis: types.hpp,
               numeric_types.hpp, type_kind.hpp, compiler_extension.hpp,
               language.hpp, abi, ir
semantics      src/semantics, the analyses that run after type checking:
               never lowering, optimizer, llvm_backend, plan, kernel_ir
lowering       never optimizer, llvm_backend, plan, kernel_ir
optimizer      never lowering, llvm_backend, kernel_ir, the checker, semantics
llvm_backend   never lowering, optimizer, kernel_ir, the checker, semantics
```

A stage's public header belongs to its stage: `quidra/lowering.hpp` (which
includes the checker) to lowering, `quidra/optimizer.hpp` to the
optimizer, `quidra/llvm_backend.hpp` to the backend. `quidra/ir.hpp` is the
umbrella of the IR and the stages, so no layer includes it; a file includes
the IR headers under `include/quidra/ir/` that it uses. The rules hold
through other headers too: Ghdr asks the compiler for every file a header
or source of the `ir`, semantics, lowering, optimizer and backend layers
reaches. The semantics headers include the checker for `CheckedProgram`;
the checker's own source runs them at the end of a check, and the lowering
reads their results.

`plan/` and `kernel_ir/` have no sources yet; their allowed edges are already
in the table.

## Setup

```sh
export QUIDRA_GOLDEN_HOME=/abs/path/golden-home   # same path for every compared capture
cat > "$QUIDRA_GOLDEN_HOME/sources.conf" <<EOF
package math /path/to/math-clone
package nn /path/to/nn-clone
package vision /path/to/vision-clone
package dnn /path/to/dnn-clone
package video /path/to/video-clone
external training-integ /path/to/training/program.qui
external training-bench /path/to/training/bench/program.qui
external layers /path/to/training/bench/layers.qui
EOF
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release -DQUIDRA_ENABLE_TEST_GPU_BACKEND=ON
```

Package clones are read with `git archive` at the commits pinned in
`corpus.toml`; nothing is written to them. An `external NAME PATH` line names
a program outside the repository; `corpus.toml` pins it by name and sha256.
CI writes `sources.conf` with `fetch_packages.sh`, which clones each pinned
package from its repository and leaves out one whose pinned commit it cannot
fetch. Entries whose sources are not listed, the external programs among
them, are skipped with `--allow-missing`.

## Running the gates

`B` is the base and `C` the head: a commit and its parent (`B` = `C^`), or a
branch head and the commit the branch started from. Without `--head-ref`,
the head is the working tree.

```sh
tests/ir_golden.sh compare --base-ref B --head-ref C          # G1 (all views, pinned statuses)
tests/ir_golden.sh walk B..C --harness-auto                   # G1 on every commit of a range
(cd build-release && ctest -j4); (cd build-test && ctest -j4) # Gt
```

What else a change needs depends on what it does:

| Change | Gates |
|---|---|
| moves code | `python3 scripts/refactor/check_moves.py --base B --head C` (Gm, lines), `tests/ir_golden.sh perf --base-ref B --head-ref C` (Gperf) |
| renames | Gm with `--rename-map FILE`, Gperf |
| mechanical edit (line breaks change) | Gm `--mode tokens` (+ rename map), Gperf |
| restructures code | `tests/ir_golden.sh coverage --base-ref B` (G1c), `python3 scripts/refactor/check_sequencing.py` (Gseq), Gperf |
| new header / layer | `bash scripts/refactor/check_headers.sh build-release` (Ghdr) |
| new class | Gm `--shadow HEADER:CLASS` (members shadowed by moved locals) |
| new code | Gm `--new-code` (no raw new/delete, std::exit, #define, std::function) |
| static or cache added | `tests/ir_golden.sh selfcheck` (Gbatch) |
| runtime / device source | `tests/ir_golden.sh objects --base-ref B --head-ref C` (Gobj), `tests/ir_golden.sh metal-sources --base-ref B --head-ref C` (run-time Metal kernel text, macOS) |
| platform / toolchain | `tests/golden/platform_cases.sh QUIDRA OUT` on both sides, `diff -r` (Gplat) |

Gm's glue is narrow on purpose, because lines mode ignores order: `static`
and `inline` are glue only at namespace scope or on a function definition
header (never on a local variable), a `Class::` qualifier only before the
parameter list of an out-of-line definition, and lone braces must balance up
to added or removed namespace scopes. `check_moves.py --self-test` (ctest
`quidra_refactor_move_check`) pins these rules with probes.

Gseq needs no list of classes: a function is effectful when its body changes
state that outlives the call (a member, a global or static, anything reached
through a reference or pointer, a mutating standard-library call on such an
object), when it calls an effectful function, or when it is a project
function defined in none of the checked files that could mutate its object or
a non-const reference argument; this is derived over all checked files at
once, to a fixpoint. New emitters, services, lowering units and scaffolding
classes are covered as they appear. Clang evaluates arguments left to right
and GCC and MSVC usually right to left, so Gseq is what keeps an unsequenced
pair from passing G1 on macOS and failing on Linux and Windows.
`check_sequencing.py --self-test` checks the derivation on a probe unit; both
need the libclang Python bindings (Homebrew `llvm`).

G1c judges each changed line by the code regions on it, not by the function
around it: a line fails when any region holding code on it never ran. A probe
or fixture that reaches the region is the fix; a line no input can reach (an
internal invariant's throw) goes into a `--accept FILE` list of
`PATH:LINE reason` lines:

```sh
tests/ir_golden.sh coverage --base-ref B --accept accept.txt   # with C checked out
```

Branch coverage is compared with a coverage build of `B` that captures the
same corpus; its per-function summary is cached under
`$QUIDRA_GOLDEN_HOME/coverage-baseline`.

To compare a whole branch with its base, run the slower gates once, against
`B`:

```sh
tests/ir_golden.sh compare --base-ref B                       # G1
tests/ir_golden.sh observe --build "$PWD/build-test" --base-ref B [--packages]  # G2
tests/ir_golden.sh selfcheck                                  # Gbatch
tests/ir_golden.sh perf --base-ref B --limit 1.0              # Gperf (Linux gates; macOS reports)
tests/ir_golden.sh objects --base-ref B                       # Gobj
tests/ir_golden.sh sanitize                                   # Gsan (Linux)
tests/ir_golden.sh census --capture "$QUIDRA_GOLDEN_HOME/captures/<key>" --require-all
bash scripts/refactor/check_headers.sh build-release          # Ghdr
bash tests/backend_regressions.sh build-release/quidra "$(command -v opt)"
```

plus the package suites on the fake GPU and on Metal with the pinned package
revisions (Gp):

```sh
tests/ir_golden.sh packages --build "$PWD/build-test"     # fake GPU
tests/ir_golden.sh packages --build "$PWD/build-release"  # Metal (macOS)
```

(`--package-root DIR` runs them from another checkout of the packages
instead of the pinned store copies; a program run directly leaves a
transient `.quidra-run-*` file next to it until the run ends), and a short
training run of the external training program on CPU and Metal.

## CI

`.github/workflows/refactor-equivalence.yml` runs on manual dispatch only,
with a required `base` (a commit, a tag or a branch):

- `per_commit`: every commit of `base..HEAD` against its parent: G1
  (`walk --harness-auto`) on Linux and macOS, Gseq on macOS (libclang from
  Homebrew `llvm`), Gperf on Linux, Gwc on Windows; and G1c of HEAD against
  the base, on macOS only: the coverage build needs clang's instrumentation,
  and the Linux jobs build with GCC;
- `full`: HEAD against the base: G1, Gbatch, G2, Ghdr, Gperf (Linux, limit
  1.0 %), Gobj, Gp, Gsan (Linux), the LLVM `opt` validation, Gw, and Gwin
  with the Windows golden subset.

It does not run Gm (its mode and rename map depend on the change), `cli`,
`repl-check`, Gplat, Gnest or the external training program; run them
locally. CI fetches the pinned packages with `fetch_packages.sh` and skips
entries whose sources it cannot fetch (`--allow-missing`), so its corpus can
be smaller than a local one.

The regular CI (`.github/workflows/ci.yml`) builds the harness too: with
`BUILD_TESTING`, `quidra_golden_support`, `quidra_golden_dump` and
`quidra_ir_full_tests` are part of every build, and ctest runs
`quidra_ir_full_tests`, `quidra_golden_expect`, `quidra_golden_census` and
`quidra_refactor_move_check`. So every push
compiles the harness with warnings as errors under GCC and Clang at C++20 and
C++23 (Ubuntu 24.04), the default GCC of Ubuntu 22.04 (that job installs
clang-15 but does not select it), the ASan+UBSan Debug build, the fake-GPU
build, Apple clang, and MSVC with `/W4 /WX`. A harness change has to build
cleanly on all of them; locally, build it with Apple clang and with a
Homebrew GCC (`-DQUIDRA_WARNINGS_AS_ERRORS=ON`).
