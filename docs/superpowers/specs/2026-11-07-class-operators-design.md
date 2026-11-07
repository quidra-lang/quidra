# Class call and index operators

The approved change names the callable operator `operator()` and the indexing
operator `operator[]`. Their parameter lists follow their names:
`int operator()()`, `int operator()(int value)` and
`int operator[](int index)`. Work and push directly on `develop`.

The previous bare `operator` method spelling is removed rather than retained as
an alternative callable spelling. `operator` remains available as an ordinary
identifier. Each operator has one ordinary method definition per class; call
and index operators may coexist. Generic classes, access control, defaults,
checked positional value-argument conversions, value semantics and typed errors follow ordinary
methods. Existing `.value(...)` mutation authority remains unchanged.

Index access `value[a, b]` invokes `operator[](a, b)`, evaluating the receiver and
arguments once, left to right. A mutating index operator requires `.value[a, b]`.
Index operators return values: their results cannot be assignment targets or
writable references. Slices and empty `[]` retain their existing meanings and
are not converted into user-defined index calls. Built-in array/string/bin/tensor
indexing and standard file-stream behavior must remain unchanged.

## Approaches

Keeping the existing special method name `operator` would minimize migration but
would treat the call and index spellings inconsistently. The selected approach
uses the complete operator names and lowers both to ordinary method calls,
reusing existing checking, effects and native lowering. Reference-returning
indexing is outside this change because it needs a separate lifetime contract.

Update affected compiler tooling, grammar, documentation, tests and first-party
repositories. No unrelated syntax or API changes belong in this work.
