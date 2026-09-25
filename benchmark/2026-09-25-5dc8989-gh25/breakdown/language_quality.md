# Language Quality: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Rust | 72.24 |
| 2 | Go | 67.22 |
| 3 | Zig | 66.76 |
| 4 | Quidra | 66.13 |
| 5 | Python | 64.4 |
| 6 | Swift | 63.64 |
| 7 | Java | 58.77 |
| 8 | Kotlin | 56.95 |
| 9 | C++ | 51.32 |
| 10 | TypeScript | 46.72 |

## Categories

| Category | Weight | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| performance | 0.25 | 47.4 | 29.6 | 59.1 | 64.8 | 54.0 | 35.4 | 22.2 | 33.6 | 43.6 | 73.1 |
| resource | 0.1875 | 67.3 | 89.9 | 68.0 | 68.9 | 68.7 | 44.0 | 40.2 | 32.4 | 64.5 | 64.7 |
| language_development | 0.25 | 85.6 | 76.2 | 59.4 | 91.9 | 78.8 | 79.4 | 75.0 | 87.5 | 93.1 | 83.8 |
| safety_robustness | 0.3125 | 64.8 | 67.4 | 28.7 | 64.5 | 67.7 | 69.8 | 47.6 | 65.9 | 55.6 | 49.4 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.native_execution_performance<br>_performance_ |  | 63.0244 (6) | 3.7684 (10) | 87.2058 (2) | 84.0001 (3) | 76.2633 (4) | 62.8212 (7) | 42.1623 (9) | 61.429 (8) | 71.0768 (5) | 97.0057 (1) |
| metric.long_running_performance<br>_performance_ |  | 61.8059 (8) | 3.67882 (10) | 85.8472 (2) | 82.0441 (3) | 74.3389 (5) | 74.5534 (4) | 41.4371 (9) | 70.5354 (6) | 69.5348 (7) | 95.3151 (1) |
| metric.compile_build_performance<br>_performance_ |  | 0.604074 (3) | 100 (1) | 0.0691769 (8) | 0.556209 (4) | 0.767761 (2) | 0.16148 (6) | 0.304773 (5) | 0.0186292 (9) | 0.14107 (7) | 0.00668313 (10) |
| metric.startup_latency<br>_performance_ |  | 64.0952 (4) | 11.074 (7) | 63.0792 (5) | 92.4442 (2) | 64.7617 (3) | 4.04949 (9) | 4.94905 (8) | 2.49002 (10) | 33.4629 (6) | 100 (1) |
| metric.memory_efficiency<br>_resource_ |  | 75.1712 (6) | 64.8395 (7) | 92.0684 (3) | 95.9313 (2) | 83.5475 (4) | 21.8709 (8) | 21.8748 (8) | 21.3486 (10) | 75.9375 (5) | 97.8123 (1) |
| metric.source_code_size<br>_resource_ |  | 92.3215 (2) | 95.1332 (1) | 63.2393 (8) | 79.8707 (5) | 91.2113 (3) | 75.0483 (6) | 60.8661 (10) | 84.0443 (4) | 71.1057 (7) | 61.2018 (9) |
| metric.binary_artifact_size<br>_resource_ |  | 1.69795 (6) | 100 (1) | 17.0934 (4) | 0.0936727 (9) | 0.168389 (7) | 52.7775 (3) | 55.9037 (2) | 0.0749731 (10) | 11.3596 (5) | 0.111721 (8) |
| metric.runtime_overhead<br>_resource_ |  | 100 (1) | 99.7186 (2) | 99.6485 (7) | 99.6835 (4) | 99.6835 (4) | 26.1942 (8) | 22.1035 (10) | 24.2702 (9) | 99.7186 (2) | 99.6835 (4) |
| metric.code_efficiency_conciseness<br>_language_development_ |  | 90 (3) | 90 (3) | 50 (9) | 75 (6) | 60 (8) | 50 (9) | 90 (3) | 95 (1) | 95 (1) | 65 (7) |
| metric.readability<br>_language_development_ |  | 95 (1) | 85 (5) | 50 (10) | 80 (7) | 95 (1) | 85 (5) | 75 (8) | 90 (3) | 90 (3) | 75 (8) |
| metric.functionality_expressiveness<br>_language_development_ |  | 85 (7) | 90 (4) | 90 (4) | 100 (1) | 75 (10) | 85 (7) | 90 (4) | 95 (2) | 95 (2) | 80 (9) |
| metric.diagnostics<br>_language_development_ |  | 100 (1) | 65 (8) | 60 (10) | 100 (1) | 65 (8) | 75 (7) | 95 (3) | 85 (6) | 95 (3) | 95 (3) |
| metric.dependency_simplicity<br>_language_development_ |  | 100 (1) | 60 (9) | 35 (10) | 100 (1) | 100 (1) | 75 (6) | 65 (7) | 65 (7) | 95 (4) | 95 (4) |
| metric.portability_design_platform_neutrality<br>_language_development_ |  | 90 (5) | 90 (5) | 50 (10) | 90 (5) | 100 (1) | 95 (4) | 80 (8) | 100 (1) | 80 (8) | 100 (1) |
| metric.ffi_interoperability_design<br>_language_development_ |  | 70 (7) | 70 (7) | 80 (6) | 100 (1) | 65 (9) | 85 (4) | 30 (10) | 85 (4) | 95 (2) | 95 (2) |
| metric.concurrency<br>_language_development_ |  | 55 (10) | 60 (8) | 60 (8) | 90 (2) | 70 (6) | 85 (3) | 75 (5) | 85 (3) | 100 (1) | 65 (7) |
| metric.type_safety<br>_safety_robustness_ |  | 74.58 (1) | 29.17 (9) | 19.58 (10) | 51.67 (6) | 62.5 (4) | 45.83 (7) | 33.33 (8) | 56.25 (5) | 67.08 (2) | 67.08 (2) |
| metric.memory_safety<br>_safety_robustness_ |  | 44.44 (7) | 66.67 (5) | 11.11 (10) | 77.78 (4) | 88.89 (3) | 100 (1) | 55.56 (6) | 100 (1) | 44.44 (7) | 33.33 (9) |
| metric.runtime_safety<br>_safety_robustness_ |  | 41.67 (5) | 68.75 (1) | 3.33 (10) | 39.02 (6) | 51.16 (3) | 57.69 (2) | 22.22 (7) | 45.83 (4) | 13.95 (8) | 9.76 (9) |
| metric.boundary_value_safety<br>_safety_robustness_ |  | 54.55 (3) | 65.91 (1) | 7.27 (10) | 51.93 (5) | 60.45 (2) | 42.27 (7) | 22.27 (9) | 37.73 (8) | 47.27 (6) | 53.75 (4) |
| metric.adversarial_input_robustness<br>_safety_robustness_ |  | 50.71 (8) | 79.29 (2) | 48.57 (9) | 69.29 (4) | 64.29 (5) | 85.71 (1) | 59.29 (6) | 75 (3) | 51.43 (7) | 42.86 (10) |
| metric.early_error_detection<br>_safety_robustness_ |  | 59.81 (5) | 57.88 (6) | 25.38 (10) | 64.47 (2) | 64.04 (3) | 65 (1) | 44.62 (9) | 61.15 (4) | 55.96 (7) | 49.86 (8) |
| metric.debuggability<br>_safety_robustness_ |  | 66.67 (2) | 68.59 (1) | 13.46 (10) | 54.49 (4) | 45.83 (6) | 61.54 (3) | 38.46 (7) | 52.24 (5) | 32.05 (8) | 27.24 (9) |
| metric.silent_bug_resistance<br>_safety_robustness_ |  | 91.18 (1) | 70.59 (5) | 29.41 (10) | 75 (3) | 72.06 (4) | 70.59 (5) | 52.94 (9) | 64.71 (7) | 88.24 (2) | 63.24 (8) |
| metric.implementation_robustness<br>_safety_robustness_ |  | 100 (1) | 100 (1) | 100 (1) | 97.06 (9) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 97.06 (9) |

