// The LLVM text layer (src/llvm_text): the exact text of every type,
// operand and builder operation, and of a module's sections. The backend's
// output is byte-for-byte the text these write, so each operation is pinned
// here line by line.

#include "llvm_text/appendable_text.hpp"
#include "llvm_text/escape.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "llvm_text/llvm_callee.hpp"
#include "llvm_text/llvm_module.hpp"
#include "llvm_text/llvm_type.hpp"
#include "llvm_text/llvm_value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace quidra::llvm_text;
using namespace quidra::llvm_text::types;

int failures = 0;

void expect(const std::string& name, const std::string& actual, const std::string& expected) {
    if (actual == expected) return;
    ++failures;
    std::fprintf(stderr, "llvm text test %s:\n  expected: [%s]\n  actual:   [%s]\n", name.c_str(),
                 expected.c_str(), actual.c_str());
}

// The text one builder operation writes into a fresh function text.
template <class Operation>
std::string written(const Operation& operation) {
    FunctionText text;
    LlvmBuilder builder(text);
    operation(builder);
    return text.str();
}

// The spelling of a type or an operand.
template <class T>
std::string printed(const T& item) {
    std::string text;
    item.print(text);
    return text;
}

void types_and_operands() {
    expect("void", printed(void_type), "void");
    expect("i1", printed(i1), "i1");
    expect("i8", printed(i8), "i8");
    expect("i16", printed(i16), "i16");
    expect("i32", printed(i32), "i32");
    expect("i64", printed(i64), "i64");
    expect("float", printed(float_type), "float");
    expect("double", printed(double_type), "double");
    expect("ptr", printed(ptr), "ptr");
    expect("array", printed(LlvmType::array(21, i8)), "[21 x i8]");
    expect("empty array", printed(LlvmType::array(0, i64)), "[0 x i64]");
    expect("structure", printed(LlvmType::structure(i32, i1)), "{ i32, i1 }");

    expect("name", printed(LlvmOperand("%v3")), "%v3");
    expect("string name", printed(LlvmOperand(std::string("%local.x"))), "%local.x");
    expect("global", printed(LlvmOperand::global(".str.4")), "@.str.4");
    expect("int", printed(LlvmOperand(-7)), "-7");
    expect("long long minimum", printed(LlvmOperand(static_cast<long long>(INT64_MIN))),
           "-9223372036854775808");
    expect("unsigned maximum", printed(LlvmOperand(~0ULL)), "18446744073709551615");
    expect("size_t", printed(LlvmOperand(std::size_t{4096})), "4096");
    expect("uint32_t", printed(LlvmOperand(std::uint32_t{12})), "12");
}

