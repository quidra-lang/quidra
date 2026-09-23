# Semantic Compression: score breakdown

Status: **None**

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.semantic_density | 0.2 | 17 (4) | 10.2 (9) | 48 (2) | 11.42 (8) | 11.77 (7) | 82 (1) | 8.9 (10) | 13 (5) | 12.85 (6) | 39 (3) |
| metric.semantic_determinacy | 0.25 | 100 (1) | 94.12 (5) | 90.02 (8) | 97.73 (2) | 93.9 (6) | 91 (7) | 96.6 (4) | 89.46 (10) | 97 (3) | 89.9 (9) |
| metric.semantic_locality | 0.2 | 67 (9) | 65.15 (10) | 87.1 (6) | 92 (2) | 85.23 (7) | 88.2 (4) | 90 (3) | 88 (5) | 70.45 (8) | 97.49 (1) |
| metric.hidden_semantic_cost | 0.2 | 97 (2) | 89.2 (7) | 90 (6) | 84 (9) | 93 (4) | 93 (4) | 88.08 (8) | 45 (10) | 93.7 (3) | 98 (1) |
| metric.capability_efficiency | 0.15 | 63.84 (4) | 29.65 (10) | 100 (1) | 40.2 (7) | 46 (6) | 64.7 (3) | 32 (9) | 39.2 (8) | 75 (2) | 50 (5) |
| metric.capability_coverage |  | 81.82 (9) | 76.14 (10) | 98.86 (1) | 98.86 (1) | 90.91 (4) | 89.77 (6) | 85.23 (8) | 88.64 (7) | 97.73 (3) | 90.91 (4) |

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
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--quidra` | Quidra |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--python` | Python |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--c` | C++ |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--rust` | Rust |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--go` | Go |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--java` | Java |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--typescript` | TypeScript |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--kotlin` | Kotlin |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--swift` | Swift |
| metric.capability_coverage | `sc-metrics-hidden-coverage--part-2--zig` | Zig |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: not applicable (no published scores).

The workers' own reasoning for every cell is under `evidence/` beside this file.
