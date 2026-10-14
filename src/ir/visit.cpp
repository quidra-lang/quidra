#include "quidra/ir/visit.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/owned_result_contracts.hpp"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::ir {
namespace detail {

template<class T> struct OperandMapPresent : std::false_type {};

struct ScopedOperandContext {
    OperandVisitor& visitor;
    const OperandContext* prior;
    ScopedOperandContext(OperandVisitor& v, const OperandContext& context)
        : visitor(v), prior(v.context_) { v.context_ = &context; }
    ~ScopedOperandContext() { visitor.context_ = prior; }
};

// Only typed ownership-sensitive fields use this predicate. Ordinary reads
// remain SSA observations and do not imply a borrowing/consumption proof.
bool usable_type(const Type& type) {
    if (type.kind == TypeKind::Invalid || type.kind == TypeKind::Auto ||
        type.kind == TypeKind::Void || type.kind == TypeKind::Never) return false;
    if ((type.kind == TypeKind::Array || type.kind == TypeKind::Tensor ||
         type.kind == TypeKind::Function || type.kind == TypeKind::Traversal) && !type.first) return false;
    if (type.kind == TypeKind::Class && type.class_name.empty()) return false;
    if (type.first && type.kind != TypeKind::Function && !usable_type(*type.first)) return false;
    for (const auto& child : type.parameters) if (!usable_type(child)) return false;
    for (const auto& child : type.cases) {
        if (child.kind != TypeKind::Void && child.kind != TypeKind::None && !usable_type(child)) return false;
    }
    return true;
}
void typed(OperandVisitor& visitor, ValueId value, const Type& type, bool consume) {
    const bool known = usable_type(type);
    visitor.use(value, {known && llvm_backend::node_owns_value(type)
        ? (consume ? OperandRole::Consume : OperandRole::Borrow) : OperandRole::Read, known});
}
void role(OperandVisitor& visitor, ValueId value, OperandRole kind, bool available = true) {
    visitor.use(value, {kind, available});
}
void role(OperandVisitor& visitor, const std::optional<ValueId>& value, OperandRole kind) {
    if (value) role(visitor, *value, kind);
}
void role(OperandVisitor& visitor, const std::vector<ValueId>& values, OperandRole kind) {
    for (const auto value : values) role(visitor, value, kind);
}
void present_role(OperandVisitor& visitor, ValueId value, OperandRole kind) {
    if (value != 0) role(visitor, value, kind);
}
const Function* target(OperandVisitor& visitor, std::string_view name) {
    if (!visitor.context() || !visitor.context()->module) return nullptr;
    const Function* found = nullptr;
    for (const auto& candidate : visitor.context()->module->functions) {
        if (candidate.name != name) continue;
        if (found) return nullptr; // Ambiguous symbols never prove a contract.
        found = &candidate;
    }
    return found;
}
void call(OperandVisitor& visitor, std::string_view name,
          const std::vector<CallArgument>& args, const Type* result) {
    const auto* function = target(visitor, name);
    bool known = function && args.size() == function->parameters.size() &&
        (!result || *result == function->result);
    llvm_backend::NodeCallContract contract;
    if (known) {
        for (const auto& parameter : function->parameters)
            if (!usable_type(parameter.type)) known = false;
    }
    if (known) {
        Call synthetic{};
        synthetic.args = args;
        try { contract = llvm_backend::node_call_contract(synthetic, *function); }
        catch (const std::logic_error&) { known = false; }
    }
    std::size_t owning = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& argument = args[i];
        if (argument.value != 0) {
            if (!known || function->parameters[i].writable) {
                role(visitor, argument.value, OperandRole::Read, false);
            } else {
                const bool consumed = owning < contract.owning_arguments.size() &&
                    contract.owning_arguments[owning] == i;
                if (consumed) ++owning;
                typed(visitor, argument.value, function->parameters[i].type, consumed);
            }
        } else if (known && owning < contract.owning_arguments.size() && contract.owning_arguments[owning] == i) {
            ++owning;
        }
        role(visitor, argument.writable_address, OperandRole::Address);
    }
}
void indirect(OperandVisitor& visitor, const IndirectCall& node) {
    role(visitor, node.callee, OperandRole::Read);
    const bool arity = node.args.size() == node.parameter_types.size();
    for (std::size_t i = 0; i < node.args.size(); ++i) {
        if (arity) typed(visitor, node.args[i], node.parameter_types[i], true);
        else role(visitor, node.args[i], OperandRole::Read, false);
    }
}
void tuple(OperandVisitor& visitor, const TupleMake& node) {
    const bool known = node.type.kind == TypeKind::Tuple &&
        node.elements.size() == node.type.parameters.size();
    for (std::size_t i = 0; i < node.elements.size(); ++i) {
        if (known) typed(visitor, node.elements[i], node.type.parameters[i], true);
        else role(visitor, node.elements[i], OperandRole::Read, false);
    }
}
void array(OperandVisitor& visitor, const ArrayMake& node) {
    for (const auto value : node.elements) {
        if (node.type.kind == TypeKind::Array && node.type.first) typed(visitor, value, *node.type.first, true);
        else role(visitor, value, OperandRole::Read, false);
    }
}
void class_fields(OperandVisitor& visitor, const ClassMake& node) {
    const ClassLayout* layout = nullptr;
    bool ambiguous = false;
    if (node.type.kind == TypeKind::Class && visitor.context() && visitor.context()->module) {
        for (const auto& candidate : visitor.context()->module->classes) {
            if (candidate.name != node.type.class_name) continue;
            if (layout) ambiguous = true;
            layout = &candidate;
        }
    }
    // Lowering may append a hidden initialization-mask field to the layout;
    // ClassMake supplies the user fields and the emitter initializes the mask.
    const bool known = layout && !ambiguous && node.fields.size() <= layout->fields.size();
    for (std::size_t i = 0; i < node.fields.size(); ++i) {
        if (!node.fields[i]) continue;
        if (known && !layout->reference_fields.contains(i)) typed(visitor, *node.fields[i], layout->fields[i], true);
        else role(visitor, *node.fields[i], OperandRole::Read, false);
    }
}

