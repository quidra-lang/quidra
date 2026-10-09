// Hand-built typed-IR modules for backend paths that no source program
// reaches (tests/golden/README.md, Inputs): debug-information combinations,
// duplicate external declarations, one module per reachable backend
// logic_error, the numeric conversions between every pair of types,
// instruction shapes the lowering never builds but the backend handles, and
// type helpers no corpus program needs.
// The golden tool emits each with and without debug information
// (`--fixture backend/NAME`).
#pragma once

#include "quidra/ir.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::backend_fixtures {

inline Type int_type() { return Type::simple(TypeKind::Int64); }
inline Type float32_tensor() { return Type::tensor(Type::simple(TypeKind::Real32), 1); }

// `$entry` returning 0 after `body`.
inline ir::Function entry(std::string source_file, std::vector<ir::Instruction> body,
                          ir::ValueId result_value = 900) {
    ir::Function function;
    function.name = "$entry";
    function.source_file = std::move(source_file);
    function.entrypoint = true;
    function.result = int_type();
    body.push_back(ir::ConstantInt{result_value, "0", int_type()});
    body.push_back(ir::Return{result_value, int_type()});
    function.blocks.push_back(ir::Block{"entry", std::move(body)});
    return function;
}

inline ir::Function helper(std::string name, std::string source_file) {
    ir::Function function;
    function.name = std::move(name);
    function.source_file = std::move(source_file);
    function.result = int_type();
    function.blocks.push_back(ir::Block{
        "entry", {ir::ConstantInt{1, "7", int_type()}, ir::Return{1, int_type()}}});
    return function;
}

inline ir::Function external(std::string name, std::string symbol, Type parameter) {
    ir::Function function;
    function.name = std::move(name);
    function.source_file = "/virtual/main.qui";
    function.parameters.push_back(ir::Parameter{"value", std::move(parameter), false, false, false});
    function.result = int_type();
    function.external_symbol = std::move(symbol);
    return function;
}

// Two float32 tensors %3 and %4 of shape [2].
inline std::vector<ir::Instruction> two_tensors() {
    return {ir::ConstantInt{1, "2", int_type()},
            ir::ArrayMake{2, {1}, Type::array(int_type())},
            ir::TensorCreate{3, 2, std::nullopt, float32_tensor(), 1, 0, 0},
            ir::TensorCreate{4, 2, std::nullopt, float32_tensor(), 1, 0, 0}};
}

inline ir::Module module_of(std::vector<ir::Function> functions) {
    ir::Module module;
    module.functions = std::move(functions);
    return module;
}

// debug_info with no source file anywhere: no compile unit, no metadata.
inline ir::Module debug_no_source() { return module_of({entry("", {})}); }

// A function without source_file next to one with it: no subprogram for it.
inline ir::Module debug_function_without_source() {
    return module_of({helper("helper", ""),
                      entry("/virtual/main.qui",
                            {ir::SourceLocation{1, 1, "/virtual/main.qui", "r", "n000000", "x"}})});
}

// No recorded variable: no llvm.dbg.declare declaration.
inline ir::Module debug_no_variables() {
    return module_of({entry("/virtual/main.qui",
                            {ir::SourceLocation{2, 3, "/virtual/main.qui", "r", "n000001", "x"}})});
}

// One recorded variable: the llvm.dbg.declare declaration appears.
inline ir::Module debug_one_variable() {
    return module_of({entry("/virtual/main.qui",
                            {ir::DeclareLocal{"value", int_type(), "value", 2, 5},
                             ir::ConstantInt{1, "3", int_type()},
                             ir::StoreLocal{"value", 1, int_type()}})});
}

// Functions in two source files: one DIFile each, in path order.
inline ir::Module debug_two_files() {
    return module_of({helper("zeta", "/virtual/z.qui"), helper("alpha", "/virtual/a.qui"),
                      entry("/virtual/main.qui", {})});
}

