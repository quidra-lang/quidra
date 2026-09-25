# Ecosystem: score breakdown

Status: **COMPLETE**

## Published ranking

| Rank | Language | Score |
| ---: | --- | ---: |
| 1 | Java | 99.25 |
| 2 | TypeScript | 98.5 |
| 3 | Python | 98.25 |
| 4 | Kotlin | 97.75 |
| 5 | Rust | 97.5 |
| 5 | Go | 97.5 |
| 7 | C++ | 96.25 |
| 8 | Swift | 91.0 |
| 9 | Zig | 66.5 |
| 10 | Quidra | 32.5 |

## Categories

| Category | Weight | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| external | 0.5 | 9.0 | 100.0 | 98.0 | 99.0 | 98.0 | 100.0 | 100.0 | 98.0 | 88.0 | 56.0 |
| tooling | 0.5 | 56.0 | 96.5 | 94.5 | 96.0 | 97.0 | 98.5 | 97.0 | 97.5 | 94.0 | 77.0 |

## Per-requirement scores

Each row is one measured requirement. `w` is its frozen weight (or its
category's, for a category mean). A rank in parentheses is the language's
position on that requirement alone.

| Requirement | w | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metric.third_party_library_availability_domain_coverage<br>_external_ |  | 0 (10) | 100 (1) | 100 (1) | 95 (5) | 90 (7) | 100 (1) | 100 (1) | 95 (5) | 80 (8) | 45 (9) |
| metric.package_ecosystem_activity_maintenance<br>_external_ |  | 30 (10) | 100 (1) | 90 (7) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 85 (8) | 50 (9) |
| metric.third_party_tool_availability<br>_external_ |  | 0 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 90 (8) | 70 (9) |
| metric.production_adoption_deployment_evidence<br>_external_ |  | 0 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (8) | 55 (9) |
| metric.community_public_knowledge_availability<br>_external_ |  | 15 (10) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (7) | 90 (8) | 60 (9) |
| metric.core_tooling_availability_quality<br>_tooling_ |  | 75 (10) | 100 (1) | 95 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 90 (9) |
| metric.package_dependency_management_quality<br>_tooling_ |  | 60 (10) | 95 (6) | 80 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 90 (7) | 70 (9) |
| metric.ide_editor_support_quality<br>_tooling_ |  | 70 (9) | 100 (1) | 100 (1) | 95 (7) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (7) | 70 (9) |
| metric.debugger_profiler_support<br>_tooling_ |  | 45 (10) | 100 (1) | 100 (1) | 90 (8) | 100 (1) | 100 (1) | 100 (1) | 95 (6) | 95 (6) | 80 (9) |
| metric.build_test_integration<br>_tooling_ |  | 60 (10) | 100 (1) | 95 (7) | 95 (7) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 85 (9) |
| metric.documentation_quality<br>_tooling_ |  | 65 (10) | 100 (1) | 85 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (7) | 75 (9) |
| metric.installation_distribution_experience<br>_tooling_ |  | 60 (10) | 100 (1) | 90 (8) | 100 (1) | 100 (1) | 100 (1) | 100 (1) | 95 (6) | 95 (6) | 80 (9) |
| metric.toolchain_stability_release_maturity<br>_tooling_ |  | 50 (10) | 100 (1) | 100 (1) | 95 (7) | 100 (1) | 100 (1) | 95 (7) | 100 (1) | 100 (1) | 60 (9) |
| metric.implemented_platform_coverage<br>_tooling_ |  | 40 (10) | 70 (9) | 100 (1) | 95 (2) | 85 (4) | 85 (4) | 85 (4) | 90 (3) | 85 (4) | 85 (4) |
| metric.external_integration_coverage<br>_tooling_ |  | 35 (10) | 100 (1) | 100 (1) | 90 (5) | 85 (7) | 100 (1) | 90 (5) | 95 (4) | 85 (7) | 75 (9) |

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
