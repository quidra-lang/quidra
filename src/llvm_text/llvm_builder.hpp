#pragma once

// LlvmBuilder: LLVM instructions written as textual IR into a function's
// text (FunctionText), one operation per line: every operation writes
// exactly one "  ...\n" instruction line, and block writes one "label:\n"
// line. The builder offers the general operations only (arithmetic,
// comparison, conversion, select, getelementptr, load, store, alloca, phi,
// extractvalue, call, branch, switch, return, unreachable, and the debug
// declaration of a variable's storage); what a sequence of them means
// belongs to the code that writes it.
//
// The builder names nothing: every result name is passed in, allocated by
// the caller (the order of name allocation is part of the output). The
// spelling choices of the text are explicit arguments without defaults:
// Align (", align N" or nothing) and Inbounds. Integers (operands, indices,
// counts) are written in decimal (append_decimal), the digits << writes.
//
// LlvmCall: a call whose arguments are known one at a time (a loop over
// parameters), with attributes on the result and on each argument, or no
// result.
//
// LlvmFunction: a function's signature, which a definition opens and a
// declaration writes; the builder writes the lines that open and close a
// definition, and the blocks and instructions between them.

#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_callee.hpp"
#include "llvm_text/llvm_type.hpp"
#include "llvm_text/llvm_value.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::llvm_text {

enum class BinaryOp : std::uint8_t {
    add, sub, mul, sdiv, udiv, srem, urem, shl, lshr, ashr, and_, or_, xor_, fadd, fsub, fmul, fdiv, frem
};
// The no-wrap flag of an integer add, sub or mul.
enum class NoWrap : std::uint8_t { nsw, nuw };
enum class IntPredicate : std::uint8_t { eq, ne, ugt, uge, ult, ule, sgt, sge, slt, sle };
enum class FloatPredicate : std::uint8_t {
    oeq, ogt, oge, olt, ole, one, ord, ueq, ugt, uge, ult, ule, une, uno
};
enum class CastOp : std::uint8_t {
    trunc, zext, sext, fptrunc, fpext, fptoui, fptosi, uitofp, sitofp, ptrtoint, inttoptr, bitcast
};
// The alignment of a load, store or alloca: none writes no ", align N".
enum class Align : std::uint32_t { none = 0, one = 1, two = 2, four = 4, eight = 8, sixteen = 16 };
enum class Inbounds : bool { no, yes };

// One incoming value of a phi: the value and the block it comes from.
struct LlvmIncoming {
    LlvmOperand value;
    LlvmBlock block;
};

// One case of a switch: the i64 value and the block it jumps to.
struct LlvmCase {
    LlvmOperand value;
    LlvmBlock block;
};

class LlvmCall {
public:
    LlvmCall(LlvmType type, LlvmOperand callee) : type_(type), callee_(callee) {}

    // The name of the call's result; without one, the call has no result.
    LlvmCall& result(std::string_view name) {
        result_ = name;
        return *this;
    }
    // Attributes of the result, written before its type (signext, ...).
    LlvmCall& result_attributes(std::string_view attributes) {
        result_attributes_ = attributes;
        return *this;
    }
    LlvmCall& argument(LlvmValue value) {
        arguments_.push_back(Argument{value.type, {}, value.operand});
        return *this;
    }
    // An argument with attributes, written after its type (nocapture, ...).
    LlvmCall& argument(LlvmType type, std::string_view attributes, LlvmOperand operand) {
        arguments_.push_back(Argument{type, attributes, operand});
        return *this;
    }

private:
    friend class LlvmBuilder;

    struct Argument {
        LlvmType type;
        std::string_view attributes;
        LlvmOperand operand;
    };

    LlvmType type_;
    LlvmOperand callee_;
    std::string_view result_;
    std::string_view result_attributes_;
    std::vector<Argument> arguments_;
};

// LlvmFunction: the signature of a function as its definition or its
// declaration writes it: "define T @name(T %a, ...) attributes {" and
// "declare [attributes ]T @name(T, ...)". It owns its texts, so it can be
// built from temporaries.
class LlvmFunction {
public:
    LlvmFunction(LlvmType result, std::string symbol) : result_(result), symbol_(std::move(symbol)) {}

    // Attributes of the result, written before its type (signext, ...).
    LlvmFunction& result_attributes(std::string_view attributes) {
        result_attributes_ = attributes;
        return *this;
    }
    // A parameter: its type, the attributes written after the type
    // (" nocapture nonnull", ...), and its name, empty in a declaration.
    LlvmFunction& parameter(LlvmType type, std::string_view attributes, std::string name) {
        parameters_.push_back(Parameter{type, std::string(attributes), std::move(name)});
        return *this;
    }
    // A function attribute written after the parameter list (alwaysinline).
    LlvmFunction& attribute(std::string_view name) {
        attributes_.push_back(std::string(name));
        return *this;
    }
    // The debug attachment of a definition: " !dbg !<subprogram>".
    LlvmFunction& debug_subprogram(std::size_t subprogram) {
        debug_subprogram_ = subprogram;
        has_debug_subprogram_ = true;
        return *this;
    }

private:
    friend class LlvmBuilder;

    struct Parameter {
        LlvmType type;
        std::string attributes;
        std::string name;
    };

    LlvmType result_;
    std::string symbol_;
    std::string result_attributes_;
    std::vector<Parameter> parameters_;
    std::vector<std::string> attributes_;
    std::size_t debug_subprogram_{};
    bool has_debug_subprogram_{};
};

