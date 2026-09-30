
namespace {

template <typename T>
bool fake_binary_backward(
    Buffer* left_gradient, Buffer* right_gradient,
    const Buffer* gradient, const Buffer* left, const Buffer* right,
    int operation, std::size_t count) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for(std::size_t i=0;i<count;++i){
        T g{},a{},b{},left_value{},right_value{};
        std::memcpy(&g,gradient->test_data.data()+i*sizeof(T),sizeof(T));
        std::memcpy(&a,left->test_data.data()+i*sizeof(T),sizeof(T));
        std::memcpy(&b,right->test_data.data()+i*sizeof(T),sizeof(T));
        if(operation==1){
            left_value=g;
            right_value=g;
        }else if(operation==2){
            left_value=g;
            right_value=static_cast<T>(-g);
        }else if(operation==3){
            left_value=static_cast<T>(g*b);
            right_value=static_cast<T>(g*a);
        }else{
            left_value=static_cast<T>(g/b);
            const T ga=static_cast<T>(g*a);
            const T bb=static_cast<T>(b*b);
            right_value=static_cast<T>(-static_cast<T>(ga/bb));
        }
        if(left_gradient==right_gradient){
            const T combined=static_cast<T>(left_value+right_value);
            std::memcpy(left_gradient->test_data.data()+i*sizeof(T),&combined,sizeof(T));
        }else{
            std::memcpy(left_gradient->test_data.data()+i*sizeof(T),&left_value,sizeof(T));
            std::memcpy(right_gradient->test_data.data()+i*sizeof(T),&right_value,sizeof(T));
        }
    }
    return true;
#else
    (void)left_gradient;(void)right_gradient;(void)gradient;(void)left;(void)right;
    (void)operation;(void)count;
    return false;
#endif
}


template <typename T>
bool fake_scalar_backward(
    Buffer* output, const Buffer* gradient, const Buffer* input,
    int operation, bool scalar_left, double scalar, std::size_t count) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const T scalar_value=static_cast<T>(scalar);
    for(std::size_t i=0;i<count;++i){
        T g{},x{},value{};
        std::memcpy(&g,gradient->test_data.data()+i*sizeof(T),sizeof(T));
        if(operation==4&&scalar_left)
            std::memcpy(&x,input->test_data.data()+i*sizeof(T),sizeof(T));
        if(operation==1)value=g;
        else if(operation==2)value=scalar_left?static_cast<T>(-g):g;
        else if(operation==3)value=static_cast<T>(g*scalar_value);
        else if(scalar_left){
            const T gs=static_cast<T>(g*scalar_value);
            const T xx=static_cast<T>(x*x);
            value=static_cast<T>(-static_cast<T>(gs/xx));
        }else{
            value=static_cast<T>(g/scalar_value);
        }
        std::memcpy(output->test_data.data()+i*sizeof(T),&value,sizeof(T));
    }
    return true;
#else
    (void)output;(void)gradient;(void)input;(void)operation;
    (void)scalar_left;(void)scalar;(void)count;
    return false;
#endif
}

