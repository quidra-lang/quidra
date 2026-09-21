#include "jit.hpp"

#include "native_build.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::jit {
namespace {

class DynamicLibrary {
public:
    DynamicLibrary() = default;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    ~DynamicLibrary() { close(); }

    static std::unique_ptr<DynamicLibrary> open(
        const fs::path& path, bool global = false) {
        auto result = std::make_unique<DynamicLibrary>();
#ifdef _WIN32
        (void)global;
        result->handle_ = LoadLibraryW(path.c_str());
#else
        result->handle_ = dlopen(
            path.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
#endif
        if (!result->handle_) return nullptr;
        return result;
    }

    template <typename T>
    T symbol(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<T>(GetProcAddress(handle_, name));
#else
        return reinterpret_cast<T>(dlsym(handle_, name));
#endif
    }

private:
    void close() {
        if (!handle_) return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

#ifdef _WIN32
    HMODULE handle_{};
#else
    void* handle_{};
#endif
};

std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::nullopt;
    std::string value(raw, size > 0 ? size - 1 : 0);
    std::free(raw);
    return value;
#else
    if (const char* raw = std::getenv(name)) return std::string(raw);
    return std::nullopt;
#endif
}

std::vector<fs::path> llvm_library_candidates() {
    std::vector<fs::path> result;
    if (const auto configured = environment_value("QUIDRA_LLVM_LIBRARY");
        configured && !configured->empty()) {
        result.emplace_back(*configured);
    }
#ifdef _WIN32
    result.emplace_back("LLVM-C.dll");
    result.emplace_back("LLVM.dll");
    result.emplace_back(R"(C:\Program Files\LLVM\bin\LLVM-C.dll)");
    result.emplace_back(R"(C:\Program Files\LLVM\bin\LLVM.dll)");
#elif defined(__APPLE__)
    result.emplace_back("libLLVM.dylib");
    result.emplace_back("/opt/homebrew/opt/llvm/lib/libLLVM.dylib");
    result.emplace_back("/usr/local/opt/llvm/lib/libLLVM.dylib");
    for (int version = 24; version >= 15; --version) {
        result.emplace_back("/opt/homebrew/opt/llvm@" + std::to_string(version) + "/lib/libLLVM.dylib");
        result.emplace_back("/usr/local/opt/llvm@" + std::to_string(version) + "/lib/libLLVM.dylib");
    }
#else
    result.emplace_back("libLLVM.so");
    for (int version = 24; version >= 15; --version) {
        result.emplace_back("libLLVM-" + std::to_string(version) + ".so");
        result.emplace_back("libLLVM-" + std::to_string(version) + ".so.1");
        result.emplace_back("/usr/lib/llvm-" + std::to_string(version) + "/lib/libLLVM.so");
        result.emplace_back("/usr/lib/llvm-" + std::to_string(version) + "/lib/libLLVM.so.1");
    }
#endif
    return result;
}

std::unique_ptr<DynamicLibrary> load_llvm_library() {
    for (const auto& candidate : llvm_library_candidates()) {
        if (auto library = DynamicLibrary::open(candidate)) return library;
    }
    throw std::runtime_error(
        "LLVM ORC JIT runtime was not found; install LLVM 15 or newer or set "
        "QUIDRA_LLVM_LIBRARY to the LLVM shared library");
}

using Ref = void*;
using ErrorRef = void*;
using ExecutorAddress = std::uint64_t;

struct LlvmApi {
    explicit LlvmApi(std::unique_ptr<DynamicLibrary> loaded)
        : library(std::move(loaded)) {
        memory_buffer_copy = required<Ref (*)(const char*, std::size_t, const char*)>(
            "LLVMCreateMemoryBufferWithMemoryRangeCopy");
        parse_ir = required<int (*)(Ref, Ref, Ref*, char**)>("LLVMParseIRInContext");
        dispose_message = required<void (*)(char*)>("LLVMDisposeMessage");
        get_error_message = required<char* (*)(ErrorRef)>("LLVMGetErrorMessage");
        dispose_error_message = required<void (*)(char*)>("LLVMDisposeErrorMessage");
        context_create = required<Ref (*)()>("LLVMContextCreate");
        context_dispose = required<void (*)(Ref)>("LLVMContextDispose");
        create_thread_safe_context =
            library->symbol<Ref (*)()>("LLVMOrcCreateNewThreadSafeContext");
        thread_safe_context_get_context =
            library->symbol<Ref (*)(Ref)>("LLVMOrcThreadSafeContextGetContext");
        create_thread_safe_context_from_context =
            library->symbol<Ref (*)(Ref)>(
                "LLVMOrcCreateNewThreadSafeContextFromLLVMContext");
        if (!create_thread_safe_context_from_context &&
            (!create_thread_safe_context || !thread_safe_context_get_context)) {
            throw std::runtime_error(
                "LLVM ORC thread-safe context API is unavailable");
        }
        dispose_thread_safe_context =
            required<void (*)(Ref)>("LLVMOrcDisposeThreadSafeContext");
        create_thread_safe_module =
            required<Ref (*)(Ref, Ref)>("LLVMOrcCreateNewThreadSafeModule");
        create_lljit = required<ErrorRef (*)(Ref*, Ref)>("LLVMOrcCreateLLJIT");
        dispose_lljit = required<ErrorRef (*)(Ref)>("LLVMOrcDisposeLLJIT");
        lljit_main_dylib = required<Ref (*)(Ref)>("LLVMOrcLLJITGetMainJITDylib");
        lljit_global_prefix = required<char (*)(Ref)>("LLVMOrcLLJITGetGlobalPrefix");
#ifdef _WIN32
        lljit_object_layer = required<Ref (*)(Ref)>("LLVMOrcLLJITGetObjLinkingLayer");
#endif
        lljit_add_ir = required<ErrorRef (*)(Ref, Ref, Ref)>("LLVMOrcLLJITAddLLVMIRModule");
        lljit_add_ir_with_tracker =
            required<ErrorRef (*)(Ref, Ref, Ref)>("LLVMOrcLLJITAddLLVMIRModuleWithRT");
        create_resource_tracker =
            required<Ref (*)(Ref)>("LLVMOrcJITDylibCreateResourceTracker");
        remove_resource_tracker =
            required<ErrorRef (*)(Ref)>("LLVMOrcResourceTrackerRemove");
        release_resource_tracker =
            required<void (*)(Ref)>("LLVMOrcReleaseResourceTracker");
        lljit_lookup = required<ErrorRef (*)(Ref, ExecutorAddress*, const char*)>("LLVMOrcLLJITLookup");
        create_process_generator = required<ErrorRef (*)(Ref*, char, void*, void*)>(
            "LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess");
#ifdef _WIN32
        if (create_thread_safe_context_from_context) {
            create_static_generator_v21 =
                required<ErrorRef (*)(Ref*, Ref, const char*)>(
                    "LLVMOrcCreateStaticLibrarySearchGeneratorForPath");
        } else {
            create_static_generator_legacy =
                required<ErrorRef (*)(Ref*, Ref, const char*, const char*)>(
                    "LLVMOrcCreateStaticLibrarySearchGeneratorForPath");
        }
#endif
        add_generator = required<void (*)(Ref, Ref)>("LLVMOrcJITDylibAddGenerator");
    }

