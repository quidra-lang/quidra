# Reference Pack — Condition I1

This is the only description of the language you will receive. Everything you need to
write the requested program is below. Twelve grammar words of this language have been
replaced by invented words; the invented words are the real spelling as far as you are
concerned, plus the table in section 10 defines every one of them.

## 1. Program shape

A source file is a single compilation unit. No unit-naming line is written: the
declarations begin at the top of the file.

Execution begins at the function named `bikoli`. It takes no parameters. Its result
designation is `! bogizu`, which reads "yields no value, but may fail". When control
falls off the end of its body the process ends with status 0.

```
mukete delosu bikoli () ! bogizu {
    ...statements...
}
```

A block is delimited by `{` with `}`. Every statement ends with `;`, including the last
one in a block. Whitespace, line ends included, carries no meaning outside a literal.

**Three build-time strictnesses that are easy to trip over.** They are build failures,
never warnings: a binding declared but never read afterwards; a binding introduced with
`rikuvu` that is never reassigned (use `viduzu` instead); a call that may fail whose
result is simply dropped (prefix it with `mozako`, section 6).

## 2. Lexical rules

* **Identifiers** are letters, digits, `_`, never beginning with a digit; upper-case
  differs from lower-case. You choose your own names freely; they are not substituted.
* **Whole-number literals** are ordinary decimal: `7`, `48271`, `2147483647`.
* **Text literals** are enclosed in double quotes: `"SUM "`, `""`. A backslash escapes
  the next character; `\n` is a line end. A literal may not span a line end.
* **Comments** run from `//` to the end of the line; there is no bracketed shape.
* A name with a leading `@`, such as `@import`, is a built-in operation of the toolchain
  rather than a word you declare. Such names are never substituted.
* **Embedding a value inside a text literal:** `{d}` marks a place in the literal where
  one whole-number argument is written in decimal; the arguments follow the literal,
  wrapped as `.{ a, b, c }` (section 9).

## 3. Declarations

A named binding you intend to reassign later is introduced with `rikuvu`, in the order
*word, name, `:`, kind, `=`, initial value, `;`*. A binding you never reassign uses
`viduzu` in exactly the same order. The `:` clause may be dropped whenever the kind is
evident from the initial value. Reassignment afterwards is a plain `=` statement.

```
rikuvu total: sebezo = 0;
viduzu limit = 1000;
total = total + 1;
```

A function is introduced with `delosu`, in the order *word, name, parenthesised parameter
list, result designation, body*. `mukete` before `delosu` makes the function reachable
beyond its own unit; the entry point needs it. The only function this task needs is that
entry point, with an empty parameter list.

## 4. Kinds used by this task

| Kind | Written | What it holds |
|---|---|---|
| whole number | `sebezo` | an unsigned 64-bit whole number, exactly, with no rounding |
| byte | `renibu` | an unsigned 8-bit whole number, used only as scratch storage |
| no value | `bogizu` | the result designation of a function yielding nothing |

A fixed-length run of `N` items of kind `K` is written `[N] K`; item `i` is read as well
as written through `b[i]`, counting from 0. A run whose items are all written before any
is read may be started with `beluba`, the explicitly uninitialised value:

```
rikuvu five: [5] sebezo = beluba ;
```

`sebezo` arithmetic is exact at every magnitude this task produces, never negative, so
`%` on two of them behaves as ordinary remainder.

## 5. Expressions, operators, precedence

| Operator | Meaning |
|---|---|
| `b[i]` | item `i` of a fixed-length run |
| `x.y` | the member `y` of the value `x` |
| `&x` | a reference to the storage of `x` |
| `*` `%` | product, remainder after division |
| `+` `-` | sum, difference |
| `<` `>` `<=` `>=` | strictly less, strictly greater, not greater, not less |
| `==` `!=` | equal, unequal |
| `=` | assignment; a statement, never part of an expression |

Precedence runs tightest-first down that table: `b[i]` `x.y` bind tightest, then `&x`,
then `*` `%`, then `+` `-`, then the comparisons, with `=` loosest. Parentheses
`(` `)` override it, so `a + b * c` is `a + (b * c)`, whereas `(a * b) % c` needs its
parentheses written.

## 6. Control flow

**Conditional branch.** `ganimu`, then a parenthesised condition, then a block.
**Conditional iteration.** `zavulu`, then a parenthesised condition, then — optionally —
`:` with a parenthesised step statement, then a block; the condition is checked before
each pass, the step statement runs after each pass. The control binding must already have
been introduced with `rikuvu` before the loop.

```
ganimu (n > limit) {
    limit = n;
}
zavulu (i < 50) : (i = i + 1) {
    ...body...
}
```

**Failure propagation.** `mozako` goes immediately before a call that may fail: on
success its value is used, on failure the enclosing function stops at once, handing the
failure to its own caller. A function whose body uses `mozako` must carry `!` in its
result designation, exactly as the entry point does.

