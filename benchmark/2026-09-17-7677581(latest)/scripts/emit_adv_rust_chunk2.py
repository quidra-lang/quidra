#!/usr/bin/env python3
"""Assemble standard/raw/adversarial/rust/chunk_2.json from the captured build/run evidence."""
import json, os, hashlib, subprocess

BASE = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC  = os.path.join(BASE, "standard/src/adversarial/rust")
INP  = os.path.join(BASE, "standard/src/adversarial/inputs")
W    = os.path.join(BASE, "work/adv_rust_chunk2")
OUT  = os.path.join(BASE, "standard/raw/adversarial/rust/chunk_2.json")

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

def raw(stem):
    return json.load(open(os.path.join(W, "raw_%s.json" % stem)))

def obs_of(stdout):
    for l in stdout.split("\n"):
        if l.startswith("OBS="):
            return l[4:]
    return None

def runs_summary(rec):
    out = []
    for i, r in enumerate(rec["runs"]):
        out.append({"index": i, "exit_status": r["exit"], "signal": r["signal"],
                    "timed_out": r["timed_out"], "wall_seconds": r["wall_seconds"],
                    "adv_start_present": "ADV-START" in r["stdout"],
                    "adv_end_present": "ADV-END" in r["stdout"],
                    "obs_payload": obs_of(r["stdout"])})
    return out

RECIPE_NOTE = ("Frozen primary recipe environment.json frozen_toolchain_recipes.rust: `rustc -O FILE.rs -o BIN`. "
               "`-O` is `-C opt-level=2` and leaves `debug-assertions` OFF, which is the check posture disclosed in "
               "toolchain_binding.check_posture_disclosure. rustc 1.95.0 (59807616e 2026-04-14) (Homebrew).")

def build_block(rec):
    b = rec["build"]
    return {"cmd": b["cmd"], "exit": b["exit"], "stdout": b["stdout"], "stderr": b["stderr"],
            "timed_out": b["timed_out"], "wall_seconds": b["wall_seconds"],
            "artifact_produced": b["artifact_produced"], "recipe_note": RECIPE_NOTE}

def run_block(rec, stdin_path):
    if not rec["runs"]:
        return {"cmd": None, "exit": None, "stdout": "", "stderr": "", "timed_out": False,
                "note": "Not run: the frozen build recipe rejected the program, so no artifact existed."}
    r = rec["runs"][0]
    return {"cmd": ["./prog"], "resolved_binary": r["cmd"][0], "stdin": stdin_path,
            "exit": r["exit"], "signal": r["signal"], "stdout": r["stdout"], "stderr": r["stderr"],
            "timed_out": r["timed_out"], "wall_seconds": r["wall_seconds"]}

OBSERVATIONS = []

def add(case_variant_id, case_id, variant, stem, stdin_file, clr, extra):
    rec = raw(stem)
    src = os.path.join(SRC, stem + ".rs")
    o = {"case_variant_id": case_variant_id, "case_id": case_id, "variant": variant,
         "language": "Rust", "language_key": "rust", "configuration": "primary",
         "program": src, "source_sha256": sha(src),
         "construction_line_range": {"first_line": clr[0], "last_line": clr[1]},
         "build": build_block(rec),
         "run": run_block(rec, os.path.join(INP, stdin_file) if stdin_file else "/dev/null"),
         "runs_all_5": runs_summary(rec)}
    o.update(extra)
    OBSERVATIONS.append(o)

A3_NOTE = ("`.unwrap()` appears on the stdin read / parse of the opacity barrier. It is the canonical "
           "shortest-direct-use form authoring rule A3 fixes for Rust, not an error handler: it consumes the "
           "Result without inspecting the failure channel. No catch, no match, no recover, no guard, no range "
           "check and no validation appears in any program of this chunk.")

add("ADV-14/R", "ADV-14", "R", "ADV-14_R", "ADV-14.in", (4, 19), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": ("none. Rust forced nothing: the program was rejected before it could run. " + A3_NOTE),
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error[E0308]: mismatched types",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case:L_TYPE", "global:L_TYPE"],
                   "pattern": "TypeError|type mismatch|mismatched types|incompatible type|cannot apply|unsupported operand|expected .* but (got|found)|expected .*, found",
                   "matching_text": "mismatched types / expected `i64`, found `String`"},
 "diagnostic_has_file_line_column": True,
 "classification_reasoning": ("D0 does not apply: Rust has i64 and String, so no TM3 capability-absence "
   "determination was made. D1 fires: the frozen build recipe exited 1. D1a does not fire: rustc exited "
   "normally with status 1 and no toolchain_failure_markers regex matches the build output. D1b-i fires: the "
   "build output matches the case lexicon L_TYPE (`mismatched types`, and also `expected .*, found`). The "
   "diagnostic is at ADV-14_R.rs:17:24, inside construction_line_range 4-19, i.e. on the call in construction "
   "step 3. Stage = Compile-time Detection (100). Rust's type system refuses to pass a String to an i64 "
   "parameter; the R variant's opacity barrier is irrelevant because the mismatch is in the declared types, "
   "not in the value."),
 "silent_transformation": False,
 "silent_transformation_watch_result": "R:777 (text repeated three times) is impossible in Rust: `String * 3` is not even expressible and the call never compiles."})

