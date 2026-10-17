// Exercise the production CUDA/HIP generators without claiming GPU execution.
#include "../src/device_backend.cpp"
#include <cstdio>

int main() {
    namespace d = quidra::device;
    namespace a = quidra::abi;
    int failures = 0;
    int cases = 0;
    const auto expect = [&](bool valid, const char* contract) {
        ++cases;
        if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", contract); }
    };
    for (int dtype : {QCORE_DTYPE_FLOAT32, QCORE_DTYPE_FLOAT64}) {
        const bool wide = dtype == QCORE_DTYPE_FLOAT64;
        for (int side : {a::scalar_side::none, a::scalar_side::left, a::scalar_side::right}) {
            for (int op : {a::tensor_binary_opcode::add, a::tensor_binary_opcode::subtract,
                           a::tensor_binary_opcode::multiply, a::tensor_binary_opcode::divide}) {
                const auto ptx = d::ptx_float_binary_kernel(dtype, op, side);
                expect(ptx.find(wide ? "testp.number.f64 %p2, %fd3;" :
                                       "testp.number.f32 %p2, %f3;") != std::string::npos &&
                       ptx.find("testp.finite") == std::string::npos &&
                       ptx.find("  @%p2 bra STORE;\n") != std::string::npos,
                       "CUDA arithmetic accepts infinity and rejects NaN before storing");
                const auto hip = d::hip_float_binary_source(dtype, op, side);
                expect(hip.find("isnan(lv)||isnan(rv)||isnan(result)") != std::string::npos &&
                       hip.find("!isfinite(result)") == std::string::npos,
                       "HIP arithmetic rejects NaN operands and results, not infinity");
            }
            const auto hip_power = d::hip_float_binary_source(dtype, a::tensor_binary_opcode::power, side);
            expect(hip_power.find("lv==0&&rv==0") != std::string::npos &&
                   hip_power.find("lv==0&&rv<0") == std::string::npos &&
                   hip_power.find("isnan(lv)||isnan(rv)||isnan(result)") != std::string::npos,
                   "HIP power admits zero to a negative exponent but cannot hide NaN");
        }
        const auto ptx = d::ptx_validate_finite_kernel(wide ? 8 : 4);
        expect(ptx.find(wide ? "and.b64 %rd11, %rd10, 0x7fffffffffffffff;" :
                              "and.b32 %r7, %r6, 0x7fffffff;") != std::string::npos &&
               ptx.find(wide ? "setp.le.u64 %p2, %rd11, 0x7ff0000000000000;" :
                              "setp.le.u32 %p2, %r7, 0x7f800000;") != std::string::npos &&
               ptx.find("  @%p2 bra DONE;\n") != std::string::npos,
               "CUDA validation compares signless bits inclusively with infinity");
        const auto hip = d::hip_validate_finite_source(wide ? 8 : 4);
        expect(hip.find(wide ? "(in[i]&0x7fffffffffffffffull)>0x7ff0000000000000ull" :
                              "(in[i]&0x7fffffffu)>0x7f800000u") != std::string::npos,
               "HIP validation rejects all NaN encodings and accepts both infinities");
    }
    // Narrowing finite values remains checked, unlike arithmetic overflow.
    expect(d::hip_float_narrow_validate_source().find("isfinite(v)&&!isfinite(narrowed)") != std::string::npos,
           "HIP finite narrowing range guard is retained");
    expect(d::ptx_float_narrow_validate_kernel().find("setp.gt.u64 %p3, %rd11, 0x47efffffe0000000;") != std::string::npos,
           "CUDA finite narrowing range guard is retained");
    std::printf("%d/%d generated infinity contracts passed\n", cases - failures, cases);
    return failures ? 1 : 0;
}
