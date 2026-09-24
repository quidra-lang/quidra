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
quidra_project_value(language_version QUIDRA_LANGUAGE_VERSION)
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
# no third-party dependencies. Everything that pulls CURL/PNG/JPEG/TIFF/WebP/
# FFmpeg/Metal belongs to the generated-program runtime and the native CLI, so a
# frontend-only build simply omits those targets. Emscripten implies it.
option(QUIDRA_BUILD_WASM_FRONTEND
    "Build only the dependency-free compiler frontend and its WebAssembly bridge" OFF)
if(EMSCRIPTEN)
  set(QUIDRA_BUILD_WASM_FRONTEND ON)
endif()

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
    src/ir.cpp
    src/llvm_backend.cpp
    src/compiler.cpp
    src/frontend.cpp
    src/source_tools.cpp
    src/source_patch.cpp
    src/formatter.cpp
    src/package_manifest.cpp
    src/toml_subset.cpp
    src/package_lock.cpp
)
target_include_directories(quidra_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(NOT QUIDRA_BUILD_WASM_FRONTEND)

find_package(CURL REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
find_path(WEBP_INCLUDE_DIR NAMES webp/decode.h)
find_library(WEBP_LIBRARY NAMES webp libwebp)
if(NOT WEBP_INCLUDE_DIR OR NOT WEBP_LIBRARY)
  message(FATAL_ERROR "libwebp headers and library are required.")
endif()

find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h)
find_library(AVFORMAT_LIBRARY NAMES avformat)
find_library(AVCODEC_LIBRARY NAMES avcodec)
find_library(AVUTIL_LIBRARY NAMES avutil)
find_library(SWSCALE_LIBRARY NAMES swscale)
if(NOT FFMPEG_INCLUDE_DIR OR NOT AVFORMAT_LIBRARY OR NOT AVCODEC_LIBRARY OR
   NOT AVUTIL_LIBRARY OR NOT SWSCALE_LIBRARY)
  message(FATAL_ERROR "FFmpeg libavformat/libavcodec/libavutil/libswscale development files are required.")
endif()

add_library(quidra_runtime STATIC
    src/runtime.cpp
    src/runtime_exact.cpp
    src/runtime_io.cpp
    src/runtime_system.cpp
    src/runtime_process.cpp
    src/runtime_json.cpp
    src/runtime_http.cpp
    src/runtime_image.cpp
    src/runtime_video.cpp
    src/device_backend.cpp
)

# The REPL uses LLVM ORC through `lli`. Keep a shared runtime alongside the
# static archive so JIT submissions can resolve the exact same Quidra runtime
# ABI without invoking the platform linker for every executable submission.
add_library(quidra_runtime_jit SHARED
    src/runtime.cpp
    src/runtime_exact.cpp
    src/runtime_io.cpp
    src/runtime_system.cpp
    src/runtime_process.cpp
    src/runtime_json.cpp
    src/runtime_http.cpp
    src/runtime_image.cpp
    src/runtime_video.cpp
    src/device_backend.cpp
)
target_include_directories(quidra_runtime_jit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CURL_INCLUDE_DIRS} ${WEBP_INCLUDE_DIR} ${FFMPEG_INCLUDE_DIR})
target_link_libraries(quidra_runtime_jit PRIVATE
    CURL::libcurl PNG::PNG JPEG::JPEG TIFF::TIFF ${WEBP_LIBRARY}
    ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
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
target_include_directories(quidra_runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CURL_INCLUDE_DIRS} ${WEBP_INCLUDE_DIR} ${FFMPEG_INCLUDE_DIR})
target_link_libraries(quidra_runtime PRIVATE
    PNG::PNG JPEG::JPEG TIFF::TIFF ${WEBP_LIBRARY}
    ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
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
    src/lsp_server.cpp
    src/package_cli.cpp
    src/device_cli.cpp
    src/device_backend.cpp
    src/native_build.cpp
    src/run_artifact.cpp
    src/jit.cpp
)
target_link_libraries(quidra PRIVATE quidra_core)
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
elseif(MSVC)
  target_compile_options(quidra_core PRIVATE /W4 /wd4456 /bigobj)
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
install(FILES README.md LICENSE quidra.manifest.json DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/spec/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/spec)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples)

endif() # NOT QUIDRA_BUILD_WASM_FRONTEND

include(CTest)
if(BUILD_TESTING AND NOT QUIDRA_BUILD_WASM_FRONTEND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(quidra_tests tests/compiler_tests.cpp)
  target_link_libraries(quidra_tests PRIVATE quidra_core)
  if(MSVC)
    # Compiler tests execute the same recursive lowering as the CLI, so they
    # need the same Windows stack reserve instead of the 1 MiB executable default.
    target_link_options(quidra_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME quidra_tests COMMAND quidra_tests)

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

  add_test(
    NAME quidra_process_portability
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/process_portability_tests.py
            $<TARGET_FILE:quidra>
  )

  if(UNIX)
    add_test(NAME quidra_cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_package_names COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_name_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_identity COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_identity_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_releases COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_release_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_stdlib COMMAND bash ${CMAKE_SOURCE_DIR}/tests/stdlib_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_neural_foundation COMMAND bash ${CMAKE_SOURCE_DIR}/tests/neural_foundation_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_image_dtype COMMAND bash ${CMAKE_SOURCE_DIR}/tests/image_dtype_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_video COMMAND bash ${CMAKE_SOURCE_DIR}/tests/video_tests.sh $<TARGET_FILE:quidra>)
    if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
      add_executable(quidra_device_backend_tests tests/device_backend_tests.cpp)
      target_include_directories(quidra_device_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
      target_compile_definitions(quidra_device_backend_tests PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
      target_link_libraries(quidra_device_backend_tests PRIVATE quidra_runtime)
      add_test(NAME quidra_device_backend_tests COMMAND quidra_device_backend_tests)
      add_test(NAME quidra_gpu_contracts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_contract_tests.sh $<TARGET_FILE:quidra>)
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
    quidra_import_path_tests
    quidra_parser_tests
    quidra_type_policy_tests
    quidra_package_manifest_tests
    quidra_package_lock_tests
    quidra_runtime_string_tests
    quidra_process_portability
    quidra_documentation_examples
    quidra_manifest_registry
    quidra_generic_specialization_benchmark_smoke
    PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
  )
  if(UNIX)
    set_tests_properties(
      quidra_cli
      quidra_package_names
      quidra_package_identity
      quidra_package_releases
      quidra_stdlib
      quidra_neural_foundation
      quidra_image_dtype
      quidra_video
      quidra_native_smoke
      PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
    )
    if(TEST quidra_gpu_contracts)
      set_tests_properties(
        quidra_gpu_contracts
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
      set_tests_properties(
        quidra_real_gpu_integration
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
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "clang-15, llvm-15, git, curl, xz-utils, libcurl4-openssl-dev, libpng-dev, libjpeg-dev, libtiff-dev, libwebp-dev, libavformat-dev, libavcodec-dev, libavutil-dev, libswscale-dev")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_ARCHIVE_FILE_NAME "quidra-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_GENERATOR "DEB;TGZ")
endif()
include(CPack)
