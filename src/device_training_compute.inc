
namespace {

template <typename T>
T training_load(const Buffer* buffer,std::size_t index) {
    T value{};
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    std::memcpy(&value,buffer->test_data.data()+index*sizeof(T),sizeof(T));
#else
    (void)buffer;(void)index;
#endif
    return value;
}

template <typename T>
void training_store(Buffer* buffer,std::size_t index,T value) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    std::memcpy(buffer->test_data.data()+index*sizeof(T),&value,sizeof(T));
#else
    (void)buffer;(void)index;(void)value;
#endif
}

template <typename T>
bool fake_conv2d_backward(
    Buffer* input_gradient,Buffer* weight_gradient,Buffer* bias_gradient,
    const Buffer* gradient,const Buffer* input,const Buffer* weight,
    std::size_t batches,std::size_t channels_in,std::size_t height,std::size_t width,
    std::size_t channels_out,std::size_t kernel_h,std::size_t kernel_w,
    std::size_t output_h,std::size_t output_w,std::size_t stride,std::size_t padding) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto input_count=batches*channels_in*height*width;
    const auto weight_count=channels_out*channels_in*kernel_h*kernel_w;
    std::fill(input_gradient->test_data.begin(),input_gradient->test_data.end(),0);
    std::fill(weight_gradient->test_data.begin(),weight_gradient->test_data.end(),0);
    std::fill(bias_gradient->test_data.begin(),bias_gradient->test_data.end(),0);
    (void)input_count;(void)weight_count;
    for(std::size_t n=0;n<batches;++n)
        for(std::size_t oc=0;oc<channels_out;++oc)
            for(std::size_t oy=0;oy<output_h;++oy)
                for(std::size_t ox=0;ox<output_w;++ox){
                    const auto go=((n*channels_out+oc)*output_h+oy)*output_w+ox;
                    const T g=training_load<T>(gradient,go);
                    training_store<T>(bias_gradient,oc,
                        static_cast<T>(training_load<T>(bias_gradient,oc)+g));
                    for(std::size_t ic=0;ic<channels_in;++ic)
                        for(std::size_t ky=0;ky<kernel_h;++ky)
                            for(std::size_t kx=0;kx<kernel_w;++kx){
                                const auto iy=static_cast<long long>(oy*stride+ky)-
                                              static_cast<long long>(padding);
                                const auto ix=static_cast<long long>(ox*stride+kx)-
                                              static_cast<long long>(padding);
                                if(iy<0||ix<0||iy>=static_cast<long long>(height)||
                                   ix>=static_cast<long long>(width))continue;
                                const auto ii=((n*channels_in+ic)*height+
                                    static_cast<std::size_t>(iy))*width+
                                    static_cast<std::size_t>(ix);
                                const auto wi=((oc*channels_in+ic)*kernel_h+ky)*kernel_w+kx;
                                training_store<T>(input_gradient,ii,
                                    static_cast<T>(training_load<T>(input_gradient,ii)+
                                                   static_cast<T>(g*training_load<T>(weight,wi))));
                                training_store<T>(weight_gradient,wi,
                                    static_cast<T>(training_load<T>(weight_gradient,wi)+
                                                   static_cast<T>(g*training_load<T>(input,ii))));
                            }
                }
    return true;
#else
    (void)input_gradient;(void)weight_gradient;(void)bias_gradient;
    (void)gradient;(void)input;(void)weight;(void)batches;(void)channels_in;
    (void)height;(void)width;(void)channels_out;(void)kernel_h;(void)kernel_w;
    (void)output_h;(void)output_w;(void)stride;(void)padding;
    return false;
#endif
}

template <typename T>
bool fake_normalize_training(
    Buffer* output,Buffer* cache,Buffer* running_mean,Buffer* running_variance,
    const Buffer* input,const Buffer* scale,const Buffer* bias,
    std::size_t count,std::size_t features,std::size_t inner,std::size_t samples,
    double momentum_raw,double epsilon_raw) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const T momentum=static_cast<T>(momentum_raw);
    const T epsilon=static_cast<T>(epsilon_raw);
    for(std::size_t f=0;f<features;++f){
        T mean{};
        for(std::size_t i=0;i<count;++i)
            if((i/inner)%features==f)
                mean=static_cast<T>(mean+training_load<T>(input,i));
        mean=static_cast<T>(mean/static_cast<T>(samples));
        T variance{};
        for(std::size_t i=0;i<count;++i)
            if((i/inner)%features==f){
                const T d=static_cast<T>(training_load<T>(input,i)-mean);
                variance=static_cast<T>(variance+static_cast<T>(d*d));
            }
        variance=static_cast<T>(variance/static_cast<T>(samples));
        const T inverse=static_cast<T>(T{1}/std::sqrt(static_cast<T>(variance+epsilon)));
        training_store<T>(cache,f,mean);
        training_store<T>(cache,features+f,inverse);
        const T old_mean=training_load<T>(running_mean,f);
        const T old_variance=training_load<T>(running_variance,f);
        training_store<T>(running_mean,f,static_cast<T>(
            static_cast<T>((T{1}-momentum)*old_mean)+static_cast<T>(momentum*mean)));
        training_store<T>(running_variance,f,static_cast<T>(
            static_cast<T>((T{1}-momentum)*old_variance)+static_cast<T>(momentum*variance)));
    }
    for(std::size_t i=0;i<count;++i){
        const auto f=(i/inner)%features;
        const T xhat=static_cast<T>(
            static_cast<T>(training_load<T>(input,i)-training_load<T>(cache,f))*
            training_load<T>(cache,features+f));
        training_store<T>(output,i,static_cast<T>(
            static_cast<T>(xhat*training_load<T>(scale,f))+training_load<T>(bias,f)));
    }
    return true;
#else
    (void)output;(void)cache;(void)running_mean;(void)running_variance;
    (void)input;(void)scale;(void)bias;(void)count;(void)features;(void)inner;
    (void)samples;(void)momentum_raw;(void)epsilon_raw;
    return false;
#endif
}

template <typename T>
bool fake_normalize_backward(
    Buffer* input_gradient,Buffer* scale_gradient,Buffer* bias_gradient,
    const Buffer* gradient,const Buffer* input,const Buffer* scale,const Buffer* cache,
    std::size_t count,std::size_t features,std::size_t inner,std::size_t samples) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for(std::size_t f=0;f<features;++f){
        T sg{},sgx{},scale_g{},bias_g{};
        const T mean=training_load<T>(cache,f);
        const T inverse=training_load<T>(cache,features+f);
        for(std::size_t i=0;i<count;++i)
            if((i/inner)%features==f){
                const T g=training_load<T>(gradient,i);
                const T xhat=static_cast<T>(
                    static_cast<T>(training_load<T>(input,i)-mean)*inverse);
                sg=static_cast<T>(sg+g);
                sgx=static_cast<T>(sgx+static_cast<T>(g*xhat));
                scale_g=static_cast<T>(scale_g+static_cast<T>(g*xhat));
                bias_g=static_cast<T>(bias_g+g);
            }
        training_store<T>(scale_gradient,f,scale_g);
        training_store<T>(bias_gradient,f,bias_g);
        const T sample_count=static_cast<T>(samples);
        const T factor=static_cast<T>(
            static_cast<T>(training_load<T>(scale,f)*inverse)/sample_count);
        for(std::size_t i=0;i<count;++i)
            if((i/inner)%features==f){
                const T g=training_load<T>(gradient,i);
                const T xhat=static_cast<T>(
                    static_cast<T>(training_load<T>(input,i)-mean)*inverse);
                training_store<T>(input_gradient,i,static_cast<T>(
                    factor*static_cast<T>(
                        static_cast<T>(sample_count*g)-sg-static_cast<T>(xhat*sgx))));
            }
    }
    return true;
#else
    (void)input_gradient;(void)scale_gradient;(void)bias_gradient;
    (void)gradient;(void)input;(void)scale;(void)cache;(void)count;(void)features;
    (void)inner;(void)samples;
    return false;
#endif
}

template <typename T>
bool fake_random_mask(
    Buffer* output,Buffer* mask,const Buffer* input,std::size_t count,
    std::uint64_t initial_state,std::uint64_t cutoff,double scale_raw) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    constexpr std::uint64_t step=0x9e3779b97f4a7c15ULL;
    const T scale=static_cast<T>(scale_raw);
    for(std::size_t i=0;i<count;++i){
        std::uint64_t z=initial_state+
            static_cast<std::uint64_t>(i+1)*step;
        z=(z^(z>>30U))*0xbf58476d1ce4e5b9ULL;
        z=(z^(z>>27U))*0x94d049bb133111ebULL;
        z^=z>>31U;
        const auto bits=z>>11U;
        const T multiplier=bits<cutoff?T{0}:scale;
        training_store<T>(mask,i,multiplier);
        training_store<T>(output,i,static_cast<T>(training_load<T>(input,i)*multiplier));
    }
    return true;
#else
    (void)output;(void)mask;(void)input;(void)count;
    (void)initial_state;(void)cutoff;(void)scale_raw;
    return false;
#endif
}

template <typename T>
bool fake_moment_update(
    Buffer* parameter,const Buffer* gradient,Buffer* first,Buffer* second,
    std::size_t count,double rate_raw,double beta1_raw,double beta2_raw,
    double epsilon_raw,double correction1_raw,double correction2_raw) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const T rate=static_cast<T>(rate_raw),beta1=static_cast<T>(beta1_raw),
            beta2=static_cast<T>(beta2_raw),epsilon=static_cast<T>(epsilon_raw),
            correction1=static_cast<T>(correction1_raw),
            correction2=static_cast<T>(correction2_raw);
    for(std::size_t i=0;i<count;++i){
        const T g=training_load<T>(gradient,i);
        const T m=static_cast<T>(
            beta1*training_load<T>(first,i)+static_cast<T>((T{1}-beta1)*g));
        const T v=static_cast<T>(
            beta2*training_load<T>(second,i)+
            static_cast<T>((T{1}-beta2)*static_cast<T>(g*g)));
        training_store<T>(first,i,m);
        training_store<T>(second,i,v);
        const T delta=static_cast<T>(
            rate*static_cast<T>(m/correction1)/
            static_cast<T>(std::sqrt(static_cast<T>(v/correction2))+epsilon));
        training_store<T>(parameter,i,static_cast<T>(training_load<T>(parameter,i)-delta));
    }
    return true;
#else
    (void)parameter;(void)gradient;(void)first;(void)second;(void)count;
    (void)rate_raw;(void)beta1_raw;(void)beta2_raw;(void)epsilon_raw;
    (void)correction1_raw;(void)correction2_raw;
    return false;
#endif
}