    template <typename T>
    T required(const char* name) {
        auto function = library->symbol<T>(name);
        if (!function)
            throw std::runtime_error(std::string("LLVM JIT symbol is unavailable: ") + name);
        return function;
    }

    void initialize_native_target() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        constexpr const char* target = "X86";
#elif defined(__aarch64__) || defined(_M_ARM64)
        constexpr const char* target = "AArch64";
#else
        throw std::runtime_error("LLVM JIT is not yet enabled for this host architecture");
#endif
        for (const char* suffix : {"TargetInfo", "Target", "TargetMC", "AsmPrinter"}) {
            const auto name = std::string("LLVMInitialize") + target + suffix;
            required<void (*)()>(name.c_str())();
        }
    }

    [[noreturn]] void throw_error(ErrorRef error, std::string_view operation) {
        char* raw = get_error_message(error);
        std::string message = raw ? raw : "unknown LLVM error";
        if (raw) dispose_error_message(raw);
        throw std::runtime_error(std::string(operation) + ": " + message);
    }

    void check(ErrorRef error, std::string_view operation) {
        if (error) throw_error(error, operation);
    }

    std::unique_ptr<DynamicLibrary> library;
    Ref (*memory_buffer_copy)(const char*, std::size_t, const char*){};
    int (*parse_ir)(Ref, Ref, Ref*, char**){};
    void (*dispose_message)(char*){};
    char* (*get_error_message)(ErrorRef){};
    void (*dispose_error_message)(char*){};
    Ref (*context_create)(){};
    void (*context_dispose)(Ref){};
    Ref (*create_thread_safe_context)(){};
    Ref (*thread_safe_context_get_context)(Ref){};
    Ref (*create_thread_safe_context_from_context)(Ref){};
    void (*dispose_thread_safe_context)(Ref){};
    Ref (*create_thread_safe_module)(Ref, Ref){};
    ErrorRef (*create_lljit)(Ref*, Ref){};
    ErrorRef (*dispose_lljit)(Ref){};
    Ref (*lljit_main_dylib)(Ref){};
    char (*lljit_global_prefix)(Ref){};
    Ref (*lljit_object_layer)(Ref){};
    ErrorRef (*lljit_add_ir)(Ref, Ref, Ref){};
    ErrorRef (*lljit_add_ir_with_tracker)(Ref, Ref, Ref){};
    Ref (*create_resource_tracker)(Ref){};
    ErrorRef (*remove_resource_tracker)(Ref){};
    void (*release_resource_tracker)(Ref){};
    ErrorRef (*lljit_lookup)(Ref, ExecutorAddress*, const char*){};
    ErrorRef (*create_process_generator)(Ref*, char, void*, void*){};
#ifdef _WIN32
    ErrorRef (*create_static_generator_legacy)(
        Ref*, Ref, const char*, const char*){};
    ErrorRef (*create_static_generator_v21)(Ref*, Ref, const char*){};
#endif
    void (*add_generator)(Ref, Ref){};
};

class Jit {
public:
    Jit() : api_(load_llvm_library()) {
        api_.initialize_native_target();
        api_.check(api_.create_lljit(&jit_, nullptr), "creating LLVM ORC JIT");
        const auto dylib = api_.lljit_main_dylib(jit_);
        const auto prefix = api_.lljit_global_prefix(jit_);

#ifdef _WIN32
        Ref runtime_generator = nullptr;
        const auto runtime_text = native::runtime_library().string();
        ErrorRef runtime_error = nullptr;
        if (api_.create_static_generator_v21) {
            runtime_error = api_.create_static_generator_v21(
                &runtime_generator, api_.lljit_object_layer(jit_),
                runtime_text.c_str());
        } else {
            runtime_error = api_.create_static_generator_legacy(
                &runtime_generator, api_.lljit_object_layer(jit_),
                runtime_text.c_str(), nullptr);
        }
        api_.check(
            runtime_error,
            "registering the Quidra runtime archive with LLVM ORC");
        api_.add_generator(dylib, runtime_generator);
#else
        // Match the established lli path: the JIT runtime is a shared library
        // with all of Quidra's runtime dependencies already resolved. Loading
        // it globally lets ORC's process generator resolve quidra_* symbols
        // without extracting broad static-archive objects into every module.
        runtime_library_ =
            DynamicLibrary::open(native::jit_runtime_library(), true);
        if (!runtime_library_) {
            throw std::runtime_error(
                "cannot load the Quidra JIT runtime library: " +
                native::jit_runtime_library().string());
        }
#endif

        Ref process_generator = nullptr;
        api_.check(
            api_.create_process_generator(&process_generator, prefix, nullptr, nullptr),
            "registering process symbols with LLVM ORC");
        api_.add_generator(dylib, process_generator);
    }

