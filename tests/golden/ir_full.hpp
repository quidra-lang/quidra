// Field-complete serialization of the typed IR (the golden view `ir.full`).
//
// `quidra ir` omits fields, types and whole instructions (README.md, Views),
// so it cannot prove that a refactoring leaves the typed IR unchanged. This
// serializer prints every field of every instruction, block, function, region,
// class layout and compiler extension, and the full recursive Type.
//
// No macros and no hand-written field lists: the field count of an aggregate is
// detected by brace-constructibility, and the fields are read through a
// structured-binding ladder. A structured binding with the wrong count is a
// compile error, so a struct that gains a field either prints it or stops the
// build; tests/golden/ir_full_tests.cpp pins every detected count.
//
// The output is representation independent: enums print as integers, bools as
// 0/1, floating values as their bit pattern, optionals as `none` or the value,
// and strings escaped. A field rename or an int -> enum change prints the same.
#pragma once

#include "quidra/ir.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace quidra::golden {

// The variant's alternatives in variant order. The arity test checks each
// name against the compiler's spelling of the alternative's type.
inline constexpr std::array<std::string_view, 189> instruction_names{
    "SourceLocation", "ConstantInt", "ConstantFloat", "ConstantExact", "ConstantBool",
    "ConstantString", "ArrayMake", "ArrayAlloc", "ClassMake", "FieldGet", "FieldSet",
    "DeclareLocal", "DeclareReference", "AddressLocal", "AddressField", "AddressElement",
    "LoadAddress", "StoreAddress", "BindReference", "ReferenceAddress", "LoadReference",
    "StoreReference", "ArrayLength", "ArrayCanAppendMove", "ArrayGrowMove", "ArraySorted",
    "ArrayInitializationComplete", "StringIndex", "StringIndexAsciiCompare",
    "StringAsciiCountPrefix", "StringLength", "StringEmpty", "StringContains",
    "StringStartsWith", "StringEndsWith", "StringFind", "StringSlice", "StringTrim",
    "StringSplit", "StringSplitIterBegin", "StringSplitIterNext", "StringSplitIterEnd",
    "StringParseTwoSigned", "StringUtf8", "StringFromUtf8", "StringFromUtf8ArrayDirect",
    "StringCodepoints", "StringJoin", "StringConcat", "StringBuild", "StringBuildAppendMove",
    "StringCanAppendMove", "StringAppendMove", "StringRepeat", "BinAlloc", "BinLength",
    "BinGet", "BinSet", "BinSlice", "ParseBin", "BinConvert", "NumericConvert",
    "FallibleNumericConvert", "ArrayNumericCast", "TensorCreate", "TensorTransfer",
    "TensorReshape", "TensorTranspose", "TensorContiguous", "TensorGather", "TensorScatter",
    "TensorShape", "TensorDevice", "TensorIsContiguous", "TensorIsTracked", "TensorHasGrad",
    "TensorClearGrad", "TensorItem", "TensorTrack", "TensorBackward", "TensorGrad",
    "TensorCast", "ShapedConstraintCheck", "ExtentEqualCheck", "TensorBinary", "TensorCompare",
    "TensorBoolReduce", "TensorIndex", "TensorSet", "ParseNumber", "ParseNumberDirect",
    "ExactAtom", "ExactUnary", "CliArgument", "CliArgumentOptional", "CliOption", "CliFlag",
    "CliFinish", "Flush", "FileOpen", "FileCreate", "FileAppend", "FileHandleRead",
    "FileHandleReadLine", "FileHandleReadBin", "FileHandleWrite", "FileHandleFlush",
    "FileHandleSeek", "FileHandleClose", "FileRead", "FileReadBin", "FileWrite",
    "FileWriteBin", "FileExists", "FileIsDirectory", "FileRemove", "FileCopy", "FileMove",
    "FileMkdir", "FileList", "EnvironmentGet", "EnvironmentHas", "TestAssert", "TimeNow",
    "TimeSince", "TimeSeconds", "TimeSleep", "GpuSync", "TaskAll", "AtomicCounterCreate",
    "AtomicCounterAdd", "AtomicCounterLoad", "AutogradTargetCreate", "AutogradTargetHasGrad",
    "AutogradTargetClearGrad", "AutogradTargetGradient", "RandomGenerator", "RandomInt",
    "RandomFloat", "RandomBool", "ProcessRun", "ProcessShell", "JsonParse", "JsonKind",
    "JsonSize", "JsonGet", "JsonAt", "JsonText", "JsonInteger", "JsonNumber", "JsonBigInt",
    "JsonBigReal", "JsonBoolean", "JsonEncode", "JsonEqual", "HttpGet", "HttpHeader",
    "ArrayGet", "ArraySet", "Clone", "Retain", "Release", "Unary", "Binary", "ToString",
    "FormatNumber", "LoadLocal", "StoreLocal", "FunctionRef", "IndirectCall", "Call",
    "VariantMake", "VariantTag", "VariantPayload", "Print", "ReplDisplay", "ReplReplayMode",
    "Input", "Exit", "FailError", "RangeCheckStep", "Return", "ReturnVoid", "Jump", "Branch",
    "Pin", "Unpin", "IterationShapeCheck", "InitializedCheck"};
static_assert(instruction_names.size() == std::variant_size_v<ir::Instruction>,
              "every Instruction alternative needs a name in instruction_names");

// Aggregates with more fields than this stop the build in the binding ladder.
inline constexpr std::size_t max_aggregate_arity = 12;

namespace detail {

// Converts to any field type. The rvalue ref-qualifier keeps the conversion
// from competing with converting constructor templates such as optional(U&&).
template <std::size_t>
struct AnyField {
    template <class T>
    operator T() const&&;
};

template <class T, std::size_t... I>
constexpr bool brace_constructible(std::index_sequence<I...>) {
    return requires { T{AnyField<I>{}...}; };
}

template <class T, std::size_t N>
constexpr std::size_t aggregate_arity_from() {
    if constexpr (N == 0) {
        return 0;
    } else if constexpr (brace_constructible<T>(std::make_index_sequence<N>{})) {
        return N;
    } else {
        return aggregate_arity_from<T, N - 1>();
    }
}

} // namespace detail

// Number of fields of an aggregate, found by brace-constructibility. The search
// starts above max_aggregate_arity so that a struct that grows past the ladder
// is detected and rejected instead of silently truncated.
template <class T>
constexpr std::size_t aggregate_arity() {
    return detail::aggregate_arity_from<T, max_aggregate_arity + 4>();
}

// The `ir.full` view of a module.
std::string serialize_module(const ir::Module& module);

// One instruction as `<name>{fields}`.
std::string serialize_instruction(const ir::Instruction& instruction);

// The full recursive Type.
std::string serialize_type(const Type& type);

// Quoted, escaped rendering of a byte string (printable ASCII kept, every other
// byte as \xHH).
std::string quote(std::string_view text);

} // namespace quidra::golden
