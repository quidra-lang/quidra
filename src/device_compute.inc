
// Tensor compute primitives live here so Buffer internals stay private to
// device_backend.cpp while runtime.cpp only depends on the stable device API.

namespace {

#include "device_soft_float.inc"

inline bool half_dtype(int dtype) {
    return dtype == QCORE_DTYPE_FLOAT16 || dtype == QCORE_DTYPE_BFLOAT16;
}

inline bool metal_real_dtype(int dtype) {
    return dtype == QCORE_DTYPE_FLOAT32 || half_dtype(dtype);
}

thread_local abi::ConversionReason numeric_cast_reason=abi::ConversionReason::out_of_range;

std::string tensor_cast_failure_message(int dtype,abi::ConversionReason reason){
    return abi::format(abi::conversion_failure_reason(reason),
        abi::FailureArgs{.type=abi::conversion_type_name(dtype),.subject=abi::ConversionSubject::tensor_element});
}

void defer_cast_status(Buffer* status,int dtype){
    defer_validation(status,tensor_cast_failure_message(dtype,abi::ConversionReason::out_of_range),
        [dtype](std::uint32_t bits){
            numeric_cast_reason=(bits&2u)?abi::ConversionReason::not_integral:abi::ConversionReason::out_of_range;
            return tensor_cast_failure_message(dtype,numeric_cast_reason);
        });
}

// The message of a tensor cast whose value is outside the range of
// `target_dtype` (NUMERIC_CONVERSION's text for a tensor element).
std::string tensor_cast_range_message(int target_dtype) {
    return abi::format(abi::FailureReason::conversion_out_of_range,
                       abi::FailureArgs{.type = abi::conversion_type_name(target_dtype),
                                        .subject = abi::ConversionSubject::tensor_element});
}

// The status bits a real tensor kernel ORs into its status word for a
// failing element: no NaN or infinity enters a tensor
// (docs/spec/numeric-and-bin.md). A kernel computes every element, so
// several bits may be set; the message names the first in the order below.
namespace real_status {
constexpr std::uint32_t zero_power_zero = 1u;
constexpr std::uint32_t zero_power_negative = 2u;
constexpr std::uint32_t negative_base = 4u;
constexpr std::uint32_t zero_divisor = 8u;
constexpr std::uint32_t overflow = 16u;
} // namespace real_status

// The message of a failed real tensor operation on `dtype` from its status
// bits: a power without a value, a division by zero, an overflow.
std::string real_status_message(int dtype, int operation, std::uint32_t bits) {
    using abi::FailureReason;
    if (bits & real_status::zero_power_zero)
        return std::string(abi::failure_reason_info(FailureReason::zero_power_zero).message);
    if (bits & real_status::zero_power_negative)
        return std::string(abi::failure_reason_info(FailureReason::zero_power_negative).message);
    if (bits & real_status::negative_base)
        return std::string(
            abi::failure_reason_info(FailureReason::negative_base_fractional_exponent).message);
    const auto type = "tensor<" + std::string(abi::conversion_type_name(dtype)) + ">";
    if (bits & real_status::zero_divisor)
        return abi::format(FailureReason::real_zero_divisor, abi::FailureArgs{.type = type});
    const auto name = operation == abi::tensor_binary_opcode::add ? "addition"
        : operation == abi::tensor_binary_opcode::subtract ? "subtraction"
        : operation == abi::tensor_binary_opcode::multiply ? "multiplication"
        : operation == abi::tensor_binary_opcode::divide ? "division"
        : operation == abi::tensor_binary_opcode::power ? "power" : "gradient";
    return abi::format(FailureReason::real_overflow,
                       abi::FailureArgs{.type = type, .operation = name});
}

// Defers the status word of a real tensor kernel of `operation` on `dtype`:
// a nonzero word fails at the next synchronization point with the message
// of its bits.
void defer_real_status(Buffer* status, int dtype, int operation) {
    defer_validation(status, real_status_message(dtype, operation, real_status::overflow),
                     [dtype, operation](std::uint32_t bits) {
                         return real_status_message(dtype, operation, bits);
                     });
}

// The status bits of one element of a real operation (the fake backend's
// kernels and the host reference of the device kernels): `left` and `right`
// are the operands (a power's base and exponent), `result` the result.
template <typename T>
std::uint32_t real_element_status(T left, T right, int operation, T result) {
    if (operation == abi::tensor_binary_opcode::power) {
        if (left == T{0} && right == T{0}) return real_status::zero_power_zero;
        if (left == T{0} && right < T{0}) return real_status::zero_power_negative;
        if (left < T{0} && std::trunc(static_cast<double>(right)) != static_cast<double>(right)) return real_status::negative_base;
    }
    if (std::isfinite(static_cast<double>(result))) return 0;
    return operation == abi::tensor_binary_opcode::divide && right == T{0}
        ? real_status::zero_divisor : real_status::overflow;
}

// The element width of a dtype the compute kernels take, or 0 for one they do
// not take: bool (no compute kernel has a bool operand) and unknown codes.
// A width of 0 makes compute_buffer_range, and the callers that test the
// width themselves, reject the operation.
std::size_t compute_dtype_width(int dtype) {
    switch (dtype) {
        case QCORE_DTYPE_INT64: case QCORE_DTYPE_UINT64: case QCORE_DTYPE_FLOAT64: return 8;
        case QCORE_DTYPE_INT8: case QCORE_DTYPE_UINT8: return 1;
        case QCORE_DTYPE_INT16: case QCORE_DTYPE_UINT16:
        case QCORE_DTYPE_FLOAT16: case QCORE_DTYPE_BFLOAT16: return 2;
        case QCORE_DTYPE_INT32: case QCORE_DTYPE_UINT32: case QCORE_DTYPE_FLOAT32:
        case QCORE_DTYPE_COMPLEX_FLOAT16: case QCORE_DTYPE_COMPLEX_BFLOAT16: return 4;
        case QCORE_DTYPE_COMPLEX_FLOAT32: return 8;
        case QCORE_DTYPE_COMPLEX_FLOAT64: return 16;
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
    if constexpr (!std::is_integral_v<T>) {
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
    if constexpr (!std::is_integral_v<T>) {
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
    if constexpr (!std::is_integral_v<T>) {
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
        case abi::tensor_binary_opcode::add: return fake_add(a, b, out);
        case abi::tensor_binary_opcode::subtract: return fake_sub(a, b, out);
        case abi::tensor_binary_opcode::multiply: return fake_mul(a, b, out);
        case abi::tensor_binary_opcode::divide:
            if constexpr (std::is_integral_v<T>) {
                if (b == 0) return false;
                if constexpr (std::is_signed_v<T>) {
                    if (a == std::numeric_limits<T>::min() && b == -1) return false;
                }
            }
            out = static_cast<T>(a / b);
            return true;
        case abi::tensor_binary_opcode::remainder:
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
        case abi::tensor_binary_opcode::power:
            if constexpr (std::is_integral_v<T>) {
                if constexpr (std::is_signed_v<T>) {
                    if (b < 0) return false;
                }
                out = T{1};
                T factor = a;
                auto exponent = static_cast<std::uint64_t>(b);
                while (exponent != 0) {
                    if ((exponent & 1U) != 0) {
                        T next{};
                        if (!fake_mul(out, factor, next)) return false;
                        out = next;
                    }
                    exponent >>= 1U;
                    if (exponent != 0) {
                        T next{};
                        if (!fake_mul(factor, factor, next)) return false;
                        factor = next;
                    }
                }
                return true;
            } else {
                if constexpr (std::is_same_v<T,abi::half::Real16> || std::is_same_v<T,abi::half::Real16b>)
                    out=T(std::pow(static_cast<float>(a),static_cast<float>(b)));
                else out=static_cast<T>(std::pow(a,b));
                return true;
            }
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
    if (scalar_side != abi::scalar_side::none) {
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
        if (scalar_side == abi::scalar_side::none) {
            std::memcpy(&bv, b + i * sizeof(T), sizeof(T));
        }
        T lhs = scalar_side == abi::scalar_side::left ? scalar_value : av;
        T rhs = scalar_side == abi::scalar_side::right ? scalar_value : (scalar_side == abi::scalar_side::left ? av : bv);
        T result{};
        if constexpr (std::is_integral_v<T>) {
            if (operation == abi::tensor_binary_opcode::power && lhs == T{0} && rhs == T{0}) {
                error = std::string(abi::failure_reason_info(abi::FailureReason::zero_power_zero).message);
                return false;
            }
        }
        if (!fake_binary_value(lhs, rhs, operation, result)) {
            error = operation == abi::tensor_binary_opcode::divide || operation == abi::tensor_binary_opcode::remainder
                ? "invalid tensor division/remainder or integer overflow"
                : operation == abi::tensor_binary_opcode::power
                    ? "invalid tensor power domain or integer overflow"
                    : "tensor integer arithmetic overflow";
            return false;
        }
        if constexpr (!std::is_integral_v<T>) {
            if (const auto bits = real_element_status(lhs, rhs, operation, result)) {
                error = real_status_message(std::is_same_v<T, float> ? QCORE_DTYPE_FLOAT32
                    : std::is_same_v<T, double> ? QCORE_DTYPE_FLOAT64
                    : std::is_same_v<T, abi::half::Real16> ? QCORE_DTYPE_FLOAT16 : QCORE_DTYPE_BFLOAT16,
                                            operation, bits);
                return false;
            }
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

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
template <typename P>
bool fake_complex_binary(Buffer* output,const Buffer* left,std::size_t left_offset,
                         const Buffer* right,std::size_t right_offset,const void* scalar,
                         int scalar_side,int operation,std::size_t count,std::string& error) {
    using C=abi::complex::ComplexElement<P>;
    if(operation>abi::tensor_binary_opcode::divide){error="unsupported complex tensor operation";return false;}
    C scalar_value{};
    if(scalar_side!=abi::scalar_side::none)std::memcpy(&scalar_value,scalar,sizeof(C));
    for(std::size_t i=0;i<count;++i){
        C a{},b{},z{};
        std::memcpy(&a,left->test_data.data()+left_offset+i*sizeof(C),sizeof(C));
        if(scalar_side==abi::scalar_side::none)
            std::memcpy(&b,right->test_data.data()+right_offset+i*sizeof(C),sizeof(C));
        else if(scalar_side==abi::scalar_side::left){b=a;a=scalar_value;}
        else b=scalar_value;
        if(operation==abi::tensor_binary_opcode::add)z=a+b;
        else if(operation==abi::tensor_binary_opcode::subtract)z=a-b;
        else if(operation==abi::tensor_binary_opcode::multiply)z=a*b;
        else z=a/b;
        if(!abi::complex::part_is_finite(z.re)||!abi::complex::part_is_finite(z.im)){
            const int dtype=std::is_same_v<P,float>?QCORE_DTYPE_COMPLEX_FLOAT32:
                std::is_same_v<P,double>?QCORE_DTYPE_COMPLEX_FLOAT64:
                std::is_same_v<P,abi::half::Real16>?QCORE_DTYPE_COMPLEX_FLOAT16:QCORE_DTYPE_COMPLEX_BFLOAT16;
            const auto bits=operation==abi::tensor_binary_opcode::divide && b==C{}?
                real_status::zero_divisor:real_status::overflow;
            error=real_status_message(dtype,operation,bits);return false;
        }
        std::memcpy(output->test_data.data()+i*sizeof(C),&z,sizeof(C));
    }
    return true;
}
#endif

template <typename Src, typename Dst>
bool fake_numeric_cast(Src source,Dst& value,int target_dtype,std::string& error) {
    if constexpr(std::is_integral_v<Dst>&&(std::is_floating_point_v<Src>||
        std::is_same_v<Src,abi::half::Real16>||std::is_same_v<Src,abi::half::Real16b>)){
        const double v=static_cast<double>(source);
        if(!std::isfinite(v)){error=tensor_cast_range_message(target_dtype);return false;}
        if(std::trunc(v)!=v){numeric_cast_reason=abi::ConversionReason::not_integral;
            error=tensor_cast_failure_message(target_dtype,numeric_cast_reason);return false;}
        const double upper=std::ldexp(1.0,std::numeric_limits<Dst>::digits);
        if(!std::isfinite(v)||v<(std::is_signed_v<Dst>?-upper:0.0)||v>=upper){
            error=tensor_cast_range_message(target_dtype);return false;}
        value=static_cast<Dst>(v);
    }else if constexpr (abi::complex::complex_element<Dst>) {
        using P=decltype(Dst{}.re);
        P re{},im{};
        if constexpr (abi::complex::complex_element<Src>) {
            if(!fake_numeric_cast(source.re,re,target_dtype,error)||
               !fake_numeric_cast(source.im,im,target_dtype,error))return false;
        }else if(!fake_numeric_cast(source,re,target_dtype,error))return false;
        value=Dst{re,im};
    }else if constexpr (abi::complex::complex_element<Src>) {
        error="tensor cast from a complex to a real dtype is unsupported";return false;
    }else if constexpr (std::is_same_v<Src,abi::half::Real16> || std::is_same_v<Src,abi::half::Real16b> ||
                      std::is_same_v<Dst,abi::half::Real16> || std::is_same_v<Dst,abi::half::Real16b>) {
            if constexpr (std::is_integral_v<Dst>) {
                error="tensor cast from a real to an integer dtype is unsupported";
                return false;
            } else if constexpr(std::is_integral_v<Src>) {
                const bool negative=std::is_signed_v<Src>&&(source<0);
                const auto raw=static_cast<std::uint64_t>(source);
                const auto magnitude=negative?std::uint64_t{0}-raw:raw;
                const auto format=std::is_same_v<Dst,abi::half::Real16>?abi::half::binary16:abi::half::bfloat16;
                value=Dst::from_bits(abi::half::round_integer(magnitude,negative,format));
                if(!std::isfinite(static_cast<double>(value))){error=tensor_cast_range_message(target_dtype);return false;}
            } else {
                value=static_cast<Dst>(static_cast<double>(source));
                if(!std::isfinite(static_cast<double>(value))){error=tensor_cast_range_message(target_dtype);return false;}
            }
        } else if constexpr (std::is_integral_v<Src> && std::is_integral_v<Dst>) {
            if (!std::in_range<Dst>(source)) {
                error = tensor_cast_range_message(target_dtype);
                return false;
            }
            value = static_cast<Dst>(source);
        } else if constexpr (std::is_integral_v<Src> && std::is_floating_point_v<Dst>) {
            value = static_cast<Dst>(source);
            if (!std::isfinite(value)) {
                error = tensor_cast_range_message(target_dtype);
                return false;
            }
        } else if constexpr (std::is_floating_point_v<Src> && std::is_integral_v<Dst>) {
            error = "tensor cast from a real to an integer dtype is unsupported";
            return false;
        } else {
            value = static_cast<Dst>(source);
            if constexpr (std::is_floating_point_v<Src> &&
                          std::is_floating_point_v<Dst>) {
                if (std::isfinite(source) && !std::isfinite(value)) {
                    error = tensor_cast_range_message(target_dtype);
                    return false;
                }
            }
        }
    return true;
}

template <typename Src, typename Dst>
bool fake_cast_values(Buffer* output, const Buffer* input,
                      std::size_t input_offset, int target_dtype, std::size_t count,
                      std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto* src = input->test_data.data() + input_offset;
    auto* dst = output->test_data.data();
    for (std::size_t i = 0; i < count; ++i) {
        Src source{};
        std::memcpy(&source, src + i * sizeof(Src), sizeof(Src));
        Dst value{};
        if(!fake_numeric_cast(source,value,target_dtype,error))return false;
        std::memcpy(dst + i * sizeof(Dst), &value, sizeof(Dst));
    }
    return true;
#else
    (void)output; (void)input; (void)input_offset; (void)target_dtype; (void)count;
    error = "test GPU backend is unavailable";
    return false;
#endif
}

template <typename Src>
bool fake_cast_from(Buffer* output, const Buffer* input,
                    std::size_t input_offset, int target_dtype,
                    std::size_t count, std::string& error) {
    switch (target_dtype) {
        case QCORE_DTYPE_INT64: return fake_cast_values<Src, std::int64_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_INT8: return fake_cast_values<Src, std::int8_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_INT16: return fake_cast_values<Src, std::int16_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_INT32: return fake_cast_values<Src, std::int32_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_UINT8: return fake_cast_values<Src, std::uint8_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_UINT16: return fake_cast_values<Src, std::uint16_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_UINT32: return fake_cast_values<Src, std::uint32_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_UINT64: return fake_cast_values<Src, std::uint64_t>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_FLOAT64: return fake_cast_values<Src, double>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_FLOAT32: return fake_cast_values<Src, float>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_FLOAT16: return fake_cast_values<Src,abi::half::Real16>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_BFLOAT16: return fake_cast_values<Src,abi::half::Real16b>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_COMPLEX_FLOAT16: return fake_cast_values<Src,abi::complex::ComplexElement<abi::half::Real16>>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_COMPLEX_BFLOAT16: return fake_cast_values<Src,abi::complex::ComplexElement<abi::half::Real16b>>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_COMPLEX_FLOAT32: return fake_cast_values<Src,abi::complex::ComplexElement<float>>(output,input,input_offset,target_dtype,count,error);
        case QCORE_DTYPE_COMPLEX_FLOAT64: return fake_cast_values<Src,abi::complex::ComplexElement<double>>(output,input,input_offset,target_dtype,count,error);
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
    if (!launch(module, "q_kernel",
                LaunchDimensions{static_cast<unsigned>(blocks64),1,1},
                LaunchDimensions{block,1,1}, arguments, error))
        return false;
    counters::add(counters::Id::CoreDispatches); // qcount
    return true;
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
    if (!launch(module, "q_kernel",
                LaunchDimensions{static_cast<unsigned>(blocks64),1,1},
                LaunchDimensions{block,1,1}, arguments, error))
        return false;
    counters::add(counters::Id::CoreDispatches); // qcount
    return true;
}

#include "device_numeric_kernel.inc"
#include "device_numeric_ptx.inc"

bool additional_dtype(int dtype) {
    return half_dtype(dtype) || abi::complex_dtype_code(dtype);
}

bool numeric_gpu_backend(const Buffer* buffer) {
    return buffer && (buffer->backend == Backend::Cuda || buffer->backend == Backend::Hip);
}

struct NumericLaunch {
    Buffer* other = nullptr;
    const Buffer* left = nullptr;
    std::size_t left_offset = 0;
    const Buffer* right = nullptr;
    std::size_t right_offset = 0;
    const Buffer* gradient = nullptr;
    const Buffer* offsets = nullptr;
    const Buffer* positions = nullptr;
    const void* scalar = nullptr;
    std::size_t scalar_width = 0;
    std::size_t count = 0;
    int dtype = 0;
    int operation = 0;
    int side = 0;
    int mode = 0;
    int source_dtype = 0;
    int target_dtype = 0;
    Buffer* status = nullptr;
};

bool launch_numeric(Buffer* output, const NumericLaunch& request, std::string& error) {
    const auto pointer = [&](const Buffer* buffer, std::size_t offset = 0) -> std::uint64_t {
        if (!buffer) return 0;
        return output->backend == Backend::Cuda ? buffer->cuda_pointer + offset
            : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer->pointer)) + offset;
    };
    auto out=pointer(output), other=pointer(request.other), left=pointer(request.left, request.left_offset),
         right=pointer(request.right, request.right_offset), gradient=pointer(request.gradient),
         offsets=pointer(request.offsets), positions=pointer(request.positions), status=pointer(request.status);
    std::uint64_t scalar[2]{};
    if (request.scalar) std::memcpy(scalar, request.scalar, request.scalar_width);
    auto count=static_cast<std::uint64_t>(request.count);
    auto dtype=request.dtype, operation=request.operation, side=request.side, mode=request.mode,
         source=request.source_dtype, target=request.target_dtype;
    void* args[]={&out,&other,&left,&right,&gradient,&offsets,&positions,&scalar[0],&scalar[1],
                  &count,&dtype,&operation,&side,&mode,&source,&target,&status};
    if (output->backend == Backend::Cuda)
        return launch_ptx_1d(output, "additional-numeric", numeric_kernel_ptx(), request.count, args, error);
    return launch_hip_1d(output, "additional-numeric", hip_numeric_source(), request.count, args, error);
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
        case QCORE_DTYPE_INT64: return "long long";
        case QCORE_DTYPE_INT8: return "signed char";
        case QCORE_DTYPE_INT16: return "short";
        case QCORE_DTYPE_INT32: return "int";
        case QCORE_DTYPE_UINT8: return "unsigned char";
        case QCORE_DTYPE_UINT16: return "unsigned short";
        case QCORE_DTYPE_UINT32: return "unsigned int";
        case QCORE_DTYPE_UINT64: return "unsigned long long";
        case QCORE_DTYPE_FLOAT64: return "double";
        case QCORE_DTYPE_FLOAT32: return "float";
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

std::string hip_gather_backward_source(int dtype) {
    const auto type=hip_dtype_type(dtype);
    return "extern \"C\" __global__ void q_kernel("+type+
           "* out,const "+type+"* grad,const unsigned long long* offsets,"
           "const unsigned long long* positions,unsigned long long count){"
           "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
           "if(i>=count)return;"+type+" total=("+type+")0;"
           "for(unsigned long long p=offsets[i];p<offsets[i+1];++p)"
           "total=("+type+")(total+grad[positions[p]]);out[i]=total;}";
}

std::string hip_integer_gather_backward_source(int dtype) {
    const auto type=hip_dtype_type(dtype);
    return "extern \"C\" __global__ void q_kernel("+type+
           "* out,const "+type+"* grad,const unsigned long long* offsets,"
           "const unsigned long long* positions,unsigned long long count,unsigned int* status){"
           "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
           "if(i>=count)return;"+type+" total=("+type+")0;"
           "for(unsigned long long p=offsets[i];p<offsets[i+1];++p){"+type+
           " next=("+type+")0;if(__builtin_add_overflow(total,grad[positions[p]],&next)){"
           "atomicExch(status,1u);return;}total=next;}out[i]=total;}";
}

bool hip_integer_dtype(int dtype) {
    return dtype>=QCORE_DTYPE_INT64 && dtype<=QCORE_DTYPE_UINT64;
}
bool hip_signed_dtype(int dtype) {
    return dtype>=QCORE_DTYPE_INT64 && dtype<=QCORE_DTYPE_INT32;
}
std::string hip_signed_min(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: return "(-9223372036854775807ll-1ll)";
        case QCORE_DTYPE_INT8: return "(-127-1)";
        case QCORE_DTYPE_INT16: return "(-32767-1)";
        case QCORE_DTYPE_INT32: return "(-2147483647-1)";
        default:return "0";
    }
}
std::string hip_integral_max(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: return "9223372036854775807ll";
        case QCORE_DTYPE_INT8: return "127";
        case QCORE_DTYPE_INT16: return "32767";
        case QCORE_DTYPE_INT32: return "2147483647";
        case QCORE_DTYPE_UINT8: return "255u";
        case QCORE_DTYPE_UINT16: return "65535u";
        case QCORE_DTYPE_UINT32: return "4294967295u";
        case QCORE_DTYPE_UINT64: return "18446744073709551615ull";
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
    if(scalar_side==abi::scalar_side::none) out<<"right[i];";
    else out<<"scalar;";
    if(scalar_side==abi::scalar_side::left) out<<type<<" tmp=a;a=b;b=tmp;";
    out<<type<<" r=0;bool bad=false;";
    if(operation==abi::tensor_binary_opcode::add) out<<"bad=__builtin_add_overflow(a,b,&r);";
    else if(operation==abi::tensor_binary_opcode::subtract) out<<"bad=__builtin_sub_overflow(a,b,&r);";
    else if(operation==abi::tensor_binary_opcode::multiply) out<<"bad=__builtin_mul_overflow(a,b,&r);";
    else if(operation==abi::tensor_binary_opcode::divide){
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
std::string hip_float_binary_source(int dtype,int operation,int scalar_side) {
    const auto type=dtype==QCORE_DTYPE_FLOAT32?"float":"double";
    const char* op=operation==abi::tensor_binary_opcode::add?"+":operation==abi::tensor_binary_opcode::subtract?"-":operation==abi::tensor_binary_opcode::multiply?"*":"/";
    const char* power=dtype==QCORE_DTYPE_FLOAT32?"powf":"pow";
    std::ostringstream out;
    out<<"#include <math.h>\nextern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* left,const "<<type<<"* right,"<<type<<" scalar,unsigned long long count,unsigned int* status){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" a=left[i];";
    const char* l=scalar_side==abi::scalar_side::left?"scalar":"a";
    const char* r=scalar_side==abi::scalar_side::none?"right[i]":scalar_side==abi::scalar_side::left?"a":"scalar";
    out<<type<<" lv="<<l<<";"<<type<<" rv="<<r<<";unsigned int bits=0u;";
    if(operation==abi::tensor_binary_opcode::power) {
        out<<"if(lv==0&&rv==0)bits="<<real_status::zero_power_zero<<"u;"
             "else if(lv==0&&rv<0)bits="<<real_status::zero_power_negative<<"u;"
             "else if(lv<0&&trunc(rv)!=rv)bits="<<real_status::negative_base<<"u;"
           <<type<<" result="<<power<<"(lv,rv);";
    } else {
        out<<type<<" result=lv"<<op<<"rv;";
    }
    out<<"if(bits==0u&&!isfinite(result))bits=";
    if(operation==abi::tensor_binary_opcode::divide)
        out<<"rv==0?"<<real_status::zero_divisor<<"u:";
    out<<real_status::overflow<<"u;"
         "if(bits!=0u)atomicOr(status,bits);out[i]=result;}";
    return out.str();
}

std::string hip_float_unary_source(int dtype) {
    const auto type=dtype==QCORE_DTYPE_FLOAT32?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
        "* out,const "+type+"* in,unsigned long long count){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count){"+type+" v=in[i];out[i]=-v;}}";
}

std::string hip_float_cast_source(int source_dtype,int target_dtype) {
    const auto src=source_dtype==QCORE_DTYPE_FLOAT32?"float":"double";
    const auto dst=target_dtype==QCORE_DTYPE_FLOAT32?"float":"double";
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

std::string ptx_gather_backward_kernel(int dtype) {
    const bool f32=dtype==QCORE_DTYPE_FLOAT32;
    const auto width=f32?4:8;
    std::ostringstream out;
    out<<ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_grad,\n"
        "  .param .u64 p_offsets,\n  .param .u64 p_positions,\n"
        "  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<3>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<28>;\n  .reg .f32 %f<4>;\n"
          : "  .reg .pred %p<3>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<28>;\n  .reg .f64 %fd<4>;\n");
    out<<"  ld.param.u64 %rd1, [p_out];\n"
          "  ld.param.u64 %rd2, [p_grad];\n"
          "  ld.param.u64 %rd3, [p_offsets];\n"
          "  ld.param.u64 %rd4, [p_positions];\n"
          "  ld.param.u64 %rd5, [p_count];\n"
          "  setp.ge.u64 %p1, %rd7, %rd5;\n"
          "  @%p1 bra DONE;\n"
          "  mul.lo.u64 %rd8, %rd7, 8;\n"
          "  add.u64 %rd9, %rd3, %rd8;\n"
          "  ld.global.u64 %rd10, [%rd9];\n"
          "  add.u64 %rd11, %rd9, 8;\n"
          "  ld.global.u64 %rd12, [%rd11];\n";
    if(f32) out<<"  mov.f32 %f1, 0f00000000;\n";
    else out<<"  mov.f64 %fd1, 0d0000000000000000;\n";
    out<<"LOOP:\n"
          "  setp.ge.u64 %p2, %rd10, %rd12;\n"
          "  @%p2 bra STORE;\n"
          "  mul.lo.u64 %rd13, %rd10, 8;\n"
          "  add.u64 %rd14, %rd4, %rd13;\n"
          "  ld.global.u64 %rd15, [%rd14];\n"
       <<"  mul.lo.u64 %rd16, %rd15, "<<width<<";\n"
          "  add.u64 %rd17, %rd2, %rd16;\n";
    if(f32) out<<"  ld.global.f32 %f2, [%rd17];\n  add.rn.f32 %f1, %f1, %f2;\n";
    else out<<"  ld.global.f64 %fd2, [%rd17];\n  add.rn.f64 %fd1, %fd1, %fd2;\n";
    out<<"  add.u64 %rd10, %rd10, 1;\n"
          "  bra LOOP;\n"
          "STORE:\n"
       <<"  mul.lo.u64 %rd18, %rd7, "<<width<<";\n"
          "  add.u64 %rd19, %rd1, %rd18;\n";
    if(f32) out<<"  st.global.f32 [%rd19], %f1;\n";
    else out<<"  st.global.f64 [%rd19], %fd1;\n";
    out<<"DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_binary_kernel(int dtype, int operation, int scalar_side) {
    const bool f32 = dtype == QCORE_DTYPE_FLOAT32;
    const auto width = f32 ? 4 : 8;
    const char* kind = f32 ? "f32" : "f64";
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_left,\n"
        "  .param .u64 p_right,\n  .param .u64 p_scalar,\n"
        "  .param .u64 p_count,\n  .param .u64 p_status\n",
        f32
          ? "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f64 %fd<8>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_left];\n"
           "  ld.param.u64 %rd3, [p_right];\n"
           "  ld.param.u64 %rd4, [p_scalar];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd12, [p_status];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
           "  add.u64 %rd10, %rd3, %rd8;\n"
           "  add.u64 %rd11, %rd1, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n";
        if (scalar_side == abi::scalar_side::none) out << "  ld.global.f32 %f2, [%rd10];\n";
        else out << "  cvt.u32.u64 %r5, %rd4;\n  mov.b32 %f2, %r5;\n";
        const char* lhs = scalar_side == abi::scalar_side::left ? "%f2" : "%f1";
        const char* rhs = scalar_side == abi::scalar_side::left ? "%f1" : "%f2";
        const char* op = operation == abi::tensor_binary_opcode::add ? "add" : operation == abi::tensor_binary_opcode::subtract ? "sub" :
                         operation == abi::tensor_binary_opcode::multiply ? "mul" : "div";
        out << "  " << op << "." << kind << " %f3, " << lhs << ", " << rhs << ";\n"
               "  testp.finite.f32 %p2, %f3;\n"
               "  @%p2 bra STORE;\n";
        if (operation == abi::tensor_binary_opcode::divide)
            out << "  setp.eq.f32 %p3, " << rhs << ", 0f00000000;\n"
                << "  selp.u32 %r6, " << real_status::zero_divisor << ", " << real_status::overflow << ", %p3;\n";
        else
            out << "  mov.u32 %r6, " << real_status::overflow << ";\n";
        out << "  atom.global.or.b32 %r7, [%rd12], %r6;\n"
               "STORE:\n"
               "  st.global.f32 [%rd11], %f3;\n";
    } else {
        out << "  ld.global.f64 %fd1, [%rd9];\n";
        if (scalar_side == abi::scalar_side::none) out << "  ld.global.f64 %fd2, [%rd10];\n";
        else out << "  mov.b64 %fd2, %rd4;\n";
        const char* lhs = scalar_side == abi::scalar_side::left ? "%fd2" : "%fd1";
        const char* rhs = scalar_side == abi::scalar_side::left ? "%fd1" : "%fd2";
        const char* op = operation == abi::tensor_binary_opcode::add ? "add" : operation == abi::tensor_binary_opcode::subtract ? "sub" :
                         operation == abi::tensor_binary_opcode::multiply ? "mul" : "div";
        out << "  " << op << "." << kind << " %fd3, " << lhs << ", " << rhs << ";\n"
               "  testp.finite.f64 %p2, %fd3;\n"
               "  @%p2 bra STORE;\n";
        if (operation == abi::tensor_binary_opcode::divide)
            out << "  setp.eq.f64 %p3, " << rhs << ", 0d0000000000000000;\n"
                << "  selp.u32 %r6, " << real_status::zero_divisor << ", " << real_status::overflow << ", %p3;\n";
        else
            out << "  mov.u32 %r6, " << real_status::overflow << ";\n";
        out << "  atom.global.or.b32 %r7, [%rd12], %r6;\n"
               "STORE:\n"
               "  st.global.f64 [%rd11], %fd3;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_unary_kernel(int dtype) {
    const bool f32 = dtype == QCORE_DTYPE_FLOAT32;
    const auto width = f32 ? 4 : 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_in,\n  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<2>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<16>;\n  .reg .f32 %f<4>;\n"
          : "  .reg .pred %p<2>;\n  .reg .b32 %r<8>;\n  .reg .b64 %rd<16>;\n  .reg .f64 %fd<4>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd2, %rd8;\n"
           "  add.u64 %rd10, %rd1, %rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1, [%rd9];\n"
               "  neg.f32 %f2, %f1;\n"
               "  st.global.f32 [%rd10], %f2;\n";
    } else {
        out << "  ld.global.f64 %fd1, [%rd9];\n"
               "  neg.f64 %fd2, %fd1;\n"
               "  st.global.f64 [%rd10], %fd2;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_float_cast_kernel(int source_dtype, int target_dtype) {
    const bool src32 = source_dtype == QCORE_DTYPE_FLOAT32;
    const bool dst32 = target_dtype == QCORE_DTYPE_FLOAT32;
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

// The exponent bits of each binary32 (width 4) or binary64 (width 8) value:
// all ones for NaN and the infinities, which set the status word.
std::string ptx_validate_finite_kernel(std::size_t width) {
    const bool wide = width == 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_in,\n  .param .u64 p_count,\n  .param .u64 p_status\n",
        "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<20>;\n");
    out << "  ld.param.u64 %rd1, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd4, [p_status];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8, %rd7, " << width << ";\n"
           "  add.u64 %rd9, %rd1, %rd8;\n";
    if (wide)
        out << "  ld.global.u64 %rd10, [%rd9];\n"
               "  and.b64 %rd11, %rd10, 0x7ff0000000000000;\n"
               "  setp.ne.u64 %p2, %rd11, 0x7ff0000000000000;\n";
    else
        out << "  ld.global.u32 %r6, [%rd9];\n"
               "  and.b32 %r7, %r6, 0x7f800000;\n"
               "  setp.ne.u32 %p2, %r7, 0x7f800000;\n";
    out << "  @%p2 bra DONE;\n"
           "  atom.global.exch.b32 %r8, [%rd4], 1;\n"
           "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string hip_validate_finite_source(std::size_t width) {
    const bool wide = width == 8;
    return std::string("extern \"C\" __global__ void q_kernel(const ") +
           (wide ? "unsigned long long" : "unsigned int") +
           "* in,unsigned long long count,unsigned int* status){"
           "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
           "if(i>=count)return;" +
           (wide ? "if((in[i]&0x7ff0000000000000ull)==0x7ff0000000000000ull)"
                 : "if((in[i]&0x7f800000u)==0x7f800000u)") +
           "atomicExch(status,1u);}";
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



#ifdef __APPLE__

// Core kernel families on Metal. A pipeline is identified by its family and
// up to three small parameters (dtype, operation, scalar side, width), so the
// per-operation lookup builds neither a source string nor a key string; the
// MSL source is generated only when the pipeline is compiled.
enum class MetalKernel : std::uint8_t {
    FillOne = 1, Gather, GatherBackwardF32, GatherBackwardInt, CompareAll,
    BinaryInt, BinaryF32, UnaryInt, UnaryNegF32, Cast, BinaryBackwardF32,
    ScalarBackwardF32, CopyBytes, ZeroBytes, PoisonBytes, StridedCopy,
    BinaryStridedF32, BroadcastBackwardF32, ValidateFinite, ComplexBinary, ComplexBinaryStrided, ComplexUnary, CompareElements,
};

constexpr std::uint32_t metal_pipeline_id(MetalKernel family, unsigned a = 0,
                                          unsigned b = 0, unsigned c = 0) {
    return (static_cast<std::uint32_t>(family) << 24) | ((a & 0xffu) << 16) |
           ((b & 0xffu) << 8) | (c & 0xffu);
}

// In safe math mode, Core kernels follow the IEEE-754 contract of
// language.md ("without fast-math"): safe math, precise library functions
// (Core uses only pow) and no floating-point contraction, so a fused
// multiply-add is never formed behind the kernel's back.
//
// Core and package Metal kernels use safe arithmetic by default, with
// floating-point contraction disabled. The comparison switches retain the
// former compiler options: QUIDRA_METAL_FAST_MATH=1 or QUIDRA_METAL_SAFE_MATH=0.
constexpr bool metal_safe_math_default = true;

bool metal_fast_math() {
    static const bool fast = [] {
        if (const auto value = platform::environment_value("QUIDRA_METAL_FAST_MATH");
            value && *value == "1")
            return true;
        if (const auto value = platform::environment_value("QUIDRA_METAL_SAFE_MATH");
            value && !value->empty())
            return *value == "0";
        return !metal_safe_math_default;
    }();
    return fast;
}

bool metal_numeric_safe_pipeline(std::uint32_t key){
    const auto family=static_cast<MetalKernel>(key>>24);
    const auto a=(key>>16)&255u,b=(key>>8)&255u,c=key&255u;
    switch(family){
        case MetalKernel::ComplexBinary:case MetalKernel::ComplexBinaryStrided:
        case MetalKernel::ComplexUnary:case MetalKernel::CompareElements:return true;
        case MetalKernel::BinaryF32:case MetalKernel::BinaryStridedF32:
        case MetalKernel::BinaryBackwardF32:case MetalKernel::ScalarBackwardF32:
            return c>=QCORE_DTYPE_FLOAT16;
        case MetalKernel::Cast:return a>=QCORE_DTYPE_FLOAT16||b>=QCORE_DTYPE_FLOAT16;
        case MetalKernel::UnaryNegF32:case MetalKernel::GatherBackwardF32:
        case MetalKernel::BroadcastBackwardF32:case MetalKernel::CompareAll:
            return a>=QCORE_DTYPE_FLOAT16;
        default:return false;
    }
}

MTLCompileOptions* metal_compile_options(bool force_safe=false) {
    if(metal_fast_math()&&!force_safe)return static_cast<MTLCompileOptions*>(nil);
    static MTLCompileOptions* options = [] {
        MTLCompileOptions* value = [[MTLCompileOptions alloc] init];
        if (@available(macOS 15.0, iOS 18.0, *)) {
            value.mathMode = MTLMathModeSafe;
            value.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        } else {
            // Before macOS 15 the only switch is fastMathEnabled; turning it
            // off also selects precise math library functions.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            value.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        return value;
    }();
    return options;
}

// The bfloat pipeline keeps binary32 subnormals in integer representation.
// Helper blocks retain their native storage bit conversions.
std::string msl_bfloat_soften(const std::string& source) {
    if(source.find("#ifndef QUIDRA_SOFT_FLOAT")==std::string::npos)return source;
    std::string result;
    for(std::size_t i=0;i<source.size();){
        if(source.compare(i,14,"// qsoft_begin")==0){
            const auto end=source.find("// qsoft_end",i);
            const auto length=end==std::string::npos?source.size()-i:end+12-i;
            result.append(source,i,length);i+=length;continue;
        }
        const auto identifier=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';};
        if(identifier(source[i])){
            auto end=i+1;while(end<source.size()&&identifier(source[end]))++end;
            const auto word=source.substr(i,end-i);
            result+=word=="float"?"qf":word=="float2"?"qf2":word=="pow"?"qpow":
                word=="fabs"?"qabs":word=="isfinite"?"qfinite":word=="trunc"?"qtrunc":word;
            i=end;
        }else result+=source[i++];
    }
    const auto replace=[&](std::string_view from,std::string_view to){
        std::size_t i=0;while((i=result.find(from,i))!=std::string::npos){result.replace(i,from.size(),to);i+=to.size();}
    };
    replace("precise::qpow(","qpow(");
    replace("as_type<uint>(result)","qraw(result)");
    replace("as_type<qf2>(", "qpair(");
    replace("as_type<qf>(", "qfromraw(");
    replace("constant qf& scalar", "constant float& scalar");
    const auto namespace_end=result.find("using namespace metal;");
    if(namespace_end!=std::string::npos)
        result.insert(namespace_end+22,std::string("\n")+msl_soft_float_body);
    return result;
}

id<MTLComputePipelineState> compile_metal_pipeline(
    id<MTLDevice> device, std::uint32_t pipeline_id, const std::string& source,
    std::string& error,int device_index) {
    observation::Scope observed(observation::Kind::Compile,device_index,"metal pipeline compile",
                                 counters::WaitReason::Sync,pipeline_id);
    counters::add(counters::Id::MetalPipelineCompiles); // qcount
    if (counters::enabled()) {
        counters::SourceSite site;
        if (counters::current_source_site(site))
            counters::detail::add_slow(counters::Id::MetalPipelineCompilesTimed, 1);
    }
    @autoreleasepool {
        const bool force_safe=metal_numeric_safe_pipeline(pipeline_id);
        const auto numeric_source=msl_bfloat_soften(source);
        const std::string text_source = metal_fast_math()&&!force_safe
            ? numeric_source : "#pragma METAL fp contract(off)\n" + numeric_source;
        NSString* text = [NSString stringWithUTF8String:text_source.c_str()];
        NSError* ns_error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:text options:metal_compile_options(force_safe)
                                   error:&ns_error];
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
            error = "Metal compute kernel q_kernel was not found for pipeline " +
                    std::to_string(pipeline_id);
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
        return pipeline;
    }
}

#include "device_metal_stream.inc"

// Pipelines are compiled once per device and kept for the process.
template <typename MakeSource>
id<MTLComputePipelineState> metal_pipeline(
    int backend_index, std::uint32_t pipeline_id, MakeSource&& make_source,
    std::string& error) {
    auto* stream = metal_stream(backend_index);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return nil;
    }
    std::lock_guard lock(stream->pipeline_mutex);
    if (const auto found = stream->pipelines.find(pipeline_id);
        found != stream->pipelines.end())
        return found->second;
    id<MTLComputePipelineState> pipeline =
        compile_metal_pipeline(stream->device, pipeline_id, make_source(), error,stream->global_index);
    if (pipeline) stream->pipelines.emplace(pipeline_id, pipeline);
    return pipeline;
}

template <typename MakeSource>
id<MTLComputePipelineState> metal_pipeline(
    const Buffer* buffer, std::uint32_t pipeline_id, MakeSource&& make_source,
    std::string& error) {
    return metal_pipeline(buffer->backend_index, pipeline_id,
                          std::forward<MakeSource>(make_source), error);
}

bool synchronize_metal_backend(int backend_index,std::string& error,
                               std::uint64_t* covered) {
    auto* stream=metal_stream(backend_index);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    return metal_stream_synchronize(*stream,counters::WaitReason::Sync,error,covered);
}

std::uint64_t metal_deferred_check_serial(const Buffer* status) {
    auto* stream=metal_stream_for(status);
    return stream ? metal_check_serial(*stream,*status) : 0;
}

bool metal_copy_from_host_blit(Buffer* raw,std::size_t offset,const void* source,
                          std::size_t bytes,std::string& error) {
    auto* stream=metal_stream_for(raw);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        id<MTLBuffer> staging=[stream->device newBufferWithBytes:source
                                                         length:bytes
                                                        options:MTLResourceStorageModeShared];
        if(!staging){
            error="Metal upload staging allocation failed";
            return false;
        }
        counters::add(counters::Id::MetalNewBuffers); // qcount
        counters::add(counters::Id::MetalStagedUploads); // qcount
        std::lock_guard lock(stream->mutex);
        id<MTLBlitCommandEncoder> encoder=metal_blit_encoder_locked(*stream,error);
        if(!encoder){
            [staging release];
            return false;
        }
        [encoder copyFromBuffer:staging sourceOffset:0
                       toBuffer:raw->metal_buffer destinationOffset:raw->base_offset+offset
                           size:bytes];
        [encoder endEncoding];
        // The command buffer retains the staging buffer until it completes.
        [staging release];
        metal_note_work_locked(*stream,1,bytes);
        return true;
    }
}

bool metal_copy_device_to_device_blit(Buffer* destination,std::size_t destination_offset,
                                 const Buffer* source,std::size_t source_offset,
                                 std::size_t bytes,std::string& error) {
    auto* stream=metal_stream_for(destination);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        id<MTLBlitCommandEncoder> encoder=metal_blit_encoder_locked(*stream,error);
        if(!encoder) return false;
        [encoder copyFromBuffer:source->metal_buffer
                   sourceOffset:source->base_offset+source_offset
                       toBuffer:destination->metal_buffer
              destinationOffset:destination->base_offset+destination_offset
                           size:bytes];
        [encoder endEncoding];
        metal_note_work_locked(*stream,1,bytes);
        return true;
    }
}

bool metal_zero_blit(Buffer* raw,std::size_t offset,std::size_t bytes,
                std::string& error) {
    auto* stream=metal_stream_for(raw);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        id<MTLBlitCommandEncoder> encoder=metal_blit_encoder_locked(*stream,error);
        if(!encoder) return false;
        [encoder fillBuffer:raw->metal_buffer
                      range:NSMakeRange(raw->base_offset+offset,bytes) value:0];
        [encoder endEncoding];
        metal_note_work_locked(*stream,1,bytes);
        return true;
    }
}

// Uploads, fills and same-device copies. All Metal storage is shared
// (unified) memory, so a buffer that no queued GPU work can touch is written
// directly by the CPU, without a command. Otherwise the write must be
// ordered with the queued work, and it is encoded as a compute kernel in
// Core's open encoder, which is much cheaper than switching between a blit
// and a compute encoder.
// QUIDRA_METAL_UPLOAD=blit restores the blit encoders.

struct MetalTransferConfig {
    bool blit{};
    std::size_t host_fill_max{64u << 10};
};

const MetalTransferConfig& metal_transfer_config() {
    static const MetalTransferConfig config = [] {
        MetalTransferConfig value;
        if (const auto upload = platform::environment_value("QUIDRA_METAL_UPLOAD"))
            value.blit = *upload == "blit";
        value.host_fill_max = static_cast<std::size_t>(
            metal_env_u64("QUIDRA_METAL_HOST_FILL_MAX", value.host_fill_max));
        return value;
    }();
    return config;
}

// QUIDRA_METAL_UPLOAD=blit selects blit transfers. A transfer is refused
// before this choice while a package holds Core's encoder or command buffer
// (metal_scope_refuses_locked): a blit encoder would end and release the
// encoder under the package, so that, for example, a copy-on-write detach
// inside a scope would crash the package's next setComputePipelineState,
// and a compute kernel would replace the package's pipeline state and
// bindings. Caller holds stream.mutex.
bool metal_use_blit_locked(const MetalStream& stream) {
    return metal_transfer_config().blit && !metal_scope_open(stream);
}

unsigned char* metal_host_bytes(const Buffer* buffer, std::size_t offset) {
    return static_cast<unsigned char*>([buffer->metal_buffer contents]) +
           buffer->base_offset + offset;
}

// Widest element (16, 4 or 1 bytes) dividing every offset and the size.
std::size_t metal_transfer_width(std::size_t a, std::size_t b, std::size_t bytes) {
    for (const std::size_t width : {std::size_t{16}, std::size_t{4}})
        if (a % width == 0 && b % width == 0 && bytes % width == 0) return width;
    return 1;
}

const char* metal_transfer_type(std::size_t width) {
    return width == 16 ? "uint4" : width == 4 ? "uint" : "uchar";
}

// out[out_index + i] = in[in_index + i] for i < count elements of `width`.
id<MTLComputePipelineState> metal_copy_pipeline(
    MetalStream& stream, std::size_t width, std::string& error) {
    return metal_pipeline(stream.backend_index,
        metal_pipeline_id(MetalKernel::CopyBytes, static_cast<unsigned>(width)), [&] {
        const std::string type = metal_transfer_type(width);
        return std::string(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void q_kernel(device " + type + "* out [[buffer(0)]], "
        "const device " + type + "* in [[buffer(1)]], "
        "constant ulong* range [[buffer(2)]], "
        "uint gid [[thread_position_in_grid]]) {\n"
        "  if (ulong(gid) >= range[2]) return;\n"
        "  out[range[0] + gid] = in[range[1] + gid];\n"
        "}\n");
    }, error);
}

id<MTLComputePipelineState> metal_fill_pipeline(
    MetalStream& stream, std::size_t width, std::string& error) {
    return metal_pipeline(stream.backend_index,
        metal_pipeline_id(MetalKernel::ZeroBytes, static_cast<unsigned>(width)), [&] {
        const std::string type = metal_transfer_type(width);
        return std::string(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void q_kernel(device " + type + "* out [[buffer(0)]], "
        "constant ulong* range [[buffer(1)]], "
        "uint gid [[thread_position_in_grid]]) {\n"
        "  if (ulong(gid) >= range[1]) return;\n"
        "  out[range[0] + gid] = " + type + "(0);\n"
        "}\n");
    }, error);
}

void metal_observe_core_dispatch(MetalStream& stream,id<MTLComputePipelineState> pipeline) noexcept {
    if(!stream.observation || stream.observation->current_compute>=stream.observation->passes.size()) return;
    auto& pass=stream.observation->passes[stream.observation->current_compute];
    std::optional<std::uint32_t> identity;
    if(profile::enabled()) try {
        std::lock_guard pipeline_lock(stream.pipeline_mutex);
        for(const auto& [key,value]:stream.pipelines) if(value==pipeline) {identity=key;break;}
    } catch (...) {}
    pass.attribution.core_dispatch(identity);
    pass.kernel.clear();
    if(const auto exact=pass.attribution.kernel()) try {pass.kernel="core."+std::to_string(*exact);} catch (...) {}
}
void metal_dispatch_locked(MetalStream& stream, id<MTLComputeCommandEncoder> encoder,
                           id<MTLComputePipelineState> pipeline, std::size_t count) {
    const auto max_threads =
        static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]);
    const auto threads = std::max<std::size_t>(1, std::min<std::size_t>(256, max_threads));
    [encoder dispatchThreads:MTLSizeMake(count,1,1)
       threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
    metal_observe_core_dispatch(stream,pipeline);
    if(profile::split_enabled() && stream.hold==MetalHold::None) {
        metal_end_encoder_locked(stream);
        stream.observation_reopen=true;
    }
}

void metal_poison_in_queue(MetalStream& stream, id<MTLBuffer> buffer,
                           std::size_t bytes, unsigned char value,
                           std::uint64_t& busy_until) {
    std::string error;
    auto pipeline = metal_pipeline(stream.backend_index,
        metal_pipeline_id(MetalKernel::PoisonBytes), [] {
        return std::string(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void q_kernel(device uchar* out [[buffer(0)]], "
        "constant ulong& count [[buffer(1)]], constant uchar& value [[buffer(2)]], "
        "uint gid [[thread_position_in_grid]]) {\n"
        "  if (ulong(gid) < count) out[gid] = value;\n"
        "}\n");
    }, error);
    if (!pipeline) return;
    std::lock_guard lock(stream.mutex);
    // Inside a package encoder scope Core encodes nothing: the encoder
    // request records a protocol violation, the buffer stays unpoisoned, and
    // the program stops with GPU_SCOPE.
    id<MTLComputeCommandEncoder> encoder = metal_compute_encoder_locked(stream, error);
    if (!encoder) return;
    const std::uint64_t count = bytes;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder setBytes:&count length:sizeof(count) atIndex:1];
    [encoder setBytes:&value length:sizeof(value) atIndex:2];
    metal_dispatch_locked(stream, encoder, pipeline, bytes);
    busy_until = stream.next_serial.load(std::memory_order_relaxed);
    metal_note_work_locked(stream, 1, bytes);
}

// Encodes destination[destination_offset..+bytes) = source[...] in Core's
// open encoder. Caller holds stream.mutex.
bool metal_encode_copy_locked(MetalStream& stream, Buffer* destination,
                              std::size_t destination_offset, const Buffer* source,
                              std::size_t source_offset, std::size_t bytes,
                              std::string& error) {
    const auto width = metal_transfer_width(
        destination->base_offset + destination_offset,
        source->base_offset + source_offset, bytes);
    auto pipeline = metal_copy_pipeline(stream, width, error);
    if (!pipeline) return false;
    id<MTLComputeCommandEncoder> encoder = metal_compute_encoder_locked(stream, error);
    if (!encoder) return false;
    const std::uint64_t range[3] = {
        (destination->base_offset + destination_offset) / width,
        (source->base_offset + source_offset) / width, bytes / width};
    [encoder setComputePipelineState:pipeline];
    metal_bind_whole(encoder, destination, 0);
    metal_bind_whole(encoder, source, 1);
    [encoder setBytes:range length:sizeof(range) atIndex:2];
    metal_dispatch_locked(stream, encoder, pipeline, static_cast<std::size_t>(range[2]));
    metal_note_work_locked(stream, 1, bytes);
    return true;
}

bool metal_copy_from_host(Buffer* raw,std::size_t offset,const void* source,
                          std::size_t bytes,std::string& error) {
    auto* stream=metal_stream_for(raw);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        if (metal_scope_refuses_locked(*stream, "a Core upload", error)) return false;
        if (metal_use_blit_locked(*stream))
            return metal_copy_from_host_blit(raw, offset, source, bytes, error);
        if (metal_buffer_idle_locked(*stream, *raw)) {
            std::memcpy(metal_host_bytes(raw, offset), source, bytes);
            counters::add(counters::Id::MetalDirectUploads); // qcount
            return true;
        }
        // Queued work may still use the destination: stage through an idle
        // pooled buffer and copy in queue order.
        Buffer staging;
        staging.backend = Backend::Metal;
        staging.global_index = raw->global_index;
        staging.backend_index = raw->backend_index;
        staging.bytes = bytes;
        std::uint64_t busy_until = 0;
        staging.metal_buffer = metal_acquire_buffer(
            raw->backend_index, bytes, true, staging.physical_bytes, busy_until,
            error);
        if (!staging.metal_buffer) return false;
        std::memcpy(metal_host_bytes(&staging, 0), source, bytes);
        counters::add(counters::Id::MetalStagedUploads); // qcount
        const bool ok = metal_encode_copy_locked(
            *stream, raw, offset, &staging, 0, bytes, error);
        metal_release_buffer(raw->backend_index, staging.metal_buffer,
                             staging.physical_bytes);
        staging.metal_buffer = nil;
        return ok;
    }
}

bool metal_copy_to_host(const Buffer* raw,std::size_t offset,void* destination,
                        std::size_t bytes,std::uint64_t& covered,
                        std::string& error) {
    auto* stream=metal_stream_for(raw);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    if(!metal_stream_synchronize(*stream,counters::WaitReason::Read,error,&covered))
        return false;
    std::memcpy(destination, metal_host_bytes(raw, offset), bytes);
    return true;
}

// Reads bytes no queued GPU work can still write, without waiting. False
// when the read needs a synchronization, which also reports a command buffer
// that completed with an error (a host read is a synchronization point).
bool metal_copy_to_host_if_idle(const Buffer* raw, std::size_t offset,
                                void* destination, std::size_t bytes) {
    if (metal_transfer_config().blit) return false;
    auto* stream = metal_stream_for(raw);
    if (!stream || stream->async_failed.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(stream->mutex);
    if (!metal_buffer_idle_locked(*stream, *raw)) return false;
    std::memcpy(destination, metal_host_bytes(raw, offset), bytes);
    return true;
}

bool metal_copy_device_to_device(Buffer* destination,std::size_t destination_offset,
                                 const Buffer* source,std::size_t source_offset,
                                 std::size_t bytes,std::string& error) {
    auto* stream=metal_stream_for(destination);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        if (metal_scope_refuses_locked(*stream, "a Core device copy", error))
            return false;
        if (metal_use_blit_locked(*stream))
            return metal_copy_device_to_device_blit(
                destination, destination_offset, source, source_offset, bytes, error);
        if (bytes <= metal_transfer_config().host_fill_max &&
            metal_buffer_idle_locked(*stream, *destination) &&
            metal_buffer_idle_locked(*stream, *source)) {
            std::memmove(metal_host_bytes(destination, destination_offset),
                         metal_host_bytes(source, source_offset), bytes);
            return true;
        }
        return metal_encode_copy_locked(*stream, destination, destination_offset,
                                        source, source_offset, bytes, error);
    }
}

// Clears a validation status page. A new page is allocated idle, and a
// recycled page is idle once every slot was consumed after the
// synchronization that covered it, so the clear is a host memset. Otherwise
// it is a kernel committed at once (outside a package hold), so a command
// buffer a package commits for a slot of the page runs after the clear; in
// a hold the package's work follows it in the same command buffer.
bool metal_clear_status_page(Buffer* page, std::size_t bytes, std::string& error) {
    auto* stream = metal_stream_for(page);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return false;
    }
    std::lock_guard lock(stream->mutex);
    if (metal_scope_refuses_locked(*stream, "a Core status page clear", error))
        return false;
    if (metal_buffer_idle_locked(*stream, *page)) {
        std::memset(metal_host_bytes(page, 0), 0, bytes);
        counters::add(counters::Id::MetalDirectFills); // qcount
        return true;
    }
    if (!metal_zero(page, 0, bytes, error)) return false;
    if (stream->hold == MetalHold::None) metal_commit_locked(*stream);
    return true;
}

bool metal_zero(Buffer* raw,std::size_t offset,std::size_t bytes,
                std::string& error) {
    auto* stream=metal_stream_for(raw);
    if(!stream){
        error="Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        if (metal_scope_refuses_locked(*stream, "a Core fill", error)) return false;
        if (metal_use_blit_locked(*stream))
            return metal_zero_blit(raw, offset, bytes, error);
        if (bytes <= metal_transfer_config().host_fill_max &&
            metal_buffer_idle_locked(*stream, *raw)) {
            std::memset(metal_host_bytes(raw, offset), 0, bytes);
            counters::add(counters::Id::MetalDirectFills); // qcount
            return true;
        }
        const auto width = metal_transfer_width(raw->base_offset + offset, 0, bytes);
        auto pipeline = metal_fill_pipeline(*stream, width, error);
        if (!pipeline) return false;
        id<MTLComputeCommandEncoder> encoder = metal_compute_encoder_locked(*stream, error);
        if (!encoder) return false;
        const std::uint64_t range[2] = {(raw->base_offset + offset) / width, bytes / width};
        [encoder setComputePipelineState:pipeline];
        metal_bind_whole(encoder, raw, 0);
        [encoder setBytes:range length:sizeof(range) atIndex:1];
        metal_dispatch_locked(*stream, encoder, pipeline, static_cast<std::size_t>(range[1]));
        counters::add(counters::Id::MetalKernelFills); // qcount
        metal_note_work_locked(*stream, 1, bytes);
        return true;
    }
}

bool dispatch_metal_1d(Buffer* output, id<MTLComputePipelineState> pipeline,
                       std::size_t count,
                       const std::function<void(id<MTLComputeCommandEncoder>)>& bind,
                       std::string& error) {
    if (count == 0) return true;
    auto* stream = metal_stream_for(output);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return false;
    }
    @autoreleasepool {
        std::lock_guard lock(stream->mutex);
        id<MTLComputeCommandEncoder> encoder =
            metal_compute_encoder_locked(*stream, error);
        if (!encoder) return false;
        [encoder setComputePipelineState:pipeline];
        bind(encoder);
        const auto max_threads =
            static_cast<std::size_t>([pipeline maxTotalThreadsPerThreadgroup]);
        const auto threads = std::max<std::size_t>(
            1, std::min<std::size_t>(256, max_threads));
        [encoder dispatchThreads:MTLSizeMake(count,1,1)
           threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
        counters::add(counters::Id::CoreDispatches); // qcount
        metal_observe_core_dispatch(*stream,pipeline);
        if(profile::split_enabled() && stream->hold==MetalHold::None) {
            metal_end_encoder_locked(*stream);
            stream->observation_reopen=true;
        }
        metal_note_work_locked(*stream, 1, output->bytes);
        return true;
    }
}

std::string msl_raw_type(std::size_t width) {
    switch (width) {
        case 1: return "uchar";
        case 2: return "ushort";
        case 4: return "uint";
        case 8: return "ulong";
        case 16: return "ulong2";
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
           "  out[gid] = " << type << "(" << bits << (width==16?"ul,0ul);\n":"ul);\n")
        << "}\n";
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

std::string msl_gather_backward_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* grad [[buffer(1)]], "
           "const device ulong* offsets [[buffer(2)]], "
           "const device ulong* positions [[buffer(3)]], "
           "constant ulong& count [[buffer(4)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  ulong i=ulong(gid); if(i>=count) return; float total=0.0f;\n"
           "  for(ulong p=offsets[i];p<offsets[i+1];++p) total+=grad[positions[p]];\n"
           "  out[i]=total;\n}\n";
}

// The MSL lines that compute out[gid] from float operands `l` and `r` and OR
// the failure's bits (real_status) into *status: the powers without a value
// are tested first, then the result's finiteness.
std::string msl_real_element(int operation, const char* l, const char* r) {
    const char* op = operation == abi::tensor_binary_opcode::add ? "+" : operation == abi::tensor_binary_opcode::subtract ? "-" :
                     operation == abi::tensor_binary_opcode::multiply ? "*" : "/";
    std::ostringstream out;
    out << "  float lv = " << l << ";\n  float rv = " << r << ";\n  uint bits = 0u;\n";
    if (operation == abi::tensor_binary_opcode::power) {
        out << "  if (lv == 0.0f && rv == 0.0f) bits = " << real_status::zero_power_zero << "u;\n"
               "  else if (lv == 0.0f && rv < 0.0f) bits = " << real_status::zero_power_negative << "u;\n"
               "  else if (lv < 0.0f && trunc(rv) != rv) bits = " << real_status::negative_base << "u;\n"
               "  float result = pow(lv, rv);\n";
    } else {
        out << "  float result = lv " << op << " rv;\n";
    }
    // The exponent bits, not isfinite(): under Metal fast math (the default,
    // metal_safe_math_default) the compiler may assume results are finite.
    out << "  if (bits == 0u && (as_type<uint>(result) & 0x7f800000u) == 0x7f800000u) bits = "
        << (operation == abi::tensor_binary_opcode::divide ? "rv == 0.0f ? " + std::to_string(real_status::zero_divisor) + "u : " : std::string())
        << real_status::overflow << "u;\n"
           "  if (bits != 0u) atomic_fetch_or_explicit(status, bits, memory_order_relaxed);\n"
           "  out[gid] = result;\n";
    return out.str();
}

// Storage conversions use integer bits so subnormal values survive even on
// devices whose native half arithmetic flushes them. Arithmetic stays binary32.
std::string msl_half_helpers(int dtype) {
    if (dtype == QCORE_DTYPE_BFLOAT16) return std::string(msl_soft_float_body)+R"MSL(
// qsoft_begin
qf qread(ushort v) { return qfromraw(uint(v)<<16); }
ushort qbits(qf v) {
 uint b=qraw(v);
 if ((b&0x7f800000u)==0x7f800000u) return ushort(b>>16);
 return ushort((b+0x7fffu+((b>>16)&1u))>>16);
}
// qsoft_end
)MSL";
    return R"MSL(
// qsoft_begin
float qread(ushort v) {
 uint s=uint(v&0x8000u)<<16,e=(v>>10)&31u,m=v&1023u;
 if(e==0u) { if(m==0u)return as_type<float>(s);
  int n=0; while((m&1024u)==0u){m<<=1;++n;}
  return as_type<float>(s|(uint(113-n)<<23)|((m&1023u)<<13)); }
 if(e==31u)return as_type<float>(s|0x7f800000u|(m<<13));
 return as_type<float>(s|((e+112u)<<23)|(m<<13));
}
ushort qbits(float v) {
 uint b=as_type<uint>(v),s=(b>>16)&0x8000u,e=(b>>23)&255u,m=b&0x7fffffu;
 if(e==255u)return ushort(s|0x7c00u|(m!=0u?512u:0u));
 int h=int(e)-112;
 if(h>=31)return ushort(s|0x7c00u);
 if(h<=0){ if(h < -10)return ushort(s);
  uint n=m|0x800000u,k=uint(14-h),t=n>>k,r=n&((1u<<k)-1u),mid=1u<<(k-1u);
  return ushort(s| (t+uint(r>mid||(r==mid&&(t&1u)!=0u)))); }
 uint t=(uint(h)<<10)|(m>>13),r=m&8191u;
 return ushort(s|(t+uint(r>4096u||(r==4096u&&(t&1u)!=0u))));
}
// qsoft_end
)MSL";
}

void msl_replace(std::string& text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

void msl_replace_load(std::string& text,const std::string& load){
    std::size_t at=0;
    while((at=text.find(load,at))!=std::string::npos){
        const char before=at==0?' ':text[at-1];
        if((before>='a'&&before<='z')||(before>='A'&&before<='Z')||
           (before>='0'&&before<='9')||before=='_'){at+=load.size();continue;}
        const auto value="qread("+load+")";
        text.replace(at,load.size(),value);at+=value.size();
    }
}

// Adapt a binary32 kernel's loads/stores, retaining its arithmetic expression.
std::string msl_half_storage(std::string source, int dtype) {
    if (!half_dtype(dtype)) return source;
    msl_replace(source, "using namespace metal;", "using namespace metal;\n" + msl_half_helpers(dtype));
    msl_replace(source, "device float*", "device ushort*");
    for (const auto* array : {"left", "right", "in", "grad", "gradient", "input"}) {
        for (const auto* index : {"gid", "i", "index", "left_index", "right_index", "positions[p]"}) {
            const auto load = std::string(array) + "[" + index + "]";
            msl_replace_load(source,load);
        }
    }
    msl_replace(source, "as_type<float>(uint(scalar_bits))", "qread(ushort(scalar_bits))");
    msl_replace(source, "  if (bits == 0u", "  result=qread(qbits(result));\n  if (bits == 0u");
    msl_replace(source, "out[gid] = result;", "out[gid] = qbits(result);");
    msl_replace(source, "out[gid] = -qread(in[gid]);", "out[gid] = qbits(-qread(in[gid]));");
    msl_replace(source, "out[i]=total;", "out[i]=qbits(total);");
    msl_replace(source, "out[gid] = total;", "out[gid] = qbits(total);");
    return source;
}

inline bool metal_complex_dtype(int dtype) {
    return dtype>=QCORE_DTYPE_COMPLEX_FLOAT16 && dtype<=QCORE_DTYPE_COMPLEX_FLOAT32;
}

std::string msl_complex_helpers(int dtype) {
    const int part=abi::complex_part_dtype_code(dtype);
    if(half_dtype(part))
        return msl_half_helpers(part)+
            "float qr(float v){return qread(qbits(v));}\n"
            "float2 cr(ushort2 v){return float2(qread(v.x),qread(v.y));}\n"
            "ushort2 cw(float2 v){return ushort2(qbits(v.x),qbits(v.y));}\n";
    return "float qr(float v){return v;}\nfloat2 cr(float2 v){return v;}\nfloat2 cw(float2 v){return v;}\n";
}

std::string msl_complex_binary_source(int dtype,int operation,int scalar_side) {
    const auto storage=half_dtype(abi::complex_part_dtype_code(dtype))?"ushort2":"float2";
    const auto scalar=half_dtype(abi::complex_part_dtype_code(dtype))?
        "cr(as_type<ushort2>(uint(scalar_bits)))":"cr(as_type<float2>(scalar_bits))";
    std::string source="#include <metal_stdlib>\nusing namespace metal;\n"+msl_complex_helpers(dtype)+
        "kernel void q_kernel(device "+storage+"* out [[buffer(0)]],const device "+storage+
        "* left [[buffer(1)]],const device "+storage+"* right [[buffer(2)]],"
        "constant ulong& scalar_bits [[buffer(3)]],constant ulong& count [[buffer(4)]],"
        "device atomic_uint* status [[buffer(5)]],uint gid [[thread_position_in_grid]]){"
        "if(ulong(gid)>=count)return; float2 p=cr(left[gid]),s="+scalar+"; float2 x="+
        (scalar_side==abi::scalar_side::left?"s":"p")+",y="+
        (scalar_side==abi::scalar_side::none?"cr(right[gid])":scalar_side==abi::scalar_side::left?"p":"s")+
        "; float2 z;uint bits=0u;";
    if(operation==abi::tensor_binary_opcode::add)source+="z=float2(qr(x.x+y.x),qr(x.y+y.y));";
    else if(operation==abi::tensor_binary_opcode::subtract)source+="z=float2(qr(x.x-y.x),qr(x.y-y.y));";
    else if(operation==abi::tensor_binary_opcode::multiply)
        source+="float ac=qr(x.x*y.x),bd=qr(x.y*y.y),ad=qr(x.x*y.y),bc=qr(x.y*y.x);"
                "z=float2(qr(ac-bd),qr(ad+bc));";
    else {
        source+="float a=x.x,b=x.y,c=y.x,d=y.y,r,t,rn,in;bool finite=true;"
                "if(fabs(c)>=fabs(d)){r=qr(d/c);float dr=qr(d*r),br=qr(b*r),ar=qr(a*r);"
                "t=qr(c+dr);rn=qr(a+br);in=qr(b-ar);finite=isfinite(r)&&isfinite(dr)&&isfinite(br)&&isfinite(ar); }"
                "else {r=qr(c/d);float crv=qr(c*r),ar=qr(a*r),br=qr(b*r);"
                "t=qr(crv+d);rn=qr(ar+b);in=qr(br-a);finite=isfinite(r)&&isfinite(crv)&&isfinite(ar)&&isfinite(br); }"
                "z=float2(qr(rn/t),qr(in/t));"
                "finite=finite&&isfinite(t)&&isfinite(rn)&&isfinite(in);"
                "if(!finite)bits=(c==0.0f&&d==0.0f)?8u:16u;";
    }
    source+="if(!isfinite(z.x)||!isfinite(z.y))bits=bits!=0u?bits:16u;"
            "if(bits!=0u)atomic_fetch_or_explicit(status,bits,memory_order_relaxed);out[gid]=cw(z);}\n";
    return source;
}

std::string msl_float_binary_source(int operation, int scalar_side) {
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* left [[buffer(1)]], "
           "const device float* right [[buffer(2)]], "
           "constant ulong& scalar_bits [[buffer(3)]], "
           "constant ulong& count [[buffer(4)]], "
           "device atomic_uint* status [[buffer(5)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  float a = left[gid];\n"
           "  float s = as_type<float>(uint(scalar_bits));\n";
    if (scalar_side == abi::scalar_side::none)
        out << msl_real_element(operation, "a", "right[gid]");
    else if (scalar_side == abi::scalar_side::left)
        out << msl_real_element(operation, "s", "a");
    else
        out << msl_real_element(operation, "a", "s");
    out << "}\n";
    return out.str();
}


// One thread per element: the exponent bits of a binary32 (width 4) or
// binary64 (width 8) value are all ones exactly for NaN and the
// infinities. Integer arithmetic on the bits, so neither fast math nor
// missing FP64 support on the GPU matters.
std::string msl_validate_finite_source(int dtype) {
    const auto width = compute_dtype_width(dtype);
    const bool wide = width == 8;
    if (width == 2) {
        const auto mask = dtype == QCORE_DTYPE_FLOAT16 ? "0x7c00u" : "0x7f80u";
        return "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void q_kernel(const device ushort* in [[buffer(0)]],"
            "constant ulong& count [[buffer(1)]],device atomic_uint* status [[buffer(2)]],"
            "uint gid [[thread_position_in_grid]]) { if(ulong(gid)<count && "
            "(in[gid]&" + std::string(mask) + ")==" + mask +
            ")atomic_store_explicit(status,1u,memory_order_relaxed); }\n";
    }
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(const device " << (wide ? "ulong" : "uint") << "* in [[buffer(0)]], "
           "constant ulong& count [[buffer(1)]], "
           "device atomic_uint* status [[buffer(2)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
        << (wide ? "  if ((in[gid] & 0x7ff0000000000000ul) == 0x7ff0000000000000ul)\n"
                 : "  if ((in[gid] & 0x7f800000u) == 0x7f800000u)\n")
        << "    atomic_store_explicit(status, 1u, memory_order_relaxed);\n"
           "}\n";
    return out.str();
}

std::string msl_integer_half_helpers(int dtype) {
    const bool bfloat=dtype==QCORE_DTYPE_BFLOAT16;
    const auto precision=bfloat?8:11,fraction=precision-1,bias=bfloat?127:15,limit=bfloat?127:15;
    return "ushort qi(ulong magnitude,bool negative){uint sign=negative?0x8000u:0u;"
        "if(magnitude==0ul)return ushort(sign);int bits=64-int(clz(magnitude));"
        "int shift=bits-"+std::to_string(precision)+";ulong kept=shift>0?magnitude>>uint(shift):magnitude<<uint(-shift);"
        "if(shift>0){ulong rest=magnitude&((1ul<<uint(shift))-1ul),midpoint=1ul<<uint(shift-1);"
        "if(rest>midpoint||(rest==midpoint&&(kept&1ul)!=0ul))++kept;}"
        "int exponent=bits-1;if(kept==(1ul<<"+std::to_string(precision)+")){kept>>=1;++exponent;}"
        "if(exponent>"+std::to_string(limit)+")return ushort(sign|"+(bfloat?"0x7f80u":"0x7c00u")+");"
        "return ushort(sign|(uint(exponent+"+std::to_string(bias)+")<<"+std::to_string(fraction)+")|"
        "uint(kept&((1ul<<"+std::to_string(fraction)+")-1ul)));}\n";
}

std::string msl_half_cast_source(int source_dtype, int target_dtype) {
    const bool integer=gpu_integer_dtype(source_dtype);
    const std::string src=integer?msl_integer_type(source_dtype):half_dtype(source_dtype)?"ushort":"float";
    const auto dst=half_dtype(target_dtype)?"ushort":"float";
    auto helpers=msl_half_helpers(half_dtype(source_dtype)?source_dtype:target_dtype);
    if(half_dtype(source_dtype)&&half_dtype(target_dtype)&&source_dtype!=target_dtype){
        auto target=msl_half_helpers(target_dtype);
        msl_replace(target,"qread(","target_read(");
        msl_replace(target,"qbits(","target_bits(");
        helpers+=target;
    }
    if(integer)helpers+=msl_integer_half_helpers(target_dtype);
    const std::string read=half_dtype(source_dtype)?"qread(in[gid])":"float(in[gid])";
    const std::string write=integer?
        (gpu_integer_signed(source_dtype)?"qi(in[gid]<0?0ul-ulong(in[gid]):ulong(in[gid]),in[gid]<0)":"qi(ulong(in[gid]),false)"):
        half_dtype(target_dtype)?
        (half_dtype(source_dtype)&&source_dtype!=target_dtype?"target_bits(v)":"qbits(v)"):"v";
    const auto mask=target_dtype==QCORE_DTYPE_FLOAT16?"0x7c00u":"0x7f80u";
    return "#include <metal_stdlib>\nusing namespace metal;\n"+helpers+
        "kernel void q_kernel(device "+dst+"* out [[buffer(0)]],const device "+src+
        "* in [[buffer(1)]],constant ulong& count [[buffer(2)]],device atomic_uint* status [[buffer(3)]],"
        "uint gid [[thread_position_in_grid]]) { if(ulong(gid)>=count)return; float v="+read+
        "; "+dst+" r="+write+"; out[gid]=r;"+
        (half_dtype(target_dtype)?"if((r&"+std::string(mask)+")=="+mask+")atomic_store_explicit(status,1u,memory_order_relaxed);":"")+"}\n";
}

std::string msl_named_helpers(std::string source,const std::string& prefix){
    for(const auto* name:{"qread","qbits","qr","cr","cw"})
        msl_replace(source,std::string(name)+"(",prefix+name+"(");
    return source;
}

std::string msl_complex_cast_source(int source_dtype,int target_dtype){
    const auto target_part=abi::complex_part_dtype_code(target_dtype);
    const bool source_complex=abi::complex_dtype_code(source_dtype);
    const bool source_integer=gpu_integer_dtype(source_dtype);
    const auto source_part=source_complex?abi::complex_part_dtype_code(source_dtype):source_dtype;
    const std::string src=source_complex?(half_dtype(source_part)?"ushort2":"float2"):
        source_integer?msl_integer_type(source_dtype):half_dtype(source_part)?"ushort":"float";
    const std::string dst=half_dtype(target_part)?"ushort2":"float2";
    auto helpers=msl_named_helpers(msl_complex_helpers(target_dtype),"t");
    std::string read;
    if(source_complex){
        helpers+=msl_named_helpers(msl_complex_helpers(source_dtype),"s");
        read="scr(in[gid])";
    }else if(half_dtype(source_part)){
        helpers+=msl_named_helpers(msl_half_helpers(source_part),"s");
        read="float2(sqread(in[gid]),0.0f)";
    }else read="float2(float(in[gid]),0.0f)";
    std::string write="tcw(v)";
    if(source_integer&&half_dtype(target_part)){
        helpers+=msl_integer_half_helpers(target_part);
        write="ushort2("+std::string(gpu_integer_signed(source_dtype)?
            "qi(in[gid]<0?0ul-ulong(in[gid]):ulong(in[gid]),in[gid]<0)":"qi(ulong(in[gid]),false)")+",0u)";
    }
    return "#include <metal_stdlib>\nusing namespace metal;\n"+helpers+
        "kernel void q_kernel(device "+dst+"* out [[buffer(0)]],const device "+src+
        "* in [[buffer(1)]],constant ulong& count [[buffer(2)]],device atomic_uint* status [[buffer(3)]],"
        "uint gid [[thread_position_in_grid]]){if(ulong(gid)>=count)return;float2 v="+read+";"+
        dst+" value="+write+";float2 checked=tcr(value);"
        "if(!all(isfinite(checked)))atomic_store_explicit(status,1u,memory_order_relaxed);out[gid]=value;}\n";
}

std::string msl_real_integer_cast_source(int source,int target){
    const int fraction=source==QCORE_DTYPE_FLOAT64?52:source==QCORE_DTYPE_FLOAT32?23:source==QCORE_DTYPE_FLOAT16?10:7;
    const int exponent=source==QCORE_DTYPE_FLOAT64?11:source==QCORE_DTYPE_FLOAT16?5:8;
    const int bias=(1<<(exponent-1))-1,width=int(compute_dtype_width(target))*8;
    const std::string src=source==QCORE_DTYPE_FLOAT64?"ulong":source==QCORE_DTYPE_FLOAT32?"uint":"ushort";
    const std::string dst=msl_integer_type(target);
    return "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void q_kernel(device "+dst+"* out [[buffer(0)]],const device "+src+
        "* in [[buffer(1)]],constant ulong& count [[buffer(2)]],device atomic_uint* status [[buffer(3)]],"
        "uint gid [[thread_position_in_grid]]){if(ulong(gid)>=count)return;ulong bits=ulong(in[gid]);"
        "bool negative=(bits>>("+std::to_string(fraction+exponent)+"))!=0ul;"
        "ulong fraction=bits&((1ul<<"+std::to_string(fraction)+")-1ul);"
        "int raw_exponent=int((bits>>"+std::to_string(fraction)+")&"+std::to_string((1<<exponent)-1)+"ul);"
        "if(raw_exponent==0&&fraction==0ul){out[gid]="+dst+"(0);return;}"
        "int e=raw_exponent-"+std::to_string(bias)+";ulong mantissa=fraction|(1ul<<"+std::to_string(fraction)+");"
        "if(e<0||(e<"+std::to_string(fraction)+"&&(mantissa&((1ul<<uint("+std::to_string(fraction)+"-e))-1ul))!=0ul)){"
        "atomic_fetch_or_explicit(status,2u,memory_order_relaxed);return;}"
        "bool range="+(gpu_integer_signed(target)?
          "e>"+std::to_string(width-1)+"||(e=="+std::to_string(width-1)+"&&(!negative||fraction!=0ul))":
          "negative||e>="+std::to_string(width))+";"
        "if(range){atomic_fetch_or_explicit(status,1u,memory_order_relaxed);return;}"
        "ulong magnitude=e>="+std::to_string(fraction)+"?mantissa<<uint(e-"+std::to_string(fraction)+"):"
        "mantissa>>uint("+std::to_string(fraction)+"-e);out[gid]="+dst+"(negative?0ul-magnitude:magnitude);}\n";
}

std::string msl_float_unary_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  out[gid] = -in[gid];\n"
           "}\n";
}

#endif // __APPLE__

bool real_compute_unavailable(const Buffer* buffer, const char* operation,
                              int dtype, std::string& error) {
    const auto* info = buffer ? find(buffer->global_index) : nullptr;
    std::ostringstream out;
    out << operation << " is not supported on ";
    if (info) out << backend_display_name(info->backend);
    else out << "this GPU backend";
    out << " for tensor dtype " << dtype;
    error = out.str();
    return false;
}

} // namespace

bool validate_finite(Buffer* buffer, std::size_t byte_offset, int dtype, std::size_t count,
                     const std::string& message, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (!abi::real_dtype_code(dtype) ||
        byte_offset % width != 0 ||
        !compute_buffer_range(buffer, byte_offset / width, count, width, error)) {
        if (error.empty()) error = "invalid GPU finite validation";
        return false;
    }
    if (count == 0) return true;
    counters::add(counters::Id::FiniteValidations); // qcount
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (buffer->backend == Backend::Test) {
        // A synchronous backend: the failure is reported now.
        for (std::size_t i = 0; i < count; ++i) {
            const auto* slot = buffer->test_data.data() + byte_offset + i * width;
            bool finite = true;
            if (width == 8) {
                double value{};
                std::memcpy(&value, slot, 8);
                finite = std::isfinite(value);
            } else if (width == 2) {
                std::uint16_t bits{};
                std::memcpy(&bits,slot,2);
                finite=abi::half::is_finite(bits,dtype==QCORE_DTYPE_FLOAT16?abi::half::binary16:abi::half::bfloat16);
            } else {
                float value{};
                std::memcpy(&value, slot, 4);
                finite = std::isfinite(value);
            }
            if (!finite) {
                error = message;
                return false;
            }
        }
        return true;
    }
#endif
    std::unique_ptr<Buffer,void(*)(Buffer*)> status(
        acquire_validation_status(buffer->global_index,error),
        [](Buffer* value){quidra::device::release(value);});
    if(!status)return false;
    if(half_dtype(dtype) && numeric_gpu_backend(buffer)) {
        if(!launch_numeric(buffer,{.left=buffer,.left_offset=byte_offset,.count=count,.dtype=dtype,
                                   .mode=7,.status=status.get()},error))return false;
        defer_validation(status.release(),message);return true;
    }
    const auto n = static_cast<std::uint64_t>(count);
    if (buffer->backend == Backend::Cuda) {
        auto in_ptr = buffer->cuda_pointer + byte_offset;
        auto status_ptr = status->cuda_pointer;
        void* args[] = {&in_ptr, const_cast<std::uint64_t*>(&n), &status_ptr};
        if (!launch_ptx_1d(buffer, "validate-finite-" + std::to_string(width),
                           ptx_validate_finite_kernel(width), count, args, error))
            return false;
        defer_validation(status.release(), message);
        return true;
    }
    if (buffer->backend == Backend::Hip) {
        auto in_ptr = static_cast<unsigned char*>(buffer->pointer) + byte_offset;
        auto status_ptr = status->pointer;
        void* args[] = {&in_ptr, const_cast<std::uint64_t*>(&n), &status_ptr};
        if (!launch_hip_1d(buffer, "validate-finite-" + std::to_string(width),
                           hip_validate_finite_source(width), count, args, error))
            return false;
        defer_validation(status.release(), message);
        return true;
    }
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal) {
        auto pipeline = metal_pipeline(
            buffer, metal_pipeline_id(MetalKernel::ValidateFinite, static_cast<unsigned>(dtype)),
            [&] { return msl_validate_finite_source(dtype); }, error);
        if (!pipeline) return false;
        if (!dispatch_metal_1d(buffer, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder, buffer, byte_offset, 0);
                [encoder setBytes:&n length:sizeof(n) atIndex:1];
                metal_bind(encoder, status, 0, 2);
            }, error)) return false;
        defer_validation(status.release(), message);
        return true;
    }
#endif
    return real_compute_unavailable(buffer, "finite validation", dtype, error);
}

bool compute_fill_ones(Buffer* output, int dtype, std::size_t count,
                       std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (!compute_buffer_range(output, 0, count, width, error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        counters::add(counters::Id::CoreDispatches); // qcount
        for (std::size_t i = 0; i < count; ++i) {
            auto* slot = output->test_data.data() + i * width;
            switch (dtype) {
                case QCORE_DTYPE_INT64: { std::int64_t v=1; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_INT8: { std::int8_t v=1; std::memcpy(slot,&v,1); break; }
                case QCORE_DTYPE_INT16: { std::int16_t v=1; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_INT32: { std::int32_t v=1; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_UINT8: { std::uint8_t v=1; std::memcpy(slot,&v,1); break; }
                case QCORE_DTYPE_UINT16: { std::uint16_t v=1; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_UINT32: { std::uint32_t v=1; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_UINT64: { std::uint64_t v=1; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_FLOAT64: { double v=1.0; std::memcpy(slot,&v,8); break; }
                case QCORE_DTYPE_FLOAT32: { float v=1.0F; std::memcpy(slot,&v,4); break; }
                case QCORE_DTYPE_FLOAT16: { std::uint16_t v=0x3c00; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_BFLOAT16: { std::uint16_t v=0x3f80; std::memcpy(slot,&v,2); break; }
                case QCORE_DTYPE_COMPLEX_FLOAT16: { std::uint16_t v[2]={0x3c00,0}; std::memcpy(slot,v,4);break;}
                case QCORE_DTYPE_COMPLEX_BFLOAT16: { std::uint16_t v[2]={0x3f80,0}; std::memcpy(slot,v,4);break;}
                case QCORE_DTYPE_COMPLEX_FLOAT32: { float v[2]={1.0f,0.0f};std::memcpy(slot,v,8);break;}
                case QCORE_DTYPE_COMPLEX_FLOAT64: { double v[2]={1.0,0.0};std::memcpy(slot,v,16);break;}
                default: error="invalid tensor dtype"; return false;
            }
        }
        return true;
    }
#endif
    if(additional_dtype(dtype) && numeric_gpu_backend(output))
        return launch_numeric(output,{.count=count,.dtype=dtype,.mode=8},error);
    std::uint64_t bits = 1;
    if(dtype==QCORE_DTYPE_COMPLEX_FLOAT64)bits=0x3ff0000000000000ull;
    else if(dtype==QCORE_DTYPE_COMPLEX_FLOAT16)bits=0x3c00u;
    else if(dtype==QCORE_DTYPE_COMPLEX_BFLOAT16)bits=0x3f80u;
    else if(dtype==QCORE_DTYPE_COMPLEX_FLOAT32)bits=0x3f800000u;
    else if (dtype == QCORE_DTYPE_FLOAT64) {
        const double one = 1.0;
        std::memcpy(&bits, &one, sizeof(one));
    } else if (half_dtype(dtype)) {
        bits = dtype == QCORE_DTYPE_FLOAT16 ? 0x3c00u : 0x3f80u;
    } else if (dtype == QCORE_DTYPE_FLOAT32) {
        const float one = 1.0F;
        std::uint32_t raw{};
        std::memcpy(&raw, &one, sizeof(one));
        bits = raw;
    }
    if (output->backend == Backend::Cuda) {
        auto ptx = ptx_fill_kernel(width, bits);
        auto out_ptr = output->cuda_pointer;
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr, &n};
        return launch_ptx_1d(
            output, "fill-one-" + std::to_string(dtype), ptx, count, args, error);
    }
    if (output->backend == Backend::Hip) {
        auto source=hip_fill_source(width,bits);
        auto out_ptr=output->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&n};
        return launch_hip_1d(
            output,"fill-one-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if (output->backend == Backend::Metal) {
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::FillOne, static_cast<unsigned>(dtype)),
            [&] { return msl_fill_source(width, bits); }, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder,output,0,0);
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
        counters::add(counters::Id::CoreDispatches); // qcount
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
    if(additional_dtype(dtype) && numeric_gpu_backend(output))
        return launch_numeric(output,{.left=source,.positions=indices.get(),.count=count,.dtype=dtype,.mode=9},error);
    if (output->backend == Backend::Cuda) {
        auto ptx = ptx_gather_kernel(width);
        auto out_ptr = output->cuda_pointer;
        auto src_ptr = source->cuda_pointer;
        auto idx_ptr = indices->cuda_pointer;
        auto n = static_cast<std::uint64_t>(count);
        void* args[] = {&out_ptr, &src_ptr, &idx_ptr, &n};
        return launch_ptx_1d(
            output, "gather-" + std::to_string(width), ptx, count, args, error);
    }
    if (output->backend == Backend::Hip) {
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
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::Gather, static_cast<unsigned>(width)),
            [&] { return msl_gather_source(width); }, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder,output,0,0);
                metal_bind(encoder,source,0,1);
                metal_bind(encoder,indices,0,2);
                [encoder setBytes:&n length:sizeof(n) atIndex:3];
            }, error);
    }
#endif
    return real_compute_unavailable(output, "tensor.contiguous", dtype, error);
}


bool compute_gather_backward(
    Buffer* input_gradient,const Buffer* output_gradient,int dtype,
    const std::uint64_t* source_indices,std::size_t source_count,
    std::size_t output_count,std::string& error,bool round_each) {
    if(!abi::numeric_dtype_code(dtype)){
        error="tensor scatter/gather backward requires a numeric tensor dtype";
        return false;
    }
    const auto width=compute_dtype_width(dtype);
    if((!source_indices&&output_count!=0)||
       !compute_same_device(input_gradient,output_gradient,error)||
       !compute_buffer_range(input_gradient,0,source_count,width,error)||
       !compute_buffer_range(output_gradient,0,output_count,width,error))
        return false;
    if(source_count==std::numeric_limits<std::size_t>::max()){
        error="tensor scatter/gather backward source is too large";
        return false;
    }

    std::vector<std::uint64_t> offsets(source_count+1,0);
    std::vector<std::uint64_t> positions(output_count,0);
    for(std::size_t i=0;i<output_count;++i){
        if(source_indices[i]>=source_count){
            error="tensor scatter/gather backward source index is out of bounds for the input";
            return false;
        }
        ++offsets[static_cast<std::size_t>(source_indices[i])+1];
    }
    for(std::size_t i=1;i<offsets.size();++i) offsets[i]+=offsets[i-1];
    auto cursor=offsets;
    for(std::size_t i=0;i<output_count;++i){
        const auto source=static_cast<std::size_t>(source_indices[i]);
        positions[static_cast<std::size_t>(cursor[source]++)]=
            static_cast<std::uint64_t>(i);
    }

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(input_gradient->backend==Backend::Test){
        counters::add(counters::Id::CoreDispatches); // qcount
        std::fill(input_gradient->test_data.begin(),input_gradient->test_data.end(),0);
        const auto run=[&](auto tag)->bool{
            using T=decltype(tag);
            using C=std::conditional_t<std::is_same_v<T,abi::half::Real16>||std::is_same_v<T,abi::half::Real16b>,float,T>;
            for(std::size_t source=0;source<source_count;++source){
                C total{};
                for(auto p=offsets[source];p<offsets[source+1];++p){
                    T value{};C next{};
                    std::memcpy(
                        &value,
                        output_gradient->test_data.data()+
                        static_cast<std::size_t>(
                            positions[static_cast<std::size_t>(p)])*sizeof(T),
                        sizeof(T));
                    if(!fake_add(total,static_cast<C>(value),next)){
                        error="tensor.scatter integer arithmetic overflow";
                        return false;
                    }
                    if constexpr (std::is_same_v<T,abi::half::Real16>||std::is_same_v<T,abi::half::Real16b>) {
                        total=round_each?static_cast<C>(static_cast<T>(next)):next;
                    } else total=next;
                }
                const T result=static_cast<T>(total);
                std::memcpy(
                    input_gradient->test_data.data()+source*sizeof(T),
                    &result,sizeof(T));
            }
            return true;
        };
        switch(dtype){
            case QCORE_DTYPE_INT64: return run(std::int64_t{});
            case QCORE_DTYPE_INT8: return run(std::int8_t{});
            case QCORE_DTYPE_INT16: return run(std::int16_t{});
            case QCORE_DTYPE_INT32: return run(std::int32_t{});
            case QCORE_DTYPE_UINT8: return run(std::uint8_t{});
            case QCORE_DTYPE_UINT16: return run(std::uint16_t{});
            case QCORE_DTYPE_UINT32: return run(std::uint32_t{});
            case QCORE_DTYPE_UINT64: return run(std::uint64_t{});
            case QCORE_DTYPE_FLOAT64: return run(double{});
            case QCORE_DTYPE_FLOAT32: return run(float{});
            case QCORE_DTYPE_FLOAT16: return run(abi::half::Real16{});
            case QCORE_DTYPE_BFLOAT16: return run(abi::half::Real16b{});
            default:error="invalid tensor scatter dtype";return false;
        }
    }
#endif

    const auto positions_bytes=
        std::max<std::size_t>(std::size_t{1},positions.size())*
        sizeof(std::uint64_t);
    std::unique_ptr<Buffer, void(*)(Buffer*)> offsets_buffer(
        allocate(input_gradient->global_index,
                 offsets.size()*sizeof(std::uint64_t),error),
        [](Buffer* value){ quidra::device::release(value); });
    if(!offsets_buffer) return false;
    std::unique_ptr<Buffer, void(*)(Buffer*)> positions_buffer(
        allocate(input_gradient->global_index,positions_bytes,error),
        [](Buffer* value){ quidra::device::release(value); });
    if(!positions_buffer) return false;
    if(!copy_from_host(offsets_buffer.get(),0,offsets.data(),
                       offsets.size()*sizeof(std::uint64_t),error) ||
       (!positions.empty()&&!copy_from_host(
           positions_buffer.get(),0,positions.data(),
           positions.size()*sizeof(std::uint64_t),error)))
        return false;

    if(half_dtype(dtype) && numeric_gpu_backend(input_gradient))
        return launch_numeric(input_gradient,{.gradient=output_gradient,.offsets=offsets_buffer.get(),
            .positions=positions_buffer.get(),.count=source_count,.dtype=dtype,.mode=round_each?11:4},error);
    if(gpu_integer_dtype(dtype)&&input_gradient->backend==Backend::Cuda){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input_gradient->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_gather_backward_source(dtype);
        auto out_ptr=input_gradient->cuda_pointer;
        auto grad_ptr=output_gradient->cuda_pointer;
        auto offsets_ptr=offsets_buffer->cuda_pointer;
        auto positions_ptr=positions_buffer->cuda_pointer;
        auto status_ptr=status->cuda_pointer;
        auto count=static_cast<std::uint64_t>(source_count);
        void* args[]={
            &out_ptr,&grad_ptr,&offsets_ptr,&positions_ptr,&count,&status_ptr};
        if(!launch_ptx_1d(
               input_gradient,"gather-backward-int-"+std::to_string(dtype),
               ptx,source_count,args,error))
            return false;
        defer_validation(
            status.release(),"tensor.scatter integer arithmetic overflow");
        return true;
    }
    if((dtype==QCORE_DTYPE_FLOAT64||dtype==QCORE_DTYPE_FLOAT32)&&input_gradient->backend==Backend::Cuda){
        auto ptx=ptx_gather_backward_kernel(dtype);
        auto out_ptr=input_gradient->cuda_pointer;
        auto grad_ptr=output_gradient->cuda_pointer;
        auto offsets_ptr=offsets_buffer->cuda_pointer;
        auto positions_ptr=positions_buffer->cuda_pointer;
        auto count=static_cast<std::uint64_t>(source_count);
        void* args[]={&out_ptr,&grad_ptr,&offsets_ptr,&positions_ptr,&count};
        return launch_ptx_1d(
            input_gradient,"gather-backward-"+std::to_string(dtype),
            ptx,source_count,args,error);
    }
    if(hip_integer_dtype(dtype)&&input_gradient->backend==Backend::Hip){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input_gradient->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source=hip_integer_gather_backward_source(dtype);
        auto out_ptr=input_gradient->pointer;
        auto grad_ptr=output_gradient->pointer;
        auto offsets_ptr=offsets_buffer->pointer;
        auto positions_ptr=positions_buffer->pointer;
        auto status_ptr=status->pointer;
        auto count=static_cast<std::uint64_t>(source_count);
        void* args[]={
            &out_ptr,&grad_ptr,&offsets_ptr,&positions_ptr,&count,&status_ptr};
        if(!launch_hip_1d(
               input_gradient,"gather-backward-int-"+std::to_string(dtype),
               source,source_count,args,error))
            return false;
        defer_validation(
            status.release(),"tensor.scatter integer arithmetic overflow");
        return true;
    }
    if((dtype==QCORE_DTYPE_FLOAT64||dtype==QCORE_DTYPE_FLOAT32)&&input_gradient->backend==Backend::Hip){
        auto source=hip_gather_backward_source(dtype);
        auto out_ptr=input_gradient->pointer;
        auto grad_ptr=output_gradient->pointer;
        auto offsets_ptr=offsets_buffer->pointer;
        auto positions_ptr=positions_buffer->pointer;
        auto count=static_cast<std::uint64_t>(source_count);
        void* args[]={&out_ptr,&grad_ptr,&offsets_ptr,&positions_ptr,&count};
        return launch_hip_1d(
            input_gradient,"gather-backward-"+std::to_string(dtype),
            source,source_count,args,error);
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(dtype)&&input_gradient->backend==Backend::Metal){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(input_gradient->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline = metal_pipeline(
            input_gradient, metal_pipeline_id(MetalKernel::GatherBackwardInt, static_cast<unsigned>(dtype)),
            [&] { return msl_integer_gather_backward_source(dtype); }, error);
        if(!pipeline)return false;
        auto count=static_cast<std::uint64_t>(source_count);
        if(!dispatch_metal_1d(
               input_gradient,pipeline,source_count,
               [&](id<MTLComputeCommandEncoder> encoder){
                   metal_bind(encoder,input_gradient,0,0);
                   metal_bind(encoder,output_gradient,0,1);
                   metal_bind(encoder,offsets_buffer,0,2);
                   metal_bind(encoder,positions_buffer,0,3);
                   [encoder setBytes:&count length:sizeof(count) atIndex:4];
                   metal_bind(encoder,status,0,5);
               },error))
            return false;
        defer_validation(
            status.release(),"tensor.scatter integer arithmetic overflow");
        return true;
    }
    if(metal_real_dtype(dtype)&&input_gradient->backend==Backend::Metal){
        auto pipeline = metal_pipeline(
            input_gradient, metal_pipeline_id(MetalKernel::GatherBackwardF32, static_cast<unsigned>(dtype), half_dtype(dtype)&&round_each?1u:0u),
            [&] { auto source=msl_half_storage(msl_gather_backward_source(), dtype);
                if(half_dtype(dtype)&&round_each)msl_replace(source,"total+=qread(grad[positions[p]]);", "total = qread(qbits(total + qread(grad[positions[p]])));");
                return source; }, error);
        if(!pipeline)return false;
        auto count=static_cast<std::uint64_t>(source_count);
        return dispatch_metal_1d(
            input_gradient,pipeline,source_count,
            [&](id<MTLComputeCommandEncoder> encoder){
                metal_bind(encoder,input_gradient,0,0);
                metal_bind(encoder,output_gradient,0,1);
                metal_bind(encoder,offsets_buffer,0,2);
                metal_bind(encoder,positions_buffer,0,3);
                [encoder setBytes:&count length:sizeof(count) atIndex:4];
            },error);
    }
#endif
    return real_compute_unavailable(
        input_gradient,"tensor.scatter/gather backward",dtype,error);
}

// ---------------------------------------------------------------------------
// Strided tensor access. An operand of a logical row-major `shape` reads its
// element i at base + sum_axis coordinate_axis(i) * strides[axis] (elements),
// so broadcast axes (stride 0) and transposed or stepped views need no
// host-built gather index map. Metal and the test backend implement it; CUDA
// and HIP keep their gather-index paths (supports_strided_compute() is false).

namespace {

// Kernel-side description of a strided view: extent-1 axes never contribute
// to an index and are dropped, which leaves at most 64 axes.
struct StridedLayout {
    std::vector<std::uint32_t> extents;
    std::vector<std::vector<std::uint64_t>> strides;
};

// Validates every operand view against its buffer and builds the compact
// layout. Each view must reference only elements in [0, buffer element count):
// the largest index base + sum((extent - 1) * stride) is computed with
// overflow checks. Strides must be non-negative.
bool strided_layout(const std::vector<long long>& shape,
                    const std::vector<const std::vector<long long>*>& operand_strides,
                    const std::vector<const Buffer*>& buffers,
                    const std::vector<std::size_t>& bases,
                    std::size_t width, std::size_t& count,
                    StridedLayout& layout, std::string& error) {
    count = 1;
    for (const auto extent : shape) {
        if (extent < 0) {
            error = "invalid GPU strided tensor shape";
            return false;
        }
        const auto value = static_cast<std::size_t>(extent);
        if (value != 0 && count > std::numeric_limits<std::size_t>::max() / value) {
            error = "GPU strided tensor element count overflow";
            return false;
        }
        count *= value;
    }
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        error = "GPU strided tensor is too large";
        return false;
    }
    layout.extents.clear();
    layout.strides.assign(operand_strides.size(), {});
    for (std::size_t operand = 0; operand < operand_strides.size(); ++operand) {
        const auto& strides = *operand_strides[operand];
        if (strides.size() != shape.size()) {
            error = "GPU strided tensor rank mismatch";
            return false;
        }
        std::size_t last = bases[operand];
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
            if (strides[axis] < 0) {
                error = "GPU strided tensor requires non-negative strides";
                return false;
            }
            if (shape[axis] <= 1) continue;
            const auto span = static_cast<std::size_t>(shape[axis] - 1);
            const auto step = static_cast<std::size_t>(strides[axis]);
            if (step != 0 && span > std::numeric_limits<std::size_t>::max() / step) {
                error = "GPU strided tensor index overflow";
                return false;
            }
            if (span * step > std::numeric_limits<std::size_t>::max() - last) {
                error = "GPU strided tensor index overflow";
                return false;
            }
            last += span * step;
        }
        if (count != 0) {
            if (last == std::numeric_limits<std::size_t>::max() ||
                last + 1 > std::numeric_limits<std::size_t>::max() / width ||
                !range_ok(*buffers[operand], 0, (last + 1) * width)) {
                error = "GPU strided tensor view exceeds device buffer";
                return false;
            }
        }
    }
    for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        if (shape[axis] == 1) continue;
        layout.extents.push_back(static_cast<std::uint32_t>(shape[axis]));
        for (std::size_t operand = 0; operand < operand_strides.size(); ++operand)
            layout.strides[operand].push_back(
                static_cast<std::uint64_t>((*operand_strides[operand])[axis]));
    }
    return true;
}

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
// Host reference for the test backend: the same index arithmetic as the
// device kernels.
std::size_t strided_index(const StridedLayout& layout, std::size_t operand,
                          std::size_t base, std::size_t logical) {
    std::size_t index = base;
    for (std::size_t axis = layout.extents.size(); axis-- > 0;) {
        const auto extent = static_cast<std::size_t>(layout.extents[axis]);
        index += (logical % extent) *
                 static_cast<std::size_t>(layout.strides[operand][axis]);
        logical /= extent;
    }
    return index;
}

template <typename T>
bool fake_binary_strided(Buffer* output, const Buffer* left, std::size_t left_base,
                         const Buffer* right, std::size_t right_base,
                         const void* scalar, int scalar_side, int operation,
                         const StridedLayout& layout, std::size_t count,
                         std::string& error) {
    T scalar_value{};
    if (scalar_side != abi::scalar_side::none) std::memcpy(&scalar_value, scalar, sizeof(T));
    for (std::size_t i = 0; i < count; ++i) {
        T av{};
        T bv{};
        std::memcpy(&av, left->test_data.data() +
                             strided_index(layout, 0, left_base, i) * sizeof(T),
                    sizeof(T));
        if (scalar_side == abi::scalar_side::none) {
            std::memcpy(&bv, right->test_data.data() +
                                 strided_index(layout, 1, right_base, i) * sizeof(T),
                        sizeof(T));
        }
        T lhs = scalar_side == abi::scalar_side::left ? scalar_value : av;
        T rhs = scalar_side == abi::scalar_side::right ? scalar_value : (scalar_side == abi::scalar_side::left ? av : bv);
        T result{};
        if constexpr (std::is_integral_v<T>) {
            if (operation == abi::tensor_binary_opcode::power && lhs == T{0} && rhs == T{0}) {
                error = std::string(abi::failure_reason_info(abi::FailureReason::zero_power_zero).message);
                return false;
            }
        }
        if (!fake_binary_value(lhs, rhs, operation, result)) {
            error = operation == abi::tensor_binary_opcode::divide || operation == abi::tensor_binary_opcode::remainder
                ? "invalid tensor division/remainder or integer overflow"
                : operation == abi::tensor_binary_opcode::power
                    ? "invalid tensor power domain or integer overflow"
                    : "tensor integer arithmetic overflow";
            return false;
        }
        if constexpr (!std::is_integral_v<T>) {
            if (const auto bits = real_element_status(lhs, rhs, operation, result)) {
                error = real_status_message(std::is_same_v<T, float> ? QCORE_DTYPE_FLOAT32
                    : std::is_same_v<T, double> ? QCORE_DTYPE_FLOAT64
                    : std::is_same_v<T, abi::half::Real16> ? QCORE_DTYPE_FLOAT16 : QCORE_DTYPE_BFLOAT16,
                                            operation, bits);
                return false;
            }
        }
        std::memcpy(output->test_data.data() + i * sizeof(T), &result, sizeof(T));
    }
    return true;
}
#endif

#ifdef __APPLE__
// Index arithmetic shared by the strided Metal kernels: thread `gid` of a
// row-major grid over `extents` reads base + sum coordinate * stride.
std::string msl_strided_index(const char* index, const char* base,
                              const char* strides) {
    std::ostringstream out;
    out << "  ulong " << index << " = " << base << ";\n"
           "  { uint rest = gid;\n"
           "    for (uint axis = rank; axis > 0; --axis) {\n"
           "      uint extent = extents[axis - 1];\n"
           "      " << index << " += ulong(rest % extent) * " << strides << "[axis - 1];\n"
           "      rest /= extent;\n"
           "    } }\n";
    return out.str();
}

std::string msl_strided_copy_source(std::size_t width) {
    const auto type = msl_raw_type(width);
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device " << type << "* out [[buffer(0)]], "
           "const device " << type << "* src [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "constant uint& rank [[buffer(3)]], "
           "constant uint* extents [[buffer(4)]], "
           "constant ulong* strides [[buffer(5)]], "
           "constant ulong& base [[buffer(6)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
        << msl_strided_index("index", "base", "strides")
        << "  out[gid] = src[index];\n"
           "}\n";
    return out.str();
}

// The arithmetic statements are those of msl_float_binary_source(); only the
// operand element indices differ, so results are bitwise those of a gather
// followed by the dense kernel.
std::string msl_float_binary_strided_source(int operation, int scalar_side) {
    std::ostringstream out;
    out << "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* left [[buffer(1)]], "
           "const device float* right [[buffer(2)]], "
           "constant ulong& scalar_bits [[buffer(3)]], "
           "constant ulong& count [[buffer(4)]], "
           "constant uint& rank [[buffer(5)]], "
           "constant uint* extents [[buffer(6)]], "
           "constant ulong* left_strides [[buffer(7)]], "
           "constant ulong* right_strides [[buffer(8)]], "
           "constant ulong& left_base [[buffer(9)]], "
           "constant ulong& right_base [[buffer(10)]], "
           "device atomic_uint* status [[buffer(11)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
        << msl_strided_index("left_index", "left_base", "left_strides");
    if (scalar_side == abi::scalar_side::none)
        out << msl_strided_index("right_index", "right_base", "right_strides");
    out << "  float a = left[left_index];\n"
           "  float s = as_type<float>(uint(scalar_bits));\n";
    if (scalar_side == abi::scalar_side::none)
        out << msl_real_element(operation, "a", "right[right_index]");
    else if (scalar_side == abi::scalar_side::left)
        out << msl_real_element(operation, "s", "a");
    else
        out << msl_real_element(operation, "a", "s");
    out << "}\n";
    return out.str();
}

// setBytes needs a non-empty range; rank-0 layouts bind one unused element.
template <typename T>
const T* metal_array_bytes(const std::vector<T>& values, std::size_t& length) {
    static const T unused{};
    if (values.empty()) {
        length = sizeof(T);
        return &unused;
    }
    length = values.size() * sizeof(T);
    return values.data();
}
#endif

} // namespace

bool supports_strided_compute(const Buffer* buffer) {
    if (!buffer) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (buffer->backend == Backend::Test) return true;
#endif
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal) return true;
#endif
    return false;
}

