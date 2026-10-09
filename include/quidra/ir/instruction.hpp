#pragma once

// Instruction: one typed-IR instruction, a variant over the structs of every
// domain header. The alternatives keep their order, which is part of the data
// model (variant indices). The folds at the end prove that every alternative
// is classified, that every domain is below domain_count, and that every
// domain below it has an instruction.

#include "quidra/ir/aggregate.hpp"
#include "quidra/ir/autograd.hpp"
#include "quidra/ir/bin.hpp"
#include "quidra/ir/call.hpp"
#include "quidra/ir/check.hpp"
#include "quidra/ir/concurrency.hpp"
#include "quidra/ir/console.hpp"
#include "quidra/ir/constant.hpp"
#include "quidra/ir/control_flow.hpp"
#include "quidra/ir/conversion.hpp"
#include "quidra/ir/debug.hpp"
#include "quidra/ir/domain.hpp"
#include "quidra/ir/file.hpp"
#include "quidra/ir/http.hpp"
#include "quidra/ir/json.hpp"
#include "quidra/ir/lifetime.hpp"
#include "quidra/ir/memory.hpp"
#include "quidra/ir/numeric.hpp"
#include "quidra/ir/random.hpp"
#include "quidra/ir/repl.hpp"
#include "quidra/ir/system.hpp"
#include "quidra/ir/tensor.hpp"
#include "quidra/ir/text.hpp"

#include <cstddef>
#include <utility>
#include <variant>

namespace quidra::ir {

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
                                 Print, ReplDisplay, ReplReplayMode, Input, Exit, FailError, RangeCheckStep, Return, ReturnVoid, Jump, Branch,
                                 Pin, Unpin, IterationShapeCheck, InitializedCheck>;

namespace detail {

using Alternatives = std::make_index_sequence<std::variant_size_v<Instruction>>;

template <std::size_t... I>
constexpr bool every_instruction_classified(std::index_sequence<I...>) {
    return (InstructionTraits<std::variant_alternative_t<I, Instruction>>::classified && ...);
}

// The domain index of an alternative (0 for an unclassified one, which the
// first assertion reports).
template <class T>
constexpr std::size_t domain_index() {
    if constexpr (InstructionTraits<T>::classified) {
        return static_cast<std::size_t>(InstructionTraits<T>::domain);
    } else {
        return 0;
    }
}

template <std::size_t... I>
constexpr bool every_domain_below_count(std::index_sequence<I...>) {
    return ((domain_index<std::variant_alternative_t<I, Instruction>>() < domain_count) && ...);
}

template <std::size_t... I>
constexpr bool domain_has_instruction(std::size_t domain, std::index_sequence<I...>) {
    return ((domain_index<std::variant_alternative_t<I, Instruction>>() == domain) || ...);
}

template <std::size_t... D>
constexpr bool every_domain_has_instruction(std::index_sequence<D...>) {
    return (domain_has_instruction(D, Alternatives{}) && ...);
}

} // namespace detail

static_assert(detail::every_instruction_classified(detail::Alternatives{}),
              "every Instruction alternative needs an InstructionTraits specialization");
static_assert(detail::every_domain_below_count(detail::Alternatives{}),
              "an instruction's domain is not below domain_count (quidra/ir/domain.hpp)");
static_assert(detail::every_domain_has_instruction(std::make_index_sequence<domain_count>{}),
              "every domain below domain_count must classify at least one instruction");

} // namespace quidra::ir
