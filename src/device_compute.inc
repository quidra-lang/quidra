
// Tensor compute primitives live here so Buffer internals stay private to
// device_backend.cpp while runtime.cpp only depends on the stable device API.

namespace {

std::size_t compute_dtype_width(int dtype) {
    switch (dtype) {
        case 1: case 8: case 9: return 8;
        case 2: case 5: return 1;
        case 3: case 6: return 2;
        case 4: case 7: case 10: return 4;
        default: return 0;
    }
}

bool compute_buffer_range(const Buffer* buffer, std::size_t offset,
                          std::size_t count, std::size_t width,
                          std::string& error) {
    if (!buffer || width == 0) {
        error = "invalid GPU tensor compute buffer";
        return false;
    }
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        error = "GPU tensor compute byte count overflow";
        return false;
    }
    const auto bytes = count * width;
    if (!range_ok(*buffer, offset, bytes)) {
        error = "GPU tensor compute range exceeds device buffer";
        return false;
    }
    return true;
}

bool compute_same_device(const Buffer* output, const Buffer* input,
                         std::string& error) {
    if (!output || !input ||
        output->global_index != input->global_index ||
        output->backend != input->backend) {
        error = "GPU tensor compute operands must use the same gpu(n)";
        return false;
    }
    return true;
}

template <typename T>
bool fake_add(T a, T b, T& out) {
    if constexpr (std::is_floating_point_v<T>) {
        out = static_cast<T>(a + b);
        return true;
    } else if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
            (b < 0 && a < std::numeric_limits<T>::min() - b)) return false;
        out = static_cast<T>(a + b);
        return true;
    } else {
        if (a > std::numeric_limits<T>::max() - b) return false;
        out = static_cast<T>(a + b);
        return true;
    }
}

template <typename T>
bool fake_sub(T a, T b, T& out) {
    if constexpr (std::is_floating_point_v<T>) {
        out = static_cast<T>(a - b);
        return true;
    } else if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a < std::numeric_limits<T>::min() + b) ||
            (b < 0 && a > std::numeric_limits<T>::max() + b)) return false;
        out = static_cast<T>(a - b);
        return true;
    } else {
        if (a < b) return false;
        out = static_cast<T>(a - b);
        return true;
    }
}

template <typename T>
bool fake_mul(T a, T b, T& out) {
    if constexpr (std::is_floating_point_v<T>) {
        out = static_cast<T>(a * b);
        return true;
    } else if constexpr (std::is_unsigned_v<T>) {
        if (b != 0 && a > std::numeric_limits<T>::max() / b) return false;
        out = static_cast<T>(a * b);
        return true;
    } else {
        if (a == 0 || b == 0) {
            out = 0;
            return true;
        }
        if (a == -1 && b == std::numeric_limits<T>::min()) return false;
        if (b == -1 && a == std::numeric_limits<T>::min()) return false;
        if (a > 0) {
            if ((b > 0 && a > std::numeric_limits<T>::max() / b) ||
                (b < 0 && b < std::numeric_limits<T>::min() / a)) return false;
        } else {
            if ((b > 0 && a < std::numeric_limits<T>::min() / b) ||
                (b < 0 && a < std::numeric_limits<T>::max() / b)) return false;
        }
        out = static_cast<T>(a * b);
        return true;
    }
}

template <typename T>
bool fake_binary_value(T a, T b, int operation, T& out) {
    switch (operation) {
        case 1: return fake_add(a, b, out);
        case 2: return fake_sub(a, b, out);
        case 3: return fake_mul(a, b, out);
        case 4:
            if constexpr (std::is_integral_v<T>) {
                if (b == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (a == std::numeric_limits<T>::min() && b == -1) return false;
                }
            }
            out = static_cast<T>(a / b);
            return true;
        case 5:
            if constexpr (std::is_integral_v<T>) {
                if (b == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (a == std::numeric_limits<T>::min() && b == -1) {
                        out = 0;
                        return true;
                    }
                }
                out = static_cast<T>(a % b);
                return true;
            }
            return false;
        default:
            return false;
    }
}

template <typename T>
bool fake_binary(Buffer* output, const Buffer* left, std::size_t left_offset,
                 const Buffer* right, std::size_t right_offset,
                 const void* scalar, int scalar_side, int operation,
                 std::size_t count, std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    auto* out = output->test_data.data() + left_offset * 0;
    const auto* a = left->test_data.data() + left_offset;
    const auto* b = right ? right->test_data.data() + right_offset : nullptr;
    T scalar_value{};
    if (scalar_side != 0) {
        if (!scalar) {
            error = "missing GPU tensor scalar operand";
            return false;
        }
        std::memcpy(&scalar_value, scalar, sizeof(T));
    }
    for (std::size_t i = 0; i < count; ++i) {
        T av{};
        T bv{};
        std::memcpy(&av, a + i * sizeof(T), sizeof(T));
        if (scalar_side == 0) {
            std::memcpy(&bv, b + i * sizeof(T), sizeof(T));
        }
        T lhs = scalar_side == 1 ? scalar_value : av;
        T rhs = scalar_side == 2 ? scalar_value : (scalar_side == 1 ? av : bv);
        T result{};
        if (!fake_binary_value(lhs, rhs, operation, result)) {
            error = operation == 4 || operation == 5
                ? "invalid tensor division/remainder or integer overflow"
                : "tensor integer arithmetic overflow";
            return false;
        }
        std::memcpy(out + i * sizeof(T), &result, sizeof(T));
    }
    return true;
#else
    (void)output; (void)left; (void)left_offset; (void)right;
    (void)right_offset; (void)scalar; (void)scalar_side; (void)operation;
    (void)count;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

template <typename Src, typename Dst>
bool fake_cast_values(Buffer* output, const Buffer* input,
                      std::size_t input_offset, std::size_t count,
                      std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto* src = input->test_data.data() + input_offset;
    auto* dst = output->test_data.data();
    for (std::size_t i = 0; i < count; ++i) {
        Src source{};
        std::memcpy(&source, src + i * sizeof(Src), sizeof(Src));
        Dst value{};
        if constexpr (std::is_integral_v<Src> && std::is_integral_v<Dst>) {
            if (!std::in_range<Dst>(source)) {
                error = "tensor cast is unsupported or a value is outside the target range";
                return false;
            }
            value = static_cast<Dst>(source);
        } else if constexpr (std::is_integral_v<Src> && std::is_floating_point_v<Dst>) {
            value = static_cast<Dst>(source);
            if (!std::isfinite(value)) {
                error = "tensor cast is unsupported or a value is outside the target range";
                return false;
            }
        } else if constexpr (std::is_floating_point_v<Src> && std::is_integral_v<Dst>) {
            error = "tensor cast is unsupported or a value is outside the target range";
            return false;
        } else {
            value = static_cast<Dst>(source);
            if constexpr (std::is_floating_point_v<Src> &&
                          std::is_floating_point_v<Dst>) {
                if (std::isfinite(source) && !std::isfinite(value)) {
                    error = "tensor cast is unsupported or a value is outside the target range";
                    return false;
                }
            }
        }
        std::memcpy(dst + i * sizeof(Dst), &value, sizeof(Dst));
    }
    return true;
#else
    (void)output; (void)input; (void)input_offset; (void)count;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

template <typename Src>
bool fake_cast_from(Buffer* output, const Buffer* input,
                    std::size_t input_offset, int target_dtype,
                    std::size_t count, std::string& error) {
    switch (target_dtype) {
        case 1: return fake_cast_values<Src, std::int64_t>(output,input,input_offset,count,error);
        case 2: return fake_cast_values<Src, std::int8_t>(output,input,input_offset,count,error);
        case 3: return fake_cast_values<Src, std::int16_t>(output,input,input_offset,count,error);
        case 4: return fake_cast_values<Src, std::int32_t>(output,input,input_offset,count,error);
        case 5: return fake_cast_values<Src, std::uint8_t>(output,input,input_offset,count,error);
        case 6: return fake_cast_values<Src, std::uint16_t>(output,input,input_offset,count,error);
        case 7: return fake_cast_values<Src, std::uint32_t>(output,input,input_offset,count,error);
        case 8: return fake_cast_values<Src, std::uint64_t>(output,input,input_offset,count,error);
        case 9: return fake_cast_values<Src, double>(output,input,input_offset,count,error);
        case 10:return fake_cast_values<Src, float>(output,input,input_offset,count,error);
        default:
            error = "invalid tensor cast target dtype";
            return false;
    }
}

std::string ptx_thread_prologue(std::string_view parameters,
                                std::string_view registers) {
    std::ostringstream out;
    out << ".version 6.0\n"
           ".target sm_30\n"
           ".address_size 64\n\n"
           ".visible .entry q_kernel(\n"
        << parameters << ")\n{\n"
        << registers
        << "  mov.u32 %r1, %ctaid.x;\n"
           "  mov.u32 %r2, %ntid.x;\n"
           "  mov.u32 %r3, %tid.x;\n"
           "  mad.lo.s32 %r4, %r1, %r2, %r3;\n"
           "  cvt.u64.u32 %rd7, %r4;\n";
    return out.str();
}

Module* cached_ptx_module(int device, const std::string& key,
                          const std::string& ptx, std::string& error) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, Module*> cache;
    const auto cache_key = std::to_string(device) + ":" + key;
    std::lock_guard lock(cache_mutex);
    const auto found = cache.find(cache_key);
    if (found != cache.end()) return found->second;
    auto* module = load_ptx(device, ptx, error);
    if (!module) return nullptr;
    cache.emplace(cache_key, module);
    return module;
}

bool launch_ptx_1d(Buffer* output, const std::string& key,
                   const std::string& ptx, std::size_t count,
                   void** arguments, std::string& error) {
    if (count == 0) return true;
    constexpr unsigned block = 256;
    const auto blocks64 = (count + block - 1) / block;
    if (blocks64 > std::numeric_limits<unsigned>::max()) {
        error = "GPU tensor compute launch is too large";
        return false;
    }
    auto* module = cached_ptx_module(output->global_index, key, ptx, error);
    if (!module) return false;
    return launch(module, "q_kernel",
                  LaunchDimensions{static_cast<unsigned>(blocks64),1,1},
                  LaunchDimensions{block,1,1}, arguments, error);
}

Module* cached_hip_module(int device, const std::string& key,
                          const std::string& source, std::string& error) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, Module*> cache;
    const auto cache_key = std::to_string(device) + ":" + key;
    std::lock_guard lock(cache_mutex);
    const auto found = cache.find(cache_key);
    if (found != cache.end()) return found->second;
    auto* module = load_hip_source(device, source, error);
    if (!module) return nullptr;
    cache.emplace(cache_key, module);
    return module;
}

bool launch_hip_1d(Buffer* output, const std::string& key,
                   const std::string& source, std::size_t count,
                   void** arguments, std::string& error) {
    if (count == 0) return true;
    constexpr unsigned block = 256;
    const auto blocks64 = (count + block - 1) / block;
    if (blocks64 > std::numeric_limits<unsigned>::max()) {
        error = "GPU tensor compute launch is too large";
        return false;
    }
    auto* module = cached_hip_module(output->global_index, key, source, error);
    if (!module) return false;
    return launch(module, "q_kernel",
                  LaunchDimensions{static_cast<unsigned>(blocks64),1,1},
                  LaunchDimensions{block,1,1}, arguments, error);
}

std::string hip_raw_type(std::size_t width) {
    switch (width) {
        case 1: return "unsigned char";
        case 2: return "unsigned short";
        case 4: return "unsigned int";
        case 8: return "unsigned long long";
        default: return {};
    }
}

std::string hip_dtype_type(int dtype) {
    switch (dtype) {
        case 1: return "long long";
        case 2: return "signed char";
        case 3: return "short";
        case 4: return "int";
        case 5: return "unsigned char";
        case 6: return "unsigned short";
        case 7: return "unsigned int";
        case 8: return "unsigned long long";
        case 9: return "double";
        case 10:return "float";
        default:return {};
    }
}

std::string hip_fill_source(std::size_t width, std::uint64_t bits) {
    const auto type=hip_raw_type(width);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,unsigned long long count){"
          "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
          "if(i<count)out[i]=("<<type<<")"<<bits<<"ull;}";
    return out.str();
}

std::string hip_gather_source(std::size_t width) {
    const auto type=hip_raw_type(width);
    return "extern \"C\" __global__ void q_kernel("+type+"* out,const "+type+
           "* src,const unsigned long long* indices,unsigned long long count){"
           "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
           "if(i<count)out[i]=src[indices[i]];}";
}


