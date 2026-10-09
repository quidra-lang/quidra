#include "llvm_backend/runtime_prelude.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_text/appendable_text.hpp"
#include "llvm_text/llvm_callee.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/process_status.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace quidra::llvm_backend {

namespace {

// The helpers written in LLVM IR below spell layouts and conventions as
// numbers: quidra_array_alloc and the array slot helpers put the elements
// of a dynamic array at offset 8, quidra_fail ends the program with status
// 101, and quidra_stack_enter allows a call depth of 4096.
static_assert(abi::array_layout::payload_offset == 8);
static_assert(abi::failure_exit_status == 101);
static_assert(abi::self_call_depth_limit == 4096);

// One piece of the prelude: the declaration of a runtime entry point,
// written from its signature, or LLVM text written as it is (the C library
// declarations with their attributes, the intrinsics, the globals and the
// helpers written in LLVM IR). `is_declaration` tells the two apart: the
// constant expressions below must not compare an object's address with
// null, which GCC rejects under -fsanitize=undefined (it implies
// -fno-delete-null-pointer-checks).
struct PreludeItem {
    bool is_declaration;
    const llvm_text::LlvmCallee* declaration;
    std::string_view text;
};

constexpr PreludeItem declare(const llvm_text::LlvmCallee& callee) { return {true, &callee, {}}; }
constexpr PreludeItem verbatim(std::string_view text) { return {false, nullptr, text}; }

// The prelude in its order.
constexpr std::array prelude_items{
    verbatim("\n"),
    declare(runtime_abi::program::set_args),
    declare(runtime_abi::program::set_source_provenance),
    declare(runtime_abi::program::set_package_source_provenance),
    declare(runtime_abi::program::user_statement),
    declare(runtime_abi::program::restore_user_statement),
    declare(runtime_abi::program::register_sources),
    declare(runtime_abi::program::export_leave),
    declare(runtime_abi::failure::fail_at),
    declare(runtime_abi::failure::bounds_fail),
    declare(runtime_abi::failure::conversion_fail),
    declare(runtime_abi::failure::conversion_message),
    declare(runtime_abi::failure::last_conversion_reason),
    declare(runtime_abi::exact::bigint_literal),
    declare(runtime_abi::exact::bigint_parse),
    declare(runtime_abi::exact::bigreal_literal),
    declare(runtime_abi::exact::bigreal_parse),
    declare(runtime_abi::exact::real_atom),
    declare(runtime_abi::exact::real_atom_float64),
    declare(runtime_abi::exact::real_unary),
    declare(runtime_abi::exact::bigint_drop),
    declare(runtime_abi::exact::bigreal_drop),
    declare(runtime_abi::exact::bigint_text),
    declare(runtime_abi::exact::bigreal_text),
    declare(runtime_abi::exact::bigint_neg),
    declare(runtime_abi::exact::bigint_binary),
    declare(runtime_abi::exact::bigint_pow),
    declare(runtime_abi::exact::bigint_compare),
    declare(runtime_abi::exact::bigreal_neg),
    declare(runtime_abi::exact::bigreal_binary),
    declare(runtime_abi::exact::bigreal_compare),
    declare(runtime_abi::exact::bigreal_pow),
    declare(runtime_abi::exact::bigint_from_i64),
    declare(runtime_abi::exact::bigint_from_u64),
    declare(runtime_abi::exact::bigreal_from_i64),
    declare(runtime_abi::exact::bigreal_from_u64),
    declare(runtime_abi::exact::bigreal_from_float64),
    declare(runtime_abi::exact::bigreal_from_bigint),
    declare(runtime_abi::exact::bigreal_to_bigint),
    declare(runtime_abi::exact::bigint_try_i64),
    declare(runtime_abi::exact::bigint_try_u64),
    declare(runtime_abi::exact::bigreal_try_bigint),
    declare(runtime_abi::exact::bigreal_try_i64),
    declare(runtime_abi::exact::bigreal_try_u64),
    declare(runtime_abi::exact::bigint_try_float64),
    declare(runtime_abi::exact::bigint_try_float32),
    declare(runtime_abi::exact::numeric_cast_fits),
    declare(runtime_abi::exact::bigreal_try_float64),
    declare(runtime_abi::exact::bigreal_try_float32),
    declare(runtime_abi::exact::bigint_to_i64),
    declare(runtime_abi::exact::bigint_to_u64),
    declare(runtime_abi::exact::bigreal_to_float64),
    declare(runtime_abi::exact::bigreal_to_float32),
    declare(runtime_abi::exact::bigint_to_float64),
    declare(runtime_abi::exact::bigint_to_float32),
    declare(runtime_abi::text::index),
    declare(runtime_abi::text::index_equal_ascii),
    declare(runtime_abi::text::count_ascii_prefix),
    declare(runtime_abi::text::length),
    declare(runtime_abi::text::contains),
    declare(runtime_abi::text::starts_with),
    declare(runtime_abi::text::ends_with),
    declare(runtime_abi::text::find),
    declare(runtime_abi::text::slice),
    declare(runtime_abi::text::trim),
    declare(runtime_abi::text::split),
    declare(runtime_abi::text::split_iter_begin),
    declare(runtime_abi::text::split_iter_begin_move),
    declare(runtime_abi::text::split_iter_next),
    declare(runtime_abi::text::split_iter_end),
    declare(runtime_abi::text::parse_two_signed),
    verbatim(R"LLVM(
; The semantic IR only reaches this helper after proving the ordinary
; split(single-byte separator) + two signed-int parse pattern.  It recognizes
; the canonical decimal success path directly in generated LLVM so hot loops do
; not cross the separately linked C++ runtime once per record.  Any spelling it
; declines is handled by the existing source-level fallback, preserving parse
; and error semantics.
define internal i1 @__quidra_string_parse_two_signed_fast(
    ptr %text, i8 %separator, ptr %out_left, ptr %out_right) alwaysinline {
entry:
  store i64 0, ptr %out_left, align 8
  store i64 0, ptr %out_right, align 8
  %text.null = icmp eq ptr %text, null
  %separator.zero = icmp eq i8 %separator, 0
  %invalid.input = or i1 %text.null, %separator.zero
  br i1 %invalid.input, label %fail, label %left.sign

left.sign:
  %left.first = load i8, ptr %text, align 1
  %left.negative = icmp eq i8 %left.first, 45
  %left.after.sign = getelementptr inbounds i8, ptr %text, i64 1
  %left.cursor.start = select i1 %left.negative, ptr %left.after.sign, ptr %text
  %left.start.byte = load i8, ptr %left.cursor.start, align 1
  %left.start.nul = icmp eq i8 %left.start.byte, 0
  %left.start.sep = icmp eq i8 %left.start.byte, %separator
  %left.empty = or i1 %left.start.nul, %left.start.sep
  br i1 %left.empty, label %fail, label %left.loop

left.loop:
  %left.cursor = phi ptr [ %left.cursor.start, %left.sign ], [ %left.next, %left.step ]
  %left.magnitude = phi i64 [ 0, %left.sign ], [ %left.magnitude.next, %left.step ]
  %left.byte = load i8, ptr %left.cursor, align 1
  %left.at.separator = icmp eq i8 %left.byte, %separator
  br i1 %left.at.separator, label %left.done, label %left.not_separator

left.not_separator:
  %left.at.nul = icmp eq i8 %left.byte, 0
  br i1 %left.at.nul, label %fail, label %left.digit

left.digit:
  %left.digit.raw = sub i8 %left.byte, 48
  %left.digit.valid = icmp ult i8 %left.digit.raw, 10
  br i1 %left.digit.valid, label %left.range, label %fail

left.range:
  %left.digit64 = zext i8 %left.digit.raw to i64
  %left.magnitude.too.large = icmp ugt i64 %left.magnitude, 922337203685477580
  %left.magnitude.at.cutoff = icmp eq i64 %left.magnitude, 922337203685477580
  %left.last.limit = select i1 %left.negative, i64 8, i64 7
  %left.digit.too.large = icmp ugt i64 %left.digit64, %left.last.limit
  %left.cutoff.bad = and i1 %left.magnitude.at.cutoff, %left.digit.too.large
  %left.range.bad = or i1 %left.magnitude.too.large, %left.cutoff.bad
  br i1 %left.range.bad, label %fail, label %left.step

left.step:
  %left.times10 = mul i64 %left.magnitude, 10
  %left.magnitude.next = add i64 %left.times10, %left.digit64
  %left.next = getelementptr inbounds i8, ptr %left.cursor, i64 1
  br label %left.loop

left.done:
  %left.negated = sub i64 0, %left.magnitude
  %left.value = select i1 %left.negative, i64 %left.negated, i64 %left.magnitude
  %right.text = getelementptr inbounds i8, ptr %left.cursor, i64 1
  %right.first = load i8, ptr %right.text, align 1
  %right.negative = icmp eq i8 %right.first, 45
  %right.after.sign = getelementptr inbounds i8, ptr %right.text, i64 1
  %right.cursor.start = select i1 %right.negative, ptr %right.after.sign, ptr %right.text
  %right.start.byte = load i8, ptr %right.cursor.start, align 1
  %right.start.nul = icmp eq i8 %right.start.byte, 0
  %right.start.sep = icmp eq i8 %right.start.byte, %separator
  %right.empty = or i1 %right.start.nul, %right.start.sep
  br i1 %right.empty, label %fail, label %right.loop

right.loop:
  %right.cursor = phi ptr [ %right.cursor.start, %left.done ], [ %right.next, %right.step ]
  %right.magnitude = phi i64 [ 0, %left.done ], [ %right.magnitude.next, %right.step ]
  %right.byte = load i8, ptr %right.cursor, align 1
  %right.at.nul = icmp eq i8 %right.byte, 0
  %right.at.separator = icmp eq i8 %right.byte, %separator
  %right.done = or i1 %right.at.nul, %right.at.separator
  br i1 %right.done, label %right.finish, label %right.digit

right.digit:
  %right.digit.raw = sub i8 %right.byte, 48
  %right.digit.valid = icmp ult i8 %right.digit.raw, 10
  br i1 %right.digit.valid, label %right.range, label %fail

right.range:
  %right.digit64 = zext i8 %right.digit.raw to i64
  %right.magnitude.too.large = icmp ugt i64 %right.magnitude, 922337203685477580
  %right.magnitude.at.cutoff = icmp eq i64 %right.magnitude, 922337203685477580
  %right.last.limit = select i1 %right.negative, i64 8, i64 7
  %right.digit.too.large = icmp ugt i64 %right.digit64, %right.last.limit
  %right.cutoff.bad = and i1 %right.magnitude.at.cutoff, %right.digit.too.large
  %right.range.bad = or i1 %right.magnitude.too.large, %right.cutoff.bad
  br i1 %right.range.bad, label %fail, label %right.step

right.step:
  %right.times10 = mul i64 %right.magnitude, 10
  %right.magnitude.next = add i64 %right.times10, %right.digit64
  %right.next = getelementptr inbounds i8, ptr %right.cursor, i64 1
  br label %right.loop

right.finish:
  %right.negated = sub i64 0, %right.magnitude
  %right.value = select i1 %right.negative, i64 %right.negated, i64 %right.magnitude
  store i64 %left.value, ptr %out_left, align 8
  store i64 %right.value, ptr %out_right, align 8
  ret i1 true

fail:
  ret i1 false
}
)LLVM"),
    declare(runtime_abi::text::utf8),
    declare(runtime_abi::bin::try_utf8),
    declare(runtime_abi::text::u8_array_try_utf8),
    declare(runtime_abi::text::codepoints),
    declare(runtime_abi::text::join),
    declare(runtime_abi::text::concat_many),
    declare(runtime_abi::text::build),
    declare(runtime_abi::text::build_append_move),
    declare(runtime_abi::text::build_append_move_unique),
    declare(runtime_abi::text::build_append_move_unique_direct),
    declare(runtime_abi::text::build_append_last_length),
    declare(runtime_abi::text::concat2),
    declare(runtime_abi::text::equal),
    declare(runtime_abi::numeric::integer_text_signed),
    declare(runtime_abi::numeric::integer_text_unsigned),
    declare(runtime_abi::text::can_append_move),
    declare(runtime_abi::text::append_move_many),
    declare(runtime_abi::text::repeat),
    declare(runtime_abi::bin::alloc),
    declare(runtime_abi::bin::index),
    declare(runtime_abi::bin::set),
    declare(runtime_abi::bin::slice),
    declare(runtime_abi::bin::parse),
    declare(runtime_abi::bin::string),
    declare(runtime_abi::bin::from_u64),
    declare(runtime_abi::bin::to_u64),
    declare(runtime_abi::bin::from_array),
    declare(runtime_abi::bin::to_array),
    declare(runtime_abi::bin::clone),
    declare(runtime_abi::bin::equal),
    declare(runtime_abi::bin::byte_length),
    declare(runtime_abi::system::cli_argument),
    declare(runtime_abi::system::cli_argument_optional),
    declare(runtime_abi::system::cli_option),
    declare(runtime_abi::system::cli_flag),
    declare(runtime_abi::system::cli_finish),
    declare(runtime_abi::system::cli_parse_int),
    declare(runtime_abi::system::cli_parse_float),
    declare(runtime_abi::system::cli_parse_bigint),
    declare(runtime_abi::system::cli_parse_bigreal),
    declare(runtime_abi::system::cli_parse_bool),
    declare(runtime_abi::console::flush),
    declare(runtime_abi::console::output_status),
    declare(runtime_abi::file::open_raw),
    declare(runtime_abi::file::create_raw),
    declare(runtime_abi::file::append_raw),
    declare(runtime_abi::file::handle_read_raw),
    declare(runtime_abi::file::handle_read_line_raw),
    declare(runtime_abi::file::handle_read_bin_raw),
    declare(runtime_abi::file::handle_write_raw),
    declare(runtime_abi::file::handle_flush_raw),
    declare(runtime_abi::file::handle_seek_raw),
    declare(runtime_abi::file::handle_close),
    declare(runtime_abi::file::handle_clone),
    declare(runtime_abi::file::handle_drop),
    declare(runtime_abi::file::read_raw),
    declare(runtime_abi::file::read_bin_raw),
    declare(runtime_abi::file::write_raw),
    declare(runtime_abi::file::write_bin_raw),
    declare(runtime_abi::file::exists_raw),
    declare(runtime_abi::file::is_directory_raw),
    declare(runtime_abi::file::remove_raw),
    declare(runtime_abi::file::copy_raw),
    declare(runtime_abi::file::move_raw),
    declare(runtime_abi::file::mkdir_raw),
    declare(runtime_abi::file::list_raw),
    declare(runtime_abi::system::environment_get),
    declare(runtime_abi::system::environment_has),
    declare(runtime_abi::check::test_assert),
    declare(runtime_abi::system::time_now),
    declare(runtime_abi::tensor::gpu_sync),
    declare(runtime_abi::system::time_sleep),
    declare(runtime_abi::concurrency::task_all),
    declare(runtime_abi::concurrency::task_all_i64),
    declare(runtime_abi::concurrency::task_all_f64),
    declare(runtime_abi::concurrency::task_all_atomic_counter),
    declare(runtime_abi::concurrency::atomic_counter_create),
    declare(runtime_abi::concurrency::atomic_counter_clone),
    declare(runtime_abi::concurrency::atomic_counter_drop),
    declare(runtime_abi::concurrency::atomic_counter_add),
    declare(runtime_abi::concurrency::atomic_counter_load),
    declare(runtime_abi::autograd::target_create),
    declare(runtime_abi::autograd::target_clone),
    declare(runtime_abi::autograd::target_drop),
    declare(runtime_abi::autograd::target_has_grad),
    declare(runtime_abi::autograd::target_clear_grad),
    declare(runtime_abi::autograd::target_gradient),
    declare(runtime_abi::random::int_value),
    declare(runtime_abi::random::float_value),
    declare(runtime_abi::random::bool_value),
    declare(runtime_abi::process::run),
    declare(runtime_abi::process::shell),
    declare(runtime_abi::json::parse_raw),
    declare(runtime_abi::json::drop),
    declare(runtime_abi::json::last_error_copy),
    declare(runtime_abi::json::kind),
    declare(runtime_abi::json::size),
    declare(runtime_abi::json::is_object),
    declare(runtime_abi::json::is_array),
    declare(runtime_abi::json::get),
    declare(runtime_abi::json::at),
    declare(runtime_abi::json::text),
    declare(runtime_abi::json::integer_ok),
    declare(runtime_abi::json::integer),
    declare(runtime_abi::json::number_ok),
    declare(runtime_abi::json::number),
    declare(runtime_abi::json::number_text),
    declare(runtime_abi::json::boolean_ok),
    declare(runtime_abi::json::boolean),
    declare(runtime_abi::json::encode),
    declare(runtime_abi::json::equal),
    declare(runtime_abi::http::get),
    declare(runtime_abi::http::last_error_copy),
    declare(runtime_abi::http::header),
    declare(runtime_abi::http::response_clone),
    declare(runtime_abi::http::response_drop),
    verbatim(R"LLVM(declare i32 @printf(ptr, ...)
declare i32 @puts(ptr nocapture nonnull readonly)
declare i64 @strlen(ptr nocapture nonnull readonly)
declare i32 @strcmp(ptr nocapture nonnull readonly, ptr nocapture nonnull readonly)
declare ptr @strchr(ptr nocapture nonnull readonly, i32)
declare void @free(ptr)
)LLVM"),
    declare(runtime_abi::memory::managed_alloc),
    declare(runtime_abi::memory::managed_retain),
    declare(runtime_abi::memory::managed_release),
    declare(runtime_abi::memory::managed_pin),
    declare(runtime_abi::memory::managed_unpin),
    declare(runtime_abi::memory::init_create),
    declare(runtime_abi::array::initialization_complete),
    declare(runtime_abi::memory::init_mark_range),
    declare(runtime_abi::memory::init_check),
    declare(runtime_abi::memory::init_require_range),
    declare(runtime_abi::memory::init_clone),
    declare(runtime_abi::array::can_append_move),
    declare(runtime_abi::array::grow_move),
    declare(runtime_abi::array::sorted),
    declare(runtime_abi::numeric::cast_element),
    declare(runtime_abi::numeric::cast_element_fits),
    declare(runtime_abi::tensor::create),
    declare(runtime_abi::tensor::to_gpu),
    declare(runtime_abi::tensor::to_cpu),
    declare(runtime_abi::tensor::clone),
    declare(runtime_abi::tensor::drop),
    declare(runtime_abi::tensor::reshape),
    declare(runtime_abi::tensor::gather),
    declare(runtime_abi::tensor::scatter),
    declare(runtime_abi::tensor::transpose),
    declare(runtime_abi::tensor::contiguous),
    declare(runtime_abi::tensor::shape),
    declare(runtime_abi::tensor::shape_fixed),
    declare(runtime_abi::tensor::device),
    declare(runtime_abi::tensor::is_contiguous),
    declare(runtime_abi::tensor::is_tracked),
    declare(runtime_abi::tensor::has_grad),
    declare(runtime_abi::tensor::clear_grad),
    declare(runtime_abi::tensor::item_ptr),
    declare(runtime_abi::tensor::track),
    declare(runtime_abi::tensor::track_target),
    declare(runtime_abi::tensor::untrack),
    declare(runtime_abi::tensor::retrack),
    declare(runtime_abi::tensor::backward_many),
    declare(runtime_abi::tensor::backward_many_with_autograd_targets),
    declare(runtime_abi::tensor::grad),
    declare(runtime_abi::tensor::cast),
    declare(runtime_abi::tensor::try_cast),
    declare(runtime_abi::tensor::rank_check),
    declare(runtime_abi::tensor::extent_check),
    declare(runtime_abi::tensor::unary),
    declare(runtime_abi::tensor::binary),
    declare(runtime_abi::tensor::compare),
    declare(runtime_abi::tensor::bool_reduce),
    declare(runtime_abi::tensor::index),
    declare(runtime_abi::tensor::set),
    declare(runtime_abi::numeric::format_float),
    declare(runtime_abi::numeric::format_signed),
    declare(runtime_abi::numeric::format_unsigned),
    declare(runtime_abi::numeric::format_number),
    verbatim(R"LLVM(declare ptr @memcpy(ptr, ptr, i64)
declare ptr @memmove(ptr, ptr, i64)
declare ptr @memset(ptr, i32, i64)
declare i32 @memcmp(ptr nocapture nonnull readonly, ptr nocapture nonnull readonly, i64)
declare i32 @snprintf(ptr, i64, ptr, ...)
declare i32 @fflush(ptr)
declare void @exit(i32)
declare void @_Exit(i32)
declare double @pow(double, double)
declare float @powf(float, float)
)LLVM"),
    declare(runtime_abi::numeric::integer_pow_signed),
    declare(runtime_abi::numeric::integer_pow_unsigned),
    verbatim(R"LLVM(declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.sadd.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.sadd.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.ssub.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.ssub.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.ssub.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.smul.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.smul.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.smul.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.uadd.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.uadd.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.usub.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.usub.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.usub.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.umul.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.umul.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.umul.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)
declare i8 @llvm.fptosi.sat.i8.f32(float)
declare i16 @llvm.fptosi.sat.i16.f32(float)
declare i32 @llvm.fptosi.sat.i32.f32(float)
declare i64 @llvm.fptosi.sat.i64.f32(float)
declare i8 @llvm.fptoui.sat.i8.f32(float)
declare i16 @llvm.fptoui.sat.i16.f32(float)
declare i32 @llvm.fptoui.sat.i32.f32(float)
declare i64 @llvm.fptoui.sat.i64.f32(float)
declare i8 @llvm.fptosi.sat.i8.f64(double)
declare i16 @llvm.fptosi.sat.i16.f64(double)
declare i32 @llvm.fptosi.sat.i32.f64(double)
declare i64 @llvm.fptosi.sat.i64.f64(double)
declare i8 @llvm.fptoui.sat.i8.f64(double)
declare i16 @llvm.fptoui.sat.i16.f64(double)
declare i32 @llvm.fptoui.sat.i32.f64(double)
declare i64 @llvm.fptoui.sat.i64.f64(double)

)LLVM"),
    declare(runtime_abi::numeric::parse_signed),
    declare(runtime_abi::numeric::parse_unsigned),
    declare(runtime_abi::numeric::parse_float32),
    declare(runtime_abi::numeric::parse_float64),
    declare(runtime_abi::console::input_read),
    verbatim(R"LLVM(
@.quidra.source.line = internal thread_local global i64 0
@.quidra.source.column = internal thread_local global i64 0

define void @quidra_fail(ptr %msg) noreturn {
entry:
  call i32 @puts(ptr %msg)
  call i32 @fflush(ptr null)
  call void @_Exit(i32 101)
  unreachable
}

define void @quidra_fail_at(ptr %code, ptr %msg, i64 %line, i64 %column) noreturn {
entry:
  call void @quidra_runtime_fail_at(ptr %code, ptr %msg, i64 %line, i64 %column)
  unreachable
}

define void @quidra_fail_code(ptr %code, ptr %msg) noreturn {
entry:
  %line = load i64, ptr @.quidra.source.line
  %column = load i64, ptr @.quidra.source.column
  call void @quidra_fail_at(ptr %code, ptr %msg, i64 %line, i64 %column)
  unreachable
}

@.quidra.stack.depth = internal thread_local global i64 0

define void @quidra_stack_enter() {
entry:
  %depth = load i64, ptr @.quidra.stack.depth
  %next = add i64 %depth, 1
  %too.deep = icmp ugt i64 %next, 4096
  br i1 %too.deep, label %fail, label %ok
fail:
  call void @quidra_fail_code(ptr @.code.stack, ptr @.msg.stack)
  unreachable
ok:
  store i64 %next, ptr @.quidra.stack.depth
  ret void
}

define void @quidra_stack_leave() {
entry:
  %depth = load i64, ptr @.quidra.stack.depth
  %next = sub i64 %depth, 1
  store i64 %next, ptr @.quidra.stack.depth
  ret void
}

define ptr @quidra_alloc(i64 %bytes) {
entry:
  %p = call ptr @quidra_managed_alloc(i64 %bytes)
  ret ptr %p
}
define ptr @quidra_float_text(double %x) {
entry:
  %text = call ptr @quidra_format_float(double %x)
  ret ptr %text
}

define ptr @quidra_array_alloc(i64 %n, i64 %stride, i32 %initialized) {
entry:
  %neg = icmp slt i64 %n, 0
  %stride.bad = icmp ule i64 %stride, 0
  %product = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %n, i64 %stride)
  %data = extractvalue { i64, i1 } %product, 0
  %mul.overflow = extractvalue { i64, i1 } %product, 1
  %sum = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %data, i64 8)
  %bytes = extractvalue { i64, i1 } %sum, 0
  %add.overflow = extractvalue { i64, i1 } %sum, 1
  %bad0 = or i1 %neg, %stride.bad
  %bad1 = or i1 %mul.overflow, %add.overflow
  %bad = or i1 %bad0, %bad1
  br i1 %bad, label %fail, label %ok
fail:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %p = call ptr @quidra_alloc(i64 %bytes)
  store i64 %n, ptr %p, align 1
  %payload = getelementptr inbounds i8, ptr %p, i64 8
  call ptr @memset(ptr %payload, i32 0, i64 %data)
  call void @quidra_init_create(ptr %p, i64 %n, i64 %stride, i64 8, i32 %initialized)
  ret ptr %p
}
define ptr @quidra_array_slot(ptr %array, i64 %index, i64 %stride, i64 %line, i64 %column) alwaysinline {
entry:
  %len = load i64, ptr %array, align 1
  %negative = icmp slt i64 %index, 0
  %past = icmp sge i64 %index, %len
  %bad = or i1 %negative, %past
  br i1 %bad, label %fail, label %ok
fail:
  call void @quidra_runtime_bounds_fail(i64 %index, i64 %len, i64 %line, i64 %column)
  unreachable
ok:
  %mul = mul i64 %index, %stride
  %off = add i64 %mul, 8
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_fixed_array_slot(ptr %array, i64 %index, i64 %len, i64 %stride, i64 %line, i64 %column) alwaysinline {
entry:
  %negative = icmp slt i64 %index, 0
  %past = icmp sge i64 %index, %len
  %bad = or i1 %negative, %past
  br i1 %bad, label %fail, label %ok
fail:
  call void @quidra_runtime_bounds_fail(i64 %index, i64 %len, i64 %line, i64 %column)
  unreachable
ok:
  %off = mul i64 %index, %stride
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_array_slot_proven(ptr %array, i64 %index, i64 %stride) alwaysinline {
entry:
  %mul = mul i64 %index, %stride
  %off = add i64 %mul, 8
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_fixed_array_slot_proven(ptr %array, i64 %index, i64 %stride) alwaysinline {
entry:
  %off = mul i64 %index, %stride
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define i64 @quidra_add(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_abs(i64 %x) {
entry:
  %negative = icmp slt i64 %x, 0
  br i1 %negative, label %neg, label %ok
neg:
  %minimum = icmp eq i64 %x, -9223372036854775808
  br i1 %minimum, label %bad, label %negate
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
negate:
  %r = sub i64 0, %x
  ret i64 %r
ok:
  ret i64 %x
}

define i64 @quidra_sub(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_mul(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_div(i64 %a, i64 %b) {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %divzero, label %overflow_check
divzero:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 0, i64 0)
  unreachable
overflow_check:
  %min = icmp eq i64 %a, -9223372036854775808
  %negone = icmp eq i64 %b, -1
  %overflow = and i1 %min, %negone
  br i1 %overflow, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %r = sdiv i64 %a, %b
  ret i64 %r
}

define i64 @quidra_mod(i64 %a, i64 %b) {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %divzero, label %overflow_check
divzero:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 0, i64 0)
  unreachable
overflow_check:
  %min = icmp eq i64 %a, -9223372036854775808
  %negone = icmp eq i64 %b, -1
  %overflow = and i1 %min, %negone
  br i1 %overflow, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %r = srem i64 %a, %b
  ret i64 %r
}

)LLVM")
};

consteval std::size_t declaration_count() {
    std::size_t count = 0;
    for (const auto& item : prelude_items) {
        if (item.is_declaration) ++count;
    }
    return count;
}

template <llvm_text::AppendableText Text>
constexpr void append_prelude(Text& out) {
    for (const auto& item : prelude_items) {
        if (item.is_declaration) llvm_text::append_declaration(out, *item.declaration);
        else out.append(item.text);
    }
}

// The prelude, written at compile time into a FixedText of the size a
// TextSize counts.
consteval std::size_t prelude_size() {
    llvm_text::TextSize size;
    append_prelude(size);
    return size.size();
}

constexpr auto prelude_text = [] {
    llvm_text::FixedText<prelude_size()> text;
    append_prelude(text);
    return text;
}();

} // namespace

std::string_view runtime_prelude_text() {
    return prelude_text.view();
}

std::span<const llvm_text::LlvmCallee* const> runtime_prelude_declarations() {
    static constexpr auto declarations = [] {
        std::array<const llvm_text::LlvmCallee*, declaration_count()> out{};
        std::size_t next = 0;
        for (const auto& item : prelude_items) {
            if (item.is_declaration) out[next++] = item.declaration;
        }
        return out;
    }();
    return declarations;
}

} // namespace quidra::llvm_backend
