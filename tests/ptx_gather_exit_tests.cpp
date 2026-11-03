// Host-side code-generation contract: no CUDA device is needed. This checks
// the real PTX generator, not a mock of its result. Runtime GPU tests remain
// a separate gate.
#include "../src/device_backend.cpp"
#include <cstdio>

int main() {
    int failures = 0;
    for (int dtype : {QCORE_DTYPE_FLOAT32, QCORE_DTYPE_FLOAT64}) {
        const auto ptx = quidra::device::ptx_gather_backward_kernel(dtype);
        const std::string guard =
            "  setp.ge.u64 %p2, %rd10, %rd12;\n"
            "  @%p2 bra STORE;\n";
        if (ptx.find(guard) == std::string::npos) {
            std::fprintf(stderr,
                "FAIL: gather-backward dtype %d must exit when position >= end\n",
                dtype);
            ++failures;
        }
        if (ptx.find("  @!%p2 bra STORE;") != std::string::npos) {
            std::fprintf(stderr,
                "FAIL: gather-backward dtype %d exits before processing valid positions\n",
                dtype);
            ++failures;
        }
    }
    std::fprintf(stderr, "%d PTX gather-exit failures\n", failures);
    return failures ? 1 : 0;
}