bool hip_integer_dtype(int dtype) {
    return dtype>=1 && dtype<=8;
}
bool hip_signed_dtype(int dtype) {
    return dtype>=1 && dtype<=4;
}
std::string hip_signed_min(int dtype) {
    switch(dtype){
        case 1:return "(-9223372036854775807ll-1ll)";
        case 2:return "(-127-1)";
        case 3:return "(-32767-1)";
        case 4:return "(-2147483647-1)";
        default:return "0";
    }
}
std::string hip_integral_max(int dtype) {
    switch(dtype){
        case 1:return "9223372036854775807ll";
        case 2:return "127";
        case 3:return "32767";
        case 4:return "2147483647";
        case 5:return "255u";
        case 6:return "65535u";
        case 7:return "4294967295u";
        case 8:return "18446744073709551615ull";
        default:return "0";
    }
}
std::string hip_integer_binary_source(int dtype,int operation,int scalar_side) {
    const auto type=hip_dtype_type(dtype);
    const bool is_signed=hip_signed_dtype(dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* left,const "<<type<<"* right,"<<type
       <<" scalar,unsigned long long count,unsigned int* status){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" a=left[i],b=";
    if(scalar_side==0) out<<"right[i];";
    else out<<"scalar;";
    if(scalar_side==1) out<<type<<" tmp=a;a=b;b=tmp;";
    out<<type<<" r=0;bool bad=false;";
    if(operation==1) out<<"bad=__builtin_add_overflow(a,b,&r);";
    else if(operation==2) out<<"bad=__builtin_sub_overflow(a,b,&r);";
    else if(operation==3) out<<"bad=__builtin_mul_overflow(a,b,&r);";
    else if(operation==4){
        out<<"if(b==0)bad=true;";
        if(is_signed) out<<"else if(a==("<<type<<")"<<hip_signed_min(dtype)<<"&&b==("<<type<<")-1)bad=true;";
        out<<"else r=("<<type<<")(a/b);";
    }else{
        out<<"if(b==0)bad=true;else {";
        if(is_signed) out<<"if(a==("<<type<<")"<<hip_signed_min(dtype)<<"&&b==("<<type<<")-1)r=("<<type<<")0;else ";
        out<<"r=("<<type<<")(a%b);}";
    }
    out<<"if(bad){status[0]=1u;return;}out[i]=r;}";
    return out.str();
}
std::string hip_integer_unary_source(int dtype) {
    const auto type=hip_dtype_type(dtype);
    return "extern \"C\" __global__ void q_kernel("+type+
      "* out,const "+type+"* in,unsigned long long count,unsigned int* status){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
      +type+" v=in[i];if(v==("+type+")"+hip_signed_min(dtype)+"){status[0]=1u;return;}out[i]=("+type+")(-v);}";
}
std::string hip_cast_source(int source_dtype,int target_dtype) {
    const auto src=hip_dtype_type(source_dtype),dst=hip_dtype_type(target_dtype);
    const bool src_int=hip_integer_dtype(source_dtype),dst_int=hip_integer_dtype(target_dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<dst<<"* out,const "<<src
       <<"* in,unsigned long long count,unsigned int* status){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
       <<src<<" v=in[i];";
    if(src_int&&dst_int){
        const bool ss=hip_signed_dtype(source_dtype),ds=hip_signed_dtype(target_dtype);
        if(ss&&ds){
            out<<"long long q=(long long)v;if(q<(long long)"<<hip_signed_min(target_dtype)
               <<"||q>(long long)"<<hip_integral_max(target_dtype)<<"){status[0]=1u;return;}";
        }else if(ss&&!ds){
            out<<"if(v<0){status[0]=1u;return;}unsigned long long q=(unsigned long long)v;"
                  "if(q>(unsigned long long)"<<hip_integral_max(target_dtype)<<"){status[0]=1u;return;}";
        }else if(!ss&&ds){
            out<<"unsigned long long q=(unsigned long long)v;if(q>(unsigned long long)"
               <<hip_integral_max(target_dtype)<<"){status[0]=1u;return;}";
        }else{
            out<<"unsigned long long q=(unsigned long long)v;if(q>(unsigned long long)"
               <<hip_integral_max(target_dtype)<<"){status[0]=1u;return;}";
        }
    }
    out<<"out[i]=("<<dst<<")v;}";
    return out.str();
}
std::string hip_matmul_source(int dtype,bool checked) {
    const auto type=hip_dtype_type(dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* left,const "<<type
       <<"* right,unsigned long long m,unsigned long long k,unsigned long long n,unsigned int* status){"
         "unsigned long long g=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(g>=m*n)return;"
         "unsigned long long row=g/n,col=g%n;"<<type<<" total=("<<type<<")0;"
         "for(unsigned long long i=0;i<k;++i){"<<type<<" p=0,next=0;";
    if(checked)
        out<<"if(__builtin_mul_overflow(left[row*k+i],right[i*n+col],&p)||"
              "__builtin_add_overflow(total,p,&next)){status[0]=1u;return;}total=next;";
    else
        out<<"total+=left[row*k+i]*right[i*n+col];";
    out<<"}out[g]=total;}";
    return out.str();
}
std::string hip_dot_source(int dtype,bool checked) {
    const auto type=hip_dtype_type(dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* left,const "<<type<<"* right,unsigned long long count,unsigned int* status){"
         "if(blockIdx.x||threadIdx.x)return;"<<type<<" total=("<<type<<")0;"
         "for(unsigned long long i=0;i<count;++i){"<<type<<" p=0,next=0;";
    if(checked)
        out<<"if(__builtin_mul_overflow(left[i],right[i],&p)||__builtin_add_overflow(total,p,&next)){status[0]=1u;return;}total=next;";
    else out<<"total+=left[i]*right[i];";
    out<<"}out[0]=total;}";
    return out.str();
}
std::string hip_reduce_source(int dtype,int operation,bool checked) {
    const auto type=hip_dtype_type(dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* in,unsigned long long count,unsigned int* status){if(blockIdx.x||threadIdx.x)return;"
       <<type<<" result=";
    if(operation==1) out<<"("<<type<<")0;unsigned long long start=0;";
    else out<<"in[0];unsigned long long start=1;";
    out<<"for(unsigned long long i=start;i<count;++i){";
    if(operation==1&&checked) out<<type<<" next=0;if(__builtin_add_overflow(result,in[i],&next)){status[0]=1u;return;}result=next;";
    else if(operation==1) out<<"result+=in[i];";
    else if(operation==2) out<<"if(in[i]<result)result=in[i];";
    else out<<"if(in[i]>result)result=in[i];";
    out<<"}out[0]=result;}";
    return out.str();
}

std::string hip_float_binary_source(int dtype,int operation,int scalar_side) {
    const auto type=dtype==10?"float":"double";
    const char* op=operation==1?"+":operation==2?"-":operation==3?"*":"/";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* left,const "<<type<<"* right,"<<type<<" scalar,unsigned long long count){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" a=left[i];";
    if(scalar_side==0) out<<type<<" b=right[i];out[i]=a"<<op<<"b;";
    else if(scalar_side==1) out<<"out[i]=scalar"<<op<<"a;";
    else out<<"out[i]=a"<<op<<"scalar;";
    out<<"}";
    return out.str();
}

std::string hip_float_unary_source(int dtype,int operation) {
    const auto type=dtype==10?"float":"double";
    std::string expr;
    if(operation==1) expr="-v";
    else if(operation==2) expr=dtype==10?"__builtin_fabsf(v)":"__builtin_fabs(v)";
    else if(operation==3) expr=dtype==10?"__builtin_expf(v)":"__builtin_exp(v)";
    else expr=dtype==10?"__builtin_logf(v)":"__builtin_log(v)";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
        "* out,const "+type+"* in,unsigned long long count){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count){"+type+" v=in[i];out[i]="+expr+";}}";
}

std::string hip_log_validate_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "#include <math.h>\nextern \"C\" __global__ void q_kernel(const "+
        std::string(type)+"* in,unsigned long long count,unsigned int* status){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count){"+type+" v=in[i];if(!(v>("+type+")0)||!isfinite(v))"
        "atomicExch(status,1u);}}";
}

std::string hip_float_cast_source(int source_dtype,int target_dtype) {
    const auto src=source_dtype==10?"float":"double";
    const auto dst=target_dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(dst)+
        "* out,const "+src+"* in,unsigned long long count){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count)out[i]=("+dst+")in[i];}";
}

std::string hip_float_narrow_validate_source() {
    return "#include <math.h>\nextern \"C\" __global__ void q_kernel("
           "const double* in,unsigned long long count,unsigned int* status){"
           "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
           "if(i<count){double v=in[i];float narrowed=(float)v;"
           "if(isfinite(v)&&!isfinite(narrowed))atomicExch(status,1u);}}";
}

std::string hip_float_matmul_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
        "* out,const "+type+"* left,const "+type+"* right,"
        "unsigned long long m,unsigned long long k,unsigned long long n){"
        "unsigned long long g=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(g>=m*n)return;unsigned long long row=g/n,col=g%n;"+type+" total=("+type+")0;"
        "for(unsigned long long i=0;i<k;++i)total+=left[row*k+i]*right[i*n+col];"
        "out[g]=total;}";
}

std::string hip_float_dot_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
        "* out,const "+type+"* left,const "+type+"* right,unsigned long long count){"
        "if(blockIdx.x||threadIdx.x)return;"+type+" total=("+type+")0;"
        "for(unsigned long long i=0;i<count;++i)total+=left[i]*right[i];out[0]=total;}";
}

std::string hip_float_reduce_source(int dtype,int operation) {
    const auto type=dtype==10?"float":"double";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type
       <<"* out,const "<<type<<"* in,unsigned long long count){"
         "if(blockIdx.x||threadIdx.x)return;"<<type<<" result=";
    if(operation==1) out<<"("<<type<<")0;unsigned long long start=0;";
    else out<<"in[0];unsigned long long start=1;";
    out<<"for(unsigned long long i=start;i<count;++i){"<<type<<" v=in[i];";
    if(operation==1) out<<"result+=v;";
    else if(operation==2) out<<"if(v<result)result=v;";
    else out<<"if(v>result)result=v;";
    out<<"}out[0]=result;}";
    return out.str();
}

std::string hip_mean_source(int dtype) {
    const auto type=hip_dtype_type(dtype);
    return "extern \"C\" __global__ void q_kernel(double* out,const "+
        type+"* in,unsigned long long count){if(blockIdx.x||threadIdx.x)return;"
        "double total=0.0;for(unsigned long long i=0;i<count;++i)total+=(double)in[i];"
        "out[0]=total/(double)count;}";
}

std::string ptx_fill_kernel(std::size_t width, std::uint64_t bits) {
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_count\n",
        "  .reg .pred %p<2>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<16>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd1, %rd8;\n";
    if (width == 1) {
        out << "  mov.u32 %r5, " << static_cast<unsigned>(bits & 0xffU) << ";\n"
               "  st.global.u8 [%rd9], %r5;\n";
    } else if (width == 2) {
        out << "  mov.u32 %r5, " << static_cast<unsigned>(bits & 0xffffU) << ";\n"
               "  st.global.u16 [%rd9], %r5;\n";
    } else if (width == 4) {
        out << "  mov.u32 %r5, 0x" << std::hex
            << static_cast<std::uint32_t>(bits) << std::dec << ";\n"
               "  st.global.u32 [%rd9], %r5;\n";
    } else {
        out << "  mov.u64 %rd10, 0x" << std::hex << bits << std::dec << ";\n"
               "  st.global.u64 [%rd9], %rd10;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_gather_kernel(std::size_t width) {
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_src,\n"
        "  .param .u64 p_indices,\n  .param .u64 p_count\n",
        "  .reg .pred %p<2>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<20>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_src];\n"
           "  ld.param.u64 %rd3, [p_indices];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
           "  mul.lo.u64 %rd8, %rd7, 8;\n"
           "  add.u64 %rd9, %rd3, %rd8;\n"
           "  ld.global.u64 %rd10, [%rd9];\n"
        << "  mul.lo.u64 %rd11, %rd10, " << width << ";\n"
           "  add.u64 %rd12, %rd2, %rd11;\n"
        << "  mul.lo.u64 %rd13, %rd7, " << width << ";\n"
           "  add.u64 %rd14, %rd1, %rd13;\n";
    if (width == 1) {
        out << "  ld.global.u8 %r5, [%rd12];\n"
               "  st.global.u8 [%rd14], %r5;\n";
    } else if (width == 2) {
        out << "  ld.global.u16 %r5, [%rd12];\n"
               "  st.global.u16 [%rd14], %r5;\n";
    } else if (width == 4) {
        out << "  ld.global.u32 %r5, [%rd12];\n"
               "  st.global.u32 [%rd14], %r5;\n";
    } else {
        out << "  ld.global.u64 %rd15, [%rd12];\n"
               "  st.global.u64 [%rd14], %rd15;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_binary_kernel(int dtype, int operation, int scalar_side) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    const char* kind = f32 ? "f32" : "f64";
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_left,\n"
        "  .param .u64 p_right,\n  .param .u64 p_scalar,\n"
        "  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<2>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f64 %fd<8>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_left];\n"
           "  ld.param.u64 %rd3, [p_right];\n"
           "  ld.param.u64 %rd4, [p_scalar];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
           "  add.u64 %rd10, %rd3, %rd8;\n"
           "  add.u64 %rd11, %rd1, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n";
        if (scalar_side == 0) out << "  ld.global.f32 %f2, [%rd10];\n";
        else out << "  cvt.u32.u64 %r5, %rd4;\n  mov.b32 %f2, %r5;\n";
        const char* lhs = scalar_side == 1 ? "%f2" : "%f1";
        const char* rhs = scalar_side == 1 ? "%f1" : "%f2";
        const char* op = operation == 1 ? "add" : operation == 2 ? "sub" :
                         operation == 3 ? "mul" : "div";
        out << "  " << op << "." << kind << " %f3, " << lhs << ", " << rhs << ";\n"
               "  st.global.f32 [%rd11], %f3;\n";
    } else {
        out << "  ld.global.f64 %fd1, [%rd9];\n";
        if (scalar_side == 0) out << "  ld.global.f64 %fd2, [%rd10];\n";
        else out << "  mov.b64 %fd2, %rd4;\n";
        const char* lhs = scalar_side == 1 ? "%fd2" : "%fd1";
        const char* rhs = scalar_side == 1 ? "%fd1" : "%fd2";
        const char* op = operation == 1 ? "add" : operation == 2 ? "sub" :
                         operation == 3 ? "mul" : "div";
        out << "  " << op << "." << kind << " %fd3, " << lhs << ", " << rhs << ";\n"
               "  st.global.f64 [%rd11], %fd3;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_log_validate_kernel(int dtype) {
    const bool f32=dtype==10;
    const auto width=f32?4:8;
    std::ostringstream out;
    out<<ptx_thread_prologue(
        "  .param .u64 p_in,\n  .param .u64 p_count,\n  .param .u64 p_status\n",
        f32
          ? "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<20>;\n  .reg .f32 %f<4>;\n"
          : "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f64 %fd<4>;\n");
    out<<"  ld.param.u64 %rd1, [p_in];\n"
          "  ld.param.u64 %rd5, [p_count];\n"
          "  ld.param.u64 %rd4, [p_status];\n"
          "  setp.ge.u64 %p1, %rd7, %rd5;\n"
          "  @%p1 bra DONE;\n"
       <<"  mul.lo.u64 %rd8, %rd7, "<<width<<";\n"
          "  add.u64 %rd9, %rd1, %rd8;\n";
    if(f32){
        out<<"  ld.global.f32 %f1, [%rd9];\n"
              "  setp.le.f32 %p2, %f1, 0f00000000;\n"
              "  ld.global.u32 %r5, [%rd9];\n"
              "  and.b32 %r6, %r5, 0x7fffffff;\n"
              "  setp.ge.u32 %p3, %r6, 0x7f800000;\n";
    }else{
        out<<"  ld.global.f64 %fd1, [%rd9];\n"
              "  setp.le.f64 %p2, %fd1, 0d0000000000000000;\n"
              "  ld.global.u64 %rd10, [%rd9];\n"
              "  and.b64 %rd11, %rd10, 0x7fffffffffffffff;\n"
              "  setp.ge.u64 %p3, %rd11, 0x7ff0000000000000;\n";
    }
    out<<"  or.pred %p4, %p2, %p3;\n"
          "  @!%p4 bra DONE;\n"
          "  atom.global.exch.b32 %r7, [%rd4], 1;\n"
          "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_unary_kernel(int dtype, int operation) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_in,\n  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<6>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<24>;\n  .reg .f32 %f<12>;\n"
          : "  .reg .pred %p<8>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<40>;\n  .reg .f64 %fd<24>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
           "  add.u64 %rd10, %rd1, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n";
        if (operation == 1) out << "  neg.f32 %f2, %f1;\n";
        else if (operation == 2) out << "  abs.f32 %f2, %f1;\n";
        else if (operation == 3) {
            out << "  mul.f32 %f3, %f1, 0f3FB8AA3B;\n"
                   "  ex2.approx.f32 %f2, %f3;\n";
        } else {
            out << "  lg2.approx.f32 %f3, %f1;\n"
                   "  mul.f32 %f2, %f3, 0f3F317218;\n";
        }
        out << "  st.global.f32 [%rd10], %f2;\n";
    } else {
        out << "  ld.global.f64 %fd1, [%rd9];\n";
        if (operation == 1) {
            out << "  neg.f64 %fd2, %fd1;\n";
        } else if (operation == 2) {
            out << "  abs.f64 %fd2, %fd1;\n";
        } else if (operation == 3) {
            out <<
                "  setp.nan.f64 %p2, %fd1, %fd1;\n"
                "  @%p2 mov.f64 %fd2, %fd1;\n"
                "  @%p2 bra F64_STORE;\n"
                "  setp.gt.f64 %p2, %fd1, 0d40862e42fefa39ef;\n"
                "  @%p2 mov.u64 %rd20, 0x7ff0000000000000;\n"
                "  @%p2 mov.b64 %fd2, %rd20;\n"
                "  @%p2 bra F64_STORE;\n"
                "  setp.lt.f64 %p2, %fd1, 0dc0874910d52d3051;\n"
                "  @%p2 mov.f64 %fd2, 0d0000000000000000;\n"
                "  @%p2 bra F64_STORE;\n"
                "  mul.f64 %fd3, %fd1, 0d3ff71547652b82fe;\n"
                "  cvt.rni.s64.f64 %rd11, %fd3;\n"
                "  setp.lt.s64 %p3, %rd11, -1074;\n"
                "  @%p3 mov.s64 %rd11, -1074;\n"
                "  cvt.rn.f64.s64 %fd4, %rd11;\n"
                "  mul.f64 %fd5, %fd4, 0d3fe62e42fefa39ef;\n"
                "  sub.f64 %fd6, %fd1, %fd5;\n"
                "  mul.f64 %fd5, %fd4, 0d3c7abc9e3b39803f;\n"
                "  sub.f64 %fd6, %fd6, %fd5;\n"
                "  mov.f64 %fd7, 0d3d6ae7f3e733b81f;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3da93974a8c07c9d;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3de6124613a86d09;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3e21eed8eff8d898;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3e5ae64567f544e4;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3e927e4fb7789f5c;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3ec71de3a556c734;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3efa01a01a01a01a;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3f2a01a01a01a01a;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3f56c16c16c16c17;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3f81111111111111;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3fa5555555555555;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3fc5555555555555;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3fe0000000000000;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3ff0000000000000;\n"
                "  mul.f64 %fd7, %fd7, %fd6;\n"
                "  add.f64 %fd7, %fd7, 0d3ff0000000000000;\n"
                "  setp.ge.s64 %p3, %rd11, -1022;\n"
                "  @!%p3 bra F64_EXP_SUBNORMAL;\n"
                "  add.s64 %rd12, %rd11, 1023;\n"
                "  shl.b64 %rd13, %rd12, 52;\n"
                "  mov.b64 %fd8, %rd13;\n"
                "  mul.f64 %fd2, %fd7, %fd8;\n"
                "  bra F64_STORE;\n"
                "F64_EXP_SUBNORMAL:\n"
                "  add.s64 %rd12, %rd11, 1074;\n"
                "  cvt.u32.u64 %r5, %rd12;\n"
                "  mov.u64 %rd13, 1;\n"
                "  shl.b64 %rd13, %rd13, %r5;\n"
                "  mov.b64 %fd8, %rd13;\n"
                "  mul.f64 %fd2, %fd7, %fd8;\n"
                "  bra F64_STORE;\n";
        } else {
            out <<
                "  mov.b64 %rd11, %fd1;\n"
                "  shr.u64 %rd12, %rd11, 52;\n"
                "  and.b64 %rd12, %rd12, 0x7ff;\n"
                "  setp.ne.u64 %p2, %rd12, 0;\n"
                "  @%p2 bra F64_LOG_NORMAL;\n"
                "  mul.f64 %fd1, %fd1, 0d4350000000000000;\n"
                "  mov.b64 %rd11, %fd1;\n"
                "  shr.u64 %rd12, %rd11, 52;\n"
                "  and.b64 %rd12, %rd12, 0x7ff;\n"
                "  sub.s64 %rd13, %rd12, 1077;\n"
                "  bra F64_LOG_MANT;\n"
                "F64_LOG_NORMAL:\n"
                "  sub.s64 %rd13, %rd12, 1023;\n"
                "F64_LOG_MANT:\n"
                "  and.b64 %rd14, %rd11, 0x000fffffffffffff;\n"
                "  or.b64 %rd14, %rd14, 0x3ff0000000000000;\n"
                "  mov.b64 %fd3, %rd14;\n"
                "  setp.gt.f64 %p3, %fd3, 0d3ff6a09e667f3bcd;\n"
                "  @!%p3 bra F64_LOG_REDUCED;\n"
                "  mul.f64 %fd3, %fd3, 0d3fe0000000000000;\n"
                "  add.s64 %rd13, %rd13, 1;\n"
                "F64_LOG_REDUCED:\n"
                "  sub.f64 %fd4, %fd3, 0d3ff0000000000000;\n"
                "  add.f64 %fd5, %fd3, 0d3ff0000000000000;\n"
                "  div.f64 %fd6, %fd4, %fd5;\n"
                "  mul.f64 %fd7, %fd6, %fd6;\n"
                "  mov.f64 %fd8, 0d3faaf286bca1af28;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fae1e1e1e1e1e1e;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fb1111111111111;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fb3b13b13b13b14;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fb745d1745d1746;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fbc71c71c71c71c;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fc2492492492492;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fc999999999999a;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3fd5555555555555;\n"
                "  mul.f64 %fd8, %fd8, %fd7;\n"
                "  add.f64 %fd8, %fd8, 0d3ff0000000000000;\n"
                "  mul.f64 %fd8, %fd8, %fd6;\n"
                "  add.f64 %fd8, %fd8, %fd8;\n"
                "  cvt.rn.f64.s64 %fd9, %rd13;\n"
                "  mul.f64 %fd9, %fd9, 0d3fe62e42fefa39ef;\n"
                "  add.f64 %fd2, %fd9, %fd8;\n"
                "  bra F64_STORE;\n";
        }
        out << "F64_STORE:\n"
               "  st.global.f64 [%rd10], %fd2;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_cast_kernel(int source_dtype, int target_dtype) {
    const bool src32 = source_dtype == 10;
    const bool dst32 = target_dtype == 10;
    const auto src_width = src32 ? 4 : 8;
    const auto dst_width = dst32 ? 4 : 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_in,\n  .param .u64 p_count\n",
        "  .reg .pred %p<2>;\n  .reg .b32 %r<10>;\n  .reg .b64 %rd<20>;\n"
        "  .reg .f32 %f<4>;\n  .reg .f64 %fd<4>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << src_width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
        << "  mul.lo.u64 %rd10, %rd7, " << dst_width << ";\n"
           "  add.u64 %rd11, %rd1, %rd10;\n";
    if (src32 && !dst32) {
        out << "  ld.global.f32 %f1, [%rd9];\n"
               "  cvt.f64.f32 %fd1, %f1;\n"
               "  st.global.f64 [%rd11], %fd1;\n";
    } else if (!src32 && dst32) {
        out << "  ld.global.f64 %fd1, [%rd9];\n"
               "  cvt.rn.f32.f64 %f1, %fd1;\n"
               "  st.global.f32 [%rd11], %f1;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_narrow_validate_kernel() {
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_in,\n  .param .u64 p_count,\n  .param .u64 p_status\n",
        "  .reg .pred %p<4>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<20>;\n");
    out << "  ld.param.u64 %rd1, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd4, [p_status];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
           "  mul.lo.u64 %rd8, %rd7, 8;\n"
           "  add.u64 %rd9, %rd1, %rd8;\n"
           "  ld.global.u64 %rd10, [%rd9];\n"
           "  and.b64 %rd11, %rd10, 0x7fffffffffffffff;\n"
           "  setp.ge.u64 %p2, %rd11, 0x7ff0000000000000;\n"
           "  @%p2 bra DONE;\n"
           "  setp.gt.u64 %p3, %rd11, 0x47efffffe0000000;\n"
           "  @!%p3 bra DONE;\n"
           "  atom.global.exch.b32 %r5, [%rd4], 1;\n"
           "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_matmul_kernel(int dtype) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
           ".visible .entry q_kernel(\n"
           "  .param .u64 p_out,\n  .param .u64 p_left,\n"
           "  .param .u64 p_right,\n  .param .u64 p_m,\n"
           "  .param .u64 p_k,\n  .param .u64 p_n)\n{\n"
        << (f32
          ? "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<32>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<32>;\n  .reg .f64 %fd<8>;\n")
        << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_left];\n"
           "  ld.param.u64 %rd3, [p_right];\n"
           "  ld.param.u64 %rd4, [p_m];\n"
           "  ld.param.u64 %rd5, [p_k];\n"
           "  ld.param.u64 %rd6, [p_n];\n"
           "  mov.u32 %r1, %ctaid.x;\n"
           "  mov.u32 %r2, %ntid.x;\n"
           "  mov.u32 %r3, %tid.x;\n"
           "  mad.lo.s32 %r4, %r1, %r2, %r3;\n"
           "  cvt.u64.u32 %rd7, %r4;\n"
           "  mul.lo.u64 %rd8, %rd4, %rd6;\n"
           "  setp.ge.u64 %p1, %rd7, %rd8;\n"
           "  @%p1 bra DONE;\n"
           "  div.u64 %rd9, %rd7, %rd6;\n"
           "  rem.u64 %rd10, %rd7, %rd6;\n"
           "  mov.u64 %rd11, 0;\n";
    if (f32) out << "  mov.f32 %f1, 0f00000000;\n";
    else out << "  mov.f64 %fd1, 0d0000000000000000;\n";
    out << "LOOP:\n"
           "  setp.ge.u64 %p2, %rd11, %rd5;\n"
           "  @%p2 bra STORE;\n"
           "  mul.lo.u64 %rd12, %rd9, %rd5;\n"
           "  add.u64 %rd12, %rd12, %rd11;\n"
        << "  mul.lo.u64 %rd12, %rd12, " << width << ";\n"
           "  add.u64 %rd13, %rd2, %rd12;\n"
           "  mul.lo.u64 %rd14, %rd11, %rd6;\n"
           "  add.u64 %rd14, %rd14, %rd10;\n"
        << "  mul.lo.u64 %rd14, %rd14, " << width << ";\n"
           "  add.u64 %rd15, %rd3, %rd14;\n";
    if (f32) {
        out << "  ld.global.f32 %f2, [%rd13];\n"
               "  ld.global.f32 %f3, [%rd15];\n"
               "  mul.f32 %f4, %f2, %f3;\n"
               "  add.f32 %f1, %f1, %f4;\n";
    } else {
        out << "  ld.global.f64 %fd2, [%rd13];\n"
               "  ld.global.f64 %fd3, [%rd15];\n"
               "  mul.f64 %fd4, %fd2, %fd3;\n"
               "  add.f64 %fd1, %fd1, %fd4;\n";
    }
    out << "  add.u64 %rd11, %rd11, 1;\n"
           "  bra LOOP;\n"
           "STORE:\n"
        << "  mul.lo.u64 %rd16, %rd7, " << width << ";\n"
           "  add.u64 %rd17, %rd1, %rd16;\n";
    if (f32) out << "  st.global.f32 [%rd17], %f1;\n";
    else out << "  st.global.f64 [%rd17], %fd1;\n";
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_dot_kernel(int dtype) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
           ".visible .entry q_kernel(\n"
           "  .param .u64 p_out,\n  .param .u64 p_left,\n"
           "  .param .u64 p_right,\n  .param .u64 p_count)\n{\n"
        << (f32
          ? "  .reg .pred %p<2>;\n  .reg .b64 %rd<20>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<2>;\n  .reg .b64 %rd<20>;\n  .reg .f64 %fd<8>;\n")
        << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_left];\n"
           "  ld.param.u64 %rd3, [p_right];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  mov.u64 %rd7, 0;\n";
    if (f32) out << "  mov.f32 %f1, 0f00000000;\n";
    else out << "  mov.f64 %fd1, 0d0000000000000000;\n";
    out << "LOOP:\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra STORE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
           "  add.u64 %rd10, %rd3, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f2, [%rd9];\n"
               "  ld.global.f32 %f3, [%rd10];\n"
               "  mul.f32 %f4, %f2, %f3;\n"
               "  add.f32 %f1, %f1, %f4;\n";
    } else {
        out << "  ld.global.f64 %fd2, [%rd9];\n"
               "  ld.global.f64 %fd3, [%rd10];\n"
               "  mul.f64 %fd4, %fd2, %fd3;\n"
               "  add.f64 %fd1, %fd1, %fd4;\n";
    }
    out << "  add.u64 %rd7, %rd7, 1;\n"
           "  bra LOOP;\nSTORE:\n";
    if (f32) out << "  st.global.f32 [%rd1], %f1;\n";
    else out << "  st.global.f64 [%rd1], %fd1;\n";
    out << "  ret;\n}\n";
    return out.str();
}

std::string ptx_float_mean_kernel(int dtype) {
    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
           ".visible .entry q_kernel(\n"
           "  .param .u64 p_out,\n  .param .u64 p_in,\n"
           "  .param .u64 p_count)\n{\n"
           "  .reg .pred %p<2>;\n  .reg .b64 %rd<20>;\n"
           "  .reg .f32 %f<4>;\n  .reg .f64 %fd<8>;\n"
           "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  mov.u64 %rd7, 0;\n"
           "  mov.f64 %fd1, 0d0000000000000000;\n"
           "LOOP:\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra STORE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n"
               "  cvt.f64.f32 %fd2, %f1;\n";
    } else {
        out << "  ld.global.f64 %fd2, [%rd9];\n";
    }
    out << "  add.f64 %fd1, %fd1, %fd2;\n"
           "  add.u64 %rd7, %rd7, 1;\n"
           "  bra LOOP;\nSTORE:\n"
           "  cvt.rn.f64.u64 %fd3, %rd5;\n"
           "  div.f64 %fd4, %fd1, %fd3;\n"
           "  st.global.f64 [%rd1], %fd4;\n"
           "  ret;\n}\n";
    return out.str();
}