std::string ptx_conv2d_backward_source(int dtype,int kind) {
    const bool f32=dtype==10;
    const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n\n"
      ".visible .entry q_kernel(\n"
      " .param .u64 p_out,\n .param .u64 p_grad,\n .param .u64 p_input,\n"
      " .param .u64 p_weight,\n .param .u64 p_count,\n .param .u64 p_batches,\n"
      " .param .u64 p_ci,\n .param .u64 p_h,\n .param .u64 p_w,\n"
      " .param .u64 p_co,\n .param .u64 p_kh,\n .param .u64 p_kw,\n"
      " .param .u64 p_oh,\n .param .u64 p_ow,\n .param .u64 p_stride,\n"
      " .param .u64 p_padding)\n{\n"
      " .reg .pred %p<16>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<96>;\n"
      <<(f32?" .reg .f32 %f<12>;\n":" .reg .f64 %fd<12>;\n")
      <<" ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_grad];\n"
        " ld.param.u64 %rd3,[p_input];\n ld.param.u64 %rd4,[p_weight];\n"
        " ld.param.u64 %rd5,[p_count];\n ld.param.u64 %rd6,[p_batches];\n"
        " ld.param.u64 %rd7,[p_ci];\n ld.param.u64 %rd8,[p_h];\n"
        " ld.param.u64 %rd9,[p_w];\n ld.param.u64 %rd10,[p_co];\n"
        " ld.param.u64 %rd11,[p_kh];\n ld.param.u64 %rd12,[p_kw];\n"
        " ld.param.u64 %rd13,[p_oh];\n ld.param.u64 %rd14,[p_ow];\n"
        " ld.param.u64 %rd15,[p_stride];\n ld.param.u64 %rd16,[p_padding];\n"
        " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
        " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd17,%r4;\n"
        " setp.ge.u64 %p1,%rd17,%rd5;\n @%p1 bra DONE;\n";
    if(f32)o<<" mov.f32 %f1,0f00000000;\n";
    else o<<" mov.f64 %fd1,0d0000000000000000;\n";
    if(kind==1){
        o<<" rem.u64 %rd20,%rd17,%rd9;\n div.u64 %rd21,%rd17,%rd9;\n"
          " rem.u64 %rd22,%rd21,%rd8;\n div.u64 %rd23,%rd21,%rd8;\n"
          " rem.u64 %rd24,%rd23,%rd7;\n div.u64 %rd25,%rd23,%rd7;\n"
          " mov.u64 %rd30,0;\nOC:\n setp.ge.u64 %p2,%rd30,%rd10;\n @%p2 bra STORE;\n"
          " mov.u64 %rd31,0;\nKY:\n setp.ge.u64 %p3,%rd31,%rd11;\n @%p3 bra NEXT_OC;\n"
          " mov.u64 %rd32,0;\nKX:\n setp.ge.u64 %p4,%rd32,%rd12;\n @%p4 bra NEXT_KY;\n"
          " cvt.s64.u64 %rd33,%rd22;\n cvt.s64.u64 %rd34,%rd16;\n add.s64 %rd33,%rd33,%rd34;\n"
          " cvt.s64.u64 %rd35,%rd31;\n sub.s64 %rd33,%rd33,%rd35;\n"
          " setp.lt.s64 %p5,%rd33,0;\n @%p5 bra SKIP;\n cvt.s64.u64 %rd36,%rd15;\n"
          " rem.s64 %rd37,%rd33,%rd36;\n setp.ne.s64 %p6,%rd37,0;\n @%p6 bra SKIP;\n"
          " div.s64 %rd38,%rd33,%rd36;\n cvt.s64.u64 %rd39,%rd13;\n"
          " setp.ge.s64 %p7,%rd38,%rd39;\n @%p7 bra SKIP;\n"
          " cvt.s64.u64 %rd40,%rd20;\n add.s64 %rd40,%rd40,%rd34;\n"
          " cvt.s64.u64 %rd41,%rd32;\n sub.s64 %rd40,%rd40,%rd41;\n"
          " setp.lt.s64 %p8,%rd40,0;\n @%p8 bra SKIP;\n"
          " rem.s64 %rd42,%rd40,%rd36;\n setp.ne.s64 %p9,%rd42,0;\n @%p9 bra SKIP;\n"
          " div.s64 %rd43,%rd40,%rd36;\n cvt.s64.u64 %rd44,%rd14;\n"
          " setp.ge.s64 %p10,%rd43,%rd44;\n @%p10 bra SKIP;\n"
          " mul.lo.u64 %rd45,%rd25,%rd10;\n add.u64 %rd45,%rd45,%rd30;\n"
          " mul.lo.u64 %rd45,%rd45,%rd13;\n cvt.u64.s64 %rd46,%rd38;\n add.u64 %rd45,%rd45,%rd46;\n"
          " mul.lo.u64 %rd45,%rd45,%rd14;\n cvt.u64.s64 %rd47,%rd43;\n add.u64 %rd45,%rd45,%rd47;\n"
          " mul.lo.u64 %rd48,%rd30,%rd7;\n add.u64 %rd48,%rd48,%rd24;\n"
          " mul.lo.u64 %rd48,%rd48,%rd11;\n add.u64 %rd48,%rd48,%rd31;\n"
          " mul.lo.u64 %rd48,%rd48,%rd12;\n add.u64 %rd48,%rd48,%rd32;\n"
          <<" mul.lo.u64 %rd49,%rd45,"<<bytes<<";\n add.u64 %rd50,%rd2,%rd49;\n"
          <<" mul.lo.u64 %rd51,%rd48,"<<bytes<<";\n add.u64 %rd52,%rd4,%rd51;\n";
    }else if(kind==2){
        o<<" rem.u64 %rd20,%rd17,%rd12;\n div.u64 %rd21,%rd17,%rd12;\n"
          " rem.u64 %rd22,%rd21,%rd11;\n div.u64 %rd23,%rd21,%rd11;\n"
          " rem.u64 %rd24,%rd23,%rd7;\n div.u64 %rd25,%rd23,%rd7;\n"
          " mov.u64 %rd30,0;\nNLOOP:\n setp.ge.u64 %p2,%rd30,%rd6;\n @%p2 bra STORE;\n"
          " mov.u64 %rd31,0;\nOY:\n setp.ge.u64 %p3,%rd31,%rd13;\n @%p3 bra NEXT_N;\n"
          " mov.u64 %rd32,0;\nOX:\n setp.ge.u64 %p4,%rd32,%rd14;\n @%p4 bra NEXT_OY;\n"
          " mul.lo.u64 %rd33,%rd31,%rd15;\n add.u64 %rd33,%rd33,%rd22;\n"
          " setp.lt.u64 %p5,%rd33,%rd16;\n @%p5 bra SKIP;\n sub.u64 %rd33,%rd33,%rd16;\n"
          " setp.ge.u64 %p6,%rd33,%rd8;\n @%p6 bra SKIP;\n"
          " mul.lo.u64 %rd34,%rd32,%rd15;\n add.u64 %rd34,%rd34,%rd20;\n"
          " setp.lt.u64 %p7,%rd34,%rd16;\n @%p7 bra SKIP;\n sub.u64 %rd34,%rd34,%rd16;\n"
          " setp.ge.u64 %p8,%rd34,%rd9;\n @%p8 bra SKIP;\n"
          " mul.lo.u64 %rd45,%rd30,%rd10;\n add.u64 %rd45,%rd45,%rd25;\n"
          " mul.lo.u64 %rd45,%rd45,%rd13;\n add.u64 %rd45,%rd45,%rd31;\n"
          " mul.lo.u64 %rd45,%rd45,%rd14;\n add.u64 %rd45,%rd45,%rd32;\n"
          " mul.lo.u64 %rd48,%rd30,%rd7;\n add.u64 %rd48,%rd48,%rd24;\n"
          " mul.lo.u64 %rd48,%rd48,%rd8;\n add.u64 %rd48,%rd48,%rd33;\n"
          " mul.lo.u64 %rd48,%rd48,%rd9;\n add.u64 %rd48,%rd48,%rd34;\n"
          <<" mul.lo.u64 %rd49,%rd45,"<<bytes<<";\n add.u64 %rd50,%rd2,%rd49;\n"
          <<" mul.lo.u64 %rd51,%rd48,"<<bytes<<";\n add.u64 %rd52,%rd3,%rd51;\n";
    }else{
        o<<" mov.u64 %rd30,0;\nNLOOP:\n setp.ge.u64 %p2,%rd30,%rd6;\n @%p2 bra STORE;\n"
          " mov.u64 %rd31,0;\nOY:\n setp.ge.u64 %p3,%rd31,%rd13;\n @%p3 bra NEXT_N;\n"
          " mov.u64 %rd32,0;\nOX:\n setp.ge.u64 %p4,%rd32,%rd14;\n @%p4 bra NEXT_OY;\n"
          " mul.lo.u64 %rd45,%rd30,%rd10;\n add.u64 %rd45,%rd45,%rd17;\n"
          " mul.lo.u64 %rd45,%rd45,%rd13;\n add.u64 %rd45,%rd45,%rd31;\n"
          " mul.lo.u64 %rd45,%rd45,%rd14;\n add.u64 %rd45,%rd45,%rd32;\n"
          <<" mul.lo.u64 %rd49,%rd45,"<<bytes<<";\n add.u64 %rd50,%rd2,%rd49;\n";
    }
    if(f32)o<<" ld.global.f32 %f2,[%rd50];\n";
    else o<<" ld.global.f64 %fd2,[%rd50];\n";
    if(kind!=3){
        if(f32)o<<" ld.global.f32 %f3,[%rd52];\n mul.f32 %f4,%f2,%f3;\n add.f32 %f1,%f1,%f4;\n";
        else o<<" ld.global.f64 %fd3,[%rd52];\n mul.f64 %fd4,%fd2,%fd3;\n add.f64 %fd1,%fd1,%fd4;\n";
    }else{
        if(f32)o<<" add.f32 %f1,%f1,%f2;\n";
        else o<<" add.f64 %fd1,%fd1,%fd2;\n";
    }
    o<<"SKIP:\n";
    if(kind==1)o<<" add.u64 %rd32,%rd32,1;\n bra KX;\nNEXT_KY:\n add.u64 %rd31,%rd31,1;\n bra KY;\nNEXT_OC:\n add.u64 %rd30,%rd30,1;\n bra OC;\n";
    else o<<" add.u64 %rd32,%rd32,1;\n bra OX;\nNEXT_OY:\n add.u64 %rd31,%rd31,1;\n bra OY;\nNEXT_N:\n add.u64 %rd30,%rd30,1;\n bra NLOOP;\n";
    o<<"STORE:\n"
      <<" mul.lo.u64 %rd60,%rd17,"<<bytes<<";\n add.u64 %rd61,%rd1,%rd60;\n";
    if(f32)o<<" st.global.f32 [%rd61],%f1;\n";
    else o<<" st.global.f64 [%rd61],%fd1;\n";
    o<<"DONE:\n ret;\n}\n";
    return o.str();
}

std::string hip_conv2d_backward_source(int dtype,int kind) {
    const char* t=dtype==10?"float":"double";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* out,const "<<t<<"* grad,const "<<t<<"* input,const "<<t<<"* weight,"
      "unsigned long long count,unsigned long long batches,unsigned long long ci,"
      "unsigned long long h,unsigned long long w,unsigned long long co,"
      "unsigned long long kh,unsigned long long kw,unsigned long long oh,"
      "unsigned long long ow,unsigned long long stride,unsigned long long padding){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i>=count)return;"<<t<<" sum=0;";
    if(kind==1){
        o<<"unsigned long long ix=i%w,t=i/w,iy=t%h;t/=h;unsigned long long ic=t%ci,n=t/ci;"
          "for(unsigned long long oc=0;oc<co;++oc)for(unsigned long long ky=0;ky<kh;++ky)"
          "for(unsigned long long kx=0;kx<kw;++kx){long long ny=(long long)iy+(long long)padding-(long long)ky;"
          "long long nx=(long long)ix+(long long)padding-(long long)kx;"
          "if(ny<0||nx<0||ny%(long long)stride||nx%(long long)stride)continue;"
          "unsigned long long oy=(unsigned long long)(ny/(long long)stride),ox=(unsigned long long)(nx/(long long)stride);"
          "if(oy>=oh||ox>=ow)continue;unsigned long long gi=((n*co+oc)*oh+oy)*ow+ox;"
          "unsigned long long wi=((oc*ci+ic)*kh+ky)*kw+kx;sum+=grad[gi]*weight[wi];}";
    }else if(kind==2){
        o<<"unsigned long long kx=i%kw,t=i/kw,ky=t%kh;t/=kh;unsigned long long ic=t%ci,oc=t/ci;"
          "for(unsigned long long n=0;n<batches;++n)for(unsigned long long oy=0;oy<oh;++oy)"
          "for(unsigned long long ox=0;ox<ow;++ox){long long iy=(long long)(oy*stride+ky)-(long long)padding;"
          "long long ix=(long long)(ox*stride+kx)-(long long)padding;if(iy<0||ix<0||iy>=(long long)h||ix>=(long long)w)continue;"
          "unsigned long long gi=((n*co+oc)*oh+oy)*ow+ox;unsigned long long ii=((n*ci+ic)*h+(unsigned long long)iy)*w+(unsigned long long)ix;"
          "sum+=grad[gi]*input[ii];}";
    }else{
        o<<"unsigned long long oc=i;for(unsigned long long n=0;n<batches;++n)for(unsigned long long oy=0;oy<oh;++oy)"
          "for(unsigned long long ox=0;ox<ow;++ox)sum+=grad[((n*co+oc)*oh+oy)*ow+ox];";
    }
    o<<"out[i]=sum;}\n";
    return o.str();
}