add("ADV-15", "ADV-15", "single", "ADV-15", None, (8, 17), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The program was rejected before it could run.",
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error[E0502]: cannot borrow `xs` as mutable because it is also borrowed as immutable",
 "lexicon_match": {"rule": "D1b-i", "matched": ["global:L_IMMUTABLE"],
                   "pattern": "cannot assign|immutable|read-only|readonly|Assignment to constant variable|final variable|val cannot be reassigned|cannot mutate",
                   "matching_text": "borrowed as immutable / immutable borrow occurs here"},
 "diagnostic_has_file_line_column": True,
 "flags": ["case_lexicon_unmatched_global_matched"],
 "case_lexicon_note": ("The case lexicon L_MUTATE (ConcurrentModificationException | concurrent modification | "
   "mutated while | modified while iterating | changed size during iteration | collection was mutated) does NOT "
   "match. Rust does not phrase this as concurrent modification because it never reaches run time: it is a "
   "borrow-check rejection. D1b-i is satisfied by the global fragment L_IMMUTABLE, which the rule accepts "
   "explicitly (`or at least one fragment of global_hazard_diagnostic_lexicon.fragments`). Published here so "
   "the mismatch can be re-checked."),
 "classification_reasoning": ("D0 does not apply: Vec<i64> is growable and the append is expressible, so the "
   "hazard genuinely arises. D1 fires: build exited 1. D1a does not fire: normal exit 1, no toolchain marker. "
   "D1b-i fires on the global fragment L_IMMUTABLE. The diagnostic is at ADV-15.rs:13:13, the `xs.push(99)` of "
   "construction step 3, inside construction_line_range 8-17. Stage = Compile-time Detection (100). Rust's "
   "borrow checker makes mutate-while-iterating a compile error rather than a runtime surprise, so neither of "
   "the two silent outcomes the case warns about (ITERS:5|LEN:6 and ITERS:6|LEN:6) can occur."),
 "silent_transformation": False})

add("ADV-16", "ADV-16", "single", "ADV-16", None, (8, 11), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The program was rejected before it could run.",
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error[E0384]: cannot assign twice to immutable variable `v`",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case:L_IMMUTABLE", "global:L_IMMUTABLE"],
                   "pattern": "cannot assign|immutable|read-only|readonly|Assignment to constant variable|final variable|val cannot be reassigned|cannot mutate",
                   "matching_text": "cannot assign twice to immutable variable"},
 "diagnostic_has_file_line_column": True,
 "authoring_choice": ("authoring_choice_table.strongest_immutable_binding_form, Rust = `let` without `mut`. "
   "That is the frozen construct and it is what line 8 uses; construct_absent is false."),
 "classification_reasoning": ("D0 does not apply: Rust has the construct. D1 fires (exit 1); D1a does not "
   "(normal exit, no toolchain marker); D1b-i fires on the case lexicon L_IMMUTABLE. Diagnostic at "
   "ADV-16.rs:9:5, the assignment of construction step 2, inside construction_line_range 8-11. Stage = "
   "Compile-time Detection (100). The silent outcome the case watches for, V:20, cannot occur in Rust: "
   "immutability of a `let` binding is a rule, not a convention."),
 "silent_transformation": False})

add("ADV-17", "ADV-17", "single", "ADV-17", None, (8, 11), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": ("none in the program. Rust nevertheless FORCES the distinction structurally: the "
   "declared type is Option<String>, and there is no way to reach String::len through it without an explicit "
   "unwrapping step, which prohibition P1 and construction step 2 both forbid. Being unable to write the "
   "hazard at all is the language's safety behaviour here."),
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error[E0624]: method `len` is private",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case:L_NULL", "global:L_NULL"],
                   "pattern": "\\bnull\\b|\\bnil\\b|\\bnone\\b|NullPointerException|nil pointer dereference|unexpectedly found nil|Cannot read propert(y|ies) of null|NoneType|unwrap.*None|attempted to unwrap",
                   "matching_text": "consider using `Option::expect` to unwrap the `String` value, panicking if the value is an `Option::None`"},
 "diagnostic_has_file_line_column": True,
 "diagnostic_quality_note": ("The headline wording is oblique: core::option::Option has a PRIVATE inherent "
   "`len`, so rustc reports E0624 `method `len` is private` rather than naming nullability directly. The "
   "L_NULL match comes from the help note, not the headline. This is recorded because it bears on the "
   "Diagnostics metric; it does not affect the stage, which D1b-ii-2 would have produced anyway (the "
   "diagnostic is at ADV-17.rs:9:22, inside construction_line_range 8-11)."),
 "classification_reasoning": ("D0 does not apply: Option<String>::None is the frozen null_or_absent_value "
   "binding for Rust and the member access is written exactly as authoring_choice_table.string_length_member "
   "prescribes (`x.len()` written directly on the Option<String> binding, no unwrap, no null test, no "
   "chaining). D1 fires (exit 1); D1a does not; D1b-i fires on the case lexicon L_NULL. Stage = Compile-time "
   "Detection (100). The silent outcome LEN:0 cannot occur."),
 "silent_transformation": False})

