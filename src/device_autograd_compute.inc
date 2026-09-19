
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
        std::memcpy(left_gradient->test_data.data()+i*sizeof(T),&left_value,sizeof(T));
        std::memcpy(right_gradient->test_data.data()+i*sizeof(T),&right_value,sizeof(T));
    }
    return true;
#else
    (void)left_gradient;(void)right_gradient;(void)gradient;(void)left;(void)right;
    (void)operation;(void)count;
    return false;
#endif
}

template <typename T>
bool fake_abs_backward(Buffer* output, const Buffer* gradient,
                       const Buffer* input, std::size_t count) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for (std::size_t i = 0; i < count; ++i) {
        T g{}, x{}, value{};
        std::memcpy(&g, gradient->test_data.data() + i * sizeof(T), sizeof(T));
        std::memcpy(&x, input->test_data.data() + i * sizeof(T), sizeof(T));
        value = x > T{0} ? g : x < T{0} ? static_cast<T>(-g) : T{0};
        std::memcpy(output->test_data.data() + i * sizeof(T), &value, sizeof(T));
    }
    return true;
#else
    (void)output; (void)gradient; (void)input; (void)count;
    return false;
#endif
}

template <typename T>
bool fake_mean_backward(Buffer* output, const Buffer* gradient,
                        std::size_t count) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    T g{};
    std::memcpy(&g, gradient->test_data.data(), sizeof(T));
    const T value = static_cast<T>(g / static_cast<T>(count));
    for (std::size_t i = 0; i < count; ++i)
        std::memcpy(output->test_data.data() + i * sizeof(T), &value, sizeof(T));
    return true;
#else
    (void)output; (void)gradient; (void)count;
    return false;
#endif
}

template <typename T>
bool fake_max_last_backward(Buffer* output, const Buffer* gradient,
                            const Buffer* input, std::size_t count,
                            std::size_t width) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for (std::size_t base = 0; base < count; base += width) {
        std::size_t selected = 0;
        T selected_value{};
        std::memcpy(&selected_value,
                    input->test_data.data() + base * sizeof(T), sizeof(T));
        for (std::size_t j = 1; j < width; ++j) {
            T value{};
            std::memcpy(&value,
                        input->test_data.data() + (base + j) * sizeof(T),
                        sizeof(T));
            if (value > selected_value) {
                selected = j;
                selected_value = value;
            }
        }
        T total{};
        for (std::size_t j = 0; j < width; ++j) {
            T g{};
            std::memcpy(&g,
                        gradient->test_data.data() + (base + j) * sizeof(T),
                        sizeof(T));
            total = static_cast<T>(total + g);
        }
        for (std::size_t j = 0; j < width; ++j) {
            const T value = j == selected ? total : T{0};
            std::memcpy(output->test_data.data() + (base + j) * sizeof(T),
                        &value, sizeof(T));
        }
    }
    return true;
#else
    (void)output; (void)gradient; (void)input; (void)count; (void)width;
    return false;
#endif
}

template <typename T>
bool fake_affine_backward(
    Buffer* input_gradient, Buffer* weight_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* weight,
    std::size_t batches, std::size_t features_in, std::size_t features_out) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    auto load = [](const Buffer* buffer, std::size_t index) {
        T value{};
        std::memcpy(&value, buffer->test_data.data() + index * sizeof(T), sizeof(T));
        return value;
    };
    auto store = [](Buffer* buffer, std::size_t index, T value) {
        std::memcpy(buffer->test_data.data() + index * sizeof(T), &value, sizeof(T));
    };
    for (std::size_t batch = 0; batch < batches; ++batch) {
        for (std::size_t in = 0; in < features_in; ++in) {
            T total{};
            for (std::size_t out = 0; out < features_out; ++out)
                total = static_cast<T>(
                    total + static_cast<T>(
                        load(gradient, batch * features_out + out) *
                        load(weight, out * features_in + in)));
            store(input_gradient, batch * features_in + in, total);
        }
    }
    for (std::size_t out = 0; out < features_out; ++out) {
        for (std::size_t in = 0; in < features_in; ++in) {
            T total{};
            for (std::size_t batch = 0; batch < batches; ++batch)
                total = static_cast<T>(
                    total + static_cast<T>(
                        load(gradient, batch * features_out + out) *
                        load(input, batch * features_in + in)));
            store(weight_gradient, out * features_in + in, total);
        }
        T bias_total{};
        for (std::size_t batch = 0; batch < batches; ++batch)
            bias_total = static_cast<T>(
                bias_total + load(gradient, batch * features_out + out));
        store(bias_gradient, out, bias_total);
    }
    return true;
#else
    (void)input_gradient; (void)weight_gradient; (void)bias_gradient;
    (void)gradient; (void)input; (void)weight;
    (void)batches; (void)features_in; (void)features_out;
    return false;
#endif
}

