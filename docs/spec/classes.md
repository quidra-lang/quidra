# Classes and initialization

A class definition always has an argument list. Its order is the type name,
optional instance name, argument list, and optional generation error contract.

```quidra
class Point point(real64 x, real64 y, local real64 scale := 2.0)
    x *= scale
    y *= scale
    length_squared := x * x + y * y
    real64 sum()
        return x + y
    Point copy()
        return point
p := Point(3.0, 4.0)

class Greeting(text := "hello")
    suffix := "!"

    string message()
        return text + suffix

greeting := Greeting()
print.line(greeting.message())
```

Ordinary header arguments declare members initialized from the supplied values.
A `local` argument is available only to initialization. Header members are
initialized first. Executable statements and member initialization expressions
then run once per instance, in written order. Method declarations define
operations whose bodies run only when called. `construct` is an ordinary name.
An empty class uses `class Empty()`.

A bare class declaration such as `Point p` declares an uninitialized binding
without creating a usable instance. Initialize or assign the whole binding with
`Point(...)` before any member read, member write, method call or instance borrow.
Copying an initialized instance does not rerun generation. Standard `map<K, V>` and `distinct<T>` bindings retain their empty-storage
initialization contract.

A normal declaration directly in the class body adds a member. A field
initializer or header default uses `:=`. Omit the type when the expression
determines it. The `Greeting` header infers `string` for `text`, and the field
initializer infers `string` for `suffix`. A required parameter or a field
without an initializer needs an explicit type. A `local` declaration keeps
a temporary in the initialization block. Declarations inside
an `if`, `for` or other control block are local with or without that qualifier.
Names are visible from their declaration through their block and inner control
blocks. Locals do not cross a function or class definition boundary. An
initialization temporary is therefore unavailable to a method.

Members use their direct names inside the class. The optional instance name is
fixed to the current instance: it is neither an extra member nor a copy, and
cannot be rebound or reassigned. Value use copies or borrows according to the
ordinary value rules; `&point` explicitly passes a mutable reference. A function
receiving that reference may change members, but cannot replace the fixed
instance. Members and the instance name cannot be shadowed by another parameter
or local declaration. Another instance's members use an explicit receiver.

```quidra
class Document document(local string path) | error
    file.read stream := try file.read(path)
    text := ""
    while .stream(string &line)
        text += "{line}{NL}"
Document | error result := Document("notes.txt")
```

The error contract belongs to generation: `Document(...)` returns
`Document | error`, while the instance name and the class type remain `Document`.
A generation error releases initialized members and local values without
returning a successful partial instance. Generation does not undo external
writes. The whole instance cannot escape while initialization is in progress,
including through a method that publishes it. Member access remains available
during initialization.

Ordinary member cleanup follows the ownership and lifetime rules of the values
held. Operations whose failure the caller must handle, such as saving or closing
an external resource, are explicit operations. Classes do not add a special
user destructor.

If initialization changes existing state not exposed by a reference argument,
the caller uses the usual explicit mutation call marker, such as `.Document(...)`.
The class header has no separate constructor definition marker; that requirement
is determined from the initialization operations.
