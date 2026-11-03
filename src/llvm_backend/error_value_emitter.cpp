#include "llvm_backend/error_value_emitter.hpp"

#include "llvm_backend/copy_idioms.hpp"
#include "llvm_backend/runtime_abi.hpp"

#include "llvm_backend/type_lowering.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

ErrorRecordConstant ErrorRecords::constant(abi::ErrorValueCode code,
                                           std::span<const abi::ErrorDetailValue> details,
                                           std::uint32_t line, std::uint32_t column) {
    const auto& schema = abi::error_value_schema(code);
    std::string message;
    if (!abi::render_error(schema, details, message))
        throw std::logic_error("error record details do not fit the schema of " +
                               std::string(schema.spelling));
    ErrorRecordConstant record;
    record.message = pool_.intern_error_text(message);
    record.code = pool_.intern_error_text(std::string(schema.spelling));
    record.schema = static_cast<unsigned>(code);
    record.line = sites_.line(line);
    record.column = static_cast<unsigned>(sites_.column(column));
    for (const auto& detail : details) {
        if (detail.kind == abi::ErrorDetailKind::integer) {
            record.slots.push_back(ErrorRecordSlot{true, {}, detail.integer});
        } else {
            record.slots.push_back(ErrorRecordSlot{
                false, detail.present ? pool_.intern_error_text(std::string(detail.text)) : "", 0});
        }
    }
    return record;
}

std::string ErrorRecords::record(abi::ErrorValueCode code,
                                 std::span<const abi::ErrorDetailValue> details,
                                 std::uint32_t line, std::uint32_t column) {
    return pool_.intern_error_records({constant(code, details, line, column)}, false);
}

std::string ErrorRecords::user_record(const std::string& text, std::string_view code,
                                      std::uint32_t line, std::uint32_t column) {
    ErrorRecordConstant record;
    record.message = text;
    if (!code.empty()) record.code = pool_.intern_error_text(std::string(code));
    record.line = sites_.line(line);
    record.column = static_cast<unsigned>(sites_.column(column));
    return pool_.intern_error_records({record}, false);
}

std::string ErrorRecords::record_array(abi::ErrorValueCode code,
                                       std::span<const abi::ErrorDetailValue> details,
                                       std::size_t varying, std::span<const std::string_view> values,
                                       std::size_t count, std::uint32_t line, std::uint32_t column) {
    std::vector<abi::ErrorDetailValue> current(details.begin(), details.end());
    std::vector<ErrorRecordConstant> records;
    for (std::size_t i = 0; i < count && i < values.size(); ++i) {
        current[varying] = abi::ErrorDetailValue::name_value(values[i]);
        records.push_back(constant(code, current, line, column));
    }
    return pool_.intern_error_records(std::move(records), true);
}

void ErrorRecords::store(const std::string& payload, const std::string& record,
                         const std::string& base) {
    if (sites_.user_code()) {
        builder_.store({ptr, record}, payload, Align::none);
        return;
    }
    const auto copy = base + ".record";
    builder_.call(copy, runtime_abi::error::from_site, {{ptr, record}});
    builder_.store({ptr, copy}, payload, Align::none);
}

std::string ErrorRecords::element(const std::string& array, abi::ErrorValueCode code,
                                  const std::string& index, const std::string& base) {
    const auto stride = abi::error_record_size(abi::error_value_schema(code).details.size());
    const auto site = base + ".site";
    builder_.getelementptr(site, Inbounds::yes, LlvmType::array(stride, i8),
                           LlvmOperand::global(array), {{i32, index}});
    return site;
}

void ErrorRecords::store_conversion(const std::string& payload, int type,
                                    abi::ConversionSubject subject, const std::string& reason,
                                    std::size_t reasons, std::uint32_t line, std::uint32_t column) {
    const auto code = abi::ErrorValueCode::numeric_conversion;
    const auto type_name = abi::conversion_type_name(type);
    const auto subject_name = abi::conversion_subject_spellings[static_cast<std::size_t>(subject)];
    const bool immediate = !reason.empty() && reason.front() >= '0' && reason.front() <= '9';
    if (immediate) {
        const auto index = static_cast<std::size_t>(std::stoul(reason));
        const std::array details{abi::ErrorDetailValue::name_value(type_name),
                                 abi::ErrorDetailValue::name_value(subject_name),
                                 abi::ErrorDetailValue::name_value(abi::conversion_reason_spellings[index])};
        store(payload, "@" + record(code, details, line, column), payload);
        return;
    }
    const std::array details{abi::ErrorDetailValue::name_value(type_name),
                             abi::ErrorDetailValue::name_value(subject_name),
                             abi::ErrorDetailValue::name_value(abi::conversion_reason_spellings[0])};
    const auto array =
        record_array(code, details, 2, abi::conversion_reason_spellings, reasons, line, column);
    store(payload, element(array, code, reason, payload), payload);
}

