# Quidra Language 0.1

## Source structure

Top-level statements execute in source order. Functions have explicit return and parameter types and are declared at top level. `main` is reserved because top-level code is the entrypoint.

Blocks use four spaces per indentation level; tabs are invalid as indentation. A dedent closes a block. Blank and comment-only lines do not affect nesting. Parentheses and square brackets permit line continuation. A trailing comma is optional in a list; canonical formatting omits it on one line and includes it in multiline lists. Strings may contain literal newlines and literal tab characters. Line comments start with `//`.

## Types

Numeric built-ins are `int8`, `int16`, `int32`, `int`/`int64`, `uint8`, `uint16`, `uint32`, `uint64`, `float32`, `float`/`float64`, `bigint`, and `bigreal`. `int` and `int64` are the same signed 64-bit type; `float` and `float64` are the same IEEE-754 binary64 type; `float32` is IEEE-754 binary32. `bigint` is an exact arbitrary-precision integer. `bigreal` represents exact rationals and symbolic exact real expressions such as `math.pi` and `math.sqrt(2.0)`. Numeric literals have no default type: integer-family literals may materialize as fixed integers or `bigint`, while real-family literals may materialize as IEEE floats or `bigreal`. A `bigint` context admits decimal integer literals beyond `uint64`; fixed-width contexts retain their normal range limits. `bool`, `string`, `bin`, and `error` hold booleans, immutable text, packed raw binary data, and error information. There is no `char`: text uses `string`, a one-byte numeric value uses `uint8`, and arbitrary raw bit sequences use `bin`. `void` denotes normal completion without data. Process termination is control flow rather than a source-visible value type. `auto` requests inference for a binding with an initializer.

`T | U` is an untagged surface description of a runtime tagged union: exactly one alternative is active. Union types flatten nested alternatives, remove duplicates, and have deterministic canonical representation independent of spelling order. A union with one distinct member is that member. Values and smaller unions can flow to compatible larger unions without wrapper calls; the runtime discriminator is adjusted to the destination type.

`none` is the absence alternative of a union. `string | none` can hold text or normal absence; `string | none | error` additionally admits failure. Standalone `none` declarations, function returns typed solely `none`, and `auto x = none` are invalid. `void` is allowed as a function result and as an alternative in a union. Non-continuing control flow is compiler-internal and is neither storable nor source-spellable.

## Arrays and shapes

Array type information precedes the binding name:

```quidra
int[] values = [1, 2, 3]
int[3] fixed = [4, 7, 8]
int[2][3] matrix = [[1, 2, 3], [4, 5, 6]]
```

`T[]` has runtime length. `T[expr]` accepts an integer extent expression. A compile-time expression is folded; otherwise it is evaluated once when that array binding is created and the resulting nonnegative extent is captured for that binding. Later mutation of variables used by the expression does not change the captured contract. Dimensions read from the outside inward: `int[5][6]` contains five arrays of six integers, while `float[][n * m]` has a runtime-sized outer dimension and a captured inner extent. Unconstrained `T[]` dimensions may still be ragged.

An array's storage and nested elements obey value semantics. Copying a nested array cannot make a later write to one copy modify another.

`array(count)` creates runtime-sized storage whose elements are initially uninitialized. It therefore requires an explicit array type context:

```quidra
int[] values = array(100)
values[0] = 7
print(values[0])
```

`auto values = array(100)` is invalid because there is no element value from which to infer the type. Reading an element before it has been initialized is a deterministic runtime safety error. The implementation tracks initialization independently from stored bits, using compact per-element state rather than treating zero or null as uninitialized.

`array(count, fill = value)` creates a fully initialized array and may infer its element type. The fill expression is evaluated once and every element receives an independent value-semantic copy. Negative counts and invalid allocation sizes produce runtime failure 101. Count zero is valid.

A fixed declaration such as `int[10] values` allocates its fixed storage immediately with all elements initially uninitialized. Individual elements may be written before the whole array is read. Fixed contiguous dimensions remain eligible for flattened native storage.

`[]` requires enough contextual element-type information. An `auto` binding initialized directly from an array literal infers runtime-sized `T[]`, preserving the normal appendable-array behavior. When `auto` receives an array-valued expression whose static type is already fixed, such as `.shape()` on a tensor whose rank is compiler-known, that fixed dimension is preserved rather than erased. A trailing comma does not add an element. Indexing requires `int`, starts at zero, and checks bounds. Assignment to an element does not append or resize an array. `len(array)` returns the runtime length as `int`.

## Bin

`bin` is a mutable packed raw bit sequence. It is not an array type and has no separate `bit`, `byte`, or `bytes` element type.

```quidra
bin zeros = bin.fill(8, 0)
bin ones = bin.fill(5, 1)
bin | error pattern = bin.parse("01010000")

bin data = bin.fill(8, 0)
data[0] = bin.fill(1, 1)
bin first = data[0]
bin nibble = data[0:4]
```

`bin.fill(n, bit)` allocates exactly `n` bits and accepts only `0` or `1` for `bit`. Length zero is valid. Negative lengths, invalid allocation sizes, and fill values other than 0 or 1 are rejected. `len(value)` returns the bit count. Indexing is zero-based and returns a one-bit `bin`; slicing uses a half-open bit range and returns `bin`. `print(bin)` and `bin.string()` expose the exact 0/1 sequence.

Written bit patterns use parsing rather than a separate literal grammar. `bin.parse(text)` accepts only `0` and `1` and always has static type `bin | error`, whether `text` is a literal or a runtime value. A statically known invalid literal may be rejected at compile time. A statically known valid literal may carry an internal success fact for optimization, but that fact does not remove the `error` alternative from the source-visible type.

Conversions between `bin` and other concrete types are always explicit. `bin(integer)` preserves the integer type's fixed-width bit representation, and `intN(bin)` / `uintN(bin)` require the bit length to equal the destination width exactly. `bin(bool)` produces one bit and `bool(bin)` requires exactly one bit. Flat integer/bool arrays convert explicitly with `bin(values)`; the reverse uses `T[](bits)` and requires the bit length to be exactly divisible by the element width. No conversion pads, truncates, wraps, or silently changes bit count.

Ordinary `bin` assignment has independent value semantics. A later write to one copy cannot change another copy. The implementation stores the sequence packed into bytes internally, but storage packing is not a source-level element model.


## Bindings and definite initialization

```quidra
int x
if true
    x = 1
else
    x = 2
print(x)
```

A declaration without an initializer leaves a scalar or ordinary aggregate binding uninitialized. Fixed arrays are the storage-oriented exception: their storage exists immediately, while initialization is tracked per element. Runtime-sized `array(n)` values use the same per-element model. Reading an element that has not been initialized is rejected at runtime; whole-array operations require the participating elements to be initialized. Whole-value assignment establishes the destination value. All continuing branches must establish ordinary binding initialization before a subsequent read; branches that return do not contribute to the merge. A loop may execute zero times, so assignment only within a loop does not establish initialization after it.

`auto x = expression` infers a static storable type. `auto x` is invalid. Assignments preserve the declared or inferred type. Compound assignments `+=`, `-=`, `*=`, `/=`, and `%=` are available when the corresponding binary operator is valid. The assignment target is evaluated exactly once, so an expression such as `values[next(&index)] += 1` does not repeat the index computation or its side effects.

Ordinary value arguments never specialize a call's static return type based on their values. Values determine behavior and compile-time facts; static types determine static types. An explicitly supplied expected type may constrain a result, and overload or generic resolution may select a return type from the static types of arguments. Compile-time-known values may still prove invalid input, prove an expected-type contradiction, remove unreachable checks, or enable other optimization; those facts remain internal and do not silently narrow the source-visible return type. In short: **values determine behavior and facts; types determine types; write the type when the result type must be fixed.**

Quidra uses absolute reservation plus monotonic visibility. A language-reserved identifier cannot be introduced by user code in any naming position, including bindings, parameters, functions, classes, fields, methods, generic parameters, loop/match binders, CLI fields, or import aliases. Qualification does not make a reserved spelling reusable. Standard-library internal declarations are language-owned and are the only implementation-level exception.

For non-reserved user names, a declaration cannot shadow a name that is already visible in its lexical environment. The same spelling may be reused only in disjoint scopes where the earlier declaration is not visible. In short: **reserved names are never reusable; visible names are never shadowable; otherwise names may be reused.**

## Functions, calls, and defaults

```quidra
int add(int a, int b = 1)
    return a + b

print(add(41))
print(add(a = 40, b = 2))
```

Function boundaries use explicit types. Named arguments use the declared parameter name followed by `=`. Positional arguments precede named arguments. Unknown names, duplicate supply, missing required arguments, and positional arguments following named arguments are errors.

Required parameters precede default parameters. A default is evaluated for each call that omits that argument; it is not evaluated when an explicit argument is supplied. Defaults are evaluated in parameter order and cannot reference parameters or caller-local bindings. They may call declared functions. Mutable array defaults are independent values on different calls. Reference parameters cannot have defaults.

A `void` function can fall through or execute bare `return`. A non-void result must be produced on every normally continuing path. `return void` can explicitly select a void alternative of a union. `return` itself carries control-flow information.

