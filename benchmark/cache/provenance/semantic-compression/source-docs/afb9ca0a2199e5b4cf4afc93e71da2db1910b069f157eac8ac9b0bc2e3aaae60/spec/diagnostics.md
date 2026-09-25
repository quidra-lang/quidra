# Diagnostics and Runtime Failures

Compiler diagnostics have a stable code, source span, and explanatory message. `quidra check program.qui --json` is the machine-readable interface. The default diagnostic limit is 20; `--max-errors N` selects a positive limit, and JSON output reports whether additional diagnostics were suppressed.

Codes are contracts for the category of failure. Message wording may become more specific without changing the code.

## Diagnostic codes

| Code | Meaning | Typical correction |
| --- | --- | --- |
| `AMBIGUOUS_NUMERIC_LITERAL` | A numeric-family literal or expression does not have one unique concrete numeric type. | Add an explicit concrete numeric type or cast so the context determines exactly one representation. |
| `AMBIGUOUS_TYPE` | Context does not determine one valid type. | Add an explicit type or remove the ambiguous alternatives. |
| `ARGUMENT_MISMATCH` | Call arguments violate arity, ordering, names, defaults, or constructor shape. | Match the declared parameters and named-argument rules. |
| `ARRAY_SHAPE` | Fixed or inferred array shapes are incompatible. | Use the required dimensions or a compatible dynamic array. |
| `BIN_PARSE` | A statically known `bin.parse` input contains characters other than `0` and `1`. | Use only `0` and `1` in the parsed bit text. |
| `CONST_INITIALIZATION` | A const value binding or const field is not initialized at the point where its immutable contract begins. | Provide an initializer; const fields must be supplied or defaulted during construction. |
| `DIVIDE_BY_ZERO` | Integer division or remainder has a divisor proven to be zero. | Make the divisor nonzero; dynamic zero remains a checked runtime failure. |
| `DUPLICATE_ARGUMENT` | A call supplies the same logical argument more than once. | Supply each positional/named option exactly once. |
| `DUPLICATE_IMPORT_ALIAS` | Two imports expose the same alias. | Give imports distinct aliases. |
| `DUPLICATE_NAME` | A declaration duplicates an existing name or uses a name reserved for that declaration kind. | Rename or remove the declaration; reserved names cannot be reused. |
| `ENUM_CYCLE` | Enum payload types form a recursive value cycle. | Break the recursive enum payload cycle. |
| `FFI_CALLBACK_TYPE` | An external C callback uses a function signature outside the explicit callback ABI subset. | Use capture-free `fn` values with only `int32`, `uint32`, `int`, `uint64`, `float32`, or `float` value parameters and the same scalar set or `void` result. |
| `FFI_DEFAULT` | An external C parameter declares a default argument. | Remove the default; external C parameters are supplied at every call site. |
| `FFI_REFERENCE` | An external C parameter uses a reference/const form its type does not admit. | Pass scalars and callbacks by value; use `const string &`, `const bin &`, or mutable `bin &` only where admitted. |
| `FFI_SYMBOL` | An external C symbol is not an ASCII C identifier. | Bind an ordinary C identifier symbol. |
| `FFI_SYMBOL_CONFLICT` | An external C symbol is reserved by the compiler/runtime implementation, or is already bound by another `extern` declaration. | Use a distinct C wrapper symbol; bind each C symbol once per compilation. |
| `FFI_TYPE` | An external C result or parameter type is not admitted at the C ABI boundary. | Use `void` or an explicit numeric/bool scalar result, and scalar values, ABI-safe callbacks, or explicit string/bin borrows. |
| `FLOAT_RANGE` | A floating literal is not a finite representable source literal. | Use a finite literal in range. |
| `FUNCTION_NOT_VALUE` | A function or method name is used where a first-class value is required. | Call it directly; declarations are not first-class values. |
| `FUNCTION_REFERENCE_CONTEXT` | A named function is used as a value without an explicit compatible `fn<...>(...)` type context. | Provide the complete function-value type context or call the function directly. |
| `FUNCTION_REFERENCE_EXTERN` | An `extern` function is used as a first-class Quidra function value. | Wrap the foreign call in an ordinary Quidra function before using it as a value. |
| `FUNCTION_REFERENCE_SIGNATURE` | A function does not match the required first-class function signature, or uses unsupported reference parameters. | Use an exact by-value `fn<...>(...)` signature or adjust the function declaration. |
| `GENERIC_ARGUMENTS_REQUIRED` | A generic class was used where explicit type arguments are required. | Supply the class type arguments with `<...>`; generic functions and methods may omit them when inference is unambiguous. |
| `GENERIC_ARITY` | The number of generic arguments is wrong. | Supply exactly the declared number. |
| `GENERIC_CONSTRAINT` | A generic constraint name is unknown or a concrete type does not satisfy the declared constraint. | Use a supported built-in constraint and a type that satisfies it. |
| `GENERIC_INFERENCE` | A generic function or method call does not determine every type argument. | Supply explicit type arguments with `<...>`. |
| `GENERIC_RECEIVER` | A generic method receiver is invalid for specialization. | Use the declared generic class/method receiver. |
| `GENERIC_TARGET` | Type arguments were supplied to a non-generic or invalid target. | Remove them or call the intended generic declaration. |
| `IMPORT_CONTEXT` | Imports were compiled through an API without file/module context. | Use file-aware compilation. |
| `IMPORT_CYCLE` | Module imports form a cycle. | Break the cycle. |
| `IMPORT_IO` | An imported source cannot be read. | Correct the path or filesystem access. |
| `IMPORT_PATH` | An import target/path form is invalid. | Use a supported logical, relative, or `@/` path. |
| `IMPORT_TOP_LEVEL` | An imported module contains executable top-level statements. | Keep imported modules declaration-only. |
| `INDENTATION` | Indentation violates Quidra's block rules. | Use four spaces per level and no indentation tabs. |
| `INDEX_ARITY` | An index list has the wrong shape for the indexed value. | Arrays use one integer index; bin uses one integer index or a bit slice; tensors may use comma-separated indices/slices up to runtime rank. |
| `INDEX_BOUNDS` | A constant array index is provably outside a statically known array length. | Use an index in the valid half-open range. |
| `INDEX_SYNTAX` | A tensor index component is structurally incomplete. | Supply an integer index or a valid slice. |
| `INTEGER_RANGE` | An integer literal exceeds `uint64`, or exceeds default `int` without an explicit wider unsigned context. | Use a representable literal; values above signed `int` maximum require an explicit `uint64` context. |
| `INVALID_ASSIGNMENT` | The left side is not assignable storage or assignment violates its contract. | Assign to valid mutable storage. |
| `INVALID_AUTO` | `auto` appears where inference is not supported or lacks an initializer. | Use an explicit type or initialize the local. |
| `INVALID_CONTEXT` | A declaration or operation is used in a frontend context where it is not legal. | Move it to a supported context. |
| `INVALID_ENUM` | A referenced enum declaration is structurally invalid. | Correct the enum declaration before using the type. |
| `INVALID_PATCH` | Patch JSON or schema is invalid. | Follow `patch-schema.md`; the message identifies the field/schema error. |
| `INVALID_TYPE` | A type is not legal in the current storage/declaration position. | Use a storable/supported type for that position. |
| `INVALID_UTF8` | Source bytes are not valid UTF-8. | Save the source as UTF-8. |
| `LEX_ERROR` | Source text cannot be tokenized. | Correct invalid characters, malformed numbers, strings, or delimiters. |
| `LOOP_CONTROL_CONTEXT` | `break` or `continue` appears without an enclosing loop. | Use loop control only inside `while` or `for`. |
| `MATCH_CASE` | A match case is duplicate or incompatible with the subject union. | Use each actual alternative exactly once. |
| `MATCH_EXHAUSTIVE` | A match omits one or more alternatives. | Cover every union alternative. |
| `MISSING_RETURN` | A non-`void` function can reach the end. | Return a value on every continuing path. |
| `NESTING_DEPTH` | Expression or statement nesting exceeds the compiler's safety budget. | Reduce nesting: split the expression or extract a function. |
| `NOT_CALLABLE` | A binding is invoked even though its type is not callable. | Call a function/function-value binding instead, or remove the call syntax. |
| `NUMERIC_CAST` | A statically known explicit numeric cast would change the value. | Choose an exact destination or an operation that states the intended transformation. |
| `NUMERIC_FAMILY` | Integer-family and real-family literals/expressions are being mixed or materialized across families implicitly. | Keep the expression within one numeric family or write an explicit cast/conversion. |
| `PACKAGE_COMPATIBILITY` | An installed package's declared Quidra version range does not include the running compiler, or the package's declared `requires.abi` is not equal to the compiler's ABI version. | Install a compatible package release or use a compatible Quidra compiler. |
| `PACKAGE_DEPENDENCY` | A package dependency is missing, malformed, or does not satisfy the declared version requirement. | Install/fix the declared dependency at a compatible version. |
| `PACKAGE_IMPORT` | An unquoted package name is structurally invalid. | Use a package name containing only ASCII letters, digits, `_`, and `-`. |
| `PACKAGE_LOCK` | `quidra.lock` cannot be read or parsed, or an installed package tree cannot be hashed. | Correct the lockfile or regenerate it with `quidra lock FILE.qui`. |
| `PACKAGE_LOCK_MISMATCH` | An installed package does not match the SHA-256 recorded in `quidra.lock`. | Reinstall the locked package, or intentionally regenerate the lockfile. |
| `PACKAGE_LOCK_MISSING` | An imported package is not recorded in `quidra.lock`. | Regenerate the lockfile with `quidra lock FILE.qui`. |
| `PACKAGE_LOCK_UNUSED` | `quidra.lock` records a package the current import graph never reaches. | Regenerate the lockfile with `quidra lock FILE.qui`. |
| `PACKAGE_LOCK_VERSION` | An installed package version does not match the version recorded in `quidra.lock`. | Reinstall the locked version, or intentionally regenerate the lockfile. |
| `PACKAGE_MANIFEST` | An installed package manifest is invalid or inconsistent with the package being resolved. | Correct/reinstall the package so its manifest is valid and names the resolved package. |
| `PACKAGE_NOT_INSTALLED` | An unquoted non-standard package cannot be resolved from configured package paths. | Install/configure the package, correct its name, or use a quoted source-module path for local code. |
| `PACKAGE_RESOLUTION_CONFLICT` | One package name resolved to more than one installed location within a single compilation. | Ensure the package name has exactly one installation across the configured package roots. |
| `PARSE_DEPTH` | Parser nesting exceeds the safety budget. | Reduce pathological nesting. |
| `PARSE_ERROR` | Tokens do not form valid Quidra grammar. | Correct the syntax near the reported span. |
| `PATCH_HASH_MISMATCH` | A patch node changed since inspection. | Re-inspect and use the new hash. |
| `PATCH_KIND_MISMATCH` | A patch target exists but its structural node kind differs from `expected_kind`. | Re-inspect the source and regenerate the patch against the current node kind. |
| `PATCH_OVERLAP` | Patch operations target overlapping spans. | Split or remove overlapping replacements. |
| `PRIVATE_MEMBER` | A private field is read, written, or addressed outside its declaring class, or a private method is called outside its declaring class. | Access the member only from a method declared by its owning class, or expose an intentional public method. Private fields may still be supplied by name during construction. |
| `RANGE_CONTEXT` | A `range` value is used outside its supported iteration context. | Use it as the iterable of `for`. |
| `REFERENCE_BINDING` | An address/reference target or binding form is invalid. | Use an addressable binding, field, or element with the required `&`. |
| `RESERVED_MAIN` | Source declares the compiler-reserved native entrypoint name. | Rename it; top-level statements define program entry. |
| `RETURN_OUTSIDE_FUNCTION` | `return` appears at top level. | Return only from a function/method. |
| `SHADOWING` | A user declaration reuses a reserved identifier, hides a visible name, or conflicts with an occupied class member name. | Choose a distinct non-reserved name; qualification does not make a reserved identifier reusable. |
| `SHIFT_COUNT` | A statically known shift count is negative or not smaller than the fixed-width integer operand. | Use a shift count in `[0, width)`. |
| `SLICE_STEP` | A tensor slice step is statically known to be non-positive. | Use a positive step. |
| `STALE_REVISION` | Patch base revision does not match current source. | Re-inspect and regenerate the patch. |
| `STANDARD_KEY_TYPE` | A standard-library keyed container uses an unsupported key/element type. | Use one of the documented deterministic key types. |
| `STANDARD_NAMESPACE_IMPORT` | Source attempts to import or alias a standard namespace that is already always visible. | Remove the import and use the standard namespace directly. |
| `SUMMARY_ANALYSIS` | Interprocedural initialization/effect summaries cannot reach a valid stable contract. | Simplify or correct the recursive effect relationship reported. |
| `TENSOR_SHAPE` | A statically known tensor rank/shape relation violates an operation's shape contract. | Use tensors with the required compatible rank and shape. |
| `TRY_CONTEXT` | `try` cannot propagate `error` through the enclosing return type. | Use it inside a function whose return accepts `error`. |
| `TYPE_MISMATCH` | An expression's type violates the required type contract. | Use a matching type or an explicitly supported exact conversion. |
| `UNINITIALIZED` | Storage may be read before definite initialization. | Initialize it on every continuing path before reading. |
| `UNINITIALIZED_ARGUMENT` | A value argument does not meet required initialization state. | Initialize the required fields/storage first. |
| `UNINITIALIZED_FIELD_EQUALITY` | Equality would inspect a class field that may be uninitialized. | Fully initialize recursively compared fields first. |
| `UNINITIALIZED_UNION_PAYLOAD` | A partially initialized class value is being converted into a union. | Fully initialize the class before using it as a union alternative. |
| `UNKNOWN_GENERIC` | A referenced generic declaration cannot be found. | Correct the generic name/import. |
| `UNKNOWN_GENERIC_METHOD` | A referenced generic method cannot be found. | Correct the receiver/method/type arguments. |
| `UNKNOWN_MEMBER` | Class/member lookup failed. | Use a declared field or method. |
| `UNKNOWN_METHOD` | Method lookup failed for a value or standard-library handle type. | Use a method declared for the receiver type or correct the method name. |
| `UNKNOWN_MODULE_MEMBER` | Imported-module member lookup failed. | Use an exported declaration from that module. |
| `UNKNOWN_NAME` | Value/function name lookup failed. | Declare/import the name or correct the spelling. |
| `UNKNOWN_NODE` | A source patch references a node absent from the inspected revision. | Re-inspect and use a current node id. |
| `UNKNOWN_STANDARD_MODULE` | Standard-namespace lookup requested a name absent from the language registry. | Use one of the standard namespaces defined by the current language version. |
| `UNKNOWN_TYPE` | Type name lookup failed. | Use a built-in, generic parameter, imported type, or declared class. |
| `WRITE_CAPABILITY` | A write is attempted through a const path, reference form mismatches the parameter, authority is improperly regained, or a writable path is otherwise unsafe. | Match `&` reference contracts; use `const T &` for read-only access and `T &` only from a path that already has write authority. |