add("ADV-18", "ADV-18", "single", "ADV-18", "ADV-18.in", (4, 21), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": ("none for the hazard. " + A3_NOTE),
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error[E0317]: `if` may be missing an `else` clause",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case:L_TYPE", "global:L_TYPE"],
                   "pattern": "TypeError|type mismatch|mismatched types|incompatible type|cannot apply|unsupported operand|expected .* but (got|found)|expected .*, found",
                   "matching_text": "expected `i64`, found `()`"},
 "diagnostic_has_file_line_column": True,
 "classification_reasoning": ("D0 does not apply. D1 fires (exit 1); D1a does not (normal exit 1, no "
   "toolchain marker); D1b-i fires: the build output matches L_TYPE via `expected .*, found` "
   "(`expected `i64`, found `()``). The case also lists L_RETURN, which does not match Rust's wording. "
   "Diagnostic at ADV-18.rs:5:5, the body of `pick` from construction step 1, inside construction_line_range "
   "4-21. Stage = Compile-time Detection (100). The opacity barrier did not matter: rustc rejects the "
   "function definition itself, before any question of which path the boolean selects. The silent outcome "
   "R:0 cannot occur."),
 "silent_transformation": False})

# --- ADV-19: 5 primary runs were captured separately (each run consumes the full 60s timeout).
ADV19_RUNS = "/tmp/adv19_5runs.json"

add("ADV-19", "ADV-19", "single", "ADV-19", "ADV-19.in", (4, 19), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": ("none for the hazard. " + A3_NOTE),
 "class": "Output Verification",
 "earliest_observable_stage": "Output Verification",
 "sublabel": "nontermination",
 "build_warning_verbatim": "warning: function cannot return without recursing",
 "build_warning_note": ("rustc's `unconditional_recursion` lint fired at ADV-19.rs:4:1 but is warn-by-default, "
   "so the build exited 0 and D1 could not fire. Recorded because it is a genuine static observation that the "
   "frozen recipe did not act on; under the recording rule it does not change the stage."),
 "classification_reasoning": ("D0 does not apply. D1 does not fire: the build exited 0 (one warning). D2 does "
   "not fire: the binary was produced. D3 does not fire: ADV-START IS present on stdout. D4 fires: all primary "
   "runs exceeded the frozen 60-second run timeout with `ADV-START` written and nothing further. Stage = "
   "Output Verification (40), sublabel nontermination, exactly as the case's silent_transformation_watch "
   "anticipates for a run that passes the timeout. D5, D6 and D7 are unreachable."),
 "optimizer_finding": ("Disassembly of the frozen -O binary shows rustc/LLVM replaced the unbounded "
   "non-tail recursion with an unconditional branch to itself: `0000000100000e50  b  0x100000e50`. The "
   "function `f` was inlined out of existence; no stack frame is pushed, no stack is consumed, and no "
   "diagnostic can ever be emitted. Confirmed live with ps during a timed-out run: 100.0% CPU, RSS steady at "
   "896 KB with no growth, i.e. a tight spin with no stack consumption at all. This is why Rust reaches the "
   "40-point rung here instead of the 75-point rung its runtime would otherwise give."),
 "secondary_non_scoring_observation": {
   "build": "rustc -g -C debug-assertions=on ADV-19.rs -o BIN (secondary_non_scoring_configurations.Rust)",
   "env": "RUST_BACKTRACE=1",
   "run_exit": -6, "signal": 6,
   "stdout": "ADV-START\n",
   "stderr": "\nthread 'main' (18152443) has overflowed its stack\nfatal runtime error: stack overflow, aborting\n",
   "would_have_matched": "L_STACK (`has overflowed its stack`, `stack overflow`) -> would be Runtime Safe Detection (75)",
   "effect_on_stage": ("NONE. classification_procedure.earliest_observable_stage_recording_rule."
     "secondary_configurations_never_set_the_stage and .no_retroactive_promotion both apply; D7b is the only "
     "route by which a secondary configuration may move a stage and D7 was never reached. The primary "
     "recipe's behaviour is what is recorded. Published as the disclosure "
     "toolchain_binding.check_posture_disclosure requires.")},
 "silent_transformation": False,
 "silent_transformation_watch_result": ("The case warns that an optimizer may remove the recursion and produce "
   "either D6a (exit 0, no OBS) or an arbitrary integer. Neither happened: LLVM kept the non-termination "
   "rather than discarding it, so the program hangs instead of lying. No value was produced, so nothing was "
   "silently transformed.")})

