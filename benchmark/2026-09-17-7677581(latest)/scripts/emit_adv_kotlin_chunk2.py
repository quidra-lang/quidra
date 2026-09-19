#!/usr/bin/env python3
"""Emit standard/raw/adversarial/kotlin/chunk_2.json from the captured harness output.

Every `class` / `earliest_observable_stage` below is derived by hand from the
frozen decision list D0..D7 in methodology/08_adversarial_cases.json; the
lexicon matches quoted in each record were computed mechanically by
re-running the frozen regexes over the captured bytes (see `matched_pattern`).
"""
import json, os, re

ROOT = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC = ROOT + "/standard/src/adversarial/kotlin"
H = json.load(open(ROOT + "/work/adv_kotlin_chunk2/harness_out.json"))
SEC = json.load(open(ROOT + "/work/adv_kotlin_chunk2/secondary/secondary.json"))
M = json.load(open(ROOT + "/methodology/08_adversarial_cases.json"))
G = M["global_hazard_diagnostic_lexicon"]["fragments"]

def first_diag(s):
    for line in s.split("\n"):
        if line.strip():
            return line.strip()
    return None

def payloads(rec):
    out = []
    for r in rec.get("runs", []):
        p = [l for l in r["stdout"].split("\n") if l.startswith("OBS=")]
        out.append(p[0][4:] if p else None)
    return out