## 7. Standard vocabulary

These names are **not** substituted; they are written exactly as shown.

| Written | Parameters | Result |
|---|---|---|
| `@import("std")` | the namespace name as a text literal | the standard namespace value |
| `std.Io.Threaded` / `.init_single_threaded` | — | the kind, then the starting value, of an output-machinery instance |
| `h.io()` | called on an instance `h` | the handle every output call needs |
| `std.Io.File.stdout()` | — | the standard-output stream |
| `s.writer(handle, buf)` | the handle, plus a `[N] renibu` run used as scratch | a buffered writer |
| `w.interface.print(lit, args)` | a text literal plus `.{ ... }` arguments | writes them; may fail |
| `w.interface.flush()` | — | pushes buffered bytes out; may fail |

## 8. Modules

The whole standard namespace is reached through the built-in operation `@import`, whose
one argument is the namespace name as a text literal. Bind the result once, at the top:

```
viduzu std = @import("std");
```

A name inside it is written *value*`.`*name*, as in section 7. Nothing further is needed.

## 9. Output

**This is the current spelling of the output path in this toolchain. Shorter, older
spellings are widely reproduced elsewhere; this toolchain no longer accepts them, so use
exactly the shape below.** Writing takes four steps: make an output-machinery instance,
take its handle, wrap the standard-output stream in a buffered writer, then write.
Buffered bytes stay invisible until `flush` runs, so the program's last output statement
must be that `flush` call.

```
rikuvu tio: std.Io.Threaded = .init_single_threaded;
viduzu io = tio.io();
rikuvu wbuf: [512] renibu = beluba ;
rikuvu out = std.Io.File.stdout().writer(io, &wbuf);

mozako out.interface.print("TOTAL {d}\n", .{total});
mozako out.interface.flush();
```

`print` writes no line end of its own: put `\n` in the literal where you want one.

## 10. Substituted words (condition I1)

Every word in this table replaces one grammar word of the underlying language. These are
the only substituted words; everything beyond them is standard vocabulary (section 7),
otherwise a name you chose yourself.

| Word | Position | What it does |
|---|---|---|
| `mukete` | leftmost in a declaration | makes the declaration reachable beyond its unit |
| `delosu` | precedes a function name | introduces a function definition |
| `bikoli` | the name of the entry function | the fixed name of the program entry point |
| `viduzu` | precedes a binding name | introduces a binding that is never reassigned |
| `rikuvu` | precedes a binding name | introduces a binding that may be reassigned |
| `sebezo` | after `:`, before `=`; also inside `[N] K` | the unsigned 64-bit whole-number kind |
| `renibu` | after `:`, before `=`; also inside `[N] K` | the unsigned 8-bit byte kind |
| `bogizu` | after `!` in a result designation | the no-value result designation |
| `beluba` | after `=` in a binding | the explicitly uninitialised starting value |
| `ganimu` | precedes a parenthesised condition plus a block | introduces a conditional branch |
| `zavulu` | precedes a parenthesised condition plus a block | introduces conditional iteration |
| `mozako` | precedes a call that may fail | hands that call's failure to the caller |

## 11. Worked example

A complete program, using every construct described above, nothing beyond that. It walks
a whole-number sequence, accumulates a total, tracks the greatest item, counts the items
divisible by two, keeps the leading five items, then writes four lines.

```
viduzu std = @import("std");

mukete delosu bikoli () ! bogizu {
    rikuvu tio: std.Io.Threaded = .init_single_threaded;
    viduzu io = tio.io();
    rikuvu wbuf: [512] renibu = beluba ;
    rikuvu out = std.Io.File.stdout().writer(io, &wbuf);

    rikuvu state: sebezo = 7;
    rikuvu sum: sebezo = 0;
    rikuvu largest: sebezo = 0;
    rikuvu evens: sebezo = 0;
    rikuvu first: [5] sebezo = beluba ;

    rikuvu i: sebezo = 0;
    zavulu (i < 50) : (i = i + 1) {
        state = (state * 48271) % 2147483647;
        viduzu term: sebezo = state % 1000;
        sum = sum + term;
        ganimu (term > largest) {
            largest = term;
        }
        ganimu (term % 2 == 0) {
            evens = evens + 1;
        }
        ganimu (i < 5) {
            first[i] = term;
        }
    }

    mozako out.interface.print("SUM {d}\n", .{sum});
    mozako out.interface.print("MAX {d}\n", .{largest});
    mozako out.interface.print("EVENS {d}\n", .{evens});
    mozako out.interface.print("JOINED {d}-{d}-{d}-{d}-{d}\n", .{ first[0], first[1], first[2], first[3], first[4] });
    mozako out.interface.flush();
}
```

It writes exactly these four lines, then ends with status 0:

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

## 12. Size of this pack

Recorded by measurement, never by estimate: 245 lines, 10171 characters.
