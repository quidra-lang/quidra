# Quidra 0.2.0 defect: repeated string concatenation leaks stack until SIGSEGV

**Found during:** benchmark micro-workload MB-08 (strings), Quidra HEAD `7677581` (branch `develop`).
**Severity:** crash (SIGSEGV) on an ordinary, idiomatic loop. No diagnostic, no error, no partial output.
**Affects:** BOTH execution modes — `quidra build` + run (native) and `quidra run` (interpreter).

This is a defect in the language implementation, not a benchmark finding. It is reported here because
the benchmark surfaced it and the evidence is already reduced to a minimal case.

## Minimal reproducer

```quidra
for w in range(0, 200000)
    string word = ""
    for c in range(0, 3)
        word = word + "x"
print("done")
```

```
$ quidra build repro.qui -o repro && ./repro
$ echo $?
139          # 128 + 11 = SIGSEGV, no output, no diagnostic
```

## It is a STACK leak, proven

The same binary that crashes at the default stack size succeeds when only the stack limit is raised.
Nothing else changed — same binary, same input, same machine:

| Stack limit | Result |
|---|---|
| 8176 KiB (macOS default) | **exit 139, SIGSEGV** |
| 16384 KiB | exit 0, prints `done` |
| 32768 KiB | exit 0, prints `done` |
| 65520 KiB | exit 0, prints `done` |

Heap growth is far too small to explain the crash (~76 bytes per outer iteration, ≈15 MB total), and
peak RSS plateaus at 9.3 MiB exactly as the process dies — the signature of walking off an 8 MiB stack
into the guard page.

## Threshold: it scales with the number of concatenations, not string length

Outer loop fixed at 200000; only the inner trip count varies:

| Inner trip | Total concatenations | Peak RSS | Result |
|---:|---:|---:|---|
| 2 | 400,000 | 7.5 MiB | exit 0 |
| 3 | 600,000 | 9.3 MiB | **SIGSEGV** |
| 4 | 800,000 | 9.3 MiB | **SIGSEGV** |
| 5 | 1,000,000 | 9.3 MiB | **SIGSEGV** |

That implies roughly **14–19 bytes of stack leaked per concatenation evaluation**, never reclaimed at
loop-iteration boundaries. The crossover sits right at the 8 MiB default stack.

Note the strings here are *tiny* (≤5 characters). This is not a large-allocation problem.

## What does NOT reproduce it

Each of these runs 200,000 outer iterations and exits 0, which narrows the trigger considerably:

| Variant | Result |
|---|---|
| One growing string, 500,000 self-concatenations at top level | exit 0 |
| Loop-local string bound to a literal, no concatenation | exit 0 |
| Concatenation assigned into an *outer* binding | exit 0 |
| Loop-local, a single self-concatenation | exit 0 |
| Loop-local, two self-concatenations unrolled | exit 0 |
| Nested loop with assignment but no concatenation | exit 0 |

So the trigger is specifically: **a loop-local `string` binding, self-concatenated inside a nested loop,
repeated enough times in aggregate.** A single growing string at top level is fine, which is why the
defect is easy to miss.

## Why it matters beyond this benchmark

`word = word + letter` inside a loop is one of the most common shapes in ordinary text-processing code.
The failure mode is the worst kind: a clean-looking program, no compile error, no runtime diagnostic,
no partial output, just signal 11. It also contradicts the language's own safety posture — Quidra
otherwise detects integer overflow, out-of-bounds indexing and uninitialized reads at compile time or
with a precise runtime error.

The LLM guide (rule 41) already recommends `values.join(separator)` for assembling strings, and `join`
does work correctly at these sizes. But the recommendation is presented as a performance preference,
not as avoidance of a crash, and `+` is documented and accepted by the compiler.

## Suggested direction (SUPERSEDED — see ROOT CAUSE below)

