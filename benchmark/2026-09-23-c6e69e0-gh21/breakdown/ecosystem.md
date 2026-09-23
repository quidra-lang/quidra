# Ecosystem: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Java | 95.55 |
| 2 | Python | 94.85 |
| 3 | Go | 94.5 |
| 4 | TypeScript | 92.35 |
| 5 | Rust | 92.1 |
| 6 | C++ | 91.35 |
| 7 | Kotlin | 91.1 |
| 8 | Swift | 82.35 |
| 9 | Zig | 55.6 |
| 10 | Quidra | 34.0 |

## Categories

| Category | Weight | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| external | 0.5 | 6.4 | 97.0 | 93.8 | 93.6 | 94.2 | 96.8 | 95.6 | 90.4 | 80.2 | 53.6 |
| tooling | 0.5 | 61.6 | 92.7 | 88.9 | 90.6 | 94.8 | 94.3 | 89.1 | 91.8 | 84.5 | 57.6 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.third_party_library_availability_domain_coverage<br>_external_ |  | 10 (10) | 98 (1) | 93 (5) | 96 (4) | 92 (6) | 97 (2) | 97 (2) | 92 (6) | 68 (9) | 70 (8) |
| metric.package_ecosystem_activity_maintenance<br>_external_ |  | 15 (10) | 97 (1) | 85 (7) | 93 (3) | 93 (3) | 96 (2) | 93 (3) | 86 (6) | 78 (8) | 45 (9) |
| metric.third_party_tool_availability<br>_external_ |  | 2 (10) | 97 (2) | 96 (4) | 91 (7) | 94 (5) | 98 (1) | 97 (2) | 94 (5) | 87 (8) | 55 (9) |
| metric.production_adoption_deployment_evidence<br>_external_ |  | 0 (10) | 96 (3) | 98 (1) | 92 (7) | 96 (3) | 97 (2) | 96 (3) | 95 (6) | 83 (8) | 40 (9) |
| metric.community_public_knowledge_availability<br>_external_ |  | 5 (10) | 97 (1) | 97 (1) | 96 (3) | 96 (3) | 96 (3) | 95 (6) | 85 (7) | 85 (7) | 58 (9) |
| metric.core_tooling_availability_quality<br>_tooling_ |  | 55 (10) | 96 (2) | 93 (5) | 93 (5) | 94 (4) | 98 (1) | 96 (2) | 92 (7) | 90 (8) | 68 (9) |
| metric.package_dependency_management_quality<br>_tooling_ |  | 27 (10) | 91 (4) | 61 (8) | 96 (1) | 94 (3) | 96 (1) | 88 (6) | 90 (5) | 78 (7) | 47 (9) |
| metric.ide_editor_support_quality<br>_tooling_ |  | 40 (10) | 95 (3) | 92 (5) | 90 (6) | 93 (4) | 97 (1) | 97 (1) | 90 (6) | 80 (8) | 70 (9) |
| metric.debugger_profiler_support<br>_tooling_ |  | 45 (10) | 95 (2) | 95 (2) | 76 (8) | 94 (5) | 98 (1) | 95 (2) | 87 (6) | 85 (7) | 52 (9) |
| metric.build_test_integration<br>_tooling_ |  | 91 (6) | 94 (4) | 87 (8) | 93 (5) | 100 (1) | 96 (2) | 91 (6) | 96 (2) | 82 (9) | 70 (10) |
| metric.documentation_quality<br>_tooling_ |  | 90 (6) | 93 (4) | 82 (9) | 97 (3) | 100 (1) | 90 (6) | 92 (5) | 98 (2) | 85 (8) | 68 (10) |
| metric.installation_distribution_experience<br>_tooling_ |  | 42 (10) | 97 (1) | 88 (6) | 95 (3) | 93 (5) | 96 (2) | 85 (7) | 94 (4) | 83 (8) | 60 (9) |
| metric.toolchain_stability_release_maturity<br>_tooling_ |  | 61 (9) | 95 (5) | 96 (2) | 92 (6) | 96 (2) | 97 (1) | 78 (8) | 96 (2) | 89 (7) | 32 (10) |
| metric.implemented_platform_coverage<br>_tooling_ |  | 80 (6) | 77 (8) | 100 (1) | 94 (2) | 88 (4) | 80 (6) | 74 (9) | 85 (5) | 90 (3) | 64 (10) |
| metric.external_integration_coverage<br>_tooling_ |  | 85 (7) | 94 (5) | 95 (2) | 80 (9) | 96 (1) | 95 (2) | 95 (2) | 90 (6) | 83 (8) | 45 (10) |

## Who measured what