void operations() {
    expect("block", written([](LlvmBuilder& b) { b.block("cast.ok.3"); }), "cast.ok.3:\n");
    expect("add", written([](LlvmBuilder& b) { b.binary("%v1", BinaryOp::add, i64, "%v0", 0); }),
           "  %v1 = add i64 %v0, 0\n");
    expect("add nsw",
           written([](LlvmBuilder& b) { b.binary("%v1", BinaryOp::add, NoWrap::nsw, i32, "%a", "%b"); }),
           "  %v1 = add nsw i32 %a, %b\n");
    expect("mul nuw",
           written([](LlvmBuilder& b) { b.binary("%v1", BinaryOp::mul, NoWrap::nuw, i8, "%a", "%b"); }),
           "  %v1 = mul nuw i8 %a, %b\n");
    const std::array<std::pair<BinaryOp, const char*>, 18> binaries{{
        {BinaryOp::add, "add"}, {BinaryOp::sub, "sub"}, {BinaryOp::mul, "mul"},
        {BinaryOp::sdiv, "sdiv"}, {BinaryOp::udiv, "udiv"}, {BinaryOp::srem, "srem"},
        {BinaryOp::urem, "urem"}, {BinaryOp::shl, "shl"}, {BinaryOp::lshr, "lshr"},
        {BinaryOp::ashr, "ashr"}, {BinaryOp::and_, "and"}, {BinaryOp::or_, "or"},
        {BinaryOp::xor_, "xor"}, {BinaryOp::fadd, "fadd"}, {BinaryOp::fsub, "fsub"},
        {BinaryOp::fmul, "fmul"}, {BinaryOp::fdiv, "fdiv"}, {BinaryOp::frem, "frem"},
    }};
    for (const auto& [op, spelling] : binaries)
        expect(spelling, written([op](LlvmBuilder& b) { b.binary("%r", op, i1, "%x", "true"); }),
               std::string("  %r = ") + spelling + " i1 %x, true\n");
    expect("fneg", written([](LlvmBuilder& b) { b.fneg("%v2", double_type, "%v1"); }),
           "  %v2 = fneg double %v1\n");
    const std::array<std::pair<IntPredicate, const char*>, 10> integer_predicates{{
        {IntPredicate::eq, "eq"}, {IntPredicate::ne, "ne"}, {IntPredicate::ugt, "ugt"},
        {IntPredicate::uge, "uge"}, {IntPredicate::ult, "ult"}, {IntPredicate::ule, "ule"},
        {IntPredicate::sgt, "sgt"}, {IntPredicate::sge, "sge"}, {IntPredicate::slt, "slt"},
        {IntPredicate::sle, "sle"},
    }};
    for (const auto& [predicate, spelling] : integer_predicates)
        expect(spelling, written([predicate](LlvmBuilder& b) { b.icmp("%c", predicate, ptr, "%p", "null"); }),
               std::string("  %c = icmp ") + spelling + " ptr %p, null\n");
    const std::array<std::pair<FloatPredicate, const char*>, 14> float_predicates{{
        {FloatPredicate::oeq, "oeq"}, {FloatPredicate::ogt, "ogt"}, {FloatPredicate::oge, "oge"},
        {FloatPredicate::olt, "olt"}, {FloatPredicate::ole, "ole"}, {FloatPredicate::one, "one"},
        {FloatPredicate::ord, "ord"}, {FloatPredicate::ueq, "ueq"}, {FloatPredicate::ugt, "ugt"},
        {FloatPredicate::uge, "uge"}, {FloatPredicate::ult, "ult"}, {FloatPredicate::ule, "ule"},
        {FloatPredicate::une, "une"}, {FloatPredicate::uno, "uno"},
    }};
    for (const auto& [predicate, spelling] : float_predicates)
        expect(spelling,
               written([predicate](LlvmBuilder& b) {
                   b.fcmp("%c", predicate, double_type, "%v", "0x47EFFFFFE0000000");
               }),
               std::string("  %c = fcmp ") + spelling + " double %v, 0x47EFFFFFE0000000\n");
    const std::array<std::pair<CastOp, const char*>, 12> casts{{
        {CastOp::trunc, "trunc"}, {CastOp::zext, "zext"}, {CastOp::sext, "sext"},
        {CastOp::fptrunc, "fptrunc"}, {CastOp::fpext, "fpext"}, {CastOp::fptoui, "fptoui"},
        {CastOp::fptosi, "fptosi"}, {CastOp::uitofp, "uitofp"}, {CastOp::sitofp, "sitofp"},
        {CastOp::ptrtoint, "ptrtoint"}, {CastOp::inttoptr, "inttoptr"}, {CastOp::bitcast, "bitcast"},
    }};
    for (const auto& [op, spelling] : casts)
        expect(spelling, written([op](LlvmBuilder& b) { b.cast("%w", op, {i64, "%v"}, i32); }),
               std::string("  %w = ") + spelling + " i64 %v to i32\n");
    expect("select",
           written([](LlvmBuilder& b) { b.select("%f", "true", {ptr, LlvmOperand::global("fn")}, {ptr, "null"}); }),
           "  %f = select i1 true, ptr @fn, ptr null\n");
    expect("getelementptr inbounds",
           written([](LlvmBuilder& b) { b.getelementptr("%p", Inbounds::yes, i8, "%box", {{i64, 8}}); }),
           "  %p = getelementptr inbounds i8, ptr %box, i64 8\n");
    expect("getelementptr",
           written([](LlvmBuilder& b) { b.getelementptr("%p", Inbounds::no, i8, "%raw", {{i64, 0}}); }),
           "  %p = getelementptr i8, ptr %raw, i64 0\n");
    expect("getelementptr array",
           written([](LlvmBuilder& b) {
               b.getelementptr("%s", Inbounds::yes, LlvmType::array(6, i8), LlvmOperand::global(".str.0"),
                               {{i64, 0}, {i64, 0}});
           }),
           "  %s = getelementptr inbounds [6 x i8], ptr @.str.0, i64 0, i64 0\n");
    expect("load", written([](LlvmBuilder& b) { b.load("%x", i64, "%slot", Align::none); }),
           "  %x = load i64, ptr %slot\n");
    expect("load align",
           written([](LlvmBuilder& b) { b.load("%x", float_type, "%slot", Align::four); }),
           "  %x = load float, ptr %slot, align 4\n");
    expect("store", written([](LlvmBuilder& b) { b.store({ptr, "null"}, "%local.x", Align::none); }),
           "  store ptr null, ptr %local.x\n");
    expect("store align",
           written([](LlvmBuilder& b) { b.store({double_type, "%d"}, "%box", Align::one); }),
           "  store double %d, ptr %box, align 1\n");
    expect("store align 8", written([](LlvmBuilder& b) { b.store({i64, "%n"}, "%s", Align::eight); }),
           "  store i64 %n, ptr %s, align 8\n");
    expect("store align 2", written([](LlvmBuilder& b) { b.store({i16, "%h"}, "%s", Align::two); }),
           "  store i16 %h, ptr %s, align 2\n");
    expect("store align 16", written([](LlvmBuilder& b) { b.store({i64, "%n"}, "%s", Align::sixteen); }),
           "  store i64 %n, ptr %s, align 16\n");
    expect("alloca", written([](LlvmBuilder& b) { b.alloca_slot("%local.x", ptr, Align::none); }),
           "  %local.x = alloca ptr\n");
    expect("alloca align",
           written([](LlvmBuilder& b) {
               b.alloca_slot("%scratch.0", LlvmType::array(3, i64), Align::eight);
           }),
           "  %scratch.0 = alloca [3 x i64], align 8\n");
    expect("phi",
           written([](LlvmBuilder& b) { b.phi("%s", ptr, {{"%a", "proven.1"}, {"%b", "checked.2"}}); }),
           "  %s = phi ptr [ %a, %proven.1 ], [ %b, %checked.2 ]\n");
    expect("extractvalue",
           written([](LlvmBuilder& b) { b.extractvalue("%v", LlvmType::structure(i64, i1), "%pair", 1); }),
           "  %v = extractvalue { i64, i1 } %pair, 1\n");
    expect("call", written([](LlvmBuilder& b) { b.call("%r", ptr, "@quidra_alloc", {{i64, 16}}); }),
           "  %r = call ptr @quidra_alloc(i64 16)\n");
    expect("call without arguments",
           written([](LlvmBuilder& b) { b.call("%r", ptr, "@quidra_json_last_error_copy", {}); }),
           "  %r = call ptr @quidra_json_last_error_copy()\n");
    expect("call without result",
           written([](LlvmBuilder& b) {
               b.call(void_type, "@quidra_managed_release", {{ptr, "%v"}, {ptr, "null"}});
           }),
           "  call void @quidra_managed_release(ptr %v, ptr null)\n");
    expect("call ignoring its result",
           written([](LlvmBuilder& b) { b.call(ptr, "@memset", {{ptr, "%a"}, {i32, 0}, {i64, 24}}); }),
           "  call ptr @memset(ptr %a, i32 0, i64 24)\n");
    expect("call with attributes",
           written([](LlvmBuilder& b) {
               b.call(LlvmCall(i8, "@ext")
                          .result("%v9")
                          .result_attributes("signext ")
                          .argument(ptr, " nocapture nonnull readonly", "%s")
                          .argument({i64, "%n"}));
           }),
           "  %v9 = call signext i8 @ext(ptr nocapture nonnull readonly %s, i64 %n)\n");
    expect("debug declare",
           written([](LlvmBuilder& b) { b.debug_declare({ptr, "%local.x"}, "!7", "!DIExpression()"); }),
           "  call void @llvm.dbg.declare(metadata ptr %local.x, metadata !7, "
           "metadata !DIExpression())\n");
    expect("br", written([](LlvmBuilder& b) { b.br("done.4"); }), "  br label %done.4\n");
    expect("br i1", written([](LlvmBuilder& b) { b.br("%c", "yes.1", "no.2"); }),
           "  br i1 %c, label %yes.1, label %no.2\n");
    const std::vector<LlvmCase> cases{{0, "case.1"}, {1, "case.2"}};
    expect("switch",
           written([&cases](LlvmBuilder& b) { b.switch_on({i64, "%tag"}, "invalid.0", cases); }),
           "  switch i64 %tag, label %invalid.0 [ i64 0, label %case.1 i64 1, label %case.2 ]\n");
    expect("switch without cases",
           written([](LlvmBuilder& b) { b.switch_on({i64, "%tag"}, "done", {}); }),
           "  switch i64 %tag, label %done [ ]\n");
    expect("ret", written([](LlvmBuilder& b) { b.ret({i64, "%v0"}); }), "  ret i64 %v0\n");
    expect("ret void", written([](LlvmBuilder& b) { b.ret_void(); }), "  ret void\n");
    expect("unreachable", written([](LlvmBuilder& b) { b.unreachable(); }), "  unreachable\n");
}

