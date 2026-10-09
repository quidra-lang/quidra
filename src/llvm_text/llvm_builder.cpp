#include "llvm_text/llvm_builder.hpp"

#include "llvm_text/appendable_text.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace quidra::llvm_text {

namespace {

// The spellings of each operation enum, in enumerator order.
constexpr std::array<std::string_view, 18> binary_spellings{
    "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "shl", "lshr",
    "ashr", "and", "or", "xor", "fadd", "fsub", "fmul", "fdiv", "frem"};
constexpr std::array<std::string_view, 10> int_predicate_spellings{
    "eq", "ne", "ugt", "uge", "ult", "ule", "sgt", "sge", "slt", "sle"};
constexpr std::array<std::string_view, 14> float_predicate_spellings{
    "oeq", "ogt", "oge", "olt", "ole", "one", "ord", "ueq", "ugt", "uge", "ult", "ule", "une", "uno"};
constexpr std::array<std::string_view, 12> cast_spellings{
    "trunc", "zext", "sext", "fptrunc", "fpext", "fptoui", "fptosi", "uitofp", "sitofp",
    "ptrtoint", "inttoptr", "bitcast"};

std::string_view spelling(BinaryOp op) { return binary_spellings[static_cast<std::size_t>(op)]; }
std::string_view spelling(NoWrap flag) { return flag == NoWrap::nsw ? "nsw" : "nuw"; }
std::string_view spelling(IntPredicate predicate) {
    return int_predicate_spellings[static_cast<std::size_t>(predicate)];
}
std::string_view spelling(FloatPredicate predicate) {
    return float_predicate_spellings[static_cast<std::size_t>(predicate)];
}
std::string_view spelling(CastOp op) { return cast_spellings[static_cast<std::size_t>(op)]; }
// By the bit width of the alignment: none (0), then 1, 2, 4, 8 and 16.
constexpr std::array<std::string_view, 6> align_spellings{
    "", ", align 1", ", align 2", ", align 4", ", align 8", ", align 16"};
std::string_view spelling(Align align) {
    return align_spellings[std::bit_width(static_cast<std::uint32_t>(align))];
}

} // namespace

void LlvmBuilder::assign(std::string_view result) {
    put("  ");
    put(result);
    put(" = ");
}

void LlvmBuilder::put_align(Align align) {
    put(spelling(align));
}

void LlvmBuilder::block(LlvmBlock block) {
    put(block.label);
    put(":\n");
}

void LlvmBuilder::binary(std::string_view result, BinaryOp op, LlvmType type, LlvmOperand lhs,
                         LlvmOperand rhs) {
    assign(result);
    put(spelling(op));
    put(" ");
    put_type(type);
    put(" ");
    put_operand(lhs);
    put(", ");
    put_operand(rhs);
    put("\n");
}

void LlvmBuilder::binary(std::string_view result, BinaryOp op, NoWrap flag, LlvmType type,
                         LlvmOperand lhs, LlvmOperand rhs) {
    assign(result);
    put(spelling(op));
    put(" ");
    put(spelling(flag));
    put(" ");
    put_type(type);
    put(" ");
    put_operand(lhs);
    put(", ");
    put_operand(rhs);
    put("\n");
}

void LlvmBuilder::fneg(std::string_view result, LlvmType type, LlvmOperand operand) {
    assign(result);
    put("fneg ");
    put_type(type);
    put(" ");
    put_operand(operand);
    put("\n");
}

void LlvmBuilder::icmp(std::string_view result, IntPredicate predicate, LlvmType type, LlvmOperand lhs,
                       LlvmOperand rhs) {
    assign(result);
    put("icmp ");
    put(spelling(predicate));
    put(" ");
    put_type(type);
    put(" ");
    put_operand(lhs);
    put(", ");
    put_operand(rhs);
    put("\n");
}

void LlvmBuilder::fcmp(std::string_view result, FloatPredicate predicate, LlvmType type, LlvmOperand lhs,
                       LlvmOperand rhs) {
    assign(result);
    put("fcmp ");
    put(spelling(predicate));
    put(" ");
    put_type(type);
    put(" ");
    put_operand(lhs);
    put(", ");
    put_operand(rhs);
    put("\n");
}

