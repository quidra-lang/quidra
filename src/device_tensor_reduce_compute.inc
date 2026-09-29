
namespace {

template <typename T>
bool fake_tensor_mean_to(Buffer* output, const Buffer* input,
                         std::size_t count, std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (count == 0) {
        error = "tensor.mean() requires at least one element";
        return false;
    }
    T total{};
    for (std::size_t i = 0; i < count; ++i) {
        T value{};
        std::memcpy(&value, input->test_data.data() + i * sizeof(T), sizeof(T));
        total = static_cast<T>(total + value);
    }
    const T result = static_cast<T>(total / static_cast<T>(count));
    std::memcpy(output->test_data.data(), &result, sizeof(T));
    return true;
#else
    (void)output; (void)input; (void)count;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

template <typename T>
bool fake_last_reduce(Buffer* output, const Buffer* input,
                      std::size_t count, std::size_t width,
                      int operation, std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (width == 0 || count % width != 0) {
        error = "invalid tensor last-axis reduction shape";
        return false;
    }
    for (std::size_t base = 0; base < count; base += width) {
        T reduced{};
        if (operation != 1) {
            std::memcpy(&reduced, input->test_data.data() + base * sizeof(T), sizeof(T));
        }
        for (std::size_t j = 0; j < width; ++j) {
            T value{};
            std::memcpy(&value,
                        input->test_data.data() + (base + j) * sizeof(T),
                        sizeof(T));
            if (operation == 1) reduced = static_cast<T>(reduced + value);
            else if (operation == 2) {
                if (value > reduced) reduced = value;
            } else {
                if (value < reduced) reduced = value;
            }
        }
        for (std::size_t j = 0; j < width; ++j)
            std::memcpy(output->test_data.data() + (base + j) * sizeof(T),
                        &reduced, sizeof(T));
    }
    return true;
#else
    (void)output; (void)input; (void)count; (void)width; (void)operation;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

std::string ptx_tensor_mean_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
           ".visible .entry q_kernel(\n"
           "  .param .u64 p_out,\n  .param .u64 p_in,\n"
           "  .param .u64 p_count)\n{\n"
           "  .reg .pred %p<2>;\n  .reg .b64 %rd<20>;\n"
        << (f32
          ? "  .reg .f32 %f<8>;\n"
          : "  .reg .f64 %fd<8>;\n")
        << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  mov.u64 %rd7, 0;\n";
    if (f32) out << "  mov.f32 %f1, 0f00000000;\n";
    else out << "  mov.f64 %fd1, 0d0000000000000000;\n";
    out << "LOOP:\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra STORE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n";
    if (f32)
        out << "  ld.global.f32 %f2, [%rd9];\n"
               "  add.f32 %f1, %f1, %f2;\n";
    else
        out << "  ld.global.f64 %fd2, [%rd9];\n"
               "  add.f64 %fd1, %fd1, %fd2;\n";
    out << "  add.u64 %rd7, %rd7, 1;\n"
           "  bra LOOP;\nSTORE:\n";
    if (f32)
        out << "  cvt.rn.f32.u64 %f3, %rd5;\n"
               "  div.f32 %f4, %f1, %f3;\n"
               "  st.global.f32 [%rd1], %f4;\n";
    else
        out << "  cvt.rn.f64.u64 %fd3, %rd5;\n"
               "  div.f64 %fd4, %fd1, %fd3;\n"
               "  st.global.f64 [%rd1], %fd4;\n";
    out << "  ret;\n}\n";
    return out.str();
}

std::string ptx_last_reduce_source(int dtype, int operation) {
    const auto bytes = compute_dtype_width(dtype);
    const bool f32 = dtype == 10;
    const bool f64 = dtype == 9;
    const bool floating = f32 || f64;
    const bool signed_int = dtype >= 1 && dtype <= 4;
    if (operation == 1 && !floating) return {};

    std::ostringstream registers;
    registers << "  .reg .pred %p<6>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<32>;\n";
    if (f32) registers << "  .reg .f32 %f<8>;\n";
    if (f64) registers << "  .reg .f64 %fd<8>;\n";

    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_in,\n"
        "  .param .u64 p_count,\n  .param .u64 p_width\n",
        registers.str());
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd6, [p_width];\n"
           "  div.u64 %rd18, %rd5, %rd6;\n"
           "  setp.ge.u64 %p1, %rd7, %rd18;\n"
           "  @%p1 bra DONE;\n"
           "  mul.lo.u64 %rd9, %rd7, %rd6;\n"
           "  mov.u64 %rd10, 0;\n";
    if (operation == 1) {
        if (f32) out << "  mov.f32 %f1, 0f00000000;\n";
        else out << "  mov.f64 %fd1, 0d0000000000000000;\n";
    } else {
        out << "  mul.lo.u64 %rd11, %rd9, " << bytes << ";\n"
               "  add.u64 %rd12, %rd2, %rd11;\n";
        if (f32) out << "  ld.global.f32 %f1, [%rd12];\n";
        else if (f64) out << "  ld.global.f64 %fd1, [%rd12];\n";
        else if (bytes == 1) out << "  ld.global.u8 %r5, [%rd12];\n";
        else if (bytes == 2) out << "  ld.global.u16 %r5, [%rd12];\n";
        else if (bytes == 4) out << "  ld.global.u32 %r5, [%rd12];\n";
        else out << "  ld.global.u64 %rd19, [%rd12];\n";
    }
    out << "REDUCE:\n"
           "  setp.ge.u64 %p2, %rd10, %rd6;\n"
           "  @%p2 bra FILL_BEGIN;\n"
           "  add.u64 %rd13, %rd9, %rd10;\n"
        << "  mul.lo.u64 %rd14, %rd13, " << bytes << ";\n"
           "  add.u64 %rd15, %rd2, %rd14;\n";

    const char* compare = operation == 2 ? "gt" : "lt";
    if (f32) {
        out << "  ld.global.f32 %f2, [%rd15];\n";
        if (operation == 1) out << "  add.f32 %f1, %f1, %f2;\n";
        else out << "  setp." << compare << ".f32 %p3, %f2, %f1;\n"
                    "  @%p3 mov.f32 %f1, %f2;\n";
    } else if (f64) {
        out << "  ld.global.f64 %fd2, [%rd15];\n";
        if (operation == 1) out << "  add.f64 %fd1, %fd1, %fd2;\n";
        else out << "  setp." << compare << ".f64 %p3, %fd2, %fd1;\n"
                    "  @%p3 mov.f64 %fd1, %fd2;\n";
    } else if (bytes == 8) {
        out << "  ld.global.u64 %rd20, [%rd15];\n"
            << "  setp." << compare << (signed_int ? ".s64" : ".u64")
            << " %p3, %rd20, %rd19;\n"
               "  @%p3 mov.u64 %rd19, %rd20;\n";
    } else {
        if (bytes == 1) out << "  ld.global.u8 %r6, [%rd15];\n";
        else if (bytes == 2) out << "  ld.global.u16 %r6, [%rd15];\n";
        else out << "  ld.global.u32 %r6, [%rd15];\n";
        if (signed_int && bytes < 4) {
            out << "  cvt.s32.s" << (bytes * 8) << " %r7, %r6;\n"
                << "  cvt.s32.s" << (bytes * 8) << " %r8, %r5;\n"
                << "  setp." << compare << ".s32 %p3, %r7, %r8;\n";
        } else {
            out << "  setp." << compare << (signed_int ? ".s32" : ".u32")
                << " %p3, %r6, %r5;\n";
        }
        out << "  @%p3 mov.u32 %r5, %r6;\n";
    }

    out << "  add.u64 %rd10, %rd10, 1;\n"
           "  bra REDUCE;\n"
           "FILL_BEGIN:\n"
           "  mov.u64 %rd10, 0;\n"
           "FILL:\n"
           "  setp.ge.u64 %p2, %rd10, %rd6;\n"
           "  @%p2 bra DONE;\n"
           "  add.u64 %rd13, %rd9, %rd10;\n"
        << "  mul.lo.u64 %rd16, %rd13, " << bytes << ";\n"
           "  add.u64 %rd17, %rd1, %rd16;\n";
    if (f32) out << "  st.global.f32 [%rd17], %f1;\n";
    else if (f64) out << "  st.global.f64 [%rd17], %fd1;\n";
    else if (bytes == 1) out << "  st.global.u8 [%rd17], %r5;\n";
    else if (bytes == 2) out << "  st.global.u16 [%rd17], %r5;\n";
    else if (bytes == 4) out << "  st.global.u32 [%rd17], %r5;\n";
    else out << "  st.global.u64 [%rd17], %rd19;\n";
    out << "  add.u64 %rd10, %rd10, 1;\n"
           "  bra FILL;\n"
           "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string hip_tensor_mean_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
      "* out,const "+type+"* in,unsigned long long count){"
      "__shared__ "+type+" scratch[256];unsigned int tid=threadIdx.x;"+
      type+" total=("+type+")0;"
      "for(unsigned long long i=tid;i<count;i+=blockDim.x)total+=in[i];"
      "scratch[tid]=total;__syncthreads();"
      "for(unsigned int stride=blockDim.x/2;stride>0;stride>>=1){"
      "if(tid<stride)scratch[tid]+=scratch[tid+stride];__syncthreads();}"
      "if(tid==0)out[0]=scratch[0]/("+type+")count;}";
}

std::string hip_tensor_last_source(int dtype,int operation) {
    const auto type=hip_dtype_type(dtype);
    if(operation==1 && dtype!=9 && dtype!=10) return {};
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* in,unsigned long long count,unsigned long long width){"
         "unsigned long long segment=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "unsigned long long segments=count/width;if(segment>=segments)return;"
         "unsigned long long base=segment*width;"<<type<<" r=";
    if(operation==1) out<<"("<<type<<")0;";
    else out<<"in[base];";
    out<<"for(unsigned long long j=0;j<width;++j){";
    if(operation==1) out<<"r+=in[base+j];";
    else if(operation==2) out<<"if(in[base+j]>r)r=in[base+j];";
    else out<<"if(in[base+j]<r)r=in[base+j];";
    out<<"}for(unsigned long long j=0;j<width;++j)out[base+j]=r;}";
    return out.str();
}

#ifdef __APPLE__
std::string msl_tensor_mean_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "uint tid [[thread_index_in_threadgroup]], "
           "uint3 group_size [[threads_per_threadgroup]]) {\n"
           " threadgroup float scratch[256]; float total=0.0f;"
           " uint threads=group_size.x;"
           " for(ulong i=ulong(tid);i<count;i+=ulong(threads)) total+=in[i];"
           " scratch[tid]=total;"
           " threadgroup_barrier(mem_flags::mem_threadgroup);"
           " for(uint stride=threads/2;stride>0;stride>>=1){"
           " if(tid<stride) scratch[tid]+=scratch[tid+stride];"
           " threadgroup_barrier(mem_flags::mem_threadgroup);}"
           " if(tid==0) out[0]=scratch[0]/float(count); }\n";
}

