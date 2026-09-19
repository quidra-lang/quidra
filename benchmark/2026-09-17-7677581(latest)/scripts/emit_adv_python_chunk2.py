#!/usr/bin/env python3
import json, os, hashlib

BASE = "/private/tmp/claude-501/-Users-koba-Desktop-Quidra/c7dc7758-3bc7-4b38-ac54-4af31eae0682/scratchpad/bench/quidra-scratch/benchmark/2026-09-17-7677581"
SRC = os.path.join(BASE, "standard/src/adversarial/python")
INP = os.path.join(BASE, "standard/src/adversarial/inputs")
WORK = os.path.join(BASE, "standard/build/adversarial/python")
OUT = os.path.join(BASE, "standard/raw/adversarial/python/chunk_2.json")
raw = json.load(open(os.path.join(WORK, "raw_runs.json")))

def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()

# key -> (case_variant_id, case_id, variant, src, stdin, class, stage, score,
#         rule, reasoning, silent, forced, flags, extra)
SPEC = [
 dict(key="ADV-14/R", cvid="ADV-14/R", cid="ADV-14", var="R", src="ADV-14_R.py",
      stdin="ADV-14.in", clr={"first_line":4,"last_line":11},
      cls="Silent Bug", stage="Silent Bug", score=0, rule="D6c-ii -> D7d",
      silent=True, flags=[],
      reasoning="No build recipe for Python, so D1 and D2 do not apply. D3 does not fire: exit status is 0 and ADV-START is present. D4 does not fire: 0.03 s, far inside the 60 s timeout. D5 does not fire: the process terminated normally (exit 0). D6a does not fire: ADV-START, OBS= and ADV-END are all present. D6b does not fire: the case's reference_observation is the sentinel DIAGNOSED_FAILURE, not a concrete value. D6c evaluates the frozen plausible_domain_predicate ^R:-?(0|[1-9][0-9]*)$ on the payload R:777 -- it MATCHES, so D6c-i does not fire and D6c-ii sends the row to D7. D7a does not fire: all 5 primary runs produced the byte-identical payload R:777. D7b does not fire: the Python secondary configuration `python3 -X dev` emitted no report and produced the identical payload R:777. D7c does not fire: Python's reference manual DEFINES str.__mul__ (sequence repetition); nothing here is undefined behaviour. D7d therefore fires. This is precisely the silent transformation the case pre-registered: the declared signature promises int64*3, the annotation ctypes.c_int64 is not enforced by CPython, and '7'*3 repeats the text into '777', which satisfies the plausibility regex as an ordinary base-10 integer while the arithmetically correct answer is 21.",
      norm_status="defined",
      norm_cite={"document":"The Python Language Reference / The Python Standard Library, str common sequence operation s*n","version":"3.14.5","clause":"Sequence Types -- list, tuple, range: 's * n' repeats s n times; and PEP 484/649: annotations are not enforced at runtime"},
      authoring=[{"category":"fixed_width_i64 parameter/result declaration","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64, written as the parameter and return ANNOTATION (Python's only way to 'declare' a parameter type)","citation":"methodology/08_adversarial_cases.json type_binding_table.bindings.fixed_width_i64"},
                 {"category":"line-reading facility (opacity barrier, S2)","table_row_used":"input() -- Python's ordinary line reader; the token is kept as str and NOT converted, per construction step 2","citation":"08_adversarial_cases.json opacity_barrier.variants.R"}]),
 dict(key="ADV-15", cvid="ADV-15", cid="ADV-15", var="single", src="ADV-15.py",
      stdin=None, clr={"first_line":4,"last_line":11},
      cls="Silent Bug", stage="Silent Bug", score=0, rule="D6c-ii -> D7d",
      silent=True, flags=["python_ctypes_binding"],
      reasoning="D1/D2 N/A (no build step). D3 does not fire (exit 0, ADV-START present). D4 does not fire (0.03 s). D5 does not fire (normal termination). D6a does not fire (all three skeleton lines present). D6b does not fire (reference_observation is the DIAGNOSED_FAILURE sentinel). D6c: the frozen predicate ^ITERS:(0|[1-9][0-9]*)\\|LEN:(0|[1-9][0-9]*)$ matches ITERS:6|LEN:6 and the numeric field ITERS=6 lies inside the declared interval [0,1000], so the observation is PLAUSIBLE and D6c-ii routes to D7. D7a does not fire (5/5 byte-identical). D7b does not fire (`python3 -X dev` emits no report and yields the same payload). D7c does not fire: CPython's list iterator is index-based and terminates on len(), and no clause of the Python reference declares mutation-during-iteration undefined; the tutorial only warns it is 'tricky'. D7d fires. ITERS:6|LEN:6 is one of the two outcomes the case named in advance as Silent Bugs: the loop walked the growing list, visited the appended 99, and reported two entirely ordinary integers with no trace that the sequence was mutated underneath the iterator.",
      norm_status="no_citation_found",
      norm_cite=None,
      authoring=[{"category":"default_ordered_sequence / growable_sequence_substitution","table_row_used":"authoring_choice_table.categories.growable_sequence_substitution.Python = list","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"element type fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64; the element comparison x.value == 2 is the binding plumbing mandated by type_binding_table.python_fixed_width_operand_rule and is explicitly NOT a guard under P1","citation":"08_adversarial_cases.json type_binding_table.python_fixed_width_operand_rule"}]),
 dict(key="ADV-16", cvid="ADV-16", cid="ADV-16", var="single", src="ADV-16.py",
      stdin=None, clr={"first_line":6,"last_line":9},
      cls="Silent Bug", stage="Silent Bug", score=0, rule="D6c-ii -> D7d",
      silent=True, flags=["construct_absent","python_ctypes_binding"],
      reasoning="TM3 determination made BEFORE running, from the frozen table: authoring_choice_table.categories.strongest_immutable_binding_form.Python reads 'construct_absent: true. Python has no immutable local binding form; the closest thing the language offers is an UPPER_CASE naming convention... This is NOT TM3a: the hazard still arises and Python is expected to reach Silent Bug.' D0 therefore does not fire. D1/D2 N/A (no build step). D3 does not fire (exit 0, ADV-START present). D4 does not fire. D5 does not fire (normal termination). D6a does not fire. D6b does not fire (reference is the DIAGNOSED_FAILURE sentinel). D6c: predicate ^V:-?(0|[1-9][0-9]*)$ matches V:20, so the value is PLAUSIBLE and D6c-ii routes to D7. D7a does not fire (5/5 identical). D7b does not fire (`-X dev` silent, same payload). D7c does not fire (rebinding a name is fully defined in Python). D7d fires. The binding declared with the strongest immutability Python offers in a function body -- an UPPER_CASE name -- was reassigned with the ordinary assignment operator and the program printed V:20 with no diagnostic anywhere. Immutability is a convention in Python, not a rule, exactly as the case's silent_transformation_watch describes.",
      norm_status="defined",
      norm_cite={"document":"The Python Language Reference, Assignment statements","version":"3.14.5","clause":"7.2 Assignment statements -- an assignment statement rebinds the target name unconditionally; Python defines no immutable local binding form"},
      authoring=[{"category":"strongest_immutable_binding_form","table_row_used":"authoring_choice_table.categories.strongest_immutable_binding_form.Python = construct_absent, UPPER_CASE naming convention","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"binding type fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64","citation":"08_adversarial_cases.json type_binding_table"}]),
 dict(key="ADV-17", cvid="ADV-17", cid="ADV-17", var="single", src="ADV-17.py",
      stdin=None, clr={"first_line":2,"last_line":4},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=[],
      reasoning="D1/D2 N/A (no build step, so the D1b compile-rejection branch is unreachable for Python). D3 does not fire: exit status is 1 but ADV-START IS present on stdout, so the defect was not reported before the first statement executed. D4 does not fire (0.03 s). D5 fires: ADV-START present AND the process terminated abnormally (exit 1). D5a: the concatenated stdout+stderr matches the case's own hazard_diagnostic_lexicon entry L_NULL via the literal fragment 'NoneType' (the diagnostic is \"TypeError: object of type 'NoneType' has no len()\"). Per the crash_vs_runtime_safe_detection_summary this is Runtime Safe Detection, not Crash: the runtime named the specific hazard -- the absent value -- and stopped before doing anything with it. No value was produced, so no silent transformation occurred.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"string_length_member","table_row_used":"authoring_choice_table.categories.string_length_member.Python = len(s) -- a function, not a member; it is Python's only direct length read","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"null_or_absent_value","table_row_used":"type_binding_table.bindings.null_or_absent_value.Python = None; the binding is written `s: str | None = None`, which is the union declaration the construction's step 1 asks for","citation":"08_adversarial_cases.json type_binding_table"}]),
 dict(key="ADV-18", cvid="ADV-18", cid="ADV-18", var="single", src="ADV-18.py",
      stdin="ADV-18.in", clr={"first_line":4,"last_line":12},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=[],
      reasoning="D1/D2 N/A (no build step; Python never rejects a function whose body falls off the end, so the D1b route the case anticipates for statically checked languages is structurally unavailable here). D3 does not fire (exit 1 but ADV-START present). D4 does not fire. D5 fires (ADV-START present, abnormal exit 1). D5a: the diagnostic \"TypeError: unsupported operand type(s) for *: 'NoneType' and 'int'\" matches this case's hazard_diagnostic_lexicon on all three entries -- L_TYPE via 'TypeError' and 'unsupported operand', L_RETURN via 'NoneType', L_NULL via 'NoneType'. Runtime Safe Detection. IMPORTANT NUANCE recorded honestly: Python did NOT diagnose the missing return itself -- pick(False) returned None silently and without complaint; the hazard only became observable one statement later, when the implicit None met the multiplication. The frozen ladder records the earliest stage at which the defect became OBSERVABLE, and that is the runtime diagnostic, so 75 stands; but the pre-registered Silent Bug outcome R:0 was avoided only because Python refuses to coerce None to a number, not because the missing return was detected.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"result type fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64, written as the return ANNOTATION","citation":"08_adversarial_cases.json type_binding_table"},
                 {"category":"text-to-boolean conversion (construction step 2)","table_row_used":"input() == \"1\" -- Python's ordinary way of writing the frozen rule '0 is false, 1 is true'","citation":"08_adversarial_cases.json case ADV-18 construction step 2"}]),
 dict(key="ADV-19", cvid="ADV-19", cid="ADV-19", var="single", src="ADV-19.py",
      stdin="ADV-19.in", clr={"first_line":4,"last_line":11},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=["python_ctypes_binding"],
      reasoning="D1/D2 N/A. D3 does not fire (exit 1, ADV-START present). D4 does not fire: the run completed in 0.04 s, nowhere near the 60 s timeout. D5 fires. D5a: the diagnostic 'RecursionError: maximum recursion depth exceeded' matches this case's hazard_diagnostic_lexicon entry L_STACK on two separate alternatives, 'RecursionError' and 'maximum recursion depth'. Runtime Safe Detection (75). The optimizer-removal outcomes the case warns about (D6a, or an arbitrary integer via D7) did not arise: CPython performs no tail-call or no-forward-progress transformation, the recursion really was executed, and the interpreter's own depth counter stopped it with a named, located diagnostic and a full traceback.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"primary_string_to_integer_facility","table_row_used":"authoring_choice_table.categories.primary_string_to_integer_facility.Python = int(text)","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"parameter/result fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64; the n.value read and the c_int64 store are the plumbing mandated by python_fixed_width_operand_rule, explicitly not a guard under P1","citation":"08_adversarial_cases.json type_binding_table.python_fixed_width_operand_rule"}]),
 dict(key="ADV-20", cvid="ADV-20", cid="ADV-20", var="single", src="ADV-20.py",
      stdin="ADV-20.in", clr={"first_line":4,"last_line":13},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=["python_ctypes_binding"],
      reasoning="D1/D2 N/A. D3 does not fire (exit 1, ADV-START present). D4 does not fire (0.04 s). D5 fires (ADV-START present, abnormal exit). D5a: 'RecursionError: maximum recursion depth exceeded' matches L_STACK. Runtime Safe Detection (75). Note the contrast with ADV-19: here the recursion HAS a mathematically correct answer (R:1000000) and the reference_observation is that concrete value, so a language that completes it would be Prevented By Construction (100) at D6b. Python does not complete it -- CPython's default sys.getrecursionlimit() is 1000 on this host and the frozen recipe forbids adjusting it -- so D5 fires before D6 is ever reached. This is a GOOD outcome (a loud, located, named diagnosis) but it is not the top rung: Python could not do the job. No value was produced, so nothing was silently transformed.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"primary_string_to_integer_facility","table_row_used":"authoring_choice_table.categories.primary_string_to_integer_facility.Python = int(text)","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"parameter/result fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64 with python_fixed_width_operand_rule plumbing","citation":"08_adversarial_cases.json type_binding_table.python_fixed_width_operand_rule"}]),
 dict(key="ADV-21", cvid="ADV-21", cid="ADV-21", var="single", src="ADV-21.py",
      stdin=None, clr={"first_line":4,"last_line":6},
      cls="Compile-time Detection", stage="Static Checking Before Execution", score=90,
      rule="D3b", silent=False, flags=[],
      reasoning="D0 does not fire. D1/D2 N/A: Python has no build recipe, so the 100-point Compile-time Detection rung is structurally unreachable for it (class_to_stage_mapping: 'stage Static Checking Before Execution when the language has no build step ... and the defect was reported before the first statement of the program executed'). D3 fires: run exit status is 1 AND ADV-START is ABSENT from stdout -- CPython compiled the whole module before executing any statement, so the print on line 3 never ran. D3a does not fire: the process was not killed by a fatal-fault signal (clean exit 1, no signal) and the captured stderr contains no toolchain_failure_markers regex (checked: no 'Segmentation fault', 'Bus error', 'Illegal instruction', 'internal compiler error', 'LLVM ERROR', 'Stack dump:'). D3b therefore fires: class Compile-time Detection, stage Static Checking Before Execution, 90. The diagnostic is a clean, located rejection naming the exact construct. Recorded for the audit: this diagnostic does NOT match either pattern of the case's own hazard_diagnostic_lexicon ('nesting' does not match 'nested'; 'maximum.*(depth|nesting)' does not match) -- but D3b, unlike D1b-i and D5a, imposes no lexicon test, so the rung is unaffected.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"source generation","table_row_used":"frozen_generators.parser_nesting, DEPTH=100000, implemented by scripts/gen_adv21_python.py; no newline inside the parenthesis runs; generated file is 200134 bytes","citation":"08_adversarial_cases.json frozen_generators.parser_nesting"}],
      extra_note="Non-scoring secondary depths required by the case were also generated and run: depth 1000 and depth 10000 BOTH fail identically with 'SyntaxError: too many nested parentheses'. CPython's tokenizer nesting limit (MAXLEVEL = 200) therefore lies far below even the smallest secondary depth, so Python's parser limit is ~200, not somewhere between 1000 and 100000."),
 dict(key="ADV-22a", cvid="ADV-22a", cid="ADV-22", var="single", src="ADV-22a.py",
      stdin=None, clr={"first_line":1,"last_line":4},
      cls="Compile-time Detection", stage="Static Checking Before Execution", score=90,
      rule="D3b", silent=False, flags=[],
      reasoning="Sub-program a of ADV-22 (first floor(0.60*155)=93 bytes of ADV-22-valid.py). The authoring precondition was verified before mutation: ADV-22-valid.py builds and runs correctly (5/5 runs exit 0, OBS=V:1, ADV-END present) and byte floor(L/2)=77 falls inside the identifier 'ctypes.c_int64' on line 3 -- no string literal and no comment spans it. D1/D2 N/A (no build step). D3 fires: exit 1 and ADV-START ABSENT (the module failed to compile, so no statement ran). D3a does not fire (no signal, no toolchain_failure_marker in stderr). D3b fires: Compile-time Detection / Static Checking Before Execution / 90. The truncation is diagnosed cleanly with file, line and caret column. Python did NOT accept the truncated program, so the toolchain-level Silent Bug the case watches for did not occur.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"source generation","table_row_used":"frozen_generators.malformed_source primary mutation, implemented by scripts/gen_adv22.py; L=155, cut=93","citation":"08_adversarial_cases.json frozen_generators.malformed_source"}]),
 dict(key="ADV-22b", cvid="ADV-22b", cid="ADV-22", var="single", src="ADV-22b.py",
      stdin=None, clr={"first_line":1,"last_line":5},
      cls="Compile-time Detection", stage="Static Checking Before Execution", score=90,
      rule="D3b", silent=False, flags=[],
      reasoning="Sub-program b of ADV-22 (bytes 0x40 0x23 0x24 inserted at byte offset floor(155/2)=77, producing 'v: ctypes.c_int64 = ctypes.c_int@#$64(1)'). D1/D2 N/A. D3 fires: exit 1, ADV-START ABSENT. D3a does not fire (no signal, no toolchain_failure_marker). D3b fires: Compile-time Detection / Static Checking Before Execution / 90, on a located diagnostic with a caret span. ADV-22 ROW AGGREGATE: unweighted arithmetic mean of ADV-22a (90) and ADV-22b (90) = 90, stage Static Checking Before Execution.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"source generation","table_row_used":"frozen_generators.malformed_source second mutation, implemented by scripts/gen_adv22.py; L=155, mid=77","citation":"08_adversarial_cases.json frozen_generators.malformed_source"}]),
 dict(key="ADV-23", cvid="ADV-23", cid="ADV-23", var="single", src="ADV-23.py",
      stdin=None, clr={"first_line":2,"last_line":4},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=[],
      reasoning="D1/D2 N/A. D3 does not fire (exit 1 but ADV-START present). D4 does not fire. D5 fires. D5a: the diagnostic \"UnicodeDecodeError: 'utf-8' codec can't decode byte 0xc3 in position 1: invalid continuation byte\" matches this case's hazard_diagnostic_lexicon entry L_UTF8 on three alternatives ('utf-?8', 'UnicodeDecodeError', 'invalid ... ' via 'decoding'/'UnicodeDecodeError'). Runtime Safe Detection (75). This is the case's emblematic silent-transformation test and Python did NOT commit it: open(path).read() uses errors='strict' by default, so the 13 malformed bytes were refused outright rather than turned into U+FFFD replacements and reported as an ordinary small length. The construction's 'NO error-handling policy, replacement policy, encoding argument' instruction is satisfied exactly -- the call is the bare open(...).read() with no arguments beyond the path, and the strictness is the language's default, not something the program asked for. The locale was LC_ALL=LANG=en_US.UTF-8 as the frozen execution_environment requires, and PYTHONUTF8/PYTHONIOENCODING were cleared.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"ordinary text-file read","table_row_used":"open(\"inputs/ADV-23.bin\").read() -- Python's ordinary text-file facility; no encoding or errors argument supplied","citation":"08_adversarial_cases.json case ADV-23 construction step 1"},
                 {"category":"string length member","table_row_used":"len(s), the same choice frozen for Python in authoring_choice_table.categories.string_length_member; Python's str length is a count of code points","citation":"08_adversarial_cases.json authoring_choice_table"}]),
 dict(key="ADV-24", cvid="ADV-24", cid="ADV-24", var="single", src="ADV-24.py",
      stdin=None, clr={"first_line":2,"last_line":4},
      cls="Prevented By Construction", stage="Prevented By Construction", score=100,
      rule="D6b", silent=False, flags=[],
      reasoning="D0 does not fire: no TM3 capability-absence determination applies -- Python HAS the construct (a decimal integer literal stored in its default arithmetic type) and the hazardous operation was genuinely attempted, satisfying the prevented_by_construction evidence_requirement (a). D1/D2 N/A. D3, D4, D5 do not fire (exit 0, all skeleton lines present, 0.03 s). D6a does not fire. D6b FIRES: the payload V:9223372036854775809 equals the case's reference_observation 'V:9223372036854775809' byte for byte, and that reference is a concrete value, not the DIAGNOSED_FAILURE sentinel. Class and stage are Prevented By Construction (100), satisfying evidence_requirement (b) by exact reference match. Python's default_arithmetic_type is int, which is arbitrary precision (type_binding_table.bindings.default_arithmetic_type.Python), so the literal is neither truncated to a 64-bit range nor rounded to the nearest binary64 value; the pre-registered silent outcome V:9223372036854775808 cannot arise. Per outcome_classes.prevented_by_construction.reporting_requirement this must be published under its own label and never merged into Compile-time Detection: the compiler did not reject anything, the hazard simply cannot exist in this language.",
      norm_status="defined",
      norm_cite={"document":"The Python Language Reference, Integer literals / Numeric Types","version":"3.14.5","clause":"2.4.5 Integer literals: 'There is no limit for the length of integer literals apart from what can be stored in available memory'; int is arbitrary precision"},
      authoring=[{"category":"default_arithmetic_type","table_row_used":"type_binding_table.bindings.default_arithmetic_type.Python = int (arbitrary precision)","citation":"08_adversarial_cases.json type_binding_table"}]),
 dict(key="ADV-25/R", cvid="ADV-25/R", cid="ADV-25", var="R", src="ADV-25_R.py",
      stdin="ADV-25.in", clr={"first_line":4,"last_line":8},
      cls="Runtime Safe Detection", stage="Runtime Safe Detection", score=75,
      rule="D5a", silent=False, flags=["python_ctypes_binding"],
      reasoning="D1/D2 N/A. D3 does not fire (exit 1, ADV-START present). D4 does not fire. D5 fires. D5a: \"ValueError: invalid literal for int() with base 10: '12x34'\" matches this case's hazard_diagnostic_lexicon entry L_PARSE on two alternatives, 'invalid (digit|number|literal|syntax)' and the explicit 'ValueError: invalid literal'. Runtime Safe Detection (75). Rule A3 was applied as written -- int(t) is used directly, with no try, no failure channel inspected, no default -- and the frozen primary_string_to_integer_facility for Python is exactly int(text). Neither pre-registered silent outcome occurred: Python's int() does not parse leading digits and discard the rest (no R:24) and it does not return a zero alongside an uninspected error value (no R:0); it refuses the whole token and names it in the message. Note this is the one case where the R-variant's malformed input IS the hazard (opacity_barrier.R_variant_parsing_rule), so the parse failure is the measurement, not an authoring defect.",
      norm_status=None, norm_cite=None,
      authoring=[{"category":"primary_string_to_integer_facility","table_row_used":"authoring_choice_table.categories.primary_string_to_integer_facility.Python = int(text)","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"target fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64, with python_fixed_width_operand_rule plumbing for the *2 multiplication","citation":"08_adversarial_cases.json type_binding_table.python_fixed_width_operand_rule"}]),
 dict(key="ADV-26/R", cvid="ADV-26/R", cid="ADV-26", var="R", src="ADV-26_R.py",
      stdin="ADV-26.in", clr={"first_line":4,"last_line":18},
      cls="Silent Bug", stage="Silent Bug", score=0, rule="D6c-ii -> D7d",
      silent=True, flags=["python_ctypes_binding"],
      reasoning="D1/D2 N/A. D3, D4, D5 do not fire (exit 0, 0.03 s, all skeleton lines present). D6a does not fire. D6b does not fire: the payload IDX:-1 differs from the case's concrete reference_observation IDX:4 (the target 3 really is present, at index 4). D6c: the frozen predicate ^IDX:(-1|[0-6])$ matches IDX:-1 -- '-1 (not found) and every valid index 0..6 are ordinary, expected answers from a search' -- so the observation is PLAUSIBLE and D6c-ii routes to D7. D7a does not fire (5/5 byte-identical IDX:-1). D7b does not fire ('python3 -X dev' emits no report and yields the identical payload). D7c does not fire: every individual operation is defined; Python has no construct for declaring or enforcing the sortedness precondition, so nothing is undefined and nothing is checked. D7d fires: Silent Bug (0). The binary search executed the frozen comparison sequence exactly -- mid=3 (14>3, hi=2), mid=1 (9>3, hi=0), mid=0 (5>3, hi=-1) -- and walked away from the element it was looking for. It then reported a completely ordinary 'not found'. This is the purest form of the hazard spec 17 singles out: an invalid program state produced an apparently valid answer with no diagnostic anywhere in the pipeline.",
      norm_status="defined",
      norm_cite={"document":"The Python Language Reference","version":"3.14.5","clause":"All operations used (list indexing within range, integer comparison, floor division) are fully defined; Python provides no contract, refinement type, or checked-construction facility with which the sortedness precondition could be expressed"},
      authoring=[{"category":"default_ordered_sequence / growable_sequence_substitution","table_row_used":"authoring_choice_table.categories.growable_sequence_substitution.Python = list","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"primary_string_to_integer_facility","table_row_used":"authoring_choice_table.categories.primary_string_to_integer_facility.Python = int(text)","citation":"08_adversarial_cases.json authoring_choice_table"},
                 {"category":"element type fixed_width_i64","table_row_used":"type_binding_table.bindings.fixed_width_i64.Python = ctypes.c_int64; .value reads for the == and < comparisons are python_fixed_width_operand_rule plumbing, not guards","citation":"08_adversarial_cases.json type_binding_table.python_fixed_width_operand_rule"}]),
]

obs = []
for s in SPEC:
    r = raw[s["key"]]
    srcpath = os.path.join(SRC, s["src"])
    runs = r["runs"]
    r0 = runs[0]
    stdin_desc = (os.path.join(INP, s["stdin"]) if s["stdin"] else "/dev/null")
    big = s["key"] == "ADV-21"
    def clip(t):
        if big and len(t) > 4000:
            return (t[:1500] + "\n...[ELIDED " + str(len(t) - 3000) +
                    " bytes of the 100000-deep parenthesis run; the complete "
                    "stderr is preserved verbatim at " +
                    os.path.join(WORK, "ADV-21", "run0.err") + "]...\n" + t[-1500:])
        return t
    rec = {
      "case_variant_id": s["cvid"], "case_id": s["cid"], "variant": s["var"],
      "language": "Python", "language_id": "python", "configuration": "primary",
      "program": srcpath,
      "source_sha256": sha(srcpath),
      "construction_line_range": s["clr"],
      "build": {"cmd": None, "exit": None, "stdout": "", "stderr": "",
                "note": "Python has no build step. environment.json frozen_toolchain_recipes.python declares run only: 'python3 FILE.py'. Decision rules D1, D1a, D1b and D2 are therefore structurally inapplicable, and the 100-point Compile-time Detection rung is unreachable for Python by construction."},
      "run": {"cmd": ["python3", s["src"]],
              "interpreter_resolved": "/opt/homebrew/opt/python@3.14/bin/python3.14 (Python 3.14.5, the toolchain frozen in environment.json toolchains.python; PATH was set so that `python3` resolves to it)",
              "cwd": os.path.join(WORK, s["key"].replace("/", "_")),
              "stdin": stdin_desc,
              "exit": r0["exit_status"], "signal": r0["signal"],
              "stdout": r0["stdout"], "stderr": clip(r0["stderr"]),
              "timed_out": r0["timed_out"],
              "wall_seconds": r0["wall_seconds"]},
      "runs_all_5": [{"index": x["index"], "exit_status": x["exit_status"],
                      "signal": x["signal"], "timed_out": x["timed_out"],
                      "wall_seconds": x["wall_seconds"],
                      "adv_start_present": x["adv_start_present"],
                      "adv_end_present": x["adv_end_present"],
                      "obs_payload": x["obs_payload"]} for x in runs],
      "obs_identical_across_runs": r["obs_identical"],
      "observed_payload": r0["obs_payload"],
      "reached_observation_point": r0["obs_payload"] is not None,
      "adv_start_present": r0["adv_start_present"],
      "adv_end_present": r0["adv_end_present"],
      "forced_error_handling": "none - the program contains no try/except, no signal handler, no sys.excepthook, no validation, no range or null check, and no clamping. Python forced no error-handling construct on the author in order to run.",
      "class": s["cls"],
      "earliest_observable_stage": s["stage"],
      "stage_score": s["score"],
      "decision_rule_fired": s["rule"],
      "classification_reasoning": s["reasoning"],
      "silent_transformation": s["silent"],
      "flags": s["flags"],
      "normative_status": s["norm_status"],
      "normative_citation": s["norm_cite"],
      "tm3_applied": None,
      "authoring_choices": s["authoring"],
      "na": None,
    }
    if s.get("extra_note"):
        rec["secondary_non_scoring_note"] = s["extra_note"]
    obs.append(rec)

doc = {
  "language": "Python", "language_key": "python", "chunk": "2",
  "toolchain": "Python 3.14.5 (environment.json toolchains.python); frozen recipe: run `python3 FILE.py`, no build step",
  "execution_environment": {
    "LC_ALL": "en_US.UTF-8", "LANG": "en_US.UTF-8", "TZ": "UTC",
    "cleared": ["PYTHONUTF8", "PYTHONIOENCODING", "RUST_BACKTRACE",
                "JAVA_TOOL_OPTIONS", "NODE_OPTIONS", "GOFLAGS"],
    "run_timeout_seconds": 60, "repetitions_primary": 5,
    "working_directory": "fresh empty directory per case under standard/build/adversarial/python/",
    "harness": os.path.join(BASE, "scripts/run_adv_python_chunk2.py")},
  "scored_rows_in_this_chunk": [
    "ADV-14/R", "ADV-15", "ADV-16", "ADV-17", "ADV-18", "ADV-19", "ADV-20",
    "ADV-21", "ADV-22 (mean of ADV-22a, ADV-22b)", "ADV-23", "ADV-24",
    "ADV-25/R", "ADV-26/R"],
  "sub_program_aggregation": {"ADV-22": {"programs": ["ADV-22a", "ADV-22b"],
     "stages": ["Static Checking Before Execution", "Static Checking Before Execution"],
     "scores": [90, 90], "row_stage_score": 90,
     "rule": "unweighted arithmetic mean (spec 25.2)"}},
  "variants_note": "Per opacity_barrier.cases_with_one_variant_only and fixed_scored_case_variant_list, the only case in this chunk with a C/R pair is none: ADV-14 and ADV-25 and ADV-26 are R-only (their hazard is inherently dynamic), and ADV-15, ADV-16, ADV-17, ADV-18, ADV-19, ADV-20, ADV-21, ADV-22, ADV-23, ADV-24 are single-variant. No C variant was invented for a case whose frozen definition does not declare one.",
  "secondary_non_scoring_observations": [
    {"case": "ADV-14/R",
     "what": "Alternative reading of type_binding_table.python_fixed_width_operand_rule in which the function body is written `ctypes.c_int64(n.value * 3)` instead of `n * 3`.",
     "result": "ADV-START printed, then `AttributeError: 'str' object has no attribute 'value'`, exit 1. That diagnostic matches NO fragment of the global lexicon and none of the case's L_TYPE alternatives, so it would classify D5b Crash (20).",
     "why_not_used": "The frozen case ADV-14 says the body is 'return parameter multiplied by the literal 3, using the ordinary multiplication operator', and its silent_transformation_watch pre-registers, before any measurement, that the outcome to watch for is exactly R:777 -- 'a language in which multiplying text by 3 repeats the text' -- which is Python and only Python among the ten. python_fixed_width_operand_rule's why_this_is_needed enumerates the cases it exists for (ADV-01, 02, 03, 04a, 04b, 05, 08) and ADV-14 is not among them; the rule describes how fixed-width VALUES are operated on, whereas ADV-14's whole point is that the argument is deliberately not one. Writing `.value` would presuppose the very type discipline the case is testing for, and would convert a pre-registered Silent Bug into a Crash. The primary implementation therefore uses the direct multiplication and the alternative is published here so an auditor can see both.",
     "affects_recorded_stage": "No. D7b is the only rule by which a secondary observation may move a stage, it can only move a record between Undefined Behavior (5) and Silent Bug (0), and this is a different implementation rather than a checked/sanitizer build of the same one."},
    {"case": "ADV-21",
     "what": "Frozen secondary depths 1000 and 10000 (frozen_generators.parser_nesting.secondary_depths).",
     "result": "Both fail with exit 1 and 'SyntaxError: too many nested parentheses'. CPython's tokenizer nesting limit lies below 1000, so Python's parser limit is roughly 200, far under even the smallest secondary depth.",
     "affects_recorded_stage": "No; non-scoring."},
    {"case": "ADV-14/R, ADV-15, ADV-16, ADV-26/R (the four D7 rows)",
     "what": "D7b test: Python's secondary configuration `python3 -X dev`.",
     "result": "For all four rows -X dev emitted no report of undefined, illegal or erroneous behaviour and produced a payload byte-identical to the primary configuration (R:777, ITERS:6|LEN:6, V:20, IDX:-1). D7b therefore does not fire for any of them and all four fall to D7d, Silent Bug (0).",
     "affects_recorded_stage": "Tested and did not fire."},
    {"case": "ADV-22",
     "what": "Authoring precondition check on ADV-22-valid.py.",
     "result": "5/5 runs exit 0 with OBS=V:1 and ADV-END present. L=155 bytes; byte floor(L/2)=77 falls inside the identifier 'ctypes.c_int64' on line 3, so no string literal and no comment spans the midpoint, as frozen_generators.malformed_source.authoring_precondition requires.",
     "affects_recorded_stage": "No; precondition evidence only."}],
  "observations": obs,
}
os.makedirs(os.path.dirname(OUT), exist_ok=True)
json.dump(doc, open(OUT, "w"), indent=1)
print("wrote", OUT, os.path.getsize(OUT), "bytes")
json.load(open(OUT))
print("JSON parses OK;", len(obs), "observation records")
from collections import Counter
print(Counter(o["earliest_observable_stage"] for o in obs))