## Who measured what

| Requirement | Work unit | Languages |
| --- | --- | --- |
| metric.native_execution_performance | `lq-micro-mechanical` | all |
| metric.long_running_performance | `lq-micro-mechanical` | all |
| metric.compile_build_performance | `lq-micro-mechanical` | all |
| metric.startup_latency | `lq-micro-mechanical` | all |
| metric.memory_efficiency | `lq-micro-mechanical` | all |
| metric.source_code_size | `lq-micro-mechanical` | all |
| metric.binary_artifact_size | `lq-micro-mechanical` | all |
| metric.runtime_overhead | `lq-micro-mechanical` | all |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--quidra` | Quidra |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--python` | Python |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--c` | C++ |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--rust` | Rust |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--go` | Go |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--java` | Java |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--typescript` | TypeScript |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--kotlin` | Kotlin |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--swift` | Swift |
| metric.code_efficiency_conciseness | `lq-language-development--part-1--zig` | Zig |
| metric.readability | `lq-language-development--part-1--quidra` | Quidra |
| metric.readability | `lq-language-development--part-1--python` | Python |
| metric.readability | `lq-language-development--part-1--c` | C++ |
| metric.readability | `lq-language-development--part-1--rust` | Rust |
| metric.readability | `lq-language-development--part-1--go` | Go |
| metric.readability | `lq-language-development--part-1--java` | Java |
| metric.readability | `lq-language-development--part-1--typescript` | TypeScript |
| metric.readability | `lq-language-development--part-1--kotlin` | Kotlin |
| metric.readability | `lq-language-development--part-1--swift` | Swift |
| metric.readability | `lq-language-development--part-1--zig` | Zig |
| metric.functionality_expressiveness | `lq-language-development--part-2--quidra` | Quidra |
| metric.functionality_expressiveness | `lq-language-development--part-2--python` | Python |
| metric.functionality_expressiveness | `lq-language-development--part-2--c` | C++ |
| metric.functionality_expressiveness | `lq-language-development--part-2--rust` | Rust |
| metric.functionality_expressiveness | `lq-language-development--part-2--go` | Go |
| metric.functionality_expressiveness | `lq-language-development--part-2--java` | Java |
| metric.functionality_expressiveness | `lq-language-development--part-2--typescript` | TypeScript |
| metric.functionality_expressiveness | `lq-language-development--part-2--kotlin` | Kotlin |
| metric.functionality_expressiveness | `lq-language-development--part-2--swift` | Swift |
| metric.functionality_expressiveness | `lq-language-development--part-2--zig` | Zig |
| metric.diagnostics | `lq-language-development--part-2--quidra` | Quidra |
| metric.diagnostics | `lq-language-development--part-2--python` | Python |
| metric.diagnostics | `lq-language-development--part-2--c` | C++ |
| metric.diagnostics | `lq-language-development--part-2--rust` | Rust |
| metric.diagnostics | `lq-language-development--part-2--go` | Go |
| metric.diagnostics | `lq-language-development--part-2--java` | Java |
| metric.diagnostics | `lq-language-development--part-2--typescript` | TypeScript |
| metric.diagnostics | `lq-language-development--part-2--kotlin` | Kotlin |
| metric.diagnostics | `lq-language-development--part-2--swift` | Swift |
| metric.diagnostics | `lq-language-development--part-2--zig` | Zig |
| metric.dependency_simplicity | `lq-language-development--part-3--quidra` | Quidra |
| metric.dependency_simplicity | `lq-language-development--part-3--python` | Python |
| metric.dependency_simplicity | `lq-language-development--part-3--c` | C++ |
| metric.dependency_simplicity | `lq-language-development--part-3--rust` | Rust |
| metric.dependency_simplicity | `lq-language-development--part-3--go` | Go |
| metric.dependency_simplicity | `lq-language-development--part-3--java` | Java |
| metric.dependency_simplicity | `lq-language-development--part-3--typescript` | TypeScript |
| metric.dependency_simplicity | `lq-language-development--part-3--kotlin` | Kotlin |
| metric.dependency_simplicity | `lq-language-development--part-3--swift` | Swift |
| metric.dependency_simplicity | `lq-language-development--part-3--zig` | Zig |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--quidra` | Quidra |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--python` | Python |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--c` | C++ |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--rust` | Rust |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--go` | Go |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--java` | Java |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--typescript` | TypeScript |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--kotlin` | Kotlin |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--swift` | Swift |
| metric.portability_design_platform_neutrality | `lq-language-development--part-3--zig` | Zig |
| metric.ffi_interoperability_design | `lq-language-development--part-4--quidra` | Quidra |
| metric.ffi_interoperability_design | `lq-language-development--part-4--python` | Python |
| metric.ffi_interoperability_design | `lq-language-development--part-4--c` | C++ |
| metric.ffi_interoperability_design | `lq-language-development--part-4--rust` | Rust |
| metric.ffi_interoperability_design | `lq-language-development--part-4--go` | Go |
| metric.ffi_interoperability_design | `lq-language-development--part-4--java` | Java |
| metric.ffi_interoperability_design | `lq-language-development--part-4--typescript` | TypeScript |
| metric.ffi_interoperability_design | `lq-language-development--part-4--kotlin` | Kotlin |
| metric.ffi_interoperability_design | `lq-language-development--part-4--swift` | Swift |
| metric.ffi_interoperability_design | `lq-language-development--part-4--zig` | Zig |
| metric.concurrency | `lq-language-development--part-4--quidra` | Quidra |
| metric.concurrency | `lq-language-development--part-4--python` | Python |
| metric.concurrency | `lq-language-development--part-4--c` | C++ |
| metric.concurrency | `lq-language-development--part-4--rust` | Rust |
| metric.concurrency | `lq-language-development--part-4--go` | Go |
| metric.concurrency | `lq-language-development--part-4--java` | Java |
| metric.concurrency | `lq-language-development--part-4--typescript` | TypeScript |
| metric.concurrency | `lq-language-development--part-4--kotlin` | Kotlin |
| metric.concurrency | `lq-language-development--part-4--swift` | Swift |
| metric.concurrency | `lq-language-development--part-4--zig` | Zig |
| metric.type_safety | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.type_safety | `lq-adversarial-mechanical--python` | Python |
| metric.type_safety | `lq-adversarial-mechanical--c` | C++ |
| metric.type_safety | `lq-adversarial-mechanical--rust` | Rust |
| metric.type_safety | `lq-adversarial-mechanical--go` | Go |
| metric.type_safety | `lq-adversarial-mechanical--java` | Java |
| metric.type_safety | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.type_safety | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.type_safety | `lq-adversarial-mechanical--swift` | Swift |
| metric.type_safety | `lq-adversarial-mechanical--zig` | Zig |
| metric.memory_safety | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.memory_safety | `lq-adversarial-mechanical--python` | Python |
| metric.memory_safety | `lq-adversarial-mechanical--c` | C++ |
| metric.memory_safety | `lq-adversarial-mechanical--rust` | Rust |
| metric.memory_safety | `lq-adversarial-mechanical--go` | Go |
| metric.memory_safety | `lq-adversarial-mechanical--java` | Java |
| metric.memory_safety | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.memory_safety | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.memory_safety | `lq-adversarial-mechanical--swift` | Swift |
| metric.memory_safety | `lq-adversarial-mechanical--zig` | Zig |
| metric.runtime_safety | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.runtime_safety | `lq-adversarial-mechanical--python` | Python |
| metric.runtime_safety | `lq-adversarial-mechanical--c` | C++ |
| metric.runtime_safety | `lq-adversarial-mechanical--rust` | Rust |
| metric.runtime_safety | `lq-adversarial-mechanical--go` | Go |
| metric.runtime_safety | `lq-adversarial-mechanical--java` | Java |
| metric.runtime_safety | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.runtime_safety | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.runtime_safety | `lq-adversarial-mechanical--swift` | Swift |
| metric.runtime_safety | `lq-adversarial-mechanical--zig` | Zig |
| metric.boundary_value_safety | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.boundary_value_safety | `lq-adversarial-mechanical--python` | Python |
| metric.boundary_value_safety | `lq-adversarial-mechanical--c` | C++ |
| metric.boundary_value_safety | `lq-adversarial-mechanical--rust` | Rust |
| metric.boundary_value_safety | `lq-adversarial-mechanical--go` | Go |
| metric.boundary_value_safety | `lq-adversarial-mechanical--java` | Java |
| metric.boundary_value_safety | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.boundary_value_safety | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.boundary_value_safety | `lq-adversarial-mechanical--swift` | Swift |
| metric.boundary_value_safety | `lq-adversarial-mechanical--zig` | Zig |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--python` | Python |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--c` | C++ |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--rust` | Rust |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--go` | Go |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--java` | Java |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--swift` | Swift |
| metric.adversarial_input_robustness | `lq-adversarial-mechanical--zig` | Zig |
| metric.early_error_detection | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.early_error_detection | `lq-adversarial-mechanical--python` | Python |
| metric.early_error_detection | `lq-adversarial-mechanical--c` | C++ |
| metric.early_error_detection | `lq-adversarial-mechanical--rust` | Rust |
| metric.early_error_detection | `lq-adversarial-mechanical--go` | Go |
| metric.early_error_detection | `lq-adversarial-mechanical--java` | Java |
| metric.early_error_detection | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.early_error_detection | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.early_error_detection | `lq-adversarial-mechanical--swift` | Swift |
| metric.early_error_detection | `lq-adversarial-mechanical--zig` | Zig |
| metric.debuggability | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.debuggability | `lq-adversarial-mechanical--python` | Python |
| metric.debuggability | `lq-adversarial-mechanical--c` | C++ |
| metric.debuggability | `lq-adversarial-mechanical--rust` | Rust |
| metric.debuggability | `lq-adversarial-mechanical--go` | Go |
| metric.debuggability | `lq-adversarial-mechanical--java` | Java |
| metric.debuggability | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.debuggability | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.debuggability | `lq-adversarial-mechanical--swift` | Swift |
| metric.debuggability | `lq-adversarial-mechanical--zig` | Zig |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--python` | Python |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--c` | C++ |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--rust` | Rust |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--go` | Go |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--java` | Java |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--swift` | Swift |
| metric.silent_bug_resistance | `lq-adversarial-mechanical--zig` | Zig |
| metric.implementation_robustness | `lq-adversarial-mechanical--quidra` | Quidra |
| metric.implementation_robustness | `lq-adversarial-mechanical--python` | Python |
| metric.implementation_robustness | `lq-adversarial-mechanical--c` | C++ |
| metric.implementation_robustness | `lq-adversarial-mechanical--rust` | Rust |
| metric.implementation_robustness | `lq-adversarial-mechanical--go` | Go |
| metric.implementation_robustness | `lq-adversarial-mechanical--java` | Java |
| metric.implementation_robustness | `lq-adversarial-mechanical--typescript` | TypeScript |
| metric.implementation_robustness | `lq-adversarial-mechanical--kotlin` | Kotlin |
| metric.implementation_robustness | `lq-adversarial-mechanical--swift` | Swift |
| metric.implementation_robustness | `lq-adversarial-mechanical--zig` | Zig |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: `0.00e+00`.

The workers' own reasoning for every cell is under `evidence/` beside this file.