void ErrorValueEmitter::reserve(const ir::ErrorMake& n, const ir::Instruction& ins) {
    if (n.schema == 0) {
        if (!n.details.empty() && !n.details.front().value) pool_.intern(n.details.front().constant);
        return;
    }
    for (const auto& detail : n.details) {
        if (detail.value) {
            scratch_.reserve(ins, LlvmType::array(abi::max_error_details, i64), Align::eight);
            return;
        }
    }
}

void ErrorValueEmitter::emit(const ir::ErrorMake& n, const ir::Instruction& ins) {
    symbols_.set_type(n.out, Type::simple(TypeKind::Error));
    if (n.schema == 0) emit_user(n);
    else emit_core(n, ins);
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Error),symbols_.value(n.out));
}

void ErrorValueEmitter::emit_user(const ir::ErrorMake& n) {
    const auto out = symbols_.value(n.out);
    const auto& text = n.details.front();
    if (!text.value && !n.cause && records_.user_code()) {
        const auto record = records_.user_record(pool_.intern(text.constant), n.code, n.position.line, n.position.column);
        copy_by_zero_gep(builder_, out, Inbounds::yes, LlvmOperand::global(record));
        return;
    }
    const std::string text_operand =
        text.value ? symbols_.value(*text.value) : "@" + pool_.intern(text.constant);
    const std::string code = n.code.empty() ? "null" : "@" + records_.text(n.code);
    const std::string cause = n.cause ? symbols_.value(*n.cause) : "null";
    builder_.call(out, runtime_abi::error::make_user,
                  {{ptr, text_operand}, {ptr, code}, {ptr, cause},
                   {i64, records_.line(n.position.line)}, {i64, records_.column(n.position.column)}});
}

void ErrorValueEmitter::emit_core(const ir::ErrorMake& n, const ir::Instruction& ins) {
    const auto out = symbols_.value(n.out);
    const auto code = static_cast<abi::ErrorValueCode>(n.schema);
    const auto& schema = abi::error_value_schema(code);
    bool values = false;
    for (const auto& detail : n.details) values = values || detail.value.has_value();
    if (!values) {
        std::array<abi::ErrorDetailValue, abi::max_error_details> details{};
        for (std::size_t i = 0; i < schema.details.size(); ++i) {
            const auto kind = schema.details[i].kind;
            details[i] = n.details[i].absent
                ? abi::ErrorDetailValue::absent(kind)
                : abi::ErrorDetailValue{true, kind, n.details[i].constant, 0};
        }
        const auto record = records_.record(code, std::span(details.data(), schema.details.size()),
                                            n.position.line, n.position.column);
        if (records_.user_code()) {
            copy_by_zero_gep(builder_, out, Inbounds::yes, LlvmOperand::global(record));
        } else {
            builder_.call(out, runtime_abi::error::from_site, {{ptr, "@" + record}});
        }
        return;
    }
    // The details in a slot array of the entry frame, in schema order.
    const auto& slots = scratch_.instruction_slot(ins);
    for (std::size_t i = 0; i < schema.details.size(); ++i) {
        const auto slot = out + ".slot." + std::to_string(i);
        builder_.getelementptr(slot, Inbounds::yes, LlvmType::array(abi::max_error_details, i64),
                               slots, {{i64, 0}, {i64, i}});
        const auto& detail = n.details[i];
        if (schema.details[i].kind == abi::ErrorDetailKind::integer) {
            builder_.store({i64, detail.value ? symbols_.value(*detail.value) : detail.constant}, slot,
                           Align::eight);
        } else if (detail.absent) {
            builder_.store({ptr, "null"}, slot, Align::eight);
        } else if (detail.value) {
            builder_.store({ptr, symbols_.value(*detail.value)}, slot, Align::eight);
        } else {
            builder_.store({ptr, "@" + records_.text(detail.constant)}, slot, Align::eight);
        }
    }
    builder_.call(out, runtime_abi::error::make,
                  {{i32, static_cast<long long>(n.schema)}, {ptr, slots},
                   {i64, records_.line(n.position.line)}, {i64, records_.column(n.position.column)}});
}

void ErrorValueEmitter::reserve(const ir::ErrorDetail& n, const ir::Instruction& ins) {
    if (n.detail < abi::error_detail_names.size() &&
        abi::error_detail_is_integer(abi::error_detail_names[n.detail]))
        scratch_.reserve(ins, i8, Align::none);
}

void ErrorValueEmitter::emit(const ir::ErrorCode& n, const ir::Instruction&) {
    symbols_.set_type(n.out, Type::simple(TypeKind::String));
    const auto out = symbols_.value(n.out);
    const auto field = out + ".field", raw = out + ".raw", absent = out + ".absent";
    builder_.getelementptr(field, Inbounds::yes, i8, symbols_.value(n.error),
                           {{i64, static_cast<long long>(offsetof(abi::ErrorRecord, code))}});
    builder_.load(raw, ptr, field, Align::eight);
    builder_.icmp(absent, IntPredicate::eq, ptr, raw, "null");
    builder_.select(out, absent,
                    {ptr, "@" + records_.text(std::string(abi::user_error_code))}, {ptr, raw});
}

