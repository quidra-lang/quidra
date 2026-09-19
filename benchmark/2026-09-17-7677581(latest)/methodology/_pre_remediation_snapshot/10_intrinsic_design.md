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
built to make it score well, and several of its choices (for example anonymizing *all* word
keywords a language uses rather than a fixed quota — §7.1) cut against whichever language happens
to have the larger keyword surface, without knowing in advance which that is.

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
  anonymity_labels.json        sealed language -> "Language A".."Language J" bijections (§8.1)
transforms/
  <unit>/manifest.json         transformation manifest (§6.3), one per replicate unit
  <unit>/<language_id>/...     forward+inverse artifacts, per language
reference_packs/
  <unit>/<language_id>.md      the Intrinsic Reference Pack actually shown to the model
  token_counts.json            published token/character counts for every pack (§2.5)
preflight/
  PF-01/ .. PF-13/             per-check evidence directories (§10)
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
| P2 | `Lexical rules` | Identifier form, literal forms, comment form, whitespace/indentation significance, case sensitivity. |
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

Measured in **model-tokenizer tokens** of the exact packaged text, using the tokenizer of the
frozen benchmark model (recorded in `config/run_config.json`). If the provider does not expose a
tokenizer, the budget is measured in **characters** with the same structure, and the substitution
is recorded as a published residual (§13).

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

### 2.5 Mandatory publication

`reference_packs/token_counts.json` must contain, for every (replicate unit, language):

- token count of P1–P11, of P12, and of the whole pack;
- character count of the same three;
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
  identifies the ecosystem) that is not itself a transformed token;
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

