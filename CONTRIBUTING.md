# Contributing to Quidra

Thanks for your interest in Quidra! This is an early-stage, MIT-licensed open-source programming language. **Contributors are welcome**, whether you are experienced with compiler internals or new to language development. Participation is voluntary; this is not a paid job posting.

Quidra is built around **Maximum Meaning Per Token (MMPT)**: express intent compactly without concealing distinctions that affect correctness. Read the [project overview](README.md), [language semantics](docs/spec/language.md), and [grammar](docs/spec/grammar.ebnf) before proposing a language change.

## Ways to contribute

| Area | Repositories |
| --- | --- |
| C++20, compiler, LLVM, JIT, diagnostics, runtime | [Core](https://github.com/quidra-lang/quidra) |
| Test coverage, CI, portability, debugging | Core and all first-party repositories |
| LSP, editor support, REPL, browser tooling | [Core](https://github.com/quidra-lang/quidra) and [Playground](https://github.com/quidra-lang/playground) |
| Numerical computing and machine learning | [Math](https://github.com/quidra-lang/math), [NN](https://github.com/quidra-lang/nn), [DNN](https://github.com/quidra-lang/dnn) |
| Collections, text parsing, and binary encodings | [Collections](https://github.com/quidra-lang/collections), [Text](https://github.com/quidra-lang/text), [Encoding](https://github.com/quidra-lang/encoding) |
| Image and video processing | [Vision](https://github.com/quidra-lang/vision), [Video](https://github.com/quidra-lang/video) |
| Documentation, examples and tutorials | All repositories and [Website](https://github.com/quidra-lang/website) |

Small improvements are important. Documentation corrections, isolated regression tests, reproducible bug reports, clearer compiler diagnostics, and cross-platform verification are welcome.

## Getting started

1. Try the [Playground](https://quidra-lang.github.io/playground/).
2. Browse [open issues](https://github.com/quidra-lang/quidra/issues), including [good first issues](https://github.com/quidra-lang/quidra/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22).
3. Comment on a relevant issue before spending significant time on a change. If your idea is not listed, open an issue describing the problem and expected behavior.
4. Read [the canonical development workflow](docs/development.md). **`develop` is the integration branch for ongoing work; `main` represents releases.** Contributors without write access can submit a pull request from their fork **targeting `develop`**.
5. Keep your changes focused and provide tests or example programs when practical.

For significant syntax, type-system, or semantic changes, propose and discuss the intended contract in an issue before implementing it. Quidra is pre-1.0; language behavior can evolve.

## Build and test Core

Follow the [README build requirements](README.md#build): CMake 3.20+, C++20, LLVM/Clang 15+, libcurl development files, Python 3, and Bash for the Unix suite.

Linux / macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/quidra run examples/hello.qui
```

Windows PowerShell (with the prerequisites noted in the README):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
.\build\Release\quidra.exe run examples\hello.qui
```

Follow [`.clang-format`](.clang-format) for C++ code. Quidra source examples can be checked using `quidra check FILE.qui` and formatted using `quidra fmt FILE.qui`.

## Pull request checklist

- Explain the problem, change, and any effect on users or language semantics.
- Add or update a regression test when the change affects behavior.
- Specify the OS, compiler/toolchain, and commands used for verification. If a test was not run, note that explicitly.
- Target **`develop`**, not `main`. Do not force-push shared development branches.
- Check relevant CI results, and respond to review feedback.
- AI-assisted contributions are welcome, but the contributor must understand, verify, and take responsibility for the proposed change.

**Not ready to write code?** Report something confusing, try a small example in the Playground, or help others reproduce a bug. Those are useful contributions too.

The project is also looking for ongoing collaborators. See the [issues list](https://github.com/quidra-lang/quidra/issues) for open opportunities, or introduce your interests in a new issue.
