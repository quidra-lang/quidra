# Diagnostics and Runtime Failures

Compiler diagnostics have a stable code, source span, and explanatory message. `quidra check program.qui --json` is the machine-readable interface. Diagnostic JSON schema version 2 also reports the SHA-256 `source_revision` and, when the source can still be parsed and one containing source node is unambiguous, the smallest matching `node_id` and `node_kind`. Lexer/parser failures and other cases without a stable node retain `source_revision` plus the exact span and return null node fields rather than inventing an association. Span start/end include byte offsets as well as line/column.

The default diagnostic limit is 20; `--max-errors N` selects a positive limit, and JSON output reports whether additional diagnostics were suppressed.

Codes are contracts for the category of failure. Message wording may become more specific without changing the code. Node IDs are revision-local and must be interpreted only with the reported `source_revision`.

## Diagnostic codes

| Code | Meaning | Typical correction |
| --- | --- | --- |
| `AMBIGUOUS_NUMERIC_LITERAL` | A numeric-family literal, contextual constant or expression does not have one unique concrete numeric type, as in `The literal constant 'math.pi' needs a unique real-family type; write 'real64 x = math.pi' or 'real64(math.pi)'.` | Add an explicit concrete numeric type or cast so the context determines exactly one representation. |
| `AMBIGUOUS_TYPE` | Context does not determine one valid type. | Add an explicit type or remove the ambiguous alternatives. |
| `ARRAY_COMPREHENSION` | A generation expression has a source other than a single array value. | Iterate an array using `[for T value = source[] then expression]`, with an optional `if` condition. |
| `ARGUMENT_MISMATCH` | Call arguments violate arity, ordering, names, defaults, or constructor shape. | Match the declared parameters and named-argument rules. |
| `ARRAY_SHAPE` | Fixed or inferred array shapes are incompatible. | Use the required dimensions or a compatible dynamic array. |
| `BIN_PARSE` | A statically known `bin.parse` input contains characters other than `0` and `1`. | Use only `0` and `1` in the parsed bit text. |
| `CHAINED_COMPARISON` | A comparison chain mixes directions (`a < b > c`), chains `!=`, puts a relational comparison next to `==` or `!=` without parentheses, or compares tensors. | Keep one direction (`<`/`<=`, `>`/`>=`, or only `==`), write `(a < b) == c` to compare a result, and combine tensor masks explicitly. |
| `CONSTRUCTOR_SIGNATURE` | A class header lacks its required argument list or has an invalid generation signature. | Write `class Type [instance](...) [\| error]`. |
| `CONSTRUCTOR_RETURN` | An ordered class initialization returns a value other than an allowed error. | Use bare return, or declare the class header with `\| error` and return an error. |
| `CONST_INITIALIZATION` | A const value binding or const field is not initialized at the point where its immutable contract begins. | Provide an initializer; const fields must be supplied or defaulted during construction. |
| `CONST_REFERENCE` | A parameter, binding or field is declared `const T &`; there are no read-only references. | Write `const T name` for a read-only value (passed at the call without `&`), or `T &name` for a reference. |
| `DISCARD` | The discard name `_` is declared or read. | Choose a real name for a value that is used; drop an unused value with an expression statement. |
| `DIVIDE_BY_ZERO` | Integer division or remainder has a divisor proven to be zero, or a fixed-width real division has a constant zero divisor and a constant dividend. | Make the divisor nonzero; dynamic zero remains a checked runtime failure. |
| `DUPLICATE_ARGUMENT` | A call supplies the same logical argument more than once. | Supply each positional/named option exactly once. |
| `DUPLICATE_IMPORT_ALIAS` | Two imports expose the same alias. | Give imports distinct aliases. |
| `DUPLICATE_NAME` | A declaration duplicates an existing name or uses a name reserved for that declaration kind. | Rename or remove the declaration; reserved names cannot be reused. |
| `ENUM_CYCLE` | Enum payload types form a recursive value cycle. | Break the recursive enum payload cycle. |
| `ERROR_CODE` | The `code =` of `error(...)` is not a string literal of words of capital letters and digits joined by single underscores starting with a letter, names a Core code (a runtime failure code, an error-value code or `ERROR`), or, in a package's code, does not start with the package's prefix (its name in upper case, every other character `_`, then `_`), or a package error omits `code =`. | Write a literal such as `code = "DATASET_LOAD"`; in package `nn`, `code = "NN_SHAPE"`. |
| `EXACT_PROVIDER` | A `real.atom` provider is not a string literal or its opcode is not a `nat32`-range integer literal. | Write the provider name and the opcode as literals. |
| `FFI_CALLBACK_TYPE` | An external C callback uses a function signature outside the explicit callback ABI subset. | Use capture-free `func` values with only `int32`, `nat32`, `int64`, `nat64`, `real32`, or `real64` value parameters and the same scalar set or `void` result. |
| `FFI_DEFAULT` | An external or exported C parameter declares a default argument. | Remove the default; C callers and external C parameters supply every argument. |
| `FFI_EXPORT` | `export "C"` is applied to something other than a top-level non-generic free function, names an ABI other than `"C"`, is used in an interactive session, or is missing on one of a prototype and its definition; or a library build exports no function. | Export a concrete top-level wrapper function with `export "C"`, outside the REPL, and mark both the prototype and the definition. |
| `LITERAL_CONSTANT` | A `const literal` declaration has a type other than `real` (or `com`), or an initializer that is not a constant exact expression: `A literal constant has type real or com and a constant exact initializer.` | Declare `const literal real` with literals, arithmetic, other contextual constants and the exact intrinsics `real.atom`, `real.unary` and `real.op`. |
| `LIBRARY_TOP_LEVEL` | The root of a library build (`quidra build --lib`) contains an executable top-level statement or a `cli` declaration. | Move the statements into a function or an `if main` guard; a library has no entry point and no command line. |
| `FFI_REFERENCE` | An external C parameter uses a reference/const form its type does not admit, or an exported C parameter is a reference. | Pass scalars and callbacks by value; use `const string`, `const bin`, `const tensor<T>`, mutable `bin &` or `tensor<T> &` only where an `extern` admits them; exported C parameters are always by value. |
| `FFI_SYMBOL` | An external C symbol is not an ASCII C identifier, or an exported function's name is a C keyword or a reserved C identifier (`_X...`, `__...`). | Bind an ordinary C identifier symbol; rename the exported function. |
| `FFI_SYMBOL_CONFLICT` | An external or exported C symbol is reserved by the compiler/runtime implementation, is already bound by another `extern` declaration, is exported more than once in a compilation, or is both exported and bound by an `extern`. | Use a distinct C wrapper symbol; bind or export each C symbol once per compilation. |
| `FFI_TYPE` | An external or exported C result or parameter type is not admitted at the C ABI boundary. | For `extern`, use `void` or an explicit numeric/bool scalar result, and scalar values, ABI-safe callbacks, or explicit string/bin borrows; for `export "C"`, use `int8`..`int64`, `nat8`..`nat64`, `real32` or `real64` values and a result of one of them or `void` (never bare `int`). |
| `FUNCTION_NOT_VALUE` | A function or method name is used where a first-class value is required. | Call it directly; declarations are not first-class values. |
| `FUNCTION_REFERENCE_CONTEXT` | A named function is used as a value without an explicit compatible `func<...>(...)` type context. | Provide the complete function-value type context or call the function directly. |
| `FUNCTION_REFERENCE_EXTERN` | An `extern` function is used as a first-class Quidra function value. | Wrap the foreign call in an ordinary Quidra function before using it as a value. |
| `FUNCTION_REFERENCE_SIGNATURE` | A function does not match the required first-class function signature, uses unsupported reference parameters, or is declared with the `.` marker. | Use a matching by-value `func<...>(...)` signature, including shape relationships up to renaming, or adjust the function declaration; call a `.` function directly. |
| `GENERIC_ARGUMENTS_REQUIRED` | A generic class type position lacks explicit type arguments. | Supply `<...>`; call inference requires uniquely determining typed arguments. |
| `GENERIC_ARITY` | The number of generic arguments is wrong. | Supply exactly the declared number. |
| `GENERIC_CONSTRAINT` | A generic constraint name is unknown or a concrete type does not satisfy the declared constraint. | Use a supported built-in constraint and a type that satisfies it. |
| `GENERIC_INFERENCE` | A generic class, function or method call does not determine every type argument. | Supply explicit `<...>` or pass typed arguments that determine them; extra factory parameters belong to an ordinary generic function. |
| `GENERIC_RECEIVER` | A generic method receiver is invalid for specialization. | Use the declared generic class/method receiver. |
| `GENERIC_TARGET` | Type arguments were supplied to a non-generic or invalid target. | Remove them or call the intended generic declaration. |
| `IF_EXPRESSION` | An if-expression lacks `then` or `else`, writes `else if`, stands unparenthesized where an operand, a nested branch or a statement header is expected, would give a reference, or an if statement has `then`. | Write `if C then A elif C2 then B else D`, parenthesize it as an operand, and use an if statement where a reference or a statement body is meant. |
| `IMPORT_CONTEXT` | Imports were compiled through an API without file/module context. | Use file-aware compilation. |
| `IMPORT_CYCLE` | Module imports form a cycle. | Break the cycle. |
| `IMPORT_IO` | An imported source cannot be read. | Correct the path or filesystem access. |
| `IMPORT_PATH` | An import target/path form is invalid. | Use a supported logical, relative, or `@/` path. |
| `IMPORT_TOP_LEVEL` | An imported module contains executable top-level statements. | Keep imported modules declaration-only. |
| `INDENTATION` | Indentation violates Quidra's block rules. | Use four spaces per level and no indentation tabs. |
| `INDEX_ARITY` | An index list has the wrong shape for the indexed value (`Arrays take one index or one slice.`). | Arrays take one integer index or one slice; bin uses one integer index or a bit slice; tensors may use comma-separated indices/slices up to runtime rank. |
| `INDEX_BOUNDS` | A constant array index is provably outside a statically known array length: `Index I out of bounds for length N.`; or a slice of a fixed array or of a string literal with constant operands breaks the slice bounds: `Slice 1:5 out of bounds for length 3.` (the slice as written). | Use an index in the valid half-open range; keep a slice's selected positions in the array, an excluded endpoint at most one position outside, and an inclusive endpoint inside (or at the length for an empty slice). |
| `INDEX_SYNTAX` | A tensor index component is structurally incomplete. | Supply an integer index or a valid slice. |
| `INTEGER_RANGE` | An integer literal is outside the range of its fixed-width context type, or a compound literal expression computed in that type leaves its range (`nat8 x = 3 - 5`). | Use a representable literal or a wider type; `int` and `nat` contexts have no width ceiling. |
| `INVALID_ASSIGNMENT` | The left side is not assignable storage or assignment violates its contract (a string slice is assigned: `A string slice cannot be assigned: strings are immutable.`); a shape variable is read-only (`Shape variable 'N' is read-only.`, for an assignment and a `&N` argument alike). | Assign to valid mutable storage. |
| `INVALID_AUTO` | `auto` appears where inference is not supported, lacks an initializer, or is used in an invalid union form; `auto \| error` additionally requires a fallible initializer. | Use bare `auto` for fail-fast success inference, `auto \| error` to retain failure, or an explicit type. |
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
| `MUTATION_MARKER` | A function definition or call hides changes to existing state, a redundant marker reaches no marked callee, a parameter default changes existing state, or family markers disagree. | Declare ordinary functions `.name` and mark their calls; class generation has an inferred requirement and uses `.Type(...)` when needed. |
| `NESTING_DEPTH` | Expression or statement nesting exceeds the compiler's safety budget. | Reduce nesting: split the expression or extract a function. |
| `NO_CONSTRUCTOR` | No initialization operation is available for the requested class instance. | Use the library factory when generation is not exposed; otherwise supply a complete class header and matching type/call arguments. |
| `NOT_CALLABLE` | A binding is invoked even though its type is not callable. | Call a function/function-value binding instead, or remove the call syntax. |
| `NUMERIC_CAST` | A statically known explicit numeric cast would change the value, or a complex value is converted to a real or integer type (`A complex value has no direct real32 conversion: use z.real or math.abs(z).`). | Choose an exact destination or an operation that states the intended transformation. |
| `NUMERIC_FAMILY` | A literal is materialized outside its category (an integer literal as a real type, a real literal as an integer type, a negative integer literal as `nat*`, an imaginary or complex literal without a complex type), integer and real literals are mixed, or an imaginary literal is written in integer form (`2i`). | Keep the expression within one category, write the literal in the target's category (`10.0`, `2.0i`), or write an explicit conversion. |
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
| `PARSE_ERROR` | Tokens do not form valid Quidra grammar. A slice marker is separated from its colon (`A slice marker is written next to its colon: '<:', '>:', ':<' or ':>'.`), has no value (`An exclusive start needs a start value: write 's<:' or 's>:'.`, `An exclusive end needs an end value: write ':<e' or ':>e'.`), or touches the second colon (`Slice markers belong to the first colon.`). Braces outside string literals appear only in tensor shapes (`Braces appear only in tensor shapes.`). | Correct the syntax near the reported span. |
| `PATCH_HASH_MISMATCH` | A patch node changed since inspection. | Re-inspect and use the new hash. |
| `PATCH_KIND_MISMATCH` | A patch target exists but its structural node kind differs from `expected_kind`. | Re-inspect the source and regenerate the patch against the current node kind. |
| `PATCH_OVERLAP` | Patch operations target overlapping spans. | Split or remove overlapping replacements. |
| `PRIVATE_DECLARATION` | Another module names a `private` top-level declaration, or a public declaration exposes a private class or enum in a signature, a public field, an enum payload or a constant type. | Use the module's public API, make the declaration public, or keep the exposing declaration private too. |
| `PRIVATE_MEMBER` | A private field is read, written, or addressed outside its declaring class, or a private method is called outside its declaring class. | Access the member only from a method or constructor declared by its owning class, or expose an intentional public API. |
| `RANGE_CONTEXT` | A `range` value is used outside its supported iteration context. | Use it as the iterable of `for`. |
| `REAL_RANGE` | A real literal is not a finite representable source literal, or a constant fixed-width real expression (real literals combined with `+`, `-`, `*`, `/`) overflows its type: `Constant real64 expression overflows: 1.0e308 * 10.0.` | Use a finite literal in range; compute the value in a wider type or reorder the operation. |
| `REFERENCE_BINDING` | A reference binding form is invalid, a field is rebound outside its declaring class, or a cli field is a reference. | Bind explicitly with `&name = &value`; initialize/rebind a reference member only in its declaring class. |
| `REFERENCE_TARGET` | A stored reference targets movable or unresolved storage, or a value would hold a reference into its own storage. A replacement could leave a pinned target uninitialized. | Target a whole variable or a path of class fields; keep the target and its holder in separate variables, and preserve target initialization on replacement. |
| `UNBOUND_REFERENCE` | A local or field is accessed before binding; equality or REPL display visits an unbound reference field. Known unbound states fail at compile time, uncertain states at run time. | Bind with `&r = &value`, or `&name = &value` in the declaring class, on every continuing path before access. |
| `RESERVED_MAIN` | Source declares the compiler-reserved native entrypoint name. | Rename it; top-level statements define program entry. |
| `RETURN_OUTSIDE_FUNCTION` | `return` appears at top level. | Return only from a function/method. |
| `SCAN_TARGET` | A `{&name}` input target is written outside a `scan` format. | Use the target only inside a `scan(...)` format string. |
| `SHADOWING` | A declaration reuses a reserved identifier, hides a visible name, or conflicts with a member, instance name or initialization local. Shape binders also cannot shadow visible names. | Choose a distinct non-reserved name; ordinary header member parameters and their generated members are one declaration. |
| `SHAPE_VARIABLE` | An axis of a tensor shape in braces names a shape variable or value wrongly: a bare name that is not declared (`'C' is not declared: write 'nat C' to declare a shape variable, or use an existing nat value.`), a bare name of a binding that is not `nat` (`'n' is int; a bare axis name refers only to a nat value: write 'nat(n)'.`, `'f' is not a nat value.`, a built-in text constant or a type parameter), a shape variable declared twice in one shape, `nat N = EXPR` axes defined by each other, a `nat N` axis in a declaration without an initializer, a shape variable typed other than `nat` (`int N`), a bare `nat N` in a result shape that cannot be inferred, or an unresolved result-shape name. | Declare a new shape variable with `nat N` or `nat N = EXPR`; name an existing `nat` value bare; convert another integer kind with `nat(n)`; give a shape without an initializer values. |
| `SHIFT_COUNT` | A statically known shift count is negative or not smaller than the fixed-width integer operand. | Use a shift count in `[0, width)`. |
| `SLICE_ASSIGNMENT` | A slice assignment's value has a statically known number of elements other than the number the slice selects: `Slice assignment needs 3 values, got 2.` | Assign an array with exactly as many elements as the slice selects. |
| `SLICE_STEP` | A slice's markers or step give no direction: two markers point different ways (`Slice markers point one way: '<:' and ':<' ascend, '>:' and ':>' descend.`), a constant step is 0 (`Slice step 0 is invalid.`) or points against a marker (`Slice step -2 cannot be used with ':<'.`), or an unmarked tensor slice step is statically known to be negative (`Tensor slices currently require a positive step.`), or its marker descends (`Tensor slices ascend: '>:' and ':>' need a negative step, which tensors do not take.`). | Write the markers of one direction and a step of that sign, or leave the step out; tensor slices take a positive step. |
| `STALE_REVISION` | Patch base revision does not match current source. | Re-inspect and regenerate the patch. |
| `STANDARD_KEY_TYPE` | A standard-library keyed container uses an unsupported key/element type. | Use one of the documented deterministic key types. |
| `STANDARD_NAMESPACE_IMPORT` | Source attempts to import or alias a standard namespace that is already always visible. | Remove the import and use the standard namespace directly. |
| `SUMMARY_ANALYSIS` | Interprocedural initialization/effect summaries cannot reach a valid stable contract. | Simplify or correct the recursive effect relationship reported. |
| `TENSOR_SHAPE` | A statically known tensor rank/shape relation violates an operation's shape contract, or a shape variable's known extent contradicts another known extent (`Shape variable 'K' is 2, but axis 1 of the initializer is 3.`, or conflicting extents in arguments of one signature). | Use tensors with the required compatible rank and shape. |
| `TENSOR_TYPE` | Old angle shape syntax is rejected with `Tensor shapes are written in braces: write 'tensor<real32>{2, 3}'.`. A tensor type is written without its element type (`tensor{2, 3}`). | Write the element type, `tensor<real32>{2, 3}`, or use `auto`. |
| `THIS_QUALIFIER` | An instance member is used where the enclosing instance is unavailable, outside its instance body or method signature. | Use a member name directly inside its class, or the explicitly declared instance name where an instance qualifier is needed. |
| `TRY_CONTEXT` | `try` cannot propagate `error` through the enclosing return type. | Use it inside a function whose return accepts `error`. |
| `TYPE_MISMATCH` | An expression's type violates the required type contract. | Use a matching type or an explicitly supported exact conversion. |
| `UNINITIALIZED` | Storage, an array element included, is read before it is initialized on every path, or it may be uninitialized where no run-time check can track it (through a reference whose target is not known, or after a call that may write it without initializing it). A read that is initialized on some paths only is checked at run time instead. | Initialize it before the read on at least one path, or on every path where the compiler cannot track it. |
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
| `UNKNOWN_STANDARD_MODULE` | Standard-namespace lookup requested a name absent from the language registry. | Use one of the standard namespaces defined by the current Quidra release. |
| `UNKNOWN_TYPE` | Type name lookup failed. | Use a built-in, generic parameter, imported type, or declared class. |
| `WRITE_CAPABILITY` | A write is attempted through a const path, reference form mismatches the parameter (a `&` argument for a const parameter, which takes a value), authority is improperly regained, a `.` call may change a const value or state reachable from it, or a writable path is otherwise unsafe. | Match `&` reference contracts; pass a const parameter a value and use `T &` only from a path that already has write authority. |