void LlvmBuilder::cast(std::string_view result, CastOp op, LlvmValue value, LlvmType target) {
    assign(result);
    put(spelling(op));
    put(" ");
    put_value(value);
    put(" to ");
    put_type(target);
    put("\n");
}

void LlvmBuilder::select(std::string_view result, LlvmOperand condition, LlvmValue if_true,
                         LlvmValue if_false) {
    assign(result);
    put("select i1 ");
    put_operand(condition);
    put(", ");
    put_value(if_true);
    put(", ");
    put_value(if_false);
    put("\n");
}

void LlvmBuilder::getelementptr(std::string_view result, Inbounds inbounds, LlvmType element,
                                LlvmOperand base, std::initializer_list<LlvmValue> indices) {
    assign(result);
    put(inbounds == Inbounds::yes ? "getelementptr inbounds " : "getelementptr ");
    put_type(element);
    put(", ptr ");
    put_operand(base);
    for (const auto& index : indices) {
        put(", ");
        put_value(index);
    }
    put("\n");
}

void LlvmBuilder::load(std::string_view result, LlvmType type, LlvmOperand address, Align align) {
    assign(result);
    put("load ");
    put_type(type);
    put(", ptr ");
    put_operand(address);
    put_align(align);
    put("\n");
}

void LlvmBuilder::store(LlvmValue value, LlvmOperand address, Align align) {
    put("  store ");
    put_value(value);
    put(", ptr ");
    put_operand(address);
    put_align(align);
    put("\n");
}

void LlvmBuilder::alloca_slot(std::string_view result, LlvmType type, Align align) {
    assign(result);
    put("alloca ");
    put_type(type);
    put_align(align);
    put("\n");
}

void LlvmBuilder::phi(std::string_view result, LlvmType type, std::initializer_list<LlvmIncoming> incoming) {
    assign(result);
    put("phi ");
    put_type(type);
    bool first = true;
    for (const auto& edge : incoming) {
        put(first ? " [ " : ", [ ");
        first = false;
        put_operand(edge.value);
        put(", %");
        put(edge.block.label);
        put(" ]");
    }
    put("\n");
}

void LlvmBuilder::extractvalue(std::string_view result, LlvmType aggregate, LlvmOperand value,
                               unsigned index) {
    assign(result);
    put("extractvalue ");
    put_type(aggregate);
    put(" ");
    put_operand(value);
    put(", ");
    append_decimal(out_, index);
    put("\n");
}

void LlvmBuilder::call(std::string_view result, LlvmType type, LlvmOperand callee,
                       std::initializer_list<LlvmValue> arguments) {
    observed_call(callee.global_symbol(), [&] {
        assign(result);
        put("call ");
        put_type(type);
        put(" ");
        put_operand(callee);
        put("(");
        bool first = true;
        for (const auto& argument : arguments) {
            if (!first) put(", ");
            first = false;
            put_value(argument);
        }
        put(")\n");
    });
}

void LlvmBuilder::call(LlvmType type, LlvmOperand callee, std::initializer_list<LlvmValue> arguments) {
    observed_call(callee.global_symbol(), [&] {
        put("  call ");
        put_type(type);
        put(" ");
        put_operand(callee);
        put("(");
        bool first = true;
        for (const auto& argument : arguments) {
            if (!first) put(", ");
            first = false;
            put_value(argument);
        }
        put(")\n");
    });
}

void LlvmBuilder::call(std::string_view result, const LlvmCallee& callee,
                       std::initializer_list<LlvmValue> arguments) {
    if (callee.result() == types::void_type) {
        throw std::logic_error("call of @" + std::string(callee.symbol()) +
                               ": a function without a result has no result name");
    }
    observed_call(callee.symbol(), [&] {
        assign(result);
        put("call ");
        put_callee_call(callee, arguments);
    });
}

void LlvmBuilder::call(const LlvmCallee& callee, std::initializer_list<LlvmValue> arguments) {
    observed_call(callee.symbol(), [&] {
        put("  call ");
        put_callee_call(callee, arguments);
    });
}

