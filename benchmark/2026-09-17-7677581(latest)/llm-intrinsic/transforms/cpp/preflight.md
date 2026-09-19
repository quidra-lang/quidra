# MANDATORY PRE-FLIGHT VALIDATION — I1 Keyword Anonymization, C++ column

Methodology: `methodology/10_intrinsic_design.md` §10.4 (requirements 1, 2, 3), §10.1 checks
PF-01, PF-02, PF-04, PF-05, PF-07, PF-08, PF-13; `methodology/00_cross_language_constraints.md`
frozen rule C-4.

- `language_id`: `cpp`
- deterministic seed: **20260918**
- toolchain: Apple clang version 17.0.0 (clang-1700.6.3.2), arm64-apple-darwin25.4.0
- build recipe (frozen, §0.1): `clang++ -std=c++20 -O2 FILE.cpp -o BIN`
- driver: `preflight/run_all.sh`; full captured transcript: `preflight/commands_output.txt`

**GATE: `all_pass = true`. No check failed. Nothing blocks the trial.**

---

## Summary

| Check | §10.4 requirement | Verdict |
|---|---|---|
| PF-01 | (1) fixture builds, runs, output matches the pack exactly | **PASS** |
| PF-01b | (1) reference solution builds, runs, produces the oracle | **PASS** |
| PF-02 / PF-04 | the reversibility invariant R1–R5 | **PASS** |
| PF-05 | (3) every validator both accepts and rejects | **PASS** |
| PF-07 | identity-leak scan of the Reference Pack | **PASS** |
| PF-08 | lexicalizer self-test: determinism, one-to-one, prefix-free, filters fire | **PASS** |
| PF-13 | (2) harness conventions are satisfied by the harness | **PASS** |
| C-4 | current stdout spelling stated in the pack | **N/A for this language, stated anyway** |

---

## (a) The fixture compiles, runs, and its output matches EXACTLY what the pack claims

The fixture is `fixture_real.cpp` (real form) / `fixture_anon.cpp` (the form that appears in
P12 of `reference_pack.md`). It is **deliberately not** the task's solution: methodology §2.3
forbids a pack example that is a worked task solution. It exercises every construct the task
needs — module directives, the entry point, an immutable declaration, both integer widths, the
bounded loop, the conditional with its alternative, remainder and comparison, text joining,
whole-number-to-text conversion, line output, and the result statement — on a different
recurrence with different labels.

```
$ clang++ -std=c++20 -O2 fixture_real.cpp -o preflight/artifacts/fixture_real.bin
build exit=0
$ ./preflight/artifacts/fixture_real.bin
run exit=0
fixture stdout: byte-identical           (diff fixture_expected.txt against the actual run)
fixture stderr bytes:        0
pack-claimed fixture output vs actual:
  identical
```

The last line is a mechanical comparison: the four lines printed inside the final fenced block
of `reference_pack.md` are extracted and compared byte for byte against the bytes the built
fixture actually wrote. They are:

```
TOTAL 193
BIGGEST 53
ODDS 3
CHAIN 21/46/19/32/22/53
```

### The oracle (`expected_output.txt`) was produced by execution, never by hand

`reference_solution.cpp` is a real, non-anonymized implementation of the task written for this
build. It was compiled and run, and its stdout **is** `expected_output.txt`:

```
$ clang++ -std=c++20 -O2 reference_solution.cpp -o preflight/artifacts/reference_solution.bin
build exit=0
$ ./preflight/artifacts/reference_solution.bin > expected_output.txt
run exit=0
oracle stdout: byte-identical
independent cross-check: identical
```

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

The file ends with a newline (0x0a) after the fourth line and contains nothing else.
"independent cross-check" is a second, independently written implementation of the same
generator (`preflight/artifacts/oracle_crosscheck.py`) whose output is byte-identical, so a bug
in one implementation could not have silently defined the oracle.

---

## (b) Every harness convention the prompt withholds is satisfied BY THE HARNESS

