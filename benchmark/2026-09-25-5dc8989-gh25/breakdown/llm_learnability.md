# LLM Learnability: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Swift | 100.0 |
| 2 | TypeScript | 99.1 |
| 3 | Rust | 98.4 |
| 4 | Python | 98.02 |
| 5 | Java | 97.8 |
| 6 | Kotlin | 97.6 |
| 7 | C++ | 97.5 |
| 8 | Go | 91.58 |
| 9 | Zig | 81.67 |
| 10 | Quidra | 74.7 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| condition.i1_keyword_anonymization | 0.2 | 96 (9) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 67.67 (10) |
| condition.i2_vocabulary_anonymization | 0.2 | 71 (8) | 100 (1) | 92 (7) | 100 (1) | 66.67 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 67.67 (9) |
| condition.i3_structural_surface_perturbation | 0.15 | 100 (1) | 100 (1) | 100 (1) | 96 (6) | 95 (8) | 92 (9) | 100 (1) | 96 (6) | 100 (1) | 92 (9) |
| condition.i4_novel_rule_generalization | 0.2 | 100 (1) | 100 (1) | 100 (1) | 95 (6) | 95 (6) | 95 (6) | 100 (1) | 95 (6) | 100 (1) | 92 (10) |
| condition.i5_held_out_rule_composition | 0.15 | 34 (10) | 91.5 (8) | 96 (5) | 100 (1) | 100 (1) | 100 (1) | 96 (5) | 96 (5) | 100 (1) | 88 (9) |
| condition.i6_prior_conflict_resistance | 0.1 | 12 (10) | 93 (8) | 97 (6) | 100 (1) | 100 (1) | 100 (1) | 97 (6) | 98 (5) | 100 (1) | 92 (9) |

## Who measured what

| Requirement | Work unit | Languages |
| --- | --- | --- |
| condition.i1_keyword_anonymization | `learnability-i1-i2--quidra` | Quidra |
| condition.i1_keyword_anonymization | `learnability-i1-i2--python` | Python |
| condition.i1_keyword_anonymization | `learnability-i1-i2--c` | C++ |
| condition.i1_keyword_anonymization | `learnability-i1-i2--rust` | Rust |
| condition.i1_keyword_anonymization | `learnability-i1-i2--go` | Go |
| condition.i1_keyword_anonymization | `learnability-i1-i2--java` | Java |
| condition.i1_keyword_anonymization | `learnability-i1-i2--typescript` | TypeScript |
| condition.i1_keyword_anonymization | `learnability-i1-i2--kotlin` | Kotlin |
| condition.i1_keyword_anonymization | `learnability-i1-i2--swift` | Swift |
| condition.i1_keyword_anonymization | `learnability-i1-i2--zig` | Zig |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--quidra` | Quidra |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--python` | Python |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--c` | C++ |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--rust` | Rust |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--go` | Go |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--java` | Java |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--typescript` | TypeScript |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--kotlin` | Kotlin |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--swift` | Swift |
| condition.i2_vocabulary_anonymization | `learnability-i1-i2--zig` | Zig |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--quidra` | Quidra |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--python` | Python |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--c` | C++ |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--rust` | Rust |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--go` | Go |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--java` | Java |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--typescript` | TypeScript |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--kotlin` | Kotlin |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--swift` | Swift |
| condition.i3_structural_surface_perturbation | `learnability-i3-i4--zig` | Zig |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--quidra` | Quidra |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--python` | Python |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--c` | C++ |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--rust` | Rust |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--go` | Go |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--java` | Java |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--typescript` | TypeScript |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--kotlin` | Kotlin |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--swift` | Swift |
| condition.i4_novel_rule_generalization | `learnability-i3-i4--zig` | Zig |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--quidra` | Quidra |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--python` | Python |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--c` | C++ |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--rust` | Rust |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--go` | Go |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--java` | Java |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--typescript` | TypeScript |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--kotlin` | Kotlin |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--swift` | Swift |
| condition.i5_held_out_rule_composition | `learnability-i5-i6--zig` | Zig |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--quidra` | Quidra |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--python` | Python |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--c` | C++ |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--rust` | Rust |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--go` | Go |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--java` | Java |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--typescript` | TypeScript |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--kotlin` | Kotlin |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--swift` | Swift |
| condition.i6_prior_conflict_resistance | `learnability-i5-i6--zig` | Zig |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: `0.00e+00`.

The workers' own reasoning for every cell is under `evidence/` beside this file.
