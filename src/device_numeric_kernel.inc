// Shared half and complex CUDA/HIP kernel source. The ABI constants are
// supplied from their owner before this source is compiled.
inline constexpr const char* numeric_kernel_body = R"QNUM(
// Kernels for the additional half and fixed complex dtypes. Storage is
// represented by integer bits; half arithmetic and backward operate in
// binary32, and complex arithmetic rounds each component operation.
#ifndef __device__
#define __device__ __attribute__((device))
#define __global__ __attribute__((global))
#endif
#pragma clang fp contract(off)
using U16=unsigned short;
using U32=unsigned int;
using U64=unsigned long long;
using I64=long long;

__device__ U64 q_index(){
#ifdef __HIP__
    return U64(blockIdx.x)*blockDim.x+threadIdx.x;
#else
    U32 block,size,thread;
    asm("mov.u32 %0, %%ctaid.x;":"=r"(block));
    asm("mov.u32 %0, %%ntid.x;":"=r"(size));
    asm("mov.u32 %0, %%tid.x;":"=r"(thread));
    return U64(block)*size+thread;
#endif
}
__device__ void q_status(U32* status,U32 value){
#ifdef __HIP__
    atomicOr(status,value);
#else
    U32 previous;
    asm volatile("atom.global.or.b32 %0, [%1], %2;":"=r"(previous):"l"(status),"r"(value):"memory");
#endif
}
__device__ int q_part(int dtype){return dtype==QCORE_DTYPE_COMPLEX_FLOAT16?QCORE_DTYPE_FLOAT16:dtype==QCORE_DTYPE_COMPLEX_BFLOAT16?QCORE_DTYPE_BFLOAT16:dtype==QCORE_DTYPE_COMPLEX_FLOAT32?QCORE_DTYPE_FLOAT32:dtype==QCORE_DTYPE_COMPLEX_FLOAT64?QCORE_DTYPE_FLOAT64:dtype;}
__device__ bool q_complex(int dtype){return dtype>=QCORE_DTYPE_COMPLEX_FLOAT16&&dtype<=QCORE_DTYPE_COMPLEX_FLOAT64;}
__device__ bool q_half(int dtype){return dtype==QCORE_DTYPE_FLOAT16||dtype==QCORE_DTYPE_BFLOAT16;}
__device__ int q_width(int dtype){
    if(dtype==QCORE_DTYPE_INT64||dtype==QCORE_DTYPE_UINT64||dtype==QCORE_DTYPE_FLOAT64)return 8;
    if(dtype==QCORE_DTYPE_INT8||dtype==QCORE_DTYPE_UINT8||dtype==QCORE_DTYPE_BOOL)return 1;
    if(dtype==QCORE_DTYPE_INT16||dtype==QCORE_DTYPE_UINT16||q_half(dtype))return 2;
    if(dtype==QCORE_DTYPE_INT32||dtype==QCORE_DTYPE_UINT32||dtype==QCORE_DTYPE_FLOAT32||dtype==QCORE_DTYPE_COMPLEX_FLOAT16||dtype==QCORE_DTYPE_COMPLEX_BFLOAT16)return 4;
    return dtype==QCORE_DTYPE_COMPLEX_FLOAT32?8:16;
}
__device__ float q_float(U32 bits){union{U32 bits;float value;} v;v.bits=bits;return v.value;}
__device__ U32 q_bits(float value){union{U32 bits;float value;} v;v.value=value;return v.bits;}
__device__ double q_double(U64 bits){union{U64 bits;double value;} v;v.bits=bits;return v.value;}
__device__ U64 q_double_bits(double value){union{U64 bits;double value;} v;v.value=value;return v.bits;}
__device__ float q_read_half(U16 bits,int dtype){
    if(dtype==QCORE_DTYPE_BFLOAT16)return q_float(U32(bits)<<16);
    U32 s=U32(bits&0x8000u)<<16,e=(bits>>10)&31u,m=bits&1023u;
    if(e==0){if(m==0)return q_float(s);int n=0;while((m&1024u)==0){m<<=1;++n;}
        return q_float(s|(U32(113-n)<<23)|((m&1023u)<<13));}
    if(e==31)return q_float(s|0x7f800000u|(m<<13));
    return q_float(s|((e+112u)<<23)|(m<<13));
}
__device__ U64 q_round_shift(U64 value,int shift){
    if(shift<=0)return value<<(-shift);
    if(shift>=64)return 0;
    U64 kept=value>>shift,rest=value&((U64(1)<<shift)-1),middle=U64(1)<<(shift-1);
    return kept+U64(rest>middle||(rest==middle&&(kept&1)!=0));
}
__device__ U16 q_half_bits(double value,int dtype){
    U64 bits=q_double_bits(value),fraction=bits&0xfffffffffffffull;
    U32 sign=U32(bits>>48)&0x8000u;
    int exp=int((bits>>52)&2047u),precision=dtype==QCORE_DTYPE_FLOAT16?11:8,min_exp=dtype==QCORE_DTYPE_FLOAT16?-14:-126,max_exp=dtype==QCORE_DTYPE_FLOAT16?15:127;
    U32 infinity=dtype==QCORE_DTYPE_FLOAT16?0x7c00u:0x7f80u;
    if(exp==2047)return U16(sign|infinity|(fraction?U32(1)<<(precision-2):0));
    if(exp==0)return U16(sign);
    int leading=exp-1023;
    U64 significand=fraction|0x10000000000000ull;
    if(leading<min_exp)return U16(sign|q_round_shift(significand,52+min_exp-(precision-1)-leading));
    U64 kept=q_round_shift(significand,53-precision);
    if(kept==(U64(1)<<precision)){kept>>=1;++leading;}
    if(leading>max_exp)return U16(sign|infinity);
    return U16(sign|(U32(leading+1-min_exp)<<(precision-1))|U32(kept&((U64(1)<<(precision-1))-1)));
}
__device__ U16 q_integer_half(U64 magnitude,bool negative,int dtype){
    U32 sign=negative?0x8000u:0;
    if(magnitude==0)return U16(sign);
    int bits=0;for(U64 rest=magnitude;rest;rest>>=1)++bits;
    int precision=dtype==QCORE_DTYPE_FLOAT16?11:8,leading=bits-1;
    U64 kept=q_round_shift(magnitude,bits-precision);
    if(kept==(U64(1)<<precision)){kept>>=1;++leading;}
    int maximum=dtype==QCORE_DTYPE_FLOAT16?15:127;
    if(leading>maximum)return U16(sign|(dtype==QCORE_DTYPE_FLOAT16?0x7c00u:0x7f80u));
    return U16(sign|(U32(leading+maximum)<<(precision-1))|U32(kept&((U64(1)<<(precision-1))-1)));
}
__device__ U64 q_integer(const void* input,U64 i,int dtype){
    if(dtype==QCORE_DTYPE_INT64)return U64(static_cast<const I64*>(input)[i]);
    if(dtype==QCORE_DTYPE_INT8)return U64(static_cast<const signed char*>(input)[i]);
    if(dtype==QCORE_DTYPE_INT16)return U64(static_cast<const short*>(input)[i]);
    if(dtype==QCORE_DTYPE_INT32)return U64(static_cast<const int*>(input)[i]);
    if(dtype==QCORE_DTYPE_UINT8)return static_cast<const unsigned char*>(input)[i];
    if(dtype==QCORE_DTYPE_UINT16)return static_cast<const U16*>(input)[i];
    if(dtype==QCORE_DTYPE_UINT32)return static_cast<const U32*>(input)[i];
    return static_cast<const U64*>(input)[i];
}
__device__ double q_read_part(const void* input,U64 i,int dtype){
    if(q_half(dtype))return q_read_half(static_cast<const U16*>(input)[i],dtype);
    if(dtype==QCORE_DTYPE_FLOAT32)return static_cast<const float*>(input)[i];
    if(dtype==QCORE_DTYPE_FLOAT64)return static_cast<const double*>(input)[i];
    U64 integer=q_integer(input,i,dtype);
    return dtype<=QCORE_DTYPE_INT32?double(I64(integer)):double(integer);
}
template<class R> struct QPair{R re,im;};
template<class R> __device__ QPair<R> q_read(const void* input,U64 i,int dtype){
    int part=q_part(dtype);U64 offset=q_complex(dtype)?i*2:i;
    return {R(q_read_part(input,offset,part)),q_complex(dtype)?R(q_read_part(input,offset+1,part)):R(0)};
}
template<class R> __device__ R q_round(R value,int dtype){return q_half(dtype)?R(q_read_half(q_half_bits(double(value),dtype),dtype)):value;}
__device__ void q_store_part(void* output,U64 i,double value,int dtype){
    if(q_half(dtype))static_cast<U16*>(output)[i]=q_half_bits(value,dtype);
    else if(dtype==QCORE_DTYPE_FLOAT32)static_cast<float*>(output)[i]=float(value);
    else static_cast<double*>(output)[i]=value;
}
template<class R> __device__ void q_store(void* output,U64 i,QPair<R> value,int dtype){
    int part=q_part(dtype);U64 offset=q_complex(dtype)?i*2:i;
    q_store_part(output,offset,double(value.re),part);
    if(q_complex(dtype))q_store_part(output,offset+1,double(value.im),part);
}
template<class R> __device__ bool q_valid(QPair<R> v){return !__builtin_isnan(v.re)&&!__builtin_isnan(v.im);}
template<class R> __device__ bool q_finite(QPair<R> value){return __builtin_isfinite(value.re)&&__builtin_isfinite(value.im);}
template<class R> __device__ QPair<R> q_binary(QPair<R> a,QPair<R> b,int dtype,int operation,U32* status){
    int part=q_part(dtype);QPair<R> result{0,0};
    if(operation==quidra::abi::tensor_binary_opcode::add)result={q_round(R(a.re+b.re),part),q_round(R(a.im+b.im),part)};
    else if(operation==quidra::abi::tensor_binary_opcode::subtract)result={q_round(R(a.re-b.re),part),q_round(R(a.im-b.im),part)};
    else if(operation==quidra::abi::tensor_binary_opcode::multiply&&!q_complex(dtype))result={q_round(R(a.re*b.re),part),0};
    else if(operation==quidra::abi::tensor_binary_opcode::multiply){
        volatile R ac=a.re*b.re,bd=a.im*b.im,ad=a.re*b.im,bc=a.im*b.re;
        result={q_round(R(q_round(R(ac),part)-q_round(R(bd),part)),part),q_round(R(q_round(R(ad),part)+q_round(R(bc),part)),part)};
    }else if(operation==quidra::abi::tensor_binary_opcode::divide){
        if(!q_complex(dtype))result={q_round(R(a.re/b.re),part),0};
        else{
            R r,t,re,im;bool valid=true;
            if(__builtin_fabs(b.re)>=__builtin_fabs(b.im)){
                r=q_round(R(b.im/b.re),part);t=q_round(R(b.re+q_round(R(b.im*r),part)),part);
                re=q_round(R(a.re+q_round(R(a.im*r),part)),part);im=q_round(R(a.im-q_round(R(a.re*r),part)),part);
            }else{
                r=q_round(R(b.re/b.im),part);t=q_round(R(q_round(R(b.re*r),part)+b.im),part);
                re=q_round(R(q_round(R(a.re*r),part)+a.im),part);im=q_round(R(q_round(R(a.im*r),part)-a.re),part);
            }
            valid=!__builtin_isnan(r)&&!__builtin_isnan(t)&&!__builtin_isnan(re)&&!__builtin_isnan(im);
            result={q_round(R(re/t),part),q_round(R(im/t),part)};
            if(!valid)q_status(status,16u);
        }
    }
#ifdef __HIP__
    else if(operation==quidra::abi::tensor_binary_opcode::power&&!q_complex(dtype)){
        extern __device__ float powf(float,float);
        if(a.re==0&&b.re==0)q_status(status,1u);
                else if(a.re<0&&__builtin_trunc(b.re)!=b.re)q_status(status,4u);
        result={q_round(R(powf(float(a.re),float(b.re))),part),0};
    }
#endif
    if(!q_valid(result))q_status(status,operation==quidra::abi::tensor_binary_opcode::divide&&b.re==0&&b.im==0?8u:16u);
    return result;
}
template<class R> __device__ void q_run(void* output,void* output_other,const void* left,const void* right,const void* gradient,
    const U64* offsets,const U64* positions,U64 scalar_low,U64 scalar_high,U64 i,int dtype,int operation,int side,int mode,
    U32* status){
    if(mode==4||mode==11){R total=0;for(U64 p=offsets[i];p<offsets[i+1];++p){total=R(total+q_read<R>(gradient,positions[p],dtype).re);if(mode==11)total=q_round(total,q_part(dtype));}
        q_store(output,i,QPair<R>{total,0},dtype);return;}
    U64 scalar_bits[2]={scalar_low,scalar_high};
    auto a=q_read<R>(left,i,dtype);QPair<R> b{0,0};
    if(mode==0||mode==3||mode==10)b=side?q_read<R>(scalar_bits,0,dtype):q_read<R>(right,i,dtype);
    if(side==quidra::abi::scalar_side::left){auto tmp=a;a=b;b=tmp;}
    if(mode==0){q_store(output,i,q_binary(a,b,dtype,operation,status),dtype);return;}
    if(mode==1){q_store(output,i,QPair<R>{-a.re,-a.im},dtype);return;}
    if(mode==3||mode==10){
        bool equal=a.re==b.re&&a.im==b.im;
        bool result=operation==quidra::abi::tensor_comparison_opcode::equal?equal:operation==quidra::abi::tensor_comparison_opcode::not_equal?!equal:operation==quidra::abi::tensor_comparison_opcode::less?a.re<b.re:operation==quidra::abi::tensor_comparison_opcode::less_equal?a.re<=b.re:operation==quidra::abi::tensor_comparison_opcode::greater?a.re>b.re:a.re>=b.re;
        if(mode==3)static_cast<unsigned char*>(output)[i]=result;
        else if(!result)q_status(status,1u);
        return;
    }
    if(mode==5||mode==6){
        R g=q_read<R>(gradient,i,dtype).re,x=q_read<R>(left,i,dtype).re,y=mode==6?R(0):q_read<R>(right,i,dtype).re;
        R l=g,r=g;
        if(operation==quidra::abi::tensor_binary_opcode::subtract){if(mode==5)r=-g;else l=side==quidra::abi::scalar_side::left?-g:g;}
        else if(operation==quidra::abi::tensor_binary_opcode::multiply&&mode==5){l=R(g*y);r=R(g*x);}
        else if(operation==quidra::abi::tensor_binary_opcode::divide&&mode==5){l=R(g/y);R ga=R(g*x),bb=R(y*y);r=R(-R(ga/bb));}
        if(mode==6){R scalar=q_read<R>(scalar_bits,0,dtype).re;
            if(operation==quidra::abi::tensor_binary_opcode::multiply)l=R(g*scalar);
#ifdef __HIP__
            else if(operation==quidra::abi::tensor_binary_opcode::power){
                extern __device__ float powf(float,float);
                l=R(R(g*scalar)*R(powf(float(x),float(R(scalar-R{1})))));
            }
#endif
            else if(operation==quidra::abi::tensor_binary_opcode::divide){if(side==quidra::abi::scalar_side::left){R gs=R(g*scalar),xx=R(x*x);l=R(-R(gs/xx));}else l=R(g/scalar);}
            q_store(output,i,QPair<R>{l,0},dtype);
        }else if(output==output_other){int part=q_part(dtype);l=q_round(l,part);r=q_round(r,part);q_store(output,i,QPair<R>{R(l+r),0},dtype);}
        else {q_store(output,i,QPair<R>{l,0},dtype);q_store(output_other,i,QPair<R>{r,0},dtype);}
    }
}
extern "C" __global__ void q_kernel(void* output,void* output_other,const void* left,const void* right,const void* gradient,
    const U64* offsets,const U64* positions,U64 scalar_low,U64 scalar_high,U64 count,int dtype,int operation,int side,int mode,
    int source_dtype,int target_dtype,U32* status){
    U64 i=q_index();if(i>=count)return;
    if(mode==2){
        auto value=q_read<double>(left,i,source_dtype);const auto original=value;int part=q_part(target_dtype);
        if(source_dtype>=QCORE_DTYPE_INT64&&source_dtype<=QCORE_DTYPE_UINT64){
            U64 raw=q_integer(left,i,source_dtype);bool negative=source_dtype<=QCORE_DTYPE_INT32&&I64(raw)<0;
            if(q_half(part)){U64 magnitude=negative?U64(0)-raw:raw;value.re=q_read_half(q_integer_half(magnitude,negative,part),part);}
            else if(part==QCORE_DTYPE_FLOAT32)value.re=source_dtype<=QCORE_DTYPE_INT32?double(float(I64(raw))):double(float(raw));
        }
        if(!q_complex(target_dtype)&&q_complex(source_dtype)){q_status(status,1u);return;}
        if(target_dtype>=QCORE_DTYPE_INT64&&target_dtype<=QCORE_DTYPE_UINT64){
            const double v=value.re;int width=q_width(target_dtype)*8;
            if(!__builtin_isfinite(v)){q_status(status,1u);return;}
            bool signed_target=target_dtype<=QCORE_DTYPE_INT32;
            U64 bits=q_double_bits(v),fraction=bits&0xfffffffffffffull;
            int exponent=int((bits>>52)&2047u)-1023;
            if(v!=0.0&&(exponent<0||(exponent<52&&(fraction&((U64(1)<<(52-exponent))-1))!=0))){
                q_status(status,2u);return;}
            bool negative=(bits>>63)!=0;
            if(v!=0.0&&(signed_target?(exponent>width-1||
                (exponent==width-1&&(!negative||fraction!=0))):(negative||exponent>=width))){
                q_status(status,1u);return;}
            U64 raw=signed_target?U64(I64(v)):U64(v);
            if(width==8)static_cast<unsigned char*>(output)[i]=static_cast<unsigned char>(raw);
            else if(width==16)static_cast<U16*>(output)[i]=U16(raw);
            else if(width==32)static_cast<U32*>(output)[i]=U32(raw);
            else static_cast<U64*>(output)[i]=raw;
            return;
        }
        q_store(output,i,value,target_dtype);
        const auto rounded=q_read<double>(output,i,target_dtype);
        if(!q_valid(original) || (__builtin_isfinite(original.re)&&!__builtin_isfinite(rounded.re)) ||
           (__builtin_isfinite(original.im)&&!__builtin_isfinite(rounded.im)))q_status(status,1u);
        return;
    }
    if(mode==7){if(!q_valid(q_read<double>(left,i,dtype)))q_status(status,1u);return;}
    if(mode==8){q_store(output,i,QPair<double>{1,0},dtype);return;}
    if(mode==9){int width=q_width(dtype);auto* out=static_cast<unsigned char*>(output)+i*width;
        auto* in=static_cast<const unsigned char*>(left)+positions[i]*width;for(int k=0;k<width;++k)out[k]=in[k];return;}
    if(q_part(dtype)==QCORE_DTYPE_FLOAT64)q_run<double>(output,output_other,left,right,gradient,offsets,positions,scalar_low,scalar_high,i,dtype,operation,side,mode,status);
    else q_run<float>(output,output_other,left,right,gradient,offsets,positions,scalar_low,scalar_high,i,dtype,operation,side,mode,status);
}

)QNUM";