`V17` exists so that a language whose current standard output or dynamic-sequence API requires an
explicit context object is described in its pack on the same footing as one that does not, rather
than that requirement being smuggled in as unexplained boilerplate.

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
    "K04": { "status": "BOUND", "tokens": ["<exact source token>"], "notes": "" },
    "K06": { "status": "NOT_LEXICALIZED", "reason": "expressed as K05 followed by K04" },
    "V11": { "status": "NOT_LEXICALIZED", "reason": "expressed by an operator, not a name" }
  }
}
```

Rules, frozen:

- `status` is exactly one of `BOUND`, `NOT_LEXICALIZED`, `NOT_WORD_TOKEN`.
- `NOT_WORD_TOKEN` means the role exists but is realized as punctuation; it is excluded from I1/I2
  lexicalization and, if it is one of the eight S-dimensions, is handled by I3 instead.
- A role may bind more than one token only when the language genuinely requires distinct spellings
  in distinct positions (e.g. two forms of a declaration marker). Each bound token receives its own
  pseudo-word; the role's pseudo-words are distinct from each other.
- The binding tables are authored **from each language's own reference documentation and verified
  against the reference solutions**, never by inspecting Quidra first and matching the others to it.
  Authoring order is the fixed column order of §0.1, which begins with Quidra only because that is
  the frozen table order; the inventory in §3.1–3.3 was fixed before any binding was written.
- Binding-table completeness is verified mechanically at PF-03: every word-shaped token appearing
  in a language's reference solutions and in its pack examples is either a bound K/V role token, a
  user identifier declared in the same file, or is listed in that language's
  `unbound_word_tokens` array with a reason. An unexplained unbound word token blocks the run.

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
- **No associative container is required.** Requiring one would force a substantially heavier
  vocabulary slot on some languages than others. Every task is solvable with sequences.
- **No reflection, no name-based lookup, no printing of identifier names.** This is what makes the
  I4 and I5 identifier-spelling rules provably semantics-neutral (§7.5).
- **Determinism.** Every task has exactly one correct output, byte for byte.

### 4.2 The oracle (identical for every task, every language, every condition)

A submission `PASSES` a task if and only if all of the following hold:

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
- one unit `EXIT_ZERO`.

Test Pass Rate for a trial = passed units / total units. If the program does not build, every unit
fails (rate 0). If it times out, every unit fails.

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
  computed by calling a user-defined function of one parameter.
- **C2 requires B + C.** Base program: for each `n` from 1 to 6, write `n` followed by the count of
  its positive divisors, using a user-defined function of one parameter.
- **C3 requires A + B + C.** Base program: for each of 5 supplied words, write the word followed by
  a value computed by a user-defined function of two parameters, which itself calls a user-defined
  function of one parameter.

Expected outputs are frozen from the reference solutions at PF-11, with rules A/B/C applied. Check
units as in §4.2.

### 4.5 Reference solutions

For every task and every one of the ten languages there is a **reference solution** in
`config/tasks/<task>/reference/<language_id>.<ext>`. Reference solutions:

- use only constructs documented in P1–P10 of that language's pack;
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

The space is `14*5*14*5*14*5 = 343,000` words, far larger than the ~45 tokens any single mapping
needs, so rejection sampling terminates comfortably.

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
`n`-th token when it binds several. Each role draws from its **own** FNV-seeded stream, so the
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

- every pseudo-word and its model-tokenizer token count (when the tokenizer is available);
- the mean, sd, min and max token count per pseudo-word, per language;
- the cross-language `max/min` ratio of mean tokens per pseudo-word.

Because all pseudo-words are drawn from the *same* generator with the *same* length for every
language, the expected tokenization is identical across languages by construction; the realized
per-language means will still differ slightly. **That residual is published, never corrected**
(§10.3: "if exact tokenizer matching is impossible, publish the residual token-count differences").
If the tokenizer is unavailable, `token_counts_available: false` is recorded, character counts are
used, and the substitution appears in `raw/residuals.json`.

### 5.5 Emission rule (prevents accidental token merging)

The forward transformer emits every substituted word token **surrounded by the whitespace that was
already there, and if there was none, by a single space**. It never places a pseudo-word
immediately adjacent to another word character. This makes token merging (`@ka` + `foo`, or
`pseudo`+`identifier`) impossible and is verified at PF-02.

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
  "reserved_marker_char": "@"
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
4. for I3: replace each `OP` (or block-introducer `WORD`) token belonging to a selected
   S-dimension with that dimension's `@xy` token, always surrounded by single spaces;
   preserve `INDENT_RUN` tokens verbatim;
5. for I6: replace each `WORD` token in the permuted role subset with the **real token of the role
   π maps it to** (§7.7), and the one permuted operator pair likewise;
6. re-emit.

Inverse is the same machinery run with the **transformed profile** (identifier pattern extended to
recognize `@xy` structural tokens as operators, comment and string rules unchanged) and the inverse
mapping. String, char, number and comment tokens are never touched in either direction.

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

- **R1 token identity.** `canon(inverse(forward(F)))` is byte-identical to `canon(F_nocomments)`,
  where `canon` collapses runs of spaces outside string/char literals to one space, strips trailing
  whitespace on each line, and preserves leading indentation verbatim.
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

**Transformation.** For each language, every `BOUND` K-role token (§3.1) is replaced by a PW-1
pseudo-word under the seed's mapping. V-roles are **not** transformed: standard-library and builtin
names keep their real spellings in I1. Ordinary user identifiers are not renamed (§10.5 I1).

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

**Replacement tokens.** Every structural replacement token has the form `@` + consonant + vowel
(e.g. `@ka`), generated by:

```
draw_pair(state):
    state = next(state); c = CONS[pick(state, 14)]
    state = next(state); v = VOW [pick(state, 5)]
    return ("@" + c + v, state)
