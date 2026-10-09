# Golden probes

Programs that exist only to reach compiler paths the rest of corpus A misses
(see ../README.md, Inputs). Every `*.qui` file here is a corpus entry, except
under `packages/`, which holds probe packages that the entries import
(`QUIDRA_PACKAGE_PATH` lists `packages/` before the pinned store). `*.session`
files are REPL sessions, read the way `quidra repl < FILE` reads its input.
A new probe is a new corpus entry, so `corpus.toml` is re-pinned with it
(`corpus.py pin`).

`minimal.qui` is the minimal program in whose LLVM, emitted afresh by
`tests/llvm_backend_tests.cpp`, the runtime prelude must occur exactly once
(gate Gprel).

`backend/` holds programs for backend paths the other inputs miss: numeric
casts, parses and formatted interpolation in their less common type
combinations, arithmetic on exact and unsigned values, string-build parts of
narrow integers and bools, a command-line binding with a value of every
scalar type, and fixed-array writes and a function value with two arguments.

`lowering/` holds programs for lowering paths the other inputs miss: the
collection and text idioms in their less common shapes, temporaries the
lowering releases after use (an element copied out of an array the
expression owns among them), a one-character comparison with the literal on
the left, `try` over several alternatives, backward over a class that
reaches itself through an array, and the check-elision facts on nested
loops and length relations.
`repl/replay_exit.session` replays a submission that ends the program.

`arguments/` holds calls that pass a by-value argument borrowed while the
call writes the same storage: through another `&` argument (element
writes, a replacement, a field, a tensor element), through the array of an
element, through the receiver of a method that writes it, and through an
argument evaluated after it, next to `int` and `string` arguments, which
are never borrowed, and a call that writes other storage; and the identity
of const parameters and locals, whose element addresses never equal the
caller's. Their expected output is in `tests/cli_tests.sh`.

`assignment/` holds compound assignments whose right-hand side grows,
shrinks or replaces the array or object their target belongs to: through a
`&` argument, an explicit or implicit receiver, a shared `ref.Cell` and an
aliasing reference, with `int` and `string` elements and fields. Their
expected output is in `tests/cli_tests.sh`.

`evaluation_order/` pins source evaluation order: call arguments (named
ones in source order, omitted defaults after them), a receiver before its
arguments, binary operands, concatenations, array literals, interpolation
segments, index operands, and constructor arguments before field defaults
and the body; and plain assignments, which evaluate the target's index
operands before the right-hand side and validate the target at the write,
beside compound assignments, which validate and read the target before the
right-hand side. Their expected output is in `tests/cli_tests.sh`.

`loops/` holds value collection loops whose body appends to, replaces, or
writes elements of the iterated storage: `int[]`, `string[]`, fixed and
nested fixed arrays and `bin`, fields reached through the implicit or an
explicit receiver, writes through element references and aliasing
reference parameters, and loops left by break, return and `try`. Each
iterates the value at loop entry; the expected output is in
`tests/cli_tests.sh`.
The `reference_*` programs there are reference loops over a reference
binding (a `&` parameter, a reference local to an element) whose body
replaces or resizes the iterated array through another binding: a call on
another reference to its holder, an appending assignment through another
`&` parameter, an assignment to the element a reference local designates,
and a growth followed by a break, each stopping with `FOR_ITERATION`; and
loops whose aliases only write elements, left by continue, break and
return, which run as before.

`optimizer/` holds programs for the optimizer's rejections: chains of
operations that chain fusion refuses (the second call does not take the
first's result, a replacement of another arity or parameter type, a
writable side input, a chain through an operation that is not bound) and
calls that the conditional rules refuse (dtype, rank, shape, layout, last
use, an unbound or unsafe replacement), with one rule that clones a
borrowed argument for its by-value target. They import
`packages/probe_rules`, whose descriptor also holds the rule values the
parser refuses. The rejections no lowered program gives are hand-built
modules in `../fixtures/optimizer_probes.hpp`.

`lang5a/` holds programs for the runtime failures and error values whose
reports and lowering a diagnostics change alters: index bounds of dynamic
and fixed arrays, strings and `bin`, a string slice, a narrowing
conversion, a numeric parse, the call depth limit, a missing file, an
uninitialized element, overflow, division by zero, error values (creation,
interpolation, equality, `try`, `match`) and an unhandled error, and
failures inside a local module (`modules/`) and inside package code
(`packages/probe_failing`). Each is also a `[[run]]` entry, so the `run`
view records what it prints and how it stops.

`numeric/` holds programs for the numeric paths a numeric-type change
alters: every fixed-width integer kind at the ends of its range, `bigint`
in and out of loops, real literals near rounding boundaries, floating
arithmetic and formatted interpolation, non-finite values, exact `bigreal`
arithmetic, conversions, parsing, powers, literals in typed contexts (and
`literal_rejected.qui`, rejected today), generics constrained by numeric
family, tensor arithmetic and JSON numbers. `exact_constants.qui` uses
`packages/exactprobe`, a provider of one exact-real atom (the square root
of two, by its decimal expansion) and one unary operation (negation)
through the native extension interface, so that no Core probe depends on
Math; it is compiled only (its native code is not built for the `run`
view). The others are `[[run]]` entries.