void read(OperandVisitor& visitor, ValueId value) { visitor.use(value, {OperandRole::Read, true}); }
void read(OperandVisitor& visitor, const std::optional<ValueId>& value) {
    if (value) visitor.use(*value, {OperandRole::Read, true});
}
void read(OperandVisitor& visitor, const std::vector<ValueId>& values) {
    for (const auto value : values) visitor.use(value, {OperandRole::Read, true});
}
void read(OperandVisitor& visitor, const std::vector<std::optional<ValueId>>& values) {
    for (const auto& value : values) read(visitor, value);
}
void read(OperandVisitor& visitor, const SliceBounds& bounds) {
    read(visitor, bounds.start); read(visitor, bounds.end); read(visitor, bounds.step);
}
void read(OperandVisitor& visitor, const std::vector<StringBuildPart>& parts) {
    for (const auto& part : parts) typed(visitor, part.value, part.type, false);
}
void read(OperandVisitor& visitor, const std::vector<TensorIndexPart>& parts) {
    for (const auto& part : parts) {
        read(visitor, part.index); read(visitor, part.start);
        read(visitor, part.stop); read(visitor, part.step);
    }
}
void read(OperandVisitor& visitor, const CheckedReal& value) {
    visitor.use(value.value, {OperandRole::Read, true});
    // The operation's inputs describe provenance. Only division reads an
    // input at this check, to distinguish a zero divisor when reporting.
    if (value.cause == FiniteCause::division) visitor.use(value.right, {OperandRole::Read, true});
}
void read(OperandVisitor& visitor, const std::vector<CheckedReal>& values) {
    for (const auto& value : values) read(visitor, value);
}
template<class... T>
void reads(OperandVisitor& visitor, const T&... values) { (read(visitor, values), ...); }
void present(OperandVisitor& visitor, ValueId value) {
    if (value != 0) visitor.use(value, {OperandRole::Read, true});
}
void present_definition(OperandVisitor& visitor, ValueId value) {
    if (value != 0) visitor.definition(value);
}

} // namespace detail

