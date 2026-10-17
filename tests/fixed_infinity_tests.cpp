#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/half_format.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
int main(int argc,char**argv){
    if(argc>1){
        const std::string mode(argv[1]);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        if(mode=="nan-format") quidra_format_float(nan);
        if(mode=="nan-fixed") quidra_format_number(nan,10,3,-1,1);
        if(mode=="nan-significant") quidra_format_number(nan,10,-1,3,1);
        if(mode=="nan32-format") quidra_format_number32(static_cast<float>(nan),10,-1,-1,1);
        if(mode=="nan32-fixed") quidra_format_number32(static_cast<float>(nan),10,3,-1,1);
        if(mode=="nan32-significant") quidra_format_number32(static_cast<float>(nan),10,-1,3,1);
        for(int format:{0,1}) {
            const auto half = format==0?quidra::abi::half::binary16:quidra::abi::half::bfloat16;
            const auto bits = quidra::abi::half::round_to(nan,half);
            const std::string prefix = format==0?"nan16-":"nan16b-";
            if(mode==prefix+"format") quidra_half_format(bits,format,10,-1,-1,1);
            if(mode==prefix+"fixed") quidra_half_format(bits,format,10,3,-1,1);
            if(mode==prefix+"significant") quidra_half_format(bits,format,10,-1,3,1);
        }
        return 5;
    }
    int failures=0;
    auto expect=[&](bool ok,const char*what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);++failures;}};
    auto text=[&](char* p,const char* expected){expect(p&&std::string(p)==expected,expected); if(p)quidra_managed_release(p,nullptr);};
    for(bool negative:{false,true}){
        const char* spelling=negative?"-inf":"inf";
        const double infinity=negative?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity();
        expect(quidra_cli_parse_float(spelling)==infinity,"CLI inf parse");
        double wide=0;float narrow=0;
        bool parsed64=quidra_parse_float64(spelling,&wide),parsed32=quidra_parse_float32(spelling,&narrow);
        expect(parsed64&&wide==infinity,"real64 inf parse");expect(parsed32&&narrow==infinity,"real32 inf parse");
        if(parsed64){
            text(quidra_format_float(wide),spelling);
            text(quidra_format_number(wide,-1,-1,-1,0),spelling);
            text(quidra_format_number(wide,10,3,-1,1),spelling);
            text(quidra_format_number(wide,10,-1,3,0),spelling);
        }
        if(parsed32){
            text(quidra_format_number32(narrow,-1,-1,-1,0),spelling);
            text(quidra_format_number32(narrow,10,3,-1,1),spelling);
            text(quidra_format_number32(narrow,10,-1,3,1),spelling);
            text(quidra_format_number32(narrow,10,-1,-1,1),spelling);
        }
        for(int format:{0,1}){
            unsigned short bits=0;
            bool parsed=quidra_half_parse(spelling,format,&bits);
            expect(parsed,"half inf parse");
            if(parsed){
                expect(quidra::abi::half::value_of(bits,format==0?quidra::abi::half::binary16:quidra::abi::half::bfloat16)==infinity,"half inf sign");
                text(quidra_half_format(bits,format,-1,-1,-1,0),spelling);
                text(quidra_half_format(bits,format,10,3,-1,1),spelling);
                text(quidra_half_format(bits,format,10,-1,3,1),spelling);
                text(quidra_half_format(bits,format,10,-1,-1,1),spelling);
            }
        }
    }
    for(const char* invalid:{"nan","-nan","inf suffix","∞"}){
        double d=7;float f=7;unsigned short h=7;
        expect(!quidra_parse_float64(invalid,&d),"real64 invalid text");
        expect(!quidra_parse_float32(invalid,&f),"real32 invalid text");
        expect(!quidra_half_parse(invalid,0,&h),"real16 invalid text");
        expect(!quidra_half_parse(invalid,1,&h),"real16b invalid text");
        expect(d==7 && f==7 && h==7,"invalid parse leaves output unchanged");
    }
    double d=0;float f=0;unsigned short h=0;
    expect(!quidra_parse_float64("1.0e999",&d),"finite real64 parse range remains checked");
    expect(!quidra_parse_float32("1.0e39",&f),"finite real32 parse range remains checked");
    expect(!quidra_half_parse("65536.0",0,&h),"finite real16 parse range remains checked");
    std::fprintf(stderr,"%d fixed infinity failures\n",failures);
    return failures?1:0;
}
