cmake_minimum_required(VERSION 3.20)

# project.toml is the single source of truth for project metadata. Reading it
# here, before project(), is what lets every other file derive its values
# instead of repeating them.
function(quidra_project_value key output)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/project.toml" matched
       REGEX "^${key} = .+$")
  list(LENGTH matched count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR
        "project.toml must declare '${key}' exactly once, found ${count}")
  endif()
  string(REGEX REPLACE "^${key} = \"?([^\"]*)\"?$" "\\1" value "${matched}")
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

quidra_project_value(version QUIDRA_PROJECT_VERSION)
quidra_project_value(name QUIDRA_PROJECT_NAME)
quidra_project_value(cli QUIDRA_PROJECT_CLI)
quidra_project_value(extension QUIDRA_PROJECT_EXTENSION)
quidra_project_value(tagline QUIDRA_TAGLINE)
quidra_project_value(abi QUIDRA_ABI_VERSION)
quidra_project_value(ir QUIDRA_IR_VERSION)
quidra_project_value(package_schema QUIDRA_PACKAGE_SCHEMA_VERSION)
quidra_project_value(lockfile_schema QUIDRA_LOCKFILE_SCHEMA_VERSION)
quidra_project_value(manifest_file QUIDRA_PACKAGE_MANIFEST_FILE)
quidra_project_value(project_file QUIDRA_PACKAGE_PROJECT_FILE)
quidra_project_value(lock_file QUIDRA_PACKAGE_LOCK_FILE)
quidra_project_value(entrypoint_stem QUIDRA_PACKAGE_ENTRYPOINT_STEM)
quidra_project_value(store_relative QUIDRA_PACKAGE_STORE_RELATIVE)
quidra_project_value(official_distribution_prefix QUIDRA_OFFICIAL_DISTRIBUTION_PREFIX)
quidra_project_value(official_repository_base QUIDRA_OFFICIAL_REPOSITORY_BASE)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/quidra.manifest.json" QUIDRA_MANIFEST_JSON)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/grammar.ebnf" QUIDRA_GRAMMAR_EBNF)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/grammar.ebnf" QUIDRA_GRAMMAR_SHA256)
project(${QUIDRA_PROJECT_CLI} VERSION ${QUIDRA_PROJECT_VERSION} LANGUAGES CXX)

