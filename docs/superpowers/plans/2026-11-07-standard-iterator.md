# Standard Iterator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved borrowed `Iterator<T>` and user collection-loop integration.

**Architecture:** Inject the standard generic class through the existing frontend standard-class mechanism, retaining ordinary class/reference lowering. Expand user value collection loops into scoped iterator calls during monomorphization, reusing conditional output initialization. Add iterator-specific lifetime and source validity checks without changing ordinary reference fields.

**Tech Stack:** C++20 frontend/checker, existing LLVM lowering, CMake/CTest, Quidra integration fixtures.

**Spec:** docs/superpowers/specs/2026-11-07-standard-iterator-design.md

## Global Constraints

- Work and push directly on develop; create no branches.
- Standard `Iterator<T>` uses `bool .operator()(T &value)`.
- Type positions require explicit generic arguments; constructors may infer them.
- Preserve existing array/bin/range semantics and ordinary reference-field behavior.
- No placeholder element initializer or unrelated syntax/API changes.

## Review Focus

- False leaves the output unchanged, including repeated exhaustion.
- Non-default-constructible and union elements initialize only after success.
- Factories and temporary sources evaluate once; nested loops have independent cursors.
- Local source escapes and active source replacement cannot dangle or silently change iteration.
- Explicit types, module/generic factories and existing loops remain checked consistently.

---

### Task 1: Standard class and output calls

**Files:** tests/iterator_tests.sh; CMakeLists.txt; include/quidra/standard_classes.hpp; src/frontend.cpp; src/checker.cpp as required.

**Interfaces:** Consumes standard class injection and conditional output initialization. Produces `Iterator<T>`, `Iterator(&values)` and `.it(&value)`.

- [x] Add direct iterator behavior, empty/exhausted, class/union element, required-type-argument and mutation-marker tests.
- [x] Run against the baseline compiler; expect unknown Iterator diagnostics.
- [x] Inject and qualify the generic standard class; preserve normal reference/code generation.
- [x] Run iterator and existing class/reference tests.

### Task 2: Collection loops and borrowing

**Files:** src/frontend.cpp; src/checker.cpp; tests/iterator_tests.sh; existing lowering only if necessary.

**Interfaces:** Consumes the standard iterator and success output summaries. Produces single-source user value `for` integration and iterator borrowing checks.

- [x] Add factory-once, nested/break/continue/reused bindings, generic/imported, local-escape and source invalidation tests; observe missing-feature failures.
- [x] Lower scoped iterator loops, infer exact element types, and apply lifetime/source checks.
- [x] Run iterator tests and affected class/reference/loop, numeric and compiler suites locally; use the existing CI matrix for the complete platform suite.

### Task 3: Documentation and first-party integration

**Files:** README.md; docs/spec/language.md; docs/spec/classes.md; docs/spec/loop-headers.md; docs/spec/llm-guide.md; docs/spec/grammar.ebnf if needed; examples/iterators.qui; affected Playground/Website tooling.

**Interfaces:** Documents the implemented API and preserves existing language descriptions.

- [x] Update the specification and runnable examples; audit first-party sources for affected type tooling or copied specifications.
- [x] Execute examples and relevant tooling checks; perform a final independent review and address material findings.
- [ ] Verify clean diffs, commit, push develop, and inspect checks on the pushed commit.

## Validation evidence

- Linux Debug compiler and native CLI built successfully against the current develop changes.
- All 19 selected CTest suites passed, including the complete compiler unit suite,
  iterators, class operators/initialization, references, tuple/loop headers,
  numeric/range contracts, errors, documentation and manifest checks.
- Metadata, diagnostic-code and ownership-predicate checks: 3/3 passed.
- Iterator example formatting and execution passed. The deep-loop regression
  reports NESTING_DEPTH; iterator expansion uses a separate guarded frame.
- Website: 44/44 compiler-backed example checks, grammar/contracts, Astro type
  check, lint, static build, links and contrast checks passed.
- Playground: 46 tests passed; 46 WASM/version-dependent checks skipped without
  staged browser artifacts. TypeScript checking passed.
- The standard Iterator WASM API regressions are included in the existing CI
  workflow; the complete platform matrix is to be verified on the pushed commit.
