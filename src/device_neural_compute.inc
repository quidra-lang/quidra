
namespace {

template <typename T>
bool fake_neural_mean_to(Buffer* output, const Buffer* input,
                         std::size_t count, std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (count == 0) {
        error = "neural.mean requires at least one element";
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
        error = "invalid neural last-axis reduction shape";
        return false;
    }
    for (std::size_t base = 0; base < count; base += width) {
        T reduced{};
        if (operation == 2) {
            std::memcpy(&reduced, input->test_data.data() + base * sizeof(T), sizeof(T));
        }
        for (std::size_t j = 0; j < width; ++j) {
            T value{};
            std::memcpy(&value,
                        input->test_data.data() + (base + j) * sizeof(T),
                        sizeof(T));
            if (operation == 1) reduced = static_cast<T>(reduced + value);
            else if (value > reduced) reduced = value;
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

template <typename T>
bool fake_affine(Buffer* output, const Buffer* input, const Buffer* weight,
                 const Buffer* bias, std::size_t batches,
                 std::size_t features_in, std::size_t features_out) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for (std::size_t batch = 0; batch < batches; ++batch) {
        for (std::size_t out = 0; out < features_out; ++out) {
            T total{};
            std::memcpy(&total, bias->test_data.data() + out * sizeof(T), sizeof(T));
            for (std::size_t in = 0; in < features_in; ++in) {
                T x{}, w{};
                std::memcpy(&x, input->test_data.data() +
                                   (batch * features_in + in) * sizeof(T), sizeof(T));
                std::memcpy(&w, weight->test_data.data() +
                                   (out * features_in + in) * sizeof(T), sizeof(T));
                total = static_cast<T>(total + static_cast<T>(x * w));
            }
            std::memcpy(output->test_data.data() +
                            (batch * features_out + out) * sizeof(T),
                        &total, sizeof(T));
        }
    }
    return true;
#else
    (void)output; (void)input; (void)weight; (void)bias;
    (void)batches; (void)features_in; (void)features_out;
    return false;
#endif
}

template <typename T>
bool fake_conv2d(Buffer* output, const Buffer* input, const Buffer* weight,
                 const Buffer* bias, std::size_t batches,
                 std::size_t channels_in, std::size_t height, std::size_t width,
                 std::size_t channels_out, std::size_t kernel_h,
                 std::size_t kernel_w, std::size_t output_h,
                 std::size_t output_w, std::size_t stride,
                 std::size_t padding) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    auto load = [](const std::vector<unsigned char>& bytes, std::size_t index) {
        T value{};
        std::memcpy(&value, bytes.data() + index * sizeof(T), sizeof(T));
        return value;
    };
    auto store = [](std::vector<unsigned char>& bytes, std::size_t index, T value) {
        std::memcpy(bytes.data() + index * sizeof(T), &value, sizeof(T));
    };
    for (std::size_t n = 0; n < batches; ++n)
        for (std::size_t oc = 0; oc < channels_out; ++oc)
            for (std::size_t oy = 0; oy < output_h; ++oy)
                for (std::size_t ox = 0; ox < output_w; ++ox) {
                    T total = load(bias->test_data, oc);
                    for (std::size_t ic = 0; ic < channels_in; ++ic)
                        for (std::size_t ky = 0; ky < kernel_h; ++ky)
                            for (std::size_t kx = 0; kx < kernel_w; ++kx) {
                                const long long iy =
                                    static_cast<long long>(oy * stride + ky) -
                                    static_cast<long long>(padding);
                                const long long ix =
                                    static_cast<long long>(ox * stride + kx) -
                                    static_cast<long long>(padding);
                                if (iy < 0 || ix < 0 ||
                                    iy >= static_cast<long long>(height) ||
                                    ix >= static_cast<long long>(width)) continue;
                                const auto input_index =
                                    ((n * channels_in + ic) * height +
                                     static_cast<std::size_t>(iy)) * width +
                                    static_cast<std::size_t>(ix);
                                const auto weight_index =
                                    ((oc * channels_in + ic) * kernel_h + ky) *
                                        kernel_w + kx;
                                total = static_cast<T>(
                                    total + static_cast<T>(
                                        load(input->test_data, input_index) *
                                        load(weight->test_data, weight_index)));
                            }
                    const auto output_index =
                        ((n * channels_out + oc) * output_h + oy) *
                            output_w + ox;
                    store(output->test_data, output_index, total);
                }
    return true;
#else
    (void)output; (void)input; (void)weight; (void)bias;
    (void)batches; (void)channels_in; (void)height; (void)width;
    (void)channels_out; (void)kernel_h; (void)kernel_w;
    (void)output_h; (void)output_w; (void)stride; (void)padding;
    return false;
#endif
}

template <typename T>
bool fake_normalize_inference(
    Buffer* output, const Buffer* input, const Buffer* scale,
    const Buffer* bias, const Buffer* mean, const Buffer* variance,
    std::size_t count, std::size_t features, std::size_t inner,
    double epsilon) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    auto load = [](const Buffer* buffer, std::size_t index) {
        T value{};
        std::memcpy(&value, buffer->test_data.data() + index * sizeof(T), sizeof(T));
        return value;
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto feature = (i / inner) % features;
        const T value = static_cast<T>(
            static_cast<T>((load(input, i) - load(mean, feature)) /
                           std::sqrt(static_cast<T>(
                               load(variance, feature) + static_cast<T>(epsilon)))) *
            load(scale, feature) + load(bias, feature));
        std::memcpy(output->test_data.data() + i * sizeof(T), &value, sizeof(T));
    }
    return true;
#else
    (void)output; (void)input; (void)scale; (void)bias; (void)mean; (void)variance;
    (void)count; (void)features; (void)inner; (void)epsilon;
    return false;
#endif
}

std::string ptx_neural_mean_source(int dtype) {
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
    const bool f32 = dtype == 10;
    const auto width_bytes = f32 ? 4 : 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_in,\n"
        "  .param .u64 p_count,\n  .param .u64 p_width\n",
        f32
          ? "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<28>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<28>;\n  .reg .f64 %fd<8>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_in];\n"
           "  ld.param.u64 %rd5, [p_count];\n"
           "  ld.param.u64 %rd6, [p_width];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
           "  div.u64 %rd8, %rd7, %rd6;\n"
           "  mul.lo.u64 %rd9, %rd8, %rd6;\n"
           "  mov.u64 %rd10, 0;\n";
    if (operation == 1) {
        if (f32) out << "  mov.f32 %f1, 0f00000000;\n";
        else out << "  mov.f64 %fd1, 0d0000000000000000;\n";
    } else {
        out << "  mul.lo.u64 %rd11, %rd9, " << width_bytes << ";\n"
               "  add.u64 %rd12, %rd2, %rd11;\n";
        if (f32) out << "  ld.global.f32 %f1, [%rd12];\n";
        else out << "  ld.global.f64 %fd1, [%rd12];\n";
    }
    out << "LOOP:\n"
           "  setp.ge.u64 %p2, %rd10, %rd6;\n"
           "  @%p2 bra STORE;\n"
           "  add.u64 %rd13, %rd9, %rd10;\n"
        << "  mul.lo.u64 %rd14, %rd13, " << width_bytes << ";\n"
           "  add.u64 %rd15, %rd2, %rd14;\n";
    if (f32) {
        out << "  ld.global.f32 %f2, [%rd15];\n";
        if (operation == 1) out << "  add.f32 %f1, %f1, %f2;\n";
        else out << "  max.f32 %f1, %f1, %f2;\n";
    } else {
        out << "  ld.global.f64 %fd2, [%rd15];\n";
        if (operation == 1) out << "  add.f64 %fd1, %fd1, %fd2;\n";
        else
            out << "  setp.gt.f64 %p3, %fd2, %fd1;\n"
                   "  @%p3 mov.f64 %fd1, %fd2;\n";
    }
    out << "  add.u64 %rd10, %rd10, 1;\n"
           "  bra LOOP;\nSTORE:\n"
        << "  mul.lo.u64 %rd16, %rd7, " << width_bytes << ";\n"
           "  add.u64 %rd17, %rd1, %rd16;\n";
    if (f32) out << "  st.global.f32 [%rd17], %f1;\n";
    else out << "  st.global.f64 [%rd17], %fd1;\n";
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_affine_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4 : 8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_input,\n"
        "  .param .u64 p_weight,\n  .param .u64 p_bias,\n"
        "  .param .u64 p_total,\n  .param .u64 p_in,\n"
        "  .param .u64 p_out_features\n",
        f32
          ? "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<36>;\n  .reg .f32 %f<10>;\n"
          : "  .reg .pred %p<4>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<36>;\n  .reg .f64 %fd<10>;\n");
    out << "  ld.param.u64 %rd1, [p_out];\n"
           "  ld.param.u64 %rd2, [p_input];\n"
           "  ld.param.u64 %rd3, [p_weight];\n"
           "  ld.param.u64 %rd4, [p_bias];\n"
           "  ld.param.u64 %rd5, [p_total];\n"
           "  ld.param.u64 %rd6, [p_in];\n"
           "  ld.param.u64 %rd18, [p_out_features];\n"
           "  setp.ge.u64 %p1, %rd7, %rd5;\n"
           "  @%p1 bra DONE;\n"
           "  div.u64 %rd8, %rd7, %rd18;\n"
           "  rem.u64 %rd9, %rd7, %rd18;\n"
        << "  mul.lo.u64 %rd10, %rd9, " << bytes << ";\n"
           "  add.u64 %rd11, %rd4, %rd10;\n";
    if (f32) out << "  ld.global.f32 %f1, [%rd11];\n";
    else out << "  ld.global.f64 %fd1, [%rd11];\n";
    out << "  mov.u64 %rd12, 0;\nLOOP:\n"
           "  setp.ge.u64 %p2, %rd12, %rd6;\n"
           "  @%p2 bra STORE;\n"
           "  mul.lo.u64 %rd13, %rd8, %rd6;\n"
           "  add.u64 %rd13, %rd13, %rd12;\n"
        << "  mul.lo.u64 %rd14, %rd13, " << bytes << ";\n"
           "  add.u64 %rd15, %rd2, %rd14;\n"
           "  mul.lo.u64 %rd16, %rd9, %rd6;\n"
           "  add.u64 %rd16, %rd16, %rd12;\n"
        << "  mul.lo.u64 %rd17, %rd16, " << bytes << ";\n"
           "  add.u64 %rd19, %rd3, %rd17;\n";
    if (f32)
        out << "  ld.global.f32 %f2, [%rd15];\n"
               "  ld.global.f32 %f3, [%rd19];\n"
               "  mul.f32 %f4, %f2, %f3;\n"
               "  add.f32 %f1, %f1, %f4;\n";
    else
        out << "  ld.global.f64 %fd2, [%rd15];\n"
               "  ld.global.f64 %fd3, [%rd19];\n"
               "  mul.f64 %fd4, %fd2, %fd3;\n"
               "  add.f64 %fd1, %fd1, %fd4;\n";
    out << "  add.u64 %rd12, %rd12, 1;\n"
           "  bra LOOP;\nSTORE:\n"
        << "  mul.lo.u64 %rd20, %rd7, " << bytes << ";\n"
           "  add.u64 %rd21, %rd1, %rd20;\n";
    if (f32) out << "  st.global.f32 [%rd21], %f1;\n";
    else out << "  st.global.f64 [%rd21], %fd1;\n";
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_conv2d_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4 : 8;
    std::ostringstream out;
    out << ".version 6.0\n.target sm_30\n.address_size 64\n\n"
           ".visible .entry q_kernel(\n"
           " .param .u64 p_out,\n .param .u64 p_input,\n"
           " .param .u64 p_weight,\n .param .u64 p_bias,\n"
           " .param .u64 p_total,\n .param .u64 p_ci,\n"
           " .param .u64 p_h,\n .param .u64 p_w,\n"
           " .param .u64 p_co,\n .param .u64 p_kh,\n .param .u64 p_kw,\n"
           " .param .u64 p_oh,\n .param .u64 p_ow,\n"
           " .param .u64 p_stride,\n .param .u64 p_padding)\n{\n"
           " .reg .pred %p<8>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<64>;\n"
        << (f32 ? " .reg .f32 %f<12>;\n" : " .reg .f64 %fd<12>;\n")
        << " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_input];\n"
           " ld.param.u64 %rd3,[p_weight];\n ld.param.u64 %rd4,[p_bias];\n"
           " ld.param.u64 %rd5,[p_total];\n ld.param.u64 %rd6,[p_ci];\n"
           " ld.param.u64 %rd20,[p_h];\n ld.param.u64 %rd21,[p_w];\n"
           " ld.param.u64 %rd22,[p_co];\n ld.param.u64 %rd23,[p_kh];\n"
           " ld.param.u64 %rd24,[p_kw];\n ld.param.u64 %rd25,[p_oh];\n"
           " ld.param.u64 %rd26,[p_ow];\n ld.param.u64 %rd27,[p_stride];\n"
           " ld.param.u64 %rd28,[p_padding];\n"
           " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
           " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
           " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
           " rem.u64 %rd8,%rd7,%rd26;\n"
           " div.u64 %rd9,%rd7,%rd26;\n"
           " rem.u64 %rd10,%rd9,%rd25;\n"
           " div.u64 %rd11,%rd9,%rd25;\n"
           " rem.u64 %rd12,%rd11,%rd22;\n"
           " div.u64 %rd13,%rd11,%rd22;\n"
        << " mul.lo.u64 %rd14,%rd12," << bytes << ";\n add.u64 %rd15,%rd4,%rd14;\n";
    if(f32) out<<" ld.global.f32 %f1,[%rd15];\n";
    else out<<" ld.global.f64 %fd1,[%rd15];\n";
    out << " mov.u64 %rd16,0;\nCI_LOOP:\n"
           " setp.ge.u64 %p2,%rd16,%rd6;\n @%p2 bra STORE;\n"
           " mov.u64 %rd17,0;\nKY_LOOP:\n"
           " setp.ge.u64 %p3,%rd17,%rd23;\n @%p3 bra NEXT_CI;\n"
           " mov.u64 %rd18,0;\nKX_LOOP:\n"
           " setp.ge.u64 %p4,%rd18,%rd24;\n @%p4 bra NEXT_KY;\n"
           " mul.lo.u64 %rd29,%rd10,%rd27;\n add.u64 %rd29,%rd29,%rd17;\n"
           " setp.lt.u64 %p5,%rd29,%rd28;\n @%p5 bra SKIP;\n"
           " sub.u64 %rd29,%rd29,%rd28;\n setp.ge.u64 %p6,%rd29,%rd20;\n @%p6 bra SKIP;\n"
           " mul.lo.u64 %rd30,%rd8,%rd27;\n add.u64 %rd30,%rd30,%rd18;\n"
           " setp.lt.u64 %p5,%rd30,%rd28;\n @%p5 bra SKIP;\n"
           " sub.u64 %rd30,%rd30,%rd28;\n setp.ge.u64 %p6,%rd30,%rd21;\n @%p6 bra SKIP;\n"
           " mul.lo.u64 %rd31,%rd13,%rd6;\n add.u64 %rd31,%rd31,%rd16;\n"
           " mul.lo.u64 %rd31,%rd31,%rd20;\n add.u64 %rd31,%rd31,%rd29;\n"
           " mul.lo.u64 %rd31,%rd31,%rd21;\n add.u64 %rd31,%rd31,%rd30;\n"
        << " mul.lo.u64 %rd31,%rd31," << bytes << ";\n add.u64 %rd32,%rd2,%rd31;\n"
           " mul.lo.u64 %rd33,%rd12,%rd6;\n add.u64 %rd33,%rd33,%rd16;\n"
           " mul.lo.u64 %rd33,%rd33,%rd23;\n add.u64 %rd33,%rd33,%rd17;\n"
           " mul.lo.u64 %rd33,%rd33,%rd24;\n add.u64 %rd33,%rd33,%rd18;\n"
        << " mul.lo.u64 %rd33,%rd33," << bytes << ";\n add.u64 %rd34,%rd3,%rd33;\n";
    if(f32)
        out<<" ld.global.f32 %f2,[%rd32];\n ld.global.f32 %f3,[%rd34];\n"
              " mul.f32 %f4,%f2,%f3;\n add.f32 %f1,%f1,%f4;\n";
    else
        out<<" ld.global.f64 %fd2,[%rd32];\n ld.global.f64 %fd3,[%rd34];\n"
              " mul.f64 %fd4,%fd2,%fd3;\n add.f64 %fd1,%fd1,%fd4;\n";
    out<<"SKIP:\n add.u64 %rd18,%rd18,1;\n bra KX_LOOP;\n"
          "NEXT_KY:\n add.u64 %rd17,%rd17,1;\n bra KY_LOOP;\n"
          "NEXT_CI:\n add.u64 %rd16,%rd16,1;\n bra CI_LOOP;\n"
          "STORE:\n"
       << " mul.lo.u64 %rd35,%rd7," << bytes << ";\n add.u64 %rd36,%rd1,%rd35;\n";
    if(f32) out<<" st.global.f32 [%rd36],%f1;\n";
    else out<<" st.global.f64 [%rd36],%fd1;\n";
    out<<"DONE:\n ret;\n}\n";
    return out.str();
}

