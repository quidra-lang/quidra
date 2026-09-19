# MANDATORY PRE-FLIGHT VALIDATION — I1, language `zig`, seed 20260918

Methodology 10 §10.4, all three checks, with evidence. Every command in this report was
executed; nothing here is a summary that claims a result it did not obtain.

Reproduce in full with:

```
sh run_preflight.sh          # writes preflight_evidence/ and preflight_evidence_log.txt
```

**Gate field: `all_pass = true`.** Last run exit status 0.

## 0. Environment and frozen inputs

| Item | Value |
|---|---|
| toolchain | `zig version` → **0.16.0** (Homebrew 0.16.0_1) |
| host | darwin 25.4.0, arm64 |
| python | 3.9.6 |
| entry filename | `solution.zig` |
| build command | `zig build-exe solution.zig -O ReleaseSafe -femit-bin=solution` |
| run command | `./solution` |
| condition / seed | I1 / 20260918 |
| anonymized token count | **12** (mechanically derived, §7.1a) |

SHA-256 of the frozen inputs, as printed by the run:

| File | SHA-256 |
|---|---|
| `wordlists/en_common.txt` | `54056731d43f793f8d0ea08285d4f736b18438694dc7c6813c1e3acbd1f01a00` |
| `wordlists/prog_terms.txt` | `dfea414450548daad073a375e585dfcb7d9c2e4eda64254598d6aab4dc3fa92b` |
| `wordlists/reserved_union.txt` | `d8b112ebaae29508258ad8dd0fe2c6112a827471ec6b5f4cbeeb7fbf22d97f03` |
| `wordlists/reserved_zig.txt` | `f18db1f61db7d680b8847f269283f794e0093a603a676ac2b7c391c6ce916e86` |
| `mapping.json` | `df4ba177521bcdfc81ea64313136a600e684c94f53191e2d497ea31813597899` |
| `fixture_real.zig` | `3498c5b2eeb170ba0d0e3b85f017c6662b0f750139cbee32ce95e2169cfe48f8` |
| `fixture_anon.zig` | `05eff0a3f81230a3262eb29c094503add2f79fb51ec21161ba50bf1bc4f9432e` |
| `expected_output.txt` | `ed254b8ea2e6526c9d3f4e2312a91216a177f6fc0c017f53286a7cfe3a9a8609` |
| `reference_pack.md` | `730e270f01fe2a2017bab796eb5e9ad3f92a6de496b6d280d75f7775f311a979` |

The first three word lists are byte-identical to the ones the sibling languages used;
their SHA-256 values match those recorded in `../go/mapping.json`. `reserved_zig.txt` is
this language's own frozen reserved surface: the 46 grammar keywords taken verbatim from
the toolchain's own tokenizer table, plus its primitive kind names and primitive value
words — 85 entries, the same treatment the sibling languages gave their predeclared
identifiers.

---

## (a) The fixture compiles, runs, and its output matches EXACTLY what the pack claims

`expected_output.txt` was produced **by execution**, never by hand: the real
implementation was built, run, and its stdout redirected into the file. The file's
content is then compared back against a fresh run.

```
$ cd .work/pf01 && zig build-exe solution.zig -O ReleaseSafe -femit-bin=solution
[exit 0]
$ ./solution > stdout.txt; echo exit=$?; cat stdout.txt
exit=0
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
[exit 0]
$ diff -u expected_output.txt .work/pf01/stdout.txt && echo 'stdout byte-identical'
stdout byte-identical to expected_output.txt
[exit 0]
```

`pack_check.py` then closes the loop between the pack and those artifacts:

```
  [PASS] worked example is byte-identical to fixture_anon.zig
  [PASS] claimed output is byte-identical to expected_output.txt
  [PASS] twelve sections present, in order
  [PASS] section 12 self-count matches measurement (245 lines, 10171 characters)
  [PASS] length within the 150-250 line target
  [PASS] all 12 pseudo-words documented in the pack (12 found)
```

So the program printed in §11 of the Reference Pack is not a retyped approximation of the
fixture — it is the same bytes, and the four lines the pack says it prints are the same
bytes the binary actually wrote.

**Reference Pack size.** 245 lines, 10171 characters
(`reference_pack_counts.json`). Sibling packs: go 247, rust 230, java 249, python 252,
cpp 245 lines. The line count sits inside the 150–250 target and inside the sibling
range. The character count is ~5% above the largest sibling (python, 9686); this is a
published property, not a correction: this language's I1 domain is 12 tokens against
go's 9, so §10 of the pack carries three extra table rows, and §9 carries the C-4
paragraph below. No extra explanation was given to make the syntax easier.