Recorded in `harness.json`. The Reference Pack describes the *language*; it says nothing about
this benchmark's file system or invocation, because those facts would identify the toolchain
(§9.1). The harness supplies them and the trial is never asked to guess them:

| Convention | Value | Who supplies it |
|---|---|---|
| entry filename | `solution.cpp` | harness, fixup H1 |
| build command | `clang++ -std=c++20 -O2 solution.cpp -o solution` | harness |
| run command | `./solution` | harness |
| working directory | fresh per-trial directory | harness |

**Pack-documented fixups applied: 0.** H2–H6 are not needed by this language for this task: the
pack documents the module directives (P9), the entry point (P1) and the result statement (P6),
so a submission that omits them is a model failure and is not rescued (§9.3).

### PF-13 evidence — a bare-body submission through the real pipeline

`preflight/bare_body_submission.txt` is a raw model-style output: prose, then one fenced block
containing a correct solution written **only** from the pack. It carries every element the pack
documents and omits exactly the unstated conventions (no filename, no build instructions, no
fence language tag). It was fed through `pipeline.py`, which is the real chain —
extraction (§9.2) → conformance gate (§4.2a) → inverse → fixups (§9.3) → build → run → oracle:

```
  extract                  OK   793 bytes
  gate                     OK   no H_REAL, no H_INVENT
  fixup H1                 OK   written as solution.cpp
  fixups pack-documented   OK   0 applied
  build                    OK   clang++ -std=c++20 -O2 solution.cpp -o solution -> exit 0
  run                      OK   exit 0
  oracle stdout            OK   byte-identical
  oracle stderr            OK   empty
  PIPELINE VERDICT: PASS
```

### Frozen rule C-4 (methodology 00) applied to this column

C-4 obliges the pack to state the **current** spelling of the standard-output facility wherever
a language's current API differs from the widely-reproduced older idiom, so that a trial is never
failed on a fact the task is not about. C-4 names TypeScript 7 and Zig 0.16 as the two toolchains
where this drift exists.

**This language is not one of them.** Its standard-output idiom — a stream object, the insertion
operator, and a line-terminator manipulator, reached through one module directive — is the same
text in the current revision as in every widely-reproduced older one, so there is no older form a
trial could write that the current toolchain would reject. The obligation is nevertheless
discharged positively rather than waved: `reference_pack.md` P10 states the exact current
spelling, says in terms that it is the current spelling, states that no other form is needed, and
states that no error-propagation marker is required on the entry point (role `K27` is unbound in
this column). PF-01 and PF-13 both compile and run that exact spelling under the frozen recipe,
so the pack's claim is verified rather than asserted.

---

## (c) The round-trip validator can BOTH accept a correct input AND reject a corrupted one

`validate.py` has three independent checks, each of which is exercised in both directions.
`pipeline.py` exercises the gate in the same place a scored trial would hit it.

### Positives — must ACCEPT

```
$ python3 validate.py roundtrip fixture_real.cpp
ACCEPT  roundtrip fixture_real.cpp
$ python3 validate.py roundtrip reference_solution.cpp
ACCEPT  roundtrip reference_solution.cpp
$ python3 validate.py gate fixture_anon.cpp
ACCEPT  gate fixture_anon.cpp
$ python3 validate.py all reference_solution_anon.cpp expected_output.txt
ACCEPT  all reference_solution_anon.cpp
```

R1 byte identity was additionally checked with `cmp` on files written to disk, not only in
memory, for both real sources:

```
forward: 21 tokens rewritten -> preflight/artifacts/rt_fixture_anon.cpp
inverse: 21 tokens restored  -> preflight/artifacts/rt_fixture_back.cpp
R1 fixture: byte-identical
forward: 24 tokens rewritten -> preflight/artifacts/rt_sol_anon.cpp
inverse: 24 tokens restored  -> preflight/artifacts/rt_sol_back.cpp
R1 solution: byte-identical
ACCEPT  build rt_fixture_anon.cpp     (R2 builds, R3 same output)
ACCEPT  build rt_sol_anon.cpp         (R2 builds, R3 same output)
```

