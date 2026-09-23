# Certified benchmark cache

This directory contains **validated measurement cache records**, not benchmark source
inputs and not historical run directories.

A cache record may be reused only when the runner recomputes the complete benchmark
input fingerprint and it matches exactly. The fingerprint includes the exact Task
Packet, model/provider, frozen sampling policy, relevant toolchain versions,
validator/workload inputs and the cache epoch.

Quidra-containing work is cached too, keyed by the versions the evaluated snapshot
declares in project.toml (`version` and `language_version`) and by the exact Task
Packet, which embeds the snapshot's docs: a Quidra record is reused only while
neither has changed, so a run in which a comparison language failed no longer
discards Quidra's own completed measurements.
Ecosystem evidence uses a declared epoch (`declared_epochs.ecosystem` in the cache
policy) because external ecosystem facts change without a language version change;
the operator changes that value when they should be measured again. Semantic
Compression uses one too (`declared_epochs.semantic_compression`): its packets are
decoded at the depth `inference_gateway.json` pins for that evaluation, and the
epoch retired the records certified before that depth was lowered, so every score
in the evaluation comes from one depth.

The three Language Quality mechanical units are certified as well: the micro
suite (`lq-micro-mechanical`), the adversarial case set (`lq-adversarial-mechanical`)
and the audit of the snapshot's own Quidra programs (`lq-quidra-audit`). No model is
involved, so their key carries no provider or sampling; it carries the pinned
toolchains of every language they measure, the programs, fixtures, workloads and
validators they read, the measurement scripts (`scripts/micro_measure.py` and
`scripts/adversarial_measure.py`), the snapshot's Quidra versions and the declared
`mechanical` epoch. The record holds the requirement-level result; the raw process
captures (tens of megabytes) stay in the run's retained workspace artifact and are
named by hash in the record's certification. They live under
`v1/language-quality/mechanical-<action>/`. The reason is time rather than money:
the measurement takes about six hours on a hosted runner, and a run that repeats it
cannot also finish its paid units inside the six-hour job limit. Change
`declared_epochs.mechanical` when the runner class changes or the numbers should be
taken again.

Workers never receive this directory as a readable path. Cache hydration is a
trusted runner operation, and every hydrated result is passed through the current
validator before it can become COMPLETE.

An eligible non-Quidra measurement record is certifiable as soon as that unit is
COMPLETE with its frozen validator recorded as PASS; the surrounding Primary
evaluation does not have to be complete. The trusted host may checkpoint those
records after the scored sandbox exits even when the paid run stopped early.
This makes retries incremental rather than cold restarts. The sandbox keeps the
cache snapshot from run start, so it cannot observe records produced by itself.

Production requests may also be evaluation-scoped. When `benchmark/.run-production`
contains `evaluation: <primary_id>`, only that Primary is dispatched. Its validated
non-Quidra units are checkpointed here without requiring a global finalize; a later
full run can hydrate them and pay only for cache misses plus always-fresh Quidra work.
Use `evaluation: all` (or omit the field) for a normal full benchmark.
