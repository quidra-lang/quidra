# Class Operators Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Unify `operator()` and `operator[]` names and support class index reads.

**Architecture:** Parse full operator names and use ordinary method resolution
for class calls and scalar index lists. Preserve built-in indexing and stream
effects; reject writable access to index-call results.

**Tech Stack:** C++20, Bash, CMake/CTest, Quidra native compiler.

**Spec:** `docs/superpowers/specs/2026-11-07-class-operators-design.md`

## Global Constraints

- Work and push directly on `develop`; create no new branch.
- Keep ordinary no-overloading, mutation authority and value/error semantics.
- Do not add reference returns, user-defined slices or implicit iteration.
- Update only files affected by class operator syntax and semantics.

## Review Focus

- Operator names must remain distinct in generic classes and imported modules.
- Receiver/argument effects must execute exactly once in source order.
- Index results must never become writable storage, even through member paths.
- File-stream calls must retain mutation markers and output initialization.
- Formatting and source inspection must accept both complete operator names.

### Task 1: Complete call-operator names

**Files:** `src/parser.cpp`, `include/quidra/parser.hpp`, `src/frontend.cpp`,
`src/checker.cpp`, `include/quidra/language.hpp`,
`src/lowering/borrow_inference.cpp`, `tests/parser_tests.cpp`,
`tests/stream_operator_tests.sh`, affected existing operator fixtures.

**Interfaces:** Parse method names as `operator()` / `operator[]`; call resolution
uses `operator()` instead of the former `operator` special name.

- [x] Add parser and native regression cases for zero/one-argument call operators,
  distinct index names and rejection of the former callable spelling.
- [x] Run the new regression on the original compiler and observe the missing
  syntax failure.
- [x] Implement full-name parsing and migrate callable/stream resolution.
- [x] Run parser and stream regression tests.

### Task 2: Class index reads

**Files:** `src/parser.cpp`, `src/frontend.cpp`,
`src/semantics/mutation_markers.cpp`, `tests/class_operator_tests.sh`,
`CMakeLists.txt`.

**Interfaces:** An ordinary `IndexExpr` on a class with `operator[]` resolves to
`MethodCallExpr` with positional arguments; marked index calls are parsed
directly as methods, preserving the existing AST layout.

- [x] Add native cases for string keys, multiple indices, generic/imported classes,
  defaults, errors, once-only evaluation, mutation markers and returned values.
  Add rejection cases for writes, references, slices and private operators.
- [x] Observe the native cases fail on the original compiler.
- [x] Implement index-to-method resolution and result type inference.
- [x] Run class operator, mutation, conversion and native regression tests.

### Task 3: Documentation, downstream migration and verification

**Files:** `docs/spec/language.md`, `docs/spec/classes.md`,
`docs/spec/grammar.ebnf`, `README.md`, affected first-party consumers.

**Interfaces:** Docs and consumers use `operator()(...)` / `operator[](...)`.

- [x] Audit all first-party develop branches and migrate affected examples/tooling.
- [x] Verify formatting, source inspection, stream behavior and built-in indexing.
- [ ] Run the CTest suite and affected downstream checks; report actual failures.
- [ ] Review the complete diff, commit only scoped files, fast-forward against
  remote develop as needed, and push affected develop branches.

## Verification record

- Native compiler and parser/operator-symbol test targets built successfully.
- All six operator, stream, line-I/O and length CTests passed.
- All five array-extension, documentation, manifest, metadata and diagnostic
  contract CTests passed.
- Both call and index operators executed successfully through the LLVM REPL.
- Website type checking, lint, build, grammar, cross-repository contracts and
  link checks passed.
- Independent review found a native-symbol collision; the new regression failed
  before the injective encoding fix and passed afterward, including generic
  operator names and the ordinary operator__ spelling.
- Full local regression build encountered the shared memory limit; full-suite
  verification is left to the branch CI as well. No passing full-suite claim.
- An ordinary zero-argument method named operator followed by a comparison
  was verified RED-to-GREEN, preserving its ordinary identifier behavior.