std::string msl_last_reduce_source(int dtype,int operation) {
    const char* type=nullptr;
    switch(dtype){
        case 2:type="char";break;
        case 3:type="short";break;
        case 4:type="int";break;
        case 5:type="uchar";break;
        case 6:type="ushort";break;
        case 7:type="uint";break;
        case 10:type="float";break;
        default:return {};
    }
    if(operation==1 && dtype!=10) return {};
    const std::string init=operation==1
        ? std::string("(")+type+")0"
        : "in[base]";
    const std::string update=operation==1
        ? "reduced += value;"
        : operation==2
            ? "if(value > reduced) reduced=value;"
            : "if(value < reduced) reduced=value;";
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device "+std::string(type)+"* out [[buffer(0)]], "
           "const device "+std::string(type)+"* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "constant ulong& width [[buffer(3)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           " ulong segment=ulong(gid); ulong segments=count/width;"
           " if(segment>=segments)return; ulong base=segment*width;"
           " "+std::string(type)+" reduced="+init+";"
           " for(ulong j=0;j<width;++j){"+std::string(type)+" value=in[base+j];"+update+"}"
           " for(ulong j=0;j<width;++j)out[base+j]=reduced; }\n";
}
#endif

} // namespace

bool compute_mean_to(Buffer* output, const Buffer* input, int dtype,
                     std::size_t count, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (count == 0) { error = "tensor.mean() requires at least one element"; return false; }
    if (!compute_same_device(output,input,error) ||
        !compute_buffer_range(output,0,1,width,error) ||
        !compute_buffer_range(input,0,count,width,error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend==Backend::Test) {
        if(dtype==10) return fake_tensor_mean_to<float>(output,input,count,error);
        if(dtype==9) return fake_tensor_mean_to<double>(output,input,count,error);
        error="tensor.mean() requires a floating dtype"; return false;
    }
#endif
    if ((dtype==9||dtype==10) && output->backend==Backend::Cuda) {
        auto ptx=ptx_tensor_mean_source(dtype);
        auto out_ptr=output->cuda_pointer, in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_ptx_module(output->global_index,
            "tensor-mean-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        return launch(module,"q_kernel",LaunchDimensions{1,1,1},
                      LaunchDimensions{1,1,1},args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Hip){
        auto source=hip_tensor_mean_source(dtype);
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_hip_module(
            output->global_index,"tensor-mean-"+std::to_string(dtype),source,error);
        if(!module)return false;
        return launch(module,"q_kernel",LaunchDimensions{1,1,1},
                      LaunchDimensions{256,1,1},args,error);
    }
#ifdef __APPLE__
    if(dtype==10 && output->backend==Backend::Metal){
        auto source=msl_tensor_mean_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],
            "tensor-mean-f32-parallel",source,error);
        if(!pipeline)return false;
        const auto max_threads=
            static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]);
        std::size_t threads=1;
        while(threads*2<=std::min<std::size_t>(256,max_threads))
            threads*=2;
        const auto n=static_cast<std::uint64_t>(count);
        @autoreleasepool {
            id<MTLDevice> device=[output->metal_buffer device];
            id<MTLCommandQueue> queue=cached_metal_command_queue(device,error);
            if(!queue)return false;
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!command||!encoder){
                error="Metal tensor.mean() command creation failed";
                return false;
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
            [encoder setBuffer:input->metal_buffer offset:0 atIndex:1];
            [encoder setBytes:&n length:sizeof(n) atIndex:2];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1)
                   threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
            [encoder endEncoding];
            return commit_metal_command(device,command,error);
        }
    }
