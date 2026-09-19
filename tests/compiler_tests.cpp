#include "quidra/compiler.hpp"
#include "quidra/ir.hpp"
#include "quidra/source_tools.hpp"
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
static void good(const std::string& s) {
    try {
        auto c=quidra::compile(s);
        (void)quidra::inspect_source_json(s,c.checked,"test.qui");
    } catch(const quidra::CompileErrors& e) {
        std::cerr<<"unexpected rejection: "<<e.what()<<"\n";
        for(const auto& d:e.diagnostics()) {
            std::cerr<<d.span.start.line<<":"<<d.span.start.column
                     <<" ["<<d.code<<"] "<<d.message<<"\n";
        }
        std::cerr<<s;
        std::exit(1);
    } catch(const quidra::CompileError& e) {
        const auto& d=e.diagnostic();
        std::cerr<<"unexpected rejection: "<<d.span.start.line<<":"<<d.span.start.column
                 <<" ["<<d.code<<"] "<<d.message<<"\n"<<s;
        std::exit(1);
    } catch(const std::exception& e){
        std::cerr<<"unexpected rejection: "<<e.what()<<"\n"<<s;
        std::exit(1);
    }
}
static void bad(const std::string& s) { try {(void)quidra::compile(s);}catch(const quidra::CompileErrors&){return;}catch(const quidra::CompileError&){return;}std::cerr<<"unexpected acceptance:\n"<<s;std::exit(1); }
static void inspect_contains(const std::string& s, const std::string& expected) {
    try {
        auto c = quidra::compile(s);
        const auto json = quidra::inspect_source_json(s, c.checked, "test.qui");
        if (json.find(expected) != std::string::npos) return;
        std::cerr << "inspect output missing expected fragment: " << expected << "\n" << json << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected inspect rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void inspect_no_source_contains(const std::string& s, const std::string& expected) {
    try {
        auto c = quidra::compile(s);
        quidra::InspectOptions options;
        options.include_source = false;
        const auto json = quidra::inspect_source_json(s, c.checked, "test.qui", options);
        if (json.find("\"source\":") != std::string::npos) {
            std::cerr << "inspect --no-source unexpectedly emitted source text\n" << json << "\n";
            std::exit(1);
        }
        if (json.find(expected) != std::string::npos) return;
        std::cerr << "inspect --no-source output missing expected fragment: " << expected << "\n"
                  << json << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected inspect --no-source rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void repl_ir_contains(
    const std::string& source, std::size_t replay_prefix_bytes,
    const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "repl_ir_contains.qui";
    try {
        std::filesystem::create_directories(root);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        auto c = quidra::compile_repl_file_source(
            path, source, {}, root, replay_prefix_bytes);
        if (std::filesystem::exists(path)) {
            std::cerr << "REPL root-source overlay unexpectedly created a source file\n";
            std::exit(1);
        }
        const auto dumped = quidra::ir::dump(c.compilation.ir);
        if (dumped.find(expected) != std::string::npos) return;
        std::cerr << "REPL typed IR output missing expected fragment: " << expected << "\n"
                  << dumped << "\n";
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected REPL IR dump rejection: " << e.what() << "\n" << source;
    }
    std::exit(1);
}

static void ir_contains(const std::string& s, const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "ir_contains.qui";
    try {
        std::filesystem::create_directories(root);
        {
            std::ofstream out(path, std::ios::binary);
            if (!out) throw std::runtime_error("cannot create temporary Quidra source");
            out << s;
        }
        auto c = quidra::compile_file(path, {}, root);
        std::filesystem::remove(path);
        const auto dumped = quidra::ir::dump(c.ir);
        if (dumped.find(expected) != std::string::npos) return;
        std::cerr << "typed IR output missing expected fragment: " << expected << "\n"
                  << dumped << "\n";
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected IR dump rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_contains(const std::string& s, const std::string& expected) {
    try {
        const auto c = quidra::compile(s);
        if (c.llvm.find(expected) != std::string::npos) return;
        std::cerr << "LLVM output missing expected fragment: " << expected << "\n"
                  << c.llvm << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_not_contains(const std::string& s, const std::string& unexpected) {
    try {
        const auto c = quidra::compile(s);
        if (c.llvm.find(unexpected) == std::string::npos) return;
        std::cerr << "LLVM output unexpectedly contains fragment: " << unexpected << "\n"
                  << c.llvm << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_file_contains(const std::string& s, const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "llvm_file_contains.qui";
    try {
        std::filesystem::create_directories(root);
        {
            std::ofstream out(path, std::ios::binary);
            if(!out) throw std::runtime_error("cannot create temporary Quidra source");
            out << s;
        }
        const auto c = quidra::compile_file(path, {}, root);
        std::filesystem::remove(path);
        if(c.llvm.find(expected) != std::string::npos) return;
        std::cerr << "file-aware LLVM output missing expected fragment: " << expected << "\n"
                  << c.llvm << "\n";
    } catch(const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected file-aware LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void bad_code(const std::string& s, const std::string& expected) {
    try { (void)quidra::compile(s); }
    catch (const quidra::CompileErrors& errors) {
        for (const auto& d : errors.diagnostics()) if (d.code == expected) return;
        std::cerr<<"wrong diagnostic code, expected "<<expected<<", got";
        for (const auto& d : errors.diagnostics()) std::cerr<<" "<<d.code;
        std::cerr<<"\n"<<s;
        std::exit(1);
    }
    catch (const quidra::CompileError& error) {
        if (error.diagnostic().code == expected) return;
        std::cerr<<"wrong diagnostic code, expected "<<expected<<" but got "<<error.diagnostic().code<<"\n"<<s; std::exit(1);
    }
    std::cerr<<"unexpected acceptance, expected "<<expected<<":\n"<<s; std::exit(1);
}
static void bad_message(const std::string& s, const std::string& code,
                        const std::string& fragment) {
    try { (void)quidra::compile(s); }
    catch (const quidra::CompileErrors& errors) {
        for (const auto& d : errors.diagnostics()) {
            if (d.code == code && d.message.find(fragment) != std::string::npos) return;
        }
        std::cerr << "missing diagnostic " << code << " containing '" << fragment << "'\n" << s;
        std::exit(1);
    }
    catch (const quidra::CompileError& error) {
        const auto& d = error.diagnostic();
        if (d.code == code && d.message.find(fragment) != std::string::npos) return;
        std::cerr << "missing diagnostic " << code << " containing '" << fragment << "'\n" << s;
        std::exit(1);
    }
    std::cerr << "unexpected acceptance, expected " << code << " containing '" << fragment
              << "':\n" << s;
    std::exit(1);
}
static void root_source_override_with_import() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-root-source-override";
    const auto main_path = root / "main.qui";
    const auto lib_path = root / "lib.qui";
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            std::ofstream out(lib_path, std::ios::binary);
            out << "int doubled(int value)\n    return value * 2\n";
        }
        {
            std::ofstream out(main_path, std::ios::binary);
            out << "print(unknown_name)\n";
        }
        const std::string source =
            "import lib = \"./lib.qui\"\n"
            "int value = lib.doubled(21)\n"
            "print(value)\n";
        (void)quidra::check_file_source(main_path, source, {}, root);
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "root source override with import failed: " << e.what() << "\n";
        std::exit(1);
    }
}


static void string_input_ignores_package_lock() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-string-package-lock";
    const auto original = std::filesystem::current_path();
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            std::ofstream out(root / "quidra.lock", std::ios::binary);
            if (!out) throw std::runtime_error("cannot create temporary package lock");
            out << "quidra-lock-v1\n"
                << "dnn 0000000000000000000000000000000000000000000000000000000000000000\n";
        }
        std::filesystem::current_path(root);
        (void)quidra::check("print(int(1))\n");
        (void)quidra::compile("print(int(1))\n");
        std::filesystem::current_path(original);
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::current_path(original, ignored);
        std::filesystem::remove_all(root, ignored);
        std::cerr << "string input package lock isolation failed: " << e.what() << "\n";
        std::exit(1);
    }
}

int main(){
 good(R"(bigint huge = 12345678901234567890123456789012345678901234567890
bigint one = 1
bigint sum = huge + one
print(sum)
)");
 good(R"(bigreal root = math.sqrt(2.0)
bigreal circle = math.pi
bigreal natural = math.e
bigreal ratio = bigreal(1) / bigreal(3)
print(root)
print(circle)
print(natural)
print(ratio)
)");
 good(R"(T identity<T>(T value)
    return value

bigint large = identity<bigint>(123456789012345678901234567890)
bigreal exact = identity<bigreal>(math.sqrt(2.0))
print(large)
print(exact)
)");
 good(R"(bigint[] values = [
    123456789012345678901234567890,
    2,
]
bigreal[] reals = bigreal(values)
print(values[0])
print(reals[1])
)");
 llvm_contains(
     "bigint x = 123456789012345678901234567890\n",
     "call ptr @quidra_bigint_literal");
 llvm_contains(
     "bigreal x = math.sqrt(2.0)\n",
     "call ptr @quidra_bigreal_sqrt");
 llvm_contains(
     "bigreal x = math.pi\n",
     "call ptr @quidra_bigreal_literal");
 bad_code("tensor<bigint> x = tensor<bigint>([1])\n", "INVALID_TYPE");
 bad_code("neural<bigreal> x = neural.track(tensor<float>([1]))\n", "INVALID_TYPE");
 bad_code("neural<bigint> x = neural.track(tensor<float>([1]))\n", "INVALID_TYPE");
 bad_code("auto x = math.sqrt(4.0)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("int x = int(math.sqrt(4.0))\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good("bigreal x = math.sqrt(4.0)\nint y = int(x)\n");
 good("bigreal x = math.sqrt(2.0)\nprint(\"{x:sig=100}\")\n");
 bad_code("map.Map<bigreal, int> values = map.Map<bigreal, int>()\n", "STANDARD_KEY_TYPE");
 bad_code("set.Set<bigreal> values = set.Set<bigreal>()\n", "STANDARD_KEY_TYPE");
 llvm_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\nauto value = values.get(1)\n",
     "call ptr @quidra_format_signed");
 llvm_not_contains(
     "set.Set<uint64> values = set.Set<uint64>()\nuint64 key = uint64(7)\nvalues.add(key)\nbool present = values.has(key)\n",
     "call ptr @quidra_format_unsigned");
 llvm_not_contains(
     "map.Map<bigint, int> values = map.Map<bigint, int>()\nbigint key = bigint(7)\nvalues.set(key, 2)\nauto value = values.get(key)\n",
     "call ptr @quidra_bigint_text");

 good(R"(uint8 a = 240
uint8 b = 204
uint8 both = a AND b
uint8 either = a OR b
uint8 different = a XOR b
uint8 inverted = NOT a
uint8 left = a << 1
uint8 right = a >> 2
int8 signed_value = -8
int8 signed_right = signed_value >> 2
print(both)
print(either)
print(different)
print(inverted)
print(left)
print(right)
print(signed_right)
)");
 llvm_contains("uint8 a = 3\nuint8 b = a << 2\n", "shl i8");
 llvm_contains("uint8 a = 3\nuint8 b = NOT a\n", "xor i8");
 good("uint8 inverted = NOT 1\n");
 good("uint8 flags = 12\nuint8 mask = 10\nbool selected = flags AND mask == 8\n");
 good("uint8 flags = 1\nuint8 mask = 2\nbool selected = flags OR mask == 3\n");
 bad_code("bool a = true\nbool b = false\nbool c = a AND b\n", "TYPE_MISMATCH");
 bad_code("float a = 1.0\nfloat b = 2.0\nfloat c = a OR b\n", "TYPE_MISMATCH");
 bad_code("bigint a = 1\nbigint b = 2\nbigint c = a XOR b\n", "TYPE_MISMATCH");
 bad_code("uint8 a = 1\nuint8 b = a << 8\n", "SHIFT_COUNT");
 good(R"(class NestedState
    neural.State<tensor<float32>> value
)");

 good("extern void scalar_abi(int8 a, int16 b, int32 c, int d, uint8 e, uint16 f, uint32 g, uint64 h, float32 i, float j, bool k) = \"scalar_abi\"\n");
 llvm_contains("extern bool c_bool(bool value) = \"c_bool\"\n", "declare zeroext i1 @c_bool(i1 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\n", "declare signext i8 @c_i8(i8 signext)");
 llvm_contains("extern int16 c_i16(int16 value) = \"c_i16\"\n", "declare signext i16 @c_i16(i16 signext)");
 llvm_contains("extern uint8 c_u8(uint8 value) = \"c_u8\"\n", "declare zeroext i8 @c_u8(i8 zeroext)");
 llvm_contains("extern uint16 c_u16(uint16 value) = \"c_u16\"\n", "declare zeroext i16 @c_u16(i16 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\nint8 x = 1\nint8 y = c_i8(x)\n", "call signext i8 @c_i8(i8 signext");
 good("extern int32 c_text(const string &text) = \"c_text\"\n");
 good("extern int32 c_bin(const bin &data) = \"c_bin\"\n");
 ir_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "function c_text(const string &text) -> int32 = \"c_text\"");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "declare i32 @c_text(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\n", "declare i32 @c_bin(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\n", "declare i32 @c_bin_mut(ptr nocapture nonnull, i64)");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "ffi.borrowed.value");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "call i64 @strlen(ptr");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "ffi.bin.length");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "call i32 @c_bin(ptr nocapture nonnull readonly");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin_mut(&value)\n", "call i32 @c_bin_mut(ptr nocapture nonnull");
 good("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n");
 llvm_contains("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n", "@quidra_bin_try_utf8");
 bad_code("auto text = string.from_utf8(\"not bin\")\n", "TYPE_MISMATCH");
 llvm_file_contains("tensor<float> source = tensor.ones<float>([1])\nneural<float> value = neural.track(source)\nneural<float> next = value + 1.0\n", "@quidra_neural_binary_scalar");
 llvm_file_contains("tensor<float32> source = tensor.ones<float32>([1])\nneural<float32> value = neural.track(source)\nuint8 scalar = 255\nneural<float32> next = value + float32(scalar)\n", "uitofp i8");
 llvm_file_contains("tensor<float32> source = tensor.ones<float32>([1])\nneural<float32> value = neural.track(source)\nint8 scalar = -1\nneural<float32> next = value + float32(scalar)\n", "sitofp i8");

 bad_code("extern int32 implicit_text(string text) = \"implicit_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 implicit_bytes(bin data) = \"implicit_bytes\"\n", "FFI_REFERENCE");
 good("extern int32 mutable_bytes(bin &data) = \"mutable_bytes\"\n");
 good("extern int c_apply(fn<int>(int) callback, int value) = \"c_apply\"\n");
 llvm_contains("extern int c_apply(fn<int>(int) callback, int value) = \"c_apply\"\n", "declare i64 @c_apply(ptr, i64)");
 llvm_contains(R"(int twice(int value)
    return value * 2
extern int c_apply(fn<int>(int) callback, int value) = "c_apply"
int result = c_apply(twice, 21)
)", "call i64 @c_apply(ptr");
 bad_code("extern int unsafe_callback(fn<int8>(int8) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int unsafe_callback(fn<string>(int) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int32 mutable_text(string &text) = \"mutable_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 const_value(const string text) = \"const_value\"\n", "FFI_REFERENCE");
 bad_code("extern int32 c_puts(const string &text) = \"puts\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_main(int32 value) = \"main\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_mangled(int32 value) = \"n_user_function\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_runtime(int32 value) = \"quidra_future_runtime_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_internal(int32 value) = \"__quidra_internal_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 first(int32 value) = \"shared_symbol\"\nextern int32 second(int32 value) = \"shared_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern string unsafe(int value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern bin unsafe_bin(int value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern int unsafe(int &value) = \"unsafe_symbol\"\n", "FFI_REFERENCE");
 bad_code("extern int unsafe(int value = 1) = \"unsafe_symbol\"\n", "FFI_DEFAULT");
 bad_code("extern int unsafe(int value) = \"bad-symbol\"\n", "FFI_SYMBOL");

 good(R"(int twice(int value)
    return value * 2

int apply(fn<int>(int) operation, int value)
    return operation(value)

fn<int>(int) operation = twice
int a = operation(3)
int b = apply(operation, 4)
print(a)
print(b)
)");
 ir_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "fn.ref twice");
 ir_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "call.indirect");
 llvm_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "select i1 true, ptr @n_twice, ptr null");
 bad_code(R"(int twice(int value)
    return value * 2
fn<float>(int) operation = twice
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(int mutate(int &value)
    value = value + 1
    return value
fn<int>(int) operation = mutate
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(extern int32 foreign(int32 value) = "foreign"
fn<int32>(int32) operation = foreign
)", "FUNCTION_REFERENCE_EXTERN");
 bad_code(R"(int twice(int value)
    return value * 2
auto operation = twice
)", "FUNCTION_REFERENCE_CONTEXT");
 bad_code(R"(int twice(int value)
    return value * 2
fn<int>(int) left = twice
fn<int>(int) right = twice
bool same = left == right
)", "TYPE_MISMATCH");
 good(R"(void first()
    print("first")

void second()
    print("second")

task.all([first, second])
task.all([])
)");
 llvm_contains(R"(void first()
    print("first")
task.all([first])
)", "call void @quidra_task_all");
 llvm_contains(R"(void first()
    print("first")
task.all([first])
)", "@.quidra.stack.depth = internal thread_local global i64 0");
 bad_code(R"(int wrong()
    return 1
task.all([wrong])
)", "FUNCTION_REFERENCE_SIGNATURE");

 bad_code(R"(fn<int>(int) operation
int result = operation(1)
)", "UNINITIALIZED");
 good(R"(int twice(int value)
    return value * 2
int apply_ref(const fn<int>(int) &operation, int value)
    return operation(value)
fn<int>(int) operation = twice
int result = apply_ref(&operation, 4)
print(result)
)");

 root_source_override_with_import();
 string_input_ignores_package_lock();
 {
  const auto compile_chain=[](std::string source,const std::string& term){
   for(int index=1;index<64;++index) source+=" + "+term;
   source+="\n";
   const auto root=std::filesystem::temp_directory_path()/"quidra-compiler-tests";
   const auto path=root/"deep-binary.qui";
   try {
    std::filesystem::create_directories(root);
    {
     std::ofstream out(path,std::ios::binary);
     if(!out) throw std::runtime_error("cannot create deep binary test source");
     out<<source;
    }
    (void)quidra::compile_file(path,{},root);
    std::filesystem::remove(path);
   } catch(const std::exception& e) {
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    std::cerr<<"unexpected deep binary rejection: "<<e.what()<<"\n"<<source;
    std::exit(1);
   }
  };
  compile_chain("string scalar_chain = \"x\"","\"x\"");
  compile_chain(
      "tensor<float32> tensor_value = tensor<float32>([1])\n"
      "tensor_value[0] = 1.0\n"
      "tensor<float32> tensor_chain = tensor_value",
      "tensor_value");
  compile_chain(
      "tensor<float32> neural_source = tensor<float32>([1])\n"
      "neural_source[0] = 1.0\n"
      "neural neural_value = neural.track(neural_source)\n"
      "neural neural_chain = neural_value",
      "neural_value");
 }
 for(const auto& s:std::vector<std::string>{
 R"(void replace(int[] &x, int count = 3)
    x = array(count, fill = 7)
int[] y = []
replace(&x = &y)
print(y[2])
)",
 R"(int | error read(bool ok)
    if ok
        return 1
    return error("bad")
string | int | error use(bool ok)
    auto x = try read(ok)
    return x + 1
int | error value = read(true)
match value
    int
        print(value + 1)
    error e
        print(e)
)",
 R"(void | error f()
    return
void | error g()
    try f()
    return
int | none x = none
match x
    none
        print("none")
    int value
        print(value)
)",
 R"(int x
if true
    x = 1
else
    x = 2
print(x)
int[2][2] matrix = [[1, 2], [3, 4]]
int[][] copy = matrix
copy[0][0] = 9
)",
 R"(int f(bool yes)
    int x
    if yes
        return 1
    else
        x = 2
    return x