using detail::reads;
using detail::role;
using detail::typed;
using detail::present_role;
using detail::present;
using detail::present_definition;

// Each map is an explicit semantic contract. The coverage marker makes a new
// Instruction alternative a compile error here until its roles are authored.
// Metadata has no generic integer/string fallback. Diagnostic CheckedReal
// inputs are not SSA uses unless the check actually needs them for reporting.
#define OPERANDS(T, ...) \
    template<> struct detail::OperandMapPresent<T> : std::true_type {}; \
    template<> void WithOperands<InstructionTraits<T>::domain, T>::visit_operands( \
        const T& n, OperandVisitor& v) { __VA_ARGS__ }

OPERANDS(SourceLocation, (void)n; (void)v;)
OPERANDS(StatementBoundary, (void)n; (void)v;)
OPERANDS(InlineScopeBegin, (void)n; (void)v;)
OPERANDS(InlineScopeCleanup, (void)n; (void)v;)
OPERANDS(InlineScopeEnd, (void)n; (void)v;)
OPERANDS(InlineBindAddress, role(v, n.address, OperandRole::Address); v.local(n.name, LocalRole::Declaration);)
OPERANDS(InlineExit, reads(v, n.status);)
OPERANDS(ConstantInt, v.definition(n.out);)
OPERANDS(ConstantFloat, v.definition(n.out);)
OPERANDS(ConstantExact, v.definition(n.out);)
OPERANDS(ConstantBool, v.definition(n.out);)
OPERANDS(ConstantString, v.definition(n.out);)

OPERANDS(DeclareLocal, v.local(n.name, LocalRole::Declaration);)
OPERANDS(DeclareReference, v.local(n.name, LocalRole::Declaration);)
OPERANDS(AddressReferenceSlot, v.definition(n.out); v.local(n.name, LocalRole::Address);)
OPERANDS(AddressLocal, v.definition(n.out); v.local(n.name, LocalRole::Address);)
OPERANDS(AddressField, v.definition(n.out); role(v, n.object, OperandRole::Borrow);)
OPERANDS(AddressElement, v.definition(n.out); role(v, n.array, OperandRole::Borrow); reads(v, n.index);)
OPERANDS(LoadAddress, v.definition(n.out); role(v, n.address, OperandRole::Address);)
OPERANDS(StoreAddress, role(v, n.address, OperandRole::Address); typed(v, n.value, n.type, true);)
// A zero address clears the reference binding; it is not an SSA operand.
OPERANDS(BindReference, present_role(v, n.address, OperandRole::Address); v.local(n.name, LocalRole::Write);)
OPERANDS(ReferenceAddress, v.definition(n.out); v.local(n.name, LocalRole::Read);)
OPERANDS(LoadReference, v.definition(n.out); v.local(n.name, LocalRole::ReferenceRead);)
OPERANDS(StoreReference, typed(v, n.value, n.type, true); v.local(n.name, LocalRole::ReferenceWrite);)
OPERANDS(LoadLocal, v.definition(n.out); v.local(n.name, n.move_out ? LocalRole::ReadWrite : LocalRole::Read);)
OPERANDS(StoreLocal, typed(v, n.value, n.type, !n.borrowed); v.local(n.name, LocalRole::Write);)

OPERANDS(Clone, v.definition(n.out); typed(v, n.value, n.type, false);)
OPERANDS(Retain, v.definition(n.out); typed(v, n.value, n.type, false);)
OPERANDS(Release, typed(v, n.value, n.type, true);)
OPERANDS(Pin, role(v, n.value, OperandRole::Borrow);)
OPERANDS(Unpin, role(v, n.value, OperandRole::Borrow);)