#ifdef __APPLE__
std::string msl_conv2d_backward_source(int kind) {
    std::ostringstream o;
    o<<"#include <metal_stdlib>\nusing namespace metal;\nkernel void q_kernel("
      "device float* out [[buffer(0)]],const device float* grad [[buffer(1)]],"
      "const device float* input [[buffer(2)]],const device float* weight [[buffer(3)]],"
      "constant ulong& count [[buffer(4)]],constant ulong& batches [[buffer(5)]],"
      "constant ulong& ci [[buffer(6)]],constant ulong& h [[buffer(7)]],constant ulong& w [[buffer(8)]],"
      "constant ulong& co [[buffer(9)]],constant ulong& kh [[buffer(10)]],constant ulong& kw [[buffer(11)]],"
      "constant ulong& oh [[buffer(12)]],constant ulong& ow [[buffer(13)]],constant ulong& stride [[buffer(14)]],"
      "constant ulong& padding [[buffer(15)]],uint gid [[thread_position_in_grid]]){"
      "ulong i=ulong(gid);if(i>=count)return;float sum=0.0f;";
    if(kind==1){
        o<<"ulong ix=i%w,t=i/w,iy=t%h;t/=h;ulong ic=t%ci,n=t/ci;"
          "for(ulong oc=0;oc<co;++oc)for(ulong ky=0;ky<kh;++ky)for(ulong kx=0;kx<kw;++kx){"
          "long ny=long(iy)+long(padding)-long(ky),nx=long(ix)+long(padding)-long(kx);"
          "if(ny<0||nx<0||ny%long(stride)!=0||nx%long(stride)!=0)continue;"
          "ulong oy=ulong(ny/long(stride)),ox=ulong(nx/long(stride));if(oy>=oh||ox>=ow)continue;"
          "sum+=grad[((n*co+oc)*oh+oy)*ow+ox]*weight[((oc*ci+ic)*kh+ky)*kw+kx];}";
    }else if(kind==2){
        o<<"ulong kx=i%kw,t=i/kw,ky=t%kh;t/=kh;ulong ic=t%ci,oc=t/ci;"
          "for(ulong n=0;n<batches;++n)for(ulong oy=0;oy<oh;++oy)for(ulong ox=0;ox<ow;++ox){"
          "long iy=long(oy*stride+ky)-long(padding),ix=long(ox*stride+kx)-long(padding);"
          "if(iy<0||ix<0||iy>=long(h)||ix>=long(w))continue;"
          "sum+=grad[((n*co+oc)*oh+oy)*ow+ox]*input[((n*ci+ic)*h+ulong(iy))*w+ulong(ix)];}";
    }else{
        o<<"ulong oc=i;for(ulong n=0;n<batches;++n)for(ulong oy=0;oy<oh;++oy)for(ulong ox=0;ox<ow;++ox)"
          "sum+=grad[((n*co+oc)*oh+oy)*ow+ox];";
    }
    o<<"out[i]=sum;}\n";
    return o.str();
}
#endif

std::string hip_normalize_stats_source(int dtype) {
    const char* t=dtype==10?"float":"double";
    const char* sqrtfn=dtype==10?"sqrtf":"sqrt";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\n#include <math.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* cache,"<<t<<"* running_mean,"<<t<<"* running_variance,const "<<t<<"* input,"
      "unsigned long long count,unsigned long long features,unsigned long long inner,"
      "unsigned long long samples,double momentum_raw,double epsilon_raw){"
      "unsigned long long f=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(f>=features)return;"
      <<t<<" mean=0;for(unsigned long long i=0;i<count;++i)if((i/inner)%features==f)mean+=input[i];"
      "mean/=("<<t<<")samples;"<<t<<" variance=0;for(unsigned long long i=0;i<count;++i)"
      "if((i/inner)%features==f){"<<t<<" d=input[i]-mean;variance+=d*d;}variance/=("<<t<<")samples;"
      <<t<<" momentum=("<<t<<")momentum_raw,epsilon=("<<t<<")epsilon_raw;"
      "cache[f]=mean;cache[features+f]=("<<t<<")1/"<<sqrtfn<<"(variance+epsilon);"
      "running_mean[f]=(("<<t<<")1-momentum)*running_mean[f]+momentum*mean;"
      "running_variance[f]=(("<<t<<")1-momentum)*running_variance[f]+momentum*variance;}\n";
    return o.str();
}

std::string hip_normalize_apply_source(int dtype) {
    const char* t=dtype==10?"float":"double";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* out,const "<<t<<"* input,const "<<t<<"* scale,const "<<t<<"* bias,const "<<t<<"* cache,"
      "unsigned long long count,unsigned long long features,unsigned long long inner){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
      "unsigned long long f=(i/inner)%features;"<<t<<" xhat=(input[i]-cache[f])*cache[features+f];"
      "out[i]=xhat*scale[f]+bias[f];}\n";
    return o.str();
}

std::string hip_normalize_backward_source(int dtype,int kind) {
    const char* t=dtype==10?"float":"double";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* out,const "<<t<<"* grad,const "<<t<<"* input,const "<<t<<"* scale,const "<<t<<"* cache,"
      "unsigned long long count,unsigned long long features,unsigned long long inner,unsigned long long samples){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;";
    if(kind==1){
        o<<"if(i>=count)return;unsigned long long f=(i/inner)%features;"<<t<<" mean=cache[f],inv=cache[features+f];"
          <<t<<" sg=0,sgx=0;for(unsigned long long j=0;j<count;++j)if((j/inner)%features==f){"
          <<t<<" xhat=(input[j]-mean)*inv;sg+=grad[j];sgx+=grad[j]*xhat;}"
          <<t<<" xhat=(input[i]-mean)*inv;out[i]=(scale[f]*inv/("<<t<<")samples)*(("<<t<<")samples*grad[i]-sg-xhat*sgx);";
    }else{
        o<<"if(i>=features)return;unsigned long long f=i;"<<t<<" sum=0,mean=cache[f],inv=cache[features+f];"
          "for(unsigned long long j=0;j<count;++j)if((j/inner)%features==f){";
        if(kind==2)o<<t<<" xhat=(input[j]-mean)*inv;sum+=grad[j]*xhat;";
        else o<<"sum+=grad[j];";
        o<<"}out[f]=sum;";
    }
    o<<"}\n";
    return o.str();
}

#ifdef __APPLE__
std::string msl_normalize_stats_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device float* cache [[buffer(0)]],device float* running_mean [[buffer(1)]],"
      "device float* running_variance [[buffer(2)]],const device float* input [[buffer(3)]],"
      "constant ulong& count [[buffer(4)]],constant ulong& features [[buffer(5)]],constant ulong& inner [[buffer(6)]],"
      "constant ulong& samples [[buffer(7)]],constant float& momentum [[buffer(8)]],constant float& epsilon [[buffer(9)]],"
      "uint gid [[thread_position_in_grid]]){ulong f=ulong(gid);if(f>=features)return;float mean=0.0f;"
      "for(ulong i=0;i<count;++i)if((i/inner)%features==f)mean+=input[i];mean/=float(samples);float variance=0.0f;"
      "for(ulong i=0;i<count;++i)if((i/inner)%features==f){float d=input[i]-mean;variance+=d*d;}variance/=float(samples);"
      "cache[f]=mean;cache[features+f]=1.0f/sqrt(variance+epsilon);"
      "running_mean[f]=(1.0f-momentum)*running_mean[f]+momentum*mean;"
      "running_variance[f]=(1.0f-momentum)*running_variance[f]+momentum*variance;}\n";
}
std::string msl_normalize_apply_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device float* out [[buffer(0)]],const device float* input [[buffer(1)]],"
      "const device float* scale [[buffer(2)]],const device float* bias [[buffer(3)]],const device float* cache [[buffer(4)]],"
      "constant ulong& count [[buffer(5)]],constant ulong& features [[buffer(6)]],constant ulong& inner [[buffer(7)]],"
      "uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;ulong f=(i/inner)%features;"
      "float xhat=(input[i]-cache[f])*cache[features+f];out[i]=xhat*scale[f]+bias[f];}\n";
}
std::string msl_normalize_backward_source(int kind) {
    std::ostringstream o;
    o<<"#include <metal_stdlib>\nusing namespace metal;\nkernel void q_kernel("
      "device float* out [[buffer(0)]],const device float* grad [[buffer(1)]],"
      "const device float* input [[buffer(2)]],const device float* scale [[buffer(3)]],"
      "const device float* cache [[buffer(4)]],constant ulong& count [[buffer(5)]],"
      "constant ulong& features [[buffer(6)]],constant ulong& inner [[buffer(7)]],"
      "constant ulong& samples [[buffer(8)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);";
    if(kind==1){
        o<<"if(i>=count)return;ulong f=(i/inner)%features;float mean=cache[f],inv=cache[features+f],sg=0.0f,sgx=0.0f;"
          "for(ulong j=0;j<count;++j)if((j/inner)%features==f){float xhat=(input[j]-mean)*inv;sg+=grad[j];sgx+=grad[j]*xhat;}"
          "float xhat=(input[i]-mean)*inv;out[i]=(scale[f]*inv/float(samples))*(float(samples)*grad[i]-sg-xhat*sgx);";
    }else{
        o<<"if(i>=features)return;ulong f=i;float sum=0.0f,mean=cache[f],inv=cache[features+f];"
          "for(ulong j=0;j<count;++j)if((j/inner)%features==f){";
        if(kind==2)o<<"float xhat=(input[j]-mean)*inv;sum+=grad[j]*xhat;";
        else o<<"sum+=grad[j];";
        o<<"}out[f]=sum;";
    }
    o<<"}\n";return o.str();
}
#endif