bool supports_strided_binary(const Buffer* buffer, int dtype, int operation) {
    if (!buffer) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (buffer->backend == Backend::Test) return dtype >= QCORE_DTYPE_INT64 && dtype <= QCORE_DTYPE_FLOAT32;
#endif
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal)
        return metal_real_dtype(dtype) && operation >= abi::tensor_binary_opcode::add && operation <= abi::tensor_binary_opcode::power && operation != abi::tensor_binary_opcode::remainder;
#endif
    (void)dtype;
    (void)operation;
    return false;
}

bool compute_strided_copy(Buffer* output, const Buffer* source, int dtype,
                          std::size_t source_base,
                          const std::vector<long long>& shape,
                          const std::vector<long long>& strides,
                          std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (width == 0 || !source) {
        error = "invalid GPU tensor compute buffer";
        return false;
    }
    if (!compute_same_device(output, source, error)) return false;
    std::size_t count = 0;
    StridedLayout layout;
    if (!strided_layout(shape, {&strides}, {source}, {source_base}, width,
                        count, layout, error) ||
        !compute_buffer_range(output, 0, count, width, error))
        return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(output->test_data.data() + i * width,
                        source->test_data.data() +
                            strided_index(layout, 0, source_base, i) * width,
                        width);
        }
        return true;
    }
