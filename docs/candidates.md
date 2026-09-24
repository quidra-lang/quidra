# Language design candidates

These are proposed language changes, not current specification.

## Tensor `!=` should negate `==`

**Status:** Candidate

Current tensor `!=` is true only when every corresponding element is unequal. This breaks the usual invariant that `a != b` means `!(a == b)`.

Proposed semantics for equal-shaped tensors:

- `a == b`: true iff every corresponding element is equal.
- `a != b`: exactly `!(a == b)`; true iff at least one corresponding element differs.
- Empty equal-shaped tensors therefore satisfy `a == b` and not `a != b`.
- Other relational operators remain unchanged unless reconsidered separately.

If an “all corresponding elements are unequal” operation is useful, expose it explicitly rather than overloading `!=`.