OPERANDS(FieldReferenceAddress, v.definition(n.out); role(v, n.object, OperandRole::Borrow);)
OPERANDS(FieldReferenceRoot, v.definition(n.out); role(v, n.object, OperandRole::Borrow);)
OPERANDS(BindFieldReference, role(v, n.object, OperandRole::Borrow); role(v, n.address, OperandRole::Address); present(v, n.root);)
OPERANDS(RootSerial, v.definition(n.out);)
OPERANDS(TupleMake, v.definition(n.out); detail::tuple(v, n);)
OPERANDS(TupleGet, v.definition(n.out); if (n.take) role(v, n.tuple, OperandRole::Read, false); else role(v, n.tuple, OperandRole::Borrow);)
OPERANDS(AddressTupleElement, v.definition(n.out); role(v, n.address, OperandRole::Address);)
OPERANDS(ArrayMake, v.definition(n.out); detail::array(v, n);)
OPERANDS(ArrayAlloc, v.definition(n.out); reads(v, n.length);)
OPERANDS(ClassMake, v.definition(n.out); detail::class_fields(v, n);)
OPERANDS(FieldGet, v.definition(n.out); role(v, n.object, OperandRole::Borrow);)
OPERANDS(FieldSet, role(v, n.object, OperandRole::Borrow); typed(v, n.value, n.field_type, true);)
OPERANDS(ArrayLength, v.definition(n.out); role(v, n.array, OperandRole::Borrow);)
OPERANDS(ArrayCanAppendMove, v.definition(n.out); role(v, n.array, OperandRole::Borrow);)
OPERANDS(ArrayGrowMove, v.definition(n.out); typed(v, n.array, n.array_type, true);)
OPERANDS(ArraySorted, v.definition(n.out); reads(v, n.array, n.descending);)
OPERANDS(ArrayInitializationComplete, v.definition(n.out); role(v, n.array, OperandRole::Borrow);)
OPERANDS(ArrayGet, v.definition(n.out); role(v, n.array, OperandRole::Borrow); reads(v, n.index); role(v, n.initialization_guard, OperandRole::Condition); role(v, n.bounds_guard, OperandRole::Condition);)
OPERANDS(ArraySet, role(v, n.array, OperandRole::Borrow); reads(v, n.index); typed(v, n.value, n.element_type, true); role(v, n.initialization_guard, OperandRole::Condition); role(v, n.bounds_guard, OperandRole::Condition); role(v, n.parallel_receipt, OperandRole::Address);)
OPERANDS(ArraySlice, v.definition(n.out); reads(v, n.array, n.bounds);)
OPERANDS(ArraySliceSet, role(v, n.array, OperandRole::Borrow); reads(v, n.bounds); role(v, n.value, OperandRole::Borrow);)
OPERANDS(SliceLoopPlan, v.definition(n.out); reads(v, n.length); role(v, n.first_address, OperandRole::Address); reads(v, n.bounds);)
OPERANDS(VariantMake, v.definition(n.out); if (n.payload != 0) typed(v, n.payload, n.payload_type, true);)
OPERANDS(VariantTag, v.definition(n.out); role(v, n.container, OperandRole::Borrow);)
OPERANDS(VariantPayload, v.definition(n.out); role(v, n.container, OperandRole::Borrow);)