#endif
#ifdef __APPLE__
    if (output->backend == Backend::Metal) {
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::StridedCopy, static_cast<unsigned>(width)),
            [&] { return msl_strided_copy_source(width); }, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        const auto rank = static_cast<std::uint32_t>(layout.extents.size());
        const auto base = static_cast<std::uint64_t>(source_base);
        std::size_t extents_length = 0;
        std::size_t strides_length = 0;
        const auto* extents_bytes = metal_array_bytes(layout.extents, extents_length);
        const auto* strides_bytes = metal_array_bytes(layout.strides[0], strides_length);
        return dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder, output, 0, 0);
                metal_bind(encoder, source, 0, 1);
                [encoder setBytes:&n length:sizeof(n) atIndex:2];
                [encoder setBytes:&rank length:sizeof(rank) atIndex:3];
                [encoder setBytes:extents_bytes length:extents_length atIndex:4];
                [encoder setBytes:strides_bytes length:strides_length atIndex:5];
                [encoder setBytes:&base length:sizeof(base) atIndex:6];
            }, error);
    }
#endif
    return real_compute_unavailable(output, "strided tensor copy", dtype, error);
}

bool compute_binary_strided(Buffer* output,
                            const Buffer* left, std::size_t left_base,
                            const std::vector<long long>& left_strides,
                            const Buffer* right, std::size_t right_base,
                            const std::vector<long long>& right_strides,
                            const void* scalar, int scalar_side, int dtype,
                            int operation, const std::vector<long long>& shape,
                            std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (width == 0 || operation < abi::tensor_binary_opcode::add || operation > abi::tensor_binary_opcode::power ||
        scalar_side < abi::scalar_side::none || scalar_side > abi::scalar_side::right ||
        (operation == abi::tensor_binary_opcode::power && scalar_side != abi::scalar_side::right) ||
        (scalar_side == abi::scalar_side::none && !right) ||
        (scalar_side != abi::scalar_side::none && !scalar) ||
        !supports_strided_binary(output, dtype, operation)) {
        error = "invalid GPU strided tensor binary operation";
        return false;
    }
    if (!compute_same_device(output, left, error) ||
        (scalar_side == abi::scalar_side::none && !compute_same_device(output, right, error)))
        return false;
    std::size_t count = 0;
    StridedLayout layout;
    const bool ok = scalar_side == abi::scalar_side::none
        ? strided_layout(shape, {&left_strides, &right_strides}, {left, right},
                         {left_base, right_base}, width, count, layout, error)
        : strided_layout(shape, {&left_strides}, {left}, {left_base}, width,
                         count, layout, error);
    if (!ok || !compute_buffer_range(output, 0, count, width, error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        switch (dtype) {
            case QCORE_DTYPE_INT64: return fake_binary_strided<std::int64_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_INT8: return fake_binary_strided<std::int8_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_INT16: return fake_binary_strided<std::int16_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_INT32: return fake_binary_strided<std::int32_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_UINT8: return fake_binary_strided<std::uint8_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_UINT16: return fake_binary_strided<std::uint16_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_UINT32: return fake_binary_strided<std::uint32_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_UINT64: return fake_binary_strided<std::uint64_t>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_FLOAT64: return fake_binary_strided<double>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_FLOAT32: return fake_binary_strided<float>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_FLOAT16: return fake_binary_strided<abi::half::Real16>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            case QCORE_DTYPE_BFLOAT16: return fake_binary_strided<abi::half::Real16b>(output,left,left_base,right,right_base,scalar,scalar_side,operation,layout,count,error);
            default: error="invalid tensor dtype"; return false;
        }
    }
#endif
#ifdef __APPLE__
    if (output->backend == Backend::Metal) {
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::BinaryStridedF32,
                                      static_cast<unsigned>(operation),
                                      static_cast<unsigned>(scalar_side), static_cast<unsigned>(dtype)),
            [&] { return msl_half_storage(msl_float_binary_strided_source(operation, scalar_side), dtype); },
            error);
        if (!pipeline) return false;
        std::uint64_t scalar_bits = 0;
        if (scalar_side != abi::scalar_side::none) std::memcpy(&scalar_bits, scalar, std::min(width,sizeof(scalar_bits)));
        const auto n = static_cast<std::uint64_t>(count);
        const auto rank = static_cast<std::uint32_t>(layout.extents.size());
        const auto lbase = static_cast<std::uint64_t>(left_base);
        const auto rbase = static_cast<std::uint64_t>(scalar_side == abi::scalar_side::none ? right_base : 0);
        const auto& right_layout =
            scalar_side == abi::scalar_side::none ? layout.strides[1] : layout.strides[0];
        std::size_t extents_length = 0;
        std::size_t left_length = 0;
        std::size_t right_length = 0;
        const auto* extents_bytes = metal_array_bytes(layout.extents, extents_length);
        const auto* left_bytes = metal_array_bytes(layout.strides[0], left_length);
        const auto* right_bytes = metal_array_bytes(right_layout, right_length);
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        if (!dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder, output, 0, 0);
                metal_bind(encoder, left, 0, 1);
                metal_bind(encoder, scalar_side == abi::scalar_side::none ? right : left, 0, 2);
                [encoder setBytes:&scalar_bits length:sizeof(scalar_bits) atIndex:3];
                [encoder setBytes:&n length:sizeof(n) atIndex:4];
                [encoder setBytes:&rank length:sizeof(rank) atIndex:5];
                [encoder setBytes:extents_bytes length:extents_length atIndex:6];
                [encoder setBytes:left_bytes length:left_length atIndex:7];
                [encoder setBytes:right_bytes length:right_length atIndex:8];
                [encoder setBytes:&lbase length:sizeof(lbase) atIndex:9];
                [encoder setBytes:&rbase length:sizeof(rbase) atIndex:10];
                metal_bind(encoder, status, 0, 11);
            }, error)) return false;
        defer_real_status(status.release(), dtype, operation);
        return true;
    }
