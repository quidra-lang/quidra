# Quidra Comprehensive Benchmark — Results

**Run id:** `2026-09-17-7677581`  
**Evaluated Quidra HEAD:** `7677581` (branch `develop`)  
**Date:** 2026-09-19  
**Host:** Apple M2, macOS 26.4.1 arm64, 8 GiB

> ## ⚠️ このレポートが記述するバージョン
>
> **評価対象: Quidra 0.2.0 (`7677581`, branch `develop`, 2026-09-17)**
>
> 本レポートは **0.2.0 を記述するものであり、現行版ではありません。** 提出時点でリポジトリは
> `b4cc154` (0.3.1 開発中) にあり、評価時点から **383 コミット**、実装は
> **39ファイル +18,410行 / −1,023行** 変化しています。
>
> 本文中の「Quidra は X」という記述はすべて「**Quidra 0.2.0 (`7677581`) は X**」の意味です。
> 詳細は `../version.txt` を参照してください。
>
> 測定は仕様 §1 に従い、開始時点の HEAD を固定評価対象として実施しました。開始後の
> fetch/pull/merge/rebase は行っていません。

---

Every normalized score below follows the universal direction rule: **100 = best, 0 = worst, higher is always better.** `N/A` means the metric was genuinely not applicable and is never rendered as zero.

> **No combined score is reported.** The four primary evaluations answer different questions and are deliberately never averaged, weighted or merged into one ranking (spec sections 6, 32, 35).

---

## Primary Evaluation 1 — Semantic Compression

### Semantic Compression Final Comparison

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Semantic Density | 89.68 | 100.00 | 42.03 | 46.58 | 49.22 | 46.76 | 50.84 | 63.77 | 54.58 | 30.48 |
| Semantic Determinacy | 92.84 | 55.51 | 57.35 | 78.70 | 78.24 | 68.29 | 63.74 | 65.17 | 60.00 | 100.00 |
| Semantic Locality | 100.00 | 43.64 | 32.43 | 38.10 | 80.00 | 34.29 | 52.17 | 37.50 | 38.10 | 48.00 |
| Hidden Semantic Cost | 92.11 | 29.41 | 48.61 | 59.32 | 100.00 | 43.75 | 38.04 | 41.18 | 56.45 | 85.37 |
| Capability Efficiency | 86.37 | 85.24 | 91.49 | 88.28 | 100.00 | 76.21 | 74.02 | 93.61 | 95.66 | 88.90 |
| Raw Semantic Compression Quality `Q` | 92.52 | 61.27 | 52.68 | 61.72 | 80.40 | 53.46 | 55.25 | 58.82 | 59.17 | 71.10 |
| Capability Coverage `C` | 72.73 | 72.73 | 98.86 | 98.86 | 93.18 | 90.91 | 75.00 | 90.91 | 97.73 | 94.32 |
| **Semantic Compression Overall Score** | 81.44 | 66.51 | 68.73 | 75.99 | 86.32 | 67.33 | 63.63 | 71.43 | 73.71 | 81.08 |

### Semantic Compression Ranking

| Rank | Language | Score |
|---:|---|---:|
| 1 | Go | 86.32 |
| 2 | Quidra | 81.44 |
| 3 | Zig | 81.08 |
| 4 | Rust | 75.99 |
| 5 | Swift | 73.71 |
| 6 | Kotlin | 71.43 |
| 7 | C++ | 68.73 |
| 8 | Java | 67.33 |
| 9 | Python | 66.51 |
| 10 | TypeScript | 63.63 |

---

## Primary Evaluation 2 — Standard