#ifdef __APPLE__

id<MTLComputePipelineState> cached_metal_pipeline(
    id<MTLDevice> device, const std::string& key,
    const std::string& source, std::string& error) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, id<MTLComputePipelineState>> cache;
    const auto cache_key =
        std::to_string(reinterpret_cast<std::uintptr_t>((__bridge void*)device)) +
        ":" + key;
    std::lock_guard lock(cache_mutex);
    const auto found = cache.find(cache_key);
    if (found != cache.end()) return found->second;
    @autoreleasepool {
        NSString* text = [NSString stringWithUTF8String:source.c_str()];
        NSError* ns_error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:text options:nil error:&ns_error];
        if (!library) {
            error = "Metal shader compilation failed";
            if (ns_error) {
                error += ": ";
                error += [[ns_error localizedDescription] UTF8String];
            }
            return nil;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"q_kernel"];
        if (!function) {
            error = "Metal compute kernel q_kernel was not found for ";
            error += key;
            NSArray<NSString*>* names = [library functionNames];
            if (names && [names count] != 0) {
                error += " (available:";
                for (NSString* name in names) {
                    error += " ";
                    error += [name UTF8String];
                }
                error += ")";
            }
            [library release];
            return nil;
        }
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&ns_error];
        [function release];
        [library release];
        if (!pipeline) {
            error = "Metal compute pipeline creation failed";
            if (ns_error) {
                error += ": ";
                error += [[ns_error localizedDescription] UTF8String];
            }
            return nil;
        }
        cache.emplace(cache_key, pipeline);
        return pipeline;
    }
}