- **R1** byte identity: `inverse(forward(F)) == F`, exactly, for both sources. The forward pass
  inserts **zero** whitespace characters — maximal-munch lexing means a word token can never be
  adjacent to another identifier character, so the §5.5 emission rule has nothing to do here, the
  position map is empty, and the inverse has nothing to delete. `forward.py` asserts this and
  refuses to emit rather than silently inserting a character it could not take back.
- **R4** non-triviality: 21 and 24 tokens rewritten respectively; the forward pass is not a no-op.
- **R5** domain containment: the token streams of `F` and `forward(F)` are compared position by
  position and every difference must be a `WORD` token that the mapping covers. No identifier,
  literal, number or comment differs.

### Negatives — must REJECT

Five distinct corruptions, each rejected, each for a different reason:

```
$ python3 validate.py all preflight/negatives/neg_real_ignores_transform.cpp expected_output.txt
REJECT  - H_REAL: submission uses untransformed real reserved words:
           const, else, for, if, include, int, long, main, return          exit=1

$ python3 validate.py all preflight/negatives/neg_corrupt_letter.cpp expected_output.txt
REJECT  - H_INVENT: submission uses unmapped pseudo-word-shaped tokens: rigumu
        - BUILD FAILED (exit 1): solution.cpp:15:9: error: use of undeclared
          identifier 'rigumu'                                              exit=1

$ python3 validate.py all preflight/negatives/neg_swap_roles.cpp expected_output.txt
REJECT  - BUILD FAILED (exit 1): solution.cpp:33:9: error: expected '(' after 'for'
                                                                           exit=1

$ python3 validate.py all preflight/negatives/neg_wrong_output.cpp expected_output.txt
REJECT  - OUTPUT MISMATCH:
            expected: b'... JOINED 897-558-614-577-405\n'
            actual:   b'... JOINED 897+558+614+577+405\n'                  exit=1

$ python3 validate.py all preflight/negatives/neg_ambiguous.cpp expected_output.txt
REJECT  - INVERSE_AMBIGUOUS: mapped word used as a declared name: kelugo(->return)
        - BUILD FAILED (exit 1): solution.cpp:7:15: error: expected unqualified-id
                                                                           exit=1
```

And the §4.2a gate negative that PF-05(b) specifically demands — **correct real source that
ignores the transformation entirely**, fed through the same pipeline a scored trial goes through:

```
$ python3 pipeline.py preflight/neg_real_submission.txt expected_output.txt
  extract                  OK   531 bytes
  gate                     FAIL H_REAL: submission uses untransformed real reserved words:
                                for, if, include, int, long, main, return
  PIPELINE VERDICT: FAIL                                                   exit=1
```

That submission is a *correct* program: it would build, run and produce the right four lines if
it were let through. It is rejected anyway, which is the point — without the gate, roughly
two-thirds of CTES would be reachable from pretraining recall alone (§4.2a).

### On §10.4's vacuity warning

§10.4 and PF-05(a) warn that a "did any transformed token survive?" test measures nothing when
the mapping is a permutation of the language's own vocabulary, because every real token is then
also a legal transformed token. **That degenerate case does not arise here**: I1's mapping is not
a permutation. Its image is nine CVCVCV pseudo-words drawn from a 343,000-word space and filtered
against the reserved words of all ten languages, so no pseudo-word is a real token of this or any
other language and no real token is a pseudo-word. The two alphabets are disjoint, which is what
makes the H_REAL test discriminating rather than vacuous.

The validator does not rest on that one test in any case. Three of the five negatives above pass
the token-survival question and are still rejected: `neg_swap_roles` uses only legal pseudo-words
(it fails at build), `neg_wrong_output` is entirely well-formed (it fails at the oracle), and
`neg_ambiguous` uses only mapped words (it fails at inverse-ambiguity and at build). A validator
that could only ask "did a real token survive?" would have accepted all three.

---

## PF-08 — lexicalizer self-test