std::string ptx_normalize_inference_source(int dtype) {
    const bool f32=dtype==10;
    const auto bytes=f32?4:8;
    std::ostringstream out;
    out << ptx_thread_prologue(
        " .param .u64 p_out,\n .param .u64 p_input,\n"
        " .param .u64 p_scale,\n .param .u64 p_bias,\n"
        " .param .u64 p_mean,\n .param .u64 p_variance,\n"
        " .param .u64 p_count,\n .param .u64 p_features,\n"
        " .param .u64 p_inner,\n .param .f64 p_epsilon\n",
        f32
          ? " .reg .pred %p<4>;\n .reg .b32 %r<12>;\n .reg .b64 %rd<32>;\n .reg .f32 %f<12>;\n .reg .f64 %fd<4>;\n"
          : " .reg .pred %p<4>;\n .reg .b32 %r<12>;\n .reg .b64 %rd<32>;\n .reg .f64 %fd<12>;\n");
    out<<" ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_input];\n"
          " ld.param.u64 %rd3,[p_scale];\n ld.param.u64 %rd4,[p_bias];\n"
          " ld.param.u64 %rd5,[p_mean];\n ld.param.u64 %rd6,[p_variance];\n"
          " ld.param.u64 %rd18,[p_count];\n ld.param.u64 %rd19,[p_features];\n"
          " ld.param.u64 %rd20,[p_inner];\n"
          " setp.ge.u64 %p1,%rd7,%rd18;\n @%p1 bra DONE;\n"
          " div.u64 %rd21,%rd7,%rd20;\n rem.u64 %rd22,%rd21,%rd19;\n"
       << " mul.lo.u64 %rd23,%rd7,"<<bytes<<";\n"
          " add.u64 %rd24,%rd2,%rd23;\n add.u64 %rd25,%rd1,%rd23;\n"
       << " mul.lo.u64 %rd26,%rd22,"<<bytes<<";\n"
          " add.u64 %rd27,%rd3,%rd26;\n add.u64 %rd28,%rd4,%rd26;\n"
          " add.u64 %rd29,%rd5,%rd26;\n add.u64 %rd30,%rd6,%rd26;\n";
    if(f32){
        out<<" ld.param.f64 %fd1,[p_epsilon];\n cvt.rn.f32.f64 %f7,%fd1;\n"
              " ld.global.f32 %f1,[%rd24];\n ld.global.f32 %f2,[%rd27];\n"
              " ld.global.f32 %f3,[%rd28];\n ld.global.f32 %f4,[%rd29];\n"
              " ld.global.f32 %f5,[%rd30];\n add.f32 %f5,%f5,%f7;\n"
              " sqrt.rn.f32 %f6,%f5;\n sub.f32 %f1,%f1,%f4;\n"
              " div.f32 %f1,%f1,%f6;\n mul.f32 %f1,%f1,%f2;\n"
              " add.f32 %f1,%f1,%f3;\n st.global.f32 [%rd25],%f1;\n";
    }else{
        out<<" ld.param.f64 %fd7,[p_epsilon];\n"
              " ld.global.f64 %fd1,[%rd24];\n ld.global.f64 %fd2,[%rd27];\n"
              " ld.global.f64 %fd3,[%rd28];\n ld.global.f64 %fd4,[%rd29];\n"
              " ld.global.f64 %fd5,[%rd30];\n add.f64 %fd5,%fd5,%fd7;\n"
              " sqrt.rn.f64 %fd6,%fd5;\n sub.f64 %fd1,%fd1,%fd4;\n"
              " div.f64 %fd1,%fd1,%fd6;\n mul.f64 %fd1,%fd1,%fd2;\n"
              " add.f64 %fd1,%fd1,%fd3;\n st.global.f64 [%rd25],%fd1;\n";
    }
    out<<"DONE:\n ret;\n}\n";
    return out.str();
}