if(APPLE)
  enable_language(OBJCXX)
  set_source_files_properties(src/device_backend.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

# file(READ) and file(STRINGS) create no dependency on their own: editing any of
# these would otherwise leave a stale copy compiled in until the next
# reconfigure. The manifest supplies the text `quidra describe` prints and the
# machine-readable grammar is embedded for the same reason.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/project.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/quidra.manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/grammar.ebnf")

include(GNUInstallDirs)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
if(CMAKE_CXX_STANDARD LESS 20)
  message(FATAL_ERROR "Quidra requires C++20 or newer.")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QUIDRA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for Quidra targets" OFF)
option(QUIDRA_SANITIZE_RUNTIME "Instrument the generated-program runtime archive in sanitizer builds" OFF)
option(QUIDRA_ENABLE_TEST_GPU_BACKEND "Compile the deterministic test-only fake GPU backend" OFF)

# The browser frontend needs the parser, checker, formatter, typed IR, source
# inspection and source patching -- all of which live in quidra_core, which has
# no third-party dependencies. Everything that pulls CURL/Metal belongs to
# the generated-program runtime and the native CLI, so a
# frontend-only build simply omits those targets. Emscripten implies it.
option(QUIDRA_BUILD_WASM_FRONTEND
    "Build only the dependency-free compiler frontend and its WebAssembly bridge" OFF)
if(EMSCRIPTEN)
  set(QUIDRA_BUILD_WASM_FRONTEND ON)
endif()
# The golden dump tool of the refactoring gates, built against the frontend
# for node (gate Gw, tests/golden/wasm_tool.sh). Off in every normal build.
option(QUIDRA_BUILD_WASM_GOLDEN
    "With the WebAssembly frontend, also build quidra_golden_dump_wasm for node" OFF)

if(QUIDRA_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Werror)
  endif()
endif()

configure_file(
    include/quidra/project.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/quidra/project.hpp
    @ONLY
)

configure_file(
    include/quidra/manifest.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/quidra/manifest.hpp
    @ONLY
)

configure_file(
    include/quidra/tooling.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/quidra/tooling.hpp
    @ONLY
)

add_library(quidra_core
    src/lexer.cpp
    src/parser.cpp
    src/checker.cpp
    src/ir/analysis/call_graph.cpp
    src/ir/analysis/device_reach.cpp
    src/ir/dump.cpp
    src/ir/instruction_text.cpp
    src/lowering/aggregate_lowering.cpp
    src/lowering/assignment_lowering.cpp
    src/lowering/assignment_order.cpp
    src/lowering/autograd_lowering.cpp
    src/lowering/bin_lowering.cpp
    src/lowering/borrow_inference.cpp
    src/lowering/builtin_lowering.cpp
    src/lowering/call_lowering.cpp
    src/lowering/check_elision_facts.cpp
    src/lowering/check_lowering.cpp
    src/lowering/collection_idioms.cpp
    src/lowering/concurrency_lowering.cpp
    src/lowering/console_lowering.cpp
    src/lowering/control_flow_lowering.cpp
    src/lowering/conversion_lowering.cpp
    src/lowering/enclosing_class.cpp
    src/lowering/expression_lowering.cpp
    src/lowering/file_lowering.cpp
    src/lowering/function_lowering.cpp
    src/lowering/http_lowering.cpp
    src/lowering/initialization_flags.cpp
    src/lowering/json_lowering.cpp
    src/lowering/lifetime_lowering.cpp
    src/lowering/lower_module.cpp
    src/lowering/lowerer.cpp
    src/lowering/operator_lowering.cpp
    src/lowering/random_lowering.cpp
    src/lowering/reflection_lowering.cpp
    src/lowering/scan_lowering.cpp
    src/lowering/shape_constraint_lowering.cpp
    src/lowering/source_locator.cpp
    src/lowering/statement_lowering.cpp
    src/lowering/storage_writes.cpp
    src/lowering/string_concat_chain.cpp
    src/lowering/syntax_queries.cpp
    src/lowering/system_lowering.cpp
    src/lowering/tensor_lowering.cpp
    src/lowering/text_idioms.cpp
    src/lowering/text_lowering.cpp
    src/lowering/user_sources.cpp
    src/semantics/argument_isolation.cpp
    src/semantics/effect_summary.cpp
    src/semantics/storage_path.cpp
    src/optimizer/call_contract.cpp
    src/optimizer/chain_fusion.cpp
    src/optimizer/conditional_rules.cpp
    src/optimizer/execution_policy.cpp
    src/optimizer/ownership_adaptation.cpp
    src/optimizer/pass_manager.cpp
    src/optimizer/region_candidates.cpp
    src/optimizer/region_formation.cpp
    src/optimizer/rule_registry.cpp
    src/optimizer/static_tensor_facts.cpp
    src/optimizer/textual_uses.cpp
    src/llvm_backend/aggregate_emitter.cpp
    src/llvm_backend/array_cast_helpers.cpp
    src/llvm_backend/autograd_emitter.cpp
    src/llvm_backend/bare_integer.cpp
    src/llvm_backend/bin_emitter.cpp
    src/llvm_backend/call_emitter.cpp
    src/llvm_backend/check_emitter.cpp
    src/llvm_backend/concurrency_emitter.cpp
    src/llvm_backend/console_emitter.cpp
    src/llvm_backend/constant_emitter.cpp
    src/llvm_backend/control_flow_emitter.cpp
    src/llvm_backend/conversion_emitter.cpp
    src/llvm_backend/debug_emitter.cpp
    src/llvm_backend/debug_module.cpp
    src/llvm_backend/debug_records.cpp
    src/llvm_backend/diagnostic_constants.cpp
    src/llvm_backend/error_result_emitter.cpp
    src/llvm_backend/fail_fast_emitter.cpp
    src/llvm_backend/file_emitter.cpp
    src/llvm_backend/foreign_abi.cpp
    src/llvm_backend/foreign_export_emitter.cpp
    src/llvm_backend/function_emitter.cpp
    src/llvm_backend/http_emitter.cpp
    src/llvm_backend/json_emitter.cpp
    src/llvm_backend/lifetime_emitter.cpp
    src/llvm_backend/memory_emitter.cpp
    src/llvm_backend/module_emitter.cpp
    src/llvm_backend/numeric_emitter.cpp
    src/llvm_backend/random_emitter.cpp
    src/llvm_backend/repl_emitter.cpp
    src/llvm_backend/repl_value_printer.cpp
    src/llvm_backend/runtime_prelude.cpp
    src/llvm_backend/scratch_planner.cpp
    src/llvm_backend/self_depth_guard.cpp
    src/llvm_backend/small_rational.cpp
    src/llvm_backend/real_power.cpp
    src/llvm_backend/statement_attribution.cpp
    src/llvm_backend/storage_layout.cpp
    src/llvm_backend/string_pool.cpp
    src/llvm_backend/symbol_resolver.cpp
    src/llvm_backend/system_emitter.cpp
    src/llvm_backend/tensor_emitter.cpp
    src/llvm_backend/text_emitter.cpp
    src/llvm_backend/type_helpers.cpp
    src/llvm_backend/type_lowering.cpp
    src/llvm_backend/union_box.cpp
    src/llvm_text/escape.cpp
    src/llvm_text/function_text.cpp
    src/llvm_text/llvm_builder.cpp
    src/llvm_text/llvm_module.cpp
    src/compile_inputs.cpp
    src/compiler.cpp
    src/frontend.cpp
    src/import_path.cpp
    src/source_tools.cpp
    src/source_patch.cpp
    src/formatter.cpp
    src/package_manifest.cpp
    src/toml_subset.cpp
    src/package_lock.cpp
    src/platform/sha256.cpp
    src/platform/strict_json.cpp
)
target_include_directories(quidra_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The compiler's internal layers (src/llvm_backend, src/llvm_text, ...) include
# each other by their directory: "llvm_backend/x.hpp".
target_include_directories(quidra_core PRIVATE src)
# Inline functions of the compiler stay hidden. Otherwise each one the
# compiler instantiates, the IR Instruction variant's dispatch tables among
# them, is a weak exported symbol of the CLI that the dynamic loader binds at
# every start. PUBLIC, so that the CLI and everything else compiled against
# the compiler use the same visibility; the runtime libraries keep theirs.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(quidra_core PUBLIC -fvisibility-inlines-hidden)
endif()

# How the REPL cuts its input into submissions and decides how to compile them
# (src/repl_submission.hpp). Compiled once; the CLI and the golden REPL session
# simulator (tests/golden/repl_session.cpp) link the same objects.
add_library(quidra_repl_submission OBJECT EXCLUDE_FROM_ALL src/repl_submission.cpp)
target_link_libraries(quidra_repl_submission PRIVATE quidra_core)

if(NOT QUIDRA_BUILD_WASM_FRONTEND)

find_package(CURL REQUIRED)

# The runtime of a Quidra program: the sources that define its entry points
# (quidra/abi/runtime_entry_points.hpp), then the device layer and its host
# services (CPU parallelism, counters, unified storage). The runtime archive
# and the JIT runtime compile both lists in this order. The CLI compiles the
# device sources too, in its own link order (add_executable(quidra) below).
set(QUIDRA_RUNTIME_SOURCES
    src/runtime.cpp
    src/runtime_integer.cpp
    src/runtime_exact_real.cpp
    src/runtime_io.cpp
    src/runtime_system.cpp
    src/runtime_location.cpp
    src/runtime_process.cpp
    src/runtime_json.cpp
    src/runtime_http.cpp
    src/platform/sha256.cpp
)
set(QUIDRA_DEVICE_SOURCES
    src/runtime_parallel.cpp
    src/runtime_counters.cpp
    src/device_backend.cpp
    src/unified_storage.cpp
)

add_library(quidra_runtime STATIC ${QUIDRA_RUNTIME_SOURCES} ${QUIDRA_DEVICE_SOURCES})

# The REPL uses LLVM ORC through `lli`. Keep a shared runtime alongside the
# static archive so JIT submissions can resolve the exact same Quidra runtime
# ABI without invoking the platform linker for every executable submission.
add_library(quidra_runtime_jit SHARED ${QUIDRA_RUNTIME_SOURCES} ${QUIDRA_DEVICE_SOURCES})
# The runtime's platform sources include their headers by directory
# ("platform/sha256.hpp").
target_include_directories(quidra_runtime_jit PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated ${CURL_INCLUDE_DIRS} src)
target_link_libraries(quidra_runtime_jit PRIVATE CURL::libcurl)
if(CMAKE_DL_LIBS)
  target_link_libraries(quidra_runtime_jit PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
  target_sources(quidra_runtime_jit PRIVATE src/runtime_jit_support.cpp)
  target_link_libraries(quidra_runtime_jit PRIVATE "-framework Foundation" "-framework Metal")

  # LLVM ORC lowers JITed thread_local globals to emulated TLS on Darwin.
  # lli resolves __emutls_get_address through the dylibs passed with --dlopen,
  # while compiler-rt normally ships that entry point only in a static builtins
  # archive. Pull the matching Homebrew LLVM builtins member into the JIT
  # runtime and export it so ORC can resolve the symbol from the loaded dylib.
  find_program(QUIDRA_JIT_CLANG
    NAMES clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    NO_DEFAULT_PATH)
  if(NOT QUIDRA_JIT_CLANG)
    set(QUIDRA_JIT_CLANG "${CMAKE_CXX_COMPILER}")
  endif()
  execute_process(
    COMMAND "${QUIDRA_JIT_CLANG}" --print-resource-dir
    OUTPUT_VARIABLE QUIDRA_JIT_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE QUIDRA_JIT_CLANG_RESOURCE_RESULT)
  set(QUIDRA_JIT_COMPILER_RT
    "${QUIDRA_JIT_CLANG_RESOURCE_DIR}/lib/darwin/libclang_rt.osx.a")
  if(NOT QUIDRA_JIT_CLANG_RESOURCE_RESULT EQUAL 0 OR
     NOT EXISTS "${QUIDRA_JIT_COMPILER_RT}")
    message(FATAL_ERROR
      "compiler-rt Darwin builtins are required for the Quidra REPL JIT; "
      "install Homebrew LLVM or use a Clang toolchain with libclang_rt.osx.a")
  endif()
  target_link_libraries(quidra_runtime_jit PRIVATE "${QUIDRA_JIT_COMPILER_RT}")
  target_link_options(quidra_runtime_jit PRIVATE
    "-Wl,-u,___emutls_get_address")
endif()
if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
  target_compile_definitions(quidra_runtime_jit PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
endif()
set_target_properties(quidra_runtime_jit PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  set_target_properties(quidra_runtime_jit PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_compile_definitions(quidra_runtime_jit PRIVATE NOMINMAX)
endif()
target_include_directories(quidra_runtime PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated ${CURL_INCLUDE_DIRS} src)
if(CMAKE_DL_LIBS)
  target_link_libraries(quidra_runtime PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
  target_link_libraries(quidra_runtime PRIVATE "-framework Foundation" "-framework Metal")
endif()
if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
  target_compile_definitions(quidra_runtime PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
endif()
set_target_properties(quidra_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_compile_definitions(quidra_runtime PRIVATE NOMINMAX)
  target_compile_definitions(quidra_core PRIVATE NOMINMAX)
endif()

# The runtime archive is linked into programs produced by Quidra itself.
# Normal sanitizer development builds keep that distributable archive
# uninstrumented so it has no hidden ASan/UBSan link dependency. CI may opt in
# to instrumenting the runtime too; generated programs already link sanitizer
# runtimes when the compiler build itself is sanitized.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND
   CMAKE_CXX_FLAGS MATCHES "sanitize" AND
   NOT QUIDRA_SANITIZE_RUNTIME)
  target_compile_options(quidra_runtime PRIVATE -fno-sanitize=all)
endif()
# The JIT runtime is loaded into an external LLVM runner process, not the
# sanitizer-instrumented Quidra compiler process. Keep it self-contained.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
  target_compile_options(quidra_runtime_jit PRIVATE -fno-sanitize=all)
  target_link_options(quidra_runtime_jit PRIVATE -fno-sanitize=all)
endif()

add_executable(quidra
    src/main.cpp
    src/repl_cli.cpp
    $<TARGET_OBJECTS:quidra_repl_submission>
    src/lsp_server.cpp
    src/package_cli.cpp
    src/device_cli.cpp
    src/device_backend.cpp
    src/runtime_counters.cpp
    src/runtime_parallel.cpp
    src/unified_storage.cpp
    src/native_build.cpp
    src/run_artifact.cpp
    src/run_cache.cpp
    src/jit.cpp
    src/platform/cache_files.cpp
    src/platform/executable.cpp
    src/platform/file_descriptor.cpp
    src/platform/file_identity.cpp
    src/platform/file_lock.cpp
    src/platform/file_names.cpp
    src/platform/native_text.cpp
    src/platform/os_version.cpp
    src/platform/private_directory.cpp
    src/platform/process.cpp
    src/platform/temporary_directory.cpp
    src/platform/user_cache_directory.cpp
    src/toolchain/archiver.cpp
    src/toolchain/compiler_identity.cpp
    src/toolchain/cuda_discovery.cpp
    src/toolchain/driver_output.cpp
    src/toolchain/installation_layout.cpp
    src/toolchain/link_flags.cpp
    src/toolchain/llvm_discovery.cpp
    src/toolchain/native_link_recipe.cpp
    src/toolchain/run_cache_eviction.cpp
    src/toolchain/run_cache_key.cpp
    src/toolchain/run_cache_metadata.cpp
    src/toolchain/run_cache_store.cpp
    src/toolchain/toolchain_identity.cpp
)
target_link_libraries(quidra PRIVATE quidra_core)
# The platform sources include their headers by directory: "platform/x.hpp".
target_include_directories(quidra PRIVATE src)
if(WIN32)
  # The platform sources include <windows.h> without its min and max macros.
  set_source_files_properties(
    src/platform/cache_files.cpp
    src/platform/executable.cpp src/platform/file_identity.cpp src/platform/file_lock.cpp
    src/platform/native_text.cpp src/platform/os_version.cpp
    src/platform/private_directory.cpp src/platform/process.cpp
    src/platform/user_cache_directory.cpp
    PROPERTIES COMPILE_DEFINITIONS NOMINMAX)
  # The user cache directory (the Local AppData known folder) and the private
  # directories' access control lists.
  target_link_libraries(quidra PRIVATE advapi32 ole32 shell32)
endif()
if(CMAKE_DL_LIBS)
  target_link_libraries(quidra PRIVATE ${CMAKE_DL_LIBS})
endif()
if(APPLE)
  target_link_libraries(quidra PRIVATE "-framework Foundation" "-framework Metal")
endif()
if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
  target_compile_definitions(quidra PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
endif()
add_dependencies(quidra quidra_runtime quidra_runtime_jit)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
  # A sanitizer development compiler also instruments the native programs it
  # produces. The runtime archive remains distributable and uninstrumented.
  target_compile_definitions(quidra PRIVATE QUIDRA_SANITIZE_GENERATED=1)
endif()

endif() # NOT QUIDRA_BUILD_WASM_FRONTEND

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(quidra_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(quidra_repl_submission PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(quidra_core PRIVATE /W4 /wd4456 /bigobj)
  target_compile_options(quidra_repl_submission PRIVATE /W4)
endif()

if(QUIDRA_BUILD_WASM_FRONTEND)
  # The bridge reports the exact revision it was built from so that a consumer
  # never has to guess which compiler produced a result. The version itself
  # still comes from project.toml by way of PROJECT_VERSION.
  set(QUIDRA_CORE_COMMIT "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE QUIDRA_CORE_COMMIT_FOUND
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(QUIDRA_CORE_COMMIT_FOUND)
      set(QUIDRA_CORE_COMMIT "${QUIDRA_CORE_COMMIT_FOUND}")
    endif()
  endif()
  if(DEFINED ENV{QUIDRA_CORE_COMMIT} AND NOT "$ENV{QUIDRA_CORE_COMMIT}" STREQUAL "")
    set(QUIDRA_CORE_COMMIT "$ENV{QUIDRA_CORE_COMMIT}")
  endif()
  message(STATUS "Quidra WebAssembly frontend: core commit ${QUIDRA_CORE_COMMIT}")

  add_executable(quidra_wasm src/wasm_api.cpp)
  target_link_libraries(quidra_wasm PRIVATE quidra_core)
  target_compile_definitions(quidra_wasm PRIVATE
      QUIDRA_CORE_COMMIT="${QUIDRA_CORE_COMMIT}")
  set_target_properties(quidra_wasm PROPERTIES OUTPUT_NAME "quidra-core")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(quidra_wasm PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(EMSCRIPTEN)
    # Native Wasm exception handling: the frontend throws to report ordinary
    # diagnostics, so the happy path must stay cheap. Compile and link both.
    target_compile_options(quidra_core PUBLIC -fwasm-exceptions)
    target_compile_options(quidra_wasm PRIVATE -fwasm-exceptions)
    # The checker and IR lowering recurse over the AST. Their depth budgets in
    # src/nesting_budget.hpp are sized against a multi-megabyte stack, so the
    # 64 KiB Emscripten default has to be raised for those budgets to be the
    # thing that stops a deep program. INITIAL_MEMORY must exceed STACK_SIZE.
    target_link_options(quidra_wasm PRIVATE
        -fwasm-exceptions
        -sSTACK_SIZE=16MB
        -sINITIAL_MEMORY=64MB
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_OVERFLOW_CHECK=1
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createQuidraCore
        -sENVIRONMENT=web,worker,node
        -sTEXTDECODER=2
        # FILESYSTEM stays on: quidra::check resolves standard namespaces
        # through the module loader, which calls std::filesystem::current_path.
        # Emscripten's default MEMFS satisfies that for a few tens of KiB; a
        # hand-written shim would be a second source of truth for no gain.
        -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToNewUTF8
        -sEXPORTED_FUNCTIONS=_quidra_wasm_invoke,_quidra_wasm_free,_malloc,_free
        --no-entry)
  endif()
endif()

if(QUIDRA_BUILD_WASM_FRONTEND AND QUIDRA_BUILD_WASM_GOLDEN)
  add_executable(quidra_golden_dump_wasm
      tests/golden/golden_dump.cpp
      tests/golden/ir_full.cpp
      tests/golden/census.cpp
      tests/golden/census_keys.cpp
      tests/golden/failure.cpp
      tests/golden/repl_session.cpp
      $<TARGET_OBJECTS:quidra_repl_submission>
  )
  target_include_directories(quidra_golden_dump_wasm PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/golden)
  target_link_libraries(quidra_golden_dump_wasm PRIVATE quidra_core)
  if(EMSCRIPTEN)
    target_compile_options(quidra_golden_dump_wasm PRIVATE -fwasm-exceptions)
    target_link_options(quidra_golden_dump_wasm PRIVATE
        -fwasm-exceptions
        -sNODERAWFS=1
        -sENVIRONMENT=node
        -sEXIT_RUNTIME=1
        -sSTACK_SIZE=16MB
        -sINITIAL_MEMORY=64MB
        -sALLOW_MEMORY_GROWTH=1)
  endif()
endif()

if(NOT QUIDRA_BUILD_WASM_FRONTEND)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(quidra_runtime PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_compile_options(quidra_runtime_jit PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_compile_options(quidra PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(quidra_runtime PRIVATE /W4 /Gy /Gw)
  target_compile_options(quidra_runtime_jit PRIVATE /W4 /Gy /Gw)
  target_compile_options(quidra PRIVATE /W4)
  # Windows reserves only 1 MiB of stack for executables by default. Compiler
  # lowering legitimately walks nested typed AST/IR structures that fit in the
  # larger defaults on Linux/macOS; reserve the same practical headroom here.
  target_link_options(quidra PRIVATE /STACK:8388608)
endif()

install(TARGETS quidra RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS quidra_runtime ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/quidra)
install(TARGETS quidra_runtime_jit
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/quidra)
install(FILES include/quidra/native_extension.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quidra)
install(FILES README.md LICENSE quidra.manifest.json DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/spec/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/spec)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples)

endif() # NOT QUIDRA_BUILD_WASM_FRONTEND

include(CTest)
if(BUILD_TESTING AND NOT QUIDRA_BUILD_WASM_FRONTEND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(quidra_tests tests/compiler_tests.cpp)
  target_link_libraries(quidra_tests PRIVATE quidra_core)
  # platform/environment.hpp for the golden corpus capture hook.
  target_include_directories(quidra_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  if(MSVC)
    # Compiler tests execute the same recursive lowering as the CLI and have
    # grown beyond COFF's default section limit on MSVC.
    target_compile_options(quidra_tests PRIVATE /bigobj)
    target_link_options(quidra_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME quidra_tests COMMAND quidra_tests)

  # The refactoring equivalence harness (tests/golden, driven by
  # tests/ir_golden.sh). The dump tool prints every observable compiler output
  # of one input; it links the compiler core with src/ on its private include
  # path.
  add_library(quidra_golden_support STATIC
      tests/golden/ir_full.cpp
      tests/golden/census.cpp
      tests/golden/census_keys.cpp
      tests/golden/failure.cpp
      tests/golden/repl_session.cpp
      $<TARGET_OBJECTS:quidra_repl_submission>
  )
  target_link_libraries(quidra_golden_support PUBLIC quidra_core)
  target_include_directories(quidra_golden_support
      PUBLIC ${CMAKE_SOURCE_DIR}/tests/golden
      PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_executable(quidra_golden_dump tests/golden/golden_dump.cpp)
  target_link_libraries(quidra_golden_dump PRIVATE quidra_golden_support)
  target_include_directories(quidra_golden_dump PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
  if(APPLE)
    # Gate Gobj: records the Metal source strings a program compiles at run
    # time (DYLD_INSERT_LIBRARIES; tests/golden/metal_source_log).
    add_library(quidra_metal_source_log SHARED
        tests/golden/metal_source_log/metal_source_log.mm)
    target_include_directories(quidra_metal_source_log PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(quidra_metal_source_log PRIVATE
        "-framework Foundation" "-framework Metal" objc)
  endif()
  add_executable(quidra_ir_full_tests tests/golden/ir_full_tests.cpp)
  target_link_libraries(quidra_ir_full_tests PRIVATE quidra_golden_support)
  add_test(NAME quidra_ir_full_tests COMMAND quidra_ir_full_tests)
  # The expected-difference rules of the golden comparison
  # (tests/golden/expect.py), on synthetic captures, and the column map of a
  # synthetic rewrite captured with the golden tool.
  add_test(NAME quidra_golden_expect
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/golden/expect_tests.py
                   --tool $<TARGET_FILE:quidra_golden_dump>)
  # The census keys and counts (tests/golden/census_keys.hpp) on small
  # programs captured with the golden tool.
  add_test(NAME quidra_golden_census
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/golden/census_tests.py
                   --tool $<TARGET_FILE:quidra_golden_dump>)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(quidra_golden_support PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(quidra_golden_dump PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(quidra_ir_full_tests PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    # One template instantiation per IR struct.
    target_compile_options(quidra_golden_support PRIVATE /W4 /bigobj)
    target_compile_options(quidra_golden_dump PRIVATE /W4 /bigobj)
    target_compile_options(quidra_ir_full_tests PRIVATE /W4 /bigobj)
  endif()

  add_executable(quidra_import_path_tests tests/import_path_tests.cpp)
  target_link_libraries(quidra_import_path_tests PRIVATE quidra_core)
  add_test(NAME quidra_import_path_tests COMMAND quidra_import_path_tests)

  add_executable(quidra_parser_tests tests/parser_tests.cpp)
  target_link_libraries(quidra_parser_tests PRIVATE quidra_core)
  add_test(NAME quidra_parser_tests COMMAND quidra_parser_tests)

  add_executable(quidra_type_policy_tests tests/type_policy_tests.cpp)
  target_link_libraries(quidra_type_policy_tests PRIVATE quidra_core)
  add_test(NAME quidra_type_policy_tests COMMAND quidra_type_policy_tests)

  add_executable(quidra_package_manifest_tests tests/package_manifest_tests.cpp)
  target_link_libraries(quidra_package_manifest_tests PRIVATE quidra_core)
  add_test(NAME quidra_package_manifest_tests COMMAND quidra_package_manifest_tests)

  add_executable(quidra_package_lock_tests tests/package_lock_tests.cpp)
  target_link_libraries(quidra_package_lock_tests PRIVATE quidra_core)
  add_test(NAME quidra_package_lock_tests COMMAND quidra_package_lock_tests)

  add_executable(quidra_runtime_string_tests tests/runtime_string_tests.cpp)
  target_link_libraries(quidra_runtime_string_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_string_tests COMMAND quidra_runtime_string_tests)

  # The runtime failure table: codes, reasons and their message templates.
  add_executable(quidra_runtime_failure_tests tests/runtime_failure_tests.cpp
      src/runtime_location.cpp src/platform/sha256.cpp)
  target_include_directories(quidra_runtime_failure_tests PRIVATE ${CMAKE_SOURCE_DIR}/include
                             ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_runtime_failure_tests COMMAND quidra_runtime_failure_tests)

  # Stable 56-byte error value ABI, typed detail schemas and reserved codes.
  add_executable(quidra_error_value_schema_tests tests/error_value_schema_tests.cpp)
  target_include_directories(quidra_error_value_schema_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_error_value_schema_tests COMMAND quidra_error_value_schema_tests)

  # Shared inclusive/exclusive slice planning independent of a container backend.
  add_executable(quidra_slice_bounds_tests tests/slice_bounds_tests.cpp)
  target_include_directories(quidra_slice_bounds_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_slice_bounds_tests COMMAND quidra_slice_bounds_tests)

  add_executable(quidra_runtime_parallel_tests tests/runtime_parallel_tests.cpp)
  target_include_directories(quidra_runtime_parallel_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_parallel_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_parallel_tests COMMAND quidra_runtime_parallel_tests)

  add_executable(quidra_platform_environment_tests tests/platform_environment_tests.cpp)
  target_include_directories(quidra_platform_environment_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_platform_environment_tests COMMAND quidra_platform_environment_tests)

  # The user cache directory, private directories and files, file identity
  # and the OS version.
  add_executable(quidra_platform_cache_directory_tests
      tests/platform_cache_directory_tests.cpp
      src/platform/file_descriptor.cpp
      src/platform/file_identity.cpp
      src/platform/os_version.cpp
      src/platform/private_directory.cpp
      src/platform/user_cache_directory.cpp)
  target_include_directories(quidra_platform_cache_directory_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  if(WIN32)
    target_compile_definitions(quidra_platform_cache_directory_tests PRIVATE NOMINMAX)
    target_link_libraries(quidra_platform_cache_directory_tests PRIVATE advapi32 ole32 shell32)
  endif()
  add_test(NAME quidra_platform_cache_directory_tests COMMAND quidra_platform_cache_directory_tests)

  add_executable(quidra_platform_strict_json_tests tests/platform_strict_json_tests.cpp)
  target_link_libraries(quidra_platform_strict_json_tests PRIVATE quidra_core)
  target_include_directories(quidra_platform_strict_json_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_platform_strict_json_tests COMMAND quidra_platform_strict_json_tests)

  add_executable(quidra_platform_sha256_tests tests/platform_sha256_tests.cpp)
  target_link_libraries(quidra_platform_sha256_tests PRIVATE quidra_core)
  target_include_directories(quidra_platform_sha256_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_platform_sha256_tests COMMAND quidra_platform_sha256_tests)

  # The native link recipe with the host toolchain: build and run modes, the
  # template, the run-mode recording, and byte-identical executables.
  add_executable(quidra_native_link_recipe_tests
      tests/native_link_recipe_tests.cpp
      src/toolchain/native_link_recipe.cpp
      src/toolchain/link_flags.cpp
      src/toolchain/cuda_discovery.cpp
      src/toolchain/installation_layout.cpp
      src/toolchain/llvm_discovery.cpp
      src/platform/executable.cpp
      src/platform/file_descriptor.cpp
      src/platform/file_names.cpp
      src/platform/native_text.cpp
      src/platform/process.cpp
      src/platform/temporary_directory.cpp)
  target_include_directories(quidra_native_link_recipe_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  add_dependencies(quidra_native_link_recipe_tests quidra_runtime)
  add_test(NAME quidra_native_link_recipe_tests COMMAND quidra_native_link_recipe_tests)
  set_tests_properties(quidra_native_link_recipe_tests PROPERTIES
      ENVIRONMENT "QUIDRA_RUNTIME_LIBRARY=$<TARGET_FILE:quidra_runtime>")

  # The toolchain and compiler identity of cached runs: the parsers of what
  # the toolchain prints, snapshots with fake and host drivers, dependency
  # records, and the compiler build id with its memo.
  add_executable(quidra_toolchain_identity_tests
      tests/toolchain_identity_tests.cpp
      src/toolchain/compiler_identity.cpp
      src/toolchain/cuda_discovery.cpp
      src/toolchain/driver_output.cpp
      src/toolchain/installation_layout.cpp
      src/toolchain/llvm_discovery.cpp
      src/toolchain/native_link_recipe.cpp
      src/toolchain/link_flags.cpp
      src/toolchain/toolchain_identity.cpp
      src/platform/executable.cpp
      src/platform/file_descriptor.cpp
      src/platform/file_identity.cpp
      src/platform/file_names.cpp
      src/platform/native_text.cpp
      src/platform/private_directory.cpp
      src/platform/process.cpp
      src/platform/temporary_directory.cpp)
  target_link_libraries(quidra_toolchain_identity_tests PRIVATE quidra_core)
  target_include_directories(quidra_toolchain_identity_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  if(WIN32)
    target_compile_definitions(quidra_toolchain_identity_tests PRIVATE NOMINMAX)
    target_link_libraries(quidra_toolchain_identity_tests PRIVATE advapi32)
  endif()
  add_dependencies(quidra_toolchain_identity_tests quidra_runtime)
  add_test(NAME quidra_toolchain_identity_tests COMMAND quidra_toolchain_identity_tests)
  set_tests_properties(quidra_toolchain_identity_tests PROPERTIES
      ENVIRONMENT "QUIDRA_RUNTIME_LIBRARY=$<TARGET_FILE:quidra_runtime>")

  # The run cache below the CLI: keys, metadata and index files, the checks
  # of a hit's records, the store's atomic publish, eviction and cleaning.
  add_executable(quidra_run_cache_unit_tests
      tests/run_cache_unit_tests.cpp
      src/toolchain/run_cache_eviction.cpp
      src/toolchain/run_cache_key.cpp
      src/toolchain/run_cache_metadata.cpp
      src/toolchain/run_cache_store.cpp
      src/platform/cache_files.cpp
      src/platform/file_descriptor.cpp
      src/platform/file_identity.cpp
      src/platform/file_lock.cpp
      src/platform/file_names.cpp
      src/platform/private_directory.cpp)
  target_link_libraries(quidra_run_cache_unit_tests PRIVATE quidra_core)
  target_include_directories(quidra_run_cache_unit_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  if(WIN32)
    target_compile_definitions(quidra_run_cache_unit_tests PRIVATE NOMINMAX)
    target_link_libraries(quidra_run_cache_unit_tests PRIVATE advapi32)
  endif()
  add_test(NAME quidra_run_cache_unit_tests COMMAND quidra_run_cache_unit_tests)

  add_executable(quidra_tensor_view_bounds_tests tests/tensor_view_bounds_tests.cpp)
  target_include_directories(quidra_tensor_view_bounds_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_tensor_view_bounds_tests COMMAND quidra_tensor_view_bounds_tests)

  # Correct rounding of decimal text to the IEEE formats.
  add_executable(quidra_numeric_formats_tests tests/numeric_formats_tests.cpp)
  target_include_directories(quidra_numeric_formats_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(quidra_numeric_formats_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_numeric_formats_tests COMMAND quidra_numeric_formats_tests)

  # The small and the general form of the exact type `real` agree.
  add_executable(quidra_exact_real_tests tests/exact_real_tests.cpp)
  target_include_directories(quidra_exact_real_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(quidra_exact_real_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_exact_real_tests COMMAND quidra_exact_real_tests)

  # Arbitrary-precision integer words agree with the boxed big integers.
  add_executable(quidra_runtime_integer_tests tests/runtime_integer_tests.cpp)
  target_include_directories(quidra_runtime_integer_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(quidra_runtime_integer_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_integer_tests COMMAND quidra_runtime_integer_tests)

  # The exact text of every operation of the LLVM text layer.
  add_executable(quidra_llvm_text_tests tests/llvm_text_tests.cpp)
  target_link_libraries(quidra_llvm_text_tests PRIVATE quidra_core)
  target_include_directories(quidra_llvm_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_llvm_text_tests COMMAND quidra_llvm_text_tests)

  # Gate Gprel: the runtime prelude occurs once, verbatim, in the LLVM that
  # the compiler emits for a minimal program.
  add_executable(quidra_llvm_backend_tests tests/llvm_backend_tests.cpp)
  target_link_libraries(quidra_llvm_backend_tests PRIVATE quidra_core)
  target_include_directories(quidra_llvm_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_llvm_backend_tests
           COMMAND quidra_llvm_backend_tests ${CMAKE_SOURCE_DIR}/tests/golden/probes/minimal.qui)
  # Every runtime entry point the prelude declares is defined by the runtime
  # library (nm; not on Windows, where the library is a .lib).
  if(NOT WIN32)
    add_test(NAME quidra_runtime_abi_symbols
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_abi_symbols.py
                     $<TARGET_FILE:quidra_llvm_backend_tests> $<TARGET_FILE:quidra_runtime>)
  endif()

  add_test(
    NAME quidra_process_portability
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/process_portability_tests.py
            $<TARGET_FILE:quidra>
  )

  if(UNIX)
    add_test(NAME quidra_cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    # The run cache end to end: changed inputs, damaged entries, permissions,
    # concurrent runs, the replayed toolchain output, the cached program
    # against quidra build's.
    add_test(NAME quidra_run_cache COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_cache_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    # Gate Gdiff: programs built with and without an internal lowering
    # switch behave the same (tests/lowering_switch_tests.sh).
    # It links the way `quidra build` does (src/native_build.cpp and the
    # platform and toolchain sources it uses).
    add_executable(quidra_lowering_switch_build
        tests/lowering_switch_build.cpp
        src/native_build.cpp
        src/platform/executable.cpp
        src/platform/file_descriptor.cpp
        src/platform/file_names.cpp
        src/platform/native_text.cpp
        src/platform/process.cpp
        src/platform/temporary_directory.cpp
        src/toolchain/cuda_discovery.cpp
        src/toolchain/installation_layout.cpp
        src/toolchain/llvm_discovery.cpp
        src/toolchain/native_link_recipe.cpp
        src/toolchain/link_flags.cpp
        src/toolchain/archiver.cpp
    )
    target_link_libraries(quidra_lowering_switch_build PRIVATE quidra_core ${CMAKE_DL_LIBS})
    target_include_directories(quidra_lowering_switch_build PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
      target_compile_definitions(quidra_lowering_switch_build PRIVATE QUIDRA_SANITIZE_GENERATED=1)
    endif()
    add_dependencies(quidra_lowering_switch_build quidra_runtime)
    add_test(NAME quidra_lowering_switches
             COMMAND bash ${CMAKE_SOURCE_DIR}/tests/lowering_switch_tests.sh
                     $<TARGET_FILE:quidra_lowering_switch_build> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_package_names COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_name_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_identity COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_identity_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_native COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_native_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_location COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_location_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_diagnostics COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_diagnostics_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_error_json COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_error_json_tests.sh $<TARGET_FILE:quidra>)
    # Exported functions called from C (export "C"): values, reentrancy,
    # concurrent host threads, failures at the boundary.
    add_test(NAME quidra_c_export COMMAND bash ${CMAKE_SOURCE_DIR}/tests/c_export_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_releases COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_release_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_metadata_sync COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_metadata_sync_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_stdlib COMMAND bash ${CMAKE_SOURCE_DIR}/tests/stdlib_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_nat_lengths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/nat_length_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_container_casts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/container_cast_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_tensor_autograd_foundation COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tensor_autograd_foundation_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_tensor_view_fast_paths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tensor_view_fast_path_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_cpu_parallel COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cpu_parallel_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_unified_memory COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unified_memory_tests.sh $<TARGET_FILE:quidra>)
    if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
      add_executable(quidra_device_backend_tests tests/device_backend_tests.cpp)
      target_include_directories(quidra_device_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
      target_compile_definitions(quidra_device_backend_tests PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
      target_link_libraries(quidra_device_backend_tests PRIVATE quidra_runtime)
      add_test(NAME quidra_device_backend_tests COMMAND quidra_device_backend_tests)
      add_test(NAME quidra_gpu_contracts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_contract_tests.sh $<TARGET_FILE:quidra>)
      add_test(NAME quidra_gpu_contracts_autograd COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_autograd_contract_tests.sh $<TARGET_FILE:quidra>)
      add_test(NAME quidra_runtime_counters COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_counters.sh $<TARGET_FILE:quidra>)
    endif()
    add_test(NAME quidra_real_gpu_integration COMMAND bash ${CMAKE_SOURCE_DIR}/tests/real_gpu_integration.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_native_smoke COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/native-smoke.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_performance_regressions COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/performance_regressions.py $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    find_program(QUIDRA_LLVM_OPT NAMES opt opt-20 opt-19 opt-18 opt-17 opt-16 opt-15)
    if(QUIDRA_LLVM_OPT)
      add_test(
        NAME quidra_backend_regressions
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/backend_regressions.sh
                $<TARGET_FILE:quidra> ${QUIDRA_LLVM_OPT}
      )
    endif()
  endif()
  add_test(NAME quidra_documentation_examples COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/documentation_examples.py $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
  add_test(NAME quidra_manifest_registry COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/manifest_registry_tests.py $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
  add_test(NAME quidra_metadata_ssot COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/metadata_ssot_tests.py)
  # The glue rules of the refactoring move gate (Gm), pinned by probes.
  add_test(NAME quidra_refactor_move_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/refactor/check_moves.py --self-test)
  add_test(
    NAME quidra_generic_specialization_benchmark_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/generic_specialization_scaling.py
            $<TARGET_FILE:quidra> --sizes 2,4 --repeats 1
  )

  # A parser/compiler/runtime regression must fail rather than leave a CI host
  # waiting indefinitely. Sanitizer builds intentionally instrument both the
  # compiler and generated programs, so their end-to-end CLI tests need a
  # wider timeout without weakening the normal hosted-runner guard.
  set(QUIDRA_TEST_TIMEOUT 90)
  if(CMAKE_CXX_FLAGS MATCHES "sanitize")
    set(QUIDRA_TEST_TIMEOUT 240)
  endif()
  set_tests_properties(
    quidra_tests
    quidra_ir_full_tests
    quidra_import_path_tests
    quidra_parser_tests
    quidra_type_policy_tests
    quidra_package_manifest_tests
    quidra_package_lock_tests
    quidra_runtime_string_tests
    quidra_runtime_parallel_tests
    quidra_platform_environment_tests
    quidra_platform_cache_directory_tests
    quidra_platform_sha256_tests
    quidra_platform_strict_json_tests
    quidra_toolchain_identity_tests
    quidra_run_cache_unit_tests
    quidra_tensor_view_bounds_tests
    quidra_numeric_formats_tests
    quidra_exact_real_tests
    quidra_runtime_integer_tests
    quidra_process_portability
    quidra_documentation_examples
    quidra_manifest_registry
    quidra_refactor_move_check
    quidra_golden_expect
    quidra_golden_census
    quidra_generic_specialization_benchmark_smoke
    PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
  )
  if(UNIX)
    set_tests_properties(
      quidra_package_names
      quidra_package_identity
      quidra_package_native
      quidra_package_releases
      quidra_package_metadata_sync
      quidra_tensor_autograd_foundation
      quidra_tensor_view_fast_paths
      quidra_cpu_parallel
      quidra_native_smoke
      quidra_nat_lengths
      PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
    )
    # The CLI and stdlib suites compile and run many programs each, so they
    # get twice the unit suites' timeout; a hang still fails the run.
    math(EXPR QUIDRA_PROGRAM_SUITE_TIMEOUT "${QUIDRA_TEST_TIMEOUT} * 2")
    set_tests_properties(
      quidra_cli
      quidra_stdlib
      PROPERTIES TIMEOUT ${QUIDRA_PROGRAM_SUITE_TIMEOUT}
    )
    # Builds a few hundred small programs, a few of them in parallel.
    math(EXPR QUIDRA_RUN_CACHE_TEST_TIMEOUT "${QUIDRA_TEST_TIMEOUT} * 6")
    set_tests_properties(
      quidra_run_cache
      PROPERTIES TIMEOUT ${QUIDRA_RUN_CACHE_TEST_TIMEOUT}
    )
    if(TEST quidra_gpu_contracts)
      set_tests_properties(
        quidra_gpu_contracts
        quidra_gpu_contracts_autograd
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
      )
    endif()
    if(TEST quidra_device_backend_tests)
      set_tests_properties(
        quidra_device_backend_tests
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
      )
    endif()
    if(TEST quidra_real_gpu_integration)
      # Builds and runs well over a hundred GPU programs, each of which
      # starts the device, so it gets ten thirds of the unit suites'
      # timeout; a hang still fails the run.
      math(EXPR QUIDRA_REAL_GPU_TEST_TIMEOUT "${QUIDRA_TEST_TIMEOUT} * 10 / 3")
      set_tests_properties(
        quidra_real_gpu_integration
        PROPERTIES TIMEOUT ${QUIDRA_REAL_GPU_TEST_TIMEOUT}
      )
    endif()
    if(TEST quidra_unified_memory)
      set_tests_properties(
        quidra_unified_memory
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
      )
    endif()
    if(TEST quidra_backend_regressions)
      set_tests_properties(
        quidra_backend_regressions
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
      )
    endif()
    # These tests contain timing-sensitive checks. Keep the rest of the suite
    # parallel, but isolate these so runner contention cannot create false failures.
    if(TEST quidra_cli)
      set_tests_properties(quidra_cli PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(TEST quidra_performance_regressions)
      # Performance regressions intentionally run longer than ordinary unit tests,
      # but a generated-program hang must still release the CI runner.
      set_tests_properties(
        quidra_performance_regressions
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 600
      )
    endif()
  endif()
endif()

set(CPACK_PACKAGE_NAME "quidra")
set(CPACK_PACKAGE_VENDOR "Quidra")
set(CPACK_PACKAGE_CONTACT "Quidra project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native general-purpose programming language for maximum meaning per token")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})

if(WIN32)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_ARCHIVE_FILE_NAME "quidra-${PROJECT_VERSION}-windows-${CMAKE_SYSTEM_PROCESSOR}")
elseif(APPLE)
  set(CPACK_GENERATOR "TGZ")
  set(CPACK_ARCHIVE_FILE_NAME "quidra-${PROJECT_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR}")
else()
  set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "clang-15, llvm-15, git, curl, xz-utils, libcurl4-openssl-dev")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_ARCHIVE_FILE_NAME "quidra-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_GENERATOR "DEB;TGZ")
endif()
include(CPack)
