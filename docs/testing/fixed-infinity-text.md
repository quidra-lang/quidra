# Signed-infinity text contracts

Fixed-width real values (`real16`, `real16b`, `real32`, `real64`) format positive
and negative infinity as `inf` and `-inf`. Explicit integer-width, zero-padding,
fractional-digit and significant-digit options do not change these spellings.
Exact `real` keeps its separate canonical spellings `∞` and `-∞`.

Numeric parsing accepts signed infinity while continuing to reject NaN, invalid
suffixes and finite decimal or hexadecimal text outside the destination range.
Failed parses leave the output slot unchanged. Unicode infinity is accepted by
exact `real`, not by the fixed-width parsers. The native CLI real parser uses the
same infinity policy as fixed-width value parsing.

NaN must never become observable text. All fixed-width formatter entry points,
including explicit digit formats, stop with the existing runtime failure exit
status 101 if an internal NaN reaches them. They report that NaN reached real
text formatting; a valid infinity is not misdiagnosed as an internal error.

`fixed_infinity_tests.cpp` covers parsing, sign preservation and all formatter
options; `infinity_runtime_failure_tests.py` also covers twelve NaN-formatting
paths. `numeric_formats_tests.cpp` and `runtime_string_tests.cpp` retain the
finite rounding, round-trip and string-runtime regressions.