#ifdef __APPLE__
std::string msl_neural_mean_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           " if (gid != 0) return; float total=0.0f;"
           " for(ulong i=0;i<count;++i) total+=in[i];"
           " out[0]=total/float(count); }\n";
}

std::string msl_last_reduce_source(int operation) {
    const char* init = operation==1 ? "0.0f" : "in[base]";
    const char* update = operation==1 ? "reduced += value;" :
        "if(value > reduced) reduced=value;";
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* in [[buffer(1)]], "
           "constant ulong& count [[buffer(2)]], "
           "constant ulong& width [[buffer(3)]], "
           "uint gid [[thread_position_in_grid]]) {\n"
           " if(ulong(gid)>=count)return; ulong base=(ulong(gid)/width)*width;"
           " float reduced="+std::string(init)+";"
           " for(ulong j=0;j<width;++j){float value=in[base+j];"+std::string(update)+"}"
           " out[gid]=reduced; }\n";
}

std::string msl_affine_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]], "
           "const device float* input [[buffer(1)]], "
           "const device float* weight [[buffer(2)]], "
           "const device float* bias [[buffer(3)]], "
           "constant ulong& total [[buffer(4)]], "
           "constant ulong& features_in [[buffer(5)]], "
           "constant ulong& features_out [[buffer(6)]], "
           "uint gid [[thread_position_in_grid]]) {"
           " if(ulong(gid)>=total)return; ulong batch=ulong(gid)/features_out;"
           " ulong o=ulong(gid)%features_out; float sum=bias[o];"
           " for(ulong i=0;i<features_in;++i)"
           " sum+=input[batch*features_in+i]*weight[o*features_in+i];"
           " out[gid]=sum; }\n";
}