void LlvmBuilder::put_callee_call(const LlvmCallee& callee, std::initializer_list<LlvmValue> arguments) {
    const auto parameters = callee.parameters();
    if (callee.variadic() ? arguments.size() < parameters.size() : arguments.size() != parameters.size())
        reject_arguments(callee);
    put_type(callee.result());
    if (callee.variadic()) {
        put(" (");
        for (const auto& parameter : parameters) {
            put_type(parameter);
            put(", ");
        }
        put("...)");
    }
    put(" @");
    put(callee.symbol());
    put("(");
    std::size_t index = 0;
    for (const auto& argument : arguments) {
        if (index < parameters.size() && !(argument.type == parameters[index])) reject_arguments(callee);
        if (index != 0) put(", ");
        ++index;
        put_value(argument);
    }
    put(")\n");
}

void LlvmBuilder::reject_arguments(const LlvmCallee& callee) {
    throw std::logic_error("call of @" + std::string(callee.symbol()) +
                           ": the arguments do not match its parameter types");
}

void LlvmBuilder::call(const LlvmCall& call) {
    observed_call(call.callee_.global_symbol(), [&] {
        if (call.result_.empty()) put("  call ");
        else {
            assign(call.result_);
            put("call ");
        }
        put(call.result_attributes_);
        put_type(call.type_);
        put(" ");
        put_operand(call.callee_);
        put("(");
        bool first = true;
        for (const auto& argument : call.arguments_) {
            if (!first) put(", ");
            first = false;
            put_type(argument.type);
            put(argument.attributes);
            put(" ");
            put_operand(argument.operand);
        }
        put(")\n");
    });
}

void LlvmBuilder::debug_declare(LlvmValue storage, LlvmOperand variable, LlvmOperand expression) {
    put("  call void @llvm.dbg.declare(metadata ");
    put_value(storage);
    put(", metadata ");
    put_operand(variable);
    put(", metadata ");
    put_operand(expression);
    put(")\n");
}

void LlvmBuilder::br(LlvmBlock target) {
    put("  br label %");
    put(target.label);
    put("\n");
}

void LlvmBuilder::br(LlvmOperand condition, LlvmBlock if_true, LlvmBlock if_false) {
    put("  br i1 ");
    put_operand(condition);
    put(", label %");
    put(if_true.label);
    put(", label %");
    put(if_false.label);
    put("\n");
}

void LlvmBuilder::switch_on(LlvmValue value, LlvmBlock otherwise, std::span<const LlvmCase> cases) {
    put("  switch ");
    put_value(value);
    put(", label %");
    put(otherwise.label);
    put(" [");
    for (const auto& item : cases) {
        put(" i64 ");
        put_operand(item.value);
        put(", label %");
        put(item.block.label);
    }
    put(" ]\n");
}

void LlvmBuilder::ret(LlvmValue value) {
    put("  ret ");
    put_value(value);
    put("\n");
}

void LlvmBuilder::ret_void() {
    put("  ret void\n");
}

void LlvmBuilder::unreachable() {
    put("  unreachable\n");
}

void LlvmBuilder::put_signature(const LlvmFunction& function, bool named) {
    put(function.result_attributes_);
    put_type(function.result_);
    put(" ");
    put(function.symbol_);
    put("(");
    bool first = true;
    for (const auto& parameter : function.parameters_) {
        if (!first) put(", ");
        first = false;
        put_type(parameter.type);
        put(parameter.attributes);
        if (named) {
            put(" ");
            put(parameter.name);
        }
    }
    put(")");
}

void LlvmBuilder::define(const LlvmFunction& function) {
    put("define ");
    put_signature(function, true);
    for (const auto& attribute : function.attributes_) {
        put(" ");
        put(attribute);
    }
    if (function.has_debug_subprogram_) {
        put(" !dbg !");
        append_decimal(out_, function.debug_subprogram_);
    }
    put(" {\n");
}

void LlvmBuilder::end_function() {
    put("}\n\n");
}

void LlvmBuilder::declare(const LlvmFunction& function) {
    put("declare ");
    put_signature(function, false);
    put("\n\n");
}

} // namespace quidra::llvm_text