When a function or method returns a class value, the checker records the field paths that are definitely initialized on every normal return. Multiple returns contribute the intersection of those paths, including nested paths such as `data.ys`. Callers receive that summary, so a factory returning `Point(x = 1)` makes `x` readable while `y` remains statically uninitialized. These summaries are solved interprocedurally to a fixed point, so declaration order does not change the result.

## Text and dynamic array operations

`string` is immutable UTF-8 text. Repetition is explicit as `string.repeat(value, n)`; `value` must contain exactly one Unicode code point and `n` must be non-negative. `len(text)` counts Unicode code points rather than UTF-8 bytes. `text[index]` returns a one-code-point `string`, using the same code-point indexing model and deterministic bounds failure as other indexed values. `text.find(needle)` returns the code-point index or `none`; `text.slice(start, end)` uses a half-open code-point range and rejects invalid bounds at runtime. `trim()` removes Unicode whitespace at both ends. `split(separator)` preserves empty fields and requires a nonempty separator. `contains`, `starts_with`, and `ends_with` perform exact text matching. `text.utf8()` explicitly returns the UTF-8 encoding as `bin`; `string.from_utf8(data) -> string | error` performs the inverse only for byte-aligned `bin` containing valid UTF-8 without NUL, and reports representation failure as `error`; `text.codepoints()` explicitly returns Unicode scalar values as `int[]`. These conversions keep byte-oriented and text-oriented operations distinct rather than introducing a `char` type.

Runtime-sized `T[]` arrays support `append(value) -> T[]` and `concat(other) -> T[]`. Fixed-width numeric, `bool`, and `string` arrays of either fixed or runtime size support `sorted() -> T[]`; sorting is non-mutating and returns an independent runtime-sized value. Floating-point sorting places finite/non-NaN values in numeric order, preserves equal-value order, orders `-0.0` before `0.0`, and places NaNs last. String sorting uses deterministic Unicode-code-point-compatible UTF-8 lexical order. Any string array additionally supports `join(separator) -> string`, which constructs the result in one operation. Repeated `text = text + piece` is linear overall rather than quadratic: `string` is an immutable value, so when the assignment target is the sole owner of its storage the implementation may append into that storage with geometric growth, and neither the reuse nor the spare capacity is observable. `append`, `concat`, and `sorted` return new array values. They do not resize the receiver's backing storage in place, so an existing safe address such as `&values[i]` is never invalidated by the operation itself. A later implementation may use capacity, moves, or copy-on-write internally only when that optimization is unobservable.

Loops support `break` and `continue`. In writable iteration, changes to the current element are committed before either advancing with `continue` or leaving with `break`.

Conditional chains use `elif`:

```quidra
int score = 85
if score >= 90
    print("A")
elif score >= 80
    print("B")
else
    print("C")
```

## Modules

Imports are top-level namespace bindings. Imported files contribute class and function declarations to the compilation but cannot contain executable top-level statements.

```text
import geometry = "./geometry.qui"
import root = "@/shared/root.qui"
import plot = plotting
```

A quoted local path that does not begin with `@/` resolves from the importing file's directory. A quoted path beginning with `@/` resolves from the command working directory. The special meaning applies only to that leading `@/` prefix: `"./@/util.qui"` addresses a real `@` directory beside the importer, and `"@/@/util.qui"` addresses a real `@` directory under the command root. Local paths must end in `.qui`; project-root imports cannot escape the command working directory.

Standard namespaces are always visible and cannot be imported or aliased. For example, use `math.sqrt(...)`, `file.read(...)`, or `tensor.zeros<T>(...)` directly; `import math` and `import m = math` are errors. The standard namespaces are reserved so later declarations cannot change what those qualified references mean.

An unquoted non-standard target denotes an installed package, for example `import plotting` or `import plot = plotting`. Package resolution never falls back to a same-named source file in the working directory. Local source modules use quoted paths only. Imported declarations are accessed through their alias, for example `geometry.Point`. A module's own imports are private implementation namespaces rather than automatic re-exports.

Import aliases cannot collide with another import alias or a class/function declared in the same file. Import cycles are compile-time errors. Nested imports are supported and retain independent namespaces.

## Generics

Classes, functions, and methods may declare explicit type parameters:

```quidra
class Box<T>
    T value

T first<T>(T[] values)
    return values[0]

class Convert
    T identity<T>(T value)
        return value
```

Generic class instantiation remains explicit. Function and method type arguments may be omitted when every generic parameter is uniquely determined from the call arguments:

```quidra
Box<int> box = Box<int>(value = 7)
int[] values = [1, 2, 3]
int first_value = first(values)
Convert convert = Convert()
int sample = 5
int same = convert.identity(sample)
```

Explicit function or method type arguments remain valid when inference would be ambiguous or when the caller wants to state them explicitly.

Generic type arguments may themselves be arrays, unions, or instantiated generic classes. Generic classes may inherit instantiated generic parents, and generic methods may participate in explicit `override`. A generic parent method can be called statically with `super.method<int>(...)`.

Quidra implements generics by monomorphization. Each used concrete type-argument tuple produces one internal concrete class, function, or method before ordinary static checking and IR lowering. Repeated uses of the same tuple reuse the same instance. Generic declarations that are never instantiated do not emit native code.

Generic function and method calls infer type arguments only when every generic parameter is uniquely determined from the call arguments. If inference is incomplete, explicit type arguments are required. Generic classes remain explicit. Supplying type arguments to a non-generic target is an error.

A generic parameter may carry one minimal built-in compile-time constraint:

```quidra
T maximum<T: ordered>(T a, T b)
    if a > b
        return a
    return b

tensor<T> normalize<T: floating>(tensor<T> value)
    return value
```

The built-in constraints are `numeric`, `integer`, `floating`, `ordered`, and `equatable`. They add no runtime representation, vtable, dynamic dispatch, or implicit conversion. `integer` accepts the fixed-width integer families and `bigint`; `floating` accepts `float32` and `float`; `numeric` accepts integer, floating, and `bigreal`; `ordered` is the numeric set with relational ordering; and `equatable` accepts values with ordinary structural/scalar equality. Constraint failure is diagnosed before a concrete generic body is emitted. User-defined traits/interfaces are intentionally not part of this constraint system.

## Named enums and variants

Named enums represent semantic alternatives without overloading ordinary structural unions:

```quidra
enum Token
    Number(float)
    Name(string)
    Plus
    End

Token token = Token.Number(3.0)
Token end = Token.End
```

The enum name is the static type. Variants are always qualified with the enum name. A variant carries either one explicitly typed payload or no payload; payload-free variants are values and are written without parentheses. Different variants remain distinct even when they carry the same payload type.

Enum matching is exhaustive:

```quidra
enum Token
    Number(float)
    Name(string)
    Plus
    End

Token token = Token.End

match token
    Token.Number(value)
        print(value)
    Token.Name(name)
        print(name)
    Token.Plus
        void
    Token.End
        void
```

A payload binder exists only inside its branch. A payload may be ignored by matching the qualified variant without a binder. Enum values are nominal: they do not implicitly convert to or from an ordinary union with the same payload types. Internally, enum storage reuses Quidra's checked tagged-union representation; there is no dynamic dispatch or hidden subtype relation.

## Classes

User-defined data types use a single construct, `class`. A class may contain fields and methods:

```quidra
class Point
    float x
    float y

    float length_squared()
        return x * x + y * y
```

Every field has an explicit storable type. A field may optionally declare a default expression:

```quidra
class Config
    int retries = 3
    float timeout = 5.0
    string endpoint
```

Construction uses the class name with named field initializers. A constructor call may explicitly initialize all, some, or none of the fields:

```quidra
Point complete = Point(x = 3.0, y = 4.0)
Point partial = Point(x = 3.0)
Point empty = Point()

Config config = Config(endpoint = "server")
```

An explicit field initializer overrides that field's declared default. Omitted defaults are evaluated afresh for each construction in flattened field declaration order, after explicit construction expressions have been evaluated in source order. This gives mutable defaults independent storage for each instance. An omitted field without a default remains uninitialized rather than becoming zero, false, empty, or none.

Members are public by default. A field or method may be prefixed with `private`:

```quidra
class Counter
    private int value = 0

    private void increment_raw()
        value = value + 1
```

A private field may be read, written, or addressed only from a method declared by that field's declaring class. The same declaring class may access the private field on another instance of itself; a derived class may not access an inherited private field directly. Named construction is intentionally different from member access: a private field may still be supplied by name, such as `Counter(value = 1)`, so factory functions can create values with hidden internal state.

A private method may be called only from a method declared by that method's declaring class, including through another instance of that same class. Derived classes cannot call an inherited private method directly or through `super`. An inherited private member name remains occupied in the class member namespace, so a derived class cannot redeclare or override it. `private override` is valid only when the inherited target itself is non-private; it makes the derived replacement private to the derived class. The modifier order is `private override`, not `override private`.

Field-default expressions are checked outside an instance receiver context: they cannot read sibling fields or caller-local bindings. Inherited fields retain their defaults. Duplicate, unknown, positional, or writable field initializers are compile-time errors. Reading an uninitialized field is a compile-time error. Assigning a field initializes it. Initialization is tracked per field and through nested class fields.

