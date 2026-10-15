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
# CI-only, default OFF: preserve normal optimized Windows Release builds.
option(QUIDRA_MSVC_CI_FAST_FRONTEND "Build MSVC compiler frontend with low optimization in CI" OFF)
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

if(MSVC)
  # Quidra's C-linkage parallel error callbacks may throw C++ exceptions.
  # /EHsc treats extern "C" as non-throwing; use /EHs instead.
  string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  # Bound concurrent cl.exe processes on Windows hosted runners.
  add_compile_options(/MP2)
endif()
if(WIN32)
  # Includes apply to independently compiled runtime test targets as well.
  add_compile_definitions(NOMINMAX)
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

set(QUIDRA_CORE_SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/class_initialization.cpp
    src/semantics/binding_entry.cpp
    src/checker.cpp
    src/parallel_for_check.cpp
    src/traversal_plan.cpp
    src/checker_shapes.cpp
    src/ir/analysis/call_graph.cpp
    src/ir/analysis/cfg.cpp
    src/ir/analysis/dominators.cpp
    src/ir/analysis/def_use.cpp
    src/ir/analysis/local_ssa.cpp
    src/ir/analysis/alias.cpp
    src/ir/analysis/liveness.cpp
    src/ir/analysis/analysis_cache.cpp
    src/ir/analysis/summary_facts.cpp
    src/ir/analysis/tensor_facts.cpp
    src/ir/analysis/loops.cpp
    src/ir/analysis/loop_invariants.cpp
    src/ir/analysis/object_provenance.cpp
    src/ir/analysis/binding_proof.cpp
    src/ir/analysis/object_bindings.cpp
    src/ir/analysis/step_regions.cpp
    src/ir/analysis/step_region_annotations.cpp
    src/ir/summaries.cpp
    src/ir/fact_sources.cpp
    src/ir/clone_contracts.cpp
    src/ir/binding_origins.cpp
    src/ir/inline_binding_origins.cpp
    src/ir/helper_binding_origins.cpp
    src/ir/check_binding_origins.cpp
    src/ir/facts/tensor.cpp
    src/ir/canonical_encoding.cpp
    src/ir/content_hash.cpp
    src/ir/facts/initialization.cpp
    src/ir/facts/shape_proof.cpp
    src/ir/analysis/tail_calls.cpp
    src/ir/analysis/device_reach.cpp
    src/ir/dump.cpp
    src/ir/instruction_text.cpp
    src/ir/visit.cpp
    src/ir/mutable_operands.cpp
    src/ir/verify.cpp
    src/ir/value_allocator.cpp
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
    src/lowering/parallel_for_lowering.cpp
    src/lowering/console_lowering.cpp
    src/lowering/traversal_lowering.cpp
    src/lowering/control_flow_lowering.cpp
    src/lowering/multi_array_loop_lowering.cpp
    src/lowering/real_loop_lowering.cpp
    src/lowering/exact_scaling.cpp
    src/lowering/discard_loop_lowering.cpp
    src/lowering/conversion_lowering.cpp
    src/lowering/enclosing_class.cpp
    src/lowering/error_lowering.cpp
    src/lowering/expression_lowering.cpp
    src/lowering/file_lowering.cpp
    src/lowering/function_lowering.cpp
    src/lowering/http_lowering.cpp
    src/lowering/initialization_flags.cpp
    src/lowering/json_lowering.cpp
    src/lowering/lifetime_lowering.cpp
    src/lowering/lower_module.cpp
    src/lowering/binding_origins.cpp
    src/lowering/lowerer.cpp
    src/lowering/operator_lowering.cpp
    src/lowering/random_lowering.cpp
    src/lowering/reflection_lowering.cpp
    src/lowering/scan_lowering.cpp
    src/lowering/shape_constraint_lowering.cpp
    src/lowering/source_locator.cpp
    src/lowering/statement_lowering.cpp
    src/lowering/statement_sites.cpp
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
    src/semantics/interface_contracts.cpp
    src/semantics/interface_fingerprint.cpp
    src/semantics/parallel_effects.cpp
    src/semantics/mutation_markers.cpp
    src/semantics/tensor_extents.cpp
    src/semantics/storage_path.cpp
    src/optimizer/call_contract.cpp
    src/optimizer/chain_fusion.cpp
    src/optimizer/conditional_rules.cpp
    src/optimizer/declaration_binding.cpp
    src/optimizer/execution_policy.cpp
    src/optimizer/finite_check_merging.cpp
    src/optimizer/ownership_adaptation.cpp
    src/optimizer/pass_manager.cpp
    src/optimizer/region_candidates.cpp
    src/optimizer/region_formation.cpp
    src/optimizer/rule_registry.cpp
    src/optimizer/rule_dispatch.cpp
    src/optimizer/helper_specialization.cpp
    src/optimizer/inlining.cpp
    src/optimizer/check_elimination.cpp
    src/optimizer/foundation_passes.cpp
    src/optimizer/static_tensor_facts.cpp
    src/optimizer/textual_uses.cpp
    src/llvm_backend/aggregate_emitter.cpp
    src/llvm_backend/array_cast_helpers.cpp
    src/llvm_backend/autograd_emitter.cpp
    src/llvm_backend/bare_integer.cpp
    src/llvm_backend/bin_emitter.cpp
    src/llvm_backend/call_emitter.cpp
    src/llvm_backend/call_acceptance_emitter.cpp
    src/llvm_backend/owned_shadow_emitter.cpp
    src/llvm_backend/node_value_ownership.cpp
    src/llvm_backend/call_site_metadata.cpp
    src/llvm_backend/check_emitter.cpp
    src/llvm_backend/concurrency_emitter.cpp
    src/llvm_backend/parallel_node_emission.cpp
    src/llvm_backend/console_emitter.cpp
    src/llvm_backend/constant_emitter.cpp
    src/llvm_backend/control_flow_emitter.cpp
    src/llvm_backend/conversion_emitter.cpp
    src/llvm_backend/debug_emitter.cpp
    src/llvm_backend/debug_module.cpp
    src/llvm_backend/debug_records.cpp
    src/llvm_backend/diagnostic_constants.cpp
    src/llvm_backend/error_result_emitter.cpp
    src/llvm_backend/error_value_emitter.cpp
    src/llvm_backend/fail_fast_emitter.cpp
    src/llvm_backend/half_precision.cpp
    src/llvm_backend/finite_check_support.cpp
    src/llvm_backend/complex_arithmetic.cpp
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
    src/llvm_backend/shape_variable_check.cpp
    src/llvm_backend/small_rational.cpp
    src/llvm_backend/real_power.cpp
    src/llvm_backend/statement_attribution.cpp
    src/llvm_backend/storage_layout.cpp
    src/llvm_backend/string_pool.cpp
    src/llvm_backend/symbol_resolver.cpp
    src/llvm_backend/system_emitter.cpp
    src/llvm_backend/tensor_emitter.cpp
    src/llvm_backend/tail_record_emitter.cpp
    src/llvm_backend/tail_continuations.cpp
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
    src/compiler_extension.cpp
    src/compiler_extension_expression.cpp
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

# Windows CI generates roughly 10 GiB of MSVC objects. lib.exe cannot pack
# those into one COFF archive (LNK1248), so divide only the CI frontend into
# small archives while keeping quidra_core a real STATIC target for consumers.
if(MSVC AND QUIDRA_MSVC_CI_FAST_FRONTEND)
  list(POP_FRONT QUIDRA_CORE_SOURCES _quidra_core_entry_source)
  add_library(quidra_core STATIC ${_quidra_core_entry_source})
  set(_quidra_core_part_index 0)
  # Eight objects per archive stay below 4 GiB even at the observed
  # ~450 MiB worst-case per translation unit.
  while(QUIDRA_CORE_SOURCES)
    list(LENGTH QUIDRA_CORE_SOURCES _quidra_core_remaining)
    if(_quidra_core_remaining GREATER 8)
      list(SUBLIST QUIDRA_CORE_SOURCES 0 8 _quidra_core_part_sources)
      list(SUBLIST QUIDRA_CORE_SOURCES 8 -1 QUIDRA_CORE_SOURCES)
    else()
      set(_quidra_core_part_sources ${QUIDRA_CORE_SOURCES})
      set(QUIDRA_CORE_SOURCES "")
    endif()
    set(_quidra_core_part quidra_core_part_${_quidra_core_part_index})
    add_library(${_quidra_core_part} STATIC ${_quidra_core_part_sources})
    target_include_directories(${_quidra_core_part}
      PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated src)
    # /O1 emits repeated C4702 diagnostics from std::visit instantiations.
    # Keep /WX, /W4 and every other warning active.
    target_compile_options(${_quidra_core_part} PRIVATE
      /W4 /wd4456 /wd4702 /bigobj /Bt
      "$<$<CONFIG:Release>:/O1>" "$<$<CONFIG:Release>:/Ob0>")
    target_link_libraries(quidra_core PUBLIC ${_quidra_core_part})
    math(EXPR _quidra_core_part_index "${_quidra_core_part_index} + 1")
  endwhile()
else()
  add_library(quidra_core ${QUIDRA_CORE_SOURCES})
endif()
target_include_directories(quidra_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The compiler's internal layers (src/llvm_backend, src/llvm_text, ...) include
# each other by their directory: "llvm_backend/x.hpp".
target_include_directories(quidra_core PRIVATE src)
if(MSVC AND QUIDRA_MSVC_CI_FAST_FRONTEND)
  # The entry source uses the same CI-only optimization as the subarchives.
  # Normal Windows Release builds continue to use /O2.
  target_compile_options(quidra_core PRIVATE
    "$<$<CONFIG:Release>:/O1>" "$<$<CONFIG:Release>:/Ob0>" /wd4702 /Bt)
endif()
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
    src/runtime_exact.cpp
    src/runtime_io.cpp
    src/runtime_system.cpp
    src/runtime_complex.cpp
    src/runtime_location.cpp
    src/runtime_trace.cpp
    src/runtime_tail.cpp
    src/runtime_tail_path.cpp
    src/runtime_process.cpp
    src/runtime_json.cpp
    src/runtime_http.cpp
    src/runtime_error.cpp
    src/platform/sha256.cpp
    src/platform/unwind.cpp
    src/platform/clock.cpp
)
set(QUIDRA_PROFILE_CAPTURE_SOURCES
    src/runtime/metal_capture.cpp
    src/platform/file_descriptor.cpp
    src/platform/private_directory.cpp
    src/platform/user_cache_directory.cpp
)
if(APPLE)
  list(APPEND QUIDRA_PROFILE_CAPTURE_SOURCES src/runtime/metal_capture.mm)
endif()
set(QUIDRA_DEVICE_SOURCES
    src/runtime_parallel.cpp
    src/runtime_counters.cpp
    src/device_backend.cpp
    src/unified_storage.cpp
    src/runtime/timeline.cpp
    src/runtime/profile.cpp
    ${QUIDRA_PROFILE_CAPTURE_SOURCES}
    src/device/capabilities.cpp
    src/platform/os_version.cpp
)
if(APPLE)
  list(APPEND QUIDRA_DEVICE_SOURCES src/runtime/metal_timeline.mm)
endif()

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
  target_link_libraries(quidra_runtime_jit PRIVATE advapi32 ole32 shell32)
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
  target_link_libraries(quidra_runtime PUBLIC advapi32 ole32 shell32)
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
    src/device/capabilities.cpp
    src/profile_cli.cpp
    src/device_backend.cpp
    src/runtime_counters.cpp
    src/runtime_parallel.cpp
    src/unified_storage.cpp
    src/platform/clock.cpp
    src/native_build.cpp
    src/runtime/timeline.cpp
    src/runtime/profile.cpp
    src/runtime/metal_capture.cpp
    src/run_artifact.cpp
    src/run_cache.cpp
    src/jit.cpp
    src/ir_cli.cpp
    src/ir_report.cpp
    src/optimization_cli.cpp
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
    src/toolchain/call_site_table.cpp
    src/toolchain/compiler_identity.cpp
    src/toolchain/cuda_discovery.cpp
    src/toolchain/driver_output.cpp
    src/toolchain/dwarf_lines.cpp
    src/toolchain/installation_layout.cpp
    src/toolchain/link_flags.cpp
    src/toolchain/llvm_discovery.cpp
    src/toolchain/native_link_recipe.cpp
    src/toolchain/object_file.cpp
    src/toolchain/run_cache_eviction.cpp
    src/toolchain/run_cache_key.cpp
    src/toolchain/run_cache_metadata.cpp
    src/toolchain/run_cache_store.cpp
    src/toolchain/toolchain_identity.cpp
)
target_link_libraries(quidra PRIVATE quidra_core quidra_runtime)
if(APPLE)
  target_sources(quidra PRIVATE src/runtime/metal_timeline.mm src/runtime/metal_capture.mm)