```

keyed by `seed_state("I3|" + set_id + "|" + dimension_id + "|" + side)` where `side` is `open`,
`close`, or `single`. **The key contains no `language_id`**: the same dimension is spelled the same
way in all ten languages within a set, which is the strongest available form of matched treatment.

The character `@` is **reserved**: PF-10 verifies that `@` appears nowhere in any task statement,
reference solution, expected output, pack example, or user identifier in any language. Because `@`
can start nothing else, and every structural token is exactly three characters, structural tokens
are unambiguous under maximal munch and none is a prefix of another. The transformed lexer profile
declares the rule "a structural token is `@` followed by exactly two lowercase letters, terminating
after the second letter", so `@kafoo` lexes as `@ka` then `foo`.

**Indentation.** For a language whose block structure is indentation-significant, perturbing `S2`
replaces the block-introducer token and **leaves indentation untouched**. Indentation is not itself
an S-dimension: no language could express the change reversibly without ambiguity.

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
(c) assignment sites; (d) call sites (a `WORD` immediately followed by the grouping-open token).
All four are obtainable lexically given the binding table. The checker's output is the
**Overlay Compliance** input to Specification Compliance (§11.4) and is exercised both ways at
PF-05.

#### Rule set N-ALPHA (published in full)

Let `A(seed)` be five distinct two-letter codes drawn by `draw_pair` without the `@` prefix, keyed
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

#### Rule set N-BETA (published in full)

Let `F(seed)` be the two-letter code keyed by `seed_state("I4|BETA|" + decimal(seed) + "|fanout")`,
and `Lf(seed)`, `Lv(seed)` the codes keyed by `...|loopfix` and `...|loopvar`.

- **B1 (fan-out sigil).** Every function the program defines must have a name ending in `_`
  followed by `F(seed)` followed by the decimal count of **distinct functions defined in this
  program that its body calls** (0, 1, 2, …). Calls to standard-library facilities do not count.
- **B2 (loop-variable kind).** Every loop control variable whose iteration count is a compile-time
  constant of the program must have a name ending in `_` + `Lf(seed)`; every loop control variable
  whose iteration count depends on a value computed at run time must end in `_` + `Lv(seed)`.

**Replicates.** 2 rule sets × 2 seeds (4001, 4002) = 4 replicate units; score = mean over the four.
Both rule sets and all four realized sigil alphabets are published in
`transforms/I4/*/manifest.json` and reproduced in the results appendix (§10.5 I4: "Publish every
rule set").

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
  summary line itself.
- **Rule C — arity sigil.** Every function the program defines has a name ending in `_` followed by
  the seed's code for its parameter count, using the same alphabet construction as N-ALPHA/A1 but
  keyed by `seed_state("I5|" + decimal(seed) + "|arity" + k)`.

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

plus **exactly one operator pair**: the strict-less-than and strict-greater-than comparison
operators are exchanged with each other. They share a precedence class and an arity, so the
exchange creates no parsing ambiguity. No other operator is permuted: exchanging operators of
different precedence would make the transformed grammar ambiguous to describe in the pack's budget
and would risk violating §10.4.

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
   records for that trial. A diagnostic whose coordinates cannot be mapped is emitted without
   coordinates and the event is logged.
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

**Per-language fixup counts are published** (`raw/residuals.json`). A language needing many fixups is
not penalized — that is the entire point — but a reader can see how much scaffolding the harness
supplied on each language's behalf. PF-04 is the test that this actually works.

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
| **PF-01** | **Fixtures build and run, output exact** (§10.4 requirement 1) | Build and run every reference solution and every pack example E1–E6 (each wrapped by the frozen example harness) for all ten languages with the frozen recipes. | 100% build success, exit status 0, stdout byte-identical to the frozen expected output. Any failure blocks the run: a fixture that does not build teaches every trial in that language to reproduce something that cannot build. |
| **PF-02** | **Round-trip token identity** | For every fixture × every replicate unit: check R1 and R5 of §6.4. | All pass. Diffs are stored. |
| **PF-03** | **Binding-table completeness** | For every language, enumerate every `WORD` token in its reference solutions and pack examples; classify as bound role token, locally declared identifier, or listed exception. | Zero unexplained word tokens. Also: no user identifier equals any bound role token in any language. |
| **PF-04** | **Inverse-mapped fixtures build and run** (§10.4 invariant) | For every fixture × unit: build and run `inverse(forward(F))` with the real toolchain; compare to `F`'s output. Also check R4 non-triviality. | R2, R3, R4 all pass for all ten languages and all units. |
| **PF-05** | **Every validator can both pass and reject** (§10.4 requirement 3) | For each validator — the oracle, the check-unit splitter, the spec-compliance checker, the I4 overlay checker, the I5 rule checker, the hallucination detector (per condition), the silent-bug detector, the I6 role-position auditor, the code extractor, the `INVERSE_AMBIGUOUS` detector — run ≥1 positive fixture that must pass and ≥1 **mutated** negative fixture that must be rejected. Mutations are recorded. | Every validator passes its positive and rejects its negative, for every language. **Explicit additional requirement:** for I6 and for any condition whose mapping is a permutation of the language's own vocabulary, the negative fixture must be an `F1_PRIOR` program — valid real source containing only real tokens — and the validator must reject it. A "did any transformed token survive?" test is vacuous here by construction and does not satisfy PF-05. |
| **PF-06** | **Pack slot conformance and minimality** | For every pack: verify the twelve sections are present in order with the template's sentence skeleton; verify exactly six examples in slots E1–E6 (XA/XB/XC for I5); verify each example demonstrates its slot and nothing outside it; verify no example contains a held-out I5 combination; record token and character counts. | Conformant; counts published; whole-pack soft cap exceeded by ≤30% for every language, or the slot list is revised for all ten before any trial. |
| **PF-07** | **Identity-leak scan of packs and prompts** | Run the §2.6 scan over every pack, every task statement, and every system/user prompt template for I1, I2, I3, I6. | Zero hits. |
| **PF-08** | **Lexicalizer self-test** | Regenerate every mapping from its seed twice in separate processes; verify determinism, one-to-one-ness, no collisions, no prefix relation, exact length 6, `^[a-z]{6}$`, and that every rejection filter fires at least once across the run. Record the three word-list SHA-256 values. | All pass; determinism byte-exact; SHA-256 recorded. |
| **PF-09** | **I3 budget realizability and matching** | Compute the eligible S-dimension set as the intersection across all ten languages; verify each frozen set's five dimensions are eligible, applying the frozen fallback order if not; verify each language receives exactly the same SPP count. | All ten receive an identical SPP count; the realized dimension list per set is published. |
| **PF-10** | **Reserved-character check** | Scan every task statement, reference solution, expected output, pack example and identifier in all ten languages for `@`. | Zero occurrences. |
| **PF-11** | **Expected-output determinism** | Run all ten reference solutions per task and byte-compare their outputs to each other and to the frozen expected file. Freeze the check-unit decomposition. | All ten identical; check-unit lists frozen. |
| **PF-12** | **Diagnostic scrubber validation** | For each language, induce a build failure and a runtime failure in a fixture; scrub the diagnostics; scan for leak terms; verify coordinate mapping against a known-position defect; verify the wholesale-replacement fallback fires when forced. | Zero leak terms in any scrubbed diagnostic; coordinates map correctly; fallback demonstrated. |
| **PF-13** | **Harness-convention sufficiency** (§10.4 requirement 2) | For each language, construct a **bare-body submission** that omits every convention the prompt does not state — no file name, no package declaration, no entry-point scaffold, no error-propagation marker — but is otherwise a correct solution to T1 using only pack-documented constructs. Feed it through the real pipeline: extraction → inverse → fixups → build → run → oracle. | The bare-body submission **passes** for all ten languages. If it does not, the fixup set is extended until it does, before any trial is scored. This is the concrete test that the harness, not the model, carries the withheld conventions. |

### 10.2 Evidence preservation

Each check writes to `preflight/PF-NN/`:

- `commands.txt` — every command executed, verbatim, in order;
- `stdout/`, `stderr/` — captured output per command;
- `artifacts/` — sources, transformed sources, inverse-mapped sources, binaries or emitted files
  (binaries may be replaced by their SHA-256 when size is prohibitive; the SHA-256 is mandatory);
- `result.json` — `{check_id, pass, per_language: {...}, notes, timestamp}`.

`preflight/preflight_report.json` aggregates all thirteen and carries the gate field
`"all_pass": true|false`. **The scoring pipeline refuses to run while `all_pass` is false.** The
report is preserved with the run; it is part of the deliverable, not scaffolding.

Pre-flight is re-run in full whenever any of the following change: a lexer profile, a binding table,
a transformer, a task statement, a reference solution, a word list, a pack template, or the fixup
set. Partial re-validation is not permitted.

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
   **final** state (after any repairs).
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
8. **Hallucination Resistance** = `1 − (trials with ≥1 hallucination event) / (trials)` (family B).
   Events are defined per condition in §11.4.
9. **Silent Bug Resistance** = `1 − (silent-bug trials) / (trials)` (family B). A trial is a
   **silent bug** iff its final program builds with exit status 0, runs to completion with exit
   status 0, emits nothing on standard error beyond the allow-list, **and** fails the oracle. That
   is: wrong output, no signal. A trial that crashes, times out, or emits a diagnostic is a failure
   but not a silent bug.
10. **Unseen-case Generalization** = pass rate on the unit's designated unseen-case task — T5 for
    `CORE` and `OVERLAY`, C3 for `COMPOSITION` — evaluated at repair turn 0.
11. **Source Token Efficiency** — raw = median across the unit's *passing* tasks of the token count
    of the final accepted source, counted with the model tokenizer (characters if unavailable).
    Family C, lower is better, `best_positive_raw` = the minimum across the ten languages **within
    the same (condition, replicate index)**. If a language has no passing task in the unit, the
    metric is N/A for that unit with reason `NO_PASSING_SOURCE` and its weight is redistributed.
12. **Total Token Efficiency** — raw = median across the unit's tasks of (prompt tokens + completion
    tokens summed over the initial turn and all repair turns). Family C, same normalization and the
    same cross-language cell definition. Never N/A (every trial consumes tokens).

Family-C publication requirement: whenever the applicable raw values for metric 11 or 12 span a
factor of 100 or more across the ten languages, publish the raw values, the ratios
`raw_i / best_positive_raw`, and the §25.1 note that the normalized score is compressed and the
ratios carry the comparison (§25.1, "Known property of family C").

### 11.4 Hallucination events, per condition

| Condition | Event type | Definition |
|---|---|---|
| I1, I2 | `H_REAL` | The submission contains, at a grammar-significant position, a **real** token of a role that the mapping anonymized. The model fell back to remembered vocabulary. |
| I1, I2 | `H_INVENT` | The submission contains a word token that is neither a mapped pseudo-word, nor a `WORD` declared as an identifier within the submission, nor a real token of an untransformed role. |
| I3 | `H_STRUCT_REAL` | The submission uses an original token of a perturbed S-dimension. |
| I3 | `H_STRUCT_INVENT` | The submission uses an `@xy` token not in the set's manifest. |
| I6 | `H_PRIOR` | A permuted token is used in its **prior** role rather than its π-assigned role. Detected by the §7.7 F1/F2 procedure — the role-position audit, **not** a token-presence test, which is vacuous here. |
| I0, I4, I5 | `H_API` | The submission calls a standard-library name that does not exist in the language, or uses a construct the pack does not document and the language does not provide. Determined from the build diagnostic class (unresolved name / unknown member), not by judgement. |

A trial counts once no matter how many events it contains. Event counts are additionally published
per language for diagnosis.

### 11.5 Normalization and CTES

Per §25.1, applied at the (condition, replicate index) level across the ten languages:

- families A and B: `Score = 100 * clamp(x, 0, 1)` and `100 * (1 - clamp(x, 0, 1))`;
- family E for Repair Efficiency, as in §11.3.6;
- family C for metrics 11 and 12: `Score_i = 100 * best_positive_raw / raw_i`. Raw token counts are
  strictly positive, so no epsilon shift is needed; `epsilon` is therefore not defined for these
  metrics and the unshifted form is used.

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
| Tokenizer unavailable | Not an N/A: character counts substitute, and the substitution is published as a residual. |

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
- Per-language `anonymized_token_count` for I1 and I2 (the number of tokens each language had to
  have anonymized). Not equalized, by the reasoning in §7.1.
- Per-language mean/sd model-tokenizer token count per pseudo-word, and the cross-language max/min
  ratio. Character length is matched exactly (always 6); tokenization is matched only in
  expectation.
- Number of collision redraws per language and seed.

### 13.2 Anonymity residuals
- I1 and I2 remove the word-level surface but not the structural shape; a reader who knows the
  languages can often still recognize one from its shape. §10.1's claim — a familiarity-controlled,
  specification-grounded evaluation, not a proof of zero prior exposure — is the claim made.
- I3 keeps real keywords and I6 keeps the language's own vocabulary, so both remain recognizable.
  The model is never told the name, which is what §10.1 requires, but recognizability is not
  removed and the results must say so.
- **I4 and I5 keep the real surface entirely** (§10.1). The model is never told the name but can
  recognize the language. **The I4 and I5 rows of the §28.1 table must carry this statement
  explicitly, not by implication.**
- Per-language rate of wholesale diagnostic replacement by the scrubber (§8.2).

### 13.3 Structural and rule residuals
- The realized S-dimension list per transformation set, and confirmation that all ten languages
  received an identical SPP count (§7.3).
- For I4/N-ALPHA rule A2: languages that already distinguish mutable from immutable bindings supply
  a cue the others do not. Published, not engineered away; N-BETA's rules have no analogous cue,
  and I4's score is the mean over both rule sets.
- Per-language fixup counts (§9.3) and `FIXUP_INSUFFICIENT` / `INVERSE_AMBIGUOUS` rates.

### 13.4 Statistical residuals
- One trial per cell (§0.3). Differences of a few points between languages are not resolved by this
  measurement and must not be described as if they were. Replication in this track is across seeds
  and transformation sets, and the per-subtest sd, min and max are published for exactly this
  reason.
- Where deterministic decoding makes repeated generations byte-identical, all trials are preserved
  and the duplication rate is reported; no ad-hoc prompt noise is added (§6.2).

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