Methods use the same function syntax and access fields directly. Quidra has no source-level `self` or `this`; an implicit receiver supplies member access. A method may call another visible method directly or through an explicit object expression. The checker infers which receiver fields a method must read before writing and which fields are definitely initialized on normal return.

```quidra
class Counter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

Counter counter = Counter()
counter.reset()
counter.increment()
```

Calling `increment()` directly on `Counter()` is invalid because `value` would be read before initialization. `reset()` requires no prior value and establishes `value`. Initialization effects use full nested field paths. Method summaries distinguish definitely initialized paths from receiver paths that may be written or invalidated: assigning `data.ys` initializes exactly that substorage, while replacing a class field with a partial value removes any old nested initialization facts that the replacement no longer guarantees. Conditional replacement is handled conservatively, and a later definite write can repair an invalidated path.

Ordinary class bindings have deep value semantics. Copying a class value produces independent nested class, array, and union storage and preserves field-initialization state:

```quidra
Point a = Point(x = 1.0)
Point b = a
b.y = 2.0
```

Here both copies begin with initialized `x` and uninitialized `y`; initializing `b.y` does not initialize `a.y`. Strings remain immutable values.

Single inheritance uses `:`:

```quidra
class Base
    int value

    int read()
        return value

class Derived : Base
    int offset

    override int read()
        return super.read() + offset
```

Inside a subclass method, `super.method(...)` invokes the implementation visible through the direct parent while using the current object as the receiver. Resolution is entirely static. `super` is not a value, cannot be stored or passed, and is valid only in the call form `super.method(...)`.

A child class inherits its parent's fields and methods but remains a distinct static type. Inheritance is member reuse and override, not implicit subtyping: `Base value = derived` and `Base &value = &derived` are invalid. Use an explicit union such as `Base | Derived` when a value may contain either type. Method selection is static; there is no dynamic dispatch, `virtual`, or object slicing.

Inherited fields precede the child's own fields in layout. Redeclaring an inherited field is forbidden. Replacing an inherited non-private method requires `override` and an exactly matching result type, parameter names, parameter types, and reference authority. An inherited private method is class-local and cannot be an override target; its name still cannot be reused in a derived class. For by-value parameters, `const` is a callee-local binding restriction and does not change the public method signature; for reference parameters, `T &` versus `const T &` is caller-visible authority and must match. `override` without an inherited target is an error. Multiple inheritance and inheritance cycles are invalid.

Reserved identifiers are absolute in user code. A field or method cannot reuse a reserved language name, bare built-in name, type name, or standard namespace name; qualification does not create an exception, so user-defined `object.math`, `object.tensor`, or `object.array()` are invalid when those identifiers are reserved. Standard-library implementation declarations are language-owned and are the only internal exception. Non-reserved member names still cannot collide with inherited or sibling members, and parameters or local bindings cannot shadow fields or methods visible in the current class.

Class `==` and `!=` compare values rather than allocation identity. The operands must have the same static class type, and all recursively compared fields must be definitely initialized. Inherited and nested fields participate in the comparison. Comparing partially initialized class values is a compile-time error because uninitialized state is not a value.

Array `==` and `!=` compare lengths and then elements recursively. Equality is available only when the element type has defined value equality. Strings compare contents. Quidra does not expose object or storage identity through an `is` operator.

## Addresses, references, and const authority

Quidra uses `&` consistently for safe storage addresses. For addressable storage `x`, `x` denotes its value and `&x` denotes its storage address. Address expressions are observation-only values: `print(&x)` and `write(&x)` expose the current raw machine address using the platform pointer representation, while `&x == &y` and `&x != &y` test whether two address expressions designate the same current storage. An address expression cannot be stored, converted to an integer, used in arithmetic or ordering, or dereferenced with `*`. Its printed representation is diagnostic and may change across executions, builds, platforms, or optimization choices. Using a reference name still accesses the referenced storage directly.

For example:

```quidra
int x = 1
int &alias = &x
int other = 1

print(&x)             // current raw address of x storage
print(&alias)         // the same address
print(&x == &alias)   // true
print(&x == &other)   // false
```

For managed values the address still means the address of the language-level storage being addressed. In particular, `&text` for a `string text` observes the binding/storage slot, not the separate UTF-8 payload pointer used internally or at an FFI borrow boundary.

`T &` is a read/write access path. `const T &` is a live read-only access path to the same storage model. `const T` is an immutable value binding. `const auto value = expression` and `const auto &view = &storage` infer the same underlying type while retaining those const contracts. The meaning of const is path-local: it prevents writes through that binding or reference; it does not freeze the underlying storage against changes made through another writable path.

```quidra
int a = 1
int &b = &a

b = 5
```

After the assignment, `a` and `b` both read as `5`. A reference binding may point at a binding, field, or array element. A read-only reference uses the same address but cannot write:

```quidra
int value = 5
const int &view = &value

value = 7
print(view) // 7
```

The change is visible through `view` because both paths address the same storage. However, `view = 8`, `&view = &other`, and `int &writer = &view` are compile-time errors. Authority may be reduced from a writable path to a const path, but cannot be recovered through the weaker path.

A reference binding may point at a binding, field, or array element:

```quidra
int[] values = [1, 2, 3]
int &first = &values[0]

Point point = Point(x = 1.0)
float &x = &point.x
```

Taking an address does not read the stored value, so uninitialized storage may be referenced by a writable reference that can initialize it:

```quidra
int value
int &alias = &value
alias = 7
print(value)
```

Reading `alias` before the write would be an uninitialized-read error. A `const T &` reference instead requires initialized storage at formation because that path cannot initialize the target. Temporary values such as `&5` or `&Point()` are not valid address targets. References are not storable inside class fields or arrays.

A writable reference can be rebound explicitly. A const reference cannot be rebound:

```quidra
int a = 1
int c = 2
int &b = &a

b = 5
&b = &c
b = 8
```

`b = c` assigns a value through `b`; `&b = &c` changes the address held by `b`. Consequently the example leaves `a == 5` and `c == 8`. Forms such as `b = &c` and `&b = c` are invalid. Taking `&` of an existing reference yields its current target address rather than a pointer-to-reference layer, so `print(&b)` observes the same raw address as `print(&c)` after the rebind and `&b == &c` is true.

Reference target identity is flow-sensitive. After an `if` or exhaustive `match`, the checker keeps a concrete target only when every continuing path resolves the reference to the same storage. If different targets remain possible, the runtime reference is still valid, but storage-specific initialization and non-alias proofs are discarded; a write through that reference initializes the reference access path without claiming which concrete root was initialized. Because a loop may execute zero or many times, a reference rebound inside a loop is treated conservatively after the loop. An explicit rebind after the control-flow join restores a precise target.

Reference parameters use the same model and remain explicit at both declaration and call sites:

```quidra
void inspect(const int &value)
    print(value)

void initialize(int &value)
    value = 7

int data = 1
inspect(&data)
initialize(&data)
print(data)
```

Both calls use `&data`: the parameter declaration determines whether the callee receives read-only or read/write authority. The checker infers read-before-write requirements for writable parameters. A write-only writable parameter can initialize previously uninitialized storage; a read-only reference always requires initialized storage. Reference markers cannot be omitted or supplied to ordinary parameters. Reference contracts preserve exact types, including fixed array shapes and class identity.

Reference aliasing is allowed. The same storage may be passed to multiple reference parameters, including `f(&data, &data)` and a mix of `const T &` and `T &`. A const parameter may therefore observe a value changed through another writable parameter during the call. Const guarantees only that the const path itself cannot perform or manufacture a write.

Addresses are rooted in existing storage: a binding, a receiver field, or substorage beneath one. Quidra does not create references into temporary values such as `&array(1, fill = 0)[0]`; bind the value first when its storage must outlive the expression.

A reference to a field or array element captures that substorage at address-formation time. Replacing the containing value does not retarget an existing substorage reference:

```quidra
int[] values = [1, 2, 3]
int &first = &values[0]
values = [4, 5]

print(first)      // 1
print(values[0])  // 4
```

The implementation keeps the old backing storage alive as long as such a reference needs it. `print(&first)` may observe the current machine address of that pinned substorage, but the numeric-looking text is diagnostic only and is not guaranteed to remain the same across separate executions, builds, platforms, or unrelated storage relocations allowed before an address becomes observable.

A reference to a whole binding is different: `Point &alias = &point` addresses the binding's storage slot, so replacing `point` is visible through `alias`.

Quidra intentionally has no Python-style `is` identity operator. Ordinary `==` compares values according to the value type, while `&left == &right` explicitly compares storage identity. Object-allocation identity remains distinct and is not exposed.

## Conditions and iteration

Conditions are `bool`. Boolean composition uses the keywords `and`, `or`, and `not`; they do not implicitly coerce numeric or other values to `bool`. `if` supports an optional `else`; `while` repeats while its condition is true.

```quidra
for i in range(2, 10, step = 2)
    print(i)

int[] values = [1, 2, 3]
for &value in values
    value = value * 2
```

`range(stop)` starts at zero. `range(start, stop)` defaults to step one. `range(start, stop, step)` accepts positive or negative nonzero steps and excludes the stop. `step = expression` names the third argument. A range is an iteration construct, not a storable value.

