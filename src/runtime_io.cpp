#include "runtime_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {
bool valid_text(const std::string& value) {
    return quidra_runtime_text_valid_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}

char* copy_validated_text(const std::string& value) {
    return quidra_runtime_copy_validated_text_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}

char* read_text_direct(const char* path) {
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error ||
        file_bytes >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max()) ||
        file_bytes >
            static_cast<std::uintmax_t>(
                std::numeric_limits<unsigned long long>::max())) {
        return nullptr;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;

    const auto size = static_cast<unsigned long long>(file_bytes);
    auto* result = quidra_runtime_allocate_text_buffer(size);
    if (!result) return nullptr;

    if (size != 0) {
        in.read(result, static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) {
            quidra_managed_release(result, nullptr);
            return nullptr;
        }
    }

    // file_size() is only a hint: if the file grew after the query, use the
    // general streaming path instead of silently truncating it.
    char extra = 0;
    in.read(&extra, 1);
    if (in.gcount() != 0 || (!in.eof() && in.fail())) {
        quidra_managed_release(result, nullptr);
        return nullptr;
    }

    if (!quidra_runtime_commit_text_buffer(result, size)) {
        quidra_managed_release(result, nullptr);
        return nullptr;
    }
    return result;
}

bool read_stream_bytes(std::istream& in, std::string& data) {
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            if (size > std::numeric_limits<std::size_t>::max() - data.size())
                return false;
            data.append(buffer.data(), size);
        }
        if (in.eof()) return true;
        if (!in) return false;
    }
}

bool read_all_bytes(const char* path, std::string& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (!size_error &&
        file_bytes <= static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max())) {
        data.reserve(static_cast<std::size_t>(file_bytes));
    }
    return read_stream_bytes(in, data);
}

enum class FileMode { Read, Create, Append };
enum class FileStreamDirection { None, Read, Write };

struct FileResource {
    std::fstream stream;
    std::mutex mutex;
    bool readable{};
    bool writable{};
    std::uint64_t stream_position{};
    bool stream_position_valid{};
    FileStreamDirection stream_direction{FileStreamDirection::None};
};

bool position_file_stream(
    FileResource& resource, std::uint64_t position,
    FileStreamDirection direction) {
    if (!resource.stream.is_open()) return false;
    if (resource.stream_position_valid &&
        resource.stream_position == position &&
        resource.stream_direction == direction) {
        return true;
    }

    // Switching away from output must publish buffered bytes before the
    // shared fstream is repositioned for a reader or another logical handle.
    if (resource.stream_direction == FileStreamDirection::Write) {
        resource.stream.flush();
        if (!resource.stream) {
            resource.stream.clear();
            resource.stream_position_valid = false;
            resource.stream_direction = FileStreamDirection::None;
            return false;
        }
    }

    resource.stream.clear();
    const auto offset = static_cast<std::streamoff>(position);
    if (direction == FileStreamDirection::Read)
        resource.stream.seekg(offset, std::ios::beg);
    else
        resource.stream.seekp(offset, std::ios::beg);
    if (!resource.stream) {
        resource.stream.clear();
        resource.stream_position_valid = false;
        resource.stream_direction = FileStreamDirection::None;
        return false;
    }
    resource.stream_position = position;
    resource.stream_position_valid = true;
    resource.stream_direction = direction;
    return true;
}

void advance_file_stream(
    FileResource& resource, std::uint64_t position,
    FileStreamDirection direction) {
    resource.stream_position = position;
    resource.stream_position_valid = true;
    resource.stream_direction = direction;
}

void invalidate_file_stream(FileResource& resource) {
    resource.stream_position_valid = false;
    resource.stream_direction = FileStreamDirection::None;
}

struct FileState {
    std::shared_ptr<FileResource> resource;
    std::uint64_t position{};
    bool closed{};

    explicit FileState(std::shared_ptr<FileResource> source)
        : resource(std::move(source)) {}
};

struct FileHandle {
    std::unique_ptr<FileState> state;
};

FileHandle* file_handle_from_value(void* value) {
    if (!value) return nullptr;
    std::uintptr_t bits{};
    std::memcpy(&bits, value, sizeof(bits));
    return reinterpret_cast<FileHandle*>(bits);
}

void* make_file_handle(FileHandle* handle) {
    auto* value = quidra_managed_alloc(sizeof(std::uintptr_t));
    const auto bits = reinterpret_cast<std::uintptr_t>(handle);
    std::memcpy(value, &bits, sizeof(bits));
    return value;
}

