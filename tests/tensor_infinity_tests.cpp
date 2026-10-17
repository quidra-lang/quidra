#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/abi/half_format.hpp"
#include "quidra/native_extension.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace a=quidra::abi;
struct Tensor {
    void* raw;
    explicit Tensor(void* p):raw(p){}
    Tensor(const Tensor&)=delete;
    ~Tensor(){if(raw)quidra_managed_release(raw,reinterpret_cast<void*>(&quidra_tensor_drop));}
};
int failures=0;
void expect(bool ok,const char* why){if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",why);}}
template<class T> void positive(int dtype){
    long long shape[]{1,1};
    Tensor one(quidra_tensor_create(shape,dtype,a::tensor_fill_mode::ones,false,0,1,1));
    T inf=T(std::numeric_limits<double>::infinity()),zero=T(0);
    Tensor huge(quidra_tensor_binary(one.raw,nullptr,&inf,a::scalar_side::right,a::tensor_binary_opcode::multiply,1,1));
    expect(*static_cast<const T*>(qcore_tensor_cpu_data_const(huge.raw))==inf,"tensor infinite result");
    Tensor divzero(quidra_tensor_binary(one.raw,nullptr,&zero,a::scalar_side::right,a::tensor_binary_opcode::divide,1,1));
    expect(*static_cast<const T*>(qcore_tensor_cpu_data_const(divzero.raw))==inf,"tensor nonzero divided by zero");
    Tensor reciprocal(quidra_tensor_binary(one.raw,huge.raw,nullptr,a::scalar_side::none,a::tensor_binary_opcode::divide,1,1));
    expect(*static_cast<const T*>(qcore_tensor_cpu_data_const(reciprocal.raw))==zero,"tensor finite divided by infinity");
    for(int destination:{QCORE_DTYPE_FLOAT16,QCORE_DTYPE_BFLOAT16,QCORE_DTYPE_FLOAT32,QCORE_DTYPE_FLOAT64}){
        Tensor converted(quidra_tensor_try_cast(huge.raw,destination,1,1));
        expect(converted.raw!=nullptr,"real-family tensor Infinity cast");
        if(converted.raw){Tensor wide(quidra_tensor_try_cast(converted.raw,QCORE_DTYPE_FLOAT64,1,1));
            expect(wide.raw && *static_cast<const double*>(qcore_tensor_cpu_data_const(wide.raw))==std::numeric_limits<double>::infinity(),"round-trip tensor Infinity cast");}
    }
    Tensor integer(quidra_tensor_try_cast(huge.raw,QCORE_DTYPE_INT64,1,1));
    expect(integer.raw==nullptr,"Infinity to integer tensor is rejected");
}
int main(int argc,char** argv){
    if(argc==2){
        long long shape[]{1,1};
        Tensor one(quidra_tensor_create(shape,QCORE_DTYPE_FLOAT32,a::tensor_fill_mode::ones,false,0,1,1));
        float inf=std::numeric_limits<float>::infinity(),zero=0;
        Tensor huge(quidra_tensor_binary(one.raw,nullptr,&inf,a::scalar_side::right,a::tensor_binary_opcode::multiply,1,1));
        if(std::string(argv[1])=="inf-minus-inf"){
            Tensor bad(quidra_tensor_binary(huge.raw,huge.raw,nullptr,a::scalar_side::none,a::tensor_binary_opcode::subtract,1,1));
        } else if(std::string(argv[1])=="zero-times-inf"){
            Tensor bad(quidra_tensor_binary(huge.raw,nullptr,&zero,a::scalar_side::right,a::tensor_binary_opcode::multiply,1,1));
        } else return 2;
        return 3;
    }
    positive<double>(QCORE_DTYPE_FLOAT64);positive<float>(QCORE_DTYPE_FLOAT32);
    positive<a::half::Real16>(QCORE_DTYPE_FLOAT16);positive<a::half::Real16b>(QCORE_DTYPE_BFLOAT16);
    long long scalar_shape[]{0};
    Tensor one(quidra_tensor_create(scalar_shape,QCORE_DTYPE_FLOAT32,a::tensor_fill_mode::ones,false,0,1,1));
    Tensor leaf(quidra_tensor_track(one.raw,1,1));
    float inf=std::numeric_limits<float>::infinity();
    Tensor loss(quidra_tensor_binary(leaf.raw,nullptr,&inf,a::scalar_side::right,a::tensor_binary_opcode::multiply,1,1));
    void* targets[]{leaf.raw};unsigned char kinds[]{0};
    quidra_tensor_backward_many(loss.raw,targets,kinds,1,false,1,1);
    Tensor gradient(quidra_tensor_grad(leaf.raw,1,1));
    expect(*static_cast<const float*>(qcore_tensor_cpu_data_const(gradient.raw))==inf,"infinite gradient is retained");
    std::printf("%d tensor Infinity failures\n",failures);return failures?1:0;
}
