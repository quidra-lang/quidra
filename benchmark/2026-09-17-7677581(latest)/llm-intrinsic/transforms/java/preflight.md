# MANDATORY PRE-FLIGHT VALIDATION — I1 / Java / seed 20260918

Implements methodology `10_intrinsic_design.md` §10.4 (checks 1–3), §5 (controlled random
lexicalization), §6.4 (round-trip invariant R1–R5), §4.2a (conformance gate), §9 (harness
conventions), and frozen rule **C-4** of `00_cross_language_constraints.md`.

**Gate result: `all_pass = true`.** Full evidence run: `preflight/full_run.log`
(reproduce with `./run_preflight.sh`; it exits non-zero when any check fails).
Per-command stdout/stderr: `preflight/commands.txt`, `preflight/pf_*.out|.err`.
Artifact SHA-256 values: `preflight/hashes.txt`.

Toolchain, as executed:

```
$ javac -version
javac 26.0.1
$ java -version
openjdk version "26.0.1" 2026-04-21
OpenJDK Runtime Environment Homebrew (build 26.0.1)
OpenJDK 64-Bit Server VM Homebrew (build 26.0.1, mixed mode, sharing)
```

`JAVA_HOME=/opt/homebrew/opt/openjdk` (methodology 00, C-3: native arm64 JDK, not the
Rosetta JRE first on PATH).

---

## Check (a) — the fixture compiles, runs, and its output matches the pack EXACTLY

`packcheck.py` compares the Reference Pack's P12 worked example against the fixture files
and then executes it through the real toolchain. All six sub-checks passed:

```
$ python3 packcheck.py
  a1 pack P12 example is byte-identical to fixture_anon.java : True
  a2 pack's claimed output is byte-identical to expected_output.txt : True
  a3 pack P12 example passes the I1 conformance gate : True []
  a4 inverse(pack P12 example) is byte-identical to fixture_real.java : True
  a5 it builds : True   exit 0 on run : True   stderr : ''
  a6 its stdout is byte-identical to the pack's claim : True
     produced:
       | SUM 25632
       | MAX 935
       | EVENS 24
       | JOINED 897-558-614-577-405

PACK CHECK PASSED
```

`expected_output.txt` was produced **by execution**, never by hand:

```
$ javac -d out Main.java && java -cp out Main > expected_output.txt
$ cat expected_output.txt
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
$ tail -c 40 expected_output.txt | od -c
...   5   7   7   -   4   0   5  \n
```

The file ends with a newline after the fourth line and contains nothing else.
An **independent** re-derivation of the oracle in a second language agreed digit for
digit before the file was accepted:

```
$ python3 -c 's=7;t=[]
for _ in range(50):
    s=(s*48271)%2147483647; t.append(s%1000)
print(sum(t), max(t), sum(1 for x in t if x%2==0), "-".join(map(str,t[:5])))'
25632 935 24 897-558-614-577-405
```

Exit status of the fixture is 0 (`run_exit: 0` in `preflight/pf_b1.out`), and its stderr
is empty.

---

## Check (b) — every withheld harness convention is satisfied BY THE HARNESS

### (b.1) What the pack states, and what the harness supplies

§9.1 divides the facts: the Reference Pack states everything about the *language*; it
states nothing about this benchmark's file system and invocation, because those facts
identify the toolchain. The harness carries the latter. The split, as actually
implemented in `harness.py`:

| Fact | Who supplies it | Where |
|---|---|---|
| Enclosing type shape and its name `Main` | **Pack**, §P1 | `reference_pack.md` P1 |
| Entry-point shape `tenuro zobolu sadora main(String[] args)` | **Pack**, §P1 | `reference_pack.md` P1 |
| Output spelling `System.out.println(...)` | **Pack**, §P8/§P10 | `reference_pack.md` P8, P10 |
| Module/namespace declaration (none needed) | **Pack**, §P9 | `reference_pack.md` P9 |
| **Entry file name `Main.java`** | **Harness** (fixup H1) | `harness.py: ENTRY_FILENAME` |
| **Build command** `javac -d OUT Main.java` | **Harness** | `harness.py: BUILD_CMD` |
| **Run command** `java -cp OUT Main` | **Harness** | `harness.py: RUN_CMD` |
| **Output directory** `OUT` | **Harness** (fixup H6) | `harness.py: OUT_DIR_NAME` |

The trial is never asked to name a file, choose an extension, or state a build command,
and is therefore never penalised for not guessing them. It is also never *rescued* for
material the pack does state: §9.3's "no fixup may supply anything the pack documents"
holds trivially here, because the only fixups implemented are H1 and H6, both of which
touch file-system facts only. **Pack-documented fixups applied: 0.**

