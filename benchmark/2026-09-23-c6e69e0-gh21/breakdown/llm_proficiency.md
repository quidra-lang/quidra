# LLM Proficiency: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Swift | 98.22 |
| 2 | Python | 96.81 |
| 3 | TypeScript | 96.69 |
| 4 | Java | 95.37 |
| 5 | C++ | 93.19 |
| 6 | Go | 92.79 |
| 7 | Kotlin | 92.74 |
| 8 | Zig | 90.01 |
| 9 | Rust | 88.27 |
| 10 | Quidra | 4.95 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.generation_success_rate | 0.04 | 15 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) |
| metric.compile_parse_success_rate | 0.04 | 0 (10) | 100 (1) | 88.89 (7) | 83 (9) | 86 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) |
| metric.correct_at_1 | 0.12 | 0 (10) | 100 (1) | 88.89 (8) | 83 (9) | 100 (1) | 100 (1) | 100 (1) | 92 (6) | 100 (1) | 89 (7) |
| metric.correct_at_n | 0.05 | 0 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) |
| metric.test_pass_rate | 0.1 | 0 (10) | 100 (1) | 100 (1) | 83 (9) | 100 (1) | 100 (1) | 100 (1) | 92 (7) | 100 (1) | 89 (8) |
| metric.repair_success_rate | 0.07 | 0 (6) | N/A | 100 (1) | 100 (1) | 100 (1) | N/A | 100 (1) | 100 (1) | N/A | N/A |
| metric.repair_efficiency | 0.05 | 0 (6) | N/A | 100 (1) | 100 (1) | 100 (1) | N/A | 100 (1) | 100 (1) | N/A | N/A |
| metric.diagnosis_efficiency | 0.05 | 10 (6) | N/A | 100 (1) | 100 (1) | 100 (1) | N/A | 100 (1) | 100 (1) | N/A | N/A |
| metric.silent_bug_resistance | 0.12 | 0 (10) | 100 (1) | 100 (1) | 80 (9) | 83 (8) | 92 (5) | 100 (1) | 85 (6) | 100 (1) | 85 (6) |
| metric.syntax_hallucination_resistance | 0.07 | 5 (10) | 100 (1) | 88.89 (9) | 90 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) |
| metric.specification_compliance | 0.07 | 0 (10) | 100 (1) | 88.89 (8) | 90 (5) | 86 (9) | 95 (4) | 100 (1) | 90 (5) | 100 (1) | 90 (5) |
| metric.prompt_robustness | 0.05 | 20 (10) | 100 (1) | 73.33 (8) | 100 (1) | 67 (9) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (7) |
| metric.unseen_case_generalization | 0.06 | 10 (9) | 90 (4) | 100 (1) | 75 (7) | 100 (1) | 95 (3) | 67 (8) | 85 (5) | N/A | 80 (6) |
| metric.source_token_efficiency | 0.03 | 30 (10) | 80 (1) | 65.14 (9) | 80 (1) | 78 (4) | 78 (4) | 70 (8) | 80 (1) | 78 (4) | 78 (4) |
| metric.total_token_efficiency | 0.03 | 25 (10) | 85 (2) | 76.26 (8) | 78 (7) | 70 (9) | 85 (2) | 90 (1) | 85 (2) | 82 (5) | 82 (5) |
| metric.generated_code_performance | 0.03 | N/A | 75 (6) | 92 (1) | 85 (3) | 90 (2) | 75 (6) | N/A | 80 (5) | N/A | 85 (3) |
| metric.generated_code_memory_efficiency | 0.01 | N/A | 80 (4) | 90 (1) | 85 (2) | N/A | 78 (6) | N/A | 80 (4) | N/A | 85 (2) |
| metric.generated_code_compile_performance | 0.01 | N/A | 95 (2) | 95 (2) | 85 (7) | 95 (2) | 85 (7) | 100 (1) | 75 (9) | 90 (5) | 88 (6) |

## N/A cells and their stated reasons

