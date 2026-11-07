# Postfix factorial implementation plan

**Goal:** Implement the already approved `n!` contract in `docs/spec/numeric-and-bin.md#factorial`, and execute the existing factorial benchmark under current Core.

**Architecture:** Preserve the AST and typed IR structure by representing factorial as unary operation `!`. Parse it after a complete call/member/index expression and before powers/signs. The checker retains integer operand types and proves constant domain/range failures; the backend calls checked Core integer runtime helpers with source locations.

**Global constraints:** Work directly on develop and push it; preserve all ten integer types, exact int/nat values, checked fixed-width arithmetic, value ownership and evaluation order. Do not implement the independent pending matrix operator or change benchmark source/metrics unnecessarily.

**Review focus:** contextual literals and integer-only operands; negative/overflow constants versus dynamic values; grouping and marked-call precedence; optimizer facts/side effects for unary `!`; boxed integer ownership and source locations.

## 1. Frontend and static checks

- [x] Capture failing existing factorial benchmark with the current parser (`LEX_ERROR`, 804/805 sources accepted).
- [x] Add black-box factorial tests and reproduce the missing-feature failure before production edits.
- [x] Add Bang token, a single grouped postfix factorial, contextual integer type checking and compile-time negative/overflow diagnostics.
- [x] Keep numeric literal classification, formatting, constant evaluation and optimizer facts correct for the new unary operation.
- [x] Verify parser/type checks against meaningful tests and parse all benchmark sources.

## 2. Runtime and native backend

- [x] Add runtime tests and reproduce missing ABI symbol failures before production edits.
- [x] Implement borrowed-input, owned-result exact factorial plus signed/unsigned fixed-width checked helpers, preserving source failure locations.
- [x] Emit calls for IR unary `!`, register ABI symbols/failure code, and run focused runtime tests.

## 3. Integration and delivery

- [x] Register focused tests with CMake; update grammar, implementation status and language/tooling documentation.
- [x] Run native factorial tests, parser tests, relevant numeric/range/ownership regressions, and the factorial benchmark against its required output.
- [x] Run relevant repository checks; report material environment or unrelated suite limitations accurately.
- [x] Obtain independent review and fix material findings before publishing directly to Core develop.

## Decisions and evidence

- Existing published factorial semantics and the user's explicit implementation request are the design authority; no new syntax choice or approval is needed.
- Runtime/backend and integration-test implementation have separate owners; root owns frontend, checker, formatter, metadata, CMake and integration.
- Full unoptimized native builds produced very large IR objects and exceeded memory at four jobs. Use bounded parallelism and optimized build settings for final validation; temporary validation drivers and downloads stay outside the repository.
- Final native validation: 25 selected CTest suites pass, covering compiler/parser, factorial and integer runtimes, tensor facts, rule dispatch, ranges, numeric conversions, source locations, class operators, iterators, lengths, reductions, exact reals, metadata and diagnostics. The rule-dispatch test interpreter now recognizes canonical `not`; an external build directory requires `QUIDRA_NATIVE_INCLUDE_DIR` for native package probes.
- The existing factorial benchmark produces exactly the Python reference's 13 output lines for `0!` through `12!`. All 805 benchmark source files parse; the 12 previously edited programs also type-check.
- Additional standalone checks of every benchmark source pass for 795/805 files, including 298/300 main programs. These are not a claim that every benchmark executes: existing failures include Math's explicitly pending `digits()` implementation and a nested length/mutation-marker check; eight additional files fail when checked as standalone modules. They are outside the postfix-factorial implementation.