std::string ptx_binary_backward_source(int dtype,int operation,bool shared_output) {
    const bool f32=dtype==10;
    const auto width=f32?4U:8U;
    std::ostringstream out;
    out<<ptx_thread_prologue(
        "  .param .u64 p_left_gradient,\n"
        "  .param .u64 p_right_gradient,\n"
        "  .param .u64 p_gradient,\n"
        "  .param .u64 p_left,\n"
        "  .param .u64 p_right,\n"
        "  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<28>;\n  .reg .f32 %f<10>;\n"
          : "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<28>;\n  .reg .f64 %fd<10>;\n");
    out<<"  ld.param.u64 %rd1,[p_left_gradient];\n"
          "  ld.param.u64 %rd2,[p_right_gradient];\n"
          "  ld.param.u64 %rd3,[p_gradient];\n"
          "  ld.param.u64 %rd4,[p_left];\n"
          "  ld.param.u64 %rd5,[p_right];\n"
          "  ld.param.u64 %rd6,[p_count];\n"
          "  setp.ge.u64 %p1,%rd7,%rd6;\n"
          "  @%p1 bra DONE;\n"
       <<"  mul.lo.u64 %rd8,%rd7,"<<width<<";\n"
          "  add.u64 %rd9,%rd3,%rd8;\n"
          "  add.u64 %rd10,%rd4,%rd8;\n"
          "  add.u64 %rd11,%rd5,%rd8;\n"
          "  add.u64 %rd12,%rd1,%rd8;\n"
          "  add.u64 %rd13,%rd2,%rd8;\n";
    if(f32){
        out<<"  ld.global.f32 %f1,[%rd9];\n"
              "  ld.global.f32 %f2,[%rd10];\n"
              "  ld.global.f32 %f3,[%rd11];\n";
        if(operation==1){
            out<<"  mov.f32 %f4,%f1;\n  mov.f32 %f5,%f1;\n";
        }else if(operation==2){
            out<<"  mov.f32 %f4,%f1;\n  neg.f32 %f5,%f1;\n";
        }else if(operation==3){
            out<<"  mul.f32 %f4,%f1,%f3;\n"
                  "  mul.f32 %f5,%f1,%f2;\n";
        }else{
            out<<"  div.f32 %f4,%f1,%f3;\n"
                  "  mul.f32 %f6,%f1,%f2;\n"
                  "  mul.f32 %f7,%f3,%f3;\n"
                  "  div.f32 %f5,%f6,%f7;\n"
                  "  neg.f32 %f5,%f5;\n";
        }
        if(shared_output)
            out<<"  add.f32 %f8,%f4,%f5;\n  st.global.f32 [%rd12],%f8;\n";
        else
            out<<"  st.global.f32 [%rd12],%f4;\n"
                  "  st.global.f32 [%rd13],%f5;\n";
    }else{
        out<<"  ld.global.f64 %fd1,[%rd9];\n"
              "  ld.global.f64 %fd2,[%rd10];\n"
              "  ld.global.f64 %fd3,[%rd11];\n";
        if(operation==1){
            out<<"  mov.f64 %fd4,%fd1;\n  mov.f64 %fd5,%fd1;\n";
        }else if(operation==2){
            out<<"  mov.f64 %fd4,%fd1;\n  neg.f64 %fd5,%fd1;\n";
        }else if(operation==3){
            out<<"  mul.f64 %fd4,%fd1,%fd3;\n"
                  "  mul.f64 %fd5,%fd1,%fd2;\n";
        }else{
            out<<"  div.f64 %fd4,%fd1,%fd3;\n"
                  "  mul.f64 %fd6,%fd1,%fd2;\n"
                  "  mul.f64 %fd7,%fd3,%fd3;\n"
                  "  div.f64 %fd5,%fd6,%fd7;\n"
                  "  neg.f64 %fd5,%fd5;\n";
        }
        if(shared_output)
            out<<"  add.f64 %fd8,%fd4,%fd5;\n  st.global.f64 [%rd12],%fd8;\n";
        else
            out<<"  st.global.f64 [%rd12],%fd4;\n"
                  "  st.global.f64 [%rd13],%fd5;\n";
    }
    out<<"DONE:\n  ret;\n}\n";
    return out.str();
}


