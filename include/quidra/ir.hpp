#pragma once
#include "quidra/checker.hpp"
#include "quidra/types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace quidra::ir {

using ValueId = std::uint32_t;

struct SourceLocation {
    std::uint32_t line{};
    std::uint32_t column{};
    std::string source_file;
    std::string source_revision;
    std::string node_id;
    std::string node_kind;
};
struct ConstantInt { ValueId out; std::string value; Type type; };
struct ConstantFloat { ValueId out; double value; Type type; };
struct ConstantExact { ValueId out; std::string spelling; Type type; };
struct ConstantBool { ValueId out; bool value; };
struct ConstantString { ValueId out; std::string value; };
struct ArrayMake { ValueId out; std::vector<ValueId> elements; Type type; };
struct ArrayAlloc { ValueId out; ValueId length; Type type; bool fully_initialized{}; };
struct ClassMake { ValueId out; Type type; std::vector<std::optional<ValueId>> fields; };
struct FieldGet { ValueId out; ValueId object; std::size_t index; Type field_type; };
struct FieldSet { ValueId object; std::size_t index; ValueId value; Type field_type; bool replace_without_release{}; };
struct DeclareLocal {
    std::string name;
    Type type;
    std::string source_name;
    std::uint32_t source_line{};
    std::uint32_t source_column{};
};
struct DeclareReference { std::string name; Type type; bool is_const{}; };
struct AddressLocal { ValueId out; std::string name; };
struct AddressField { ValueId out; ValueId object; std::size_t index; };
struct AddressElement { ValueId out; ValueId array; ValueId index; Type array_type; Type element_type; bool bin_element{}; std::uint32_t line{}; std::uint32_t column{}; };
struct LoadAddress { ValueId out; ValueId address; Type type; };
struct StoreAddress { ValueId address; ValueId value; Type type; };
struct BindReference { std::string name; ValueId address; };
struct ReferenceAddress { ValueId out; std::string name; };
struct LoadReference { ValueId out; std::string name; Type type; };
struct StoreReference { std::string name; ValueId value; Type type; };
struct ArrayLength { ValueId out; ValueId array; };
struct ArrayCanAppendMove { ValueId out; ValueId array; };
struct ArrayGrowMove { ValueId out; ValueId array; Type array_type; };
struct ArraySorted { ValueId out; ValueId array; Type array_type; std::uint32_t line{}; std::uint32_t column{}; };
struct StringIndex { ValueId out; ValueId text; ValueId index; std::uint32_t line{}; std::uint32_t column{}; };
struct StringIndexAsciiCompare { ValueId out; ValueId text; ValueId index; unsigned char byte{}; bool negate{}; std::uint32_t line{}; std::uint32_t column{}; };
struct StringAsciiCountPrefix {
    ValueId out;
    ValueId text;
    ValueId count;
    ValueId initial;
    unsigned char byte{};
    bool negate{};
    std::uint32_t index_line{};
    std::uint32_t index_column{};
    std::uint32_t overflow_line{};
    std::uint32_t overflow_column{};
};
struct StringLength { ValueId out; ValueId text; };
struct StringEmpty { ValueId out; ValueId text; bool negate{}; };
struct StringContains { ValueId out; ValueId text; ValueId needle; };
struct StringStartsWith { ValueId out; ValueId text; ValueId prefix; };
struct StringEndsWith { ValueId out; ValueId text; ValueId suffix; };
struct StringFind { ValueId out; ValueId text; ValueId needle; Type result_type; };
struct StringSlice { ValueId out; ValueId text; ValueId start; ValueId end; };
struct StringTrim { ValueId out; ValueId text; };
struct StringSplit { ValueId out; ValueId text; ValueId separator; };
struct StringSplitIterBegin { ValueId out; ValueId text; ValueId separator; bool move_source{}; };
struct StringSplitIterNext { ValueId text; ValueId has_value; ValueId cursor; };
struct StringSplitIterEnd { ValueId cursor; };
struct StringParseTwoSigned { ValueId left; ValueId right; ValueId ok; ValueId text; unsigned char separator{}; };
struct StringUtf8 { ValueId out; ValueId text; };
struct StringFromUtf8 { ValueId out; ValueId bin; Type result_type; };
struct StringFromUtf8ArrayDirect { ValueId text; ValueId ok; ValueId error; ValueId array; };
struct StringCodepoints { ValueId out; ValueId text; };
struct StringJoin { ValueId out; ValueId values; ValueId separator; std::uint32_t line{}; std::uint32_t column{}; };
struct StringConcat { ValueId out; std::vector<ValueId> values; };
struct StringBuildPart { ValueId value; Type type; bool single_byte_ascii{}; };
struct StringBuild { ValueId out; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringBuildAppendMove { ValueId out; ValueId added_length; ValueId text; std::vector<StringBuildPart> parts; ValueId separator; };
struct StringCanAppendMove { ValueId out; ValueId text; };
struct StringAppendMove { ValueId out; ValueId text; std::vector<ValueId> suffixes; };
struct StringRepeat { ValueId out; ValueId count; ValueId fill; };
struct BinAlloc { ValueId out; ValueId length; ValueId fill; };
struct BinLength { ValueId out; ValueId bin; };
struct BinGet { ValueId out; ValueId bin; ValueId index; std::uint32_t line{}; std::uint32_t column{}; bool bounds_proven{}; };
struct BinSet { ValueId bin; ValueId index; ValueId value; std::uint32_t line{}; std::uint32_t column{}; bool bounds_proven{}; };
struct BinSlice { ValueId out; ValueId bin; ValueId start; ValueId end; };
struct ParseBin { ValueId out; ValueId text; Type result_type; bool success_proven{}; };
struct BinConvert { ValueId out; ValueId value; Type source_type; Type target_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NumericConvert { ValueId out; ValueId value; Type source_type; Type target_type; bool checked_range{}; std::uint32_t line{}; std::uint32_t column{}; };
struct FallibleNumericConvert { ValueId out; ValueId value; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ArrayNumericCast { ValueId out; ValueId array; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorCreate { ValueId out; ValueId shape; std::optional<ValueId> gpu; Type type; int fill_mode{}; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTransfer { ValueId out; ValueId tensor; std::optional<ValueId> gpu; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorReshape { ValueId out; ValueId tensor; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTranspose { ValueId out; ValueId tensor; ValueId axis0; ValueId axis1; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorContiguous { ValueId out; ValueId tensor; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorGather { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorScatter { ValueId out; ValueId tensor; ValueId indices; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorShape { ValueId out; ValueId tensor; Type type; };
struct TensorDevice { ValueId out; ValueId tensor; };
struct TensorIsContiguous { ValueId out; ValueId tensor; };
struct TensorIsTracked { ValueId out; ValueId tensor; };
struct TensorHasGrad { ValueId out; ValueId tensor; };
struct TensorClearGrad { ValueId tensor; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorItem { ValueId out; ValueId tensor; Type element_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTrack { ValueId out; ValueId tensor; ValueId target{}; Type type; int mode{}; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorBackwardTarget { ValueId value; bool autograd_target{}; };
struct TensorBackward { ValueId tensor; std::vector<TensorBackwardTarget> targets; ValueId autograd_targets{}; ValueId track; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorGrad { ValueId out; ValueId tensor; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorCast { ValueId out; ValueId tensor; Type source_type; Type target_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ShapedConstraintCheck {
    ValueId value;
    TypeKind kind{TypeKind::Tensor};
    std::vector<std::optional<ValueId>> extents;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct ExtentEqualCheck {
    ValueId actual;
    ValueId expected;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorBinary {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
};

struct TensorCompare {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type left_type;
    Type right_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorBoolReduce {
    ValueId out;
    ValueId tensor;
    bool all{};
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorIndexPart {
    bool slice{};
    std::optional<ValueId> index;
    std::optional<ValueId> start;
    std::optional<ValueId> stop;
    std::optional<ValueId> step;
};
struct TensorIndex {
    ValueId out;
    ValueId tensor;
    std::vector<TensorIndexPart> items;
    Type type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct TensorSet {
    ValueId tensor;
    std::vector<ValueId> indices;
    ValueId value;
    Type element_type;
    std::uint32_t line{};
    std::uint32_t column{};
};
struct ParseNumber { ValueId out; ValueId text; Type target_type; Type result_type; };
struct ParseNumberDirect {
    ValueId value_out;
    ValueId ok_out;
    ValueId error_out;
    ValueId text;
    Type target_type;
};
struct ExactAtom {
    ValueId out;
    std::string provider;
    std::uint32_t opcode{};
    Type type;
};
struct ExactUnary {
    ValueId out;
    std::string provider;
    std::uint32_t opcode{};
    ValueId input;
    Type type;
};
struct CliArgument { ValueId out; ValueId name; ValueId index; Type type; };
struct CliArgumentOptional { ValueId out; ValueId name; ValueId index; ValueId default_value; Type type; };
struct CliOption { ValueId out; ValueId name; ValueId default_value; Type type; };
struct CliFlag { ValueId out; ValueId name; };
struct CliFinish {};
struct Flush { ValueId out; Type result_type; };
struct FileOpen { ValueId out; ValueId path; Type result_type; };
struct FileCreate { ValueId out; ValueId path; Type result_type; };
struct FileAppend { ValueId out; ValueId path; Type result_type; };
struct FileHandleRead { ValueId out; ValueId handle; Type result_type; };
struct FileHandleReadLine { ValueId out; ValueId handle; Type result_type; };
struct FileHandleReadBin { ValueId out; ValueId handle; Type result_type; };
struct FileHandleWrite { ValueId out; ValueId handle; ValueId text; Type result_type; bool line{}; };
struct FileHandleFlush { ValueId out; ValueId handle; Type result_type; };
struct FileHandleSeek { ValueId out; ValueId handle; ValueId position; Type result_type; };
struct FileHandleClose { ValueId handle; };
struct FileRead { ValueId out; ValueId path; Type result_type; };
struct FileReadBin { ValueId out; ValueId path; Type result_type; };
struct FileWrite { ValueId out; ValueId path; ValueId text; Type result_type; };
struct FileWriteBin { ValueId out; ValueId path; ValueId bin; Type result_type; };
struct FileExists { ValueId out; ValueId path; Type result_type; };
struct FileIsDirectory { ValueId out; ValueId path; Type result_type; };
struct FileRemove { ValueId out; ValueId path; Type result_type; };
struct FileCopy { ValueId out; ValueId source; ValueId destination; Type result_type; };
struct FileMove { ValueId out; ValueId source; ValueId destination; Type result_type; };
struct FileMkdir { ValueId out; ValueId path; Type result_type; };
struct FileList { ValueId out; ValueId path; ValueId recursive; Type result_type; };
struct EnvironmentGet { ValueId out; ValueId name; Type result_type; };
struct EnvironmentHas { ValueId out; ValueId name; };
struct TestAssert { ValueId condition; };
struct TimeNow { ValueId out; ValueId sync; };
struct TimeSince { ValueId out; ValueId start; ValueId sync; };
struct TimeSeconds { ValueId out; ValueId seconds; };
struct TimeSleep { ValueId duration; std::uint32_t line{}; std::uint32_t column{}; };
struct GpuSync { ValueId index; std::uint32_t line{}; std::uint32_t column{}; };
struct TaskAll { ValueId out{}; ValueId operations{}; ValueId shared{}; Type result_type; Type shared_type; std::uint32_t line{}; std::uint32_t column{}; };
struct AtomicCounterCreate { ValueId out; ValueId initial; };
struct AtomicCounterAdd { ValueId out; ValueId counter; ValueId delta; std::uint32_t line{}; std::uint32_t column{}; };
struct AtomicCounterLoad { ValueId out; ValueId counter; };
struct AutogradTargetCreate { ValueId out; };
struct AutogradTargetHasGrad { ValueId out; ValueId target; };
struct AutogradTargetClearGrad { ValueId target; std::uint32_t line{}; std::uint32_t column{}; };
struct AutogradTargetGradient { ValueId out; ValueId target; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct RandomGenerator { ValueId out; ValueId seed; };
struct RandomInt { ValueId out; ValueId generator; ValueId start; ValueId end; std::uint32_t line{}; std::uint32_t column{}; };
struct RandomFloat { ValueId out; ValueId generator; };
struct RandomBool { ValueId out; ValueId generator; };
struct ProcessRun { ValueId out; ValueId program; ValueId args; };
struct ProcessShell { ValueId out; ValueId command; };
struct JsonParse { ValueId out; ValueId text; Type result_type; };
struct JsonKind { ValueId out; ValueId value; };
struct JsonSize { ValueId out; ValueId value; Type result_type; };
struct JsonGet { ValueId out; ValueId value; ValueId key; Type result_type; };
struct JsonAt { ValueId out; ValueId value; ValueId index; Type result_type; };
struct JsonText { ValueId out; ValueId value; Type result_type; };
struct JsonInteger { ValueId out; ValueId value; Type result_type; };
struct JsonNumber { ValueId out; ValueId value; Type result_type; };
struct JsonBigInt { ValueId out; ValueId value; Type result_type; };
struct JsonBigReal { ValueId out; ValueId value; Type result_type; };
struct JsonBoolean { ValueId out; ValueId value; Type result_type; };
struct JsonEncode { ValueId out; ValueId value; };
struct JsonEqual { ValueId out; ValueId left; ValueId right; };
struct HttpGet { ValueId out; ValueId url; Type result_type; };
struct HttpHeader { ValueId out; ValueId response; ValueId name; Type result_type; };
struct ArrayInitializationComplete { ValueId out; ValueId array; };
struct ArrayGet { ValueId out; ValueId array; ValueId index; Type element_type; std::uint32_t line{}; std::uint32_t column{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; };
struct ArraySet { ValueId array; ValueId index; ValueId value; Type element_type; std::uint32_t line{}; std::uint32_t column{}; bool initialization_proven{}; bool bounds_proven{}; std::optional<ValueId> initialization_guard{}; std::optional<ValueId> bounds_guard{}; };
struct Clone { ValueId out; ValueId value; Type type; };
struct Retain { ValueId out; ValueId value; Type type; };
struct Release { ValueId value; Type type; };
struct Unary { ValueId out; std::string op; ValueId operand; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct Binary {
    ValueId out;
    std::string op;
    ValueId left;
    ValueId right;
    Type operand_type;
    Type result_type;
    std::uint32_t line{};
    std::uint32_t column{};
    bool overflow_proven{};

    Binary(ValueId out_value, std::string operation, ValueId left_value,
           ValueId right_value, Type operand, Type result,
           std::uint32_t source_line = 0,
           std::uint32_t source_column = 0,
           bool proven_no_overflow = false)
        : out(out_value), op(operation), left(left_value), right(right_value),
          operand_type(operand), result_type(result), line(source_line),
          column(source_column), overflow_proven(proven_no_overflow) {}
};
struct ToString { ValueId out; ValueId value; Type source_type; };
struct FormatNumber {
    ValueId out;
    ValueId value;
    Type source_type;
    std::optional<std::uint32_t> integer_width;
    std::optional<std::uint32_t> fractional_digits;
    std::optional<std::uint32_t> significant_digits;
    bool zero{};
};
struct LoadLocal { ValueId out; std::string name; Type type; };
struct StoreLocal {
    std::string name;
    ValueId value;
    Type type;
    bool borrowed{};
    bool replace_without_release{};
};
struct CallArgument { ValueId value; std::optional<ValueId> writable_address; };
struct FunctionRef { ValueId out; std::string function; Type type; };
struct IndirectCall { ValueId out; ValueId callee; std::vector<ValueId> args; std::vector<Type> parameter_types; Type result; std::uint32_t line{}; std::uint32_t column{}; };
struct Call {
    ValueId out;
    std::string callee;
    std::vector<CallArgument> args;
    Type result;
    std::uint32_t line{};
    std::uint32_t column{};
    bool no_normal_return{};
};
struct VariantMake { ValueId out; int tag; ValueId payload; Type container_type; Type payload_type; };
struct VariantTag { ValueId out; ValueId container; };
struct VariantPayload { ValueId out; ValueId container; Type payload_type; };
// print/flush report output failure as an error alternative: the result is
// void | error, and a discarded error fails fast at the statement.
struct Print { ValueId value; Type type; ValueId out; Type result_type; };
struct ReplDisplay {
    ValueId value;
    Type type;
    std::vector<std::string> initialized_paths;
};
struct ReplReplayMode { bool active{}; };
struct Input { ValueId out; Type result_type; };
struct Exit { ValueId status; };
struct FailError { ValueId error; std::uint32_t line{}; std::uint32_t column{}; };
struct RangeCheckStep { ValueId step; std::uint32_t line{}; std::uint32_t column{}; };
struct Return { ValueId value; Type type; };
struct ReturnVoid {};
struct Jump { std::string target; };
struct Branch { ValueId condition; std::string if_true; std::string if_false; };

using Instruction = std::variant<SourceLocation, ConstantInt, ConstantFloat, ConstantExact, ConstantBool, ConstantString,
                                 ArrayMake, ArrayAlloc, ClassMake, FieldGet, FieldSet,
                                 DeclareLocal, DeclareReference, AddressLocal, AddressField, AddressElement,
                                 LoadAddress, StoreAddress, BindReference, ReferenceAddress, LoadReference, StoreReference,
                                 ArrayLength, ArrayCanAppendMove, ArrayGrowMove, ArraySorted,
                                 ArrayInitializationComplete,
                                 StringIndex, StringIndexAsciiCompare, StringAsciiCountPrefix, StringLength, StringEmpty, StringContains, StringStartsWith,
                                 StringEndsWith, StringFind, StringSlice, StringTrim, StringSplit,
                                 StringSplitIterBegin, StringSplitIterNext, StringSplitIterEnd,
                                 StringParseTwoSigned, StringUtf8, StringFromUtf8, StringFromUtf8ArrayDirect, StringCodepoints, StringJoin, StringConcat, StringBuild,
                                 StringBuildAppendMove, StringCanAppendMove, StringAppendMove, StringRepeat,
                                 BinAlloc, BinLength, BinGet, BinSet, BinSlice,
                                 ParseBin, BinConvert,
                                 NumericConvert, FallibleNumericConvert, ArrayNumericCast, TensorCreate, TensorTransfer, TensorReshape, TensorTranspose, TensorContiguous, TensorGather, TensorScatter, 
                                 TensorShape, TensorDevice, TensorIsContiguous, TensorIsTracked, TensorHasGrad, TensorClearGrad, TensorItem, TensorTrack, TensorBackward, TensorGrad, TensorCast,
                                 ShapedConstraintCheck, ExtentEqualCheck,
                                 TensorBinary, TensorCompare, TensorBoolReduce, TensorIndex, TensorSet, ParseNumber, ParseNumberDirect, ExactAtom, ExactUnary,
                                 CliArgument, CliArgumentOptional, CliOption, CliFlag, CliFinish, Flush,
                                 FileOpen, FileCreate, FileAppend, FileHandleRead, FileHandleReadLine, FileHandleReadBin, FileHandleWrite, FileHandleFlush, FileHandleSeek, FileHandleClose,
                                 FileRead, FileReadBin, FileWrite, FileWriteBin, FileExists, FileIsDirectory, FileRemove, FileCopy, FileMove, FileMkdir, FileList,
                                 EnvironmentGet, EnvironmentHas, TestAssert,
                                 TimeNow, TimeSince, TimeSeconds, TimeSleep, GpuSync, TaskAll,
                                 AtomicCounterCreate, AtomicCounterAdd, AtomicCounterLoad,
                                 AutogradTargetCreate, AutogradTargetHasGrad, AutogradTargetClearGrad, AutogradTargetGradient,
                                 RandomGenerator, RandomInt, RandomFloat, RandomBool, ProcessRun, ProcessShell,
                                 JsonParse, JsonKind, JsonSize, JsonGet, JsonAt, JsonText,
                                 JsonInteger, JsonNumber, JsonBigInt, JsonBigReal, JsonBoolean, JsonEncode, JsonEqual,
                                 HttpGet, HttpHeader,
                                 ArrayGet, ArraySet, Clone, Retain, Release,
                                 Unary, Binary, ToString, FormatNumber, LoadLocal, StoreLocal,
                                 FunctionRef, IndirectCall, Call, VariantMake, VariantTag, VariantPayload,
                                 Print, ReplDisplay, ReplReplayMode, Input, Exit, FailError, RangeCheckStep, Return, ReturnVoid, Jump, Branch>;

struct Block { std::string label; std::vector<Instruction> instructions; };
struct TensorRegionLocation {
    std::size_t block{};
    std::size_t instruction{};
};
struct TensorRegion {
    // Candidate computation region only. It carries no source-visible semantics
    // and does not authorize reordering across explicit placement/tracking/effect
    // boundaries. Later fusion/AD passes may refine a region conservatively.
    std::vector<TensorRegionLocation> instructions;
    std::vector<ValueId> external_inputs;
    std::vector<ValueId> values;
    bool reaches_backward{};
    bool may_require_higher_order{};
    std::vector<std::string> compiler_extensions;
    // Fully qualified descriptor table references (extension:table). Core
    // schedules these generically; package-owned compiler logic interprets
    // their domain semantics.
    std::vector<std::string> compiler_extension_tables;
    // Opaque package operation IDs in region execution order. Core never
    // assigns domain meaning to these IDs.
    std::vector<std::string> compiler_operations;
    // Descriptor-owned fusion tables whose opaque operation sequence matches
    // this region. These are candidates only; package policy owns validity and
    // lowering.
    std::vector<std::string> compiler_fusion_candidates;
};
struct Parameter {
    std::string name;
    Type type;
    bool writable{};
    bool borrowed{};
    bool is_const{};
};
struct Function {
    std::string name;
    std::string source_file;
    std::uint32_t source_line{1};
    std::uint32_t source_column{1};
    std::vector<Parameter> parameters;
    Type result{Type::simple(TypeKind::Void)};
    std::vector<Block> blocks;
    std::vector<TensorRegion> tensor_regions;
    bool entrypoint{};
    std::optional<std::string> external_symbol;
};
struct ClassLayout {
    std::string name;
    std::vector<std::string> field_names;
    std::vector<Type> fields;
};
struct Module {
    std::vector<ClassLayout> classes;
    std::vector<Function> functions;
    std::vector<CompilerExtensionRegistration> compiler_extensions;
};

Module lower(
    const CheckedProgram& checked,
    const Expr* repl_expression = nullptr,
    std::size_t replay_prefix_bytes = 0);
Module optimize(Module module);
std::string dump(const Module& module);

} // namespace quidra::ir