add("ADV-20", "ADV-20", "single", "ADV-20", "ADV-20.in", (4, 22), {
 "observed_payload": "R:1000000",
 "reached_observation_point": True,
 "forced_error_handling": ("none for the hazard. " + A3_NOTE),
 "class": "Prevented By Construction",
 "earliest_observable_stage": "Prevented By Construction",
 "classification_reasoning": ("D0 does not apply (no TM3 determination; Rust has i64 and ordinary recursion). "
   "D1/D2 do not fire: build exited 0 with no warning and produced a binary. D3 does not fire. D4 does not "
   "fire: every run completed in under 0.4 s. D5 does not fire: every run exited 0. D6 is entered; D6a does "
   "not fire (ADV-START, OBS= and ADV-END all present); D6b FIRES: obs_payload `R:1000000` equals the case's "
   "`reference_observation` `R:1000000` byte for byte, and that reference is a concrete value, not the "
   "DIAGNOSED_FAILURE sentinel. Stage = Prevented By Construction (100). The evidence_requirement is met: the "
   "source at ADV-20.rs:4-9 genuinely writes the specified non-tail recursion with the mandatory `1 +` after "
   "the call, and the observation matches the reference exactly."),
 "optimizer_finding": ("Full disclosure of HOW the 100 was earned: at -O, LLVM did not execute a million "
   "frames. It solved the recursion in closed form. The emitted arm64 for main contains "
   "`mov w8, #0x4240 ; movk w8, #0xf, lsl #16 ; sub x8, x8, x11` - i.e. it computes 1000000 - n directly and "
   "prints it. First run 0.38 s (process start), runs 2-5 measured 0.00 s. Under the secondary "
   "checked/unoptimized configuration `rustc -g -C debug-assertions=on` the SAME source aborts with signal 6 "
   "and `thread 'main' has overflowed its stack / fatal runtime error: stack overflow, aborting` after "
   "printing only ADV-START, which would be Runtime Safe Detection (75) under L_STACK. The frozen primary "
   "recipe is what is scored (no_retroactive_promotion), and D7b cannot apply because D6b fired, but the "
   "divergence is published here because it means Rust's 100 on this row is an optimizer property, not a "
   "stack property: rustc's default 8 MiB main-thread stack does NOT hold a million frames of this function."),
 "secondary_non_scoring_observation": {
   "build": "rustc -g -C debug-assertions=on ADV-20.rs -o BIN",
   "env": "RUST_BACKTRACE=1",
   "run_exit": -6, "signal": 6,
   "stdout": "ADV-START\n",
   "stderr": "\nthread 'main' (18152232) has overflowed its stack\nfatal runtime error: stack overflow, aborting\n",
   "effect_on_stage": "NONE (see optimizer_finding)."},
 "silent_transformation": False})

