# MANDATORY PRE-FLIGHT VALIDATION — I1 / kotlin / seed 20260918

Methodology 10 §10.4 (spec §10.4), three required checks, each with the actual commands,
exit statuses and output — not a summary claiming they passed.

Reproduce everything below with:

```
./run_preflight.sh                      # exits 0 iff every check passed
```

Last run: `PREFLIGHT_TOTAL_NONZERO_EXITS=0`. Captured evidence is under `preflight/`
(`commands.txt`, per-command `.out`/`.err`, `hashes.txt`).

**GATE: `all_pass = true`.** No blocking problem was found. The findings in §6 are
published residuals, not blockers, and each carries the evidence for that judgement.

---

## 0. Environment and the frozen recipe

| | |
|---|---|
| compiler | `kotlinc-jvm 2.3.21 (JRE 26.0.1)` at `/opt/homebrew/bin/kotlinc` |
| runner | `/opt/homebrew/bin/kotlin` |
| entry file name (harness-supplied) | `solution.kt` |
| build command (harness-supplied) | `kotlinc <FILE> -d <OUT>` |
| run command (harness-supplied) | `kotlin -cp <OUT> SolutionKt` |
| anonymized token count (§7.1, published) | **7** |
| Reference Pack | 249 lines, 9607 characters (band 150–250 lines) |

**The run command is the `kotlin` runner, deliberately, and not `java -cp`.** The two are
not interchangeable here and the difference is invisible on the fixture, which is exactly
how an infrastructure defect hides. Evidence:

```
$ printf 'fun main() {\n    val xs = listOf(1, 2, 3)\n    println("N " + xs.size)\n}\n' > solution.kt
$ kotlinc solution.kt -d out
$ java -cp out SolutionKt
Exception in thread "main" java.lang.NoClassDefFoundError: kotlin/collections/CollectionsKt
        at SolutionKt.main(solution.kt:2)
exit=1
$ kotlin -cp out SolutionKt
N 3
exit=0
```

The compiler emits class-file major version 52 and the fixture itself touches no library
type, so the fixture alone runs under either command. Any trial that reaches for the
standard library would fail under `java -cp` — a language-looking failure with an
infrastructure cause. The frozen recipe therefore uses `kotlin -cp`, which supplies the
runtime library. Related: the `java` on `PATH` is 1.8.0_275, while the `kotlin` runner
uses its own JRE 26.0.1; the pipeline never invokes the `PATH` `java`.

---

## 1. Check (a) — the fixture compiles, runs, and its output matches the pack EXACTLY

`packcheck.py` does not trust the pack's prose. It takes the example out of the pack,
inverse-maps it, builds it, runs it, and byte-compares.

```
$ python3 packcheck.py
  a1 pack P12 example is byte-identical to fixture_anon.kt : True
  a2 pack's claimed output is byte-identical to expected_output.txt : True
  a3 pack P12 example passes the I1 conformance gate : True []
  a4 inverse(pack P12 example) is byte-identical to fixture_real.kt : True
  a5 it builds : True   exit 0 on run : True   stderr : ''
  a6 its stdout is byte-identical to the pack's claim : True
     produced:
       | SUM 25632
       | MAX 935
       | EVENS 24
       | JOINED 897-558-614-577-405

PACK CHECK PASSED
exit=0
```

`expected_output.txt` was produced **by execution**, never by hand: `fixture_real.kt` was
copied to `solution.kt`, compiled with the frozen recipe, run, and its captured stdout was
written to the file. Build stderr was 0 bytes and run stderr was 0 bytes. The four lines
are byte-identical to the independently-written reference implementation for another of the
ten languages, which is an external cross-check on the oracle rather than a self-consistent
one.

Exit status of the fixture is 0 (`run_exit: 0`, §2).

---

## 2. Check (b) — every withheld harness convention is satisfied BY THE HARNESS

§10.4 requirement (2) / PF-13. The principle (§9.1): the trial is never penalised for
failing to guess a fact about this benchmark's file system and invocation, and the harness
never supplies anything the pack documents.

**What the harness supplies, and why each is not a language fact:**

| Fixup | Supplied | Why it is harness responsibility |
|---|---|---|
| H1 | entry file name `solution.kt` | a fact about this benchmark's file system; the pack never states a file name |
| H1b | run entry symbol `SolutionKt` | derived from the file name by the toolchain's own rule; since the model is never told the file name, it could not name this symbol even in principle |
| H6 | output directory `out` | a fact about this benchmark's file system |
| — | build and run command lines | given to the trial; never guessed |