struct MetalPendingState {
    std::mutex mutex;
    std::unordered_map<std::uintptr_t,std::vector<id<MTLCommandBuffer>>> pending;
    std::unordered_map<std::uintptr_t,std::string> async_errors;

    ~MetalPendingState() {
        for (auto& entry : pending) {
            for (id<MTLCommandBuffer> command : entry.second) {
                [command waitUntilCompleted];
                [command release];
            }
        }
    }
};

MetalPendingState& metal_pending_state() {
    static MetalPendingState state;
    return state;
}

id<MTLCommandQueue> cached_metal_command_queue(
    id<MTLDevice> device, std::string& error) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::uintptr_t, id<MTLCommandQueue>> cache;
    const auto key=
        reinterpret_cast<std::uintptr_t>((__bridge void*)device);
    std::lock_guard lock(cache_mutex);
    if(const auto found=cache.find(key);found!=cache.end())
        return found->second;
    id<MTLCommandQueue> queue=[device newCommandQueue];
    if(!queue){
        error="Metal command queue creation failed";
        return nil;
    }
    cache.emplace(key,queue);
    return queue;
}

void reap_metal_completed_locked(MetalPendingState& state,
                                 std::uintptr_t key) {
    auto found=state.pending.find(key);
    if(found==state.pending.end()) return;
    auto& commands=found->second;
    auto out=commands.begin();
    for(auto it=commands.begin();it!=commands.end();++it){
        id<MTLCommandBuffer> command=*it;
        const auto status=[command status];
        if(status==MTLCommandBufferStatusCompleted||
           status==MTLCommandBufferStatusError){
            if(status==MTLCommandBufferStatusError&&state.async_errors[key].empty()){
                NSError* ns_error=[command error];
                state.async_errors[key]="Metal command failed";
                if(ns_error){
                    state.async_errors[key]+=": ";
                    state.async_errors[key]+=[[ns_error localizedDescription] UTF8String];
                }
            }
            [command release];
        }else{
            *out++=command;
        }
    }
    commands.erase(out,commands.end());
}

bool commit_metal_command(id<MTLDevice> device,
                          id<MTLCommandBuffer> command,
                          std::string& error) {
    if(!device||!command){
        error="Metal command creation failed";
        return false;
    }
    const auto key=
        reinterpret_cast<std::uintptr_t>((__bridge void*)device);
    auto& state=metal_pending_state();
    std::lock_guard lock(state.mutex);
    reap_metal_completed_locked(state,key);
    [command retain];
    [command commit];
    state.pending[key].push_back(command);
    return true;
}

bool synchronize_metal_device(id<MTLDevice> device,std::string& error) {
    const auto key=
        reinterpret_cast<std::uintptr_t>((__bridge void*)device);
    std::vector<id<MTLCommandBuffer>> commands;
    std::string prior_error;
    {
        auto& state=metal_pending_state();
        std::lock_guard lock(state.mutex);
        reap_metal_completed_locked(state,key);
        prior_error=state.async_errors[key];
        state.async_errors[key].clear();
        auto found=state.pending.find(key);
        if((found==state.pending.end()||found->second.empty())&&
           prior_error.empty())
            return true;
        id<MTLCommandQueue> queue=cached_metal_command_queue(device,error);
        if(!queue) return false;
        id<MTLCommandBuffer> barrier=[queue commandBuffer];
        if(!barrier){
            error="Metal synchronization command creation failed";
            return false;
        }
        [barrier retain];
        [barrier commit];
        state.pending[key].push_back(barrier);
        commands.swap(state.pending[key]);
    }
    for(id<MTLCommandBuffer> command:commands){
        [command waitUntilCompleted];
        if([command status]==MTLCommandBufferStatusError&&prior_error.empty()){
            NSError* ns_error=[command error];
            prior_error="Metal command failed";
            if(ns_error){
                prior_error+=": ";
                prior_error+=[[ns_error localizedDescription] UTF8String];
            }
        }
        [command release];
    }
    if(!prior_error.empty()){
        error=std::move(prior_error);
        return false;
    }
    return true;
}

bool synchronize_metal_backend(int backend_index,std::string& error) {
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices=MTLCopyAllDevices();
        if(backend_index<0||
           static_cast<NSUInteger>(backend_index)>=[devices count]){
            [devices release];
            error="Metal GPU index is unavailable";
            return false;
        }
        id<MTLDevice> device=[devices objectAtIndex:static_cast<NSUInteger>(backend_index)];
        const bool ok=synchronize_metal_device(device,error);
        [devices release];
        return ok;
    }
}

bool metal_copy_from_host(Buffer* raw,std::size_t offset,const void* source,
                          std::size_t bytes,std::string& error) {
    @autoreleasepool {
        id<MTLDevice> device=[raw->metal_buffer device];
        id<MTLCommandQueue> queue=cached_metal_command_queue(device,error);
        if(!queue) return false;
        id<MTLBuffer> staging=[device newBufferWithBytes:source
                                                 length:bytes
                                                options:MTLResourceStorageModeShared];
        if(!staging){
            error="Metal upload staging allocation failed";
            return false;
        }
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
        if(!command||!encoder){
            [staging release];
            error="Metal upload command creation failed";
            return false;
        }
        [encoder copyFromBuffer:staging sourceOffset:0
                       toBuffer:raw->metal_buffer destinationOffset:offset
                           size:bytes];
        [encoder endEncoding];
        const bool ok=commit_metal_command(device,command,error);
        [staging release];
        return ok;
    }
}

bool metal_copy_to_host(const Buffer* raw,std::size_t offset,void* destination,
                        std::size_t bytes,std::string& error) {
    id<MTLDevice> device=[raw->metal_buffer device];
    if(!synchronize_metal_device(device,error)) return false;
    std::memcpy(destination,
                static_cast<unsigned char*>([raw->metal_buffer contents])+offset,
                bytes);
    return true;
}

bool metal_copy_device_to_device(Buffer* destination,std::size_t destination_offset,
                                 const Buffer* source,std::size_t source_offset,
                                 std::size_t bytes,std::string& error) {
    @autoreleasepool {
        id<MTLDevice> device=[destination->metal_buffer device];
        id<MTLCommandQueue> queue=cached_metal_command_queue(device,error);
        if(!queue) return false;
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
        if(!command||!encoder){
            error="Metal device-copy command creation failed";
            return false;
        }
        [encoder copyFromBuffer:source->metal_buffer sourceOffset:source_offset
                       toBuffer:destination->metal_buffer destinationOffset:destination_offset
                           size:bytes];
        [encoder endEncoding];
        return commit_metal_command(device,command,error);
    }
}

bool metal_zero(Buffer* raw,std::size_t offset,std::size_t bytes,
                std::string& error) {
    @autoreleasepool {
        id<MTLDevice> device=[raw->metal_buffer device];
        id<MTLCommandQueue> queue=cached_metal_command_queue(device,error);
        if(!queue) return false;
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
        if(!command||!encoder){
            error="Metal memset command creation failed";
            return false;
        }
        [encoder fillBuffer:raw->metal_buffer range:NSMakeRange(offset,bytes) value:0];
        [encoder endEncoding];
        return commit_metal_command(device,command,error);
    }
}

bool dispatch_metal_1d(Buffer* output, id<MTLComputePipelineState> pipeline,
                       std::size_t count,
                       const std::function<void(id<MTLComputeCommandEncoder>)>& bind,
                       std::string& error) {
    if (count == 0) return true;
    @autoreleasepool {
        id<MTLDevice> device = [output->metal_buffer device];
        id<MTLCommandQueue> queue = cached_metal_command_queue(device,error);
        if (!queue) return false;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!command || !encoder) {
            error = "Metal compute command creation failed";
            return false;
        }
        [encoder setComputePipelineState:pipeline];
        bind(encoder);
        const auto max_threads =
            static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]);
        const auto threads = std::max<std::size_t>(
            1, std::min<std::size_t>(256, max_threads));
        [encoder dispatchThreads:MTLSizeMake(count,1,1)
           threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
        [encoder endEncoding];
        return commit_metal_command(device,command,error);
    }
}

std::string msl_raw_type(std::size_t width) {
    switch (width) {
        case 1: return "uchar";
        case 2: return "ushort";
        case 4: return "uint";
        case 8: return "ulong";
        default: return {};
    }
}

std::string msl_fill_source(std::size_t width, std::uint64_t bits) {
    const auto type = msl_raw_type(width);
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device " << type << "* out [[buffer(0)]], "
           "constant ulong& count [[buffer(1)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  out[gid] = " << type << "(" << bits << "ul);\n"
           "}\n";
    return out.str();
}

std::string msl_gather_source(std::size_t width) {
    const auto type = msl_raw_type(width);
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device " << type << "* out [[buffer(0)]], "
           "const device " << type << "* src [[buffer(1)]], "
           "const device ulong* indices [[buffer(2)]], "
           "constant ulong& count [[buffer(3)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  out[gid] = src[indices[gid]];\n"
           "}\n";
    return out.str();
}

std::string msl_float_binary_source(int operation, int scalar_side) {
    const char* op = operation == 1 ? "+" : operation == 2 ? "-" :
                     operation == 3 ? "*" : "/";
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* left [[buffer(1)]], "
           "const device float* right [[buffer(2)]], "
           "constant ulong& scalar_bits [[buffer(3)]], "
           "constant ulong& count [[buffer(4)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  float a = left[gid];\n"
           "  float s = as_type<float>(uint(scalar_bits));\n";
    if (scalar_side == 0)
        out << "  float b = right[gid];\n  out[gid] = a " << op << " b;\n";
    else if (scalar_side == 1)
        out << "  out[gid] = s " << op << " a;\n";
    else
        out << "  out[gid] = a " << op << " s;\n";
    out << "}\n";
    return out.str();
}

std::string msl_float_unary_source(int operation) {
    std::string expr;
    if (operation == 1) expr = "-value";
    else if (operation == 2) expr = "fabs(value)";
    else if (operation == 3) expr = "exp(value)";
    else expr = "log(value)";
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  float value = in[gid];\n"
           "  out[gid] = " + expr + ";\n"
           "}\n";
}

std::string msl_log_validate_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(const device float* in [[buffer(0)]], "
           "constant ulong& count [[buffer(1)]], "
           "device atomic_uint* status [[buffer(2)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  ulong i=ulong(gid); if(i>=count) return; float value=in[i];\n"
           "  if(!(value>0.0f) || !isfinite(value)) "
           "atomic_store_explicit(status,1u,memory_order_relaxed);\n"
           "}\n";
}

std::string msl_float_matmul_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* left [[buffer(1)]], "
           "const device float* right [[buffer(2)]], "
           "constant ulong& m [[buffer(3)]], "
           "constant ulong& k [[buffer(4)]], "
           "constant ulong& n [[buffer(5)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  ulong total = m * n;\n"
           "  if (ulong(gid) >= total) return;\n"
           "  ulong row = ulong(gid) / n;\n"
           "  ulong col = ulong(gid) % n;\n"
           "  float sum = 0.0f;\n"
           "  for (ulong inner = 0; inner < k; ++inner)\n"
           "    sum += left[row*k + inner] * right[inner*n + col];\n"
           "  out[gid] = sum;\n"
           "}\n";
}

#endif // __APPLE__

bool real_compute_unavailable(const Buffer* buffer, const char* operation,
                              int dtype, std::string& error) {
    const auto* info = buffer ? find(buffer->global_index) : nullptr;
    std::ostringstream out;
    out << operation << " is not supported on ";
    if (info) out << backend_name(info->backend);
    else out << "this GPU backend";
    out << " for tensor dtype " << dtype;
    error = out.str();
    return false;
}

} // namespace

bool compute_fill_ones(Buffer* output, int dtype, std::size_t count,
                       std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (!compute_buffer_range(output, 0, count, width, error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        for (std::size_t i = 0; i < count; ++i) {
            auto* slot = output->test_data.data() + i * width;
            switch (dtype) {
                case 1: { std::int64_t v=1; std::memcpy(slot,&v,8); break; }
                case 2: { std::int8_t v=1; std::memcpy(slot,&v,1); break; }
                case 3: { std::int16_t v=1; std::memcpy(slot,&v,2); break; }
                case 4: { std::int32_t v=1; std::memcpy(slot,&v,4); break; }
                case 5: { std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                case 6: { std::uint16_t v=1; std::memcpy(slot,&v,2); break; }
                case 7: { std::uint32_t v=1; std::memcpy(slot,&v,4); break; }
                case 8: { std::uint64_t v=1; std::memcpy(slot,&v,8); break; }
                case 9: { double v=1.0; std::memcpy(slot,&v,8); break; }
                case 10:{ float v=1.0F; std::memcpy(slot,&v,4); break; }
                default: error="invalid tensor dtype"; return false;
            }
        }
        return true;
    }
#endif
    std::uint64_t bits = 1;
    if (dtype == 9) {
        const double one = 1.0;
        std::memcpy(&bits, &one, sizeof(one));
    } else if (dtype == 10) {
        const float one = 1.0F;
        std::uint32_t raw{};
        std::memcpy(&raw, &one, sizeof(one));
        bits = raw;
    }
    if (output->backend == Backend::Nvidia) {
        auto ptx = ptx_fill_kernel(width, bits);
        auto out_ptr = output->cuda_pointer;
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr, &n};
        return launch_ptx_1d(
            output, "fill-one-" + std::to_string(dtype), ptx, count, args, error);
    }
    if (output->backend == Backend::Amd) {
        auto source=hip_fill_source(width,bits);
        auto out_ptr=output->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&n};
        return launch_hip_1d(
            output,"fill-one-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if (output->backend == Backend::Metal) {
        auto source = msl_fill_source(width, bits);
        auto pipeline = cached_metal_pipeline(
            [output->metal_buffer device],
            "fill-one-" + std::to_string(dtype), source, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBytes:&n length:sizeof(n) atIndex:1];
            }, error);
    }