**Identity-leak scan (PF-07).**

```
reference_pack.md: 245 lines, 10171 characters
exempt V-role spellings present by design (section 2.6 I1 exemption):
  File, Io, Threaded, flush, import, init_single_threaded, interface, io, print, std, stdout, writer
LEAK SCAN: 0 hits
LEAK SCAN negative control (pack + one line of real keywords): 3 hit(s) -- scan can fail: OK
```

The scan checks every one of the 85 reserved words plus all 12 transformed tokens as
whole words anywhere in the pack, and the toolchain/ecosystem identity terms. Zero hits:
the pack never names the language and contains no real keyword. The V-role spellings that
remain real are the §2.6 I1 exemption, recorded here as the anonymity residual for this
row. The scan's negative control demonstrates that it *can* fail.

*Recorded deviation.* The §2.2 heading for slot P5 is "Expressions and operators". The
conjunction in that heading happens to be a reserved word of **this** language, so the
heading is written "Expressions, operators, precedence". The slot, its contents, and its
position are unchanged; the change is cosmetic and is recorded here rather than left to
be discovered.

---

## (b) Every harness convention the prompt withholds is satisfied BY THE HARNESS

Per §9.1, the pack states everything about the *language* and nothing about this
benchmark's file system and invocation, because those facts identify the toolchain. The
harness carries them, **and the trial is additionally told them in its prompt**, so no
trial can be penalised for not guessing them:

| Withheld convention | Supplied by | Value |
|---|---|---|
| entry filename (H1) | harness, and stated in the prompt | `solution.zig` |
| build command | harness, and stated in the prompt | `zig build-exe solution.zig -O ReleaseSafe -femit-bin=solution` |
| emitted binary name (H6) | harness | `solution` |
| run command | harness | `./solution` |
| working directory | harness | a fresh directory per trial |

H2 (compilation-unit declaration) does not apply: this language requires none. H3 (entry
point inside a named type) does not apply. H4 and H5 do not apply **because the pack
documents the entry-point shape and the failure-propagation marker**, and §9.3 forbids a
fixup from supplying pack-documented material.

**PF-13, the bare-body submission.** A submission built from the pack alone — containing
every element the pack documents (the standard-namespace binding, the visibility marker,
the entry-point shape, its failure-propagating result designation, the output sequence),
in transformed spelling, and omitting only the unstated conventions — was fed through the
real pipeline: extraction → gate → inverse → fixups → build → run → oracle.

```
  [PASS] PF-13 bare-body submission: stage=oracle exit=0
         pack_documented_fixups=[]  unstated=['H1_entry_filename', 'H6_emitted_binary_name']
      stdout:
        SUM 25632
        MAX 935
        EVENS 24
        JOINED 897-558-614-577-405
```

Zero pack-documented fixups were applied, which is the §9.3 requirement. The evidence
file is `preflight_evidence/pf13_bare_body_submission.txt`; the result JSON is
`preflight_evidence/pf13_result.json`.

**Code extraction (§9.2)** exercised with zero, one and several fenced blocks:

```
  [PASS] extractor[zero] blocks=0
  [PASS] extractor[one] blocks=1
  [PASS] extractor[several] blocks=2
```

### Frozen rule C-4 (methodology 00) — applied

This language's current standard-output API differs from the widely-reproduced older
idiom: the 0.16 toolchain replaced the old `std.io` stdout helpers with an explicit `Io`
instance. The task is not about that, so **the Reference Pack states the current spelling
outright**, in §9, flagged so a trial cannot miss it:

> *"This is the current spelling of the output path in this toolchain. Shorter, older
> spellings are widely reproduced elsewhere; this toolchain no longer accepts them, so use
> exactly the shape below."*

followed by the full four-step sequence (instance, handle, buffered writer, write) and the
explicit statement that buffered bytes stay invisible until the flush call, so the
program's last output statement must be that flush. §7 lists each spelling with its
parameters and result. A trial therefore cannot fail this row on API drift.

---

## (c) The round-trip validator can BOTH accept a correct input AND reject a corrupted one

47 checks, 0 failures (`preflight_evidence/result.json`). Every validator was run against
at least one input it must accept and at least one **mutated** input it must reject. The
mutations are recorded below and are reproducible from `validate.py`.