int[] a = [
  1,
  2,
]
print(f(yes = true,))
)",
 R"(int[] values = array(2, fill = 0)
for &value in values
    value = 7
for i in range(0, 3, step = 1)
    print(i)
)",
 R"(int f(int[] a = [1])
    a[0] = a[0] + 1
    return a[0]
print(f())
print(f())
)",
 R"(auto path = "C:\Users\data\image.png"
auto raw = "\n\t\r\b\f\v\a\u3042"
auto controls = enter + tab + home + quote + backspace + page + vtab + bell
string separator = tab
print("A{separator}B")
print("nested {error("ok")}")
)",
 R"(class Point
    int x
    int y

    int sum()
        return x + y

class LabeledPoint : Point
    string label

class OffsetPoint : Point
    int offset

    override int sum()
        return x + y + offset

class Box
    Point point

Point a = Point(x = 1, y = 2)
Point b = a
b.x = 9
Point &alias = &a
alias.y = 5
LabeledPoint labeled = LabeledPoint(x = 3, y = 4, label = "p")
OffsetPoint offset = OffsetPoint(x = 3, y = 4, offset = 10)
Box box = Box(point = a)
Box copy = box
copy.point.x = 99
print(a.sum())
print(labeled.sum())
print(offset.sum())
print(box.point.x)
)",
 R"(int x