endif()
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
      tests/golden/target_copy_census.cpp
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
  # CLI translation units can exceed the default COFF section limit.
  target_compile_options(quidra PRIVATE /W4 /bigobj)
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
  add_executable(quidra_native_code_info tests/golden/native_code_info.cpp
      src/toolchain/object_file.cpp src/toolchain/dwarf_lines.cpp)
  target_include_directories(quidra_native_code_info PRIVATE ${CMAKE_SOURCE_DIR}/src)

  find_package(Threads REQUIRED)
  add_executable(quidra_tail_calls_tests tests/tail_calls_tests.cpp)
  target_include_directories(quidra_tail_calls_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_tail_calls_tests PRIVATE quidra_core)
  add_test(NAME quidra_tail_calls_tests COMMAND quidra_tail_calls_tests)

  add_executable(quidra_runtime_tail_tests tests/runtime_tail_tests.cpp src/runtime_tail.cpp src/platform/unwind.cpp)
  target_include_directories(quidra_runtime_tail_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(quidra_runtime_tail_tests PRIVATE QUIDRA_TAIL_TEST_ALLOCATOR=1)
  target_link_libraries(quidra_runtime_tail_tests PRIVATE Threads::Threads)
  add_test(NAME quidra_runtime_tail_tests COMMAND quidra_runtime_tail_tests)

  add_executable(quidra_runtime_tail_path_tests tests/runtime_tail_path_tests.cpp src/runtime_tail_path.cpp)
  target_include_directories(quidra_runtime_tail_path_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_runtime_tail_path_tests COMMAND quidra_runtime_tail_path_tests)

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

  add_executable(quidra_shape_relation_tests tests/shape_relation_tests.cpp)
  target_link_libraries(quidra_shape_relation_tests PRIVATE quidra_core)
  target_include_directories(quidra_shape_relation_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_shape_relation_tests COMMAND quidra_shape_relation_tests)

  foreach(analysis IN ITEMS analysis dominator def_use local_ssa alias liveness analysis_cache work_budget summary content_hash tensor_facts loop_invariant clone_contracts binding_proof binding_origin object_bindings inline_binding_origin helper_binding_origin check_binding_origin step_regions step_region_annotation node_cleanup_query object_binding_inline)
    add_executable(quidra_ir_${analysis}_tests tests/ir_${analysis}_tests.cpp)
    target_include_directories(quidra_ir_${analysis}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(quidra_ir_${analysis}_tests PRIVATE quidra_core)
    if(MSVC)
      target_compile_options(quidra_ir_${analysis}_tests PRIVATE /bigobj)
    endif()
    add_test(NAME quidra_ir_${analysis} COMMAND quidra_ir_${analysis}_tests)
  endforeach()

  add_executable(quidra_compiler_extension_tests tests/compiler_extension_tests.cpp)
  target_include_directories(quidra_compiler_extension_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_compiler_extension_tests PRIVATE quidra_core)
  add_test(NAME quidra_compiler_extension
      COMMAND quidra_compiler_extension_tests ${CMAKE_BINARY_DIR}/compiler-extension-unit)

  foreach(feature IN ITEMS declaration_binding rule_dispatch statement_sites helper_specialization interface_fingerprint inlining inlining_hot_sites check_elimination)
    add_executable(quidra_${feature}_tests tests/${feature}_tests.cpp)
    target_include_directories(quidra_${feature}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(quidra_${feature}_tests PRIVATE quidra_core)
    add_test(NAME quidra_${feature} COMMAND quidra_${feature}_tests)
  endforeach()
  add_test(NAME quidra_ir_cli
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/ir_cli_tests.py $<TARGET_FILE:quidra>)
  add_test(NAME quidra_native_optimizer_cli
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/native_optimizer_cli_tests.py $<TARGET_FILE:quidra>)

  # Emits baseline/inlined programs for the native ownership and trace gate.
  add_executable(quidra_inlining_native_driver
      tests/inlining_native_driver.cpp
      src/toolchain/call_site_table.cpp
      src/toolchain/dwarf_lines.cpp
      src/toolchain/object_file.cpp)
  target_include_directories(quidra_inlining_native_driver PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_inlining_native_driver PRIVATE quidra_core)
  if(MSVC)
    target_compile_options(quidra_inlining_native_driver PRIVATE /bigobj)
  endif()
  add_test(NAME quidra_inlining_nested_exit
      COMMAND quidra_inlining_native_driver emit
        ${CMAKE_SOURCE_DIR}/tests/inlining_nested_exit.qui
        ${CMAKE_BINARY_DIR}/inlining-nested-exit ordinary stop nested)

  # The refactoring equivalence harness (tests/golden, driven by
  # tests/ir_golden.sh). The dump tool prints every observable compiler output
  # of one input; it links the compiler core with src/ on its private include
  # path.
  add_library(quidra_golden_support STATIC
      tests/golden/ir_full.cpp
      tests/golden/census.cpp
      tests/golden/census_keys.cpp
      tests/golden/target_copy_census.cpp
      tests/golden/failure.cpp
      tests/golden/repl_session.cpp
      $<TARGET_OBJECTS:quidra_repl_submission>
  )
  target_link_libraries(quidra_golden_support PUBLIC quidra_core)
  if(MSVC)
    # This golden AST walker intentionally repeats node names in mutually
    # exclusive if/else-if branches (C4456 under /WX).
    target_compile_options(quidra_golden_support PRIVATE /wd4456)
  endif()
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
  target_include_directories(quidra_ir_full_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_ir_full_tests PRIVATE quidra_golden_support)
  add_test(NAME quidra_ir_full_tests COMMAND quidra_ir_full_tests)
  add_executable(quidra_ir_operand_tests tests/ir_operand_tests.cpp)
  target_link_libraries(quidra_ir_operand_tests PRIVATE quidra_core)
  add_test(NAME quidra_ir_operand_tests COMMAND quidra_ir_operand_tests)
  add_executable(quidra_ir_instruction_text_tests tests/ir_instruction_text_tests.cpp)
  target_include_directories(quidra_ir_instruction_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_ir_instruction_text_tests PRIVATE quidra_core)
  add_test(NAME quidra_ir_instruction_text COMMAND quidra_ir_instruction_text_tests)
  add_executable(quidra_ir_optimizer_operand_tests tests/ir_optimizer_operand_tests.cpp)
  target_include_directories(quidra_ir_optimizer_operand_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_ir_optimizer_operand_tests PRIVATE quidra_core)
  add_test(NAME quidra_ir_optimizer_operands COMMAND quidra_ir_optimizer_operand_tests)
  add_executable(quidra_ir_value_allocator_tests tests/ir_value_allocator_tests.cpp)
  target_include_directories(quidra_ir_value_allocator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_ir_value_allocator_tests PRIVATE quidra_core)
  add_test(NAME quidra_ir_value_allocator COMMAND quidra_ir_value_allocator_tests)
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
      src/runtime_location.cpp src/runtime_trace.cpp src/runtime_tail.cpp src/runtime_tail_path.cpp src/platform/sha256.cpp
      src/platform/unwind.cpp)
  target_include_directories(quidra_runtime_failure_tests PRIVATE ${CMAKE_SOURCE_DIR}/include
                             ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_runtime_failure_tests COMMAND quidra_runtime_failure_tests)

  # The error record layout, the detail schemas, their renderings and the
  # identity of error values.
  add_executable(quidra_error_value_schema_tests tests/error_value_schema_tests.cpp)
  target_include_directories(quidra_error_value_schema_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_error_value_schema_tests COMMAND quidra_error_value_schema_tests)

  # The runtime's error records: constructors, drop, cause chains.
  add_executable(quidra_runtime_error_tests tests/runtime_error_tests.cpp)
  target_include_directories(quidra_runtime_error_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_error_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_error_tests COMMAND quidra_runtime_error_tests)

  # Shared inclusive/exclusive slice planning independent of a container backend.
  add_executable(quidra_slice_bounds_tests tests/slice_bounds_tests.cpp)
  target_include_directories(quidra_slice_bounds_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_slice_bounds_tests COMMAND quidra_slice_bounds_tests)

  add_executable(quidra_runtime_parallel_tests tests/runtime_parallel_tests.cpp)
  target_include_directories(quidra_runtime_parallel_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_parallel_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_parallel_tests COMMAND quidra_runtime_parallel_tests)
  add_executable(quidra_runtime_saved_tensor_tests tests/runtime_saved_tensor_tests.cpp)
  target_include_directories(quidra_runtime_saved_tensor_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_saved_tensor_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_saved_tensors COMMAND quidra_runtime_saved_tensor_tests)
  if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
    add_test(NAME quidra_runtime_saved_tensors_fake COMMAND quidra_runtime_saved_tensor_tests --fake)
    set_tests_properties(quidra_runtime_saved_tensors_fake PROPERTIES
        ENVIRONMENT "QUIDRA_TEST_FAKE_GPU_COUNT=1" TIMEOUT 90)
  endif()
  add_executable(quidra_runtime_counter_report_probe tests/runtime_counter_report_probe.cpp)
  target_include_directories(quidra_runtime_counter_report_probe PRIVATE ${CMAKE_SOURCE_DIR}/src
                             ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_counter_report_probe PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_counter_reports
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_counter_report_tests.py
                   $<TARGET_FILE:quidra_runtime_counter_report_probe>)
  add_executable(quidra_runtime_counter_flush_exception_tests
      tests/runtime_counter_flush_exception_tests.cpp)
  target_include_directories(quidra_runtime_counter_flush_exception_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_counter_flush_exception_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_counter_flush_exceptions
           COMMAND quidra_runtime_counter_flush_exception_tests)
  add_executable(quidra_runtime_observation_probe tests/runtime_observation_probe.cpp)
  add_executable(quidra_runtime_tensor_proven_tests tests/runtime_tensor_proven_tests.cpp)
  target_include_directories(quidra_runtime_tensor_proven_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_tensor_proven_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_tensor_proven
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_tensor_proven_tests.py
          $<TARGET_FILE:quidra_runtime_tensor_proven_tests>)
  target_include_directories(quidra_runtime_observation_probe PRIVATE ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_runtime_observation_probe PRIVATE quidra_runtime)
  add_test(NAME quidra_runtime_observation
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_observation_tests.py
                   $<TARGET_FILE:quidra_runtime_observation_probe>)
  add_executable(quidra_runtime_timeline_probe tests/runtime_timeline_probe.cpp
      src/runtime/timeline.cpp src/platform/clock.cpp)
  target_include_directories(quidra_runtime_timeline_probe PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_timeline_probe PRIVATE Threads::Threads)
  add_test(NAME quidra_runtime_timeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_timeline_tests.py
                   $<TARGET_FILE:quidra_runtime_timeline_probe>)
  add_executable(quidra_runtime_timeline_stream_tests tests/runtime_timeline_stream_tests.cpp)
  target_include_directories(quidra_runtime_timeline_stream_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_timeline_stream_tests PRIVATE Threads::Threads)
  add_test(NAME quidra_runtime_timeline_stream COMMAND quidra_runtime_timeline_stream_tests)
  set_tests_properties(quidra_runtime_timeline_stream PROPERTIES ENVIRONMENT "QUIDRA_TIMELINE=stderr")
  add_executable(quidra_runtime_profile_probe tests/runtime_profile_probe.cpp
      src/runtime/profile.cpp src/platform/clock.cpp ${QUIDRA_PROFILE_CAPTURE_SOURCES})
  target_include_directories(quidra_runtime_profile_probe PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_profile_probe PRIVATE Threads::Threads)
  if(APPLE)
    target_link_libraries(quidra_runtime_profile_probe PRIVATE "-framework Foundation" "-framework Metal")
  elseif(WIN32)
    target_link_libraries(quidra_runtime_profile_probe PRIVATE advapi32 ole32 shell32)
  endif()
  add_test(NAME quidra_runtime_profile
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_profile_tests.py
                   $<TARGET_FILE:quidra_runtime_profile_probe>)
  add_executable(quidra_runtime_capture_config_probe tests/runtime_capture_config_probe.cpp
      src/runtime/profile.cpp src/platform/clock.cpp ${QUIDRA_PROFILE_CAPTURE_SOURCES})
  target_include_directories(quidra_runtime_capture_config_probe PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_runtime_capture_config_probe PRIVATE Threads::Threads)
  if(APPLE)
    target_link_libraries(quidra_runtime_capture_config_probe PRIVATE
        "-framework Foundation" "-framework Metal")
  elseif(WIN32)
    target_link_libraries(quidra_runtime_capture_config_probe PRIVATE advapi32 ole32 shell32)
  endif()
  add_test(NAME quidra_runtime_capture_configuration
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/runtime_capture_config_tests.py
                   $<TARGET_FILE:quidra_runtime_capture_config_probe>)

  add_executable(quidra_parallel_effects_tests tests/parallel_effects_tests.cpp)
  target_link_libraries(quidra_parallel_effects_tests PRIVATE quidra_core)
  target_include_directories(quidra_parallel_effects_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_parallel_effects_tests COMMAND quidra_parallel_effects_tests)

  add_executable(quidra_parallel_block_abi_tests tests/parallel_block_abi_tests.cpp)
  target_include_directories(quidra_parallel_block_abi_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_parallel_block_abi_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_parallel_block_abi_tests COMMAND quidra_parallel_block_abi_tests)

  add_executable(quidra_owned_native_fences_tests tests/owned_native_fences_tests.cpp)
  target_include_directories(quidra_owned_native_fences_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_owned_native_fences_tests PRIVATE quidra_runtime)
  add_test(NAME quidra_owned_native_fences_tests COMMAND quidra_owned_native_fences_tests)

  add_executable(quidra_platform_environment_tests tests/platform_environment_tests.cpp)
  target_include_directories(quidra_platform_environment_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_platform_environment_tests COMMAND quidra_platform_environment_tests)
  add_executable(quidra_platform_clock_tests tests/platform_clock_tests.cpp)
  target_include_directories(quidra_platform_clock_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_platform_clock_tests PRIVATE Threads::Threads)
  add_test(NAME quidra_platform_clock_tests COMMAND quidra_platform_clock_tests)
  add_executable(quidra_device_capabilities_tests tests/device_capabilities_tests.cpp
      src/device/capabilities.cpp src/platform/sha256.cpp src/platform/os_version.cpp)
  target_include_directories(quidra_device_capabilities_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_device_capabilities_tests COMMAND quidra_device_capabilities_tests)
  add_executable(quidra_device_cli_tests tests/device_cli_tests.cpp src/device_cli.cpp)
  target_include_directories(quidra_device_cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
  add_test(NAME quidra_device_cli_tests COMMAND quidra_device_cli_tests)
  add_executable(quidra_device_observation_tests tests/device_observation_tests.cpp)
  target_include_directories(quidra_device_observation_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_device_observation_scopes COMMAND quidra_device_observation_tests)
  add_executable(quidra_encoder_attribution_tests tests/encoder_attribution_tests.cpp)
  target_include_directories(quidra_encoder_attribution_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_encoder_attribution COMMAND quidra_encoder_attribution_tests)
  add_executable(quidra_capture_request_tests tests/capture_request_tests.cpp)
  target_include_directories(quidra_capture_request_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_capture_request COMMAND quidra_capture_request_tests)
  add_executable(quidra_metal_capture_tests tests/metal_capture_tests.cpp
      src/runtime/metal_capture.cpp src/platform/clock.cpp)
  target_include_directories(quidra_metal_capture_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_metal_capture_tests PRIVATE Threads::Threads)
  if(APPLE)
    target_sources(quidra_metal_capture_tests PRIVATE src/runtime/metal_capture.mm)
    target_link_libraries(quidra_metal_capture_tests PRIVATE "-framework Foundation" "-framework Metal")
  endif()
  add_test(NAME quidra_metal_capture COMMAND quidra_metal_capture_tests)
  add_executable(quidra_device_capture_gate_tests tests/device_capture_gate_tests.cpp
      src/runtime/metal_capture.cpp src/platform/clock.cpp)
  target_include_directories(quidra_device_capture_gate_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(quidra_device_capture_gate_tests PRIVATE Threads::Threads)
  if(APPLE)
    target_sources(quidra_device_capture_gate_tests PRIVATE src/runtime/metal_capture.mm)
    target_link_libraries(quidra_device_capture_gate_tests PRIVATE "-framework Foundation" "-framework Metal")
  endif()
  add_test(NAME quidra_device_capture_gate COMMAND quidra_device_capture_gate_tests)
  add_executable(quidra_native_capability_source_tests tests/native_capability_source_tests.cpp)
  target_include_directories(quidra_native_capability_source_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_native_capability_source COMMAND quidra_native_capability_source_tests)
  add_executable(quidra_device_observation_probe tests/device_observation_probe.cpp)
  target_include_directories(quidra_device_observation_probe PRIVATE ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_device_observation_probe PRIVATE quidra_runtime)
  add_executable(quidra_device_observation_failure_probe tests/device_observation_failure_probe.cpp)
  target_include_directories(quidra_device_observation_failure_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(quidra_device_observation_failure_probe PRIVATE quidra_runtime)
  if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
    add_test(NAME quidra_device_observation_backend
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/device_observation_tests.py
                     $<TARGET_FILE:quidra_device_observation_probe>
                     $<TARGET_FILE:quidra_device_observation_failure_probe>)
    set_tests_properties(quidra_device_observation_backend PROPERTIES TIMEOUT 90)
    add_test(NAME quidra_device_capture
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/device_capture_tests.py
                     $<TARGET_FILE:quidra_device_observation_probe>)
    set_tests_properties(quidra_device_capture PROPERTIES TIMEOUT 90)
  endif()
  if(APPLE)
    add_executable(quidra_metal_capture_smoke tests/metal_capture_smoke.mm
        src/runtime/metal_capture.cpp src/runtime/metal_capture.mm src/platform/clock.cpp)
    target_include_directories(quidra_metal_capture_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(quidra_metal_capture_smoke PRIVATE
        "-framework Foundation" "-framework Metal")
    add_test(NAME quidra_metal_capture_hardware
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/metal_capture_smoke.py
                     $<TARGET_FILE:quidra_metal_capture_smoke>)
    set_tests_properties(quidra_metal_capture_hardware PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
    add_executable(quidra_metal_timeline_tests tests/metal_timeline_tests.mm
        src/runtime/metal_timeline.mm src/runtime/timeline.cpp src/runtime/profile.cpp
        src/platform/clock.cpp ${QUIDRA_PROFILE_CAPTURE_SOURCES})
    target_include_directories(quidra_metal_timeline_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(quidra_metal_timeline_tests PRIVATE Threads::Threads
        "-framework Foundation" "-framework Metal")
    add_test(NAME quidra_metal_timeline COMMAND quidra_metal_timeline_tests)
    set_tests_properties(quidra_metal_timeline PROPERTIES ENVIRONMENT "QUIDRA_TIMELINE=stderr;QUIDRA_PROFILE=")
    add_test(NAME quidra_metal_timeline_disabled COMMAND quidra_metal_timeline_tests --disabled)
    set_tests_properties(quidra_metal_timeline_disabled PROPERTIES
        ENVIRONMENT "QUIDRA_TIMELINE=;QUIDRA_PROFILE=")
    add_executable(quidra_metal_timeline_smoke tests/metal_timeline_smoke.mm
        src/runtime/metal_timeline.mm src/runtime/timeline.cpp src/runtime/profile.cpp
        src/platform/clock.cpp ${QUIDRA_PROFILE_CAPTURE_SOURCES})
    target_include_directories(quidra_metal_timeline_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(quidra_metal_timeline_smoke PRIVATE Threads::Threads
        "-framework Foundation" "-framework Metal")
    add_test(NAME quidra_metal_timeline_hardware COMMAND quidra_metal_timeline_smoke)
    set_tests_properties(quidra_metal_timeline_hardware PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "QUIDRA_TIMELINE=stderr;QUIDRA_PROFILE=" TIMEOUT 90)
  endif()

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

  add_executable(quidra_platform_process_tests
      tests/platform_process_tests.cpp
      src/platform/file_descriptor.cpp
      src/platform/native_text.cpp
      src/platform/process.cpp
      src/platform/temporary_directory.cpp)
  target_include_directories(quidra_platform_process_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_platform_process_tests COMMAND quidra_platform_process_tests)
  add_executable(quidra_profile_cli_tests tests/profile_cli_tests.cpp src/profile_cli.cpp
      src/platform/strict_json.cpp src/platform/user_cache_directory.cpp
      src/platform/private_directory.cpp src/platform/file_descriptor.cpp)
  target_include_directories(quidra_profile_cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME quidra_profile_cli_tests COMMAND quidra_profile_cli_tests)
  add_executable(quidra_profile_capture_reader_tests tests/profile_capture_reader_tests.cpp
      src/profile_cli.cpp src/platform/strict_json.cpp src/platform/user_cache_directory.cpp
      src/platform/private_directory.cpp src/platform/file_descriptor.cpp)
  target_include_directories(quidra_profile_capture_reader_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  if(WIN32)
    target_link_libraries(quidra_profile_capture_reader_tests PRIVATE advapi32 ole32 shell32)
  endif()
  add_test(NAME quidra_profile_capture_reader COMMAND quidra_profile_capture_reader_tests)

  # The native link recipe with the host toolchain: build and run modes, the
  # template, the run-mode recording, and byte-identical executables.
  add_executable(quidra_native_link_recipe_tests
      tests/native_link_recipe_tests.cpp
      src/toolchain/native_link_recipe.cpp
      src/toolchain/call_site_table.cpp
      src/toolchain/dwarf_lines.cpp
      src/toolchain/object_file.cpp
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
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
    target_compile_definitions(quidra_native_link_recipe_tests PRIVATE QUIDRA_SANITIZE_GENERATED=1)
  endif()
  add_dependencies(quidra_native_link_recipe_tests quidra_runtime)
  add_test(NAME quidra_native_link_recipe_tests COMMAND quidra_native_link_recipe_tests)
  set_tests_properties(quidra_native_link_recipe_tests PROPERTIES
      ENVIRONMENT "QUIDRA_RUNTIME_LIBRARY=$<TARGET_FILE:quidra_runtime>")

  # The call-site table step: the object reader, the DWARF decoder and the
  # table's entries, on objects the driver compiles for each target it has.
  add_executable(quidra_call_site_table_tests
      tests/call_site_table_tests.cpp
      src/toolchain/call_site_table.cpp
      src/toolchain/dwarf_lines.cpp
      src/toolchain/object_file.cpp
      src/toolchain/llvm_discovery.cpp
      src/platform/executable.cpp
      src/platform/file_descriptor.cpp
      src/platform/file_names.cpp
      src/platform/native_text.cpp
      src/platform/process.cpp
      src/platform/temporary_directory.cpp)
  target_include_directories(quidra_call_site_table_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME quidra_call_site_table_tests COMMAND quidra_call_site_table_tests)

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
      src/toolchain/call_site_table.cpp
      src/toolchain/dwarf_lines.cpp
      src/toolchain/object_file.cpp
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
    add_test(NAME quidra_numeric_conversions COMMAND bash ${CMAKE_SOURCE_DIR}/tests/numeric_conversions_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_numeric_conversion_matrix COMMAND bash ${CMAKE_SOURCE_DIR}/tests/numeric_conversion_tests.sh $<TARGET_FILE:quidra>)
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
        src/toolchain/call_site_table.cpp
        src/toolchain/dwarf_lines.cpp
        src/toolchain/object_file.cpp
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
    add_test(NAME quidra_tensor_array_borrow COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tensor_array_borrow_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_location COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_location_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_diagnostics COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_diagnostics_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_error_json COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_error_json_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_error_values COMMAND bash ${CMAKE_SOURCE_DIR}/tests/error_value_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_runtime_trace COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_trace_tests.sh $<TARGET_FILE:quidra>)
    # External hosts must use the runtime's compiler to resolve sanitizer
    # instrumentation with its matching runtime. Ordinary builds keep the
    # scripts' CC/CXX defaults, and the C export sources remain compiled as C.
    set(QUIDRA_TEST_HOST_LINKER "")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_FLAGS MATCHES "sanitize")
      set(QUIDRA_TEST_HOST_LINKER "${CMAKE_CXX_COMPILER}")
    endif()
    add_test(NAME quidra_runtime_trace_concurrency
             COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_trace_concurrency_tests.sh
                     $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR} "${QUIDRA_TEST_HOST_LINKER}")
    add_test(NAME quidra_runtime_tail_trace COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_tail_trace_tests.sh $<TARGET_FILE:quidra>)
    # Exported functions called from C (export "C"): values, reentrancy,
    # concurrent host threads, failures at the boundary.
    add_test(NAME quidra_c_export
             COMMAND bash ${CMAKE_SOURCE_DIR}/tests/c_export_tests.sh
                     $<TARGET_FILE:quidra> "${QUIDRA_TEST_HOST_LINKER}")
    add_test(NAME quidra_package_releases COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_release_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_package_metadata_sync COMMAND bash ${CMAKE_SOURCE_DIR}/tests/package_metadata_sync_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_structural_traversals COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/structural_traversal_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_array_comprehensions COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/array_comprehension_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_parallel_determinism COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/parallel_determinism_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_stdlib COMMAND bash ${CMAKE_SOURCE_DIR}/tests/stdlib_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    add_test(NAME quidra_nat_lengths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/nat_length_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_exact_real_cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/exact_real_cli_tests.sh $<TARGET_FILE:quidra> ${CMAKE_SOURCE_DIR})
    # No visible NaN or infinity: real operations whose results are not finite.
    add_test(NAME quidra_nonfinite COMMAND bash ${CMAKE_SOURCE_DIR}/tests/nonfinite_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_half_precision COMMAND bash ${CMAKE_SOURCE_DIR}/tests/half_precision_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_complex COMMAND bash ${CMAKE_SOURCE_DIR}/tests/complex_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_shape_variables COMMAND bash ${CMAKE_SOURCE_DIR}/tests/shape_variable_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_shape_ellipsis COMMAND bash ${CMAKE_SOURCE_DIR}/tests/shape_ellipsis_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_generic_container_reductions COMMAND bash ${CMAKE_SOURCE_DIR}/tests/generic_container_reductions.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_container_casts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/container_cast_tests.sh $<TARGET_FILE:quidra>)
    # Slices and slice assignment against a brute-force oracle.
    add_test(NAME quidra_slices COMMAND bash ${CMAKE_SOURCE_DIR}/tests/slice_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_slice_direction COMMAND bash ${CMAKE_SOURCE_DIR}/tests/slice_direction_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_real_loop_headers COMMAND bash ${CMAKE_SOURCE_DIR}/tests/real_loop_headers_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_for_shorthand COMMAND bash ${CMAKE_SOURCE_DIR}/tests/for_shorthand_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_tuples COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tuple_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_class_initialization COMMAND bash ${CMAKE_SOURCE_DIR}/tests/class_initialization_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_local_scopes COMMAND bash ${CMAKE_SOURCE_DIR}/tests/local_scope_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_unbound_references COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unbound_reference_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_reference_fields COMMAND bash ${CMAKE_SOURCE_DIR}/tests/reference_field_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_stored_reference_overlap COMMAND bash ${CMAKE_SOURCE_DIR}/tests/stored_reference_overlap_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_for_headers COMMAND bash ${CMAKE_SOURCE_DIR}/tests/for_header_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_parallel_for COMMAND bash ${CMAKE_SOURCE_DIR}/tests/parallel_for_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_multi_array_for COMMAND bash ${CMAKE_SOURCE_DIR}/tests/multi_array_for_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_ownership_predicate_sites COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ownership_predicate_sites.py)
    add_test(NAME quidra_autograd_target_copy COMMAND bash ${CMAKE_SOURCE_DIR}/tests/autograd_target_copy_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_floating_casts COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/floating_cast_tests.py $<TARGET_FILE:quidra>)
    add_test(NAME quidra_tensor_autograd_foundation COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tensor_autograd_foundation_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_tensor_view_fast_paths COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tensor_view_fast_path_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_cpu_parallel COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cpu_parallel_tests.sh $<TARGET_FILE:quidra>)
    add_test(NAME quidra_unified_memory COMMAND bash ${CMAKE_SOURCE_DIR}/tests/unified_memory_tests.sh $<TARGET_FILE:quidra>)
    if(QUIDRA_ENABLE_TEST_GPU_BACKEND)
      add_executable(quidra_device_backend_tests tests/device_backend_tests.cpp)
      target_include_directories(quidra_device_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
      target_compile_definitions(quidra_device_backend_tests PRIVATE QUIDRA_ENABLE_TEST_GPU_BACKEND=1)
      target_link_libraries(quidra_device_backend_tests PRIVATE quidra_runtime)
      add_test(NAME quidra_device_backend_tests COMMAND quidra_device_backend_tests)
      add_test(NAME quidra_gpu_contracts COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_contract_tests.sh $<TARGET_FILE:quidra>)
      add_test(NAME quidra_gpu_contracts_autograd COMMAND bash ${CMAKE_SOURCE_DIR}/tests/gpu_autograd_contract_tests.sh $<TARGET_FILE:quidra>)
      add_test(NAME quidra_runtime_counters COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_counters.sh $<TARGET_FILE:quidra>)
      add_test(NAME quidra_runtime_validation_counters
               COMMAND bash ${CMAKE_SOURCE_DIR}/tests/runtime_validation_counters.sh $<TARGET_FILE:quidra>)
      set_tests_properties(quidra_runtime_validation_counters PROPERTIES TIMEOUT 90)
      add_test(NAME quidra_profile_cli_integration
               COMMAND bash ${CMAKE_SOURCE_DIR}/tests/profile_cli_integration.sh $<TARGET_FILE:quidra>)
      set_tests_properties(quidra_profile_cli_integration PROPERTIES TIMEOUT 90)
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
  # docs/spec/diagnostics.md documents every runtime, error-value and compile-time code.
  add_test(NAME quidra_diagnostic_codes COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/diagnostic_codes_tests.py ${CMAKE_SOURCE_DIR})
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
    quidra_ir_operand_tests
    quidra_ir_analysis
    quidra_ir_dominator
    quidra_ir_def_use
    quidra_ir_local_ssa
    quidra_ir_alias
    quidra_ir_liveness
    quidra_ir_analysis_cache
    quidra_ir_summary
    quidra_ir_content_hash
    quidra_ir_tensor_facts
    quidra_ir_loop_invariant
    quidra_ir_clone_contracts
    quidra_ir_binding_proof
    quidra_ir_binding_origin
    quidra_ir_object_bindings
    quidra_ir_inline_binding_origin
    quidra_ir_helper_binding_origin
    quidra_ir_step_regions
    quidra_ir_step_region_annotation
    quidra_ir_node_cleanup_query
    quidra_ir_object_binding_inline
    quidra_compiler_extension
    quidra_declaration_binding
    quidra_rule_dispatch
    quidra_statement_sites
    quidra_helper_specialization
    quidra_interface_fingerprint
    quidra_inlining
    quidra_inlining_hot_sites
    quidra_inlining_nested_exit
    quidra_check_elimination
    quidra_ir_cli
    quidra_runtime_tensor_proven
    quidra_ir_instruction_text
    quidra_ir_optimizer_operands
    quidra_ir_value_allocator
    quidra_import_path_tests
    quidra_parser_tests
    quidra_type_policy_tests
    quidra_shape_relation_tests
    quidra_package_manifest_tests
    quidra_package_lock_tests
    quidra_runtime_string_tests
    quidra_runtime_parallel_tests
    quidra_runtime_saved_tensors
    quidra_runtime_counter_reports
    quidra_runtime_counter_flush_exceptions
    quidra_runtime_observation
    quidra_runtime_timeline
    quidra_runtime_timeline_stream
    quidra_runtime_profile
    quidra_runtime_capture_configuration
    quidra_parallel_effects_tests
    quidra_parallel_block_abi_tests
    quidra_owned_native_fences_tests
    quidra_platform_environment_tests
    quidra_platform_clock_tests
    quidra_device_capabilities_tests
    quidra_device_cli_tests
    quidra_device_observation_scopes
    quidra_encoder_attribution
    quidra_capture_request
    quidra_metal_capture
    quidra_device_capture_gate
    quidra_native_capability_source
    quidra_platform_cache_directory_tests
    quidra_platform_sha256_tests
    quidra_platform_strict_json_tests
    quidra_platform_process_tests
    quidra_profile_cli_tests
    quidra_profile_capture_reader
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
    add_test(NAME quidra_parallel_blocks COMMAND bash ${CMAKE_SOURCE_DIR}/tests/parallel_blocks_tests.sh $<TARGET_FILE:quidra>)
    set_tests_properties(quidra_parallel_blocks PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT})
  endif()
  if(UNIX)
    set_tests_properties(
      quidra_package_names
      quidra_runtime_trace_concurrency
      quidra_package_identity
      quidra_package_native
      quidra_package_releases
      quidra_package_metadata_sync
      quidra_autograd_target_copy
      quidra_floating_casts
      quidra_tensor_autograd_foundation
      quidra_tensor_view_fast_paths
      quidra_cpu_parallel
      quidra_native_smoke
      quidra_nat_lengths
      quidra_nonfinite
      quidra_half_precision
      quidra_complex
      PROPERTIES TIMEOUT ${QUIDRA_TEST_TIMEOUT}
    )
    # The CLI and stdlib suites compile and run many programs each. Fully
    # instrumented compiler runs need additional cumulative build time;
    # each suite still has a finite hang guard.
    math(EXPR QUIDRA_PROGRAM_SUITE_TIMEOUT "${QUIDRA_TEST_TIMEOUT} * 2")
    if(CMAKE_CXX_FLAGS MATCHES "sanitize")
      math(EXPR QUIDRA_PROGRAM_SUITE_TIMEOUT "${QUIDRA_TEST_TIMEOUT} * 3")
    endif()
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

# Windows CI builds every test target after the compiler and CLI. The
# generated compiler was already using /O1 /Ob0 here, but the 30+ test
# executables still used Release /O2, leading to very slow MSVC codegen in
# large template-heavy tests. Apply the same CI-only options to test targets.
# /bigobj is needed for shape_relation_tests.cpp (MSVC C1128). C4702 is
# repeated from visitor template instantiations. Production executables,
# native runtime and normal Release builds are unchanged; /WX remains on.
if(MSVC AND QUIDRA_MSVC_CI_FAST_FRONTEND AND BUILD_TESTING)
  get_property(_quidra_ci_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_quidra_ci_target IN LISTS _quidra_ci_targets)
    if(_quidra_ci_target MATCHES "^quidra_" AND
       NOT _quidra_ci_target MATCHES
           "^quidra_(core($|_part_)|runtime($|_jit$)|repl_submission$)")
      target_compile_options(${_quidra_ci_target} PRIVATE
        "$<$<CONFIG:Release>:/O1>"
        "$<$<CONFIG:Release>:/Ob0>"
        /bigobj /wd4702 /Bt)
    endif()
  endforeach()
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
