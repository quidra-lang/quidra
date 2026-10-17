#include "device_backend.hpp"
#include "quidra/abi/half_format.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
namespace d=quidra::device;
namespace a=quidra::abi;
int failures=0;
void expect(bool value,const char*name,const std::string&error={}){
    if(!value){std::fprintf(stderr,"FAIL: %s %s\n",name,error.c_str());++failures;}
}
template<class T> void run(int dtype){
    std::string error;
    auto release=[](d::Buffer*p){d::release(p);};
    std::unique_ptr<d::Buffer,decltype(release)> l(d::allocate(0,sizeof(T),error),release),r(d::allocate(0,sizeof(T),error),release),out(d::allocate(0,sizeof(T),error),release);
    if(!l||!r||!out){expect(false,"allocate",error);return;}
    auto load=[&](T x,T y){expect(d::copy_from_host(l.get(),0,&x,sizeof(T),error),"upload left",error);expect(d::copy_from_host(r.get(),0,&y,sizeof(T),error),"upload right",error);};
    const T inf=T(std::numeric_limits<double>::infinity());
    load(inf,T(1));
    expect(d::validate_finite(l.get(),0,dtype,1,"NaN",error),"Infinity passes external tensor validation",error);
    auto binary=[&](int operation,T x,T y,bool good,T wanted,const char*name){
        load(x,y);error.clear();
        bool power=operation==a::tensor_binary_opcode::power;
        bool okay=d::compute_binary(out.get(),l.get(),0,power?nullptr:r.get(),0,power?&y:nullptr,power?a::scalar_side::right:a::scalar_side::none,dtype,operation,1,error);
        expect(okay==good,name,error);
        if(okay&&good){T actual{};expect(d::copy_to_host(out.get(),0,&actual,sizeof(T),error),"download",error);expect(actual==wanted,name);}
    };
    binary(a::tensor_binary_opcode::add,inf,T(1),true,inf,"inf plus one");
    binary(a::tensor_binary_opcode::divide,T(1),T(0),true,inf,"nonzero divided by zero");
    binary(a::tensor_binary_opcode::divide,T(-1),T(0),true,-inf,"negative divided by zero");
    binary(a::tensor_binary_opcode::divide,T(1),inf,true,T(0),"finite divided by infinity");
    binary(a::tensor_binary_opcode::subtract,inf,inf,false,T(0),"inf minus inf is rejected");
    binary(a::tensor_binary_opcode::multiply,T(0),inf,false,T(0),"zero times inf is rejected");
    binary(a::tensor_binary_opcode::divide,T(0),T(0),false,T(0),"zero divided by zero is rejected");
    binary(a::tensor_binary_opcode::power,T(0),T(-1),true,inf,"zero to negative power");
    binary(a::tensor_binary_opcode::power,T(0),T(0),false,T(0),"zero to zero still rejected");
    load(T(std::numeric_limits<double>::quiet_NaN()),T(1));
    expect(!d::validate_finite(l.get(),0,dtype,1,"NaN",error),"NaN fails external tensor validation",error);
}
int main(){
    run<double>(QCORE_DTYPE_FLOAT64);run<float>(QCORE_DTYPE_FLOAT32);
    run<a::half::Real16>(QCORE_DTYPE_FLOAT16);run<a::half::Real16b>(QCORE_DTYPE_BFLOAT16);
    std::fprintf(stderr,"%d device Infinity failures\n",failures);return failures?1:0;
}