void functions() {
    expect("define",
           written([](LlvmBuilder& b) {
               b.define(LlvmFunction(i64, "@n_count")
                            .parameter(i64, "", "%arg.n")
                            .parameter(ptr, " nocapture nonnull readonly", "%arg.s")
                            .attribute("alwaysinline")
                            .debug_subprogram(12));
           }),
           "define i64 @n_count(i64 %arg.n, ptr nocapture nonnull readonly %arg.s) alwaysinline !dbg !12 {\n");
    expect("define without parameters",
           written([](LlvmBuilder& b) { b.define(LlvmFunction(void_type, "@n_run")); }),
           "define void @n_run() {\n");
    expect("end of a function", written([](LlvmBuilder& b) { b.end_function(); }), "}\n\n");
    expect("declare",
           written([](LlvmBuilder& b) {
               b.declare(LlvmFunction(i8, "@ext")
                             .result_attributes("signext ")
                             .parameter(ptr, " nocapture nonnull readonly", "")
                             .parameter(i64, "", "")
                             .parameter(i16, " signext", ""));
           }),
           "declare signext i8 @ext(ptr nocapture nonnull readonly, i64, i16 signext)\n\n");
}

void function_text() {
    FunctionText text;
    LlvmBuilder builder(text);
    builder.block("entry");
    const auto before = text.offset();
    builder.ret_void();
    expect("offset", std::to_string(before) + " " + std::to_string(text.offset()), "7 18");
    text.append(std::string_view());
    expect("empty piece", std::to_string(text.offset()), "18");
}

