#include <cstdio>
#if __has_include("device/encoder_attribution.hpp")
#include "device/encoder_attribution.hpp"
int main() {
    using quidra::device::detail::EncoderAttribution;
    using Owner=EncoderAttribution::Owner;
    int failed{};
    const auto check=[&](bool value,const char* name){if(!value){std::fprintf(stderr,"FAIL: %s\n",name);++failed;}};
    EncoderAttribution one;
    one.core_dispatch(10);
    check(one.kernel()==10 && one.source_available() && one.owner()==Owner::Core,"one Core dispatch retains exact identity");
    one.core_dispatch(20);
    check(!one.kernel() && !one.source_available() && one.owner()==Owner::Core,"two different Core kernels remain a generic encoder");
    EncoderAttribution repeated;
    repeated.core_dispatch(10); repeated.core_dispatch(10);
    check(!repeated.kernel() && !repeated.source_available(),"repeated kernels do not fabricate a single source/call identity");
    EncoderAttribution mixed;
    mixed.core_dispatch(10); mixed.package_open();
    check(mixed.owner()==Owner::Mixed && !mixed.source_available() && !mixed.kernel(),"package opening cannot reattribute earlier Core work");
    EncoderAttribution reverse;
    reverse.package_open(); reverse.core_dispatch(20);
    check(reverse.owner()==Owner::Mixed && !reverse.source_available(),"Core following a package remains mixed");
    EncoderAttribution package;
    package.package_open();
    check(package.owner()==Owner::Package && package.source_available() && !package.kernel(),"one package call retains coarse unit source");
    package.package_open();
    check(!package.source_available(),"several package calls do not claim the last caller source");
    if(!failed)std::puts("Encoder attribution identities: ok");
    return failed ? 1 : 0;
}
#else
int main(){std::fputs("FAIL: truthful encoder attribution policy is missing\n",stderr);return 1;}
#endif