std::string ptx_normalize_stats_source(int dtype) {
    const bool f32=dtype==10;const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n.visible .entry q_kernel("
      ".param .u64 p_cache,.param .u64 p_rm,.param .u64 p_rv,.param .u64 p_input,"
      ".param .u64 p_count,.param .u64 p_features,.param .u64 p_inner,.param .u64 p_samples,"
      ".param .f64 p_momentum,.param .f64 p_epsilon){"
      ".reg .pred %p<8>; .reg .b32 %r<12>; .reg .b64 %rd<48>;"
      <<(f32?".reg .f32 %f<16>; .reg .f64 %fd<4>;":".reg .f64 %fd<16>;")
      <<"ld.param.u64 %rd1,[p_cache];ld.param.u64 %rd2,[p_rm];ld.param.u64 %rd3,[p_rv];"
        "ld.param.u64 %rd4,[p_input];ld.param.u64 %rd5,[p_count];ld.param.u64 %rd6,[p_features];"
        "ld.param.u64 %rd8,[p_inner];ld.param.u64 %rd9,[p_samples];"
        "mov.u32 %r1,%ctaid.x;mov.u32 %r2,%ntid.x;mov.u32 %r3,%tid.x;mad.lo.s32 %r4,%r1,%r2,%r3;"
        "cvt.u64.u32 %rd7,%r4;setp.ge.u64 %p1,%rd7,%rd6;@%p1 bra DONE;";
    if(f32)o<<"mov.f32 %f1,0f00000000;";
    else o<<"mov.f64 %fd1,0d0000000000000000;";
    o<<"mov.u64 %rd10,0;SUM: setp.ge.u64 %p2,%rd10,%rd5;@%p2 bra MEAN;"
      "div.u64 %rd11,%rd10,%rd8;rem.u64 %rd12,%rd11,%rd6;setp.ne.u64 %p3,%rd12,%rd7;@%p3 bra SNEXT;"
      <<"mul.lo.u64 %rd13,%rd10,"<<bytes<<";add.u64 %rd14,%rd4,%rd13;";
    if(f32)o<<"ld.global.f32 %f2,[%rd14];add.f32 %f1,%f1,%f2;";
    else o<<"ld.global.f64 %fd2,[%rd14];add.f64 %fd1,%fd1,%fd2;";
    o<<"SNEXT:add.u64 %rd10,%rd10,1;bra SUM;MEAN:";
    if(f32)o<<"cvt.rn.f32.u64 %f3,%rd9;div.f32 %f4,%f1,%f3;mov.f32 %f5,0f00000000;";
    else o<<"cvt.rn.f64.u64 %fd3,%rd9;div.f64 %fd4,%fd1,%fd3;mov.f64 %fd5,0d0000000000000000;";
    o<<"mov.u64 %rd10,0;VAR:setp.ge.u64 %p2,%rd10,%rd5;@%p2 bra VDONE;"
      "div.u64 %rd11,%rd10,%rd8;rem.u64 %rd12,%rd11,%rd6;setp.ne.u64 %p3,%rd12,%rd7;@%p3 bra VNEXT;"
      <<"mul.lo.u64 %rd13,%rd10,"<<bytes<<";add.u64 %rd14,%rd4,%rd13;";
    if(f32)o<<"ld.global.f32 %f2,[%rd14];sub.f32 %f6,%f2,%f4;mul.f32 %f6,%f6,%f6;add.f32 %f5,%f5,%f6;";
    else o<<"ld.global.f64 %fd2,[%rd14];sub.f64 %fd6,%fd2,%fd4;mul.f64 %fd6,%fd6,%fd6;add.f64 %fd5,%fd5,%fd6;";
    o<<"VNEXT:add.u64 %rd10,%rd10,1;bra VAR;VDONE:";
    if(f32){
      o<<"div.f32 %f5,%f5,%f3;ld.param.f64 %fd1,[p_epsilon];cvt.rn.f32.f64 %f7,%fd1;add.f32 %f8,%f5,%f7;"
        "sqrt.rn.f32 %f9,%f8;rcp.approx.f32 %f10,%f9;";
    }else{
      o<<"div.f64 %fd5,%fd5,%fd3;ld.param.f64 %fd7,[p_epsilon];add.f64 %fd8,%fd5,%fd7;sqrt.rn.f64 %fd9,%fd8;"
        "mov.f64 %fd10,0d3ff0000000000000;div.f64 %fd10,%fd10,%fd9;";
    }
    o<<"mul.lo.u64 %rd20,%rd7,"<<bytes<<";add.u64 %rd21,%rd1,%rd20;"
      "add.u64 %rd22,%rd7,%rd6;mul.lo.u64 %rd22,%rd22,"<<bytes<<";add.u64 %rd23,%rd1,%rd22;"
      "add.u64 %rd24,%rd2,%rd20;add.u64 %rd25,%rd3,%rd20;";
    if(f32){
      o<<"st.global.f32 [%rd21],%f4;st.global.f32 [%rd23],%f10;ld.global.f32 %f11,[%rd24];ld.global.f32 %f12,[%rd25];"
        "ld.param.f64 %fd2,[p_momentum];cvt.rn.f32.f64 %f13,%fd2;mov.f32 %f14,0f3f800000;sub.f32 %f14,%f14,%f13;"
        "mul.f32 %f11,%f11,%f14;mul.f32 %f4,%f4,%f13;add.f32 %f11,%f11,%f4;"
        "mul.f32 %f12,%f12,%f14;mul.f32 %f5,%f5,%f13;add.f32 %f12,%f12,%f5;"
        "st.global.f32 [%rd24],%f11;st.global.f32 [%rd25],%f12;";
    }else{
      o<<"st.global.f64 [%rd21],%fd4;st.global.f64 [%rd23],%fd10;ld.global.f64 %fd11,[%rd24];ld.global.f64 %fd12,[%rd25];"
        "ld.param.f64 %fd13,[p_momentum];mov.f64 %fd14,0d3ff0000000000000;sub.f64 %fd14,%fd14,%fd13;"
        "mul.f64 %fd11,%fd11,%fd14;mul.f64 %fd4,%fd4,%fd13;add.f64 %fd11,%fd11,%fd4;"
        "mul.f64 %fd12,%fd12,%fd14;mul.f64 %fd5,%fd5,%fd13;add.f64 %fd12,%fd12,%fd5;"
        "st.global.f64 [%rd24],%fd11;st.global.f64 [%rd25],%fd12;";
    }
    o<<"DONE:ret;}";return o.str();
}

std::string ptx_normalize_apply_source(int dtype) {
    const bool f32=dtype==10;const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<ptx_thread_prologue(
      " .param .u64 p_out,\n .param .u64 p_input,\n .param .u64 p_scale,\n .param .u64 p_bias,\n"
      " .param .u64 p_cache,\n .param .u64 p_count,\n .param .u64 p_features,\n .param .u64 p_inner\n",
      f32?" .reg .pred %p<2>;\n .reg .b32 %r<8>;\n .reg .b64 %rd<28>;\n .reg .f32 %f<8>;\n":
          " .reg .pred %p<2>;\n .reg .b32 %r<8>;\n .reg .b64 %rd<28>;\n .reg .f64 %fd<8>;\n");
    o<<"ld.param.u64 %rd1,[p_out];ld.param.u64 %rd2,[p_input];ld.param.u64 %rd3,[p_scale];"
      "ld.param.u64 %rd4,[p_bias];ld.param.u64 %rd5,[p_cache];ld.param.u64 %rd6,[p_count];"
      "ld.param.u64 %rd18,[p_features];ld.param.u64 %rd19,[p_inner];setp.ge.u64 %p1,%rd7,%rd6;@%p1 bra DONE;"
      "div.u64 %rd20,%rd7,%rd19;rem.u64 %rd21,%rd20,%rd18;"
      <<"mul.lo.u64 %rd22,%rd7,"<<bytes<<";add.u64 %rd23,%rd2,%rd22;add.u64 %rd24,%rd1,%rd22;"
      <<"mul.lo.u64 %rd25,%rd21,"<<bytes<<";add.u64 %rd26,%rd3,%rd25;add.u64 %rd27,%rd4,%rd25;"
      "add.u64 %rd28,%rd5,%rd25;add.u64 %rd29,%rd21,%rd18;mul.lo.u64 %rd29,%rd29,"<<bytes<<";add.u64 %rd30,%rd5,%rd29;";
    if(f32)o<<"ld.global.f32 %f1,[%rd23];ld.global.f32 %f2,[%rd26];ld.global.f32 %f3,[%rd27];ld.global.f32 %f4,[%rd28];ld.global.f32 %f5,[%rd30];"
      "sub.f32 %f1,%f1,%f4;mul.f32 %f1,%f1,%f5;mul.f32 %f1,%f1,%f2;add.f32 %f1,%f1,%f3;st.global.f32 [%rd24],%f1;";
    else o<<"ld.global.f64 %fd1,[%rd23];ld.global.f64 %fd2,[%rd26];ld.global.f64 %fd3,[%rd27];ld.global.f64 %fd4,[%rd28];ld.global.f64 %fd5,[%rd30];"
      "sub.f64 %fd1,%fd1,%fd4;mul.f64 %fd1,%fd1,%fd5;mul.f64 %fd1,%fd1,%fd2;add.f64 %fd1,%fd1,%fd3;st.global.f64 [%rd24],%fd1;";
    o<<"DONE:ret;}\n";return o.str();
}

std::string ptx_normalize_backward_source(int dtype,int kind) {
    const bool f32=dtype==10;const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n.visible .entry q_kernel("
      ".param .u64 p_out,.param .u64 p_grad,.param .u64 p_input,.param .u64 p_scale,.param .u64 p_cache,"
      ".param .u64 p_count,.param .u64 p_features,.param .u64 p_inner,.param .u64 p_samples){"
      ".reg .pred %p<8>; .reg .b32 %r<12>; .reg .b64 %rd<64>;"
      <<(f32?".reg .f32 %f<16>;":".reg .f64 %fd<16>;")
      <<"ld.param.u64 %rd1,[p_out];ld.param.u64 %rd2,[p_grad];ld.param.u64 %rd3,[p_input];"
        "ld.param.u64 %rd4,[p_scale];ld.param.u64 %rd5,[p_cache];ld.param.u64 %rd6,[p_count];"
        "ld.param.u64 %rd8,[p_features];ld.param.u64 %rd9,[p_inner];ld.param.u64 %rd10,[p_samples];"
        "mov.u32 %r1,%ctaid.x;mov.u32 %r2,%ntid.x;mov.u32 %r3,%tid.x;mad.lo.s32 %r4,%r1,%r2,%r3;cvt.u64.u32 %rd7,%r4;";
    if(kind==1){
      o<<"setp.ge.u64 %p1,%rd7,%rd6;@%p1 bra DONE;div.u64 %rd11,%rd7,%rd9;rem.u64 %rd12,%rd11,%rd8;";
    }else{
      o<<"setp.ge.u64 %p1,%rd7,%rd8;@%p1 bra DONE;mov.u64 %rd12,%rd7;";
    }
    o<<"mul.lo.u64 %rd13,%rd12,"<<bytes<<";add.u64 %rd14,%rd5,%rd13;"
      "add.u64 %rd15,%rd12,%rd8;mul.lo.u64 %rd15,%rd15,"<<bytes<<";add.u64 %rd16,%rd5,%rd15;";
    if(f32)o<<"ld.global.f32 %f1,[%rd14];ld.global.f32 %f2,[%rd16];mov.f32 %f3,0f00000000;mov.f32 %f4,0f00000000;";
    else o<<"ld.global.f64 %fd1,[%rd14];ld.global.f64 %fd2,[%rd16];mov.f64 %fd3,0d0000000000000000;mov.f64 %fd4,0d0000000000000000;";
    o<<"mov.u64 %rd20,0;LOOP:setp.ge.u64 %p2,%rd20,%rd6;@%p2 bra REDUCE;div.u64 %rd21,%rd20,%rd9;rem.u64 %rd22,%rd21,%rd8;"
      "setp.ne.u64 %p3,%rd22,%rd12;@%p3 bra NEXT;mul.lo.u64 %rd23,%rd20,"<<bytes<<";"
      "add.u64 %rd24,%rd2,%rd23;add.u64 %rd25,%rd3,%rd23;";
    if(f32)o<<"ld.global.f32 %f5,[%rd24];ld.global.f32 %f6,[%rd25];sub.f32 %f6,%f6,%f1;mul.f32 %f6,%f6,%f2;";
    else o<<"ld.global.f64 %fd5,[%rd24];ld.global.f64 %fd6,[%rd25];sub.f64 %fd6,%fd6,%fd1;mul.f64 %fd6,%fd6,%fd2;";
    if(kind==3){
      if(f32)o<<"add.f32 %f3,%f3,%f5;";
      else o<<"add.f64 %fd3,%fd3,%fd5;";
    }else{
      if(f32)o<<"mul.f32 %f7,%f5,%f6;add.f32 %f3,%f3,%f7;";
      else o<<"mul.f64 %fd7,%fd5,%fd6;add.f64 %fd3,%fd3,%fd7;";
      if(kind==1){
        if(f32)o<<"add.f32 %f4,%f4,%f5;";
        else o<<"add.f64 %fd4,%fd4,%fd5;";
      }
    }
    o<<"NEXT:add.u64 %rd20,%rd20,1;bra LOOP;REDUCE:";
    if(kind==1){
      o<<"mul.lo.u64 %rd30,%rd7,"<<bytes<<";add.u64 %rd31,%rd2,%rd30;add.u64 %rd32,%rd3,%rd30;add.u64 %rd33,%rd1,%rd30;"
        "add.u64 %rd34,%rd4,%rd13;";
      if(f32)o<<"ld.global.f32 %f5,[%rd31];ld.global.f32 %f6,[%rd32];ld.global.f32 %f8,[%rd34];sub.f32 %f6,%f6,%f1;mul.f32 %f6,%f6,%f2;"
        "cvt.rn.f32.u64 %f9,%rd10;mul.f32 %f10,%f9,%f5;sub.f32 %f10,%f10,%f4;mul.f32 %f6,%f6,%f3;sub.f32 %f10,%f10,%f6;"
        "mul.f32 %f8,%f8,%f2;div.f32 %f8,%f8,%f9;mul.f32 %f10,%f10,%f8;st.global.f32 [%rd33],%f10;";
      else o<<"ld.global.f64 %fd5,[%rd31];ld.global.f64 %fd6,[%rd32];ld.global.f64 %fd8,[%rd34];sub.f64 %fd6,%fd6,%fd1;mul.f64 %fd6,%fd6,%fd2;"
        "cvt.rn.f64.u64 %fd9,%rd10;mul.f64 %fd10,%fd9,%fd5;sub.f64 %fd10,%fd10,%fd4;mul.f64 %fd6,%fd6,%fd3;sub.f64 %fd10,%fd10,%fd6;"
        "mul.f64 %fd8,%fd8,%fd2;div.f64 %fd8,%fd8,%fd9;mul.f64 %fd10,%fd10,%fd8;st.global.f64 [%rd33],%fd10;";
    }else{
      o<<"mul.lo.u64 %rd30,%rd7,"<<bytes<<";add.u64 %rd31,%rd1,%rd30;";
      if(f32)o<<"st.global.f32 [%rd31],%f3;";
      else o<<"st.global.f64 [%rd31],%fd3;";
    }
    o<<"DONE:ret;}\n";return o.str();
}

std::string hip_random_mask_source(int dtype) {
    const char* t=dtype==10?"float":"double";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* out,"<<t<<"* mask,const "<<t<<"* input,unsigned long long count,"
      "unsigned long long state,unsigned long long cutoff,double scale_raw){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
      "unsigned long long z=state+(i+1ULL)*0x9e3779b97f4a7c15ULL;"
      "z=(z^(z>>30U))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27U))*0x94d049bb133111ebULL;z^=z>>31U;"
      "unsigned long long bits=z>>11U;"<<t<<" m=bits<cutoff?("<<t<<")0:("<<t<<")scale_raw;"
      "mask[i]=m;out[i]=input[i]*m;}\n";
    return o.str();
}
#ifdef __APPLE__
std::string msl_random_mask_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device float* out [[buffer(0)]],device float* mask [[buffer(1)]],"
      "const device float* input [[buffer(2)]],constant ulong& count [[buffer(3)]],"
      "constant ulong& state [[buffer(4)]],constant ulong& cutoff [[buffer(5)]],constant float& scale [[buffer(6)]],"
      "uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"
      "ulong z=state+(i+1ul)*0x9e3779b97f4a7c15ul;z=(z^(z>>30u))*0xbf58476d1ce4e5b9ul;"
      "z=(z^(z>>27u))*0x94d049bb133111ebul;z^=z>>31u;ulong bits=z>>11u;"
      "float m=bits<cutoff?0.0f:scale;mask[i]=m;out[i]=input[i]*m;}\n";
}
#endif
std::string ptx_random_mask_source(int dtype) {
    const bool f32=dtype==10;const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<ptx_thread_prologue(
      " .param .u64 p_out,\n .param .u64 p_mask,\n .param .u64 p_input,\n .param .u64 p_count,\n"
      " .param .u64 p_state,\n .param .u64 p_cutoff,\n .param .f64 p_scale\n",
      f32?" .reg .pred %p<4>;\n .reg .b32 %r<10>;\n .reg .b64 %rd<32>;\n .reg .f32 %f<6>;\n .reg .f64 %fd<2>;\n":
          " .reg .pred %p<4>;\n .reg .b32 %r<10>;\n .reg .b64 %rd<32>;\n .reg .f64 %fd<6>;\n");
    o<<"ld.param.u64 %rd1,[p_out];ld.param.u64 %rd2,[p_mask];ld.param.u64 %rd3,[p_input];ld.param.u64 %rd5,[p_count];"
      "ld.param.u64 %rd18,[p_state];ld.param.u64 %rd19,[p_cutoff];setp.ge.u64 %p1,%rd7,%rd5;@%p1 bra DONE;"
      "add.u64 %rd20,%rd7,1;mov.u64 %rd21,0x9e3779b97f4a7c15;mul.lo.u64 %rd20,%rd20,%rd21;add.u64 %rd20,%rd20,%rd18;"
      "shr.u64 %rd22,%rd20,30;xor.b64 %rd20,%rd20,%rd22;mov.u64 %rd23,0xbf58476d1ce4e5b9;mul.lo.u64 %rd20,%rd20,%rd23;"
      "shr.u64 %rd22,%rd20,27;xor.b64 %rd20,%rd20,%rd22;mov.u64 %rd23,0x94d049bb133111eb;mul.lo.u64 %rd20,%rd20,%rd23;"
      "shr.u64 %rd22,%rd20,31;xor.b64 %rd20,%rd20,%rd22;shr.u64 %rd20,%rd20,11;"
      "setp.lt.u64 %p2,%rd20,%rd19;"
      <<"mul.lo.u64 %rd24,%rd7,"<<bytes<<";add.u64 %rd25,%rd3,%rd24;add.u64 %rd26,%rd2,%rd24;add.u64 %rd27,%rd1,%rd24;";
    if(f32)o<<"ld.param.f64 %fd1,[p_scale];cvt.rn.f32.f64 %f1,%fd1;@%p2 mov.f32 %f1,0f00000000;ld.global.f32 %f2,[%rd25];mul.f32 %f3,%f2,%f1;st.global.f32 [%rd26],%f1;st.global.f32 [%rd27],%f3;";
    else o<<"ld.param.f64 %fd1,[p_scale];@%p2 mov.f64 %fd1,0d0000000000000000;ld.global.f64 %fd2,[%rd25];mul.f64 %fd3,%fd2,%fd1;st.global.f64 [%rd26],%fd1;st.global.f64 [%rd27],%fd3;";
    o<<"DONE:ret;}\n";return o.str();
}

std::string hip_moment_source(int dtype) {
    const char* t=dtype==10?"float":"double";const char* sqrtfn=dtype==10?"sqrtf":"sqrt";
    std::ostringstream o;
    o<<"#include <hip/hip_runtime.h>\n#include <math.h>\nextern \"C\" __global__ void q_kernel("
      <<t<<"* parameter,const "<<t<<"* gradient,"<<t<<"* first,"<<t<<"* second,"
      "unsigned long long count,double rate_raw,double beta1_raw,double beta2_raw,double epsilon_raw,"
      "double correction1_raw,double correction2_raw){unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i>=count)return;"<<t<<" rate=("<<t<<")rate_raw,beta1=("<<t<<")beta1_raw,beta2=("<<t<<")beta2_raw,"
      "epsilon=("<<t<<")epsilon_raw,c1=("<<t<<")correction1_raw,c2=("<<t<<")correction2_raw,g=gradient[i];"
      <<t<<" m=beta1*first[i]+(("<<t<<")1-beta1)*g;"<<t<<" v=beta2*second[i]+(("<<t<<")1-beta2)*g*g;"
      "first[i]=m;second[i]=v;parameter[i]-=rate*(m/c1)/("<<sqrtfn<<"(v/c2)+epsilon);}\n";
    return o.str();
}
#ifdef __APPLE__
std::string msl_moment_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device float* parameter [[buffer(0)]],const device float* gradient [[buffer(1)]],"
      "device float* first [[buffer(2)]],device float* second [[buffer(3)]],constant ulong& count [[buffer(4)]],"
      "constant float& rate [[buffer(5)]],constant float& beta1 [[buffer(6)]],constant float& beta2 [[buffer(7)]],"
      "constant float& epsilon [[buffer(8)]],constant float& c1 [[buffer(9)]],constant float& c2 [[buffer(10)]],"
      "uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;float g=gradient[i];"
      "float m=beta1*first[i]+(1.0f-beta1)*g;float v=beta2*second[i]+(1.0f-beta2)*g*g;"
      "first[i]=m;second[i]=v;parameter[i]-=rate*(m/c1)/(sqrt(v/c2)+epsilon);}\n";
}
#endif
std::string ptx_moment_source(int dtype) {
    const bool f32=dtype==10;const auto bytes=f32?4:8;
    std::ostringstream o;
    o<<ptx_thread_prologue(
      " .param .u64 p_parameter,\n .param .u64 p_gradient,\n .param .u64 p_first,\n .param .u64 p_second,\n"
      " .param .u64 p_count,\n .param .f64 p_rate,\n .param .f64 p_beta1,\n .param .f64 p_beta2,\n"
      " .param .f64 p_epsilon,\n .param .f64 p_c1,\n .param .f64 p_c2\n",
      f32?" .reg .pred %p<2>;\n .reg .b32 %r<8>;\n .reg .b64 %rd<24>;\n .reg .f32 %f<18>;\n .reg .f64 %fd<8>;\n":
          " .reg .pred %p<2>;\n .reg .b32 %r<8>;\n .reg .b64 %rd<24>;\n .reg .f64 %fd<18>;\n");
    o<<"ld.param.u64 %rd1,[p_parameter];ld.param.u64 %rd2,[p_gradient];ld.param.u64 %rd3,[p_first];ld.param.u64 %rd4,[p_second];"
      "ld.param.u64 %rd5,[p_count];setp.ge.u64 %p1,%rd7,%rd5;@%p1 bra DONE;mul.lo.u64 %rd8,%rd7,"<<bytes<<";"
      "add.u64 %rd9,%rd1,%rd8;add.u64 %rd10,%rd2,%rd8;add.u64 %rd11,%rd3,%rd8;add.u64 %rd12,%rd4,%rd8;";
    if(f32){
      o<<"ld.global.f32 %f1,[%rd9];ld.global.f32 %f2,[%rd10];ld.global.f32 %f3,[%rd11];ld.global.f32 %f4,[%rd12];"
        "ld.param.f64 %fd1,[p_rate];cvt.rn.f32.f64 %f5,%fd1;ld.param.f64 %fd2,[p_beta1];cvt.rn.f32.f64 %f6,%fd2;"
        "ld.param.f64 %fd3,[p_beta2];cvt.rn.f32.f64 %f7,%fd3;ld.param.f64 %fd4,[p_epsilon];cvt.rn.f32.f64 %f8,%fd4;"
        "ld.param.f64 %fd5,[p_c1];cvt.rn.f32.f64 %f9,%fd5;ld.param.f64 %fd6,[p_c2];cvt.rn.f32.f64 %f10,%fd6;"
        "mov.f32 %f11,0f3f800000;sub.f32 %f12,%f11,%f6;mul.f32 %f3,%f3,%f6;mul.f32 %f13,%f12,%f2;add.f32 %f3,%f3,%f13;"
        "sub.f32 %f12,%f11,%f7;mul.f32 %f4,%f4,%f7;mul.f32 %f13,%f2,%f2;mul.f32 %f13,%f13,%f12;add.f32 %f4,%f4,%f13;"
        "st.global.f32 [%rd11],%f3;st.global.f32 [%rd12],%f4;div.f32 %f14,%f3,%f9;div.f32 %f15,%f4,%f10;"
        "sqrt.rn.f32 %f15,%f15;add.f32 %f15,%f15,%f8;div.f32 %f14,%f14,%f15;mul.f32 %f14,%f14,%f5;sub.f32 %f1,%f1,%f14;"
        "st.global.f32 [%rd9],%f1;";
    }else{
      o<<"ld.global.f64 %fd1,[%rd9];ld.global.f64 %fd2,[%rd10];ld.global.f64 %fd3,[%rd11];ld.global.f64 %fd4,[%rd12];"
        "ld.param.f64 %fd5,[p_rate];ld.param.f64 %fd6,[p_beta1];ld.param.f64 %fd7,[p_beta2];ld.param.f64 %fd8,[p_epsilon];"
        "ld.param.f64 %fd9,[p_c1];ld.param.f64 %fd10,[p_c2];mov.f64 %fd11,0d3ff0000000000000;sub.f64 %fd12,%fd11,%fd6;"
        "mul.f64 %fd3,%fd3,%fd6;mul.f64 %fd13,%fd12,%fd2;add.f64 %fd3,%fd3,%fd13;sub.f64 %fd12,%fd11,%fd7;"
        "mul.f64 %fd4,%fd4,%fd7;mul.f64 %fd13,%fd2,%fd2;mul.f64 %fd13,%fd13,%fd12;add.f64 %fd4,%fd4,%fd13;"
        "st.global.f64 [%rd11],%fd3;st.global.f64 [%rd12],%fd4;div.f64 %fd14,%fd3,%fd9;div.f64 %fd15,%fd4,%fd10;"
        "sqrt.rn.f64 %fd15,%fd15;add.f64 %fd15,%fd15,%fd8;div.f64 %fd14,%fd14,%fd15;mul.f64 %fd14,%fd14,%fd5;"
        "sub.f64 %fd1,%fd1,%fd14;st.global.f64 [%rd9],%fd1;";
    }
    o<<"DONE:ret;}\n";return o.str();
}

} // namespace


