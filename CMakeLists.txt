cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/quidra.manifest.json" QUIDRA_MANIFEST_JSON)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/grammar.ebnf" QUIDRA_GRAMMAR_EBNF)
string(JSON QUIDRA_PROJECT_VERSION GET "${QUIDRA_MANIFEST_JSON}" compiler_version)
project(quidra VERSION ${QUIDRA_PROJECT_VERSION} LANGUAGES CXX)

if(APPLE)
  enable_language(OBJCXX)
  set_source_files_properties(src/device_backend.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

# The manifest supplies both the project version and the text `quidra describe`
# prints, but file(READ) creates no dependency on its own: editing the manifest
# would otherwise leave a stale copy compiled in until the next reconfigure.
# The machine-readable grammar is embedded for the same reason.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
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

if(QUIDRA_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Werror)
  endif()
endif()

configure_file(
    include/quidra/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/quidra/version.hpp
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
    src/package_lock.cpp
)
target_include_directories(quidra_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)

find_package(CURL REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
find_path(WEBP_INCLUDE_DIR NAMES webp/decode.h)
find_library(WEBP_LIBRARY NAMES webp libwebp)
if(NOT WEBP_INCLUDE_DIR OR NOT WEBP_LIBRARY)
  message(FATAL_ERROR "libwebp headers and library are required.")
endif()

add_library(quidra_runtime STATIC
    src/runtime.cpp
    src/runtime_io.cpp
    src/runtime_system.cpp
    src/runtime_process.cpp
    src/runtime_json.cpp
    src/runtime_http.cpp
    src/runtime_image.cpp
    src/device_backend.cpp
)
target_include_directories(quidra_runtime PRIVATE ${CURL_INCLUDE_DIRS} ${WEBP_INCLUDE_DIR})
target_link_libraries(quidra_runtime PRIVATE PNG::PNG JPEG::JPEG TIFF::TIFF ${WEBP_LIBRARY})
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

add_executable(quidra
    src/main.cpp
    src/repl_cli.cpp
    src/lsp_server.cpp
    src/package_cli.cpp
    src/device_cli.cpp
    src/device_backend.cpp
    src/native_build.cpp
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
add_dependencies(quidra quidra_runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
  # A sanitizer development compiler also instruments the native programs it
  # produces. The runtime archive remains distributable and uninstrumented.
  target_compile_definitions(quidra PRIVATE QUIDRA_SANITIZE_GENERATED=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(quidra_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(quidra_runtime PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
  target_compile_options(quidra PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  # MSVC C4456 treats repeated condition-declaration names across an if/else-if
  # variant-matching chain as shadowing. The names are intentionally branch-local.
  target_compile_options(quidra_core PRIVATE /W4 /wd4456)
  target_compile_options(quidra_runtime PRIVATE /W4 /Gy /Gw)
  target_compile_options(quidra PRIVATE /W4)
endif()

install(TARGETS quidra RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS quidra_runtime ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/quidra)
install(FILES README.md LICENSE quidra.manifest.json DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/spec/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/spec)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(quidra_tests tests/compiler_tests.cpp)
  target_link_libraries(quidra_tests PRIVATE quidra_core)
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

  if(UNIX)
    add_test(NAME quidra_cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_package_names COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_name_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_releases COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_release_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_stdlib COMMAND bash ${CMAKE_SOURCE_DIR}/tests/stdlib_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_neural_foundation COMMAND bash ${CMAKE_SOURCE_DIR}/tests/neural_foundation_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_image_dtype COMMAND bash ${CMAKE_SOURCE_DIR}/tests/image_dtype_tests.sh $<TARGET_FILE:quidra>)
    if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
      add_test(NAME quidra_gpu_contracts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_contract_tests.sh $<TARGET_FILE:quidra>)
    endif()
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
  add_test(
    NAME quidra_generic_specialization_benchmark_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/benchmark/generic_specialization_scaling.py
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
    quidra_documentation_examples
    quidra_manifest_registry
    quidra_generic_specialization_benchmark_smoke
    PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
  )
  if(UNIX)
    set_tests_properties(
      quidra_cli
      quidra_package_names
      quidra_package_releases
      quidra_stdlib
      quidra_neural_foundation
      quidra_image_dtype
      quidra_native_smoke
      PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
    )
    if(TEST quidra_gpu_contracts)
      set_tests_properties(
        quidra_gpu_contracts
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
      )
    endif()
    if(TEST quidra_backend_regressions)
      set_tests_properties(
        quidra_backend_regressions
        PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
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
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "clang-15, libcurl4-openssl-dev, libpng-dev, libjpeg-dev, libtiff-dev, libwebp-dev")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_ARCHIVE_FILE_NAME "quidra-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_GENERATOR "DEB;TGZ")
endif()
include(CPack)