std::string ptx_binary_backward_source(int dtype,int operation) {
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
        out<<"  st.global.f64 [%rd12],%fd4;\n"
              "  st.global.f64 [%rd13],%fd5;\n";
    }
    out<<"DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_abs_backward_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4U : 8U;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_grad,\n"
        "  .param .u64 p_input,\n  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<24>;\n  .reg .f64 %fd<8>;\n");
    out << "  ld.param.u64 %rd1,[p_out];\n"
           "  ld.param.u64 %rd2,[p_grad];\n"
           "  ld.param.u64 %rd3,[p_input];\n"
           "  ld.param.u64 %rd5,[p_count];\n"
           "  setp.ge.u64 %p1,%rd7,%rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8,%rd7," << bytes << ";\n"
           "  add.u64 %rd9,%rd2,%rd8;\n"
           "  add.u64 %rd10,%rd3,%rd8;\n"
           "  add.u64 %rd11,%rd1,%rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1,[%rd9];\n"
               "  ld.global.f32 %f2,[%rd10];\n"
               "  setp.gt.f32 %p2,%f2,0f00000000;\n"
               "  @%p2 bra POS;\n"
               "  setp.lt.f32 %p3,%f2,0f00000000;\n"
               "  @%p3 bra NEG;\n"
               "  mov.f32 %f3,0f00000000;\n"
               "  bra STORE;\n"
               "POS:\n  mov.f32 %f3,%f1;\n  bra STORE;\n"
               "NEG:\n  neg.f32 %f3,%f1;\n"
               "STORE:\n  st.global.f32 [%rd11],%f3;\n";
    } else {
        out << "  ld.global.f64 %fd1,[%rd9];\n"
               "  ld.global.f64 %fd2,[%rd10];\n"
               "  setp.gt.f64 %p2,%fd2,0d0000000000000000;\n"
               "  @%p2 bra POS;\n"
               "  setp.lt.f64 %p3,%fd2,0d0000000000000000;\n"
               "  @%p3 bra NEG;\n"
               "  mov.f64 %fd3,0d0000000000000000;\n"
               "  bra STORE;\n"
               "POS:\n  mov.f64 %fd3,%fd1;\n  bra STORE;\n"
               "NEG:\n  neg.f64 %fd3,%fd1;\n"
               "STORE:\n  st.global.f64 [%rd11],%fd3;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_mean_backward_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4U : 8U;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_grad,\n"
        "  .param .u64 p_count\n",
        f32
          ? "  .reg .pred %p<3>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<20>;\n  .reg .f32 %f<8>;\n"
          : "  .reg .pred %p<3>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<20>;\n  .reg .f64 %fd<8>;\n");
    out << "  ld.param.u64 %rd1,[p_out];\n"
           "  ld.param.u64 %rd2,[p_grad];\n"
           "  ld.param.u64 %rd5,[p_count];\n"
           "  setp.ge.u64 %p1,%rd7,%rd5;\n"
           "  @%p1 bra DONE;\n"
        << "  mul.lo.u64 %rd8,%rd7," << bytes << ";\n"
           "  add.u64 %rd9,%rd1,%rd8;\n";
    if (f32) {
        out << "  ld.global.f32 %f1,[%rd2];\n"
               "  cvt.rn.f32.u64 %f2,%rd5;\n"
               "  div.f32 %f3,%f1,%f2;\n"
               "  st.global.f32 [%rd9],%f3;\n";
    } else {
        out << "  ld.global.f64 %fd1,[%rd2];\n"
               "  cvt.rn.f64.u64 %fd2,%rd5;\n"
               "  div.f64 %fd3,%fd1,%fd2;\n"
               "  st.global.f64 [%rd9],%fd3;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_max_last_backward_source(int dtype) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4U : 8U;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_grad,\n"
        "  .param .u64 p_input,\n  .param .u64 p_count,\n"
        "  .param .u64 p_width\n",
        f32
          ? "  .reg .pred %p<8>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<36>;\n  .reg .f32 %f<10>;\n"
          : "  .reg .pred %p<8>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<36>;\n  .reg .f64 %fd<10>;\n");
    out << "  ld.param.u64 %rd1,[p_out];\n"
           "  ld.param.u64 %rd2,[p_grad];\n"
           "  ld.param.u64 %rd3,[p_input];\n"
           "  ld.param.u64 %rd5,[p_count];\n"
           "  ld.param.u64 %rd6,[p_width];\n"
           "  setp.ge.u64 %p1,%rd7,%rd5;\n"
           "  @%p1 bra DONE;\n"
           "  div.u64 %rd8,%rd7,%rd6;\n"
           "  mul.lo.u64 %rd9,%rd8,%rd6;\n"
        << "  mul.lo.u64 %rd10,%rd9," << bytes << ";\n"
           "  add.u64 %rd11,%rd3,%rd10;\n"
           "  mov.u64 %rd12,0;\n"
           "  mov.u64 %rd13,1;\n";
    if (f32)
        out << "  ld.global.f32 %f1,[%rd11];\n";
    else
        out << "  ld.global.f64 %fd1,[%rd11];\n";
    out << "SELECT_LOOP:\n"
           "  setp.ge.u64 %p2,%rd13,%rd6;\n"
           "  @%p2 bra SUM_BEGIN;\n"
           "  add.u64 %rd14,%rd9,%rd13;\n"
        << "  mul.lo.u64 %rd15,%rd14," << bytes << ";\n"
           "  add.u64 %rd16,%rd3,%rd15;\n";
    if (f32) {
        out << "  ld.global.f32 %f2,[%rd16];\n"
               "  setp.gt.f32 %p3,%f2,%f1;\n"
               "  @!%p3 bra SELECT_NEXT;\n"
               "  mov.f32 %f1,%f2;\n";
    } else {
        out << "  ld.global.f64 %fd2,[%rd16];\n"
               "  setp.gt.f64 %p3,%fd2,%fd1;\n"
               "  @!%p3 bra SELECT_NEXT;\n"
               "  mov.f64 %fd1,%fd2;\n";
    }
    out << "  mov.u64 %rd12,%rd13;\n"
           "SELECT_NEXT:\n"
           "  add.u64 %rd13,%rd13,1;\n"
           "  bra SELECT_LOOP;\n"
           "SUM_BEGIN:\n"
           "  mov.u64 %rd17,0;\n";
    if (f32) out << "  mov.f32 %f3,0f00000000;\n";
    else out << "  mov.f64 %fd3,0d0000000000000000;\n";
    out << "SUM_LOOP:\n"
           "  setp.ge.u64 %p4,%rd17,%rd6;\n"
           "  @%p4 bra WRITE;\n"
           "  add.u64 %rd18,%rd9,%rd17;\n"
        << "  mul.lo.u64 %rd19,%rd18," << bytes << ";\n"
           "  add.u64 %rd20,%rd2,%rd19;\n";
    if (f32)
        out << "  ld.global.f32 %f4,[%rd20];\n"
               "  add.f32 %f3,%f3,%f4;\n";
    else
        out << "  ld.global.f64 %fd4,[%rd20];\n"
               "  add.f64 %fd3,%fd3,%fd4;\n";
    out << "  add.u64 %rd17,%rd17,1;\n"
           "  bra SUM_LOOP;\n"
           "WRITE:\n"
           "  rem.u64 %rd21,%rd7,%rd6;\n"
           "  setp.eq.u64 %p5,%rd21,%rd12;\n"
        << "  mul.lo.u64 %rd22,%rd7," << bytes << ";\n"
           "  add.u64 %rd23,%rd1,%rd22;\n";
    if (f32) {
        out << "  @%p5 bra WRITE_SELECTED;\n"
               "  mov.f32 %f5,0f00000000;\n"
               "  st.global.f32 [%rd23],%f5;\n"
               "  bra DONE;\n"
               "WRITE_SELECTED:\n"
               "  st.global.f32 [%rd23],%f3;\n";
    } else {
        out << "  @%p5 bra WRITE_SELECTED;\n"
               "  mov.f64 %fd5,0d0000000000000000;\n"
               "  st.global.f64 [%rd23],%fd5;\n"
               "  bra DONE;\n"
               "WRITE_SELECTED:\n"
               "  st.global.f64 [%rd23],%fd3;\n";
    }
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}