#endif
    return real_compute_unavailable(output, "tensor.ones", dtype, error);
}

bool compute_gather(Buffer* output, const Buffer* source, int dtype,
                    const std::uint64_t* source_indices, std::size_t count,
                    std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (!source_indices && count != 0) {
        error = "missing GPU gather indices";
        return false;
    }
    if (!compute_same_device(output, source, error) ||
        !compute_buffer_range(output, 0, count, width, error)) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (source_indices[i] >
            std::numeric_limits<std::size_t>::max() / std::max<std::size_t>(width,1)) {
            error = "GPU gather source index overflow";
            return false;
        }
        const auto offset = static_cast<std::size_t>(source_indices[i]) * width;
        if (!range_ok(*source, offset, width)) {
            error = "GPU gather source index exceeds device buffer";
            return false;
        }
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(output->test_data.data() + i * width,
                        source->test_data.data() +
                            static_cast<std::size_t>(source_indices[i]) * width,
                        width);
        }
        return true;
    }
#endif
    std::unique_ptr<Buffer, void(*)(Buffer*)> indices(
        allocate(output->global_index, count * sizeof(std::uint64_t), error),
        [](Buffer* value){ quidra::device::release(value); });
    if (!indices) return false;
    if (!copy_from_host(indices.get(), 0, source_indices,
                        count * sizeof(std::uint64_t), error)) return false;
    if (output->backend == Backend::Nvidia) {
        auto ptx = ptx_gather_kernel(width);
        auto out_ptr = output->cuda_pointer;
        auto src_ptr = source->cuda_pointer;
        auto idx_ptr = indices->cuda_pointer;
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr, &src_ptr, &idx_ptr, &n};
        return launch_ptx_1d(
            output, "gather-" + std::to_string(width), ptx, count, args, error);
    }
    if (output->backend == Backend::Amd) {
        auto source_code=hip_gather_source(width);
        auto out_ptr=output->pointer;
        auto src_ptr=source->pointer;
        auto idx_ptr=indices->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&src_ptr,&idx_ptr,&n};
        return launch_hip_1d(
            output,"gather-"+std::to_string(width),source_code,count,args,error);
    }
#ifdef __APPLE__
    if (output->backend == Backend::Metal) {
        auto source_code = msl_gather_source(width);
        auto pipeline = cached_metal_pipeline(
            [output->metal_buffer device],
            "gather-" + std::to_string(width), source_code, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBuffer:source->metal_buffer offset:0 atIndex:1];
                [encoder setBuffer:indices->metal_buffer offset:0 atIndex:2];
                [encoder setBytes:&n length:sizeof(n) atIndex:3];
            }, error);
    }
#endif
    return real_compute_unavailable(output, "tensor.contiguous", dtype, error);
}


template <typename T>
bool fake_compare_all_values(
    const Buffer* left, std::size_t left_offset,
    const Buffer* right, std::size_t right_offset,
    int operation, std::size_t count, bool& result, std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto* a = left->test_data.data() + left_offset;
    const auto* b = right->test_data.data() + right_offset;
    result = true;
    for (std::size_t i = 0; i < count; ++i) {
        T av{}, bv{};
        std::memcpy(&av, a + i * sizeof(T), sizeof(T));
        std::memcpy(&bv, b + i * sizeof(T), sizeof(T));
        bool matches = false;
        switch (operation) {
            case 1: matches = av == bv; break;
            case 2: matches = av != bv; break;
            case 3: matches = av < bv; break;
            case 4: matches = av <= bv; break;
            case 5: matches = av > bv; break;
            case 6: matches = av >= bv; break;
            default:
                error = "invalid GPU tensor comparison";
                return false;
        }
        if (!matches) {
            result = false;
            return true;
        }
    }
    return true;
#else
    (void)left; (void)left_offset; (void)right; (void)right_offset;
    (void)operation; (void)count; (void)result;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

std::string hip_compare_all_source(int dtype, int operation) {
    const auto type = hip_dtype_type(dtype);
    const char* op = operation == 1 ? "==" : operation == 2 ? "!=" :
                     operation == 3 ? "<" : operation == 4 ? "<=" :
                     operation == 5 ? ">" : ">=";
    return "extern \"C\" __global__ void q_kernel(const " + type +
        "* left,const " + type +
        "* right,unsigned long long count,unsigned int* result){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count && !(left[i]" + op + "right[i])) atomicExch(result,0u);}";
}

std::string ptx_compare_all_kernel(int dtype, int operation) {
    std::ostringstream out;
    const char* integer_cmp = operation == 1 ? "eq" : operation == 2 ? "ne" :
                              operation == 3 ? "lt" : operation == 4 ? "le" :
                              operation == 5 ? "gt" : "ge";
    const char* float_cmp = operation == 1 ? "eq" : operation == 2 ? "neu" :
                            operation == 3 ? "lt" : operation == 4 ? "le" :
                            operation == 5 ? "gt" : "ge";
    if (gpu_integer_dtype(dtype)) {
        const auto width = static_cast<std::size_t>(gpu_integer_bits(dtype) / 8);
        const char* kind = gpu_integer_signed(dtype) ? "s64" : "u64";
        out << ptx_thread_prologue(
            "  .param .u64 p_left,\n  .param .u64 p_right,\n"
            "  .param .u64 p_count,\n  .param .u64 p_result\n",
            "  .reg .pred %p<4>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<32>;\n");
        out << "  ld.param.u64 %rd1, [p_left];\n"
               "  ld.param.u64 %rd2, [p_right];\n"
               "  ld.param.u64 %rd5, [p_count];\n"
               "  ld.param.u64 %rd4, [p_result];\n"
               "  setp.ge.u64 %p1, %rd7, %rd5;\n"
               "  @%p1 bra DONE;\n"
            << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
               "  add.u64 %rd9, %rd1, %rd8;\n"
               "  add.u64 %rd10, %rd2, %rd8;\n"
            << ptx_load_integer_global(dtype, "%rd9", "%rd20", "%r5")
            << ptx_load_integer_global(dtype, "%rd10", "%rd21", "%r6")
            << "  setp." << integer_cmp << "." << kind
            << " %p2, %rd20, %rd21;\n"
               "  @%p2 bra DONE;\n"
               "  mov.u32 %r10, 0;\n"
               "  atom.global.exch.b32 %r11, [%rd4], %r10;\n"
               "DONE:\n  ret;\n}\n";
        return out.str();
    }

    const bool f32 = dtype == 10;
    const auto width = f32 ? 4 : 8;
    out << ptx_thread_prologue(
        "  .param .u64 p_left,\n  .param .u64 p_right,\n"
        "  .param .u64 p_count,\n  .param .u64 p_result\n",
        f32
          ? "  .reg .pred %p<4>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<32>;\n  .reg .f32 %f<4>;\n"
          : "  .reg .pred %p<4>;\n  .reg .b32 %r<16>;\n  .reg .b64 %rd<32>;\n  .reg .f64 %fd<4>;\n");
    out << "  ld.param.u64 %rd1, [p_left];\n"
           "  ld.param.u64 %rd2, [p_right];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd4, [p_result];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd1, %rd8;\n"
           "  add.u64 %rd10, %rd2, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n"
               "  ld.global.f32 %f2, [%rd10];\n"
            << "  setp." << float_cmp << ".f32 %p2, %f1, %f2;\n";
    } else {
        out << "  ld.global.f64 %fd1, [%rd9];\n"
               "  ld.global.f64 %fd2, [%rd10];\n"
            << "  setp." << float_cmp << ".f64 %p2, %fd1, %fd2;\n";
    }
    out << "  @%p2 bra DONE;\n"
           "  mov.u32 %r10, 0;\n"
           "  atom.global.exch.b32 %r11, [%rd4], %r10;\n"
           "DONE:\n  ret;\n}\n";
    return out.str();
}

#ifdef __APPLE__
std::string msl_compare_all_source(int dtype, int operation) {
    std::string type;
    if (gpu_integer_dtype(dtype)) type = msl_integer_type(dtype);
    else if (dtype == 10) type = "float";
    else return {};
    const char* op = operation == 1 ? "==" : operation == 2 ? "!=" :
                     operation == 3 ? "<" : operation == 4 ? "<=" :
                     operation == 5 ? ">" : ">=";
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(const device " + type +
           "* left [[buffer(0)]],const device " + type +
           "* right [[buffer(1)]],constant ulong& count [[buffer(2)]],"
           "device atomic_uint* result [[buffer(3)]],"
           "uint gid [[thread_position_in_grid]]){"
           "ulong i=ulong(gid);if(i<count && !(left[i]" + op +
           "right[i])) atomic_store_explicit(result,0u,memory_order_relaxed);}\n";
}
#endif

bool compute_compare_all(
    const Buffer* left, std::size_t left_offset,
    const Buffer* right, std::size_t right_offset,
    int dtype, int operation, std::size_t count,
    bool& result, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (operation < 1 || operation > 6 || width == 0) {
        error = "invalid GPU tensor comparison";
        return false;
    }
    if (!compute_same_device(left, right, error) ||
        !compute_buffer_range(left, left_offset, count, width, error) ||
        !compute_buffer_range(right, right_offset, count, width, error)) {
        return false;
    }
    if (count == 0) {
        result = true;
        return true;
    }

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (left->backend == Backend::Test) {
        switch (dtype) {
            case 1: return fake_compare_all_values<std::int64_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 2: return fake_compare_all_values<std::int8_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 3: return fake_compare_all_values<std::int16_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 4: return fake_compare_all_values<std::int32_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 5: return fake_compare_all_values<std::uint8_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 6: return fake_compare_all_values<std::uint16_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 7: return fake_compare_all_values<std::uint32_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 8: return fake_compare_all_values<std::uint64_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 9: return fake_compare_all_values<double>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case 10:return fake_compare_all_values<float>(
                left,left_offset,right,right_offset,operation,count,result,error);
            default:
                error = "invalid GPU tensor comparison dtype";
                return false;
        }
    }
#endif

    std::unique_ptr<Buffer,void(*)(Buffer*)> flag(
        allocate(left->global_index, sizeof(std::uint32_t), error),
        [](Buffer* value){ quidra::device::release(value); });
    if (!flag) return false;
    std::uint32_t host_flag = 1;
    if (!copy_from_host(flag.get(), 0, &host_flag, sizeof(host_flag), error))
        return false;

    if (left->backend == Backend::Nvidia) {
        auto ptx = ptx_compare_all_kernel(dtype, operation);
        auto left_ptr = left->cuda_pointer + left_offset;
        auto right_ptr = right->cuda_pointer + right_offset;
        auto n = static_cast<std::uint64_t>(count);
        auto flag_ptr = flag->cuda_pointer;
        void* args[] = {&left_ptr, &right_ptr, &n, &flag_ptr};
        if (!launch_ptx_1d(
                flag.get(),
                "compare-all-" + std::to_string(dtype) + "-" +
                    std::to_string(operation),
                ptx, count, args, error)) {
            return false;
        }
    } else if (left->backend == Backend::Amd) {
        auto source = hip_compare_all_source(dtype, operation);
        auto* left_ptr = static_cast<unsigned char*>(left->pointer) + left_offset;
        auto* right_ptr = static_cast<unsigned char*>(right->pointer) + right_offset;
        auto n = static_cast<std::uint64_t>(count);
        auto* flag_ptr = flag->pointer;
        void* args[] = {&left_ptr, &right_ptr, &n, &flag_ptr};
        if (!launch_hip_1d(
                flag.get(),
                "compare-all-" + std::to_string(dtype) + "-" +
                    std::to_string(operation),
                source, count, args, error)) {
            return false;
        }
#ifdef __APPLE__
    } else if (left->backend == Backend::Metal) {
        auto source = msl_compare_all_source(dtype, operation);
        if (source.empty())
            return real_compute_unavailable(left, "tensor comparison", dtype, error);
        auto pipeline = cached_metal_pipeline(
            [flag->metal_buffer device],
            "compare-all-" + std::to_string(dtype) + "-" +
                std::to_string(operation),
            source, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        if (!dispatch_metal_1d(
                flag.get(), pipeline, count,
                [&](id<MTLComputeCommandEncoder> encoder) {
                    [encoder setBuffer:left->metal_buffer offset:left_offset atIndex:0];
                    [encoder setBuffer:right->metal_buffer offset:right_offset atIndex:1];
                    [encoder setBytes:&n length:sizeof(n) atIndex:2];
                    [encoder setBuffer:flag->metal_buffer offset:0 atIndex:3];
                }, error)) {
            return false;
        }
#endif
    } else {
        return real_compute_unavailable(left, "tensor comparison", dtype, error);
    }

    if (!copy_to_host(flag.get(), 0, &host_flag, sizeof(host_flag), error))
        return false;
    result = host_flag != 0;
    return true;
}

bool compute_binary(Buffer* output, const Buffer* left, std::size_t left_offset,
                    const Buffer* right, std::size_t right_offset,
                    const void* scalar, int scalar_side, int dtype,
                    int operation, std::size_t count, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (operation < 1 || operation > 5 ||
        scalar_side < 0 || scalar_side > 2 ||
        (scalar_side == 0 && !right) ||
        (scalar_side != 0 && !scalar)) {
        error = "invalid GPU tensor binary operation";
        return false;
    }
    if (!compute_same_device(output, left, error) ||
        !compute_buffer_range(output, 0, count, width, error) ||
        !compute_buffer_range(left, left_offset, count, width, error)) return false;
    if (scalar_side == 0) {
        if (!compute_same_device(output, right, error) ||
            !compute_buffer_range(right, right_offset, count, width, error)) return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        switch (dtype) {
            case 1: return fake_binary<std::int64_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 2: return fake_binary<std::int8_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 3: return fake_binary<std::int16_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 4: return fake_binary<std::int32_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 5: return fake_binary<std::uint8_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 6: return fake_binary<std::uint16_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 7: return fake_binary<std::uint32_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 8: return fake_binary<std::uint64_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 9: return fake_binary<double>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case 10:return fake_binary<float>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            default: error="invalid tensor dtype"; return false;
        }
    }
#endif
    if (gpu_integer_dtype(dtype) && output->backend==Backend::Nvidia) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->cuda_pointer;
        auto left_ptr=left->cuda_pointer+left_offset;
        auto right_ptr=scalar_side==0?right->cuda_pointer+right_offset:left_ptr;
        auto status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        std::uint64_t zero_scalar=0;
        void* scalar_storage=scalar_side==0?static_cast<void*>(&zero_scalar):const_cast<void*>(scalar);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n,&status_ptr};
        if(!launch_ptx_1d(output,
            "binary-int-"+std::to_string(dtype)+"-"+std::to_string(operation)+"-"+std::to_string(scalar_side),
            ptx,count,args,error))return false;
        const auto validation_error=operation==4||operation==5
            ?"invalid tensor division/remainder or integer overflow"
            :"tensor integer arithmetic overflow";
        defer_validation(status.release(),validation_error);
        return true;
    }
#ifdef __APPLE__
    if (gpu_integer_dtype(dtype) && output->backend==Backend::Metal) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=msl_integer_binary_source(dtype,operation,scalar_side);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],
            "binary-int-"+std::to_string(dtype)+"-"+std::to_string(operation)+"-"+std::to_string(scalar_side),
            source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        std::uint64_t zero_scalar=0;
        const void* scalar_storage=scalar_side==0?static_cast<const void*>(&zero_scalar):scalar;
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:left->metal_buffer offset:left_offset atIndex:1];
            [e setBuffer:(right?right->metal_buffer:left->metal_buffer)
                   offset:scalar_side==0?right_offset:left_offset atIndex:2];
            [e setBytes:scalar_storage length:compute_dtype_width(dtype) atIndex:3];
            [e setBytes:&n length:sizeof(n) atIndex:4];
            [e setBuffer:status->metal_buffer offset:0 atIndex:5];
        },error))return false;
        const auto validation_error=operation==4||operation==5
            ?"invalid tensor division/remainder or integer overflow"
            :"tensor integer arithmetic overflow";
        defer_validation(status.release(),validation_error);
        return true;
    }