## Compile-time guarantees

The checker rejects unsafe or ambiguous operations including uninitialized reads, incompatible value/shape conversions, writes or rebinding through const paths, authority escalation from `const T &` to `T &`, invalid class initialization, non-exhaustive typed matches, import/generic errors, statically known integer division by zero, unsupported numeric cast categories, and statically known integer cast range failures. Multiple reference arguments may intentionally alias the same storage; const is path-local rather than a global freeze.

Parser and checker recovery occurs only at safe boundaries. Expressions depending on an already-invalid binding use an internal invalid type so one root failure does not needlessly cascade.

## Runtime failures

Deterministic runtime safety failures terminate with status `101`. They include:

- integer overflow at every supported signed or unsigned width;
- dynamically determined integer division or remainder by zero;
- dynamically determined fixed-width integer shift counts outside `[0, width)`;
- array or bin index bounds violations (array failures include the offending index and current length);
- reads from runtime-tracked array or tensor storage that has not been initialized;
- invalid allocation sizes;
- integer casts whose runtime value is outside the destination range;
- `math.trunc`, `math.round`, `math.floor`, and `math.ceil` results to `int` that are not finite or fall outside `int` range;
- tensor shape, broadcasting, indexing, contiguity, and elementwise cast range violations;
- neural operand shape and dtype mismatches, layer rank and dimension preconditions, autograd and gradient/Parameter correspondence, and optimizer moment-state validity;
- `.quistate` save and load failures, including an invalid path, file I/O failure, and a schema, field path/type, tensor dtype/shape, version, bounds, or checksum disagreement;
- invalid runtime text operations such as malformed UTF-8 or out-of-range string slices;
- a zero `range` step;
- call depth exceeding the native safety limit before host stack exhaustion.

