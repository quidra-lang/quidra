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
Ecosystem evidence uses a UTC-month epoch because external ecosystem facts change
without a language version change.

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