#endif
    if (hip_integer_dtype(dtype) && output->backend==Backend::Amd) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=hip_integer_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->pointer;
        auto left_ptr=static_cast<unsigned char*>(left->pointer)+left_offset;
        void* right_ptr=scalar_side==0
            ? static_cast<void*>(static_cast<unsigned char*>(right->pointer)+right_offset)
            : left_ptr;
        auto status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* scalar_storage=const_cast<void*>(scalar);
        std::uint64_t zero_scalar=0;
        if(scalar_side==0)scalar_storage=&zero_scalar;
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n,&status_ptr};
        if(!launch_hip_1d(output,
            "binary-int-"+std::to_string(dtype)+"-"+std::to_string(operation)+"-"+std::to_string(scalar_side),
            source_code,count,args,error))return false;
        const auto validation_error=operation==4||operation==5
            ?"invalid tensor division/remainder or integer overflow"
            :"tensor integer arithmetic overflow";
        defer_validation(status.release(),validation_error);
        return true;
    }
    if ((dtype == 9 || dtype == 10) && operation != 5 &&
        output->backend == Backend::Nvidia) {
        auto ptx = ptx_float_binary_kernel(dtype, operation, scalar_side);
        auto out_ptr = output->cuda_pointer;
        auto left_ptr = left->cuda_pointer + left_offset;
        std::uint64_t right_ptr =
            scalar_side == 0 ? right->cuda_pointer + right_offset : 0;
        std::uint64_t scalar_bits = 0;
        if (scalar_side != 0) std::memcpy(&scalar_bits, scalar, width);
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr,&left_ptr,&right_ptr,&scalar_bits,&n};
        return launch_ptx_1d(
            output,
            "binary-" + std::to_string(dtype) + "-" +
                std::to_string(operation) + "-" + std::to_string(scalar_side),
            ptx, count, args, error);
    }
    if ((dtype==9||dtype==10) && operation!=5 &&
        output->backend==Backend::Amd) {
        auto source_code=hip_float_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->pointer;
        auto left_ptr=static_cast<unsigned char*>(left->pointer)+left_offset;
        void* right_ptr=scalar_side==0
            ? static_cast<void*>(static_cast<unsigned char*>(right->pointer)+right_offset)
            : left_ptr;
        auto n=static_cast<std::uint64_t>(count);
        void* scalar_storage=const_cast<void*>(scalar);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n};
        return launch_hip_1d(
            output,"binary-"+std::to_string(dtype)+"-"+
                std::to_string(operation)+"-"+std::to_string(scalar_side),
            source_code,count,args,error);
    }
#ifdef __APPLE__
    if (dtype == 10 && operation != 5 && output->backend == Backend::Metal) {
        auto source_code = msl_float_binary_source(operation, scalar_side);
        auto pipeline = cached_metal_pipeline(
            [output->metal_buffer device],
            "binary-f32-" + std::to_string(operation) + "-" +
                std::to_string(scalar_side), source_code, error);
        if (!pipeline) return false;
        std::uint64_t scalar_bits = 0;
        if (scalar_side != 0) std::memcpy(&scalar_bits, scalar, sizeof(float));
        const auto n = static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBuffer:left->metal_buffer offset:left_offset atIndex:1];
                [encoder setBuffer:right ? right->metal_buffer : left->metal_buffer
                         offset:scalar_side == 0 ? right_offset : left_offset atIndex:2];
                [encoder setBytes:&scalar_bits length:sizeof(scalar_bits) atIndex:3];
                [encoder setBytes:&n length:sizeof(n) atIndex:4];
            }, error);
    }
#endif
    return real_compute_unavailable(output, "tensor arithmetic", dtype, error);
}

bool compute_unary(Buffer* output, const Buffer* input, std::size_t input_offset,
                   int dtype, int operation, std::size_t count,
                   std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (operation < 1 || operation > 4 ||
        !compute_same_device(output,input,error) ||
        !compute_buffer_range(output,0,count,width,error) ||
        !compute_buffer_range(input,input_offset,count,width,error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        if (operation == 1 && gpu_integer_signed(dtype)) {
            auto run = [&](auto tag) -> bool {
                using T = decltype(tag);
                const auto* source = input->test_data.data() + input_offset;
                auto* destination = output->test_data.data();
                for (std::size_t i=0;i<count;++i) {
                    T value{};
                    std::memcpy(&value,source+i*sizeof(T),sizeof(T));
                    T result{};
                    if (!fake_sub(T{},value,result)) {
                        error = "tensor integer negation overflow";
                        return false;
                    }
                    std::memcpy(destination+i*sizeof(T),&result,sizeof(T));
                }
                return true;
            };
            switch (dtype) {
                case 1: return run(std::int64_t{});
                case 2: return run(std::int8_t{});
                case 3: return run(std::int16_t{});
                case 4: return run(std::int32_t{});
                default: break;
            }
        }
        if (dtype != 9 && dtype != 10) {
            error = "GPU neural unary operation requires a floating dtype";
            return false;
        }
        if (dtype == 10) {
            const auto* source = input->test_data.data() + input_offset;
            auto* destination = output->test_data.data();
            for (std::size_t i=0;i<count;++i) {
                float value{};
                std::memcpy(&value,source+i*4,4);
                if (operation==1) value=-value;
                else if (operation==2) value=std::fabs(value);
                else if (operation==3) value=std::exp(value);
                else {
                    if (!(value>0.0F) || !std::isfinite(value)) {
                        error="logarithm requires finite positive values";
                        return false;
                    }
                    value=std::log(value);
                }
                std::memcpy(destination+i*4,&value,4);
            }
            return true;
        }
        const auto* source = input->test_data.data() + input_offset;
        auto* destination = output->test_data.data();
        for (std::size_t i=0;i<count;++i) {
            double value{};
            std::memcpy(&value,source+i*8,8);
            if (operation==1) value=-value;
            else if (operation==2) value=std::fabs(value);
            else if (operation==3) value=std::exp(value);
            else {
                if (!(value>0.0) || !std::isfinite(value)) {
                    error="logarithm requires finite positive values";
                    return false;
                }
                value=std::log(value);
            }
            std::memcpy(destination+i*8,&value,8);
        }
        return true;
    }
#endif
    if (operation==4 && (dtype==9 || dtype==10)) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        bool launched=false;
        if(output->backend==Backend::Nvidia){
            auto ptx=ptx_log_validate_kernel(dtype);
            auto in_ptr=input->cuda_pointer+input_offset;
            auto status_ptr=status->cuda_pointer;
            auto n=static_cast<std::uint64_t>(count);
            void* args[]={&in_ptr,&n,&status_ptr};
            launched=launch_ptx_1d(
                status.get(),"log-domain-"+std::to_string(dtype),ptx,count,args,error);
        } else if(output->backend==Backend::Amd){
            auto source=hip_log_validate_source(dtype);
            auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
            auto status_ptr=status->pointer;
            auto n=static_cast<std::uint64_t>(count);
            void* args[]={&in_ptr,&n,&status_ptr};
            launched=launch_hip_1d(
                status.get(),"log-domain-"+std::to_string(dtype),source,count,args,error);
#ifdef __APPLE__
        } else if(dtype==10 && output->backend==Backend::Metal){
            auto source=msl_log_validate_source();
            auto pipeline=cached_metal_pipeline(
                [status->metal_buffer device],"log-domain-f32",source,error);
            if(pipeline){
                const auto n=static_cast<std::uint64_t>(count);
                launched=dispatch_metal_1d(status.get(),pipeline,count,
                    [&](id<MTLComputeCommandEncoder> encoder){
                        [encoder setBuffer:input->metal_buffer offset:input_offset atIndex:0];
                        [encoder setBytes:&n length:sizeof(n) atIndex:1];
                        [encoder setBuffer:status->metal_buffer offset:0 atIndex:2];
                    },error);
            }
#endif
        } else {
            launched=true;
        }
        if(!launched)return false;
        if(output->backend==Backend::Nvidia || output->backend==Backend::Amd
#ifdef __APPLE__
           || (dtype==10 && output->backend==Backend::Metal)
#endif
        ){
            defer_validation(
                status.release(),"logarithm requires finite positive values");
        }
    }
    if (gpu_integer_signed(dtype) && output->backend==Backend::Nvidia) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_unary_source(dtype);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer+input_offset;
        auto status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
        if(!launch_ptx_1d(output,"unary-int-"+std::to_string(dtype),ptx,count,args,error))return false;
        defer_validation(status.release(),"tensor integer negation overflow");
        return true;
    }
#ifdef __APPLE__
    if (gpu_integer_signed(dtype) && output->backend==Backend::Metal) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=msl_integer_unary_source(dtype);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],"unary-int-"+std::to_string(dtype),source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:input_offset atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
            [e setBuffer:status->metal_buffer offset:0 atIndex:3];
        },error))return false;
        defer_validation(status.release(),"tensor integer negation overflow");
        return true;
    }
#endif
    if (hip_signed_dtype(dtype) && output->backend==Backend::Amd) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=hip_integer_unary_source(dtype);
        auto out_ptr=output->pointer;
        auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
        auto status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
        if(!launch_hip_1d(output,
            "unary-int-"+std::to_string(dtype),source_code,count,args,error))return false;
        defer_validation(status.release(),"tensor integer negation overflow");
        return true;
    }
    if ((dtype==9 || dtype==10) &&
        output->backend==Backend::Nvidia) {
        auto ptx=ptx_float_unary_kernel(dtype,operation);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_ptx_1d(
            output,"unary-"+std::to_string(dtype)+"-"+std::to_string(operation),
            ptx,count,args,error);
    }
    if ((dtype==9||dtype==10) && output->backend==Backend::Amd) {
        auto source_code=hip_float_unary_source(dtype,operation);
        auto out_ptr=output->pointer;
        auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_hip_1d(
            output,"unary-"+std::to_string(dtype)+"-"+std::to_string(operation),
            source_code,count,args,error);
    }
#ifdef __APPLE__
    if (dtype==10 && output->backend==Backend::Metal) {
        auto source_code=msl_float_unary_source(operation);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],
            "unary-f32-"+std::to_string(operation),source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,
            [&](id<MTLComputeCommandEncoder> encoder){
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBuffer:input->metal_buffer offset:input_offset atIndex:1];
                [encoder setBytes:&n length:sizeof(n) atIndex:2];
            },error);
    }
#endif
    return real_compute_unavailable(output,"tensor unary operation",dtype,error);
}

bool compute_cast(Buffer* output, const Buffer* input, std::size_t input_offset,
                  int source_dtype, int target_dtype, std::size_t count,
                  std::string& error) {
    const auto source_width=compute_dtype_width(source_dtype);
    const auto target_width=compute_dtype_width(target_dtype);
    if(!compute_same_device(output,input,error) ||
       !compute_buffer_range(output,0,count,target_width,error) ||
       !compute_buffer_range(input,input_offset,count,source_width,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        switch(source_dtype){
            case 1:return fake_cast_from<std::int64_t>(output,input,input_offset,target_dtype,count,error);
            case 2:return fake_cast_from<std::int8_t>(output,input,input_offset,target_dtype,count,error);
            case 3:return fake_cast_from<std::int16_t>(output,input,input_offset,target_dtype,count,error);
            case 4:return fake_cast_from<std::int32_t>(output,input,input_offset,target_dtype,count,error);
            case 5:return fake_cast_from<std::uint8_t>(output,input,input_offset,target_dtype,count,error);
            case 6:return fake_cast_from<std::uint16_t>(output,input,input_offset,target_dtype,count,error);
            case 7:return fake_cast_from<std::uint32_t>(output,input,input_offset,target_dtype,count,error);
            case 8:return fake_cast_from<std::uint64_t>(output,input,input_offset,target_dtype,count,error);
            case 9:return fake_cast_from<double>(output,input,input_offset,target_dtype,count,error);
            case 10:return fake_cast_from<float>(output,input,input_offset,target_dtype,count,error);
            default:error="invalid tensor cast source dtype";return false;
        }
    }
#endif
    if(source_dtype==target_dtype)
        return copy_device_to_device(output,0,input,input_offset,count*source_width,error);
    if(gpu_integer_dtype(source_dtype) &&
       (gpu_integer_dtype(target_dtype)||target_dtype==9||target_dtype==10) &&
       output->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_cast_source(source_dtype,target_dtype);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer+input_offset;
        auto status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
        if(!launch_ptx_1d(output,
            "cast-"+std::to_string(source_dtype)+"-"+std::to_string(target_dtype),
            ptx,count,args,error))return false;
        defer_validation(
            status.release(),
            "tensor cast is unsupported or a value is outside the target range");
        return true;
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(source_dtype) &&
       (gpu_integer_dtype(target_dtype)||target_dtype==10) &&
       output->backend==Backend::Metal){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=msl_integer_cast_source(source_dtype,target_dtype);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],
            "cast-"+std::to_string(source_dtype)+"-"+std::to_string(target_dtype),
            source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:input_offset atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
            [e setBuffer:status->metal_buffer offset:0 atIndex:3];
        },error))return false;
        defer_validation(
            status.release(),
            "tensor cast is unsupported or a value is outside the target range");
        return true;
    }