add("ADV-21", "ADV-21", "single", "ADV-21", None, (8, 10), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The compiler died before producing an artifact.",
 "class": "Crash",
 "earliest_observable_stage": "Crash",
 "sublabel": "toolchain_crash",
 "generator": "scripts/gen_adv21_rust.py, frozen_generators.parser_nesting, DEPTH = 100000",
 "first_diagnostic_verbatim": "error: rustc interrupted by SIGBUS, printing backtrace",
 "classification_reasoning": ("D0 does not apply. D1 fires: the build exited non-zero. D1a FIRES: the build "
   "process died by a fatal signal - rustc terminated with SIGBUS (signal 10, returncode -10) after 0.15 s, "
   "and its own signal handler printed a backtrace that is a pure recursion cycle inside "
   "rustc_parse::lexer::tokentrees::Lexer::lex_token_trees, ending `### recursed 50 times` and `note: we "
   "would appreciate a report at https://github.com/rust-lang/rust`. That is the compiler failing rather than "
   "diagnosing. Stage = Crash (20), sublabel toolchain_crash. D1b is never reached, so the incidental lexicon "
   "hit on the word `depth` (from `maximum backtrace depth reached`, part of the crash backtrace, not a "
   "diagnosis of the input) does not and must not promote this row. This must also be reported under "
   "Compiler / Interpreter Robustness."),
 "secondary_depth_probes_non_scoring": [
   {"depth": 1000, "build_exit": 0, "build_wall_seconds": 0.84,
    "build_warning": "warning: unnecessary parentheses around assigned value",
    "run_exit": 0, "obs_payload": "V:1", "outcome": "compiles and runs correctly"},
   {"depth": 10000, "build_exit": -10, "signal": 10, "build_wall_seconds": 0.03,
    "first_diagnostic_verbatim": "error: rustc interrupted by SIGBUS, printing backtrace",
    "crash_site": "rustc_parse::parser::expr::Parser::parse_expr_catch_underscore (parser recursion), cycle period 5",
    "outcome": "rustc crashed"}],
 "toolchain_limit_finding": ("rustc's parenthesis-nesting limit on this host lies between 1000 (fine) and "
   "10000 (SIGBUS). At the frozen depth of 100000 the crash moves earlier still, into the token-tree LEXER "
   "rather than the expression parser, so rustc 1.95.0 dies before it can produce any diagnosis of the input. "
   "rustc has no graceful `recursion limit reached` path for delimiter nesting."),
 "silent_transformation": False,
 "silent_transformation_watch_result": "No binary was produced, so the misparse-silently outcome did not occur."})

V22 = raw("ADV-22-valid")
VALID_NOTE = {
  "file": os.path.join(SRC, "ADV-22-valid.rs"),
  "sha256": sha(os.path.join(SRC, "ADV-22-valid.rs")),
  "length_bytes": len(open(os.path.join(SRC, "ADV-22-valid.rs"), "rb").read()),
  "authoring_precondition": ("frozen_generators.malformed_source.authoring_precondition: the valid file must "
    "contain no string literal and no comment spanning its midpoint byte. L = 295, midpoint byte 147 is the "
    "`:` of `let v: i64 = 1;`. The file contains no comment at all, and byte 147 is outside every string "
    "literal. Precondition satisfied."),
  "build_exit": V22["build"]["exit"],
  "run_exit": V22["runs"][0]["exit"] if V22["runs"] else None,
  "obs_payload": obs_of(V22["runs"][0]["stdout"]) if V22["runs"] else None,
  "note": "Non-scoring. Confirms the unmutated file builds and runs correctly, as construction step 1 requires."}

add("ADV-22a", "ADV-22", "single", "ADV-22a", None, (1, 10), {
 "sub_program": "ADV-22a",
 "mutation": "first floor(0.60 * 295) = 177 bytes of ADV-22-valid.rs; nothing appended",
 "generator": "scripts/gen_adv22.py (frozen)",
 "valid_file_evidence": VALID_NOTE,
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The program was rejected before it could run.",
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error: this file contains an unclosed delimiter",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case pattern"],
                   "pattern": "syntax error|parse error|unexpected|expected|unterminated|SyntaxError|error:",
                   "matching_text": "error: this file contains an unclosed delimiter"},
 "diagnostic_has_file_line_column": True,
 "classification_reasoning": ("D0 does not apply. D1 fires: build exited 1. D1a does not fire: rustc exited "
   "normally and no toolchain_failure_markers regex matches. D1b-i fires on the case lexicon. The diagnostic "
   "points at ADV-22a.rs:10:19 and additionally back-references the unclosed `fn main() {` at line 3. Stage = "
   "Compile-time Detection (100). rustc diagnosed a truncated file cleanly and did not crash."),
 "silent_transformation": False})

add("ADV-22b", "ADV-22", "single", "ADV-22b", None, (1, 14), {
 "sub_program": "ADV-22b",
 "mutation": "bytes 0x40 0x23 0x24 ('@#$') inserted at byte offset floor(295/2) = 147 of ADV-22-valid.rs",
 "generator": "scripts/gen_adv22.py (frozen)",
 "valid_file_evidence": VALID_NOTE,
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The program was rejected before it could run.",
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error: expected binding pattern, found `#`",
 "lexicon_match": {"rule": "D1b-i", "matched": ["case pattern", "global:L_TYPE"],
                   "pattern": "syntax error|parse error|unexpected|expected|unterminated|SyntaxError|error:",
                   "matching_text": "error: expected binding pattern, found `#`"},
 "diagnostic_has_file_line_column": True,
 "classification_reasoning": ("D0 does not apply. D1 fires (exit 1); D1a does not (normal exit, no toolchain "
   "marker); D1b-i fires on the case lexicon. The injection landed inside `let v: i64 = 1;`, producing "
   "`let v@#$: i64 = 1;`, and rustc points at ADV-22b.rs:8:11, the exact injected byte. Stage = Compile-time "
   "Detection (100)."),
 "silent_transformation": False,
 "case_aggregation_note": ("ADV-22's case stage score is the unweighted mean of ADV-22a and ADV-22b "
   "(fixed_scored_case_variant_list, sub_program_aggregation): (100 + 100) / 2 = 100.")})