std::string ptx_affine_backward_source(int dtype, int target) {
    const bool f32 = dtype == 10;
    const auto bytes = f32 ? 4U : 8U;
    std::ostringstream out;
    out << ptx_thread_prologue(
        "  .param .u64 p_out,\n  .param .u64 p_grad,\n"
        "  .param .u64 p_input,\n  .param .u64 p_weight,\n"
        "  .param .u64 p_count,\n  .param .u64 p_batches,\n"
        "  .param .u64 p_in,\n  .param .u64 p_out_features\n",
        f32
          ? "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<40>;\n  .reg .f32 %f<10>;\n"
          : "  .reg .pred %p<5>;\n  .reg .b32 %r<12>;\n  .reg .b64 %rd<40>;\n  .reg .f64 %fd<10>;\n");
    out << "  ld.param.u64 %rd1,[p_out];\n"
           "  ld.param.u64 %rd2,[p_grad];\n"
           "  ld.param.u64 %rd3,[p_input];\n"
           "  ld.param.u64 %rd4,[p_weight];\n"
           "  ld.param.u64 %rd5,[p_count];\n"
           "  ld.param.u64 %rd18,[p_batches];\n"
           "  ld.param.u64 %rd19,[p_in];\n"
           "  ld.param.u64 %rd20,[p_out_features];\n"
           "  setp.ge.u64 %p1,%rd7,%rd5;\n"
           "  @%p1 bra DONE;\n";
    if (f32) out << "  mov.f32 %f1,0f00000000;\n";
    else out << "  mov.f64 %fd1,0d0000000000000000;\n";

    if (target == 1) {
        // input gradient: gid => batch,input_feature
        out << "  div.u64 %rd8,%rd7,%rd19;\n"
               "  rem.u64 %rd9,%rd7,%rd19;\n"
               "  mov.u64 %rd10,0;\n"
               "LOOP:\n"
               "  setp.ge.u64 %p2,%rd10,%rd20;\n"
               "  @%p2 bra STORE;\n"
               "  mul.lo.u64 %rd11,%rd8,%rd20;\n"
               "  add.u64 %rd11,%rd11,%rd10;\n"
            << "  mul.lo.u64 %rd12,%rd11," << bytes << ";\n"
               "  add.u64 %rd13,%rd2,%rd12;\n"
               "  mul.lo.u64 %rd14,%rd10,%rd19;\n"
               "  add.u64 %rd14,%rd14,%rd9;\n"
            << "  mul.lo.u64 %rd15,%rd14," << bytes << ";\n"
               "  add.u64 %rd16,%rd4,%rd15;\n";
    } else if (target == 2) {
        // weight gradient: gid => output_feature,input_feature
        out << "  div.u64 %rd8,%rd7,%rd19;\n"
               "  rem.u64 %rd9,%rd7,%rd19;\n"
               "  mov.u64 %rd10,0;\n"
               "LOOP:\n"
               "  setp.ge.u64 %p2,%rd10,%rd18;\n"
               "  @%p2 bra STORE;\n"
               "  mul.lo.u64 %rd11,%rd10,%rd20;\n"
               "  add.u64 %rd11,%rd11,%rd8;\n"
            << "  mul.lo.u64 %rd12,%rd11," << bytes << ";\n"
               "  add.u64 %rd13,%rd2,%rd12;\n"
               "  mul.lo.u64 %rd14,%rd10,%rd19;\n"
               "  add.u64 %rd14,%rd14,%rd9;\n"
            << "  mul.lo.u64 %rd15,%rd14," << bytes << ";\n"
               "  add.u64 %rd16,%rd3,%rd15;\n";
    } else {
        // bias gradient: gid => output_feature
        out << "  mov.u64 %rd8,%rd7;\n"
               "  mov.u64 %rd10,0;\n"
               "LOOP:\n"
               "  setp.ge.u64 %p2,%rd10,%rd18;\n"
               "  @%p2 bra STORE;\n"
               "  mul.lo.u64 %rd11,%rd10,%rd20;\n"
               "  add.u64 %rd11,%rd11,%rd8;\n"
            << "  mul.lo.u64 %rd12,%rd11," << bytes << ";\n"
               "  add.u64 %rd13,%rd2,%rd12;\n";
    }

    if (f32) {
        out << "  ld.global.f32 %f2,[%rd13];\n";
        if (target != 3) out << "  ld.global.f32 %f3,[%rd16];\n  mul.f32 %f4,%f2,%f3;\n";
        else out << "  mov.f32 %f4,%f2;\n";
        out << "  add.f32 %f1,%f1,%f4;\n";
    } else {
        out << "  ld.global.f64 %fd2,[%rd13];\n";
        if (target != 3) out << "  ld.global.f64 %fd3,[%rd16];\n  mul.f64 %fd4,%fd2,%fd3;\n";
        else out << "  mov.f64 %fd4,%fd2;\n";
        out << "  add.f64 %fd1,%fd1,%fd4;\n";
    }
    out << "  add.u64 %rd10,%rd10,1;\n"
           "  bra LOOP;\n"
           "STORE:\n"
        << "  mul.lo.u64 %rd30,%rd7," << bytes << ";\n"
           "  add.u64 %rd31,%rd1,%rd30;\n";
    if (f32) out << "  st.global.f32 [%rd31],%f1;\n";
    else out << "  st.global.f64 [%rd31],%fd1;\n";
    out << "DONE:\n  ret;\n}\n";
    return out.str();
}


