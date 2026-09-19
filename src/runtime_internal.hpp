#pragma once

extern "C" void* quidra_managed_alloc(unsigned long long bytes);
extern "C" void quidra_init_create(
    void* base, unsigned long long count, unsigned long long unit_bytes,
    unsigned long long data_offset, int initialized);
extern "C" bool quidra_runtime_text_valid_bytes(
    const char* data, unsigned long long size);
extern "C" void quidra_runtime_text_error(const char* message);
extern "C" char* quidra_runtime_copy_text_bytes(
    const char* data, unsigned long long size);
extern "C" char* quidra_runtime_copy_validated_text_bytes(
    const char* data, unsigned long long size);
extern "C" char* quidra_runtime_try_copy_text_bytes(
    const char* data, unsigned long long size);
extern "C" char* quidra_runtime_allocate_text_buffer(
    unsigned long long size);
extern "C" bool quidra_runtime_commit_text_buffer(
    char* data, unsigned long long size);
extern "C" void quidra_managed_release(void* value, void* drop_function);
extern "C" unsigned long long quidra_runtime_text_byte_length(
    const char* text);