# (case_variant_id, case_id, variant, harness tag, source basename, extra fields)
SPEC = [
 dict(cvid="ADV-14/R", cid="ADV-14", var="R", tag="ADV-14_R", src="ADV-14_R.kt",
      clr=[1, 9],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["L_TYPE", "type mismatch"], silent=False,
      forced="none - the program contains no handler; the compiler rejected the call outright",
      reason="D0 n/a (no TM3a determination: Kotlin has Long and String, so the hazard arises). D1 fires: the frozen recipe `kotlinc ADV-14_R.kt -include-runtime -d prog.jar` exited 1. D1a n/a: no fatal signal and no toolchain_failure_markers regex matches the build output. D1b-i fires: the build stderr matches global fragment L_TYPE on the literal text `type mismatch` (\"argument type mismatch: actual type is 'String', but 'Long' was expected\"). Class and stage = Compile-time Detection (100). The hazardous call was never executed."),
 dict(cvid="ADV-15", cid="ADV-15", var="single", tag="ADV-15", src="ADV-15.kt",
      clr=[4, 10],
      cls="Runtime Safe Detection", stage="Runtime Safe Detection",
      matched=["L_MUTATE", "ConcurrentModificationException"], silent=False,
      forced="none - Kotlin has no checked exceptions, so nothing forced a handler; the program has none",
      reason="D0 n/a. D1/D2 n/a: the build exited 0 and produced prog.jar. D3 n/a: ADV-START is present on stdout, so the program started. D4 n/a: the run finished in well under 60 s. D5 fires: ADV-START present and the process exited 1 (uncaught JVM exception). D5a fires: stderr matches the case lexicon L_MUTATE on `ConcurrentModificationException`. Stage = Runtime Safe Detection (75). TM3b applied at authoring: Kotlin's frozen default_ordered_sequence is LongArray, which is fixed-length, so the ordinary growable sequence `mutableListOf<Long>` (java.util.ArrayList) was used, as ADV-15 step 1 directs. java.util.ArrayList's iterator performs a modCount comparison and refuses to continue, naming the hazard; no OBS line was produced."),
 dict(cvid="ADV-16", cid="ADV-16", var="single", tag="ADV-16", src="ADV-16.kt",
      clr=[4, 6],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=[None, None], silent=False, flag="compile_rejection_unmatched_lexicon",
      forced="Kotlin FORCES the immutability: `val` is not reassignable and the compiler refuses to emit code for the assignment. This forced refusal IS the measured safety behaviour.",
      reason="D0 n/a. D1 fires: build exited 1. D1a n/a: no toolchain_failure_markers match. D1b-i does NOT fire: the diagnostic is \"error: 'val' cannot be reassigned.\" and the frozen L_IMMUTABLE alternative is the unquoted literal `val cannot be reassigned`, which the quoted Kotlin wording does not match; no other case pattern and no global fragment matches (verified by re-running every frozen regex over the captured bytes). D1b-ii applies. D1b-ii-1 does not fire: A6 re-verification confirms the file is exactly ADV-16's construction (line 4 introduces the fixed_width_i64 binding with the strongest in-function immutable form frozen for Kotlin, `val`; line 5 assigns 20 with the ordinary assignment operator; line 6 renders it). D1b-ii-2 fires: the first diagnostic carries source line 5, inside construction_line_range [4,6]. Stage = Compile-time Detection (100), flagged compile_rejection_unmatched_lexicon for the published unmatched-compile-rejections table."),
 dict(cvid="ADV-17", cid="ADV-17", var="single", tag="ADV-17", src="ADV-17.kt",
      clr=[4, 6],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["L_NULL", "null"], silent=False,
      forced="Kotlin FORCES a null-safety construct: the compiler will not accept `.length` on a `String?` receiver without `?.` or `!!.`. Per the case's construction the program was written with neither, and the build rejected it. Being forced to handle the absence IS the language's safety behaviour and is exactly what this case measures.",
      reason="D0 n/a: Kotlin has a null_or_absent_value (`null` in a `String?` binding), so the hazard arises. D1 fires: build exited 1. D1a n/a. D1b-i fires: the build stderr \"error: only safe (?.) or non-null asserted (!!.) calls are allowed on a nullable receiver of type 'String?'\" matches case lexicon L_NULL, whose `\\bnull\\b` alternative matches the token `null` inside `non-null` (hyphen and space are word boundaries). Stage = Compile-time Detection (100). No member access was ever executed, so there is no LEN payload."),
 dict(cvid="ADV-18", cid="ADV-18", var="single", tag="ADV-18", src="ADV-18.kt",
      clr=[1, 11],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["L_RETURN", "missing return"], silent=False,
      forced="Kotlin FORCES a value-returning block body to return on every path; this refusal is the detection.",
      reason="D0 n/a. D1 fires: build exited 1. D1a n/a. D1b-i fires: build stderr \"error: missing return statement.\" matches case lexicon L_RETURN on the fragment `missing return`. Stage = Compile-time Detection (100). The opacity barrier (line 10 reads the boolean from stdin) means no constant folding could have proved which path is taken; Kotlin rejects the function on its declared signature alone, before the call is considered."),
 dict(cvid="ADV-19", cid="ADV-19", var="single", tag="ADV-19", src="ADV-19.kt",
      clr=[1, 9],
      cls="Runtime Safe Detection", stage="Runtime Safe Detection",
      matched=["L_STACK", "StackOverflowError"], silent=False,
      forced="none",
      reason="D0 n/a. D1/D2 n/a: build exited 0 with an artifact. D3 n/a: ADV-START present. D4 n/a: the run terminated in under a second, far inside the 60 s timeout. D5 fires: ADV-START present and exit status 1. D5a fires: stderr matches case lexicon L_STACK on `StackOverflowError` (\"Exception in thread \\\"main\\\" java.lang.StackOverflowError\"). Stage = Runtime Safe Detection (75). The non-tail `1 + f(n + 1)` was NOT optimised away - the stack trace names ADV-19.kt:2 repeatedly - so the specified unbounded recursion genuinely ran and the JVM diagnosed the exhaustion by name."),
 dict(cvid="ADV-20", cid="ADV-20", var="single", tag="ADV-20", src="ADV-20.kt",
      clr=[1, 12],
      cls="Runtime Safe Detection", stage="Runtime Safe Detection",
      matched=["L_STACK", "StackOverflowError"], silent=False,
      forced="none",
      reason="D0 n/a. D1/D2 n/a. D3 n/a: ADV-START present. D4 n/a. D5 fires: exit 1 with ADV-START present. D5a fires: stderr matches case lexicon L_STACK on `StackOverflowError`, with frames at ADV-20.kt:5 (the non-tail recursive call). Stage = Runtime Safe Detection (75). D6b was never reached, so the concrete reference_observation R:1000000 does not apply: Kotlin on the JVM cannot complete a 1,000,000-deep non-tail recursion at the default thread stack size, but it says so loudly rather than faulting silently. Per the frozen note, no stack size was adjusted for this case."),
 dict(cvid="ADV-21", cid="ADV-21", var="single", tag="ADV-21", src="ADV-21.kt",
      clr=[4, 4],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["L_STACK", "StackOverflowError"], silent=False,
      flag="compile_rejection_via_uncaught_compiler_StackOverflowError",
      forced="none in the program; the failure is entirely inside kotlinc",
      reason="D0 n/a. D1 fires: `kotlinc ADV-21.kt -include-runtime -d prog.jar` exited 2 in 1.84 s (well inside the 300 s build timeout) and produced no jar. D1a does NOT fire: the build process did not die by a fatal signal (it exited 2), and re-running every one of the twelve frozen toolchain_failure_markers regexes over the concatenated 115304-byte build output produced NO match - notably the Kotlin-specific marker is `kotlin.KotlinInternalException|org.jetbrains.kotlin.util.KotlinFrontEndException` and the Java StackOverflowError marker is anchored to `com.sun.tools`, neither of which appears. D1b-i then fires: the build output matches global fragments L_OVERFLOW (on `Overflow`) and L_STACK (on `StackOverflowError`). The mechanical result is therefore Compile-time Detection (100). HONEST CAVEAT, published with the row: the evidence is an UNCAUGHT java.lang.StackOverflowError escaping kotlinc's own recursive-descent parser (frames org.jetbrains.kotlin.parsing.KotlinExpressionParsing.parseParenthesizedExpression / com.intellij.lang.impl.PsiBuilderImpl), i.e. the compiler crashed rather than issuing a diagnostic about nesting depth; it carries no file, line or column and no case-lexicon `nesting`/`depth` wording. Under the frozen procedure this is still D1b-i because the toolchain_failure_markers list does not cover a bare StackOverflowError from the Kotlin frontend while the global lexicon does match it. This is a candidate erratum for the markers list and is flagged rather than silently scored."),
 dict(cvid="ADV-22a", cid="ADV-22", var="single (sub-program ADV-22a, 60% truncation)", tag="ADV-22a", src="ADV-22a.kt",
      clr=[1, 5],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["syntax error|parse error|unexpected|expected|unterminated|SyntaxError|error:", "error:"], silent=False,
      forced="none",
      reason="D0 n/a. D1 fires: build exited 1. D1a n/a: no toolchain_failure_markers match. D1b-i fires: build stderr \"ADV-22a.kt:5:26: error: syntax error: Expecting '}'.\" matches the case's lexicon on the alternatives `syntax error` and `error:`. Stage = Compile-time Detection (100). The truncated file was rejected with a file:line:column diagnostic; nothing was emitted."),
 dict(cvid="ADV-22b", cid="ADV-22", var="single (sub-program ADV-22b, '@#$' injected at floor(L/2))", tag="ADV-22b", src="ADV-22b.kt",
      clr=[1, 9],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["syntax error|parse error|unexpected|expected|unterminated|SyntaxError|error:", "syntax error"], silent=False,
      forced="none",
      reason="D0 n/a. D1 fires: build exited 1. D1a n/a. D1b-i fires: build stderr \"ADV-22b.kt:5:10: error: syntax error: Expecting an element.\" (plus a second diagnostic naming the unexpected tokens) matches the case lexicon on `syntax error` and `unexpected`. Stage = Compile-time Detection (100). Authoring precondition verified: the valid file is 178 bytes, floor(L/2)=89 falls between `prin` and `tln` on line 5 - not inside a string literal and not inside a comment - and ADV-22-valid.kt itself builds and runs, printing ADV-START / OBS=V:1 / ADV-END."),
 dict(cvid="ADV-23", cid="ADV-23", var="single", tag="ADV-23", src="ADV-23.kt",
      clr=[4, 5],
      cls="Silent Bug", stage="Silent Bug",
      matched=[None, None], silent=True,
      forced="none - kotlin.io.File.readText() reports no failure channel at all, so there was nothing to ignore and nothing the compiler could force",
      reason="D0 n/a. D1/D2 n/a: build exited 0 with an artifact. D3 n/a: ADV-START present. D4 n/a. D5 n/a: the process exited 0. D6 applies. D6a n/a: OBS= and ADV-END are both present. D6b n/a: this case's reference_observation is the sentinel DIAGNOSED_FAILURE, not a concrete value, so D6b cannot fire. D6c evaluates the plausible_domain_predicate on `CP:11`: it matches ^CP:(0|[1-9][0-9]*)$ and 11 lies inside the declared interval [0,10000], so D6c-ii fires and we proceed to D7. D7a does not fire: all 5 primary runs produced the byte-identical payload CP:11. D7b does not fire: the frozen Kotlin secondary configuration `java -ea -jar prog.jar` builds, runs, exits 0 and produces the SAME payload CP:11 with empty stderr - no report of undefined, illegal or erroneous behaviour. D7c does not fire: no clause of any Kotlin normative document declares this operation undefined; on the contrary kotlin.io.readText/String(ByteArray, Charset) is documented to decode with malformed-input REPLACEment, so the behaviour is defined. D7d therefore fires: Silent Bug (0). THIS IS A SILENT TRANSFORMATION. The 13 frozen bytes are not valid UTF-8 (C3 28, the surrogate ED A0 80, the above-U+10FFFF sequence F4 90 80 80, and a lone continuation 80), and Kotlin turned them into an ordinary 11-unit String, printed an ordinary small integer, exited 0, and left no trace in stdout or stderr that the input was invalid. Length member used: `String.length` (UTF-16 code units), the construct frozen for Kotlin in authoring_choice_table.string_length_member. Facility used: kotlin.io.File.readText(), Kotlin's own ordinary whole-file text read - note that this differs from java.nio.file.Files.readString, which reports malformed input; the divergence is a real property of Kotlin's standard library rather than of the JVM."),
 dict(cvid="ADV-24", cid="ADV-24", var="single", tag="ADV-24", src="ADV-24.kt",
      clr=[4, 5],
      cls="Compile-time Detection", stage="Compile-time Detection",
      matched=["L_OVERFLOW", "out of range"], silent=False,
      forced="none",
      reason="D0 n/a. D1 fires: build exited 1. D1a n/a. D1b-i fires: build stderr \"ADV-24.kt:4:19: error: value out of range.\" matches case lexicon L_OVERFLOW on `out of range` (and the case's own pattern `literal out of range` does not, but one match suffices). Stage = Compile-time Detection (100). The literal 9223372036854775809 was written plainly with no suffix, separator, sign or wrapper into a binding of Kotlin's default_arithmetic_type Long; kotlinc refuses the literal outright, so the silent outcome the case watches for (V:9223372036854775808, the binary64 rounding) cannot occur."),
 dict(cvid="ADV-25/R", cid="ADV-25", var="R", tag="ADV-25_R", src="ADV-25_R.kt",
      clr=[4, 7],
      cls="Runtime Safe Detection", stage="Runtime Safe Detection",
      matched=["L_PARSE", "NumberFormatException"], silent=False,
      forced="none - Kotlin has no checked exceptions and String.toLong() exposes no Result/Option to match on, so rule A3's shortest direct use IS the plain call; the program inspects no failure channel",
      reason="D0 n/a. D1/D2 n/a: build exited 0 with an artifact. D3 n/a: ADV-START present. D4 n/a. D5 fires: ADV-START present and exit status 1. D5a fires: stderr matches case lexicon L_PARSE on `NumberFormatException` (\"java.lang.NumberFormatException: For input string: \\\"12x34\\\"\"), and the message quotes the offending input verbatim. Stage = Runtime Safe Detection (75). The primary string-to-integer facility frozen for Kotlin is `text.toLong()`, which is what the program uses; neither silent outcome the case watches for (R:24 from leading-digit parsing, R:0 from an ignored error channel) occurred."),
 dict(cvid="ADV-26/R", cid="ADV-26", var="R", tag="ADV-26_R", src="ADV-26_R.kt",
      clr=[4, 22],
      cls="Silent Bug", stage="Silent Bug",
      matched=[None, None], silent=True,
      forced="none",
      reason="D0 n/a. D1/D2 n/a: build exited 0 with an artifact. D3 n/a: ADV-START present. D4 n/a. D5 n/a: the process exited 0. D6 applies. D6a n/a: OBS= and ADV-END both present. D6b n/a: the payload `IDX:-1` is not byte-identical to the concrete reference_observation `IDX:4`. D6c evaluates the predicate ^IDX:(-1|[0-6])$ on `IDX:-1`: TRUE - -1 is the ordinary 'not found' answer of a search - so D6c-ii fires and we proceed to D7. D7a does not fire: all 5 primary runs produced the byte-identical payload IDX:-1. D7b does not fire: the Kotlin secondary configuration `java -ea -jar prog.jar` produced the same payload IDX:-1, exit 0, empty stderr, no hazard report. D7c does not fire: every individual operation (LongArray indexing, integer division, comparison) is fully defined by Kotlin; the invalidity is in the program STATE, which no Kotlin document declares undefined. D7d fires: Silent Bug (0). THIS IS A SILENT TRANSFORMATION. The target 3 is present in the sequence at index 4, the search walked away from it exactly as the frozen verification predicts (mid=3 -> 14>3 -> hi=2; mid=1 -> 9>3 -> hi=0; mid=0 -> 5>3 -> hi=-1), and the program printed a completely ordinary 'not found' that no consumer could distinguish from a true negative. Kotlin has no construct in this position that would express or enforce the sortedness precondition, and none was added."),
]