#endif
    return real_compute_unavailable(output, "tensor arithmetic", dtype, error);
}

namespace {

#ifdef __APPLE__
// One thread per source element. The output elements broadcast from it are
// visited in ascending output index order and summed serially from +0.0,
// which is the order of the gather-backward kernel over its ascending CSR
// positions, so the bits are those of the gather path.
std::string msl_broadcast_backward_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* grad [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "constant uint& rank [[buffer(3)]], "
           "constant uint* source_extents [[buffer(4)]], "
           "constant uint* reduce_extents [[buffer(5)]], "
           "constant ulong* output_strides [[buffer(6)]], "
           "constant ulong& reduce_count [[buffer(7)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           "  if (ulong(gid) >= count) return;\n"
           "  ulong first = 0;\n"
           "  { uint rest = gid;\n"
           "    for (uint axis = rank; axis > 0; --axis) {\n"
           "      uint extent = source_extents[axis - 1];\n"
           "      first += ulong(rest % extent) * output_strides[axis - 1];\n"
           "      rest /= extent;\n"
           "    } }\n"
           "  float total = 0.0f;\n"
           "  for (ulong k = 0; k < reduce_count; ++k) {\n"
           "    ulong index = first;\n"
           "    ulong rest = k;\n"
           "    for (uint axis = rank; axis > 0; --axis) {\n"
           "      ulong extent = ulong(reduce_extents[axis - 1]);\n"
           "      index += (rest % extent) * output_strides[axis - 1];\n"
           "      rest /= extent;\n"
           "    }\n"
           "    total += grad[index];\n"
           "  }\n"
           "  out[gid] = total;\n"
           "}\n";
}
#endif

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
template <typename T>
void fake_broadcast_backward(Buffer* input_gradient, const Buffer* output_gradient,
                             const std::vector<std::uint32_t>& source_extents,
                             const std::vector<std::uint32_t>& reduce_extents,
                             const std::vector<std::uint64_t>& output_strides,
                             std::size_t source_count, std::size_t reduce_count) {
    using C=std::conditional_t<std::is_same_v<T,abi::half::Real16>||std::is_same_v<T,abi::half::Real16b>,float,T>;
    const auto rank = source_extents.size();
    for (std::size_t source = 0; source < source_count; ++source) {
        std::size_t first = 0;
        std::size_t rest = source;
        for (std::size_t axis = rank; axis-- > 0;) {
            first += (rest % source_extents[axis]) *
                     static_cast<std::size_t>(output_strides[axis]);
            rest /= source_extents[axis];
        }
        C total{};
        for (std::size_t k = 0; k < reduce_count; ++k) {
            std::size_t index = first;
            std::size_t remaining = k;
            for (std::size_t axis = rank; axis-- > 0;) {
                index += (remaining % reduce_extents[axis]) *
                         static_cast<std::size_t>(output_strides[axis]);
                remaining /= reduce_extents[axis];
            }
            T value{};
            std::memcpy(&value, output_gradient->test_data.data() + index * sizeof(T),
                        sizeof(T));
            total = static_cast<C>(total + static_cast<C>(value));
        }
        const T result=static_cast<T>(total);
        std::memcpy(input_gradient->test_data.data() + source * sizeof(T), &result,
                    sizeof(T));
    }
}
#endif

} // namespace

