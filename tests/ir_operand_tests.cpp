// Step 7 foundations: operands are typed IR references, independent of the
// compact diagnostic rendering.
#include "quidra/ir/instruction.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace quidra;
using namespace quidra::ir;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Collected final : OperandVisitor {
    std::vector<ValueId> definitions, uses;
    std::vector<std::pair<std::string, LocalRole>> locals;
    std::vector<std::string> successors;

    void definition(ValueId value) override { definitions.push_back(value); }
    void use(ValueId value) override { uses.push_back(value); }
    void local(std::string_view name, LocalRole role) override {
        locals.emplace_back(name, role);
    }
    void successor(std::string_view name) override { successors.emplace_back(name); }
};

template<class T>
Collected collect(const T& node) {
    Collected result;
    InstructionTraits<T>::visit_operands(node, result);
    return result;
}

template<class T>
T sample_instruction() {
    if constexpr (std::is_same_v<T, Binary>)
        return Binary{301, "+", 11, 12, Type::simple(TypeKind::Int64), Type::simple(TypeKind::Int64)};
    else
        return T{};
}

template<std::size_t... I>
void every_alternative(std::index_sequence<I...>) {
    const auto check = []<class T>() {
        auto node = sample_instruction<T>();
        if constexpr (requires { node.out; }) node.out = 301;
        const auto direct = collect(node);
        Collected variant;
        visit_operands(Instruction{node}, variant);
        require(direct.definitions == variant.definitions && direct.uses == variant.uses &&
                    direct.locals == variant.locals && direct.successors == variant.successors,
                "variant visitor must dispatch through each instruction's traits");
        if constexpr (requires { node.out; })
            require(std::find(direct.definitions.begin(), direct.definitions.end(), 301) !=
                        direct.definitions.end(), "an instruction result must be a definition");
    };
    (check.template operator()<std::variant_alternative_t<I, Instruction>>(), ...);
}

void hidden_and_multiple_results() {
    const auto compare = collect(TensorCompare{20, "<", 11, 12, {}, {}, {}, {801, 802}});
    require(compare.definitions == std::vector<ValueId>{20} &&
                compare.uses == std::vector<ValueId>({11, 12}),
            "tensor comparisons expose operands even without compact text");
    require(collect(StringSplitIterNext{21, 22, 13}).definitions == std::vector<ValueId>({21, 22}),
            "split iterators define both text and presence");
    const auto parsed = collect(ParseNumberDirect{23, 24, 25, 14, {}, {803, 804}});
    require(parsed.definitions == std::vector<ValueId>({23, 24, 25}) &&
                parsed.uses == std::vector<ValueId>{14}, "direct parsing defines value, success and error");
    const auto text = collect(StringFromUtf8ArrayDirect{26, 27, 28, 15, {805, 806}});
    require(text.definitions == std::vector<ValueId>({26, 27, 28}) &&
                text.uses == std::vector<ValueId>{15}, "UTF8 decoding's error is a definition");
    const auto appended = collect(StringBuildAppendMove{29, 30, 16, {{17, {}}, {18, {}}}, 19});
    require(appended.definitions == std::vector<ValueId>({29, 30}) &&
                appended.uses == std::vector<ValueId>({16, 17, 18, 19}),
            "append length is a definition and build parts are uses");
}

void nested_optional_operands() {
    const auto call = collect(Call{31, "callee", {{11, {}}, {0, 12}}, {}, {807, 808}});
    require(call.uses == std::vector<ValueId>({11, 12}),
            "absent call value is skipped while a writable address is a use");
    const auto shaped = collect(ShapedConstraintCheck{13, TypeKind::Tensor,
        {14, {}, 15}, {809, 810}, {"N", "", "M"}, "subject", 1});
    require(shaped.uses == std::vector<ValueId>({13, 14, 15}) && shaped.locals.empty(),
            "shape variable diagnostic names and ellipsis boundary are metadata");
    const auto index = collect(TensorIndex{32, 16,
        {{0, 17, {}, {}, {}}, {1, {}, 18, {}, 19}}, {}, {811, 812}});
    require(index.uses == std::vector<ValueId>({16, 17, 18, 19}),
            "tensor index parts expose every present operand");
    const auto backward = collect(TensorBackward{20, {{21, false}, {22, true}}, 23, 24, {813, 814}});
    require(backward.uses == std::vector<ValueId>({20, 21, 22, 23, 24}) &&
                backward.successors.empty(), "autograd targets are values, not CFG targets");
    ParallelBlock parallel;
    parallel.out = 33;
    parallel.nodes = {{"worker", {{25, {}}, {0, 26}}, {901}, 815, 816}};
    parallel.iterations = ParallelIteration{27, 28, {{902, 29, false, false}}};
    require(collect(parallel).uses == std::vector<ValueId>({25, 26, 27, 28, 29}),
            "parallel captures and receipts are uses; predecessor/capture indices are metadata");
}