obs = []
for s in SPEC:
    rec = H[s["tag"]]
    b = rec["build"]
    built = b["exit"] == 0 and b["artifact_produced"]
    runs = rec.get("runs", [])
    ps = payloads(rec)
    r0 = runs[0] if runs else None
    entry = {
        "case_variant_id": s["cvid"],
        "case_id": s["cid"],
        "variant": s["var"],
        "program": os.path.join(SRC, s["src"]),
        "build": {"cmd": b["cmd"], "exit": b["exit"], "stdout": b["stdout"],
                  "stderr": b["stderr"], "wall_seconds": b["wall_seconds"],
                  "timed_out": b["timed_out"], "artifact_produced": b["artifact_produced"]},
        "run": ({"cmd": r0["cmd"], "exit": r0["exit"], "signal": r0["signal"],
                 "stdout": r0["stdout"], "stderr": r0["stderr"],
                 "timed_out": r0["timed_out"], "wall_seconds": r0["wall_seconds"]}
                if r0 else {"cmd": None, "exit": None, "signal": None, "stdout": "",
                            "stderr": "", "timed_out": False,
                            "note": "not run: the build rejected the program"}),
        "runs_all_5": [{"exit": r["exit"], "signal": r["signal"], "timed_out": r["timed_out"],
                        "obs_payload": p} for r, p in zip(runs, ps)],
        "adv_start_present": bool(r0 and "ADV-START" in r0["stdout"]),
        "obs_line_present": bool(r0 and "OBS=" in r0["stdout"]),
        "adv_end_present": bool(r0 and "ADV-END" in r0["stdout"]),
        "observed_payload": ps[0] if ps else None,
        "reached_observation_point": bool(ps and ps[0] is not None),
        "obs_identical_across_runs": (len(set(map(str, ps))) == 1) if ps else None,
        "forced_error_handling": s["forced"],
        "class": s["cls"],
        "earliest_observable_stage": s["stage"],
        "stage_score": {"Compile-time Detection": 100, "Static Checking Before Execution": 90,
                        "Runtime Safe Detection": 75, "Test Failure": 55,
                        "Output Verification": 40, "Crash": 20,
                        "Undefined Behavior": 5, "Silent Bug": 0,
                        "Prevented By Construction": 100}[s["stage"]],
        "classification_reasoning": s["reason"],
        "silent_transformation": s["silent"],
        "construction_line_range": s["clr"],
        "first_diagnostic_line": (first_diag(b["stderr"] or b["stdout"]) if b["exit"] != 0
                                  else (first_diag(r0["stderr"]) if r0 and r0["stderr"] else None)),
        "matched_lexicon_fragment": s["matched"][0],
        "matched_text": s["matched"][1],
    }
    if s.get("flag"):
        entry["flag"] = s["flag"]
    if s["cvid"] in ("ADV-23", "ADV-26/R"):
        entry["secondary_configuration_D7b"] = {
            "non_scoring": True,
            "cmd": SEC[s["tag"]]["run"]["cmd"],
            "exit": SEC[s["tag"]]["run"]["exit"],
            "stdout": SEC[s["tag"]]["run"]["stdout"],
            "stderr": SEC[s["tag"]]["run"]["stderr"],
            "d7b_fired": False,
            "note": "same payload as the primary configuration and no report of undefined/illegal/erroneous behaviour, so D7b does not move this row from Silent Bug to Undefined Behavior",
        }
    if s["cvid"] == "ADV-21":
        entry["secondary_depths_non_scoring"] = {
            "depth_1000": SEC["ADV-21_depth1000"],
            "depth_10000": SEC["ADV-21_depth10000"],
            "note": "kotlinc parses 1000 nested parentheses correctly (prints OBS=V:1) and already dies with the same uncaught java.lang.StackOverflowError at 10000, so its parser limit lies between 1000 and 10000 nesting levels.",
        }
        entry["toolchain_failure_marker_check"] = {
            "matched_any": False,
            "markers_tested": M["toolchain_failure_markers"]["case_insensitive_regexes"],
            "note": "re-run mechanically over the concatenated build stdout+stderr; none matched, so D1a did not fire",
        }
    if s["cvid"] == "ADV-22b":
        entry["adv22_authoring_precondition"] = {
            "valid_file": os.path.join(SRC, "ADV-22-valid.kt"),
            "valid_file_bytes": 178, "cut_offset": 106, "mid_offset": 89,
            "midpoint_context": "val v: Long = 1\\n    prin|tln(\"OBS=V:\" + v)",
            "midpoint_inside_string_or_comment": False,
            "valid_file_builds_and_runs": True,
            "valid_file_stdout": SEC["ADV-22-valid"]["run_stdout"],
        }
    obs.append(entry)

