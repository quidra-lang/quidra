#include "ir/instruction_text.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
using namespace quidra;
using namespace quidra::ir;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T sample() {
    if constexpr (std::is_same_v<T, Binary>)
        return Binary{20, "+", 11, 12, Type::simple(TypeKind::Int64), Type::simple(TypeKind::Int64)};
    else return T{};
}

template<std::size_t... I>
void every_instruction(std::index_sequence<I...>) {
    const auto check = []<class T>() {
        auto node = sample<T>();
        if constexpr (requires { node.out; }) node.out = 20;
        const auto text = instruction_text(Instruction{node});
        require(!text.empty(), "every instruction alternative must have compact text");
        require(text.find('\n') == std::string::npos, "one instruction renders as one compact line");
    };
    (check.template operator()<std::variant_alternative_t<I, Instruction>>(), ...);
}

void important_missing_operands() {
    TensorBinary binary{20,"+",11,12,{},{},{},{801,802}};
    const auto ordinary_binary=instruction_text(binary);binary.broadcast_proven=true;
    require(instruction_text(binary)==ordinary_binary+" broadcast-proven","binary proof flag must be visible without changing ordinary text");
    TensorCompare proven_compare{20,"<",11,12,{},{},{},{801,802}};
    const auto ordinary_compare=instruction_text(proven_compare);proven_compare.shape_proven=true;
    require(instruction_text(proven_compare)==ordinary_compare+" shape-proven","compare proof flag must be visible without changing ordinary text");
    const auto compare = instruction_text(TensorCompare{20, "<", 11, 12, {}, {}, {}, {801, 802}});
    require(compare.find("tensor.compare < %11, %12") != std::string::npos,
            "tensor comparison displays its operation and both inputs");
    const auto set = instruction_text(TensorSet{13, {14, 15}, 16, {}, {803, 804}});
    require(set.find("tensor.set %13") != std::string::npos &&
                set.find("%14") != std::string::npos && set.find("%15") != std::string::npos &&
                set.find("%16") != std::string::npos, "tensor assignment shows indices and stored value");
    require(instruction_text(StringRepeat{21, 17, 18}).find("string.repeat %17, %18") != std::string::npos,
            "string repetition shows count and fill");
    require(instruction_text(TimeSince{22, 19, 23}).find("time.since %19, %23") != std::string::npos,
            "time query shows its sync operand");
    require(instruction_text(CliFinish{}) == "cli.finish", "operand-free instructions still render");
}

void existing_text_is_unchanged() {
    require(instruction_text(Branch{11, "yes", "no"}) == "branch %11, yes, no",
            "existing branch text is unchanged");
    require(instruction_text(ReturnVoid{}) == "return", "existing return text is unchanged");
    require(instruction_text(LoadLocal{20, "value", {}, true}) == "%20 = load.move value",
            "existing ownership transfer text is unchanged");
    require(instruction_text(ConstantBool{20, true}) == "%20 = const.bool true",
            "existing scalar text is unchanged");
    const auto shaped = instruction_text(ShapedConstraintCheck{11, TypeKind::Tensor, {12, {}}, {805, 806},
                                                             {"N", ""}, "subject", 1});
    require(shaped == "shape.constraint %11 rank>=2 ellipsis=1",
            "ellipsis retains a minimum rank and explicit boundary");
}
} // namespace

int main() {
    try {
        every_instruction(std::make_index_sequence<std::variant_size_v<Instruction>>{});
        important_missing_operands();
        existing_text_is_unchanged();
        std::cout << "compact IR: every instruction renders; existing spellings preserved\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