Note on anonymity: the file name is supplied *by the harness writing the file*, not by
telling the trial the string `Main.java`, because the extension would identify the
language and breach §2.6. The trial is told only that the harness writes, builds and runs
its source, and that the enclosing type must be named `Main` (which the pack states).
This satisfies the substance of the requirement — the trial cannot lose a point for a
convention it was not given — without re-opening the identity leak the condition exists
to close.

### (b.2) Demonstrated: a submission written from the pack ALONE passes end to end

`pipeline/bare_body_submission.md` is a solution written purely against the Reference
Pack. It is deliberately **not** the fixture: different identifier names, a `bilite
mepone` chain instead of a nested conditional, and `"" + v` instead of the fixture's
first-term branch. It contains every element the pack documents and omits only the
unstated conventions. Fed through the real pipeline
(extraction → gate → inverse → fixups → build → run → oracle):

```
$ python3 harness.py pipeline/bare_body_submission.md --expected expected_output.txt
extracted_chars:         950
gate_conformant:         True
gate_events:             []
inverse_substitutions:   19
build_cmd:               .../javac -J-Duser.language=en -J-Duser.country=US -d <TMP>/out <TMP>/Main.java
build_exit:              0
build_stderr:
run_cmd:                 .../java -cp <TMP>/out Main
run_exit:                0
stdout:
    | SUM 25632
    | MAX 935
    | EVENS 24
    | JOINED 897-558-614-577-405
stderr:
stdout_byte_identical:   True
verdict:                 PASS
```

### (b.3) Frozen rule C-4 — current stdout API vs. the widely-reproduced older idiom

C-4 names two toolchains whose current stdout API differs from the older idiom models
reproduce from memory (TypeScript 7's removal of `--outFile`; Zig 0.16's explicit `Io`
instance). **This language is not one of them.** Its standard-output call has had the same
spelling since its first release and is unchanged on JDK 26.0.1; there is no newer
spelling a trial could be penalised for not knowing, and no older spelling that would
fail to compile. Verified by executing the pack's own P10 spelling on the frozen
toolchain (check (a), a5/a6 above): it builds and runs with zero diagnostics.

C-4's *obligation* is nonetheless discharged rather than skipped: the pack states the
current spelling explicitly, in P8 and P10, with the sentence "the spelling above is the
current one: write it exactly". A trial therefore never has to recall it. Recorded
residual: `c4_api_drift_applies = false` for this language, with the reason above.

---

## Check (c) — the round-trip validator can BOTH accept a correct input AND reject a corrupted one

A validator that cannot fail measures nothing, so every validator here is exercised in
both directions. `validate.py` run with no arguments executes the whole battery and exits
non-zero if any case behaves the wrong way. Verbatim output
(`preflight/pf_c.out`):

```
========================================================================
POSITIVE CASES (must pass)
========================================================================
  P1 round_trip(fixture_real.java)  R1_byte_identity         PASS
  P1 round_trip(fixture_real.java)  R2_build_identity        PASS
  P1 round_trip(fixture_real.java)  R3_behavioral_identity   PASS
  P1 round_trip(fixture_real.java)  R4_non_triviality        PASS
  P1 round_trip(fixture_real.java)  R5_domain_containment    PASS
  P2 gate(fixture_anon.java)            conformant=True events=[]  PASS
  P3 gate(source whose STRING literal contains real keywords) conformant=True  PASS
  P4 forward() left the string literal untouched: True  PASS

========================================================================
NEGATIVE CASES (must be REJECTED)
========================================================================
  N1 gate(correct REAL source, transformation ignored)
     conformant=False  H_REAL events=['class', 'else', 'for', 'if', 'int', 'long',
                                      'public', 'static', 'void']
     -> REJECTED (PASS)
  N2 round_trip on a CORRUPTED transformed source (one letter changed)
     R1 byte identity vs real source: False
     inverse image builds: False   diagnostic: Main.java:12: error: ';' expected
     -> REJECTED (PASS)
  N3 gate(transformed source with ONE real keyword written from memory)
     conformant=False  H_REAL events=['if']
     -> REJECTED (PASS)
  N4 INVERSE_AMBIGUOUS: a variable named with a pseudo-word
     detector hits=2  inverse image builds=False
     diagnostic: Main.java:3: error: not a statement
     -> REJECTED (PASS)
  N5 round_trip with a MUTATED mapping (one token left unmapped)
     transformed source still contains the real token 'if': True
     -> REJECTED (PASS)

========================================================================
LEXICALIZER FILTER SELF-TEST (each accept() rule must be able to fire)
========================================================================
  filter 1 shape            candidate=toolong  accepted=False fired=['1']  PASS
  filter 2 used             candidate=mepone   accepted=False fired=['2']  PASS
  filter 3 en_common        candidate=animal   accepted=False fired=['3']  PASS
  filter 4 prog_terms       candidate=buffer   accepted=False fired=['4']  PASS
  filter 5 reserved_union   candidate=typeof   accepted=False fired=['5']  PASS
  filter 6 substring        candidate=zitemu   accepted=False fired=['6']  PASS
  filter 7 material         candidate=rintln   accepted=False fired=['7']  PASS

========================================================================
BATTERY PASSED: every positive case passed and every negative case was rejected.
```