void escapes() {
    std::string escaped;
    append_escaped(escaped, std::string_view("a\"b\\c\n\x01\xff z", 10));
    expect("escape bytes", escaped, "a\\22b\\5Cc\\0A\\01\\FF z");
    expect("escape metadata", escape_metadata("dir\\main \"x\".qui"), "dir\\5Cmain \\22x\\22.qui");
}

// Constant text written at compile time is the text written at run time.
void constant_text() {
    std::string line;
    append_string_constant(line, ".fmt", "\"%s\"\n");
    expect("string constant", line,
           "@.fmt = private unnamed_addr constant [6 x i8] c\"\\22%s\\22\\0A\\00\"\n");
    std::string empty;
    append_string_constant(empty, ".e", "");
    expect("empty string constant", empty, "@.e = private unnamed_addr constant [1 x i8] c\"\\00\"\n");
    constexpr auto fixed = [] {
        FixedText<96> text;
        append_string_constant(text, ".fmt", "\"%s\"\n");
        return text;
    }();
    expect("string constant at compile time", std::string(fixed.view()), line);
    TextSize size;
    append_string_constant(size, ".fmt", "\"%s\"\n");
    expect("text size", std::to_string(size.size()), std::to_string(line.size()));
    constexpr auto numbers = [] {
        FixedText<48> text;
        append_decimal(text, static_cast<long long>(INT64_MIN));
        text.append(" ");
        append_decimal(text, ~0ULL);
        text.append(" ");
        append_decimal(text, 0);
        return text;
    }();
    expect("decimal at compile time", std::string(numbers.view()),
           "-9223372036854775808 18446744073709551615 0");
}

void module_sections() {
    LlvmModule module;
    module.append(LlvmModule::Section::metadata, "!0 = !{}\n");
    module.append(LlvmModule::Section::bodies, "define void @n_run() {\n}\n\n");
    module.append(LlvmModule::Section::helpers, "define void @helper() {\n}\n\n");
    module.string_constant(".str.0", "a\"b\n");
    module.private_constant(".src.0", {{ptr, LlvmOperand::global(".str.0")}, {i64, 7}});
    module.internal_global(".flag", {i1, "false"});
    module.append(LlvmModule::Section::header, "source_filename = \"main.qui\"\n");
    module.append(LlvmModule::Section::prelude, "declare void @f()\n");
    expect("module", module.str(),
           "; Quidra 0.1 generated LLVM IR\n"
           "source_filename = \"main.qui\"\n"
           "@.flag = internal global i1 false\n"
           "declare void @f()\n"
           "@.str.0 = private unnamed_addr constant [5 x i8] c\"a\\22b\\0A\\00\"\n"
           "@.src.0 = private constant { ptr, i64 } { ptr @.str.0, i64 7 }\n"
           "\n"
           "define void @helper() {\n}\n\n"
           "define void @n_run() {\n}\n\n"
           "!0 = !{}\n");
}