void locals_and_control_targets() {
    require(collect(DeclareLocal{"value", {}, "source", 817, 818}).locals ==
                std::vector<std::pair<std::string, LocalRole>>{{"value", LocalRole::Declaration}},
            "only the IR local name declares storage");
    require(collect(LoadLocal{34, "value", {}, false}).locals.front().second == LocalRole::Read &&
                collect(LoadLocal{35, "value", {}, true}).locals.front().second == LocalRole::ReadWrite,
            "moving out reads and clears the local");
    require(collect(StoreLocal{"value", 30, {}}).locals.front().second == LocalRole::Write,
            "store writes the local");
    require(collect(AddressLocal{36, "value"}).locals.front().second == LocalRole::Address,
            "taking a local address is not reading its value");
    require(collect(BindReference{"ref", 37}).locals.front().second == LocalRole::Write &&
                collect(LoadReference{38, "ref", {}}).locals.front().second == LocalRole::ReferenceRead &&
                collect(StoreReference{"ref", 39, {}}).locals.front().second == LocalRole::ReferenceWrite,
            "rebinding a reference is distinct from accessing its target");
    const auto branch = collect(Branch{31, "yes", "no"});
    require(branch.uses == std::vector<ValueId>{31} &&
                branch.successors == std::vector<std::string>({"yes", "no"}) && branch.locals.empty(),
            "branch targets have their own role");
    const auto source = collect(SourceLocation{{819, 820}, "file", "revision", "node", "kind"});
    require(source.definitions.empty() && source.uses.empty() && source.locals.empty() &&
                source.successors.empty(), "provenance is never an operand");
}

void absent_value_sentinels() {
    require(collect(Call{0, "procedure", {{40, {}}}, Type::simple(TypeKind::Void)}).definitions.empty(),
            "a void direct call's zero result does not define an SSA value");
    require(collect(IndirectCall{0, 41, {42}, {}, Type::simple(TypeKind::Void)}).definitions.empty(),
            "a void indirect call's zero result does not define an SSA value");
    require(collect(BindReference{"ref", 0}).uses.empty(),
            "clearing a reference uses no address value");
    require(collect(VariantMake{43, 0, 0, {}, Type::simple(TypeKind::Void)}).uses.empty(),
            "a void variant has no payload operand");
    require(collect(BindFieldReference{44, 901, 45, 0}).uses == std::vector<ValueId>({44, 45}) &&
                collect(BindFieldReference{44, 901, 45, 46}).uses == std::vector<ValueId>({44, 45, 46}),
            "reference-field root zero is a literal, not an SSA value");
    require(collect(ReplDisplay{0, Type::simple(TypeKind::None), {}}).uses.empty(),
            "REPL none is a literal without an SSA value");
}

void finite_report_metadata() {
    const CheckedReal addition{47, Type::simple(TypeKind::Real32), {902, 903},
                               FiniteCause::addition, 48, 49};
    const CheckedReal division{50, Type::simple(TypeKind::Real32), {904, 905},
                               FiniteCause::division, 51, 52};
    require(collect(FiniteFlagCheck{"flag", addition}).uses.empty(),
            "a sunk addition check must not use loop-body SSA values at the exit");
    require(collect(FiniteFlagUpdate{"flag", addition}).uses == std::vector<ValueId>{47},
            "updating a finite flag reads the result, not operation metadata");
    require(collect(FiniteCheck{{division}, {addition}}).uses == std::vector<ValueId>({50, 52, 47}),
            "a finite check reads results and division's error-report divisor");
    require(collect(FiniteFlagCheck{"flag", division}).uses == std::vector<ValueId>{52},
            "a division failure report reads its divisor even without testing the result");
}
} // namespace

int main() {
    try {
        every_alternative(std::make_index_sequence<std::variant_size_v<quidra::ir::Instruction>>{});
        hidden_and_multiple_results();
        nested_optional_operands();
        locals_and_control_targets();
        absent_value_sentinels();
        finite_report_metadata();
        std::cout << "typed operands: all instruction alternatives and semantic roles verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