std::string ptx_scalar_backward_source(int dtype,int operation,bool scalar_left) {
    const bool f32=dtype==10;
    const auto width=f32?4U:8U;
    std::ostringstream out;
    out<<ptx_thread_prologue(
        f32
          ? "  .param .u64 p_output,\n  .param .u64 p_gradient,\n  .param .u64 p_input,\n"
            "  .param .u64 p_count,\n  .param .f32 p_scalar\n"
          : "  .param .u64 p_output,\n  .param .u64 p_gradient,\n  .param .u64 p_input,\n"
            "  .param .u64 p_count,\n  .param .f64 p_scalar\n",
        f32
          ? "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f32 %f<10>;\n"
          : "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f64 %fd<10>;\n");
    out<<"  ld.param.u64 %rd1,[p_output];\n"
          "  ld.param.u64 %rd2,[p_gradient];\n"
          "  ld.param.u64 %rd3,[p_input];\n"
          "  ld.param.u64 %rd4,[p_count];\n"
          "  setp.ge.u64 %p1,%rd7,%rd4;\n"
          "  @%p1 bra DONE;\n"
       <<"  mul.lo.u64 %rd8,%rd7,"<<width<<";\n"
          "  add.u64 %rd9,%rd1,%rd8;\n"
          "  add.u64 %rd10,%rd2,%rd8;\n"
          "  add.u64 %rd11,%rd3,%rd8;\n";
    if(f32){
        out<<"  ld.param.f32 %f1,[p_scalar];\n"
              "  ld.global.f32 %f2,[%rd10];\n";
        if(operation==1) out<<"  mov.f32 %f4,%f2;\n";
        else if(operation==2)
            out<<(scalar_left?"  neg.f32 %f4,%f2;\n":"  mov.f32 %f4,%f2;\n");
        else if(operation==3) out<<"  mul.f32 %f4,%f2,%f1;\n";
        else if(scalar_left)
            out<<"  ld.global.f32 %f3,[%rd11];\n"
                  "  mul.f32 %f5,%f2,%f1;\n"
                  "  mul.f32 %f6,%f3,%f3;\n"
                  "  div.f32 %f4,%f5,%f6;\n"
                  "  neg.f32 %f4,%f4;\n";
        else out<<"  div.f32 %f4,%f2,%f1;\n";
        out<<"  st.global.f32 [%rd9],%f4;\n";
    }else{
        out<<"  ld.param.f64 %fd1,[p_scalar];\n"
              "  ld.global.f64 %fd2,[%rd10];\n";
        if(operation==1) out<<"  mov.f64 %fd4,%fd2;\n";
        else if(operation==2)
            out<<(scalar_left?"  neg.f64 %fd4,%fd2;\n":"  mov.f64 %fd4,%fd2;\n");
        else if(operation==3) out<<"  mul.f64 %fd4,%fd2,%fd1;\n";
        else if(scalar_left)
            out<<"  ld.global.f64 %fd3,[%rd11];\n"
                  "  mul.f64 %fd5,%fd2,%fd1;\n"
                  "  mul.f64 %fd6,%fd3,%fd3;\n"
                  "  div.f64 %fd4,%fd5,%fd6;\n"
                  "  neg.f64 %fd4,%fd4;\n";
        else out<<"  div.f64 %fd4,%fd2,%fd1;\n";
        out<<"  st.global.f64 [%rd9],%fd4;\n";
    }
    out<<"DONE:\n  ret;\n}\n";
    return out.str();
}

std::string hip_binary_backward_source(int dtype,int operation,bool shared_output) {
    const auto type=dtype==10?"float":"double";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* left_gradient,"<<type
       <<"* right_gradient,const "<<type<<"* gradient,const "<<type
       <<"* left,const "<<type<<"* right,unsigned long long count){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" g=gradient[i],a=left[i],b=right[i],"
         "left_value,right_value;";
    if(operation==1)
        out<<"left_value=g;right_value=g;";
    else if(operation==2)
        out<<"left_value=g;right_value=-g;";
    else if(operation==3)
        out<<"left_value=g*b;right_value=g*a;";
    else
        out<<type<<" ga=g*a,bb=b*b;"
              "left_value=g/b;right_value=-(ga/bb);";
    if(shared_output)
        out<<"left_gradient[i]=left_value+right_value;";
    else
        out<<"left_gradient[i]=left_value;right_gradient[i]=right_value;";
    out<<"}";
    return out.str();
}


std::string hip_scalar_backward_source(int dtype,int operation,bool scalar_left) {
    const auto type=dtype==10?"float":"double";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* output,const "<<type
       <<"* gradient,const "<<type<<"* input,unsigned long long count,"<<type<<" scalar){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" g=gradient[i],value;";
    if(operation==1) out<<"value=g;";
    else if(operation==2) out<<(scalar_left?"value=-g;":"value=g;");
    else if(operation==3) out<<"value=g*scalar;";
    else if(scalar_left)
        out<<type<<" x=input[i],gs=g*scalar,xx=x*x;value=-(gs/xx);";
    else out<<"value=g/scalar;";
    out<<"output[i]=value;}";
    return out.str();
}

#ifdef __APPLE__