// The signature llvm_callee derives from a C function type, a call through
// a callee, and the checks of its arguments.
void callees() {
    constexpr LlvmCallee mixed = llvm_callee<char* (*)(bool, signed char, short, int, long long,
                                                       unsigned long long, float, double,
                                                       const void*)>("mixed");
    std::string signature;
    mixed.result().print(signature);
    for (const auto& parameter : mixed.parameters()) {
        signature += " ";
        parameter.print(signature);
    }
    expect("callee signature", signature, "ptr i1 i8 i16 i32 i64 i64 float double ptr");
    constexpr LlvmCallee none = llvm_callee<void (*)()>("none");
    expect("callee without parameters",
           std::to_string(none.parameters().size()) + " " + printed(none.result()), "0 void");
    expect("callee call", written([&](LlvmBuilder& b) {
               b.call("%r", mixed,
                      {{i1, "1"}, {i8, 2}, {i16, 3}, {i32, 4}, {i64, 5}, {i64, 6U}, {float_type, "0x0"},
                       {double_type, "0x0"}, {ptr, "null"}});
           }),
           "  %r = call ptr @mixed(i1 1, i8 2, i16 3, i32 4, i64 5, i64 6, float 0x0, double 0x0, "
           "ptr null)\n");
    expect("callee call without result", written([&](LlvmBuilder& b) { b.call(none, {}); }),
           "  call void @none()\n");
    const auto rejected = [&](auto operation) {
        try {
            written(operation);
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    expect("callee argument count",
           rejected([&](LlvmBuilder& b) { b.call(none, {{i64, 1}}); }) ? "rejected" : "accepted",
           "rejected");
    expect("callee argument type",
           rejected([&](LlvmBuilder& b) {
               b.call("%r", mixed,
                      {{i1, "1"}, {i8, 2}, {i16, 3}, {i64, 4}, {i64, 5}, {i64, 6}, {float_type, "0x0"},
                       {double_type, "0x0"}, {ptr, "null"}});
           }) ? "rejected" : "accepted",
           "rejected");
    expect("callee result name without result",
           rejected([&](LlvmBuilder& b) { b.call("%r", none, {}); }) ? "rejected" : "accepted",
           "rejected");

    // A variadic callee: its declaration ends in ..., a call names its type
    // and passes further arguments of any type.
    constexpr LlvmCallee format = llvm_callee<int (*)(const char*, ...)>("format");
    constexpr LlvmCallee any = llvm_callee<void (*)(...)>("any");
    std::string declarations;
    append_declaration(declarations, format);
    append_declaration(declarations, any);
    expect("variadic declarations", declarations,
           "declare i32 @format(ptr, ...)\ndeclare void @any(...)\n");
    expect("variadic call", written([&](LlvmBuilder& b) {
               b.call(format, {{ptr, LlvmOperand::global(".fmt.string.raw")}, {ptr, "%t"}});
               b.call("%n", format, {{ptr, "@.fmt"}});
           }),
           "  call i32 (ptr, ...) @format(ptr @.fmt.string.raw, ptr %t)\n"
           "  %n = call i32 (ptr, ...) @format(ptr @.fmt)\n");
    expect("variadic callee without its fixed arguments",
           rejected([&](LlvmBuilder& b) { b.call(format, {}); }) ? "rejected" : "accepted",
           "rejected");
    const auto wrong_fixed_argument = [&](LlvmBuilder& b) {
        b.call(format, {{i64, 1}, {ptr, "%t"}});
    };
    expect("variadic callee with a wrong fixed argument",
           rejected(wrong_fixed_argument) ? "rejected" : "accepted", "rejected");
}

} // namespace

int main() {
    types_and_operands();
    operations();
    functions();
    function_text();
    escapes();
    constant_text();
    module_sections();
    callees();
    if (failures) {
        std::fprintf(stderr, "llvm text tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("llvm text tests: ok\n");
    return 0;
}
