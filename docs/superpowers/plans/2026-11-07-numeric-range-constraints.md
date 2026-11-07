# Checked numeric conversion and range constraints Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans; independent syntax and numeric-conversion changes may be implemented in isolated develop checkouts and reviewed together before integration.

**Goal:** Implement the approved numeric-only implicit conversion and range-constraint design throughout Quidra.

**Architecture:** Extend existing AST records with optional RangeConstraint bounds. Preserve existing assignment/binding IR and lower range guards to checked numeric comparisons followed by an attributed FailError. Reuse checked numeric-argument conversion for other value destinations; keep shapes in the existing shape-plan machinery.

**Tech Stack:** C++20, CMake/Ninja, LLVM text generation, shell/Python contract tests.

**Spec:** docs/superpowers/specs/2026-11-07-numeric-range-constraints-design.md

## Global Constraints

- Work and push on develop; create no branches.
- Preserve unrelated remote changes; never force-push.
- Numeric conversion must check range and integrality, preserve existing rounding, and reject boolean coercion.
- Guarded assignments validate converted candidates before committing writes.

## Review Focus

- Bounds with effects and failed guarded writes must not evaluate the RHS twice.
- Typed candidates must be checked after destination rounding/conversion.
- Imported/generic/cloned syntax must retain all constraints.
- Function-value/constructor/default call paths must execute entry guards.
- Shape matches must fail to select rather than abort on predicate mismatch.

## Task 1: Baseline and numeric conversions

Files: src/checker.cpp, relevant frontend inference and lowering conversion code, numeric tests.
- [x] Establish build and run a new conversion/bool rejection test against baseline (expected failure).
- [x] Reuse checked numeric conversions in declarations/assignments/defaults/returns; remove integer truth conversions.
- [x] Update superseded tests and verify numeric conversions and boolean rejection.

## Task 2: Range syntax and source preservation

Files: include/quidra/ast.hpp, include/quidra/parser.hpp, src/parser.cpp, src/frontend.cpp, formatter/source visitors, parser tests.
Interface: RangeConstraint has shared_ptr<Expr> lower/upper, bool lower_inclusive/upper_inclusive, SourceSpan span; optional<RangeConstraint> range_constraint appended to BindingStmt, AssignStmt, Parameter, ShapeAxis.
- [x] Add failing parser and format/clone round-trip tests.
- [x] Parse typed/inferred bindings, guarded assignment, parameters and shape axes into this interface.
- [x] Preserve constraints through cloning, qualification, formatting and source inspection.
- [x] Verify syntax tests.

## Task 3: Range checking and lowering

Files: checker/checker_shapes, lowering helpers and binding/assignment/function/shape paths, CMake and tests.
- [x] Add failing end-to-end scalar, function/class, shape, mismatch and effect tests.
- [x] Check ordered numeric bounds and candidate types; reject statically proven violations.
- [x] Lower bounds once with ordinary common-type comparisons and RANGE_CONSTRAINT errors before stores/body.
- [x] Integrate shape captures, equality and match selection.
- [x] Verify dedicated contract tests and existing shape/call/ownership tests.

## Task 4: Migration, review and integration

Files: README/specifications, examples and affected first-party sources/benchmarks.
- [x] Migrate integer truth contexts to explicit comparisons and update specifications/grammar/LLM guide.
- [x] Build the compiler and affected test targets; run focused CTest, CLI, package and benchmark checks. Use the CI matrix for remaining platform and target coverage.
- [x] Obtain an independent whole-change review, fix material findings with regression tests.
- [x] Commit on develop and reconcile remote changes. Publish the identical validated tree without force; track the resulting CI run separately.

## Verification evidence

- Compiler, parser, source-preservation, effect traversal and interface fingerprint targets built. Parser/source/fingerprint/traversal tests passed.
- Before replaying the concurrent class-operator commit, final focused validation: 8/8 passed, including the main compiler unit suite, numeric binding/compound conversion, range constraints, array extensions, ownership predicates, metadata and diagnostics.
- Complete CLI shell suite passed with a 720-second host guard; the normal CTest 180-second guard was insufficient for this Debug build.
- Existing numeric, call, shape, class, reference, tuple, parallel, standard-library and documentation suites exercised. Superseded integer-truth and numeric-conversion expectations were migrated.
- Independent reviews covered reference initialization, managed snapshots/try cleanup, bound order, shape/function contract identity and compound reference rebinding. Regression tests reproduce the reported failures and pass after correction.
- First-party numeric audit checked 385 sources with no remaining integer-truth diagnostics; six native package integrations passed. Vision/Video native checks require unavailable codec dependencies. Playground native/browser tooling tests and Website checks/build passed; staged WASM frontend tests remain unavailable locally.
- Publishing must retain the concurrently added class operator support on origin/develop.

## Concurrent integration

The class-operator commit `353d37c` is retained in full. Its complete 23-file diff is preserved on top of the tested feature tree. The merged parser/formatter suite and operator-symbol tests are checked separately; changed checker/frontend/lowering sources pass C++ syntax checks, and metadata/diagnostic/ownership source checks pass. The full merged native matrix is verified by the CI run for the published tree rather than inferred from the earlier binary. Lowering-switch differential validation also passed before integration.