int &y = &x
y = 10
print(x)
)",
 R"(void initialize(int &x)
    x = 7
int x
initialize(&x)
print(x)
)",
 R"(void accept_address(int &x)
    return
int x
accept_address(&x)
)",
 R"(int a = 1
int c = 2
int &b = &a
b = 5
&b = &c
b = 8
print(a)
print(c)
)",
 R"(class PartialPoint
    int x
    int y

PartialPoint p = PartialPoint()
p.x = 1
print(p.x)
PartialPoint q = p
q.y = 5
print(q.x)
print(q.y)
)",
 R"(int local_scope_value()
    int x = 5
    return x

int x = 7
print(local_scope_value())
print(x)
)",
 R"(class ResetCounter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

ResetCounter counter = ResetCounter()
counter.reset()
counter.increment()
print(counter.value)
)",
 R"(class RefPoint
    int x
    int y

RefPoint p = RefPoint(x = 1)
int &field = &p.y
field = 5
print(p.y)
)",
 R"(int[] values = [1, 2, 3]
int &first = &values[0]
first = 9
values = [4, 5]
print(first)
print(values[0])
)",
 R"(class DefaultPoint
    int x = 1
    int y = 2

    int sum()
        return x + y

class DefaultChild : DefaultPoint
    override int sum()
        return super.sum() + 1