#endif
    if((source_dtype==9||source_dtype==10) &&
       (target_dtype==9||target_dtype==10) &&
       output->backend==Backend::Nvidia){
        if(source_dtype==9 && target_dtype==10){
            std::unique_ptr<Buffer,void(*)(Buffer*)> status(
                acquire_validation_status(output->global_index,error),
                [](Buffer* value){quidra::device::release(value);});
            if(!status)return false;
            auto validate=ptx_float_narrow_validate_kernel();
            auto validate_in=input->cuda_pointer+input_offset;
            auto validate_status=status->cuda_pointer;
            auto validate_count=static_cast<std::uint64_t>(count);
            void* validate_args[]={&validate_in,&validate_count,&validate_status};
            if(!launch_ptx_1d(
                status.get(),"cast-range-f64-f32",validate,count,validate_args,error))return false;
            defer_validation(
                status.release(),
                "tensor cast is unsupported or a value is outside the target range");
        }
        auto ptx=ptx_float_cast_kernel(source_dtype,target_dtype);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_ptx_1d(
            output,"cast-"+std::to_string(source_dtype)+"-"+std::to_string(target_dtype),
            ptx,count,args,error);
    }
    if (output->backend==Backend::Amd &&
        hip_integer_dtype(source_dtype) &&
        (hip_integer_dtype(target_dtype)||target_dtype==9||target_dtype==10)) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=hip_cast_source(source_dtype,target_dtype);
        auto out_ptr=output->pointer;
        auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
        auto status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
        if(!launch_hip_1d(output,
            "cast-"+std::to_string(source_dtype)+"-"+std::to_string(target_dtype),
            source_code,count,args,error))return false;
        defer_validation(
            status.release(),
            "tensor cast is unsupported or a value is outside the target range");
        return true;
    }
    if (output->backend==Backend::Amd &&
        (source_dtype==9||source_dtype==10) && hip_integer_dtype(target_dtype)) {
        error="tensor cast is unsupported or a value is outside the target range";
        return false;
    }
    if ((source_dtype==9||source_dtype==10) &&
        (target_dtype==9||target_dtype==10) &&
        output->backend==Backend::Amd) {
        if(source_dtype==9 && target_dtype==10){
            std::unique_ptr<Buffer,void(*)(Buffer*)> status(
                acquire_validation_status(output->global_index,error),
                [](Buffer* value){quidra::device::release(value);});
            if(!status)return false;
            auto validate=hip_float_narrow_validate_source();
            auto validate_in=static_cast<unsigned char*>(input->pointer)+input_offset;
            auto validate_status=status->pointer;
            auto validate_count=static_cast<std::uint64_t>(count);
            void* validate_args[]={&validate_in,&validate_count,&validate_status};
            if(!launch_hip_1d(
                status.get(),"cast-range-f64-f32",validate,count,validate_args,error))return false;
            defer_validation(
                status.release(),
                "tensor cast is unsupported or a value is outside the target range");
        }
        auto source_code=hip_float_cast_source(source_dtype,target_dtype);
        auto out_ptr=output->pointer;
        auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_hip_1d(
            output,"cast-"+std::to_string(source_dtype)+"-"+std::to_string(target_dtype),
            source_code,count,args,error);
    }
    return real_compute_unavailable(output,"tensor.cast",source_dtype,error);
}

bool compute_matmul(Buffer* output, const Buffer* left, const Buffer* right,
                    int dtype, std::size_t m, std::size_t k, std::size_t n,
                    std::string& error) {
    const auto width=compute_dtype_width(dtype);
    const auto out_count=m*n;
    if(!compute_same_device(output,left,error)||!compute_same_device(output,right,error)||
       !compute_buffer_range(output,0,out_count,width,error)||
       !compute_buffer_range(left,0,m*k,width,error)||
       !compute_buffer_range(right,0,k*n,width,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        auto run=[&](auto tag)->bool{
            using T=decltype(tag);
            for(std::size_t row=0;row<m;++row){
                for(std::size_t col=0;col<n;++col){
                    T total{};
                    for(std::size_t inner=0;inner<k;++inner){
                        T a{},b{},product{},next{};
                        std::memcpy(&a,left->test_data.data()+(row*k+inner)*sizeof(T),sizeof(T));
                        std::memcpy(&b,right->test_data.data()+(inner*n+col)*sizeof(T),sizeof(T));
                        if(!fake_mul(a,b,product)||!fake_add(total,product,next)){
                            error="linear.matmul integer arithmetic overflow";
                            return false;
                        }
                        total=next;
                    }
                    std::memcpy(output->test_data.data()+(row*n+col)*sizeof(T),&total,sizeof(T));
                }
            }
            return true;
        };
        switch(dtype){
            case 1:return run(std::int64_t{});
            case 2:return run(std::int8_t{});
            case 3:return run(std::int16_t{});
            case 4:return run(std::int32_t{});
            case 5:return run(std::uint8_t{});
            case 6:return run(std::uint16_t{});
            case 7:return run(std::uint32_t{});
            case 8:return run(std::uint64_t{});
            case 9:return run(double{});
            case 10:return run(float{});
            default:error="invalid tensor dtype";return false;
        }
    }
#endif
    if(gpu_integer_dtype(dtype)&&output->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_matmul_source(dtype);
        auto out_ptr=output->cuda_pointer,left_ptr=left->cuda_pointer,right_ptr=right->cuda_pointer,
             status_ptr=status->cuda_pointer;
        auto mm=static_cast<std::uint64_t>(m),kk=static_cast<std::uint64_t>(k),
             nn=static_cast<std::uint64_t>(n);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,&mm,&kk,&nn,&status_ptr};
        if(!launch_ptx_1d(output,"matmul-int-"+std::to_string(dtype),ptx,out_count,args,error))return false;
        defer_validation(
            status.release(),"linear.matmul integer arithmetic overflow");
        return true;
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(dtype)&&output->backend==Backend::Metal){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=msl_integer_matmul_source(dtype);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],"matmul-int-"+std::to_string(dtype),source_code,error);
        if(!pipeline)return false;
        const auto mm=static_cast<std::uint64_t>(m),kk=static_cast<std::uint64_t>(k),
                   nn=static_cast<std::uint64_t>(n);
        if(!dispatch_metal_1d(output,pipeline,out_count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:left->metal_buffer offset:0 atIndex:1];
            [e setBuffer:right->metal_buffer offset:0 atIndex:2];
            [e setBytes:&mm length:8 atIndex:3];[e setBytes:&kk length:8 atIndex:4];
            [e setBytes:&nn length:8 atIndex:5];[e setBuffer:status->metal_buffer offset:0 atIndex:6];
        },error))return false;
        defer_validation(
            status.release(),"linear.matmul integer arithmetic overflow");
        return true;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_float_matmul_kernel(dtype);
        auto out_ptr=output->cuda_pointer;
        auto left_ptr=left->cuda_pointer;
        auto right_ptr=right->cuda_pointer;
        auto mm=static_cast<std::uint64_t>(m);
        auto kk=static_cast<std::uint64_t>(k);
        auto nn=static_cast<std::uint64_t>(n);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,&mm,&kk,&nn};
        return launch_ptx_1d(
            output,"matmul-"+std::to_string(dtype),ptx,out_count,args,error);
    }
    if(output->backend==Backend::Amd){
        const bool checked=hip_integer_dtype(dtype);
        auto source_code=checked
            ? hip_matmul_source(dtype,true)
            : hip_float_matmul_source(dtype);
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto out_ptr=output->pointer,left_ptr=left->pointer,right_ptr=right->pointer,
             status_ptr=status->pointer;
        auto mm=static_cast<std::uint64_t>(m),kk=static_cast<std::uint64_t>(k),
             nn=static_cast<std::uint64_t>(n);
        if(checked){
            void* args[]={&out_ptr,&left_ptr,&right_ptr,&mm,&kk,&nn,&status_ptr};
            if(!launch_hip_1d(output,"matmul-"+std::to_string(dtype),
                              source_code,out_count,args,error))return false;
        }else{
            void* args[]={&out_ptr,&left_ptr,&right_ptr,&mm,&kk,&nn};
            if(!launch_hip_1d(output,"matmul-"+std::to_string(dtype),
                              source_code,out_count,args,error))return false;
        }
        if(checked)
            defer_validation(
                status.release(),"linear.matmul integer arithmetic overflow");
        return true;
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source_code=msl_float_matmul_source();
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],"matmul-f32",source_code,error);
        if(!pipeline)return false;
        const auto mm=static_cast<std::uint64_t>(m);
        const auto kk=static_cast<std::uint64_t>(k);
        const auto nn=static_cast<std::uint64_t>(n);
        return dispatch_metal_1d(output,pipeline,out_count,
            [&](id<MTLComputeCommandEncoder> encoder){
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBuffer:left->metal_buffer offset:0 atIndex:1];
                [encoder setBuffer:right->metal_buffer offset:0 atIndex:2];
                [encoder setBytes:&mm length:sizeof(mm) atIndex:3];
                [encoder setBytes:&kk length:sizeof(kk) atIndex:4];
                [encoder setBytes:&nn length:sizeof(nn) atIndex:5];
            },error);
    }
#endif
    return real_compute_unavailable(output,"linear.matmul",dtype,error);
}

bool compute_dot(const Buffer* left, const Buffer* right, int dtype,
                 std::size_t count, void* host_result, std::string& error) {
    if(!left||!right||!host_result||left->global_index!=right->global_index||
       left->backend!=right->backend){
        error="invalid GPU linear.dot operands";
        return false;
    }
    const auto width=compute_dtype_width(dtype);
    if(!compute_buffer_range(left,0,count,width,error)||
       !compute_buffer_range(right,0,count,width,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(left->backend==Backend::Test){
        auto run=[&](auto tag)->bool{
            using T=decltype(tag);
            T total{};
            for(std::size_t i=0;i<count;++i){
                T a{},b{},product{},next{};
                std::memcpy(&a,left->test_data.data()+i*sizeof(T),sizeof(T));
                std::memcpy(&b,right->test_data.data()+i*sizeof(T),sizeof(T));
                if(!fake_mul(a,b,product)||!fake_add(total,product,next)){
                    error="linear.dot integer arithmetic overflow";
                    return false;
                }
                total=next;
            }
            std::memcpy(host_result,&total,sizeof(T));
            return true;
        };
        switch(dtype){
            case 1:return run(std::int64_t{});
            case 2:return run(std::int8_t{});
            case 3:return run(std::int16_t{});
            case 4:return run(std::int32_t{});
            case 5:return run(std::uint8_t{});
            case 6:return run(std::uint16_t{});
            case 7:return run(std::uint32_t{});
            case 8:return run(std::uint64_t{});
            case 9:return run(double{});
            case 10:return run(float{});
            default:error="invalid tensor dtype";return false;
        }
    }
#endif
    if(gpu_integer_dtype(dtype)&&left->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> result(
            allocate(left->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(left->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!result||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto ptx=ptx_integer_dot_source(dtype);
        auto out_ptr=result->cuda_pointer,left_ptr=left->cuda_pointer,right_ptr=right->cuda_pointer,
             status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,&n,&status_ptr};
        auto* module=cached_ptx_module(left->global_index,"dot-int-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},LaunchDimensions{1,1,1},args,error))return false;
        defer_validation(
            status.release(),"linear.dot integer arithmetic overflow");
        return copy_to_host(result.get(),0,host_result,width,error);
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(dtype)&&left->backend==Backend::Metal){
        std::unique_ptr<Buffer,void(*)(Buffer*)> result(
            allocate(left->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(left->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!result||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto source_code=msl_integer_dot_source(dtype);
        auto pipeline=cached_metal_pipeline(
            [result->metal_buffer device],"dot-int-"+std::to_string(dtype),source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(result.get(),pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:result->metal_buffer offset:0 atIndex:0];
            [e setBuffer:left->metal_buffer offset:0 atIndex:1];
            [e setBuffer:right->metal_buffer offset:0 atIndex:2];
            [e setBytes:&n length:8 atIndex:3];[e setBuffer:status->metal_buffer offset:0 atIndex:4];
        },error))return false;
        defer_validation(
            status.release(),"linear.dot integer arithmetic overflow");
        return copy_to_host(result.get(),0,host_result,width,error);
    }
#endif
    if((dtype==9||dtype==10)&&left->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> result(
            allocate(left->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!result)return false;
        auto ptx=ptx_float_dot_kernel(dtype);
        auto out_ptr=result->cuda_pointer;
        auto left_ptr=left->cuda_pointer;
        auto right_ptr=right->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,&n};
        auto* module=cached_ptx_module(
            left->global_index,"dot-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                   LaunchDimensions{1,1,1},args,error))return false;
        return copy_to_host(result.get(),0,host_result,width,error);
    }
#ifdef __APPLE__
    if(dtype==10&&left->backend==Backend::Metal){
        const std::string source=
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void q_kernel(device float* out [[buffer(0)]],"
            "const device float* left [[buffer(1)]],const device float* right [[buffer(2)]],"
            "constant ulong& count [[buffer(3)]],uint gid [[thread_position_in_grid]]){"
            "if(gid!=0)return;float total=0.0f;for(ulong i=0;i<count;++i)total+=left[i]*right[i];out[0]=total;}\n";
        auto pipeline=cached_metal_pipeline(
            [left->metal_buffer device],"dot-f32",source,error);
        if(!pipeline)return false;
        std::unique_ptr<Buffer,void(*)(Buffer*)> result_buffer(
            allocate(left->global_index,sizeof(float),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!result_buffer)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(result_buffer.get(),pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:result_buffer->metal_buffer offset:0 atIndex:0];
            [e setBuffer:left->metal_buffer offset:0 atIndex:1];
            [e setBuffer:right->metal_buffer offset:0 atIndex:2];
            [e setBytes:&n length:sizeof(n) atIndex:3];
        },error))return false;
        return copy_to_host(result_buffer.get(),0,host_result,sizeof(float),error);
    }
#endif
    if(left->backend==Backend::Amd){
        const bool checked=hip_integer_dtype(dtype);
        std::unique_ptr<Buffer,void(*)(Buffer*)> result(
            allocate(left->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(left->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!result||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto source_code=checked?hip_dot_source(dtype,true):hip_float_dot_source(dtype);
        auto out_ptr=result->pointer,left_ptr=left->pointer,right_ptr=right->pointer,
             status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        auto* module=cached_hip_module(
            left->global_index,"dot-"+std::to_string(dtype),source_code,error);
        if(!module)return false;
        if(checked){
            void* args[]={&out_ptr,&left_ptr,&right_ptr,&n,&status_ptr};
            if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                       LaunchDimensions{1,1,1},args,error))return false;
        }else{
            void* args[]={&out_ptr,&left_ptr,&right_ptr,&n};
            if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                       LaunchDimensions{1,1,1},args,error))return false;
        }
        if(checked)
            defer_validation(
                status.release(),"linear.dot integer arithmetic overflow");
        return copy_to_host(result.get(),0,host_result,width,error);
    }
    return real_compute_unavailable(left,"linear.dot",dtype,error);
}


bool compute_reduce(const Buffer* input, int dtype, int operation,
                    std::size_t count, void* host_result, std::string& error) {
    if (!input || !host_result || operation < 1 || operation > 3) {
        error = "invalid GPU tensor reduction";
        return false;
    }
    const auto width = compute_dtype_width(dtype);
    if (!compute_buffer_range(input, 0, count, width, error)) return false;
    if (count == 0) {
        if (operation == 1) {
            std::memset(host_result, 0, width);
            return true;
        }
        error = operation == 2
            ? "stats.min is undefined for an empty tensor"
            : "stats.max is undefined for an empty tensor";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (input->backend == Backend::Test) {
        auto run = [&](auto tag) -> bool {
            using T = decltype(tag);
            T result{};
            std::memcpy(&result, input->test_data.data(), sizeof(T));
            if (operation == 1) result = T{};
            for (std::size_t i = 0; i < count; ++i) {
                T value{};
                std::memcpy(&value, input->test_data.data() + i * sizeof(T), sizeof(T));
                if (operation == 1) {
                    T next{};
                    if (!fake_add(result, value, next)) {
                        error = "stats.sum integer arithmetic overflow";
                        return false;
                    }
                    result = next;
                } else if (operation == 2) {
                    if (value < result) result = value;
                } else {
                    if (value > result) result = value;
                }
            }
            std::memcpy(host_result, &result, sizeof(T));
            return true;
        };
        switch (dtype) {
            case 1: return run(std::int64_t{});
            case 2: return run(std::int8_t{});
            case 3: return run(std::int16_t{});
            case 4: return run(std::int32_t{});
            case 5: return run(std::uint8_t{});
            case 6: return run(std::uint16_t{});
            case 7: return run(std::uint32_t{});
            case 8: return run(std::uint64_t{});
            case 9: return run(double{});
            case 10:return run(float{});
            default:
                error = "invalid tensor reduction dtype";
                return false;
        }
    }
#endif

    if (gpu_integer_dtype(dtype) && input->backend == Backend::Nvidia) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto ptx=ptx_integer_reduce_source(dtype,operation);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer,status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
        auto* module=cached_ptx_module(
            input->global_index,"reduce-int-"+std::to_string(dtype)+"-"+std::to_string(operation),ptx,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},LaunchDimensions{1,1,1},args,error))return false;
        defer_validation(
            status.release(),"stats.sum integer arithmetic overflow");
        return copy_to_host(output.get(),0,host_result,width,error);
    }
#ifdef __APPLE__
    if (gpu_integer_dtype(dtype) && input->backend == Backend::Metal) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto source=msl_integer_reduce_source(dtype,operation);
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],
            "reduce-int-"+std::to_string(dtype)+"-"+std::to_string(operation),source,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output.get(),pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBuffer:status->metal_buffer offset:0 atIndex:3];
        },error))return false;
        defer_validation(
            status.release(),"stats.sum integer arithmetic overflow");
        return copy_to_host(output.get(),0,host_result,width,error);
    }
