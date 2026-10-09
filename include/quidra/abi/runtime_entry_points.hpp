#pragma once

// The C entry points of the Quidra runtime: every function with C linkage
// that the runtime sources define for generated code or for each other, one
// prototype each, grouped by the source that defines it. Every runtime source
// includes this header, so the compiler checks each definition against its
// prototype. The C ABI that native packages use is quidra/native_extension.h;
// its qcore_* functions are not repeated here.
//
// Not installed: generated code reaches these functions through the LLVM
// declarations of the backend's runtime prelude.

#include "quidra/abi/layout.hpp"
#include "quidra/native_extension.h"

#include <cstdint>

// runtime.cpp: values, text, arrays, tensors, autograd, tasks, the command line and failures.
extern "C" void qcore_execution_fast();
extern "C" void qcore_execution_deterministic();
extern "C" char* quidra_format_float(double value);
extern "C" char* quidra_runtime_copy_text(const char* data, unsigned long long raw_size);
extern "C" char* quidra_format_signed(
    long long value, int integer_width, int fractional, int significant, int zero);
extern "C" char* quidra_format_unsigned(
    unsigned long long value, int integer_width, int fractional, int significant, int zero);
extern "C" char* quidra_format_number(
    double value, int integer_width, int fractional, int significant, int zero);
extern "C" char* quidra_int_format(
    long long value, int integer_width, int fractional, int significant, int zero);
extern "C" void* quidra_managed_alloc(unsigned long long bytes);
extern "C" bool quidra_runtime_text_valid_bytes(const char* data, unsigned long long size);
extern "C" void quidra_runtime_text_error(const char* message);
extern "C" char* quidra_runtime_copy_text_bytes(const char* data, unsigned long long size);
extern "C" char* quidra_runtime_copy_validated_text_bytes(
    const char* data, unsigned long long size);
extern "C" char* quidra_runtime_try_copy_text_bytes(const char* data, unsigned long long size);
extern "C" char* quidra_runtime_allocate_text_buffer(unsigned long long size);
extern "C" bool quidra_runtime_commit_text_buffer(char* data, unsigned long long size);
extern "C" unsigned long long quidra_runtime_text_byte_length(const char* text);
extern "C" void quidra_managed_retain(void* value);
extern "C" void quidra_managed_release(void* value, void* drop_function);
extern "C" void quidra_managed_pin(void* address);
extern "C" void quidra_managed_unpin(void* address);
extern "C" void quidra_init_create(
    void* base, unsigned long long count, unsigned long long unit_bytes,
    unsigned long long data_offset, int fully_initialized);
extern "C" bool quidra_array_initialization_complete(void* array);
extern "C" void quidra_init_mark_range(void* address, unsigned long long bytes);
extern "C" void quidra_init_check(
    void* address, unsigned long long line, unsigned long long column);
extern "C" void quidra_init_require_range(
    void* address, unsigned long long bytes, unsigned long long line, unsigned long long column);
extern "C" void quidra_init_clone(
    void* destination, void* source_data, unsigned long long count, unsigned long long unit_bytes,
    unsigned long long destination_offset);
extern "C" void* quidra_atomic_counter_create(long long initial);
extern "C" void* quidra_atomic_counter_clone(void* value);
extern "C" void quidra_atomic_counter_drop(void* value);
extern "C" long long quidra_atomic_counter_load(void* value);
extern "C" long long quidra_atomic_counter_add(
    void* value, long long delta, unsigned long long line, unsigned long long column);
extern "C" void quidra_task_all_atomic_counter(
    void* raw, void* shared, unsigned long long line, unsigned long long column);
extern "C" void quidra_task_all(void* raw, unsigned long long line, unsigned long long column);
extern "C" void quidra_task_all_i64(
    void* raw, void* output, unsigned long long line, unsigned long long column);
extern "C" void quidra_task_all_f64(
    void* raw, void* output, unsigned long long line, unsigned long long column);
extern "C" void quidra_task_all_int(
    void* raw, void* output, unsigned long long line, unsigned long long column);
extern "C" bool quidra_array_can_append_move(void* array);
extern "C" void* quidra_array_grow_move(void* array, unsigned long long raw_stride);
extern "C" void* quidra_array_sorted(
    void* raw, int kind, unsigned long long raw_stride, unsigned long long line,
    unsigned long long column);