**The bare-body test.** `pipeline/bare_body_submission.md` contains every element the pack
documents — entry-point shape, binding words, control flow, the output call — in their
transformed spelling, uses identifier names that appear nowhere in the fixture (`s`,
`total`, `biggest`, `evenCount`, `head`, `k`, `v`), exercises the chained alternative
(`nifise pozebe`) that the fixture does not, and omits **only** the unstated conventions.
It was fed through the real pipeline: extraction → gate → inverse → fixups → build → run →
oracle.

```
$ python3 harness.py pipeline/bare_body_submission.md --expected expected_output.txt
extracted_chars:         651
gate_conformant:         True
gate_events:             []
inverse_substitutions:   14
build_cmd:               /opt/homebrew/bin/kotlinc .../solution.kt -d .../out
build_exit:              0
build_stderr:
run_cmd:                 /opt/homebrew/bin/kotlin -cp .../out SolutionKt
run_exit:                0
stdout:
    | SUM 25632
    | MAX 935
    | EVENS 24
    | JOINED 897-558-614-577-405
stderr:
stdout_byte_identical:   True
verdict:                 PASS
exit=0
```

**Zero pack-documented fixups were applied.** The fixup set was not widened.

### Frozen rule C-4 (methodology 00) — applied

C-4 requires that where a language's current stdout API differs from the widely-reproduced
older idiom, the Reference Pack must state the current spelling, because the task is not
about that and a trial must not fail on it. C-4 names TypeScript 7 (`--outFile` removed)
and Zig 0.16 (`std.io` helpers replaced) as the two toolchains where this drift exists.

**Determination for this language: no drift.** The current stdout spelling and the
widely-reproduced older idiom are the same token, `println(x)`, and it needs no import, no
receiver, no error-propagation marker and no explicit flush. Verified on the frozen
compiler:

```
$ cat c4.kt
fun main() {
    println("X")
    System.out.println("Y")
    print("Z\n")
}
$ kotlinc c4.kt -d o && kotlin -cp o C4Kt
X
Y
Z
```

C-4 is nevertheless **discharged positively rather than by omission**: the pack states the
spelling explicitly and marks it current, so a trial cannot lose on it either way.

- P8: `` `println(x)` `` … "the spelling above is the current one: write it exactly."
- P10: "`println(x)` writes the text form of `x` and then a line terminator. Four calls
  produce four terminated lines, including the last. No other line terminator is written
  by hand, and nothing is left buffered when the program ends."
- P9: "No module or namespace declaration is required, and none may be required.
  `println` is reachable from the entry point without any declaration."

No K27 error-propagation marker is bound for this language, because its output path
requires none.

### Two further non-task facts the pack must state, and does

Both were confirmed against the compiler, not recalled. Neither is what the task measures,
and either would produce a failure with a language-looking cause:

1. **Silent 32-bit overflow.** A whole-number literal without the `L` suffix gives the
   32-bit type, and the generator's product leaves that range. The program still builds and
   runs, and prints a wrong answer:

   ```
   $ cat t1.kt
   fun main() {
       var state = 7
       for (i in 0..49) { state = (state * 48271) % 2147483647 }
       println("no-L result: " + state)
   }
   $ kotlinc t1.kt -d o1 && kotlin -cp o1 T1Kt
   no-L result: -1496260057
   ```

   Stated by pack P4 rule 1 and shown throughout P12 (`7L`, `0L`).

2. **The two whole-number types do not mix under `==`.** This is a hard build error, not a
   coercion:

   ```
   $ cat t2.kt
   fun main() {
       val a = 7L
       println(a % 2 == 0)
   }
   $ kotlinc t2.kt -d o2
   t2.kt:3:13: error: operator '==' cannot be applied to 'Long' and 'Int'.
       println(a % 2 == 0)
               ^^^^^^^^^^
   ```

   Stated by pack P4 rule 2, with the correct form `a % 2 == 0L` given verbatim.

---

## 3. Check (c) — the validator can BOTH accept a correct input AND reject a corrupted one

§10.4 requirement (3). A validator that cannot fail measures nothing. Both directions are
exercised; `validate.py` exits non-zero if any positive case fails **or** any negative case
is accepted.

### 3.1 §10.4's warning about vacuous tests, addressed