> The paragraph below was an initial hypothesis written before the compiler source was
> read. It is **wrong** and is kept only to show the reasoning trail. The actual cause is an
> `alloca` emitted outside the entry block; see **ROOT CAUSE** at the end of this document.

The managed-ownership bookkeeping described in `docs/spec/architecture.md` ("typed IR emits
retain/release … composite drops recursively release children") appears not to release the temporary
produced by a string `+` when the result is reassigned to the same loop-local binding, and the
reservation is made on the stack frame rather than per iteration. Releasing the temporary at the end of
each loop iteration, or hoisting the reservation out of the loop, would both fix it.

## Reproduction environment

Quidra 0.2.0 at `7677581` (`develop`), built Release with `cmake --build build -j8`;
Apple M2, macOS 26.4.1 arm64, 8 GiB RAM, Apple clang 17.

## Effect on the benchmark

MB-08's Quidra implementation avoids `+` and uses `string[]` + `join`, which is the language's own
documented recommendation and is a legitimate idiomatic implementation. The workload is therefore
measurable. This defect is recorded as an independent finding and does not alter any score, but it IS
scored where it belongs: as evidence in the adversarial/safety and Implementation Robustness metrics,
because a segfault with no diagnostic is exactly what those metrics exist to detect.

---

# ROOT CAUSE — confirmed in the compiler source

The cause is an `alloca` emitted **at the instruction site** instead of in the function's entry block.

`src/llvm_backend.cpp`, `ir::StringConcat` (line 770):

```cpp
const auto items = temp("string.concat.items");
out << "  " << items << " = alloca ptr, i64 " << n.values.size() << "\n";
```

and the identical pattern in `ir::StringAppendMove` (line 785):

```cpp
const auto items = temp("string.append.items");
out << "  " << items << " = alloca ptr, i64 " << n.suffixes.size() << "\n";
```

In LLVM, an `alloca` that is **not** in the entry block is not promoted by `mem2reg`/SROA and its stack
space is not reclaimed until the enclosing function returns. Emitted inside a loop body, every *execution*
of the concatenation therefore consumes fresh, permanently-held stack.

## The arithmetic matches the observation exactly

A two-operand concatenation (`a + b`) emits `alloca ptr, i64 2` = two pointers = **16 bytes per
execution**. The default macOS main-thread stack is 8 MiB:

```
8 MiB / 16 bytes = 524,288 executions
```

Measured threshold: survives 500,000 executions, dies at 550,000 — and the threshold is **independent of
string length**, which is exactly what a per-execution fixed-size leak predicts and what a
string-buffer problem would not.

This also explains every result in the table above:

- *One growing string, 500,000 self-concatenations* survives — just under the cliff.
- *200,000 outer iterations x 3 inner concatenations* = 600,000 executions — over the cliff, crashes.
- *Inner trip 2* = 400,000 executions — survives. *Inner trip 3* = 600,000 — crashes.
- The result being **discarded** does not help: the leak is per execution, not per retained value.

## Suggested fix

Hoist the `alloca` into the function's entry block, which is the standard LLVM idiom and lets `mem2reg`
promote it. Since `n.values.size()` is known per instruction, one entry-block `alloca` sized to the
maximum operand count used anywhere in the function would serve every concatenation site in it.
Alternatives: bracket the site with `llvm.stacksave` / `llvm.stackrestore`, or allocate the operand
vector on the heap and free it after `quidra_string_concat_many` returns.

The same fix applies to both `StringConcat` and `StringAppendMove`. It is worth grepping the backend for
other `alloca` emissions at instruction sites, since any of them inside a loop will have the same
behaviour.

## Severity note

`quidra fmt --check` passes, the type checker is satisfied, and the program is idiomatic by the
language's own documentation. The failure is a bare SIGSEGV with no diagnostic. That combination —
accepted by every static check, then dying silently at scale — is precisely the class of defect Quidra's
design otherwise works hard to eliminate.