bool compute_broadcast_backward(Buffer* input_gradient, const Buffer* output_gradient,
                                int dtype, const std::vector<long long>& input_shape,
                                const std::vector<long long>& output_shape,
                                std::string& error) {
    if (!abi::real_dtype_code(dtype)) {
        error = "tensor broadcast backward requires a floating tensor dtype";
        return false;
    }
    const auto width = compute_dtype_width(dtype);
    if (!compute_same_device(input_gradient, output_gradient, error)) return false;
    if (input_shape.size() != output_shape.size()) {
        error = "tensor broadcast backward rank mismatch";
        return false;
    }
    // Axes of output extent 1 address nothing and are dropped. On every other
    // axis the source either keeps the output extent or broadcasts extent 1,
    // in which case that axis is summed over.
    std::vector<std::uint32_t> source_extents;
    std::vector<std::uint32_t> reduce_extents;
    std::vector<std::uint64_t> output_strides;
    std::size_t source_count = 1;
    std::size_t output_count = 1;
    std::size_t reduce_count = 1;
    std::uint64_t stride = 1;
    std::vector<std::uint64_t> all_strides(output_shape.size(), 0);
    for (std::size_t axis = output_shape.size(); axis-- > 0;) {
        const auto input_extent = input_shape[axis];
        const auto output_extent = output_shape[axis];
        if (input_extent < 0 || output_extent < 0 ||
            (input_extent != output_extent && input_extent != 1) ||
            output_extent > std::numeric_limits<std::uint32_t>::max()) {
            error = "invalid tensor broadcast backward shape";
            return false;
        }
        all_strides[axis] = stride;
        const auto extent = static_cast<std::uint64_t>(output_extent);
        if (extent != 0 && stride > std::numeric_limits<std::uint32_t>::max() / extent) {
            error = "tensor broadcast backward is too large";
            return false;
        }
        stride *= extent;
    }
    for (std::size_t axis = 0; axis < output_shape.size(); ++axis) {
        const auto input_extent = static_cast<std::size_t>(input_shape[axis]);
        const auto output_extent = static_cast<std::size_t>(output_shape[axis]);
        source_count *= input_extent;
        output_count *= output_extent;
        if (output_extent == 1) continue;
        const auto reduced = input_extent == 1 ? output_extent : std::size_t{1};
        reduce_count *= reduced;
        source_extents.push_back(static_cast<std::uint32_t>(input_extent));
        reduce_extents.push_back(static_cast<std::uint32_t>(reduced));
        output_strides.push_back(all_strides[axis]);
    }
    if (!compute_buffer_range(input_gradient, 0, source_count, width, error) ||
        !compute_buffer_range(output_gradient, 0, output_count, width, error))
        return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (input_gradient->backend == Backend::Test) {
        if (dtype == QCORE_DTYPE_FLOAT32)
            fake_broadcast_backward<float>(input_gradient, output_gradient,
                source_extents, reduce_extents, output_strides,
                source_count, reduce_count);
        else if(dtype==QCORE_DTYPE_FLOAT16)
            fake_broadcast_backward<abi::half::Real16>(input_gradient,output_gradient,
                source_extents,reduce_extents,output_strides,source_count,reduce_count);
        else if(dtype==QCORE_DTYPE_BFLOAT16)
            fake_broadcast_backward<abi::half::Real16b>(input_gradient,output_gradient,
                source_extents,reduce_extents,output_strides,source_count,reduce_count);
        else
            fake_broadcast_backward<double>(input_gradient, output_gradient,
                source_extents, reduce_extents, output_strides,
                source_count, reduce_count);
        return true;
    }
#endif
#ifdef __APPLE__
    if (metal_real_dtype(dtype) && input_gradient->backend == Backend::Metal) {
        auto pipeline = metal_pipeline(
            input_gradient, metal_pipeline_id(MetalKernel::BroadcastBackwardF32, static_cast<unsigned>(dtype)),
            [&] { return msl_half_storage(msl_broadcast_backward_source(), dtype); }, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(source_count);
        const auto rank = static_cast<std::uint32_t>(source_extents.size());
        const auto reduce = static_cast<std::uint64_t>(reduce_count);
        std::size_t source_length = 0;
        std::size_t reduce_length = 0;
        std::size_t strides_length = 0;
        const auto* source_bytes = metal_array_bytes(source_extents, source_length);
        const auto* reduce_bytes = metal_array_bytes(reduce_extents, reduce_length);
        const auto* strides_bytes = metal_array_bytes(output_strides, strides_length);
        return dispatch_metal_1d(input_gradient, pipeline, source_count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder, input_gradient, 0, 0);
                metal_bind(encoder, output_gradient, 0, 1);
                [encoder setBytes:&n length:sizeof(n) atIndex:2];
                [encoder setBytes:&rank length:sizeof(rank) atIndex:3];
                [encoder setBytes:source_bytes length:source_length atIndex:4];
                [encoder setBytes:reduce_bytes length:reduce_length atIndex:5];
                [encoder setBytes:strides_bytes length:strides_length atIndex:6];
                [encoder setBytes:&reduce length:sizeof(reduce) atIndex:7];
            }, error);
    }
