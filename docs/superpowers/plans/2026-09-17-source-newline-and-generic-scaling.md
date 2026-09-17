# Source Newline Normalization and Generic Scaling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make multiline string values independent of CRLF/LF source line endings and add a reproducible benchmark for generic monomorphization scaling.

**Architecture:** Preserve Quidra's existing string model (backslashes remain literal; `enter`/`tab` remain explicit control-character values) and normalize only physical CRLF sequences encountered inside source string literals to logical LF while retaining physical source offsets for diagnostics. Add a standalone benchmark that separates generic-specialization cost from ordinary parsing/class-definition cost by compiling matched baseline and specialized programs at increasing specialization counts.

**Tech Stack:** C++20 compiler frontend, C++ parser tests, Python 3 benchmark harness, CMake/CTest.

**Spec:** `docs/spec/language.md`

## Global Constraints

- Keep `\` literal; do not introduce C-style escape sequences.
- Keep reference syntax and import syntax unchanged.
- Preserve physical source spans/offsets for diagnostics.
- Benchmark results are observational and must not introduce timing-based CI failures.

---

### Task 1: Normalize CRLF inside string literals

**Files:**
- Modify: `tests/parser_tests.cpp`
- Modify: `src/lexer.cpp`
- Modify: `docs/spec/language.md`

**Interfaces:**
- Consumes: `Lexer::string()` and `StringExpr`/`StringTemplateExpr` token values.
- Produces: identical runtime string contents for otherwise-identical LF and CRLF multiline source literals.

- [ ] **Step 1: Write the failing parser test**

Add a parser test that parses LF and CRLF variants of the same multiline literal, extracts the resulting literal/template text, and requires the values to be identical and contain LF rather than CRLF.

- [ ] **Step 2: Run the parser test to verify it fails**

Run: `ctest --test-dir build -R quidra_parser_tests --output-on-failure`
Expected before the implementation: FAIL because the CR character is retained in the CRLF string token value.

- [ ] **Step 3: Implement minimal lexer normalization**

In `Lexer::string()`, when the current physical source bytes are `\r\n`, consume both bytes but append one `\n` to the token value. Do not globally rewrite the source buffer, so source offsets remain byte-accurate.

- [ ] **Step 4: Document the semantic rule**

State in `docs/spec/language.md` that physical CRLF inside a source string literal is normalized to LF, making literal newline contents independent of checkout line-ending style; lone CR remains representable via `home`.

- [ ] **Step 5: Run parser and full tests**

Run: `ctest --test-dir build -R quidra_parser_tests --output-on-failure`, then `ctest --test-dir build --output-on-failure`.
Expected: PASS.

### Task 2: Add generic-specialization scaling benchmark

**Files:**
- Create: `benchmark/generic_specialization_scaling.py`
- Modify: `benchmark/master_prompt.md` only if a benchmark inventory section already exists and can reference the new script without changing scoring semantics.

**Interfaces:**
- Consumes: path to a built `quidra` executable and optional specialization counts/repeat count.
- Produces: per-count median baseline compile time, specialized compile time, specialization delta, baseline binary size, specialized binary size, and binary-size delta.

- [ ] **Step 1: Implement matched-source generator**

Generate N trivial classes for both baseline and specialized programs. The baseline constructs the values directly; the specialized program routes the same values through one generic identity function instantiated once per class. This controls for parsing and class-definition growth.

- [ ] **Step 2: Implement measurement harness**

Compile each generated program repeatedly with `quidra build`, use the median elapsed wall time, measure executable size with `os.path.getsize`, and print a stable tabular summary. Fail only on compiler/build errors, never on a timing threshold.

- [ ] **Step 3: Smoke-test the benchmark**

Run: `python3 benchmark/generic_specialization_scaling.py ./build/quidra --sizes 4,8 --repeats 1`
Expected: both rows build successfully and report non-negative timing/size deltas.

### Task 3: Verify branch state

**Files:** none

- [ ] **Step 1: Run the full test suite**

Run: `ctest --test-dir build --output-on-failure`.
Expected: PASS.

- [ ] **Step 2: Confirm branch diff is scoped**

Verify the branch contains only the newline semantic/test/docs changes, the generic scaling benchmark, and this plan.
