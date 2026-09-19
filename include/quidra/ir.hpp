#pragma once
#include "quidra/checker.hpp"
#include "quidra/types.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace quidra::ir {

using ValueId = std::uint32_t;

struct SourceLocation { std::uint32_t line{}; std::uint32_t column{}; };
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
struct ArrayNumericCast { ValueId out; ValueId array; Type source_type; Type target_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorCreate { ValueId out; ValueId shape; std::optional<ValueId> gpu; Type type; int fill_mode{}; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTransfer { ValueId out; ValueId tensor; std::optional<ValueId> gpu; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorReshape { ValueId out; ValueId tensor; ValueId shape; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorTranspose { ValueId out; ValueId tensor; ValueId axis0; ValueId axis1; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorContiguous { ValueId out; ValueId tensor; Type type; };
struct TensorShape { ValueId out; ValueId tensor; Type type; };
struct TensorIsContiguous { ValueId out; ValueId tensor; };
struct TensorItem { ValueId out; ValueId tensor; Type element_type; std::uint32_t line{}; std::uint32_t column{}; };
struct TensorCast { ValueId out; ValueId tensor; Type source_type; Type target_type; std::uint32_t line{}; std::uint32_t column{}; };
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
struct NeuralNumericCast { ValueId out; ValueId value; Type source_type; Type target_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralTrack { ValueId out; ValueId tensor; Type type; bool parameter{}; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralUntrack { ValueId out; ValueId value; Type type; };
struct NeuralUnary { ValueId out; ValueId value; Type type; BuiltinCallable operation; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralBinary { ValueId out; std::string op; ValueId left; ValueId right; Type left_type; Type right_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralGrad { ValueId out; ValueId loss; Type loss_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralAffine { ValueId out; ValueId input; ValueId weight; ValueId bias; Type input_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralConvolve2D { ValueId out; ValueId input; ValueId weight; ValueId bias; ValueId stride; ValueId padding; Type input_type; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralParameterRef { std::string path; ValueId value; };
struct NeuralUpdate { std::vector<NeuralParameterRef> parameters; ValueId gradients; ValueId rate; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralNormalize { ValueId out; ValueId input; ValueId scale; ValueId bias; ValueId running_mean; ValueId running_variance; ValueId momentum; ValueId epsilon; Type result_type; bool training{}; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralRandomMask { ValueId out; ValueId input; ValueId state; ValueId rate; Type result_type; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralMomentUpdate { std::vector<NeuralParameterRef> parameters; ValueId rate; ValueId beta1; ValueId beta2; ValueId epsilon; ValueId step; ValueId moments; ValueId gradients; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralStateValue { std::string path; ValueId value; Type type; };
struct NeuralStateTarget { std::string path; ValueId address; Type type; };
struct NeuralSave { ValueId path; std::string schema; std::vector<NeuralStateValue> values; std::uint32_t line{}; std::uint32_t column{}; };
struct NeuralLoad { ValueId path; std::string schema; std::vector<NeuralStateTarget> targets; std::uint32_t line{}; std::uint32_t column{}; };
struct StatsMean { ValueId out; ValueId tensor; Type tensor_type; std::uint32_t line{}; std::uint32_t column{}; };
struct StatsReduce { ValueId out; ValueId tensor; Type element_type; BuiltinCallable operation; std::uint32_t line{}; std::uint32_t column{}; };
struct LinearMatmul { ValueId out; ValueId left; ValueId right; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct LinearDot { ValueId out; ValueId left; ValueId right; Type element_type; std::uint32_t line{}; std::uint32_t column{}; };
struct ImageRead {
    ValueId out;
    ValueId path;
    Type result_type;
    std::optional<Type> target_dtype;
    std::optional<ValueId> target_channels;
    std::vector<long long> expected_shape_prefix;
};
struct ImageWrite { ValueId out; ValueId path; ValueId image; ValueId quality; Type result_type; };
struct ImageTensorOp {
    ValueId out;
    BuiltinCallable operation;
    std::vector<ValueId> args;
    Type result_type;
    Type element_type;
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
struct NumericAbs { ValueId out; ValueId value; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct Sqrt { ValueId out; ValueId value; Type type; std::uint32_t line{}; std::uint32_t column{}; };
struct MathUnary { ValueId out; ValueId value; Type type; BuiltinCallable operation; };
struct MathRoundInt { ValueId out; ValueId value; Type source_type; Type result_type; BuiltinCallable operation; std::uint32_t line{}; std::uint32_t column{}; };
struct MathPow { ValueId out; ValueId base; ValueId exponent; Type type; };
struct CliArgument { ValueId out; ValueId name; ValueId index; Type type; };
struct CliOption { ValueId out; ValueId name; ValueId default_value; Type type; };
struct CliFlag { ValueId out; ValueId name; };
struct CliFinish {};
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
struct TimeNow { ValueId out; };
struct TimeSince { ValueId out; ValueId start; };
struct TimeSeconds { ValueId out; ValueId seconds; };
struct TimeSleep { ValueId duration; std::uint32_t line{}; std::uint32_t column{}; };
struct TaskAll { ValueId operations; std::uint32_t line{}; std::uint32_t column{}; };
struct RandomGenerator { ValueId out; ValueId seed; };
struct RandomInt { ValueId out; ValueId generator; ValueId start; ValueId end; std::uint32_t line{}; std::uint32_t column{}; };
struct RandomFloat { ValueId out; ValueId generator; };
struct RandomBool { ValueId out; ValueId generator; };
struct ProcessRun { ValueId out; ValueId program; ValueId args; };
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
struct NumericMinMax { ValueId out; ValueId left; ValueId right; Type type; bool maximum{}; };
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
struct Call { ValueId out; std::string callee; std::vector<CallArgument> args; Type result; std::uint32_t line{}; std::uint32_t column{}; };
struct VariantMake { ValueId out; int tag; ValueId payload; Type container_type; Type payload_type; };
struct VariantTag { ValueId out; ValueId container; };
struct VariantPayload { ValueId out; ValueId container; Type payload_type; };
struct Print { ValueId value; Type type; };
struct Write { ValueId value; Type type; };
struct ReplDisplay {
    ValueId value;
    Type type;
    std::vector<std::string> initialized_paths;
};
struct ReplReplayMode { bool active{}; };
struct Input { ValueId out; Type result_type; };
struct Exit { ValueId status; };
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
                                 NumericConvert, ArrayNumericCast, TensorCreate, TensorTransfer, TensorReshape, TensorTranspose, TensorContiguous,
                                 TensorShape, TensorIsContiguous, TensorItem, TensorCast,
                                 ShapedConstraintCheck, ExtentEqualCheck, NeuralNumericCast,
                                 NeuralTrack, NeuralUntrack, NeuralUnary, NeuralBinary, NeuralGrad,
                                 NeuralAffine, NeuralConvolve2D, NeuralUpdate, NeuralNormalize,
                                 NeuralRandomMask, NeuralMomentUpdate,
                                 NeuralSave, NeuralLoad,
                                 StatsMean, StatsReduce, LinearMatmul, LinearDot, ImageRead, ImageWrite, ImageTensorOp, TensorBinary, TensorIndex, TensorSet, ParseNumber, ParseNumberDirect, NumericAbs, Sqrt, MathUnary, MathRoundInt, MathPow,
                                 CliArgument, CliOption, CliFlag, CliFinish,
                                 FileRead, FileReadBin, FileWrite, FileWriteBin, FileExists, FileIsDirectory, FileRemove, FileCopy, FileMove, FileMkdir, FileList,
                                 EnvironmentGet, EnvironmentHas, TestAssert,
                                 TimeNow, TimeSince, TimeSeconds, TimeSleep, TaskAll,
                                 RandomGenerator, RandomInt, RandomFloat, RandomBool, ProcessRun,
                                 JsonParse, JsonKind, JsonSize, JsonGet, JsonAt, JsonText,
                                 JsonInteger, JsonNumber, JsonBigInt, JsonBigReal, JsonBoolean, JsonEncode, JsonEqual,
                                 HttpGet, HttpHeader,
                                 NumericMinMax, ArrayGet, ArraySet, Clone, Retain, Release,
                                 Unary, Binary, ToString, FormatNumber, LoadLocal, StoreLocal,
                                 FunctionRef, IndirectCall, Call, VariantMake, VariantTag, VariantPayload,
                                 Print, Write, ReplDisplay, ReplReplayMode, Input, Exit, RangeCheckStep, Return, ReturnVoid, Jump, Branch>;

struct Block { std::string label; std::vector<Instruction> instructions; };
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
};

Module lower(
    const CheckedProgram& checked,
    const Expr* repl_expression = nullptr,
    std::size_t replay_prefix_bytes = 0);
std::string dump(const Module& module);

} // namespace quidra::ir