std::string hip_numeric_source() {
    std::ostringstream source;
    source << "#define __HIP__ 1\n";
    source << "constexpr int QCORE_DTYPE_INT64=" << QCORE_DTYPE_INT64 << ";\n";
    source << "constexpr int QCORE_DTYPE_INT8=" << QCORE_DTYPE_INT8 << ";\n";
    source << "constexpr int QCORE_DTYPE_INT16=" << QCORE_DTYPE_INT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_INT32=" << QCORE_DTYPE_INT32 << ";\n";
    source << "constexpr int QCORE_DTYPE_UINT8=" << QCORE_DTYPE_UINT8 << ";\n";
    source << "constexpr int QCORE_DTYPE_UINT16=" << QCORE_DTYPE_UINT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_UINT32=" << QCORE_DTYPE_UINT32 << ";\n";
    source << "constexpr int QCORE_DTYPE_UINT64=" << QCORE_DTYPE_UINT64 << ";\n";
    source << "constexpr int QCORE_DTYPE_FLOAT64=" << QCORE_DTYPE_FLOAT64 << ";\n";
    source << "constexpr int QCORE_DTYPE_FLOAT32=" << QCORE_DTYPE_FLOAT32 << ";\n";
    source << "constexpr int QCORE_DTYPE_BOOL=" << QCORE_DTYPE_BOOL << ";\n";
    source << "constexpr int QCORE_DTYPE_FLOAT16=" << QCORE_DTYPE_FLOAT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_BFLOAT16=" << QCORE_DTYPE_BFLOAT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_COMPLEX_FLOAT16=" << QCORE_DTYPE_COMPLEX_FLOAT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_COMPLEX_BFLOAT16=" << QCORE_DTYPE_COMPLEX_BFLOAT16 << ";\n";
    source << "constexpr int QCORE_DTYPE_COMPLEX_FLOAT32=" << QCORE_DTYPE_COMPLEX_FLOAT32 << ";\n";
    source << "constexpr int QCORE_DTYPE_COMPLEX_FLOAT64=" << QCORE_DTYPE_COMPLEX_FLOAT64 << ";\n";
    source << "namespace quidra { namespace abi { namespace tensor_binary_opcode {\n";
    source << "constexpr int add=" << abi::tensor_binary_opcode::add << ";\n";
    source << "constexpr int subtract=" << abi::tensor_binary_opcode::subtract << ";\n";
    source << "constexpr int multiply=" << abi::tensor_binary_opcode::multiply << ";\n";
    source << "constexpr int divide=" << abi::tensor_binary_opcode::divide << ";\n";
    source << "constexpr int remainder=" << abi::tensor_binary_opcode::remainder << ";\n";
    source << "constexpr int power=" << abi::tensor_binary_opcode::power << ";\n";
    source << "}}}\n";
    source << "namespace quidra { namespace abi { namespace tensor_comparison_opcode {\n";
    source << "constexpr int equal=" << abi::tensor_comparison_opcode::equal << ";\n";
    source << "constexpr int not_equal=" << abi::tensor_comparison_opcode::not_equal << ";\n";
    source << "constexpr int less=" << abi::tensor_comparison_opcode::less << ";\n";
    source << "constexpr int less_equal=" << abi::tensor_comparison_opcode::less_equal << ";\n";
    source << "constexpr int greater=" << abi::tensor_comparison_opcode::greater << ";\n";
    source << "constexpr int greater_equal=" << abi::tensor_comparison_opcode::greater_equal << ";\n";
    source << "}}}\n";
    source << "namespace quidra { namespace abi { namespace scalar_side {\n";
    source << "constexpr int none=" << abi::scalar_side::none << ";\n";
    source << "constexpr int left=" << abi::scalar_side::left << ";\n";
    source << "constexpr int right=" << abi::scalar_side::right << ";\n";
    source << "}}}\n";
    source << numeric_kernel_body;
    return source.str();
}