## Compile-time guarantees

The checker rejects unsafe or ambiguous operations including uninitialized reads, incompatible value/shape conversions, writes or rebinding through const paths, writable paths formed to const values, invalid class initialization, non-exhaustive typed matches, import/generic errors, statically known integer and real division by zero, constant real expressions whose result is not finite, unsupported numeric cast categories, and statically known integer cast range failures. Multiple reference arguments may intentionally alias the same storage; a const parameter keeps the value its argument had at the call.

Parser and checker recovery occurs only at safe boundaries. Expressions depending on an already-invalid binding use an internal invalid type so one root failure does not needlessly cascade.

## Runtime failures

Deterministic runtime safety failures terminate with status `101`. They include:

- integer overflow at every supported signed or unsigned width;
- a fixed-width real operation (`+`, `-`, `*`, `/`, `^`) whose result is not finite: an overflow (`REAL_OVERFLOW`) or a division by zero (`DIVISION_BY_ZERO`);
- dynamically determined integer division or remainder by zero;
- dynamically determined fixed-width integer shift counts outside `[0, width)`;
- index bounds violations of arrays, fixed arrays, strings and bin, which include the offending index and the current length, and of slices of arrays, strings and bin and of tensor indexes and slices, which show the slice as written (and a tensor's axis);
- reads from runtime-tracked array or tensor storage that has not been initialized;
- invalid allocation sizes;
- numeric conversions whose runtime value cannot be represented in the destination type (`NUMERIC_CONVERSION`, naming the destination type and the reason);
- `math.trunc`, `math.round`, `math.floor`, and `math.ceil` results to `int` that are not finite or fall outside `int` range;
- tensor shape, broadcasting, index arity and contiguity violations;
- tensor/autograd shape and dtype mismatches, invalid tracked mutations, unavailable gradients, and unsupported higher-order transforms;
- invalid runtime text operations such as malformed UTF-8;
- a zero `range` step;
- call depth exceeding the native safety limit before host stack exhaustion.

An explicit `error("message")` is instead a typed value (see [Error values](#error-values)). Numeric `Type.parse(text)` returns `error` for invalid or out-of-range text. A range-checked numeric cast likewise exposes `converted-type | error` when a runtime value may not fit; array and tensor casts use the whole converted container as the success alternative and fail atomically if any leaf is out of range. Bare `auto` infers the success type and fails fast, `auto | error` preserves the failure alternative, a success-only destination also fails fast, `try` propagates it, and `match` can recover from an explicitly preserved result locally. `scan(...)` returns `error` for end of input, invalid text, or input that does not match its format, and `print` and `flush` return `error` for an output failure; an expression statement that discards such an error fails fast (a standalone expression in an interactive session displays the error value instead).

No value of a fixed-width real type (`real32`, `real64`) that a program can observe, scalar or tensor element, is NaN or an infinity: an operation that would produce one stops the program instead, so `print` and string interpolation never see such a value. A real tensor operation on the CPU stops at its first failing element; on a GPU its check is deferred to the next synchronization point and reported there (`TENSOR`, or `GPU_ASYNC` at exit) with the operation's message and `(deferred GPU check from FILE:L:C)`.


## Runtime failure format

Coded runtime safety failures use status 101. The report names the file, line and
column in the compile-time `FILE:L:C` form, so editors and terminals can open the
position; when the compiled source file is present and unchanged, the source line and a
caret follow; the message stands inside the same gutter:

```
Quidra runtime error[INDEX_BOUNDS] at main.qui:2:14

2 | print(values[3])
  |              ^
  | index 3 out of bounds for length 3
```

- `FILE` is the file as the program was given to the compiler (`quidra run main.qui`,
  `quidra build examples/main.qui`); a local import is shown relative to it
  (`examples/lib/util.qui`), keeping any `..` steps. `L` is the line in that file and `C`
  the column, counted in bytes: the same numbers `check --json` reports for a compile
  diagnostic at that position. The header has no trailing colon.
- The source line and the caret appear only when the file is present at the absolute path
  it was compiled from and identical to the compiled file: a regular file of the recorded
  size whose SHA-256 equals the recorded one. The executable embeds no source text, only
  each file's paths, digest, line count and size; a deleted, edited, replaced or unreadable
  file shows no snippet, and a built executable run from another directory still shows it
  while the source is unchanged. The snippet is set off by an empty line; the source line
  follows its line number and ` | `, with its indentation kept (a CRLF file's `\r` is not
  shown); the caret line has a blank gutter of the same width, and its padding covers the
  columns before `C` by display width (a tab stays a tab, East Asian Wide and Fullwidth
  characters take two columns, combining marks none).
- The message stands inside the gutter, one body line per line of the message. Without a
  snippet the gutter has zero width and the message follows the header directly:

  ```
  Quidra runtime error[INDEX_BOUNDS] at main.qui:2:14
  | index 3 out of bounds for length 3
  ```

- A failure with no source site and no running user statement (a completion callback, a
  warm-up, a check at program exit, a `qcore_parallel_for` worker) names the program's root
  file without a line: `Quidra runtime error[CODE] at FILE`, then the message.
- The file, line and column belong to the user's statement: a failure inside package code
  (an installed package, or a file it reaches through its own quoted imports) is reported
  at the statement of the user's code that called into it, never at a package-internal
  line; the user files are the root and the files its quoted imports reach.
- A deferred GPU check's message names where its work was queued:
  `(deferred GPU check from FILE:L:C)`; the header names where the failure surfaced.
- Only a failure before the program registered its source table keeps the one-line form
  `Quidra runtime error[CODE]: message`; a compiled program registers it before its first
  statement.

The coded runtime failure codes:

| Code | Failure |
| --- | --- |
| `AUTOGRAD` | A tensor autograd operation failed: shape or dtype mismatch, invalid tracked mutation, unavailable gradient, or an unsupported higher-order transform. Floating tensor casts preserve first-order gradients; a graph containing real16 or real16b rejects `backward(track = true)`. |
| `CALL_DEPTH_LIMIT` | Call depth would exceed the native safety limit before host stack exhaustion: `maximum recursion depth exceeded (limit 4096)`, reported at the call that would exceed it. |
| `CPU_THREADS` | `QUIDRA_CPU_THREADS` is not an integer from 1 to 256. Reported at the running statement, or naming the root file when none runs. |
| `DIVISION_BY_ZERO` | Integer division or remainder, or an exact `real` division, by a divisor that is zero at run time: `division by zero`. A fixed-width real division by zero (`0.0 / 0.0` included): `real64 division by zero` (reason `REAL_ZERO_DIVISOR`; the JSON report carries `type`). |
| `EXACT_NUMERIC` | The exact-number substrate failed: an exact-real provider or atom is unavailable or outside its domain, a symbolic value is not a finite real where one is needed, or an internal invariant broke. The message names the failure. |
| `EXACT_UNPROVEN` | Refinement could not prove an exact `real` decision within its deterministic budget: `exact comparison could not be proven`, `exact digits could not be proven` (an interpolation format of a `real`), `exact rounding could not be proven`. The JSON report names the reason as `reason`: `ZERO_UNDECIDED` (the message adds ` within the maximum precision`; at the maximum precision the enclosure still contains zero, or the rounding or integer boundary in question), `BUDGET_EXHAUSTED` (the message adds `: the refinement budget ran out`; the expression needs more node visits or nesting than the budget allows) or `NO_REFINEMENT` (the message adds `: a value has no certified refinement`, such as an operation of a version-1 provider). |
| `FOR_HEADER` | A comparison loop has mixed directions, a zero or wrongly signed constant step, or a real bound without an explicit step. Collection loops require the element-selection form. Multiple collections support Array value bindings only and reject reference, range, traversal, pattern and index mixtures. |
| `PARALLEL_FOR` | A parallel iteration lacks the required scalar value, disjoint place, control-flow, or worker ownership proof. Runtime reason `UNPROVEN_ITERATION` also rejects an untracked array or a boxed previous integer output element before dispatch. Use a supported integer range/scalar Array value header and direct array writes; use an ordinary `for` explicitly when ordered behavior is required. |
| `FOR_LENGTH` | Arrays in a multiple-collection `for` header have unequal lengths: `all arrays in a for header must have the same length` (catalog reason `LENGTH_MISMATCH`). A fail-fast runtime diagnostic after all sources have been evaluated and snapshotted, before any body iteration, including an empty/nonempty mismatch. It is not a recoverable error-value schema. |
| `FOR_STEP` | A dynamic comparison-loop step is zero or points against its header, checked once at entry even for an empty loop. |
| `LOOP_VARIABLE_WRITE` | The body writes or takes a writable reference to a comparison-loop variable or its iteration index. |
| `PARALLEL_OWNERSHIP` | A parallel consuming argument lacks an owning origin or an admissible capture lifetime. |
| `TRAVERSAL` | A structural traversal is used as a value, visits unsupported storage, violates access authority, or has an inadmissible traversal function body. |
| `TUPLE` | A tuple type, literal, pattern or operation violates the tuple element, shape or operation rules. |
| `FOR_ITERATION` | A writable collection loop (`for auto &value = &values[]`) over a reference binding (a reference local or a `&` parameter) found that the body replaced or resized the iterated array through another binding that aliases it. Reported at that statement: `array iterated by reference was replaced or resized during the loop`. |
| `GPU_ASYNC` | A deferred GPU check that no synchronization point consumed failed when the program exited. Reported naming the root file without a line. |
| `GPU_SCOPE` | Package native code broke the Metal encode protocol of `include/quidra/native_extension.h`: it asked Core for device work inside its encoder scope, for a commit or wait while it held a device stream, or for another device during the hold, or it returned without ending the hold. Reported at the statement that made the extern call or that requested the refused work, at the parallel block that a hold left open earlier in its statement would block, at the `backward()` call whose custom autograd backward callback broke the protocol, and naming the root file without a line for a completion callback or a warm-up, which run outside any statement and end the process at once, and for a hold still open at program exit. Any other coded failure of a thread that still holds a stream ends the hold first and reports a violation the hold recorded instead, as `GPU_SCOPE`. |
| `GPU_SYNC` | Synchronizing a GPU failed: `gpu.sync(index)` with an unavailable or negative index or a failed device, or the wait of `time.now(sync = true)` and `time.since(start, sync = true)`, which is reported at the running statement. |
| `INDEX_BOUNDS` | An element index of an array, fixed array, string or bin is outside the current length: `index I out of bounds for length N` (a negative index included), reported at the index operand. A slice of an array, a string or a bin is outside its value: `slice S:<E out of bounds for length N`, the slice in its written form with its operands' values (`2:<7`, `10>:0:-2`, `:7`). A tensor element index or slice is outside its axis (counted from 0): `index I out of bounds for axis A with length N`, `slice 0:<3 out of bounds for axis A with length N`. Slices and tensor accesses are reported at the indexing expression. |
| `INTEGER_OVERFLOW` | A fixed-width integer operation's result does not fit its type, at every supported signed or unsigned width, or a `nat` subtraction's result is negative (`nat subtraction result is negative`). |
| `INVALID_RANDOM_RANGE` | `Generator.int(start, end)` received an empty range (`start >= end`). |
| `INVALID_SLEEP_DURATION` | `time.sleep` received a negative or non-finite duration. |
| `NONFINITE_RESULT` | A real value that code outside Quidra made is NaN or an infinity: the result of an `extern` function, reported at the call (`extern 'f' produced a non-finite real64 value`; reason `EXTERN_RESULT`), or a real tensor a package function wrote through a `&` argument (`package operation f produced a non-finite real32 value`; reason `PACKAGE_OPERATION`), reported at the call, or at the user's statement when the package's own kernel reports it. The JSON report carries `type` and `operation` (the function's name). |
| `NUMERIC_CONVERSION` | A numeric conversion failed; the message names what was converted (a value, an array element or a tensor element), the destination type and the reason: `numeric conversion out of range: value cannot be represented as int8` (`OUT_OF_RANGE`), `numeric conversion failed: non-finite value cannot be represented as real32` (`NON_FINITE`: a finite value that rounds to infinity when narrowed to `real32`, or a NaN or an infinity that C passes to an `export "C"` function, reported at its declaration), `numeric conversion failed: value is not an integer and cannot be represented as int` (`NOT_INTEGRAL`: an exact `real` that is a non-integral rational), `numeric conversion failed: the exact value could not be decided for int64` (`UNDECIDED`: a symbolic `real` the proof budget cannot decide). The JSON report carries `type`, `subject` (`VALUE`, `ARRAY_ELEMENT`, `TENSOR_ELEMENT`) and `reason`. A conversion that may fail has an `error` alternative with the same message; a fail-fast use of it stops with that error. `NUMERIC_CAST_RANGE` and the merged tensor cast text are no longer printed. |
| `PACKAGE_EXECUTION_POLICY` | Package native code requested an execution policy other than fast or deterministic. Reported at the user's statement that called the package. |
| `PARALLEL_CONFLICT` | Unordered nodes have overlapping storage writes, shared-state aliases or incompatible resource uses. |
| `PARALLEL_CONTROL` | A return, break or continue would leave its parallel node. |
| `PARALLEL_START` | The shared worker pool or a parallel node table could not be started. |
| `PARALLEL_BODY` | Package native code called a Core function other than the pure data-access functions inside a `qcore_parallel_for` body (`docs/spec/architecture.md`). Reported naming the root file without a line (a body runs no statement); the process ends at once. |
| `POWER_DOMAIN` | A power has no value: an integer power has a negative exponent (`integer exponent must be non-negative`), the base is 0 and the exponent 0 or negative (`0 ^ 0 is undefined`, `0 ^ a negative exponent is undefined`), or a fixed-width real base is negative and the exponent is not an integer (`a negative base requires an integer exponent`); for tensor powers, element by element. |
| `RANGE_STEP_ZERO` | A `range` step is zero. |
| `REAL_OVERFLOW` | A fixed-width real operation on finite operands has a result beyond the type's finite range: `real64 multiplication overflow` (the type, then `addition`, `subtraction`, `multiplication`, `division` or `power`), `tensor<real32> addition overflow` for a tensor operation on the CPU, `tensor<real32> gradient overflow` for a gradient a backward pass delivers. Reported at the operation; of the operations of one expression, the first one in evaluation order that overflows. The JSON report carries `type` and `operation`. |
| `SHAPE_MISMATCH` | An array's length differs from the extent its declared shape (`T[n][m]`) names, or a tensor's extent differs from the shape variable that fixes its axis at a declaration or call entry: `shape variable 'C' is 3, but axis 0 of 'y' is 4` (a shaped mutable external borrow is checked again after the call; the JSON report carries `name`, `expected`, `axis`, `path` and `actual`). |
| `SHIFT_COUNT` | A fixed-width integer shift count is outside `[0, width)` at run time. |
| `SLICE_ASSIGNMENT` | A slice assignment's value has another number of elements than the slice selects: `slice assignment needs 3 values, got 2`, at the target. |
| `SLICE_STEP` | A slice's run-time step is 0 (`slice step 0 is invalid`), points against its first marker (`slice step -2 cannot be used with ':<'`), or is an unmarked negative tensor step (`tensor slices currently require a positive step`), at the slice. |
| `TENSOR` | A tensor operation failed: shape, broadcasting, index arity, contiguity, an unsupported cast dtype pair or a tracked cast to an integer or complex type (a bool tensor cast is rejected by the checker), or a device failure (an index or slice outside its axis is `INDEX_BOUNDS`; a `tensor.gather` or `tensor.scatter` index outside its tensor keeps `TENSOR` or `AUTOGRAD` and says it is out of bounds; an element outside a cast's destination range is `NUMERIC_CONVERSION`). |
| `UNINITIALIZED` | Runtime-tracked array or tensor storage is read before it is initialized ("value is uninitialized"), or a binding whose initialization depends on the path taken is read on a path where it is not initialized ("'x' is uninitialized", naming the storage as written). |

A coded failure inside a parallel node is captured with its worker path. After all started nodes join, the first failing node in source order is reported once.

Runtime-library failures that carry no code, such as host allocation failure or low-level
text/runtime invariant checks (an empty `split` separator, a `string.repeat` fill that is
not one code point, a `bin.fill` value other than 0 or 1), print the same report without
`[CODE]`: `Quidra runtime error at FILE:L:C`, naming the running user statement (or the
root file without a line when none runs), which is where the program was rather than an
invented site; their messages are unchanged, and they terminate with status 101.

### Error values

An `error` value is structured: it has a code, the details its code defines, and an
optional cause; apart from these it keeps its origin, the place it was made. Its message
is a rendering of the code and the details, made once when the error is made:
`cannot read "data.csv": permission denied` is the message of a `FILE_READ` error whose
details are `path` = `data.csv` and `reason` = `PERMISSION_DENIED`. Printing an error,
interpolating it (`"{e}"`) and `e.string()` give its message.

When an error reaches a fail-fast site (bare `auto`, a success-only destination, or a
statement that discards a `void | error` result), the program stops with the error's own
code, `ERROR` for a user `error("...")` without a code, and its message, at its origin:
where it was made, or, for an error made inside package code, the statement of the user's
code that called into the package.

```
Quidra runtime error[NUMERIC_CONVERSION] at main.qui:2:7

2 | print(int8(value))
  |       ^
  | numeric conversion out of range: value cannot be represented as int8
```

The report of an error that propagated, or that wraps others, continues inside the gutter
after an empty body line, with its trace:

```
Quidra runtime error[DATASET_LOAD] at main.qui:13:20

13 |             return error("dataset loading failed", cause = problem, code = "DATASET_LOAD")
   |                    ^
   | dataset loading failed
   |
   | propagated through load_dataset() at main.qui:16:23
   | propagated through train() at main.qui:19:13
   | caused by: [FILE_READ] cannot read "data.csv": permission denied (at main.qui:2:16)
   |     propagated through read_file() at main.qui:5:23
   |     propagated through read_dataset() at main.qui:9:11
```

- `propagated through F() at FILE:L:C`, one line per hop, inner to outer: the function the
  error left and the position of its call (a method call's position is the call
  expression's start, the receiver). A hop is recorded where an error leaves a call of a
  user function, method or constructor, or a call through an `func` value, on the error path:
  at `try CALL`, at a fail-fast stop of `CALL`, and in the error case of `match CALL` that
  binds the error. Calls of package, Core and `extern` functions record none (their errors
  name the user statement already), and storing an error records nothing. An error that
  left one call N times in a row (a recursion) prints one line ending ` (N times)`.
- `unhandled at FILE:L:C` follows when an error stops away from where it was made and no
  hop names the stop: an error stored and consumed later.
- `caused by: [CODE] message (at FILE:L:C)` follows for each cause, outermost first: its
  code, its message on one line (a line feed written `\n`) and its origin; its own hops
  follow it, indented four spaces. Only a wrapped error has `caused by:` lines.
- A list longer than 20 lines (the hops, each cause's hops, the causes) prints the first 10,
  `… N more …` and the last 10. The JSON report carries every hop.

Runtime traps and fail-fast stops also show the path of user calls, recovered from the
failing thread's stack at the time of reporting. These lines use the same
`propagated through F() at FILE:L:C` form, inner to outer, after the error's own hops
and any `unhandled at` line and before its causes. Inlined user calls appear too. Package
and compiler-generated frames are transparent, and a package failure names the user
statement that entered the package. A deferred GPU check shows the host path where the
check surfaces; its recorded origin stays in the message.

The path ends at the program's top level, at the Quidra entry of a C export, or at the
body of a parallel node. Its report adds the node boundary and the joining caller's
stack is not shown. C frames are transparent when their unwind information permits the
walk to continue. A missing or ambiguous part is shown as `… call path incomplete …`.
A call LLVM merged from several calls in one scope names its possible sites, in source
order, joined by ` or `; sites from incompatible inline scopes produce the marker.
Optimized tail calls retain their logical path too. A repeated self call is one line
with its count; a tail call selected from several branches names the branch taken. If
insufficient history remains to prove a tail path, the missing part uses the same marker.
The REPL shows no walked path.

Release executables carry the table as data, so stripping symbols does not remove the
path. A `--lib` archive also carries its object line tables. A host link may map these
like other debug objects; the host chooses its own debug stripping flags if it needs
reproducible executables.

`UNHANDLED_ERROR`, the code every such stop printed before, is removed.

The codes of the errors Core operations make, with their details. A text detail is a
string as the program passed it (a path is written between double quotes in the message,
unchanged); a name detail is a spelling from the list given with its code (a type is
spelled as the language spells it, such as `int8` or `real64`); an integer detail is a
number. An absent detail is not part of the error (a `file.Handle` operation has no
`path`; its message says `the file handle`).

| Code | Made by | Details | Message |
| --- | --- | --- | --- |
| `FILE_READ` | `file.read`, `file.read_bin`, `file.Handle.read`, `.read_bin`, `.read_line` | `path` (text, absent for a handle), `reason` | `cannot read "PATH": REASON` |
| `FILE_WRITE` | `file.write`, `file.write_bin`, `file.Handle.write`, `.write_line`, `.flush` | `path`, `reason` | `cannot write "PATH": REASON` |
| `FILE_OPEN` | `file.open`, `file.create`, `file.append` | `path`, `mode` (`READ`, `CREATE`, `APPEND`), `reason` | `cannot open "PATH": REASON`; `create`, `open for appending` by the mode |
| `FILE_SEEK` | `file.Handle.seek` | `path` (absent), `reason` | `cannot seek in the file handle: REASON` |
| `FILE_STATUS` | `file.exists`, `file.is_directory` | `path`, `reason` | `cannot inspect "PATH": REASON` |
| `FILE_LIST` | `file.list` | `path`, `reason` | `cannot list "PATH": REASON` |
| `FILE_REMOVE` | `file.remove` | `path`, `reason` | `cannot remove "PATH": REASON` |
| `FILE_COPY` | `file.copy` | `path` (the source), `destination` (text), `reason` | `cannot copy "PATH" to "DESTINATION": REASON` |
| `FILE_MOVE` | `file.move` | `path`, `destination`, `reason` | `cannot move "PATH" to "DESTINATION": REASON` |
| `FILE_MKDIR` | `file.mkdir` | `path`, `reason` | `cannot create directory "PATH": REASON` |
| `NUMERIC_PARSE` | `T.parse` for every numeric `T` | `type` (the parsed type), `reason` (`INVALID_TEXT`, `OUT_OF_RANGE`) | `cannot parse numeric input as TYPE`, `... as TYPE: value out of range` |
| `BIN_PARSE` | `bin.parse` of a runtime text | none | `cannot parse bin input` |
| `UTF8_DECODE` | `string.from_utf8` | none | `invalid UTF-8 text` |
| `INPUT_END` | `scan` at the end of input | none | `scan reached the end of input` |
| `INPUT_READ` | `scan`'s line read | `reason` (`INVALID_UTF8`, `EMBEDDED_NUL`, `IO_FAILURE`) | `input failed: REASON` |
| `INPUT_FORMAT` | `scan`: input that does not match its format | `reason` (`MISSING_TEXT`, `UNREADABLE_FIELD`, `TRAILING_INPUT`), `literal` (text; `MISSING_TEXT`), `type` (`UNREADABLE_FIELD`), `input` (text; `UNREADABLE_FIELD`, `TRAILING_INPUT`) | `scan expected "LITERAL" in the input`, `scan could not read TYPE from "INPUT"`, `scan found unexpected input after the format: "INPUT"` |
| `OUTPUT_WRITE` | `print`, `flush()` | none | `output failed` |
| `NUMERIC_CONVERSION` | fallible numeric conversions: scalar, array, tensor and exact | `type` (the destination), `subject` (`VALUE`, `ARRAY_ELEMENT`, `TENSOR_ELEMENT`), `reason` (`OUT_OF_RANGE`, `NON_FINITE`, `NOT_INTEGRAL`, `UNDECIDED`) | the texts of the `NUMERIC_CONVERSION` failure above |
| `JSON_PARSE` | `json.parse` | `problem` (text: what the parser expected), `offset` (integer: the byte offset) | `PROBLEM at byte OFFSET` |
| `JSON_VALUE` | the `json.Value` accessors `size`, `get`, `at`, `text`, `integer`, `number`, `real`, `boolean` | none | `JSON value has incompatible kind` |
| `HTTP_REQUEST` | `http.get` | `problem` (text: why the request failed) | `PROBLEM` |
| `ERROR` | a user `error("text")` without a code | `text` (text) | the text |

The reasons of the file codes and their texts: `NOT_FOUND` (file not found),
`PERMISSION_DENIED` (permission denied), `IS_A_DIRECTORY` (path is a directory),
`NOT_A_DIRECTORY` (path is not a directory), `ALREADY_EXISTS` (path already exists),
`INVALID_UTF8` (invalid UTF-8 text), `EMBEDDED_NUL` (text contains a NUL character),
`UNALIGNED_BIN` (bin length is not a multiple of 8), `PATH_NOT_TEXT` (path is not valid
text), `HANDLE_CLOSED` (handle is closed), `NOT_WRITABLE` (handle is not writable),
`IO_FAILURE` (I/O failure). `INPUT_READ` uses the texts of its three reasons.

A user error is `error("text")`, `error("text", code = "CODE")` or, wrapping another
error, `error("text", cause = problem, code = "CODE")`: its text is its one detail
(`text`), its code a literal of words of capital letters and digits joined by single
underscores, starting with a letter, that is not a Core code (`ERROR_CODE`); package code
starts its codes with the package's prefix (`NN_SHAPE` in `nn`). A program reads an error's
parts as members: `e.code` (`string`, `"ERROR"` for a user error without a code),
`e.message` (`string`, the rendering), `e.cause` (`error | none`) and one member per
detail name, `string | none` (`e.offset` is `nat | none`), `none` when the error's code has
no such detail or the error lacks it. The detail names are `path`, `destination`, `mode`,
`reason`, `type`, `subject`, `literal`, `input`, `problem`, `text` and `offset`; a name
detail reads as its spelling (`problem.reason` is `"NOT_FOUND"`), and any other member is
`UNKNOWN_MEMBER`.

`==` compares two errors by their identity: the codes, the details (every detail takes
part) and the causes, by the same rule, along the whole chain. Where and how often an
error was made or propagated never takes part: `file.read("data.csv")` failing at two
places gives equal errors, two reads of different paths unequal ones, and a Core error and
a user error with the same message unequal ones. An interactive session displays an error
in the form of its constructor, the code left out for `ERROR`, the details other than a
user error's text in their order, and the cause nested as `cause = error(...)`:

```text
error("cannot parse numeric input as int", code = "NUMERIC_PARSE", type = "int", reason = "INVALID_TEXT")
```

The JSON report below carries an error's details by their names.

### Machine-readable runtime reports

With `QUIDRA_ERROR_FORMAT=json` in a program's environment (a built executable,
`quidra run`, `quidra FILE.qui` and test programs alike), each runtime failure and test
assertion report is written as one line of JSON on stderr in place of the text (schema
version 1, announced by `quidra describe llm` as `runtime_errors`). A later report of the
same thread, such as a deferred GPU check failing at exit after a first failure, is a
second line. For example:

```json
{"schema_version": 1, "kind": "runtime_error", "status": 101, "code": "INDEX_BOUNDS",
 "message": "index 3 out of bounds for length 3", "details": {"index": 3, "length": 3},
 "location": {"file": "main.qui", "line": 2, "column": 14,
              "source_file": "/work/main.qui", "source_revision": "<sha256 of main.qui>"},
 "provenance": {"source_revision": "<sha256 of main.qui>", "node_id": "<id>",
                "node_kind": "expression_statement", "source_file": "/work/main.qui"},
 "deferred_origin": null, "hops": [], "unhandled_at": null, "path": [],
 "path_complete": true, "causes": []}
```

- `kind` is `runtime_error` or `test_assertion`; `status` is the exit status; `code` is
  `null` for the uncoded runtime-library failures; `message` is the text the report shows.
- `details` holds a failure's arguments by name (`index` and `length` for an index failure,
  `slice` (the written form, a string) and `length` for a slice, and `axis` beside them for
  a tensor, `limit` for the recursion limit, `type`, `subject` and `reason` for a numeric
  conversion), and an error's details by their names (text and name details as strings,
  integer details as numbers, absent details left out; a user error's `text`); a failure
  without details has `{}`.
- `location` is the header's position: `file` (the display path), `line` and `column`, and
  `source_file` and `source_revision`, which let a tool check the file it opens; only `file`
  for a failure that names the root file without a line; `null` before the program
  registered its source table.
- `provenance` is the innermost statement's `source_revision`, `node_id`, `node_kind` and
  `source_file` when the compiler associated the statement with the public structural
  schema; the revision and node id are exactly those `quidra inspect` returns for that
  source snapshot. `null` otherwise.
- `hops` holds every hop of the error, unelided: `function` (its name), `file`, `line`,
  `column`, `count` (how many times in a row the error left that call) and `via` (`try`,
  `fail_fast` or `match`); `unhandled_at` the stop's `file`, `line` and `column` when the text
  prints `unhandled at`, else `null`; `causes` each cause, outermost first, with its `code`,
  `message`, `details`, `location` (its origin) and `hops`.
- `deferred_origin` describes where a deferred check was queued, or is `null`.
- `path` lists the walked user calls, each with `function`, `file`, `line`, `column`,
  `count` and `via` (`call` or `inline`). A merged call has a `sites` array of possible
  positions instead of one position. Markers are omitted from `path`, and
  `path_complete` is false when the text shows one. Without a path, `path` is empty.
- Strings are escaped as JSON; writing a report allocates nothing.

## REPL session safety errors

`REPL_REPLAY_UNSAFE` is a REPL session-safety error rather than a source diagnostic or runtime failure. It means the accumulated session may already have executed an external or nondeterministic effect that cannot be reconstructed safely by rerunning accepted source. Use `:reset` before executing another native submission. The barrier is deliberately conservative and may remain armed after a failed runtime submission if an effect could have occurred before the failure.

## Loop headers

`FOR_HEADER` rejects mixed directions, constant zero or wrongly signed
steps, a missing real-family step, an invalid index-binding type, and
reference headers that do not name writable storage. A runtime zero or
wrongly signed step fails once at entry with `FOR_STEP`, even for an empty
range. Its `STEP_ZERO`, `STEP_ASCENDING` or `STEP_DESCENDING` reason and
`step` detail retain the actual arbitrary-precision integer value.

`LOOP_VARIABLE_WRITE` rejects writes and writable references to a range
variable or collection index; both are fixed by the header. Value collection
elements may be reassigned. `ELEMENT_ITERATION` reports `[]` or `[nat name]`
outside a collection header. Direct replacement of a live iteration's source
or prefix is `WRITE_CAPABILITY`; possible replacement or resizing through
an alias is `FOR_ITERATION` at the statement that changed it. A bit loop
place has no address and rejects address use with `WRITE_CAPABILITY`.

Inspect major schema 1, minor 1 adds `ForRange` and `ForEach` kinds.
`ForRange` exposes `start_inclusive` and `end_inclusive`. Omitted bounds
are reflected in those flags and have no synthetic replaceable source node.

`TUPLE` reports an empty or trailing-comma tuple, an invalid element type, positional access, and ordering. Tuple literals use element-wise numeric context and retain the ordinary `AMBIGUOUS_NUMERIC_LITERAL` and range diagnostics. A tuple at a C boundary reports `FFI_TYPE`.

Inspect schema version 1 minor 2 adds `TupleExpr` without changing existing node kinds.

`TRAVERSAL` rejects a traversal used as a value, an unsupported structure that may contain its target, a writable visit below const storage, a private visit from another user module, or a traversal function with more than one return or recursive calls. Name the target type, use a package traversal for private representation, or place the target in supported value fields, arrays or tuples.
`UNBOUND_REFERENCE`: a local or reference field is used before it is bound. Bind it
with `&r = &value`. Definitely unbound paths are diagnosed at compile time;
unknown binding paths test for null at the access and report the source path.

Stored-reference runtime failures use `UNBOUND_REFERENCE` for a null target and `REFERENCE_TARGET` for interior self-reference or exhausted root serials. They report the access, binding or destination store rather than a fabricated target identity.