std::string hip_binary_backward_source(int dtype,int operation) {
    const auto type=dtype==10?"float":"double";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* left_gradient,"<<type
       <<"* right_gradient,const "<<type<<"* gradient,const "<<type
       <<"* left,const "<<type<<"* right,unsigned long long count){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;"<<type<<" g=gradient[i],a=left[i],b=right[i];";
    if(operation==1)
        out<<"left_gradient[i]=g;right_gradient[i]=g;";
    else if(operation==2)
        out<<"left_gradient[i]=g;right_gradient[i]=-g;";
    else if(operation==3)
        out<<"left_gradient[i]=g*b;right_gradient[i]=g*a;";
    else
        out<<type<<" ga=g*a,bb=b*b;"
              "left_gradient[i]=g/b;right_gradient[i]=-(ga/bb);";
    out<<"}";
    return out.str();
}

std::string hip_abs_backward_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
      "* out,const "+type+"* grad,const "+type+"* input,unsigned long long count){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
      +type+" x=input[i],g=grad[i];out[i]=x>("+type+")0?g:(x<("+type+")0?-g:("+type+")0);}";
}
std::string hip_mean_backward_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
      "* out,const "+type+"* grad,unsigned long long count){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i<count)out[i]=grad[0]/("+type+")count;}";
}
std::string hip_max_last_backward_source(int dtype) {
    const auto type=dtype==10?"float":"double";
    return "extern \"C\" __global__ void q_kernel("+std::string(type)+
      "* out,const "+type+"* grad,const "+type+"* input,unsigned long long count,unsigned long long width){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;"
      "unsigned long long base=(i/width)*width,selected=0;"+type+" total=("+type+")0;"
      "for(unsigned long long j=0;j<width;++j){total+=grad[base+j];if(j&&input[base+j]>input[base+selected])selected=j;}"
      "out[i]=(i==base+selected)?total:("+type+")0;}";
}
std::string hip_affine_backward_source(int dtype,int target) {
    const auto type=dtype==10?"float":"double";
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* grad,const "<<type<<"* input,const "<<type
       <<"* weight,unsigned long long count,unsigned long long batches,"
         "unsigned long long fin,unsigned long long fout){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;";
    if(target==1) out<<"unsigned long long b=i/fin,f=i%fin;"<<type<<" total=("<<type<<")0;"
                       "for(unsigned long long o=0;o<fout;++o)total+=grad[b*fout+o]*weight[o*fin+f];out[i]=total;";
    else if(target==2) out<<"unsigned long long o=i/fin,f=i%fin;"<<type<<" total=("<<type<<")0;"
                            "for(unsigned long long b=0;b<batches;++b)total+=grad[b*fout+o]*input[b*fin+f];out[i]=total;";
    else out<<"unsigned long long o=i;"<<type<<" total=("<<type<<")0;"
              "for(unsigned long long b=0;b<batches;++b)total+=grad[b*fout+o];out[i]=total;";
    out<<"}";
    return out.str();
}