    ~Jit() {
        if (!jit_) return;
        if (auto error = api_.dispose_lljit(jit_)) {
            char* raw = api_.get_error_message(error);
            if (raw) api_.dispose_error_message(raw);
        }
    }

    std::int64_t run(
        std::string_view llvm_ir,
        std::string_view argv0,
        const std::vector<std::string>& arguments,
        bool* entry_started = nullptr) {
        if (entry_started) *entry_started = false;
        Ref context = nullptr;
        Ref thread_safe_context = nullptr;
        if (api_.create_thread_safe_context_from_context) {
            context = api_.context_create();
            if (!context)
                throw std::runtime_error("cannot create LLVM context");
            thread_safe_context =
                api_.create_thread_safe_context_from_context(context);
            if (!thread_safe_context) {
                api_.context_dispose(context);
                throw std::runtime_error(
                    "cannot create LLVM thread-safe context");
            }
        } else {
            thread_safe_context = api_.create_thread_safe_context();
            if (!thread_safe_context)
                throw std::runtime_error(
                    "cannot create LLVM thread-safe context");
            context = api_.thread_safe_context_get_context(thread_safe_context);
            if (!context) {
                api_.dispose_thread_safe_context(thread_safe_context);
                throw std::runtime_error(
                    "LLVM thread-safe context has no LLVMContext");
            }
        }

        Ref module = nullptr;
        char* parse_message = nullptr;
        const auto buffer =
            api_.memory_buffer_copy(llvm_ir.data(), llvm_ir.size(), "<quidra-jit>");
        if (!buffer) {
            api_.dispose_thread_safe_context(thread_safe_context);
            throw std::runtime_error("cannot allocate LLVM IR memory buffer");
        }

        if (api_.parse_ir(context, buffer, &module, &parse_message) != 0) {
            std::string message = parse_message ? parse_message : "unknown LLVM IR parse error";
            if (parse_message) api_.dispose_message(parse_message);
            api_.dispose_thread_safe_context(thread_safe_context);
            throw std::runtime_error("parsing generated LLVM IR: " + message);
        }
        if (parse_message) api_.dispose_message(parse_message);

        Ref thread_safe_module = api_.create_thread_safe_module(module, thread_safe_context);
        if (!thread_safe_module) {
            api_.dispose_thread_safe_context(thread_safe_context);
            throw std::runtime_error("cannot create LLVM thread-safe module");
        }

        const auto dylib = api_.lljit_main_dylib(jit_);
        Ref tracker = api_.create_resource_tracker(dylib);
        if (!tracker) {
            api_.dispose_thread_safe_context(thread_safe_context);
            throw std::runtime_error("cannot create LLVM ORC resource tracker");
        }

        const auto discard_tracker_error = [&](ErrorRef error) {
            if (!error) return;
            char* raw = api_.get_error_message(error);
            if (raw) api_.dispose_error_message(raw);
        };
        const auto cleanup_tracker = [&](bool report_error) {
            const auto error = api_.remove_resource_tracker(tracker);
            api_.release_resource_tracker(tracker);
            tracker = nullptr;
            if (report_error) api_.check(error, "removing generated LLVM IR from ORC");
            else discard_tracker_error(error);
        };

        try {
            const auto error =
                api_.lljit_add_ir_with_tracker(jit_, tracker, thread_safe_module);
            api_.dispose_thread_safe_context(thread_safe_context);
            thread_safe_context = nullptr;
            api_.check(error, "adding generated LLVM IR to ORC");

            ExecutorAddress address = 0;
            api_.check(
                api_.lljit_lookup(jit_, &address, "main"),
                "looking up JIT entrypoint");
            if (!address)
                throw std::runtime_error("LLVM ORC returned a null entrypoint");

            std::vector<std::string> storage;
            storage.reserve(arguments.size() + 1);
            storage.emplace_back(argv0);
            storage.insert(storage.end(), arguments.begin(), arguments.end());

            std::vector<char*> argv;
            argv.reserve(storage.size() + 1);
            for (auto& value : storage) argv.push_back(value.data());
            argv.push_back(nullptr);

            using Entry = std::int64_t (*)(int, char**);
            const auto entry =
                reinterpret_cast<Entry>(static_cast<std::uintptr_t>(address));
            if (entry_started) *entry_started = true;
            const auto status =
                entry(static_cast<int>(storage.size()), argv.data());

            // The user program already completed. A cleanup failure must not
            // turn a successful execution into a retryable pre-execution error:
            // doing so could duplicate side effects in the caller.
            cleanup_tracker(false);
            return status;
        } catch (...) {
            if (thread_safe_context)
                api_.dispose_thread_safe_context(thread_safe_context);
            if (tracker) cleanup_tracker(false);
            throw;
        }
    }

private:
    LlvmApi api_;
    Ref jit_{};
    std::unique_ptr<DynamicLibrary> runtime_library_;
};

} // namespace