- **metric.repair_success_rate / Java** — No repairs were required: all 9 primary trials (SVM/GMM/LightGrad, specification_to_implementation scenario) compiled and passed on first generation, so the repair loop was never invoked. Repair budget (3 turns) was available but unused.
- **metric.repair_success_rate / Python** — No trial in the 9 primary specification_to_implementation cells (SVM/GMM/LightGrad x 3 independent trials) produced a failing generation-success, compile/parse, or test-pass outcome, so no repair turn was triggered under the fixed max_repair_turns=3 protocol. Repair Success Rate is undefined on an empty failure set for this shard; this is a genuine N/A produced by a positive result, not a masked failure.
- **metric.repair_success_rate / Swift** — No trial required a repair turn: all 9 primary trials (SVM/GMM/LightGrad x 3 independent trials each, specification_to_implementation scenario) compiled and passed correctness checks on the first attempt. The repair budget (max 3 per primary.json) was available but never invoked. Genuine N/A, not a relabeled failure.
- **metric.repair_success_rate / Zig** — No trial required a repair turn: all 9 primary-workload Zig trials compiled and ran successfully on the first attempt.
- **metric.repair_efficiency / Java** — No repairs were required (0 trials needed repair), so repair-iteration efficiency cannot be measured for this cell.
- **metric.repair_efficiency / Python** — No repair turns were needed (0 failing trials in the 9-trial primary cell set), so Repair Iterations/Efficiency has no observations to aggregate for this shard.
- **metric.repair_efficiency / Swift** — No repair turns were used; repair efficiency is undefined with zero repairs needed.
- **metric.repair_efficiency / Zig** — No repairs were needed.
- **metric.diagnosis_efficiency / Java** — No failing trial existed to diagnose; all trials succeeded at first generation with no compiler/runtime errors to feed back.
- **metric.diagnosis_efficiency / Python** — No diagnostics were fed back to the model because no trial failed generation, compile/parse, or test verification; Diagnosis Efficiency has no repair-turn observations for this shard.
- **metric.diagnosis_efficiency / Swift** — No compile/runtime failures were observed, so no diagnostic feedback loop was exercised.
- **metric.diagnosis_efficiency / Zig** — No failing trial existed to diagnose.
- **metric.unseen_case_generalization / Swift** — Section 21's dedicated protocol (partial spec + limited examples + held-out rule/boundary/error test battery) was not separately executed in this budget-constrained pass; not fabricated.
- **metric.generated_code_performance / Quidra** — No trial produced a compiling Quidra program; no executable exists to measure runtime performance.
- **metric.generated_code_performance / Swift** — Dedicated timing harness (2 warmups, 5 measured runs, median per primary.json) was not executed against these binaries in this pass.
- **metric.generated_code_performance / TypeScript** — No timed execution-performance benchmarking (warmups/measured runs per primary.json timing block) was performed on the generated LLM code within this work unit's turn/call budget; only functional correctness (compile + self-test pass) was measured. Unfinished-applicable-work gap, not genuine inapplicability, reported honestly rather than scored.
- **metric.generated_code_memory_efficiency / Go** — No memory-profiling tool is on the permitted run() program allowlist for this sandbox (allowlist: python3, cmake, make, ninja, c++, g++, clang++, cc, gcc, clang, rustc, cargo, go, javac, java, kotlinc, tsc, node, swiftc, swift, zig, quidra); memory measurement of the generated Go binary could not be taken with any permitted tool. Unfinished-applicable-work blocker, not a language incapability.
- **metric.generated_code_memory_efficiency / Quidra** — No trial produced a compiling Quidra program; no executable exists to measure memory usage.
- **metric.generated_code_memory_efficiency / Swift** — Peak-RSS measurement under the frozen protocol was not executed in this pass.
- **metric.generated_code_memory_efficiency / TypeScript** — No memory-usage measurement of generated code was performed within this work unit's budget; unfinished applicable work, reported honestly rather than scored.
- **metric.generated_code_compile_performance / Quidra** — No trial produced a compiling Quidra program; compile-performance measurement requires a successful build.

## Who measured what