// A union of `value` (a ptr, null for none) and none: the tag chosen by a
// select, the value stored as the payload.
void ErrorValueEmitter::store_optional(const std::string& out, const Type& type,
                                       const Type& present_type, const std::string& value,
                                       LlvmType value_type, const std::string& absent) {
    union_box_.allocate(out, type);
    const auto tag = out + ".tag", payload = out + ".payload";
    builder_.select(tag, absent, {i64, case_index(type, Type::simple(TypeKind::None))},
                    {i64, case_index(type, present_type)});
    builder_.store({i64, tag}, out, Align::eight);
    union_box_.payload_slot(payload, out);
    builder_.store({value_type, value}, payload, Align::none);
}

void ErrorValueEmitter::emit(const ir::ErrorCause& n, const ir::Instruction&) {
    symbols_.set_type(n.out, n.result_type);
    const auto out = symbols_.value(n.out);
    const auto field = out + ".field", cause = out + ".value", absent = out + ".absent";
    builder_.getelementptr(field, Inbounds::yes, i8, symbols_.value(n.error),
                           {{i64, static_cast<long long>(offsetof(abi::ErrorRecord, cause))}});
    builder_.load(cause, ptr, field, Align::eight);
    builder_.call(runtime_abi::memory::managed_retain, {{ptr, cause}});
    symbols_.acquire_temporary(cause,Type::simple(TypeKind::Error));
    builder_.icmp(absent, IntPredicate::eq, ptr, cause, "null");
    store_optional(out, n.result_type, Type::simple(TypeKind::Error), cause, ptr, absent);
    symbols_.transfer_temporary(cause,Type::simple(TypeKind::Error));
}

void ErrorValueEmitter::emit(const ir::ErrorDetail& n, const ir::Instruction& ins) {
    symbols_.set_type(n.out, n.result_type);
    const auto out = symbols_.value(n.out);
    const auto absent = out + ".absent";
    const bool integer = n.detail < abi::error_detail_names.size() &&
                         abi::error_detail_is_integer(abi::error_detail_names[n.detail]);
    if (!integer) {
        const auto text = out + ".value";
        builder_.call(text, runtime_abi::error::detail,
                      {{ptr, symbols_.value(n.error)}, {i32, static_cast<long long>(n.detail)}});
        symbols_.acquire_temporary(text,Type::simple(TypeKind::String));
        builder_.icmp(absent, IntPredicate::eq, ptr, text, "null");
        store_optional(out, n.result_type, Type::simple(TypeKind::String), text, ptr, absent);
        symbols_.transfer_temporary(text,Type::simple(TypeKind::String));
        return;
    }
    // A nat: the word of an inline integer (its value shifted left once).
    const auto& present_slot = scratch_.instruction_slot(ins);
    const auto raw = out + ".raw", word = out + ".value", present = out + ".present";
    builder_.call(raw, runtime_abi::error::detail_int,
                  {{ptr, symbols_.value(n.error)}, {i32, static_cast<long long>(n.detail)},
                   {ptr, present_slot}});
    builder_.binary(word, BinaryOp::shl, NoWrap::nsw, i64, raw, 1);
    builder_.load(present, i1, present_slot, Align::none);
    builder_.binary(absent, BinaryOp::xor_, i1, present, "true");
    store_optional(out, n.result_type, Type::simple(TypeKind::Nat), word, i64, absent);
}

void ErrorValueEmitter::emit(const ir::ErrorHop& n, const ir::Instruction&) {
    symbols_.set_type(n.out, Type::simple(TypeKind::Error));
    const auto out = symbols_.value(n.out);
    const auto slot = out + ".slot", payload = out + ".payload";
    union_box_.payload_slot(slot, symbols_.value(n.container));
    builder_.load(payload, ptr, slot, Align::eight);
    symbols_.acquire_temporary(payload,Type::simple(TypeKind::Error));
    builder_.store({ptr, "null"}, slot, Align::eight);
    // runtime hop accepts this obligation immediately on entry, with no
    // allocation before its local owner guard is active.
    symbols_.transfer_temporary(payload,Type::simple(TypeKind::Error));
    const auto site = "@" + records_.hop_site(n.name, n.kind, n.position.line, n.position.column);
    if (n.callee != 0)
        builder_.call(out, runtime_abi::error::hop_indirect,
                      {{ptr, payload}, {ptr, site}, {ptr, symbols_.value(n.callee)}});
    else
        builder_.call(out, runtime_abi::error::hop, {{ptr, payload}, {ptr, site}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::Error),out);
}

void ErrorValueEmitter::emit(const ir::ErrorMessage& n, const ir::Instruction&) {
    symbols_.set_type(n.out, Type::simple(TypeKind::String));
    const auto out = symbols_.value(n.out);
    builder_.load(out, ptr, symbols_.value(n.error), Align::eight);
    builder_.call(runtime_abi::memory::managed_retain, {{ptr, out}});
    symbols_.acquire_owned(n.out,Type::simple(TypeKind::String),out);
}

} // namespace quidra::llvm_backend
