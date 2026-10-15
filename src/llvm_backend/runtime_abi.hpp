#pragma once

// The runtime ABI (namespace runtime_abi): every function that generated
// code calls by name, as an LlvmCallee (its symbol and its LLVM signature),
// and the composed names of the LLVM intrinsics it calls.
//
// Owns:
// - the runtime's entry points, one namespace per family. Each signature is
//   derived from the entry point's C prototype (quidra/abi/runtime_entry_points.hpp;
//   real_atom, real_atom_float64, real_unary and tensor::device from
//   quidra/native_extension.h), so the declaration the prelude writes and
//   every call the backend writes follow the definition the runtime
//   compiles;
// - the helpers the runtime prelude defines in LLVM IR (prelude) and the C
//   library functions generated code calls (c_library, printf among them,
//   which is variadic), whose signatures are written here as the C function
//   types they correspond to;
// - the start of the names of the helpers the backend generates per type
//   (generated_helper_prefix).
//
// The runtime prelude (runtime_prelude.cpp) declares the entry points, in its
// fixed order, from these signatures: an entry point added here gets a
// declare() item there (tests/metadata_ssot_tests.py checks that each one
// has exactly one). LlvmBuilder::call checks a call's arguments against the
// signatures.

#include "llvm_text/llvm_callee.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/standard_classes.hpp"

#include <array>
#include <string>
#include <string_view>

