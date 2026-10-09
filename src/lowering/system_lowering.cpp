// System lowering: the command line (cli.argument, cli.option, cli.flag,
// cli.finish), the environment, the clock (time.now, time.since,
// time.seconds, time.sleep) and processes (process.run, process.shell).

#include "lowering/lowerer.hpp"

namespace quidra::lowering {

ValueId SystemLowering::lower_cli_argument(const Expr& e, const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),index=lowerer_.lower_int64(*n.args[1].value),out=builder_.fresh();
    builder_.emit(CliArgument{out,name,index,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,name);
    return out;
}

ValueId SystemLowering::lower_cli_argument_optional(const Expr& e, const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),index=lowerer_.lower_int64(*n.args[1].value),fallback=lowerer_.lower(*n.args[2].value),out=builder_.fresh();
    builder_.emit(CliArgumentOptional{out,name,index,fallback,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,name);
    lifetime_.release_temporary(*n.args[2].value,fallback);
    return out;
}

ValueId SystemLowering::lower_cli_option(const Expr& e, const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),fallback=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(CliOption{out,name,fallback,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,name);
    lifetime_.release_temporary(*n.args[1].value,fallback);
    return out;
}

ValueId SystemLowering::lower_cli_flag(const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(CliFlag{out,name});
    lifetime_.release_temporary(*n.args[0].value,name);
    return out;
}

ValueId SystemLowering::lower_cli_finish() {
    builder_.emit(CliFinish{});
    return 0;
}

ValueId SystemLowering::lower_environment_get(const Expr& e, const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(EnvironmentGet{out,name,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[0].value,name);
    return out;
}

ValueId SystemLowering::lower_environment_has(const CallExpr& n) {
    auto name=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(EnvironmentHas{out,name});
    lifetime_.release_temporary(*n.args[0].value,name);
    return out;
}

ValueId SystemLowering::lower_time_now(const CallExpr& n) {
    auto sync=n.args.empty()?builder_.const_bool(false):lowerer_.lower(*n.args[0].value);
    auto out=builder_.fresh();
    builder_.emit(TimeNow{out,sync});
    return out;
}

ValueId SystemLowering::lower_time_since(const CallExpr& n) {
    auto start=lowerer_.lower(*n.args[0].value);
    auto sync=n.args.size()==2?lowerer_.lower(*n.args[1].value):builder_.const_bool(false);
    auto out=builder_.fresh();
    builder_.emit(TimeSince{out,start,sync});
    lifetime_.release_temporary(*n.args[0].value,start);
    return out;
}

ValueId SystemLowering::lower_time_seconds(const CallExpr& n) {
    auto seconds=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(TimeSeconds{out,seconds});
    return out;
}

ValueId SystemLowering::lower_time_sleep(const Expr& e, const CallExpr& n) {
    auto duration=lowerer_.lower(*n.args[0].value);
    builder_.emit(TimeSleep{duration,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
    lifetime_.release_temporary(*n.args[0].value,duration);
    return 0;
}

ValueId SystemLowering::lower_process_run(const CallExpr& n) {
    auto program=lowerer_.lower(*n.args[0].value),args=lowerer_.lower(*n.args[1].value),out=builder_.fresh();
    builder_.emit(ProcessRun{out,program,args});
    lifetime_.release_temporary(*n.args[0].value,program);
    lifetime_.release_temporary(*n.args[1].value,args);
    return out;
}

ValueId SystemLowering::lower_process_shell(const CallExpr& n) {
    auto command=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
    builder_.emit(ProcessShell{out,command});
    lifetime_.release_temporary(*n.args[0].value,command);
    return out;
}

} // namespace quidra::lowering