// LlvmCallObserver: told of every call the builder writes (debug
// declarations excepted), before its line and after it, with the callee's
// symbol without the '@', or an empty symbol for a call through a value. It
// may write instructions through the same builder from either callback; the
// calls it writes are reported to it in turn.
class LlvmCallObserver {
public:
    virtual void before_call(std::string_view symbol) = 0;
    virtual void after_call(std::string_view symbol) = 0;

protected:
    ~LlvmCallObserver() = default;
};

class LlvmBuilder {
public:
    explicit LlvmBuilder(FunctionText& text) : out_(text) {}

    // The observer of the calls written from now on (null: none).
    void observe_calls(LlvmCallObserver* observer) { observer_ = observer; }

    // label:
    void block(LlvmBlock block);

    // result = op [flag] type lhs, rhs
    void binary(std::string_view result, BinaryOp op, LlvmType type, LlvmOperand lhs, LlvmOperand rhs);
    void binary(std::string_view result, BinaryOp op, NoWrap flag, LlvmType type, LlvmOperand lhs,
                LlvmOperand rhs);
    // result = fneg type operand
    void fneg(std::string_view result, LlvmType type, LlvmOperand operand);
    // result = icmp predicate type lhs, rhs
    void icmp(std::string_view result, IntPredicate predicate, LlvmType type, LlvmOperand lhs,
              LlvmOperand rhs);
    // result = fcmp predicate type lhs, rhs
    void fcmp(std::string_view result, FloatPredicate predicate, LlvmType type, LlvmOperand lhs,
              LlvmOperand rhs);
    // result = op type value to target
    void cast(std::string_view result, CastOp op, LlvmValue value, LlvmType target);
    // result = select i1 condition, type a, type b
    void select(std::string_view result, LlvmOperand condition, LlvmValue if_true, LlvmValue if_false);
    // result = getelementptr [inbounds] element, ptr base, index...
    void getelementptr(std::string_view result, Inbounds inbounds, LlvmType element, LlvmOperand base,
                       std::initializer_list<LlvmValue> indices);
    // result = load type, ptr address[, align N]
    void load(std::string_view result, LlvmType type, LlvmOperand address, Align align);
    // store type value, ptr address[, align N]
    void store(LlvmValue value, LlvmOperand address, Align align);
    // result = alloca type[, align N]
    void alloca_slot(std::string_view result, LlvmType type, Align align);
    // result = phi type [ value, %block ], ...
    void phi(std::string_view result, LlvmType type, std::initializer_list<LlvmIncoming> incoming);
    // result = extractvalue aggregate value, index
    void extractvalue(std::string_view result, LlvmType aggregate, LlvmOperand value, unsigned index);
    // [result = ]call type callee(arguments)
    void call(std::string_view result, LlvmType type, LlvmOperand callee,
              std::initializer_list<LlvmValue> arguments);
    void call(LlvmType type, LlvmOperand callee, std::initializer_list<LlvmValue> arguments);
    // [result = ]call T @symbol(arguments), T the callee's result type; a
    // variadic callee is called with its type, "T (P, ...) @symbol". The
    // arguments must have the callee's parameter types (and may continue
    // after them for a variadic callee), and a call with a result name a
    // callee that returns a value; otherwise std::logic_error, an internal
    // error that abandons the text being written. The arguments are checked
    // as they are written.
    void call(std::string_view result, const LlvmCallee& callee,
              std::initializer_list<LlvmValue> arguments);
    void call(const LlvmCallee& callee, std::initializer_list<LlvmValue> arguments);
    void call(const LlvmCall& call);
    // call void @llvm.dbg.declare(metadata T storage, metadata variable,
    // metadata expression): the arguments of the debug declaration are
    // metadata, the storage a typed value wrapped as metadata.
    void debug_declare(LlvmValue storage, LlvmOperand variable, LlvmOperand expression);
    // br label %target
    void br(LlvmBlock target);
    // br i1 condition, label %if_true, label %if_false
    void br(LlvmOperand condition, LlvmBlock if_true, LlvmBlock if_false);
    // switch type value, label %otherwise [ i64 value, label %block ... ]
    void switch_on(LlvmValue value, LlvmBlock otherwise, std::span<const LlvmCase> cases);
    // ret type value
    void ret(LlvmValue value);
    // ret void
    void ret_void();
    // unreachable
    void unreachable();

    // define T @name(params) attributes {
    void define(const LlvmFunction& function);
    // }, and a blank line after the definition
    void end_function();
    // declare [attributes ]T @name(params), and a blank line
    void declare(const LlvmFunction& function);

private:
    void put(std::string_view text) {
        out_.append(text);
    }
    void put_type(LlvmType type) { type.print(out_); }
    void put_operand(const LlvmOperand& operand) { operand.print(out_); }
    void put_value(const LlvmValue& value) {
        put_type(value.type);
        put(" ");
        put_operand(value.operand);
    }
    // "  result = "
    void assign(std::string_view result);
    // "T @symbol(arguments)\n" of a call through a callee; throws
    // std::logic_error (reject_arguments) at the first argument that does not
    // match the callee's parameters.
    void put_callee_call(const LlvmCallee& callee, std::initializer_list<LlvmValue> arguments);
    [[noreturn]] static void reject_arguments(const LlvmCallee& callee);
    // [attributes ]T @name(params): with the parameter names in a definition.
    void put_signature(const LlvmFunction& function, bool named);
    void put_align(Align align);

    // Calls `write` between the observer's two callbacks.
    template <class Write>
    void observed_call(std::string_view symbol, Write&& write) {
        if (observer_) observer_->before_call(symbol);
        write();
        if (observer_) observer_->after_call(symbol);
    }

    FunctionText& out_;
    LlvmCallObserver* observer_{};
};

} // namespace quidra::llvm_text
