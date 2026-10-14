// Pins the field counts that ir_full.cpp detects, and the instruction names it
// prints. A failure here is intended when an IR struct gains or loses a field:
// update the table in the same change that accepts the new output.
// Also pins how failure.cpp names the class of a thrown exception.
#include "failure.hpp"
#include "ir_full.hpp"
#include "ir/summaries.hpp"
#include "ir/fact_sources.hpp"
#include "ir/facts/entry.hpp"
#include "semantics/interface_contracts.hpp"
#include "quidra/compiler_extension_model.hpp"

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
    check_arity<ir::SourcePosition>("SourcePosition", 2);
    check_arity<ir::SourceLocation>("SourceLocation", 5);
    check_arity<ir::StatementBoundary>("StatementBoundary", 1);
    check_arity<ir::InlineScopeBegin>("InlineScopeBegin", 1);
    check_arity<ir::InlineScopeCleanup>("InlineScopeCleanup", 1);
    check_arity<ir::InlineScopeEnd>("InlineScopeEnd", 1);
    check_arity<ir::InlineBindAddress>("InlineBindAddress", 3);
    check_arity<ir::InlineExit>("InlineExit", 2);
    check_arity<ir::InlineScope>("InlineScope", 12);
    check_arity<ir::InlineSlot>("InlineSlot", 6);
    check_arity<ir::InlineStatementIdentity>("InlineStatementIdentity", 2);
    check_arity<ir::ConstantInt>("ConstantInt", 3);
    check_arity<ir::ConstantFloat>("ConstantFloat", 4);
    check_arity<ir::ConstantExact>("ConstantExact", 3);
    check_arity<ir::ConstantBool>("ConstantBool", 2);
    check_arity<ir::ConstantString>("ConstantString", 2);
    check_arity<ir::TraversalSiteCheck>("TraversalSiteCheck",3);
    check_arity<ir::TupleMake>("TupleMake",3);
    check_arity<ir::TupleGet>("TupleGet",5);
    check_arity<ir::AddressTupleElement>("AddressTupleElement",4);
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
    check_arity<ir::ArraySorted>("ArraySorted", 6);
    check_arity<ir::ArrayInitializationComplete>("ArrayInitializationComplete", 2);
    check_arity<ir::StringIndex>("StringIndex", 4);
    check_arity<ir::StringIndexAsciiCompare>("StringIndexAsciiCompare", 6);
    check_arity<ir::StringAsciiCountPrefix>("StringAsciiCountPrefix", 10);
    check_arity<ir::StringLength>("StringLength", 2);
    check_arity<ir::StringEmpty>("StringEmpty", 3);
    check_arity<ir::StringContains>("StringContains", 3);
    check_arity<ir::StringStartsWith>("StringStartsWith", 3);
    check_arity<ir::StringEndsWith>("StringEndsWith", 3);
    check_arity<ir::StringFind>("StringFind", 4);
    check_arity<ir::StringSlice>("StringSlice", 5);
    check_arity<ir::StringTrim>("StringTrim", 2);
    check_arity<ir::StringSplit>("StringSplit", 3);
    check_arity<ir::StringSplitIterBegin>("StringSplitIterBegin", 4);
    check_arity<ir::StringSplitIterNext>("StringSplitIterNext", 3);
    check_arity<ir::StringSplitIterEnd>("StringSplitIterEnd", 1);
    check_arity<ir::StringParseTwoSigned>("StringParseTwoSigned", 5);
    check_arity<ir::StringUtf8>("StringUtf8", 2);
    check_arity<ir::StringFromUtf8>("StringFromUtf8", 4);
    check_arity<ir::StringFromUtf8ArrayDirect>("StringFromUtf8ArrayDirect", 5);
    check_arity<ir::StringCodepoints>("StringCodepoints", 2);
    check_arity<ir::StringJoin>("StringJoin", 4);
    check_arity<ir::StringConcat>("StringConcat", 2);
    check_arity<ir::StringBuild>("StringBuild", 3);
    check_arity<ir::StringBuildAppendMove>("StringBuildAppendMove", 5);
    check_arity<ir::StringCanAppendMove>("StringCanAppendMove", 2);
    check_arity<ir::StringAppendMove>("StringAppendMove", 3);
    check_arity<ir::StringRepeat>("StringRepeat", 3);
    check_arity<ir::BinAlloc>("BinAlloc", 3);
    check_arity<ir::BinLength>("BinLength", 2);
    check_arity<ir::BinGet>("BinGet", 5);
    check_arity<ir::BinSet>("BinSet", 5);
    check_arity<ir::BinSlice>("BinSlice", 5);
    check_arity<ir::ParseBin>("ParseBin", 5);
    check_arity<ir::BinConvert>("BinConvert", 5);
    check_arity<ir::NumericConvert>("NumericConvert", 7);
    check_arity<ir::FallibleNumericConvert>("FallibleNumericConvert", 6);
    check_arity<ir::ArrayNumericCast>("ArrayNumericCast", 6);
    check_arity<ir::TensorCreate>("TensorCreate", 6);
    check_arity<ir::TensorTransfer>("TensorTransfer", 5);
    check_arity<ir::TensorReshape>("TensorReshape", 5);
    check_arity<ir::TensorTranspose>("TensorTranspose", 6);
    check_arity<ir::TensorContiguous>("TensorContiguous", 4);
    check_arity<ir::TensorGather>("TensorGather", 6);
    check_arity<ir::TensorScatter>("TensorScatter", 6);
    check_arity<ir::TensorShape>("TensorShape", 3);
    check_arity<ir::TensorDevice>("TensorDevice", 2);
    check_arity<ir::TensorIsContiguous>("TensorIsContiguous", 2);
    check_arity<ir::TensorIsTracked>("TensorIsTracked", 2);
    check_arity<ir::TensorHasGrad>("TensorHasGrad", 2);
    check_arity<ir::TensorClearGrad>("TensorClearGrad", 2);
    check_arity<ir::TensorItem>("TensorItem", 4);
    check_arity<ir::TensorTrack>("TensorTrack", 6);
    check_arity<ir::TensorBackward>("TensorBackward", 5);
    check_arity<ir::TensorGrad>("TensorGrad", 4);
    check_arity<ir::TensorCast>("TensorCast", 6);
    check_arity<ir::ShapedConstraintCheck>("ShapedConstraintCheck", 7);
    check_arity<ir::ExtentEqualCheck>("ExtentEqualCheck", 4);
    check_arity<ir::TensorBinary>("TensorBinary", 8);
    check_arity<ir::TensorCompare>("TensorCompare", 8);
    check_arity<ir::TensorBoolReduce>("TensorBoolReduce", 4);
    check_arity<ir::TensorIndex>("TensorIndex", 5);
    check_arity<ir::TensorSet>("TensorSet", 5);
    check_arity<ir::ParseNumber>("ParseNumber", 5);
    check_arity<ir::ParseNumberDirect>("ParseNumberDirect", 6);
    check_arity<ir::ExactAtom>("ExactAtom", 4);
    check_arity<ir::ExactUnary>("ExactUnary", 5);
    check_arity<ir::ExactOp>("ExactOp", 5);
    check_arity<ir::RealLoopCount>("RealLoopCount", 9);
    check_arity<ir::RealLoopValue>("RealLoopValue", 6);
    check_arity<ir::ExactLoopPlan>("ExactLoopPlan", 8);
    check_arity<ir::ExactFromScaled>("ExactFromScaled", 3);
    check_arity<ir::ExactLoopValue>("ExactLoopValue", 4);
    check_arity<ir::ExactScaledCompare>("ExactScaledCompare", 6);
    check_arity<ir::ExactScaledConvert>("ExactScaledConvert", 6);
    check_arity<ir::ExactRound>("ExactRound", 5);
    check_arity<ir::ExactConstant>("ExactConstant", 3);
    check_arity<ir::CliArgument>("CliArgument", 4);
    check_arity<ir::CliArgumentOptional>("CliArgumentOptional", 5);
    check_arity<ir::CliOption>("CliOption", 4);
    check_arity<ir::CliFlag>("CliFlag", 2);
    check_arity<ir::CliFinish>("CliFinish", 0);
    check_arity<ir::Flush>("Flush", 3);
    check_arity<ir::FileOpen>("FileOpen", 4);
    check_arity<ir::FileCreate>("FileCreate", 4);
    check_arity<ir::FileAppend>("FileAppend", 4);
    check_arity<ir::FileHandleRead>("FileHandleRead", 4);
    check_arity<ir::FileHandleReadLine>("FileHandleReadLine", 4);
    check_arity<ir::FileHandleReadBin>("FileHandleReadBin", 4);
    check_arity<ir::FileHandleWrite>("FileHandleWrite", 6);
    check_arity<ir::FileHandleFlush>("FileHandleFlush", 4);
    check_arity<ir::FileHandleSeek>("FileHandleSeek", 5);
    check_arity<ir::FileHandleClose>("FileHandleClose", 1);
    check_arity<ir::FileRead>("FileRead", 4);
    check_arity<ir::FileReadBin>("FileReadBin", 4);
    check_arity<ir::FileWrite>("FileWrite", 5);
    check_arity<ir::FileWriteBin>("FileWriteBin", 5);
    check_arity<ir::FileExists>("FileExists", 4);
    check_arity<ir::FileIsDirectory>("FileIsDirectory", 4);
    check_arity<ir::FileRemove>("FileRemove", 4);
    check_arity<ir::FileCopy>("FileCopy", 5);
    check_arity<ir::FileMove>("FileMove", 5);
    check_arity<ir::FileMkdir>("FileMkdir", 4);
    check_arity<ir::FileList>("FileList", 5);
    check_arity<ir::EnvironmentGet>("EnvironmentGet", 3);
    check_arity<ir::EnvironmentHas>("EnvironmentHas", 2);
    check_arity<ir::TestAssert>("TestAssert", 1);
    check_arity<ir::TimeNow>("TimeNow", 2);
    check_arity<ir::TimeSince>("TimeSince", 3);
    check_arity<ir::TimeSeconds>("TimeSeconds", 2);
    check_arity<ir::TimeSleep>("TimeSleep", 2);
    check_arity<ir::GpuSync>("GpuSync", 2);
    check_arity<ir::AutogradTargetCreate>("AutogradTargetCreate", 1);
    check_arity<ir::AutogradTargetHasGrad>("AutogradTargetHasGrad", 2);
    check_arity<ir::AutogradTargetClearGrad>("AutogradTargetClearGrad", 2);
    check_arity<ir::AutogradTargetGradient>("AutogradTargetGradient", 4);
    check_arity<ir::RandomGenerator>("RandomGenerator", 2);
    check_arity<ir::RandomInt>("RandomInt", 5);
    check_arity<ir::RandomFloat>("RandomFloat", 2);
    check_arity<ir::RandomBool>("RandomBool", 2);
    check_arity<ir::ProcessRun>("ProcessRun", 3);
    check_arity<ir::ProcessShell>("ProcessShell", 2);
    check_arity<ir::JsonParse>("JsonParse", 4);
    check_arity<ir::JsonKind>("JsonKind", 2);
    check_arity<ir::JsonSize>("JsonSize", 4);
    check_arity<ir::JsonGet>("JsonGet", 5);
    check_arity<ir::JsonAt>("JsonAt", 5);
    check_arity<ir::JsonText>("JsonText", 4);
    check_arity<ir::JsonInteger>("JsonInteger", 4);
    check_arity<ir::JsonNumber>("JsonNumber", 4);
    check_arity<ir::JsonBigInt>("JsonBigInt", 4);
    check_arity<ir::JsonBigReal>("JsonBigReal", 4);
    check_arity<ir::JsonBoolean>("JsonBoolean", 4);
    check_arity<ir::JsonEncode>("JsonEncode", 2);
    check_arity<ir::JsonEqual>("JsonEqual", 3);
    check_arity<ir::HttpGet>("HttpGet", 4);
    check_arity<ir::HttpHeader>("HttpHeader", 4);
    check_arity<ir::ArrayGet>("ArrayGet", 9);
    check_arity<ir::ArraySlice>("ArraySlice", 7);
    check_arity<ir::ArraySliceSet>("ArraySliceSet", 6);
    check_arity<ir::SliceBounds>("SliceBounds", 6);
    check_arity<ir::StringSliceStepped>("StringSliceStepped", 4);
    check_arity<ir::BinSlicePlanned>("BinSlicePlanned", 4);
    check_arity<ir::ArraySet>("ArraySet", 10);
    check_arity<ir::Clone>("Clone", 3);
    check_arity<ir::Retain>("Retain", 3);
    check_arity<ir::Release>("Release", 2);
    check_arity<ir::Unary>("Unary", 6);
    check_arity<ir::ToString>("ToString", 3);
    check_arity<ir::FormatNumber>("FormatNumber", 7);
    check_arity<ir::LoadLocal>("LoadLocal", 4);
    check_arity<ir::StoreLocal>("StoreLocal", 5);
    check_arity<ir::FunctionRef>("FunctionRef", 3);
    check_arity<ir::IndirectCall>("IndirectCall", 6);
    check_arity<ir::Call>("Call", 7);
    check_arity<ir::VariantMake>("VariantMake", 5);
    check_arity<ir::VariantTag>("VariantTag", 2);
    check_arity<ir::VariantPayload>("VariantPayload", 3);
    check_arity<ir::Print>("Print", 5);
    check_arity<ir::ReplDisplay>("ReplDisplay", 3);
    check_arity<ir::ReplReplayMode>("ReplReplayMode", 1);
    check_arity<ir::Input>("Input", 3);
    check_arity<ir::Exit>("Exit", 1);
    check_arity<ir::FailError>("FailError", 2);
    check_arity<ir::ForStepCheck>("ForStepCheck", 3);
    check_arity<ir::SliceLoopPlan>("SliceLoopPlan", 5);
    check_arity<ir::ParallelBlock>("ParallelBlock", 5);
    check_arity<ir::AddressReferenceSlot>("AddressReferenceSlot", 2);
    check_arity<ir::Return>("Return", 2);
    check_arity<ir::ReturnVoid>("ReturnVoid", 0);
    check_arity<ir::Jump>("Jump", 1);
    check_arity<ir::Branch>("Branch", 3);
    check_arity<ir::Pin>("Pin", 1);
    check_arity<ir::Unpin>("Unpin", 1);
    check_arity<ir::IterationShapeCheck>("IterationShapeCheck", 4);
    check_arity<ir::InitializedCheck>("InitializedCheck", 4);
    check_arity<ir::ErrorMake>("ErrorMake", 6);
    check_arity<ir::ErrorMessage>("ErrorMessage", 2);
    check_arity<ir::ErrorCode>("ErrorCode", 2);
    check_arity<ir::ErrorCause>("ErrorCause", 3);
    check_arity<ir::ErrorDetail>("ErrorDetail", 4);
    check_arity<ir::ErrorHop>("ErrorHop", 7);
    check_arity<ir::ErrorDetailOperand>("ErrorDetailOperand", 3);
    check_arity<ir::FiniteCheck>("FiniteCheck", 2);
    check_arity<ir::FiniteFlagUpdate>("FiniteFlagUpdate", 2);
    check_arity<ir::FiniteFlagCheck>("FiniteFlagCheck", 2);
    check_arity<ir::ComplexMake>("ComplexMake", 4);
    check_arity<ir::ComplexPart>("ComplexPart", 4);
    check_arity<ir::CheckedReal>("CheckedReal", 6);
    check_arity<Type>("Type", 12);
    check_arity<ir::Block>("Block", 2);
    check_arity<ir::TensorRegionLocation>("TensorRegionLocation", 2);
    check_arity<ir::TensorRegion>("TensorRegion", 9);
    check_arity<ir::Parameter>("Parameter", 6);
    check_arity<ir::ParallelNodeMetadata>("ParallelNodeMetadata", 3);
    check_arity<ir::Function>("Function", 19);
    check_arity<ir::ClassLayout>("ClassLayout", 5);
    check_arity<ir::Module>("Module", 8);
    check_arity<ir::UserSource>("UserSource", 5);
    check_arity<ir::StringQuote>("StringQuote", 2);
    check_arity<ir::ReferenceBoundCheck>("ReferenceBoundCheck", 3);
    check_arity<ir::FieldReferenceAddress>("FieldReferenceAddress", 3);
    check_arity<ir::FieldReferenceRoot>("FieldReferenceRoot", 3);
    check_arity<ir::BindFieldReference>("BindFieldReference", 4);
    check_arity<ir::RootSerial>("RootSerial", 1);
    check_arity<ir::SelfReferenceCheck>("SelfReferenceCheck", 5);
    check_arity<ir::StoredTargetsCheck>("StoredTargetsCheck", 5);
    check_arity<ir::StringBuildPart>("StringBuildPart", 4);
    check_arity<ir::TensorBackwardTarget>("TensorBackwardTarget", 2);
    check_arity<ir::TensorIndexPart>("TensorIndexPart", 5);
    check_arity<ir::CallArgument>("CallArgument", 2);
    check_arity<CompilerExtensionRegistration>("CompilerExtensionRegistration", 10);
    check_arity<semantics::EffectSummary>("EffectSummary", 19);
    check_arity<ir::analysis::IrSummaryFacts>("IrSummaryFacts", 7);
    check_arity<CompilerExtensionModel>("CompilerExtensionModel", 2);
    check_arity<CompilerExtensionTable>("CompilerExtensionTable", 3);
    check_arity<CompilerExpression>("CompilerExpression", 3);
    check_arity<CompilerExpression::Node>("CompilerExpressionNode", 4);
    check_arity<CompilerDeclarationSelector>("CompilerDeclarationSelector", 4);
    check_arity<CompilerPodField>("CompilerPodField", 5);
    check_arity<DeclarationIdentity>("DeclarationIdentity", 15);
    check_arity<DeclarationParameter>("DeclarationParameter", 4);
    check_arity<ir::facts::FactSources>("FactSources", 2);
    check_arity<ir::facts::FunctionFactSource>("FunctionFactSource", 2);
    check_arity<ir::facts::PolicyContracts>("PolicyContracts", 2);
    check_arity<ir::facts::PolicyAssignment>("PolicyAssignment", 2);
    check_arity<ExtentOrigin>("ExtentOrigin", 8);
    check_arity<ir::facts::ValidatedEntryFacts>("ValidatedEntryFacts", 7);
    check_arity<ir::facts::TensorFacts>("TensorFacts", 13);
    check_arity<semantics::InterfaceSources>("InterfaceSources", 1);
    check_arity<semantics::OriginalModuleInterfaces>("OriginalModuleInterfaces", 5);
    check_arity<semantics::InterfaceSourceBinding>("InterfaceSourceBinding", 3);
    check_arity<semantics::InterfaceImportSource>("InterfaceImportSource", 3);
    check_arity<semantics::InterfaceContracts>("InterfaceContracts", 15);
    check_arity<semantics::FunctionInterface>("FunctionInterface", 12);
    check_arity<semantics::ClassInterface>("ClassInterface", 7);
    check_arity<semantics::ClassInterface::Instance>("ClassInstanceInterface", 2);
    check_arity<semantics::FieldInterface>("FieldInterface", 6);
    check_arity<semantics::EnumInterface>("EnumInterface", 3);
    check_arity<semantics::EnumVariantInterface>("EnumVariantInterface", 2);
    check_arity<semantics::PublicImportInterface>("PublicImportInterface", 2);
    check_arity<semantics::NamedTypeIdentity>("NamedTypeIdentity", 3);
    check_arity<semantics::ConstantInterface>("ConstantInterface", 5);
    check_arity<semantics::AliasInterface>("AliasInterface", 3);
    check_arity<semantics::SignatureType>("SignatureType", 2);
    check_arity<semantics::ModuleSelector>("ModuleSelector", 2);
    check_arity<semantics::CanonicalInterfacePayload>("CanonicalInterfacePayload", 2);
    check_arity<semantics::BorrowParameterContract>("BorrowParameterContract", 5);
    check_arity<semantics::InitializationContract>("InitializationContract", 3);
    check_arity<semantics::StorageInitializationContract>("StorageInitializationContract", 4);
}