// Two declarations of one C symbol with the same signature: the second is dropped.
inline ir::Module extern_identical_duplicate() {
    return module_of({external("first", "quidra_fixture_symbol", int_type()),
                      external("second", "quidra_fixture_symbol", int_type()),
                      entry("/virtual/main.qui", {})});
}

// Two declarations of one C symbol with different signatures: logic_error at
// the second.
inline ir::Module extern_conflicting_duplicate() {
    return module_of({external("first", "quidra_fixture_symbol", int_type()),
                      external("second", "quidra_fixture_symbol", Type::simple(TypeKind::Real64)),
                      entry("/virtual/main.qui", {})});
}

inline ir::Module error_tensor_binary_operator() {
    auto body = two_tensors();
    body.push_back(ir::TensorBinary{5, "??", 3, 4, float32_tensor(), float32_tensor(),
                                    float32_tensor(), 0, 0});
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

inline ir::Module error_tensor_comparison_operator() {
    auto body = two_tensors();
    body.push_back(ir::TensorCompare{5, "~~", 3, 4, float32_tensor(), float32_tensor(),
                                     Type::tensor(Type::simple(TypeKind::Bool), 1), 0, 0});
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

inline ir::Module error_tensor_element_type() {
    return module_of({entry("/virtual/main.qui",
                            {ir::ConstantInt{1, "2", int_type()},
                             ir::ArrayMake{2, {1}, Type::array(int_type())},
                             ir::TensorCreate{3, 2, std::nullopt,
                                              Type::tensor(Type::simple(TypeKind::String), 1), 1,
                                              0, 0}})});
}

inline ir::Module error_sorted_element_type() {
    const auto array = Type::array(Type::simple(TypeKind::Int));
    return module_of({entry("/virtual/main.qui",
                            {ir::ArrayMake{1, {}, array}, ir::ArraySorted{2, 1, array, 0, 0}})});
}

inline ir::Module error_backward_without_target() {
    auto body = two_tensors();
    body.push_back(ir::TensorBackward{3, {}, 0, 0, 0, 0});
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

// The helper collectors run after every function is emitted (I6).
inline ir::Module error_array_cast_structure() {
    return module_of({entry("/virtual/main.qui",
                            {ir::ConstantInt{1, "2", int_type()},
                             ir::ArrayNumericCast{2, 1, int_type(), Type::simple(TypeKind::Real64),
                                                  Type::simple(TypeKind::Real64), 0, 0}})});
}

// The scalar types a numeric conversion converts between, in a fixed order.
inline std::vector<Type> numeric_types() {
    std::vector<Type> types;
    for (const auto kind : {TypeKind::Int8, TypeKind::Int16, TypeKind::Int32, TypeKind::Int64,
                            TypeKind::Nat8, TypeKind::Nat16, TypeKind::Nat32, TypeKind::Nat64,
                            TypeKind::Real32, TypeKind::Real64, TypeKind::Int, TypeKind::Real})
        types.push_back(Type::simple(kind));
    return types;
}

// A constant of each numeric type: value i+1 holds a constant of type i.
inline std::vector<ir::Instruction> numeric_constants(const std::vector<Type>& types) {
    std::vector<ir::Instruction> body;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const auto id = static_cast<ir::ValueId>(i + 1);
        const auto& type = types[i];
        if (is_fixed_integer(type)) body.push_back(ir::ConstantInt{id, "1", type});
        else if (is_fixed_real(type)) body.push_back(ir::ConstantFloat{id, 1.5, type, "1.5"});
        else body.push_back(ir::ConstantExact{id, "1", type});
    }
    return body;
}

// NumericConvert between every pair of numeric types, unchecked and
// checked. The lowering emits only some of them (a cast that needs a range
// check or an exact source lowers to FallibleNumericConvert, and a checked
// NumericConvert comes only from shape extents); the backend handles all.
inline ir::Module numeric_convert_matrix() {
    const auto types = numeric_types();
    auto body = numeric_constants(types);
    ir::ValueId next = 100;
    for (const bool checked : {false, true})
        for (std::size_t source = 0; source < types.size(); ++source)
            for (const auto& target : types)
                body.push_back(ir::NumericConvert{next++, static_cast<ir::ValueId>(source + 1),
                                                  types[source], target, checked, 3, 7});
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

// FallibleNumericConvert between every pair of numeric types the backend
// converts fallibly (integer to integer, float to float32, an exact number
// to an integer or IEEE type, a big real to a big integer), each to
// T | error. The lowering emits it only where a conversion can fail.
inline ir::Module fallible_numeric_convert_matrix() {
    const auto types = numeric_types();
    auto body = numeric_constants(types);
    ir::ValueId next = 100;
    for (std::size_t source = 0; source < types.size(); ++source)
        for (const auto& target : types) {
            const auto& from = types[source];
            const bool exact = from.kind == TypeKind::Int || from.kind == TypeKind::Real;
            const bool supported =
                (is_fixed_integer(from) && is_fixed_integer(target)) ||
                (from.kind == TypeKind::Real64 && target.kind == TypeKind::Real32) ||
                (exact && (is_fixed_integer(target) || is_fixed_real(target))) ||
                (from.kind == TypeKind::Real && target.kind == TypeKind::Int);
            if (!supported) continue;
            body.push_back(ir::FallibleNumericConvert{
                next++, static_cast<ir::ValueId>(source + 1), from, target,
                Type::union_of({target, Type::simple(TypeKind::Error)}), 3, 7});
        }
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

// Conversions the lowering never emits in this form: a parse of a bin or
// of a big number whose result is not a union (parses give T | error), and
// the text of a string or an error (the lowering retains the value).
inline ir::Module conversion_shapes() {
    const auto text = Type::simple(TypeKind::String);
    const auto error = Type::simple(TypeKind::Error);
    return module_of({entry("/virtual/main.qui",
                            {ir::ConstantString{1, "01"},
                             ir::ParseBin{2, 1, Type::simple(TypeKind::Bin), false},
                             ir::ParseNumber{3, 1, Type::simple(TypeKind::Int),
                                             Type::simple(TypeKind::Int)},
                             ir::ParseNumber{4, 1, Type::simple(TypeKind::Real),
                                             Type::simple(TypeKind::Real)},
                             ir::ToString{5, 1, text},
                             ir::ToString{6, 1, error}})});
}

// Arithmetic, calls and returns no source program writes: an unsigned add
// proven free of overflow (the lowering proves it only for int), the
// address of a bin element (the checker rejects it), an indirect call with
// a never result (never is not a source type), and an entry function that
// returns void.
inline ir::Module operation_shapes() {
    const auto u8 = Type::simple(TypeKind::Nat8);
    const auto bin = Type::simple(TypeKind::Bin);
    ir::Function main;
    main.name = "$entry";
    main.source_file = "/virtual/main.qui";
    main.entrypoint = true;
    main.result = int_type();
    main.blocks.push_back(ir::Block{
        "entry",
        {ir::ConstantInt{1, "7", u8}, ir::ConstantInt{2, "9", u8},
         ir::Binary{3, "+", 1, 2, u8, u8, 0, 0, true},
         ir::ConstantInt{4, "2", int_type()},
         ir::BinAlloc{5, 4, 1},
         ir::AddressElement{6, 5, 4, bin, u8, true, 3, 7},
         ir::ConstantBool{7, true},
         ir::Branch{7, "call", "done"}}});
    main.blocks.push_back(ir::Block{
        "call",
        {ir::IndirectCall{8, 6, {4}, {int_type()}, Type::simple(TypeKind::Never), 3, 9}}});
    main.blocks.push_back(ir::Block{"done", {ir::ReturnVoid{}}});
    return module_of({std::move(main)});
}

// Tensor and autograd shapes the lowering never builds: a backward pass
// whose target is an autograd target, and a non-slice index item without an
// index.
inline ir::Module tensor_shapes() {
    auto body = two_tensors();
    body.push_back(ir::TensorBackward{3, {{4, true}}, 0, 0, 3, 7});
    body.push_back(ir::TensorIndex{5, 3, {ir::TensorIndexPart{}}, float32_tensor(), 3, 9});
    return module_of({entry("/virtual/main.qui", std::move(body))});
}

// The equality helper of a class whose fields have every kind of
// equality: a big integer, a big real, a bool, a float, a string and a
// narrow integer.
inline ir::Module class_equality() {
    ir::ClassLayout layout;
    layout.name = "Record";
    layout.field_names = {"count", "ratio", "flag", "weight", "label", "small"};
    layout.fields = {Type::simple(TypeKind::Int), Type::simple(TypeKind::Real),
                     Type::simple(TypeKind::Bool), Type::simple(TypeKind::Real64),
                     Type::simple(TypeKind::String), Type::simple(TypeKind::Int16)};
    const auto record = Type::class_type("Record");
    const std::vector<std::optional<ir::ValueId>> unset(6);
    auto module = module_of({entry("/virtual/main.qui",
                                   {ir::ClassMake{1, record, unset}, ir::ClassMake{2, record, unset},
                                    ir::Binary{3, "==", 1, 2, record, Type::simple(TypeKind::Bool)}})});
    module.classes.push_back(std::move(layout));
    return module;
}

// REPL displays of values no session prints: narrow and unsigned
// integers, big numbers, float32 and float, a bool, an address, a function,
// a fixed array of fixed arrays (its children inline), a union with a none
// case, and a class nested in a class with one of its fields initialized.
inline ir::Module repl_values() {
    ir::ClassLayout inner;
    inner.name = "Inner";
    inner.field_names = {"x", "y"};
    inner.fields = {int_type(), int_type()};
    ir::ClassLayout outer;
    outer.name = "Outer";
    outer.field_names = {"inner"};
    outer.fields = {Type::class_type("Inner")};
    const std::vector<Type> types{
        Type::simple(TypeKind::Int8), Type::simple(TypeKind::Nat16), Type::simple(TypeKind::Nat64),
        Type::simple(TypeKind::Int), Type::simple(TypeKind::Real), Type::simple(TypeKind::Real32),
        Type::simple(TypeKind::Real64), Type::simple(TypeKind::Bool), Type::simple(TypeKind::Address),
        Type::function(int_type()), Type::array(Type::array(int_type(), 3), 2),
        Type::union_of({int_type(), Type::simple(TypeKind::None)})};
    std::vector<ir::Instruction> body;
    ir::ValueId next = 1;
    for (const auto& type : types) body.push_back(ir::ReplDisplay{next++, type, {}});
    body.push_back(ir::ReplDisplay{next++, Type::class_type("Outer"), {"inner", "inner.x"}});
    auto module = module_of({entry("/virtual/main.qui", std::move(body))});
    module.classes.push_back(std::move(inner));
    module.classes.push_back(std::move(outer));
    return module;
}

// A recursive function that calls only itself, by name: without debug
// information it gets a self-depth wrapper. One returns void, one never
// returns; both take their parameter by address.
inline ir::Function self_recursive(std::string name, Type result) {
    ir::Function function;
    function.name = name;
    function.source_file = "/virtual/main.qui";
    function.parameters.push_back(ir::Parameter{"value", int_type(), true, false, false});
    function.result = result;
    std::vector<ir::Instruction> body{ir::AddressLocal{1, "value"},
                                      ir::Call{2, name, {ir::CallArgument{1, 1}}, result, 3, 7}};
    if (result.kind == TypeKind::Void) body.push_back(ir::ReturnVoid{});
    function.blocks.push_back(ir::Block{"entry", std::move(body)});
    return function;
}

inline ir::Module self_depth_shapes() {
    return module_of({self_recursive("again", Type::simple(TypeKind::Void)),
                      self_recursive("forever", Type::simple(TypeKind::Never)),
                      entry("/virtual/main.qui", {})});
}

// Type helpers no corpus program needs: the numeric casts of arrays of
// big numbers, to and from each kind of numeric element (each cast also
// gets its validator, which range-checks the casts that can fail), the
// clone of a union with a case that is cloned, one that is shared, one
// stored as it is and a none case, and the equality helper of a class
// without fields.
inline ir::Module helper_shapes() {
    const auto big_int = Type::simple(TypeKind::Int);
    const auto big_real = Type::simple(TypeKind::Real);
    const auto int8 = Type::simple(TypeKind::Int8);
    const auto uint8 = Type::simple(TypeKind::Nat8);
    const auto uint64 = Type::simple(TypeKind::Nat64);
    const auto float32 = Type::simple(TypeKind::Real32);
    const auto float64 = Type::simple(TypeKind::Real64);
    const std::vector<std::pair<Type, Type>> casts{
        {big_int, big_int},    {big_real, big_int},  {int8, big_int},     {uint8, big_int},
        {uint64, big_int},     {big_real, big_real}, {big_int, big_real}, {int8, big_real},
        {uint8, big_real},     {uint64, big_real},   {float32, big_real}, {float64, big_real},
        {big_int, int8},       {big_int, uint64},    {big_real, int_type()}, {big_real, uint8},
        {big_int, float32},    {big_int, float64},   {big_real, float32}, {big_real, float64}};
    std::vector<ir::Instruction> body;
    ir::ValueId next = 1;
    for (const auto& [from, to] : casts) {
        const auto source = Type::array(from);
        const auto target = Type::array(to);
        body.push_back(ir::ArrayMake{next, {}, source});
        body.push_back(ir::ArrayNumericCast{next + 1, next, source, target, target, 3, 7});
        next += 2;
    }
    const auto choice = Type::union_of({int_type(), Type::simple(TypeKind::String),
                                        Type::array(int_type()), Type::simple(TypeKind::None)});
    body.push_back(ir::ConstantInt{next, "1", int_type()});
    body.push_back(ir::VariantMake{next + 1, case_index(choice, int_type()), next, choice, int_type()});
    body.push_back(ir::Clone{next + 2, next + 1, choice});
    next += 3;
    const auto empty = Type::class_type("Empty");
    body.push_back(ir::ClassMake{next, empty, {}});
    body.push_back(ir::ClassMake{next + 1, empty, {}});
    body.push_back(ir::Binary{next + 2, "==", next, next + 1, empty, Type::simple(TypeKind::Bool)});
    ir::ClassLayout layout;
    layout.name = "Empty";
    auto module = module_of({entry("/virtual/main.qui", std::move(body))});
    module.classes.push_back(std::move(layout));
    return module;
}

struct Fixture {
    std::string_view name;
    ir::Module (*build)();
};

inline constexpr std::array<Fixture, 22> all{{
    {"debug_no_source", debug_no_source},
    {"debug_function_without_source", debug_function_without_source},
    {"debug_no_variables", debug_no_variables},
    {"debug_one_variable", debug_one_variable},
    {"debug_two_files", debug_two_files},
    {"extern_identical_duplicate", extern_identical_duplicate},
    {"extern_conflicting_duplicate", extern_conflicting_duplicate},
    {"error_tensor_binary_operator", error_tensor_binary_operator},
    {"error_tensor_comparison_operator", error_tensor_comparison_operator},
    {"error_tensor_element_type", error_tensor_element_type},
    {"error_sorted_element_type", error_sorted_element_type},
    {"error_backward_without_target", error_backward_without_target},
    {"error_array_cast_structure", error_array_cast_structure},
    {"numeric_convert_matrix", numeric_convert_matrix},
    {"fallible_numeric_convert_matrix", fallible_numeric_convert_matrix},
    {"conversion_shapes", conversion_shapes},
    {"operation_shapes", operation_shapes},
    {"tensor_shapes", tensor_shapes},
    {"class_equality", class_equality},
    {"repl_values", repl_values},
    {"self_depth_shapes", self_depth_shapes},
    {"helper_shapes", helper_shapes},
}};

} // namespace quidra::backend_fixtures