```
### PF-08a determinism: regenerate the mapping in two separate processes
8595e3956ae25421f7043b594c62d70b436e39f90935a9b0fca8ba1c5471d487  mapping.json
8595e3956ae25421f7043b594c62d70b436e39f90935a9b0fca8ba1c5471d487  mapping.json

### PF-08b mapping invariants
one-to-one:9  shape:^[a-z]{6}$ CVCVCV  prefix-free:yes  char-len: all 6  seed: 20260918

### PF-08c rejection filters can fire
  filter 1  shape ^[a-z]{6}$                    candidate toolong  fired=['1']  OK
  filter 2  already assigned in this mapping    candidate riguma   fired=['2']  OK
  filter 3  natural-language word list          candidate better   fired=['3']  OK
  filter 4  programming-term list               candidate buffer   fired=['4']  OK
  filter 5  reserved words of the ten languages candidate unsafe   fired=['5']  OK
  filter 6  contains a list entry >= 4 chars    candidate datave   fired=['6']  OK
  filter 7  occurs in this column's task material candidate vizeka fired=['7']  OK
  all seven filters fired as expected: True
```

Word-list SHA-256 (recorded, per §5.3):

```
76dc0ea51fd126f3d5c7ffe4cfcde58b9297174251b4075481c8c7ce74535909  wordlists/en_common.txt
7a2e85348b2c93213d1826ff1c396881c259db5868b49de21d4567fe66d06815  wordlists/prog_terms.txt
1c7f71164f20915073b810a422dca032c9c4387815ea40aabb9d67e15e227d6d  wordlists/reserved_union.txt
```

The mapping, nine tokens, seed 20260918:

| Role | Real token | Pseudo-word |
|---|---|---|
| K04 | conditional branch | `rigumo` |
| K05 | alternative branch | `pemafo` |
| K07 | bounded iteration | `sobisi` |
| K11 | function result | `kelugo` |
| K14 | module directive word | `nusabo` |
| K21a#1 | basic integer type | `derane` |
| K21a#2 | width qualifier | `litasa` |
| K22 | entry-point name | `konazo` |
| K24 | immutability qualifier | `febuvo` |

`anonymized_token_count = 9`; `collision_redraws = {}`; every pseudo-word is exactly 6
characters, so the character-length distribution is a point mass at 6 with zero residual, and no
pseudo-word can be a prefix of another. None of the nine collides with an identifier or a literal
in either fixture or in the task material (filter 7).

---

## PF-07 — identity-leak scan of the Reference Pack

```
  mapped real keywords in prose:                NONE
  mapped real keywords anywhere in the pack:    NONE
  language/tool identity terms:                 NONE
  lines: 245   chars: 8450
```

The scan strips fenced code blocks and inline code spans, then looks for each of the nine mapped
real tokens as a whole word, and for language, compiler and standard-body names anywhere in the
file. The pack does not name the language and contains none of the nine real keywords in any
position, prose or code.

**Reference Pack size: 245 lines, 8,450 characters** (within the 150–250 line target).

---

## Published residuals — not defects, not corrected

1. **Untransformed library surface (§2.6 exemption, §13.2 anonymity residual).** I1 transforms
   K-role and non-role reserved *word* tokens only; standard-library and builtin names keep their
   real spellings by design (§7.1), and §2.6 exempts them from the leak scan. This column's pack
   therefore carries `std`, `iostream`, `string`, `cout`, `endl`, `to_string` in their real form.
   A reader who knows the language will recognize the library surface even though the pack never
   names the language and every grammar word is anonymized. This is a designed property of I1, it
   applies to whichever of the ten languages I1 is run on, and it is recorded here rather than
   removed. I2 is the condition that anonymizes this surface.

2. **`anonymized_token_count = 9`, deliberately not quota-matched (§7.1).** This column's
   task-relevant grammar is spelled with nine word tokens. Languages needing more get more; the
   count is published beside the score and is never capped, because capping would leave the
   remaining keywords familiar.