void check_rendering() {
    expect(golden::serialize_instruction(ir::ExtentEqualCheck{1,2,{3,4}})==
           "ExtentEqualCheck{1, 2, 3, 4}","default extent serialization is preserved");
    expect(golden::serialize_instruction(ir::ExtentEqualCheck{1,2,{3,4},true})==
           "ExtentEqualCheck{1, 2, 3, 4, for_collections=true}","zip length purpose is serialized");
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
    // A shape check prints its shape variables only when it has some, so
    // the view of every check without them is unchanged.
    const ir::Instruction shape = ir::ShapedConstraintCheck{
        4, TypeKind::Tensor, {std::nullopt, 5}, {6, 7}, {}, {}};
    expect(golden::serialize_instruction(shape) == "ShapedConstraintCheck{4, 20, [none, 5], 6, 7}",
           "shape check rendering: " + golden::serialize_instruction(shape));
    const ir::Instruction shaped = ir::ShapedConstraintCheck{
        4, TypeKind::Tensor, {std::nullopt, 5}, {6, 7}, {"", "K"}, "sq"};
    expect(golden::serialize_instruction(shaped) ==
               "ShapedConstraintCheck{4, 20, [none, 5], 6, 7, variables=[\"\", \"K\"], "
               "subject=\"sq\"}",
           "shape variable check rendering: " + golden::serialize_instruction(shaped));
    const ir::Instruction variadic_shape = ir::ShapedConstraintCheck{
        4, TypeKind::Tensor, {std::nullopt, 5}, {6, 7}, {}, {}, 1};
    expect(golden::serialize_instruction(variadic_shape) ==
               "ShapedConstraintCheck{4, 20, [none, 5], 6, 7, ellipsis=1}",
           "ellipsis check rendering: " + golden::serialize_instruction(variadic_shape));
    const ir::Instruction get = ir::ArrayGet{1, 2, 3, Type::simple(TypeKind::Int64), {0, 0},
                                             false, true, std::nullopt, 9};
    expect(golden::serialize_instruction(get) ==
               "ArrayGet{1, 2, 3, " + int_type + ", 0, 0, 0, 1, none, 9}",
           "optional rendering: " + golden::serialize_instruction(get));

    ir::ParallelBlock parallel{};
    parallel.out=1;parallel.table_name="iterations";
    const auto ordinary_parallel=golden::serialize_instruction(parallel);
    parallel.iterations=ir::ParallelIteration{2,3,{{0,4,true,false},{1,5,false,true}}};
    expect(golden::serialize_instruction(parallel).find("iterations={2, 3, [{0, 4, 1, 0}, {1, 5, 0, 1}]}")!=std::string::npos,
           "parallel iteration count, worker guard and receipt contract are serialized");
    parallel.iterations.reset();
    expect(golden::serialize_instruction(parallel)==ordinary_parallel,"ordinary parallel view remains unchanged");
    ir::ArraySet set{};set.element_type=Type::simple(TypeKind::Int64);
    const auto ordinary_set=golden::serialize_instruction(set);
    set.parallel_receipt=9;
    expect(golden::serialize_instruction(set).find("parallel_receipt=9")!=std::string::npos,
           "parallel element initialization receipt is serialized");
    set.parallel_receipt.reset();
    expect(golden::serialize_instruction(set)==ordinary_set,"ordinary array store view remains unchanged");

    ir::Module metadata;
    ir::ClassLayout layout{};
    layout.name = "Holder";
    metadata.classes.push_back(layout);
    ir::Function function{};
    function.name = "f";
    metadata.functions.push_back(function);
    const auto defaults = golden::serialize_module(metadata);
    expect(defaults.find("allocation_next_value 1") != std::string::npos,
           "default allocation metadata is explicitly serialized");
    metadata.functions[0].next_value = std::uint64_t{1} << 32;
    expect(golden::serialize_module(metadata).find("allocation_next_value 4294967296") != std::string::npos,
           "exhausted allocation cursor is serialized without narrowing");
    metadata.functions[0].next_value = 1;
    metadata.classes[0].standard_library = true;
    expect(golden::serialize_module(metadata) == defaults, "class origin remains omitted");
    metadata.classes[0].reference_fields = {0, 2};
    expect(golden::serialize_module(metadata).find("references=[0, 2]") != std::string::npos,
           "class reference field ordinals are serialized");
    metadata.functions[0].parallel_node = ir::ParallelNodeMetadata{"wrapper", {"slot"}, true};
    metadata.functions[0].source_function = "source";
    metadata.functions[0].shape_wrapper = true;
    const auto rendered = golden::serialize_module(metadata);
    expect(rendered.find("parallel_node {\"wrapper\", [\"slot\"], 1}") != std::string::npos,
           "parallel node wrapper, reference slots and fallibility are serialized");
    expect(rendered.find("source_function \"source\"") != std::string::npos,
           "source function identity is serialized");
    expect(rendered.find("shape_wrapper 1") != std::string::npos,
           "shape wrapper behavior is serialized");
    metadata.functions[0].dispatch_stub = true;
    expect(golden::serialize_module(metadata).find("dispatch_stub 1") != std::string::npos,
           "generated dispatch fallback behavior is serialized");
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