OPERANDS(RealLoopCount, v.definition(n.out); reads(v, n.start, n.end, n.step);)
OPERANDS(RealLoopValue, v.definition(n.out); reads(v, n.multiplier, n.start, n.step); present(v, n.native_fma);)
OPERANDS(ExactLoopPlan, v.definition(n.out); reads(v, n.start, n.end, n.step);)
OPERANDS(ExactFromScaled, v.definition(n.out); reads(v, n.numerator, n.denominator);)
OPERANDS(ExactLoopValue, v.definition(n.out); reads(v, n.multiplier, n.start, n.step);)
OPERANDS(ExactScaledCompare, v.definition(n.out); reads(v, n.numerator, n.denominator);)
OPERANDS(ExactScaledConvert, v.definition(n.out); reads(v, n.numerator, n.denominator);)
OPERANDS(ExactAtom, v.definition(n.out);)
OPERANDS(ExactUnary, v.definition(n.out); reads(v, n.input);)
OPERANDS(ExactOp, v.definition(n.out); reads(v, n.inputs);)
// Exact constant slots name module-global caches, not function locals.
OPERANDS(ExactRound, v.definition(n.out); reads(v, n.value);)
OPERANDS(ExactConstant, v.definition(n.out);)
OPERANDS(Unary, v.definition(n.out); reads(v, n.operand);)
OPERANDS(Binary, v.definition(n.out); reads(v, n.left, n.right);)
OPERANDS(ComplexMake, v.definition(n.out); reads(v, n.re, n.im);)
OPERANDS(ComplexPart, v.definition(n.out); reads(v, n.value);)

OPERANDS(ParseBin, v.definition(n.out); reads(v, n.text);)
OPERANDS(BinConvert, v.definition(n.out); reads(v, n.value);)
OPERANDS(NumericConvert, v.definition(n.out); reads(v, n.value);)
OPERANDS(FallibleNumericConvert, v.definition(n.out); reads(v, n.value);)
OPERANDS(ArrayNumericCast, v.definition(n.out); reads(v, n.array);)
OPERANDS(ParseNumber, v.definition(n.out); reads(v, n.text);)
OPERANDS(ParseNumberDirect, v.definition(n.value_out); v.definition(n.ok_out); v.definition(n.error_out); reads(v, n.text);)
OPERANDS(ToString, v.definition(n.out); reads(v, n.value);)
OPERANDS(FormatNumber, v.definition(n.out); reads(v, n.value);)

OPERANDS(TensorCreate, v.definition(n.out); reads(v, n.shape, n.gpu);)
OPERANDS(TensorTransfer, v.definition(n.out); reads(v, n.tensor, n.gpu);)
OPERANDS(TensorReshape, v.definition(n.out); reads(v, n.tensor, n.shape);)
OPERANDS(TensorTranspose, v.definition(n.out); reads(v, n.tensor, n.axis0, n.axis1);)
OPERANDS(TensorContiguous, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorGather, v.definition(n.out); reads(v, n.tensor, n.indices, n.shape);)
OPERANDS(TensorScatter, v.definition(n.out); reads(v, n.tensor, n.indices, n.shape);)
OPERANDS(TensorShape, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorDevice, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorIsContiguous, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorItem, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorCast, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorBinary, v.definition(n.out); reads(v, n.left, n.right);)
OPERANDS(TensorCompare, v.definition(n.out); reads(v, n.left, n.right);)
OPERANDS(TensorBoolReduce, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorIndex, v.definition(n.out); reads(v, n.tensor, n.items);)
OPERANDS(TensorSet, reads(v, n.tensor, n.indices, n.value);)
OPERANDS(GpuSync, reads(v, n.index);)

OPERANDS(TensorIsTracked, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorHasGrad, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorClearGrad, role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(TensorTrack, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow); present_role(v, n.target, OperandRole::AutogradTarget);)
OPERANDS(TensorBackward, role(v, n.tensor, OperandRole::Borrow); for (const auto& target : n.targets) role(v, target.value, target.autograd_target ? OperandRole::AutogradTarget : OperandRole::Borrow); present_role(v, n.autograd_targets, OperandRole::AutogradTarget); role(v, n.track, OperandRole::Condition);)
OPERANDS(TensorGrad, v.definition(n.out); role(v, n.tensor, OperandRole::Borrow);)
OPERANDS(AutogradTargetCreate, v.definition(n.out);)
OPERANDS(AutogradTargetHasGrad, v.definition(n.out); role(v, n.target, OperandRole::AutogradTarget);)
OPERANDS(AutogradTargetClearGrad, role(v, n.target, OperandRole::AutogradTarget);)
OPERANDS(AutogradTargetGradient, v.definition(n.out); role(v, n.target, OperandRole::AutogradTarget);)