void* make_bin_value(const std::string& data) {
    if (data.size() >
            static_cast<std::size_t>(
                std::numeric_limits<long long>::max() / 8) ||
        data.size() > std::numeric_limits<std::size_t>::max() - 8) {
        return nullptr;
    }
    auto* result = static_cast<unsigned char*>(
        quidra_managed_alloc(
            static_cast<unsigned long long>(8 + data.size())));
    const auto bit_count = static_cast<long long>(data.size() * 8);
    std::memcpy(result, &bit_count, sizeof(bit_count));
    if (!data.empty()) std::memcpy(result + 8, data.data(), data.size());
    return result;
}
}

void* file_open_mode_raw(const char* path, FileMode mode) {
    if (!path || !*path) return nullptr;
    try {
        auto resource = std::make_shared<FileResource>();
        resource->readable = true;
        resource->writable = mode != FileMode::Read;

        if (mode == FileMode::Read) {
            resource->stream.open(path, std::ios::binary | std::ios::in);
        } else if (mode == FileMode::Create) {
            resource->stream.open(
                path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        } else {
            // Ensure the target exists, then reopen without std::ios::app so each
            // copied Handle can keep an independent logical position.
            {
                std::ofstream ensure(path, std::ios::binary | std::ios::app);
                if (!ensure) return nullptr;
            }
            resource->stream.open(path, std::ios::binary | std::ios::in | std::ios::out);
        }
        if (!resource->stream) return nullptr;

        auto state = std::make_unique<FileState>(resource);
        if (mode == FileMode::Append) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > std::numeric_limits<std::uint64_t>::max()) return nullptr;
            state->position = static_cast<std::uint64_t>(size);
        }
        auto* handle = new FileHandle{std::move(state)};
        return make_file_handle(handle);
    } catch (...) {
        return nullptr;
    }
}

extern "C" void* quidra_file_open_raw(const char* path) {
    return file_open_mode_raw(path, FileMode::Read);
}

extern "C" void* quidra_file_create_raw(const char* path) {
    return file_open_mode_raw(path, FileMode::Create);
}

extern "C" void* quidra_file_append_raw(const char* path) {
    return file_open_mode_raw(path, FileMode::Append);
}

extern "C" char* quidra_file_handle_read_raw(void* value) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource) {
        return nullptr;
    }
    const auto start = handle->state->position;
    if (start > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return nullptr;
    }
    if (!handle->state->resource->readable) return nullptr;
    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!position_file_stream(resource, start, FileStreamDirection::Read))
        return nullptr;

    std::string data;
    if (!read_stream_bytes(stream, data) || !valid_text(data) ||
        data.size() > std::numeric_limits<std::uint64_t>::max() - start) {
        stream.clear();
        invalidate_file_stream(resource);
        return nullptr;
    }
    auto* result = copy_validated_text(data);
    if (!result) {
        stream.clear();
        invalidate_file_stream(resource);
        return nullptr;
    }
    handle->state->position =
        start + static_cast<std::uint64_t>(data.size());
    advance_file_stream(
        resource, handle->state->position, FileStreamDirection::Read);
    return result;
}

extern "C" int quidra_file_handle_read_line_raw(void* value, char** out) {
    if (!out) return -1;
    *out = nullptr;
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource || !handle->state->resource->readable) return -1;
    const auto start = handle->state->position;
    if (start > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return -1;

    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!position_file_stream(resource, start, FileStreamDirection::Read))
        return -1;

    std::string line;
    if (!std::getline(stream, line)) {
        const bool eof = stream.eof();
        stream.clear();
        if (eof) {
            advance_file_stream(resource, start, FileStreamDirection::Read);
            return 0;
        }
        invalidate_file_stream(resource);
        return -1;
    }

    const auto delimiter_bytes = stream.eof() ? 0ULL : 1ULL;
    if (line.size() > std::numeric_limits<std::uint64_t>::max() - delimiter_bytes ||
        start > std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(line.size()) - delimiter_bytes ||
        !valid_text(line)) {
        stream.clear();
        invalidate_file_stream(resource);
        return -1;
    }
    auto* text = copy_validated_text(line);
    if (!text) {
        stream.clear();
        invalidate_file_stream(resource);
        return -1;
    }
    handle->state->position =
        start + static_cast<std::uint64_t>(line.size()) + delimiter_bytes;
    advance_file_stream(
        resource, handle->state->position, FileStreamDirection::Read);
    *out = text;
    return 1;
}

extern "C" void* quidra_file_handle_read_bin_raw(void* value) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource) {
        return nullptr;
    }
    const auto start = handle->state->position;
    if (start > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return nullptr;
    }
    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!position_file_stream(resource, start, FileStreamDirection::Read))
        return nullptr;

    std::string data;
    if (!read_stream_bytes(stream, data) ||
        data.size() > std::numeric_limits<std::uint64_t>::max() - start) {
        stream.clear();
        invalidate_file_stream(resource);
        return nullptr;
    }
    auto* result = make_bin_value(data);
    if (!result) {
        stream.clear();
        invalidate_file_stream(resource);
        return nullptr;
    }
    handle->state->position =
        start + static_cast<std::uint64_t>(data.size());
    advance_file_stream(
        resource, handle->state->position, FileStreamDirection::Read);
    return result;
}

