Language-specific opacity-barrier input for the `rust` configuration.

ADV-09.in
  ADV-09 construction step 2 is a two-branch step:
    "If the language's index type is signed, read -1 directly through the
     opacity barrier. If the language's index type is UNSIGNED, read the value 0
     through the barrier and compute (0 - 1) in that unsigned index type."
  type_binding_table.authoring_choice_table.categories.index_type_construction
  freezes Rust on the UNSIGNED branch:
    "Rust: unsigned index (usize): read 0 as usize and compute 0 - 1".
  The shared file adversarial_src/inputs/ADV-09.in holds "-1", which is the
  SIGNED branch's barrier value (used by Go, Java, Kotlin, Swift, TypeScript,
  Python).  Feeding "-1" to the Rust program makes `parse::<usize>()` fail, so
  the measured event becomes the opacity barrier's own parse rather than the
  case's array lower-bound hazard.  opacity_barrier.R_variant_parsing_rule
  requires that to be corrected as an authoring defect BEFORE any result is
  recorded.  This file supplies the frozen unsigned-branch barrier value, 0.

  Evidence, captured before the correction (rustc 1.95.0, `rustc -O`):
    stdin "-1"  -> ADV-START, then
                   panicked at ADV-09_R.rs:13:49: called `Result::unwrap()` on
                   an `Err` value: ParseIntError { kind: InvalidDigit }
    stdin "0"   -> ADV-START, then
                   panicked at ADV-09_R.rs:16:20: index out of bounds: the len
                   is 5 but the index is 18446744073709551615
  The second is the case's hazard; the first is not.