| Requirement | Work unit | Languages |
| --- | --- | --- |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--quidra` | Quidra |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--python` | Python |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--c` | C++ |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--rust` | Rust |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--go` | Go |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--java` | Java |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--typescript` | TypeScript |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--kotlin` | Kotlin |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--swift` | Swift |
| metric.third_party_library_availability_domain_coverage | `ecosystem-external--part-1--zig` | Zig |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--quidra` | Quidra |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--python` | Python |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--c` | C++ |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--rust` | Rust |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--go` | Go |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--java` | Java |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--typescript` | TypeScript |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--kotlin` | Kotlin |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--swift` | Swift |
| metric.package_ecosystem_activity_maintenance | `ecosystem-external--part-1--zig` | Zig |
| metric.third_party_tool_availability | `ecosystem-external--part-2--quidra` | Quidra |
| metric.third_party_tool_availability | `ecosystem-external--part-2--python` | Python |
| metric.third_party_tool_availability | `ecosystem-external--part-2--c` | C++ |
| metric.third_party_tool_availability | `ecosystem-external--part-2--rust` | Rust |
| metric.third_party_tool_availability | `ecosystem-external--part-2--go` | Go |
| metric.third_party_tool_availability | `ecosystem-external--part-2--java` | Java |
| metric.third_party_tool_availability | `ecosystem-external--part-2--typescript` | TypeScript |
| metric.third_party_tool_availability | `ecosystem-external--part-2--kotlin` | Kotlin |
| metric.third_party_tool_availability | `ecosystem-external--part-2--swift` | Swift |
| metric.third_party_tool_availability | `ecosystem-external--part-2--zig` | Zig |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--quidra` | Quidra |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--python` | Python |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--c` | C++ |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--rust` | Rust |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--go` | Go |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--java` | Java |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--typescript` | TypeScript |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--kotlin` | Kotlin |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--swift` | Swift |
| metric.production_adoption_deployment_evidence | `ecosystem-external--part-2--zig` | Zig |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--quidra` | Quidra |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--python` | Python |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--c` | C++ |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--rust` | Rust |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--go` | Go |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--java` | Java |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--typescript` | TypeScript |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--kotlin` | Kotlin |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--swift` | Swift |
| metric.community_public_knowledge_availability | `ecosystem-external--part-3--zig` | Zig |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--quidra` | Quidra |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--python` | Python |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--c` | C++ |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--rust` | Rust |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--go` | Go |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--java` | Java |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--typescript` | TypeScript |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--kotlin` | Kotlin |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--swift` | Swift |
| metric.core_tooling_availability_quality | `ecosystem-tooling--part-1--zig` | Zig |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--quidra` | Quidra |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--python` | Python |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--c` | C++ |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--rust` | Rust |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--go` | Go |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--java` | Java |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--typescript` | TypeScript |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--kotlin` | Kotlin |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--swift` | Swift |
| metric.package_dependency_management_quality | `ecosystem-tooling--part-1--zig` | Zig |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--quidra` | Quidra |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--python` | Python |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--c` | C++ |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--rust` | Rust |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--go` | Go |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--java` | Java |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--typescript` | TypeScript |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--kotlin` | Kotlin |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--swift` | Swift |
| metric.ide_editor_support_quality | `ecosystem-tooling--part-2--zig` | Zig |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--quidra` | Quidra |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--python` | Python |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--c` | C++ |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--rust` | Rust |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--go` | Go |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--java` | Java |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--typescript` | TypeScript |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--kotlin` | Kotlin |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--swift` | Swift |
| metric.debugger_profiler_support | `ecosystem-tooling--part-2--zig` | Zig |
| metric.build_test_integration | `ecosystem-tooling--part-3--quidra` | Quidra |
| metric.build_test_integration | `ecosystem-tooling--part-3--python` | Python |
| metric.build_test_integration | `ecosystem-tooling--part-3--c` | C++ |
| metric.build_test_integration | `ecosystem-tooling--part-3--rust` | Rust |
| metric.build_test_integration | `ecosystem-tooling--part-3--go` | Go |
| metric.build_test_integration | `ecosystem-tooling--part-3--java` | Java |
| metric.build_test_integration | `ecosystem-tooling--part-3--typescript` | TypeScript |
| metric.build_test_integration | `ecosystem-tooling--part-3--kotlin` | Kotlin |
| metric.build_test_integration | `ecosystem-tooling--part-3--swift` | Swift |
| metric.build_test_integration | `ecosystem-tooling--part-3--zig` | Zig |
| metric.documentation_quality | `ecosystem-tooling--part-3--quidra` | Quidra |
| metric.documentation_quality | `ecosystem-tooling--part-3--python` | Python |
| metric.documentation_quality | `ecosystem-tooling--part-3--c` | C++ |
| metric.documentation_quality | `ecosystem-tooling--part-3--rust` | Rust |
| metric.documentation_quality | `ecosystem-tooling--part-3--go` | Go |
| metric.documentation_quality | `ecosystem-tooling--part-3--java` | Java |
| metric.documentation_quality | `ecosystem-tooling--part-3--typescript` | TypeScript |
| metric.documentation_quality | `ecosystem-tooling--part-3--kotlin` | Kotlin |
| metric.documentation_quality | `ecosystem-tooling--part-3--swift` | Swift |
| metric.documentation_quality | `ecosystem-tooling--part-3--zig` | Zig |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--quidra` | Quidra |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--python` | Python |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--c` | C++ |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--rust` | Rust |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--go` | Go |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--java` | Java |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--typescript` | TypeScript |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--kotlin` | Kotlin |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--swift` | Swift |
| metric.installation_distribution_experience | `ecosystem-tooling--part-4--zig` | Zig |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--quidra` | Quidra |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--python` | Python |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--c` | C++ |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--rust` | Rust |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--go` | Go |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--java` | Java |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--typescript` | TypeScript |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--kotlin` | Kotlin |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--swift` | Swift |
| metric.toolchain_stability_release_maturity | `ecosystem-tooling--part-4--zig` | Zig |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--quidra` | Quidra |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--python` | Python |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--c` | C++ |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--rust` | Rust |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--go` | Go |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--java` | Java |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--typescript` | TypeScript |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--kotlin` | Kotlin |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--swift` | Swift |
| metric.implemented_platform_coverage | `ecosystem-tooling--part-5--zig` | Zig |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--quidra` | Quidra |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--python` | Python |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--c` | C++ |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--rust` | Rust |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--go` | Go |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--java` | Java |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--typescript` | TypeScript |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--kotlin` | Kotlin |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--swift` | Swift |
| metric.external_integration_coverage | `ecosystem-tooling--part-5--zig` | Zig |

## Reconstruction check

These figures were recomputed with the same functions that produced the
published score. Largest disagreement with the published score: `0.00e+00`.

The workers' own reasoning for every cell is under `evidence/` beside this file.