extern "C" bool quidra_numeric_cast_element_fits(
    const void* source, int source_dtype, int target_dtype, unsigned long long line,
    unsigned long long column);
extern "C" void quidra_numeric_cast_element(
    void* destination, const void* source, int source_dtype, int target_dtype,
    unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_create(
    void* shape_array, int dtype, int fill_mode, bool has_gpu, long long gpu,
    unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_to_gpu(
    void* raw, long long gpu, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_to_cpu(
    void* raw, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_clone(void* raw);
extern "C" void quidra_tensor_drop(void* raw);
extern "C" bool quidra_tensor_is_contiguous(void* raw);
extern "C" void* quidra_tensor_shape(void* raw);
extern "C" void* quidra_tensor_shape_fixed(void* raw, unsigned long long expected_rank);
extern "C" void* quidra_tensor_reshape(
    void* raw, void* shape_array, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_transpose(
    void* raw, long long axis0, long long axis1, unsigned long long line,
    unsigned long long column);
extern "C" void* quidra_tensor_contiguous(
    void* raw, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_item_ptr(
    void* raw, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_cast(
    void* raw, int target_dtype, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_try_cast(
    void* raw, int target_dtype, unsigned long long line, unsigned long long column);
extern "C" void quidra_tensor_rank_check(
    void* raw, long long expected_rank, unsigned long long line, unsigned long long column);
extern "C" void quidra_tensor_extent_check(
    void* raw, long long axis, long long expected, unsigned long long line,
    unsigned long long column);
extern "C" void* quidra_autograd_target_create();
extern "C" void* quidra_autograd_target_clone(void* value);
extern "C" void quidra_autograd_target_drop(void* value);
extern "C" bool quidra_autograd_target_has_grad(void* value);
extern "C" void quidra_autograd_target_clear_grad(
    void* value, unsigned long long line, unsigned long long column);
extern "C" void* quidra_autograd_target_gradient(
    void* value, int dtype, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_track(void* raw,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_track_target(
    void* raw, void* target_raw, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_untrack(void* raw,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_retrack(void* raw,unsigned long long line,unsigned long long column);
extern "C" void* quidra_tensor_grad(void* raw,unsigned long long line,unsigned long long column);
extern "C" bool quidra_tensor_is_tracked(void* raw);
extern "C" bool quidra_tensor_has_grad(void* raw);
extern "C" void quidra_tensor_clear_grad(
    void* raw, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_gather(
    void* raw, void* indices_array, void* shape_array, unsigned long long line,
    unsigned long long column);
extern "C" void* quidra_tensor_scatter(
    void* raw, void* indices_array, void* shape_array, unsigned long long line,
    unsigned long long column);
extern "C" void quidra_tensor_backward_many(
    void* raw, void** target_raws, const unsigned char* target_kinds,
    unsigned long long target_count, bool track, unsigned long long line,
    unsigned long long column);
extern "C" void quidra_tensor_backward_many_with_autograd_targets(
    void* raw, void** target_raws, const unsigned char* target_kinds,
    unsigned long long target_count, void* autograd_targets, bool track, unsigned long long line,
    unsigned long long column);
extern "C" long long quidra_integer_pow_signed(
    long long base, long long exponent, int bits, unsigned long long line,
    unsigned long long column);
extern "C" unsigned long long quidra_integer_pow_unsigned(
    unsigned long long base, unsigned long long exponent, int bits, unsigned long long line,
    unsigned long long column);
extern "C" void* quidra_tensor_unary(
    void* raw, int operation, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_compare(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column);
extern "C" bool quidra_tensor_bool_reduce(
    void* raw, bool all, unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_binary(
    void* primary_raw, void* other_raw, void* scalar, int scalar_side, int operation,
    unsigned long long line, unsigned long long column);
extern "C" void* quidra_tensor_index(
    void* raw, const long long* specs, unsigned long long count, unsigned long long line,
    unsigned long long column);
extern "C" void quidra_tensor_set(
    void* raw, const long long* indices, unsigned long long count, const void* value,
    unsigned long long line, unsigned long long column);
extern "C" char* quidra_string_index(
    const char* text, long long index, unsigned long long line, unsigned long long column);
extern "C" bool quidra_string_index_equal_ascii(
    const char* text, long long index, unsigned char expected, unsigned long long line,
    unsigned long long column);
extern "C" long long quidra_string_count_ascii_prefix(
    const char* text, long long count, unsigned char expected, bool negate, long long initial,
    unsigned long long index_line, unsigned long long index_column,
    unsigned long long overflow_line, unsigned long long overflow_column);
extern "C" long long quidra_string_length(const char* text);
extern "C" bool quidra_string_contains(const char* text, const char* needle);
extern "C" bool quidra_string_starts_with(const char* text, const char* prefix);
extern "C" bool quidra_string_ends_with(const char* text, const char* suffix);
extern "C" long long quidra_string_find(const char* text, const char* needle);
extern "C" char* quidra_string_slice(const char* text, long long start, long long end,
                                     unsigned long long line, unsigned long long column);
extern "C" char* quidra_string_trim(const char* text);
extern "C" void* quidra_string_split_iter_begin(const char* text, const char* separator);
extern "C" void* quidra_string_split_iter_begin_move(const char* text, const char* separator);
extern "C" char* quidra_string_split_iter_next(void* raw);
extern "C" void quidra_string_split_iter_end(void* raw);
extern "C" void* quidra_string_split(const char* text, const char* separator);
extern "C" bool quidra_string_can_append_move(void* raw);
extern "C" char* quidra_string_build_append_move_unique_direct(
    char* raw, const unsigned char* kinds, const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator, long long* added_length_out);
extern "C" char* quidra_string_build_append_move_unique(
    char* raw, const unsigned char* kinds, const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator);
extern "C" char* quidra_string_build_append_move(
    char* raw, const unsigned char* kinds, const unsigned long long* raw_values,
    unsigned long long raw_count, const char* separator);
extern "C" long long quidra_string_build_append_last_length();
extern "C" char* quidra_string_append_move_many(
    char* raw, const char* const* suffixes, unsigned long long raw_count);
extern "C" char* quidra_string_build(
    const unsigned char* kinds, const unsigned long long* raw_values, unsigned long long raw_count,
    const char* separator);
extern "C" char* quidra_string_concat_many(const char* const* values, unsigned long long raw_count);
extern "C" char* quidra_string_concat2(const char* left, const char* right);
extern "C" bool quidra_string_equal(const char* left, const char* right);
extern "C" void* quidra_bin_alloc(long long bit_count, long long fill);
extern "C" void* quidra_bin_index(void* raw, long long index, unsigned long long line,
                                  unsigned long long column);
extern "C" void quidra_bin_set(void* raw, long long index, void* bit, unsigned long long line,
                               unsigned long long column);
extern "C" void* quidra_bin_slice(void* raw, long long start, long long end,
                                  unsigned long long line, unsigned long long column);
extern "C" void* quidra_bin_parse(const char* text);
extern "C" char* quidra_bin_string(void* raw);
extern "C" void* quidra_bin_from_u64(unsigned long long value, int width);
extern "C" unsigned long long quidra_bin_to_u64(void* raw, int width);
extern "C" void* quidra_bin_clone(void* raw);
extern "C" bool quidra_bin_equal(void* left, void* right);
extern "C" long long quidra_bin_byte_length(void* raw);
extern "C" char* quidra_u8_array_try_utf8(void* raw);
extern "C" char* quidra_bin_try_utf8(void* raw);
extern "C" void* quidra_bin_from_array(void* raw, int width, int stride);
extern "C" void* quidra_bin_to_array(void* raw, int width, int stride);
extern "C" char* quidra_string_repeat(long long count, const char* fill);
extern "C" void* quidra_string_utf8(const char* text);
extern "C" void* quidra_string_codepoints(const char* text);
extern "C" char* quidra_string_join(
    void* raw, const char* separator, unsigned long long line, unsigned long long column);
extern "C" void quidra_runtime_set_args(int argc, char** argv);
extern "C" int quidra_runtime_argc();
extern "C" const char* quidra_runtime_argv(int index);
extern "C" const char* quidra_cli_argument(long long index);
extern "C" const char* quidra_cli_argument_optional(long long index);
extern "C" const char* quidra_cli_option(const char* name);
extern "C" bool quidra_cli_flag(const char* name);
extern "C" void quidra_cli_finish();
extern "C" int quidra_input_read(char** out);
extern "C" bool quidra_string_parse_two_signed(
    const char* text, unsigned char separator, long long* out_left, long long* out_right);
extern "C" bool quidra_parse_signed(const char* text, long long* out);
extern "C" bool quidra_parse_unsigned(const char* text, unsigned long long* out);
extern "C" char* quidra_integer_text_signed(long long value);
extern "C" char* quidra_integer_text_unsigned(unsigned long long value);
extern "C" bool quidra_parse_float32(const char* text, float* out);
extern "C" bool quidra_parse_float64(const char* text, double* out);
extern "C" long long quidra_cli_parse_int(const char* text);
extern "C" double quidra_cli_parse_float(const char* text);
extern "C" void* quidra_cli_parse_bigint(const char* text);
extern "C" void* quidra_cli_parse_bigreal(const char* text);
extern "C" bool quidra_cli_parse_bool(const char* text);

// runtime_exact.cpp: big integers, big reals and exact-real providers.
extern "C" void* qcore_exact_real_atom(const char* provider,std::uint32_t opcode);
extern "C" double qcore_exact_real_atom_float64(const char* provider,std::uint32_t opcode);
extern "C" void quidra_bigint_drop(void*p);
extern "C" void quidra_bigreal_drop(void*p);
extern "C" void* quidra_bigint_literal(const char*text);
extern "C" void* quidra_bigint_parse(const char*text);
extern "C" void* quidra_bigreal_literal(const char*text);
extern "C" void* quidra_bigreal_parse(const char*text);
extern "C" char* quidra_bigint_text(void*p);
extern "C" char* quidra_bigreal_text(void*p,int significant);
extern "C" void* quidra_bigint_neg(void*p,unsigned long long line,unsigned long long column);
extern "C" void* quidra_bigint_binary(
    void*left, void*right, int operation, unsigned long long line, unsigned long long column);
extern "C" void* quidra_bigint_pow(
    void*left, void*right, unsigned long long line, unsigned long long column);
extern "C" int quidra_bigint_compare(void*left,void*right);
extern "C" void* quidra_bigreal_neg(void*p,unsigned long long line,unsigned long long column);
extern "C" void* quidra_bigreal_binary(
    void*left, void*right, int operation, unsigned long long line, unsigned long long column);
extern "C" int quidra_bigreal_compare(
    void*left, void*right, unsigned long long line, unsigned long long column);
extern "C" void* quidra_bigreal_pow(
    void*left, void*right, unsigned long long line, unsigned long long column);
extern "C" void* quidra_bigint_from_i64(long long value);
extern "C" void* quidra_bigint_from_u64(unsigned long long value);
extern "C" void* quidra_bigreal_from_i64(long long value);
extern "C" void* quidra_bigreal_from_u64(unsigned long long value);
extern "C" void* quidra_bigreal_from_float64(double value);
extern "C" void* quidra_bigreal_from_bigint(void*p);
extern "C" bool quidra_bigint_try_i64(void*p,int bits,long long*out);
extern "C" bool quidra_bigint_try_u64(void*p,int bits,unsigned long long*out);
extern "C" void* quidra_bigreal_try_bigint(void*p);
extern "C" bool quidra_bigreal_try_i64(void*p,int bits,long long*out);
extern "C" bool quidra_bigreal_try_u64(void*p,int bits,unsigned long long*out);
extern "C" void* quidra_bigreal_to_bigint(void*p,unsigned long long line,unsigned long long column);
extern "C" long long quidra_bigint_to_i64(
    void*p, int bits, unsigned long long line, unsigned long long column);
extern "C" unsigned long long quidra_bigint_to_u64(
    void*p, int bits, unsigned long long line, unsigned long long column);
extern "C" bool quidra_bigreal_try_float64(void*p,double*out);
extern "C" bool quidra_bigreal_try_float32(void*p,float*out);
extern "C" bool quidra_bigint_try_float64(void*p,double*out);
extern "C" bool quidra_bigint_try_float32(void*p,float*out);
extern "C" bool quidra_exact_numeric_cast_fits(void* p,int source_kind,int target_kind,int bits);
extern "C" double quidra_bigreal_to_float64(
    void*p, unsigned long long line, unsigned long long column);
extern "C" float quidra_bigreal_to_float32(
    void*p, unsigned long long line, unsigned long long column);
extern "C" double quidra_bigint_to_float64(
    void*p, unsigned long long line, unsigned long long column);
extern "C" float quidra_bigint_to_float32(void*p,unsigned long long line,unsigned long long column);
extern "C" void* quidra_real_promote(long long numerator,long long denominator);
extern "C" void quidra_real_demote(void* handle,quidra::abi::ExactRealValue* out);
extern "C" bool quidra_real_small_binary(
    long long left_numerator, long long left_denominator,
    long long right_numerator, long long right_denominator,
    int operation, quidra::abi::ExactRealValue* out);
extern "C" char* quidra_real_small_text(long long numerator,long long denominator,int significant);
extern "C" bool quidra_real_small_try_float64(long long numerator,long long denominator,double* out);
extern "C" bool quidra_real_small_try_float32(long long numerator,long long denominator,float* out);
extern "C" double quidra_real_small_to_float64(
    long long numerator, long long denominator, unsigned long long line, unsigned long long column);
extern "C" float quidra_real_small_to_float32(
    long long numerator, long long denominator, unsigned long long line, unsigned long long column);
extern "C" bool quidra_real_small_try_i64(
    long long numerator, long long denominator, int bits, long long* out);
extern "C" bool quidra_real_small_try_u64(
    long long numerator, long long denominator, int bits, unsigned long long* out);
extern "C" void* quidra_real_small_try_bigint(long long numerator,long long denominator);
extern "C" void* quidra_real_small_to_bigint(
    long long numerator, long long denominator, unsigned long long line, unsigned long long column);
extern "C" bool quidra_real_small_cast_fits(
    long long numerator, long long denominator, int target_kind, int bits);

// runtime_integer.cpp: arbitrary-precision integers as words
// (abi::bare_integer_layout). Each takes and returns canonical words; the
// generated code takes the inline fast paths itself and calls these for
// the rest.
extern "C" long long quidra_int_add(long long left, long long right);
extern "C" long long quidra_int_sub(long long left, long long right);
extern "C" long long quidra_int_mul(long long left, long long right);
extern "C" long long quidra_int_div(
    long long left, long long right, unsigned long long line, unsigned long long column);
extern "C" long long quidra_int_rem(
    long long left, long long right, unsigned long long line, unsigned long long column);
extern "C" long long quidra_int_neg(
    long long value, unsigned long long line, unsigned long long column);
extern "C" long long quidra_int_pow(
    long long left, long long right, unsigned long long line, unsigned long long column);
extern "C" int quidra_int_compare(long long left, long long right);
extern "C" char* quidra_int_text(long long value);
extern "C" bool quidra_int_parse(const char* text, long long* out);
extern "C" long long quidra_int_literal(const char* text);
extern "C" long long quidra_int_from_i64(long long value);
extern "C" long long quidra_int_from_u64(unsigned long long value);
extern "C" long long quidra_int_to_i64_checked(
    long long value, int bits, unsigned long long line, unsigned long long column);
extern "C" unsigned long long quidra_int_to_u64_checked(
    long long value, int bits, unsigned long long line, unsigned long long column);
extern "C" bool quidra_int_try_i64(long long value, int bits, long long* out);
extern "C" bool quidra_int_try_u64(long long value, int bits, unsigned long long* out);
extern "C" double quidra_int_to_float64(
    long long value, unsigned long long line, unsigned long long column);
extern "C" float quidra_int_to_float32(
    long long value, unsigned long long line, unsigned long long column);
extern "C" bool quidra_int_try_float64(long long value, double* out);
extern "C" bool quidra_int_try_float32(long long value, float* out);
extern "C" bool quidra_int_cast_fits(long long value, int target_kind, int bits);
extern "C" long long quidra_int_adopt(void* big_integer);
extern "C" long long quidra_nat_sub(
    long long left, long long right, unsigned long long line, unsigned long long column);
// Between the units of the runtime: a word leaving the thread that owns its
// box (a task result) and arriving at the joining thread.
extern "C" long long quidra_int_detach(long long value);
extern "C" long long quidra_int_attach(long long value);

// runtime_io.cpp: console input and output, files and file handles.
extern "C" void* quidra_file_open_raw(const char* path);
extern "C" void* quidra_file_create_raw(const char* path);
extern "C" void* quidra_file_append_raw(const char* path);
extern "C" char* quidra_file_handle_read_raw(void* value);
extern "C" int quidra_file_handle_read_line_raw(void* value, char** out);
extern "C" void* quidra_file_handle_read_bin_raw(void* value);
extern "C" bool quidra_file_handle_write_raw(void* value, const char* text, bool line);
extern "C" bool quidra_file_handle_flush_raw(void* value);
extern "C" bool quidra_file_handle_seek_raw(void* value, long long position);
extern "C" void quidra_file_handle_close(void* value);
extern "C" void* quidra_file_handle_clone(void* value);
extern "C" void quidra_file_handle_drop(void* value);
extern "C" char* quidra_file_read_raw(const char* path);
extern "C" void* quidra_file_read_bin_raw(const char* path);
extern "C" bool quidra_file_write_raw(const char* path,const char* text);
extern "C" bool quidra_file_write_bin_raw(const char* path,const void* bin_raw);
extern "C" int quidra_file_exists_raw(const char* path);
extern "C" int quidra_file_is_directory_raw(const char* path);
extern "C" bool quidra_file_remove_raw(const char* path);
extern "C" bool quidra_file_copy_raw(const char* source,const char* destination);
extern "C" bool quidra_file_move_raw(const char* source,const char* destination);
extern "C" bool quidra_file_mkdir_raw(const char* path);
extern "C" void* quidra_file_list_raw(const char* path,bool recursive);
extern "C" char* quidra_environment_get(const char* name);
extern "C" bool quidra_environment_has(const char* name);

// runtime_system.cpp: source provenance, the environment and the clock.
extern "C" void quidra_runtime_set_source_provenance(const void* provenance);
extern "C" void quidra_runtime_set_package_source_provenance(const void* provenance);
extern "C" const void* quidra_runtime_user_statement();
extern "C" void quidra_runtime_restore_user_statement(const void* statement);
extern "C" void quidra_runtime_export_leave();

// runtime_location.cpp: the program's source table.
extern "C" void quidra_runtime_register_sources(const void* table);
extern "C" void quidra_runtime_fail_at(
    const char* code, const char* message, unsigned long long line, unsigned long long column);
extern "C" void quidra_runtime_bounds_fail(
    long long index, long long length, unsigned long long line, unsigned long long column);
extern "C" void quidra_runtime_conversion_fail(
    int type, int subject, int reason, unsigned long long line, unsigned long long column);
extern "C" const char* quidra_runtime_conversion_message(int type, int subject, int reason);
extern "C" int quidra_runtime_last_conversion_reason();
extern "C" void quidra_test_assert(bool condition);
extern "C" int quidra_flush();
extern "C" int quidra_output_status();
extern "C" double quidra_time_now(bool sync);
extern "C" void quidra_gpu_sync(
    long long index, unsigned long long line, unsigned long long column);
extern "C" bool quidra_time_sleep(double seconds);
extern "C" long long quidra_random_int(void* generator,long long start,long long end);
extern "C" double quidra_random_float(void* generator);
extern "C" bool quidra_random_bool(void* generator);

// runtime_process.cpp: child processes.
extern "C" void* quidra_process_run(const char* program, void* args_array);
extern "C" void* quidra_process_shell(const char* command);

// runtime_json.cpp: JSON values.
extern "C" void* quidra_json_parse_raw(const char* text);
extern "C" void quidra_json_drop(void* value);
extern "C" char* quidra_json_last_error_copy();
extern "C" const char* quidra_json_kind(void* value);
extern "C" long long quidra_json_size(void* value);
extern "C" bool quidra_json_is_object(void* value);
extern "C" bool quidra_json_is_array(void* value);
extern "C" void* quidra_json_get(void* value, const char* key);
extern "C" void* quidra_json_at(void* value, long long index);
extern "C" char* quidra_json_text(void* value);
extern "C" bool quidra_json_integer_ok(void* value);
extern "C" long long quidra_json_integer(void* value);
extern "C" bool quidra_json_number_ok(void* value);
extern "C" double quidra_json_number(void* value);
extern "C" char* quidra_json_number_text(void* value);
extern "C" bool quidra_json_boolean_ok(void* value);
extern "C" bool quidra_json_boolean(void* value);
extern "C" char* quidra_json_encode(void* value);
extern "C" bool quidra_json_equal(void* left, void* right);

// runtime_http.cpp: HTTP requests.
extern "C" void* quidra_http_response_clone(void* response);
extern "C" void quidra_http_response_drop(void* response);
extern "C" void* quidra_http_get(const char* url);
extern "C" char* quidra_http_last_error_copy();
extern "C" char* quidra_http_header(void* response, const char* name);
