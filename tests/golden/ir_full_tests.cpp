// Pins the field counts that ir_full.cpp detects, and the instruction names it
// prints. A failure here is intended when an IR struct gains or loses a field:
// update the table in the same change that accepts the new output.
// Also pins how failure.cpp names the class of a thrown exception.
#include "failure.hpp"
#include "ir_full.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <variant>

namespace {

using namespace quidra;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "ir_full_tests: " << message << "\n";
    ++failures;
}

template <class T>
void check_arity(std::string_view name, std::size_t expected) {
    const auto detected = golden::aggregate_arity<T>();
    expect(detected == expected, std::string(name) + " has " + std::to_string(detected) +
                                     " detected fields, expected " + std::to_string(expected));
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The compiler's own spelling of the alternative: an Itanium mangled name ends
// in `<length><name>E`, an MSVC name in `::<name>`.
template <class T>
void check_name(std::size_t index) {
    const std::string_view spelling = typeid(T).name();
    const auto name = golden::instruction_names[index];
    const auto itanium = std::to_string(name.size()) + std::string(name) + "E";
    const auto msvc = "::" + std::string(name);
    expect(ends_with(spelling, itanium) || ends_with(spelling, msvc),
           "instruction_names[" + std::to_string(index) + "] is " + std::string(name) +
               " but the alternative is " + std::string(spelling));
}

template <std::size_t... I>
void check_names(std::index_sequence<I...>) {
    (check_name<std::variant_alternative_t<I, ir::Instruction>>(I), ...);
}

void check_arities() {
    check_arity<ir::SourceLocation>("SourceLocation", 6);
    check_arity<ir::ConstantInt>("ConstantInt", 3);
    check_arity<ir::ConstantFloat>("ConstantFloat", 4);
    check_arity<ir::ConstantExact>("ConstantExact", 3);
    check_arity<ir::ConstantBool>("ConstantBool", 2);
    check_arity<ir::ConstantString>("ConstantString", 2);
    check_arity<ir::ArrayMake>("ArrayMake", 3);
    check_arity<ir::ArrayAlloc>("ArrayAlloc", 4);
    check_arity<ir::ClassMake>("ClassMake", 3);
    check_arity<ir::FieldGet>("FieldGet", 4);
    check_arity<ir::FieldSet>("FieldSet", 5);
    check_arity<ir::DeclareLocal>("DeclareLocal", 5);
    check_arity<ir::DeclareReference>("DeclareReference", 3);
    check_arity<ir::AddressLocal>("AddressLocal", 2);
    check_arity<ir::AddressField>("AddressField", 3);
    check_arity<ir::AddressElement>("AddressElement", 8);
    check_arity<ir::LoadAddress>("LoadAddress", 3);
    check_arity<ir::StoreAddress>("StoreAddress", 3);
    check_arity<ir::BindReference>("BindReference", 2);
    check_arity<ir::ReferenceAddress>("ReferenceAddress", 2);
    check_arity<ir::LoadReference>("LoadReference", 3);
    check_arity<ir::StoreReference>("StoreReference", 3);
    check_arity<ir::ArrayLength>("ArrayLength", 2);
    check_arity<ir::ArrayCanAppendMove>("ArrayCanAppendMove", 2);
    check_arity<ir::ArrayGrowMove>("ArrayGrowMove", 3);
    check_arity<ir::ArraySorted>("ArraySorted", 5);
    check_arity<ir::ArrayInitializationComplete>("ArrayInitializationComplete", 2);
    check_arity<ir::StringIndex>("StringIndex", 5);
    check_arity<ir::StringIndexAsciiCompare>("StringIndexAsciiCompare", 7);
    check_arity<ir::StringAsciiCountPrefix>("StringAsciiCountPrefix", 10);
    check_arity<ir::StringLength>("StringLength", 2);
    check_arity<ir::StringEmpty>("StringEmpty", 3);
    check_arity<ir::StringContains>("StringContains", 3);
    check_arity<ir::StringStartsWith>("StringStartsWith", 3);
    check_arity<ir::StringEndsWith>("StringEndsWith", 3);
    check_arity<ir::StringFind>("StringFind", 4);
    check_arity<ir::StringSlice>("StringSlice", 6);
    check_arity<ir::StringTrim>("StringTrim", 2);
    check_arity<ir::StringSplit>("StringSplit", 3);
    check_arity<ir::StringSplitIterBegin>("StringSplitIterBegin", 4);
    check_arity<ir::StringSplitIterNext>("StringSplitIterNext", 3);
    check_arity<ir::StringSplitIterEnd>("StringSplitIterEnd", 1);
    check_arity<ir::StringParseTwoSigned>("StringParseTwoSigned", 5);
    check_arity<ir::StringUtf8>("StringUtf8", 2);
    check_arity<ir::StringFromUtf8>("StringFromUtf8", 3);
    check_arity<ir::StringFromUtf8ArrayDirect>("StringFromUtf8ArrayDirect", 4);
    check_arity<ir::StringCodepoints>("StringCodepoints", 2);
    check_arity<ir::StringJoin>("StringJoin", 5);
    check_arity<ir::StringConcat>("StringConcat", 2);
    check_arity<ir::StringBuild>("StringBuild", 3);
    check_arity<ir::StringBuildAppendMove>("StringBuildAppendMove", 5);
    check_arity<ir::StringCanAppendMove>("StringCanAppendMove", 2);
    check_arity<ir::StringAppendMove>("StringAppendMove", 3);
    check_arity<ir::StringRepeat>("StringRepeat", 3);
    check_arity<ir::BinAlloc>("BinAlloc", 3);
    check_arity<ir::BinLength>("BinLength", 2);
    check_arity<ir::BinGet>("BinGet", 6);
    check_arity<ir::BinSet>("BinSet", 6);
    check_arity<ir::BinSlice>("BinSlice", 6);
    check_arity<ir::ParseBin>("ParseBin", 4);
    check_arity<ir::BinConvert>("BinConvert", 6);
    check_arity<ir::NumericConvert>("NumericConvert", 8);
    check_arity<ir::FallibleNumericConvert>("FallibleNumericConvert", 7);
    check_arity<ir::ArrayNumericCast>("ArrayNumericCast", 7);
    check_arity<ir::TensorCreate>("TensorCreate", 7);
    check_arity<ir::TensorTransfer>("TensorTransfer", 6);
    check_arity<ir::TensorReshape>("TensorReshape", 6);
    check_arity<ir::TensorTranspose>("TensorTranspose", 7);
    check_arity<ir::TensorContiguous>("TensorContiguous", 5);
    check_arity<ir::TensorGather>("TensorGather", 7);
    check_arity<ir::TensorScatter>("TensorScatter", 7);
    check_arity<ir::TensorShape>("TensorShape", 3);
    check_arity<ir::TensorDevice>("TensorDevice", 2);
    check_arity<ir::TensorIsContiguous>("TensorIsContiguous", 2);
    check_arity<ir::TensorIsTracked>("TensorIsTracked", 2);
    check_arity<ir::TensorHasGrad>("TensorHasGrad", 2);
    check_arity<ir::TensorClearGrad>("TensorClearGrad", 3);
    check_arity<ir::TensorItem>("TensorItem", 5);
    check_arity<ir::TensorTrack>("TensorTrack", 7);
    check_arity<ir::TensorBackward>("TensorBackward", 6);
    check_arity<ir::TensorGrad>("TensorGrad", 5);
    check_arity<ir::TensorCast>("TensorCast", 7);
    check_arity<ir::ShapedConstraintCheck>("ShapedConstraintCheck", 5);
    check_arity<ir::ExtentEqualCheck>("ExtentEqualCheck", 4);
    check_arity<ir::TensorBinary>("TensorBinary", 9);
    check_arity<ir::TensorCompare>("TensorCompare", 9);
    check_arity<ir::TensorBoolReduce>("TensorBoolReduce", 5);
    check_arity<ir::TensorIndex>("TensorIndex", 6);
    check_arity<ir::TensorSet>("TensorSet", 6);
    check_arity<ir::ParseNumber>("ParseNumber", 4);
    check_arity<ir::ParseNumberDirect>("ParseNumberDirect", 5);
    check_arity<ir::ExactAtom>("ExactAtom", 4);
    check_arity<ir::ExactUnary>("ExactUnary", 5);
    check_arity<ir::CliArgument>("CliArgument", 4);
    check_arity<ir::CliArgumentOptional>("CliArgumentOptional", 5);
    check_arity<ir::CliOption>("CliOption", 4);
    check_arity<ir::CliFlag>("CliFlag", 2);
    check_arity<ir::CliFinish>("CliFinish", 0);
    check_arity<ir::Flush>("Flush", 2);
    check_arity<ir::FileOpen>("FileOpen", 3);
    check_arity<ir::FileCreate>("FileCreate", 3);
    check_arity<ir::FileAppend>("FileAppend", 3);
    check_arity<ir::FileHandleRead>("FileHandleRead", 3);
    check_arity<ir::FileHandleReadLine>("FileHandleReadLine", 3);
    check_arity<ir::FileHandleReadBin>("FileHandleReadBin", 3);
    check_arity<ir::FileHandleWrite>("FileHandleWrite", 5);
    check_arity<ir::FileHandleFlush>("FileHandleFlush", 3);
    check_arity<ir::FileHandleSeek>("FileHandleSeek", 4);
    check_arity<ir::FileHandleClose>("FileHandleClose", 1);
    check_arity<ir::FileRead>("FileRead", 3);
    check_arity<ir::FileReadBin>("FileReadBin", 3);
    check_arity<ir::FileWrite>("FileWrite", 4);
    check_arity<ir::FileWriteBin>("FileWriteBin", 4);
    check_arity<ir::FileExists>("FileExists", 3);
    check_arity<ir::FileIsDirectory>("FileIsDirectory", 3);
    check_arity<ir::FileRemove>("FileRemove", 3);
    check_arity<ir::FileCopy>("FileCopy", 4);
    check_arity<ir::FileMove>("FileMove", 4);
    check_arity<ir::FileMkdir>("FileMkdir", 3);
    check_arity<ir::FileList>("FileList", 4);
    check_arity<ir::EnvironmentGet>("EnvironmentGet", 3);
    check_arity<ir::EnvironmentHas>("EnvironmentHas", 2);
    check_arity<ir::TestAssert>("TestAssert", 1);
    check_arity<ir::TimeNow>("TimeNow", 2);
    check_arity<ir::TimeSince>("TimeSince", 3);
    check_arity<ir::TimeSeconds>("TimeSeconds", 2);
    check_arity<ir::TimeSleep>("TimeSleep", 3);
    check_arity<ir::GpuSync>("GpuSync", 3);
    check_arity<ir::TaskAll>("TaskAll", 7);
    check_arity<ir::AtomicCounterCreate>("AtomicCounterCreate", 2);
    check_arity<ir::AtomicCounterAdd>("AtomicCounterAdd", 5);
    check_arity<ir::AtomicCounterLoad>("AtomicCounterLoad", 2);
    check_arity<ir::AutogradTargetCreate>("AutogradTargetCreate", 1);
    check_arity<ir::AutogradTargetHasGrad>("AutogradTargetHasGrad", 2);
    check_arity<ir::AutogradTargetClearGrad>("AutogradTargetClearGrad", 3);
    check_arity<ir::AutogradTargetGradient>("AutogradTargetGradient", 5);
    check_arity<ir::RandomGenerator>("RandomGenerator", 2);
    check_arity<ir::RandomInt>("RandomInt", 6);
    check_arity<ir::RandomFloat>("RandomFloat", 2);
    check_arity<ir::RandomBool>("RandomBool", 2);
    check_arity<ir::ProcessRun>("ProcessRun", 3);
    check_arity<ir::ProcessShell>("ProcessShell", 2);
    check_arity<ir::JsonParse>("JsonParse", 3);
    check_arity<ir::JsonKind>("JsonKind", 2);
    check_arity<ir::JsonSize>("JsonSize", 3);
    check_arity<ir::JsonGet>("JsonGet", 4);
    check_arity<ir::JsonAt>("JsonAt", 4);
    check_arity<ir::JsonText>("JsonText", 3);
    check_arity<ir::JsonInteger>("JsonInteger", 3);
    check_arity<ir::JsonNumber>("JsonNumber", 3);
    check_arity<ir::JsonBigInt>("JsonBigInt", 3);
    check_arity<ir::JsonBigReal>("JsonBigReal", 3);
    check_arity<ir::JsonBoolean>("JsonBoolean", 3);
    check_arity<ir::JsonEncode>("JsonEncode", 2);
    check_arity<ir::JsonEqual>("JsonEqual", 3);
    check_arity<ir::HttpGet>("HttpGet", 3);
    check_arity<ir::HttpHeader>("HttpHeader", 4);
    check_arity<ir::ArrayGet>("ArrayGet", 10);
    check_arity<ir::ArraySet>("ArraySet", 10);
    check_arity<ir::Clone>("Clone", 3);
    check_arity<ir::Retain>("Retain", 3);
    check_arity<ir::Release>("Release", 2);
    check_arity<ir::Unary>("Unary", 7);
    check_arity<ir::ToString>("ToString", 3);
    check_arity<ir::FormatNumber>("FormatNumber", 7);
    check_arity<ir::LoadLocal>("LoadLocal", 3);
    check_arity<ir::StoreLocal>("StoreLocal", 5);
    check_arity<ir::FunctionRef>("FunctionRef", 3);
    check_arity<ir::IndirectCall>("IndirectCall", 7);
    check_arity<ir::Call>("Call", 7);
    check_arity<ir::VariantMake>("VariantMake", 5);
    check_arity<ir::VariantTag>("VariantTag", 2);
    check_arity<ir::VariantPayload>("VariantPayload", 3);
    check_arity<ir::Print>("Print", 4);
    check_arity<ir::ReplDisplay>("ReplDisplay", 3);
    check_arity<ir::ReplReplayMode>("ReplReplayMode", 1);
    check_arity<ir::Input>("Input", 2);
    check_arity<ir::Exit>("Exit", 1);
    check_arity<ir::FailError>("FailError", 3);
    check_arity<ir::RangeCheckStep>("RangeCheckStep", 3);
    check_arity<ir::Return>("Return", 2);
    check_arity<ir::ReturnVoid>("ReturnVoid", 0);
    check_arity<ir::Jump>("Jump", 1);
    check_arity<ir::Branch>("Branch", 3);
    check_arity<ir::Pin>("Pin", 1);
    check_arity<ir::Unpin>("Unpin", 1);
    check_arity<ir::IterationShapeCheck>("IterationShapeCheck", 5);
    check_arity<ir::InitializedCheck>("InitializedCheck", 5);
    check_arity<Type>("Type", 10);
    check_arity<ir::Block>("Block", 2);
    check_arity<ir::TensorRegionLocation>("TensorRegionLocation", 2);
    check_arity<ir::TensorRegion>("TensorRegion", 9);
    check_arity<ir::Parameter>("Parameter", 5);
    check_arity<ir::Function>("Function", 11);
    check_arity<ir::ClassLayout>("ClassLayout", 4);
    check_arity<ir::Module>("Module", 4);
    check_arity<ir::UserSource>("UserSource", 5);
    check_arity<ir::StringBuildPart>("StringBuildPart", 3);
    check_arity<ir::TensorBackwardTarget>("TensorBackwardTarget", 2);
    check_arity<ir::TensorIndexPart>("TensorIndexPart", 5);
    check_arity<ir::CallArgument>("CallArgument", 2);
    check_arity<CompilerExtensionRegistration>("CompilerExtensionRegistration", 7);
}

void check_rendering() {
    expect(golden::quote("a\"b\\c\n\xff") == "\"a\\\"b\\\\c\\x0a\\xff\"", "quote escapes");
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32), 2, {-1, 3}, {4});
    expect(golden::serialize_type(tensor) ==
               "T{20, T{10, null, [], [], -1, [], [], \"\", \"\", []}, [], [], 2, [-1, 3], [4], "
               "\"\", \"\", []}",
           "tensor type rendering: " + golden::serialize_type(tensor));
    const ir::Instruction constant =
        ir::ConstantFloat{7, 1.5, Type::simple(TypeKind::Real64), "1.5"};
    expect(golden::serialize_instruction(constant) ==
               "ConstantFloat{7, f64:3ff8000000000000, T{9, null, [], [], -1, [], [], \"\", "
               "\"\", []}, \"1.5\"}",
           "constant rendering: " + golden::serialize_instruction(constant));
    const ir::Instruction binary = ir::Binary(3, "+", 1, 2, Type::simple(TypeKind::Int64),
                                              Type::simple(TypeKind::Int64), 4, 5, true);
    const auto int_type = std::string("T{0, null, [], [], -1, [], [], \"\", \"\", []}");
    expect(golden::serialize_instruction(binary) == "Binary{3, \"+\", 1, 2, " + int_type +
                                                        ", " + int_type + ", 4, 5, 1, 0}",
           "binary rendering: " + golden::serialize_instruction(binary));
    const ir::Instruction get = ir::ArrayGet{1, 2, 3, Type::simple(TypeKind::Int64), 0, 0,
                                             false, true, std::nullopt, 9};
    expect(golden::serialize_instruction(get) ==
               "ArrayGet{1, 2, 3, " + int_type + ", 0, 0, 0, 1, none, 9}",
           "optional rendering: " + golden::serialize_instruction(get));
}

template <class Exception>
golden::Failure failure_of(const Exception& exception) {
    try {
        throw exception;
    } catch (...) {
        return golden::current_failure();
    }
}

void check_failures() {
    const auto exact = failure_of(std::logic_error("m"));
    expect(exact.status == "error:logic_error" && exact.text == "logic_error \"m\"\n",
           "logic_error rendering: " + exact.text);
    const auto derived = failure_of(std::out_of_range("m"));
    expect(derived.status == "error:logic_error" &&
               derived.text == "logic_error type=std::out_of_range \"m\"\n",
           "out_of_range rendering: " + derived.text);
    const auto other = failure_of(std::runtime_error("m"));
    expect(other.status == "error:exception" &&
               other.text == "exception type=std::runtime_error \"m\"\n",
           "runtime_error rendering: " + other.text);
}

} // namespace

int main() {
    check_arities();
    check_names(std::make_index_sequence<std::variant_size_v<ir::Instruction>>{});
    check_rendering();
    check_failures();
    if (failures) {
        std::cerr << "ir_full_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ir_full_tests: ok\n";
    return 0;
}
