# 10 — LLM Intrinsic Learnability: FROZEN transformation and evaluation design

**Implements:** spec §10 (10.1–10.8), under the scoring rules of §25.1, the N/A policy of §26,
the fixed LLM execution configuration of §6.2, the evidence requirements of §23/§24/§30,
and the fairness prohibitions of §32 and §6.1.7.

**Status: FROZEN.** This document is written before any Reference Pack is built, before any
transformer is run, and before any scored generation. Nothing in it may be changed after the
first scored generation except through the change-control procedure in §14 of this document.

**Fairness statement.** Nothing in this design is derived from Quidra's syntax, operators, types,
or feature set. Every inventory, task, rule set, budget, and validator below is defined in
language-neutral terms (semantic *roles*, not tokens) and is bound to concrete tokens only by a
mechanical per-language binding step that is applied identically to all ten languages. Capabilities
Quidra lacks are not removed. Where a language cannot realize something, the outcome is recorded
as a published residual or an N/A with a stated reason — never silently equalized in a direction
that helps any particular language. Quidra is the language under evaluation; this design is not
built to make it score well, and several of its choices (for example anonymizing **every reserved
word a language actually uses in its fixtures**, not a fixed quota and not only the frozen role
inventory — §7.1a) cut against whichever language happens to have the larger keyword surface,
without knowing in advance which that is.

**Symmetry rule (frozen).** Every rule in this document must be statable without naming any
language. A rule that is only motivated by something one particular language does or lacks is
invalid, whichever language it is. Where this document names a language it is to record a *fact*
about that language's toolchain (published as a residual), never to grant it an exemption.

**No results have been observed.** No Reference Pack has been built, no transformer has been run,
no scored generation has been produced, and no score has been computed from this document or any
sibling methodology document. Every rule, weight, budget, seed set, oracle and aggregation shape
below — including those introduced by the remediation recorded in the changelog at the end of this
document — is therefore pre-registered, not selected after seeing an outcome (§32).

---

## 0. Fixed inputs, identifiers, and directory layout

### 0.1 The ten fixed languages and their internal identifiers

Fixed column order for every table produced by this evaluation:

| # | Column | `language_id` | Source extension | Build recipe (frozen, `environment.json` → `frozen_toolchain_recipes`) | Run recipe |
|---:|---|---|---|---|---|
| 1 | Quidra | `quidra` | `.qui` | `quidra build FILE.qui -o BIN` | `./BIN` |
| 2 | Python | `python` | `.py` | (parse/compile check, §9.4) | `python3 FILE.py` |
| 3 | C++ | `cpp` | `.cpp` | `clang++ -std=c++20 -O2 FILE.cpp -o BIN` | `./BIN` |
| 4 | Rust | `rust` | `.rs` | `rustc -O FILE.rs -o BIN` | `./BIN` |
| 5 | Go | `go` | `.go` | `go build -o BIN FILE.go` | `./BIN` |
| 6 | Java | `java` | `.java` | `javac -d OUT FILE.java` | `java -cp OUT Main` |
| 7 | TypeScript | `typescript` | `.ts` | `tsc FILE.ts` | `node FILE.js` |
| 8 | Kotlin | `kotlin` | `.kt` | `kotlinc FILE.kt -include-runtime -d FILE.jar` | `java -jar FILE.jar` |
| 9 | Swift | `swift` | `.swift` | `swiftc -O FILE.swift -o BIN` | `./BIN` |
| 10 | Zig | `zig` | `.zig` | `zig build-exe -OReleaseFast FILE.zig -femit-bin=BIN` | `./BIN` |

`JAVA_HOME=/opt/homebrew/opt/openjdk` is exported for every `java` and `kotlin` invocation
(methodology 00, constraint C-3).

**Quidra execution mode for this evaluation (frozen).** The Quidra column of Primary Evaluation 3
uses **native mode only** (`quidra build` + run the produced binary). Compile/Parse Success is the
success of `quidra build`. The interpreter/REPL path is measured separately under spec §11 and is
not scored here. Reason: the other nine languages are each evaluated through exactly one frozen
build+run pipeline, and giving Quidra two attempts at the same cell would be a fairness violation
under §32.

### 0.1a Runtime-check parity of the build configuration (frozen, this track only)

**Silent Bug Resistance is a scored metric in this track (10%, §11.2).** Its definition (§11.3.9) is
"the program built and ran cleanly and produced wrong output". Whether a given defect surfaces as a
loud runtime trap or as a silent wrong answer is therefore *directly controlled by the build flags*.
Comparing a language built with its runtime safety checks ON against a language built with its
runtime safety checks OFF does not measure the languages; it measures the flag.

The `frozen_toolchain_recipes` of `environment.json` are the benchmark's **performance** recipes and
select each toolchain's fastest release configuration. Two of them disable checks the same toolchain
performs by default:

| Language | Performance recipe | Checks disabled by that recipe |
|---|---|---|
| Rust | `rustc -O` | `debug_assertions`, integer-overflow checks |
| Zig | `zig build-exe -OReleaseFast` | bounds, overflow, alignment, unreachable, optional/error-union safety |

**Frozen rule for Primary Evaluation 3:** every language is built in its **checks-enabled**
configuration — the configuration in which the toolchain performs the runtime safety checks it is
capable of performing — and no language is built with a check disabled that its own toolchain
performs by default. Concretely, the two recipes above become, for this track only:

| Language | Intrinsic-track build recipe (frozen) |
|---|---|
| Rust | `rustc -O -C debug-assertions=on -C overflow-checks=on FILE.rs -o BIN` |
| Zig | `zig build-exe -OReleaseSafe FILE.zig -femit-bin=BIN` |

All other recipes in §0.1 are unchanged: `clang++ -O2`, `swiftc -O`, `go build`, `javac`, `kotlinc`,
`tsc`, `python3` and `quidra build` each already run with whatever checks their toolchain performs,
and none of them has a checks-disabling flag applied. `swiftc -Ounchecked`, `clang++ -fno-*` safety
removals, `go build -gcflags=-B` (bounds-check elimination) and any equivalent in any other toolchain
are **prohibited** in this track.

**What this rule does NOT do.** It does not add checks a toolchain does not have. C++ performs no
bounds or overflow checking at any optimization level; that is a real, permanent property of C++ and
is **not** compensated for, engineered around, or discounted (spec §7). The per-language inventory of
runtime checks actually active under the recipe above is published in `raw/residuals.json` as
`runtime_check_inventory`, so a reader can see that a low Silent Bug Resistance for an unchecked
language is a language result and a high one for a checked language is likewise a language result —
and that neither is an artifact of the flags.

This rule would be written the same way if the checked language were Python, Go or Java and the
checks-disabled build belonged to Quidra. It is applied by the same sentence to all ten.

**Counterpart obligation.** `environment.json → frozen_toolchain_recipes` and methodology 00 must
record the intrinsic-track recipes above alongside the performance recipes, so that the two are not
confused and so that the performance tracks continue to use the release recipes unchanged.

### 0.2 Frozen seed and replicate identifiers

Every replicate unit in this evaluation has a stable identifier. These are frozen here and must be
used verbatim in every manifest, filename, and table.

| Subtest | Replicate units | `seed` values (integers) | Count |
|---|---|---|---:|
| I0 (control, diagnostic only) | `I0/base` | — (no randomization) | 1 |
| I1 | `I1/s1001` … `I1/s1005` | 1001, 1002, 1003, 1004, 1005 | 5 |
| I2 | `I2/s2001` … `I2/s2005` | 2001, 2002, 2003, 2004, 2005 | 5 |
| I3 | `I3/x3001`, `I3/x3002`, `I3/x3003` | 3001, 3002, 3003 | 3 |
| I4 | `I4/alpha-s4001`, `I4/alpha-s4002`, `I4/beta-s4001`, `I4/beta-s4002` | 4001, 4002 × 2 rule sets | 4 |
| I5 | `I5/s5001`, `I5/s5002`, `I5/s5003` | 5001, 5002, 5003 | 3 |
| I6 | `I6/m6001`, `I6/m6002`, `I6/m6003` | 6001, 6002, 6003 | 3 |

Seed counts meet or exceed every §10.3 / §10.5 minimum (≥5 for I1 and I2, ≥3 transformation sets
for I3, ≥2 published rule sets for I4, multiple deterministic mappings for I6).

### 0.3 Trial allocation and total generation budget

Per §6.2 "Allocating the five trials", **one initial trial per cell** is sufficient and is what is
executed here, because replication in this track is supplied by the seed/transformation-set count
and the subtest score is the seed mean. Repair budget is **3 turns** per §6.2.

Task sets (defined in §4):

- `CORE` = {T1, T2, T3, T4, T5} — used by I0, I1, I2, I3, I6
- `OVERLAY` = {T1, T2, T5} — used by I4
- `COMPOSITION` = {C1, C2, C3} — used by I5

Frozen generation budget:

| Subtest | Languages | Replicates | Tasks | Initial generations |
|---|---:|---:|---:|---:|
| I0 | 10 | 1 | 5 | 50 |
| I1 | 10 | 5 | 5 | 250 |
| I2 | 10 | 5 | 5 | 250 |
| I3 | 10 | 3 | 5 | 150 |
| I4 | 10 | 4 | 3 | 120 |
| I5 | 10 | 3 | 3 | 90 |
| I6 | 10 | 3 | 5 | 150 |
| **Total** | | | | **1060** |

Worst case with the full 3-repair budget: 4240 model calls. The run must record the trial count
actually executed per cell and must not present one sample as though it carried the precision of
five (§6.2). Where only one trial was run, differences of a few points between languages are not
resolved by the measurement and must not be described as if they were.

**Output caps and failure classification (frozen, inherited from §6.2).** The per-generation output
cap is **16,384 tokens** and the cumulative per-trial output cap is **65,536 tokens**. A trial that
exhausts either cap is terminated at that point, is scored **FAIL** for that task with reason
`TOKEN_BUDGET_EXHAUSTED`, and is **never** recorded as N/A; the truncated output is preserved in the
trial record and the per-language rate is published in `raw/residuals.json`. Provider, rate-limit and
transport failures are *not* model failures: they are retried under the retry policy of
`09_llm_run_config`, are recorded in `_deviations.json` separately from model failures, and are never
converted into an incorrect generation or into a FAIL. This rule is identical for all ten languages
and all seven conditions.

### 0.4 Directory layout (evidence root)

All paths below are relative to
`benchmark/2026-09-17-7677581/llm-intrinsic/`.

```
config/
  run_config.json              immutable LLM run configuration (§6.2 fields + §2.4 budgets)
  role_inventory.json          K-roles, V-roles, S-dimensions (language-neutral, §3.1-3.3)
  role_bindings.json           per-language binding tables (§3.4)
  lex_profiles.json            generic-lexer profiles, one per language (§6.1)
  tasks/                       T1..T5, C1..C3 statements, reference solutions, expected outputs
  wordlists/en_common.txt      frozen natural-language rejection list (+ SHA-256 in manifest)
  wordlists/prog_terms.txt     frozen programming-term rejection list (+ SHA-256)
  wordlists/reserved_union.txt frozen union of reserved words of all ten languages (+ SHA-256)
  wordlists/reserved_<lang>.txt per-language normative reserved-word list (+ SHA-256), §7.1a
  anonymity_labels.json        sealed language -> "Language A".."Language J" bijections (§8.1)
  diagnostic_classes.json      per-language diagnostic-class regexes (§11.4), frozen + SHA-256
  leak_terms.txt               frozen identity-leak term list (§2.6), + SHA-256
  stderr_allow.json            frozen per-toolchain non-diagnostic stderr allow-list (§4.2)
  pack_template.md             the fixed sentence skeleton for P1-P10 (§2.2)
  config_manifest.json         SHA-256 of EVERY file under config/ (§10.1 PF-14)
transforms/
  <unit>/manifest.json         transformation manifest (§6.3), one per replicate unit
  <unit>/<language_id>/...     forward+inverse artifacts, per language
reference_packs/
  <unit>/<language_id>.md      the Intrinsic Reference Pack actually shown to the model
  token_counts.json            published token/character counts for every pack (§2.5)
preflight/
  PF-01/ .. PF-14/             per-check evidence directories (§10)
  preflight_report.json        pass/fail per check; gate field `all_pass`
trials/
  <unit>/<language_id>/<task>/ full trial record per §23 (prompt, turns, sources, logs, tokens)
raw/
  metrics.csv                  per-trial metric primitives
  condition_scores.json        CTES per (language, unit)
  subtest_scores.json          mean/sd/min/max per (language, subtest)
  intrinsic_scores.json        final I1..I6 + aggregate
  familiarity_drop.json        §10.7 diagnostic (both variants, §11.7)
  residuals.json               every published residual required by this document
  defects.json                 infrastructure defects found, fixes, re-run records (§10.5)
```

---

## 1. What is transformed, and what is never transformed

The single structural idea that makes this evaluation language-neutral:

> **Transformations are defined over language-neutral ROLES, not over tokens.**
> A role is a job the grammar or standard library does (e.g. "introduces a conditional branch",
> "writes a line to standard output"). Each language has a **Role Binding Table** naming the
> concrete token(s) that play that role in that language. The transformer's domain is exactly the
> bound tokens; everything else in the source is untouched.

Consequences, frozen:

1. **Ordinary user identifiers are never renamed** merely to raise difficulty (§10.5 I1 explicitly
   forbids it). Variable names, function names chosen by the model, parameter names, and field
   names are outside the transformer's domain in I1, I2, I3 and I6. (I4 and I5 *constrain* the
   spelling of identifiers the model itself chooses; that is a rule the model must apply, not a
   transformation the harness performs, and it is applied identically in all ten languages.)
2. **String literals, character literals, numeric literals, and their escape sequences are never
   transformed**, in any condition. A transformation that reached inside a literal could change
   program output and would violate the §10.4 invariant.
3. **Comments are stripped from reference fixtures before transformation** and are never part of
   the domain. Comments in model output are passed through unchanged by the inverse transformer.