#endif
    // Read only by the device paths above, which a portable build compiles out.
    (void)reduce_count;
    return real_compute_unavailable(
        input_gradient, "tensor broadcast backward", dtype, error);
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
            case abi::tensor_comparison_opcode::equal: matches = av == bv; break;
            case abi::tensor_comparison_opcode::not_equal: matches = av != bv; break;
            case abi::tensor_comparison_opcode::less: matches = av < bv; break;
            case abi::tensor_comparison_opcode::less_equal: matches = av <= bv; break;
            case abi::tensor_comparison_opcode::greater: matches = av > bv; break;
            case abi::tensor_comparison_opcode::greater_equal: matches = av >= bv; break;
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
    const char* op = operation == abi::tensor_comparison_opcode::equal ? "==" : operation == abi::tensor_comparison_opcode::not_equal ? "!=" :
                     operation == abi::tensor_comparison_opcode::less ? "<" : operation == abi::tensor_comparison_opcode::less_equal ? "<=" :
                     operation == abi::tensor_comparison_opcode::greater ? ">" : ">=";
    return "extern \"C\" __global__ void q_kernel(const " + type +
        "* left,const " + type +
        "* right,unsigned long long count,unsigned int* result){"
        "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
        "if(i<count && !(left[i]" + op + "right[i])) atomicExch(result,0u);}";
}

std::string ptx_compare_all_kernel(int dtype, int operation) {
    std::ostringstream out;
    const char* integer_cmp = operation == abi::tensor_comparison_opcode::equal ? "eq" : operation == abi::tensor_comparison_opcode::not_equal ? "ne" :
                              operation == abi::tensor_comparison_opcode::less ? "lt" : operation == abi::tensor_comparison_opcode::less_equal ? "le" :
                              operation == abi::tensor_comparison_opcode::greater ? "gt" : "ge";
    const char* float_cmp = operation == abi::tensor_comparison_opcode::equal ? "eq" : operation == abi::tensor_comparison_opcode::not_equal ? "neu" :
                            operation == abi::tensor_comparison_opcode::less ? "lt" : operation == abi::tensor_comparison_opcode::less_equal ? "le" :
                            operation == abi::tensor_comparison_opcode::greater ? "gt" : "ge";
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

    const bool f32 = dtype == QCORE_DTYPE_FLOAT32;
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
    else if (metal_real_dtype(dtype)) type = "float";
    else return {};
    const char* op = operation == abi::tensor_comparison_opcode::equal ? "==" : operation == abi::tensor_comparison_opcode::not_equal ? "!=" :
                     operation == abi::tensor_comparison_opcode::less ? "<" : operation == abi::tensor_comparison_opcode::less_equal ? "<=" :
                     operation == abi::tensor_comparison_opcode::greater ? ">" : ">=";
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

namespace {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
template <typename T>
bool fake_compare_elements(Buffer* output,const Buffer* left,const Buffer* right,
                           const void* scalar,int side,int operation,std::size_t count){
    T sv{};
    if(side!=abi::scalar_side::none)std::memcpy(&sv,scalar,sizeof(T));
    for(std::size_t i=0;i<count;++i){
        T a{},b{};
        std::memcpy(&a,left->test_data.data()+i*sizeof(T),sizeof(T));
        if(side==abi::scalar_side::none)std::memcpy(&b,right->test_data.data()+i*sizeof(T),sizeof(T));
        else if(side==abi::scalar_side::left){b=a;a=sv;}
        else b=sv;
        bool result=false;
        if(operation==abi::tensor_comparison_opcode::equal)result=a==b;
        else if(operation==abi::tensor_comparison_opcode::not_equal)result=a!=b;
        else if constexpr (!abi::complex::complex_element<T>) {
            if(operation==abi::tensor_comparison_opcode::less)result=a<b;
            else if(operation==abi::tensor_comparison_opcode::less_equal)result=a<=b;
            else if(operation==abi::tensor_comparison_opcode::greater)result=a>b;
            else result=a>=b;
        }
        output->test_data[i]=result?1:0;
    }
    return true;
}
#endif
#ifdef __APPLE__
std::string msl_compare_elements_source(int dtype,int operation,int side){
    const auto part=abi::complex_part_dtype_code(dtype);
    const bool complex=abi::complex_dtype_code(dtype);
    const auto type=complex?(half_dtype(part)?"ushort2":"float2"):"ushort";
    const auto value_type=complex?"float2":"float";
    const std::string read=complex?"cr":"qread";
    const auto helpers=complex?msl_complex_helpers(dtype):msl_half_helpers(dtype);
    const auto scalar=complex?(half_dtype(part)?"cr(as_type<ushort2>(uint(scalar_bits)))":"cr(as_type<float2>(scalar_bits))"):"qread(ushort(scalar_bits))";
    const char* op=operation==abi::tensor_comparison_opcode::equal?"==":operation==abi::tensor_comparison_opcode::not_equal?"!=":
        operation==abi::tensor_comparison_opcode::less?"<":operation==abi::tensor_comparison_opcode::less_equal?"<=":
        operation==abi::tensor_comparison_opcode::greater?">":">=";
    const auto predicate=complex?
        "(a.x"+std::string(op)+"b.x)"+(operation==abi::tensor_comparison_opcode::equal?"&&":"||")+"(a.y"+op+"b.y)":
        "a"+std::string(op)+"b";
    return "#include <metal_stdlib>\nusing namespace metal;\n"+helpers+
        "kernel void q_kernel(device uchar* out [[buffer(0)]],const device "+type+
        "* left [[buffer(1)]],const device "+type+"* right [[buffer(2)]],constant ulong& scalar_bits [[buffer(3)]],"
        "constant ulong& count [[buffer(4)]],uint gid [[thread_position_in_grid]]){if(ulong(gid)>=count)return; "+
        value_type+" p="+read+"(left[gid]),s="+scalar+",a="+(side==abi::scalar_side::left?"s":"p")+",b="+
        (side==abi::scalar_side::none?read+"(right[gid])":side==abi::scalar_side::left?"p":"s")+
        ";out[gid]=("+predicate+")?1:0;}\n";
}
#endif
} // namespace

bool compute_compare_elements(Buffer* output,const Buffer* left,const Buffer* right,
                              const void* scalar,int side,int dtype,int operation,
                              std::size_t count,std::string& error){
    const auto width=compute_dtype_width(dtype);
    if((!half_dtype(dtype)&&!abi::complex_dtype_code(dtype))||
       operation<abi::tensor_comparison_opcode::equal||operation>abi::tensor_comparison_opcode::greater_equal||
       (abi::complex_dtype_code(dtype)&&operation>abi::tensor_comparison_opcode::not_equal)||
       !compute_same_device(output,left,error)||!compute_buffer_range(output,0,count,1,error)||
       !compute_buffer_range(left,0,count,width,error)||
       (side==abi::scalar_side::none&&(!compute_same_device(output,right,error)||!compute_buffer_range(right,0,count,width,error)))){
        if(error.empty())error="invalid GPU tensor comparison";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        counters::add(counters::Id::CoreDispatches);
        using abi::complex::ComplexElement;
        switch(dtype){
            case QCORE_DTYPE_FLOAT16:return fake_compare_elements<abi::half::Real16>(output,left,right,scalar,side,operation,count);
            case QCORE_DTYPE_BFLOAT16:return fake_compare_elements<abi::half::Real16b>(output,left,right,scalar,side,operation,count);
            case QCORE_DTYPE_COMPLEX_FLOAT16:return fake_compare_elements<ComplexElement<abi::half::Real16>>(output,left,right,scalar,side,operation,count);
            case QCORE_DTYPE_COMPLEX_BFLOAT16:return fake_compare_elements<ComplexElement<abi::half::Real16b>>(output,left,right,scalar,side,operation,count);
            case QCORE_DTYPE_COMPLEX_FLOAT32:return fake_compare_elements<ComplexElement<float>>(output,left,right,scalar,side,operation,count);
            case QCORE_DTYPE_COMPLEX_FLOAT64:return fake_compare_elements<ComplexElement<double>>(output,left,right,scalar,side,operation,count);
            default:break;
        }
    }
#endif
    if(numeric_gpu_backend(output))
        return launch_numeric(output,{.left=left,.right=right,.scalar=scalar,.scalar_width=width,
            .count=count,.dtype=dtype,.operation=operation,.side=side,.mode=3},error);
#ifdef __APPLE__
    if(output->backend==Backend::Metal&&dtype==QCORE_DTYPE_COMPLEX_FLOAT64){
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::CompareElements,
            static_cast<unsigned>(dtype),static_cast<unsigned>(operation),static_cast<unsigned>(side)),[&]{
            const std::string lhs=side==abi::scalar_side::left?"scalar":"left[gid]";
            const std::string rhs=side==abi::scalar_side::none?"right[gid]":side==abi::scalar_side::left?"left[gid]":"scalar";
            return std::string("#include <metal_stdlib>\nusing namespace metal;\n")+
                "bool qe(ulong a,ulong b){return a==b||((a&0x7ffffffffffffffful)==0ul&&(b&0x7ffffffffffffffful)==0ul);}"+
                "kernel void q_kernel(device uchar* out [[buffer(0)]],const device ulong2* left [[buffer(1)]],"
                "const device ulong2* right [[buffer(2)]],constant ulong2& scalar [[buffer(3)]],"
                "constant ulong& count [[buffer(4)]],uint gid [[thread_position_in_grid]]){if(ulong(gid)>=count)return;"
                "ulong2 a="+lhs+",b="+rhs+";bool equal=qe(a.x,b.x)&&qe(a.y,b.y);out[gid]="+
                (operation==abi::tensor_comparison_opcode::equal?"equal;}":"!equal;}");
        },error);
        if(!pipeline)return false;
        std::uint64_t bits[2]{};if(side!=abi::scalar_side::none)std::memcpy(bits,scalar,sizeof(bits));
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,left,0,1);metal_bind(e,right?right:left,0,2);
            [e setBytes:bits length:sizeof(bits) atIndex:3];[e setBytes:&n length:sizeof(n) atIndex:4];
        },error);
    }
