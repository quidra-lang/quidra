# Length shorthand implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement these tasks in order.

**Goal:** Make `#value` equivalent to a normal zero-argument `value.len()` call for every current and future type that supports that call.

**Architecture:** Parse the prefix into the existing method-call AST, preserving receiver source spans and evaluating the receiver once. Reuse normal checking, extension/generic resolution, error handling, effects, initialization analysis and native lowering. Keep `.len()` available.

**Tech Stack:** C++20 compiler, Quidra packages, TypeScript editors, TextMate highlighting, Python benchmark tooling.

**Spec:** User-approved length shorthand contract; `docs/spec/language.md`, section "Length shorthand".

## Global constraints

- Work on existing `develop` branches and push without creating feature branches.
- Preserve return types, accessibility, fallibility, default parameters and mutation-marker rules of normal `.len()` calls.
- Standard built-in lengths remain `nat`; do not impose that type on arbitrary user methods.
- Postfix access belongs to the receiver: `#matrix[0]` means `matrix[0].len()`; `#values ^ 2` squares the length.
- No new runtime operation, implicit type registration, or unrelated language change.

## Review focus

- Receiver evaluation happens once, including fallible and effectful factories.
- Private, missing, mutating and argument-requiring `len` methods retain normal diagnostics.
- Nested selectors, ranges, interpolation, shapes and exponentiation retain explicit precedence.
- Formatting and highlighting preserve `#` as an operator, including inside interpolation.
- First-party package baselines and benchmark source selections stay consistent with updated code.

### Task 1: Compiler and regression coverage

**Files:** `include/quidra/token.hpp`, `src/lexer.cpp`, `src/parser.cpp`, `src/formatter.cpp`, `tests/parser_tests.cpp`, `tests/compiler_tests.cpp`, `tests/length_operator_tests.sh`, `CMakeLists.txt`.

**Interfaces:** Produce ordinary `MethodCallExpr(receiver, "len", no arguments)` nodes; downstream compilation consumes its existing method-call interface.

- [ ] Add parser, frontend and executable regression tests for built-ins, custom/default/generic methods, preserved error/type rules, initialization, precedence and single evaluation.
- [ ] Run the parser regression before implementation; expect rejection of `#`.
- [ ] Add the token, parse-time method-call expansion and tight prefix formatting.
- [ ] Run compiler/parser suites and executable length tests; expect all tests to pass.

### Task 2: Public contracts and tools

**Files:** Core README, language/grammar/LLM specifications, manifest and example; Playground language mode/tests/README; Website grammar, examples and relevant documentation.

**Interfaces:** Consume Task 1's spelling and precedence; preserve existing `.len()` compatibility.

- [ ] Document the exact equivalence and examples, including user-defined classes.
- [ ] Add highlighting coverage before adding `#` to each editor grammar.
- [ ] Run documentation/manifest checks, editor tests and Website verification.

### Task 3: First-party integration and delivery

**Files:** Active Quidra sources/examples in Math, Collections, Text, Encoding, NN, Vision, Video, DNN and the latest benchmark dataset; affected compatibility baselines and source selections.

**Interfaces:** Consume the verified Core `develop` commit; do not change historical evaluated benchmark snapshots.

- [ ] Migrate appropriate active length calls to `#`, preserving method definitions and `.len()` compatibility tests.
- [ ] Verify packages and current examples with the updated compiler; update affected token/source-selection evidence without running a new scored benchmark.
- [ ] Request independent final review, fix material findings, commit and push affected `develop` branches.
- [ ] Inspect resulting CI runs and report their observed status.
