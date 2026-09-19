# Superseded partial probe run (run 1)

These artifacts were produced by the FIRST semantic-compression probe pass, which was **stopped
deliberately and before completion** (55 of 400 probe files) when the adversarial methodology audit
found systematic pro-Quidra bias in the frozen capability universe and counting rules it was built on.

They are preserved as evidence that the run happened and was abandoned, per spec section 32's
prohibition on hiding inconvenient raw data. **They are not used in any score.**

## Why it was stopped

The audit of `01_capability_universe_and_probes.json` returned 4 blockers and 10 majors, including:

- **No probe in the 40-probe universe required a function value or closure** — the one construct Quidra
  cannot express (`FUNCTION_NOT_VALUE`) — and three probes routed around it. Spec section 32 forbids
  removing a capability from Semantic Compression because Quidra lacks it.
- **The document's Quidra-independence claim was false.** Probe F15.P1's canonical task is Quidra's own
  README generics example, and the generated Quidra fragment reproduced it verbatim.
- **Asymmetric toolchain safety posture**: Rust and Zig were pinned to checks-disabled release modes
  while Quidra's only mode is optimized-and-checked, and overflow probes then scored that difference.
- **Determinacy double-counted** inside Capability Coverage, pushing dynamically typed languages to
  PARTIAL support for being dynamic.

Correcting the universe changes which probes exist and how every probe is annotated, so probe sources
and annotations from this pass are not comparable with the corrected universe and are not reused.

Separately, `CORRECTIONS.md` D-4 records a token-rule defect (the one-token interpolation hole) that also
inflated Quidra's Semantic Density and was fixed. Token counts in this superseded run predate that fix.

No score in this benchmark is derived from anything in this directory.
