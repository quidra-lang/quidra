# Infinity in constants, tensors and package boundaries

The extended-real policy applies both when the checker evaluates a constant and
when a native operation executes. Nonzero real division by zero, arithmetic
overflow, and zero to a negative real power may produce signed infinity. NaN
is an error, including an intermediate NaN later hidden by raising to zero.
`0 ^ 0`, a negative real base with a fractional exponent, integer division by
zero, out-of-range literals and finite narrowing conversions remain checked.
Exact `real` has no signed zero; fixed-width real types retain its sign.

Tensor element arithmetic and delivered gradients admit signed infinity but
reject NaN. Explicit tensor conversions among fixed-width real formats preserve
infinity and reject a finite value that overflows the destination. Infinity
cannot be converted to an integer tensor.

Native package output validation checks for NaN on both tensor and tensor-array
write boundaries. The legacy `QCORE_OP_FINITE_PRESERVING` flag remains accepted
for compatibility, but it no longer waives validation: preserving finite inputs
does not establish safety for infinite inputs. `QCORE_OP_VALIDATES_NAN` and
`QCORE_OP_NAN_PRESERVING` express the stronger contracts already declared by the
native ABI. Unknown flag bits remain rejected.

CUDA/PTX arithmetic uses `testp.number`, not `testp.finite`. CUDA, HIP and Metal
validation compares signless IEEE encodings with the infinity encoding; NaN is
strictly above that encoding. Range checks on finite narrowing remain separate.
The host generator suite does not constitute a real-GPU execution test.

## Regression coverage

- `quidra_infinity_constant_tests`: 50 production lexer/parser/checker cases.
- `device_infinity_source_tests.cpp`: 60 production CUDA/HIP generator contracts,
  run by the existing PTX contract workflow.
- `infinity_tests.py`: 124 native language cases, including constant negative
  powers of positive and negative zero.
- Runtime tensor/package/fixed/exact/format/string suites and 22 failure modes.

Tensor and package negative modes print and flush `before` immediately before
the operation under test. The failure harness requires that exact prefix, the
runtime error category, and status 101. A premature failure while constructing
a valid infinite input cannot satisfy a NaN-rejection test.

Full platform CI and real GPU execution remain independent verification gates.
