# LLM Proficiency: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Python | 90.13 |
| 2 | Rust | 86.88 |
| 3 | Go | 82.77 |
| 4 | Kotlin | 77.65 |
| 5 | C++ | 76.53 |
| 6 | Java | 75.97 |
| 7 | Swift | 67.32 |
| 8 | TypeScript | 34.11 |
| 9 | Quidra | 5.32 |
| 10 | Zig | 4.0 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.generation_success_rate | 0.04 | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) |
| metric.compile_parse_success_rate | 0.04 | 0 (8) | 88.8889 (2) | 66.6667 (7) | 77.7778 (3) | 72.2222 (4) | 94.4444 (1) | 0 (8) | 72.2222 (4) | 72.2222 (4) | 0 (8) |
| metric.correct_at_1 | 0.12 | 0 (8) | 88.8889 (1) | 66.6667 (6) | 77.7778 (3) | 72.2222 (4) | 83.3333 (2) | 0 (8) | 72.2222 (4) | 50 (7) | 0 (8) |
| metric.correct_at_n | 0.05 | 0 (9) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 88.8889 (7) | 61.1111 (8) | 100 (1) | 100 (1) | 0 (9) |
| metric.test_pass_rate | 0.1 | 0 (9) | 86.9565 (1) | 60 (6) | 82.1918 (2) | 80 (3) | 64.2857 (5) | 20.6704 (8) | 65.2174 (4) | 55.0459 (7) | 0 (9) |
| metric.repair_success_rate | 0.07 | 0 (9) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 33.3333 (8) | 61.1111 (7) | 100 (1) | 100 (1) | 0 (9) |
| metric.repair_efficiency | 0.05 | 0 (9) | 95.8333 (1) | 83.3333 (6) | 94.4444 (2) | 93.0556 (3) | 86.1111 (4) | 40.2778 (8) | 86.1111 (4) | 79.1667 (7) | 0 (9) |
| metric.diagnosis_efficiency | 0.05 | 0 (6) | 50 (3) | 0 (6) | 100 (1) | 100 (1) | 0 (6) | 44.4444 (4) | 0 (6) | 33.3333 (5) | 0 (6) |
| metric.silent_bug_resistance | 0.12 | 0 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 88.2353 (7) | 0 (8) | 100 (1) | 92.3077 (6) | 0 (8) |
| metric.syntax_hallucination_resistance | 0.07 | 0 (9) | 100 (1) | 90 (4) | 77.8 (5) | 72.2 (6) | 94.4 (3) | 100 (1) | 72.2 (6) | 50 (8) | 0 (9) |
| metric.specification_compliance | 0.07 | 0 (9) | 100 (1) | 100 (1) | 90 (4) | 72.2 (6) | 88.9 (5) | 61.11 (8) | 100 (1) | 66.67 (7) | 0 (9) |
| metric.prompt_robustness | 0.05 | 0 (8) | 66.6667 (2) | 50 (6) | 66.6667 (2) | 66.6667 (2) | 83.3333 (1) | 0 (8) | 66.6667 (2) | 33.3333 (7) | 0 (8) |
| metric.unseen_case_generalization | 0.06 | 0 (8) | 90.4762 (1) | 66.6667 (6) | 78.5714 (3) | 76.1905 (4) | 85.7143 (2) | 0 (8) | 73.8095 (5) | 52.381 (7) | 0 (8) |
| metric.source_token_efficiency | 0.03 | 25 (9) | 80 (1) | 70 (3) | 80 (1) | 65 (7) | 70 (3) | 55 (8) | 70 (3) | 70 (3) | 0 (10) |
| metric.total_token_efficiency | 0.03 | 10 (9) | 75 (1) | 70 (3) | 75 (1) | 65 (4) | 65 (4) | 35 (8) | 65 (4) | 55 (7) | 0 (10) |
| metric.generated_code_performance | 0.03 | N/A | N/A | 70 (3) | 80 (1) | 70 (3) | 75 (2) | 50 (7) | 70 (3) | 70 (3) | 0 (8) |
| metric.generated_code_memory_efficiency | 0.01 | N/A | N/A | 70 (3) | 80 (1) | 70 (3) | 75 (2) | 50 (7) | 70 (3) | 70 (3) | 0 (8) |
| metric.generated_code_compile_performance | 0.01 | N/A | N/A | 90 (1) | 85 (3) | 85 (3) | 90 (1) | 50 (7) | 60 (6) | 70 (5) | 0 (8) |

## N/A cells and their stated reasons

- **metric.generated_code_performance / Python** — No runner-owned timing measurements for generated Python solutions were exposed to this worker; only compile/gate verification booleans were preserved in session.json.
- **metric.generated_code_performance / Quidra** — No trial produced a program that passed the public repair gate in any of the 18 Primary cells (SVM/GMM/LightGrad x specification_to_implementation/reference_to_porting x 3 prompt-variant trials each), so no working Quidra binary exists to benchmark for runtime performance.
- **metric.generated_code_memory_efficiency / Python** — No runner-owned memory measurements for generated Python solutions were exposed to this worker.
- **metric.generated_code_memory_efficiency / Quidra** — No trial produced a program that passed the public repair gate, so no working Quidra binary exists to measure peak memory usage.
- **metric.generated_code_compile_performance / Python** — Python is an interpreted/parsed language with no separate compile-performance timing captured in preserved verification records; parse success is already captured under compile_parse_success_rate.
- **metric.generated_code_compile_performance / Quidra** — No trial produced source that ever reached a successful build+correct-output state; only 3 of 18 trials ever achieved compile_parse_ok=true at all (and only on later repair attempts, never at the first completion, and never with correct public output), so there is no comparable passing-build compile-time sample.

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