#endif
    return real_compute_unavailable(output,"tensor.mean()",dtype,error);
}

bool compute_last_reduce_broadcast(Buffer* output, const Buffer* input, int dtype,
                                   std::size_t count, std::size_t width,
                                   int operation, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(width==0||count%width!=0||operation<1||operation>3){
        error="invalid tensor last-axis reduction";return false;
    }
    if(operation==1 && dtype!=9 && dtype!=10){
        error="tensor sum_last() requires a floating dtype";return false;
    }
    if(!compute_same_device(output,input,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        switch(dtype){
            case 1:return fake_last_reduce<std::int64_t>(output,input,count,width,operation,error);
            case 2:return fake_last_reduce<std::int8_t>(output,input,count,width,operation,error);
            case 3:return fake_last_reduce<std::int16_t>(output,input,count,width,operation,error);
            case 4:return fake_last_reduce<std::int32_t>(output,input,count,width,operation,error);
            case 5:return fake_last_reduce<std::uint8_t>(output,input,count,width,operation,error);
            case 6:return fake_last_reduce<std::uint16_t>(output,input,count,width,operation,error);
            case 7:return fake_last_reduce<std::uint32_t>(output,input,count,width,operation,error);
            case 8:return fake_last_reduce<std::uint64_t>(output,input,count,width,operation,error);
            case 9:return fake_last_reduce<double>(output,input,count,width,operation,error);
            case 10:return fake_last_reduce<float>(output,input,count,width,operation,error);
            default:error="tensor last-axis reduction requires a numeric dtype";return false;
        }
    }
#endif
    const auto segments=count/width;
    if(output->backend==Backend::Cuda){
        auto ptx=ptx_last_reduce_source(dtype,operation);
        if(ptx.empty())return real_compute_unavailable(output,"tensor last-axis reduction",dtype,error);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        auto w=static_cast<std::uint64_t>(width);
        void* args[]={&out_ptr,&in_ptr,&n,&w};
        return launch_ptx_1d(output,
            "tensor-last-"+std::to_string(dtype)+"-"+std::to_string(operation),
            ptx,segments,args,error);
    }
    if(output->backend==Backend::Hip){
        auto source=hip_tensor_last_source(dtype,operation);
        if(source.empty())return real_compute_unavailable(output,"tensor last-axis reduction",dtype,error);
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count),w=static_cast<std::uint64_t>(width);
        void* args[]={&out_ptr,&in_ptr,&n,&w};
        return launch_hip_1d(output,
            "tensor-last-"+std::to_string(dtype)+"-"+std::to_string(operation),
            source,segments,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        auto source=msl_last_reduce_source(dtype,operation);
        if(source.empty())return real_compute_unavailable(output,"tensor last-axis reduction",dtype,error);
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],
            "tensor-last-"+std::to_string(dtype)+"-"+std::to_string(operation),source,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        const auto w=static_cast<std::uint64_t>(width);
        return dispatch_metal_1d(output,pipeline,segments,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
            [e setBytes:&w length:sizeof(w) atIndex:3];
        },error);
    }
#endif
    return real_compute_unavailable(output,"tensor last-axis reduction",dtype,error);
}