bool cudnn_conv2d_backward(
    Buffer* input_gradient,Buffer* weight_gradient,Buffer* bias_gradient,
    const Buffer* gradient,const Buffer* input,const Buffer* weight,
    int dtype,std::size_t batches,std::size_t channels_in,std::size_t height,
    std::size_t width,std::size_t channels_out,std::size_t kernel_h,
    std::size_t kernel_w,std::size_t output_h,std::size_t output_w,
    std::size_t stride,std::size_t padding) {
    if(input_gradient->backend!=Backend::Nvidia||(dtype!=9&&dtype!=10))return false;
    const auto fits=[](std::size_t v){
        return v<=static_cast<std::size_t>(std::numeric_limits<int>::max());
    };
    for(auto v:{batches,channels_in,height,width,channels_out,kernel_h,kernel_w,
                output_h,output_w,stride,padding})if(!fits(v))return false;
    auto& api=cudnn();
    if(!api.ready)return false;
    std::string ignored; auto handle=api.handle(input_gradient->backend_index,ignored);
    if(!handle)return false;
    auto operation_mutex=api.operation_mutex(input_gradient->backend_index);
    std::lock_guard operation_lock(*operation_mutex);
    const auto mode=dnn_mode();

    CudnnApi::TensorDescriptor x_desc=nullptr,dy_desc=nullptr,dx_desc=nullptr,db_desc=nullptr;
    CudnnApi::FilterDescriptor w_desc=nullptr,dw_desc=nullptr;
    CudnnApi::ConvolutionDescriptor conv_desc=nullptr;
    const auto cleanup=[&]{
        if(x_desc)api.destroy_tensor(x_desc);
        if(dy_desc)api.destroy_tensor(dy_desc);
        if(dx_desc)api.destroy_tensor(dx_desc);
        if(db_desc)api.destroy_tensor(db_desc);
        if(w_desc)api.destroy_filter(w_desc);
        if(dw_desc)api.destroy_filter(dw_desc);
        if(conv_desc)api.destroy_convolution(conv_desc);
    };
    if(api.create_tensor(&x_desc)!=0||api.create_tensor(&dy_desc)!=0||
       api.create_tensor(&dx_desc)!=0||api.create_tensor(&db_desc)!=0||
       api.create_filter(&w_desc)!=0||api.create_filter(&dw_desc)!=0||
       api.create_convolution(&conv_desc)!=0){cleanup();return false;}
    constexpr int nchw=0,cross_correlation=1;
    const int data_type=dtype==10?0:1;
    if(api.set_tensor4d(x_desc,nchw,data_type,static_cast<int>(batches),
                        static_cast<int>(channels_in),static_cast<int>(height),
                        static_cast<int>(width))!=0||
       api.set_tensor4d(dx_desc,nchw,data_type,static_cast<int>(batches),
                        static_cast<int>(channels_in),static_cast<int>(height),
                        static_cast<int>(width))!=0||
       api.set_tensor4d(dy_desc,nchw,data_type,static_cast<int>(batches),
                        static_cast<int>(channels_out),static_cast<int>(output_h),
                        static_cast<int>(output_w))!=0||
       api.set_tensor4d(db_desc,nchw,data_type,1,static_cast<int>(channels_out),1,1)!=0||
       api.set_filter4d(w_desc,data_type,nchw,static_cast<int>(channels_out),
                        static_cast<int>(channels_in),static_cast<int>(kernel_h),
                        static_cast<int>(kernel_w))!=0||
       api.set_filter4d(dw_desc,data_type,nchw,static_cast<int>(channels_out),
                        static_cast<int>(channels_in),static_cast<int>(kernel_h),
                        static_cast<int>(kernel_w))!=0||
       api.set_convolution2d(conv_desc,static_cast<int>(padding),static_cast<int>(padding),
                             static_cast<int>(stride),static_cast<int>(stride),1,1,
                             cross_correlation,data_type)!=0){cleanup();return false;}
    if(api.set_convolution_math_type){
        constexpr int default_math=0,fma_math=3;
        (void)api.set_convolution_math_type(
            conv_desc,mode==DnnMode::Deterministic?fma_math:default_math);
    }

    const bool deterministic=mode==DnnMode::Deterministic;
    std::ostringstream algorithm_key_stream;
    algorithm_key_stream<<input_gradient->global_index<<':'<<dtype<<':'
        <<static_cast<int>(mode)<<':'<<batches<<':'<<channels_in<<':'
        <<height<<':'<<width<<':'<<channels_out<<':'<<kernel_h<<':'
        <<kernel_w<<':'<<output_h<<':'<<output_w<<':'<<stride<<':'<<padding;
    const auto algorithm_key=algorithm_key_stream.str();
    static std::mutex data_cache_mutex,filter_cache_mutex;
    static std::unordered_map<std::string,std::pair<int,std::size_t>> data_cache,filter_cache;

    auto load_cached=[&](auto& mutex,auto& cache,int& algo,std::size_t& bytes,Buffer*& ws){
        std::lock_guard lock(mutex);
        if(const auto found=cache.find(algorithm_key);found!=cache.end()){
            algo=found->second.first;bytes=found->second.second;
        }
        if(algo>=0&&bytes){
            std::string alloc_error;
            ws=allocate(input_gradient->global_index,bytes,alloc_error);
            if(!ws){cache.erase(algorithm_key);algo=-1;bytes=0;}
        }
    };

    int data_algo=-1; Buffer* data_ws=nullptr; std::size_t data_ws_bytes=0;
    load_cached(data_cache_mutex,data_cache,data_algo,data_ws_bytes,data_ws);
    if(data_algo<0){
        std::array<CudnnApi::BwdDataPerf,8> data_perf{};
        int data_returned=0;
        const auto query=(!deterministic&&api.find_bwd_data_algorithms)
            ?api.find_bwd_data_algorithms:api.get_bwd_data_algorithms;
        if(query(handle,w_desc,dy_desc,conv_desc,dx_desc,
                 static_cast<int>(data_perf.size()),&data_returned,data_perf.data())!=0||
           data_returned<=0){cleanup();return false;}
        for(int i=0;i<data_returned;++i){
            const auto& p=data_perf[static_cast<std::size_t>(i)];
            if(p.status!=0||(deterministic&&p.determinism!=1))continue;
            std::size_t bytes=0;
            if(api.get_bwd_data_workspace(handle,w_desc,dy_desc,conv_desc,dx_desc,p.algo,&bytes)!=0)continue;
            std::string alloc_error;
            Buffer* ws=bytes?allocate(input_gradient->global_index,bytes,alloc_error):nullptr;
            if(bytes&&!ws)continue;
            data_algo=p.algo;data_ws=ws;data_ws_bytes=bytes;
            {std::lock_guard lock(data_cache_mutex);data_cache[algorithm_key]={data_algo,data_ws_bytes};}
            break;
        }
    }
    if(data_algo<0){cleanup();return false;}

    int filter_algo=-1; Buffer* filter_ws=nullptr; std::size_t filter_ws_bytes=0;
    load_cached(filter_cache_mutex,filter_cache,filter_algo,filter_ws_bytes,filter_ws);
    if(filter_algo<0){
        std::array<CudnnApi::BwdFilterPerf,8> filter_perf{};
        int filter_returned=0;
        const auto query=(!deterministic&&api.find_bwd_filter_algorithms)
            ?api.find_bwd_filter_algorithms:api.get_bwd_filter_algorithms;
        if(query(handle,x_desc,dy_desc,conv_desc,dw_desc,
                 static_cast<int>(filter_perf.size()),&filter_returned,filter_perf.data())!=0||
           filter_returned<=0){
            if(data_ws)release(data_ws);
            cleanup();
            return false;
        }
        for(int i=0;i<filter_returned;++i){
            const auto& p=filter_perf[static_cast<std::size_t>(i)];
            if(p.status!=0||(deterministic&&p.determinism!=1))continue;
            std::size_t bytes=0;
            if(api.get_bwd_filter_workspace(handle,x_desc,dy_desc,conv_desc,dw_desc,p.algo,&bytes)!=0)continue;
            std::string alloc_error;
            Buffer* ws=bytes?allocate(input_gradient->global_index,bytes,alloc_error):nullptr;
            if(bytes&&!ws)continue;
            filter_algo=p.algo;filter_ws=ws;filter_ws_bytes=bytes;
            {std::lock_guard lock(filter_cache_mutex);filter_cache[algorithm_key]={filter_algo,filter_ws_bytes};}
            break;
        }
    }
    if(filter_algo<0){
        if(data_ws)release(data_ws);
        cleanup();
        return false;
    }

    const auto ptr=[](const Buffer* b){
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(b->cuda_pointer));
    };
    const auto mut=[](Buffer* b){
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(b->cuda_pointer));
    };
    void* data_ws_ptr=data_ws?mut(data_ws):nullptr;
    void* filter_ws_ptr=filter_ws?mut(filter_ws):nullptr;
    int status=0;
    if(dtype==10){
        const float one=1.0f,zero=0.0f;
        status=api.convolution_backward_data(handle,&one,w_desc,ptr(weight),dy_desc,
            ptr(gradient),conv_desc,data_algo,data_ws_ptr,data_ws_bytes,&zero,
            dx_desc,mut(input_gradient));
        if(status==0)status=api.convolution_backward_filter(handle,&one,x_desc,ptr(input),
            dy_desc,ptr(gradient),conv_desc,filter_algo,filter_ws_ptr,filter_ws_bytes,
            &zero,dw_desc,mut(weight_gradient));
        if(status==0)status=api.convolution_backward_bias(handle,&one,dy_desc,ptr(gradient),
            &zero,db_desc,mut(bias_gradient));
    }else{
        const double one=1.0,zero=0.0;
        status=api.convolution_backward_data(handle,&one,w_desc,ptr(weight),dy_desc,
            ptr(gradient),conv_desc,data_algo,data_ws_ptr,data_ws_bytes,&zero,
            dx_desc,mut(input_gradient));
        if(status==0)status=api.convolution_backward_filter(handle,&one,x_desc,ptr(input),
            dy_desc,ptr(gradient),conv_desc,filter_algo,filter_ws_ptr,filter_ws_bytes,
            &zero,dw_desc,mut(weight_gradient));
        if(status==0)status=api.convolution_backward_bias(handle,&one,dy_desc,ptr(gradient),
            &zero,db_desc,mut(bias_gradient));
    }
    if(data_ws)release(data_ws);
    if(filter_ws)release(filter_ws);
    cleanup();
    if(status!=0){
        {std::lock_guard lock(data_cache_mutex);data_cache.erase(algorithm_key);}
        {std::lock_guard lock(filter_cache_mutex);filter_cache.erase(algorithm_key);}
        return false;
    }
    auto& cu=cuda(); return cu.ctx_synchronize&&cu.ctx_synchronize()==0;
}

