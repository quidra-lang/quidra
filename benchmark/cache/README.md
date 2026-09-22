# Certified benchmark cache

This directory contains **validated measurement cache records**, not benchmark source
inputs and not historical run directories.

A cache record may be reused only when the runner recomputes the complete benchmark
input fingerprint and it matches exactly. The fingerprint includes the exact Task
Packet, model/provider, frozen sampling policy, relevant toolchain versions,
validator/workload inputs and the cache epoch.

Quidra-containing work is never cached because Quidra is the changing target.
Ecosystem evidence uses a UTC-month epoch because external ecosystem facts change
without a language version change.

Workers never receive this directory as a readable path. Cache hydration is a
trusted runner operation, and every hydrated result is passed through the current
validator before it can become COMPLETE.