> 10.4 / PF-05(a): a "did any transformed token survive?" test is vacuous when the mapping
> is a permutation of the language's own vocabulary.

It is not vacuous **here**, and the reason is structural rather than lucky: this mapping is
not a permutation of the language's own vocabulary. It maps 7 real keywords onto 7 PW-1
pseudo-words, and every pseudo-word is rejected by `accept()` rule 5 against
`reserved_union.txt` ∪ `reserved_kotlin.txt`, so **no pseudo-word is a reserved word of
this or any of the ten languages**. The two alphabets are therefore disjoint, and "a real
keyword appears as a WORD token" is a decisive signal.

That said, a survivor test alone would still be weak, so the battery does not rely on one.
It exercises six independent negative paths, including the two that a survivor test cannot
see at all: N2 (a corrupted pseudo-word — no real keyword survives, yet the round trip must
fail) and N6 (a pseudo-word hidden inside a text literal — it is not a real keyword either).

### 3.2 The battery, verbatim

```
$ python3 validate.py
========================================================================
POSITIVE CASES (must pass)
========================================================================
  P1 round_trip(fixture_real.kt)    R1_byte_identity         PASS
  P1 round_trip(fixture_real.kt)    R2_build_identity        PASS
  P1 round_trip(fixture_real.kt)    R3_behavioral_identity   PASS
  P1 round_trip(fixture_real.kt)    R4_non_triviality        PASS
  P1 round_trip(fixture_real.kt)    R5_domain_containment    PASS
  P2 gate(fixture_anon.kt)              conformant=True events=[]  PASS
  P3 gate(source whose text literal contains real keywords) conformant=True  PASS
  P4 forward() left the text literal untouched: True  PASS
  P5 forward() left a back-quoted identifier `if` alone while still transforming real keywords: True  PASS
  P6 gate() does not count a back-quoted `if` as a real keyword: True  PASS

========================================================================
NEGATIVE CASES (must be REJECTED)
========================================================================
  N1 gate(correct REAL source, transformation ignored)
     conformant=False  H_REAL events=['else', 'for', 'fun', 'if', 'in', 'val', 'var']
     -> REJECTED (PASS)
  N2 round_trip on a CORRUPTED transformed source (one letter changed)
     R1 byte identity vs real source: False
     inverse image builds: False   diagnostic: solution.kt:11:9: error: unresolved reference '
     -> REJECTED (PASS)
  N3 gate(transformed source with ONE real keyword written from memory)
     conformant=False  H_REAL events=['if']
     -> REJECTED (PASS)
  N4 INVERSE_AMBIGUOUS: a binding named with a pseudo-word
     detector hits=2  inverse image builds=False
     diagnostic: solution.kt:2:5: error: this variable must eith
     -> REJECTED (PASS)
  N5 round_trip with a MUTATED mapping (one token left unmapped)
     transformed source still contains the real token 'if': True
     the gate catches the survivor: conformant=False events=['if']
     -> REJECTED (PASS)
  N6 TEMPLATE_ESCAPE: a pseudo-word inside an in-literal embedding
     detector hits=2  inverse image builds=False
     diagnostic: solution.kt:25:20: error: unresolved reference
     -> REJECTED (PASS)

========================================================================
LEXICALIZER FILTER SELF-TEST (each accept() rule must be able to fire)
========================================================================
  filter 1 shape            candidate=toolong  accepted=False fired=['1']  PASS
  filter 2 used             candidate=pozebe   accepted=False fired=['2']  PASS
  filter 3 en_common        candidate=animal   accepted=False fired=['3']  PASS
  filter 4 prog_terms       candidate=buffer   accepted=False fired=['4']  PASS
  filter 5 reserved_union   candidate=typeof   accepted=False fired=['5']  PASS
  filter 6 substring        candidate=zitemu   accepted=False fired=['6']  PASS
  filter 7 material         candidate=rintln   accepted=False fired=['7']  PASS

========================================================================
BATTERY PASSED: every positive case passed and every negative case was rejected.
exit=0
```

### 3.3 What each negative case proves