extern "C" bool quidra_file_handle_write_raw(void* value, const char* text, bool line) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource || !handle->state->resource->writable || !text) {
        return false;
    }
    const auto byte_count = quidra_runtime_text_byte_length(text);
    const auto extra = line ? 1ULL : 0ULL;
    if (byte_count > static_cast<unsigned long long>(std::numeric_limits<std::streamsize>::max()) ||
        byte_count > std::numeric_limits<std::uint64_t>::max() - extra ||
        handle->state->position > std::numeric_limits<std::uint64_t>::max() - byte_count - extra ||
        handle->state->position > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }

    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!position_file_stream(
            resource, handle->state->position, FileStreamDirection::Write))
        return false;
    if (byte_count) stream.write(text, static_cast<std::streamsize>(byte_count));
    if (line) stream.put('\n');
    if (!stream) {
        stream.clear();
        invalidate_file_stream(resource);
        return false;
    }
    handle->state->position += byte_count + extra;
    advance_file_stream(
        resource, handle->state->position, FileStreamDirection::Write);
    return true;
}

extern "C" bool quidra_file_handle_flush_raw(void* value) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource || !handle->state->resource->writable) {
        return false;
    }
    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!stream.is_open()) return false;
    stream.flush();
    if (!stream) {
        stream.clear();
        invalidate_file_stream(resource);
        return false;
    }
    return true;
}

extern "C" bool quidra_file_handle_seek_raw(void* value, long long position) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed ||
        !handle->state->resource || position < 0) {
        return false;
    }
    const auto requested = static_cast<std::uint64_t>(position);
    if (requested > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;

    auto& resource = *handle->state->resource;
    std::lock_guard<std::mutex> lock(resource.mutex);
    auto& stream = resource.stream;
    if (!stream.is_open()) return false;
    if (resource.writable) {
        stream.flush();
        if (!stream) {
            stream.clear();
            invalidate_file_stream(resource);
            return false;
        }
    }
    stream.clear();
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    invalidate_file_stream(resource);
    if (end == std::streampos(-1) || end < std::streampos(0)) {
        stream.clear();
        return false;
    }
    const auto size = static_cast<std::uint64_t>(end - std::streampos(0));
    if (requested > size) return false;
    handle->state->position = requested;
    return true;
}

extern "C" void quidra_file_handle_close(void* value) {
    auto* handle = file_handle_from_value(value);
    if (!handle || !handle->state || handle->state->closed) return;
    // Release this value's ownership immediately. Other copied Handles keep
    // the shared opened resource alive and retain their own logical positions.
    handle->state->resource.reset();
    handle->state->closed = true;
}

extern "C" void* quidra_file_handle_clone(void* value) {
    try {
        auto* handle = file_handle_from_value(value);
        if (!handle || !handle->state) {
            std::fprintf(stderr, "Quidra runtime error: invalid file handle copy\n");
            std::exit(101);
        }

        auto state = std::make_unique<FileState>(handle->state->resource);
        state->position = handle->state->position;
        state->closed = handle->state->closed;

        // Preserve the originally opened resource identity. The copied value
        // has its own logical position and close state.
        auto* copy = new FileHandle{std::move(state)};
        return make_file_handle(copy);
    } catch (...) {
        std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
        std::exit(101);
    }
}

extern "C" void quidra_file_handle_drop(void* value) {
    if (!value) return;
    auto* handle = file_handle_from_value(value);
    std::uintptr_t zero{};
    std::memcpy(value, &zero, sizeof(zero));
    delete handle;
}

extern "C" char* quidra_file_read_raw(const char* path) {
    if (!path) return nullptr;
    if (auto* direct = read_text_direct(path)) return direct;
    std::string data;
    if (!read_all_bytes(path, data)) return nullptr;
    return quidra_runtime_try_copy_text_bytes(
        data.data(), static_cast<unsigned long long>(data.size()));
}

extern "C" void* quidra_file_read_bin_raw(const char* path) {
    if (!path) return nullptr;
    std::string data;
    if (!read_all_bytes(path, data)) return nullptr;
    return make_bin_value(data);
}

extern "C" bool quidra_file_write_raw(const char* path,const char* text) {
    if(!path||!text) return false;
    const auto byte_count = quidra_runtime_text_byte_length(text);
    if (byte_count >
        static_cast<unsigned long long>(std::numeric_limits<std::streamsize>::max()))
        return false;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) return false;
    if(byte_count)
        out.write(text,static_cast<std::streamsize>(byte_count));
    out.close();
    return static_cast<bool>(out);
}

