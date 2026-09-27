# Semantic Compression: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Zig | 78.48 |
| 2 | Quidra | 77.52 |
| 3 | Swift | 77.34 |
| 4 | Go | 72.08 |
| 5 | Kotlin | 69.24 |
| 6 | Rust | 67.91 |
| 7 | Java | 53.48 |
| 8 | TypeScript | 48.19 |
| 9 | Python | 46.99 |
| 10 | C++ | 20.38 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.semantic_density | 0.2 | 100 (1) | 23.42 (7) | 4.96 (9) | 29.99 (6) | 45.65 (5) | 0 (10) | 10.88 (8) | 58.52 (4) | 70.49 (2) | 58.69 (3) |
| metric.semantic_determinacy | 0.25 | 100 (1) | 64.75 (6) | 0 (10) | 70.5 (4) | 41 (9) | 70.5 (4) | 80.33 (2) | 64.75 (6) | 80.33 (2) | 45.08 (8) |
| metric.semantic_locality | 0.2 | 34.45 (5) | 45.93 (3) | 0 (9) | 28.71 (6) | 91.86 (2) | 17.22 (8) | 40.19 (4) | 28.71 (6) | 0 (9) | 100 (1) |
| metric.hidden_semantic_cost | 0.2 | 81.48 (2) | 18.52 (8) | 11.11 (9) | 66.67 (4) | 48.15 (6) | 48.15 (6) | 0 (10) | 66.67 (4) | 74.07 (3) | 100 (1) |
| metric.capability_efficiency | 0.15 | 36.46 (7) | 0 (10) | 54.31 (5) | 60.12 (3) | 82.2 (2) | 47.9 (6) | 24.43 (9) | 59.63 (4) | 100 (1) | 27.91 (8) |
| metric.capability_coverage |  | 81.82 (9) | 77.27 (10) | 98.86 (1) | 98.86 (1) | 90.91 (5) | 90.91 (5) | 82.95 (8) | 90.91 (5) | 97.73 (3) | 94.32 (4) |

## Who measured what

| Requirement | Work unit | Languages |
| --- | --- | --- |
| metric.semantic_density | `sc-metrics-local--part-1--quidra` | Quidra |
| metric.semantic_density | `sc-metrics-local--part-1--python` | Python |
| metric.semantic_density | `sc-metrics-local--part-1--c` | C++ |
| metric.semantic_density | `sc-metrics-local--part-1--rust` | Rust |
| metric.semantic_density | `sc-metrics-local--part-1--go` | Go |
| metric.semantic_density | `sc-metrics-local--part-1--java` | Java |
| metric.semantic_density | `sc-metrics-local--part-1--typescript` | TypeScript |
| metric.semantic_density | `sc-metrics-local--part-1--kotlin` | Kotlin |
| metric.semantic_density | `sc-metrics-local--part-1--swift` | Swift |
| metric.semantic_density | `sc-metrics-local--part-1--zig` | Zig |
| metric.semantic_determinacy | `sc-metrics-local--part-3--quidra` | Quidra |
| metric.semantic_determinacy | `sc-metrics-local--part-3--python` | Python |
| metric.semantic_determinacy | `sc-metrics-local--part-3--c` | C++ |
| metric.semantic_determinacy | `sc-metrics-local--part-3--rust` | Rust |
| metric.semantic_determinacy | `sc-metrics-local--part-3--go` | Go |
| metric.semantic_determinacy | `sc-metrics-local--part-3--java` | Java |
| metric.semantic_determinacy | `sc-metrics-local--part-3--typescript` | TypeScript |
| metric.semantic_determinacy | `sc-metrics-local--part-3--kotlin` | Kotlin |
| metric.semantic_determinacy | `sc-metrics-local--part-3--swift` | Swift |
| metric.semantic_determinacy | `sc-metrics-local--part-3--zig` | Zig |
| metric.semantic_locality | `sc-metrics-local--part-2--quidra` | Quidra |
| metric.semantic_locality | `sc-metrics-local--part-2--python` | Python |
| metric.semantic_locality | `sc-metrics-local--part-2--c` | C++ |
| metric.semantic_locality | `sc-metrics-local--part-2--rust` | Rust |
| metric.semantic_locality | `sc-metrics-local--part-2--go` | Go |
| metric.semantic_locality | `sc-metrics-local--part-2--java` | Java |
| metric.semantic_locality | `sc-metrics-local--part-2--typescript` | TypeScript |
| metric.semantic_locality | `sc-metrics-local--part-2--kotlin` | Kotlin |
| metric.semantic_locality | `sc-metrics-local--part-2--swift` | Swift |
| metric.semantic_locality | `sc-metrics-local--part-2--zig` | Zig |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--quidra` | Quidra |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--python` | Python |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--c` | C++ |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--rust` | Rust |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--go` | Go |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--java` | Java |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--typescript` | TypeScript |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--kotlin` | Kotlin |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--swift` | Swift |
| metric.hidden_semantic_cost | `sc-metrics-hidden-coverage--part-1--zig` | Zig |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--quidra` | Quidra |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--python` | Python |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--c` | C++ |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--rust` | Rust |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--go` | Go |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--java` | Java |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--typescript` | TypeScript |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--kotlin` | Kotlin |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--swift` | Swift |
| metric.capability_efficiency | `sc-metrics-hidden-coverage--part-3--zig` | Zig |
| metric.capability_coverage | `sc-capability-coverage` | all |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: `0.00e+00`.

The workers' own reasoning for every cell is under `evidence/` beside this file.