| Case | Mutation | Why it must be rejected |
|---|---|---|
| N1 | none — correct **real** source that ignores the transformation entirely | PF-05(b): the I1 conformance gate must reject correct real source. All 7 real keywords are reported. |
| N2 | one letter of one pseudo-word (`pozebe` → `pozeba`) | R1 byte identity fails **and** the inverse image does not build. No real keyword survives, so only the round trip catches this. |
| N3 | one pseudo-word written as the real keyword from memory (`pozebe` → `if`) | the gate reports the single leaked token. |
| N4 | the submission names one of its own bindings with a pseudo-word (`state` → `ruroge`) | §6.1 `INVERSE_AMBIGUOUS`: the inverse image uses a keyword as an identifier and does not build. Detector fires (2 hits) **before** the build, so it is recorded as a model failure (H-INVENT), not an infrastructure defect. |
| N5 | the **mapping itself** is mutated — one token left unmapped | a forward transformer silently failing to bind a token is caught, not tolerated. |
| N6 | a pseudo-word placed inside an in-literal embedding (`"SUM ${pozebe ...}"`) | language-specific; see §6 finding 3. Detector fires and the inverse image does not build. |

The leak scanner is likewise demonstrated to be able to fail: the first draft of the pack
contained one stray occurrence of a real keyword in prose, and `leakscan.py` exited 1 with
`LEAK SCAN FAILED: ['CRITICAL:else']`. The sentence was rewritten; the scan now exits 0.

---

## 4. Supporting checks

### 4.1 PF-03 — the §7.1a domain, recomputed mechanically

The domain is not authored. It is `reserved_kotlin.txt ∩ {WORD tokens of fixture_real.kt}`,
recomputed by `domaincheck.py` from the two frozen inputs and compared to the binding table.

```
$ python3 domaincheck.py
frozen reserved-word list entries : 77
distinct WORD tokens in fixture   : 16
RECOMPUTED domain: else, for, fun, if, in, val, var        count = 7
AUTHORED  table  : else, for, fun, if, in, val, var        count = 7
  recomputed domain == authored table          : True
  anonymized_token_count matches recompute     : True (7)
  unclassified word tokens                     : 0
  unbound_word_tokens                          : [] (empty)
DOMAIN CHECK PASSED
exit=0
```

Every distinct word token of the fixture is classified: 7 BOUND, 2 V-roles kept in their
real spelling in I1 (`main`, `println`), 7 locally declared identifiers that are never
renamed (`state`, `sum`, `max`, `evens`, `joined`, `term`, `i`).

Role bindings: `fun`→K01, `val`→K02, `var`→K03, `if`→K04, `else`→K05, `for`→K07, and
`in`→`U:in` (a non-role reserved word, bound under §7.1a by the identical PW-1 procedure).

### 4.2 PF-08 — lexicalizer determinism and the §10.3 rules

```
$ python3 lexicalize.py        # re-run in a separate process
  K01      fun    -> dunisi
  K02      val    -> rinuso
  K03      var    -> ruroge
  K04      if     -> pozebe
  K05      else   -> nifise
  K07      for    -> kepafa
  U:in     in     -> pibemo
rejection filters fired: {'1': 0, '2': 0, '3': 0, '4': 0, '5': 0, '6': 2, '7': 0}
DETERMINISM: byte-identical across processes
```

| §10.3 requirement | Status | Evidence |
|---|---|---|
| deterministic from seed 20260918 | yes | byte-identical `mapping.json` across separate processes; seed recorded in the file (`"seed": 20260918`) |
| one-to-one | yes | 7 distinct pseudo-words for 7 distinct tokens; asserted in `lexicalize.py` |
| no collision with identifiers or literals | yes | `accept()` rule 7 rejects any word occurring anywhere in the task material; the 7 locally declared identifiers are all short real words, none CVCVCV |
| no pseudo-word is a prefix of another | yes | all are exactly 6 characters and distinct, so a prefix relation is impossible; asserted |
| comparable character length | yes | point mass at 6 (mean 6.0, sd 0.0, min 6, max 6) |
| rejects natural-language words | yes | rule 3 (`en_common.txt`) and rule 6 (no ≥4-letter entry as a substring) |
| rejects common programming terms | yes | rule 4 (`prog_terms.txt`) and rule 6 |
| seed recorded in the file | yes | `mapping.json` `"seed": 20260918` |

Rule 6 fired **twice during real generation**, not only in the self-test: two candidate
words were discarded for containing a ≥4-letter natural-language or programming term. The
resulting words — `dunisi`, `rinuso`, `ruroge`, `pozebe`, `nifise`, `kepafa`, `pibemo` — are
not English words and not programming terms.

### 4.3 PF-07 — identity-leak scan of the pack