`for value in values` iterates an array or `bin` by value. `for &value in values` writes through to the original element. A `bin` iteration variable has type `bin`, with each value containing exactly one bit. A writable iterable must designate initialized storage. Writes may not invalidate the active iteration's storage or shape.

## Error propagation and match

```quidra
int | error calculate(bool valid)
    if valid
        return 21
    return error("invalid input")

int | error doubled(bool valid)
    auto value = try calculate(valid)
    return value * 2

match doubled(true)
    int value
        print(value)
    error problem
        print(problem)
```

A fallible union is preserved when no narrower destination is requested. In
particular, `auto result = operation()` retains the full `T | error` static
type.

When a value whose unnamed union contains `error` is consumed where the
expected type accepts every non-`error` alternative but excludes `error`,
Quidra inserts fail-fast handling at that consumption site. Success supplies the
non-error value; an actual `error` is reported and terminates the program at
that site. This rule removes only the distinguished `error` alternative:
ordinary unions are never implicitly narrowed, so `int | float` cannot flow
to `float` without explicit handling.

`try` requires an expression whose union includes `error`, inside a function
that can return that error. On error it returns the same error from the current
function. Otherwise its type is the input union with `error` removed,
collapsing a one-member residual type. Normal absence is not propagated by
`try`.

Process termination itself is not a source type. `process.exit(status)` and
other compiler-known non-continuing operations are tracked as control-flow
facts. The compiler may propagate that fact through a user function when it can
prove the function has no normal return. A loop is not assumed to be infinite
merely from its syntax.

A `match` evaluates its subject once. Each case selects one actual alternative using `T`, `T name`, or `none`. A binding is local to its case. Matching a named variable automatically narrows that name within each branch. This refinement cannot permit writes or aliases that corrupt the union representation or invalidate the checked branch type.

Cases must cover every alternative exactly once; missing, duplicate, and impossible cases are compile-time errors. Branch initialization information merges using only continuing branches.

## Strings and arithmetic

Strings support `{expression}` interpolation, concatenation with `+`, content equality, literal newlines, and literal tab characters. Backslash has no escape semantics: `\n`, `\t`, `\r`, `\b`, `\f`, `\v`, `\a`, `\xNN`, and Unicode-style backslash sequences are ordinary source characters. A backslash is written as itself. Because `"` still delimits a string, a quote character inside string data is written through the `quote` built-in value rather than by escaping it.

Numeric interpolation supports a compact format after `:`: `int=N` sets the minimum integer-part width, `frac=N` fixes fractional digits, `sig=N` fixes significant digits, and `zero` changes `int` padding from spaces to zeros. Examples are `"{value:int=5}"`, `"{value:int=4,frac=2,zero}"`, and `"{value:sig=4}"`. `frac` and `sig` are mutually exclusive because they define competing rounding rules, and `zero` requires `int`. `int` and `sig` accept literal values 1..1000; `frac` accepts 0..1000. A formatted interpolation must be numeric. Ordinary `{value}` interpolation keeps canonical scalar formatting.

The immutable built-in `string` values `enter`, `tab`, `home`, `quote`, `backspace`, `page`, `vtab`, and `bell` denote LF, HT, CR, `"`, BS, FF, VT, and BEL respectively. They are ordinary expressions, so both `"A{tab}B"` and `string separator = tab` use the same value. Literal `{{` and `}}` continue to represent braces in an interpolated string. NUL remains forbidden in source strings.

Numeric arithmetic requires matching operand types. An already-typed numeric value never changes representation implicitly, even when the conversion would be lossless. Numeric literals may be contextually typed directly when the literal is representable in that type. Representation changes require an explicit cast or an API operation whose arguments explicitly request that conversion.

Fixed-width integers additionally support `AND`, `OR`, `XOR`, unary `NOT`, `<<`, and `>>`. The uppercase words are deliberately distinct from bool-only `and`, `or`, and `not`; `&` remains storage access and `|` remains union syntax. Bitwise operations do not apply to `bool`, floating-point values, `bigint`, `bigreal`, `bin`, tensor, or neural values. Signed fixed-width integers have a defined two's-complement bit representation. `<<` operates on the N-bit representation and discards shifted-out bits without turning the operation into checked arithmetic; signed `>>` is arithmetic with sign extension and unsigned `>>` is logical with zero fill. Shift counts outside `[0, width)` are rejected statically when proven and otherwise fail deterministically at runtime.

Explicit numeric casts use the destination type directly: `int8(value)`, `uint32(value)`, or `float32(value)`. Integer-to-integer casts are allowed only when the runtime value is in the destination range; an out-of-range conversion fails deterministically and never wraps or clamps. Integer-to-float and float-to-float casts are explicit practical conversions and may use the destination IEEE-754 rounding. Generic float-to-integer casts are forbidden because they hide a rounding choice; use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` instead.

Numeric types expose `Type.parse(text) -> T | error`. Scalar values expose `.string()` for their standard textual form. Parsing is interpretation of text and is distinct from casting. `abs` accepts numeric values, `sqrt` accepts floating-point values, and `min`/`max` require two values of the same numeric type.

`print(value)` writes a scalar followed by a newline; `write(value)` writes without adding a newline. `input()` returns `string | none | error`: a valid UTF-8 line without embedded NUL produces a string with the trailing LF removed, EOF produces `none`, and an input or text-validation failure produces `error`. Runtime `string` values are always valid UTF-8 text and cannot contain embedded NUL; raw binary data belongs in `bin`.

Integer division or remainder whose divisor is statically known to be zero is a compile-time error. Otherwise integer overflow at every integer width, dynamically determined integer division/remainder by zero, array and bin bounds failures, invalid allocation sizes, out-of-range explicit integer casts, zero range steps, and exceeding the native call-depth safety limit are deterministic runtime errors with exit status 101. The call-depth guard fails before host stack exhaustion rather than allowing a segmentation fault. Floating-point exceptional values follow the corresponding IEEE-754 binary32 or binary64 behavior. Text formatting is canonical: NaN is `nan`, positive infinity is `inf`, and negative infinity is `-inf`.

Float text uses the shortest decimal representation that round-trips to the same binary floating-point value. If that shortest representation would look integral, at least one fractional digit is retained, so `0.6` stays `0.6`, `1.0 / 3.0` is `0.3333333333333333`, and `4.0` remains `4.0`. The same canonical form is used by print, write, interpolation, REPL display, and `.string()`. Fractional literals continue to require a leading zero; `.5` is invalid and `0.5` is the canonical form.

## Interactive evaluation

When attached to a terminal, `quidra` starts an interactive session. `quidra repl` starts the same session explicitly. Accepted top-level declarations and statements accumulate in session order and are rechecked with the normal language rules for each submission.

A standalone expression is an interactive submission result. Its checked value is displayed automatically unless its type is `void`; a process-terminating expression never reaches display. This is a REPL presentation rule, not an implicit source-level call to `print`; the expression retains the same parsing, typing, arithmetic, reference, class, generic, equality, and error semantics as the same expression in a source file.

The accumulated-source implementation compiles the root source from an in-memory overlay while retaining a stable virtual source path for relative imports, package-lock checks, and diagnostics; it does not write and re-read the growing root source on every submission. It still rechecks and recompiles the accumulated program. The already accepted prefix is marked explicitly in typed IR, and console output from that replay prefix is suppressed. External or nondeterministic operations are not silently replayed: before native execution of a candidate that may reach such an operation, the session arms a conservative safety barrier. A later submission is rejected with `REPL_REPLAY_UNSAFE` until `:reset`; the barrier remains armed if native execution fails because an external effect may already have occurred. `:reset` clears all accepted REPL state.

A rejected compile-time submission does not become part of the session. EOF exits successfully. Ctrl-C cancels the current incomplete submission without discarding previously accepted state.

## Tensor

`tensor<T>` is a first-class dense numeric N-dimensional value type. `T` must be numeric and is always static; Quidra does not have a tensor type with an unknown element type. Without a shape pattern, rank and known extents are compiler-inferred flow facts obtained from construction, reshape/index operations, control flow, and APIs such as `image.read`.

An optional second angle group is an **exact-rank shape pattern**:

```quidra
tensor<float32> any_rank
tensor<float32><3> vector_of_three
tensor<float32><3, _> rank_two_first_axis_three
tensor<float32><3, 224, 224> chw_224
tensor<float32><_, _, _> any_rank_three
```

The number of shape entries is the required rank. Each entry is either an integer expression or `_`. A constant integer expression is folded by the compiler; a runtime integer expression is evaluated once when the binding is created and the resulting nonnegative extent is captured for that binding. `_` requires the axis to exist but leaves its extent unrestricted. Therefore `tensor<float32><3, _, _>` accepts `[3,H,W]` but rejects `[3,H]`, `[3,H,W,D]`, and `[1,H,W]`. Element type and shape always occupy separate angle groups, the tensor element type is mandatory, and empty slots or trailing commas are invalid.

Known rank or extent conflicts are rejected statically. A tensor expected type may supply the numeric element type when an explicit shape argument is present, so `tensor<float32> x = tensor.zeros([2, 3])` is valid and avoids repeating `float32`. Without either an explicit tensor type argument or an expected tensor element type, as in `auto x = tensor.zeros([2, 3])`, the element type is ambiguous and the call is rejected. If a source tensor's relevant rank or extent is not statically known, assignment or parameter passing to a constrained destination performs the corresponding runtime constraint check instead of silently assuming the shape. Declared captured constraints remain fixed for that binding across reassignment, while inferred flow facts may weaken after reassignment or control-flow joins. APIs that produce runtime data may additionally validate an expected pattern through their normal result model; `image.read` is the primary example. Shape constraints and inferred rank/shape facts do not change TensorStorage or the LLVM ABI.

```quidra
tensor<float32> a = tensor<float32>([3, 224, 224])
a[0, 0, 0] = 1.0

