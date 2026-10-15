# Array and extension integration verification plan

**Goal:** Complete the October 15 array/range/extension specification on develop.
**Architecture:** Reuse the already committed array, range, tensor mask and extension implementations. Repair failures at their existing parser, module-loader and ownership boundaries; keep direct slices independent of materialized index arrays.
**Tech stack:** C++20 compiler, LLVM native backend, Quidra Math, CTest and shell integration tests.
**Spec:** User-approved October 15 specification; public contracts in docs/spec/language.md and docs/spec/grammar.ebnf.

## Constraints

- Work on develop; fetch its latest HEAD before a non-forced push.
- Preserve value semantics, explicit mutation, exact integer arithmetic and existing scalar/tensor Math behavior.
- Keep `array.range` and implicit conversions absent.
- CPU and deterministic fake-GPU tests run locally; real hardware and other operating systems remain CI gates.

## Tasks

- [x] Inspect current Core d8a7e7c and Math 961b555 implementations and CI.
- [x] Reproduce parser/checker and allocation-lifetime failures with focused drivers while the full native compiler builds.
- [x] Fix range parsing/direction in parser.cpp and lowering/call_lowering.cpp; add range_contract_tests.sh.
- [x] Fix gathered-array ownership and evaluation lifetime in lowering/lifetime_lowering.cpp and expression_lowering.cpp; add array_selection_contract_tests.sh.
- [x] Preserve module-specific visible extensions through the resolved import graph in frontend.cpp; test private imports, public reexports and intrinsic conflicts in extension_visibility_tests.sh.
- [x] Replace the GCC 11 CI optional-initialization failure pattern without suppressing warnings; verify optimized runtime compilation and existing slice bounds tests locally with GCC 13.
- [ ] Register regressions and run Core/Math integration, fake GPU, ownership and parser checks.
- [ ] Review changes, fetch latest develop, integrate concurrent commits, push and inspect CI.

## Review focus

- Effectful later arguments must not invalidate previously evaluated arrays.
- New selection allocations must be released rather than misclassified as borrowed views.
- Import aliases and public reexports must preserve extension visibility without exposing sibling modules.
- Negative range endpoints and equal descending endpoints must respect the slice direction contract.
- Built-in methods must never silently defeat a compatible imported extension (or vice versa).

## Evidence and decisions

- Initial feature commits already exist on develop; this work verifies and completes them rather than duplicating them.
- Ubuntu 22.04 CI job 116488004415 fails GCC's `maybe-uninitialized` check for a conditional optional payload in runtime.cpp. Integrated upstream 9a8fee5, which initializes the optional before setting or resetting its payload; retained warnings-as-errors.
- First-party compatibility only consumes the compiler executable. Its build should target that executable; the dedicated Core workflow retains full Core unit tests.
- Full parser tests and 26 focused checker cases pass. The extension visibility semantic suite fails 30 cases against the saved baseline frontend and passes against the repaired frontend; its execution assertions still require the native CLI.
- The selection ownership probe and the early-return IR probe both reproduce the original failures and pass after the repair. Modified lowering translation units compile with strict warnings.
- Independent review identified and verified fixes for contextual array receivers, gathered/sliced array result inference, fixed-shape generic cache keys, nested array type metadata, and temporary cleanup on propagated errors.
- Math's integration example, precision fixture, and array reductions pass frontend checking, as do ten representative NN/DNN/Vision examples and tests. Package ABI/layer checks pass.
- Native execution remains a required gate. The shared local environment has interrupted full builds through memory and disk exhaustion; an inactive older generated object cache was removed, and the current build continues with one job and a local precompiled header. CI provides an independent native/platform verification path after push.
- Follow-up verification against `60faf1b` reproduced five failures in 34 additional array/extension cases: contextual array/scalar literals, mixed literal/nat selectors, uninitialized array reads, native method/unary receiver inference, and generic literal-argument disambiguation. All 34 pass production frontend checking after the targeted fixes; CTest also registers their execution assertions.
- Materialized exact-integer ranges copy the iteration value before the consuming `ArraySet`, preserving the loop local's ownership for boxed integers. The existing huge-endpoint range contracts and the added large-step case cover this path when executed by the native CLI.

- Follow-up review verified free/method parity for concrete and explicit generic fallible receivers, retained inferred union types, and excluded unused specializations from intrinsic collision checks. Paired success/error regressions cover both call forms.
