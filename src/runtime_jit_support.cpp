#if defined(__APPLE__)

// LLVM ORC uses emulated TLS for JITed thread_local globals on Darwin.
// Force compiler-rt emutls into the JIT runtime dylib so lli can resolve it.
extern "C" void* __emutls_get_address(void*);

extern "C" __attribute__((visibility("default")))
void* quidra_jit_emutls_anchor(void* control) {
    return __emutls_get_address(control);
}

#endif