An explicit `error("message")` is instead a typed value. Numeric `Type.parse(text)` returns `error` for invalid or out-of-range text. `input()` returns `none` for EOF and `error` for input failure.

Floating-point exceptional values follow IEEE-754 behavior. Runtime text is canonicalized to `nan`, `inf`, and `-inf`.


## Runtime failure format

Coded runtime safety failures use status 101 and the stable text shape:

`Quidra runtime error[CODE] at LINE:COLUMN: message`

Current coded runtime failure codes include `INDEX_BOUNDS`, `UNINITIALIZED`, `TENSOR`,
`NEURAL`, `NEURAL_STATE`, `INTEGER_OVERFLOW`, `DIVISION_BY_ZERO`, `RANGE_STEP_ZERO`,
`CALL_DEPTH_LIMIT`, `NUMERIC_CAST_RANGE`, `NUMERIC_CONVERSION`, `SHIFT_COUNT`, `INVALID_SLEEP_DURATION`,
and `INVALID_RANDOM_RANGE`.
Source-bearing coded operations report their source location.

Runtime-library failures that currently do not carry a source span, such as host allocation
failure or low-level text/runtime invariant checks, use `Quidra runtime error: message`
and still terminate with status 101 rather than inventing a source location.

## REPL session safety errors

`REPL_REPLAY_UNSAFE` is a REPL session-safety error rather than a source diagnostic or runtime failure. It means the accumulated session may already have executed an external or nondeterministic effect that cannot be reconstructed safely by rerunning accepted source. Use `:reset` before executing another native submission. The barrier is deliberately conservative and may remain armed after a failed runtime submission if an effect could have occurred before the failure.