tensor<float32> contextual = tensor.zeros([2, 3])
tensor<float32><3, _, _> z = tensor.zeros<float32>([3, 224, 224])
tensor<float32><1, _, _> o = tensor.ones<float32>([1, 224, 224])
```

The direct `tensor<T>(shape)` form creates uninitialized tensor storage. Scalar indexed assignment initializes an element. `tensor.zeros<T>` and `tensor.ones<T>` create fully initialized tensors. When the expected tensor type supplies the element type and every exact extent, `tensor.zeros()` and `tensor.ones()` may use that context to allocate; an unconstrained rank or `_` extent still requires an explicit shape argument. Initialization is tracked independently from shape knowledge and numeric contents; reading an uninitialized element is a deterministic safety failure.

### Tensor device placement

Device placement is explicit runtime/compiler metadata and is not part of the nominal `tensor<T><...>` type. CPU is the default. A tensor constructor may instead name a zero-based GPU index with the ordinary named-argument syntax `gpu = n`:

```quidra
tensor<float32> cpu = tensor.zeros<float32>([1024])
tensor<float32> gpu0 = tensor.zeros<float32>([1024], gpu = 0)
tensor<float32><1024> gpu1 = tensor.ones(gpu = 1)
```

The `gpu` argument is optional but, when present, must be named, integer-valued, and non-negative. There is no public negative GPU sentinel: `gpu = -1` is invalid. Omission means CPU. The same placement rule applies to uninitialized `tensor<T>(shape)`, zeros, and ones.

Transfers are explicit value operations. `value.gpu(index)` copies a tensor to the requested GPU and requires exactly one non-negative integer index. `value.gpu()` is invalid. `value.cpu()` copies a tensor to CPU and takes no arguments. An explicit transfer remains an observable semantic boundary for optimization: `tensor.zeros<T>(shape).gpu(0)` means CPU allocation followed by CPU-to-GPU transfer and may not be rewritten as direct GPU allocation. Likewise a later `.gpu(n)` cannot relocate an earlier computation to that GPU.

Quidra never performs an implicit CPU/GPU or GPU/GPU transfer. Tensor-to-tensor operations require compatible operands to be on the same device; a mismatch fails rather than copying either input. Results remain on the input device. Scalar literals and scalar variables are not tensor placements and may be passed as scalar kernel arguments to an operation on the tensor's device.

A requested GPU that does not exist or whose backend is unavailable is a runtime error, for example `error: gpu(0) is not available`. CPU fallback is forbidden. If an operation has no implementation for the tensor's current GPU backend, it fails explicitly with a diagnostic such as `operation is not supported on gpu(0)`; executing the operation over hidden CPU storage is not a valid implementation.

The public placement semantics are independent of OS and vendor. NVIDIA systems use Quidra's NVIDIA backend through the CUDA Driver API rather than a user `nvcc`, `CUDA_HOME`, or `/usr/local/cuda` selection. Apple Silicon uses Metal. AMD uses the HIP runtime and runtime-compiled HIP kernels when a compatible ROCm/HIP runtime is present, without changing Quidra source syntax. Backend kernel availability may still be element-type-specific; an unsupported backend/element-type combination is an explicit runtime error and never permission for CPU fallback. Unified-memory hardware may allow a backend to elide a physical copy, but the explicit logical device transition remains part of the program semantics. `quidra gpu` reports Quidra's zero-based device enumeration and active backend information.

Tensor storage is row-major, with the last dimension contiguous. Integer indices remove axes, slices retain axes, and omitted trailing dimensions mean full slices. Inferred rank and known shape facts are projected accordingly. Fully indexing a known rank-N tensor with N integer indices yields an internal rank-0 tensor, not a scalar; `.item()` is the explicit scalar extraction operation. On a GPU tensor, `.item()` performs only the required one-element device-to-host read. This scalar extraction is an explicit semantic boundary and is not permission to move or evaluate the surrounding tensor operation on the CPU.

```quidra
tensor<float32> z = tensor.zeros<float32>([3, 224, 224])
auto channel = z[1]
auto crop = z[:, 10:20, 30:40]
float32 value = z[0, 10, 20].item()
```

Slices may share internal storage, but source semantics remain value-oriented. Mutating a copied tensor or slice triggers copy-on-write when needed. Slice assignment and writable `&` references to tensor elements are intentionally not exposed.

`.transpose(axis0, axis1)` swaps two non-negative axes as a metadata-only view: shape and strides are permuted while storage, offset, and device placement are preserved. Constant axis values may be used to reject an invalid axis at compile time, but they do not permute or specialize the source-visible static shape type. `.reshape(shape)` requires contiguous storage and never performs a hidden copy. Use `.contiguous()` explicitly before reshaping a non-contiguous view. The static length of the shape array establishes result rank; its extent values may be used for diagnostics or expected-type checks but do not refine the source-visible result extents. `.shape()` returns `int[N]` when rank N is inferred at that program point and `int[]` when rank is unknown. `.is_contiguous()` reports layout state. `linear.dot` requires two rank-1 tensors. `linear.matmul` supports vector-matrix, matrix-vector, and matrix-matrix multiplication; vector-vector multiplication remains `linear.dot`. Statically known rank mismatches are rejected and unknown rank retains runtime validation. Image values decoded by `image.read` always carry inferred rank 3.

Tensor `+`, `-`, `*`, `/`, and integer `%` are elementwise. Tensor-to-tensor implicit broadcasting requires identical rank; each axis must match or have size 1 on one side. Rank-changing broadcasting is not implicit. Scalars are the one exception and broadcast to any tensor rank.

Explicit numeric casts use the destination scalar type as the operation: `float(value)`, `float32(value)`, `int8(value)`, and so on. Applied to a tensor, the cast preserves rank, shape constraints, known extents, and value semantics while converting every initialized numeric element. Applied to a neural value, a floating-to-floating cast preserves the differentiable graph. Integer narrowing is range-checked; integer-to-float and float-to-float conversion may use destination IEEE-754 rounding. Generic float-to-integer casts are forbidden because they hide a rounding choice. Container-specific cast methods are not part of the surface language.

## Implementation scope

The native core supports fixed-width numeric types, no implicit representation-changing numeric conversion, and practical explicit casts, numeric parsing and scalar text conversion, packed mutable bin, initialized/uninitialized arrays, first-class dense tensors, tensor statistics and vector/matrix multiplication, PNG/JPEG/BMP/TIFF/WebP image I/O through `image`, console I/O, automatic standard namespaces, explicit package/local-module resolution, monomorphized generics with unambiguous function/method inference, user-defined classes, single inheritance, capture-free typed function values, and a minimal structured-concurrency primitive (`task.all`) for capture-free `fn<void>()` operations. Richer concurrency (typed task results, cancellation, and explicit value transfer), WASM, self-hosting, broader signal-processing APIs, and broader package distribution remain development areas.


## Standard namespaces and imports

Standard namespaces are always visible; importing them is an error. Current reserved namespaces are `math`, `cli`, `file`, `environment`, `test`, `time`, `task`, `random`, `process`, `map`, `set`, `json`, `http`, `tensor`, `stats`, `linear`, `signal`, `image`, and `neural`.

A source-file import always uses a quoted path:

```quidra
import local = "./local.qui"
import shared = "@/shared.qui"
```

An unquoted non-standard import is an installed package:

```text
import plotting
import plot = plotting
```

Package resolution never silently falls back to the current directory. This distinction is semantic: adding or removing a local file cannot change the meaning of an unquoted import.

### math

`math` exports `pi`, `e`, `sin`, `cos`, `tan`, `log`, `exp`, `pow`, `abs`, `sqrt`, `min`, and `max`. Transcendental functions require floating-point inputs. `pow` requires two values of the same floating-point type.

### cli

`cli` enables one declarative command-line binding in the root source file:

```quidra
cli args
    string source = argument()
    int count = option(default = 1)
    bool verbose = flag()

