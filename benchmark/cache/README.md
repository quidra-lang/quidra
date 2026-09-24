# Certified benchmark cache

This directory contains **validated measurement cache records**, not benchmark source
inputs and not historical run directories.

A cache record may be reused only when the runner recomputes the complete benchmark
input fingerprint and it matches exactly. The fingerprint includes the exact Task
Packet, model/provider, frozen sampling policy, relevant toolchain versions,
validator/workload inputs and the cache epoch. Primary configuration is scoped by
evaluation: every worker sees shared Primary policy plus only its own evaluation-local
section. A Proficiency-only configuration change therefore cannot invalidate or even
change the prompt of Learnability, Language Quality, Semantic Compression, or
Ecosystem.

The same rule applies below Primary configuration: a unit hashes only the methodology
sections actually embedded in its Task Packet and the exact assigned-requirement
projection, not unrelated parts of those files.

Records created before these scoping rules are not discarded. On an exact-key miss,
the trusted runner may recover one historical record only when every unscoped prompt
component and every other fingerprint dependency still match exactly, the old full
Primary JSON projects to the same configuration for the current evaluation, and the
historical selected methodology/assigned-requirement bodies are byte-identical to
the current ones. That compatibility hit still passes the current validator and is
checkpointed under the new scoped fingerprint. If multiple compatible historical
records disagree on the result, none is reused.

Quidra-containing work is cached too. Its historical fingerprint still carries
the declared `version` / `language_version` and exact Task Packet so already-paid
records remain addressable, but reuse has an additional trusted compatibility
check: `init` records Git object IDs for the compiler/runtime execution inputs
(`src`, `include`, CMake/project/manifest metadata, and the embedded grammar).
A same-version compiler/runtime edit therefore makes the Quidra record a MISS,
while a benchmark-only documentation/harness commit does not. New records carry
this execution identity explicitly. Identity-less legacy records are accepted
only from the migration commits frozen in `cache_policy.json` and only while the
current target still matches that migration baseline.
Ecosystem evidence uses a declared epoch (\`declared_epochs.ecosystem\` in the cache
policy) because external ecosystem facts change without a language version change;
the operator changes that value when they should be measured again. Historical
Ecosystem scores are not migrated across the runner-rubric-v2 boundary: retained
records prove that some language workers used different rubrics for the same metric,
so preserving those scores would break comparability.

Semantic Compression also uses a declared epoch
(\`declared_epochs.semantic_compression\`). An epoch change is not automatically a
paid cache miss. For the explicitly approved historical SC epochs, a language-scoped
record whose model/provider, sampling, toolchains, requirements, Primary projection,
evaluation specification and all non-SC readable inputs still match may be staged as
a **validator-recertification candidate**. The trusted runner then applies the full
current validator, including current canonical-fragment/runtime/fixed-stdout checks.
A PASS is checkpointed under the current fingerprint; a rejection discards only that
candidate and executes the leaf normally. LLM Proficiency deliberately has no such
migration because its prompt allocation and repair trajectory are themselves the
measurement.

Language Quality mechanical work is certified as well. The micro suite
(`lq-micro-mechanical`) remains one intentionally coupled all-language unit because
its timing schedule interleaves languages and normalizes each workload against the
same measured cohort; splitting that timing cohort would reduce comparability. The
adversarial/safety set is independent by language, so
`lq-adversarial-mechanical--<language>` is a separate leaf/cache record. The
snapshot's own Quidra-program audit remains `lq-quidra-audit`. No model is
involved, so their key carries no provider or sampling; it carries the pinned
toolchains of every language they measure, the programs, fixtures, workloads and
validators they read, the measurement scripts (`scripts/micro_measure.py` and
`scripts/adversarial_measure.py`), the snapshot's Quidra versions and the declared
`mechanical` epoch. The record holds the requirement-level result; the raw process
captures (tens of megabytes) stay in the run's retained workspace artifact and are
named by hash in the record's certification. They live under
`v1/language-quality/mechanical-<action>/`. The reason is time rather than money:
the measurement takes about six hours on a hosted runner, and a run that repeats it
cannot also finish its paid units inside the six-hour job limit. The runner
hydrates these records before it runs any command unit, so a certified measurement
is never taken again by a run that could reuse it. Change
`declared_epochs.mechanical` when the runner class changes or the numbers should be
taken again.

Workers never receive this directory as a readable path. Cache hydration is a
trusted runner operation, and every hydrated result is passed through the current
validator before it can become COMPLETE. A structurally intact record that a newer
validator rejects is treated as a cache MISS: only that staged cached output is
discarded and the unit executes normally. Integrity corruption of the cache record
itself remains fatal.

An eligible measurement record is certifiable as soon as that unit is COMPLETE
with its frozen validator recorded as PASS; Quidra records additionally carry the
trusted compiler/runtime execution identity. The surrounding Primary
evaluation does not have to be complete. The trusted host may checkpoint those
records after the scored sandbox exits even when the paid run stopped early.
This makes retries incremental rather than cold restarts. The sandbox keeps the
cache snapshot from run start, so it cannot observe records produced by itself.

Production requests may also be evaluation-scoped. When `benchmark/.run-production`
contains `evaluation: <primary_id>`, only that Primary is dispatched. Its validated units are checkpointed here without requiring a global finalize; a later
full run can hydrate them and pay only for cache misses. Quidra work is reused too when its declared versions, exact Task Packet, and trusted compiler/runtime execution identity remain compatible.
Use `evaluation: all` (or omit the field) for a normal full benchmark.