add("ADV-23", "ADV-23", "single", "ADV-23", None, (8, 10), {
 "observed_payload": None,
 "reached_observation_point": False,
 "input_file": {"path": os.path.join(INP, "ADV-23.bin"),
                "sha256": sha(os.path.join(INP, "ADV-23.bin")),
                "bytes_hex": "41 C3 28 ED A0 80 F4 90 80 80 80 42 0A"},
 "forced_error_handling": ("YES, and it is the measurement. Rust's ordinary whole-file text read, "
   "std::fs::read_to_string, returns Result<String, std::io::Error> because String is defined to hold only "
   "valid UTF-8, so the language will not hand the author a String without first making the author confront "
   "the failure channel. Authoring rule A3 fixes the shortest direct use for Rust as `.unwrap()`, which is "
   "what the program writes: it does not inspect the error, it just takes the value. There is no catch, "
   "match, recover or validation. The forced construct is the Result itself, not a handler."),
 "length_member_choice": ("String::len (byte length). Rust's std::string::String documents len as `Returns "
   "the length of this String, in bytes`; it is the only method called `len` on String and is the direct "
   "reading of observation_spec CP:<reported string length>. Never evaluated, because step 1 failed."),
 "class": "Runtime Safe Detection",
 "earliest_observable_stage": "Runtime Safe Detection",
 "first_diagnostic_verbatim": "called `Result::unwrap()` on an `Err` value: Error { kind: InvalidData, message: \"stream did not contain valid UTF-8\" }",
 "lexicon_match": {"rule": "D5a", "matched": ["case:L_UTF8", "global:L_UTF8"],
                   "pattern": "utf-?8|UnicodeDecodeError|invalid (byte sequence|utf|encoding|unicode)|malformed input|not valid UTF-8|FromUtf8Error|decoding",
                   "matching_text": "stream did not contain valid UTF-8"},
 "classification_reasoning": ("D0 does not apply. D1/D2 do not fire: the build exited 0, silently, and "
   "produced a binary. D3 does not fire: ADV-START is present. D4 does not fire (0.0 s). D5 is entered: "
   "ADV-START present and the process terminated abnormally with exit status 101, Rust's panic exit code. "
   "D5a FIRES: stderr matches the CASE lexicon L_UTF8 on `valid UTF-8`. Per the D5a note, a controlled abort "
   "accompanied by a runtime diagnostic naming the hazard is Runtime Safe Detection, not Crash: the panic "
   "message names the file, line and column (ADV-23.rs:8:66) and the specific fault. Stage = Runtime Safe "
   "Detection (75). All 5 runs behaved identically apart from the thread id in the message."),
 "silent_transformation": False,
 "silent_transformation_watch_result": ("The case calls this the emblematic silent-transformation case: a "
   "language that substitutes U+FFFD or stores the bytes unvalidated returns an ordinary small integer. Rust "
   "does neither. String cannot represent the invalid bytes, so the read fails loudly and no CP: value is "
   "ever produced. No silent transformation.")})

add("ADV-24", "ADV-24", "single", "ADV-24", None, (8, 10), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": "none. The program was rejected before it could run.",
 "type_binding_used": "default_arithmetic_type, Rust = i64 (type_binding_table.bindings.default_arithmetic_type)",
 "class": "Compile-time Detection",
 "earliest_observable_stage": "Compile-time Detection",
 "first_diagnostic_verbatim": "error: literal out of range for `i64`",
 "lexicon_match": {"rule": "D1b-i",
                   "matched": ["case:literal out of range|integer literal is too large|constant overflow|number too large|cannot be represented",
                               "case:L_OVERFLOW", "global:L_OVERFLOW", "global:L_NARROW"],
                   "matching_text": "literal out of range for `i64` / the literal `9223372036854775809` does not fit into the type `i64` whose range is `-9223372036854775808..=9223372036854775807`"},
 "diagnostic_has_file_line_column": True,
 "classification_reasoning": ("D0 does not apply. D1 fires: build exited 1. D1a does not fire. D1b-i fires on "
   "the case's own pattern `literal out of range` and on L_OVERFLOW. Diagnostic at ADV-24.rs:8:18, the "
   "literal of construction step 1, inside construction_line_range 8-10. Stage = Compile-time Detection "
   "(100). Note this is a lint (`#[deny(overflowing_literals)]` on by default), not a hard type error, but "
   "it is deny-by-default under the frozen recipe with no suppression, so the build genuinely exits 1; "
   "prohibition P3 forbade adding any `#[allow]`, and none was added."),
 "silent_transformation": False,
 "silent_transformation_watch_result": ("The silent outcome the case watches for, V:9223372036854775808 (the "
   "literal rounded to the nearest binary64 value), cannot occur: Rust's default arithmetic type is an "
   "integer type and rustc refuses the literal outright rather than rounding or wrapping it.")})