#endif
    if ((dtype == 9 || dtype == 10) && input->backend == Backend::Nvidia) {
        const bool f32 = dtype == 10;
        const auto element_width = f32 ? 4U : 8U;
        std::ostringstream ptx;
        ptx << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
               ".visible .entry q_kernel(\n"
               "  .param .u64 p_out,\n  .param .u64 p_in,\n"
               "  .param .u64 p_count)\n{\n"
            << (f32
              ? "  .reg .pred %p<4>;\n  .reg .b64 %rd<20>;\n  .reg .f32 %f<8>;\n"
              : "  .reg .pred %p<4>;\n  .reg .b64 %rd<20>;\n  .reg .f64 %fd<8>;\n")
            << "  ld.param.u64 %rd1, [p_out];\n"
               "  ld.param.u64 %rd2, [p_in];\n"
               "  ld.param.u64 %rd5, [p_count];\n"
               "  mov.u64 %rd7, 0;\n";
        if (f32) {
            if (operation == 1) ptx << "  mov.f32 %f1, 0f00000000;\n";
            else ptx << "  ld.global.f32 %f1, [%rd2];\n  mov.u64 %rd7, 1;\n";
        } else {
            if (operation == 1) ptx << "  mov.f64 %fd1, 0d0000000000000000;\n";
            else ptx << "  ld.global.f64 %fd1, [%rd2];\n  mov.u64 %rd7, 1;\n";
        }
        ptx << "LOOP:\n"
               "  setp.ge.u64 %p1, %rd7, %rd5;\n"
               "  @%p1 bra STORE;\n"
            << "  mul.lo.u64 %rd8, %rd7, " << element_width << ";\n"
               "  add.u64 %rd9, %rd2, %rd8;\n";
        if (f32) {
            ptx << "  ld.global.f32 %f2, [%rd9];\n";
            if (operation == 1) ptx << "  add.f32 %f1, %f1, %f2;\n";
            else if (operation == 2) ptx << "  min.f32 %f1, %f1, %f2;\n";
            else ptx << "  max.f32 %f1, %f1, %f2;\n";
        } else {
            ptx << "  ld.global.f64 %fd2, [%rd9];\n";
            if (operation == 1) ptx << "  add.f64 %fd1, %fd1, %fd2;\n";
            else {
                ptx << "  setp." << (operation == 2 ? "lt" : "gt")
                    << ".f64 %p2, %fd2, %fd1;\n"
                       "  @%p2 mov.f64 %fd1, %fd2;\n";
            }
        }
        ptx << "  add.u64 %rd7, %rd7, 1;\n"
               "  bra LOOP;\nSTORE:\n";
        if (f32) ptx << "  st.global.f32 [%rd1], %f1;\n";
        else ptx << "  st.global.f64 [%rd1], %fd1;\n";
        ptx << "  ret;\n}\n";

        std::unique_ptr<Buffer, void(*)(Buffer*)> output(
            allocate(input->global_index, width, error),
            [](Buffer* value){ quidra::device::release(value); });
        if (!output) return false;
        auto out_ptr = output->cuda_pointer;
        auto in_ptr = input->cuda_pointer;
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr, &in_ptr, &n};
        auto* module = cached_ptx_module(
            input->global_index,
            "reduce-" + std::to_string(dtype) + "-" + std::to_string(operation),
            ptx.str(), error);
        if (!module) return false;
        if (!launch(module, "q_kernel", LaunchDimensions{1,1,1},
                    LaunchDimensions{1,1,1}, args, error)) return false;
        return copy_to_host(output.get(), 0, host_result, width, error);
    }

    if (input->backend==Backend::Amd) {
        const bool checked=hip_integer_dtype(dtype);
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,width,error),
            [](Buffer* value){quidra::device::release(value);});
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output||!status||!zero(status.get(),0,sizeof(std::uint32_t),error))return false;
        auto source_code=checked
            ?hip_reduce_source(dtype,operation,true)
            :hip_float_reduce_source(dtype,operation);
        auto out_ptr=output->pointer,in_ptr=input->pointer,status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        auto* module=cached_hip_module(
            input->global_index,
            "reduce-"+std::to_string(dtype)+"-"+std::to_string(operation),
            source_code,error);
        if(!module)return false;
        if(checked){
            void* args[]={&out_ptr,&in_ptr,&n,&status_ptr};
            if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                       LaunchDimensions{1,1,1},args,error))return false;
        }else{
            void* args[]={&out_ptr,&in_ptr,&n};
            if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                       LaunchDimensions{1,1,1},args,error))return false;
        }
        if(checked)
            defer_validation(
                status.release(),"stats.sum integer arithmetic overflow");
        return copy_to_host(output.get(),0,host_result,width,error);
    }
#ifdef __APPLE__
    if (dtype == 10 && input->backend == Backend::Metal) {
        const char* init = operation == 1 ? "0.0f" : "in[0]";
        const char* start = operation == 1 ? "0" : "1";
        const char* update = operation == 1
            ? "result += value;"
            : operation == 2
                ? "if (value < result) result = value;"
                : "if (value > result) result = value;";
        std::string source =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void q_kernel(device float* out [[buffer(0)]], "
            "const device float* in [[buffer(1)]], "
            "constant ulong& count [[buffer(2)]], "
            "uint gid [[thread_position_in_grid]]) {\n"
            "  if (gid != 0) return;\n"
            "  float result = " + std::string(init) + ";\n"
            "  for (ulong i = " + std::string(start) + "; i < count; ++i) {\n"
            "    float value = in[i]; " + std::string(update) + "\n"
            "  }\n"
            "  out[0] = result;\n"
            "}\n";
        auto pipeline = cached_metal_pipeline(
            [input->metal_buffer device],
            "reduce-f32-" + std::to_string(operation), source, error);
        if (!pipeline) return false;
        std::unique_ptr<Buffer, void(*)(Buffer*)> output(
            allocate(input->global_index, sizeof(float), error),
            [](Buffer* value){ quidra::device::release(value); });
        if (!output) return false;
        const auto n = static_cast<std::uint64_t>(count);
        if (!dispatch_metal_1d(output.get(), pipeline, 1,
            [&](id<MTLComputeCommandEncoder> encoder) {
                [encoder setBuffer:output->metal_buffer offset:0 atIndex:0];
                [encoder setBuffer:input->metal_buffer offset:0 atIndex:1];
                [encoder setBytes:&n length:sizeof(n) atIndex:2];
            }, error)) return false;
        return copy_to_host(output.get(), 0, host_result, sizeof(float), error);
    }
#endif
    return real_compute_unavailable(input, "stats reduction", dtype, error);
}

bool compute_mean(const Buffer* input, int dtype, std::size_t count,
                  double& result, std::string& error) {
    if(!input||count==0){
        error="stats.mean is undefined for an empty tensor";
        return false;
    }
    const auto width=compute_dtype_width(dtype);
    if(!compute_buffer_range(input,0,count,width,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(input->backend==Backend::Test){
        long double total=0.0L;
        auto run=[&](auto tag){
            using T=decltype(tag);
            for(std::size_t i=0;i<count;++i){
                T value{};
                std::memcpy(&value,input->test_data.data()+i*sizeof(T),sizeof(T));
                total+=static_cast<long double>(value);
            }
        };
        switch(dtype){
            case 1:run(std::int64_t{});break;
            case 2:run(std::int8_t{});break;
            case 3:run(std::int16_t{});break;
            case 4:run(std::int32_t{});break;
            case 5:run(std::uint8_t{});break;
            case 6:run(std::uint16_t{});break;
            case 7:run(std::uint32_t{});break;
            case 8:run(std::uint64_t{});break;
            case 9:run(double{});break;
            case 10:run(float{});break;
            default:error="invalid tensor dtype";return false;
        }
        result=static_cast<double>(total/static_cast<long double>(count));
        return true;
    }
#endif
    if(gpu_integer_dtype(dtype)&&input->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,sizeof(double),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output)return false;
        auto ptx=ptx_integer_mean_source(dtype);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_ptx_module(
            input->global_index,"mean-int-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},LaunchDimensions{1,1,1},args,error))return false;
        return copy_to_host(output.get(),0,&result,sizeof(result),error);
    }
    if((dtype==9||dtype==10)&&input->backend==Backend::Nvidia){
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,sizeof(double),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output)return false;
        auto ptx=ptx_float_mean_kernel(dtype);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_ptx_module(
            input->global_index,"mean-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                   LaunchDimensions{1,1,1},args,error))return false;
        return copy_to_host(output.get(),0,&result,sizeof(result),error);
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(dtype)&&input->backend==Backend::Metal){
        if(count>static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())){
            error="stats.mean tensor is too large for Metal integer reduction";
            return false;
        }
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,2*sizeof(std::uint64_t),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output)return false;
        auto source_code=msl_integer_mean_source(dtype);
        auto pipeline=cached_metal_pipeline(
            [input->metal_buffer device],
            "mean-int-"+std::to_string(dtype),source_code,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output.get(),pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error))return false;
        std::array<std::uint64_t,2> parts{};
        if(!copy_to_host(output.get(),0,parts.data(),sizeof(parts),error))return false;
        if(gpu_integer_signed(dtype)){
            std::int64_t quotient{},remainder{};
            std::memcpy(&quotient,&parts[0],sizeof(quotient));
            std::memcpy(&remainder,&parts[1],sizeof(remainder));
            const long double exact=
                static_cast<long double>(quotient)+
                static_cast<long double>(remainder)/static_cast<long double>(count);
            result=static_cast<double>(exact);
        }else{
            const long double exact=
                static_cast<long double>(parts[0])+
                static_cast<long double>(parts[1])/static_cast<long double>(count);
            result=static_cast<double>(exact);
        }
        return true;
    }
    if(dtype==10&&input->backend==Backend::Metal){
        const std::string source=
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void q_kernel(device float* out [[buffer(0)]],const device float* in [[buffer(1)]],"
            "constant ulong& count [[buffer(2)]],uint gid [[thread_position_in_grid]]){"
            "if(gid!=0)return;float total=0.0f;for(ulong i=0;i<count;++i)total+=in[i];out[0]=total/float(count);}\n";
        auto pipeline=cached_metal_pipeline(
            [input->metal_buffer device],"mean-f32",source,error);
        if(!pipeline)return false;
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,sizeof(float),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output.get(),pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error))return false;
        float value=0.0F;
        if(!copy_to_host(output.get(),0,&value,sizeof(value),error))return false;
        result=static_cast<double>(value);
        return true;
    }
#endif
    if(input->backend==Backend::Amd){
        std::unique_ptr<Buffer,void(*)(Buffer*)> output(
            allocate(input->global_index,sizeof(double),error),
            [](Buffer* value){quidra::device::release(value);});
        if(!output)return false;
        auto source_code=hip_mean_source(dtype);
        auto out_ptr=output->pointer;
        auto in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_hip_module(
            input->global_index,"mean-"+std::to_string(dtype),source_code,error);
        if(!module)return false;
        if(!launch(module,"q_kernel",LaunchDimensions{1,1,1},
                   LaunchDimensions{1,1,1},args,error))return false;
        return copy_to_host(output.get(),0,&result,sizeof(result),error);
    }
    return real_compute_unavailable(input,"stats.mean",dtype,error);
}
