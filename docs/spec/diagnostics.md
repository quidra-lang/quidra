# Diagnostics and Runtime Failures

Compiler diagnostics have a stable code, source span, and explanatory message. `quidra check program.qui --json` is the machine-readable interface. Diagnostic JSON schema version 2 also reports the SHA-256 `source_revision` and, when the source can still be parsed and one containing source node is unambiguous, the smallest matching `node_id` and `node_kind`. Lexer/parser failures and other cases without a stable node retain `source_revision` plus the exact span and return null node fields rather than inventing an association. Span start/end include byte offsets as well as line/column.

The default diagnostic limit is 20; `--max-errors N` selects a positive limit, and JSON output reports whether additional diagnostics were suppressed.

Codes are contracts for the category of failure. Message wording may become more specific without changing the code. Node IDs are revision-local and must be interpreted only with the reported `source_revision`.

## Diagnostic codes

| Code | Meaning | Typical correction |
| --- | --- | --- |
| `AMBIGUOUS_NUMERIC_LITERAL` | A numeric-family literal or expression does not have one unique concrete numeric type. | Add an explicit concrete numeric type or cast so the context determines exactly one representation. |
| `AMBIGUOUS_TYPE` | Context does not determine one valid type. | Add an explicit type or remove the ambiguous alternatives. |
| `ARGUMENT_MISMATCH` | Call arguments violate arity, ordering, names, defaults, or constructor shape. | Match the declared parameters and named-argument rules. |
| `ARRAY_SHAPE` | Fixed or inferred array shapes are incompatible. | Use the required dimensions or a compatible dynamic array. |
| `BIN_PARSE` | A statically known `bin.parse` input contains characters other than `0` and `1`. | Use only `0` and `1` in the parsed bit text. |
| `CONST_INITIALIZATION` | A const value binding or const field is not initialized at the point where its immutable contract begins. | Provide an initializer; const fields must be supplied or defaulted during construction. |
| `DISCARD` | The discard name `_` is declared or read. | Choose a real name for a value that is used; drop an unused value with an expression statement. |
| `DIVIDE_BY_ZERO` | Integer division or remainder has a divisor proven to be zero. | Make the divisor nonzero; dynamic zero remains a checked runtime failure. |
| `DUPLICATE_ARGUMENT` | A call supplies the same logical argument more than once. | Supply each positional/named option exactly once. |
| `DUPLICATE_IMPORT_ALIAS` | Two imports expose the same alias. | Give imports distinct aliases. |
| `DUPLICATE_NAME` | A declaration duplicates an existing name or uses a name reserved for that declaration kind. | Rename or remove the declaration; reserved names cannot be reused. |
| `ENUM_CYCLE` | Enum payload types form a recursive value cycle. | Break the recursive enum payload cycle. |
| `FFI_CALLBACK_TYPE` | An external C callback uses a function signature outside the explicit callback ABI subset. | Use capture-free `fn` values with only `int32`, `nat32`, `int64`, `nat64`, `real32`, or `real64` value parameters and the same scalar set or `void` result. |
| `FFI_DEFAULT` | An external or exported C parameter declares a default argument. | Remove the default; C callers and external C parameters supply every argument. |
| `FFI_EXPORT` | `export "C"` is applied to something other than a top-level non-generic free function, names an ABI other than `"C"`, is used in an interactive session, or is missing on one of a prototype and its definition; or a library build exports no function. | Export a concrete top-level wrapper function with `export "C"`, outside the REPL, and mark both the prototype and the definition. |
| `LIBRARY_TOP_LEVEL` | The root of a library build (`quidra build --lib`) contains an executable top-level statement or a `cli` declaration. | Move the statements into a function or an `if main` guard; a library has no entry point and no command line. |
| `FFI_REFERENCE` | An external C parameter uses a reference/const form its type does not admit, or an exported C parameter is a reference. | Pass scalars and callbacks by value; use `const string &`, `const bin &`, or mutable `bin &` only where an `extern` admits them; exported C parameters are always by value. |
| `FFI_SYMBOL` | An external C symbol is not an ASCII C identifier, or an exported function's name is a C keyword or a reserved C identifier (`_X...`, `__...`). | Bind an ordinary C identifier symbol; rename the exported function. |
| `FFI_SYMBOL_CONFLICT` | An external or exported C symbol is reserved by the compiler/runtime implementation, is already bound by another `extern` declaration, is exported more than once in a compilation, or is both exported and bound by an `extern`. | Use a distinct C wrapper symbol; bind or export each C symbol once per compilation. |
| `FFI_TYPE` | An external or exported C result or parameter type is not admitted at the C ABI boundary. | For `extern`, use `void` or an explicit numeric/bool scalar result, and scalar values, ABI-safe callbacks, or explicit string/bin borrows; for `export "C"`, use `int8`..`int64`, `nat8`..`nat64`, `real32` or `real64` values and a result of one of them or `void` (never bare `int`). |
| `FUNCTION_NOT_VALUE` | A function or method name is used where a first-class value is required. | Call it directly; declarations are not first-class values. |
| `FUNCTION_REFERENCE_CONTEXT` | A named function is used as a value without an explicit compatible `fn<...>(...)` type context. | Provide the complete function-value type context or call the function directly. |
| `FUNCTION_REFERENCE_EXTERN` | An `extern` function is used as a first-class Quidra function value. | Wrap the foreign call in an ordinary Quidra function before using it as a value. |
| `FUNCTION_REFERENCE_SIGNATURE` | A function does not match the required first-class function signature, or uses unsupported reference parameters. | Use an exact by-value `fn<...>(...)` signature or adjust the function declaration. |
| `GENERIC_ARGUMENTS_REQUIRED` | A generic class was used where explicit type arguments are required: a type position, or a construction call of a class without a constructor. | Supply the class type arguments with `<...>`; construction calls, generic functions and methods may omit them when inference is unambiguous. |
| `GENERIC_ARITY` | The number of generic arguments is wrong. | Supply exactly the declared number. |
| `GENERIC_CONSTRAINT` | A generic constraint name is unknown or a concrete type does not satisfy the declared constraint. | Use a supported built-in constraint and a type that satisfies it. |
| `GENERIC_INFERENCE` | A generic function or method call, or a construction call of a generic class or of a constructor with type parameters of its own, does not determine every type argument. | Supply explicit type arguments with `<...>`; a constructor's own type parameters are always inferred, so pass arguments that determine them. |
| `GENERIC_RECEIVER` | A generic method receiver is invalid for specialization. | Use the declared generic class/method receiver. |
| `GENERIC_TARGET` | Type arguments were supplied to a non-generic or invalid target. | Remove them or call the intended generic declaration. |
| `IF_EXPRESSION` | An if-expression lacks `then` or `else`, writes `else if`, stands unparenthesized where an operand, a nested branch or a statement header is expected, would give a reference, or an if statement has `then`. | Write `if C then A elif C2 then B else D`, parenthesize it as an operand, and use an if statement where a reference or a statement body is meant. |
| `IMPORT_CONTEXT` | Imports were compiled through an API without file/module context. | Use file-aware compilation. |
| `IMPORT_CYCLE` | Module imports form a cycle. | Break the cycle. |
| `IMPORT_IO` | An imported source cannot be read. | Correct the path or filesystem access. |
| `IMPORT_PATH` | An import target/path form is invalid. | Use a supported logical, relative, or `@/` path. |
| `IMPORT_TOP_LEVEL` | An imported module contains executable top-level statements. | Keep imported modules declaration-only. |
| `INDENTATION` | Indentation violates Quidra's block rules. | Use four spaces per level and no indentation tabs. |
| `INDEX_ARITY` | An index list has the wrong shape for the indexed value. | Arrays use one integer index; bin uses one integer index or a bit slice; tensors may use comma-separated indices/slices up to runtime rank. |
| `INDEX_BOUNDS` | A constant array index is provably outside a statically known array length: `Index I out of bounds for length N.` | Use an index in the valid half-open range. |
| `INDEX_SYNTAX` | A tensor index component is structurally incomplete. | Supply an integer index or a valid slice. |
| `INTEGER_RANGE` | An integer literal is outside the range of its fixed-width context type, or a compound literal expression computed in that type leaves its range (`nat8 x = 3 - 5`). | Use a representable literal or a wider type; `int` and `nat` contexts have no width ceiling. |
| `INVALID_ASSIGNMENT` | The left side is not assignable storage or assignment violates its contract. | Assign to valid mutable storage. |
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
| `NESTING_DEPTH` | Expression or statement nesting exceeds the compiler's safety budget. | Reduce nesting: split the expression or extract a function. |
| `NOT_CALLABLE` | A binding is invoked even though its type is not callable. | Call a function/function-value binding instead, or remove the call syntax. |
| `NUMERIC_CAST` | A statically known explicit numeric cast would change the value. | Choose an exact destination or an operation that states the intended transformation. |
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
| `PARSE_ERROR` | Tokens do not form valid Quidra grammar. | Correct the syntax near the reported span. |
| `PATCH_HASH_MISMATCH` | A patch node changed since inspection. | Re-inspect and use the new hash. |
| `PATCH_KIND_MISMATCH` | A patch target exists but its structural node kind differs from `expected_kind`. | Re-inspect the source and regenerate the patch against the current node kind. |
| `PATCH_OVERLAP` | Patch operations target overlapping spans. | Split or remove overlapping replacements. |
| `PRIVATE_MEMBER` | A private field is read, written, or addressed outside its declaring class, or a private method is called outside its declaring class. | Access the member only from a method or constructor declared by its owning class, or expose an intentional public API. |
| `RANGE_CONTEXT` | A `range` value is used outside its supported iteration context. | Use it as the iterable of `for`. |
| `REAL_RANGE` | A real literal is not a finite representable source literal. | Use a finite literal in range. |
| `REFERENCE_BINDING` | An address/reference target or binding form is invalid. | Use an addressable binding, field, or element with the required `&`. |
| `RESERVED_MAIN` | Source declares the compiler-reserved native entrypoint name. | Rename it; top-level statements define program entry. |
| `RETURN_OUTSIDE_FUNCTION` | `return` appears at top level. | Return only from a function/method. |
| `SHADOWING` | A user declaration reuses a reserved identifier (names beginning with `__quidra_` included), hides a visible name, or conflicts with an occupied class member name (a binding may share a field's name, never a method's). | Choose a distinct non-reserved name; qualification does not make a reserved identifier reusable. |
| `SHIFT_COUNT` | A statically known shift count is negative or not smaller than the fixed-width integer operand. | Use a shift count in `[0, width)`. |
| `SLICE_STEP` | A tensor slice step is statically known to be non-positive. | Use a positive step. |
| `STALE_REVISION` | Patch base revision does not match current source. | Re-inspect and regenerate the patch. |
| `STANDARD_KEY_TYPE` | A standard-library keyed container uses an unsupported key/element type. | Use one of the documented deterministic key types. |
| `STANDARD_NAMESPACE_IMPORT` | Source attempts to import or alias a standard namespace that is already always visible. | Remove the import and use the standard namespace directly. |
| `SUMMARY_ANALYSIS` | Interprocedural initialization/effect summaries cannot reach a valid stable contract. | Simplify or correct the recursive effect relationship reported. |
| `TENSOR_SHAPE` | A statically known tensor rank/shape relation violates an operation's shape contract. | Use tensors with the required compatible rank and shape. |
| `THIS_QUALIFIER` | A bare name inside a method or constructor body names a field of the enclosing class (fields are written `this.NAME`), or `this` is used other than as a receiver-field qualifier: alone (`foo(this)`, `&this`, `auto x = this`, `return this`, `this == other`, `this[0]`), to call a method (`this.reset()`), or outside a method or constructor body (top-level code, functions, field and parameter defaults, a constructor's parameter shapes). | Write a receiver field as `this.<field>` inside a method or constructor body; call methods bare (`reset()`) or through an object (`other.reset()`); copy a function value held in a field into a local before calling it. |
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
| `WRITE_CAPABILITY` | A write is attempted through a const path, reference form mismatches the parameter, authority is improperly regained, or a writable path is otherwise unsafe. | Match `&` reference contracts; use `const T &` for read-only access and `T &` only from a path that already has write authority. |

## Compile-time guarantees

The checker rejects unsafe or ambiguous operations including uninitialized reads, incompatible value/shape conversions, writes or rebinding through const paths, authority escalation from `const T &` to `T &`, invalid class initialization, non-exhaustive typed matches, import/generic errors, statically known integer division by zero, unsupported numeric cast categories, and statically known integer cast range failures. Multiple reference arguments may intentionally alias the same storage; const is path-local rather than a global freeze.

Parser and checker recovery occurs only at safe boundaries. Expressions depending on an already-invalid binding use an internal invalid type so one root failure does not needlessly cascade.

## Runtime failures

Deterministic runtime safety failures terminate with status `101`. They include:

- integer overflow at every supported signed or unsigned width;
- dynamically determined integer division or remainder by zero;
- dynamically determined fixed-width integer shift counts outside `[0, width)`;
- index bounds violations of arrays, fixed arrays, strings and bin, which include the offending index and the current length, and of `text.slice`, bin slices and tensor indexes and slices, which name their bounds (and a tensor's axis);
- reads from runtime-tracked array or tensor storage that has not been initialized;
- invalid allocation sizes;
- numeric conversions whose runtime value cannot be represented in the destination type (`NUMERIC_CONVERSION`, naming the destination type and the reason);
- `math.trunc`, `math.round`, `math.floor`, and `math.ceil` results to `int` that are not finite or fall outside `int` range;
- tensor shape, broadcasting, index arity and contiguity violations;
- tensor/autograd shape and dtype mismatches, invalid tracked mutations, unavailable gradients, and unsupported higher-order transforms;
- invalid runtime text operations such as malformed UTF-8;
- a zero `range` step;
- call depth exceeding the native safety limit before host stack exhaustion.

An explicit `error("message")` is instead a typed value. Numeric `Type.parse(text)` returns `error` for invalid or out-of-range text. A range-checked numeric cast likewise exposes `converted-type | error` when a runtime value may not fit; array and tensor casts use the whole converted container as the success alternative and fail atomically if any leaf is out of range. Bare `auto` infers the success type and fails fast, `auto | error` preserves the failure alternative, a success-only destination also fails fast, `try` propagates it, and `match` can recover from an explicitly preserved result locally. `scan(...)` returns `error` for end of input, invalid text, or input that does not match its format, and `print` and `flush` return `error` for an output failure; an expression statement that discards such an error fails fast (a standalone expression in an interactive session displays the error value instead).

Floating-point exceptional values follow IEEE-754 behavior. Runtime text is canonicalized to `nan`, `inf`, and `-inf`.


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
| `AUTOGRAD` | A tensor autograd operation failed: shape or dtype mismatch, invalid tracked mutation, unavailable gradient, or an unsupported higher-order transform. |
| `CALL_DEPTH_LIMIT` | Call depth would exceed the native safety limit before host stack exhaustion: `maximum recursion depth exceeded (limit 4096)`, reported at the call that would exceed it. |
| `CPU_THREADS` | `QUIDRA_CPU_THREADS` is not an integer from 1 to 256. Reported at the running statement, or naming the root file when none runs. |
| `DIVISION_BY_ZERO` | Integer division or remainder, or an exact `real` division, by a divisor that is zero at run time: `division by zero`. |
| `EXACT_NUMERIC` | The exact-number substrate failed: an exact-real provider or atom is unavailable or outside its domain, a symbolic value is not a finite real where one is needed, or an internal invariant broke. The message names the failure. |
| `EXACT_UNPROVEN` | An exact `real` comparison or evaluation could not be decided within the finite proof budget: `real comparison could not be proven within the finite proof budget`, `real comparison operand domain could not be proven`, `real evaluation budget exhausted`. |
| `FOR_ITERATION` | A writable collection loop (`for &value in values`) over a reference binding (a reference local or a `&` parameter) found that the body replaced or resized the iterated array through another binding that aliases it. Reported at that statement: `array iterated by reference was replaced or resized during the loop`. |
| `GPU_ASYNC` | A deferred GPU check that no synchronization point consumed failed when the program exited. Reported naming the root file without a line. |
| `GPU_SCOPE` | Package native code broke the Metal encode protocol of `include/quidra/native_extension.h`: it asked Core for device work inside its encoder scope, for a commit or wait while it held a device stream, or for another device during the hold, or it returned without ending the hold. Reported at the statement that made the extern call or that requested the refused work, at the `task.all` call that a hold left open earlier in its statement would block, at the `backward()` call whose custom autograd backward callback broke the protocol, and naming the root file without a line for a completion callback or a warm-up, which run outside any statement and end the process at once, and for a hold still open at program exit. Any other coded failure of a thread that still holds a stream ends the hold first and reports a violation the hold recorded instead, as `GPU_SCOPE`. |
| `GPU_SYNC` | Synchronizing a GPU failed: `gpu.sync(index)` with an unavailable or negative index or a failed device, or the wait of `time.now(sync = true)` and `time.since(start, sync = true)`, which is reported at the running statement. |
| `INDEX_BOUNDS` | An element index of an array, fixed array, string or bin is outside the current length: `index I out of bounds for length N` (a negative index included), reported at the index operand. A `text.slice` or bin slice is outside its value: `slice [S, E) out of bounds for length N`. A tensor element index or slice is outside its axis (counted from 0): `index I out of bounds for axis A with length N`, `slice [S, E) out of bounds for axis A with length N`. Slices and tensor accesses are reported at the indexing expression. |
| `INTEGER_OVERFLOW` | A fixed-width integer operation's result does not fit its type, at every supported signed or unsigned width, or a `nat` subtraction's result is negative (`nat subtraction result is negative`). |
| `INVALID_RANDOM_RANGE` | `Generator.int(start, end)` received an empty range (`start >= end`). |
| `INVALID_SLEEP_DURATION` | `time.sleep` received a negative or non-finite duration. |
| `NUMERIC_CONVERSION` | A numeric conversion failed; the message names what was converted (a value, an array element or a tensor element), the destination type and the reason: `numeric conversion out of range: value cannot be represented as int8` (`OUT_OF_RANGE`), `numeric conversion failed: non-finite value cannot be represented as real32` (`NON_FINITE`: ±infinity narrowed to `real32`), `numeric conversion failed: value is not an integer and cannot be represented as bigint` (`NOT_INTEGRAL`: an exact `real` that is a non-integral rational), `numeric conversion failed: the exact value could not be decided for int64` (`UNDECIDED`: a symbolic `real` the proof budget cannot decide). The JSON report carries `type`, `subject` (`VALUE`, `ARRAY_ELEMENT`, `TENSOR_ELEMENT`) and `reason`. A conversion that may fail has an `error` alternative with the same message; a fail-fast use of it stops with that error. `NUMERIC_CAST_RANGE` and the merged tensor cast text are no longer printed. |
| `PACKAGE_EXECUTION_POLICY` | Package native code requested an execution policy other than fast or deterministic. Reported at the user's statement that called the package. |
| `PARALLEL_BODY` | Package native code called a Core function other than the pure data-access functions inside a `qcore_parallel_for` body (`docs/spec/architecture.md`). Reported naming the root file without a line (a body runs no statement); the process ends at once. |
| `POWER_DOMAIN` | A power has no value: an integer power has a negative exponent (`integer exponent must be non-negative`), the base is 0 and the exponent 0 or negative (`0 ^ 0 is undefined`, `0 ^ a negative exponent is undefined`), or a fixed-width real base is negative and the exponent is not an integer (`a negative base requires an integer exponent`). |
| `RANGE_STEP_ZERO` | A `range` step is zero. |
| `SHAPE_MISMATCH` | An array's length differs from the extent its declared shape (`T[n][m]`) names. |
| `SHIFT_COUNT` | A fixed-width integer shift count is outside `[0, width)` at run time. |
| `TASK_ARRAY` | `task.all` received invalid operation or result storage. |
| `TASK_NULL` | `task.all` received a null operation. |
| `TASK_START` | `task.all` could not start a task thread. |
| `TENSOR` | A tensor operation failed: shape, broadcasting, index arity, contiguity, an unsupported cast dtype pair (`tensor cast from real64 to int8 is unsupported`), or a device failure (an index or slice outside its axis is `INDEX_BOUNDS`; a `tensor.gather` or `tensor.scatter` index outside its tensor keeps `TENSOR` or `AUTOGRAD` and says it is out of bounds; an element outside a cast's destination range is `NUMERIC_CONVERSION`). |
| `UNHANDLED_ERROR` | An `error` value reached a fail-fast site: bare `auto`, a success-only destination, or an expression statement that discards it. The message is the error's message. |
| `UNINITIALIZED` | Runtime-tracked array or tensor storage is read before it is initialized ("value is uninitialized"), or a binding whose initialization depends on the path taken is read on a path where it is not initialized ("'x' is uninitialized", naming the storage as written). |

A coded failure on a `task.all` task thread ends the process at once after its report,
with standard output and standard error flushed: exit handlers do not run while the other
tasks may still be running.
Source-bearing coded operations report the file, line and column of the user's statement.
Failed `test.check` / `test.equal` assertions report
`Quidra test assertion failed at FILE:L:C` and the snippet, with no message, while
retaining test-failure status 1 instead of runtime-safety status 101. Human text carries
no machine-stable fields; the JSON report below does. Compiler-generated operations with no public source node do not
invent an association.

Runtime-library failures that carry no code, such as host allocation failure or low-level
text/runtime invariant checks (an empty `split` separator, a `string.repeat` fill that is
not one code point, a `bin.fill` value other than 0 or 1), print the same report without
`[CODE]`: `Quidra runtime error at FILE:L:C`, naming the running user statement (or the
root file without a line when none runs), which is where the program was rather than an
invented site; their messages are unchanged, and they terminate with status 101.

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
  `start`, `end` and `length` for a slice, and `axis` beside them for a tensor, `limit` for
  the recursion limit, `type`, `subject` and `reason` for a numeric conversion); a failure
  without details has `{}`.
- `location` is the header's position: `file` (the display path), `line` and `column`, and
  `source_file` and `source_revision`, which let a tool check the file it opens; only `file`
  for a failure that names the root file without a line; `null` before the program
  registered its source table.
- `provenance` is the innermost statement's `source_revision`, `node_id`, `node_kind` and
  `source_file` when the compiler associated the statement with the public structural
  schema; the revision and node id are exactly those `quidra inspect` returns for that
  source snapshot. `null` otherwise.
- `deferred_origin`, `hops`, `unhandled_at`, `path`, `path_complete` and `causes` describe
  where an error came from and the call path; they are empty or `null` for failures without
  them.
- Strings are escaped as JSON; writing a report allocates nothing.

## REPL session safety errors

`REPL_REPLAY_UNSAFE` is a REPL session-safety error rather than a source diagnostic or runtime failure. It means the accumulated session may already have executed an external or nondeterministic effect that cannot be reconstructed safely by rerunning accepted source. Use `:reset` before executing another native submission. The barrier is deliberately conservative and may remain armed after a failed runtime submission if an effect could have occurred before the failure.
