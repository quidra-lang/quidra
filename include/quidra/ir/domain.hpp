#pragma once

// The one classification of typed-IR instructions by domain.
//
// Every Instruction alternative belongs to exactly one domain, declared once
// by its InstructionTraits specialization (in quidra/ir/<domain>.hpp).
// Whatever is split by domain (IR headers, LLVM backend emitters, lowering
// units) takes the division from this list, so that one word names one concept
// in every layer. An instruction whose purpose is converting a value to
// another type is a conversion (parse and format included); device
// synchronization belongs to tensor; user assertions and compiler guards to
// check.
//
// Owns: the Domain enumeration and the primary InstructionTraits template.
// Declarations only: the IR data model is unchanged.

#include <cstddef>

namespace quidra::ir {

// The enumerators are lower case, unlike the PascalCase enumerators elsewhere
// in the repository, on purpose: each is also the file name of its domain in
// every layer (quidra/ir/control_flow.hpp, control_flow_lowering.cpp), so
// the code and the files spell the domain the same way.
enum class Domain {
    debug,        // source locations for runtime provenance and debug info
    constant,     // literal values
    numeric,      // scalar arithmetic, comparison and exact-number atoms
    conversion,   // numeric casts, parsing, formatting, to-string
    memory,       // locals, references, addresses, loads and stores
    lifetime,     // clone, retain, release
    aggregate,    // arrays, classes, variants
    bin,          // byte buffers
    text,         // strings
    tensor,       // tensor values, views, arithmetic, device synchronization
    autograd,     // tracking, backward, gradients, autograd targets
    console,      // print, input, flush
    file,         // files and file handles
    system,       // command line, environment, clock, processes
    json,         // JSON values
    http,         // HTTP requests
    concurrency,  // tasks and atomic counters
    random,       // random generators
    call,         // function references and calls
    check,        // shape, extent and range guards; test assertions
    control_flow, // block terminators
    repl,         // REPL display and replay
};

// The number of domains; repl is the last enumerator. instruction.hpp
// proves that every instruction's domain is below this count and that every
// domain below it classifies at least one instruction, so an instruction
// classified into an enumerator added after repl without updating this line,
// or an enumerator inserted before repl that no instruction uses, does not
// compile.
inline constexpr std::size_t domain_count = static_cast<std::size_t>(Domain::repl) + 1;

// The traits of an instruction type. The primary template marks a type that no
// specialization classifies; instruction.hpp proves that none of the
// Instruction alternatives is such a type.
template <class T>
struct InstructionTraits {
    static constexpr bool classified = false;
};

// Base of every specialization: `domain`, and `is_terminator`, which holds
// exactly for control_flow.
template <Domain D>
struct InDomain {
    static constexpr bool classified = true;
    static constexpr Domain domain = D;
    static constexpr bool is_terminator = D == Domain::control_flow;
};

} // namespace quidra::ir
