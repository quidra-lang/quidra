# Quidra benchmark programs

The Quidra side of the Language Quality benchmark fixtures. The nine comparison
languages keep their programs under `benchmark/template/programs/`; Quidra keeps
them here, inside the evaluated snapshot, because they change with the compiler.
A benchmark run reads this directory read-only from `/quidra-benchmark/repo/`,
re-audits it mechanically against the compiler built from the same commit, and
never reuses it across commits from the template or from a previous run.

```
micro/mb00.qui .. mb11.qui          the frozen micro suite (workloads/micro.md)
adversarial/ADV-*.qui               the frozen adversarial case set (37 scored
                                    programs, ADV-22-valid, ADV-21 depth probes)
adversarial/generate.py             the frozen generators for ADV-21 and ADV-22a/b
quidra_type_binding_amendment.json  the Quidra rows of the case set's type-binding
                                    table, with citations and TM3 branches
representation.json                 the representation/API pins for the micro suite
```

## Rules

- The programs implement the language-neutral pseudocode in
  `benchmark/template/workloads/micro.md` and the constructions in
  `benchmark/template/methodology-assets/language_quality/adversarial_cases.json`
  verbatim: same algorithm, same loop order, same accumulation order, same
  skeleton, no memoisation, no library substitute for a pinned kernel, no
  benchmark-specific hack. `micro.md` section 2.5 and the case set's authoring
  rules A1-A7 and prohibitions P1-P5 apply exactly as they do to every other
  language.
- Representation choices are not free: `representation.json` and the amendment
  pin them, with a citation into `docs/spec/`, before anything is measured.
- `tests/benchmark_programs.sh` builds every program with the compiler from the
  current tree, runs the micro suite once against the frozen oracle, checks the
  adversarial skeleton and the generated sources, and validates both JSON files.
  It runs in CI, so a language change that breaks a program fails the commit
  that made it, exactly like any other test.
- Changing a program is an ordinary code change reviewed with the compiler
  change that motivates it. A benchmark run may report that a program looks
  non-idiomatic or violates a pin; that report is advice for the next commit,
  never an edit made by the run.