namespace quidra::llvm_backend::runtime_abi {

using llvm_text::llvm_callee;
using llvm_text::LlvmCallee;

namespace real_loop {
inline constexpr LlvmCallee count = llvm_callee<decltype(&quidra_fixed_real_loop_count)>("quidra_fixed_real_loop_count");
inline constexpr LlvmCallee value = llvm_callee<decltype(&quidra_fixed_real_loop_value)>("quidra_fixed_real_loop_value");
inline constexpr LlvmCallee plan = llvm_callee<decltype(&quidra_exact_loop_plan)>("quidra_exact_loop_plan");
inline constexpr LlvmCallee from_scaled = llvm_callee<decltype(&quidra_exact_from_scaled)>("quidra_exact_from_scaled");
inline constexpr LlvmCallee exact_value = llvm_callee<decltype(&quidra_exact_loop_value)>("quidra_exact_loop_value");
inline constexpr LlvmCallee scaled_compare = llvm_callee<decltype(&quidra_exact_scaled_compare)>("quidra_exact_scaled_compare");
inline constexpr LlvmCallee scaled_convert = llvm_callee<decltype(&quidra_exact_scaled_convert)>("quidra_exact_scaled_convert");
}

// The program: its command line and the source provenance of the running statement.
namespace program {
inline constexpr LlvmCallee parallel_frame_create =
    llvm_callee<decltype(&quidra_parallel_frame_create)>("quidra_parallel_frame_create");
inline constexpr LlvmCallee parallel_cleanup_active =
    llvm_callee<decltype(&quidra_parallel_cleanup_active)>("quidra_parallel_cleanup_active");
inline constexpr LlvmCallee parallel_cleanup_rethrow =
    llvm_callee<decltype(&quidra_parallel_cleanup_rethrow)>("quidra_parallel_cleanup_rethrow");
inline constexpr LlvmCallee parallel_frame_cleanup =
    llvm_callee<decltype(&quidra_parallel_frame_cleanup)>("quidra_parallel_frame_cleanup");
inline constexpr LlvmCallee parallel_frame_leave =
    llvm_callee<decltype(&quidra_parallel_frame_leave)>("quidra_parallel_frame_leave");
inline constexpr LlvmCallee process_exit =
    llvm_callee<decltype(&quidra_process_exit)>("quidra_process_exit");
inline constexpr LlvmCallee set_args =
    llvm_callee<decltype(&quidra_runtime_set_args)>("quidra_runtime_set_args");
inline constexpr LlvmCallee set_source_provenance =
    llvm_callee<decltype(&quidra_runtime_set_source_provenance)>(
        "quidra_runtime_set_source_provenance");
// A statement of package code: it names the innermost statement and leaves
// the user's statement alone.
inline constexpr LlvmCallee set_package_source_provenance =
    llvm_callee<decltype(&quidra_runtime_set_package_source_provenance)>(
        "quidra_runtime_set_package_source_provenance");
// The innermost user statement: read before a call from package code that
// may enter user code, and restored after it; also set by a user statement
// without a public node and before a call that may read it.
inline constexpr LlvmCallee user_statement =
    llvm_callee<decltype(&quidra_runtime_user_statement)>("quidra_runtime_user_statement");
inline constexpr LlvmCallee restore_user_statement =
    llvm_callee<decltype(&quidra_runtime_restore_user_statement)>(
        "quidra_runtime_restore_user_statement");
// The module's source table (@.quidra.sources), registered by the entry
// before its first statement.
inline constexpr LlvmCallee register_sources =
    llvm_callee<decltype(&quidra_runtime_register_sources)>("quidra_runtime_register_sources");
// The return of an exported function that may leave device work behind:
// drains it before control goes back to C.
inline constexpr LlvmCallee foreign_leave =
    llvm_callee<decltype(&quidra_runtime_foreign_leave)>("quidra_runtime_foreign_leave");
inline constexpr LlvmCallee export_leave =
    llvm_callee<decltype(&quidra_runtime_export_leave)>("quidra_runtime_export_leave");
} // namespace program

namespace tail {
inline constexpr LlvmCallee record =
    llvm_callee<decltype(&quidra_tail_record)>("quidra_tail_record");
inline constexpr LlvmCallee return_cleanup =
    llvm_callee<decltype(&quidra_tail_return)>("quidra_tail_return");
} // namespace tail

// Runtime failures the runtime reports at a source location.
namespace failure {
inline constexpr LlvmCallee fail_at =
    llvm_callee<decltype(&quidra_runtime_fail_at)>("quidra_runtime_fail_at");
inline constexpr LlvmCallee bounds_fail =
    llvm_callee<decltype(&quidra_runtime_bounds_fail)>("quidra_runtime_bounds_fail");
inline constexpr LlvmCallee conversion_fail =
    llvm_callee<decltype(&quidra_runtime_conversion_fail)>("quidra_runtime_conversion_fail");
// Why the last failed parse, input read or declined conversion of the
// thread failed (the index into an error site's record array).
inline constexpr LlvmCallee last_reason =
    llvm_callee<decltype(&quidra_runtime_last_reason)>("quidra_runtime_last_reason");
// A fail-fast stop of an error value.
inline constexpr LlvmCallee fail_error =
    llvm_callee<decltype(&quidra_runtime_fail_error)>("quidra_runtime_fail_error");
} // namespace failure

// Error values (quidra/abi/error_record.hpp): the records the runtime makes.
namespace error {
inline constexpr LlvmCallee make_user =
    llvm_callee<decltype(&quidra_error_new)>("quidra_error_new");
inline constexpr LlvmCallee make =
    llvm_callee<decltype(&quidra_error_make)>("quidra_error_make");
inline constexpr LlvmCallee from_site =
    llvm_callee<decltype(&quidra_error_from_site)>("quidra_error_from_site");
inline constexpr LlvmCallee file =
    llvm_callee<decltype(&quidra_error_file)>("quidra_error_file");
inline constexpr LlvmCallee json_last =
    llvm_callee<decltype(&quidra_error_json_last)>("quidra_error_json_last");
inline constexpr LlvmCallee http_last =
    llvm_callee<decltype(&quidra_error_http_last)>("quidra_error_http_last");
inline constexpr LlvmCallee detail =
    llvm_callee<decltype(&quidra_error_detail)>("quidra_error_detail");
inline constexpr LlvmCallee detail_int =
    llvm_callee<decltype(&quidra_error_detail_int)>("quidra_error_detail_int");
inline constexpr LlvmCallee equal =
    llvm_callee<decltype(&quidra_error_equal)>("quidra_error_equal");
inline constexpr LlvmCallee display =
    llvm_callee<decltype(&quidra_error_display)>("quidra_error_display");
inline constexpr LlvmCallee hop =
    llvm_callee<decltype(&quidra_error_hop)>("quidra_error_hop");
inline constexpr LlvmCallee hop_indirect =
    llvm_callee<decltype(&quidra_error_hop_indirect)>("quidra_error_hop_indirect");
} // namespace error

// Test assertions.
namespace check {
inline constexpr LlvmCallee for_step =
    llvm_callee<decltype(&quidra_for_step_check)>("quidra_for_step_check");
inline constexpr LlvmCallee test_assert =
    llvm_callee<decltype(&quidra_test_assert)>("quidra_test_assert");
} // namespace check

// Managed storage and its element initialization tracking.
namespace memory {
inline constexpr LlvmCallee real_op_drop = llvm_callee<decltype(&quidra_real_op_drop)>("quidra_real_op_drop");
inline constexpr LlvmCallee managed_alloc =
    llvm_callee<decltype(&quidra_managed_alloc)>("quidra_managed_alloc");
inline constexpr LlvmCallee managed_retain =
    llvm_callee<decltype(&quidra_managed_retain)>("quidra_managed_retain");
inline constexpr LlvmCallee managed_release =
    llvm_callee<decltype(&quidra_managed_release)>("quidra_managed_release");
inline constexpr LlvmCallee reference_root_serial =
    llvm_callee<decltype(&quidra_reference_root_serial)>("quidra_reference_root_serial");
inline constexpr LlvmCallee managed_pin =
    llvm_callee<decltype(&quidra_managed_pin)>("quidra_managed_pin");
inline constexpr LlvmCallee managed_unpin =
    llvm_callee<decltype(&quidra_managed_unpin)>("quidra_managed_unpin");
inline constexpr LlvmCallee init_create_unowned =
    llvm_callee<decltype(&quidra_init_create_unowned)>("quidra_init_create_unowned");
inline constexpr LlvmCallee init_create =
    llvm_callee<decltype(&quidra_init_create)>("quidra_init_create");
inline constexpr LlvmCallee init_mark_range =
    llvm_callee<decltype(&quidra_init_mark_range)>("quidra_init_mark_range");
inline constexpr LlvmCallee init_check =
    llvm_callee<decltype(&quidra_init_check)>("quidra_init_check");
inline constexpr LlvmCallee init_require_range =
    llvm_callee<decltype(&quidra_init_require_range)>("quidra_init_require_range");
inline constexpr LlvmCallee init_clone =
    llvm_callee<decltype(&quidra_init_clone)>("quidra_init_clone");
} // namespace memory

// Dynamic arrays: append storage, sorting and initialization.
namespace array {
inline constexpr LlvmCallee initialization_complete =
    llvm_callee<decltype(&quidra_array_initialization_complete)>(
        "quidra_array_initialization_complete");
inline constexpr LlvmCallee can_append_move =
    llvm_callee<decltype(&quidra_array_can_append_move)>("quidra_array_can_append_move");
inline constexpr LlvmCallee grow_move =
    llvm_callee<decltype(&quidra_array_grow_move)>("quidra_array_grow_move");
inline constexpr LlvmCallee sorted =
    llvm_callee<decltype(&quidra_array_sorted)>("quidra_array_sorted");
inline constexpr LlvmCallee sorted_desc =
    llvm_callee<decltype(&quidra_array_sorted_desc)>("quidra_array_sorted_desc");
} // namespace array

// Bins (bit strings).
namespace bin {
inline constexpr LlvmCallee try_utf8 =
    llvm_callee<decltype(&quidra_bin_try_utf8)>("quidra_bin_try_utf8");
inline constexpr LlvmCallee alloc = llvm_callee<decltype(&quidra_bin_alloc)>("quidra_bin_alloc");
inline constexpr LlvmCallee index = llvm_callee<decltype(&quidra_bin_index)>("quidra_bin_index");
inline constexpr LlvmCallee set = llvm_callee<decltype(&quidra_bin_set)>("quidra_bin_set");
inline constexpr LlvmCallee slice = llvm_callee<decltype(&quidra_bin_slice)>("quidra_bin_slice");
inline constexpr LlvmCallee slice_planned =
    llvm_callee<decltype(&quidra_bin_slice_planned)>("quidra_bin_slice_planned");
inline constexpr LlvmCallee parse = llvm_callee<decltype(&quidra_bin_parse)>("quidra_bin_parse");
inline constexpr LlvmCallee string = llvm_callee<decltype(&quidra_bin_string)>("quidra_bin_string");
inline constexpr LlvmCallee from_u64 =
    llvm_callee<decltype(&quidra_bin_from_u64)>("quidra_bin_from_u64");
inline constexpr LlvmCallee to_u64 = llvm_callee<decltype(&quidra_bin_to_u64)>("quidra_bin_to_u64");
inline constexpr LlvmCallee from_array =
    llvm_callee<decltype(&quidra_bin_from_array)>("quidra_bin_from_array");
inline constexpr LlvmCallee to_array =
    llvm_callee<decltype(&quidra_bin_to_array)>("quidra_bin_to_array");
inline constexpr LlvmCallee clone = llvm_callee<decltype(&quidra_bin_clone)>("quidra_bin_clone");
inline constexpr LlvmCallee equal = llvm_callee<decltype(&quidra_bin_equal)>("quidra_bin_equal");
inline constexpr LlvmCallee byte_length =
    llvm_callee<decltype(&quidra_bin_byte_length)>("quidra_bin_byte_length");
} // namespace bin

// Strings.
namespace text {
inline constexpr LlvmCallee index =
    llvm_callee<decltype(&quidra_string_index)>("quidra_string_index");
inline constexpr LlvmCallee index_equal_ascii =
    llvm_callee<decltype(&quidra_string_index_equal_ascii)>("quidra_string_index_equal_ascii");
inline constexpr LlvmCallee count_ascii_prefix =
    llvm_callee<decltype(&quidra_string_count_ascii_prefix)>("quidra_string_count_ascii_prefix");
inline constexpr LlvmCallee length =
    llvm_callee<decltype(&quidra_string_length)>("quidra_string_length");
inline constexpr LlvmCallee contains =
    llvm_callee<decltype(&quidra_string_contains)>("quidra_string_contains");
inline constexpr LlvmCallee starts_with =
    llvm_callee<decltype(&quidra_string_starts_with)>("quidra_string_starts_with");
inline constexpr LlvmCallee ends_with =
    llvm_callee<decltype(&quidra_string_ends_with)>("quidra_string_ends_with");
inline constexpr LlvmCallee find = llvm_callee<decltype(&quidra_string_find)>("quidra_string_find");
inline constexpr LlvmCallee slice =
    llvm_callee<decltype(&quidra_string_slice)>("quidra_string_slice");
inline constexpr LlvmCallee slice_stepped =
    llvm_callee<decltype(&quidra_string_slice_stepped)>("quidra_string_slice_stepped");
inline constexpr LlvmCallee trim = llvm_callee<decltype(&quidra_string_trim)>("quidra_string_trim");
inline constexpr LlvmCallee split =
    llvm_callee<decltype(&quidra_string_split)>("quidra_string_split");
inline constexpr LlvmCallee split_iter_begin =
    llvm_callee<decltype(&quidra_string_split_iter_begin)>("quidra_string_split_iter_begin");
inline constexpr LlvmCallee split_iter_begin_move =
    llvm_callee<decltype(&quidra_string_split_iter_begin_move)>(
        "quidra_string_split_iter_begin_move");
inline constexpr LlvmCallee split_iter_next =
    llvm_callee<decltype(&quidra_string_split_iter_next)>("quidra_string_split_iter_next");
inline constexpr LlvmCallee split_iter_end =
    llvm_callee<decltype(&quidra_string_split_iter_end)>("quidra_string_split_iter_end");
inline constexpr LlvmCallee parse_two_signed =
    llvm_callee<decltype(&quidra_string_parse_two_signed)>("quidra_string_parse_two_signed");
inline constexpr LlvmCallee utf8 = llvm_callee<decltype(&quidra_string_utf8)>("quidra_string_utf8");
inline constexpr LlvmCallee u8_array_try_utf8 =
    llvm_callee<decltype(&quidra_u8_array_try_utf8)>("quidra_u8_array_try_utf8");
inline constexpr LlvmCallee codepoints =
    llvm_callee<decltype(&quidra_string_codepoints)>("quidra_string_codepoints");
inline constexpr LlvmCallee join = llvm_callee<decltype(&quidra_string_join)>("quidra_string_join");
inline constexpr LlvmCallee concat_many =
    llvm_callee<decltype(&quidra_string_concat_many)>("quidra_string_concat_many");
inline constexpr LlvmCallee build =
    llvm_callee<decltype(&quidra_string_build)>("quidra_string_build");
inline constexpr LlvmCallee build_append_move =
    llvm_callee<decltype(&quidra_string_build_append_move)>("quidra_string_build_append_move");
inline constexpr LlvmCallee build_append_move_unique =
    llvm_callee<decltype(&quidra_string_build_append_move_unique)>(
        "quidra_string_build_append_move_unique");
inline constexpr LlvmCallee build_append_move_unique_direct =
    llvm_callee<decltype(&quidra_string_build_append_move_unique_direct)>(
        "quidra_string_build_append_move_unique_direct");
inline constexpr LlvmCallee build_append_last_length =
    llvm_callee<decltype(&quidra_string_build_append_last_length)>(
        "quidra_string_build_append_last_length");
inline constexpr LlvmCallee concat2 =
    llvm_callee<decltype(&quidra_string_concat2)>("quidra_string_concat2");
inline constexpr LlvmCallee equal =
    llvm_callee<decltype(&quidra_string_equal)>("quidra_string_equal");
inline constexpr LlvmCallee can_append_move =
    llvm_callee<decltype(&quidra_string_can_append_move)>("quidra_string_can_append_move");
inline constexpr LlvmCallee append_move_many =
    llvm_callee<decltype(&quidra_string_append_move_many)>("quidra_string_append_move_many");
inline constexpr LlvmCallee repeat =
    llvm_callee<decltype(&quidra_string_repeat)>("quidra_string_repeat");
} // namespace text

// Integer and float conversions, formatting, parsing and integer powers.
namespace numeric {
inline constexpr LlvmCallee integer_text_signed =
    llvm_callee<decltype(&quidra_integer_text_signed)>("quidra_integer_text_signed");
inline constexpr LlvmCallee integer_text_unsigned =
    llvm_callee<decltype(&quidra_integer_text_unsigned)>("quidra_integer_text_unsigned");
inline constexpr LlvmCallee cast_element =
    llvm_callee<decltype(&quidra_numeric_cast_element)>("quidra_numeric_cast_element");
inline constexpr LlvmCallee cast_element_fits =
    llvm_callee<decltype(&quidra_numeric_cast_element_fits)>("quidra_numeric_cast_element_fits");
inline constexpr LlvmCallee format_float =
    llvm_callee<decltype(&quidra_format_float)>("quidra_format_float");
inline constexpr LlvmCallee format_signed =
    llvm_callee<decltype(&quidra_format_signed)>("quidra_format_signed");
inline constexpr LlvmCallee format_unsigned =
    llvm_callee<decltype(&quidra_format_unsigned)>("quidra_format_unsigned");
inline constexpr LlvmCallee format_number =
    llvm_callee<decltype(&quidra_format_number)>("quidra_format_number");
inline constexpr LlvmCallee format_number32 =
    llvm_callee<decltype(&quidra_format_number32)>("quidra_format_number32");
inline constexpr LlvmCallee integer_pow_signed =
    llvm_callee<decltype(&quidra_integer_pow_signed)>("quidra_integer_pow_signed");
inline constexpr LlvmCallee integer_pow_unsigned =
    llvm_callee<decltype(&quidra_integer_pow_unsigned)>("quidra_integer_pow_unsigned");
inline constexpr LlvmCallee parse_signed =
    llvm_callee<decltype(&quidra_parse_signed)>("quidra_parse_signed");
inline constexpr LlvmCallee parse_unsigned =
    llvm_callee<decltype(&quidra_parse_unsigned)>("quidra_parse_unsigned");
inline constexpr LlvmCallee parse_float32 =
    llvm_callee<decltype(&quidra_parse_float32)>("quidra_parse_float32");
inline constexpr LlvmCallee parse_float64 =
    llvm_callee<decltype(&quidra_parse_float64)>("quidra_parse_float64");
} // namespace numeric

// Big integers, exact reals and exact-real providers.
namespace exact {
inline constexpr LlvmCallee bigint_literal =
    llvm_callee<decltype(&quidra_bigint_literal)>("quidra_bigint_literal");
inline constexpr LlvmCallee bigint_parse =
    llvm_callee<decltype(&quidra_bigint_parse)>("quidra_bigint_parse");
inline constexpr LlvmCallee bigreal_literal =
    llvm_callee<decltype(&quidra_bigreal_literal)>("quidra_bigreal_literal");
inline constexpr LlvmCallee bigreal_parse =
    llvm_callee<decltype(&quidra_bigreal_parse)>("quidra_bigreal_parse");
inline constexpr LlvmCallee real_atom =
    llvm_callee<decltype(&qcore_exact_real_atom)>("qcore_exact_real_atom");
inline constexpr LlvmCallee real_atom_float64 =
    llvm_callee<decltype(&qcore_exact_real_atom_float64)>("qcore_exact_real_atom_float64");
inline constexpr LlvmCallee real_unary =
    llvm_callee<decltype(&qcore_exact_real_unary)>("qcore_exact_real_unary");
inline constexpr LlvmCallee bigint_drop =
    llvm_callee<decltype(&quidra_bigint_drop)>("quidra_bigint_drop");
inline constexpr LlvmCallee bigreal_drop =
    llvm_callee<decltype(&quidra_bigreal_drop)>("quidra_bigreal_drop");
inline constexpr LlvmCallee bigint_text =
    llvm_callee<decltype(&quidra_bigint_text)>("quidra_bigint_text");
inline constexpr LlvmCallee bigreal_text =
    llvm_callee<decltype(&quidra_bigreal_text)>("quidra_bigreal_text");
inline constexpr LlvmCallee bigint_neg =
    llvm_callee<decltype(&quidra_bigint_neg)>("quidra_bigint_neg");
inline constexpr LlvmCallee bigint_binary =
    llvm_callee<decltype(&quidra_bigint_binary)>("quidra_bigint_binary");
inline constexpr LlvmCallee bigint_pow =
    llvm_callee<decltype(&quidra_bigint_pow)>("quidra_bigint_pow");
inline constexpr LlvmCallee bigint_compare =
    llvm_callee<decltype(&quidra_bigint_compare)>("quidra_bigint_compare");
inline constexpr LlvmCallee bigreal_neg =
    llvm_callee<decltype(&quidra_bigreal_neg)>("quidra_bigreal_neg");
inline constexpr LlvmCallee bigreal_binary =
    llvm_callee<decltype(&quidra_bigreal_binary)>("quidra_bigreal_binary");
inline constexpr LlvmCallee bigreal_compare =
    llvm_callee<decltype(&quidra_bigreal_compare)>("quidra_bigreal_compare");
inline constexpr LlvmCallee bigreal_pow =
    llvm_callee<decltype(&quidra_bigreal_pow)>("quidra_bigreal_pow");
inline constexpr LlvmCallee bigint_from_i64 =
    llvm_callee<decltype(&quidra_bigint_from_i64)>("quidra_bigint_from_i64");
inline constexpr LlvmCallee bigint_from_u64 =
    llvm_callee<decltype(&quidra_bigint_from_u64)>("quidra_bigint_from_u64");
inline constexpr LlvmCallee bigreal_from_i64 =
    llvm_callee<decltype(&quidra_bigreal_from_i64)>("quidra_bigreal_from_i64");
inline constexpr LlvmCallee bigreal_from_u64 =
    llvm_callee<decltype(&quidra_bigreal_from_u64)>("quidra_bigreal_from_u64");
inline constexpr LlvmCallee bigreal_from_float64 =
    llvm_callee<decltype(&quidra_bigreal_from_float64)>("quidra_bigreal_from_float64");
inline constexpr LlvmCallee bigreal_from_bigint =
    llvm_callee<decltype(&quidra_bigreal_from_bigint)>("quidra_bigreal_from_bigint");
inline constexpr LlvmCallee bigreal_to_bigint =
    llvm_callee<decltype(&quidra_bigreal_to_bigint)>("quidra_bigreal_to_bigint");
inline constexpr LlvmCallee bigint_try_i64 =
    llvm_callee<decltype(&quidra_bigint_try_i64)>("quidra_bigint_try_i64");
inline constexpr LlvmCallee bigint_try_u64 =
    llvm_callee<decltype(&quidra_bigint_try_u64)>("quidra_bigint_try_u64");
inline constexpr LlvmCallee bigreal_try_bigint =
    llvm_callee<decltype(&quidra_bigreal_try_bigint)>("quidra_bigreal_try_bigint");
inline constexpr LlvmCallee bigreal_try_i64 =
    llvm_callee<decltype(&quidra_bigreal_try_i64)>("quidra_bigreal_try_i64");
inline constexpr LlvmCallee bigreal_try_u64 =
    llvm_callee<decltype(&quidra_bigreal_try_u64)>("quidra_bigreal_try_u64");
inline constexpr LlvmCallee bigint_try_float64 =
    llvm_callee<decltype(&quidra_bigint_try_float64)>("quidra_bigint_try_float64");
inline constexpr LlvmCallee bigint_try_float32 =
    llvm_callee<decltype(&quidra_bigint_try_float32)>("quidra_bigint_try_float32");
inline constexpr LlvmCallee numeric_cast_fits =
    llvm_callee<decltype(&quidra_exact_numeric_cast_fits)>("quidra_exact_numeric_cast_fits");
inline constexpr LlvmCallee bigreal_try_float64 =
    llvm_callee<decltype(&quidra_bigreal_try_float64)>("quidra_bigreal_try_float64");
inline constexpr LlvmCallee bigreal_try_float32 =
    llvm_callee<decltype(&quidra_bigreal_try_float32)>("quidra_bigreal_try_float32");
inline constexpr LlvmCallee bigint_to_i64 =
    llvm_callee<decltype(&quidra_bigint_to_i64)>("quidra_bigint_to_i64");
inline constexpr LlvmCallee bigint_to_u64 =
    llvm_callee<decltype(&quidra_bigint_to_u64)>("quidra_bigint_to_u64");
inline constexpr LlvmCallee bigreal_to_float64 =
    llvm_callee<decltype(&quidra_bigreal_to_float64)>("quidra_bigreal_to_float64");
inline constexpr LlvmCallee bigreal_to_float32 =
    llvm_callee<decltype(&quidra_bigreal_to_float32)>("quidra_bigreal_to_float32");
inline constexpr LlvmCallee bigint_to_float64 =
    llvm_callee<decltype(&quidra_bigint_to_float64)>("quidra_bigint_to_float64");
inline constexpr LlvmCallee bigint_to_float32 =
    llvm_callee<decltype(&quidra_bigint_to_float32)>("quidra_bigint_to_float32");
} // namespace exact

// The small-rational entry points of `real` values. The support block of a
// module that uses `real` declares them (small_rational.hpp), not the
// prelude.
namespace exact_real_support {
inline constexpr LlvmCallee real_node_cleanup_active =
    llvm_callee<decltype(&quidra_real_node_cleanup_active)>("quidra_real_node_cleanup_active");
inline constexpr LlvmCallee real_node_unary =
    llvm_callee<decltype(&quidra_real_node_unary)>("quidra_real_node_unary");
inline constexpr LlvmCallee real_node_binary =
    llvm_callee<decltype(&quidra_real_node_binary)>("quidra_real_node_binary");
inline constexpr LlvmCallee real_node_compare =
    llvm_callee<decltype(&quidra_real_node_compare)>("quidra_real_node_compare");
inline constexpr LlvmCallee real_node_pow =
    llvm_callee<decltype(&quidra_real_node_pow)>("quidra_real_node_pow");
inline constexpr LlvmCallee real_promote =
    llvm_callee<decltype(&quidra_real_promote)>("quidra_real_promote");
inline constexpr LlvmCallee real_demote =
    llvm_callee<decltype(&quidra_real_demote)>("quidra_real_demote");
inline constexpr LlvmCallee real_small_binary =
    llvm_callee<decltype(&quidra_real_small_binary)>("quidra_real_small_binary");
inline constexpr LlvmCallee real_small_text =
    llvm_callee<decltype(&quidra_real_small_text)>("quidra_real_small_text");
inline constexpr LlvmCallee real_small_try_float64 =
    llvm_callee<decltype(&quidra_real_small_try_float64)>("quidra_real_small_try_float64");
inline constexpr LlvmCallee real_small_try_float32 =
    llvm_callee<decltype(&quidra_real_small_try_float32)>("quidra_real_small_try_float32");
inline constexpr LlvmCallee real_small_to_float64 =
    llvm_callee<decltype(&quidra_real_small_to_float64)>("quidra_real_small_to_float64");
inline constexpr LlvmCallee real_small_to_float32 =
    llvm_callee<decltype(&quidra_real_small_to_float32)>("quidra_real_small_to_float32");
inline constexpr LlvmCallee real_small_try_i64 =
    llvm_callee<decltype(&quidra_real_small_try_i64)>("quidra_real_small_try_i64");
inline constexpr LlvmCallee real_small_try_u64 =
    llvm_callee<decltype(&quidra_real_small_try_u64)>("quidra_real_small_try_u64");
inline constexpr LlvmCallee real_small_try_bigint =
    llvm_callee<decltype(&quidra_real_small_try_bigint)>("quidra_real_small_try_bigint");
inline constexpr LlvmCallee real_small_to_bigint =
    llvm_callee<decltype(&quidra_real_small_to_bigint)>("quidra_real_small_to_bigint");
inline constexpr LlvmCallee real_small_cast_fits =
    llvm_callee<decltype(&quidra_real_small_cast_fits)>("quidra_real_small_cast_fits");
} // namespace exact_real_support

// The entry points of the exact-real intrinsics that only some modules use
// (real.op, interpolation formats of real). Their support block declares them
// (small_rational.hpp), not the prelude.
namespace exact_extension_support {
inline constexpr LlvmCallee real_op_push =
    llvm_callee<decltype(&quidra_real_op_push)>("quidra_real_op_push");
inline constexpr LlvmCallee real_op_apply =
    llvm_callee<decltype(&quidra_real_op_apply)>("quidra_real_op_apply");
inline constexpr LlvmCallee real_format =
    llvm_callee<decltype(&quidra_real_format)>("quidra_real_format");
inline constexpr LlvmCallee real_round =
    llvm_callee<decltype(&quidra_real_round)>("quidra_real_round");
} // namespace exact_extension_support

// The entry points of the arbitrary-precision integer words. The support
// block of a module that uses them declares them (bare_integer.hpp), not the
// prelude.
namespace bare_integer_support {
inline constexpr LlvmCallee int_add =
    llvm_callee<decltype(&quidra_int_add)>("quidra_int_add");
inline constexpr LlvmCallee int_sub =
    llvm_callee<decltype(&quidra_int_sub)>("quidra_int_sub");
inline constexpr LlvmCallee int_mul =
    llvm_callee<decltype(&quidra_int_mul)>("quidra_int_mul");
inline constexpr LlvmCallee int_div =
    llvm_callee<decltype(&quidra_int_div)>("quidra_int_div");
inline constexpr LlvmCallee int_rem =
    llvm_callee<decltype(&quidra_int_rem)>("quidra_int_rem");
inline constexpr LlvmCallee int_neg =
    llvm_callee<decltype(&quidra_int_neg)>("quidra_int_neg");
inline constexpr LlvmCallee int_pow =
    llvm_callee<decltype(&quidra_int_pow)>("quidra_int_pow");
inline constexpr LlvmCallee int_compare =
    llvm_callee<decltype(&quidra_int_compare)>("quidra_int_compare");
inline constexpr LlvmCallee int_text =
    llvm_callee<decltype(&quidra_int_text)>("quidra_int_text");
inline constexpr LlvmCallee int_parse =
    llvm_callee<decltype(&quidra_int_parse)>("quidra_int_parse");
inline constexpr LlvmCallee int_literal =
    llvm_callee<decltype(&quidra_int_literal)>("quidra_int_literal");
inline constexpr LlvmCallee int_from_i64 =
    llvm_callee<decltype(&quidra_int_from_i64)>("quidra_int_from_i64");
inline constexpr LlvmCallee int_from_u64 =
    llvm_callee<decltype(&quidra_int_from_u64)>("quidra_int_from_u64");
inline constexpr LlvmCallee int_to_i64_checked =
    llvm_callee<decltype(&quidra_int_to_i64_checked)>("quidra_int_to_i64_checked");
inline constexpr LlvmCallee int_to_u64_checked =
    llvm_callee<decltype(&quidra_int_to_u64_checked)>("quidra_int_to_u64_checked");
inline constexpr LlvmCallee int_try_i64 =
    llvm_callee<decltype(&quidra_int_try_i64)>("quidra_int_try_i64");
inline constexpr LlvmCallee int_try_u64 =
    llvm_callee<decltype(&quidra_int_try_u64)>("quidra_int_try_u64");
inline constexpr LlvmCallee int_to_float64 =
    llvm_callee<decltype(&quidra_int_to_float64)>("quidra_int_to_float64");
inline constexpr LlvmCallee int_to_float32 =
    llvm_callee<decltype(&quidra_int_to_float32)>("quidra_int_to_float32");
inline constexpr LlvmCallee int_try_float64 =
    llvm_callee<decltype(&quidra_int_try_float64)>("quidra_int_try_float64");
inline constexpr LlvmCallee int_try_float32 =
    llvm_callee<decltype(&quidra_int_try_float32)>("quidra_int_try_float32");
inline constexpr LlvmCallee int_cast_fits =
    llvm_callee<decltype(&quidra_int_cast_fits)>("quidra_int_cast_fits");
inline constexpr LlvmCallee int_adopt =
    llvm_callee<decltype(&quidra_int_adopt)>("quidra_int_adopt");
inline constexpr LlvmCallee nat_sub =
    llvm_callee<decltype(&quidra_nat_sub)>("quidra_nat_sub");
inline constexpr LlvmCallee int_format =
    llvm_callee<decltype(&quidra_int_format)>("quidra_int_format");
} // namespace bare_integer_support

// The failures of real values that are not finite: a real operation's
// result, and an extern function's result. The support block of a module
// with such checks declares them (finite_check_support.hpp), not the
// prelude.
namespace finite_support {
inline constexpr LlvmCallee real_fail =
    llvm_callee<decltype(&quidra_runtime_real_fail)>("quidra_runtime_real_fail");
// The failure of an extern function whose real result is not finite.
inline constexpr LlvmCallee extern_nonfinite =
    llvm_callee<decltype(&quidra_runtime_extern_nonfinite)>("quidra_runtime_extern_nonfinite");
// The validation of a real tensor an extern function wrote.
inline constexpr LlvmCallee validate_package =
    llvm_callee<decltype(&quidra_tensor_validate_package)>("quidra_tensor_validate_package");
} // namespace finite_support

// The text, parsing and conversions of the 16-bit reals. The support block
// of a module that uses them declares them (half_precision.hpp), not the
// prelude.
namespace half_support {
inline constexpr LlvmCallee try_real =
    llvm_callee<decltype(&quidra_half_try_real)>("quidra_half_try_real");
inline constexpr LlvmCallee text =
    llvm_callee<decltype(&quidra_half_format)>("quidra_half_format");
inline constexpr LlvmCallee parse =
    llvm_callee<decltype(&quidra_half_parse)>("quidra_half_parse");
inline constexpr LlvmCallee real16b_from_float64 =
    llvm_callee<decltype(&quidra_real16b_from_float64)>("quidra_real16b_from_float64");
inline constexpr LlvmCallee real16b_from_int64 =
    llvm_callee<decltype(&quidra_real16b_from_int64)>("quidra_real16b_from_int64");
inline constexpr LlvmCallee real16b_from_uint64 =
    llvm_callee<decltype(&quidra_real16b_from_uint64)>("quidra_real16b_from_uint64");
} // namespace half_support

// Complex values (complex_arithmetic.hpp); declared by the support block of
// a module that uses them, not the prelude.
namespace complex_support {
inline constexpr LlvmCallee text =
    llvm_callee<decltype(&quidra_complex_text)>("quidra_complex_text");
inline constexpr LlvmCallee parse =
    llvm_callee<decltype(&quidra_complex_parse)>("quidra_complex_parse");
inline constexpr LlvmCallee pow =
    llvm_callee<decltype(&quidra_complex_pow)>("quidra_complex_pow");
inline constexpr LlvmCallee from_parts =
    llvm_callee<decltype(&quidra_complex_from_parts)>("quidra_complex_from_parts");
inline constexpr LlvmCallee from_integer =
    llvm_callee<decltype(&quidra_complex_from_integer)>("quidra_complex_from_integer");
inline constexpr LlvmCallee from_real =
    llvm_callee<decltype(&quidra_complex_from_real)>("quidra_complex_from_real");
inline constexpr LlvmCallee from_com =
    llvm_callee<decltype(&quidra_complex_from_com)>("quidra_complex_from_com");
inline constexpr LlvmCallee com_drop =
    llvm_callee<decltype(&quidra_com_drop)>("quidra_com_drop");
inline constexpr LlvmCallee com_make =
    llvm_callee<decltype(&quidra_com_make)>("quidra_com_make");
inline constexpr LlvmCallee com_from_float64 =
    llvm_callee<decltype(&quidra_com_from_float64)>("quidra_com_from_float64");
inline constexpr LlvmCallee com_from_integer =
    llvm_callee<decltype(&quidra_com_from_integer)>("quidra_com_from_integer");
inline constexpr LlvmCallee com_binary =
    llvm_callee<decltype(&quidra_com_binary)>("quidra_com_binary");
inline constexpr LlvmCallee com_neg =
    llvm_callee<decltype(&quidra_com_neg)>("quidra_com_neg");
inline constexpr LlvmCallee com_equal =
    llvm_callee<decltype(&quidra_com_equal)>("quidra_com_equal");
inline constexpr LlvmCallee com_text =
    llvm_callee<decltype(&quidra_com_text)>("quidra_com_text");
inline constexpr LlvmCallee com_parse =
    llvm_callee<decltype(&quidra_com_parse)>("quidra_com_parse");
} // namespace complex_support
// The check of a tensor axis that a shape variable fixes. The support block
// of a module that checks one declares it (shape_variable_check.hpp), not
// the prelude.
namespace shape_variable_support {
inline constexpr LlvmCallee check =
    llvm_callee<decltype(&quidra_tensor_shape_variable_check)>(
        "quidra_tensor_shape_variable_check");
} // namespace shape_variable_support

// Tensors and device synchronization.
namespace tensor {
inline constexpr LlvmCallee array_borrow_begin =
    llvm_callee<decltype(&quidra_tensor_array_borrow_begin)>("quidra_tensor_array_borrow_begin");
inline constexpr LlvmCallee array_borrow_end =
    llvm_callee<decltype(&quidra_tensor_array_borrow_end)>("quidra_tensor_array_borrow_end");
inline constexpr LlvmCallee array_validate_package =
    llvm_callee<decltype(&quidra_tensor_array_validate_package)>("quidra_tensor_array_validate_package");
inline constexpr LlvmCallee gpu_sync = llvm_callee<decltype(&quidra_gpu_sync)>("quidra_gpu_sync");
inline constexpr LlvmCallee create =
    llvm_callee<decltype(&quidra_tensor_create)>("quidra_tensor_create");
inline constexpr LlvmCallee to_gpu =
    llvm_callee<decltype(&quidra_tensor_to_gpu)>("quidra_tensor_to_gpu");
inline constexpr LlvmCallee to_cpu =
    llvm_callee<decltype(&quidra_tensor_to_cpu)>("quidra_tensor_to_cpu");
inline constexpr LlvmCallee clone =
    llvm_callee<decltype(&quidra_tensor_clone)>("quidra_tensor_clone");
inline constexpr LlvmCallee drop = llvm_callee<decltype(&quidra_tensor_drop)>("quidra_tensor_drop");
inline constexpr LlvmCallee reshape =
    llvm_callee<decltype(&quidra_tensor_reshape)>("quidra_tensor_reshape");
inline constexpr LlvmCallee gather =
    llvm_callee<decltype(&quidra_tensor_gather)>("quidra_tensor_gather");
inline constexpr LlvmCallee mask_select =
    llvm_callee<decltype(&quidra_tensor_mask_select)>("quidra_tensor_mask_select");
inline constexpr LlvmCallee scatter =
    llvm_callee<decltype(&quidra_tensor_scatter)>("quidra_tensor_scatter");
inline constexpr LlvmCallee transpose =
    llvm_callee<decltype(&quidra_tensor_transpose)>("quidra_tensor_transpose");
inline constexpr LlvmCallee contiguous =
    llvm_callee<decltype(&quidra_tensor_contiguous)>("quidra_tensor_contiguous");
inline constexpr LlvmCallee shape =
    llvm_callee<decltype(&quidra_tensor_shape)>("quidra_tensor_shape");
inline constexpr LlvmCallee shape_fixed =
    llvm_callee<decltype(&quidra_tensor_shape_fixed)>("quidra_tensor_shape_fixed");
inline constexpr LlvmCallee device =
    llvm_callee<decltype(&qcore_tensor_device)>("qcore_tensor_device");
inline constexpr LlvmCallee is_contiguous =
    llvm_callee<decltype(&quidra_tensor_is_contiguous)>("quidra_tensor_is_contiguous");
inline constexpr LlvmCallee is_tracked =
    llvm_callee<decltype(&quidra_tensor_is_tracked)>("quidra_tensor_is_tracked");
inline constexpr LlvmCallee has_grad =
    llvm_callee<decltype(&quidra_tensor_has_grad)>("quidra_tensor_has_grad");
inline constexpr LlvmCallee clear_grad =
    llvm_callee<decltype(&quidra_tensor_clear_grad)>("quidra_tensor_clear_grad");
inline constexpr LlvmCallee item_ptr =
    llvm_callee<decltype(&quidra_tensor_item_ptr)>("quidra_tensor_item_ptr");
inline constexpr LlvmCallee track =
    llvm_callee<decltype(&quidra_tensor_track)>("quidra_tensor_track");
inline constexpr LlvmCallee track_target =
    llvm_callee<decltype(&quidra_tensor_track_target)>("quidra_tensor_track_target");
inline constexpr LlvmCallee untrack =
    llvm_callee<decltype(&quidra_tensor_untrack)>("quidra_tensor_untrack");
inline constexpr LlvmCallee retrack =
    llvm_callee<decltype(&quidra_tensor_retrack)>("quidra_tensor_retrack");
inline constexpr LlvmCallee backward_many =
    llvm_callee<decltype(&quidra_tensor_backward_many)>("quidra_tensor_backward_many");
inline constexpr LlvmCallee backward_many_with_autograd_targets =
    llvm_callee<decltype(&quidra_tensor_backward_many_with_autograd_targets)>(
        "quidra_tensor_backward_many_with_autograd_targets");
inline constexpr LlvmCallee grad = llvm_callee<decltype(&quidra_tensor_grad)>("quidra_tensor_grad");
inline constexpr LlvmCallee cast = llvm_callee<decltype(&quidra_tensor_cast)>("quidra_tensor_cast");
inline constexpr LlvmCallee try_cast =
    llvm_callee<decltype(&quidra_tensor_try_cast)>("quidra_tensor_try_cast");
inline constexpr LlvmCallee rank_check =
    llvm_callee<decltype(&quidra_tensor_rank_check)>("quidra_tensor_rank_check");
inline constexpr LlvmCallee extent_check =
    llvm_callee<decltype(&quidra_tensor_extent_check)>("quidra_tensor_extent_check");
inline constexpr LlvmCallee unary =
    llvm_callee<decltype(&quidra_tensor_unary)>("quidra_tensor_unary");
inline constexpr LlvmCallee binary =
    llvm_callee<decltype(&quidra_tensor_binary)>("quidra_tensor_binary");
inline constexpr LlvmCallee binary_proven =
    llvm_callee<decltype(&quidra_tensor_binary_proven)>("quidra_tensor_binary_proven");
inline constexpr LlvmCallee compare =
    llvm_callee<decltype(&quidra_tensor_compare)>("quidra_tensor_compare");
inline constexpr LlvmCallee compare_proven =
    llvm_callee<decltype(&quidra_tensor_compare_proven)>("quidra_tensor_compare_proven");
inline constexpr LlvmCallee bool_reduce =
    llvm_callee<decltype(&quidra_tensor_bool_reduce)>("quidra_tensor_bool_reduce");
inline constexpr LlvmCallee index =
    llvm_callee<decltype(&quidra_tensor_index)>("quidra_tensor_index");
inline constexpr LlvmCallee set = llvm_callee<decltype(&quidra_tensor_set)>("quidra_tensor_set");
} // namespace tensor

// Autograd targets.
namespace autograd {
inline constexpr LlvmCallee target_create =
    llvm_callee<decltype(&quidra_autograd_target_create)>("quidra_autograd_target_create");
inline constexpr LlvmCallee target_clone =
    llvm_callee<decltype(&quidra_autograd_target_clone)>("quidra_autograd_target_clone");
inline constexpr LlvmCallee target_drop =
    llvm_callee<decltype(&quidra_autograd_target_drop)>("quidra_autograd_target_drop");
inline constexpr LlvmCallee target_has_grad =
    llvm_callee<decltype(&quidra_autograd_target_has_grad)>("quidra_autograd_target_has_grad");
inline constexpr LlvmCallee target_clear_grad =
    llvm_callee<decltype(&quidra_autograd_target_clear_grad)>("quidra_autograd_target_clear_grad");
inline constexpr LlvmCallee target_gradient =
    llvm_callee<decltype(&quidra_autograd_target_gradient)>("quidra_autograd_target_gradient");
} // namespace autograd

// Console input and output.
namespace console {
inline constexpr LlvmCallee flush = llvm_callee<decltype(&quidra_flush)>("quidra_flush");
inline constexpr LlvmCallee output_status =
    llvm_callee<decltype(&quidra_output_status)>("quidra_output_status");
inline constexpr LlvmCallee input_read =
    llvm_callee<decltype(&quidra_input_read)>("quidra_input_read");
} // namespace console

// Files and file handles.
namespace file {
inline constexpr LlvmCallee open_raw =
    llvm_callee<decltype(&quidra_file_open_raw)>("quidra_file_open_raw");
inline constexpr LlvmCallee create_raw =
    llvm_callee<decltype(&quidra_file_create_raw)>("quidra_file_create_raw");
inline constexpr LlvmCallee append_raw =
    llvm_callee<decltype(&quidra_file_append_raw)>("quidra_file_append_raw");
inline constexpr LlvmCallee handle_read_raw =
    llvm_callee<decltype(&quidra_file_handle_read_raw)>("quidra_file_handle_read_raw");
inline constexpr LlvmCallee handle_read_line_raw =
    llvm_callee<decltype(&quidra_file_handle_read_line_raw)>("quidra_file_handle_read_line_raw");
inline constexpr LlvmCallee handle_read_bin_raw =
    llvm_callee<decltype(&quidra_file_handle_read_bin_raw)>("quidra_file_handle_read_bin_raw");
inline constexpr LlvmCallee handle_write_raw =
    llvm_callee<decltype(&quidra_file_handle_write_raw)>("quidra_file_handle_write_raw");
inline constexpr LlvmCallee handle_flush_raw =
    llvm_callee<decltype(&quidra_file_handle_flush_raw)>("quidra_file_handle_flush_raw");
inline constexpr LlvmCallee handle_seek_raw =
    llvm_callee<decltype(&quidra_file_handle_seek_raw)>("quidra_file_handle_seek_raw");
inline constexpr LlvmCallee handle_close =
    llvm_callee<decltype(&quidra_file_handle_close)>("quidra_file_handle_close");
inline constexpr LlvmCallee handle_clone =
    llvm_callee<decltype(&quidra_file_handle_clone)>("quidra_file_handle_clone");
inline constexpr LlvmCallee handle_drop =
    llvm_callee<decltype(&quidra_file_handle_drop)>("quidra_file_handle_drop");
inline constexpr LlvmCallee read_raw =
    llvm_callee<decltype(&quidra_file_read_raw)>("quidra_file_read_raw");
inline constexpr LlvmCallee read_bin_raw =
    llvm_callee<decltype(&quidra_file_read_bin_raw)>("quidra_file_read_bin_raw");
inline constexpr LlvmCallee write_raw =
    llvm_callee<decltype(&quidra_file_write_raw)>("quidra_file_write_raw");
inline constexpr LlvmCallee write_bin_raw =
    llvm_callee<decltype(&quidra_file_write_bin_raw)>("quidra_file_write_bin_raw");
inline constexpr LlvmCallee exists_raw =
    llvm_callee<decltype(&quidra_file_exists_raw)>("quidra_file_exists_raw");
inline constexpr LlvmCallee is_directory_raw =
    llvm_callee<decltype(&quidra_file_is_directory_raw)>("quidra_file_is_directory_raw");
inline constexpr LlvmCallee remove_raw =
    llvm_callee<decltype(&quidra_file_remove_raw)>("quidra_file_remove_raw");
inline constexpr LlvmCallee copy_raw =
    llvm_callee<decltype(&quidra_file_copy_raw)>("quidra_file_copy_raw");
inline constexpr LlvmCallee move_raw =
    llvm_callee<decltype(&quidra_file_move_raw)>("quidra_file_move_raw");
inline constexpr LlvmCallee mkdir_raw =
    llvm_callee<decltype(&quidra_file_mkdir_raw)>("quidra_file_mkdir_raw");
inline constexpr LlvmCallee list_raw =
    llvm_callee<decltype(&quidra_file_list_raw)>("quidra_file_list_raw");
} // namespace file

// The command line interface, the environment and the clock.
namespace system {
inline constexpr LlvmCallee cli_argument =
    llvm_callee<decltype(&quidra_cli_argument)>("quidra_cli_argument");
inline constexpr LlvmCallee cli_argument_optional =
    llvm_callee<decltype(&quidra_cli_argument_optional)>("quidra_cli_argument_optional");
inline constexpr LlvmCallee cli_option =
    llvm_callee<decltype(&quidra_cli_option)>("quidra_cli_option");
inline constexpr LlvmCallee cli_flag = llvm_callee<decltype(&quidra_cli_flag)>("quidra_cli_flag");
inline constexpr LlvmCallee cli_finish =
    llvm_callee<decltype(&quidra_cli_finish)>("quidra_cli_finish");
inline constexpr LlvmCallee cli_parse_int =
    llvm_callee<decltype(&quidra_cli_parse_int)>("quidra_cli_parse_int");
inline constexpr LlvmCallee cli_parse_float =
    llvm_callee<decltype(&quidra_cli_parse_float)>("quidra_cli_parse_float");
inline constexpr LlvmCallee cli_parse_bigint =
    llvm_callee<decltype(&quidra_cli_parse_bigint)>("quidra_cli_parse_bigint");
inline constexpr LlvmCallee cli_parse_bigreal =
    llvm_callee<decltype(&quidra_cli_parse_bigreal)>("quidra_cli_parse_bigreal");
inline constexpr LlvmCallee cli_parse_numeric=llvm_callee<decltype(&quidra_cli_parse_numeric)>("quidra_cli_parse_numeric");
inline constexpr LlvmCallee cli_parse_nat=llvm_callee<decltype(&quidra_cli_parse_nat)>("quidra_cli_parse_nat");
inline constexpr LlvmCallee cli_parse_bool =
    llvm_callee<decltype(&quidra_cli_parse_bool)>("quidra_cli_parse_bool");
inline constexpr LlvmCallee environment_get =
    llvm_callee<decltype(&quidra_environment_get)>("quidra_environment_get");
inline constexpr LlvmCallee environment_has =
    llvm_callee<decltype(&quidra_environment_has)>("quidra_environment_has");
inline constexpr LlvmCallee time_now = llvm_callee<decltype(&quidra_time_now)>("quidra_time_now");
inline constexpr LlvmCallee time_sleep =
    llvm_callee<decltype(&quidra_time_sleep)>("quidra_time_sleep");
} // namespace system

// Child processes.
namespace process {
inline constexpr LlvmCallee run = llvm_callee<decltype(&quidra_process_run)>("quidra_process_run");
inline constexpr LlvmCallee shell =
    llvm_callee<decltype(&quidra_process_shell)>("quidra_process_shell");
} // namespace process

// Parallel blocks.
namespace concurrency {
inline constexpr LlvmCallee call_begin = llvm_callee<decltype(&quidra_parallel_call_begin)>("quidra_parallel_call_begin");
inline constexpr LlvmCallee call_accept = llvm_callee<decltype(&quidra_parallel_call_accept)>("quidra_parallel_call_accept");
inline constexpr LlvmCallee call_end = llvm_callee<decltype(&quidra_parallel_call_end)>("quidra_parallel_call_end");
inline constexpr LlvmCallee call_require_accept = llvm_callee<decltype(&quidra_parallel_call_require_accept)>("quidra_parallel_call_require_accept");
inline constexpr LlvmCallee parallel_iterations = llvm_callee<decltype(&quidra_parallel_iterations)>("quidra_parallel_iterations");
inline constexpr LlvmCallee array_prepare = llvm_callee<decltype(&quidra_parallel_array_prepare)>("quidra_parallel_array_prepare");
inline constexpr LlvmCallee array_inline = llvm_callee<decltype(&quidra_parallel_array_inline)>("quidra_parallel_array_inline");
inline constexpr LlvmCallee array_mark = llvm_callee<decltype(&quidra_parallel_array_mark)>("quidra_parallel_array_mark");
inline constexpr LlvmCallee parallel_block = llvm_callee<decltype(&quidra_parallel_block)>("quidra_parallel_block");
} // namespace concurrency

// Random number generators.
namespace random {
inline constexpr LlvmCallee integer_value=llvm_callee<decltype(&quidra_random_integer)>("quidra_random_integer");
inline constexpr LlvmCallee int_value =
    llvm_callee<decltype(&quidra_random_int)>("quidra_random_int");
inline constexpr LlvmCallee float_value =
    llvm_callee<decltype(&quidra_random_float)>("quidra_random_float");
inline constexpr LlvmCallee bool_value =
    llvm_callee<decltype(&quidra_random_bool)>("quidra_random_bool");
} // namespace random

// JSON values.
namespace json {
inline constexpr LlvmCallee parse_raw =
    llvm_callee<decltype(&quidra_json_parse_raw)>("quidra_json_parse_raw");
inline constexpr LlvmCallee drop = llvm_callee<decltype(&quidra_json_drop)>("quidra_json_drop");
inline constexpr LlvmCallee kind = llvm_callee<decltype(&quidra_json_kind)>("quidra_json_kind");
inline constexpr LlvmCallee size = llvm_callee<decltype(&quidra_json_size)>("quidra_json_size");
inline constexpr LlvmCallee is_object =
    llvm_callee<decltype(&quidra_json_is_object)>("quidra_json_is_object");
inline constexpr LlvmCallee is_array =
    llvm_callee<decltype(&quidra_json_is_array)>("quidra_json_is_array");
inline constexpr LlvmCallee get = llvm_callee<decltype(&quidra_json_get)>("quidra_json_get");
inline constexpr LlvmCallee at = llvm_callee<decltype(&quidra_json_at)>("quidra_json_at");
inline constexpr LlvmCallee text = llvm_callee<decltype(&quidra_json_text)>("quidra_json_text");
inline constexpr LlvmCallee integer_ok =
    llvm_callee<decltype(&quidra_json_integer_ok)>("quidra_json_integer_ok");
inline constexpr LlvmCallee integer =
    llvm_callee<decltype(&quidra_json_integer)>("quidra_json_integer");
inline constexpr LlvmCallee number_ok =
    llvm_callee<decltype(&quidra_json_number_ok)>("quidra_json_number_ok");
inline constexpr LlvmCallee number =
    llvm_callee<decltype(&quidra_json_number)>("quidra_json_number");
inline constexpr LlvmCallee number_text =
    llvm_callee<decltype(&quidra_json_number_text)>("quidra_json_number_text");
inline constexpr LlvmCallee boolean_ok =
    llvm_callee<decltype(&quidra_json_boolean_ok)>("quidra_json_boolean_ok");
inline constexpr LlvmCallee boolean =
    llvm_callee<decltype(&quidra_json_boolean)>("quidra_json_boolean");
inline constexpr LlvmCallee encode =
    llvm_callee<decltype(&quidra_json_encode)>("quidra_json_encode");
inline constexpr LlvmCallee equal = llvm_callee<decltype(&quidra_json_equal)>("quidra_json_equal");
} // namespace json

// HTTP requests and responses.
namespace http {
inline constexpr LlvmCallee get = llvm_callee<decltype(&quidra_http_get)>("quidra_http_get");
inline constexpr LlvmCallee header =
    llvm_callee<decltype(&quidra_http_header)>("quidra_http_header");
inline constexpr LlvmCallee response_clone =
    llvm_callee<decltype(&quidra_http_response_clone)>("quidra_http_response_clone");
inline constexpr LlvmCallee response_drop =
    llvm_callee<decltype(&quidra_http_response_drop)>("quidra_http_response_drop");
} // namespace http
// Slices: the plan every slice of an array goes through, and the
// length check of a slice assignment.
namespace slice {
inline constexpr LlvmCallee plan = llvm_callee<decltype(&quidra_slice_plan)>("quidra_slice_plan");
inline constexpr LlvmCallee assignment_check =
    llvm_callee<decltype(&quidra_slice_assignment_check)>("quidra_slice_assignment_check");
} // namespace slice

// The helpers the runtime prelude defines in LLVM IR.
namespace prelude {
inline constexpr LlvmCallee fail = llvm_callee<void (*)(const char* message)>("quidra_fail");
inline constexpr LlvmCallee fail_at =
    llvm_callee<void (*)(const char* code, const char* message, long long line, long long column)>(
        "quidra_fail_at");
inline constexpr LlvmCallee fail_code =
    llvm_callee<void (*)(const char* code, const char* message)>("quidra_fail_code");
inline constexpr LlvmCallee stack_enter = llvm_callee<void (*)()>("quidra_stack_enter");
inline constexpr LlvmCallee stack_leave = llvm_callee<void (*)()>("quidra_stack_leave");
inline constexpr LlvmCallee alloc = llvm_callee<void* (*)(long long bytes)>("quidra_alloc");
inline constexpr LlvmCallee float_text = llvm_callee<char* (*)(double value)>("quidra_float_text");
inline constexpr LlvmCallee array_alloc =
    llvm_callee<void* (*)(long long count, long long stride, int initialized)>(
        "quidra_array_alloc");
inline constexpr LlvmCallee array_slot =
    llvm_callee<void* (*)(void* array, long long index, long long stride, long long line,
                          long long column)>("quidra_array_slot");
inline constexpr LlvmCallee fixed_array_slot =
    llvm_callee<void* (*)(void* array, long long index, long long length, long long stride,
                          long long line, long long column)>("quidra_fixed_array_slot");
inline constexpr LlvmCallee array_slot_proven =
    llvm_callee<void* (*)(void* array, long long index, long long stride)>(
        "quidra_array_slot_proven");
inline constexpr LlvmCallee fixed_array_slot_proven =
    llvm_callee<void* (*)(void* array, long long index, long long stride)>(
        "quidra_fixed_array_slot_proven");
inline constexpr LlvmCallee add = llvm_callee<long long (*)(long long, long long)>("quidra_add");
inline constexpr LlvmCallee abs = llvm_callee<long long (*)(long long)>("quidra_abs");
inline constexpr LlvmCallee sub = llvm_callee<long long (*)(long long, long long)>("quidra_sub");
inline constexpr LlvmCallee mul = llvm_callee<long long (*)(long long, long long)>("quidra_mul");
inline constexpr LlvmCallee div = llvm_callee<long long (*)(long long, long long)>("quidra_div");
inline constexpr LlvmCallee mod = llvm_callee<long long (*)(long long, long long)>("quidra_mod");
inline constexpr LlvmCallee string_parse_two_signed =
    llvm_callee<bool (*)(const char* text, char separator, long long* left, long long* right)>(
        "__quidra_string_parse_two_signed_fast");
} // namespace prelude

// The C library functions generated code calls by name.
namespace c_library {
inline constexpr LlvmCallee printf = llvm_callee<int (*)(const char*, ...)>("printf");
inline constexpr LlvmCallee puts = llvm_callee<int (*)(const char*)>("puts");
inline constexpr LlvmCallee strlen = llvm_callee<long long (*)(const char*)>("strlen");
inline constexpr LlvmCallee memcpy =
    llvm_callee<void* (*)(void*, const void*, long long)>("memcpy");
inline constexpr LlvmCallee memset = llvm_callee<void* (*)(void*, int, long long)>("memset");
inline constexpr LlvmCallee exit = llvm_callee<void (*)(int)>("exit");
} // namespace c_library

// A standard class whose values are runtime handles
// (standard_class::is_runtime_handle): the runtime function that drops a
// value, and the one that clones it. json.Value has no clone function:
// generated code shares a JSON value instead of copying it.
struct RuntimeHandleClass {
    std::string_view id;
    const LlvmCallee* drop;
    const LlvmCallee* clone;
};

inline constexpr std::array<RuntimeHandleClass, 4> runtime_handle_classes{{
    {standard_class::json_value, &json::drop, nullptr},
    {standard_class::http_response, &http::response_drop, &http::response_clone},
    {standard_class::file_handle, &file::handle_drop, &file::handle_clone},
    {standard_class::autograd_target, &autograd::target_drop, &autograd::target_clone},
}};

// The runtime handle class with the id `class_id`, or null.
inline const RuntimeHandleClass* runtime_handle_class(std::string_view class_id) {
    for (const auto& handle : runtime_handle_classes) {
        if (class_id == handle.id) return &handle;
    }
    return nullptr;
}

// "@symbol": a callee as a function pointer operand (a drop callback).
inline std::string global_name(const LlvmCallee& callee) {
    std::string name;
    name.reserve(1 + callee.symbol().size());
    name += '@';
    name += callee.symbol();
    return name;
}

// The LLVM intrinsics generated code calls by a name composed from its
// operands' types.
namespace llvm_intrinsic {
// @llvm.{s,u}{add,sub,mul}.with.overflow.i<width>: the result and whether it
// overflowed.
inline std::string with_overflow(bool is_signed, std::string_view operation, int width) {
    return std::string("@llvm.") + (is_signed ? "s" : "u") + std::string(operation) +
           ".with.overflow.i" + std::to_string(width);
}
// @llvm.fpto{s,u}i.sat.i<width>.f{32,64}: a float converted to an integer of
// `width` bits, saturated at its range.
inline std::string saturating_float_to_integer(bool to_signed, int width, bool from_float32) {
    return std::string(to_signed ? "@llvm.fptosi.sat.i" : "@llvm.fptoui.sat.i") +
           std::to_string(width) + (from_float32 ? ".f32" : ".f64");
}
} // namespace llvm_intrinsic

// The start of the names of the helpers the backend defines per type: clone,
// drop, equality and slice helpers (type_helpers.cpp), and the array casts
// and their validation (array_cast_helpers.cpp). A helper's name continues with the
// type's identifier.
namespace generated_helper_prefix {
inline constexpr std::string_view clone = "quidra_clone_";
inline constexpr std::string_view drop = "quidra_drop_";
inline constexpr std::string_view equal = "quidra_equal_";
inline constexpr std::string_view array_cast = "quidra_array_cast_";
inline constexpr std::string_view array_cast_validate = "quidra_array_cast_validate_";
inline constexpr std::string_view slice_get = "quidra_slice_get_";
inline constexpr std::string_view slice_set = "quidra_slice_set_";
} // namespace generated_helper_prefix

} // namespace quidra::llvm_backend::runtime_abi