std::string msl_binary_backward_source(int operation,bool shared_output) {
    std::ostringstream out;
    out<<"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void q_kernel(device float* left_gradient [[buffer(0)]],"
         "device float* right_gradient [[buffer(1)]],"
         "const device float* gradient [[buffer(2)]],"
         "const device float* left [[buffer(3)]],"
         "const device float* right [[buffer(4)]],"
         "constant ulong& count [[buffer(5)]],"
         "uint gid [[thread_position_in_grid]]) {"
         " ulong i=ulong(gid); if(i>=count)return;"
         " float g=gradient[i],a=left[i],b=right[i],left_value,right_value;";
    if(operation==1)
        out<<" left_value=g; right_value=g;";
    else if(operation==2)
        out<<" left_value=g; right_value=-g;";
    else if(operation==3)
        out<<" left_value=g*b; right_value=g*a;";
    else
        out<<" float ga=g*a,bb=b*b;"
             " left_value=g/b; right_value=-(ga/bb);";
    if(shared_output)
        out<<" left_gradient[i]=left_value+right_value;";
    else
        out<<" left_gradient[i]=left_value; right_gradient[i]=right_value;";
    out<<" }\n";
    return out.str();
}


std::string msl_scalar_backward_source(int operation,bool scalar_left) {
    std::ostringstream out;
    out<<"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void q_kernel(device float* output [[buffer(0)]],"
         "const device float* gradient [[buffer(1)]],"
         "const device float* input [[buffer(2)]],"
         "constant ulong& count [[buffer(3)]],"
         "constant float& scalar [[buffer(4)]],"
         "uint gid [[thread_position_in_grid]]) {"
         " ulong i=ulong(gid); if(i>=count)return; float g=gradient[i],value;";
    if(operation==1) out<<" value=g;";
    else if(operation==2) out<<(scalar_left?" value=-g;":" value=g;");
    else if(operation==3) out<<" value=g*scalar;";
    else if(scalar_left) out<<" float x=input[i],gs=g*scalar,xx=x*x; value=-(gs/xx);";
    else out<<" value=g/scalar;";
    out<<" output[i]=value; }\n";
    return out.str();
}

#endif

} // namespace