OPERANDS(ShapedConstraintCheck, reads(v, n.value, n.extents);)
OPERANDS(ExtentEqualCheck, reads(v, n.actual, n.expected);)
OPERANDS(TestAssert, role(v, n.condition, OperandRole::Condition);)
OPERANDS(ForStepCheck, reads(v, n.step);)
OPERANDS(TraversalSiteCheck, reads(v, n.expected, n.current);)
OPERANDS(IterationShapeCheck, reads(v, n.array, n.entry_array, n.entry_length);)
OPERANDS(InitializedCheck, role(v, n.flag, OperandRole::Condition);)
OPERANDS(FiniteCheck, reads(v, n.primary, n.covered);)
OPERANDS(FiniteFlagUpdate, reads(v, n.value.value); v.local(n.flag, LocalRole::ReadWrite);)
// A sunk check's result was defined in the loop body and need not dominate
// this exit. The flag is the tested value; the record supplies provenance.
OPERANDS(FiniteFlagCheck, if (n.value.cause == FiniteCause::division) reads(v, n.value.right); v.local(n.flag, LocalRole::Read);)
OPERANDS(ReferenceBoundCheck, role(v, n.address, OperandRole::Address);)
OPERANDS(SelfReferenceCheck, reads(v, n.holder_root, n.target_root);)
OPERANDS(StoredTargetsCheck, reads(v, n.value, n.destination_root);)

OPERANDS(FunctionRef, v.definition(n.out);)
// Void/never calls have out == 0 and define no SSA value.
OPERANDS(IndirectCall, present_definition(v, n.out); detail::indirect(v, n);)
OPERANDS(Call, present_definition(v, n.out); detail::call(v, n.callee, n.args, &n.result);)

OPERANDS(Exit, reads(v, n.status);)
OPERANDS(FailError, reads(v, n.error);)
OPERANDS(Return, typed(v, n.value, n.type, true);)
OPERANDS(ReturnVoid, (void)n; (void)v;)
OPERANDS(Jump, v.successor(n.target);)
OPERANDS(Branch, role(v, n.condition, OperandRole::Condition); v.successor(n.if_true); v.successor(n.if_false);)

OPERANDS(ErrorMake, v.definition(n.out); for (const auto& detail : n.details) reads(v, detail.value); reads(v, n.cause);)
OPERANDS(ErrorMessage, v.definition(n.out); reads(v, n.error);)
OPERANDS(ErrorCode, v.definition(n.out); reads(v, n.error);)
OPERANDS(ErrorCause, v.definition(n.out); reads(v, n.error);)
OPERANDS(ErrorDetail, v.definition(n.out); reads(v, n.error);)
OPERANDS(ErrorHop, v.definition(n.out); reads(v, n.container); present(v, n.callee);)

OPERANDS(ParallelBlock, v.definition(n.out); for (const auto& node : n.nodes) detail::call(v, node.function, node.captures, nullptr); if (n.iterations) { reads(v, n.iterations->count); role(v, n.iterations->worker_safe, OperandRole::Condition); for (const auto& array : n.iterations->arrays) role(v, array.array, OperandRole::Borrow); })

OPERANDS(BinAlloc, v.definition(n.out); reads(v, n.length, n.fill);)
OPERANDS(BinLength, v.definition(n.out); reads(v, n.bin);)
OPERANDS(BinGet, v.definition(n.out); reads(v, n.bin, n.index);)
OPERANDS(BinSet, reads(v, n.bin, n.index, n.value);)
OPERANDS(BinSlice, v.definition(n.out); reads(v, n.bin, n.start, n.end);)
// Valid bin slices have no step. Retain an authored step reference so a future
// verifier can diagnose malformed IR instead of hiding an operand.
OPERANDS(BinSlicePlanned, v.definition(n.out); reads(v, n.bin, n.bounds);)