DefaultPoint a = DefaultPoint()
DefaultPoint b = DefaultPoint(x = 1)
bool same = a == b
int[] left = [1, 2, 3]
int[] right = [1, 2, 3]
bool arrays_same = left == right
DefaultChild child = DefaultChild()
print(a.sum())
print(child.sum())
print(same)
print(arrays_same)
)",
 R"(class DefaultBag
    int[] values = [1]

DefaultBag a = DefaultBag()
DefaultBag b = DefaultBag()
a.values[0] = 9
print(b.values[0])
)",
 R"(class Box<T>
    T value

    T get()
        return value

T first<T>(T[] values)
    return values[0]

Box<int> box = Box<int>(value = 7)
print(box.get())
print(first<int>([4, 5]))
)",
 R"(class GenericBase<T>
    T value

    T read()
        return value

class GenericChild<T> : GenericBase<T>
    override T read()
        return super.read()

GenericChild<int> child = GenericChild<int>(value = 9)
print(child.read())
)",
 R"(class GenericMethod
    T identity<T>(T value)
        return value

GenericMethod g = GenericMethod()
print(g.identity<int>(8))
)",
 R"(class GenericParent
    T echo<T>(T value)
        return value

class GenericChild : GenericParent
    override T echo<T>(T value)
        return super.echo<T>(value)

GenericChild child = GenericChild()
print(child.echo<int>(11))
)",
 R"(class NestedBox<T>
    T value

NestedBox<NestedBox<int>> outer = NestedBox<NestedBox<int>>(
    value = NestedBox<int>(value = 12),
)
print(outer.value.value)
)",
 R"(class OneArgMethod
    T choose<T>(T value)
        return value

class TwoArgMethod
    T choose<T, U>(T value, U ignored)
        return value

OneArgMethod one = OneArgMethod()
print(one.choose<int>(13))
)",
 R"(class GoodGenericMethod
    T route<T>(T value)
        return value

class UnusedGenericMethod
    T route<T>(T value)
        return value + 1

GoodGenericMethod good = GoodGenericMethod()
print(good.route<string>("ok"))
)",
 R"(class GenericRouter
    T pass<T>(T value)
        return value

class RouterFactory
    GenericRouter make()
        return GenericRouter()

    string run()
        return make().pass<string>("method")

GenericRouter make_router()
    return GenericRouter()

string use_router()
    return make_router().pass<string>("function")

auto router = GenericRouter()
GenericRouter[] routers = [GenericRouter()]
print(router.pass<string>("auto"))
print(routers[0].pass<int>(14))
print(RouterFactory().run())
print(use_router())
)",
 R"(class ReturnModel
    float bb

ReturnModel build_model()
    return ReturnModel(bb = 2.0)

ReturnModel model = build_model()
print(model.bb)
)",
 R"(class ReturnPoint
    int x
    int y

ReturnPoint choose_point(bool full)
    if full
        return ReturnPoint(x = 1, y = 2)
    return ReturnPoint(x = 3)

ReturnPoint point = choose_point(true)
print(point.x)
)",
 R"(class ForwardProduct
    int value

ForwardProduct outer_build()
    return inner_build()

ForwardProduct inner_build()
    return ForwardProduct(value = 8)

ForwardProduct forward = outer_build()
print(forward.value)
)",
 R"(class MethodProduct
    int value

class MethodFactory
    MethodProduct outer()
        return inner()

    MethodProduct inner()
        return MethodProduct(value = 9)

MethodFactory factory = MethodFactory()
MethodProduct product = factory.outer()
print(product.value)
)",
 R"(class NestedData
    float[] ys

class NestedOwner
    NestedData data

    float first()
        data.ys = [3.0]
        return data.ys[0]

    void initialize()
        data.ys = [4.0]

NestedOwner owner = NestedOwner(data = NestedData())
print(owner.first())
owner.initialize()
print(owner.data.ys[0])
)",
 R"(class ReplaceInner
    int x
    int y

class ReplaceOuter
    ReplaceInner inner

    void reset()
        inner = ReplaceInner(x = 5)

ReplaceOuter outer = ReplaceOuter(inner = ReplaceInner(x = 1, y = 2))
outer.reset()
print(outer.inner.x)
)",
 R"(class ConditionalInner
    int x
    int y

class ConditionalOuter
    ConditionalInner inner

    void maybe_reset(bool replace)
        if replace
            inner = ConditionalInner(x = 5)

ConditionalOuter outer = ConditionalOuter(inner = ConditionalInner(x = 1, y = 2))
outer.maybe_reset(false)
print(outer.inner.x)
)",
 R"(class RepairInner
    int x
    int y

class RepairOuter
    RepairInner inner

    void reset()
        inner = RepairInner(x = 5)
        inner.y = 6

RepairOuter outer = RepairOuter(inner = RepairInner(x = 1, y = 2))
outer.reset()
print(outer.inner.y)
)",
 R"(int denominator = 0
int result = 10 / denominator
)",
 R"(int8 small = 127
int16 wider = int16(small)
uint8 byte_value = 255
uint32 count = 100
int total = int(count)
float exact_float = 1.0
float32 exact_small_float = 1.5
int8 casted = int8(100)
int exact_from_float = 3
string small_text = small.string()
string flag_text = true.string()
int | error parsed = int.parse("123")
float32 | error parsed_float = float32.parse("1.5")
bin allocated = bin.fill(8, 0)
string repeated = string.repeat("a", 3)
bin data = bin.parse("0000000111111110")
bin first = data[0]
bin slice = data[0:8]
uint8[] decoded = uint8[](data)
bin copy = data
copy[0] = bin.parse("1")
bool same = data == copy
for value in data
    bin x = value
for &value in copy
    value = value
write(small_text)
)",
 R"(int local_name()
    int x = 5
    return x

int x = 7
print(local_name())
print(x)
)",
 "int x\nx = 4\nprint(x)\n", "auto x = int(41)\nprint(x)\n", "int end = 7\nprint(end)\n", "// comment only\nint x = 1 // trailing comment\nprint(x)\n"}) good(s);
 good("int exit = 7\nprint(exit)\n");

 ir_contains(R"(bin bits = bin.parse("01")
print(bits[0])
)", "release %");
 ir_contains(R"(bool flag = bool(bin.parse("1"))
print(flag)
)", "release %");
 ir_contains(R"(bin bits = bin.fill(1, 0)
bits[0] = bin.parse("1")
print(bits)
)", "release %");
 ir_contains(R"(bin bits = bin.parse("01")
for bit in bits
    print(bit)
)", "bin.get");

 good(R"(float scalar = 3.0
float32 scalar32 = 2.0
float[] values = [1.0, 2.0, 3.0]

float half(float value)
    return value / 2.0

float negative = -3.0
float product = 2.0 * 3.0
float argument = half(3.0)
int8 small = 5
int8 sum = small + 100

print(scalar)
print(scalar32)
print(values[2])
print(negative)
print(product)
print(argument)
print(sum)
)");

 good(R"(class Pair
    int a
    int b

Pair | error make_pair(bool ok)
    if not ok
        return error("bad")
    return Pair(a = 2, b = 3)

int | error pair_sum(bool ok)
    Pair pair = try make_pair(ok)
    return pair.a + pair.b

auto result = pair_sum(true)
match result
    int value
        print(value)
    error problem
        print(problem)
)");

 const std::string repl_replay_surface = "print(\"A\")\nprint(\"B\")\n";
 repl_ir_contains(
     repl_replay_surface, std::string("print(\"A\")\n").size(),
     "repl.replay on");
 repl_ir_contains(
     repl_replay_surface, std::string("print(\"A\")\n").size(),
     "repl.replay off");

 const std::string const_ir_surface = R"(void inspect(const int &value)
    print(value)

void modify(int &value)
    value = 1