add("ADV-25/R", "ADV-25", "R", "ADV-25_R", "ADV-25.in", (9, 15), {
 "observed_payload": None,
 "reached_observation_point": False,
 "forced_error_handling": ("YES, and it is the measurement. str::parse returns Result<i64, ParseIntError>; "
   "Rust will not produce an i64 from text without the author passing through that Result. "
   "authoring_choice_table.primary_string_to_integer_facility fixes Rust's facility as "
   "`text.trim().parse::<i64>().unwrap()`, which is exactly line 11. `.unwrap()` is rule A3's canonical "
   "shortest direct use: the failure channel is NOT inspected, it is discarded. No catch, match, recover, "
   "`unwrap_or`, `ok()` or validation appears."),
 "parsing_facility_citation": ("std::primitive::str::parse -> core::str::FromStr for i64. The Rust standard "
   "library documents `parse` as `Parses this string slice into another type`, and it is the facility a "
   "first-time reader of the documentation reaches for. Frozen in authoring_choice_table."),
 "class": "Runtime Safe Detection",
 "earliest_observable_stage": "Runtime Safe Detection",
 "first_diagnostic_verbatim": "called `Result::unwrap()` on an `Err` value: ParseIntError { kind: InvalidDigit }",
 "lexicon_match": {"rule": "D5a", "matched": ["case:L_PARSE", "global:L_PARSE"],
                   "pattern": "invalid (digit|number|literal|syntax)|ParseIntError|NumberFormatException|ValueError: invalid literal|cannot parse|not a valid|parse error|strconv",
                   "matching_text": "ParseIntError { kind: InvalidDigit }"},
 "classification_reasoning": ("D0 does not apply. D1/D2 do not fire: build exited 0 silently and produced a "
   "binary. D3 does not fire: ADV-START present. D4 does not fire. D5 is entered: ADV-START present, process "
   "exited 101. D5a FIRES on the case lexicon L_PARSE (`ParseIntError`, and `InvalidDigit` also matches "
   "`invalid (digit...)` case-insensitively). The panic names the file, line and column "
   "(ADV-25_R.rs:11:45). Stage = Runtime Safe Detection (75). All 5 runs identical apart from thread id."),
 "silent_transformation": False,
 "silent_transformation_watch_result": ("Neither of the two silent outcomes the case predicts occurred. R:24 "
   "(leading digits parsed, rest discarded) is impossible: Rust's FromStr for integers rejects any trailing "
   "non-digit rather than stopping at it. R:0 (zero returned beside an ignored error value) is impossible: "
   "the error is not a second return value, it is the only thing in the Result, so there is no zero to take.")})

add("ADV-26/R", "ADV-26", "R", "ADV-26_R", "ADV-26.in", (9, 32), {
 "observed_payload": "IDX:-1",
 "reached_observation_point": True,
 "forced_error_handling": ("none for the hazard. " + A3_NOTE + " Every operation in the search - the "
   "comparisons, the integer division, the indexing - is an ordinary unqualified Rust operation with no "
   "checked_/wrapping_/saturating_ form and no bounds test added."),
 "index_type_note": ("lo, hi, mid and result are i64, because construction step 3 requires hi to take the "
   "value -1 and result to render as -1. `xs[mid as usize]` is the ordinary Rust spelling of indexing with a "
   "signed loop bound. Using usize would have changed the algorithm (0usize - 1 does not terminate the "
   "loop), which authoring rule A1 forbids."),
 "class": "Silent Bug",
 "earliest_observable_stage": "Silent Bug",
 "reference_observation": "IDX:4",
 "classification_reasoning": ("D0 does not apply. D1/D2 do not fire: build exited 0 silently, binary "
   "produced. D3 does not fire. D4 does not fire (0.0 s). D5 does not fire: all 5 runs exited 0. D6 is "
   "entered. D6a does not fire: ADV-START, OBS= and ADV-END are all present. D6b does not fire: the payload "
   "`IDX:-1` is not byte-equal to the case's reference_observation `IDX:4`. D6c is evaluated: the frozen "
   "plausible_domain_predicate `^IDX:(-1|[0-6])$` matches `IDX:-1`, so the predicate is TRUE and D6c-ii "
   "sends this to D7. D7a does not fire: all 5 primary runs produced the byte-identical payload `IDX:-1`. "
   "D7b does not fire: the secondary checked configuration `rustc -g -C debug-assertions=on` (no -O) "
   "produced the SAME payload `IDX:-1`, exit 0, empty stderr - no different payload and no report naming "
   "undefined, illegal or erroneous behaviour. D7c does not fire: every operation performed is fully defined "
   "by the Rust reference; there is no clause making a binary search over an unsorted slice undefined, and "
   "no index ever went out of bounds. D7d therefore fires. Stage = Silent Bug (0). normative_status: "
   "`defined` - the Rust Reference defines integer comparison, integer division and slice indexing, and the "
   "program's behaviour is exactly what those definitions produce."),
 "normative_status": "defined; no_citation_of_undefinedness_exists_because_no_operation_is_undefined",
 "secondary_non_scoring_observation": {
   "build": "rustc -g -C debug-assertions=on ADV-26_R.rs -o BIN",
   "env": "RUST_BACKTRACE=1", "run_exit": 0,
   "stdout": "ADV-START\nOBS=IDX:-1\nADV-END\n", "stderr": "",
   "effect_on_stage": "D7b not satisfied: identical payload, no report. Stage stays Silent Bug."},
 "silent_transformation": True,
 "silent_transformation_watch_result": ("Confirmed exactly as the case predicts. The target 3 IS in the "
   "sequence, at index 4. The unsorted input makes the search walk away from it: mid=3 (14>3, hi=2), mid=1 "
   "(9>3, hi=0), mid=0 (5>3, hi=-1), loop ends, result=-1. The program printed `IDX:-1`, a completely "
   "ordinary `not found`, exited 0, and said nothing. Rust's type system, borrow checker and runtime checks "
   "have nothing to say about a violated ordering precondition: it is not expressible in the language. This "
   "is the purest silent transformation in this chunk and it scores 0.")})