### Standard Final Comparison

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Native Performance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Interactive Performance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Long-running Performance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Compile / Build Performance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Startup / REPL Latency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Memory Efficiency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Source Code Size | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Binary / Artifact Size | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Deployment Footprint | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Runtime Overhead | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Code Efficiency / Conciseness | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Readability | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Functionality / Expressiveness | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Diagnostics | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Dependency Simplicity | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Portability | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Interoperability | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Concurrency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Type Safety | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Memory Safety | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Runtime Safety | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Boundary Value Safety | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Adversarial Input Robustness | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Early Error Detection | 64.44 | 49.00 | 16.71 | 62.78 | 63.09 | 56.72 | 39.00 | 58.51 | 51.86 | 49.56 |
| Debuggability | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Silent Bug Resistance | 91.67 | 63.33 | 71.43 | 83.33 | 73.53 | 68.75 | 51.43 | 67.57 | 88.57 | 85.29 |
| Implementation Robustness | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Ecosystem Breadth | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Tooling | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Library Availability | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Package / Dependency Management | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| IDE / Editor Support | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Debugger / Profiler Support | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Build / Test Integration | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Production Adoption / Deployment Evidence | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Documentation / Community | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| Toolchain Stability / Release Maturity | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| **Standard Overall Score** | 42.40 | 55.25 | 66.82 | 76.08 | 72.86 | 65.21 | 53.09 | 58.07 | 70.79 | 65.23 |

### Standard Ranking

| Rank | Language | Score |
|---:|---|---:|
| 1 | Rust | 76.08 |
| 2 | Go | 72.86 |
| 3 | Swift | 70.79 |
| 4 | C++ | 66.82 |
| 5 | Zig | 65.23 |
| 6 | Java | 65.21 |
| 7 | Kotlin | 58.07 |
| 8 | Python | 55.25 |
| 9 | TypeScript | 53.09 |
| 10 | Quidra | 42.40 |

---

## Primary Evaluation 3 — LLM Intrinsic / Unknown-Language Learnability

### LLM Intrinsic Learnability

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| I1 Keyword Anonymization | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I2 Vocabulary Anonymization | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I3 Structural Surface Perturbation | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I4 Novel-rule Generalization | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I5 Held-out Rule Composition | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I6 Prior-conflict Resistance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I1 Familiarity Drop, raw diagnostic | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| I2 Familiarity Drop, raw diagnostic | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| **LLM Intrinsic Learnability Score** | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |

### LLM Intrinsic Learnability Ranking

| Rank | Language | Score |
|---:|---|---:|

---

## Primary Evaluation 4 — LLM Standard / Knowledge-Dependent Performance

### LLM Practical Effectiveness

| Metric | Quidra | Python | C++ | Rust | Go | Java | TypeScript | Kotlin | Swift | Zig |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| LLM Generation Success | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Compile Success | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Correct@1 | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Repair Success | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Repair Efficiency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Diagnosis Efficiency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Token Efficiency | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Prompt Robustness | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Unseen-case Generalization | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Hallucination Resistance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Silent Bug Resistance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| LLM Generated Code Performance | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| **LLM Practical Effectiveness Score** | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |

### LLM Practical Effectiveness Ranking

| Rank | Language | Score |
|---:|---|---:|

---

## N/A and Not Executed

Recorded explicitly rather than estimated or fabricated (spec sections 26 and 33).

| Item | Status | Reason |
|---|---|---|
| Semantic Compression ranking | NOT ESTABLISHED | 4/4 spotchecks: metrics not cross-language comparable; annotation depth, not language semantics, sets fact_count. See CORRECTIONS D-8. |
| LLM Intrinsic Learnability (Primary 3) | WITHDRAWN | Answer-in-prompt contamination in 5/10 reference packs (my prompt's defect). See CORRECTIONS D-10. |
| LLM Practical full metric set (Primary 4) | PARTIAL | 1 trial per cell, not the 5 spec 6.2 requires for Correct@1. Correct@1=10/10 measured; small inter-language differences NOT resolved. |
| SVM/GMM/LightGrad implementations in 10 languages | Not Executed | budget; goldens exist |
| I2-I6 Intrinsic subtests | Not Executed | budget |