print(args.source)
print(args.count)
print(args.verbose)
```

`argument()` declares a required positional value. `option(default = value)` declares a named `--field value` option whose type is inferred from the field declaration and checked against its default. `flag()` declares a boolean `--field` flag. Field names are the command-line names, so they are not repeated as string literals. CLI scalar values support `string`, `int`, `float`, `bigint`, `bigreal`, and `bool`; `flag()` is `bool` only. Exact numeric CLI text is parsed directly into `bigint`/`bigreal` without an intermediate fixed-width integer or IEEE float. Missing required values, invalid typed values, non-UTF-8 text values, duplicate or unknown arguments, and misplaced arguments terminate with CLI status 2.

The generated CLI binding is a root top-level value. Function bodies do not implicitly capture top-level bindings, including the CLI binding; pass CLI-derived values or a configuration value explicitly to reusable functions.

Direct execution passes arguments after the source path. With the explicit `run` command, `--` separates compiler options from program arguments:

```text
quidra app.qui input.png --count 5 --verbose
quidra run app.qui -- input.png --count 5 --verbose
```

### file

`file` provides simple whole-file and filesystem operations:

```quidra
auto text = file.read("input.txt")               // string | error
auto raw = file.read_bin("input.bin")            // bin | error
auto saved = file.write("out.txt", "x")          // void | error
auto present = file.exists("out.txt")            // bool | error
```

It also exports `remove`, `copy`, `move`, and `mkdir`, each returning `void | error`. `file.list(path)` returns `string[] | error`: on success it contains the direct child paths of the directory, non-recursively, sorted lexicographically so enumeration order is deterministic. Returned paths preserve the directory prefix supplied by the caller. I/O failure is typed data rather than an implicit process abort. `read` and `write` are text-oriented whole-file operations. `read` accepts only valid UTF-8 without embedded NUL and returns `error` for invalid text. `read_bin(path) -> bin | error` and `write_bin(path, bin) -> void | error` are binary whole-file operations. File input produces a byte-aligned `bin`; file output requires `len(value) % 8 == 0` and otherwise fails deterministically. Arbitrary byte values, including NUL and invalid UTF-8, are preserved exactly. `file.list` converts host paths to UTF-8 and returns `error` if a path cannot be represented as Quidra text. Text and binary I/O are separate so arbitrary bytes never enter `string` storage implicitly.

For incremental ownership of an open file, `file.open(path) -> file.Handle | error` returns an opaque resource handle:

```quidra
string | error read_open_file(string path)
    file.Handle handle = try file.open(path)
    string text = try handle.read()
    return text
```

`file.Handle.read() -> string | error` and `read_bin() -> bin | error` consume the remaining bytes from the handle's current position. `close() -> void` is an optional early release and is idempotent. If `close()` is not called, that owned handle closes its native file deterministically when the handle leaves its lifetime, including ordinary return and `error` propagation; no `defer` or manual cleanup is required, and release timing does not depend on GC.

`file.Handle` follows Quidra value semantics. Copying a live handle creates an independent native reader positioned at the same byte offset. Reading or closing one copy does not change another copy. A closed handle copies as closed. `file.open` is currently read-only; whole-file writes remain explicit `file.write` / `file.write_bin` operations.

### environment

`environment.get(name)` returns `string | none`; an unset variable is normal absence and yields `none`. `environment.has(name)` returns `bool`. Retrieved strings are copied into runtime-owned immutable text so an already returned value cannot change if the host environment later changes. Because this API has no error case, a host value that is not valid UTF-8 text causes a deterministic runtime text failure instead of creating an invalid `string`.

### test

`test.check(condition)` requires `bool`. `test.equal(actual, expected)` requires identical static types and follows the same equality availability and class definite-initialization requirements as ordinary `==`.

A failed assertion exits with status 1. This is a test failure, not a runtime-safety violation, so it does not use the runtime-safety status 101.

### time

`time` represents elapsed-time concepts with opaque value types rather than unitless integers:

```quidra
time.Instant start = time.now()
time.Duration pause = time.seconds(0.01)
time.sleep(pause)
time.Duration elapsed = time.since(start)
print(elapsed.seconds())
```

`time.now()` returns a monotonic `time.Instant`. `time.since(start)` returns a `time.Duration`. `time.seconds(value)` constructs a duration from a finite nonnegative-or-negative numeric duration value; `time.sleep(duration)` requires a finite nonnegative duration and otherwise terminates as a runtime-safety failure with status 101. `Duration.seconds()` exposes the duration as `float` when numeric computation is explicitly desired. Standard-library time values cannot be constructed through their class names, and their internal representation is not source-visible.

### random

`random` has no implicit process-global generator. Randomness is explicit state:

```quidra
random.Generator rng = random.generator(seed = 42)
int n = rng.int(1, 10)
float x = rng.float()
bool b = rng.bool()
```

`random.generator(seed)` accepts an `int` seed and deterministically creates a generator. `Generator.int(start, end)` samples uniformly from the half-open integer range `[start, end)` using rejection sampling; `start >= end` is a runtime-safety failure with status 101. `Generator.float()` yields a `float` in `[0.0, 1.0)`; `Generator.bool()` yields a boolean. Calls advance only that generator's state. Ordinary assignment copies generator state according to normal class value semantics, so later advancement of one copy cannot mutate another. Standard-library generator values cannot be directly constructed and their state representation is not source-visible.

### process

`process.run` launches an executable directly with an explicit string-array argument vector:

```quidra
process.Result result = process.run("git", ["status", "--short"])
print(result.started)
print(result.status)
print(result.output)
print(result.error)
```

No shell is inserted between the program and its arguments. `process.shell(command)` is the explicit alternative when shell syntax such as pipelines, redirections, or shell expansion is required. It invokes `/bin/sh -c` on POSIX systems and `cmd.exe /S /C` on Windows, returning the same `process.Result`. The command is interpreted by that shell, so untrusted values should be passed as structured arguments to `process.run` rather than concatenated into a shell command. Quidra string interpolation is processed first; literal braces in a shell command are therefore written as `{{` and `}}`. `Result.started` is false when the executable or shell could not be launched; in that case `status` is -1 and `error` describes the launch failure. If the process starts, `status` is its exit code, or `128 + signal` when it terminates by signal on POSIX systems. `output` and `error` capture stdout and stderr independently. They are text fields, so captured streams must be valid UTF-8 and contain no embedded NUL; arbitrary binary child output is not silently coerced into a `string` and causes a deterministic runtime text failure. This separates launch failure from an ordinary nonzero child exit without treating child exit codes as Quidra runtime failures. `process.Result` is compiler-provided and cannot be directly constructed. `process.exit(status)` terminates the current Quidra program with the supplied `int` status. It is deliberately namespaced so adding process functionality does not consume another bare user identifier.

### map

`map.Map<K, V>` is an explicit generic value container. Construction uses ordinary generic class syntax:

```quidra
map.Map<string, int> counts = map.Map<string, int>()
counts.set("apple", 2)
counts.set("banana", 1)
counts.set("apple", 3)