OPERANDS(StringIndex, v.definition(n.out); reads(v, n.text, n.index);)
OPERANDS(StringIndexAsciiCompare, v.definition(n.out); reads(v, n.text, n.index);)
OPERANDS(StringAsciiCountPrefix, v.definition(n.out); reads(v, n.text, n.count, n.initial);)
OPERANDS(StringLength, v.definition(n.out); reads(v, n.text);)
OPERANDS(StringEmpty, v.definition(n.out); reads(v, n.text);)
OPERANDS(StringContains, v.definition(n.out); reads(v, n.text, n.needle);)
OPERANDS(StringStartsWith, v.definition(n.out); reads(v, n.text, n.prefix);)
OPERANDS(StringEndsWith, v.definition(n.out); reads(v, n.text, n.suffix);)
OPERANDS(StringFind, v.definition(n.out); reads(v, n.text, n.needle);)
OPERANDS(StringSlice, v.definition(n.out); reads(v, n.text, n.start, n.end);)
OPERANDS(StringSliceStepped, v.definition(n.out); reads(v, n.text, n.bounds);)
OPERANDS(StringTrim, v.definition(n.out); reads(v, n.text);)
OPERANDS(StringSplit, v.definition(n.out); reads(v, n.text, n.separator);)
OPERANDS(StringSplitIterBegin, v.definition(n.out); reads(v, n.text, n.separator);)
OPERANDS(StringSplitIterNext, v.definition(n.text); v.definition(n.has_value); reads(v, n.cursor);)
OPERANDS(StringSplitIterEnd, role(v, n.cursor, OperandRole::Borrow);)
OPERANDS(StringParseTwoSigned, v.definition(n.left); v.definition(n.right); v.definition(n.ok); reads(v, n.text);)
OPERANDS(StringUtf8, v.definition(n.out); reads(v, n.text);)
OPERANDS(StringFromUtf8, v.definition(n.out); reads(v, n.bin);)
OPERANDS(StringFromUtf8ArrayDirect, v.definition(n.text); v.definition(n.ok); v.definition(n.error); reads(v, n.array);)
OPERANDS(StringCodepoints, v.definition(n.out); reads(v, n.text);)
OPERANDS(StringJoin, v.definition(n.out); reads(v, n.values, n.separator);)
OPERANDS(StringConcat, v.definition(n.out); reads(v, n.values);)
OPERANDS(StringQuote, v.definition(n.out); role(v, n.text, OperandRole::Borrow);)
OPERANDS(StringBuild, v.definition(n.out); reads(v, n.parts, n.separator);)
OPERANDS(StringBuildAppendMove, v.definition(n.out); v.definition(n.added_length); role(v, n.text, OperandRole::Consume); reads(v, n.parts); role(v, n.separator, OperandRole::Borrow);)
OPERANDS(StringCanAppendMove, v.definition(n.out); role(v, n.text, OperandRole::Borrow);)
OPERANDS(StringAppendMove, v.definition(n.out); role(v, n.text, OperandRole::Read, false); role(v, n.suffixes, OperandRole::Borrow);)
OPERANDS(StringRepeat, v.definition(n.out); reads(v, n.count, n.fill);)

OPERANDS(Flush, v.definition(n.out);)
OPERANDS(Print, v.definition(n.out); reads(v, n.value);)
OPERANDS(Input, v.definition(n.out);)
OPERANDS(ReplDisplay, present(v, n.value);)
OPERANDS(ReplReplayMode, (void)n; (void)v;)

// CLI name/index fields are ValueIds; they are not local names or indices into
// the instruction list, even where an emitter currently ignores a name.
OPERANDS(CliArgument, v.definition(n.out); reads(v, n.name, n.index);)
OPERANDS(CliArgumentOptional, v.definition(n.out); reads(v, n.name, n.index, n.default_value);)
OPERANDS(CliOption, v.definition(n.out); reads(v, n.name, n.default_value);)
OPERANDS(CliFlag, v.definition(n.out); reads(v, n.name);)
OPERANDS(CliFinish, (void)n; (void)v;)
OPERANDS(EnvironmentGet, v.definition(n.out); reads(v, n.name);)
OPERANDS(EnvironmentHas, v.definition(n.out); reads(v, n.name);)
OPERANDS(TimeNow, v.definition(n.out); reads(v, n.sync);)
OPERANDS(TimeSince, v.definition(n.out); reads(v, n.start, n.sync);)
OPERANDS(TimeSeconds, v.definition(n.out); reads(v, n.seconds);)
OPERANDS(TimeSleep, reads(v, n.duration);)
OPERANDS(ProcessRun, v.definition(n.out); reads(v, n.program, n.args);)
OPERANDS(ProcessShell, v.definition(n.out); reads(v, n.command);)

