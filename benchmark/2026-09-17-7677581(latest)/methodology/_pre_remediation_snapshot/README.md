# Pre-remediation methodology snapshot

These are the methodology documents **as first drafted**, before the adversarial audit's findings were
applied. They are preserved so that a third party can see exactly what was corrected and verify that the
corrections were real rather than cosmetic.

## Why they were corrected

An adversarial audit ran one independent auditor per document, instructed to hunt for bias toward
Quidra, mechanical-irreproducibility, and spec non-compliance. The result:

**All 10 of 10 documents were flagged for pro-Quidra bias.**
**Totals: 35 blockers, 87 majors, 56 minors.**

The full findings are preserved beside this directory in `../_audit_findings/`.

Representative defects, each of which would have inflated Quidra's scores:

| # | Defect | Who it favoured |
|---|---|---|
| 1 | No probe in the 40-probe universe required a function value or closure — the single construct Quidra rejects (`FUNCTION_NOT_VALUE`) — and three probes routed around it. Spec §32 forbids removing a capability because Quidra lacks it. | Quidra: a real capability gap worth 0 of 103 points |
| 2 | The universe's "no Quidra documentation was consulted" claim was contradicted by its own probes: F15.P1's task is Quidra's README generics example, and the generated Quidra fragment reproduced it verbatim. | Quidra |
| 3 | Rust and Zig were pinned to checks-disabled release modes (`rustc -O`, `-OReleaseFast`) while Quidra's only mode is optimized-and-checked; overflow probes then scored exactly that difference. | Quidra, against Rust/Zig |
| 4 | Support level required facts be "DETERMINABLE", so dynamically typed languages were pushed to PARTIAL for being dynamic — paying once in Capability Coverage and again in metrics B/D. | against Python/TypeScript |
| 5 | The interpolation hole was charged one token instead of the two delimiters actually written, discounting exactly the five interpolating languages, Quidra among them. | Quidra (fixed separately as CORRECTIONS D-4) |

## Status of these files

**Superseded.** No score in this benchmark is computed from anything in this directory. They exist as
evidence of the correction, in keeping with spec §32's prohibition on hiding inconvenient data.

## Legitimacy of correcting a "frozen" document

Spec §25.4 forbids changing formulas, weights or definitions **after observing results**. At the moment
these corrections were made, **no probe had been scored, no workload had been timed, and no LLM trial had
been run** — the single partial probe pass that had begun was stopped and discarded (see
`../../semantic-compression/_superseded_run1/`). Correcting the methodology at this point is
pre-registration, not post-result formula selection. The checksums below fix the "before" state so the
distinction is auditable rather than asserted.