#ifdef __APPLE__

std::string msl_binary_backward_source(int operation) {
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
         " float g=gradient[i],a=left[i],b=right[i];";
    if(operation==1)
        out<<" left_gradient[i]=g; right_gradient[i]=g;";
    else if(operation==2)
        out<<" left_gradient[i]=g; right_gradient[i]=-g;";
    else if(operation==3)
        out<<" left_gradient[i]=g*b; right_gradient[i]=g*a;";
    else
        out<<" float ga=g*a,bb=b*b;"
             " left_gradient[i]=g/b; right_gradient[i]=-(ga/bb);";
    out<<" }\n";
    return out.str();
}

std::string msl_abs_backward_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* grad [[buffer(1)]],"
           "const device float* input [[buffer(2)]],"
           "constant ulong& count [[buffer(3)]],"
           "uint gid [[thread_position_in_grid]]) {"
           " ulong i=ulong(gid); if(i>=count)return;"
           " float x=input[i]; out[i]=x>0.0f?grad[i]:x<0.0f?-grad[i]:0.0f; }\n";
}

std::string msl_mean_backward_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* grad [[buffer(1)]],"
           "constant ulong& count [[buffer(2)]],"
           "uint gid [[thread_position_in_grid]]) {"
           " ulong i=ulong(gid); if(i>=count)return; out[i]=grad[0]/float(count); }\n";
}

std::string msl_max_last_backward_source() {
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* grad [[buffer(1)]],"
           "const device float* input [[buffer(2)]],"
           "constant ulong& count [[buffer(3)]],"
           "constant ulong& width [[buffer(4)]],"
           "uint gid [[thread_position_in_grid]]) {"
           " ulong i=ulong(gid); if(i>=count)return; ulong base=(i/width)*width;"
           " ulong selected=0; float best=input[base];"
           " for(ulong j=1;j<width;++j){float v=input[base+j];if(v>best){best=v;selected=j;}}"
           " float total=0.0f; for(ulong j=0;j<width;++j)total+=grad[base+j];"
           " out[i]=((i-base)==selected)?total:0.0f; }\n";
}

std::string msl_affine_backward_source(int target) {
    if (target == 1)
        return "#include <metal_stdlib>\nusing namespace metal;\n"
               "kernel void q_kernel(device float* out [[buffer(0)]],"
               "const device float* grad [[buffer(1)]],"
               "const device float* input [[buffer(2)]],"
               "const device float* weight [[buffer(3)]],"
               "constant ulong& count [[buffer(4)]],constant ulong& batches [[buffer(5)]],"
               "constant ulong& fin [[buffer(6)]],constant ulong& fout [[buffer(7)]],"
               "uint gid [[thread_position_in_grid]]) {"
               " ulong i=ulong(gid); if(i>=count)return; ulong b=i/fin, f=i%fin;"
               " float total=0.0f; for(ulong o=0;o<fout;++o) total+=grad[b*fout+o]*weight[o*fin+f];"
               " out[i]=total; }\n";
    if (target == 2)
        return "#include <metal_stdlib>\nusing namespace metal;\n"
               "kernel void q_kernel(device float* out [[buffer(0)]],"
               "const device float* grad [[buffer(1)]],"
               "const device float* input [[buffer(2)]],"
               "const device float* weight [[buffer(3)]],"
               "constant ulong& count [[buffer(4)]],constant ulong& batches [[buffer(5)]],"
               "constant ulong& fin [[buffer(6)]],constant ulong& fout [[buffer(7)]],"
               "uint gid [[thread_position_in_grid]]) {"
               " ulong i=ulong(gid); if(i>=count)return; ulong o=i/fin, f=i%fin;"
               " float total=0.0f; for(ulong b=0;b<batches;++b) total+=grad[b*fout+o]*input[b*fin+f];"
               " out[i]=total; }\n";
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           "kernel void q_kernel(device float* out [[buffer(0)]],"
           "const device float* grad [[buffer(1)]],"
           "const device float* input [[buffer(2)]],"
           "const device float* weight [[buffer(3)]],"
           "constant ulong& count [[buffer(4)]],constant ulong& batches [[buffer(5)]],"
           "constant ulong& fin [[buffer(6)]],constant ulong& fout [[buffer(7)]],"
           "uint gid [[thread_position_in_grid]]) {"
           " ulong o=ulong(gid); if(o>=count)return; float total=0.0f;"
           " for(ulong b=0;b<batches;++b) total+=grad[b*fout+o]; out[o]=total; }\n";
}
#endif

} // namespace