### R1–R5 on the correct pair (must all PASS)

```
  [PASS] positive.R1   byte identity: inverse(forward(F)) == F_nocomments
  [PASS] positive.R2   build identity: the inverse image builds, exit 0
  [PASS] positive.R3   behavioural identity: same stdout as F
  [PASS] positive.R4   non-triviality: forward(F) differs from F
  [PASS] positive.R5   domain containment: changed tokens ⊆ bound tokens
```

### The same machinery on mutated inputs (must all be REJECTED)

| Mutation | What it models | Result |
|---|---|---|
| two pseudo-words transposed (`ganimu` ↔ `zavulu`) | a mapping defect | R1 **fails**, R2∧R3 **fail** — rejected |
| one inserted space deleted | a position-map defect | `POSMAP_MISMATCH` raised — rejected |
| a pseudo-word replaced by an unmapped word (`sebezo`→`sebezx`) | a token that survives transformation as an identifier | R1 **fails**, R2 **fails** — rejected |
| a byte changed inside a **string literal** (`"SUM`→`"SUMX`) | a corruption in the region the transformer must never touch | R1 **fails**, R3 **fails** — rejected |

```
  [PASS] neg1_transposed_pseudo_words.R1        expected=False got=False
  [PASS] neg1_transposed_pseudo_words.R2_or_R3  expected=False got=False
  [PASS] neg2_deleted_inserted_space            expected=False got=False  POSMAP_MISMATCH
  [PASS] neg3_unmapped_word.R1                  expected=False got=False
  [PASS] neg3_unmapped_word.R2                  expected=False got=False
  [PASS] neg4_string_literal_mutated.R1         expected=False got=False
  [PASS] neg4_string_literal_mutated.R3         expected=False got=False
```

### The §4.2a conformance gate — and §10.4's "vacuous test" warning

§10.4 warns that a *"did any transformed token survive?"* test is vacuous **when the
mapping is a permutation of the language's own vocabulary** (the I6 case). For I1 the
mapping is not a permutation — the 12 images are invented six-letter words that are in no
language's reserved list — but the warning is still honoured rather than argued around:
the gate's negative fixture is **correct real source that ignores the transformation
entirely** (PF-05 requirement (b)), i.e. `fixture_real.zig` itself, and the gate must
reject it.

```
  [PASS] positive[transformed_fixture]     expected=True  got=True   ok:12_pseudo_tokens
  [PASS] negative[correct_real_source]     expected=False got=False
         REAL_TOKENS_PRESENT:const,fn,if,main,pub,try,u64,u8,undefined,var,void,while
  [PASS] negative[no_bound_token_at_all]   expected=False got=False  NO_TRANSFORMED_TOKEN_PRESENT
```

The gate rejects a program that is *correct and compiles* purely because it was written in
the real surface. That is the behaviour a gate that cannot fail would lack.

### The `INVERSE_AMBIGUOUS` detector

```
  [PASS] positive[clean_submission]                     clash=[]
  [PASS] negative[variable_named_with_pseudo_word]      clash=['ganimu']
  [PASS] negative[inverse_image_fails_to_build]
         solution.zig:9:9: error: expected 'an identifier', found 'if'
```

The negative is a submission that names a binding `ganimu`. The detector flags it, and the
build of its inverse image is shown failing — which is what makes this a model failure of
type H-INVENT rather than a silent pass. The entry-point name is excluded from the check,
because K22 binds it and its pseudo-word is the pack's own documented spelling.

### The lexer

```
  [PASS] lossless[fixture_real.zig]
  [PASS] lossless[fixture_anon.zig]
  [PASS] lossless[strings/chars/comments/builtins]
  [PASS] strings_untouched_by_forward        string literal intact
  [PASS] builtin_at_name_untouched           @import preserved
  [PASS] rejects[unterminated_string]        expected=False got=False
```

The third case is a probe containing real keywords **inside** a text literal, a character
literal, a line-continued multiline literal and a comment; forward leaves every one of
them untouched, which is what makes "never inside a string literal or an identifier" a
structural property rather than a matter of careful regex authoring. The lexer's negative
control is an unterminated literal, which it raises on rather than silently accepting.

### The lexicalizer (PF-08)