# ---- merge the separately captured 5 ADV-19 primary runs, if present ----
if os.path.exists(ADV19_RUNS):
    try:
        rr = json.load(open(ADV19_RUNS))
        for o in OBSERVATIONS:
            if o["case_variant_id"] == "ADV-19":
                o["runs_all_5"] = [{"index": r["index"], "exit_status": r["exit"], "signal": None,
                                    "timed_out": r["timed_out"], "wall_seconds": r["wall"],
                                    "adv_start_present": "ADV-START" in r["stdout"],
                                    "adv_end_present": "ADV-END" in r["stdout"],
                                    "obs_payload": obs_of(r["stdout"])} for r in rr]
                o["run"]["stdout"] = rr[0]["stdout"]
                o["run"]["stderr"] = rr[0]["stderr"]
                o["run"]["exit"] = rr[0]["exit"]
                o["run"]["timed_out"] = rr[0]["timed_out"]
                o["run"]["wall_seconds"] = rr[0]["wall"]
    except Exception as e:
        print("WARN adv19 runs merge:", e)

DOC = {
 "language": "Rust",
 "language_key": "rust",
 "chunk": "2",
 "toolchain": "rustc 1.95.0 (59807616e 2026-04-14) (Homebrew), arm64 macOS 26.4.1",
 "primary_recipe": "rustc -O FILE.rs -o BIN   (environment.json frozen_toolchain_recipes.rust)",
 "check_posture": ("CHECKS DISABLED. `-O` is `-C opt-level=2`; `-C debug-assertions` defaults to OFF whenever "
   "opt-level is non-zero, so rustc's arithmetic-overflow checks are compiled out. Slice bounds checks and "
   "UTF-8 validation of String are NOT debug assertions and remain on at -O. This is the disclosure "
   "toolchain_binding.check_posture_disclosure.operator_obligation requires for Rust."),
 "environment": {"LC_ALL": "en_US.UTF-8", "LANG": "en_US.UTF-8", "TZ": "UTC",
                 "cleared": ["PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE", "JAVA_TOOL_OPTIONS",
                             "NODE_OPTIONS", "GOFLAGS"],
                 "build_timeout_seconds": 300, "run_timeout_seconds": 60, "repetitions_primary": 5},
 "prohibition_compliance": ("P1: no program in this chunk contains a catch, except, recover, rescue, try "
   "block, Result/Option match, guard clause, range check, null check, validation, clamping or saturation. "
   "The only failure-channel construct anywhere is `.unwrap()`, which authoring rule A3 names as Rust's "
   "canonical shortest-direct-use form and which discards rather than inspects the channel. P2: no logging, "
   "timing, tracing or assertion beyond S1/S4/S5. P3: no #[allow], no attribute, no extra flag. P4: std only. "
   "P5: OBS payloads are written exactly as each observation_spec states."),
 "observations": OBSERVATIONS}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
json.dump(DOC, open(OUT, "w"), indent=1)
print("wrote", OUT, len(OBSERVATIONS), "observations")