print(counts.has("apple"))
auto value = counts.get("apple")
string[] keys = counts.keys()
```

`set(key, value)` inserts or replaces without changing an existing key's position. `remove(key)` removes the key and returns whether it was present; reinserting a removed key places it at the end of insertion order. `get(key)` returns `V | none`; `has(key)` returns `bool`; `size()` returns `int`; `keys()` and `values()` return value arrays in stable insertion order. Keys are restricted to integer types, `bool`, and `string`; float keys are intentionally excluded because NaN would make equality and hashing ambiguous. The native implementation uses deterministic open addressing for average O(1) `has`, `get`, and `set` lookup while retaining separate insertion-order arrays for `keys()` and `values()`. Hash buckets and capacity are implementation details and are not observable language semantics.

### set

`set.Set<T>` is the corresponding unique-value container:

```quidra
set.Set<string> tags = set.Set<string>()
tags.add("compiler")
tags.add("ai")
tags.add("compiler")
print(tags.size())
print(tags.has("ai"))
```

`add(value)` preserves the first insertion position. `remove(value)` removes the value and returns whether it was present; adding it again places it at the end of insertion order. `has(value)` tests membership, `size()` returns the number of unique values, and `values()` returns insertion order. Elements use the same key-domain restriction as `map.Map`. The implementation uses the same deterministic open-addressing index, giving average O(1) membership and insertion while preserving stable insertion order. Map/set internals are compiler-generated and are not source-visible. Assignment follows ordinary independent value semantics rather than sharing mutable container identity.


### json

`json.parse(text)` returns `json.Value | error`. `json.Value` is an immutable standard value whose runtime representation is not source-visible.

The value methods are:

- `kind() -> string`: one of `"null"`, `"bool"`, `"number"`, `"string"`, `"array"`, or `"object"`.
- `size() -> int | error`: array length or object member count; other kinds return `error`.
- `get(key) -> json.Value | none | error`: object lookup. Missing keys are `none`; calling it on a non-object returns `error`.
- `at(index) -> json.Value | none | error`: array lookup. Out-of-range, including negative indexes, is `none`; calling it on a non-array returns `error`.
- `text() -> string | error`, `integer() -> int | error`, `number() -> float | error`, and `boolean() -> bool | error`: explicit typed extraction.
- `encode() -> string`: compact JSON serialization preserving object insertion order and the original JSON number spelling.
- `equal(other) -> bool`: recursive structural equality. Object member order does not affect equality. JSON numbers compare by parsed numeric value.

JSON `null` is not Quidra `none`. A present JSON null is a `json.Value` with kind `"null"`; `none` is reserved for missing lookup results. Duplicate object keys are rejected during parsing rather than silently overwritten. Parsing rejects malformed numbers, malformed escapes, unmatched surrogate escapes, trailing content, and nesting deeper than 256 levels. Integer extraction accepts JSON numbers written in integer syntax and representable by Quidra `int`; exponent or fractional spellings use `number()`.

Ordinary source-level `==` is intentionally not defined for `json.Value`; use `equal` so JSON comparison semantics remain explicit.


### http

`http.get(url)` returns `http.Response | error`.

`http.Response` exposes:

- `status: int`: the HTTP response status code.
- `body: bin`: the complete response body as byte-aligned raw binary data.
- `header(name) -> string | none`: the first matching response header, using ASCII case-insensitive field-name comparison. Absence is `none`. Header values are exposed as Quidra text, so a returned value must be valid UTF-8 without embedded NUL; malformed host text fails deterministically rather than entering `string` storage.

HTTP status is not transport success. Any response received through HTTP, including 4xx and 5xx, is represented as `Response`. DNS resolution failure, connection failure, TLS verification failure, unsupported protocol, redirect failure, or timeout returns `error`.

The v0.1 implementation uses libcurl directly in the native runtime. It permits only HTTP and HTTPS URLs and redirects, follows at most ten redirects, enables content decoding supported by libcurl, uses a 5-second connection timeout and a 30-second total timeout, and leaves normal TLS certificate and hostname verification enabled. The runtime buffers the complete response body in memory. The body is deliberately `bin`; text decoding is not implicit.

`http.Response` contains immutable internal header metadata which is not source-visible. Ordinary assignment preserves independent observable value behavior; header metadata may be shared because it is immutable. Source-level `==` is not defined for `http.Response`.


### stats

`stats.sum(value)`, `stats.min(value)`, and `stats.max(value)` reduce a numeric tensor to a scalar of the same element type. Integer `sum` is overflow-checked. `min` and `max` are undefined for an empty tensor. `stats.mean(value)` returns the arithmetic mean as `float` and is likewise undefined for an empty tensor. All four reductions accept contiguous tensors or views, require every participating element to be initialized, and preserve explicit GPU placement semantics without hidden CPU fallback; only the scalar result is transferred to the host when the API returns a host scalar.

### linear

`linear.dot(a, b)` computes the scalar dot product of two rank-1 numeric tensors with the same element type and length. The scalar result preserves that element type. `linear.matmul(a, b)` supports vector-matrix `[k] × [k, n] -> [n]`, matrix-vector `[m, k] × [k] -> [m]`, and matrix-matrix `[m, k] × [k, n] -> [m, n]` multiplication. Operands must have the same tensor element type and compatible inner dimensions; results preserve that element type and device placement. Integer multiplication and accumulation remain overflow-checked. Vector-vector multiplication remains `linear.dot`; higher-rank batched matmul is not implicit in language version 0.1.

### image

`image.read(path)` decodes to CHW and preserves both source channel count and every sample element type representable by Quidra and the codec. Its successful tensor alternative has inferred rank 3. Grayscale is `[1,H,W]`, RGB is `[3,H,W]`, and RGBA is `[4,H,W]`. PNG decodes to `uint8` or `uint16`; TIFF supports every built-in numeric tensor element type; JPEG, BMP, and WebP decode to `uint8`.

The expected tensor type is an acceptance constraint, never an implicit conversion request. For example:

```quidra
string path = "input.png"
tensor<uint16><3, _, _> | error loaded = image.read(path)
```

accepts only a rank-3 uint16 image whose decoded CHW channel axis is 3. An element-type, rank, or fixed-extent mismatch returns `error`.

Conversion is performed only by explicit named arguments:

```quidra
string path = "input.png"
image.read(path, channel = 1)
image.read(path, channel = 3, type = float32)
```

`channel` is an ordinary `int` value. Runtime values are accepted and must evaluate to 1, 3, or 4; a compile-time-known invalid value may be rejected early. The value does not specialize an `auto` result type. 1→3 replicates gray; 3/4→1 uses `0.299R + 0.587G + 0.114B` and ignores alpha; 4→3 explicitly discards alpha; 1/3→4 adds opaque alpha (integer maximum or 1.0 for floating point). `type = T` explicitly changes numeric representation without normalizing ranges. Integer-to-integer conversion fails if any value is out of range; integer-to-float and float-to-float use the explicit numeric rounding policy; float-to-integer remains forbidden without an explicit rounding operation. A conversion argument that conflicts with the surrounding expected output type is a compile-time error.

`image.write(path, image, quality = 95)` accepts a fully initialized CPU CHW numeric tensor and returns `void | error`. Image codecs and filesystem I/O are host operations: a GPU tensor is rejected rather than being copied to CPU implicitly, so callers must write `image.write(path, image.cpu(), ...)` when that transfer is intended. The codec is selected from the filename extension and writing succeeds only when that codec can represent the tensor element type without conversion: PNG supports `uint8` and `uint16`, TIFF supports every built-in numeric tensor element type, and JPEG/BMP/WebP require `uint8`. JPEG and WebP quality is 1 through 100. JPEG rejects RGBA input unless the caller explicitly converts channels first.


Higher-level tensor image processing is provided by the official `vision`
source package through `import vision`. It is resolved by the ordinary package
system and has no compiler-specific name handling.

### video

`video.open(path)` returns `video.Reader | error` and opens one video stream for incremental decoding. The runtime does not decode or buffer the complete video up front.

`video.Reader` exposes methods rather than source-visible representation fields:

- `width() -> int` and `height() -> int`
- `fps() -> float | none`
- `frames() -> int | none`
- `duration() -> float | none` in seconds
- `position() -> int`, the zero-based index of the next logical frame
- `seek(frame) -> void | error`
- `read(...) -> tensor<T> | none | error`

A successful `read()` yields one CPU CHW frame. The default channel layout is RGB. `channel = 1`, `3`, or `4` explicitly requests gray, RGB, or RGBA. Decoded component precision is represented as `uint8` for up-to-8-bit sources and `uint16` for higher decoded precision, rather than silently narrowing high-bit-depth video to 8-bit. `type = T` is an explicit numeric representation conversion and follows the same range-preserving policy as `image.read`; sample ranges are not normalized. The conversion from an encoded YUV/RGB pixel format into the requested gray/RGB/RGBA layout is part of decoding, not an implicit tensor cast.

Conversion option values do not specialize the static result type of an `auto` expression. An explicit expected type such as `tensor<uint16><3, _, _> | none | error` is an acceptance constraint and may narrow the checked result. A runtime dtype/shape mismatch returns `error`. Clean end-of-stream is `none`.

`fps()`, `frames()`, and `duration()` return `none` when the container cannot supply the corresponding metadata. `seek(frame)` uses a logical frame index and does not silently reinterpret it as a timestamp. Reader assignment, parameter passing, and return preserve ordinary Quidra value semantics: a copied Reader has an independent logical position. Immutable source metadata may be shared internally, while decoder state is reconstructed lazily for a copied value. Native decoder resources are released automatically with value lifetime.

No GPU transfer is hidden in video I/O; decoded frames stay on CPU until an explicit `.gpu(n)`. Audio is ignored by this API. The native implementation uses FFmpeg libavformat/libavcodec/libavutil/libswscale behind the standard API, but FFmpeg types and handles are not source-visible.

`signal` is reserved as a standard namespace so its future qualified API cannot be captured by a user bare declaration, but language version 0.1 does not define public signal-processing callables.

## Function values and union payload initialization

Quidra has explicit capture-free typed function values. The type form is `fn<R>(P...)`, with the result type before the parameter list:

```quidra
int twice(int value)
    return value * 2

int apply(fn<int>(int) operation, int value)
    return operation(value)

fn<int>(int) operation = twice
print(apply(operation, 21))
```

A function declaration becomes a value only when an explicit `fn<...>(...)` context supplies the complete signature. `auto operation = twice` is rejected rather than inferring an implicit function type. Conversion requires an exact result/parameter match and currently represents only by-value parameters; a function with a reference parameter cannot be converted to an `fn` value. `extern` C declarations are not function values, and methods are not implicitly converted into bound closures. Function-value calls use positional by-value arguments only.

The value contains only the statically known code target: it captures no local, receiver, top-level, or hidden environment. Ordinary assignment/pass/return therefore does not create a closure allocation or hidden lifetime. Function identity is not part of source semantics, so `==` and `!=` are not defined for function values. Calling an uninitialized function binding is a definite-initialization error. A function value stored behind an explicit reference still follows the ordinary reference read-before-use rules.

### Structured concurrency

`task.all(operations)` is the current deliberately small concurrency surface.
It accepts `fn<void>()[]`, starts each capture-free operation, waits for every
started operation, and returns only after all have joined. The operation lifetime
is therefore lexically bounded by the call: there are no detached tasks.

```quidra
void first()
    print("first")

void second()
    print("second")