```
  [PASS] determinism_two_processes          two separate processes, byte-identical output
  [PASS] matches_frozen_mapping.json
  [PASS] one_to_one
  [PASS] all_match_^[a-z]{6}$
  [PASS] all_length_6
  [PASS] no_prefix_relation
  [PASS] no_collision_with_reserved_zig
  [PASS] no_collision_with_identifiers
  [PASS] absent_from_task_material
  [PASS] filter_rejects[not_word6]        probe='abc'
  [PASS] filter_rejects[already_used]     probe='beluba'
  [PASS] filter_rejects[en_common]        probe='banana'
  [PASS] filter_rejects[prog_terms]       probe='buffer'
  [PASS] filter_rejects[reserved_union]   probe='return'
  [PASS] filter_rejects[substring]        probe='xreturn'
  [PASS] filter_rejects[task_material]    probe='largest'
  [PASS] filter_accepts[valid_pseudo_word] probe='zumeku'
  [PASS] accept_agrees_with_reject_reasons
```

All seven rejection filters are exercised **in isolation**. `accept()` short-circuits, so
a probe that trips several filters would only ever be attributed to the first; a parallel
non-short-circuiting `reject_reasons()` reports every filter a probe trips, and the two
are checked to agree on the boolean for every probe. Without that, "every filter fires"
would have been an unverifiable claim.

During generation only the `substring` filter actually fired (12 redraws across 12 roles);
that count is recorded in `mapping.json` under `rejection_filters_fired`, and is *not*
inflated by the isolation probes above.

### The oracle

```
  [PASS] positive[reference_solution].built
  [PASS] positive[reference_solution].exit0
  [PASS] positive[reference_solution].stdout
  [PASS] negative[49_terms_instead_of_50]   expected=False got=False  'SUM 24847'
```

The oracle's negative is a program that is otherwise identical but walks 49 terms. It
builds and runs cleanly and is still rejected, because the oracle compares bytes rather
than checking that something was printed.

---

## Scope notes, recorded rather than smoothed over

1. **What is transformed.** The 12 tokens are the 11 reserved words of this language that
   occur in the fixture (`const`, `fn`, `if`, `pub`, `try`, `u64`, `u8`, `undefined`,
   `var`, `void`, `while`) plus the toolchain-fixed entry-point name (`main`, role K22).
   The intersection is recomputed mechanically at validation time from
   `reserved_zig.txt` ∩ fixture word tokens, so the table cannot drift from the fixture.
   `unbound_word_tokens` is empty, as §7.1a requires.
2. **What is not transformed.** V-roles keep their real spelling in I1 (§7.1): `std`,
   `@import`, `Io`, `Threaded`, `init_single_threaded`, `File`, `stdout`, `writer`,
   `interface`, `print`, `flush`. Ordinary user identifiers (`state`, `sum`, `largest`,
   `evens`, `first`, `i`, `term`, `tio`, `io`, `wbuf`, `out`) are never renamed — renaming
   them would add difficulty without removing familiarity.
3. **`V18`.** `{d}` interpolation inside a text literal is `NOT_TRANSFORMABLE`: its syntax
   lies inside a text literal, which §1 consequence 2 never transforms. It consumes no I1
   budget and is excluded from `anonymized_token_count`. It is documented in the fixed P2
   line of the pack.
4. **Two lexer guards specific to this language**, both recorded in `mapping.json`: a word
   immediately after `@` is a compiler builtin and is never substituted in either
   direction (G1); `@"..."` is a quoted identifier whose body lexes as a string and is
   therefore unreachable by substitution (G2).
5. **The emission rule inserted 6 single spaces** into the fixture (recorded in
   `fixture_anon.zig.posmap.json`), every one at a position where a bound token abutted a
   delimiter: `main(` → `bikoli (`, `!void` → `! bogizu`, `]u8` → `] renibu`, `]u64` →
   `] sebezo`, and twice `undefined;` → `beluba ;`. R1 is consequently a plain
   byte-identity test with no canonicalization, exactly as §6.4 demands: a fixture that
   never wrote a bound token against a delimiter could have dodged the whole class of
   defect, so this fixture deliberately contains five such sites.

## Verdict

| §10.4 check | Result |
|---|---|
| (a) fixture compiles, runs, output matches the pack exactly | **PASS** |
| (b) withheld harness conventions supplied by the harness; C-4 applied | **PASS** |
| (c) every validator both accepts a correct input and rejects a corrupted one | **PASS** |

**`all_pass = true`. Nothing blocks the trial.**