4. Only **word-shaped tokens** (matching the language's identifier pattern) are lexically
   anonymized in I1/I2. A role whose realization is punctuation (e.g. text concatenation spelled
   `+`) is NOT-LEXICALIZED for I1/I2 and is left alone; punctuation is the subject of I3 instead.
   The converse also holds and is the reason for §3.2's `V18`: a facility whose syntax lives
   *inside* a text literal (value interpolation) cannot be transformed by any condition, because
   consequence 2 forbids reaching inside a literal. That is a real, published asymmetry (§13.1),
   not a licence to drop the facility from the pack.
5. A role a language does not lexicalize at all is recorded `NOT_LEXICALIZED` in that language's
   binding table, consumes no budget, and is published. It is never used as a reason to drop the
   role from the inventory.

---

## 2. The Intrinsic Reference Pack

### 2.1 Purpose and hard constraints

The Reference Pack is the *only* description of the language the model receives. It must describe
exactly the subset of syntax and semantics the task set needs, **including every transformed token
or rule**, and nothing else. It must not name the language, must not contain any content beyond
the fixed slots below, and must not give one language extra explanation because its syntax is
harder (§10.2).

### 2.2 Fixed section template (identical for all ten languages, all conditions)

Every pack has exactly these twelve sections, in this order, with these headings verbatim.
A section that is genuinely empty for a language is present with the literal text
`(this language has no construct in this category)` — it is never deleted, and never expanded.

| § | Heading | Content slot (fixed) |
|---:|---|---|
| P1 | `Program shape` | How a complete program is laid out: compilation-unit scaffold, where the entry point goes, how statements are terminated, how blocks are delimited. |
| P2 | `Lexical rules` | Identifier form, literal forms, comment form, whitespace/indentation significance, case sensitivity, **and one fixed line present in all ten packs: `Embedding a value inside a text literal: <spelling>.` or, where the language has none, `(this language has no construct in this category)`** (§3.2 `V18`). |
| P3 | `Declarations` | How named values and functions are introduced; mutability distinction if the language has one; parameter and result declaration. |
| P4 | `Types used by the tasks` | The spellings for: integer, boolean, text, dynamic sequence, and the result-less/unit designation. Nothing else. |
| P5 | `Expressions and operators` | The arithmetic, comparison, logical, indexing, member-access and assignment operators the tasks need, **with an explicit precedence table**. |
| P6 | `Control flow` | Conditional, chained alternative, bounded iteration, conditional iteration, early exit, next iteration, result return. |
| P7 | `Aggregates` | How a record/struct/class with named fields is declared, constructed, and read. |
| P8 | `Standard vocabulary` | One entry per V-role in §3.2 that the task set needs: the exact spelling, its parameters, and its result. |
| P9 | `Imports and namespaces` | Exactly the imports/namespace declarations required to reach P8, and where they go. |
| P10 | `Output` | Exact spelling for writing one line to standard output; how a trailing newline is produced. |
| P11 | `Rules specific to this evaluation` | The transformed-token table (I1/I2), the structural table (I3), the overlay rule set (I4), the taught rules (I5), the counterfactual mapping (I6). Empty for I0. |
| P12 | `Worked examples` | Exactly six examples, slots E1–E6, §2.3. |

Sections P1–P10 use a **fixed sentence skeleton** stored in
`config/pack_template.md`: each slot is a sentence with holes, and filling a hole is the only
freedom the pack author has. No language receives an extra explanatory sentence, an extra caveat,
a rationale, or a "note that…" that another language does not receive in the same slot.

### 2.3 The examples budget, and how it is equalized

**Frozen: exactly six worked examples per pack, occupying six fixed slots that are the same for
every language and every condition.**

| Slot | What the example must demonstrate | Must NOT demonstrate |
|---|---|---|
| E1 | A complete minimal program that writes one fixed line to standard output. | Anything else. |
| E2 | Declaring a named value and a function with two parameters that returns a value; calling it. | Iteration, aggregates. |
| E3 | Bounded iteration over an integer range with a conditional and a chained alternative inside. | Aggregates, sequences of sequences. |
| E4 | Building a dynamic sequence, appending, reading its length, indexing it. | Sorting, nesting. |
| E5 | Declaring a record with named fields, constructing instances into a sequence, sorting that sequence with a supplied two-key ordering. | Nested sequences, functions returning sequences. |
| E6 | Text handling: length, character/substring access, splitting on a single-character separator, concatenation, integer-to-text conversion. | Anything else. |

Equalization is by **information slots, not by bytes**:

1. Same number of examples (6) for every language and every condition.
2. Same slot semantics for every language — the six examples teach the same six things everywhere.
3. Each example is the **minimal complete realization** of its slot in that language, using only
   constructs already named in P1–P10, and containing no construct outside its slot. "Minimal" is
   operationalized: an example is minimal if no line can be deleted and no token replaced by a
   shorter one already documented in the pack without the example ceasing to compile, ceasing to
   produce its stated output, or ceasing to demonstrate its slot. This is checked at PF-06.
4. No example carries a comment, a docstring, or explanatory prose beyond the one-line caption
   `Example E<k>: <slot sentence from the table above>` — which is the same sentence in all ten
   packs.
5. **Length differences that remain after (1)–(4) are a property of the language, not a favor.**
   They are published (§2.5), not corrected. Padding a terse language's examples or trimming a
   verbose language's examples would both be forms of tuning the benchmark to a language.

Nothing outside slots E1–E6 may appear. In particular: no example of a task solution, no example
of any held-out combination for I5, and no worked example of the I4 overlay applied to a program
larger than the slot requires.

### 2.4 Token budget

Measured in **characters** and in **frozen lexical tokens** of the exact packaged text. The lexical
counter is the one frozen in `09_llm_run_config → source_token_counting` (one implementation, applied
identically to all ten languages, SHA-256 recorded); the model's own tokenizer is **not** used,
because the sibling FROZEN document 09 records that this client does not expose it for arbitrary
strings, and a BPE count would make the budget partly a measure of how the tokenizer happens to
segment each language's identifiers. Where the harness reports its own prompt-token counts for a
pack, those are published beside the lexical counts and **never substituted for them**. The caps in
the table below are stated in lexical tokens.

| Component | Target | Soft cap | Floor |
|---|---:|---:|---:|
| P1–P11 prose and tables (no example code) | 3,000 | 3,600 | 2,400 |
| P12 example code (all six examples) | 1,200 | 1,800 | — |
| Whole pack | 4,200 | 5,400 | — |

The caps are **budgets on content slots, not gag orders**. Frozen overage rule:

> If a language's minimal, slot-faithful realization exceeds a cap, the pack keeps the minimal
> realization, the overage is recorded in `reference_packs/token_counts.json`, and **the same
> allowance is automatically available to every other language whose minimal realization needs
> it.** No language may exceed a cap by adding content; only by being unable to express the fixed
> slots more briefly. A cap is never raised for one language alone.

If any pack exceeds the whole-pack soft cap by more than 30% (i.e. > 7,020 tokens), that is a
design defect, not a language result: the slot list is re-examined at PF-06 *before any trial*,
and any change applies to all ten languages.

**The floor is a review trigger, not a padding instruction.** A pack whose P1–P11 falls below the
2,400-token floor is **never padded**. The floor fires a PF-06 review whose only question is whether
every fixed slot was actually filled for that language; if every slot is filled, the pack ships
below the floor and the shortfall is published in `token_counts.json`. Adding a sentence to reach a
floor would be exactly the "extra explanatory sentence another language does not receive" that
§2.2 forbids.

### 2.5 Mandatory publication

`reference_packs/token_counts.json` must contain, for every (replicate unit, language):

- frozen-lexical-token count of P1–P11, of P12, and of the whole pack;
- character count of the same three;
- the harness-reported prompt-token count of the whole pack, published beside them and never
  substituted for them;
- per-example token and character counts for E1–E6;
- the number of K-roles and V-roles the pack documents;
- the number of transformed tokens the pack documents (0 for I0/I4/I5);
- `max/min` ratios across the ten languages for whole-pack tokens and for P12 tokens;
- an explicit note whenever a `max/min` ratio exceeds 1.50, naming the longest and shortest packs.

### 2.6 Identity-leak scrub (applies to I1, I2, I3, I6)

Before a pack is used, it must pass the automated leak scan of PF-07. The scan rejects a pack
containing any of:

- the name of any programming language, its common abbreviations, or its file extensions;
- the name of any compiler, interpreter, runtime, build tool, or package manager;
- any URL, documentation reference, or standard-library domain name (e.g. a package path that
  identifies the ecosystem) that is not itself a transformed token. **Exemption, stated here so the
  scan is realizable:** in I1 the V-role spellings and the standard namespace/module path are
  untransformed *by design* (§7.1) and are therefore exempt from this bullet; in I3 and I6 the
  library surface is likewise real. The resulting identity exposure is not waved away — it is
  recorded as an anonymity residual (§13.2) and stated on the affected rows of the §28.1 table.
  The exemption is written once and applies to whichever of the ten languages the condition is
  applied to; it is never granted to an individual language;
- any untransformed reserved word of any of the ten languages appearing outside a code example,
  unless that word is documented in P11 as part of the counterfactual mapping (I6);
- any string from `config/leak_terms.txt` (frozen, published, includes the ten language names,
  the ten tool names, and common idiom markers).

For I1 and I2 the *code* in P12 also carries no untransformed keyword or bound vocabulary name by
construction. For I3 and I6 the surface remains recognizable to a reader who knows the language;
this is unavoidable and is stated in the results (§13.2). The requirement the spec sets is that the
model is **not told** the name; that requirement is met in all four conditions.

---

## 3. Role inventories and Role Binding Tables

### 3.1 K-roles — grammar-significant word tokens (domain of I1)

Frozen inventory. These are the word-shaped grammar roles the frozen task set needs. IDs are
stable and are the sort key used by the lexicalizer (§5.3).

| Role ID | Role (language-neutral) |
|---|---|
| `K01` | introduces a function/procedure definition |
| `K02` | introduces a named value binding (general / immutable form) |
| `K03` | introduces a named value binding, mutable form, when spelled differently from `K02` |
| `K04` | introduces a conditional branch |
| `K05` | introduces the alternative branch |
| `K06` | introduces a chained alternative branch, when spelled differently from `K05`+`K04` |
| `K07` | introduces bounded iteration |
| `K08` | introduces conditional iteration |
| `K09` | exits the innermost loop |
| `K10` | proceeds to the next iteration of the innermost loop |
| `K11` | yields a function result |
| `K12` | introduces an aggregate (record/struct/class) type declaration |
| `K13` | marks a field/member declaration, when a distinct word token is required |
| `K14` | introduces an import of an external module/namespace |
| `K15` | declares the compilation unit's module/package/namespace |
| `K16` | the boolean true literal |
| `K17` | the boolean false literal |
| `K18` | logical conjunction, when spelled as a word |
| `K19` | logical disjunction, when spelled as a word |
| `K20` | logical negation, when spelled as a word |
| `K21a` | the integer type name used by the tasks |
| `K21b` | the boolean type name |
| `K21c` | the text/string type name |
| `K21d` | the dynamic-sequence type name |
| `K21e` | the result-less / unit designation, when a word token |
| `K22` | marks the program entry point, when a distinct word token is required |
| `K23` | visibility/linkage marker required on the entry point or on declarations the tasks need |
| `K24` | mutability qualifier applied to parameters or locals, when distinct from `K03` |
| `K25` | a word token that introduces a type annotation, when the language uses one |
| `K26` | the construct that introduces a type conversion/cast, when spelled as a word |
| `K27` | error/exception declaration or propagation marker, when the frozen task set's I/O requires it |

`K27` exists because two toolchains in the frozen environment require an error-propagation marker
on the output path (methodology 00, C-4). It is bound only where genuinely required by the pack's
own P10 spelling.

### 3.2 V-roles — standard-library / builtin names (additional domain of I2)

| Role ID | Role (language-neutral) |
|---|---|
| `V01` | write one line of text to standard output (text + line terminator) |
| `V02` | write text to standard output without a line terminator, when a distinct name exists |
| `V03` | number of elements in a dynamic sequence |
| `V04` | number of characters in a text value, when a distinct name from `V03` |
| `V05` | construct the bounds of a bounded integer iteration (range/count constructor), when a named facility |
| `V06` | append one element to the end of a dynamic sequence |
| `V07` | construct an empty dynamic sequence of a stated element type |
| `V08` | order a sequence in place or produce an ordered copy, using a supplied ordering |
| `V09` | compare two text values in byte/lexicographic order (or the named comparison facility) |
| `V10` | convert an integer value to its decimal text form |
| `V11` | concatenate two text values, when a named facility rather than an operator |
| `V12` | read one character or a substring at a given position |
| `V13` | split a text value on a single-character separator |
| `V14` | the smaller / the larger of two values, when a named facility |
| `V15` | absolute value, when a named facility |
| `V16` | the standard namespace/module path that must be imported to reach V01–V15 |
| `V17` | the allocator/handle/context value the language requires to reach V01 or V07, when it requires one |
| `V18` | embed a value's text form inside a text literal (value interpolation), when the language provides one |

`V17` exists so that a language whose current standard output or dynamic-sequence API requires an
explicit context object is described in its pack on the same footing as one that does not, rather
than that requirement being smuggled in as unexplained boilerplate.

`V18` exists because several of the ten languages express integer-to-text conversion (`V10`) and text
concatenation (`V11`) *inside* a text literal rather than by calling a named facility. Frozen rules
for `V18`:

- Its status in every binding table is `BOUND` (with the spelling) or `NOT_LEXICALIZED` (with the
  reason), on the same footing as every other V-role, and its spelling is documented in the fixed
  P2 line of every pack (§2.2). It is never omitted from a pack because a language lacks it and
  never omitted because a language has it.
- Its transformation status is a third value, **`NOT_TRANSFORMABLE`**, with the frozen reason
  *"its syntax lies inside a text literal, which §1 consequence 2 never transforms in any
  condition"*. It consumes no I1/I2 budget and is excluded from `anonymized_token_count`.
- **Consequence, published, not corrected.** In I2 a language that has `V18` can route `V10` and
  `V11` through an untransformed literal and never has to learn their pseudo-worded spellings,
  while a language without `V18` must use the anonymized facilities. I2's anonymization pressure is
  therefore materially lighter for interpolating languages. §13.1 must publish, per language,
  which bound V-roles that language's `V18` spelling makes optional in I2, and the I2 narrative
  must state it beside the I2 scores. It is **not** equalized by forbidding interpolation (that
  would describe those languages unidiomatically) and **not** equalized by adding a compensating
  burden elsewhere.

### 3.3 S-dimensions — structural surface (domain of I3)

| Dim ID | Structural dimension | Kind |
|---|---|---|
| `S1` | grouping delimiters for call argument lists and expression precedence | paired |
| `S2` | block delimiters / block introducer | paired or single |
| `S3` | statement or declaration terminator | single |
| `S4` | argument and element separator | single |
| `S5` | indexing delimiters | paired |
| `S6` | primary member/field access operator | single |
| `S7` | simple assignment operator | single |
| `S8` | equality comparison operator | single |

`S6` is the **primary member access operator only**. A distinct scope-qualification operator
(where a language has one) is a different role and is not part of the S-inventory.

### 3.4 Role Binding Tables

`config/role_bindings.json` records, for each of the ten languages and each role:

```json
{
  "language_id": "<id>",
  "roles": {
    "K04": { "status": "BOUND", "tokens": ["<exact source token>"],
             "citation": "<normative document>, <section>", "notes": "" },
    "K06": { "status": "NOT_LEXICALIZED", "reason": "expressed as K05 followed by K04" },
    "V11": { "status": "NOT_LEXICALIZED", "reason": "expressed by an operator, not a name" },
    "V18": { "status": "BOUND", "tokens": ["<spelling>"], "transform": "NOT_TRANSFORMABLE" },
    "U:<token>": { "status": "BOUND", "tokens": ["<token>"], "role_prose": "<what it does>",
                   "citation": "<reserved-word list>, <section>" }
  },
  "entry_point_name": "<the name the toolchain fixes for the entry point, or null>",
  "fixed_signature_names": ["<parameter names the toolchain fixes, e.g. the entry point's>"],
  "loop_var_position": "<where the control identifier appears at a bounded-iteration site>",
  "assignment_site_forms": ["=", "+=", "-=", "++", "--", "..."],
  "declaration_predicates": { "record_type": "<lexical predicate>", "...": "..." },
  "nested_sequence_predicate": "<lexical predicate>",
  "unbound_word_tokens": []
}
```

Rules, frozen:

- `status` is exactly one of `BOUND`, `NOT_LEXICALIZED`, `NOT_WORD_TOKEN`.
- `NOT_WORD_TOKEN` means the role exists but is realized as punctuation; it is excluded from I1/I2
  lexicalization and, if it is one of the eight S-dimensions, is handled by I3 instead.
- `transform` is `NOT_TRANSFORMABLE` only for `V18`, for the frozen reason in §3.2.
- A role may bind more than one token only when the language genuinely requires distinct spellings
  in distinct positions (e.g. two forms of a declaration marker). Each bound token receives its own
  pseudo-word; the role's pseudo-words are distinct from each other.
- **Binding is mechanical and citable, not a judgement call.** A token is `BOUND` to role `R` in
  language `L` **only if** (a) it occurs in `L`'s reference solutions or pack examples, **and**
  (b) it is either listed in `L`'s normative reserved-word list (`config/wordlists/reserved_<L>.txt`)
  or named as the spelling for that role in `L`'s normative language reference. Every binding
  records the `citation` — document and section — it is taken from. Where a language offers several
  spellings for one role, the reference solution uses the one **named first in that citation**, and
  the alternatives are bound as additional tokens of the same role. Two independent analysts
  following this rule with the same citations must produce the same table; if they cannot, the
  disagreement is a defect in this rule and is handled under §14, not settled by preference.
- **Reference-solution style rule (frozen, all ten languages).** Reference solutions write an
  **explicit type annotation wherever the language permits one**, and use the language's ordinary
  declaration forms rather than inference shortcuts. Without this rule a language whose idiom is
  type inference would silently shrink its own anonymization load, which would change its I1/I2
  score by a choice of authoring style rather than by measurement.
- The binding tables are authored **from each language's own normative reference documentation and
  verified against the reference solutions**, never by inspecting any one language first and
  matching the others to it. Authoring order is the fixed column order of §0.1, which begins with
  Quidra only because that is the frozen table order; the inventory in §3.1–3.3 was fixed before any
  binding was written.
- `entry_point_name`, `fixed_signature_names`, `loop_var_position`, `assignment_site_forms`,
  `declaration_predicates` and `nested_sequence_predicate` are frozen per language before PF-05 and
  are the **only** inputs the I4/I5 overlay checkers (§7.4, §7.6) and the Specification Compliance
  checker (§11.3.7) use. No checker may consult anything not recorded here.
- Binding-table completeness is verified mechanically at PF-03: every word-shaped token appearing
  in a language's reference solutions and in its pack examples is either a bound K/V/`U:` role
  token or a user identifier declared in the same file. **`unbound_word_tokens` must be empty**
  (§7.1a); a non-empty array blocks the run. The hand-authored "with a reason" escape hatch is
  removed, because its size — and therefore how much familiar surface each language kept in I1/I2 —
  was a judgement call, and a judgement call that moves scores is not a measurement.

---

## 4. The task set and its oracles

### 4.1 Design constraints (frozen, with rationale)

- **No standard input, no command-line arguments.** Every program is self-contained and writes to
  standard output only. Rationale: stdin and argv access differ enough across the ten toolchains
  that a failure there would measure harness fit, not learnability.
- **Integer and text output only; no floating-point output.** Default floating-point formatting
  differs across the ten languages (methodology 00, C-2) and would inject infrastructure noise
  into a learnability measurement. Floating-point behavior is exercised elsewhere in this
  benchmark (micro benchmarks, SVM, GMM, LightGrad) and its exclusion here is a control, not a
  gap. Consequently there is no "float to text" V-role.
- **No integer wraparound is required anywhere.** All intermediate values fit exactly in 64-bit
  signed integers and in IEEE-754 doubles (methodology 00, C-1).
- **No associative container is required.** The frozen V-role inventory (§3.2) contains no
  associative-container role, so no pack documents one and no task needs one; every task is solvable
  with sequences. This is a **scope limitation of the task set**, fixed before any binding table was
  written and identical for all ten languages — not a capability dropped because some language lacks
  it. All ten languages provide associative containers; the limitation costs each of them the same
  thing (one unexercised facility) and is published as a stated scope limitation in §13.3.
- **No reflection, no name-based lookup, no printing of identifier names.** This is what makes the
  I4 and I5 identifier-spelling rules provably semantics-neutral (§7.5).
- **Determinism.** Every task has exactly one correct output, byte for byte.

### 4.2 The oracle (identical for every task, every language, every condition)

A submission `PASSES` a task if and only if all of the following hold:

0. the submission passes the **transformed-source conformance gate** of §4.2a for the condition;
1. the harness-prepared source (§9) builds with the frozen recipe for that language, exit status 0;
2. the built program runs to completion with **exit status 0** within a **10 second** wall-clock
   timeout and a 2 GiB address-space limit;
3. its standard output is **byte-identical** to the frozen expected output for that task,
   including the final newline;
4. its standard error is empty **or** contains only content the harness has classified as
   non-diagnostic runtime noise for that toolchain (frozen list in `config/stderr_allow.json`;
   empty for most languages, non-empty only where the toolchain unconditionally emits a banner).

Anything else is `FAIL`. There is no partial credit at the task level.

**Check units (for Test Pass Rate).** Each task's expected output is decomposed into an ordered
list of check units, frozen at PF-11:

- one unit per expected output line (the produced line at that index must equal the expected line);
- one unit `NO_EXTRA_LINES` (the produced output has exactly the expected number of lines);
- one unit `EXIT_ZERO`;
- one unit `CONFORMANT` — the §4.2a gate verdict for this turn — present in **every** condition that
  describes a rule in P11 (I1, I2, I3, I4, I5, I6). It is absent from I0, which describes no such
  rule. Its presence makes Test Pass Rate, and not only the pass/fail oracle, sensitive to whether
  the described rule was applied.

Test Pass Rate for a trial = passed units / total units. If the program does not build, every unit
fails (rate 0). If it times out, every unit fails. If the §4.2a gate fails, `CONFORMANT` fails and
the remaining units are evaluated normally on whatever the program produced, so that a
non-conformant but otherwise correct program is visibly distinguished from a non-conformant and
incorrect one in the diagnostics — but the task is still `FAIL` (§4.2a).

### 4.2a The transformed-source conformance gate (frozen)

**The defect this closes.** Without this gate, a submission that ignores the transformation entirely
and emits ordinary, familiar real source still passes in I1, I2, I3 and I4. The inverse transformer
(§6.1) "treats any `WORD` equal to a mapped pseudo-word as a role token and any other `WORD` as an
identifier", so a real conditional keyword written from memory passes through the inverse unchanged
and compiles. Identically for I3: the inverse maps the structural token back to its real spelling,
and a real delimiter the model wrote is passed through, so real-shaped source builds and satisfies
the oracle. For I4 the overlay is semantics-neutral by construction (§7.4), so ignoring it changes
nothing observable. The only penalty would be one hallucination event (8%) and a diluted item inside
Specification Compliance (10%) — leaving roughly 65% of CTES (Compile 7 + Correct@1 18 + Correct@N 8
+ Test Pass Rate 15 + Silent-bug 10 + Unseen 7) obtainable **purely from pretraining recall of the
real language**, which is precisely the advantage this track exists to remove. I6 is the only
condition structurally immune, because its transformed alphabet *is* the real alphabet and the
inverse scrambles a prior-role program into a build failure; the other conditions have no such
immunity, so it is supplied here explicitly and identically.

**Where it runs.** On the model's **raw pre-inverse submission** (after code extraction §9.2, before
inverse mapping §6.5 step 3), at **every turn** — the initial turn and each repair turn. This is the
same place §10.5 I4 already mandates overlay validation.