3. **Model-tokenizer residual is unmeasurable (§5.4).** Pseudo-word matching is exact in
   characters (all 6) and exact in frozen lexical tokens (all 1). How the benchmark model's own
   BPE segments a given six-letter string is not exposed by this client and is therefore
   **unknown**, not equalized. `model_tokenizer_available: false`.

4. **Scope of this build.** This is the I1 infrastructure for one language and one task, at seed
   20260918. It is not the five-seed replicate set of §0.2, and the cross-language checks that are
   only meaningful with all ten columns present — PF-03 binding-table completeness against a
   frozen per-language reserved-word list, PF-06 twelve-slot pack conformance and the six-example
   budget, PF-09/PF-10 (I3 only), PF-11 cross-language expected-output determinism, PF-12
   diagnostic scrubbing, PF-14 config hashing — are out of scope here and remain owed before a
   scored run. None of them is a failing check; they are unrun checks belonging to the full
   assembly. The three checks §10.4 makes mandatory for this column — (a), (b), (c) — all pass.

5. **Control-flow vocabulary is exactly what the task needs.** The pack documents one loop form,
   one branch form with its alternative, and the result statement. Loop-exit and next-iteration
   words (`K09`, `K10`) and the conditional-iteration word (`K08`) are **not** mapped, because
   they do not occur in this column's fixture or reference solution and §7.1a derives the domain
   mechanically from occurrence rather than from a hand-picked list. P6 of the pack states in
   terms that these are the only control-flow words available, so a trial is told, not left to
   discover it at the gate.

---

## File inventory and hashes

```
8595e3956ae25421f7043b594c62d70b436e39f90935a9b0fca8ba1c5471d487  mapping.json
30b6bd03e7c6a8576c7a20cf702288786c63f3abad2963746e86edf575aa48eb  forward.py
0afc122b8be53f3b2e8d71e27f565068b7e8d5be1f15232b889ca857b384a25d  inverse.py
9ae6dfd612ca1aa002b7048041942a3d69e265bbf30a06d40c1d50b4c28dde8d  lex_cpp.py
a03e803541f79a81ecb24e2d7581e329c22962f59c8d70ed2a2c03ce78da9c74  validate.py
7987ed0737ac857b943e9f8a015f7ff886a4f522279766fe3bc6e6104c15d89e  pipeline.py
bf7418c6c2cabba19afc30c372c74073c68a346ea7dbb814cc1e15d94c8309c2  gen_mapping.py
98aec1edf6891192f4f1dc47990aecf47a45e42e576aa5cafe1e3d9b0e88543a  fixture_real.cpp
ef4ea70e61b15f7c04cf2f317e689665199bfce6eead4740060ad6a48e1c278d  fixture_anon.cpp
a39f7e80692fd81726fb137a0222a1f7a15841f36ec134a57aa82b65c8bf029d  fixture_expected.txt
2376a62827febac8c151bb04eef050cd2fc728ef465d0c712d911c3599baf104  reference_solution.cpp
eb8fb620bf949d872fcc21662260ce8a80cf4c8883fbea5f5327182bfaf2838b  reference_solution_anon.cpp
ed254b8ea2e6526c9d3f4e2312a91216a177f6fc0c017f53286a7cfe3a9a8609  expected_output.txt
0f18bd7a5c429a7001d2f4764e7cea8402dfa9a1d273682effcda3dd2b92b910  reference_pack.md
76dc0ea51fd126f3d5c7ffe4cfcde58b9297174251b4075481c8c7ce74535909  wordlists/en_common.txt
7a2e85348b2c93213d1826ff1c396881c259db5868b49de21d4567fe66d06815  wordlists/prog_terms.txt
1c7f71164f20915073b810a422dca032c9c4387815ea40aabb9d67e15e227d6d  wordlists/reserved_union.txt
```

Hashes are recomputed by the last section of `preflight/run_all.sh`; the values above were taken
from that run and any edit to a listed file invalidates them, requiring the pre-flight to be
re-run in full (§10.2 forbids partial re-validation).