```
CRITICAL — anonymized real spellings anywhere in the pack:  all 7 clean
IDENTITY — language / toolchain / extension names:          clean (0 hits across 46 terms)
pack line count      : 249
pack character count : 9607
LEAK SCAN PASSED
exit=0
```

Writing 249 lines of English that never use the words `if`, `else`, `for` or `in` as whole
words is the binding constraint on this pack, and the scan is what enforces it.

ADVISORY (published anonymity residuals, §13.2 — ordinary English usage of words that are
*soft* keywords of the real language, appearing in prose only, none of them anonymized by
this condition): `is` ×47, `by` ×14, `this` ×13, `it` ×12, `value` ×12, `as` ×5, `when` ×5,
`do` ×2, `operator` ×1, `where` ×1.

### 4.4 Lexer losslessness

`ktlex.py` is lossless: concatenating every token's raw text reproduces the input byte for
byte (`lossless: True`). Verified by `lextest.py` on nine adversarial inputs: the range
operator `0..49` (which a Java-shaped number scanner swallows whole), underscored and
hex and float literals, a **nested** block comment, a back-quoted identifier, a text literal
containing real keywords, a raw triple-quoted literal, `!in`, a character literal, and the
`?:`/`?.`/`===` operators. All lossless, all tokenized as intended.

---

## 5. The §6.4 invariant

> real source → forward → inverse → **byte-identical** real source

```
$ python3 forward.py fixture_real.kt -o fixture_anon.kt
$ python3 inverse.py fixture_anon.kt | cmp - fixture_real.kt && echo IDENTICAL
IDENTICAL
```

R1 holds as a plain byte-identity test with no canonicalization step, because the forward
transformer inserts no whitespace: a WORD token produced by the scanner is maximal, so
substituting one word for another cannot merge it with a neighbour, and this language is
not indentation-significant. The unit position map is consequently empty. `forward.py` and
`inverse.py` both re-tokenize their own output and abort if the token shape changed, so a
merge would be an error rather than a silent corruption.

R2/R3 (build and behavioural identity) and R4/R5 (non-triviality, domain containment) are
the P1 block of §3.2. R5 is checked position-for-position: every token that differs between
`F` and `forward(F)` must be a bound token replaced by its own pseudo-word — no identifier,
literal or unrelated token is in the diff.

---

## 6. Published findings and residuals

None of these blocks the run. Each is recorded because §10.4 forbids silent correction.

**1. `reserved_union.txt` omits 14 spellings reserved by this language, including one of
the seven anonymized tokens (`fun`).** The file is the shared, frozen, cross-language union;
its SHA-256 here (`272592d7…`) is byte-identical to the copy used by the already-built
sibling language, so it has not been edited. Impact, analysed rather than assumed:

- *Locally, none.* `accept()` tests candidates against `reserved_union.txt` **∪
  `reserved_kotlin.txt`** for both the equality filter (5) and the substring filter (6), so
  every one of the 14 is covered for this language's mapping.
- *Cross-language:* the only way the omission could matter is if another language's
  pseudo-word were spelled exactly like one of the omitted words. Only the two 6-character
  omissions can do that — `actual` and `expect` — and **PW-1 cannot generate either**: the
  pattern is CVCVCV with position 0 drawn from `CONS`, and both words begin with a vowel. No
  realizable collision exists.

Recommendation, for the run owner rather than for this language: add the 14 spellings to
`reserved_union.txt` centrally at PF-08 and re-run pre-flight for all ten, since the file is
shared. Not done here, because editing a frozen shared file from one language's directory
would invalidate the sibling's recorded hash and is a §14 change-control event, not an edit.

**2. The conformance gate uses the *hard* keywords (28), not the full reserved list (77).**
Unlike most of the ten, this language has soft/modifier keywords — `value`, `data`, `set`,
`get`, `it`, `open`, `out`, `where`, and 41 more — that are **legal ordinary identifiers**.
A gate built from the full reserved list would reject a perfectly conformant submission for
naming one of its own bindings `value`: an infrastructure defect that would look exactly
like a language failure and would depress this language's I1 score for a naming choice.
The gate set is asserted at import time to be a superset of the mapping
(`set(load_mapping()).issubset(HARD_SET)`), so every anonymized token is still covered, and
N1/N3/N5 demonstrate it rejecting real-keyword recall.

**3. This language can hide code inside a text literal, and §6.2 forbids entering text
literals in either direction.** A pseudo-word written inside an in-literal embedding
(`"SUM ${pozebe ...}"`) is therefore not translated by the inverse and the inverse image
does not build. Three mitigations, all in place:

- the pack tells the model not to do it, in P2, as a lexical rule and not as a hint: "**Do
  not write the character `$` inside a text literal.** It is reserved there and this pack
  documents no meaning to give it." This is a true statement about the language, not a
  fiction — `$` really is reserved inside a text literal here;
- the pack gives the route it *does* document, twice: P2 points at `+`, and P5 spells out
  `"" + total` as the conversion from a number to text;
- `inverse.template_report()` detects the case and reports it, so it is recorded as a model
  failure (H-INVENT, out-of-pack invention) rather than surfacing as an unexplained build
  error. Exercised as negative case N6.

The rule "text literals are never entered in either direction" is kept, so this language is
transformed by the same machinery as the other nine.

**4. Model-tokenizer residual (§5.4), unchanged and restated.** Pseudo-word matching is
exact in characters (all 6) and exact in frozen lexical tokens (all 1). How the benchmark
model's own BPE tokenizer segments a given six-letter string is not measurable with this
client. `model_tokenizer_available: false`.

**5. Pack size.** 249 lines / 9607 characters, against the sibling language's 249 lines /
9057 characters. Within the 150–250 band and not given more explanation than a sibling,
which is the §2.3 equalization requirement. The extra 550 characters are the two non-task
type rules of §2 — silent overflow and the `==` type match — which exist because this
language genuinely has those traps and C-4's principle ("the task is not about that")
applies to them. They are stated as briefly as correctness allows.

---

## 7. Artifact hashes

From `preflight/hashes.txt` (SHA-256):

```
c98081c9ec047098f393a613a6a7944c8d8eb12f65fd710ace77b57582e172fa  mapping.json
4fb3e04e84b828ab2b757f9942d22e33d3c5acbb9573e7e6e80d4d07fcd279c3  fixture_real.kt
42377932108a8f4032752adbf6f19c43a6ddd25a03db5b60133d052136d6e81b  fixture_anon.kt
ed254b8ea2e6526c9d3f4e2312a91216a177f6fc0c017f53286a7cfe3a9a8609  expected_output.txt
9447eb73120e479d84e6f0e40498ce65c8a7b7de967849a058b7ba2296916213  reference_pack.md
c3fcbdc87519e5e6e9f3fcfaa5a9d4f179e7852e0f8b00890802881980e6954b  forward.py
6548347a7fa074e2f026ce9f0a79314b7dc6b239506ca886e3115eefdc6fb58b  inverse.py
7232855e69c1eedcd5fc7dc20e78baf5d407cb6b76a119d40b1bb2e12ca7faf9  ktlex.py
0dcb422807b2e47d756cbea233482e927a41c484f3eb9cdb85e4bcc2acfbf165  lexicalize.py
ab312b655e27236ab5ff7d7975e76be85fb5c4ac09ab9478e3c6d90189c84db4  validate.py
46cbe9c352e49b898b1fe19402b078c124f59e267f178c6ce3bfb3a37a7d6c27  harness.py
71925e1865f40479298c9cd0295eb96d49abf8909bf0d563c7ca42b15435b9ae  domaincheck.py
fd8421175e7afca828b395c80ef01a833b1a6aa3bc9863ecd788e1165429605f  packcheck.py
5aeeda35708a10df98ee08d2ea5d3d96dce587ef360af190c1e88a01bf4a278d  leakscan.py
80df6a893c5e4db7483cf8b208a265a769e5301cb0e9f64f347386df574c20f7  lextest.py
02e017bee875884633bcf4c5b450b5d561224b71da42f8f8323ad1dc8d535698  wordlists/en_common.txt
dc70ad18e8c35ebefcb89aa7564bc333ad4e8e9b005d43d8aed0c60180c159fe  wordlists/hard_kotlin.txt
ff2771cd8c6beab062669b8e16161402ff96d05b312ba06e94c629339c2dac75  wordlists/prog_terms.txt
0754a96dee4b6fc0c973dd69340827d9edfed3705c8d9210e7c17496f5b37be6  wordlists/reserved_kotlin.txt
272592d758de231643f7f34e8392041f382d8e2e5dd33eed6cc048497bb6abd7  wordlists/reserved_union.txt
```

The three shared frozen word lists (`en_common.txt`, `prog_terms.txt`,
`reserved_union.txt`) hash identically to the sibling language's copies, confirming they
were used unmodified.