std::string msl_conv2d_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* input [[buffer(1)]],"
           "const device float* weight [[buffer(2)]],"
           "const device float* bias [[buffer(3)]],"
           "constant ulong& total [[buffer(4)]],"
           "constant ulong& ci [[buffer(5)]],constant ulong& h [[buffer(6)]],"
           "constant ulong& w [[buffer(7)]],constant ulong& co [[buffer(8)]],"
           "constant ulong& kh [[buffer(9)]],constant ulong& kw [[buffer(10)]],"
           "constant ulong& oh [[buffer(11)]],constant ulong& ow [[buffer(12)]],"
           "constant ulong& stride [[buffer(13)]],constant ulong& padding [[buffer(14)]],"
           "uint gid [[thread_position_in_grid]]) {"
           " ulong g=ulong(gid); if(g>=total)return; ulong ox=g%ow; g/=ow;"
           " ulong oy=g%oh; g/=oh; ulong oc=g%co; ulong n=g/co; float sum=bias[oc];"
           " for(ulong ic=0;ic<ci;++ic)for(ulong y=0;y<kh;++y)for(ulong x=0;x<kw;++x){"
           " long iy=long(oy*stride+y)-long(padding); long ix=long(ox*stride+x)-long(padding);"
           " if(iy<0||ix<0||iy>=long(h)||ix>=long(w))continue;"
           " ulong ii=((n*ci+ic)*h+ulong(iy))*w+ulong(ix);"
           " ulong wi=((oc*ci+ic)*kh+y)*kw+x; sum+=input[ii]*weight[wi];}"
           " out[gid]=sum; }\n";
}