#endif
#ifdef __APPLE__
    if(output->backend==Backend::Metal&&(half_dtype(dtype)||metal_complex_dtype(dtype))){
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::CompareElements,
            static_cast<unsigned>(dtype),static_cast<unsigned>(operation),static_cast<unsigned>(side)),
            [&]{return msl_compare_elements_source(dtype,operation,side);},error);
        if(!pipeline)return false;
        std::uint64_t bits=0;
        if(side!=abi::scalar_side::none)std::memcpy(&bits,scalar,width);
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,left,0,1);
            metal_bind(e,right?right:left,0,2);
            [e setBytes:&bits length:sizeof(bits) atIndex:3];[e setBytes:&n length:sizeof(n) atIndex:4];
        },error);
    }
#endif
    return real_compute_unavailable(output,"tensor comparison",dtype,error);
}

bool compute_compare_all(
    const Buffer* left, std::size_t left_offset,
    const Buffer* right, std::size_t right_offset,
    int dtype, int operation, std::size_t count,
    bool& result, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (operation < abi::tensor_comparison_opcode::equal || operation > abi::tensor_comparison_opcode::greater_equal || width == 0) {
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
        counters::add(counters::Id::CoreDispatches); // qcount
        switch (dtype) {
            case QCORE_DTYPE_INT64: return fake_compare_all_values<std::int64_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_INT8: return fake_compare_all_values<std::int8_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_INT16: return fake_compare_all_values<std::int16_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_INT32: return fake_compare_all_values<std::int32_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_UINT8: return fake_compare_all_values<std::uint8_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_UINT16: return fake_compare_all_values<std::uint16_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_UINT32: return fake_compare_all_values<std::uint32_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_UINT64: return fake_compare_all_values<std::uint64_t>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_FLOAT64: return fake_compare_all_values<double>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_FLOAT32: return fake_compare_all_values<float>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_FLOAT16: return fake_compare_all_values<abi::half::Real16>(
                left,left_offset,right,right_offset,operation,count,result,error);
            case QCORE_DTYPE_BFLOAT16: return fake_compare_all_values<abi::half::Real16b>(
                left,left_offset,right,right_offset,operation,count,result,error);
            default:
                error = "invalid GPU tensor comparison dtype";
                return false;
        }
    }
#endif

    if(half_dtype(dtype) && numeric_gpu_backend(left)) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(acquire_validation_status(left->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        if(!launch_numeric(status.get(),{.left=left,.left_offset=left_offset,.right=right,
            .right_offset=right_offset,.count=count,.dtype=dtype,.operation=operation,.mode=10,
            .status=status.get()},error))return false;
        std::uint32_t bits=0;if(!copy_to_host(status.get(),0,&bits,sizeof(bits),error))return false;
        result=bits==0;return true;
    }
    std::unique_ptr<Buffer,void(*)(Buffer*)> flag(
        allocate(left->global_index, sizeof(std::uint32_t), error),
        [](Buffer* value){ quidra::device::release(value); });
    if (!flag) return false;
    std::uint32_t host_flag = 1;
    if (!copy_from_host(flag.get(), 0, &host_flag, sizeof(host_flag), error))
        return false;

    if (left->backend == Backend::Cuda) {
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
    } else if (left->backend == Backend::Hip) {
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
        if (!gpu_integer_dtype(dtype) && !metal_real_dtype(dtype))
            return real_compute_unavailable(left, "tensor comparison", dtype, error);
        auto pipeline = metal_pipeline(
            flag.get(), metal_pipeline_id(MetalKernel::CompareAll, static_cast<unsigned>(dtype), static_cast<unsigned>(operation)),
            [&] { return msl_half_storage(msl_compare_all_source(dtype, operation), dtype); }, error);
        if (!pipeline) return false;
        const auto n = static_cast<std::uint64_t>(count);
        if (!dispatch_metal_1d(
                flag.get(), pipeline, count,
                [&](id<MTLComputeCommandEncoder> encoder) {
                    metal_bind(encoder,left,left_offset,0);
                    metal_bind(encoder,right,right_offset,1);
                    [encoder setBytes:&n length:sizeof(n) atIndex:2];
                    metal_bind(encoder,flag,0,3);
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
    if (operation < abi::tensor_binary_opcode::add || operation > abi::tensor_binary_opcode::power ||
        scalar_side < abi::scalar_side::none || scalar_side > abi::scalar_side::right ||
        (operation == abi::tensor_binary_opcode::power && scalar_side != abi::scalar_side::right) ||
        (scalar_side == abi::scalar_side::none && !right) ||
        (scalar_side != abi::scalar_side::none && !scalar)) {
        error = "invalid GPU tensor binary operation";
        return false;
    }
    if (!compute_same_device(output, left, error) ||
        !compute_buffer_range(output, 0, count, width, error) ||
        !compute_buffer_range(left, left_offset, count, width, error)) return false;
    if (scalar_side == abi::scalar_side::none) {
        if (!compute_same_device(output, right, error) ||
            !compute_buffer_range(right, right_offset, count, width, error)) return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        counters::add(counters::Id::CoreDispatches); // qcount
        switch (dtype) {
            case QCORE_DTYPE_INT64: return fake_binary<std::int64_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_INT8: return fake_binary<std::int8_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_INT16: return fake_binary<std::int16_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_INT32: return fake_binary<std::int32_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_UINT8: return fake_binary<std::uint8_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_UINT16: return fake_binary<std::uint16_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_UINT32: return fake_binary<std::uint32_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_UINT64: return fake_binary<std::uint64_t>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_FLOAT64: return fake_binary<double>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_FLOAT32: return fake_binary<float>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT16: return fake_complex_binary<abi::half::Real16>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_COMPLEX_BFLOAT16: return fake_complex_binary<abi::half::Real16b>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT32: return fake_complex_binary<float>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT64: return fake_complex_binary<double>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_FLOAT16: return fake_binary<abi::half::Real16>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            case QCORE_DTYPE_BFLOAT16: return fake_binary<abi::half::Real16b>(output,left,left_offset,right,right_offset,scalar,scalar_side,operation,count,error);
            default: error="invalid tensor dtype"; return false;
        }
    }
#endif
    if(additional_dtype(dtype) && numeric_gpu_backend(output)) {
        if(operation==abi::tensor_binary_opcode::remainder ||
           (abi::complex_dtype_code(dtype)&&operation==abi::tensor_binary_opcode::power) ||
           (output->backend==Backend::Cuda&&operation==abi::tensor_binary_opcode::power))
            return real_compute_unavailable(output,"tensor arithmetic",dtype,error);
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        if(!launch_numeric(output,{.left=left,.left_offset=left_offset,.right=right,.right_offset=right_offset,
            .scalar=scalar,.scalar_width=width,.count=count,.dtype=dtype,.operation=operation,
            .side=scalar_side,.mode=0,.status=status.get()},error))return false;
        defer_real_status(status.release(),dtype,operation);return true;
    }
    if (operation == abi::tensor_binary_opcode::power && gpu_integer_dtype(dtype)) {
        return real_compute_unavailable(output, "integer tensor power", dtype, error);
    }
    if (gpu_integer_dtype(dtype) && output->backend==Backend::Cuda) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto ptx=ptx_integer_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->cuda_pointer;
        auto left_ptr=left->cuda_pointer+left_offset;
        auto right_ptr=scalar_side==abi::scalar_side::none?right->cuda_pointer+right_offset:left_ptr;
        auto status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        std::uint64_t zero_scalar=0;
        void* scalar_storage=scalar_side==abi::scalar_side::none?static_cast<void*>(&zero_scalar):const_cast<void*>(scalar);
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n,&status_ptr};
        if(!launch_ptx_1d(output,
            "binary-int-"+std::to_string(dtype)+"-"+std::to_string(operation)+"-"+std::to_string(scalar_side),
            ptx,count,args,error))return false;
        const auto validation_error=operation==abi::tensor_binary_opcode::divide||operation==abi::tensor_binary_opcode::remainder
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
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::BinaryInt, static_cast<unsigned>(dtype), static_cast<unsigned>(operation), static_cast<unsigned>(scalar_side)),
            [&] { return msl_integer_binary_source(dtype,operation,scalar_side); }, error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        std::uint64_t zero_scalar=0;
        const void* scalar_storage=scalar_side==abi::scalar_side::none?static_cast<const void*>(&zero_scalar):scalar;
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);
            metal_bind(e,left,left_offset,1);
            metal_bind(e,right?right:left,
                       scalar_side==abi::scalar_side::none?right_offset:left_offset,2);
            [e setBytes:scalar_storage length:compute_dtype_width(dtype) atIndex:3];
            [e setBytes:&n length:sizeof(n) atIndex:4];
            metal_bind(e,status,0,5);
        },error))return false;
        const auto validation_error=operation==abi::tensor_binary_opcode::divide||operation==abi::tensor_binary_opcode::remainder
            ?"invalid tensor division/remainder or integer overflow"
            :"tensor integer arithmetic overflow";
        defer_validation(status.release(),validation_error);
        return true;
    }
#endif
    if (hip_integer_dtype(dtype) && output->backend==Backend::Hip) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto source_code=hip_integer_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->pointer;
        auto left_ptr=static_cast<unsigned char*>(left->pointer)+left_offset;
        void* right_ptr=scalar_side==abi::scalar_side::none
            ? static_cast<void*>(static_cast<unsigned char*>(right->pointer)+right_offset)
            : left_ptr;
        auto status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* scalar_storage=const_cast<void*>(scalar);
        std::uint64_t zero_scalar=0;
        if(scalar_side==abi::scalar_side::none)scalar_storage=&zero_scalar;
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n,&status_ptr};
        if(!launch_hip_1d(output,
            "binary-int-"+std::to_string(dtype)+"-"+std::to_string(operation)+"-"+std::to_string(scalar_side),
            source_code,count,args,error))return false;
        const auto validation_error=operation==abi::tensor_binary_opcode::divide||operation==abi::tensor_binary_opcode::remainder
            ?"invalid tensor division/remainder or integer overflow"
            :"tensor integer arithmetic overflow";
        defer_validation(status.release(),validation_error);
        return true;
    }
    if ((dtype == QCORE_DTYPE_FLOAT64 || dtype == QCORE_DTYPE_FLOAT32) && operation == abi::tensor_binary_opcode::power &&
        output->backend == Backend::Cuda) {
        return real_compute_unavailable(output, "tensor power", dtype, error);
    }
    if ((dtype == QCORE_DTYPE_FLOAT64 || dtype == QCORE_DTYPE_FLOAT32) && operation != abi::tensor_binary_opcode::remainder &&
        output->backend == Backend::Cuda) {
        auto ptx = ptx_float_binary_kernel(dtype, operation, scalar_side);
        auto out_ptr = output->cuda_pointer;
        auto left_ptr = left->cuda_pointer + left_offset;
        std::uint64_t right_ptr =
            scalar_side == abi::scalar_side::none ? right->cuda_pointer + right_offset : 0;
        std::uint64_t scalar_bits = 0;
        if (scalar_side != abi::scalar_side::none) std::memcpy(&scalar_bits, scalar, std::min(width,sizeof(scalar_bits)));
        auto n = static_cast<std::uint64_t>(count);
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto status_ptr=status->cuda_pointer;
        void* args[] = {&out_ptr,&left_ptr,&right_ptr,&scalar_bits,&n,&status_ptr};
        if (!launch_ptx_1d(
            output,
            "binary-" + std::to_string(dtype) + "-" +
                std::to_string(operation) + "-" + std::to_string(scalar_side),
            ptx, count, args, error)) return false;
        defer_real_status(status.release(), dtype, operation);
        return true;
    }
    if ((dtype==QCORE_DTYPE_FLOAT64||dtype==QCORE_DTYPE_FLOAT32) && operation!=abi::tensor_binary_opcode::remainder &&
        output->backend==Backend::Hip) {
        auto source_code=hip_float_binary_source(dtype,operation,scalar_side);
        auto out_ptr=output->pointer;
        auto left_ptr=static_cast<unsigned char*>(left->pointer)+left_offset;
        void* right_ptr=scalar_side==abi::scalar_side::none
            ? static_cast<void*>(static_cast<unsigned char*>(right->pointer)+right_offset)
            : left_ptr;
        auto n=static_cast<std::uint64_t>(count);
        void* scalar_storage=const_cast<void*>(scalar);
        std::uint64_t zero_scalar=0;
        if(scalar_side==abi::scalar_side::none)scalar_storage=&zero_scalar;
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto status_ptr=status->pointer;
        void* args[]={&out_ptr,&left_ptr,&right_ptr,scalar_storage,&n,&status_ptr};
        if(!launch_hip_1d(
            output,"binary-"+std::to_string(dtype)+"-"+
                std::to_string(operation)+"-"+std::to_string(scalar_side),
            source_code,count,args,error))return false;
        defer_real_status(status.release(),dtype,operation);
        return true;
    }
#ifdef __APPLE__
    if ((metal_real_dtype(dtype) || (metal_complex_dtype(dtype) && operation<=abi::tensor_binary_opcode::divide)) && operation != abi::tensor_binary_opcode::remainder && output->backend == Backend::Metal) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(abi::complex_dtype_code(dtype)?MetalKernel::ComplexBinary:MetalKernel::BinaryF32, static_cast<unsigned>(operation), static_cast<unsigned>(scalar_side), static_cast<unsigned>(dtype)),
            [&] { return abi::complex_dtype_code(dtype)?msl_complex_binary_source(dtype,operation,scalar_side):msl_half_storage(msl_float_binary_source(operation, scalar_side), dtype); }, error);
        if (!pipeline) return false;
        std::uint64_t scalar_bits = 0;
        if (scalar_side != abi::scalar_side::none) std::memcpy(&scalar_bits, scalar, std::min(width,sizeof(scalar_bits)));
        const auto n = static_cast<std::uint64_t>(count);
        if (!dispatch_metal_1d(output, pipeline, count,
            [&](id<MTLComputeCommandEncoder> encoder) {
                metal_bind(encoder,output,0,0);
                metal_bind(encoder,left,left_offset,1);
                metal_bind(encoder, right ? right : left,
                           scalar_side == abi::scalar_side::none ? right_offset : left_offset, 2);
                [encoder setBytes:&scalar_bits length:sizeof(scalar_bits) atIndex:3];
                [encoder setBytes:&n length:sizeof(n) atIndex:4];
                metal_bind(encoder,status,0,5);
            }, error)) return false;
        defer_real_status(status.release(), dtype, operation);
        return true;
    }
#endif
    return real_compute_unavailable(output, "tensor arithmetic", dtype, error);
}

bool compute_unary(Buffer* output, const Buffer* input, std::size_t input_offset,
                   int dtype, int operation, std::size_t count,
                   std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (operation != abi::tensor_unary_opcode::negate ||
        !compute_same_device(output,input,error) ||
        !compute_buffer_range(output,0,count,width,error) ||
        !compute_buffer_range(input,input_offset,count,width,error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend == Backend::Test) {
        counters::add(counters::Id::CoreDispatches); // qcount
        if (gpu_integer_signed(dtype)) {
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
                case QCORE_DTYPE_INT64: return run(std::int64_t{});
                case QCORE_DTYPE_INT8: return run(std::int8_t{});
                case QCORE_DTYPE_INT16: return run(std::int16_t{});
                case QCORE_DTYPE_INT32: return run(std::int32_t{});
                default: break;
            }
        }
        if (dtype == QCORE_DTYPE_FLOAT32) {
            const auto* source = input->test_data.data() + input_offset;
            auto* destination = output->test_data.data();
            for (std::size_t i=0;i<count;++i) {
                float value{};
                std::memcpy(&value,source+i*sizeof(float),sizeof(float));
                value=-value;
                std::memcpy(destination+i*sizeof(float),&value,sizeof(float));
            }
            return true;
        }
        if (dtype == QCORE_DTYPE_FLOAT64) {
            const auto* source = input->test_data.data() + input_offset;
            auto* destination = output->test_data.data();
            for (std::size_t i=0;i<count;++i) {
                double value{};
                std::memcpy(&value,source+i*sizeof(double),sizeof(double));
                value=-value;
                std::memcpy(destination+i*sizeof(double),&value,sizeof(double));
            }
            return true;
        }
        if (half_dtype(dtype)) {
            for(std::size_t i=0;i<count;++i){
                std::uint16_t bits{};
                std::memcpy(&bits,input->test_data.data()+input_offset+i*2,2);
                bits ^= 0x8000u;
                std::memcpy(output->test_data.data()+i*2,&bits,2);
            }
            return true;
        }
        if(abi::complex_dtype_code(dtype)){
            const auto part_width=width/2;
            for(std::size_t i=0;i<count*2;++i){
                std::uint64_t bits{};
                std::memcpy(&bits,input->test_data.data()+input_offset+i*part_width,part_width);
                bits ^= std::uint64_t{1}<<(part_width*8-1);
                std::memcpy(output->test_data.data()+i*part_width,&bits,part_width);
            }
            return true;
        }
        error = "tensor negation requires a signed numeric tensor";
        return false;
    }
#endif
    if(additional_dtype(dtype) && numeric_gpu_backend(output))
        return launch_numeric(output,{.left=input,.left_offset=input_offset,.count=count,.dtype=dtype,.mode=1},error);
    if (gpu_integer_signed(dtype) && output->backend==Backend::Cuda) {
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
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::UnaryInt, static_cast<unsigned>(dtype)),
            [&] { return msl_integer_unary_source(dtype); }, error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);
            metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];
            metal_bind(e,status,0,3);
        },error))return false;
        defer_validation(status.release(),"tensor integer negation overflow");
        return true;
    }
#endif
    if (hip_signed_dtype(dtype) && output->backend==Backend::Hip) {
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
    if ((dtype==QCORE_DTYPE_FLOAT64 || dtype==QCORE_DTYPE_FLOAT32) && output->backend==Backend::Cuda) {
        auto ptx=ptx_float_unary_kernel(dtype);
        auto out_ptr=output->cuda_pointer;
        auto in_ptr=input->cuda_pointer+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_ptx_1d(
            output,"unary-neg-"+std::to_string(dtype),ptx,count,args,error);
    }
    if ((dtype==QCORE_DTYPE_FLOAT64||dtype==QCORE_DTYPE_FLOAT32) && output->backend==Backend::Hip) {
        auto source_code=hip_float_unary_source(dtype);
        auto out_ptr=output->pointer;
        auto in_ptr=static_cast<unsigned char*>(input->pointer)+input_offset;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        return launch_hip_1d(
            output,"unary-neg-"+std::to_string(dtype),source_code,count,args,error);
    }
#ifdef __APPLE__
    if (metal_real_dtype(dtype) && output->backend==Backend::Metal) {
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::UnaryNegF32, static_cast<unsigned>(dtype)),
            [&] { return msl_half_storage(msl_float_unary_source(), dtype); }, error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,
            [&](id<MTLComputeCommandEncoder> encoder){
                metal_bind(encoder,output,0,0);
                metal_bind(encoder,input,input_offset,1);
                [encoder setBytes:&n length:sizeof(n) atIndex:2];
            },error);
    }
#endif
#ifdef __APPLE__
    if(dtype==QCORE_DTYPE_COMPLEX_FLOAT64&&output->backend==Backend::Metal){
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::ComplexUnary,static_cast<unsigned>(dtype)),[]{
            return std::string("#include <metal_stdlib>\nusing namespace metal;\n")+
                "kernel void q_kernel(device ulong2* out [[buffer(0)]],const device ulong2* in [[buffer(1)]],"
                "constant ulong& count [[buffer(2)]],uint gid [[thread_position_in_grid]]){"
                "if(ulong(gid)<count)out[gid]=in[gid]^ulong2(0x8000000000000000ul);}";
        },error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error);
    }
    if(metal_complex_dtype(dtype)&&output->backend==Backend::Metal){
        const auto type=half_dtype(abi::complex_part_dtype_code(dtype))?"ushort2":"float2";
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::ComplexUnary,static_cast<unsigned>(dtype)),[&]{
            return std::string("#include <metal_stdlib>\nusing namespace metal;\n")+msl_complex_helpers(dtype)+
                "kernel void q_kernel(device "+type+"* out [[buffer(0)]],const device "+type+
                "* in [[buffer(1)]],constant ulong& count [[buffer(2)]],uint gid [[thread_position_in_grid]]){"
                "if(ulong(gid)<count)out[gid]=cw(-cr(in[gid]));}\n";
        },error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error);
    }
#endif
    return real_compute_unavailable(output,"tensor negation",dtype,error);
}

abi::ConversionReason last_cast_reason(){return numeric_cast_reason;}