OPERANDS(FileOpen, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileCreate, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileAppend, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileHandleRead, v.definition(n.out); reads(v, n.handle);)
OPERANDS(FileHandleReadLine, v.definition(n.out); reads(v, n.handle);)
OPERANDS(FileHandleReadBin, v.definition(n.out); reads(v, n.handle);)
OPERANDS(FileHandleWrite, v.definition(n.out); reads(v, n.handle, n.text);)
OPERANDS(FileHandleFlush, v.definition(n.out); reads(v, n.handle);)
OPERANDS(FileHandleSeek, v.definition(n.out); reads(v, n.handle, n.offset);)
OPERANDS(FileHandleClose, reads(v, n.handle);)
OPERANDS(FileRead, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileReadBin, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileWrite, v.definition(n.out); reads(v, n.path, n.text);)
OPERANDS(FileWriteBin, v.definition(n.out); reads(v, n.path, n.bin);)
OPERANDS(FileExists, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileIsDirectory, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileRemove, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileCopy, v.definition(n.out); reads(v, n.source, n.destination);)
OPERANDS(FileMove, v.definition(n.out); reads(v, n.source, n.destination);)
OPERANDS(FileMkdir, v.definition(n.out); reads(v, n.path);)
OPERANDS(FileList, v.definition(n.out); reads(v, n.path, n.recursive);)

OPERANDS(HttpGet, v.definition(n.out); reads(v, n.url);)
OPERANDS(HttpHeader, v.definition(n.out); reads(v, n.response, n.name);)

OPERANDS(JsonParse, v.definition(n.out); reads(v, n.text);)
OPERANDS(JsonKind, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonSize, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonGet, v.definition(n.out); reads(v, n.value, n.key);)
OPERANDS(JsonAt, v.definition(n.out); reads(v, n.value, n.index);)
OPERANDS(JsonText, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonInteger, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonNumber, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonBigInt, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonBigReal, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonBoolean, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonEncode, v.definition(n.out); reads(v, n.value);)
OPERANDS(JsonEqual, v.definition(n.out); reads(v, n.left, n.right);)

OPERANDS(RandomGenerator, v.definition(n.out); reads(v, n.seed);)
OPERANDS(RandomInt, v.definition(n.out); reads(v, n.generator, n.start, n.end);)
OPERANDS(RandomFloat, v.definition(n.out); reads(v, n.generator);)
OPERANDS(RandomBool, v.definition(n.out); reads(v, n.generator);)

#undef OPERANDS

namespace detail {
template<std::size_t... I>
constexpr bool every_operand_map(std::index_sequence<I...>) {
    return (OperandMapPresent<std::variant_alternative_t<I, Instruction>>::value && ...);
}
} // namespace detail

static_assert(detail::every_operand_map(std::make_index_sequence<std::variant_size_v<Instruction>>{}),
              "every Instruction alternative needs an explicit typed operand-role map");

void visit_operands(const Instruction& instruction, OperandVisitor& visitor, const OperandContext& context) {
    detail::ScopedOperandContext scoped(visitor, context);
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        InstructionTraits<T>::visit_operands(node, visitor);
    }, instruction);
}

void visit_operands(const Instruction& instruction, OperandVisitor& visitor) {
    const OperandContext context{};
    visit_operands(instruction, visitor, context);
}

} // namespace quidra::ir