std::string msl_normalize_inference_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* input [[buffer(1)]],const device float* scale [[buffer(2)]],"
           "const device float* bias [[buffer(3)]],const device float* mean [[buffer(4)]],"
           "const device float* variance [[buffer(5)]],constant ulong& count [[buffer(6)]],"
           "constant ulong& features [[buffer(7)]],constant ulong& inner [[buffer(8)]],"
           "constant float& epsilon [[buffer(9)]],uint gid [[thread_position_in_grid]]) {"
           " ulong i=ulong(gid); if(i>=count)return; ulong f=(i/inner)%features;"
           " out[i]=((input[i]-mean[f])/sqrt(variance[f]+epsilon))*scale[f]+bias[f]; }\n";
}
#endif

} // namespace

bool compute_mean_to(Buffer* output, const Buffer* input, int dtype,
                     std::size_t count, std::string& error) {
    const auto width = compute_dtype_width(dtype);
    if (count == 0) { error = "neural.mean requires at least one element"; return false; }
    if (!compute_same_device(output,input,error) ||
        !compute_buffer_range(output,0,1,width,error) ||
        !compute_buffer_range(input,0,count,width,error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (output->backend==Backend::Test) {
        if(dtype==10) return fake_neural_mean_to<float>(output,input,count,error);
        if(dtype==9) return fake_neural_mean_to<double>(output,input,count,error);
        error="neural.mean requires a floating dtype"; return false;
    }
#endif
    if ((dtype==9||dtype==10) && output->backend==Backend::Nvidia) {
        auto ptx=ptx_neural_mean_source(dtype);
        auto out_ptr=output->cuda_pointer, in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&in_ptr,&n};
        auto* module=cached_ptx_module(output->global_index,
            "neural-mean-"+std::to_string(dtype),ptx,error);
        if(!module)return false;
        return launch(module,"q_kernel",LaunchDimensions{1,1,1},
                      LaunchDimensions{1,1,1},args,error);
    }
#ifdef __APPLE__
    if(dtype==10 && output->backend==Backend::Metal){
        auto source=msl_neural_mean_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],
            "neural-mean-f32",source,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,1,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.mean",dtype,error);
}

bool compute_last_reduce_broadcast(Buffer* output, const Buffer* input, int dtype,
                                   std::size_t count, std::size_t width,
                                   int operation, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(width==0||count%width!=0||(operation!=1&&operation!=2)){
        error="invalid neural last-axis reduction";return false;
    }
    if(!compute_same_device(output,input,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_last_reduce<float>(output,input,count,width,operation,error);
        if(dtype==9)return fake_last_reduce<double>(output,input,count,width,operation,error);
        error="neural last-axis reduction requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_last_reduce_source(dtype,operation);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        auto w=static_cast<std::uint64_t>(width);
        void* args[]={&out_ptr,&in_ptr,&n,&w};
        return launch_ptx_1d(output,
            "neural-last-"+std::to_string(dtype)+"-"+std::to_string(operation),
            ptx,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_last_reduce_source(operation);
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],
            "neural-last-f32-"+std::to_string(operation),source,error);
        if(!pipeline)return false;
        const auto n=static_cast<std::uint64_t>(count);
        const auto w=static_cast<std::uint64_t>(width);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
            [e setBytes:&w length:sizeof(w) atIndex:3];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural last-axis reduction",dtype,error);
}

bool compute_affine(Buffer* output, const Buffer* input, const Buffer* weight,
                    const Buffer* bias, int dtype, std::size_t batches,
                    std::size_t features_in, std::size_t features_out,
                    std::string& error) {
    const auto bytes=compute_dtype_width(dtype), total=batches*features_out;
    if(!compute_same_device(output,input,error)||!compute_same_device(output,weight,error)||
       !compute_same_device(output,bias,error)||
       !compute_buffer_range(output,0,total,bytes,error)||
       !compute_buffer_range(input,0,batches*features_in,bytes,error)||
       !compute_buffer_range(weight,0,features_out*features_in,bytes,error)||
       !compute_buffer_range(bias,0,features_out,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_affine<float>(output,input,weight,bias,batches,features_in,features_out);
        if(dtype==9)return fake_affine<double>(output,input,weight,bias,batches,features_in,features_out);
        error="neural.affine requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_affine_source(dtype);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer,w_ptr=weight->cuda_pointer,b_ptr=bias->cuda_pointer;
        auto total64=static_cast<std::uint64_t>(total),in64=static_cast<std::uint64_t>(features_in),out64=static_cast<std::uint64_t>(features_out);
        void* args[]={&out_ptr,&in_ptr,&w_ptr,&b_ptr,&total64,&in64,&out64};
        return launch_ptx_1d(output,"neural-affine-"+std::to_string(dtype),ptx,total,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_affine_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-affine-f32",source,error);
        if(!pipeline)return false;
        const auto total64=static_cast<std::uint64_t>(total),in64=static_cast<std::uint64_t>(features_in),out64=static_cast<std::uint64_t>(features_out);
        return dispatch_metal_1d(output,pipeline,total,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBuffer:weight->metal_buffer offset:0 atIndex:2];
            [e setBuffer:bias->metal_buffer offset:0 atIndex:3];
            [e setBytes:&total64 length:sizeof(total64) atIndex:4];
            [e setBytes:&in64 length:sizeof(in64) atIndex:5];
            [e setBytes:&out64 length:sizeof(out64) atIndex:6];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.affine",dtype,error);
}

bool compute_conv2d(Buffer* output, const Buffer* input, const Buffer* weight,
                    const Buffer* bias, int dtype,
                    std::size_t batches, std::size_t channels_in,
                    std::size_t height, std::size_t width,
                    std::size_t channels_out, std::size_t kernel_h,
                    std::size_t kernel_w, std::size_t output_h,
                    std::size_t output_w, std::size_t stride,
                    std::size_t padding, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    const auto total=batches*channels_out*output_h*output_w;
    if(!compute_same_device(output,input,error)||!compute_same_device(output,weight,error)||
       !compute_same_device(output,bias,error)||
       !compute_buffer_range(output,0,total,bytes,error)||
       !compute_buffer_range(input,0,batches*channels_in*height*width,bytes,error)||
       !compute_buffer_range(weight,0,channels_out*channels_in*kernel_h*kernel_w,bytes,error)||
       !compute_buffer_range(bias,0,channels_out,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_conv2d<float>(output,input,weight,bias,batches,channels_in,height,width,channels_out,kernel_h,kernel_w,output_h,output_w,stride,padding);
        if(dtype==9)return fake_conv2d<double>(output,input,weight,bias,batches,channels_in,height,width,channels_out,kernel_h,kernel_w,output_h,output_w,stride,padding);
        error="neural.convolve2d requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_conv2d_source(dtype);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer,w_ptr=weight->cuda_pointer,b_ptr=bias->cuda_pointer;
        auto total64=static_cast<std::uint64_t>(total),ci=static_cast<std::uint64_t>(channels_in),hh=static_cast<std::uint64_t>(height),ww=static_cast<std::uint64_t>(width),co=static_cast<std::uint64_t>(channels_out),kh=static_cast<std::uint64_t>(kernel_h),kw=static_cast<std::uint64_t>(kernel_w),oh=static_cast<std::uint64_t>(output_h),ow=static_cast<std::uint64_t>(output_w),st=static_cast<std::uint64_t>(stride),pa=static_cast<std::uint64_t>(padding);
        void* args[]={&out_ptr,&in_ptr,&w_ptr,&b_ptr,&total64,&ci,&hh,&ww,&co,&kh,&kw,&oh,&ow,&st,&pa};
        return launch_ptx_1d(output,"neural-conv2d-"+std::to_string(dtype),ptx,total,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_conv2d_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-conv2d-f32",source,error);
        if(!pipeline)return false;
        auto total64=static_cast<std::uint64_t>(total),ci=static_cast<std::uint64_t>(channels_in),hh=static_cast<std::uint64_t>(height),ww=static_cast<std::uint64_t>(width),co=static_cast<std::uint64_t>(channels_out),kh=static_cast<std::uint64_t>(kernel_h),kw=static_cast<std::uint64_t>(kernel_w),oh=static_cast<std::uint64_t>(output_h),ow=static_cast<std::uint64_t>(output_w),st=static_cast<std::uint64_t>(stride),pa=static_cast<std::uint64_t>(padding);
        return dispatch_metal_1d(output,pipeline,total,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0]; [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBuffer:weight->metal_buffer offset:0 atIndex:2]; [e setBuffer:bias->metal_buffer offset:0 atIndex:3];
            [e setBytes:&total64 length:8 atIndex:4]; [e setBytes:&ci length:8 atIndex:5]; [e setBytes:&hh length:8 atIndex:6];
            [e setBytes:&ww length:8 atIndex:7]; [e setBytes:&co length:8 atIndex:8]; [e setBytes:&kh length:8 atIndex:9];
            [e setBytes:&kw length:8 atIndex:10]; [e setBytes:&oh length:8 atIndex:11]; [e setBytes:&ow length:8 atIndex:12];
            [e setBytes:&st length:8 atIndex:13]; [e setBytes:&pa length:8 atIndex:14];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.convolve2d",dtype,error);
}

bool compute_normalize_inference(
    Buffer* output, const Buffer* input, const Buffer* scale,
    const Buffer* bias, const Buffer* mean, const Buffer* variance,
    int dtype, std::size_t count, std::size_t features,
    std::size_t inner, double epsilon, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(features==0||inner==0||!compute_same_device(output,input,error)||
       !compute_same_device(output,scale,error)||!compute_same_device(output,bias,error)||
       !compute_same_device(output,mean,error)||!compute_same_device(output,variance,error)||
       !compute_buffer_range(output,0,count,bytes,error)||!compute_buffer_range(input,0,count,bytes,error)||
       !compute_buffer_range(scale,0,features,bytes,error)||!compute_buffer_range(bias,0,features,bytes,error)||
       !compute_buffer_range(mean,0,features,bytes,error)||!compute_buffer_range(variance,0,features,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_normalize_inference<float>(output,input,scale,bias,mean,variance,count,features,inner,epsilon);
        if(dtype==9)return fake_normalize_inference<double>(output,input,scale,bias,mean,variance,count,features,inner,epsilon);
        error="neural.normalize_inference requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_normalize_inference_source(dtype);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer,s_ptr=scale->cuda_pointer,b_ptr=bias->cuda_pointer,m_ptr=mean->cuda_pointer,v_ptr=variance->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),in=static_cast<std::uint64_t>(inner);
        double eps=epsilon;
        void* args[]={&out_ptr,&in_ptr,&s_ptr,&b_ptr,&m_ptr,&v_ptr,&n,&f,&in,&eps};
        return launch_ptx_1d(output,"neural-normalize-inference-"+std::to_string(dtype),ptx,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_normalize_inference_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-normalize-inference-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),f=static_cast<std::uint64_t>(features),in=static_cast<std::uint64_t>(inner);
        float eps=static_cast<float>(epsilon);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0]; [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBuffer:scale->metal_buffer offset:0 atIndex:2]; [e setBuffer:bias->metal_buffer offset:0 atIndex:3];
            [e setBuffer:mean->metal_buffer offset:0 atIndex:4]; [e setBuffer:variance->metal_buffer offset:0 atIndex:5];
            [e setBytes:&n length:8 atIndex:6]; [e setBytes:&f length:8 atIndex:7]; [e setBytes:&in length:8 atIndex:8];
            [e setBytes:&eps length:4 atIndex:9];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.normalize_inference",dtype,error);
}