The mutated negative fixtures are preserved: `neg/corrupt_pseudoword.java` (N2),
`neg/keyword_leak.java` (N3), `neg/inverse_ambiguous.java` (N4),
`neg/lit_in_string.java` (P3/P4). Each is a one-token mutation of a file this directory
also holds in its correct form, so the mutation is inspectable by diff.

### Why these negatives are not vacuous

§10.4 warns that a "did any transformed token survive?" test is **vacuous when the mapping
is a permutation of the language's own vocabulary** — the I6 case, where the transformed
alphabet *is* the real alphabet. That warning is why the battery does not rest on a
survival test:

- This mapping is **not** a permutation of the language's own vocabulary. Every image is a
  novel CVCVCV pseudo-word drawn from a 343,000-word space and filtered against the
  reserved-word lists of all ten languages, so no image can coincide with any real token.
  A survival test would therefore be *trivially* satisfiable rather than vacuous — which
  is a different failure mode, and equally uninformative.
- N1 is the check §10.4/PF-05(b) actually demands: the negative fixture is **correct real
  source that ignores the transformation entirely**, and the gate rejects it before the
  toolchain ever sees it. Without this gate a trial could score most of the metric from
  pretraining recall alone. Demonstrated end to end through the real pipeline:

```
$ python3 harness.py pipeline/real_source_submission.md --expected expected_output.txt
extracted_chars:         897
gate_conformant:         False
gate_events:             ['class','else','for','if','int','long','public','static','void']
verdict:                 FAIL
reason:                  CONFORMANT check failed (section 4.2a)
```

- N2 corrupts one letter of one pseudo-word. The corrupted word is no longer in the
  inverse mapping, so the inverse treats it as an identifier, R1 byte identity fails, and
  the inverse image fails to build — two independent rejections of the same defect.
- N5 mutates the *mapping itself* rather than the source: with one token removed from the
  domain, the forward transformer leaves a real token in the transformed text, and the
  check catches it. This is the test that a silently under-applied transformer cannot pass.
- N4 exercises the `INVERSE_AMBIGUOUS` path of §6.1: a submission naming its own variable
  with a pseudo-word inverse-maps to a program that uses a keyword as an identifier and
  fails to build. It is detected and classified, not silently absorbed.
- P3/P4 are the converse guard: real keywords *inside a text literal* must be neither
  transformed nor flagged. Both hold, which is what makes §1 consequence 2 true in this
  implementation and not merely asserted.

### Determinism of the lexicalizer

```
$ cp mapping.json preflight/mapping.before.json
$ python3 lexicalize.py          # separate process
$ cmp preflight/mapping.before.json mapping.json
DETERMINISM: byte-identical across processes
```

---

## The mapping, and how §10.3 is satisfied

Domain (§7.1a): every token of `wordlists/reserved_java.txt` that occurs as a whole WORD
token in the fixture. That intersection is mechanically derived, not authored — it is
nine tokens.

| Role token key | pseudo-word | length | redraws |
|---|---|---:|---:|
| `K04` conditional branch | `mepone` | 6 | 0 |
| `K05` alternative branch | `bilite` | 6 | 0 |
| `K07` bounded iteration | `lukuki` | 6 | 0 |
| `K12` enclosing type declaration | `balinu` | 6 | 0 |
| `K21a` 32-bit integer type | `nasisa` | 6 | 0 |
| `K21e` result-less designation | `sadora` | 6 | 0 |
| `K23` visibility marker | `tenuro` | 6 | 0 |
| `U:long` 64-bit integer type | `metolu` | 6 | 0 |
| `U:static` type-level member marker | `zobolu` | 6 | 0 |

`anonymized_token_count = 9`. Published, not neutralized (§7.1).