bool compute_binary_backward(
    Buffer* left_gradient,Buffer* right_gradient,
    const Buffer* gradient,const Buffer* left,const Buffer* right,
    int dtype,int operation,std::size_t count,std::string& error) {
    if(operation<1||operation>4){
        error="invalid neural binary backward operation";
        return false;
    }
    const auto bytes=compute_dtype_width(dtype);
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
        error="neural binary backward requires a floating dtype";
        return false;
    }
#endif
    if((dtype==9||dtype==10)&&left_gradient->backend==Backend::Nvidia){
        auto ptx=ptx_binary_backward_source(dtype,operation);
        auto lg=left_gradient->cuda_pointer,rg=right_gradient->cuda_pointer,
             g=gradient->cuda_pointer,a=left->cuda_pointer,b=right->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&lg,&rg,&g,&a,&b,&n};
        return launch_ptx_1d(
            left_gradient,
            "neural-binary-backward-"+std::to_string(dtype)+"-"+std::to_string(operation),
            ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&left_gradient->backend==Backend::Amd){
        auto source=hip_binary_backward_source(dtype,operation);
        auto lg=left_gradient->pointer,rg=right_gradient->pointer,
             g=gradient->pointer,a=left->pointer,b=right->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&lg,&rg,&g,&a,&b,&n};
        return launch_hip_1d(
            left_gradient,
            "neural-binary-backward-"+std::to_string(dtype)+"-"+std::to_string(operation),
            source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&left_gradient->backend==Backend::Metal){
        auto source=msl_binary_backward_source(operation);
        auto pipeline=cached_metal_pipeline(
            [left_gradient->metal_buffer device],
            "neural-binary-backward-f32-"+std::to_string(operation),source,error);
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
        left_gradient,"neural binary backward",dtype,error);
}

bool compute_abs_backward(Buffer* output, const Buffer* gradient,
                          const Buffer* input, int dtype,
                          std::size_t count, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(!compute_same_device(output,gradient,error)||
       !compute_same_device(output,input,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error)) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_abs_backward<float>(output,gradient,input,count);
        if(dtype==9)return fake_abs_backward<double>(output,gradient,input,count);
        error="neural absolute backward requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_abs_backward_source(dtype);
        auto out_ptr=output->cuda_pointer,g_ptr=gradient->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&g_ptr,&in_ptr,&n};
        return launch_ptx_1d(output,"neural-abs-backward-"+std::to_string(dtype),ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Amd){
        auto source=hip_abs_backward_source(dtype);
        auto out_ptr=output->pointer,g_ptr=gradient->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&g_ptr,&in_ptr,&n};
        return launch_hip_1d(output,"neural-abs-backward-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_abs_backward_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-abs-backward-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:gradient->metal_buffer offset:0 atIndex:1];
            [e setBuffer:input->metal_buffer offset:0 atIndex:2];
            [e setBytes:&n length:sizeof(n) atIndex:3];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.absolute backward",dtype,error);
}

bool compute_mean_backward(Buffer* output, const Buffer* gradient_scalar,
                           int dtype, std::size_t count, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(count==0){error="mean gradient requires at least one element";return false;}
    if(!compute_same_device(output,gradient_scalar,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(gradient_scalar,0,1,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_mean_backward<float>(output,gradient_scalar,count);
        if(dtype==9)return fake_mean_backward<double>(output,gradient_scalar,count);
        error="neural mean backward requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_mean_backward_source(dtype);
        auto out_ptr=output->cuda_pointer,g_ptr=gradient_scalar->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&g_ptr,&n};
        return launch_ptx_1d(output,"neural-mean-backward-"+std::to_string(dtype),ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Amd){
        auto source=hip_mean_backward_source(dtype);
        auto out_ptr=output->pointer,g_ptr=gradient_scalar->pointer;
        auto n=static_cast<std::uint64_t>(count);
        void* args[]={&out_ptr,&g_ptr,&n};
        return launch_hip_1d(output,"neural-mean-backward-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_mean_backward_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-mean-backward-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:gradient_scalar->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:sizeof(n) atIndex:2];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.mean backward",dtype,error);
}

bool compute_max_last_backward(Buffer* output, const Buffer* gradient,
                               const Buffer* input, int dtype,
                               std::size_t count, std::size_t width,
                               std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    if(width==0||count%width!=0){error="invalid max_last gradient shape";return false;}
    if(!compute_same_device(output,gradient,error)||
       !compute_same_device(output,input,error)||
       !compute_buffer_range(output,0,count,bytes,error)||
       !compute_buffer_range(gradient,0,count,bytes,error)||
       !compute_buffer_range(input,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        if(dtype==10)return fake_max_last_backward<float>(output,gradient,input,count,width);
        if(dtype==9)return fake_max_last_backward<double>(output,gradient,input,count,width);
        error="neural max_last backward requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&output->backend==Backend::Nvidia){
        auto ptx=ptx_max_last_backward_source(dtype);
        auto out_ptr=output->cuda_pointer,g_ptr=gradient->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),w=static_cast<std::uint64_t>(width);
        void* args[]={&out_ptr,&g_ptr,&in_ptr,&n,&w};
        return launch_ptx_1d(output,"neural-max-last-backward-"+std::to_string(dtype),ptx,count,args,error);
    }
    if((dtype==9||dtype==10)&&output->backend==Backend::Amd){
        auto source=hip_max_last_backward_source(dtype);
        auto out_ptr=output->pointer,g_ptr=gradient->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count),w=static_cast<std::uint64_t>(width);
        void* args[]={&out_ptr,&g_ptr,&in_ptr,&n,&w};
        return launch_hip_1d(output,"neural-max-last-backward-"+std::to_string(dtype),source,count,args,error);
    }
#ifdef __APPLE__
    if(dtype==10&&output->backend==Backend::Metal){
        auto source=msl_max_last_backward_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"neural-max-last-backward-f32",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),w=static_cast<std::uint64_t>(width);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:gradient->metal_buffer offset:0 atIndex:1];
            [e setBuffer:input->metal_buffer offset:0 atIndex:2];
            [e setBytes:&n length:sizeof(n) atIndex:3];
            [e setBytes:&w length:sizeof(w) atIndex:4];
        },error);
    }
