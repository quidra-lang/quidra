# Checked numeric conversion and range constraints

Approved by the user in the conversation on 2026-11-07; implementation is authorized directly on develop, without creating branches, followed by verification and push.

## Intent and scope

Allow checked implicit conversion only between numeric types (nat/int/real/com), consistently in initialized declarations, assignments, returns, defaults, and value arguments. Preserve the numeric common-type rules for operators and comparisons. Reject integer truth conversion in every boolean context. References retain exact storage types. Keep complex-to-real/integer conversion unsupported; select a component explicitly. Literal families may first materialize in their own category before checked conversion into a numeric destination.

Add ordered scalar range constraints to bindings, guarded assignment, function/method/class-header parameters, and tensor shape axes. Examples:

```quidra
0 < x < 5 := 3
y := 4
0 < x < 5 = y
void f(3 < int n < 7, limit < 9 := 8)
    println(n)
void image(tensor<real32>{1 <= nat B <= 64, 3, nat H > 0, nat W > 0} pixels)
    println(B)
```

The annotated type or initializer/default determines a binding/parameter type before comparing. Bounds use ordinary numeric common-type conversion; they do not change storage types. Evaluate each initializer/RHS and each bound at most once. Convert the candidate into its destination type, validate the candidate, and only then commit a guarded assignment. An ordinary comparison remains bool. Constraint failures provable with compiler constant facts are compile errors; other failures fail fast with RANGE_CONSTRAINT before the function/class body or the guarded store. Release builds retain checks unless proven redundant.

Existing shape names retain equality semantics: `{0 < x < 5}` means axis == nat(x) plus a range constraint on x; `{0 < nat N < 5}` captures the incoming extent in a new read-only nat N. `{N := 32}` remains an exact extent, not a default. Capture shape extents once; later changes of source variables do not change an established shape contract. Shape-match constraints participate in case selection rather than aborting a nonmatching case.

## Approaches

The conversation considered treating comparisons as ordinary boolean dimensions, admitting constraints only with explicit declarations, and recognizing constraints in declaration/shape contexts. The final choice is contextual range syntax with explicit :=/= distinctions and numeric-only implicit conversion. This preserves useful existing-variable shape constraints while preventing bool-to-number coercion.

## Implementation rulings

- A chained range identifies its central binding. A single comparison identifies its left variable when both operands are existing variables; literals on the left identify the right variable. An explicit typed binding always identifies the subject. Ambiguous or malformed ranges are diagnosed, not guessed from runtime values.
- Class-header range checks are generation-time preconditions. They do not create permanent member invariants; a later guarded assignment explicitly checks its own candidate.
- Bounds are ordered numeric scalar expressions. Constraints do not add general boolean predicates or assignment expressions.
- No release or version bump is requested. Update affected first-party source/examples and documentation to use explicit boolean tests.

## Validation

Exercise scalar widening/narrowing/rounding/integrality, numeric containers, defaults/returns, bool rejection, fixed references, inferred and typed ranges, inclusive/exclusive endpoints, exactly-once RHS and bounds, validation after conversion, parameter defaults/named calls/indirect calls/classes/generics/imports, shape captures/equality/reassignment/match behavior, formatter round trips, and runtime failure in optimized builds. Run the repository test suite and affected first-party checks.