int data = 0
inspect(&data)
modify(&data)
)";
 ir_contains(const_ir_surface, "function inspect(const int &value)");
 ir_contains(const_ir_surface, "function modify(int &value)");
 llvm_contains(const_ir_surface, "ptr nocapture nonnull readonly %arg.value");
 llvm_contains(const_ir_surface, "define void @n_modify(ptr nocapture nonnull %arg.value)");

 const std::string local_const_reference_ir = R"(int data = 1
const int &view = &data
int &writer = &data
const int frozen = 3
print(view)
writer = 2
print(frozen)
)";
 ir_contains(local_const_reference_ir, "const reference ");
 ir_contains(local_const_reference_ir, "reference.bind");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"read_only_reference\"");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"read_write_reference\"");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"const_value\"");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"read_only_reference\"}");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"read_write_reference\"}");
 bad_code(R"(int data = 1
const int &view = &data
int &writer = &view
)", "REFERENCE_BINDING");
 bad_code(R"(int data = 1
const int &view = &data
view = 2
)", "WRITE_CAPABILITY");
 bad_code(R"(int data = 1
int other = 2
const int &view = &data
&view = &other
)", "WRITE_CAPABILITY");

 const std::string ir_surface = R"(int source = 1
int &alias = &source
alias = 2
tensor<float32> a = tensor.zeros<float32>([2, 2])
tensor<float32> b = tensor.ones<float32>([2, 2])
tensor<float32> c = a + b
auto row = c[0, :]
float mean = stats.mean(row)
tensor<float32> product = linear.matmul(a, b)
auto decoded_image = image.read("input.png")
)";
 for (const auto& fragment : std::vector<std::string>{
          "reference ", "reference.bind", "reference.store",
          "tensor.create", "tensor.binary", "tensor.index",
          "stats.mean", "linear.matmul", "image.read"}) {
     ir_contains(ir_surface, fragment);
 }
 bad_code("auto loaded = image.read<uint8>(\"input.png\")\n", "GENERIC_TARGET");
 for(const auto& s:std::vector<std::string>{
 "break\n", "continue\n",
 "int x\nprint(x)\n",
 "int x = 10 / 0\n",
 "int x = 10 % (3 - 3)\n",
 "uint8 x = 10\nprint(x / uint8(0))\n",
 "float x = .5\n",
 R"(class PartialReturn
    int x
    int y

PartialReturn make_partial()
    return PartialReturn(x = 1)

PartialReturn p = make_partial()
print(p.y)
)",
 R"(class ReplaceInnerBad
    int x
    int y

class ReplaceOuterBad
    ReplaceInnerBad inner

    void reset()
        inner = ReplaceInnerBad(x = 5)

ReplaceOuterBad outer = ReplaceOuterBad(inner = ReplaceInnerBad(x = 1, y = 2))
outer.reset()
print(outer.inner.y)
)",
 R"(class ConditionalInnerBad
    int x
    int y

class ConditionalOuterBad
    ConditionalInnerBad inner

    void maybe_reset(bool replace)
        if replace
            inner = ConditionalInnerBad(x = 5)

ConditionalOuterBad outer = ConditionalOuterBad(inner = ConditionalInnerBad(x = 1, y = 2))
outer.maybe_reset(false)
print(outer.inner.y)
)",
 "int x\nif true\n    x = 1\nprint(x)\n", "int x\nwhile false\n    x = 1\nprint(x)\n",
 "auto x\n", "none x = none\n", "auto x = none\n", "none f()\n    return none\n", "never x\n", "void[] x = []\n", "unit f()\n    return\n", "auto x = unit\n",
 "int[2] x = [1]\n", "int[] x = [1]\nint[2] y = x\n", "int[-1] x\n", "int x[2]\n",
 "int f()\n    return\n", "int f(auto x)\n    return 1\n", "int main()\n    return 0\n",
 "void f()\n  return\n", "void f()\n\treturn\n", "void f()\n        return\n",
 "auto x = some(1)\n", "result<int, error> x = success(1)\n", "auto x = array(3)\n",
 "void f(int &x)\n    x = 2\nint x = 1\nf(x)\n", "void f(int x)\n    return\nint x = 1\nf(&x)\n",
 "int f(int x = 1, int y)\n    return y\n", "void f(int &x = 1)\n    return\n", "void f(const int &x = 1)\n    return\n", "int f(int x, int y = x)\n    return y\n",
 "int f(int x)\n    return x\nprint(f(x: 1))\n", "int f(int x, int y)\n    return x\nprint(f(x = 1, 2))\n",
 "int | none x = 1\nmatch x\n    int\n        print(x)\n", "int | none x = 1\nmatch x\n    int\n        print(x)\n    int y\n        print(y)\n    none\n        print(int(0))\n",
 "int | none x = 1\nmatch x\n    int\n        x = none\n    none\n        print(int(0))\n",
 "auto x = 9223372036854775808\n", "auto x = []\n", "auto x = range(3)\n",
 "int8 x = 128\n", "uint8 x = -1\n", "int8 x = int8(300)\n",
 "bin x = bin(2, fill = 0)\n", "bin x = bin.fill(-1, 0)\n", "bin x = bin.fill(2, 2)\n", "string x = string(2, fill = \"a\")\n", "string x = string.repeat(\"a\", -1)\n",
 "string tab = \"x\"\n", "void f(string enter)\n    return\n",
 "int x = 1\nif true\n    int x = 2\n", "int x = 1\nint x = 2\n",
 "class A\n    int x\n    int x\n",
 "class A\n    int x\n    void set(int x)\n        return\n",
 "class A\n    int value\n    int get()\n        return value\nclass B : A\n    int get()\n        return value\n",
 "class A\n    int value\nclass B : A\n    override int missing()\n        return 0\n",
 "class A\n    int get()\n        return 1\nclass B : A\n    override float get()\n        return 1.0\n",
 "class A\n    int x\nclass B : A\n    int x\n",
 "class A : B\n    int x\nclass B : A\n    int y\n",
 "class A\n    int tab\n",
 "class A\n    int x\nA a = A(x = 1)\nA &b = a\n",
 "class A\n    int x\n    int y\nA a = A(x = 1)\nprint(a.y)\n",
 "class Counter\n    int value\n    void increment()\n        value = value + 1\nCounter c = Counter()\nc.increment()\n",
 "int x\nint &y = &x\nprint(y)\n",
 "int a = 1\nint b = 2\nint &r = &a\nr = &b\n",
 "int a = 1\nint b = 2\nint &r = &a\n&r = b\n",
 "int &x = &5\n",
 "class Base\n    int x\nclass Child : Base\n    int y\nChild child = Child(x = 1, y = 2)\nBase base = child\n",
 R"(class EqPartial
    int x
    int y

EqPartial a = EqPartial(x = 1)
EqPartial b = EqPartial(x = 1)
bool same = a == b
)",
 R"(class NoParent
    int value()
        return super.value()
)",
 R"(import x = "./x.qui"
)",
 R"(T generic<T>(T value)
    return value
print(generic<int, int>(1))
)",
 R"(int plain(int value)
    return value
print(plain<int>(1))
)",
 R"(class GenericOnly<T>
    T value