**The gate, per condition:**

| Condition | Gate passes iff |
|---|---|
| I1, I2 | Zero `H_REAL` events: no `WORD` token of the submission is a reserved word of the real language (per `config/wordlists/reserved_<L>.txt`), other than tokens the pack documents untransformed. |
| I3 | Zero `H_STRUCT_REAL` events: no original token of a perturbed S-dimension appears anywhere outside a string/char literal or comment. |
| I4 | Zero violations from the overlay checker (§7.4) at any applicable site. |
| I5 | Zero Rule-C violations (Rules A and B are already oracle-visible in the expected output, so they need no gate). |
| I6 | Zero prior-role occurrences under the §7.7 role-position audit. |
| I0 | No gate (nothing is described that could be ignored). |

**Frozen consequence.** *A submission that fails the gate is `FAIL` for that task at that turn
regardless of its output; the gate verdict is recorded per turn, the submission is preserved in the
trial record, and the repair prompt states which described rule was violated in language-neutral
prose without revealing the real spelling.* The hallucination metrics of §11.4 are **unchanged** and
remain separate diagnostics; the gate is not a re-weighting of them.

**Symmetry.** The gate asks only "did the submission obey the rule the pack described?". Every one of
the ten languages faces the same question in the same condition, and a language that keeps more
familiar surface in its pack (§7.1a publishes how much) faces a correspondingly larger gate surface —
which is the point, not a penalty. PF-05 must exercise the gate with a positive fixture and with a
negative fixture that is *correct real source ignoring the transformation*, in every language and
every transformed condition.

### 4.3 CORE tasks

Task statements are stored verbatim in `config/tasks/`. They are written in language-neutral prose
and contain **no** language-specific terms; the only language information the model receives is the
Reference Pack.

---

**T1 — Banded counter.**
For every integer `n` from 1 to 60 inclusive, in increasing order, determine `band`:

- if `n` is divisible by 15, `band` is `C`;
- otherwise if `n` is divisible by 3, `band` is `A`;
- otherwise if `n` is divisible by 5, `band` is `B`;
- otherwise `band` is `-`.

Write one line per `n`, of the form `<n> <band> <r>` with single spaces, where `r` is
`(n * n) mod 97`. After the 60 lines, write one final line:
`bands A=<a> B=<b> C=<c> other=<d>` where `a`, `b`, `c`, `d` are the counts of each band.
Total: 61 lines, 63 check units.
Exercises: bounded iteration, chained conditionals, remainder, multiplication, counters,
integer-to-text, output.

---

**T2 — Record sort and report.**
The statement supplies a fixed table of 12 entries, each with an integer `id`, a lowercase ASCII
`name`, and an integer `score`. Build a record per entry with three named fields. Order the records
by `score` descending; where scores are equal, order by `name` ascending in byte order. Write one
line per record in the resulting order: `<rank> <id> <name> <score>`, `rank` starting at 1.
Then one final line: `count=12 sum=<S> mean=<M>` where `S` is the sum of all scores and `M` is
`S` divided by 12 using integer division that truncates toward zero.
Total: 13 lines, 15 check units.
Exercises: aggregate declaration and construction, dynamic sequence, ordering with a supplied
two-key comparison, byte-order text comparison, integer division.

---

**T3 — Sequence statistics.**
Starting from `state = 1`, generate 200 successive states by `state = (state * 48271) mod 2147483647`.
The `i`-th value (`i` from 1 to 200) is `value_i = state_i mod 1000`, where `state_i` is the state
after the `i`-th update. Write exactly four lines:

```
min=<smallest value>
max=<largest value>
sum=<sum of all 200 values>
above=<count of i for which value_i * 200 is strictly greater than the sum>
```

Total: 4 lines, 6 check units.
Exercises: iteration with accumulator state, exact integer arithmetic, comparison, sequence build,
min/max. The `above` line asks for the count of values strictly above the arithmetic mean, phrased
as an exact integer comparison so that no floating point is needed.

---

**T4 — Word field report.**
The statement supplies one fixed line of lowercase ASCII words separated by single spaces. Split it
on the single space character into a sequence of words, indexed from 0. For each word whose length
is at least 4, in original order, write one line `<index> <word> <length>`. After those lines, write
one final line `selected=<k> initials=<s>` where `k` is the number of words written and `s` is the
concatenation, in order, of the first character of each written word.
Check units: (number of qualifying words) + 1 + 2.
Exercises: text splitting, text length, indexing, character access, concatenation, filtering.

---

**T5 — Nested band matrix (the unseen-case task).**
For `r` from 1 to 8 and `c` from 1 to 8, define `m[r][c] = (r * c) mod 11`. Build each row as a
sequence of 8 integers, and collect the 8 rows into a sequence of sequences. Order the rows by the
sum of their elements, descending; where sums are equal, order by the row's first element,
ascending. Write one line per row in the resulting order: the row's 8 values separated by single
spaces. Then one final line: `rows=8 total=<T>` where `T` is the sum of all 64 values.
Total: 9 lines, 11 check units.
Exercises, **none of which is demonstrated in any of the six pack examples**: a sequence whose
elements are sequences; a function that returns a sequence; ordering by a key computed from a
composite element. All three are derivable from the rules stated in P4, P5, P7 and P8. T5 is
therefore the Unseen-case Generalization probe (§21 of the spec) for every condition that uses
`CORE`.

### 4.4 COMPOSITION tasks (I5 only)

The three rules taught individually in the I5 pack (§7.6), and the three tasks that require their
withheld combinations:

- **C1 requires A + C.** Base program: write the first 10 triangular numbers, one per line, each
  computed by calling a user-defined function of one parameter. **The task statement says verbatim:
  "the ten triangular-number lines form one group."**
- **C2 requires B + C.** Base program: for each `n` from 1 to 6, write `n` followed by the count of
  its positive divisors, using a user-defined function of one parameter. **The task statement says
  verbatim: "the six divisor-count lines form one group."**
- **C3 requires A + B + C.** Base program: for each of 5 supplied words, write the word followed by
  a value computed by a user-defined function of two parameters, which itself calls a user-defined
  function of one parameter. **The task statement says verbatim: "the five word lines form one
  group."**

**Grouping is stated, never guessed.** Rule B refers to a "group"; a group is whatever the task
statement names as one, and every task that uses Rule B names its groups in the sentence quoted
above. A program with a single named group writes exactly one summary line, after that group's last
line. Leaving the grouping unstated would make the harness's frozen expected output depend on a
convention the model was never told — the exact withheld fact §9.1 and §10.4 requirement 2 forbid.
PF-06 verifies that **no task statement using Rule B leaves a grouping unstated**, and the frozen
wording lives in `config/tasks/`.

Expected outputs are frozen from the reference solutions at PF-11, with rules A/B/C applied. Check
units as in §4.2 (including `CONFORMANT`).

### 4.5 Reference solutions

For every task and every one of the ten languages there is a **reference solution** in
`config/tasks/<task>/reference/<language_id>.<ext>`. Reference solutions:

- use only constructs documented in P1–P10 of that language's pack;
- follow the frozen reference-solution style rule of §3.4 (explicit type annotations wherever the
  language permits them, ordinary declaration forms rather than inference shortcuts), so that no
  language's anonymization load is shrunk or inflated by authoring style;
- are the source of the frozen expected output (PF-11: all ten must produce byte-identical output);
- are the fixtures the forward/inverse transformers are round-tripped on (PF-02, PF-04);
- are **never shown to the model**.

---

## 5. Controlled random lexicalization (spec §10.3)

### 5.1 Deterministic primitives

All randomness in this evaluation comes from two frozen primitives. Both are integer-exact and
reproduce identically in any implementation.

```
FNV1a32(s):                     # s: ASCII byte string
    h = 2166136261
    for each byte b in s:
        h = h XOR b
        h = (h * 16777619) mod 4294967296
    return h

seed_state(key):                # key: ASCII byte string
    return (FNV1a32(key) mod 2147483646) + 1        # in [1, 2147483646], a valid Lehmer state

next(state):                    # Park-Miller minimal standard (methodology 00, C-1)
    return (state * 48271) mod 2147483647

pick(state, n):                 # unbiased enough, uses high bits, never the low bits
    return floor(state / 128) mod n
```

`pick` deliberately discards the low 7 bits: the low bits of a Lehmer sequence are the weakest, and
using them would visibly correlate successive letters. The divisor 128 is frozen.

### 5.2 Pseudo-word generation (algorithm PW-1)

```
CONS = ["b","d","f","g","k","l","m","n","p","r","s","t","v","z"]     # 14, fixed order
VOW  = ["a","e","i","o","u"]                                         # 5,  fixed order

draw_word(state):                                # always exactly 6 characters, C V C V C V
    w = ""
    for i in 0,1,2,3,4,5:
        state = next(state)
        if i is even: w = w + CONS[pick(state, 14)]
        else:         w = w + VOW[pick(state, 5)]
    return (w, state)
```

**Every pseudo-word is exactly six lowercase ASCII characters in the pattern CVCVCV.** This single
choice discharges three §10.3 requirements outright:

- *comparable character length*: the character-length distribution is a point mass at 6, identical
  for every language and every seed — the strongest possible match, with zero residual;
- *no transformed token is a prefix of another*: all pseudo-words within a mapping are distinct and
  of equal length, so no prefix relation can exist;
- *pronounceable, non-word, ASCII lowercase*: guaranteed by construction plus the filters below.

The space is `14*5*14*5*14*5 = 343,000` words, far larger than the number of tokens any single
mapping needs (the §7.1a domain is bounded by the size of a language's reserved-word list, at most
low hundreds), so rejection sampling terminates comfortably.

**Two-letter codes** (used by I3's structural tokens §7.3, and by the I4/I5 sigil alphabets §7.4,
§7.6) are drawn by `draw_pair` (§7.3). Frozen: **`draw_pair` is always called through the §5.3
rejection loop against the set of codes already drawn for the same key family**, so two codes drawn
for the same alphabet can never collide and make a sigil ambiguous. Redraws are recorded in the
manifest as `collision_redraws`, exactly as for pseudo-words.

### 5.3 Assignment procedure

```
assign(condition_id, language_id, seed, roles):
    ordered = sort(roles)                          # ascending ASCII order of role token key
    assigned = {}                                  # role_token_key -> pseudo-word
    used     = {}                                  # set of pseudo-words already taken
    for rk in ordered:
        key = condition_id + "|" + language_id + "|" + decimal(seed) + "|" + rk
        st  = seed_state(key)
        ok  = false
        for attempt in 1 .. 10000:
            (w, st) = draw_word(st)
            if accept(w, used):
                assigned[rk] = w; used.add(w); ok = true; break
        if not ok: ABORT("LEXICALIZATION_EXHAUSTION", key)   # infrastructure failure, §10.5
    return assigned
```

`rk` (the role token key) is `<role_id>` when the role binds one token, and `<role_id>#<n>` for the
`n`-th token when it binds several. Non-role reserved words (§7.1a) use the key `U:<token>` and are
sorted into the same ascending ASCII order as every other key, so their mappings are drawn by the
identical procedure. Each role draws from its **own** FNV-seeded stream, so the
mapping for a role is stable under inventory changes except where collision resolution intervenes;
collision resolution is deterministic given the fixed sort order and is recorded in the manifest.

`accept(w, used)` returns true iff **all** of:

1. `w` matches `^[a-z]{6}$` (guaranteed by PW-1, re-checked);
2. `w` is not in `used`;
3. `w` is not in `wordlists/en_common.txt` (frozen natural-language list, entries of length 4–6);
4. `w` is not in `wordlists/prog_terms.txt` (frozen list of common programming terms, e.g. loop,
   print, index, array, value, count, sort, split, main, func, class, param, …);
5. `w` is not in `wordlists/reserved_union.txt` (the union of the reserved/keyword lists of all ten
   languages, so a pseudo-word can never accidentally be a real keyword of *any* of them);
6. no entry of length ≥ 4 in lists (3)–(5) is a contiguous substring of `w`;
7. `w` does not appear anywhere in `config/tasks/**` — not in a task statement, not in a reference
   solution, not in an expected output, not as any user identifier or literal, in any language.

The three word lists are frozen files whose **SHA-256 is recorded in `preflight_report.json`** and
whose provenance is recorded in `config/wordlists/PROVENANCE.md`. They are not edited after PF-08
passes.

### 5.4 Tokenizer accounting and published residuals

For each replicate unit and language, `transforms/<unit>/manifest.json` records:

