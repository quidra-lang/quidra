# Micro suite: correctness gate result

**121 / 121 cells verified.** Every one of the 11 frozen workloads, in every one of the 11 execution
configurations, reproduces the golden output within the frozen tolerance
(`|Δ| ≤ max(1e-9·|expected|, 1e-12)` for floats; exact for integers and checksums).

| Configuration | Verified |
|---|---|
| quidra_native | 11/11 |
| quidra_interpreter | 11/11 |
| python, cpp, rust, go, java, typescript, kotlin, swift, zig | 11/11 each |

Raw record: `raw/micro_verify_all.json`.

## What this gate means

Correctness gates timing (methodology 06 §5.1): nothing is timed until it has reproduced the golden
output. A program that builds and runs but prints a wrong answer is classified a **silent bug**, never a
success, and contributes no timing.

The golden itself does not rest on one implementation: an independent C reference and an independent
Python reference agree **byte-for-byte on all 11 workloads**, including every float printed to 17
significant digits (`reference/PROVENANCE.md`).

## How it got here

The first implementation round passed the gate but failed the fairness audit with **23 blockers**. Those
were not correctness failures — they were measurement-validity failures, and every one would have
corrupted the Performance scores:

- **`steady` mode absent from essentially all 110 programs.** Methodology 06 §5.2 requires every program
  to accept `steady` and run the body K times in-process emitting `ITER <k> <elapsed_ns>`; §5.4 builds
  Long-running Performance from those lines. Without it that metric was unmeasurable. Now present in
  110/110 files and verified to emit real `ITER` lines.
- **Wrong containers**, in both directions: TypeScript used `Int32Array` where §4.12 pins `Float64Array`
  (unfairly fast); Python used a plain `list` where `array('d')` is pinned (unfairly fast).
- **Elided work**: MB-08's pass 5 was implemented in **no language at all** — every implementation did
  less work than the workload defines. Now implemented in all ten.
- **Artificial slowness**: Go hand-rolled `abs` instead of `math.Abs`; TypeScript used
  `fs.createWriteStream`; Swift used a slower parse path. Spec §7 forbids penalising a language this way
  just as firmly as it forbids advantaging one.

Most of these hurt languages **other** than Quidra, which is the point of auditing in both directions.

## Two forced substitutions, recorded rather than hidden

Both are genuine capability facts about Quidra 0.2.0, not implementation choices:

1. **MB-03 has no XOR.** Quidra has no bitwise operators at all; `^` is not even a lexer token. The
   kernel's XOR is synthesised arithmetically under the pinned rule in
   `../methodology/00_cross_language_constraints.md` C-7, and the resulting figure is published with an
   explicit emulation disclosure.
2. **MB-10 cannot use the pinned accumulation.** §4.12(c) pins `content = content + line`; that form
   **crashes** at this workload's size because of the compiler defect documented in
   `../QUIDRA_DEFECT_string_concat_stack_leak.md` (stack leak, SIGSEGV at ~524,288 concatenations). The
   `string[]` + `join` builder is used instead — the language's own documented recommendation — with the
   crash evidence recorded in the source.

Neither is scored as a slow number alone; both are recorded where they belong, as capability and
robustness evidence.