GenericOnly value
)",
 "class A\n    int x\nclass B\n    int y\nclass C : A, B\n    int z\n",
 "#if DEBUG\nprint(int(1))\n", "# comment\nprint(int(1))\n"}) bad(s);
 good(R"(class Linear
    int unused = 0
class Rbf
    float gamma = 1.0
float apply(Linear | Rbf kernel)
    match kernel
        Linear
            return 0.0
        Rbf r
            return r.gamma
print(apply(Rbf()))
)");
 bad_code(R"(class Partial
    int x
    int y
Partial value = Partial(x = 1)
Partial | none maybe = value
)", "UNINITIALIZED_UNION_PAYLOAD");
 good(R"(class LocalReset
    int value
    void reset()
        value = 0
void run()
    LocalReset item = LocalReset()
    item.reset()
    print(item.value)
run()
)");
 bad_code(R"(class EarlyReceiver
    float[] values
    void | error initialize(int count)
        if count < 0
            return error("bad")
        values = array(count, fill = 0.0)
        return
EarlyReceiver item = EarlyReceiver()
auto result = item.initialize(-1)
print(item.values[0])
)", "UNINITIALIZED");
 bad_code(R"(class EarlyUnit
    int value
    void reset(bool ok)
        if not ok
            return
        value = 1
EarlyUnit item = EarlyUnit()
item.reset(false)
print(item.value)
)", "UNINITIALIZED");
 bad_code(R"(void maybe_initialize(int &value, bool ok)
    if not ok
        return
    value = 1
int value
maybe_initialize(&value, false)
print(value)
)", "UNINITIALIZED");
 inspect_contains(R"(class SummaryReceiver
    int value
    void reset(bool ok)
        if not ok
            return
        value = 1
)", "\"function\":\"$method.SummaryReceiver.reset\",\"receiver\":{\"requires\":[],\"writes\":[\"value\"],\"initializes\":[],\"invalidates\":[]}");

 // Aliased reference preconditions are all checked against call-entry state.
 // A later write through another alias must not retroactively make an earlier
 // read requirement safe.
 bad_code(R"(void initialize_then_read(int &destination, const int &observation)
    destination = 1
    print(observation)

int value
initialize_then_read(&value, &value)
)", "UNINITIALIZED");

 bad_code(R"(void initialize_then_read_writable(int &destination, int &observation)
    destination = 1
    print(observation)

int value
initialize_then_read_writable(&value, &value)
)", "UNINITIALIZED");

 // Receiver/reference overlap follows the same rule. The method body writes the
 // receiver field first, but the aliased argument still has a call-entry read
 // requirement and cannot consume uninitialized storage.
 bad_code(R"(class AliasReceiver
    int value

    void initialize_then_read(int &observation)
        value = 1
        print(observation)

AliasReceiver item = AliasReceiver()
item.initialize_then_read(&item.value)
)", "UNINITIALIZED");

 // Aliasing itself is legal once every read precondition is satisfied.
 good(R"(void observe_and_update(const int &observation, int &destination)
    print(observation)
    destination = observation + 1

int value = 4
observe_and_update(&value, &value)
print(value)
)");

 // Const authority is path-local and one-way: a readonly path may observe a
 // writable alias, but it cannot be promoted back into write authority.
 bad_code(R"(int value = 1
const int &view = &value
int &writer = &view
)", "REFERENCE_BINDING");

 bad_code(R"(class ConstNested
    int[] values

const ConstNested item = ConstNested(values = [1, 2])
item.values[0] = 3
)", "WRITE_CAPABILITY");

 bad_code(R"(class ConstMethod
    int value

    void change_value()
        value = 2

const ConstMethod item = ConstMethod(value = 1)
item.change_value()
)", "WRITE_CAPABILITY");

 good(R"(T observe_generic<T>(const T &value)
    return value

int source = 7
print(observe_generic(&source))
)");

 bad_code(R"(class BaseConstOverride
    int observe_value(const int &value)
        return value

class ChildConstOverride : BaseConstOverride
    override int observe_value(int &value)
        return value
)", "OVERRIDE_MISMATCH");

 // A control-flow-dependent rebind must never let a later write be credited to
 // the pre-branch target.
 bad_code(R"(int left
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
slot = 9
print(left)
)", "UNINITIALIZED");

 // Rebinding to possibly-uninitialized storage also invalidates the reference's
 // own read proof after the join.
 bad_code(R"(int left = 1
int right
int &slot = &left
bool choose = true
if choose
    &slot = &right
print(slot)
)", "UNINITIALIZED");

 // When every continuing path resolves to the same storage, precision is kept.
 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
else
    &slot = &right
slot = 9
print(right)
)");

 // A write through an ambiguous target initializes the reference path itself,
 // but still must not initialize either possible concrete root.
 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
slot = 9
print(slot)
)");

 // A loop may execute zero or many times. Any escaping rebind therefore loses
 // target identity and previous read proofs until an explicit write/rebind.
 bad_code(R"(int left = 1
int right
int &slot = &left
for i in range(0, 1)
    &slot = &right
print(slot)
)", "UNINITIALIZED");

 bad_code(R"(int left = 1
int right
int &slot = &left
bool choose = true
while choose
    &slot = &right
    break
print(slot)
)", "UNINITIALIZED");

 bad_code(R"(int | none choice = 1
int left
int right = 2
int &slot = &left
match choice
    int value
        &slot = &right
    none
        print("none")
slot = 9
print(left)
)", "UNINITIALIZED");

 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