#ifndef _WIN32
namespace {

bool fd_write_all(int fd, std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(
            fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool redirect_jit_output(
    const fs::path& stdout_path, const fs::path& stderr_path,
    int protocol_stdout, int protocol_stderr,
    int& output_fd, int& error_fd) {
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();

    output_fd = ::open(
        stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output_fd < 0) return false;
    error_fd = ::open(
        stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (error_fd < 0) {
        ::close(output_fd);
        output_fd = -1;
        return false;
    }
    if (::dup2(output_fd, STDOUT_FILENO) < 0 ||
        ::dup2(error_fd, STDERR_FILENO) < 0) {
        ::dup2(protocol_stdout, STDOUT_FILENO);
        ::dup2(protocol_stderr, STDERR_FILENO);
        ::close(output_fd);
        ::close(error_fd);
        output_fd = -1;
        error_fd = -1;
        return false;
    }
    return true;
}

void restore_jit_output(
    int protocol_stdout, int protocol_stderr,
    int& output_fd, int& error_fd) {
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();
    ::dup2(protocol_stdout, STDOUT_FILENO);
    ::dup2(protocol_stderr, STDERR_FILENO);
    std::cout.clear();
    std::cerr.clear();
    if (output_fd >= 0) ::close(output_fd);
    if (error_fd >= 0) ::close(error_fd);
    output_fd = -1;
    error_fd = -1;
}

} // namespace
#endif

int run_server(
    const fs::path& stdout_path,
    const fs::path& stderr_path) {
#ifdef _WIN32
    (void)stdout_path;
    (void)stderr_path;
    throw std::runtime_error("persistent JIT server is unavailable on Windows");
#else
    const int protocol_stdout = ::dup(STDOUT_FILENO);
    const int protocol_stderr = ::dup(STDERR_FILENO);
    if (protocol_stdout < 0 || protocol_stderr < 0)
        return 1;

    try {
        Jit jit;
        if (!fd_write_all(protocol_stdout, "READY\n"))
            return 1;

        std::string header;
        while (std::getline(std::cin, header)) {
            if (header.empty()) continue;
            if (header == "QUIT") break;
            if (header.rfind("RUN ", 0) != 0) {
                const std::string message = "invalid persistent JIT request";
                fd_write_all(
                    protocol_stdout,
                    "ERR " + std::to_string(message.size()) + "\n" + message);
                continue;
            }

            std::size_t count = 0;
            try {
                const auto parsed = std::stoull(header.substr(4));
                if (parsed > std::numeric_limits<std::size_t>::max())
                    throw std::out_of_range("request too large");
                count = static_cast<std::size_t>(parsed);
            } catch (...) {
                const std::string message = "invalid persistent JIT request length";
                fd_write_all(
                    protocol_stdout,
                    "ERR " + std::to_string(message.size()) + "\n" + message);
                continue;
            }

            std::string llvm_ir(count, '\0');
            if (count != 0) {
                std::cin.read(llvm_ir.data(), static_cast<std::streamsize>(count));
                if (static_cast<std::size_t>(std::cin.gcount()) != count)
                    break;
            }

            int output_fd = -1;
            int error_fd = -1;
            if (!redirect_jit_output(
                    stdout_path, stderr_path,
                    protocol_stdout, protocol_stderr,
                    output_fd, error_fd)) {
                const std::string message = "cannot redirect persistent JIT output";
                fd_write_all(
                    protocol_stdout,
                    "ERR " + std::to_string(message.size()) + "\n" + message);
                continue;
            }

            bool entry_started = false;
            try {
                const auto status =
                    jit.run(llvm_ir, "<repl>", {}, &entry_started);
                restore_jit_output(
                    protocol_stdout, protocol_stderr, output_fd, error_fd);
                if (!fd_write_all(
                        protocol_stdout,
                        "OK " + std::to_string(status) + "\n"))
                    break;
            } catch (const std::exception& error) {
                restore_jit_output(
                    protocol_stdout, protocol_stderr, output_fd, error_fd);
                const std::string message = error.what();
                const auto tag = entry_started ? "POSTERR " : "ERR ";
                if (!fd_write_all(
                        protocol_stdout,
                        std::string(tag) + std::to_string(message.size()) +
                            "\n" + message))
                    break;
            }
        }
    } catch (const std::exception& error) {
        const std::string message = error.what();
        fd_write_all(
            protocol_stdout,
            "STARTERR " + std::to_string(message.size()) + "\n" + message);
        ::close(protocol_stdout);
        ::close(protocol_stderr);
        return 1;
    }

    ::close(protocol_stdout);
    ::close(protocol_stderr);
    return 0;
#endif
}

int run_llvm(
    std::string_view llvm_ir,
    std::string_view argv0,
    const std::vector<std::string>& arguments) {
    Jit jit;
    return static_cast<int>(jit.run(llvm_ir, argv0, arguments));
}

} // namespace quidra::jit
