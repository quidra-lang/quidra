#pragma once

// LlvmCallee: a function as a call or a declaration names it: its symbol and
// its LLVM signature (the result type, the parameter types, and whether
// further arguments follow them: a variadic C function such as printf). A
// call written through LlvmBuilder with a callee takes its result type from
// the signature and checks the arguments against it.
//
// llvm_callee<F>(symbol) derives the signature from a C function pointer type
// F, the way the C ABI passes each type: void; bool as i1; an integer or an
// enumeration by its size (i8, i16, i32 or i64, so a type keeps its width
// on every platform that compiles it); float, double; every pointer as ptr;
// a trailing ... as variadic.
// A callee that is not a C function in this process (a helper written in
// LLVM IR, a C library function) is derived from the C type it corresponds
// to.
//
// append_declaration writes a callee's declaration line,
// "declare T @symbol(P, ...)", into a text; a FixedText takes it at compile
// time.

#include "llvm_text/llvm_type.hpp"

#include <array>
#include <span>
#include <string_view>
#include <type_traits>

namespace quidra::llvm_text {

class LlvmCallee {
public:
    constexpr LlvmCallee(std::string_view symbol, LlvmType result,
                         std::span<const LlvmType> parameters, bool variadic = false)
        : symbol_(symbol), result_(result), parameters_(parameters), variadic_(variadic) {}

    // The symbol without the leading '@'.
    constexpr std::string_view symbol() const { return symbol_; }
    constexpr LlvmType result() const { return result_; }
    constexpr std::span<const LlvmType> parameters() const { return parameters_; }
    // Whether arguments of any type may follow the parameters.
    constexpr bool variadic() const { return variadic_; }

private:
    std::string_view symbol_;
    LlvmType result_;
    std::span<const LlvmType> parameters_;
    bool variadic_;
};

template <class T>
consteval LlvmType llvm_type_of() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_void_v<U>) {
        return types::void_type;
    } else if constexpr (std::is_same_v<U, bool>) {
        return types::i1;
    } else if constexpr (std::is_pointer_v<U>) {
        return types::ptr;
    } else if constexpr (std::is_same_v<U, float>) {
        return types::float_type;
    } else if constexpr (std::is_same_v<U, double>) {
        return types::double_type;
    } else {
        static_assert(std::is_integral_v<U> || std::is_enum_v<U>, "no LLVM type for this C type");
        static_assert(sizeof(U) == 1 || sizeof(U) == 2 || sizeof(U) == 4 || sizeof(U) == 8,
                      "no LLVM integer type of this size");
        if constexpr (sizeof(U) == 1) return types::i8;
        else if constexpr (sizeof(U) == 2) return types::i16;
        else if constexpr (sizeof(U) == 4) return types::i32;
        else return types::i64;
    }
}

template <class F>
struct LlvmSignatureOf;

template <class R, class... A>
struct LlvmSignatureOf<R (*)(A...)> {
    static constexpr LlvmType result = llvm_type_of<R>();
    static constexpr std::array<LlvmType, sizeof...(A)> parameters{llvm_type_of<A>()...};
    static constexpr bool variadic = false;
};

template <class R, class... A>
struct LlvmSignatureOf<R (*)(A..., ...)> {
    static constexpr LlvmType result = llvm_type_of<R>();
    static constexpr std::array<LlvmType, sizeof...(A)> parameters{llvm_type_of<A>()...};
    static constexpr bool variadic = true;
};

template <class F>
consteval LlvmCallee llvm_callee(std::string_view symbol) {
    return LlvmCallee(symbol, LlvmSignatureOf<F>::result, LlvmSignatureOf<F>::parameters,
                      LlvmSignatureOf<F>::variadic);
}

// declare T @symbol(P, ...), and the end of the line.
template <AppendableText Text>
constexpr void append_declaration(Text& out, const LlvmCallee& callee) {
    out.append("declare ");
    callee.result().print(out);
    out.append(" @");
    out.append(callee.symbol());
    out.append("(");
    bool first = true;
    for (const auto& parameter : callee.parameters()) {
        if (!first) out.append(", ");
        first = false;
        parameter.print(out);
    }
    if (callee.variadic()) out.append(first ? "..." : ", ...");
    out.append(")\n");
}

} // namespace quidra::llvm_text