bool compute_conv2d_backward(
    Buffer* input_gradient,Buffer* weight_gradient,Buffer* bias_gradient,
    const Buffer* gradient,const Buffer* input,const Buffer* weight,
    int dtype,std::size_t batches,std::size_t channels_in,std::size_t height,
    std::size_t width,std::size_t channels_out,std::size_t kernel_h,
    std::size_t kernel_w,std::size_t output_h,std::size_t output_w,
    std::size_t stride,std::size_t padding,std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    const auto input_count=batches*channels_in*height*width;
    const auto weight_count=channels_out*channels_in*kernel_h*kernel_w;
    const auto gradient_count=batches*channels_out*output_h*output_w;
    if(!compute_same_device(input_gradient,gradient,error)||
       !compute_same_device(input_gradient,input,error)||
       !compute_same_device(input_gradient,weight,error)||
       !compute_same_device(input_gradient,weight_gradient,error)||
       !compute_same_device(input_gradient,bias_gradient,error)||
       !compute_buffer_range(input_gradient,0,input_count,bytes,error)||
       !compute_buffer_range(weight_gradient,0,weight_count,bytes,error)||
       !compute_buffer_range(bias_gradient,0,channels_out,bytes,error)||
       !compute_buffer_range(gradient,0,gradient_count,bytes,error)||
       !compute_buffer_range(input,0,input_count,bytes,error)||
       !compute_buffer_range(weight,0,weight_count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(input_gradient->backend==Backend::Test){
        if(dtype==10)return fake_conv2d_backward<float>(input_gradient,weight_gradient,bias_gradient,gradient,input,weight,batches,channels_in,height,width,channels_out,kernel_h,kernel_w,output_h,output_w,stride,padding);
        if(dtype==9)return fake_conv2d_backward<double>(input_gradient,weight_gradient,bias_gradient,gradient,input,weight,batches,channels_in,height,width,channels_out,kernel_h,kernel_w,output_h,output_w,stride,padding);
        error="neural convolution backward requires a floating dtype";return false;
    }
#endif
    const auto launch_counts=std::array<std::size_t,3>{input_count,weight_count,channels_out};
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Nvidia){
        if(cudnn_conv2d_backward(input_gradient,weight_gradient,bias_gradient,
                                 gradient,input,weight,dtype,batches,channels_in,
                                 height,width,channels_out,kernel_h,kernel_w,
                                 output_h,output_w,stride,padding))
            return true;
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?weight_gradient:bias_gradient;
            auto ptx=ptx_conv2d_backward_source(dtype,kind);
            auto out_ptr=target->cuda_pointer,g_ptr=gradient->cuda_pointer,
                 in_ptr=input->cuda_pointer,w_ptr=weight->cuda_pointer;
            auto count=static_cast<std::uint64_t>(launch_counts[kind-1]);
            auto ba=static_cast<std::uint64_t>(batches),ci=static_cast<std::uint64_t>(channels_in),
                 h=static_cast<std::uint64_t>(height),w=static_cast<std::uint64_t>(width),
                 co=static_cast<std::uint64_t>(channels_out),kh=static_cast<std::uint64_t>(kernel_h),
                 kw=static_cast<std::uint64_t>(kernel_w),oh=static_cast<std::uint64_t>(output_h),
                 ow=static_cast<std::uint64_t>(output_w),st=static_cast<std::uint64_t>(stride),
                 pa=static_cast<std::uint64_t>(padding);
            void* args[]={&out_ptr,&g_ptr,&in_ptr,&w_ptr,&count,&ba,&ci,&h,&w,&co,&kh,&kw,&oh,&ow,&st,&pa};
            if(!launch_ptx_1d(target,"conv2d-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),
                              ptx,launch_counts[kind-1],args,error))return false;
        }
        return true;
    }
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Amd){
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?weight_gradient:bias_gradient;
            auto source=hip_conv2d_backward_source(dtype,kind);
            auto out_ptr=target->pointer,g_ptr=gradient->pointer,in_ptr=input->pointer,w_ptr=weight->pointer;
            auto count=static_cast<std::uint64_t>(launch_counts[kind-1]);
            auto ba=static_cast<std::uint64_t>(batches),ci=static_cast<std::uint64_t>(channels_in),
                 h=static_cast<std::uint64_t>(height),w=static_cast<std::uint64_t>(width),
                 co=static_cast<std::uint64_t>(channels_out),kh=static_cast<std::uint64_t>(kernel_h),
                 kw=static_cast<std::uint64_t>(kernel_w),oh=static_cast<std::uint64_t>(output_h),
                 ow=static_cast<std::uint64_t>(output_w),st=static_cast<std::uint64_t>(stride),
                 pa=static_cast<std::uint64_t>(padding);
            void* args[]={&out_ptr,&g_ptr,&in_ptr,&w_ptr,&count,&ba,&ci,&h,&w,&co,&kh,&kw,&oh,&ow,&st,&pa};
            if(!launch_hip_1d(target,"conv2d-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),
                              source,launch_counts[kind-1],args,error))return false;
        }
        return true;
    }