task.all([first, second])
```

The completion order of independent operations is intentionally unspecified.
Because function values have no captures and `task.all` accepts no arguments,
managed Quidra values cannot currently cross the thread boundary through this
API. Managed ownership therefore remains thread-local rather than becoming
implicitly synchronized. Shared mutable state, implicit thread sharing, detached
task lifetime, hidden cancellation, and implicit value transfer are not part of
this subset. Typed task results, cancellation, and explicit cross-task value
transfer require a separate ownership/error contract and are intentionally not
specified yet.

Class values preserve field-level definite initialization while they remain class-typed. Converting a class value into a union closes that hidden initialization state: the class must be fully definitely initialized first. Therefore a class alternative selected by exhaustive `match` is known to be fully initialized, including when the union crossed a function boundary.

For arrays whose type has a statically known length, a constant index outside `[0, length)` is rejected at compile time. Dynamic arrays remain bounds-checked at runtime.

## Native execution cost model

Generated native code is optimized with Clang `-O3` without fast-math. Runtime safety remains explicit: checked arithmetic, bounds checks, ownership, and recursion protection are not disabled for speed.

Direct calls remain statically resolved. Function-value calls lower through an explicit indirect-call IR node whose signature was already checked. The backend identifies direct recursive cycles and also treats any function whose address becomes a function value as a possible indirect recursion target; only those functions receive dynamic call-depth bookkeeping. Ordinary acyclic direct calls do not pay that fixed overhead.

Managed ownership bookkeeping is local to the executing thread in the current runtime. The language does not currently expose cross-thread sharing of managed values, so retain/release does not require a process-wide mutex. If a future concurrency model introduces shared managed values, that ownership contract must be revisited explicitly rather than silently changing this assumption.


Writable named arguments expose both sides of the reference contract:

```text
update(&state = &state)
```

The left `&state` identifies the writable parameter and the right `&state` forms the storage address. `state = &state` is invalid.

## Explicit numeric conversion

Already-typed numeric values are never converted implicitly. The one generic numeric cast syntax is `T(value)`, where `T` is the destination scalar numeric type:

- for a scalar, it converts that scalar;
- for a numeric array of any nesting, it preserves every fixed/dynamic array dimension and recursively converts numeric leaves;
- for a tensor, it preserves rank/shape facts and converts the element type.

For example, `float(values)` maps `int[][]` to `float[][]`, while `float32(image)` maps `tensor<uint8><3, _, _>` to `tensor<float32><3, _, _>`. A container cast is a whole-value operation: every source leaf is read, so an uninitialized element fails deterministically, and no partial converted value is observable.

The admissibility rule is exactly the scalar rule applied to every leaf: integer-to-integer is allowed when the runtime value is in range and never wraps; integer-to-floating point is allowed with destination IEEE-754 rounding; floating-point to floating-point may reduce precision deterministically; floating-point to integer is not a generic cast because it requires a rounding choice. Use `math.trunc`, `math.round`, `math.floor`, or `math.ceil` for floating-point to `int`. They reject non-finite and out-of-range results deterministically. `math.round` rounds halfway cases away from zero.

Container syntax is not duplicated at the cast site. Numeric representation conversion always uses the scalar destination form `T(value)`; tensor type syntax remains reserved for tensor storage construction.


## Neural values and training state

`neural` is shorthand for `neural<float32>`; another floating element type is written explicitly. Neural uses the same exact-rank integer-expression/`_` shape patterns and binding-time captured extent semantics as tensor. `neural<3, _, _>` is shorthand for `neural<float32><3, _, _>`, while `neural<float><_, 768>` explicitly selects float64 and rank 2. `neural.track(tensor)` creates an immutable value in a dynamic graph while preserving rank/shape facts, `.untrack()` returns ordinary tensor storage with those facts restored, and floating numeric casts remain differentiable graph operations. `neural.grad(loss)` traverses the executed graph and returns an independent `neural.Gradients` value without hidden accumulation.

Learnable storage is represented by `neural.Parameter<T>` and persistent non-gradient storage by `neural.State<T>`. A model is an ordinary class containing these values. Parameters have opaque persistent identities used to match gradients; user code cannot replace or index-write `Parameter.value`.

The namespace exposes operand-level differentiable primitives: affine transformation, NCHW convolution, elementwise absolute/exponential/logarithm operations, mean and last-axis reductions, explicit normalization state, random masking, and safe first- and second-moment parameter updates. These are infrastructure for ordinary source packages rather than layer or optimizer types.

High-level deep-learning APIs are provided by the official `dnn` package through `import dnn`. It defines layers, activations, losses, and optimizers without compiler recognition of the package name.

### Neural state persistence

`neural.save` and `neural.load` use the typed `.quistate` format. The supplied class object graph determines what is serialized. The format records exact nominal roots, structural field paths and types, tensor element type and shape, version, and checksum. Loading validates the complete payload before mutating existing storage and preserves Parameter identity.

### Package management

Unquoted non-standard imports resolve installed source packages from configured
`QUIDRA_PACKAGE_PATH` roots and then the default per-user store
`~/.quidra/packages/<name>/main.qui`.

Released packages contain `quidra.package`, including an exact package version
and a `requires.quidra` range. `quidra install PACKAGE` examines only exact
stable release tags named `vMAJOR.MINOR.PATCH`; branches are never install
identities. With no version specified, the newest released tag compatible with
the running Quidra compiler is selected. `quidra install PACKAGE@X.Y.Z`
requires that exact release and rejects it when its Quidra compatibility range
does not match. Remote package installation uses Git but never executes package
install scripts.

A local source directory can be installed with `quidra install DIR`; legacy
local packages without a manifest remain usable for development. Existing local
installs require explicit `--force` replacement. `quidra remove`,
`quidra list`, and `quidra package-path` manage the default store.

`quidra lock FILE.qui` resolves the direct and transitive installed-package
import graph and writes sorted `quidra-lock-v2` entries containing package
name, package version (or `-` for an unversioned local package), and a SHA-256
over the complete regular-file tree excluding `.git` metadata. When the
lockfile exists, normal compilation requires the same package graph, declared
version, and content hash. `--check` verifies the current resolution without
rewriting the lockfile.

The full package/release metadata contract is documented in
`docs/packages.md`.

### C FFI

A deliberately narrow C ABI boundary is available for functions with scalar/`void` results and optional borrowed text/binary inputs:

```quidra
extern int c_abs(int value) = "llabs"
print(c_abs(-42))
```

The declaration is top-level only and binds a Quidra function name to an explicit C symbol. Results are restricted to `void` or ABI-stable by-value scalars: all fixed-width signed/unsigned integer types, `int`/`int64`, `float32`, `float`/`float64`, and `bool`. Scalar and bool parameters are also by value. Capture-free Quidra function values may cross as explicit C callback pointers when the `fn` signature uses only `int32`, `uint32`, `int`/`int64`, `uint64`, `float32`, and `float`/`float64` value parameters and the same scalar set or `void` as its result. The narrower callback scalar set deliberately excludes 8/16-bit integers and `bool` so callback ABI extension rules cannot become platform-dependent hidden behavior. A callback is only a code pointer: it has no capture environment or bound receiver and therefore no hidden allocation or captured-object lifetime. Managed buffers cross the boundary only through explicit call-scoped storage borrows: `const string &name` is a read-only UTF-8 span, `const bin &name` is a read-only byte span, and `bin &name` is a mutable byte span; all are passed as `&storage` at the call. Mutable `string &` is forbidden so foreign code cannot violate Quidra's UTF-8 invariant. Each borrowed parameter expands at the C ABI boundary to two adjacent C parameters: a non-null data pointer followed by a `uint64` byte length. For `string`, the bytes are the existing validated UTF-8 representation; for `bin`, only the payload after Quidra's private length header is exposed, and only when its bit length is byte-aligned. The adjacent length is the payload byte count, so foreign code cannot resize the Quidra value or access its ownership metadata. No encoding conversion is performed and the foreign contract does not depend on NUL termination. A `const` borrow may not be mutated; a mutable `bin &` borrow may mutate only bytes inside the supplied length. Neither form may retain the pointer after the call. Quidra `bin` has independent value storage, so ordinary value copies remain independent even when one copy is later passed mutably to C. This deliberately means a one-pointer C-string API such as `puts(const char*)` is not directly compatible with a Quidra `string` parameter; use a small C wrapper with an explicit pointer+length signature instead. LLVM marks borrowed pointers `nocapture nonnull`; read-only `string`/`bin` borrows additionally carry `readonly`. Integer ABI extension contracts are explicit: signed 8/16-bit values use `signext`, unsigned 8/16-bit values use `zeroext`, and `bool` uses `zeroext`, on both declarations and call sites. By-value/const-value managed buffers, scalar references, default arguments, generics, managed-value results, arrays, tensors, neural values, unions, and classes remain rejected. Foreign failure is never inferred from `errno`, a null pointer, or ownership convention: expose an explicit scalar status/result and handle it in Quidra. C symbols must be ordinary C identifiers. The generated entrypoint `main`, the compiler-owned `n_*` function-mangling namespace, the implementation-owned `quidra_*` / `__quidra_*` symbol namespaces, and symbols already owned by the generated runtime prelude cannot be rebound through `extern`; use a distinct C wrapper symbol instead. A C symbol may be bound by only one source `extern` declaration per compilation; duplicate aliases are rejected during checking rather than producing conflicting LLVM declarations. External calls are treated as potentially effectful by the REPL, so accumulated-source replay never silently re-executes them. The core declaration does not load libraries or run foreign initialization code. Foreign object, archive, shared-library, or import-library files are linked only when explicitly named with repeatable `--link FILE` options on `quidra build`, `quidra run`, or `quidra debug`; each path must name an existing regular file. There is no implicit package linker script, library search, or build-time network access.