- every pseudo-word, its character count (always 6), and its frozen-lexical-token count (always 1,
  by the §09 counter's identifier rule);
- the mean, sd, min and max of both counts per pseudo-word, per language;
- the cross-language `max/min` ratio of both means.

Because all pseudo-words are drawn from the *same* generator with the *same* length for every
language, character length is matched **exactly** and the frozen lexical count is matched exactly.
What is **not** matched, and cannot be, is how the benchmark model's own BPE tokenizer happens to
segment a given six-letter string; that tokenizer is not exposed by this client for arbitrary
strings (09_llm_run_config → `source_token_counting`), so the residual cannot even be measured here.
**That limitation is published, never papered over** (§10.3: "if exact tokenizer matching is
impossible, publish the residual token-count differences"): `raw/residuals.json` records
`model_tokenizer_available: false` and states plainly that pseudo-word matching is exact in
characters and in frozen lexical tokens and *unknown* in model-tokenizer tokens. Character counts
are not a "substitution" for an unavailable tokenizer — they are one of the two frozen instruments,
alongside the lexical counter, and both are used for all ten languages identically.

### 5.5 Emission rule (prevents accidental token merging)

The forward transformer emits every substituted word token **surrounded by the whitespace that was
already there, and if there was none, by a single space** — subject to the following frozen
exceptions, without which the rule corrupts source in indentation-significant languages and makes
the §6.4 round-trip untestable:

> **No whitespace is inserted when the preceding or following token is `NEWLINE` or `INDENT_RUN`,
> nor anywhere inside a leading-whitespace run in a language whose lexer profile sets
> `indentation_significant`.** A bound token that begins a line at column 0 is emitted at column 0.

Without this exception a bound token at the start of a line would be preceded by an inserted space
(the preceding token being `NEWLINE`, not whitespace), which in an indentation-significant language
changes the block structure and produces an indentation error — an infrastructure defect that would
be scored as a language failure. It would do so for **any** of the ten languages whose profile sets
`indentation_significant`; the rule names none of them.

Subject to those exceptions the transformer never places a pseudo-word immediately adjacent to
another word character, so token merging (`<marker>ka` + `foo`, or `pseudo`+`identifier`) remains
impossible. Verified at PF-02.

**Every inserted whitespace character is recorded** in the unit's **position map** — the same map
§8.2 step 2 uses to translate diagnostic coordinates — and the inverse transformer **deletes exactly
those recorded insertions**. Insertion is therefore exactly reversible, which is what makes §6.4 R1
a plain byte-identity test rather than a test weakened to accommodate the transformer.

---

## 6. Reversible transformation machinery (spec §10.4)

### 6.1 The generic lexer (`lex10`)

Ten hand-written parsers would be ten places for an infrastructure defect to hide. Instead there is
**one** lexer, parameterized by a per-language profile in `config/lex_profiles.json`:

```json
{
  "language_id": "rust",
  "identifier_start": "[A-Za-z_]",
  "identifier_continue": "[A-Za-z0-9_]",
  "line_comments": ["//"],
  "block_comments": [["/*", "*/"]],
  "nested_block_comments": true,
  "string_literals": [
    {"open": "\"", "close": "\"", "escape": "\\", "multiline": false},
    {"open": "r#\"", "close": "\"#", "escape": null, "multiline": true}
  ],
  "char_literals": [{"open": "'", "close": "'", "escape": "\\"}],
  "number_pattern": "...",
  "operators": ["...", "longest-first list"],
  "indentation_significant": false,
  "structural_marker": "<the marker selected at PF-10, §7.3 — identical in all ten profiles>"
}
```

`lex10` emits a token stream of typed tokens: `WORD`, `NUMBER`, `STRING`, `CHAR`, `COMMENT`,
`OP`, `WS`, `NEWLINE`, `INDENT_RUN`. It is lossless: concatenating every token's raw text
reproduces the input byte for byte. Operators are matched longest-first (maximal munch).

**Scope honesty.** `lex10` is a lexer, not a parser. It cannot, in general, tell a keyword used as
a keyword from the same word used as an identifier. This matters in exactly two places, and both
are handled explicitly:

- The task set and reference solutions contain **no user identifier that equals any bound role
  token in any language** (verified at PF-03). So within fixtures the distinction never arises.
- In *model output*, a model may name a variable with a bound token. The inverse transformer treats
  any `WORD` equal to a mapped pseudo-word as a role token and any other `WORD` as an identifier.
  A model that names a variable with a pseudo-word therefore produces a program whose inverse image
  uses a keyword as an identifier — which will fail to build, and is recorded as trial outcome
  `INVERSE_AMBIGUOUS`. This is counted as a **model failure of type H-INVENT** (§11.4), not as an
  infrastructure defect, and the rate is published per language. PF-05 requires at least one
  negative fixture exercising this path.

### 6.2 Forward and inverse transformers

```
forward(source, unit, language_id) -> transformed_source
inverse(transformed_source, unit, language_id) -> real_source
```

Forward, in order:

1. lex the source with the language's real profile;
2. drop `COMMENT` tokens (fixtures only; model output has no forward pass);
3. for I1/I2: replace each `WORD` token whose text is a bound role token with its pseudo-word,
   applying the emission rule of §5.5;
4. for I3: replace **every** occurrence of each `OP` (or block-introducer `WORD`) token belonging to
   a selected S-dimension with that dimension's `<marker>xy` token, whatever syntactic role the
   occurrence plays (§7.3), surrounded by single spaces under the §5.5 emission rule and its
   `NEWLINE`/`INDENT_RUN` exceptions; preserve `INDENT_RUN` tokens verbatim;
5. for I6: replace each `WORD` token in the permuted role subset with the **real token of the role
   π maps it to** (§7.7), and the one permuted operator pair likewise;
6. re-emit, recording every inserted whitespace character in the unit position map (§5.5).

Inverse is the same machinery run with the **transformed profile** (operator table extended to
recognize `<marker>xy` structural tokens, comment and string rules unchanged) and the inverse
mapping, followed by deletion of exactly the whitespace insertions the position map records (§5.5).
String, char, number and comment tokens are never touched in either direction.

### 6.3 Transformation manifest

`transforms/<unit>/manifest.json`, one per replicate unit, contains:

```json
{
  "unit": "I1/s1001",
  "condition": "I1",
  "seed": 1001,
  "generated_at": "<ISO-8601>",
  "generator_version": "<sha256 of the transformer source>",
  "primitives": {"rng": "park-miller-48271", "hash": "fnv1a32", "pick_divisor": 128},
  "wordlist_sha256": {"en_common": "...", "prog_terms": "...", "reserved_union": "..."},
  "languages": {
    "<language_id>": {
      "mapping": {"K04": "bekado", "K07": "nusivi", "...": "..."},
      "not_lexicalized": ["K06", "K15"],
      "collision_redraws": {"K11": 2},
      "pseudo_word_token_counts": {"bekado": 2, "nusivi": 3},
      "anonymized_token_count": 27,
      "roundtrip": {"fixtures": 5, "identical": 5, "built": 5, "output_match": 5}
    }
  }
}
```

The manifest is written **before** any generation for that unit and is not modified afterwards;
corrections go through §10.5.

### 6.4 The required invariant and its test

> **real source → transformed source → inverse mapping → semantically identical real source**

Tested at PF-02 and PF-04 on every reference solution and every pack example in every language:

- **R1 byte identity.** `inverse(forward(F))` is **byte-identical** to `F_nocomments`. Because the
  forward transformer records every whitespace character it inserts in the unit position map and the
  inverse deletes exactly those insertions (§5.5), no canonicalization step is needed and none is
  permitted. The earlier `canon`-relaxed form of R1 was unsound: `canon` could only *collapse* runs
  of spaces, never delete an inserted one, so any fixture containing a bound token immediately
  followed by a delimiter (a conditional or loop keyword written against an opening parenthesis)
  would fail R1 and block the run — while a fixture author could dodge the failure by never writing
  that form, which would hide a real transformer defect behind a style choice.
- **R2 build identity.** `inverse(forward(F))` builds with the frozen recipe, exit status 0.
- **R3 behavioral identity.** The program built from `inverse(forward(F))` produces standard output
  byte-identical to the program built from `F`, and the same exit status.
- **R4 non-triviality.** `forward(F)` differs from `F` in at least one token for every language and
  every condition that is supposed to transform something. A forward transformer that is silently a
  no-op for one language would make that language's row meaningless.
- **R5 domain containment.** The multiset of tokens changed by `forward` is a subset of the bound
  role tokens for that language and condition. No user identifier, literal, or unrelated token is
  in the diff.

R1–R5 must all pass for all ten languages before any Reference Pack for that unit is built.

### 6.5 What the real toolchain sees

**Only inverse-mapped source is ever compiled or executed.** Transformed source is never given to
a compiler. Concretely, for every scored trial:

1. the model's raw output is captured verbatim;
2. the code block is extracted by the frozen extraction rule (§9.2);
3. `inverse(...)` is applied (identity for I0, I4, I5, which are untransformed);
4. harness conventions are applied (§9.3);
5. the result is built and run by the real toolchain with the frozen recipe;
6. the oracle of §4.2 is applied.

A mapping defect at step 3 is an **infrastructure failure, not a language failure** (§10.4), and is
handled under §10.5.

---

## 7. The six subtests

### 7.0 I0 — Untransformed control (diagnostic only, not part of the Intrinsic score)

Spec §10.7 requires reporting `Practical baseline score − transformed score` for I1 and I2. To make
that subtraction meaningful the two terms must be measured on the same tasks. **I0 is frozen as the
primary baseline:** the same `CORE` tasks, the same pack template, **no transformation**, real
token spellings, and the real language name stated. It is scored with the same 12-metric
Condition Task Effectiveness Score.

I0 is **not** one of I1–I6 and carries **zero weight** in the Intrinsic score. It appears only in
the §10.7 diagnostic and in `raw/familiarity_drop.json`. §11.7 also requires publishing the
alternative reading of §10.7 that uses the §9 Practical Effectiveness Score as the baseline, so
both readings of the spec sentence are available to a reader.

### 7.1 I1 — Keyword Anonymization (weight 20%)

**Transformation.** For each language, every `BOUND` K-role token (§3.1) **and every non-role
reserved word in the I1/I2 domain defined in §7.1a** is replaced by a PW-1 pseudo-word under the
seed's mapping. V-roles are **not** transformed: standard-library and builtin names keep their real
spellings in I1. Ordinary user identifiers are not renamed (§10.5 I1). `V18` is never transformed in
any condition (§3.2).

**Anonymity.** The language name is never given; the pack is presented as "Language A"…"Language J"
per §8.1.

**Pack.** P11 contains the full transformed-token table: one row per anonymized token, giving the
pseudo-word, its grammar role in language-neutral prose, and, where the role is positional (e.g. a
declaration marker that must precede the name), its required position. The table is the *only*
place the pseudo-words are defined; the examples then use them.

**Replicates.** 5 seeds (1001–1005), score = seed mean.

**Budget is deliberately NOT quota-matched, and here is why.** §10.5 requires matching the
transformation budget for **I3 only**. For I1 the instruction is to anonymize "language keywords and
grammar-significant word tokens" — i.e. all of them. A language whose task-relevant grammar is
spelled with 34 word tokens genuinely presents more vocabulary to learn from specification than one
that uses 12; capping both at 12 would leave the verbose language's remaining keywords in their
familiar form and hand it exactly the pretraining advantage this subtest exists to remove.
The per-language `anonymized_token_count` is therefore **published** in the manifest and in
`raw/residuals.json`, and the results narrative must state it beside the I1 scores. It is reported,
not neutralized.

### 7.1a The I1/I2 lexical domain is the whole reserved surface, not the role inventory

**The defect this closes.** Restricting the domain to the 27 frozen K-roles would leave every *other*
real keyword a language uses in its fixtures untouched and usable from memory — alternative
selection and loop forms, cast and storage-class markers, allocation keywords, pattern-matching
introducers, and so on. Those survivors were previously routed into a hand-authored per-language
`unbound_word_tokens` array "with a reason": an unbounded, judgement-based escape hatch whose size
determined how much familiar surface each language kept, and therefore how hard I1 and I2 were for
it. Its size differs arbitrarily between languages in **both** directions, and an arbitrary quantity
that moves scores is not a measurement.

**Frozen rule.** *Every token that appears in the language's frozen reserved-word list
(`config/wordlists/reserved_<L>.txt`) **and** occurs in that language's reference solutions or pack
examples is anonymized in I1 and I2, whether or not it maps to a K-role.* Non-role reserved words
receive the role-token key `U:<token>` (§5.3), are drawn by the identical PW-1 procedure, and are
documented in P11 with language-neutral prose describing the job they do — on exactly the same
footing as a K-role token, with no extra explanatory sentence (§2.2).

Consequences, frozen:

- `unbound_word_tokens` **must be empty** for every language; a non-empty array blocks the run at
  PF-03.
- `anonymized_token_count` becomes a **mechanically derived** quantity — the size of the intersection
  of a frozen word list with a frozen fixture set — rather than an authored one. Two independent
  analysts must compute the same number.
- The preamble's claim that this design anonymizes *all* the word keywords a language uses, rather
  than a quota, is now true as written.
- The direction of this change is not knowable in advance and is not supposed to be: it increases the
  I1/I2 load of whichever languages carry the larger reserved surface in their fixtures, and leaves
  the smallest-surface language roughly where it was. The realized per-language counts are published
  before the scores are discussed.

### 7.2 I2 — Vocabulary Anonymization (weight 20%)

**Transformation.** I1's mapping **plus** every `BOUND` V-role token (§3.2): output, length, range
construction, sequence construction and append, ordering, text comparison, integer-to-text,
substring/character access, splitting, min/max, absolute value, and the standard namespace/module
path(s) needed to reach them. Namespace paths are transformed **component by component** — each
path component that is a word token gets its own pseudo-word — so that a path like
`<ns>.<sub>.<name>` becomes `<pw1>.<pw2>.<pw3>` and the structure of qualification is still
visible, which is what P9 describes.

**Replicates.** 5 seeds (2001–2005), score = seed mean. Anonymity as in I1.

**Pack.** P8 and P9 are written entirely in pseudo-words, with each entry's parameters and result
stated in language-neutral prose. P11's table covers both the K and V mappings, in one table sorted
by role ID.

### 7.3 I3 — Structural Surface Perturbation (weight 15%)

**What is transformed.** Structure only. Keywords keep their real spellings; the surface *shape*
changes. This isolates the structural effect from the lexical effect measured by I1; layering I1
under I3 would make neither separable.

**The transformation budget, as a countable quantity.** The budget is measured in
**Structural Perturbation Points (SPP)**, where perturbing one S-dimension costs exactly 1 SPP.

> **Frozen budget: exactly 5 SPP per language per transformation set, drawn from the subset of the
> eight S-dimensions that ALL TEN languages lexicalize.**

How equality is guaranteed:

1. At PF-09, each language's binding table is checked for each S-dimension. The **eligible set** is
   the intersection across all ten languages — a dimension is eligible only if every one of the ten
   lexicalizes it. `S3` (statement terminator) and any other dimension some language does not
   lexicalize are excluded from *every* language, so no language is perturbed on an axis another
   language escapes.
2. Each transformation set selects exactly 5 eligible dimensions. The **same five dimensions** are
   perturbed in all ten languages within a set.
3. Therefore every language receives exactly 5 SPP in every set — matched by construction, with no
   per-language adjustment and no judgement call.
4. If the eligible set turns out to contain fewer than 5 dimensions, the budget for **all ten**
   languages drops to the size of the eligible set, and the reduction is published. The budget is
   never raised for some languages to compensate.

Frozen transformation sets (dimension selections fixed here, before PF-09 runs; if PF-09 finds a
dimension ineligible, the set falls back to the next dimension in the frozen priority order
`S1, S2, S4, S5, S6, S7, S8, S3`, applied identically to all ten languages, and the substitution is
published):

| Set | Seed | Dimensions perturbed |
|---|---:|---|
| `I3/x3001` | 3001 | S1, S2, S4, S6, S7 |
| `I3/x3002` | 3002 | S1, S2, S5, S7, S8 |
| `I3/x3003` | 3003 | S2, S4, S5, S6, S8 |

**SPP matches dimensions, not occurrences.** Perturbing one S-dimension costs 1 SPP whatever that
dimension's *density* is in a given language: a block-delimiter dimension rewrites two token types in
a paired-delimiter language and one in a single-introducer language, and an indexing dimension
touches array *type* syntax as well as indexing in the languages that spell types that way. The
realized number of perturbed characters can therefore differ by an order of magnitude between the
tersest and the most delimiter-heavy of the ten. That is a real property of the languages and is
**published, not equalized**: `transforms/I3/<set>/manifest.json` records, per language, the count of
perturbed sites and the **sites per 100 frozen-lexical-tokens** in that language's reference
solutions, and the results narrative must state that the I3 budget is matched **by axis and not by
density**. Equalizing density would require perturbing different dimensions in different languages,
which is the per-language adjustment §7.3 clause 3 exists to forbid.

**A perturbed dimension replaces EVERY occurrence of its token**, whatever syntactic role that
occurrence plays — grouping delimiters in declarations, control-flow headers, casts and expression
grouping alike; indexing delimiters in type syntax as well as in element access. P11 states this in
the **same sentence for all ten languages**. A lexer cannot distinguish those roles (§6.1), and a
rule that pretended it could would be unimplementable and would silently differ between languages.
Consequently the description of `S1` in §3.3 is to be read as "the grouping delimiter token, in every
position it occurs", not as "only call argument lists".

**Compound assignment operators (`+=`, `-=`, `*=`, and increment/decrement forms) are distinct
operators. They are NOT part of `S7` and are never perturbed by any transformation set.** `S7` is the
simple-assignment operator alone. This is stated because the alternative reading would change the
perturbed-site count in the languages that have compound forms and not in those that do not.

**Replacement tokens.** Every structural replacement token has the form `<MARKER>` + consonant +
vowel (e.g. `%%ka` if the selected marker is `%%`), generated by:

```
draw_pair(state):                      # returns the bare two-letter code
    state = next(state); c = CONS[pick(state, 14)]
    state = next(state); v = VOW [pick(state, 5)]
    return (c + v, state)

structural_token(state):               # I3 only; MARKER is the PF-10 selection
    (code, state) = draw_pair(state)
    return (MARKER + code, state)
```

keyed by `seed_state("I3|" + set_id + "|" + dimension_id + "|" + side)` where `side` is `open`,
`close`, or `single`, and drawn through the §5.2 rejection loop so two dimensions in a set can never
receive the same code. **The key contains no `language_id`**: the same dimension is spelled the same
way in all ten languages within a set, which is the strongest available form of matched treatment.

**The structural marker is selected, not assumed (frozen procedure).** The marker is the **first
entry of the frozen priority list `%%`, `~~`, `¤`** for which PF-10 finds **zero** occurrences in
every task statement, reference solution, expected output and pack example in all ten languages, and
which is **not a lexical prefix of any token in any of the ten grammars** (i.e. no token of any of
the ten languages begins with it). The selected marker is published in `preflight_report.json`,
written into all ten lexer profiles (`structural_marker`), and used identically in all ten. If no
entry of the list qualifies, that is a design defect handled under §14, not a per-language
workaround.

**`@` is excluded a priori** and is not on the list, for a reason of fact rather than preference:
`@` begins ordinary builtins in Zig (`@import`, `@intCast`, `@as`) — and the frozen environment
reaches stdout through exactly such a builtin (methodology 00, C-4) — and it begins annotation,
attribute and decorator syntax in Java, Kotlin, Swift and TypeScript. A `@`-based marker would
therefore (a) make PF-10 unsatisfiable by construction and block the whole run, and (b) under the
three-character maximal-munch rule, lex `@import` as `@im` + `port`, silently corrupting source in
the one language whose fixtures cannot avoid it. That is an infrastructure defect that would arrive
looking exactly like a language failure (§10).

Maximal munch, restated against the selected marker: every structural token is the marker followed by
**exactly two lowercase letters**, terminating after the second letter, so `<MARKER>kafoo` lexes as
`<MARKER>ka` then `foo`, no structural token is a prefix of another, and the transformed lexer
profile declares this rule verbatim.

**Indentation.** For a language whose block structure is indentation-significant, perturbing `S2`
replaces the block-introducer token (if it has one) and **leaves indentation untouched**. Indentation
is not itself an S-dimension: no language could express the change reversibly without ambiguity. A
language that has *no* block-introducer token at all does not lexicalize `S2`, which under clause 1
makes `S2` ineligible for **all ten** languages — the frozen fallback order then applies identically
everywhere and the substitution is published. It is expected that one or more of the three frozen
sets will need that fallback; that is the mechanism working, not a defect.

**Pack.** P11 contains the structural table: for each perturbed dimension, the original role in
language-neutral prose ("the delimiter that groups a call's argument list"), the new spelling, and
an explicit statement that **precedence and associativity are unchanged** — precedence is a property
of the role, not of the spelling.

**Replicates.** 3 transformation sets, score = set mean.

**Honest caveat.** The keywords remain real, so a reader of the transformed source can still
recognize the language. The model is never told the name (§10.1's requirement), but I3 does not
remove recognizability, and the results narrative must say so (§13.2).

### 7.4 I4 — Novel-rule Generalization (weight 20%)

I4 keeps the **real surface** (spec §10.1: anonymizing underneath the overlay would mix I1's effect
into I4's and leave neither measurable). The model is still never told the language name.

**What makes the rule genuinely novel.** Each rule set constrains the spelling of identifiers the
model itself chooses, as a function of a statically observable property of the program. No language
in the comparison set marks these properties in its identifiers, so the rule cannot have been
learned as a property of any of them; and the *sigil alphabet is drawn fresh per seed by PW-1*, so
it cannot have been memorized from anywhere.

**Why it is provably semantics-neutral.** The overlay constrains only identifier *spelling*. The
frozen task set never prints an identifier name, never uses reflection, and never performs
name-based lookup (§4.1). Therefore renaming every identifier back to any other valid spelling
leaves observable behavior unchanged, and "removal" of the overlay is a no-op: the inverse-mapped
source *is* the model's source, and it is compiled and run by the real toolchain as-is.

**Validation before inverse mapping.** A static checker runs on the model's source, using `lex10`
plus the language's role bindings, and produces a per-site verdict. It must be able to identify:
(a) function declaration sites and their parameter counts; (b) named-value declaration sites;
(c) assignment sites (as defined below); (d) call sites (a `WORD` immediately followed by the
grouping-open token). All four are obtainable lexically given the binding table.

**The checker's verdict has two consumers, and I4 is sensitive to both.** Routing it into a single
checklist item inside Specification Compliance would have made a 20%-weight subtest named
"Novel-rule Generalization" roughly 98% insensitive to whether the novel rule was acquired: one item
among ~5 per task × 3 tasks, inside a 10% metric, is ≈2 CTES points, so a model that ignored the
overlay entirely would keep ≈98 and I4 would report ordinary correctness under a different name.
Frozen instead:

1. **A gate.** Zero overlay violations is a **precondition of the I4 oracle** (§4.2a). A submission
   that ignores the overlay is `FAIL` for that task at that turn.
2. **A rate, not an item.** *Overlay Compliance = compliant sites / applicable sites*, where an
   applicable site is each function definition (A1, B1), each named-value introduction (A2) and each
   loop control variable (B2). **For I4 the task checklist of §11.3.7 is 50% ordinary items and 50%
   Overlay Compliance.** The rate is reported per language and per rule set.

The checker is exercised both ways at PF-05, in every one of the ten languages.

**Entry-point and fixed-signature exemption (frozen; identical wording in P11 of all ten packs).**

> *The compilation unit's required entry point, and any parameter whose name or signature the
> toolchain fixes, are exempt from A1, A2, B1, B2 and Rule C. Every other function and named value
> the program defines is subject to them.*

Without this exemption the rules would demand that a language whose toolchain fixes the entry-point
name rename it, making the program unbuildable or unrunnable — while the harness fixups (§9.3) only
adjust file names and wrap top-level code and cannot rescue it. Languages that need no named entry
point would be unaffected, so the rule would be unsatisfiable for some of the ten and free for
others. The exemption is applied **mechanically**, from `entry_point_name` and
`fixed_signature_names` in each language's binding table (§3.4), never by inspection, and a language
whose `entry_point_name` is `null` simply has nothing to exempt. Since §9.1 makes unstated
conventions the harness's responsibility, the exemption is *stated in the pack* rather than left for
the model to guess.

#### Rule set N-ALPHA (published in full)

Let `A(seed)` be five distinct two-letter codes drawn by `draw_pair` (which returns a bare code,
with no structural marker — the marker belongs to I3 alone), keyed
by `seed_state("I4|ALPHA|" + decimal(seed) + "|arity" + k)` for `k = 0..4`, and let `P(seed)` be one
two-letter code keyed by `seed_state("I4|ALPHA|" + decimal(seed) + "|reassign")`.

- **A1 (arity sigil).** Every function the program defines must have a name ending in `_` followed
  by the code for its parameter count: `A(seed)[0]` for 0 parameters, `[1]` for 1, `[2]` for 2,
  `[3]` for 3, `[4]` for 4 or more.
- **A2 (reassignment prefix).** Every named value the program introduces (local, parameter, or
  field) that is assigned a new value at least once after its introduction must have a name
  beginning with `P(seed)` followed by `_`. A named value never reassigned must not carry that
  prefix.

Both are decided statically, in every language, from the declaration site and the set of assignment
sites. A language that already distinguishes mutable from immutable bindings gives the model a cue
for A2; that asymmetry is real, is a genuine language property, and is **published as a residual**
(§13.3) rather than engineered away — the spec itself names mutability as an acceptable basis
(§10.5 I4).

**An assignment site (frozen, mechanical).** *An assignment site is an occurrence of the `S7` simple
assignment operator, of any compound-assignment or increment/decrement operator listed in that
language's `assignment_site_forms` (§3.4), or the implicit rebinding of a bounded-iteration control
variable — which counts as reassignment in all ten languages.* Without this definition A2's verdict
for the most common variable in every task would depend on each language's loop idiom, which is
exactly the "equivalent reasoning difficulty" §10.5 I4 forbids breaking.

#### Rule set N-BETA (published in full)

Let `F(seed)` be the two-letter code keyed by `seed_state("I4|BETA|" + decimal(seed) + "|fanout")`,
and `Lf(seed)`, `Lv(seed)` the codes keyed by `...|loopfix` and `...|loopvar`.

- **B1 (fan-out sigil).** Every function the program defines must have a name ending in `_`
  followed by `F(seed)` followed by the decimal count of **distinct functions defined in this
  program that its body calls** (0, 1, 2, …). Calls to standard-library facilities do not count.
- **B2 (loop-variable kind).** Every loop control variable whose iteration count is a compile-time
  constant of the program must have a name ending in `_` + `Lf(seed)`; every loop control variable
  whose iteration count depends on a value computed at run time must end in `_` + `Lv(seed)`.

**B2's two terms are frozen mechanically, so the checker needs no dataflow and no per-language
judgement:**

- *A **loop control variable** is the identifier introduced at a bounded-iteration (`K07`) site in
  the position that language's binding table records as `loop_var_position` (§3.4). A
  conditional-iteration (`K08`) loop has no loop control variable and is **out of scope for B2**.*
  This makes B2 well-posed for a language whose idiomatic bounded loop iterates over a sequence and
  for one whose idiomatic bounded loop counts an integer, without either idiom being privileged.
- *Its iteration count is **compile-time constant** iff every operand of its bound expression is an
  integer literal, or an identifier whose only assignments anywhere in the program are from
  literal-only expressions.* This is decidable from the lexical site kinds of §7.4 plus the
  assignment-site definition above; it requires no type or dataflow analysis.

**Replicates.** 2 rule sets × 2 seeds (4001, 4002) = 4 replicate units; score = mean over the four.
Both rule sets and all four realized sigil alphabets are published in
`transforms/I4/*/manifest.json` and reproduced in the results appendix (§10.5 I4: "Publish every
rule set").

**Which seed rule governs I4 and I6 (stated explicitly, because it was previously only implied).**
§10.3's "≥5 independent seeds" binds the **lexical-randomization conditions**, which are the
pseudo-word conditions **I1 and I2** — and both run 5 seeds. I4 and I6 are governed by the
subtest-specific sentences of §10.5, which require *multiple published rule sets* (I4: 2 rule sets ×
2 seeds = 4 units) and *multiple deterministic mappings* (I6: 3 mappings) rather than five seeds.
Their sigil alphabets and permutations are drawn with the same frozen primitives, but the randomized
quantity is the rule set / mapping, not a lexicalization of the language's vocabulary. Replicate
counts are identical for all ten languages in every subtest, so this reading favours no language;
the per-subtest sd, min and max (§11.6) carry the resulting precision.

**Code collisions.** The five arity codes of A1, the fan-out code of B1 and the loop codes of B2 are
drawn through the §5.2 rejection loop against the codes already drawn for the same seed's alphabet,
so two arities can never receive the same code and no sigil is ambiguous. Redraws are recorded in
the manifest as `collision_redraws`.

**Pack.** P11 states the rule set in language-neutral prose, plus a table of the realized codes.
The six examples E1–E6 are shown **with the overlay already applied**, so the model sees the rule
obeyed, but no example is a task solution and no example demonstrates a combination the tasks
need beyond what its slot requires.

**Task set.** `OVERLAY` = {T1, T2, T5}. T5 remains the Unseen-case probe.

### 7.5 Why identifier-spelling rules are the right shape for I4

The alternative — an overlay that changes program *semantics* — cannot satisfy §10.5's requirement
that the overlay "leave the underlying program semantics unchanged after validation/removal". An
overlay that constrains identifier spelling satisfies every clause: it is described only in the
pack; it is mechanically validated before inverse mapping; it is semantics-neutral by the argument
in §7.4; and its reasoning load (track a static property of each declaration, then spell
accordingly) is the same operation in all ten languages.

### 7.6 I5 — Held-out Rule Composition (weight 15%)

I5 keeps the **real surface** (§10.1), and the model is never told the language name.

**The three rules, taught individually in P11:**

- **Rule A — line numbering.** Every line the program writes to standard output, without exception,
  is prefixed by `<n>| ` where `n` counts written lines starting at 1 and increasing by 1 for each
  line written, including any line produced by Rule B.
- **Rule B — group summary.** After the last line of a group of related lines, the program writes
  one additional line `total=<k>` where `k` is the number of lines in that group, not counting the
  summary line itself. **A group is the maximal run of lines the task statement names as one group;
  every task that uses Rule B names its groups verbatim (§4.4). A program with a single named group
  writes exactly one summary line, after that group's last line.** Rule B never asks the model to
  infer a grouping the statement did not state: the harness's expected output is frozen from the
  reference solutions, so an unstated grouping would be a fact the harness knows and the model must
  guess, which §9.1 and §10.4 requirement 2 forbid.
- **Rule C — arity sigil.** Every function the program defines has a name ending in `_` followed by
  the seed's code for its parameter count, using the same alphabet construction as N-ALPHA/A1 but
  keyed by `seed_state("I5|" + decimal(seed) + "|arity" + k)`, and drawn through the §5.2 rejection
  loop so two arities cannot collide. **The entry-point and fixed-signature exemption of §7.4
  applies to Rule C in the same wording, in P11 of all ten packs.**

**What is withheld, fixed here before any result is observed:**

| Combination | Shown in an example? | Tested by |
|---|---|---|
| A alone | yes — example XA, a program with no defined functions and no summary | — |
| B alone | yes — example XB, a program with no defined functions and no numbering | — |
| C alone | yes — example XC, a program with defined functions, one written line, no numbering, no summary | — |
| **A + C** | **no** | **C1** |
| **B + C** | **no** | **C2** |
| **A + B + C** | **no** | **C3** |

Examples XA, XB, XC **replace** slots E1, E2, E3 for the I5 pack; slots E4, E5, E6 are unchanged
(and, being examples of sequence, aggregate and text handling, they contain neither numbering nor
summaries nor defined functions, so they leak no combination). The six-example budget is therefore
still exactly six, as for every other condition.

The interaction between A and B is **derivable and unambiguous**: A says *every* written line is
numbered, without exception; B says the summary is a written line. Hence the summary is numbered
and its number continues the sequence. The pack states A's "without exception" explicitly so that
the composition is a matter of applying the stated rules, not of guessing an unstated convention.

**Replicates.** 3 seeds (5001–5003). The seed selects (i) the arity alphabet for Rule C and
(ii) which of three frozen variants of each of XA, XB, XC is shown, from
`config/tasks/I5/examples/`. Score = seed mean.

**Task set.** `COMPOSITION` = {C1, C2, C3}. **Unseen-case Generalization for I5 is the pass rate on
C3**, the triple composition — the case furthest from anything demonstrated.

### 7.7 I6 — Prior-conflict Resistance (weight 10%)

**Construction.** A **counterfactual permutation of the language's own vocabulary**: for a frozen
subset of roles, the transformed language spells role `R` using the *real token of a different
role*. The pack states the new mapping unambiguously.

The permuted role subset (frozen, same roles for every language; a role `NOT_LEXICALIZED` in a
language is dropped from that language's permutation and the drop is published):

`K02, K04, K05, K07, K08, K09, K10, K11, K12, K14, K16, K17`

plus **exactly one operator pair**: *the non-strict **less-than-or-equal** and
**greater-than-or-equal** operators are exchanged with each other. The strict less-than /
greater-than pair is deliberately **NOT** permuted, because in five of the ten languages those two
characters also delimit generic type arguments, and `lex10` is a lexer, not a parser (§6.1), so the
forward transformer cannot restrict the exchange to comparison sites.* Swapping the strict pair would
rewrite every generic type in those five languages into an unparseable mirror image that the model
would then have to reproduce to be inverse-mapped correctly, while the five languages that spell the
same task-set types without angle brackets would absorb none of that burden — a large,
language-dependent asymmetry created by the transformer rather than measured in the languages, and a
§10.4 hazard besides, since the resulting grammar is not describable within the pack's budget.

The chosen pair has the properties the exchange needs: the two operators share a precedence class and
an arity, neither is ever a delimiter in any of the ten languages, and maximal munch already keeps
the three-character comparison operator and the shift operators intact. No other operator is
permuted: exchanging operators of different precedence would make the transformed grammar ambiguous
to describe in the pack's budget and would risk violating §10.4.

**PF-09 verifies the choice** rather than assuming it: the permuted pair must be recorded as a
comparison operator in **all ten** binding tables and must appear in **no** delimiter position in any
of the ten grammars or fixtures. If a candidate pair fails that check it is not used, and the failure
is published.

**Permutation generator (Sattolo's algorithm — guarantees a single cycle, hence no fixed point):**

```
sattolo(items_sorted, state):
    a = copy(items_sorted)
    for i from len(a)-1 down to 1:
        state = next(state)
        j = pick(state, i)            # 0 <= j < i, strictly less than i
        swap(a[i], a[j])
    return (a, state)
```

with `state = seed_state("I6|" + language_id + "|" + decimal(seed))` and `items_sorted` the bound
subset in ascending role-ID order. The mapping is then
`spelling(role items_sorted[k]) = real_token(a[k])`. Because Sattolo produces a derangement,
`a[k] != items_sorted[k]` for every `k`: **no role keeps its own token**, so there is no role for
which prior expectation and the stated rule happen to agree.

**Semantics are not altered.** The transformed program is a re-spelling; `inverse` restores the
original real source, which is what the real toolchain compiles and runs. A correct transformed
program and its real counterpart produce identical output. This satisfies §10.5's prohibition on
"semantic traps that alter the actual program result".

**Pack.** P11 contains the counterfactual table, stated as: *"the token `X` introduces
<role prose>"*, for every permuted role, plus the explicit sentence *"these spellings are not the
ones you may expect; the table above is authoritative and complete"*, plus the statement that
precedence and associativity follow the role, not the spelling.

**Replicates.** 3 deterministic mappings (seeds 6001–6003). Score = mapping mean.

**Failure modes, reported separately** (§10.5 I6 requires this). Every I6 trial that does not pass
is classified into exactly one of:

| Code | Failure mode | Mechanical detection |
|---|---|---|
| `F1_PRIOR` | the model used tokens in their **prior** roles, ignoring the mapping | the raw model output, read as plain real source and built with the frozen recipe **without** inverse mapping, builds and passes the oracle |
| `F2_MIXED` | the mapping was applied at some sites and not others | neither the inverse-mapped source nor the raw source passes, **and** the role-position audit (§11.4) finds both mapping-consistent and prior-consistent occurrences of permuted tokens |
| `F3_OVER` | the mapping was applied where it must not be — inside string/char literals, or applied twice | the diff between the model output and its inverse contains a `STRING`/`CHAR` token, or applying the mapping a second time yields the model's output |
| `F4_OTHER` | an ordinary failure unrelated to the mapping | none of the above |

Counts of F1–F4 per language and per mapping appear in `raw/subtest_scores.json` and in the results
appendix. They are diagnostics; they do not alter the I6 score.

**The vacuity trap, addressed.** Because I6's mapping is a permutation of the language's *own*
vocabulary, a validator asking "did any transformed token survive?" answers "yes" for every
submission, correct or not, and measures nothing (§10.4 warns of exactly this). Every I6 validator
in this design is therefore **role-position aware** rather than token-presence aware, and PF-05
requires each to be demonstrated rejecting an `F1_PRIOR` negative fixture — a program that is
*valid real source* and contains only real tokens, and must still be rejected.

---

## 8. Anonymity

### 8.1 Neutral language labels

For each (condition, replicate unit) a bijection from the ten `language_id`s to the ten labels
`Language A` … `Language J` is generated by a seeded Fisher–Yates shuffle:

```
labels = ["Language A", ..., "Language J"]          # fixed order
state  = seed_state("label|" + condition_id + "|" + decimal(seed))
for i from 9 down to 1:
    state = next(state)
    j = pick(state, i+1)                            # 0 <= j <= i
    swap(labels[i], labels[j])
assignment[language_order[k]] = labels[k]           # language_order = the fixed §0.1 order
```

The assignment is **independent of presentation order** (§10.1) and is re-drawn for every replicate
unit, so a label carries no information across units. The table is written to
`config/anonymity_labels.json` before the unit runs and is published with the results.

Presentation order of trials is *separately* randomized, and every initial trial runs in a fresh
isolated session with no cross-trial memory (§6.2), so order cannot leak identity in any case.

Labels are used in I1, I2, I3 and I6. In I4 and I5 the pack still never states the language name,
and a neutral label is still used, but the surface is real and the model may recognize the
language. **This limitation is stated explicitly in the results for I4 and I5** (§10.1), not
implied.

### 8.2 Diagnostic scrubbing (the leak that repair turns would otherwise open)

Repair turns feed real compiler and runtime diagnostics back to the model (§9 of the spec). Real
diagnostics name the tool, the language, error codes, file paths, and the real token spellings —
which would destroy anonymity at the first repair turn of the first trial. For I1, I2, I3 and I6
every diagnostic is therefore passed through a **scrubber** before it reaches the model:

1. **Re-map token spellings.** Apply `forward` to any source excerpt quoted in the diagnostic, and
   replace every occurrence of a bound real token in the message text with its transformed
   spelling. (For I6, apply the permutation; for I3, the structural spellings.)
2. **Re-map coordinates.** Line and column numbers are translated from the inverse-mapped source
   back to the transformed source the model actually wrote, using the position map the transformer
   records for that trial (the same map that records §5.5's whitespace insertions). A diagnostic
   whose coordinates cannot be mapped is emitted without coordinates and the event is logged.
   **A diagnostic whose primary position falls inside harness-inserted scaffolding (§9.3 H2–H6) is
   suppressed entirely rather than emitted without coordinates**, the suppression is logged, and the
   per-language suppression rate is published in `raw/residuals.json` beside the wholesale-replacement
   rate. Otherwise the model would receive a real error, with no location, about code it did not
   write — and would receive it disproportionately in whichever languages need the most scaffolding,
   so that their Repair Success and Repair Efficiency would measure harness noise rather than the
   language.
3. **Redact identity.** Every string in `config/leak_terms.txt` is replaced by a fixed neutral
   placeholder: tool names → `the toolchain`, language names → `the language`, file paths → the
   neutral name `program`, ecosystem-identifying error-code prefixes → removed, standard-library
   paths → their transformed spellings (I2) or `the standard library` (I1, I3, I6).
4. **Verify.** The scrubbed text is re-scanned; if any leak term survives, the diagnostic is
   replaced wholesale by the frozen generic message
   `the toolchain rejected the program; it reported <k> problem(s) at the marked positions` with
   the mapped positions appended, and the substitution is logged.

Both the raw and the scrubbed diagnostic are preserved in the trial record (§23 of the spec). PF-12
validates the scrubber in both directions.

**Published residual.** Scrubbing degrades diagnostic quality, and it does not degrade it equally:
a toolchain whose messages are mostly prose survives scrubbing better than one that leans on
ecosystem-specific error codes. The per-language rate of step-4 wholesale replacements is published
in `raw/residuals.json`, and the interpretation section must note that Repair Success and Repair
Efficiency in I1/I2/I3/I6 are measured through this filter, while I0/I4/I5 are not.

---

## 9. Harness conventions and fixups (spec §10.4, requirement (b))

> "If the prompt withholds a fact — for instance because stating it would reveal the language — the
> model cannot be marked down for not knowing it."

### 9.1 The principle, frozen

Any convention that the task statement and the Reference Pack do not state is the **harness's**
responsibility, not the model's. The pack states everything about the *language*; it does not state
facts about *this benchmark's file system and invocation*, because those facts identify the
toolchain. The harness supplies them.

### 9.2 Code extraction (frozen)

From the model's raw output: take the content of the **last** fenced code block if any fenced block
exists; otherwise take the entire output. Strip a leading language tag line from the fence if
present (it is ignored, never used as a signal). Nothing else is edited. The extraction rule is
identical for all ten languages and all conditions, and is exercised at PF-05 with outputs that
have zero, one, and several fenced blocks.

### 9.3 Fixups (applied in this fixed order; every application is logged per trial)

| # | Fixup | Applies to |
|---:|---|---|
| H1 | Write the source to the correct file name and extension for the language. | all |
| H2 | If the language requires a compilation-unit/package/module declaration and the submission has none, prepend the required declaration. | languages that require one |
| H3 | If the language requires the entry point to live inside a named type, and the submission declares such a type with a different name, adjust the **file name** to match the submission rather than demanding a fixed name; if the submission declares no such type, wrap the submission's top-level code in the required scaffold. | languages that require one |
| H4 | If the language requires a named entry-point function and the submission has none, wrap the submission's top-level statements in the required entry point, leaving declarations at top level. | languages that require one |
| H5 | If the language's entry point must declare error propagation for the output facility the pack documents, add that declaration. | languages that require one (C-4) |
| H6 | If the build recipe needs a companion artifact (output directory, jar name, emitted JS file name) the source cannot express, the harness creates it. | all |

Fixups **never** add an import, never add a statement that produces output, never change an
expression, and never reorder user code. A fixup that would need to do any of those is not applied;
instead the trial fails and is recorded with reason `FIXUP_INSUFFICIENT`.

**No fixup may supply anything the pack documents (frozen).** *H2–H6 apply only to material that is
**absent from the pack**. If a language's pack documents its entry-point shape (P1), its
compilation-unit declaration (P1/P9), its imports (P9) or its error-propagation marker (P10 with
role `K27`), then the absence of that element from a submission is a **model failure**, and the trial
proceeds to build as submitted.* The principle of §9.1 is that the harness carries the facts the
prompt **withholds**; it is not a licence to carry facts the prompt **states**. Under the previous
wording a model that failed to apply a documented — and, in I1/I2, *anonymized* — rule was silently
rescued, but only in the languages that need scaffolding, while a model that failed to apply the
anonymized conditional keyword was not. That is a rescue granted by language rather than by rule.

**Per-language fixup counts are published** (`raw/residuals.json`), **split into two columns:
pack-documented (which must be zero) and unstated-convention.** A language needing many
unstated-convention fixups is not penalized — that is the entire point — but a reader can see how
much scaffolding the harness supplied on each language's behalf, and can verify that none of it was
material the model was told to write. PF-04 and PF-13 are the tests that this actually works.

---

## 10. Mandatory pre-flight validation (spec §10.4)

> An infrastructure defect in this track does not announce itself. It arrives looking exactly like
> a language failure.

No Reference Pack is built and no trial is scored until `preflight/preflight_report.json` records
`"all_pass": true`. Each check below has a fixed ID, a procedure, a pass criterion, and an evidence
directory. Evidence is the actual command lines, exit statuses, stdout/stderr, and produced
artifacts — not a summary claiming they passed.

### 10.1 The checks

| ID | Check | Procedure | Pass criterion |
|---|---|---|---|
| **PF-01** | **Fixtures build and run, output exact** (§10.4 requirement 1) | Build and run every reference solution and every pack example E1–E6 (each wrapped by the frozen example harness) for all ten languages with the frozen **intrinsic-track** recipes of §0.1/§0.1a, and record the `runtime_check_inventory` (§0.1a) each recipe actually enables. | 100% build success, exit status 0, stdout byte-identical to the frozen expected output. Any failure blocks the run: a fixture that does not build teaches every trial in that language to reproduce something that cannot build. |
| **PF-02** | **Round-trip token identity** | For every fixture × every replicate unit: check R1 and R5 of §6.4. | All pass. Diffs are stored. |
| **PF-03** | **Binding-table completeness** | For every language, enumerate every `WORD` token in its reference solutions and pack examples; classify as bound K/V/`U:` role token or locally declared identifier. Independently recompute the §7.1a domain as (frozen reserved-word list ∩ fixture word tokens) and compare it to the binding table. Verify every `BOUND` entry carries a `citation` (§3.4). | Zero unclassified word tokens; **`unbound_word_tokens` empty for all ten languages** (a non-empty array blocks the run); the recomputed §7.1a domain equals the table's; every binding cited. |
| **PF-04** | **Inverse-mapped fixtures build and run** (§10.4 invariant) | For every fixture × unit: build and run `inverse(forward(F))` with the real toolchain; compare to `F`'s output. Also check R4 non-triviality. | R2, R3, R4 all pass for all ten languages and all units. |
| **PF-05** | **Every validator can both pass and reject** (§10.4 requirement 3) | For each validator — the oracle, the check-unit splitter, **the §4.2a conformance gate (per condition)**, the spec-compliance checker **including each `declaration_predicates` and `nested_sequence_predicate` entry**, the I4 overlay checker **and its Overlay Compliance rate**, the I5 rule checker, the hallucination detector (per condition) **including the `config/diagnostic_classes.json` regexes for `UNRESOLVED_NAME` and `UNKNOWN_MEMBER`**, the silent-bug detector, the I6 role-position auditor, the code extractor, the `INVERSE_AMBIGUOUS` detector — run ≥1 positive fixture that must pass and ≥1 **mutated** negative fixture that must be rejected, **in every one of the ten languages**. Mutations are recorded. | Every validator passes its positive and rejects its negative, for every language. **Explicit additional requirements:** (a) for I6 and for any condition whose mapping is a permutation of the language's own vocabulary, the negative fixture must be an `F1_PRIOR` program — valid real source containing only real tokens — and the validator must reject it; a "did any transformed token survive?" test is vacuous here by construction and does not satisfy PF-05. (b) For I1, I2, I3 and I4 the conformance gate's negative fixture must be **correct real source that ignores the transformation entirely**, and the gate must reject it. (c) The H_API detector's negative fixture must be exercised for every language, including those whose build step performs no name resolution, using the runtime diagnostic class. |
| **PF-06** | **Pack slot conformance and minimality** | For every pack: verify the twelve sections are present in order with the template's sentence skeleton; verify the fixed P2 interpolation line (§2.2) is present in all ten packs in one of its two permitted forms; verify exactly six examples in slots E1–E6 (XA/XB/XC for I5); verify each example demonstrates its slot and nothing outside it; verify no example contains a held-out I5 combination; verify **no task statement using Rule B leaves a grouping unstated** (§4.4); verify the I4/I5 entry-point exemption sentence (§7.4) appears in P11 of all ten packs in identical wording; record token and character counts. | Conformant; counts published; whole-pack soft cap exceeded by ≤30% for every language, or the slot list is revised for all ten before any trial. A pack below the P1–P11 floor triggers the §2.4 slot-fill review and is **never padded**. |
| **PF-07** | **Identity-leak scan of packs and prompts** | Run the §2.6 scan over every pack, every task statement, and every system/user prompt template for I1, I2, I3, I6. | Zero hits. |
| **PF-08** | **Lexicalizer self-test** | Regenerate every mapping from its seed twice in separate processes; verify determinism, one-to-one-ness, no collisions, no prefix relation, exact length 6, `^[a-z]{6}$`, and that every rejection filter fires at least once across the run. Record the three word-list SHA-256 values. | All pass; determinism byte-exact; SHA-256 recorded. |
| **PF-09** | **I3 budget realizability and matching; I6 permuted-operator eligibility** | Compute the eligible S-dimension set as the intersection across all ten languages; verify each frozen set's five dimensions are eligible, applying the frozen fallback order if not; verify each language receives exactly the same SPP count; record the perturbed-site count and sites-per-100-lexical-tokens density per language and set. **Also:** verify the §7.7 permuted operator pair is recorded as a comparison operator in all ten binding tables and occurs in **no** delimiter position in any of the ten grammars or fixtures. | All ten receive an identical SPP count; the realized dimension list and the density table per set are published. The permuted operator pair passes the eligibility check in all ten languages, or it is not used and the failure is published. |
| **PF-10** | **Structural-marker selection and reservation** | Walk the frozen priority list `%%`, `~~`, `¤` in order. For each candidate, scan every task statement, reference solution, expected output, pack example and identifier in all ten languages for the candidate, and check that no token of any of the ten grammars begins with it. Select the first candidate that passes both, write it into all ten lexer profiles as `structural_marker`, and publish it. `@` is excluded a priori (§7.3). | A marker is selected with zero occurrences in any of the ten languages' material and no prefix relation in any of the ten grammars; the selection is recorded in `preflight_report.json`. If no candidate qualifies, the run is blocked and the defect is handled under §14. |
| **PF-11** | **Expected-output determinism** | Run all ten reference solutions per task and byte-compare their outputs to each other and to the frozen expected file. Freeze the check-unit decomposition. | All ten identical; check-unit lists frozen. |
| **PF-12** | **Diagnostic scrubber validation** | For each language, induce a build failure and a runtime failure in a fixture; scrub the diagnostics; scan for leak terms; verify coordinate mapping against a known-position defect; verify the wholesale-replacement fallback fires when forced. | Zero leak terms in any scrubbed diagnostic; coordinates map correctly; fallback demonstrated. |
| **PF-13** | **Harness-convention sufficiency** (§10.4 requirement 2) | For each language, construct a **bare-body submission from that language's pack**: it contains **every element the pack documents** (entry-point shape, compilation-unit declaration, imports, error-propagation marker — all of them, in their condition-appropriate transformed spelling) and omits **only the unstated conventions** (file name, output directory, jar name, and any other fact about this benchmark's file system and invocation that the pack does not state). It is otherwise a correct solution to T1 using only pack-documented constructs. Feed it through the real pipeline: extraction → gate → inverse → fixups → build → run → oracle. | The bare-body submission **passes** for all ten languages, with **zero pack-documented fixups applied** (§9.3). If it does not pass, the fixup set may be extended **only** for unstated conventions, before any trial is scored; a failure that would require supplying pack-documented material is a defect in the pack or the recipe, not a licence to widen the fixups. |

| **PF-14** | **Configuration freeze and hash manifest** | Compute the SHA-256 of **every** file under `config/` — `role_bindings.json`, all ten `lex_profiles.json` entries, every `tasks/<task>/compliance.json`, every task statement and its frozen data (the T2 twelve-entry table, the T4 word line), every reference solution, the three word lists and the ten per-language reserved-word lists, `leak_terms.txt`, `stderr_allow.json`, `diagnostic_classes.json`, `pack_template.md`, `run_config.json`, `anonymity_labels.json` and the I5 example variants — and write them to `config/config_manifest.json`. Re-verify every hash immediately before each scored cell. | The manifest is complete (no file under `config/` missing) and is written **before PF-01 runs**. **The scoring pipeline refuses to run if any recorded hash changes**, and a changed hash is a §14 change-control event, not an edit. Rationale: these files, not this prose, are what actually determine the numbers; unhashed, they are an unaudited degree of freedom larger than any weight in §11.2. |

### 10.2 Evidence preservation

Each check writes to `preflight/PF-NN/`:

- `commands.txt` — every command executed, verbatim, in order;
- `stdout/`, `stderr/` — captured output per command;
- `artifacts/` — sources, transformed sources, inverse-mapped sources, binaries or emitted files
  (binaries may be replaced by their SHA-256 when size is prohibitive; the SHA-256 is mandatory);
- `result.json` — `{check_id, pass, per_language: {...}, notes, timestamp}`.

`preflight/preflight_report.json` aggregates all fourteen and carries the gate field
`"all_pass": true|false`. **The scoring pipeline refuses to run while `all_pass` is false.** The
report is preserved with the run; it is part of the deliverable, not scaffolding.

Pre-flight is re-run in full whenever any of the following change: a lexer profile, a binding table,
a transformer, a task statement, a reference solution, a word list, a pack template, a build recipe,
a diagnostic-class table, a compliance checklist, or the fixup set — equivalently, whenever any hash
in `config/config_manifest.json` changes. Partial re-validation is not permitted.

### 10.3 The zero-row rule

> **An all-zero or near-all-zero row for one language, one condition, or one validator is treated as
> a SUSPECTED INFRASTRUCTURE DEFECT and is investigated before it is reported as a result.**

Trigger conditions (any one fires the investigation):

- **Z1** — for a (language, condition) cell: Compile/Parse Success ≤ 0.10, or Correct@N = 0 across
  all tasks, or Test Pass Rate ≤ 0.05.
- **Z2** — for a (language, condition, task) cell: every replicate fails at the same stage with the
  same diagnostic signature.
- **Z3** — a validator that never rejects anything across the entire run, or never passes anything.
- **Z4** — a language whose score on one condition is more than 40 points below its I0 control
  score while the median language's drop on that condition is under 15 points.
- **Z5** — a `FIXUP_INSUFFICIENT` or `INVERSE_AMBIGUOUS` rate above 0.20 for any (language,
  condition).

Investigation procedure, in order, with each step recorded in `raw/defects.json`:

1. Re-run PF-01, PF-04, PF-05 and PF-13 restricted to the affected language and condition.
2. Manually inspect three affected trials end to end: raw model output → extracted code → inverse
   image → fixed-up source → build log → run log → oracle verdict.
3. Take the **reference solution** for the affected task, forward-transform it, and feed it through
   the scoring pipeline as if it were a model submission. If it does not pass, the defect is in the
   infrastructure, full stop — a correct program must score correct.
4. Take the reference solution, apply the condition's rule set by hand, and check that the
   spec-compliance and overlay validators accept it.
5. Check the scrubbed diagnostics the model actually received for that cell; an empty or
   uninformative diagnostic stream is an infrastructure defect in the repair loop.
6. If steps 1–5 find no defect, record the positive evidence that the outcome is genuine: the
   failing model outputs, the diagnostics they received, and the specific rule they violated. Only
   then may the row be reported as a language result.

Defects found are fixed, the affected cells are re-run or re-verified, and **both the defect and the
fix are recorded** in `raw/defects.json` with the trial identifiers affected. They are never
silently corrected (§10.4).

---

## 11. Metrics, normalization, weights, and aggregation

### 11.1 The unit of scoring

For each (language, replicate unit) a **Condition Task Effectiveness Score (CTES)** on 0–100 is
computed from the twelve metrics below, over the unit's task set. The subtest score for a language
is the **mean of CTES across that subtest's replicate units**; sd, min and max are also reported
(§10.6).

### 11.2 The fixed weight table (spec §10.6, verbatim)

| Intrinsic condition metric | Weight | §25.1 family |
|---|---:|---|
| Compile / Parse Success | 7% | A |
| Correct@1 | 18% | A |
| Correct@N | 8% | A |
| Test Pass Rate | 15% | A |
| Repair Success | 8% | A |
| Repair Efficiency | 6% | E |
| Specification Compliance | 10% | A |
| Hallucination Resistance | 8% | B |
| Silent Bug Resistance | 10% | B |
| Unseen-case Generalization | 7% | A |
| Source Token Efficiency | 1% | C |
| Total Token Efficiency | 2% | C |

Sum = 100%. **These weights are fixed and are not tuned after observing results** (§10.6, §32).

### 11.3 Raw definitions (mechanical)

Within a (language, replicate unit), over its task set (`|CORE| = 5`, `|OVERLAY| = 3`,
`|COMPOSITION| = 3`), one trial per task:

1. **Compile / Parse Success** = (tasks whose *initial* submission, after inverse mapping and
   fixups, builds with exit status 0) / (tasks). For Python the build step is
   `python3 -m py_compile FILE.py`; for TypeScript it is `tsc FILE.ts` with a non-zero exit on type
   error. Every other language uses its frozen build recipe.
2. **Correct@1** = (tasks passing the §4.2 oracle at repair turn 0) / (tasks).
3. **Correct@N** = (tasks passing the oracle within 3 repair turns) / (tasks).
4. **Test Pass Rate** = mean over tasks of (passed check units / total check units) at the trial's
   **final** state (after any repairs). The unit list includes `CONFORMANT` in every condition that
   describes a rule in P11 (§4.2), so Test Pass Rate is sensitive to whether the described rule was
   applied and not only to the bytes produced.
5. **Repair Success** = (tasks that failed at turn 0 and passed within 3 repairs) / (tasks that
   failed at turn 0). If no task failed at turn 0, this metric is **N/A** for that unit, with reason
   `NO_FAILURES_TO_REPAIR`, and its weight is redistributed per §26 (excluded from the denominator,
   remaining weights renormalized).
6. **Repair Efficiency** = mean over tasks of `100 * (1 - repair_turns/3)`, clipped to [0,100],
   where a Correct@1 task contributes 100 and a task still failing after the third repair
   contributes 0 (family E). A task that never built and never passed contributes 0.
7. **Specification Compliance** = (satisfied checklist items) / (total checklist items), summed
   across the unit's tasks. Each task carries a frozen machine-checkable checklist stored in
   `config/tasks/<task>/compliance.json`, containing items of these kinds only:
   output-line-count; output-field-format per line class; presence of a required declaration kind
   (a record type for T2, a nested sequence for T5, a user-defined function of the stated arity for
   C1–C3); absence of constructs the task forbids; and — where the condition has one — the
   **overlay/rule compliance verdict** (I4's N-ALPHA/N-BETA checker, I5's A/B/C checker, I3's
   structural-conformance check, I6's role-position audit). Compliance is evaluated on the trial's
   **initial** submission, so that it measures following the specification rather than surviving
   the repair loop.

   **Required-declaration items are decided by frozen per-language predicates, not by inspection.**
   `lex10` cannot decide "a record type is declared" or "a nested sequence is built" across ten
   languages that spell those things a dozen different ways, and a predicate that is re-invented per
   language per run is not reproducible. Frozen: *a required-declaration item is satisfied iff the
   token bound to the relevant role (`K12` for a record type) occurs at a declaration site, per the
   per-language predicate recorded in `role_bindings.json` under `declaration_predicates`; a
   nested-sequence item is satisfied iff that language's `nested_sequence_predicate` matches. Both
   predicates are frozen before PF-05, which must exercise each with a positive and a mutated
   negative fixture in every one of the ten languages.* A language whose idiom satisfies the item
   without a dedicated declaration (a nested literal rather than a declared type) has that fact
   encoded in its own predicate, cited like every other binding (§3.4).

   **I4 checklist split.** For I4 the checklist for each task is **50% ordinary items and 50%
   Overlay Compliance**, where *Overlay Compliance = compliant sites / applicable sites* (§7.4).
   Overlay Compliance is not one item among many; that shape made a 20%-weight subtest named
   "Novel-rule Generalization" almost entirely insensitive to whether the novel rule was acquired.
8. **Hallucination Resistance** = `1 − (trials with ≥1 hallucination event) / (trials)` (family B).
   Events are defined per condition in §11.4.
9. **Silent Bug Resistance** = `1 − (silent-bug trials) / (trials)` (family B). A trial is a
   **silent bug** iff its final program builds with exit status 0, runs to completion with exit
   status 0, emits nothing on standard error beyond the allow-list, **and** fails the oracle. That
   is: wrong output, no signal. A trial that crashes, times out, or emits a diagnostic is a failure
   but not a silent bug. **Nor is a trial that failed the §4.2a conformance gate**: its defect was
   detected and reported by the harness, so it is a signalled failure. Gate-failed trials are
   excluded from the silent-bug numerator and remain in the denominator, so that one trait is not
   charged twice — once to the oracle and again to a metric that exists to measure *undetected*
   wrongness. The gate's deliberate overlap with the hallucination metrics of §11.4 is different in
   kind and is intended: the gate sets the task's oracle verdict, §11.4 publishes the per-trial rate,
   and both are stated rather than folded into one number.
10. **Unseen-case Generalization** = pass rate on the unit's designated unseen-case task — T5 for
    `CORE` and `OVERLAY`, C3 for `COMPOSITION` — evaluated at repair turn 0.
11. **Source Token Efficiency** — counted with the **frozen lexical source tokenizer** of
    `09_llm_run_config → source_token_counting` (one implementation for all ten languages, SHA-256
    recorded); the model's own tokenizer is not used and harness-reported counts are published
    beside it and never substituted for it. **The count is taken on the inverse-mapped source that
    is actually compiled, in every condition**, so that a language is not scored on how many
    six-character pseudo-words its transformation happened to insert — that count differs
    systematically between languages by `anonymized_token_count`, which §7.1a makes a property of
    the language's reserved surface rather than of the program the model wrote. The
    transformed-source count is published as a residual (§13.1).

    **Normalization is per task, at the lowest comparable workload level (§25.2).** *For each
    (condition, replicate index, **task**), `best_positive_raw` is the minimum final-source lexical
    token count among the languages **that passed that task**; a language's unit score for this
    metric is the unweighted mean of its per-task normalized scores over the tasks it passed.* If it
    passed none, the metric is N/A for that unit with reason `NO_PASSING_SOURCE` and its weight is
    redistributed (§26). Taking a median over each language's own passing set and then comparing
    those medians across languages would compare different workloads: a language that passed only
    the shortest task would beat a language that passed all five, and would do so **for failing
    more** — a score changed by the shape of the aggregation rather than by measurement.
12. **Total Token Efficiency** — raw = (prompt tokens + completion tokens summed over the initial
    turn and all repair turns), taken from the harness's own accounting, **per task**. Family C, with
    the **same per-task normalization** as metric 11: `best_positive_raw` is the minimum across the
    ten languages for that (condition, replicate index, task), and the unit score is the unweighted
    mean over the unit's tasks. Every task consumes tokens whether or not it passed, so all of the
    unit's tasks are included and the metric is never N/A.

Family-C publication requirement: whenever the applicable raw values for metric 11 or 12 span a
factor of 100 or more across the ten languages **for any task**, publish the raw values, the ratios
`raw_i / best_positive_raw`, and the §25.1 note that the normalized score is compressed and the
ratios carry the comparison (§25.1, "Known property of family C").

### 11.4 Hallucination events, per condition

| Condition | Event type | Definition |
|---|---|---|
| I1, I2 | `H_REAL` | The submission contains, at a grammar-significant position, a **real** token of a role that the mapping anonymized. The model fell back to remembered vocabulary. |
| I1, I2 | `H_INVENT` | The submission contains a word token that is neither a mapped pseudo-word, nor a `WORD` declared as an identifier within the submission, nor a real token of an untransformed role. |
| I3 | `H_STRUCT_REAL` | The submission uses an original token of a perturbed S-dimension. |
| I3 | `H_STRUCT_INVENT` | The submission uses a `<marker>xy` structural token not in the set's manifest. |
| I6 | `H_PRIOR` | A permuted token is used in its **prior** role rather than its π-assigned role. Detected by the §7.7 F1/F2 procedure — the role-position audit, **not** a token-presence test, which is vacuous here. |
| I0, I4, I5 | `H_API` | The submission calls a standard-library name that does not exist in the language, or uses a construct the pack does not document and the language does not provide. Determined mechanically from the frozen diagnostic classes below — *an `UNRESOLVED_NAME` or `UNKNOWN_MEMBER` diagnostic from the build step, **or the equivalent runtime diagnostic class on standard error for languages whose build step does not perform name resolution*** — never by judgement. |

**Frozen diagnostic classes (`config/diagnostic_classes.json`).** The two classes `UNRESOLVED_NAME`
and `UNKNOWN_MEMBER` are defined by per-language regular expressions over **both build and run
output**, frozen and hashed before PF-05 (PF-14) and exercised with a positive and a negative fixture
per language at PF-05.

This matters because the ten build steps do not all resolve names. A language whose frozen build step
is a syntax/compile check that never reports an unresolved name would otherwise be **incapable** of
registering an `H_API` event and would score ~100 on Hallucination Resistance in I0, I4 and I5 *by
construction* — an 8%-weight metric handed to it by the harness rather than earned. Covering the
runtime class (name-resolution and attribute errors reported on standard error at run time) puts
every language on the same instrument. Where a language's coverage is still partial, the partiality
is recorded per language in `raw/residuals.json`, so the reader can see which languages the metric
observes fully and which only partly.

A trial counts once no matter how many events it contains. Event counts are additionally published
per language for diagnosis. The hallucination metrics remain **diagnostics that are scored**; they
are not a substitute for the §4.2a gate and the gate is not a re-weighting of them.

### 11.5 Normalization and CTES

Per §25.1, applied at the (condition, replicate index) level across the ten languages:

- families A and B: `Score = 100 * clamp(x, 0, 1)` and `100 * (1 - clamp(x, 0, 1))`;
- family E for Repair Efficiency, as in §11.3.6;
- family C for metrics 11 and 12: `Score_i = 100 * best_positive_raw / raw_i`, applied **per
  (condition, replicate index, task)** and then averaged unweighted over the language's applicable
  tasks (§11.3.11–12). Raw token counts are strictly positive, so no epsilon shift is needed;
  `epsilon` is therefore not defined for these metrics and the unshifted form is used.

`CTES(language, unit) = Σ_m weight_m * Score_m / Σ_m weight_m` over the metrics applicable in that
unit (§26 renormalization for any N/A). All scores are clipped to [0, 100]; raw values are retained
at full precision; reported scores carry two decimal places; ranking uses unrounded values (§25.3).

### 11.6 Subtest and final aggregation

For each subtest and language: report **every** replicate's CTES, then mean, sd, min, max. The mean
is the subtest's primary score (§10.6).

```
LLM Intrinsic Learnability Score
    = 0.20*I1 + 0.20*I2 + 0.15*I3 + 0.20*I4 + 0.15*I5 + 0.10*I6
```

Fixed before results are observed; not tuned afterwards (§10.6, §32). Each of I1–I6 is also
reported separately, in the §28.1 table, with `100 = best`.

### 11.7 Familiarity-drop diagnostics (§10.7)

Two quantities are published, both **diagnostic only**, both reported in a separate block of the
§28.1 table, and **neither is ever used as a correction factor** to any score:

- **Primary (frozen definition): `Drop_task(Ix) = CTES(I0) − Score(Ix)` for `Ix ∈ {I1, I2}`**,
  using the I0 untransformed control (§7.0), which shares the task set, pack template, oracle and
  metric definitions with the transformed conditions. This is the only form of the subtraction in
  which the two terms are commensurable.
- **Secondary: `Drop_practical(Ix) = LLM Practical Effectiveness Score − Score(Ix)`**, using the
  §9.1 score. Published so that the other reading of the §10.7 sentence is also available, with an
  explicit note that the two terms come from different task sets and are therefore a weaker
  comparison than the primary form.

Both are reported per language. A small drop can mean the model learned the transformed
specification well; it does not prove the model had no structural prior (§10.7).

### 11.8 N/A policy in this evaluation (§26)

| Situation | Treatment |
|---|---|
| No task failed at turn 0 | Repair Success = N/A, reason `NO_FAILURES_TO_REPAIR`, weight redistributed. |
| No task passed in the unit | Source Token Efficiency = N/A, reason `NO_PASSING_SOURCE`, weight redistributed. Every other metric is scored, including the zeros. |
| A role is `NOT_LEXICALIZED` in a language | Not an N/A: the role is simply absent from that language's mapping, the count is published, and every metric is still scored. |
| A language fails everything in a condition | **Not** N/A. It is a zero, and it triggers the §10.3 zero-row investigation before it is reported. |
| Model tokenizer unavailable | Not an N/A and not a substitution: the frozen lexical counter and character counts are the instruments (§2.4, §5.4, §11.3.11), applied identically to all ten languages, and the unavailability of the model's BPE tokenizer is published as a stated limitation. |
| A trial exhausts an output cap | **Not** N/A. FAIL with reason `TOKEN_BUDGET_EXHAUSTED` (§0.3); the rate is published per language. |
| A submission fails the §4.2a conformance gate | **Not** N/A. FAIL for that task at that turn, with the gate verdict and the submission preserved. |
| A provider / rate-limit / transport failure | Neither N/A nor FAIL: retried per `09_llm_run_config` and recorded in `_deviations.json`, never converted into a model failure (§0.3). |

A missing capability never escapes scoring by being relabelled N/A (§26). Every N/A carries a
recorded reason in `raw/condition_scores.json`.

---

## 12. Prompt structure (frozen)

### 12.1 System prompt (identical for all ten languages and all conditions)

Stored verbatim in `config/run_config.json → system_prompt`. It states: the model will be given a
reference description of a programming language and a task; it must produce one complete program in
that language; it must use only what the reference describes; it must output the program in a single
fenced code block and nothing else. It names no language, no toolchain, and no file name.

### 12.2 User prompt template (identical structure for all ten languages)

```
You are writing a program in {LABEL}.

--- {LABEL} REFERENCE ---
{REFERENCE_PACK}
--- END REFERENCE ---

--- TASK ---
{TASK_STATEMENT}
--- END TASK ---

Write one complete program in {LABEL} that performs the task exactly.
Output only the program, in a single fenced code block.
```

`{LABEL}` is the neutral label from §8.1 for I1/I2/I3/I6, and for I4/I5 as well (the surface is real
but the name is still never stated). The template is byte-identical across languages; only the
three slots differ.

### 12.3 Repair prompt template (identical for all ten languages)

```
The program did not satisfy the task. The toolchain and the checks reported:

--- REPORT ---
{SCRUBBED_DIAGNOSTICS_AND_CHECK_RESULTS}
--- END REPORT ---

Revise the program. Output only the corrected complete program, in a single fenced code block.
```

The report contains the scrubbed build/run diagnostics (§8.2) and, when the program ran, the first
failing check unit expressed as `expected line <k>: <expected>` / `produced line <k>: <produced>`.
The number of failing check units disclosed is capped at **3** for every language and condition.
Expected output lines are quoted only for lines the task statement already determines; no hidden
expected output beyond the first three mismatches is revealed.

**When the §4.2a conformance gate failed**, the report additionally states **which described rule was
violated, in language-neutral prose, without revealing the real spelling** — for example "the program
used a word that the reference does not define for this language; every grammar word must come from
the table in section 11" or "a function's name does not carry the sigil the reference requires for
its parameter count". The wording is drawn from a frozen per-condition sentence list in
`config/run_config.json`, is identical across the ten languages, and never quotes the untransformed
token.

### 12.4 Preservation (§23, §24)

Every trial directory `trials/<unit>/<language_id>/<task>/` contains: `prompt_initial.txt`,
`output_initial.txt`, `source_initial.<ext>`, `source_initial_inverse.<ext>`, `build_00.log`,
`run_00.log`, `oracle_00.json`, then per repair turn `prompt_repair_0k.txt`,
`output_repair_0k.txt`, `source_repair_0k.<ext>`, `source_repair_0k_inverse.<ext>`,
`build_0k.log`, `run_0k.log`, `oracle_0k.json`, plus `diagnostics_raw/`, `diagnostics_scrubbed/`,
`fixups.json`, `tokens.json`, and `trial.json` (final verdict, metric primitives, failure-mode
codes). Silent-bug versions are preserved as part of the history, never discarded (§23).

---

## 13. Published residuals and stated limitations

These are published with the results in `raw/residuals.json` and summarized in the report narrative.
They are honest statements of where matching is imperfect, not defects to be hidden.

### 13.1 Lexicalization residuals
- Per-language `anonymized_token_count` for I1 and I2 — now the **mechanically derived** size of
  (frozen reserved-word list ∩ fixture word tokens) plus bound V-roles, per §7.1a, not an authored
  quantity. Not equalized, by the reasoning in §7.1.
- Per-language `unbound_word_tokens`, which must be **empty**; publishing the empty arrays is the
  evidence that the judgement-based escape hatch is closed.
- Per-language character and frozen-lexical-token counts per pseudo-word (both matched exactly by
  construction), and the stated limitation that model-BPE matching is **unmeasurable** here because
  the client does not expose that tokenizer (§5.4).
- Number of collision redraws per language and seed, for pseudo-words **and** for the two-letter
  codes of I3/I4/I5 (§5.2).
- For each language, **which bound V-roles its `V18` interpolation spelling makes optional in I2**
  (§3.2), with the explicit statement that I2's anonymization pressure is lighter for interpolating
  languages and that this is published rather than equalized.
- Per-language transformed-source lexical token counts alongside the inverse-mapped counts that
  Source Token Efficiency actually uses (§11.3.11).

### 13.2 Anonymity residuals
- **I1 anonymizes grammar words only.** Standard-library names and namespace paths remain real by
  design (§7.1), so I1 packs are **substantially more recognizable than I2 packs** — the library
  surface is the most identity-revealing part of a pack. **The I1 row of the §28.1 table must carry
  that statement explicitly**, and §2.6's leak scan exempts exactly that material (and nothing else)
  so the scan is realizable rather than self-blocking.
- I1 and I2 remove the word-level surface but not the structural shape; a reader who knows the
  languages can often still recognize one from its shape. §10.1's claim — a familiarity-controlled,
  specification-grounded evaluation, not a proof of zero prior exposure — is the claim made.
- I3 keeps real keywords and I6 keeps the language's own vocabulary, so both remain recognizable.
  The model is never told the name, which is what §10.1 requires, but recognizability is not
  removed and the results must say so.
- **I4 and I5 keep the real surface entirely** (§10.1). The model is never told the name but can
  recognize the language. **The I4 and I5 rows of the §28.1 table must carry this statement
  explicitly, not by implication.**
- Per-language rate of wholesale diagnostic replacement by the scrubber, **and** the per-language
  rate of diagnostics suppressed because their primary position fell inside harness-inserted
  scaffolding (§8.2 step 2).

### 13.3 Structural and rule residuals
- The realized S-dimension list per transformation set, and confirmation that all ten languages
  received an identical SPP count (§7.3).
- For I4/N-ALPHA rule A2: languages that already distinguish mutable from immutable bindings supply
  a cue the others do not. Published, not engineered away; N-BETA's rules have no analogous cue,
  and I4's score is the mean over both rule sets.
- Per-language fixup counts (§9.3), **split into pack-documented (which must be zero) and
  unstated-convention**, and `FIXUP_INSUFFICIENT` / `INVERSE_AMBIGUOUS` rates.
- Per-language I3 perturbed-site counts and sites-per-100-lexical-tokens density, with the statement
  that the SPP budget is matched **by axis and not by density** (§7.3).
- `runtime_check_inventory`: which runtime safety checks each language's intrinsic-track build
  actually enables (§0.1a), so that Silent Bug Resistance is read as a language property and not as
  an artifact of a build flag. C++ performs no such checks at any optimization level; that is a real
  and permanent property of C++, published and **not** compensated for (spec §7).
- The **scope limitation** that no task requires an associative container (§4.1): all ten languages
  provide one, none is exercised, and the limitation costs each of them the same unexercised
  facility.
- Per-language coverage of the `H_API` diagnostic classes (§11.4) — full or partial — so a high
  Hallucination Resistance in I0/I4/I5 can be read against how much the instrument could observe.
- Per-language `TOKEN_BUDGET_EXHAUSTED` rate (§0.3).

### 13.4 Statistical residuals
- One trial per cell (§0.3). Differences of a few points between languages are not resolved by this
  measurement and must not be described as if they were. Replication in this track is across seeds
  and transformation sets, and the per-subtest sd, min and max are published for exactly this
  reason.
- Where deterministic decoding makes repeated generations byte-identical, all trials are preserved
  and the duplication rate is reported; no ad-hoc prompt noise is added (§6.2).
- **Unseen-case Generalization is a single binary observation per replicate.** It is the turn-0 pass
  rate on one designated task (T5 for `CORE`/`OVERLAY`, C3 for `COMPOSITION`) with one trial per
  cell, so each replicate contributes 0 or 100 and a language's subtest value is quantized to
  `1/(number of replicates)` — steps of 20 points for a five-seed subtest, 33 for a three-seed one.
  **Differences below that step are not resolved by the measurement and must not be described as if
  they were.** The same designated task also contributes to Correct@1, Correct@N and Test Pass Rate,
  so a single outcome on it moves roughly a quarter of CTES; that concentration is a property of a
  one-trial-per-cell design and is stated rather than smoothed away by averaging.

---

## 14. Change control

This document is frozen. If a defect in it is discovered during the run:

1. Stop scoring the affected condition.
2. Record the defect, the evidence, and the proposed fix in `raw/defects.json` **before** changing
   anything.
3. Apply the fix identically to all ten languages.
4. Re-run the full pre-flight (§10.2 forbids partial re-validation).
5. Re-run every affected cell.
6. Record both the defect and the fix in the results. Defects are not silently corrected (§10.4).

No weight, metric definition, task, oracle, rule set, budget, seed count or aggregation rule in this
document may be changed after any scored generation for the purpose of changing any language's
position. Any change to a §25.1 normalization family must be registered before the run it first
applies to, and that run must publish results under both the old and new formula (§25.4).

Quidra is the language under evaluation here, not the language this design is built around. If
applying this document mechanically produces a poor Quidra result, that is the result.

---

## Remediation changelog

**Status of this remediation.** An adversarial audit of the frozen methodology found bias toward
Quidra, mechanical-reproducibility defects, and spec-compliance defects in this document. Every
blocker and every major finding below is applied; the four minor findings are applied as well.
**No results have been observed**: no Reference Pack has been built, no transformer has been run, no
scored generation has been produced, and no score has been computed from this document or any sibling
methodology document. These corrections are therefore pre-registration, not post-result formula
selection (§32). No finding was rejected.

Every change below was tested against the symmetry rule stated in the preamble: it is written so that
it would read the same way with any of the ten languages in Quidra's place.

| # | Severity | Defect (one line) | What changed | Direction for Quidra |
|---:|---|---|---|---|
| 1 | BLOCKER | In I1/I2/I3/I4 a submission that ignored the transformation and emitted ordinary real source still passed, leaving ~65% of CTES obtainable from pretraining recall — the exact advantage the track exists to remove. | New **§4.2a transformed-source conformance gate**, applied to the raw pre-inverse submission at every turn, with a per-condition pass criterion; gate failure is FAIL for that task at that turn; new `CONFORMANT` check unit in §4.2 makes Test Pass Rate sensitive; §12.3 adds the neutral repair wording; PF-05 must reject a "correct real source ignoring the transformation" negative in every language. Hallucination metrics unchanged and kept as separate diagnostics. | **Mixed by language, unknown in advance.** The gate bites hardest on whichever languages the model can reproduce from memory most fluently. It removes a free pass that was available to all ten and to Quidra as well. |
| 2 | BLOCKER | I4 carried 20% weight but was ~98% insensitive to whether the novel rule was acquired: the overlay verdict was one checklist item inside a 10% metric. | §7.4 makes overlay compliance a precondition of the I4 oracle (via the §4.2a gate) **and** replaces the single item with a rate — *Overlay Compliance = compliant sites / applicable sites* — with the I4 checklist frozen at 50% ordinary items / 50% Overlay Compliance (§11.3.7). Cross-reference corrected: §7.4 now cites §11.3.7, not §11.4. | **Mixed, measurement-determined.** I4 now measures what it is named for, for all ten languages identically. |
| 3 | BLOCKER | The reserved structural marker `@` is unavailable: it begins ordinary Zig builtins (including the frozen stdout path, methodology 00 C-4) and Java/Kotlin/Swift/TypeScript attribute syntax, so PF-10 was unsatisfiable and the three-character maximal-munch rule would silently corrupt source. | `@` excluded a priori. §7.3 freezes a **selection procedure** over the priority list `%%`, `~~`, `¤`; PF-10 is rewritten as marker selection plus reservation, requiring zero occurrences in all ten languages' material and no prefix relation in any of the ten grammars; the selected marker is published and written into all ten lexer profiles; `draw_pair` now returns a bare code and I3's marker is applied separately. | **Unchanged.** Infrastructure correctness; it unblocks a run that would otherwise not start. |
| 4 | BLOCKER | I6's permuted operator pair (strict `<`/`>`) is also the generic-argument delimiter in five of the ten languages, and `lex10` is a lexer, so five languages absorbed a large extra burden that five — including Quidra — escaped entirely. | §7.7 permutes the **non-strict** `<=`/`>=` pair instead, with the reason stated in language-neutral terms; PF-09 extended to verify the chosen pair is a comparison operator in all ten binding tables and occurs in no delimiter position. | **LOWER for Quidra.** This was the clearest Quidra-favouring asymmetry in the document: Quidra was one of the five languages that escaped the burden entirely. Removing it removes Quidra's relative advantage in I6. |
| 5 | BLOCKER | A1/B1/Rule C demanded a sigil on "every function", with no exemption for a toolchain-fixed entry point — unbuildable for the languages whose entry-point name is fixed, free for those that need no named entry point. | §7.4 and §7.6 add the frozen exemption sentence (entry point and any toolchain-fixed signature), in identical wording in P11 of all ten packs, applied mechanically from new `entry_point_name` / `fixed_signature_names` fields in the binding table (§3.4); PF-06 verifies the sentence is present in all ten packs. | **Unchanged / slightly lower.** It removes an unstated convention the model would otherwise have had to guess, in the languages that have a fixed entry point. Quidra is affected the same way as any language with a fixed entry point. |
| 6 | BLOCKER | §5.5's emission rule inserted a space before a bound token at column 0, breaking indentation-significant languages; and §6.4 R1's `canon` could collapse spaces but never delete an inserted one, so real source containing a keyword against a delimiter failed R1 — a defect a fixture author could dodge by style choice. | §5.5 adds the frozen `NEWLINE`/`INDENT_RUN`/leading-run exception and requires every insertion to be recorded in the unit position map; the inverse deletes exactly those insertions; §6.4 R1 becomes **plain byte identity** with no canonicalization; §6.2 updated. | **LOWER for Quidra.** Quidra is indentation-significant, so the old rule would have corrupted Quidra source and shown up as a language failure; correcting it removes an unearned penalty — but the same correction also removes the style-dodge that let a broken transformer pass PF-02 undetected for any language. Net effect on Quidra's I1/I2 is to make the transformation actually apply. |
| 7 | MAJOR | The preamble claimed all word keywords were anonymized, but only the 27 frozen K-roles were; every other real keyword survived via a hand-authored, unbounded `unbound_word_tokens` array whose size decided how much familiar surface each language kept. | New **§7.1a**: the I1/I2 domain is (frozen per-language reserved-word list ∩ fixture word tokens), whether or not a token maps to a K-role; non-role reserved words take key `U:<token>` and are documented in P11 in the same form; `unbound_word_tokens` must be empty and a non-empty array blocks the run at PF-03; `anonymized_token_count` becomes mechanically derived. Preamble claim corrected to match. | **Mixed, and not knowable in advance — which is the point.** It raises the I1/I2 load of whichever languages carry the larger reserved surface in their fixtures. Quidra's reserved set is small and largely role-mapped, so Quidra gains fewer new anonymizations than the larger-keyword languages; the correction is made because an arbitrary hand-authored quantity that moves scores is not a measurement, in either direction. |
| 8 | MAJOR | The document was labelled FROZEN while every artifact that determines the numbers (`role_bindings.json`, ten lexer profiles, compliance checklists, task data, leak terms, pack template) was deferred, unhashed and authored by judgement. | New **PF-14** hashes every file under `config/` into `config_manifest.json` before PF-01 and refuses to score if a hash changes; §3.4 makes binding **mechanical and citable** (occurrence in fixtures + normative list or normative reference, with a recorded citation, first-named spelling wins); a frozen reference-solution style rule (explicit type annotations wherever permitted) stops type-inference idioms silently shrinking one language's anonymization load; §4.5 and §10.2 updated; §0.4 layout lists the newly required files. | **Unchanged in expectation, lower in variance.** It removes a degree of freedom larger than any weight in §11.2 — one that could have moved Quidra either way. |
| 9 | MAJOR | §2.6 bullet 3 would have rejected every I1 pack (I1 leaves the library surface real by design), blocking the run; and §13.2 wrongly claimed I1 and I2 both remove the word-level surface. | §2.6 bullet 3 gains the stated design exemption for untransformed V-role spellings and namespace paths, with the exposure recorded as an anonymity residual; §13.2 now states plainly that I1 packs are substantially more recognizable than I2 packs and requires that statement on the I1 row of the §28.1 table. | **Unchanged.** Honesty and realizability; applies to all ten identically. |
| 10 | MAJOR | §2.4/§5.4/§11.3.11–12 specified model-tokenizer counts with a character fallback, contradicting frozen sibling doc 09 (which records that this client does not expose the tokenizer and freezes a lexical counter governing both tracks); and it was undefined whether Source Token Efficiency counted the transformed or the inverse-mapped source. | All token counting moves to the **frozen lexical counter of `09_llm_run_config → source_token_counting`** (SHA-256 recorded), with characters as the second frozen instrument and harness counts published beside but never substituted; §11.3.11 fixes the count to the **inverse-mapped source actually compiled**, with the transformed-source count published as a residual; §5.4 restated; §11.8 row corrected. | **LOWER for Quidra where it matters.** Counting the transformed source would have rewarded whichever language's transformation inserted fewest pseudo-words — a quantity §7.1a shows is a property of the language's reserved surface, and Quidra's is small. Counting the compiled source removes that. |
| 11 | MAJOR | Family-C Source Token Efficiency took a median over each language's own passing-task set and then compared across languages, so a language that passed only the shortest task beat one that passed all five — a score changed by aggregation shape rather than measurement. | §11.3.11–12 and §11.5: **per-task normalization** at the lowest comparable workload level (§25.2) — `best_positive_raw` is the minimum among languages that passed **that task**; the unit score is the unweighted mean over the language's applicable tasks; N/A only if it passed none. Applied to Total Token Efficiency too. | **Mixed, measurement-determined.** It removes a reward for failing more, available to any language. |
| 12 | MAJOR | N-BETA was not mechanically decidable: "loop control variable" and "compile-time constant iteration count" needed dataflow and were ill-posed for languages whose idiomatic loop has no numeric control variable; A2's "assigned a new value" never said whether increment/compound forms counted. | §7.4 freezes both definitions lexically — control variable = the identifier at a `K07` site in the binding table's `loop_var_position`, `K08` loops out of scope; compile-time-constant test stated over literals and literal-only-assigned identifiers; and a frozen **assignment site** definition covering `S7`, the binding table's `assignment_site_forms`, and implicit rebinding of a bounded-iteration control variable in all ten languages. | **Unchanged.** It equalizes reasoning difficulty across loop idioms rather than moving any language's score. |
| 13 | MAJOR | H2–H5 supplied material the pack **documents** (entry-point scaffold, package declaration, imports, the `K27` error-propagation marker — which in I1/I2 is an anonymized token the model was supposed to learn), rescuing model failures, but only in the languages that need scaffolding; PF-13's "extend the fixups until it passes" made this open-ended. | §9.3: **no fixup may supply anything the pack documents**; absence of a documented element is a model failure and the trial builds as submitted. Fixup counts published split into pack-documented (must be zero) and unstated-convention. PF-13's bare-body fixture is now **constructed from the pack**, containing every pack-documented element and omitting only unstated conventions, and must pass with zero pack-documented fixups. | **Mixed by language.** It removes a rescue that was granted by language rather than by rule. Quidra's scaffolding needs are modest, so Quidra benefited less from the rescue and loses less — but the rescue is removed for all ten. |
| 14 | MAJOR | Specification Compliance (10% of every condition) rested on predicates `lex10` cannot decide and the document never defined per language ("a record type", "a nested sequence"). | §11.3.7 binds them to frozen per-language `declaration_predicates` and `nested_sequence_predicate` entries in `role_bindings.json` (§3.4), cited like every other binding; PF-05 must exercise each with a positive and a mutated negative **in every one of the ten languages**. | **Unchanged in expectation, lower in variance.** Reproducibility. |
| 15 | MAJOR | `H_API` — the only hallucination event for I0/I4/I5, 8% weight — was read off "the build diagnostic class", but not all ten build steps resolve names, so a language whose build step is a compile check could essentially never register an event and would score ~100 by construction. | §11.4 adds frozen `config/diagnostic_classes.json` (per-language regexes for `UNRESOLVED_NAME` / `UNKNOWN_MEMBER` over **build and run** output) and redefines `H_API` to include the equivalent **runtime** diagnostic class for languages whose build step does not perform name resolution; PF-05 exercises a positive and a negative per language; residual publishes per-language coverage. | **Mixed.** It takes a free ~100 away from whichever languages' build steps do not resolve names. |
| 16 | MAJOR | I3's budget was matched in dimensions but not in sites, with no residual recording the difference; `S1` was described as call-argument-only though a lexer must perturb every occurrence; compound assignment's membership in `S7` was undefined. | §7.3 adds: SPP matches **dimensions, not occurrences**, with per-language perturbed-site counts and sites-per-100-lexical-tokens density published per set and stated in the narrative; **every occurrence** of a perturbed dimension's token is replaced whatever its syntactic role, in the same P11 sentence for all ten; **compound assignment and increment/decrement are distinct operators, not part of `S7`, never perturbed**; the indentation paragraph now covers a language with no block introducer and states that the frozen fallback then applies to all ten. PF-09 records the density table. | **Unchanged in direction, honest in reporting.** Density differences are published rather than equalized, which is the treatment §2.3 clause 5 already prescribes for length. |
| 17 | MAJOR | The pack template had no slot for value interpolation, which several of the ten languages (Quidra among them) provide; and because §1 never transforms literals, an interpolating language could route `V10`/`V11` through an untransformed literal in I2 and never learn their anonymized spellings. | §2.2 adds a fixed P2 line present in all ten packs; §3.2 adds **`V18`** with transformation status `NOT_TRANSFORMABLE` and the frozen reason; §1 consequence 4 states the converse rule; §13.1 must publish, per language, which bound V-roles that language's interpolation form makes optional in I2, and the I2 narrative must state it. PF-06 checks the line is present in all ten packs. | **LOWER for Quidra.** Quidra interpolates, so this was a real and unrecorded reduction in Quidra's I2 anonymization pressure. It is now documented, published beside the I2 scores, and no longer invisible. It is published rather than equalized because forbidding interpolation would describe those languages unidiomatically. |
| 18 | MAJOR | I5 Rule B referred to a "group" that nothing defined, so the harness knew the intended grouping (frozen from reference solutions) while the model had to guess — a withheld fact under §9.1 and §10.4 requirement 2, under 15% of the Intrinsic score. | Rule B now defines a group as the maximal run of lines **the task statement names as one group**, and §4.4 gives C1, C2 and C3 the verbatim naming sentence; PF-06 verifies no Rule-B task leaves a grouping unstated. | **Unchanged.** It removes a guess that faced all ten languages equally. |
| 19 | MINOR | Scrubbed diagnostics could point at harness-inserted scaffolding the model never wrote, with coordinates stripped — disproportionately in the languages needing the most scaffolding, whose Repair metrics then measured harness noise. | §8.2 step 2: such a diagnostic is **suppressed entirely**, the suppression is logged, and the per-language suppression rate is published beside the wholesale-replacement rate. | **Unchanged.** Removes harness noise from whichever languages need scaffolding. |
| 20 | MINOR | §10.3's five-seed rule appeared to bind I4 (2 seeds/rule set) and I6 (3 mappings), and the two-letter code draws had no collision rule, so two arities could share a sigil. | §7.4 states the reading explicitly (§10.3's five seeds bind the pseudo-word conditions I1/I2, which run 5; I4 and I6 are governed by §10.5's multiple-rule-set / multiple-mapping sentences), notes that replicate counts are identical for all ten so the reading favours nobody, and adds the §5.2 rejection-loop rule for all two-letter codes with redraws recorded as `collision_redraws`. | **Unchanged.** Same replicate counts for all ten languages. |
| 21 | MINOR | §6.2's output caps and the provider/transport-vs-model failure separation were inherited but never restated, leaving an exhausted trial's classification undefined. | §0.3 freezes the 16,384 / 65,536 caps, scores an exhausted trial **FAIL with reason `TOKEN_BUDGET_EXHAUSTED`, never N/A**, publishes the per-language rate, and keeps provider/rate-limit/transport failures in `_deviations.json`, never converted into model failures. §11.8 rows added. | **Unchanged.** Closes an N/A escape (§26). |
| 22 | MINOR | Unseen-case Generalization is one binary observation per replicate, quantized to 20-point steps, and its task also feeds Correct@1, Correct@N and Test Pass Rate; §2.4's pack floor had no defined consequence. | §13.4 states the quantization and the concentration explicitly and forbids describing sub-step differences as resolved; §2.4 states that a pack below the floor is **never padded** and that the floor is a PF-06 slot-fill review trigger. A second unseen-case task was **not** added, because adding one would change the frozen task set and generation budget after the audit; the limitation is stated instead, which is the finding's own alternative. | **Unchanged.** Reporting honesty. |
| SR-1 | SELF-REVIEW (blocker-class) | **Not itemised in the brief.** Silent Bug Resistance is 10% of every condition and is defined as "built cleanly, ran cleanly, wrong output" — a verdict controlled directly by build flags. The frozen performance recipes build two languages with their runtime safety checks **disabled** (`rustc -O` drops `debug_assertions` and overflow checks; `zig build-exe -OReleaseFast` drops bounds, overflow, alignment and optional/error-union safety) while Quidra's `quidra build` is overflow-checked by default (methodology 00, C-1). A defect that traps loudly in Quidra therefore becomes a silent wrong answer in those two, at the flag's discretion rather than the language's. | New **§0.1a**: for this track every language is built in its **checks-enabled** configuration and no language is built with a check disabled that its own toolchain performs by default. Rust becomes `rustc -O -C debug-assertions=on -C overflow-checks=on`; Zig becomes `-OReleaseSafe`; `-Ounchecked`, bounds-check elimination and equivalents are prohibited. Checks a toolchain does not have are **not** invented: C++ performs no bounds or overflow checking at any level, that is a real and permanent property of C++, and it is published rather than compensated for (spec §7). The per-language `runtime_check_inventory` is published as a residual and recorded at PF-01. | **LOWER for Quidra.** Quidra's always-on checking was being scored against two competitors built with their checking switched off, on a 10%-weight metric. Removing that comparison removes a Quidra advantage that the build flags, not the languages, had created. |
| SR-2 | SELF-REVIEW (minor) | §4.1 justified excluding associative containers by "it would force a substantially heavier vocabulary slot on some languages than others" — a burden-equalization argument the same document rejects in §2.3 clause 5 ("length differences are a property of the language, not a favor"). | §4.1's rationale restated in neutral, non-equalizing terms: the frozen V-role inventory contains no associative-container role, the limitation was fixed before any binding table was written, all ten languages have the facility, and the exclusion is published as a **stated scope limitation** in §13.3 rather than defended as fairness. | **Unchanged.** Consistency between two sections of this document. |

### Counterpart obligations for sibling frozen documents

| Document | Change required there |
|---|---|
| `environment.json` (`frozen_toolchain_recipes`) and `00_cross_language_constraints.md` | Record the **intrinsic-track** build recipes of §0.1a alongside the existing performance recipes — Rust `rustc -O -C debug-assertions=on -C overflow-checks=on`, Zig `zig build-exe -OReleaseSafe` — and state that the performance tracks continue to use the release recipes unchanged, so the two sets are never confused. Also record the prohibition on `-Ounchecked`, bounds-check elimination and equivalents for the intrinsic track. |
| `09_llm_run_config.json` | Confirm that `source_token_counting` (the frozen lexical counter, one implementation, SHA-256 recorded) governs Primary Evaluation 3 as it governs the other tracks, and that harness-reported prompt/completion counts are published beside it and never substituted for it. This document now depends on that section by name (§2.4, §5.4, §11.3.11–12) instead of on a model tokenizer this client does not expose. |
| `09_llm_run_config.json` | Add the frozen per-condition neutral sentence list that §12.3 draws on when the §4.2a conformance gate fails, so the repair prompt can name the violated rule without revealing a real spelling; the list must be identical across the ten languages. |
| `00_cross_language_constraints.md` | Record that the structural marker for Primary Evaluation 3 is selected at PF-10 from the frozen priority list `%%`, `~~`, `¤`, and that `@` is excluded a priori because of Zig builtins (C-4's stdout path among them) and Java/Kotlin/Swift/TypeScript attribute syntax. |
| Results / §28.1 table owner | Carry the explicit recognizability statements this document now requires: on the I1 row (library surface real, packs substantially more recognizable than I2), and on the I4 and I5 rows (real surface entirely). Carry also the I2 interpolation residual (§13.1) and the I3 density residual (§13.3). |