#ifdef __APPLE__
    if(dtype==10&&input_gradient->backend==Backend::Metal){
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?weight_gradient:bias_gradient;
            auto source=msl_conv2d_backward_source(kind);
            auto pipeline=cached_metal_pipeline([target->metal_buffer device],
                "conv2d-backward-f32-"+std::to_string(kind),source,error);
            if(!pipeline)return false;
            auto count=static_cast<std::uint64_t>(launch_counts[kind-1]);
            auto ba=static_cast<std::uint64_t>(batches),ci=static_cast<std::uint64_t>(channels_in),
                 h=static_cast<std::uint64_t>(height),w=static_cast<std::uint64_t>(width),
                 co=static_cast<std::uint64_t>(channels_out),kh=static_cast<std::uint64_t>(kernel_h),
                 kw=static_cast<std::uint64_t>(kernel_w),oh=static_cast<std::uint64_t>(output_h),
                 ow=static_cast<std::uint64_t>(output_w),st=static_cast<std::uint64_t>(stride),
                 pa=static_cast<std::uint64_t>(padding);
            if(!dispatch_metal_1d(target,pipeline,launch_counts[kind-1],[&](id<MTLComputeCommandEncoder> e){
                [e setBuffer:target->metal_buffer offset:0 atIndex:0];[e setBuffer:gradient->metal_buffer offset:0 atIndex:1];
                [e setBuffer:input->metal_buffer offset:0 atIndex:2];[e setBuffer:weight->metal_buffer offset:0 atIndex:3];
                [e setBytes:&count length:8 atIndex:4];[e setBytes:&ba length:8 atIndex:5];[e setBytes:&ci length:8 atIndex:6];
                [e setBytes:&h length:8 atIndex:7];[e setBytes:&w length:8 atIndex:8];[e setBytes:&co length:8 atIndex:9];
                [e setBytes:&kh length:8 atIndex:10];[e setBytes:&kw length:8 atIndex:11];[e setBytes:&oh length:8 atIndex:12];
                [e setBytes:&ow length:8 atIndex:13];[e setBytes:&st length:8 atIndex:14];[e setBytes:&pa length:8 atIndex:15];
            },error))return false;
        }
        return true;
    }
#endif
    return real_compute_unavailable(input_gradient,"neural.convolve2d backward",dtype,error);
}