doc = {
    "language": "Kotlin",
    "language_key": "kotlin",
    "chunk": "2",
    "toolchain": "kotlinc-jvm 2.3.21 (JRE 26.0.1); java openjdk 26.0.1 (Homebrew, arm64)",
    "recipe": {"build": "kotlinc FILE.kt -include-runtime -d FILE.jar",
               "run": "java -jar FILE.jar",
               "env": "JAVA_HOME=/opt/homebrew/opt/openjdk; LC_ALL=LANG=en_US.UTF-8; TZ=UTC; PYTHONUTF8/PYTHONIOENCODING/RUST_BACKTRACE/JAVA_TOOL_OPTIONS/NODE_OPTIONS/GOFLAGS unset",
               "build_timeout_seconds": 300, "run_timeout_seconds": 60,
               "repetitions_primary": 5},
    "authoring_notes": {
        "A3_shortest_direct_use": "Kotlin's non-null line read `readln()` (kotlin.io, stdlib) is used for the opacity barrier, not `readLine()!!`; it is the shortest direct-use form and introduces no unwrap operator.",
        "primary_string_to_integer_facility": "text.toLong() (frozen in authoring_choice_table)",
        "strongest_immutable_binding_form": "val (frozen; a local `const val` is not permitted by the language in a function body)",
        "string_length_member": "s.length (frozen)",
        "default_ordered_sequence": "LongArray (frozen). ADV-15 needs a GROWABLE sequence, so TM3b applies there and mutableListOf<Long> (java.util.ArrayList) is used, as ADV-15 step 1 directs; it is named here as required.",
        "prohibitions": "No program in this chunk contains a catch, try, guard, range check, null check, validation, clamp, saturation, suppression, pragma or third-party dependency.",
    },
    "observations": obs,
}
out = ROOT + "/standard/raw/adversarial/kotlin/chunk_2.json"
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump(doc, open(out, "w"), indent=1)
print(out)
