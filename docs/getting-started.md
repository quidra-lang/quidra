# Getting started with Quidra

This is a short first tour for someone who has not used Quidra before. It
targets the `develop` branch and uses only syntax already present in the
repository's examples and README. It does not replace the [language
specification](spec/language.md) or the [full build
requirements](../README.md#build).

## 1. Try the Playground first

The fastest way to see Quidra is the [official
Playground](https://quidra-lang.github.io/playground/). It runs the same Core
language services in the browser, so you can check and format a small program
without installing a compiler.

For local compiler development, the repository also documents a loopback UI in
the [Playground section of the README](../README.md#playground). The official
Playground is a separate repository and deployment.

## 2. Build or install Core

Use the platform-specific prerequisites and build commands in the
[README Build section](../README.md#build). The [development
guide](development.md) describes the repository workflow and branch roles.

After a successful local build, use the executable produced for your platform:

```bash
QUIDRA=./build/quidra
```

On Windows PowerShell:

```powershell
$quidra = ".\build\Release\quidra.exe"
```

## 3. Write a first program

Create a file named `first.qui` with this small program:

```quidra
void increment(int &value)
    value += 1

count := 7
increment(&count)
print("count = ")
println(count)
```

Omit the type by default when the initializer determines it, as in `label := "ready"`. Numeric literals default to `int`, exact `real`, or exact `com`, so `count := 7` infers `int`. In `int32 count := 7`, the annotation supplies a narrower context; `nat` requires context.

The example also shows how values and references are passed:

- `int count := 7` creates a value.
- `int &value` makes writable access to existing storage explicit in the
  function signature.
- `increment(&count)` makes that access explicit at the call site.
- Ordinary `=` does not silently create an alias.

These are existing Core forms used by the project's language documentation;
this tutorial does not introduce new syntax.

You can also create empty, typed collections without a constructor call:

```quidra
map<string, int> counts
.counts.set("apple", 2)

distinct<string> tags
.tags.add("apple")
.tags.add("apple")

println(counts.has("apple"))
println(tags.size())
```

`map<K, V>` associates keys with values; `distinct<T>` keeps unique elements
in insertion order. The `.counts.set(...)` method is separate from a
user-defined function named `set`, which is no longer reserved. For this
`develop`-branch syntax, see [the current collection specification](spec/language.md#map).

## 4. Check, format, and run it

On Linux or macOS, from the directory containing `first.qui`:

```bash
$QUIDRA check first.qui
$QUIDRA fmt first.qui
$QUIDRA run first.qui
```

On Windows PowerShell:

```powershell
& $quidra check first.qui
& $quidra fmt first.qui
& $quidra run first.qui
```

`check` validates the source without running it. `fmt` writes the canonical
format back to the file. `run` builds and executes the program using the
native toolchain described in the README.

If you want to try a repository-provided program instead, the same commands
work with `examples/hello.qui`:

```bash
$QUIDRA check examples/hello.qui
$QUIDRA fmt examples/hello.qui
$QUIDRA run examples/hello.qui
```

## 5. If the result differs

First check that the source and compiler came from the same `develop` checkout,
then record:

1. operating system and compiler/toolchain versions;
2. the Quidra commit or branch;
3. the exact source file and command;
4. expected output and actual output;
5. the complete diagnostic, including stderr.

Search [existing issues](https://github.com/quidra-lang/quidra/issues) before
opening a new one. If no existing issue covers the behaviour, open a report
with those details and include a minimal program that reproduces it. For
contribution workflow questions, see [CONTRIBUTING.md](../CONTRIBUTING.md).