bool compute_normalize_training(
    Buffer* output,Buffer* cache,Buffer* running_mean,Buffer* running_variance,
    const Buffer* input,const Buffer* scale,const Buffer* bias,int dtype,
    std::size_t count,std::size_t features,std::size_t inner,std::size_t samples,
    double momentum,double epsilon,std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(features==0||inner==0||samples==0||
       !compute_same_device(output,input,error)||!compute_same_device(output,cache,error)||
       !compute_same_device(output,running_mean,error)||!compute_same_device(output,running_variance,error)||
       !compute_same_device(output,scale,error)||!compute_same_device(output,bias,error)||
       !compute_buffer_range(output,0,count,bytes,error)||!compute_buffer_range(cache,0,features*2,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error)||!compute_buffer_range(scale,0,features,bytes,error)||
       !compute_buffer_range(bias,0,features,bytes,error)||!compute_buffer_range(running_mean,0,features,bytes,error)||
       !compute_buffer_range(running_variance,0,features,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_normalize_training<float>(output,cache,running_mean,running_variance,input,scale,bias,count,features,inner,samples,momentum,epsilon);
        if(dtype==9)return fake_normalize_training<double>(output,cache,running_mean,running_variance,input,scale,bias,count,features,inner,samples,momentum,epsilon);
        error="neural.normalize training requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto stats=ptx_normalize_stats_source(dtype);
        auto cache_ptr=cache->cuda_pointer,rm=running_mean->cuda_pointer,rv=running_variance->cuda_pointer,in=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
        double m=momentum,e=epsilon;void* stats_args[]={&cache_ptr,&rm,&rv,&in,&n,&f,&inn,&s,&m,&e};
        if(!launch_ptx_1d(cache,"normalize-train-stats-"+std::to_string(dtype),stats,features,stats_args,error))return false;
        auto apply=ptx_normalize_apply_source(dtype);
        auto out=output->cuda_pointer,sc=scale->cuda_pointer,bi=bias->cuda_pointer;
        void* apply_args[]={&out,&in,&sc,&bi,&cache_ptr,&n,&f,&inn};
        return launch_ptx_1d(output,"normalize-train-apply-"+std::to_string(dtype),apply,count,apply_args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Amd){
        auto stats=hip_normalize_stats_source(dtype);
        auto cache_ptr=cache->pointer,rm=running_mean->pointer,rv=running_variance->pointer,in=input->pointer;
        auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
        double m=momentum,e=epsilon;void* stats_args[]={&cache_ptr,&rm,&rv,&in,&n,&f,&inn,&s,&m,&e};
        if(!launch_hip_1d(cache,"normalize-train-stats-"+std::to_string(dtype),stats,features,stats_args,error))return false;
        auto apply=hip_normalize_apply_source(dtype);
        auto out=output->pointer,sc=scale->pointer,bi=bias->pointer;
        void* apply_args[]={&out,&in,&sc,&bi,&cache_ptr,&n,&f,&inn};
        return launch_hip_1d(output,"normalize-train-apply-"+std::to_string(dtype),apply,count,apply_args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto stats=msl_normalize_stats_source();
        auto stats_pipeline=cached_metal_pipeline([cache->metal_buffer device],"normalize-train-stats-f32",stats,error);
        if(!stats_pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
        float m=static_cast<float>(momentum),e=static_cast<float>(epsilon);
        if(!dispatch_metal_1d(cache,stats_pipeline,features,[&](id<MTLComputeCommandEncoder> enc){
            [enc setBuffer:cache->metal_buffer offset:0 atIndex:0];[enc setBuffer:running_mean->metal_buffer offset:0 atIndex:1];
            [enc setBuffer:running_variance->metal_buffer offset:0 atIndex:2];[enc setBuffer:input->metal_buffer offset:0 atIndex:3];
            [enc setBytes:&n length:8 atIndex:4];[enc setBytes:&f length:8 atIndex:5];[enc setBytes:&inn length:8 atIndex:6];
            [enc setBytes:&s length:8 atIndex:7];[enc setBytes:&m length:4 atIndex:8];[enc setBytes:&e length:4 atIndex:9];
        },error))return false;
        auto apply=msl_normalize_apply_source();
        auto apply_pipeline=cached_metal_pipeline([output->metal_buffer device],"normalize-train-apply-f32",apply,error);
        if(!apply_pipeline)return false;
        return dispatch_metal_1d(output,apply_pipeline,count,[&](id<MTLComputeCommandEncoder> enc){
            [enc setBuffer:output->metal_buffer offset:0 atIndex:0];[enc setBuffer:input->metal_buffer offset:0 atIndex:1];
            [enc setBuffer:scale->metal_buffer offset:0 atIndex:2];[enc setBuffer:bias->metal_buffer offset:0 atIndex:3];
            [enc setBuffer:cache->metal_buffer offset:0 atIndex:4];[enc setBytes:&n length:8 atIndex:5];
            [enc setBytes:&f length:8 atIndex:6];[enc setBytes:&inn length:8 atIndex:7];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.normalize training",dtype,error);
}

bool compute_normalize_backward(
    Buffer* input_gradient,Buffer* scale_gradient,Buffer* bias_gradient,
    const Buffer* gradient,const Buffer* input,const Buffer* scale,
    const Buffer* cache,int dtype,std::size_t count,std::size_t features,
    std::size_t inner,std::size_t samples,std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(features==0||inner==0||samples==0||
       !compute_same_device(input_gradient,gradient,error)||
       !compute_same_device(input_gradient,input,error)||
       !compute_same_device(input_gradient,scale,error)||
       !compute_same_device(input_gradient,cache,error)||
       !compute_same_device(input_gradient,scale_gradient,error)||
       !compute_same_device(input_gradient,bias_gradient,error)||
       !compute_buffer_range(input_gradient,0,count,bytes,error)||
       !compute_buffer_range(scale_gradient,0,features,bytes,error)||
       !compute_buffer_range(bias_gradient,0,features,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error)||
       !compute_buffer_range(scale,0,features,bytes,error)||
       !compute_buffer_range(cache,0,features*2,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(input_gradient->backend==Backend::Test){
        if(dtype==10)return fake_normalize_backward<float>(input_gradient,scale_gradient,bias_gradient,gradient,input,scale,cache,count,features,inner,samples);
        if(dtype==9)return fake_normalize_backward<double>(input_gradient,scale_gradient,bias_gradient,gradient,input,scale,cache,count,features,inner,samples);
        error="neural.normalize backward requires a floating dtype";return false;
    }
#endif
    const auto counts=std::array<std::size_t,3>{count,features,features};
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Nvidia){
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?scale_gradient:bias_gradient;
            auto ptx=ptx_normalize_backward_source(dtype,kind);
            auto out=target->cuda_pointer,g=gradient->cuda_pointer,in=input->cuda_pointer,sc=scale->cuda_pointer,ca=cache->cuda_pointer;
            auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
            void* args[]={&out,&g,&in,&sc,&ca,&n,&f,&inn,&s};
            if(!launch_ptx_1d(target,"normalize-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),ptx,counts[kind-1],args,error))return false;
        }
        return true;
    }
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Amd){
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?scale_gradient:bias_gradient;
            auto source=hip_normalize_backward_source(dtype,kind);
            auto out=target->pointer,g=gradient->pointer,in=input->pointer,sc=scale->pointer,ca=cache->pointer;
            auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
            void* args[]={&out,&g,&in,&sc,&ca,&n,&f,&inn,&s};
            if(!launch_hip_1d(target,"normalize-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),source,counts[kind-1],args,error))return false;
        }
        return true;
    }
#ifdef __APPLE__
    if(dtype==10&&input_gradient->backend==Backend::Metal){
        for(int kind=1;kind<=3;++kind){
            Buffer* target=kind==1?input_gradient:kind==2?scale_gradient:bias_gradient;
            auto source=msl_normalize_backward_source(kind);
            auto pipeline=cached_metal_pipeline([target->metal_buffer device],
                "normalize-backward-f32-"+std::to_string(kind),source,error);
            if(!pipeline)return false;
            auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),inn=static_cast<std::uint64_t>(inner),s=static_cast<std::uint64_t>(samples);
            if(!dispatch_metal_1d(target,pipeline,counts[kind-1],[&](id<MTLComputeCommandEncoder> enc){
                [enc setBuffer:target->metal_buffer offset:0 atIndex:0];[enc setBuffer:gradient->metal_buffer offset:0 atIndex:1];
                [enc setBuffer:input->metal_buffer offset:0 atIndex:2];[enc setBuffer:scale->metal_buffer offset:0 atIndex:3];
                [enc setBuffer:cache->metal_buffer offset:0 atIndex:4];[enc setBytes:&n length:8 atIndex:5];
                [enc setBytes:&f length:8 atIndex:6];[enc setBytes:&inn length:8 atIndex:7];[enc setBytes:&s length:8 atIndex:8];
            },error))return false;
        }
        return true;
    }
#endif
    return real_compute_unavailable(input_gradient,"neural.normalize backward",dtype,error);
}

bool compute_random_mask(
    Buffer* output,Buffer* mask,const Buffer* input,int dtype,std::size_t count,
    std::uint64_t initial_state,std::uint64_t cutoff,double scale,std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(!compute_same_device(output,input,error)||!compute_same_device(output,mask,error)||
       !compute_buffer_range(output,0,count,bytes,error)||!compute_buffer_range(mask,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_random_mask<float>(output,mask,input,count,initial_state,cutoff,scale);
        if(dtype==9)return fake_random_mask<double>(output,mask,input,count,initial_state,cutoff,scale);
        error="neural.random_mask requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_random_mask_source(dtype);
        auto out=output->cuda_pointer,ma=mask->cuda_pointer,in=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),state=initial_state,cut=cutoff;double sc=scale;
        void* args[]={&out,&ma,&in,&n,&state,&cut,&sc};
        return launch_ptx_1d(output,"random-mask-"+std::to_string(dtype),ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Amd){
        auto source=hip_random_mask_source(dtype);
        auto out=output->pointer,ma=mask->pointer,in=input->pointer;
        auto n=static_cast<std::uint64_t>(count),state=initial_state,cut=cutoff;double sc=scale;
        void* args[]={&out,&ma,&in,&n,&state,&cut,&sc};
        return launch_hip_1d(output,"random-mask-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_random_mask_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"random-mask-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),state=initial_state,cut=cutoff;float sc=static_cast<float>(scale);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> enc){
            [enc setBuffer:output->metal_buffer offset:0 atIndex:0];[enc setBuffer:mask->metal_buffer offset:0 atIndex:1];
            [enc setBuffer:input->metal_buffer offset:0 atIndex:2];[enc setBytes:&n length:8 atIndex:3];
            [enc setBytes:&state length:8 atIndex:4];[enc setBytes:&cut length:8 atIndex:5];[enc setBytes:&sc length:4 atIndex:6];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.random_mask",dtype,error);
}

bool compute_moment_update(
    Buffer* parameter,const Buffer* gradient,Buffer* first,Buffer* second,
    int dtype,std::size_t count,double rate,double beta1,double beta2,
    double epsilon,double correction1,double correction2,std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(!compute_same_device(parameter,gradient,error)||!compute_same_device(parameter,first,error)||
       !compute_same_device(parameter,second,error)||!compute_buffer_range(parameter,0,count,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||!compute_buffer_range(first,0,count,bytes,error)||
       !compute_buffer_range(second,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(parameter->backend==Backend::Test){
        if(dtype==10)return fake_moment_update<float>(parameter,gradient,first,second,count,rate,beta1,beta2,epsilon,correction1,correction2);
        if(dtype==9)return fake_moment_update<double>(parameter,gradient,first,second,count,rate,beta1,beta2,epsilon,correction1,correction2);
        error="neural.moment_update requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&parameter->backend==Backend::Nvidia){
        auto ptx=ptx_moment_source(dtype);
        auto pa=parameter->cuda_pointer,g=gradient->cuda_pointer,f=first->cuda_pointer,s=second->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);double r=rate,b1=beta1,b2=beta2,e=epsilon,c1=correction1,c2=correction2;
        void* args[]={&pa,&g,&f,&s,&n,&r,&b1,&b2,&e,&c1,&c2};
        return launch_ptx_1d(parameter,"moment-update-"+std::to_string(dtype),ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&parameter->backend==Backend::Amd){
        auto source=hip_moment_source(dtype);
        auto pa=parameter->pointer,g=gradient->pointer,f=first->pointer,s=second->pointer;
        auto n=static_cast<std::uint64_t>(count);double r=rate,b1=beta1,b2=beta2,e=epsilon,c1=correction1,c2=correction2;
        void* args[]={&pa,&g,&f,&s,&n,&r,&b1,&b2,&e,&c1,&c2};
        return launch_hip_1d(parameter,"moment-update-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&parameter->backend==Backend::Metal){
        auto source=msl_moment_source();
        auto pipeline=cached_metal_pipeline([parameter->metal_buffer device],"moment-update-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);float r=static_cast<float>(rate),b1=static_cast<float>(beta1),
             b2=static_cast<float>(beta2),e=static_cast<float>(epsilon),c1=static_cast<float>(correction1),c2=static_cast<float>(correction2);
        return dispatch_metal_1d(parameter,pipeline,count,[&](id<MTLComputeCommandEncoder> enc){
            [enc setBuffer:parameter->metal_buffer offset:0 atIndex:0];[enc setBuffer:gradient->metal_buffer offset:0 atIndex:1];
            [enc setBuffer:first->metal_buffer offset:0 atIndex:2];[enc setBuffer:second->metal_buffer offset:0 atIndex:3];
            [enc setBytes:&n length:8 atIndex:4];[enc setBytes:&r length:4 atIndex:5];[enc setBytes:&b1 length:4 atIndex:6];
            [enc setBytes:&b2 length:4 atIndex:7];[enc setBytes:&e length:4 atIndex:8];[enc setBytes:&c1 length:4 atIndex:9];
            [enc setBytes:&c2 length:4 atIndex:10];
        },error);
    }
#endif
    return real_compute_unavailable(parameter,"neural.moment_update",dtype,error);
}