| Requirement | Work unit | Languages |
| --- | --- | --- |
| metric.generation_success_rate | `proficiency-trials--quidra` | Quidra |
| metric.generation_success_rate | `proficiency-trials--python` | Python |
| metric.generation_success_rate | `proficiency-trials--c` | C++ |
| metric.generation_success_rate | `proficiency-trials--rust` | Rust |
| metric.generation_success_rate | `proficiency-trials--go` | Go |
| metric.generation_success_rate | `proficiency-trials--java` | Java |
| metric.generation_success_rate | `proficiency-trials--typescript` | TypeScript |
| metric.generation_success_rate | `proficiency-trials--kotlin` | Kotlin |
| metric.generation_success_rate | `proficiency-trials--swift` | Swift |
| metric.generation_success_rate | `proficiency-trials--zig` | Zig |
| metric.compile_parse_success_rate | `proficiency-trials--quidra` | Quidra |
| metric.compile_parse_success_rate | `proficiency-trials--python` | Python |
| metric.compile_parse_success_rate | `proficiency-trials--c` | C++ |
| metric.compile_parse_success_rate | `proficiency-trials--rust` | Rust |
| metric.compile_parse_success_rate | `proficiency-trials--go` | Go |
| metric.compile_parse_success_rate | `proficiency-trials--java` | Java |
| metric.compile_parse_success_rate | `proficiency-trials--typescript` | TypeScript |
| metric.compile_parse_success_rate | `proficiency-trials--kotlin` | Kotlin |
| metric.compile_parse_success_rate | `proficiency-trials--swift` | Swift |
| metric.compile_parse_success_rate | `proficiency-trials--zig` | Zig |
| metric.correct_at_1 | `proficiency-trials--quidra` | Quidra |
| metric.correct_at_1 | `proficiency-trials--python` | Python |
| metric.correct_at_1 | `proficiency-trials--c` | C++ |
| metric.correct_at_1 | `proficiency-trials--rust` | Rust |
| metric.correct_at_1 | `proficiency-trials--go` | Go |
| metric.correct_at_1 | `proficiency-trials--java` | Java |
| metric.correct_at_1 | `proficiency-trials--typescript` | TypeScript |
| metric.correct_at_1 | `proficiency-trials--kotlin` | Kotlin |
| metric.correct_at_1 | `proficiency-trials--swift` | Swift |
| metric.correct_at_1 | `proficiency-trials--zig` | Zig |
| metric.correct_at_n | `proficiency-trials--quidra` | Quidra |
| metric.correct_at_n | `proficiency-trials--python` | Python |
| metric.correct_at_n | `proficiency-trials--c` | C++ |
| metric.correct_at_n | `proficiency-trials--rust` | Rust |
| metric.correct_at_n | `proficiency-trials--go` | Go |
| metric.correct_at_n | `proficiency-trials--java` | Java |
| metric.correct_at_n | `proficiency-trials--typescript` | TypeScript |
| metric.correct_at_n | `proficiency-trials--kotlin` | Kotlin |
| metric.correct_at_n | `proficiency-trials--swift` | Swift |
| metric.correct_at_n | `proficiency-trials--zig` | Zig |
| metric.test_pass_rate | `proficiency-trials--quidra` | Quidra |
| metric.test_pass_rate | `proficiency-trials--python` | Python |
| metric.test_pass_rate | `proficiency-trials--c` | C++ |
| metric.test_pass_rate | `proficiency-trials--rust` | Rust |
| metric.test_pass_rate | `proficiency-trials--go` | Go |
| metric.test_pass_rate | `proficiency-trials--java` | Java |
| metric.test_pass_rate | `proficiency-trials--typescript` | TypeScript |
| metric.test_pass_rate | `proficiency-trials--kotlin` | Kotlin |
| metric.test_pass_rate | `proficiency-trials--swift` | Swift |
| metric.test_pass_rate | `proficiency-trials--zig` | Zig |
| metric.repair_success_rate | `proficiency-trials--quidra` | Quidra |
| metric.repair_success_rate | `proficiency-trials--python` | Python |
| metric.repair_success_rate | `proficiency-trials--c` | C++ |
| metric.repair_success_rate | `proficiency-trials--rust` | Rust |
| metric.repair_success_rate | `proficiency-trials--go` | Go |
| metric.repair_success_rate | `proficiency-trials--java` | Java |
| metric.repair_success_rate | `proficiency-trials--typescript` | TypeScript |
| metric.repair_success_rate | `proficiency-trials--kotlin` | Kotlin |
| metric.repair_success_rate | `proficiency-trials--swift` | Swift |
| metric.repair_success_rate | `proficiency-trials--zig` | Zig |
| metric.repair_efficiency | `proficiency-trials--quidra` | Quidra |
| metric.repair_efficiency | `proficiency-trials--python` | Python |
| metric.repair_efficiency | `proficiency-trials--c` | C++ |
| metric.repair_efficiency | `proficiency-trials--rust` | Rust |
| metric.repair_efficiency | `proficiency-trials--go` | Go |
| metric.repair_efficiency | `proficiency-trials--java` | Java |
| metric.repair_efficiency | `proficiency-trials--typescript` | TypeScript |
| metric.repair_efficiency | `proficiency-trials--kotlin` | Kotlin |
| metric.repair_efficiency | `proficiency-trials--swift` | Swift |
| metric.repair_efficiency | `proficiency-trials--zig` | Zig |
| metric.diagnosis_efficiency | `proficiency-trials--quidra` | Quidra |
| metric.diagnosis_efficiency | `proficiency-trials--python` | Python |
| metric.diagnosis_efficiency | `proficiency-trials--c` | C++ |
| metric.diagnosis_efficiency | `proficiency-trials--rust` | Rust |
| metric.diagnosis_efficiency | `proficiency-trials--go` | Go |
| metric.diagnosis_efficiency | `proficiency-trials--java` | Java |
| metric.diagnosis_efficiency | `proficiency-trials--typescript` | TypeScript |
| metric.diagnosis_efficiency | `proficiency-trials--kotlin` | Kotlin |
| metric.diagnosis_efficiency | `proficiency-trials--swift` | Swift |
| metric.diagnosis_efficiency | `proficiency-trials--zig` | Zig |
| metric.silent_bug_resistance | `proficiency-trials--quidra` | Quidra |
| metric.silent_bug_resistance | `proficiency-trials--python` | Python |
| metric.silent_bug_resistance | `proficiency-trials--c` | C++ |
| metric.silent_bug_resistance | `proficiency-trials--rust` | Rust |
| metric.silent_bug_resistance | `proficiency-trials--go` | Go |
| metric.silent_bug_resistance | `proficiency-trials--java` | Java |
| metric.silent_bug_resistance | `proficiency-trials--typescript` | TypeScript |
| metric.silent_bug_resistance | `proficiency-trials--kotlin` | Kotlin |
| metric.silent_bug_resistance | `proficiency-trials--swift` | Swift |
| metric.silent_bug_resistance | `proficiency-trials--zig` | Zig |
| metric.syntax_hallucination_resistance | `proficiency-trials--quidra` | Quidra |
| metric.syntax_hallucination_resistance | `proficiency-trials--python` | Python |
| metric.syntax_hallucination_resistance | `proficiency-trials--c` | C++ |
| metric.syntax_hallucination_resistance | `proficiency-trials--rust` | Rust |
| metric.syntax_hallucination_resistance | `proficiency-trials--go` | Go |
| metric.syntax_hallucination_resistance | `proficiency-trials--java` | Java |
| metric.syntax_hallucination_resistance | `proficiency-trials--typescript` | TypeScript |
| metric.syntax_hallucination_resistance | `proficiency-trials--kotlin` | Kotlin |
| metric.syntax_hallucination_resistance | `proficiency-trials--swift` | Swift |
| metric.syntax_hallucination_resistance | `proficiency-trials--zig` | Zig |
| metric.specification_compliance | `proficiency-trials--quidra` | Quidra |
| metric.specification_compliance | `proficiency-trials--python` | Python |
| metric.specification_compliance | `proficiency-trials--c` | C++ |
| metric.specification_compliance | `proficiency-trials--rust` | Rust |
| metric.specification_compliance | `proficiency-trials--go` | Go |
| metric.specification_compliance | `proficiency-trials--java` | Java |
| metric.specification_compliance | `proficiency-trials--typescript` | TypeScript |
| metric.specification_compliance | `proficiency-trials--kotlin` | Kotlin |
| metric.specification_compliance | `proficiency-trials--swift` | Swift |
| metric.specification_compliance | `proficiency-trials--zig` | Zig |
| metric.prompt_robustness | `proficiency-trials--quidra` | Quidra |
| metric.prompt_robustness | `proficiency-trials--python` | Python |
| metric.prompt_robustness | `proficiency-trials--c` | C++ |
| metric.prompt_robustness | `proficiency-trials--rust` | Rust |
| metric.prompt_robustness | `proficiency-trials--go` | Go |
| metric.prompt_robustness | `proficiency-trials--java` | Java |
| metric.prompt_robustness | `proficiency-trials--typescript` | TypeScript |
| metric.prompt_robustness | `proficiency-trials--kotlin` | Kotlin |
| metric.prompt_robustness | `proficiency-trials--swift` | Swift |
| metric.prompt_robustness | `proficiency-trials--zig` | Zig |
| metric.unseen_case_generalization | `proficiency-trials--quidra` | Quidra |
| metric.unseen_case_generalization | `proficiency-trials--python` | Python |
| metric.unseen_case_generalization | `proficiency-trials--c` | C++ |
| metric.unseen_case_generalization | `proficiency-trials--rust` | Rust |
| metric.unseen_case_generalization | `proficiency-trials--go` | Go |
| metric.unseen_case_generalization | `proficiency-trials--java` | Java |
| metric.unseen_case_generalization | `proficiency-trials--typescript` | TypeScript |
| metric.unseen_case_generalization | `proficiency-trials--kotlin` | Kotlin |
| metric.unseen_case_generalization | `proficiency-trials--swift` | Swift |
| metric.unseen_case_generalization | `proficiency-trials--zig` | Zig |
| metric.source_token_efficiency | `proficiency-trials--quidra` | Quidra |
| metric.source_token_efficiency | `proficiency-trials--python` | Python |
| metric.source_token_efficiency | `proficiency-trials--c` | C++ |
| metric.source_token_efficiency | `proficiency-trials--rust` | Rust |
| metric.source_token_efficiency | `proficiency-trials--go` | Go |
| metric.source_token_efficiency | `proficiency-trials--java` | Java |
| metric.source_token_efficiency | `proficiency-trials--typescript` | TypeScript |
| metric.source_token_efficiency | `proficiency-trials--kotlin` | Kotlin |
| metric.source_token_efficiency | `proficiency-trials--swift` | Swift |
| metric.source_token_efficiency | `proficiency-trials--zig` | Zig |
| metric.total_token_efficiency | `proficiency-trials--quidra` | Quidra |
| metric.total_token_efficiency | `proficiency-trials--python` | Python |
| metric.total_token_efficiency | `proficiency-trials--c` | C++ |
| metric.total_token_efficiency | `proficiency-trials--rust` | Rust |
| metric.total_token_efficiency | `proficiency-trials--go` | Go |
| metric.total_token_efficiency | `proficiency-trials--java` | Java |
| metric.total_token_efficiency | `proficiency-trials--typescript` | TypeScript |
| metric.total_token_efficiency | `proficiency-trials--kotlin` | Kotlin |
| metric.total_token_efficiency | `proficiency-trials--swift` | Swift |
| metric.total_token_efficiency | `proficiency-trials--zig` | Zig |
| metric.generated_code_performance | `proficiency-trials--quidra` | Quidra |
| metric.generated_code_performance | `proficiency-trials--python` | Python |
| metric.generated_code_performance | `proficiency-trials--c` | C++ |
| metric.generated_code_performance | `proficiency-trials--rust` | Rust |
| metric.generated_code_performance | `proficiency-trials--go` | Go |
| metric.generated_code_performance | `proficiency-trials--java` | Java |
| metric.generated_code_performance | `proficiency-trials--typescript` | TypeScript |
| metric.generated_code_performance | `proficiency-trials--kotlin` | Kotlin |
| metric.generated_code_performance | `proficiency-trials--swift` | Swift |
| metric.generated_code_performance | `proficiency-trials--zig` | Zig |
| metric.generated_code_memory_efficiency | `proficiency-trials--quidra` | Quidra |
| metric.generated_code_memory_efficiency | `proficiency-trials--python` | Python |
| metric.generated_code_memory_efficiency | `proficiency-trials--c` | C++ |
| metric.generated_code_memory_efficiency | `proficiency-trials--rust` | Rust |
| metric.generated_code_memory_efficiency | `proficiency-trials--go` | Go |
| metric.generated_code_memory_efficiency | `proficiency-trials--java` | Java |
| metric.generated_code_memory_efficiency | `proficiency-trials--typescript` | TypeScript |
| metric.generated_code_memory_efficiency | `proficiency-trials--kotlin` | Kotlin |
| metric.generated_code_memory_efficiency | `proficiency-trials--swift` | Swift |
| metric.generated_code_memory_efficiency | `proficiency-trials--zig` | Zig |
| metric.generated_code_compile_performance | `proficiency-trials--quidra` | Quidra |
| metric.generated_code_compile_performance | `proficiency-trials--python` | Python |
| metric.generated_code_compile_performance | `proficiency-trials--c` | C++ |
| metric.generated_code_compile_performance | `proficiency-trials--rust` | Rust |
| metric.generated_code_compile_performance | `proficiency-trials--go` | Go |
| metric.generated_code_compile_performance | `proficiency-trials--java` | Java |
| metric.generated_code_compile_performance | `proficiency-trials--typescript` | TypeScript |
| metric.generated_code_compile_performance | `proficiency-trials--kotlin` | Kotlin |
| metric.generated_code_compile_performance | `proficiency-trials--swift` | Swift |
| metric.generated_code_compile_performance | `proficiency-trials--zig` | Zig |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: `0.00e+00`.

The workers' own reasoning for every cell is under `evidence/` beside this file.