bool compute_binary_backward(
    Buffer* left_gradient,Buffer* right_gradient,
    const Buffer* gradient,const Buffer* left,const Buffer* right,
    int dtype,int operation,std::size_t count,std::string& error) {
    if(operation<1||operation>4){
        error="invalid tensor binary backward operation";
        return false;
    }
    const auto bytes=compute_dtype_width(dtype);
    const bool shared_output=left_gradient==right_gradient;
    if(!compute_same_device(left_gradient,right_gradient,error)||
       !compute_same_device(left_gradient,gradient,error)||
       !compute_same_device(left_gradient,left,error)||
       !compute_same_device(left_gradient,right,error)||
       !compute_buffer_range(left_gradient,0,count,bytes,error)||
       !compute_buffer_range(right_gradient,0,count,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||
       !compute_buffer_range(left,0,count,bytes,error)||
       !compute_buffer_range(right,0,count,bytes,error))
        return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(left_gradient->backend==Backend::Test){
        if(dtype==10)return fake_binary_backward<float>(
            left_gradient,right_gradient,gradient,left,right,operation,count);
        if(dtype==9)return fake_binary_backward<double>(
            left_gradient,right_gradient,gradient,left,right,operation,count);
        error="tensor binary backward requires a floating dtype";
        return false;
    }
#endif
    if((dtype==9||dtype==10)&&left_gradient->backend==Backend::Cuda){
        auto ptx=ptx_binary_backward_source(dtype,operation,shared_output);
        auto lg=left_gradient->cuda_pointer,rg=right_gradient->cuda_pointer,
             g=gradient->cuda_pointer,a=left->cuda_pointer,b=right->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&lg,&rg,&g,&a,&b,&n};
        return launch_ptx_1d(
            left_gradient,
            "autograd-binary-backward-"+std::to_string(dtype)+"-"+std::to_string(operation)+
                 (shared_output?"-shared":"-split"),
            ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&left_gradient->backend==Backend::Hip){
        auto source=hip_binary_backward_source(dtype,operation,shared_output);
        auto lg=left_gradient->pointer,rg=right_gradient->pointer,
             g=gradient->pointer,a=left->pointer,b=right->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&lg,&rg,&g,&a,&b,&n};
        return launch_hip_1d(
            left_gradient,
            "autograd-binary-backward-"+std::to_string(dtype)+"-"+std::to_string(operation)+
                (shared_output?"-shared":"-split"),
            source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&left_gradient->backend==Backend::Metal){
        auto source=msl_binary_backward_source(operation,shared_output);
        auto pipeline=cached_metal_pipeline(
            [left_gradient->metal_buffer device],
            "autograd-binary-backward-f32-"+std::to_string(operation)+
                (shared_output?"-shared":"-split"),source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(
            left_gradient,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
                [e setBuffer:left_gradient->metal_buffer offset:0 atIndex:0];
                [e setBuffer:right_gradient->metal_buffer offset:0 atIndex:1];
                [e setBuffer:gradient->metal_buffer offset:0 atIndex:2];
                [e setBuffer:left->metal_buffer offset:0 atIndex:3];
                [e setBuffer:right->metal_buffer offset:0 atIndex:4];
                [e setBytes:&n length:sizeof(n) atIndex:5];
            },error);
    }
#endif
    return real_compute_unavailable(
        left_gradient,"tensor binary backward",dtype,error);
}


bool compute_scalar_backward(
    Buffer* output,const Buffer* gradient,const Buffer* input,
    int dtype,int operation,bool scalar_left,double scalar,
    std::size_t count,std::string& error) {
    if(operation<1||operation>4){
        error="invalid tensor scalar backward operation";
        return false;
    }
    const auto bytes=compute_dtype_width(dtype);
    if(!compute_same_device(output,gradient,error)||
       !compute_same_device(output,input,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error))
        return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_scalar_backward<float>(
            output,gradient,input,operation,scalar_left,scalar,count);
        if(dtype==9)return fake_scalar_backward<double>(
            output,gradient,input,operation,scalar_left,scalar,count);
        error="tensor scalar backward requires a floating dtype";
        return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Cuda){
        auto source=ptx_scalar_backward_source(dtype,operation,scalar_left);
        auto o=output->cuda_pointer,g=gradient->cuda_pointer,in=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        if(dtype==10){
            float s=static_cast<float>(scalar);
            void* args[]={&o,&g,&in,&n,&s};
            return launch_ptx_1d(
                output,
                "autograd-scalar-backward-10-"+std::to_string(operation)+"-"+
                    (scalar_left?"left":"right"),
                source,count,args,error);
        }
        double s=scalar;
        void* args[]={&o,&g,&in,&n,&s};
        return launch_ptx_1d(
            output,
            "autograd-scalar-backward-9-"+std::to_string(operation)+"-"+
                (scalar_left?"left":"right"),
            source,count,args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Hip){
        auto source=hip_scalar_backward_source(dtype,operation,scalar_left);
        auto o=output->pointer,g=gradient->pointer,in=input->pointer;
        auto n=static_cast<std::uint64_t>(count);
        if(dtype==10){
            float s=static_cast<float>(scalar);
            void* args[]={&o,&g,&in,&n,&s};
            return launch_hip_1d(
                output,
                "autograd-scalar-backward-10-"+std::to_string(operation)+"-"+
                    (scalar_left?"left":"right"),
                source,count,args,error);
        }
        double s=scalar;
        void* args[]={&o,&g,&in,&n,&s};
        return launch_hip_1d(
            output,
            "autograd-scalar-backward-9-"+std::to_string(operation)+"-"+
                (scalar_left?"left":"right"),
            source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_scalar_backward_source(operation,scalar_left);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],
            "autograd-scalar-backward-f32-"+std::to_string(operation)+"-"+
                (scalar_left?"left":"right"),
            source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);
        float s=static_cast<float>(scalar);
        return dispatch_metal_1d(
            output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
                [e setBuffer:output->metal_buffer offset:0 atIndex:0];
                [e setBuffer:gradient->metal_buffer offset:0 atIndex:1];
                [e setBuffer:input->metal_buffer offset:0 atIndex:2];
                [e setBytes:&n length:sizeof(n) atIndex:3];
                [e setBytes:&s length:sizeof(s) atIndex:4];
            },error);
    }
#endif
    return real_compute_unavailable(
        output,"tensor scalar backward",dtype,error);
}