extern "C" bool quidra_file_write_bin_raw(const char* path,const void* bin_raw) {
    if(!path||!bin_raw) return false;
    long long bit_count=0;
    std::memcpy(&bit_count,bin_raw,sizeof(bit_count));
    if(bit_count<0 || bit_count%8!=0) return false;
    const auto byte_count=static_cast<unsigned long long>(bit_count/8);
    const auto size=static_cast<std::size_t>(byte_count);
    if(static_cast<unsigned long long>(size)!=byte_count ||
       size>static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) return false;
    if(size) out.write(static_cast<const char*>(bin_raw)+8,static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

extern "C" int quidra_file_exists_raw(const char* path) {
    if(!path) return -1;
    std::error_code error;
    const bool result=std::filesystem::exists(path,error);
    return error?-1:(result?1:0);
}
extern "C" int quidra_file_is_directory_raw(const char* path) {
    if(!path) return -1;
    std::error_code error;
    const bool exists=std::filesystem::exists(path,error);
    if(error) return -1;
    if(!exists) return 0;
    const bool result=std::filesystem::is_directory(path,error);
    return error?-1:(result?1:0);
}
extern "C" bool quidra_file_remove_raw(const char* path) {
    if(!path) return false;
    std::error_code error;
    return std::filesystem::remove(path,error)&&!error;
}
extern "C" bool quidra_file_copy_raw(const char* source,const char* destination) {
    if(!source||!destination) return false;
    std::error_code error;
    return std::filesystem::copy_file(
        source,destination,std::filesystem::copy_options::overwrite_existing,error)&&!error;
}
extern "C" bool quidra_file_move_raw(const char* source,const char* destination) {
    if(!source||!destination) return false;
    std::error_code error;
    std::filesystem::rename(source,destination,error);
    return !error;
}
extern "C" bool quidra_file_mkdir_raw(const char* path) {
    if(!path) return false;
    std::error_code error;
    return std::filesystem::create_directory(path,error)&&!error;
}

extern "C" void* quidra_file_list_raw(const char* path,bool recursive) {
    if(!path) return nullptr;
    std::error_code error;
    std::vector<std::string> entries;
    auto append=[&](const std::filesystem::path& entry){
        try{
            const auto encoded=entry.u8string();
            std::string text(reinterpret_cast<const char*>(encoded.data()),encoded.size());
            if(!valid_text(text)) return false;
            entries.push_back(std::move(text));
            return true;
        }catch(const std::system_error&){return false;}
    };
    if(recursive){
        std::filesystem::recursive_directory_iterator current(path,error);
        if(error) return nullptr;
        const std::filesystem::recursive_directory_iterator end;
        while(current!=end){
            if(!append(current->path())) return nullptr;
            current.increment(error);
            if(error) return nullptr;
        }
    }else{
        std::filesystem::directory_iterator current(path,error);
        if(error) return nullptr;
        const std::filesystem::directory_iterator end;
        while(current!=end){
            if(!append(current->path())) return nullptr;
            current.increment(error);
            if(error) return nullptr;
        }
    }
    std::sort(entries.begin(),entries.end());
    if(entries.size()>(std::numeric_limits<std::size_t>::max()-8)/sizeof(char*)) return nullptr;
    const auto bytes=8+entries.size()*sizeof(char*);
    auto* result=static_cast<unsigned char*>(
        quidra_managed_alloc(static_cast<unsigned long long>(bytes)));
    const auto count=static_cast<long long>(entries.size());
    std::memcpy(result,&count,sizeof(count));
    for(std::size_t i=0;i<entries.size();++i){
        auto* item=copy_validated_text(entries[i]);
        std::memcpy(result+8+i*sizeof(char*),&item,sizeof(item));
    }
    return result;
}

extern "C" char* quidra_environment_get(const char* name) {
    if(!name) return nullptr;
#ifdef _WIN32
    char* value=nullptr;
    std::size_t size=0;
    if(_dupenv_s(&value,&size,name)!=0||!value) return nullptr;
    const std::string text(value,size?size-1:0);
    std::free(value);
#else
    const char* value=std::getenv(name);
    if(!value) return nullptr;
    const std::string text(value);
#endif
    if(!valid_text(text)) {
        quidra_runtime_text_error(
            "environment value must be valid UTF-8 text without NUL");
    }
    return copy_validated_text(text);
}

extern "C" bool quidra_environment_has(const char* name) {
    if(!name) return false;
#ifdef _WIN32
    char* value=nullptr;
    std::size_t size=0;
    const auto error=_dupenv_s(&value,&size,name);
    const bool present=error==0&&value!=nullptr;
    std::free(value);
    return present;
#else
    return std::getenv(name)!=nullptr;
#endif
}
