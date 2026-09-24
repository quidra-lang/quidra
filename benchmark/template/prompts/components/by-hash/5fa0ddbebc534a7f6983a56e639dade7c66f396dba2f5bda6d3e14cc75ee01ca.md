

---

## Embedded input: language_quality.md
Source SHA-256: `cc718e9dda3eb349812f0feb507c9aa5cfd219333521a3548263e0c924d4b2c7`

# 7. Language Quality Evaluation Fairness

For Language Quality Evaluation, use normal, idiomatic, production-reasonable best practices for each language.

The goal is to compare:

**the same algorithm, input, output, numerical requirements, and workload implemented appropriately in each language.**

Do not intentionally write inefficient code in one language.

For example, do not penalize C++ by unnecessarily copying large `std::vector` objects when normal code would use:

- references
- `const&`
- `std::span`

Likewise, use normal idioms such as:

- Rust borrowing and slices
- Go slices
- Java reference semantics
- appropriate native data structures
- avoidance of unnecessary copies
- release / optimized builds
- normal compiler optimizations

However, the following are prohibited:

- using a different algorithm for only one language
- GPU acceleration for only one language
- handwritten SIMD for only one language
- benchmark-specific hacks for only one language
- outsourcing the core computation to an optimized external native library for only one language
- deliberately weakening one language
- giving one language disproportionate manual optimization

The principle is:

**Language Quality Score = the intrinsic quality of the language and its implementation under comparable workloads, excluding maturity/network-effect advantages that primarily accumulate with age and adoption.**

Language Quality must not be tuned to Quidra's design philosophy. Established languages must receive full credit for genuinely better language semantics, implementation quality, performance, diagnostics, safety, concurrency, portability design, interoperability design, and other intrinsic properties.

However, do **not** reduce Language Quality merely because a language is young and therefore has fewer surrounding tools, supported integrations, mature editor/debugger workflows, years of documentation, release history, package infrastructure, or implemented target platforms. Those present-day disadvantages are real, but they belong to Primary Evaluation 4 — Ecosystem.

Mixed concepts must be split instead of double-counted:

- language-level portability / platform neutrality -> Language Quality;
- realized supported-platform breadth -> Ecosystem;
- FFI / interoperability design quality -> Language Quality;
- breadth of working external integrations -> Ecosystem;
- dependency semantics / dependency simplicity -> Language Quality;
- package-manager maturity, package corpus, and maintenance activity -> Ecosystem;
- diagnostics and code-level debuggability -> Language Quality;
- debugger/profiler product maturity -> Ecosystem.

This boundary is deliberate: Language Quality asks how good the language and implementation are; Ecosystem asks how much usable surrounding infrastructure and real-world evidence currently exists.

---

## Language / Development

- Code Efficiency / Conciseness
- Readability
- Functionality / Expressiveness
- Diagnostics
- Dependency Simplicity
- Portability Design / Platform Neutrality
- FFI / Interoperability Design
- Concurrency

# 16. LLM Implementation Scenarios

For SVM, GMM, LightGrad, and other substantial tasks, evaluate at least these two independent scenarios.

## A. Specification → Implementation

Do not show the reference source code.

Provide only:

- algorithm specification
- API specification
- input/output requirements
- constraints
- correctness requirements
- tests

Then ask the LLM to implement the program.

## B. Reference → Porting

Provide the reference implementation and ask the LLM to port it to the target language with equivalent behavior.

Keep Scenario A and Scenario B results separate.

Do not merge them into a single raw dataset.

---
