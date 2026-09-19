# I1: measured anonymity residual — the condition did not hold equally for all ten languages

**This is a finding about the evaluation, recorded because it bears on how the I1 scores may be read.**

## What was measured

Each I1 trial was asked, as a diagnostic that does not affect its score, which real language it believed
"Language A" to be. The frozen design (§10.1) already concedes that I1 "does not claim to mathematically
remove pretraining" and that "structural similarities, general programming knowledge, tokenizer behavior,
and learned abstractions cannot be erased completely". The question is how much residual there is.

## Result

| Underlying language | Model's guess | Identified? |
|---|---|---|
| Python | Python | ✅ |
| Go | Go | ✅ |
| Rust | Rust | ✅ |
| Zig | Zig | ✅ |
| Swift | Swift | ✅ |
| TypeScript | TypeScript | ✅ |
| C++ | C++ | ✅ |
| Java | Java | ✅ |
| Kotlin | Kotlin | ✅ |
| **Quidra** | **"unknown"** | ❌ |

**Nine of ten were identified correctly from structure alone, after every keyword had been replaced by a
controlled pseudo-word.** Keyword anonymization removed the lexical surface but not the grammatical
signature: indentation-and-colon layout, brace-and-semicolon layout, `::` paths, trait/impl shapes,
`@`-builtins and so on survive the transformation and are individually diagnostic.

## Why this matters, and in which direction

I1's stated purpose is to "reduce the advantage of having seen a language many times before". On this
evidence it did **not** achieve that for the nine established languages: a model that has identified the
underlying language can fall back on everything it already knows about it, using the pack only as a
lexical decoder ring.

For Quidra it did — but for a reason that has nothing to do with the transformation. Quidra was already
unfamiliar; the anonymization added little. **So I1 as executed compares "nine familiar languages wearing
a thin disguise" against "one genuinely unfamiliar language", which is not the controlled comparison the
design intends.**

The direction of the resulting bias is **not determinable from this run** and must not be asserted:

- It could **favour Quidra**: the nine are handicapped relative to their true fluency, having to decode
  pseudo-words for constructs they know cold, while Quidra's trial is no worse off than a normal
  Quidra task.
- It could **disadvantage Quidra**: the nine retain their full structural priors and merely translate,
  while Quidra's trial must learn genuinely unfamiliar semantics from the pack alone.

Both readings are consistent with the data. With every language scoring Correct@1 = 1/1, the measurement
has no resolving power to distinguish them.

## Consequence for reporting

1. The I1 scores are published with this residual attached. They are **not** presented as a
   familiarity-controlled comparison, because the control demonstrably did not hold for nine of ten.
2. The §10.7 familiarity-drop diagnostic (Practical baseline − transformed) is reported as a raw
   diagnostic only, per the frozen rule that it "must not be used as a correction factor".
3. Every language scored Correct@1 on I1 at **one trial per cell**. Per §6.2 a single sample does not
   carry the precision of five, and no difference between languages is claimed from this measurement.

## What would fix it

§10.5's I3 (structural surface perturbation) is the condition designed to attack exactly this residual —
it perturbs grouping delimiters, block markers and declaration separators rather than words. Running I1
without I3 leaves the structural signature intact. A future run should treat I1 alone as insufficient
evidence of familiarity control, and should record the identification rate (this table) as a standing
validity check on every transformed condition.