bool compute_cast(Buffer* output, const Buffer* input, std::size_t input_offset,
                  int source_dtype, int target_dtype, std::size_t count,
                  std::string& error) {
    numeric_cast_reason=abi::ConversionReason::out_of_range;
    const auto source_width=compute_dtype_width(source_dtype);
    const auto target_width=compute_dtype_width(target_dtype);
    if(!compute_same_device(output,input,error) ||
       !compute_buffer_range(output,0,count,target_width,error) ||
       !compute_buffer_range(input,input_offset,count,source_width,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        counters::add(counters::Id::CoreDispatches); // qcount
        switch(source_dtype){
            case QCORE_DTYPE_INT64: return fake_cast_from<std::int64_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_INT8: return fake_cast_from<std::int8_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_INT16: return fake_cast_from<std::int16_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_INT32: return fake_cast_from<std::int32_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_UINT8: return fake_cast_from<std::uint8_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_UINT16: return fake_cast_from<std::uint16_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_UINT32: return fake_cast_from<std::uint32_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_UINT64: return fake_cast_from<std::uint64_t>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_FLOAT64: return fake_cast_from<double>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_FLOAT32: return fake_cast_from<float>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_FLOAT16: return fake_cast_from<abi::half::Real16>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_BFLOAT16: return fake_cast_from<abi::half::Real16b>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT16: return fake_cast_from<abi::complex::ComplexElement<abi::half::Real16>>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_COMPLEX_BFLOAT16: return fake_cast_from<abi::complex::ComplexElement<abi::half::Real16b>>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT32: return fake_cast_from<abi::complex::ComplexElement<float>>(output,input,input_offset,target_dtype,count,error);
            case QCORE_DTYPE_COMPLEX_FLOAT64: return fake_cast_from<abi::complex::ComplexElement<double>>(output,input,input_offset,target_dtype,count,error);
            default:error="invalid tensor cast source dtype";return false;
        }
    }
#endif
    if(source_dtype==target_dtype)
        return copy_device_to_device(output,0,input,input_offset,count*source_width,error);
    if(abi::complex_dtype_code(source_dtype)&&!abi::complex_dtype_code(target_dtype)) {
        error="tensor cast from a complex to a real dtype is unsupported";return false;
    }
    if((additional_dtype(source_dtype)||additional_dtype(target_dtype)||
        (abi::real_dtype_code(source_dtype)&&gpu_integer_dtype(target_dtype))) && numeric_gpu_backend(output)) {
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        if(!launch_numeric(output,{.left=input,.left_offset=input_offset,.count=count,.mode=2,
            .source_dtype=source_dtype,.target_dtype=target_dtype,.status=status.get()},error))return false;
        defer_cast_status(status.release(),target_dtype);return true;
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal&&abi::real_dtype_code(source_dtype)&&gpu_integer_dtype(target_dtype)){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::Cast,
            static_cast<unsigned>(source_dtype),static_cast<unsigned>(target_dtype)),
            [&]{return msl_real_integer_cast_source(source_dtype,target_dtype);},error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];metal_bind(e,status,0,3);
        },error))return false;
        defer_cast_status(status.release(),target_dtype);return true;
    }
    if(output->backend==Backend::Metal&&metal_complex_dtype(target_dtype)&&
       (metal_complex_dtype(source_dtype)||metal_real_dtype(source_dtype)||gpu_integer_dtype(source_dtype))){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline=metal_pipeline(output,metal_pipeline_id(MetalKernel::Cast,
            static_cast<unsigned>(source_dtype),static_cast<unsigned>(target_dtype)),
            [&]{return msl_complex_cast_source(source_dtype,target_dtype);},error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];metal_bind(e,status,0,3);
        },error))return false;
        defer_validation(status.release(),tensor_cast_range_message(target_dtype));return true;
    }
    if(output->backend==Backend::Metal && (metal_real_dtype(source_dtype)||gpu_integer_dtype(source_dtype)) &&
       metal_real_dtype(target_dtype) && (half_dtype(source_dtype)||half_dtype(target_dtype))){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline=metal_pipeline(output,
            metal_pipeline_id(MetalKernel::Cast,static_cast<unsigned>(source_dtype),static_cast<unsigned>(target_dtype)),
            [&]{return msl_half_cast_source(source_dtype,target_dtype);},error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0); metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2]; metal_bind(e,status,0,3);
        },error))return false;
        defer_validation(status.release(),tensor_cast_range_message(target_dtype));
        return true;
    }
#endif
    if(gpu_integer_dtype(source_dtype) &&
       (gpu_integer_dtype(target_dtype)||target_dtype==QCORE_DTYPE_FLOAT64||target_dtype==QCORE_DTYPE_FLOAT32) &&
       output->backend==Backend::Cuda){
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
            tensor_cast_range_message(target_dtype));
        return true;
    }
#ifdef __APPLE__
    if(gpu_integer_dtype(source_dtype) &&
       (gpu_integer_dtype(target_dtype)||target_dtype==QCORE_DTYPE_FLOAT32) &&
       output->backend==Backend::Metal){
        std::unique_ptr<Buffer,void(*)(Buffer*)> status(
            acquire_validation_status(output->global_index,error),
            [](Buffer* value){quidra::device::release(value);});
        if(!status)return false;
        auto pipeline = metal_pipeline(
            output, metal_pipeline_id(MetalKernel::Cast, static_cast<unsigned>(source_dtype), static_cast<unsigned>(target_dtype)),
            [&] { return msl_integer_cast_source(source_dtype,target_dtype); }, error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            metal_bind(e,output,0,0);
            metal_bind(e,input,input_offset,1);
            [e setBytes:&n length:sizeof(n) atIndex:2];
            metal_bind(e,status,0,3);
        },error))return false;
        defer_validation(
            status.release(),
            tensor_cast_range_message(target_dtype));
        return true;
    }
#endif
    if((source_dtype==QCORE_DTYPE_FLOAT64||source_dtype==QCORE_DTYPE_FLOAT32) &&
       (target_dtype==QCORE_DTYPE_FLOAT64||target_dtype==QCORE_DTYPE_FLOAT32) &&
       output->backend==Backend::Cuda){
        if(source_dtype==QCORE_DTYPE_FLOAT64 && target_dtype==QCORE_DTYPE_FLOAT32){
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
                tensor_cast_range_message(target_dtype));
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
    if (output->backend==Backend::Hip &&
        hip_integer_dtype(source_dtype) &&
        (hip_integer_dtype(target_dtype)||target_dtype==QCORE_DTYPE_FLOAT64||target_dtype==QCORE_DTYPE_FLOAT32)) {
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
            tensor_cast_range_message(target_dtype));
        return true;
    }
    if (output->backend==Backend::Hip &&
        (source_dtype==QCORE_DTYPE_FLOAT64||source_dtype==QCORE_DTYPE_FLOAT32) && hip_integer_dtype(target_dtype)) {
        error="tensor cast from a real to an integer dtype is unsupported";
        return false;
    }
    if ((source_dtype==QCORE_DTYPE_FLOAT64||source_dtype==QCORE_DTYPE_FLOAT32) &&
        (target_dtype==QCORE_DTYPE_FLOAT64||target_dtype==QCORE_DTYPE_FLOAT32) &&
        output->backend==Backend::Hip) {
        if(source_dtype==QCORE_DTYPE_FLOAT64 && target_dtype==QCORE_DTYPE_FLOAT32){
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
                tensor_cast_range_message(target_dtype));
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
std::uint64_t queue_native_handle(int index) {
    const auto* info = find(index);
    if (!info) return 0;
    if(info->backend==Backend::Cuda||info->backend==Backend::Hip)
        lend_native_package_work(info->backend,index,info->backend_index);
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (!stream) return 0;
        counters::add(counters::Id::PackageQueueBorrows); // qcount
        // Package code that commits its own command buffer on this queue must
        // be ordered after the work Core has encoded so far.
        metal_lend_to_package(*stream, nullptr);
        return reinterpret_cast<std::uint64_t>((__bridge void*)stream->queue);
    }
#endif
    return 0;
}

std::uint64_t native_device_handle(int index) {
    const auto* info = find(index);
    if (!info) return 0;
    if(info->backend==Backend::Cuda||info->backend==Backend::Hip)
        lend_native_package_work(info->backend,index,info->backend_index);
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        return stream ? reinterpret_cast<std::uint64_t>((__bridge void*)stream->device)
                      : 0;
    }
#endif
    return 0;
}

// Core's device command stream, exposed to package code through the native
// ABI (the protocol is specified in native_extension.h and implemented with
// MetalHold in device_metal_stream.inc). Only Metal batches submissions; on
// the other backends a package hold or scope is unavailable (false or 0),
// and flush/wait map to synchronization.
namespace {
// Set once this thread keeps the hold whose failure it reports (see
// end_left_package_hold): the process ends at once (exit_failed_program)
// with the stream still held, and the hold is not ended or reported again.
thread_local bool keeping_failed_hold = false;

#ifdef __APPLE__
// Starts this thread's hold on `stream`: locks it until the hold ends.
void metal_take_hold(MetalStream& stream) {
    stream.mutex.lock();
    metal_align_node_locked(stream);
    stream.hold = MetalHold::Preparing;
    stream.hold_lent_queue = false;
    stream.hold_violation.clear();
    metal_held_stream = &stream;
    open_package_holds.fetch_add(1, std::memory_order_relaxed);
}

// Ends this thread's hold on `stream` and unlocks it.
void metal_release_hold(MetalStream& stream) {
    stream.hold = MetalHold::None;
    stream.hold_lent_queue = false;
    stream.hold_violation.clear();
    metal_held_stream = nullptr;
    open_package_holds.fetch_sub(1, std::memory_order_relaxed);
    stream.mutex.unlock();
}

const char* metal_hold_name(const MetalStream& stream) {
    switch (stream.hold) {
        case MetalHold::Encoder:
            return "package encoder scope (qcore_device_compute_encoder)";
        case MetalHold::CommandBuffer:
            return "package command-buffer scope (qcore_metal_command_buffer)";
        default:
            return "package encode hold (qcore_device_encode_begin)";
    }
}

// A hold or scope requested while this thread already holds a stream: holds
// do not nest (one encoder scope per stream, one hold per thread). Recorded
// on the hold the thread has, which then stops the program when it ends.
void metal_refuse_nested_hold(const char* call, int index) {
    auto* held = metal_held_stream;
    if (metal_scope_open(*held)) {
        metal_record_violation_locked(*held,
            std::string(call) + " for gpu(" + std::to_string(index) +
            ") was called while package code holds Core's " +
            (held->hold == MetalHold::Encoder ? "compute encoder"
                                              : "command buffer") +
            " on gpu(" + std::to_string(held->global_index) +
            "): encoder scopes do not nest, and an encoder scope cannot "
            "become a command-buffer scope (include/quidra/native_extension.h)");
    } else {
        metal_record_violation_locked(*held,
            std::string(call) + " for gpu(" + std::to_string(index) +
            ") was called while the thread already holds the command stream "
            "of gpu(" + std::to_string(held->global_index) +
            "): package encodes do not nest (include/quidra/native_extension.h)");
    }
}
#endif
} // namespace

bool hold_package_stream(int index) {
    const auto* info = find(index);
    if (!info) return false;
    if(info->backend==Backend::Cuda||info->backend==Backend::Hip)
        lend_native_package_work(info->backend,index,info->backend_index);
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        if (!metal_stream_config().stream) return false;
        auto* stream = metal_stream(info->backend_index);
        if (!stream) return false;
        if (metal_held_stream) {
            metal_refuse_nested_hold("qcore_device_encode_begin", index);
            return false;
        }
        metal_take_hold(*stream);
        counters::add(counters::Id::PackageEncodeHolds); // qcount
        return true;
    }
#endif
    return false;
}

std::uint64_t begin_package_encode(int index, bool command_buffer,
                                   std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return 0;
    }
    if(info->backend==Backend::Cuda||info->backend==Backend::Hip)
        lend_native_package_work(info->backend,index,info->backend_index);
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        if (!metal_stream_config().stream) return 0;
        auto* stream = metal_stream(info->backend_index);
        if (!stream) return 0;
        const char* call = command_buffer ? "qcore_metal_command_buffer"
                                          : "qcore_device_compute_encoder";
        // The scope opens from this thread's Preparing hold on the stream,
        // or from a hold taken here (handles lent before it were lent
        // outside a hold and committed Core's batch).
        const bool own_hold = metal_held_stream == nullptr;
        if (!own_hold && (metal_held_stream != stream ||
                          stream->hold != MetalHold::Preparing)) {
            metal_refuse_nested_hold(call, index);
            error = "nested package encode";
            return 0;
        }
        if (own_hold) metal_take_hold(*stream);
        std::uint64_t handle = 0;
        @autoreleasepool {
            if (command_buffer) {
                if (metal_open_locked(*stream, error)) {
                    metal_end_encoder_locked(*stream);
                    stream->observation_reopen=false;
                    if(stream->observation) stream->observation->gap(
                        stream->next_serial.load(std::memory_order_relaxed),timeline::current_step(),"package_owned_encoders");
                    stream->hold = MetalHold::CommandBuffer;
                    handle = reinterpret_cast<std::uint64_t>(
                        (__bridge void*)stream->open);
                }
            } else if (id<MTLComputeCommandEncoder> encoder =
                           metal_compute_encoder_locked(*stream, error, false)) {
                stream->hold = MetalHold::Encoder;
                handle = reinterpret_cast<std::uint64_t>((__bridge void*)encoder);
            }
        }
        if (handle == 0) {
            // A hold taken by qcore_device_encode_begin stays: its one
            // qcore_metal_note_work still ends it.
            if (own_hold) metal_release_hold(*stream);
            return 0;
        }
        counters::add(counters::Id::PackageEncodeScopes); // qcount
        return handle;
    }
#endif
    (void)command_buffer;
    error = "Core command stream encoding is not available on this backend";
    return 0;
}

bool end_package_encode(int index, std::uint32_t dispatches,
                        std::uint64_t bytes, bool& violated,
                        std::string& error) {
    violated = false;
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (!stream || metal_held_stream != stream) {
            error = "no package encode is open on this device";
            return false;
        }
        if (!stream->hold_violation.empty()) {
            // Nothing of a hold that broke the protocol runs; the caller
            // stops the program. Other threads would run on while this one
            // ran exit handlers: the program beside a warm-up or completion
            // callback (a warm-up thread cannot even join itself there), the
            // callback thread. Outside a parallel node the
            // stream stays held until the process ends at once: here for a
            // callback, which has no statement to stop at, and after the
            // callback report.
            if (running_package_callback && !runtime::node_failure_active())
                fail_package_scope_unlocated(stream->hold_violation);
            violated = true;

            error = std::move(stream->hold_violation);
            metal_discard_open_locked(*stream);
            metal_release_hold(*stream);
            return false;
        }
        const bool lent_queue = stream->hold_lent_queue;
        const bool package_encoder = stream->hold==MetalHold::Encoder;
        stream->hold = MetalHold::None;
        if(profile::split_enabled() && package_encoder && stream->encoder) {
            metal_end_encoder_locked(*stream); stream->observation_reopen=true;
        }
        metal_note_work_locked(*stream, dispatches, bytes);
        counters::add(counters::Id::PackageDispatches, dispatches);
        // The package may commit command buffers of its own on the queue it
        // received during the hold: they must run after the hold's work.
        if (lent_queue && stream->open) {
            counters::add(counters::Id::PackageBorrowFlushes); // qcount
            metal_commit_locked(*stream);
        }
        metal_release_hold(*stream);
        return true;
    }
#endif
    (void)dispatches;
    (void)bytes;
    error = "Core command stream encoding is not available on this backend";
    return false;
}

bool package_scope_open(int index) {
#ifdef __APPLE__
    // Only the hold's owner thread can see its own stream here, and it reads
    // state that only it writes.
    const auto* held = metal_held_stream;
    return held && held->global_index == index && metal_scope_open(*held);
#else
    (void)index;
    return false;
#endif
}

void note_package_scope_violation(int index, const std::string& work) {
#ifdef __APPLE__
    auto* held = metal_held_stream;
    if (!held || held->global_index != index) return;
    std::string ignored;
    (void)metal_scope_refuses_locked(*held, work, ignored);
#else
    (void)index;
    (void)work;
#endif
}

bool package_hold_active() {
#ifdef __APPLE__
    return metal_held_stream != nullptr;
#else
    return false;
#endif
}

bool note_package_hold_violation(const std::string& work) {
#ifdef __APPLE__
    auto* held = metal_held_stream;
    if (!held) return false;
    metal_record_violation_locked(*held,
        work + " was requested while package code holds the command stream "
        "of gpu(" + std::to_string(held->global_index) + "), where Core "
        "neither commits nor waits: take it before qcore_device_encode_begin "
        "(include/quidra/native_extension.h)");
    return true;
#else
    (void)work;
    return false;
#endif
}

bool package_hold_refuses_device(int index, const std::string& call) {
#ifdef __APPLE__
    auto* held = metal_held_stream;
    if (!held || held->global_index == index) return false;
    // The other device's stream may be held by a thread that waits for this
    // one: refused before anything locks it.
    metal_record_violation_locked(*held,
        call + " for gpu(" + std::to_string(index) + ") was called while the "
        "thread holds the command stream of gpu(" +
        std::to_string(held->global_index) + "): a package encode covers one "
        "device, so take what another device needs before "
        "qcore_device_encode_begin or after qcore_metal_note_work "
        "(include/quidra/native_extension.h)");
    return true;
#else
    (void)index;
    (void)call;
    return false;
#endif
}

LeftHold end_left_package_hold(const char* returned, std::string& message) {
#ifdef __APPLE__
    auto* held = metal_held_stream;
    if (!held || keeping_failed_hold) return LeftHold::None;
    LeftHold left = LeftHold::Violated;
    if (!held->hold_violation.empty()) {
        message = std::move(held->hold_violation);
    } else {
        left = LeftHold::Open;
        counters::add(counters::Id::PackageScopeViolations); // qcount
        message = std::string("the ") + metal_hold_name(*held) + " on gpu(" +
            std::to_string(held->global_index) + ") was still open when " +
            returned + ": every package encode ends with qcore_metal_note_work "
            "before the package code that began it returns, and nothing "
            "encoded in the command buffer it held runs "
            "(include/quidra/native_extension.h)";
    }
    // A callback's failure ends the process without exit
    // handlers (exit_failed_program), so its hold stays: the command buffer
    // it keeps open is never committed, and no other thread (another task,
    // a warm-up, a completion callback) gets the stream to wait for or read
    // work that never runs.
    if (running_package_callback && !runtime::node_failure_active()) {
        keeping_failed_hold = true;
        return left;
    }
    metal_discard_open_locked(*held);
    metal_release_hold(*held);
    return left;
#else
    (void)returned;
    (void)message;
    return LeftHold::None;
#endif
}

void exit_failed_program() {
    // Package callbacks and unreleased failed holds cannot run exit handlers.
    if (running_package_callback || keeping_failed_hold) {
        runtime::immediate_failure_exit();
    }
    std::exit(abi::failure_exit_status);
}

void fail_package_scope_unlocated(const std::string& message) {
    std::fflush(stdout);
    counters::report_unlocated_failure(abi::FailureReason::package_scope_violation, message,
                                       true);
}

void check_package_hold_returned(const char* returned) {
    seal_native_package_work();
    if (open_package_holds.load(std::memory_order_relaxed) == 0) return;
    std::string message;
    if (end_left_package_hold(returned, message) == LeftHold::None) return;
    // No Quidra statement runs here, so the failure has no source location
    // (0:0), and the process ends at once: exit handlers may wait for work
    // this thread was to run, as a deferred GPU failure at exit does.
    fail_package_scope_unlocated(message);
}

bool flush(int index, std::string& error) {
    profile::Range observed("core","gpu flush",index,profile::ProfileEvent::Kind::CommandBuffer);
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (!stream) {
            error = "Metal GPU index is unavailable";
            return false;
        }
        std::lock_guard lock(stream->mutex);
        if (metal_hold_refuses_commit_locked(*stream, "qcore_device_flush", error))
            return false;
        metal_commit_locked(*stream);
        return true;
    }
#endif
    // CUDA/HIP launches and the fake backend submit immediately.
    return true;
}

bool wait_idle(int index, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (!stream) {
            error = "Metal GPU index is unavailable";
            return false;
        }
        return metal_stream_synchronize(*stream, counters::WaitReason::Status,
                                        error, nullptr, false);
    }
#endif
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) return true;
#endif
    // CUDA/HIP: a full device synchronization. Deferred checks stay pending
    // for the next synchronization point, as on Metal, instead of failing
    // this package wait with an unrelated earlier operation's error.
    return synchronize_device(index, false, error);
}

bool on_complete(int index, void (*function)(void*), void* context,
                 std::string& error) {
    if (!function) {
        error = "missing completion callback";
        return false;
    }
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (!stream) {
            error = "Metal GPU index is unavailable";
            return false;
        }
        // Callbacks run on Core's completion queue (see MetalStream), one at
        // a time, after the work completed. Registered ones are counted, so
        // synchronizations and the exit guard can wait for them.
        arm_deferred_validation_exit_guard();
        {
            std::lock_guard lock(stream->mutex);
            // The open command buffer commits after every command buffer
            // committed so far, package ones included. A package hold
            // commits nothing, so the callback rides on the command buffer
            // the hold encodes into.
            if (stream->open ||
                (stream->hold != MetalHold::None && metal_open_locked(*stream, error))) {
                stream->completions_pending.fetch_add(1, std::memory_order_acq_rel);
                stream->open_completions.emplace_back(function,context,index,stream->next_serial.load(std::memory_order_relaxed));
                return true;
            }
            if (stream->hold != MetalHold::None) return false;
            // Packages may have committed command buffers of their own on
            // Core's queue: a marker committed now completes after them.
            if (stream->package_lent) {
                stream->completions_pending.fetch_add(1, std::memory_order_acq_rel);
                std::vector<MetalCompletion> completions;
                completions.emplace_back(function,context,index,stream->next_serial.load(std::memory_order_relaxed));
                if (metal_commit_marker_locked(*stream,error,std::move(completions)))
                    return true;
                stream->completions_pending.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            std::lock_guard done(stream->done_mutex);
            if (!stream->in_flight.empty()) {
                stream->completions_pending.fetch_add(1, std::memory_order_acq_rel);
                stream->in_flight.back().completions.emplace_back(
                    function,context,index,stream->in_flight.back().serial);
                return true;
            }
        }
        if(auto owner=runtime_parallel::detail::deferred_node()) {
            runtime_parallel::detail::DeferredCallback completion(function,context,index,0);
            const auto& outcome=completion.invoke(index,[] {
                check_package_hold_returned("a qcore_device_on_complete callback returned");
            });
            if(owner->raise_failure && (!outcome.message.empty() || outcome.report || outcome.exit_status))
                owner->raise_failure(outcome);
        } else function(context);
        return true;
    }
#endif
    // Other backends: wait, then run the callback.
    if (!wait_idle(index, error)) return false;
    if(auto owner=runtime_parallel::detail::deferred_node()) {
        runtime_parallel::detail::DeferredCallback completion(function,context,index,0);
        const auto& outcome=completion.invoke(index,[] {
            check_package_hold_returned("a qcore_device_on_complete callback returned");
        });
        if(owner->raise_failure && (!outcome.message.empty() || outcome.report || outcome.exit_status))
            owner->raise_failure(outcome);
    } else function(context);
    return true;
}