| §10.3 requirement | How it is met | Evidence |
|---|---|---|
| deterministic from seed 20260918 | FNV1a32-seeded Park–Miller stream per key `I1\|java\|20260918\|<role key>` | determinism check above; `mapping.json.primitives` |
| one-to-one | 9 distinct images asserted in `lexicalize.py` | assertion in `main()`; `used` set in `assign()` |
| no collision with identifiers or literals | `accept()` rule 7 rejects any word occurring anywhere in the task material (fixture, expected output, task statement) | filter 7 fires in the self-test |
| no collision with each other | `accept()` rule 2 | filter 2 fires |
| no pseudo-word a prefix of another | all images are exactly 6 characters and distinct, so a prefix relation is impossible; re-checked by `prefix_free()` | assertion in `main()` |
| comparable character length | point mass at 6: mean 6.0, sd 0.0, min 6, max 6 | `mapping.json.invariants` |
| reject natural-language words | `accept()` rule 3 against `en_common.txt` (1445 entries) | filter 3 fires |
| reject common programming terms | `accept()` rule 4 against `prog_terms.txt` (301 entries) | filter 4 fires |
| reject real keywords of any of the ten languages | `accept()` rule 5 against `reserved_union.txt` (233 entries) | filter 5 fires |
| reject words *containing* a rejected term | `accept()` rule 6, substrings of length ≥ 4 | filter 6 fires |
| seed recorded in the file | `mapping.json.seed = 20260918` | `mapping.json` |

Word-list SHA-256 (also in `mapping.json.wordlist_sha256`):

```
02e017bee875884633bcf4c5b450b5d561224b71da42f8f8323ad1dc8d535698  en_common.txt
ff2771cd8c6beab062669b8e16161402ff96d05b312ba06e94c629339c2dac75  prog_terms.txt
7a3e24ec0c8f5ab8e6e8a5c36c5e1bd48e2810e575d2ba15574dc2805e112dfd  reserved_java.txt
272592d758de231643f7f34e8392041f382d8e2e5dd33eed6cc048497bb6abd7  reserved_union.txt
```

---

## Reference Pack size and leak scan

```
pack line count      : 249          (target band 150–250)
pack character count : 9057
prose characters     : 7639
code characters      : 1370
```

Leak scan (`leakscan.py`, §2.6):

- **CRITICAL** — none of the nine anonymized real spellings appears anywhere in the pack,
  as a whole word, in prose or in code. Clean.
- **IDENTITY** — zero hits across 46 language, compiler, runtime, build-tool and file-
  extension terms covering all ten languages of the fixed set. Clean.
- **ADVISORY** — one reserved word of the real language occurs in prose: `this`, 13
  times, as the ordinary English demonstrative ("this task", "this pack"). It is not the
  spelling of any anonymized token and carries no identifying signal in English usage.
  Published as an anonymity residual rather than removed, because contorting the prose to
  omit an English function word would make this pack read differently from every other
  language's pack, which §2.2 forbids.

**Stated anonymity residual (§2.6 exemption, §13.2).** I1 transforms grammar-significant
word tokens only; standard-vocabulary names keep their real spellings (§7.1). The pack
therefore contains `System.out.println`, `String`, `Main`, `main` and `args` in their real
form, and a reader who already knows the language will recognise the surface. This is
by design and identical in kind for every language the condition is applied to; the
requirement the spec sets is that the model is **not told** the language's name, and that
requirement is met.

---

## Files

| File | What it is |
|---|---|
| `mapping.json` | the keyword map, seed, primitives, filters fired, invariants |
| `lexicalize.py` | regenerates `mapping.json` deterministically from seed 20260918 |
| `javalex.py` | lossless token scanner; only WORD tokens are ever substituted |
| `forward.py` | real source → anonymized source |
| `inverse.py` | anonymized source → real source (+ `INVERSE_AMBIGUOUS` report) |
| `validate.py` | round-trip validator R1–R5, the §4.2a gate, and the positive/negative battery |
| `harness.py` | extraction → gate → inverse → fixups → build → run → oracle |
| `packcheck.py` | check (a): pack example ≡ fixture, builds, output matches the claim |
| `leakscan.py` | §2.6 identity-leak scan of the pack |
| `run_preflight.sh` | runs everything above and writes `preflight/` |
| `fixture_real.java` | the real fixture (written to `Main.java` by the harness) |
| `fixture_anon.java` | `forward(fixture_real.java)`, and the pack's P12 example |
| `expected_output.txt` | the oracle, produced by execution |
| `reference_pack.md` | the Reference Pack shown to the trial |
| `task_statement.txt` | the task, stated without naming any language |
| `neg/` | the mutated negative fixtures |
| `pipeline/` | the two whole-submission fixtures fed through `harness.py` |
| `wordlists/` | the four frozen rejection lists |
| `preflight/` | captured commands, stdout, stderr, hashes, full run log |

**No blocking problems. `all_pass = true`.**
