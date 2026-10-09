// Builtin lowering: a call of a builtin (print, file.read, json.parse, ...)
// dispatched on the builtin to the domain that lowers it (console_lowering.cpp,
// file_lowering.cpp, json_lowering.cpp, ...). range() is lowered only by for
// statements.

#include "lowering/lowerer.hpp"
#include <stdexcept>

namespace quidra::lowering {

ValueId BuiltinLowering::lower_builtin_call(
    const Expr& e, const CallExpr& n, const CallResolution& resolution) {
    if(!resolution.builtin) throw std::logic_error("Builtin resolution has no builtin kind.");
    switch(*resolution.builtin){
        case BuiltinCallable::Print: return console_.lower_print(e, n);
        case BuiltinCallable::Scan: return scan_.lower_scan(e);
        case BuiltinCallable::Exit: return control_flow_.lower_exit(n);
        case BuiltinCallable::ReflectTypeName: return reflection_.lower_reflect_type_name(n);
        case BuiltinCallable::ReflectCollect:
        case BuiltinCallable::ReflectPaths: return reflection_.lower_reflect_collection(e, n, resolution);
        case BuiltinCallable::TensorCreate:
        case BuiltinCallable::TensorZeros:
        case BuiltinCallable::TensorOnes: return tensors_.lower_tensor_create(e, n, resolution);
        case BuiltinCallable::Array: return aggregates_.lower_array_allocation(e, n);
        case BuiltinCallable::Range:
            throw std::logic_error("range is lowered only by for statements");
        case BuiltinCallable::Len: return aggregates_.lower_len(e, n);
        case BuiltinCallable::ExactAtom: return operators_.lower_exact_atom(e, n);
        case BuiltinCallable::ExactUnary: return operators_.lower_exact_unary(e, n);
        case BuiltinCallable::Flush: return console_.lower_flush(e);
        case BuiltinCallable::CliArgument: return system_.lower_cli_argument(e, n);
        case BuiltinCallable::CliArgumentOptional: return system_.lower_cli_argument_optional(e, n);
        case BuiltinCallable::CliOption: return system_.lower_cli_option(e, n);
        case BuiltinCallable::CliFlag: return system_.lower_cli_flag(n);
        case BuiltinCallable::CliFinish: return system_.lower_cli_finish();
        case BuiltinCallable::FileOpen:
        case BuiltinCallable::FileCreate:
        case BuiltinCallable::FileAppend: return files_.lower_file_open(e, n, resolution);
        case BuiltinCallable::FileRead: return files_.lower_file_read(e, n);
        case BuiltinCallable::FileReadBin: return files_.lower_file_read_bin(e, n);
        case BuiltinCallable::FileList: return files_.lower_file_list(e, n);
        case BuiltinCallable::FileWrite: return files_.lower_file_write(e, n);
        case BuiltinCallable::FileWriteBin: return files_.lower_file_write_bin(e, n);
        case BuiltinCallable::FileExists: return files_.lower_file_exists(e, n);
        case BuiltinCallable::FileIsDirectory: return files_.lower_file_is_directory(e, n);
        case BuiltinCallable::FileRemove: return files_.lower_file_remove(e, n);
        case BuiltinCallable::FileCopy: return files_.lower_file_copy(e, n);
        case BuiltinCallable::FileMove: return files_.lower_file_move(e, n);
        case BuiltinCallable::FileMkdir: return files_.lower_file_mkdir(e, n);
        case BuiltinCallable::EnvironmentGet: return system_.lower_environment_get(e, n);
        case BuiltinCallable::EnvironmentHas: return system_.lower_environment_has(n);
        case BuiltinCallable::TestCheck: return checks_.lower_test_check(n);
        case BuiltinCallable::TestEqual: return checks_.lower_test_equal(n);
        case BuiltinCallable::TimeNow: return system_.lower_time_now(n);
        case BuiltinCallable::TimeSince: return system_.lower_time_since(n);
        case BuiltinCallable::TimeSeconds: return system_.lower_time_seconds(n);
        case BuiltinCallable::TimeSleep: return system_.lower_time_sleep(e, n);
        case BuiltinCallable::GpuSync: return tensors_.lower_gpu_sync(e, n);
        case BuiltinCallable::AutogradTarget: return autograd_.lower_autograd_target();
        case BuiltinCallable::AtomicCounter: return concurrency_.lower_atomic_counter(n);
        case BuiltinCallable::TaskAll: return concurrency_.lower_task_all(e, n);
        case BuiltinCallable::RandomGenerator: return random_.lower_random_generator(n);
        case BuiltinCallable::RandomInt: return random_.lower_random_int(e, n);
        case BuiltinCallable::RandomFloat: return random_.lower_random_float();
        case BuiltinCallable::RandomBool: return random_.lower_random_bool();
        case BuiltinCallable::ProcessRun: return system_.lower_process_run(n);
        case BuiltinCallable::ProcessShell: return system_.lower_process_shell(n);
        case BuiltinCallable::JsonParse: return json_.lower_json_parse(e, n);
        case BuiltinCallable::JsonKind: return json_.lower_json_kind();
        case BuiltinCallable::JsonSize: return json_.lower_json_size(e);
        case BuiltinCallable::JsonGet: return json_.lower_json_get(e, n);
        case BuiltinCallable::JsonAt: return json_.lower_json_at(e, n);
        case BuiltinCallable::JsonText: return json_.lower_json_text(e);
        case BuiltinCallable::JsonInteger: return json_.lower_json_integer(e);
        case BuiltinCallable::JsonNumber: return json_.lower_json_number(e);
        case BuiltinCallable::JsonBigReal: return json_.lower_json_big_real(e);
        case BuiltinCallable::JsonBoolean: return json_.lower_json_boolean(e);
        case BuiltinCallable::JsonEncode: return json_.lower_json_encode();
        case BuiltinCallable::JsonEqual: return json_.lower_json_equal(n);
        case BuiltinCallable::HttpGet: return http_.lower_http_get(e, n);
        case BuiltinCallable::HttpHeader: return http_.lower_http_header(e, n);
    }
    throw std::logic_error("Unsupported resolved call kind.");
}

} // namespace quidra::lowering