#endif
    return real_compute_unavailable(output,"neural.max_last backward",dtype,error);
}


bool cublas_affine_backward(
    Buffer* input_gradient, Buffer* weight_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* weight,
    int dtype, std::size_t batches, std::size_t features_in,
    std::size_t features_out) {
    if(input_gradient->backend!=Backend::Nvidia||(dtype!=9&&dtype!=10))return false;
    if(batches>static_cast<std::size_t>(std::numeric_limits<int>::max())||
       features_in>static_cast<std::size_t>(std::numeric_limits<int>::max())||
       features_out>static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    auto& api=cublas();
    if(!api.ready)return false;
    std::string ignored;
    auto handle=api.handle(input_gradient->backend_index,ignored);
    if(!handle)return false;
    auto operation_mutex=api.operation_mutex(input_gradient->backend_index);
    std::lock_guard operation_lock(*operation_mutex);
    const auto mode=dnn_mode();
    if(api.set_atomics_mode){
        constexpr CublasApi::AtomicsMode not_allowed=0,allowed=1;
        (void)api.set_atomics_mode(
            handle,mode==DnnMode::Deterministic?not_allowed:allowed);
    }
    constexpr CublasApi::Operation op_n=0,op_t=1;
    const int b=static_cast<int>(batches);
    const int k=static_cast<int>(features_in);
    const int o=static_cast<int>(features_out);
    int status=0;
    if(dtype==10){
        const float alpha=1.0f,beta=0.0f;
        status=api.sgemm(
            handle,op_n,op_n,k,b,o,&alpha,
            reinterpret_cast<const float*>(
                static_cast<std::uintptr_t>(weight->cuda_pointer)),k,
            reinterpret_cast<const float*>(
                static_cast<std::uintptr_t>(gradient->cuda_pointer)),o,
            &beta,
            reinterpret_cast<float*>(
                static_cast<std::uintptr_t>(input_gradient->cuda_pointer)),k);
        if(status==0)status=api.sgemm(
            handle,op_n,op_t,k,o,b,&alpha,
            reinterpret_cast<const float*>(
                static_cast<std::uintptr_t>(input->cuda_pointer)),k,
            reinterpret_cast<const float*>(
                static_cast<std::uintptr_t>(gradient->cuda_pointer)),o,
            &beta,
            reinterpret_cast<float*>(
                static_cast<std::uintptr_t>(weight_gradient->cuda_pointer)),k);
    }else{
        const double alpha=1.0,beta=0.0;
        status=api.dgemm(
            handle,op_n,op_n,k,b,o,&alpha,
            reinterpret_cast<const double*>(
                static_cast<std::uintptr_t>(weight->cuda_pointer)),k,
            reinterpret_cast<const double*>(
                static_cast<std::uintptr_t>(gradient->cuda_pointer)),o,
            &beta,
            reinterpret_cast<double*>(
                static_cast<std::uintptr_t>(input_gradient->cuda_pointer)),k);
        if(status==0)status=api.dgemm(
            handle,op_n,op_t,k,o,b,&alpha,
            reinterpret_cast<const double*>(
                static_cast<std::uintptr_t>(input->cuda_pointer)),k,
            reinterpret_cast<const double*>(
                static_cast<std::uintptr_t>(gradient->cuda_pointer)),o,
            &beta,
            reinterpret_cast<double*>(
                static_cast<std::uintptr_t>(weight_gradient->cuda_pointer)),k);
    }
    if(status!=0)return false;

    auto ptx=ptx_affine_backward_source(dtype,3);
    auto out_ptr=bias_gradient->cuda_pointer,g_ptr=gradient->cuda_pointer,
         in_ptr=input->cuda_pointer,w_ptr=weight->cuda_pointer;
    auto count64=static_cast<std::uint64_t>(features_out),
         batches64=static_cast<std::uint64_t>(batches),
         in64=static_cast<std::uint64_t>(features_in),
         out64=static_cast<std::uint64_t>(features_out);
    void* args[]={&out_ptr,&g_ptr,&in_ptr,&w_ptr,&count64,&batches64,&in64,&out64};
    std::string reduction_error;
    return launch_ptx_1d(
        bias_gradient,"cublas-affine-backward-bias-"+std::to_string(dtype),
        ptx,features_out,args,reduction_error);
}

bool compute_affine_backward(
    Buffer* input_gradient, Buffer* weight_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* weight,
    int dtype, std::size_t batches, std::size_t features_in,
    std::size_t features_out, std::string& error) {
    const auto bytes=compute_dtype_width(dtype);
    const auto input_count=batches*features_in;
    const auto weight_count=features_out*features_in;
    const auto gradient_count=batches*features_out;
    if(!compute_same_device(input_gradient,gradient,error)||
       !compute_same_device(input_gradient,input,error)||
       !compute_same_device(input_gradient,weight,error)||
       !compute_same_device(input_gradient,weight_gradient,error)||
       !compute_same_device(input_gradient,bias_gradient,error)||
       !compute_buffer_range(input_gradient,0,input_count,bytes,error)||
       !compute_buffer_range(weight_gradient,0,weight_count,bytes,error)||
       !compute_buffer_range(bias_gradient,0,features_out,bytes,error)||
       !compute_buffer_range(gradient,0,gradient_count,bytes,error)||
       !compute_buffer_range(input,0,input_count,bytes,error)||
       !compute_buffer_range(weight,0,weight_count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(input_gradient->backend==Backend::Test){
        if(dtype==10)return fake_affine_backward<float>(
            input_gradient,weight_gradient,bias_gradient,gradient,input,weight,
            batches,features_in,features_out);
        if(dtype==9)return fake_affine_backward<double>(
            input_gradient,weight_gradient,bias_gradient,gradient,input,weight,
            batches,features_in,features_out);
        error="neural affine backward requires a floating dtype";return false;
    }
#endif
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Nvidia){
        if(cublas_affine_backward(input_gradient,weight_gradient,bias_gradient,
                                  gradient,input,weight,dtype,batches,features_in,
                                  features_out))
            return true;
        auto launch_target=[&](Buffer* target,int kind,std::size_t count)->bool{
            auto ptx=ptx_affine_backward_source(dtype,kind);
            auto out_ptr=target->cuda_pointer,g_ptr=gradient->cuda_pointer,
                 in_ptr=input->cuda_pointer,w_ptr=weight->cuda_pointer;
            auto count64=static_cast<std::uint64_t>(count),
                 batches64=static_cast<std::uint64_t>(batches),
                 in64=static_cast<std::uint64_t>(features_in),
                 out64=static_cast<std::uint64_t>(features_out);
            void* args[]={&out_ptr,&g_ptr,&in_ptr,&w_ptr,&count64,&batches64,&in64,&out64};
            return launch_ptx_1d(target,
                "neural-affine-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),
                ptx,count,args,error);
        };
        return launch_target(input_gradient,1,input_count) &&
               launch_target(weight_gradient,2,weight_count) &&
               launch_target(bias_gradient,3,features_out);
    }
    if((dtype==9||dtype==10)&&input_gradient->backend==Backend::Amd){
        auto launch_target=[&](Buffer* target,int kind,std::size_t target_count)->bool{
            auto source=hip_affine_backward_source(dtype,kind);
            auto out_ptr=target->pointer,g_ptr=gradient->pointer,in_ptr=input->pointer,w_ptr=weight->pointer;
            auto count64=static_cast<std::uint64_t>(target_count),b64=static_cast<std::uint64_t>(batches),
                 fin64=static_cast<std::uint64_t>(features_in),fout64=static_cast<std::uint64_t>(features_out);
            void* args[]={&out_ptr,&g_ptr,&in_ptr,&w_ptr,&count64,&b64,&fin64,&fout64};
            return launch_hip_1d(target,
                "neural-affine-backward-"+std::to_string(dtype)+"-"+std::to_string(kind),
                source,target_count,args,error);
        };
        return launch_target(input_gradient,1,input_count)&&
               launch_target(weight_gradient,2,weight_count)&&
               launch_target(bias_gradient,3,features_out);
    }
#ifdef __APPLE__
    if(dtype==10&&input_gradient->backend==Backend::Metal){
        auto launch_target=[&](Buffer* target,int kind,std::size_t count)->bool{
            auto source=msl_affine_backward_source(kind);
            auto pipeline=cached_metal_pipeline([target->metal_buffer device],
                "neural-affine-backward-f32-"+std::to_string(kind),source,error);
            if(!pipeline)return false;
            auto count64=static_cast<std::uint64_t>(count),
                 batches64=static_cast<std::uint64_t>(batches),
                 in64=static_cast<std::uint64_t>(features_in),
                 out64=static_cast<std::uint64_t>(features_out);
            return dispatch_metal_1d(target,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
                [e setBuffer:target->metal_buffer offset:0 atIndex:0];
                [e setBuffer:gradient->metal_buffer offset:0 atIndex:1];
                [e setBuffer:input->metal_buffer offset:0 atIndex:2];
                [e setBuffer:weight->metal_buffer offset:0 atIndex:3];
                [e setBytes:&count64 length:8 atIndex:4];
                [e setBytes:&batches64 length:8 atIndex:5];
                [e setBytes:&in64 length:8 atIndex:6];
                [e setBytes:&out64 length:8 atIndex:7];
            },error);
        };
        return launch_target(input_gradient,1,input_count) &&
               launch_target(weight_gradient,2,weight_count) &&
               launch_target(bias_gradient,3,features_out);
    }
#endif
    return real_compute_unavailable(input_gradient,"neural.affine backward",dtype,error);
}