&slot = &right
print(slot)
)");
 bad_code(R"(int square(int x)
    return x * x
auto f = square
)", "FUNCTION_REFERENCE_CONTEXT");
 bad_code("int[0] xs = []\nprint(xs[0])\n", "INDEX_BOUNDS");
 bad_code("int[2] xs = [1, 2]\nxs[2] = 3\n", "INDEX_BOUNDS");
 llvm_contains(
     "int[2] xs = [1, 2]\nprint(xs[1])\n",
     "call ptr @quidra_fixed_array_slot_proven");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, len(values)))
)", "@quidra_array_initialization_complete");
 bad_code("int | none x = 1\nmatch x\n    int\n        print(x)\n", "MATCH_EXHAUSTIVE");
 bad_code("auto values = []\n", "AMBIGUOUS_TYPE");
 bad_code("int f(int x)\n    return x\nprint(f())\n", "ARGUMENT_MISMATCH");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(first = 1, 2))
)", "ARGUMENT_MISMATCH", "Positional argument cannot follow named arguments");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(first = 1, first = 2))
)", "ARGUMENT_MISMATCH", "Argument 'first' is supplied more than once");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(third = 1))
)", "ARGUMENT_MISMATCH", "Unknown argument 'third'");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine())
)", "ARGUMENT_MISMATCH", "Missing required argument 'first'");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(1, 2, 3))
)", "ARGUMENT_MISMATCH", "Too many positional arguments");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(1, first = 2))
)", "ARGUMENT_MISMATCH", "Argument 'first' is supplied more than once");
 bad_code("int[2] values = [1]\n", "ARRAY_SHAPE");
 bad_code("class A\n    int x\nclass A\n    int y\n", "DUPLICATE_NAME");
 bad_code(R"(T identity<T>(T value)
    return value
auto result = identity(1)
)", "GENERIC_INFERENCE");
 good(R"(T identity<T>(T value)
    return value
auto result = identity(int32(1))
print(result)
)");
 bad_code(R"(T identity<T>(T value)
    return value
print(identity<int, int>(1))
)", "GENERIC_ARITY");
 bad_code(R"(int plain(int value)
    return value
print(plain<int>(1))
)", "GENERIC_TARGET");
 bad_code("void f()\n\treturn\n", "INDENTATION");
 bad_code("class A : B\n    int x\nclass B : A\n    int y\n", "INHERITANCE_CYCLE");
 bad_code("auto x\n", "INVALID_AUTO");
 bad_code(R"(class A
    int value
class B : A
    override int missing()
        return 0
)", "INVALID_OVERRIDE");
 bad_code("break\n", "LOOP_CONTROL_CONTEXT");
 bad_code("int | none x = 1\nmatch x\n    int\n        print(int(1))\n    int y\n        print(y)\n    none\n        print(int(0))\n", "MATCH_CASE");
 bad_code("int f(bool yes)\n    if yes\n        return 1\n", "MISSING_RETURN");
 bad_code("int8 x = int8(300)\n", "NUMERIC_CAST");
 bad_code("int x = int(3.5)\n", "NUMERIC_CAST");
 good("int8 minimum = -128\nint minimum64 = -9223372036854775808\n");
 bad_code("int8 too_small = -129\n", "INTEGER_RANGE");
 good("int8 minimum = int8(-128)\n");
 bad_code("int8 too_small = int8(-129)\n", "NUMERIC_CAST");

 // Numeric literals carry families, not default concrete types.
 good("int32 x = 3\nfloat32 y = 3.0\nfloat32 z = 0.1\n");
 bad_code("auto x = 3\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("auto x = 1.5\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("print(1)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("float x = 3\n", "NUMERIC_FAMILY");
 bad_code("int x = 3.0\n", "NUMERIC_FAMILY");
 bad_code("auto x = [1, 2, 3]\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good(R"(int32 seed = 1
auto values = [2, seed, 3]
print(values[0])
)");
 good(R"(int32 fixed_identity(int32 value)
    return value
auto result = fixed_identity(3)
print(result)
)");
 bad_code("bin raw = bin(3567446)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good("bin raw = bin(int32(3567446))\nprint(raw)\n");
 bad_code("bigint value = 1\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 bad_code("bigreal value = 1.0\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 good("float32 rounded = float32(16777217)\n");
 bad_code("float32 too_large = 1.0e100\n", "FLOAT_RANGE");
 bad_code("float32 too_large = float32(1.0e100)\n", "NUMERIC_CAST");

 // Numeric spelling has one integer radix; exponent notation is visibly floating.
 bad_code("float x = 1e8\n", "LEX_ERROR");
 good("float x = 1.0e8\n");
 bad_code("int x = 0x10\n", "LEX_ERROR");
 bad_code(R"(class A
    int get()
        return 1
class B : A
    override float get()
        return 1.0
)", "OVERRIDE_MISMATCH");
 bad_code(R"(class A
    int get()
        return 1
class B : A
    int get()
        return 2
)", "OVERRIDE_REQUIRED");
 bad_code("int array = 1\n", "SHADOWING");
 bad_code("int math = 1\n", "SHADOWING");
 bad_code("class ReservedField\n    int math\n", "SHADOWING");
 bad_code("class ReservedBuiltinField\n    int array\n", "SHADOWING");
 bad_code("class ReservedMethod\n    int math()\n        return 1\n", "SHADOWING");
 bad_code("class ReservedBuiltinMethod\n    int array()\n        return 1\n", "SHADOWING");
 bad_code("class ReservedNamespaceField\n    int math\n", "SHADOWING");
 bad_code("class ReservedNamespaceMethod\n    int tensor()\n        return 1\n", "SHADOWING");
 
 bad_code("int input = 1\n", "SHADOWING");
 bad_code("int len = 1\n", "SHADOWING");
 bad_code("void use(int print)\n    return\n", "SHADOWING");
  bad_code("range(3)\n", "RANGE_CONTEXT");
 bad_code("int main()\n    return 0\n", "RESERVED_MAIN");
 bad_code("return\n", "RETURN_OUTSIDE_FUNCTION");
 bad_code(R"(int | error read()
    return 1
int use()
    return try read()
)", "TRY_CONTEXT");
 bad_code(R"(class P
    int x
    int y
int read_y(P value)
    return value.y
P value = P(x = 1)
print(read_y(value))
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class P
    int x
    int y
P a = P(x = 1)
P b = P(x = 1)
bool same = a == b
)", "UNINITIALIZED_FIELD_EQUALITY");
 bad_code("class A\n    int x\nA a = A(x = 1)\nprint(a.y)\n", "UNKNOWN_MEMBER");
 bad_code("print(missing)\n", "UNKNOWN_NAME");
 bad_code("Missing value\n", "UNKNOWN_TYPE");
 bad_code("float value = 1.0e9999\n", "FLOAT_RANGE");
 bad_code("import math\n", "STANDARD_NAMESPACE_IMPORT");
 bad_code("auto loaded = vision.read<uint8>(\"input.png\")\n", "GENERIC_RECEIVER");
 bad_code("auto loaded = vision.read(\"input.png\")\n", "UNKNOWN_NAME");
 good("print(math.sqrt(float(16.0)))\n");
 good("tensor<float32> grid = tensor.zeros<float32>([2, 2])\nprint(grid.shape()[0])\n");
 bad_code("1 = 2\n", "INVALID_ASSIGNMENT");
 bad_code(R"(class Broken : Missing
    int x
class Child : Broken
    int y
)", "INVALID_CLASS");
 bad_code("void[] values = []\n", "INVALID_TYPE");
 bad_code("auto value = 1e\n", "LEX_ERROR");
 bad_code(R"(class A
    int value()
        return super.value()
)", "SUPER_CONTEXT");
 bad_code(R"(class A
    int x
class B : A
    int value()
        return super.missing()
)", "SUPER_METHOD");
 bad_code("Missing<int> value\n", "UNKNOWN_GENERIC");
 bad_code(R"(class A
    int x = 0
A value = A()
print(value.missing<int>(1))
)", "UNKNOWN_GENERIC_METHOD");
 bad_code("int[] values = [1]\nvalues.missing<int>()\n", "GENERIC_RECEIVER");

 // Tensor shape patterns fix rank exactly; '_' keeps only that extent unconstrained.
 good(R"(tensor<float32><2, 3> matrix = tensor.zeros<float32>([2, 3])
int[2] dimensions = matrix.shape()
tensor<float32> erased = matrix
int[2] inferred_dimensions = erased.shape()
tensor<float32><3> row = matrix[0]
tensor<float32><2> column = matrix[:, 0]
tensor<float32> cell = matrix[0, 0]
float32 value = cell.item()
tensor<float32><6> reshaped = matrix.reshape([6])
tensor<float32><2, 3> contiguous = matrix.contiguous()
tensor<float><2, 3> converted = float(matrix)
tensor<float32> product = linear.matmul(matrix, tensor.ones<float32>([3, 2]))
float32 dot = linear.dot(reshaped, tensor.ones<float32>([6]))
)");
 good(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<float32> source = tensor.ones<float32>([3, 2])
tensor<float32><3, _> constrained = first_three(source)
)");
 // Generic dtype inference may proceed through an unknown shape, but the
 // specialized call still enforces the exact-rank shape pattern.
 bad_code(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<float32> source = tensor.ones<float32>([2, 2])
tensor<float32><3, _> constrained = first_three(source)
)", "TYPE_MISMATCH");
 good(R"(tensor<float32> | error direct = tensor.zeros<float32>([2, 2])
tensor<float32><2, _> | error constrained = tensor.ones<float32>([2, 2])
tensor<float32> | error widened = constrained
match widened
    tensor<float32> pixels
        print(pixels.shape()[0])
    error problem
        print(problem)
)");
 bad_code("tensor<float32><3, _> wrong = tensor.zeros<float32>([2, 2])\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><2, 4> wrong = tensor.zeros<float32>([2, 3])\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><3, _> wrong_rank = tensor.zeros<float32>([3, 4, 5])\n", "TYPE_MISMATCH");
 good("tensor<float32><3, _, _> exact_rank = tensor.zeros<float32>([3, 4, 5])\n");
 bad_code("tensor<float32><2, 3> value = tensor.ones<float32>([2, 3])\nint[3] wrong_shape = value.shape()\n", "TYPE_MISMATCH");
 good(R"(tensor<float32> erase(tensor<float32> value)
    return value
tensor<float32><2, _> known = erase(tensor.zeros<float32>([2, 2]))
)");
 bad_code("tensor<float32> value = tensor.ones<float32>([1])\nfloat32 scalar = value.item()\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><2, 2> value = tensor.ones<float32>([2, 2])\nauto bad = value[0, 0, 0]\n", "INDEX_ARITY");
 bad_code("tensor<float32> value = tensor.ones<float32>([2, 2])\nfloat32 bad = linear.dot(value, value)\n", "TYPE_MISMATCH");
 bad_code("tensor<float32> value = tensor.ones<float32>([2])\nauto bad = linear.matmul(value, value)\n", "TYPE_MISMATCH");
 bad_code("tensor<uint8><2, _> value = tensor.zeros<uint8>([2, 2])\nauto result = image.write(home, value)\n", "TYPE_MISMATCH");

 // Shape-pattern match cases select only exact-rank compatible tensor alternatives.
 good(R"(void classify(tensor<float32><3, 4> | tensor<float32><1, 4> value)
    match value
        tensor<float32><3, _> rgb
            print(rgb.shape()[0])
        tensor<float32><1, _> gray
            print(gray.shape()[0])
)");
 bad_code(R"(void invalid_case(tensor<float32><1, 4> | tensor<float32><4, 4> value)
    match value
        tensor<float32><3, _> impossible
            print(impossible.shape()[0])
        tensor<float32><1, _> gray
            print(gray.shape()[0])
        tensor<float32><4, _> rgba
            print(rgba.shape()[0])
)", "MATCH_CASE");

 // neural shares the same exact shape pattern; shape-only neural defaults to float32.
 good(R"(tensor<float32> source = tensor.ones<float32>([3, 8, 8])
neural<3, _, _> value = neural.track(source)
tensor<float32><3, _, _> restored = value.untrack()
)");
 good(R"(tensor<float> source = tensor.ones<float>([2, 4])
neural<float><2, _> value = neural.track(source)
tensor<float><2, _> restored = value.untrack()
)");
 bad_code(R"(tensor<float32> source = tensor.ones<float32>([3, 8, 8])
neural<3, _> wrong = neural.track(source)
)", "TYPE_MISMATCH");

 // Dependent shape expressions in function signatures are checked at the boundary.
 good(R"(tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> unknown = tensor.ones<float>([3, 2])
tensor<float><3, 2> checked = keep_shape(3, unknown)
)");
 good(R"(int[][n] keep_rows(int n, int[][n] rows)
    return rows
int[][] dynamic_rows = [[1, 2], [3, 4]]
int[][] checked_rows = keep_rows(2, dynamic_rows)
)");

 // Runtime extent expressions are captured per binding; mutable sources remain legal.
 good(R"(int n = 3
int m = 4
tensor<float><n * 2 + 1, 224> captured = tensor.ones<float>([7, 224])
n = 10
tensor<float><n, 224> later = tensor.ones<float>([10, 224])
float[n * m] dynamic_fixed
dynamic_fixed[0] = 1.0
tensor<float><3, 224> contextual = tensor.zeros()
tensor<float><_, 224> explicit_shape = tensor.zeros([3, 224])
)");
 bad_code(R"(int n = 3
tensor<float><n, 224> wrong = tensor.ones<float>([3, 224, 1])
)", "TYPE_MISMATCH");

 // Numeric container casts preserve array structure and tensor shape facts.
 good(R"(int[][] values = [[1, 2], [3, 4]]
float[][] converted = float(values)
int[2][2] fixed = [[1, 2], [3, 4]]
float[2][2] fixed_converted = float(fixed)
tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
tensor<float><2, 2> tensor_converted = float(matrix)
tensor<float32><2, 2> tracked_source = tensor.ones<float32>([2, 2])
neural<2, 2> tracked = neural.track(tracked_source)
neural<float><2, 2> neural_converted = float(tracked)
tensor<float><2, 2> neural_restored = neural_converted.untrack()
)");
 bad_code("int[] values = [1, 2]\nfloat[] converted = values\n", "TYPE_MISMATCH");
 bad_code("float[] values = [1.0, 2.0]\nint[] converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<float> values = tensor.ones<float>([2])\nauto converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<int> values = tensor.ones<int>([2])\nauto converted = values.cast<float>()\n", "UNKNOWN_MEMBER");

 // Tensor flow facts weaken at joins. A later exact-shape binding accepts the
 // unknown fact set and emits a runtime constraint check.
 good(R"(void branch_shape(bool flag)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if flag
        value = tensor.zeros<float32>([3, 5])
    tensor<float32><3, 4> exact = value
)");
 bad_code(R"(void branch_rank(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    if flag
        value = tensor.zeros<float32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void while_rank(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    while flag
        value = tensor.zeros<float32>([2, 2, 2])
        flag = false
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void for_rank(int[] items)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    for item in items
        value = tensor.zeros<float32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void match_rank(int | string choice)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    match choice
        int
            value = tensor.zeros<float32>([2, 2, 2])
        string
            value = tensor.zeros<float32>([2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");

 // Device placement is runtime/compiler metadata, not part of the nominal tensor type.
 good(R"(tensor<float32> cpu = tensor.zeros<float32>([2])
tensor<float32> direct_gpu = tensor.zeros<float32>([2], gpu = 0)
tensor<float32><2> contextual_gpu = tensor.ones(gpu = 1)
tensor<float32> copied = cpu.gpu(0)
tensor<float32> returned = copied.cpu()
)");
 bad_message("auto value = tensor.zeros<float32>([1], gpu = -1)\n",
             "ARGUMENT_MISMATCH", "gpu index must be non-negative");
 bad_message("auto value = tensor.zeros<float32>([1], gpu = 1.5)\n",
             "NUMERIC_FAMILY", "Real-family literal cannot materialize as an integer-family type");
 bad_code("auto value = tensor.zeros<float32>([1], device = 0)\n",
          "ARGUMENT_MISMATCH");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu()\n",
             "ARGUMENT_MISMATCH", "requires exactly one positional integer GPU index");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu(-1)\n",
             "ARGUMENT_MISMATCH", "non-negative GPU index");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.cpu(0)\n",
             "ARGUMENT_MISMATCH", "takes no arguments");

 // Explicit transfer remains an observable IR boundary; it must not be folded
 // into the preceding allocation or relocate earlier arithmetic.
 const std::string explicit_gpu_transfer = R"(tensor<float32> source = tensor.zeros<float32>([2])
tensor<float32> moved = source.gpu(0)
)";
 ir_contains(explicit_gpu_transfer, "tensor.create");
 ir_contains(explicit_gpu_transfer, " cpu");
 ir_contains(explicit_gpu_transfer, "tensor.gpu");
 const std::string post_compute_transfer = R"(tensor<float32> left = tensor.ones<float32>([2])
tensor<float32> right = tensor.ones<float32>([2])
tensor<float32> sum = left + right
tensor<float32> moved = sum.gpu(0)
)";
 ir_contains(post_compute_transfer, "tensor.binary");
 ir_contains(post_compute_transfer, "tensor.gpu");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1], gpu = 0)\n",
     "@quidra_tensor_create");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu(0)\n",
     "@quidra_tensor_to_gpu");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1])\nauto moved = value.cpu()\n",
     "@quidra_tensor_to_cpu");

 // Loop-carried tensor facts must be weakened before checking the loop body.
 good(R"(void loop_backedge(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    while flag
        auto product = linear.matmul(value, value)
        value = tensor.zeros<float32>([2, 2, 2])
        flag = false
)");
 good(R"(void for_backedge(int[] items)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    for item in items
        auto product = linear.matmul(value, value)
        value = tensor.zeros<float32>([2, 2, 2])
)");

 std::string deep = "print(";
 deep.append(5000, '(');
 deep += "1";
 deep.append(5000, ')');
 deep += ")\n";
 bad_code(deep, "PARSE_DEPTH");
 std::cout<<"all compiler tests passed\n";
}
