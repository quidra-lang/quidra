
namespace {

std::size_t vision_dtype_width(int dtype) {
    return compute_dtype_width(dtype);
}

template <typename T>
T vision_load_test(const Buffer* buffer, std::size_t index) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    T value{};
    std::memcpy(&value, buffer->test_data.data() + index * sizeof(T), sizeof(T));
    return value;
#else
    (void)buffer; (void)index;
    return T{};
#endif
}

template <typename T>
void vision_store_test(Buffer* buffer, std::size_t index, T value) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    std::memcpy(buffer->test_data.data() + index * sizeof(T), &value, sizeof(T));
#else
    (void)buffer; (void)index; (void)value;
#endif
}

[[maybe_unused]] bool fake_image_geometry(
    Buffer* output,const Buffer* input,std::size_t width_bytes,
    std::size_t channels,std::size_t input_height,std::size_t input_width,
    std::size_t output_height,std::size_t output_width,
    int operation,std::size_t p0,std::size_t p1,std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto output_count=channels*output_height*output_width;
    for(std::size_t linear=0;linear<output_count;++linear){
        const auto x=linear%output_width;
        const auto y=(linear/output_width)%output_height;
        const auto c=linear/(output_width*output_height);
        std::size_t sy=0,sx=0;
        switch(operation){
            case 1: sy=p0+y; sx=p1+x; break; // crop
            case 2: sy=y*input_height/output_height; sx=x*input_width/output_width; break;
            case 3: sy=y; sx=input_width-1-x; break;
            case 4: sy=input_height-1-y; sx=x; break;
            case 5: sy=input_height-1-x; sx=y; break; // clockwise 90
            case 6: sy=x; sx=input_width-1-y; break; // clockwise 270
            case 7: sy=input_height-1-y; sx=input_width-1-x; break; // 180
            default: error="invalid image geometry operation";return false;
        }
        if(sy>=input_height||sx>=input_width){
            error="image geometry source coordinate is outside input";return false;
        }
        const auto source=(c*input_height+sy)*input_width+sx;
        std::memcpy(output->test_data.data()+linear*width_bytes,
                    input->test_data.data()+source*width_bytes,width_bytes);
    }
    return true;
#else
    (void)output;(void)input;(void)width_bytes;(void)channels;(void)input_height;
    (void)input_width;(void)output_height;(void)output_width;(void)operation;
    (void)p0;(void)p1;
    error="test GPU backend is unavailable";return false;
#endif
}

[[maybe_unused]] bool fake_image_grayscale(
    Buffer* output,const Buffer* input,std::size_t channels,
    std::size_t height,std::size_t width) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto pixels=height*width;
    for(std::size_t i=0;i<pixels;++i){
        std::uint8_t value{};
        if(channels==1){
            value=input->test_data[i];
        }else{
            const double r=static_cast<double>(input->test_data[i]);
            const double g=static_cast<double>(input->test_data[pixels+i]);
            const double b=static_cast<double>(input->test_data[2*pixels+i]);
            auto rounded=std::llround(0.299*r+0.587*g+0.114*b);
            rounded=std::clamp<long long>(rounded,0,255);
            value=static_cast<std::uint8_t>(rounded);
        }
        output->test_data[i]=value;
    }
    return true;
#else
    (void)output;(void)input;(void)channels;(void)height;(void)width;
    return false;
#endif
}

[[maybe_unused]] bool fake_image_threshold(
    Buffer* output,const Buffer* input,std::size_t count,
    std::uint8_t cutoff,std::uint8_t low,std::uint8_t high) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for(std::size_t i=0;i<count;++i)
        output->test_data[i]=input->test_data[i]>=cutoff?high:low;
    return true;
#else
    (void)output;(void)input;(void)count;(void)cutoff;(void)low;(void)high;
    return false;
#endif
}

[[maybe_unused]] bool fake_image_blur(
    Buffer* output,const Buffer* input,std::size_t channels,
    std::size_t height,std::size_t width,std::size_t radius) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                std::uint64_t total=0,count=0;
                const auto y0=y>radius?y-radius:0;
                const auto y1=std::min(height-1,y+std::min(radius,height-1-y));
                const auto x0=x>radius?x-radius:0;
                const auto x1=std::min(width-1,x+std::min(radius,width-1-x));
                for(std::size_t sy=y0;sy<=y1;++sy)
                    for(std::size_t sx=x0;sx<=x1;++sx){
                        total+=input->test_data[(c*height+sy)*width+sx];
                        ++count;
                    }
                output->test_data[(c*height+y)*width+x]=
                    static_cast<std::uint8_t>(total/count);
            }
    return true;
#else
    (void)output;(void)input;(void)channels;(void)height;(void)width;(void)radius;
    return false;
#endif
}

[[maybe_unused]] bool fake_image_filter(
    Buffer* output,const Buffer* input,const Buffer* kernel,
    std::size_t channels,std::size_t height,std::size_t width,
    std::size_t kh,std::size_t kw,std::int64_t divisor,std::int64_t offset,
    std::string& error) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    const auto cy=kh/2,cx=kw/2;
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                std::int64_t total=0;
                for(std::size_t ky=0;ky<kh;++ky){
                    const auto sy_signed=static_cast<long long>(y)+
                        static_cast<long long>(ky)-static_cast<long long>(cy);
                    if(sy_signed<0||sy_signed>=static_cast<long long>(height))continue;
                    for(std::size_t kx=0;kx<kw;++kx){
                        const auto sx_signed=static_cast<long long>(x)+
                            static_cast<long long>(kx)-static_cast<long long>(cx);
                        if(sx_signed<0||sx_signed>=static_cast<long long>(width))continue;
                        std::int64_t weight{};
                        std::memcpy(&weight,kernel->test_data.data()+(ky*kw+kx)*8,8);
                        const auto pixel=input->test_data[
                            (c*height+static_cast<std::size_t>(sy_signed))*width+
                            static_cast<std::size_t>(sx_signed)];
                        std::int64_t product{},next{};
                        if(!fake_mul(static_cast<std::int64_t>(pixel),weight,product)||
                           !fake_add(total,product,next)){
                            error="vision.filter integer arithmetic overflow";
                            return false;
                        }
                        total=next;
                    }
                }
                if(total==std::numeric_limits<std::int64_t>::min()&&divisor==-1){
                    error="vision.filter integer arithmetic overflow";
                    return false;
                }
                auto value=static_cast<std::int64_t>(total/divisor);
                std::int64_t with_offset{};
                if(!fake_add(value,offset,with_offset)){
                    error="vision.filter integer arithmetic overflow";
                    return false;
                }
                value=with_offset;
                value=std::clamp<std::int64_t>(value,0,255);
                output->test_data[(c*height+y)*width+x]=
                    static_cast<std::uint8_t>(value);
            }
    return true;
#else
    (void)output;(void)input;(void)kernel;(void)channels;(void)height;(void)width;
    (void)kh;(void)kw;(void)divisor;(void)offset;
    error="test GPU backend is unavailable";return false;
#endif
}

template <typename T>
bool fake_image_morphology(
    Buffer* output,const Buffer* input,std::size_t channels,
    std::size_t height,std::size_t width,std::size_t radius,bool dilate) {
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    for(std::size_t c=0;c<channels;++c)
        for(std::size_t y=0;y<height;++y)
            for(std::size_t x=0;x<width;++x){
                auto best=vision_load_test<T>(input,(c*height+y)*width+x);
                const auto y0=y>radius?y-radius:0;
                const auto y1=std::min(height-1,y+std::min(radius,height-1-y));
                const auto x0=x>radius?x-radius:0;
                const auto x1=std::min(width-1,x+std::min(radius,width-1-x));
                for(std::size_t sy=y0;sy<=y1;++sy)
                    for(std::size_t sx=x0;sx<=x1;++sx){
                        const auto candidate=vision_load_test<T>(
                            input,(c*height+sy)*width+sx);
                        if(dilate?candidate>best:candidate<best)best=candidate;
                    }
                vision_store_test<T>(output,(c*height+y)*width+x,best);
            }
    return true;
#else
    (void)output;(void)input;(void)channels;(void)height;(void)width;(void)radius;(void)dilate;
    return false;
#endif
}

std::string ptx_image_geometry_source(std::size_t width_bytes,int operation) {
    std::ostringstream out;
    out << ptx_thread_prologue(
        " .param .u64 p_out,\n .param .u64 p_in,\n .param .u64 p_count,\n"
        " .param .u64 p_ih,\n .param .u64 p_iw,\n .param .u64 p_oh,\n"
        " .param .u64 p_ow,\n .param .u64 p_p0,\n .param .u64 p_p1\n",
        " .reg .pred %p<6>;\n .reg .b32 %r<12>;\n .reg .b64 %rd<40>;\n");
    out<<" ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n"
          " ld.param.u64 %rd5,[p_count];\n ld.param.u64 %rd10,[p_ih];\n"
          " ld.param.u64 %rd11,[p_iw];\n ld.param.u64 %rd12,[p_oh];\n"
          " ld.param.u64 %rd13,[p_ow];\n ld.param.u64 %rd14,[p_p0];\n"
          " ld.param.u64 %rd15,[p_p1];\n setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
          " rem.u64 %rd16,%rd7,%rd13;\n div.u64 %rd17,%rd7,%rd13;\n"
          " rem.u64 %rd18,%rd17,%rd12;\n div.u64 %rd19,%rd17,%rd12;\n";
    if(operation==1){
        out<<" add.u64 %rd20,%rd14,%rd18;\n add.u64 %rd21,%rd15,%rd16;\n";
    }else if(operation==2){
        out<<" mul.lo.u64 %rd20,%rd18,%rd10;\n div.u64 %rd20,%rd20,%rd12;\n"
              " mul.lo.u64 %rd21,%rd16,%rd11;\n div.u64 %rd21,%rd21,%rd13;\n";
    }else if(operation==3){
        out<<" mov.u64 %rd20,%rd18;\n sub.u64 %rd21,%rd11,1;\n sub.u64 %rd21,%rd21,%rd16;\n";
    }else if(operation==4){
        out<<" sub.u64 %rd20,%rd10,1;\n sub.u64 %rd20,%rd20,%rd18;\n mov.u64 %rd21,%rd16;\n";
    }else if(operation==5){
        out<<" sub.u64 %rd20,%rd10,1;\n sub.u64 %rd20,%rd20,%rd16;\n mov.u64 %rd21,%rd18;\n";
    }else if(operation==7){
        out<<" sub.u64 %rd20,%rd10,1;\n sub.u64 %rd20,%rd20,%rd18;\n"
              " sub.u64 %rd21,%rd11,1;\n sub.u64 %rd21,%rd21,%rd16;\n";
    }else{
        out<<" mov.u64 %rd20,%rd16;\n sub.u64 %rd21,%rd11,1;\n sub.u64 %rd21,%rd21,%rd18;\n";
    }
    out<<" mul.lo.u64 %rd22,%rd19,%rd10;\n add.u64 %rd22,%rd22,%rd20;\n"
          " mul.lo.u64 %rd22,%rd22,%rd11;\n add.u64 %rd22,%rd22,%rd21;\n"
       << " mul.lo.u64 %rd23,%rd22,"<<width_bytes<<";\n add.u64 %rd24,%rd2,%rd23;\n"
       << " mul.lo.u64 %rd25,%rd7,"<<width_bytes<<";\n add.u64 %rd26,%rd1,%rd25;\n";
    if(width_bytes==1)out<<" ld.global.u8 %r5,[%rd24];\n st.global.u8 [%rd26],%r5;\n";
    else if(width_bytes==2)out<<" ld.global.u16 %r5,[%rd24];\n st.global.u16 [%rd26],%r5;\n";
    else if(width_bytes==4)out<<" ld.global.u32 %r5,[%rd24];\n st.global.u32 [%rd26],%r5;\n";
    else out<<" ld.global.u64 %rd27,[%rd24];\n st.global.u64 [%rd26],%rd27;\n";
    out<<"DONE:\n ret;\n}\n";
    return out.str();
}

std::string ptx_image_grayscale_source() {
    return
        ".version 6.0\n.target sm_30\n.address_size 64\n\n"
        ".visible .entry q_kernel(\n"
        " .param .u64 p_out,\n .param .u64 p_in,\n .param .u64 p_pixels,\n"
        " .param .u64 p_channels)\n{\n"
        " .reg .pred %p<4>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<24>;\n .reg .f64 %fd<12>;\n"
        " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n"
        " ld.param.u64 %rd5,[p_pixels];\n ld.param.u64 %rd6,[p_channels];\n"
        " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
        " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
        " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
        " setp.eq.u64 %p2,%rd6,1;\n @%p2 bra COPY;\n"
        " add.u64 %rd8,%rd2,%rd7;\n ld.global.u8 %r5,[%rd8];\n"
        " add.u64 %rd9,%rd7,%rd5;\n add.u64 %rd9,%rd2,%rd9;\n ld.global.u8 %r6,[%rd9];\n"
        " mul.lo.u64 %rd10,%rd5,2;\n add.u64 %rd10,%rd10,%rd7;\n add.u64 %rd10,%rd2,%rd10;\n ld.global.u8 %r7,[%rd10];\n"
        " cvt.rn.f64.u32 %fd1,%r5;\n cvt.rn.f64.u32 %fd2,%r6;\n cvt.rn.f64.u32 %fd3,%r7;\n"
        " mul.f64 %fd1,%fd1,0d3FD322D0E5604189;\n"
        " mul.f64 %fd2,%fd2,0d3FE2C8B439581062;\n"
        " mul.f64 %fd3,%fd3,0d3FBD2F1A9FBE76C9;\n"
        " add.f64 %fd4,%fd1,%fd2;\n add.f64 %fd4,%fd4,%fd3;\n"
        " cvt.rni.s32.f64 %r8,%fd4;\n"
        " min.s32 %r8,%r8,255;\n max.s32 %r8,%r8,0;\n"
        " add.u64 %rd11,%rd1,%rd7;\n st.global.u8 [%rd11],%r8;\n bra DONE;\n"
        "COPY:\n add.u64 %rd8,%rd2,%rd7;\n ld.global.u8 %r5,[%rd8];\n"
        " add.u64 %rd11,%rd1,%rd7;\n st.global.u8 [%rd11],%r5;\n"
        "DONE:\n ret;\n}\n";
}

std::string ptx_image_threshold_source() {
    return
        ".version 6.0\n.target sm_30\n.address_size 64\n\n"
        ".visible .entry q_kernel(\n .param .u64 p_out,\n .param .u64 p_in,\n"
        " .param .u64 p_count,\n .param .u32 p_cutoff,\n .param .u32 p_low,\n .param .u32 p_high)\n{\n"
        " .reg .pred %p<4>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<20>;\n"
        " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n ld.param.u64 %rd5,[p_count];\n"
        " ld.param.u32 %r8,[p_cutoff];\n ld.param.u32 %r9,[p_low];\n ld.param.u32 %r10,[p_high];\n"
        " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
        " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
        " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
        " add.u64 %rd8,%rd2,%rd7;\n ld.global.u8 %r5,[%rd8];\n"
        " setp.ge.u32 %p2,%r5,%r8;\n selp.u32 %r6,%r10,%r9,%p2;\n"
        " add.u64 %rd9,%rd1,%rd7;\n st.global.u8 [%rd9],%r6;\n"
        "DONE:\n ret;\n}\n";
}

std::string ptx_image_blur_source() {
    return
        ".version 6.0\n.target sm_30\n.address_size 64\n\n"
        ".visible .entry q_kernel(\n .param .u64 p_out,\n .param .u64 p_in,\n"
        " .param .u64 p_count,\n .param .u64 p_h,\n .param .u64 p_w,\n .param .u64 p_radius)\n{\n"
        " .reg .pred %p<8>;\n .reg .b32 %r<12>;\n .reg .b64 %rd<48>;\n"
        " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n ld.param.u64 %rd5,[p_count];\n"
        " ld.param.u64 %rd10,[p_h];\n ld.param.u64 %rd11,[p_w];\n ld.param.u64 %rd12,[p_radius];\n"
        " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
        " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
        " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
        " rem.u64 %rd13,%rd7,%rd11;\n div.u64 %rd14,%rd7,%rd11;\n rem.u64 %rd15,%rd14,%rd10;\n div.u64 %rd16,%rd14,%rd10;\n"
        " mov.u64 %rd17,0;\n mov.u64 %rd18,0;\n"
        " sub.s64 %rd19,%rd15,%rd12;\n mov.s64 %rd20,%rd19;\nYLOOP:\n"
        " add.s64 %rd21,%rd15,%rd12;\n setp.gt.s64 %p2,%rd20,%rd21;\n @%p2 bra STORE;\n"
        " setp.lt.s64 %p3,%rd20,0;\n @%p3 bra YNEXT;\n setp.ge.s64 %p4,%rd20,%rd10;\n @%p4 bra YNEXT;\n"
        " sub.s64 %rd22,%rd13,%rd12;\n mov.s64 %rd23,%rd22;\nXLOOP:\n"
        " add.s64 %rd24,%rd13,%rd12;\n setp.gt.s64 %p5,%rd23,%rd24;\n @%p5 bra YNEXT;\n"
        " setp.lt.s64 %p6,%rd23,0;\n @%p6 bra XNEXT;\n setp.ge.s64 %p7,%rd23,%rd11;\n @%p7 bra XNEXT;\n"
        " mul.lo.u64 %rd25,%rd16,%rd10;\n add.u64 %rd25,%rd25,%rd20;\n mul.lo.u64 %rd25,%rd25,%rd11;\n add.u64 %rd25,%rd25,%rd23;\n"
        " add.u64 %rd26,%rd2,%rd25;\n ld.global.u8 %r5,[%rd26];\n cvt.u64.u32 %rd27,%r5;\n"
        " add.u64 %rd17,%rd17,%rd27;\n add.u64 %rd18,%rd18,1;\n"
        "XNEXT:\n add.s64 %rd23,%rd23,1;\n bra XLOOP;\n"
        "YNEXT:\n add.s64 %rd20,%rd20,1;\n bra YLOOP;\n"
        "STORE:\n div.u64 %rd28,%rd17,%rd18;\n cvt.u32.u64 %r6,%rd28;\n"
        " add.u64 %rd29,%rd1,%rd7;\n st.global.u8 [%rd29],%r6;\n"
        "DONE:\n ret;\n}\n";
}

std::string ptx_image_morphology_source(int dtype,bool dilate) {
    const auto bytes=compute_dtype_width(dtype);
    const bool floating=dtype==9||dtype==10;
    const bool signed_int=dtype>=1&&dtype<=4;
    const char* value_type =
        dtype==9?".f64":dtype==10?".f32":
        bytes==8?(signed_int?".s64":".u64"):
        bytes==4?(signed_int?".s32":".u32"):
        bytes==2?(signed_int?".s16":".u16"):(signed_int?".s8":".u8");
    std::ostringstream out;
    out<<".version 6.0\n.target sm_30\n.address_size 64\n\n"
          ".visible .entry q_kernel(\n .param .u64 p_out,\n .param .u64 p_in,\n"
          " .param .u64 p_count,\n .param .u64 p_h,\n .param .u64 p_w,\n .param .u64 p_radius)\n{\n"
          " .reg .pred %p<10>;\n .reg .b32 %r<20>;\n .reg .b64 %rd<48>;\n"
       <<(floating?(dtype==10?" .reg .f32 %f<6>;\n":" .reg .f64 %fd<6>;\n"):"")
       <<" ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n ld.param.u64 %rd5,[p_count];\n"
         " ld.param.u64 %rd10,[p_h];\n ld.param.u64 %rd11,[p_w];\n ld.param.u64 %rd12,[p_radius];\n"
         " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
         " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
         " rem.u64 %rd13,%rd7,%rd11;\n div.u64 %rd14,%rd7,%rd11;\n rem.u64 %rd15,%rd14,%rd10;\n div.u64 %rd16,%rd14,%rd10;\n"
       << " mul.lo.u64 %rd17,%rd7,"<<bytes<<";\n add.u64 %rd18,%rd2,%rd17;\n";
    if(dtype==10)out<<" ld.global.f32 %f1,[%rd18];\n";
    else if(dtype==9)out<<" ld.global.f64 %fd1,[%rd18];\n";
    else if(bytes==1)out<<" ld.global.u8 %r5,[%rd18];\n";
    else if(bytes==2)out<<" ld.global.u16 %r5,[%rd18];\n";
    else if(bytes==4)out<<" ld.global.u32 %r5,[%rd18];\n";
    else out<<" ld.global.u64 %rd30,[%rd18];\n";
    out<<" sub.s64 %rd19,%rd15,%rd12;\n mov.s64 %rd20,%rd19;\nYLOOP:\n"
         " add.s64 %rd21,%rd15,%rd12;\n setp.gt.s64 %p2,%rd20,%rd21;\n @%p2 bra STORE;\n"
         " setp.lt.s64 %p3,%rd20,0;\n @%p3 bra YNEXT;\n setp.ge.s64 %p4,%rd20,%rd10;\n @%p4 bra YNEXT;\n"
         " sub.s64 %rd22,%rd13,%rd12;\n mov.s64 %rd23,%rd22;\nXLOOP:\n"
         " add.s64 %rd24,%rd13,%rd12;\n setp.gt.s64 %p5,%rd23,%rd24;\n @%p5 bra YNEXT;\n"
         " setp.lt.s64 %p6,%rd23,0;\n @%p6 bra XNEXT;\n setp.ge.s64 %p7,%rd23,%rd11;\n @%p7 bra XNEXT;\n"
         " mul.lo.u64 %rd25,%rd16,%rd10;\n add.u64 %rd25,%rd25,%rd20;\n mul.lo.u64 %rd25,%rd25,%rd11;\n add.u64 %rd25,%rd25,%rd23;\n"
       << " mul.lo.u64 %rd26,%rd25,"<<bytes<<";\n add.u64 %rd27,%rd2,%rd26;\n";
    if(dtype==10){
        out<<" ld.global.f32 %f2,[%rd27];\n setp."<<(dilate?"gt":"lt")<<".f32 %p8,%f2,%f1;\n @!%p8 bra XNEXT;\n mov.f32 %f1,%f2;\n";
    }else if(dtype==9){
        out<<" ld.global.f64 %fd2,[%rd27];\n setp."<<(dilate?"gt":"lt")<<".f64 %p8,%fd2,%fd1;\n @!%p8 bra XNEXT;\n mov.f64 %fd1,%fd2;\n";
    }else if(bytes==8){
        out<<" ld.global.u64 %rd31,[%rd27];\n setp."<<(dilate?"gt":"lt")<<value_type<<" %p8,%rd31,%rd30;\n @!%p8 bra XNEXT;\n mov.u64 %rd30,%rd31;\n";
    }else{
        if(bytes==1)out<<" ld.global.u8 %r6,[%rd27];\n";
        else if(bytes==2)out<<" ld.global.u16 %r6,[%rd27];\n";
        else out<<" ld.global.u32 %r6,[%rd27];\n";
        if(signed_int && bytes<4){
            out<<" cvt.s32.s"<<(bytes*8)<<" %r7,%r6;\n cvt.s32.s"<<(bytes*8)<<" %r8,%r5;\n"
               <<" setp."<<(dilate?"gt":"lt")<<".s32 %p8,%r7,%r8;\n";
        }else{
            out<<" setp."<<(dilate?"gt":"lt")<<(signed_int?".s32":".u32")<<" %p8,%r6,%r5;\n";
        }
        out<<" @!%p8 bra XNEXT;\n mov.u32 %r5,%r6;\n";
    }
    out<<"XNEXT:\n add.s64 %rd23,%rd23,1;\n bra XLOOP;\n"
         "YNEXT:\n add.s64 %rd20,%rd20,1;\n bra YLOOP;\nSTORE:\n"
       << " mul.lo.u64 %rd28,%rd7,"<<bytes<<";\n add.u64 %rd29,%rd1,%rd28;\n";
    if(dtype==10)out<<" st.global.f32 [%rd29],%f1;\n";
    else if(dtype==9)out<<" st.global.f64 [%rd29],%fd1;\n";
    else if(bytes==1)out<<" st.global.u8 [%rd29],%r5;\n";
    else if(bytes==2)out<<" st.global.u16 [%rd29],%r5;\n";
    else if(bytes==4)out<<" st.global.u32 [%rd29],%r5;\n";
    else out<<" st.global.u64 [%rd29],%rd30;\n";
    out<<"DONE:\n ret;\n}\n";
    return out.str();
}


std::string ptx_image_filter_source() {
    return
        ".version 6.0\n.target sm_30\n.address_size 64\n\n"
        ".visible .entry q_kernel(\n"
        " .param .u64 p_out,\n .param .u64 p_in,\n .param .u64 p_kernel,\n"
        " .param .u64 p_status,\n .param .u64 p_count,\n .param .u64 p_h,\n"
        " .param .u64 p_w,\n .param .u64 p_kh,\n .param .u64 p_kw,\n"
        " .param .s64 p_divisor,\n .param .s64 p_offset)\n{\n"
        " .reg .pred %p<16>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<64>;\n"
        " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_in];\n"
        " ld.param.u64 %rd3,[p_kernel];\n ld.param.u64 %rd4,[p_status];\n"
        " ld.param.u64 %rd5,[p_count];\n ld.param.u64 %rd10,[p_h];\n"
        " ld.param.u64 %rd11,[p_w];\n ld.param.u64 %rd12,[p_kh];\n"
        " ld.param.u64 %rd13,[p_kw];\n ld.param.s64 %rd14,[p_divisor];\n"
        " ld.param.s64 %rd15,[p_offset];\n"
        " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
        " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
        " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
        " rem.u64 %rd16,%rd7,%rd11;\n div.u64 %rd17,%rd7,%rd11;\n"
        " rem.u64 %rd18,%rd17,%rd10;\n div.u64 %rd19,%rd17,%rd10;\n"
        " div.u64 %rd20,%rd12,2;\n div.u64 %rd21,%rd13,2;\n"
        " mov.s64 %rd22,0;\n mov.u64 %rd23,0;\n"
        "KY_LOOP:\n setp.ge.u64 %p2,%rd23,%rd12;\n @%p2 bra REDUCE;\n"
        " cvt.s64.u64 %rd24,%rd18;\n cvt.s64.u64 %rd25,%rd23;\n"
        " add.s64 %rd26,%rd24,%rd25;\n cvt.s64.u64 %rd27,%rd20;\n sub.s64 %rd26,%rd26,%rd27;\n"
        " setp.lt.s64 %p3,%rd26,0;\n @%p3 bra KY_NEXT;\n"
        " cvt.s64.u64 %rd28,%rd10;\n setp.ge.s64 %p4,%rd26,%rd28;\n @%p4 bra KY_NEXT;\n"
        " mov.u64 %rd29,0;\n"
        "KX_LOOP:\n setp.ge.u64 %p5,%rd29,%rd13;\n @%p5 bra KY_NEXT;\n"
        " cvt.s64.u64 %rd30,%rd16;\n cvt.s64.u64 %rd31,%rd29;\n"
        " add.s64 %rd32,%rd30,%rd31;\n cvt.s64.u64 %rd33,%rd21;\n sub.s64 %rd32,%rd32,%rd33;\n"
        " setp.lt.s64 %p6,%rd32,0;\n @%p6 bra KX_NEXT;\n"
        " cvt.s64.u64 %rd34,%rd11;\n setp.ge.s64 %p7,%rd32,%rd34;\n @%p7 bra KX_NEXT;\n"
        " mul.lo.u64 %rd35,%rd19,%rd10;\n cvt.u64.s64 %rd36,%rd26;\n"
        " add.u64 %rd35,%rd35,%rd36;\n mul.lo.u64 %rd35,%rd35,%rd11;\n"
        " cvt.u64.s64 %rd37,%rd32;\n add.u64 %rd35,%rd35,%rd37;\n"
        " add.u64 %rd38,%rd2,%rd35;\n ld.global.u8 %r5,[%rd38];\n"
        " mul.lo.u64 %rd39,%rd23,%rd13;\n add.u64 %rd39,%rd39,%rd29;\n"
        " mul.lo.u64 %rd40,%rd39,8;\n add.u64 %rd41,%rd3,%rd40;\n"
        " ld.global.s64 %rd42,[%rd41];\n cvt.s64.u32 %rd43,%r5;\n"
        " mul.lo.s64 %rd44,%rd43,%rd42;\n mul.hi.s64 %rd45,%rd43,%rd42;\n"
        " setp.lt.s64 %p8,%rd44,0;\n selp.s64 %rd46,-1,0,%p8;\n"
        " setp.ne.s64 %p9,%rd45,%rd46;\n @%p9 bra OVERFLOW;\n"
        " setp.gt.s64 %p10,%rd44,0;\n @!%p10 bra ADD_NEG;\n"
        " mov.s64 %rd47,0x7fffffffffffffff;\n sub.s64 %rd48,%rd47,%rd44;\n"
        " setp.gt.s64 %p11,%rd22,%rd48;\n @%p11 bra OVERFLOW;\n bra ADD_OK;\n"
        "ADD_NEG:\n setp.lt.s64 %p12,%rd44,0;\n @!%p12 bra ADD_OK;\n"
        " mov.s64 %rd49,0x8000000000000000;\n sub.s64 %rd50,%rd49,%rd44;\n"
        " setp.lt.s64 %p13,%rd22,%rd50;\n @%p13 bra OVERFLOW;\n"
        "ADD_OK:\n add.s64 %rd22,%rd22,%rd44;\n"
        "KX_NEXT:\n add.u64 %rd29,%rd29,1;\n bra KX_LOOP;\n"
        "KY_NEXT:\n add.u64 %rd23,%rd23,1;\n bra KY_LOOP;\n"
        "REDUCE:\n mov.s64 %rd51,0x8000000000000000;\n"
        " setp.eq.s64 %p14,%rd22,%rd51;\n setp.eq.s64 %p15,%rd14,-1;\n"
        " and.pred %p14,%p14,%p15;\n @%p14 bra OVERFLOW;\n"
        " div.s64 %rd52,%rd22,%rd14;\n"
        " setp.gt.s64 %p10,%rd15,0;\n @!%p10 bra OFFSET_NEG;\n"
        " mov.s64 %rd47,0x7fffffffffffffff;\n sub.s64 %rd48,%rd47,%rd15;\n"
        " setp.gt.s64 %p11,%rd52,%rd48;\n @%p11 bra OVERFLOW;\n bra OFFSET_OK;\n"
        "OFFSET_NEG:\n setp.lt.s64 %p12,%rd15,0;\n @!%p12 bra OFFSET_OK;\n"
        " mov.s64 %rd49,0x8000000000000000;\n sub.s64 %rd50,%rd49,%rd15;\n"
        " setp.lt.s64 %p13,%rd52,%rd50;\n @%p13 bra OVERFLOW;\n"
        "OFFSET_OK:\n add.s64 %rd52,%rd52,%rd15;\n"
        " max.s64 %rd52,%rd52,0;\n min.s64 %rd52,%rd52,255;\n"
        " cvt.u32.u64 %r6,%rd52;\n add.u64 %rd53,%rd1,%rd7;\n"
        " st.global.u8 [%rd53],%r6;\n bra DONE;\n"
        "OVERFLOW:\n mov.u32 %r7,1;\n atom.global.exch.b32 %r8,[%rd4],%r7;\n"
        "DONE:\n ret;\n}\n";
}


std::string hip_image_geometry_source(std::size_t bytes,int operation) {
    const auto type=hip_raw_type(bytes);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* in,unsigned long long count,unsigned long long ih,unsigned long long iw,"
         "unsigned long long oh,unsigned long long ow,unsigned long long p0,unsigned long long p1){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;unsigned long long x=i%ow,t=i/ow,y=t%oh,c=t/oh,sy=0,sx=0;";
    if(operation==1) out<<"sy=p0+y;sx=p1+x;";
    else if(operation==2) out<<"sy=y*ih/oh;sx=x*iw/ow;";
    else if(operation==3) out<<"sy=y;sx=iw-1-x;";
    else if(operation==4) out<<"sy=ih-1-y;sx=x;";
    else if(operation==5) out<<"sy=ih-1-x;sx=y;";
    else if(operation==7) out<<"sy=ih-1-y;sx=iw-1-x;";
    else out<<"sy=x;sx=iw-1-y;";
    out<<"out[i]=in[(c*ih+sy)*iw+sx];}";
    return out.str();
}

std::string hip_image_grayscale_source() {
    return
      "extern \"C\" __global__ void q_kernel(unsigned char* out,const unsigned char* in,"
      "unsigned long long pixels,unsigned long long channels){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i>=pixels)return;if(channels==1){out[i]=in[i];return;}"
      "double v=0.299*(double)in[i]+0.587*(double)in[pixels+i]+0.114*(double)in[2*pixels+i];"
      "long long q=(long long)__builtin_round(v);if(q<0)q=0;if(q>255)q=255;out[i]=(unsigned char)q;}";
}

std::string hip_image_threshold_source() {
    return
      "extern \"C\" __global__ void q_kernel(unsigned char* out,const unsigned char* in,"
      "unsigned long long count,unsigned char cutoff,unsigned char low,unsigned char high){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i<count)out[i]=in[i]>=cutoff?high:low;}";
}

std::string hip_image_blur_source() {
    return
      "extern \"C\" __global__ void q_kernel(unsigned char* out,const unsigned char* in,"
      "unsigned long long count,unsigned long long h,unsigned long long w,unsigned long long radius){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i>=count)return;unsigned long long x=i%w,t=i/w,y=t%h,c=t/h,total=0,n=0;"
      "long long r=(long long)radius;"
      "for(long long dy=-r;dy<=r;++dy){long long sy=(long long)y+dy;if(sy<0||sy>=(long long)h)continue;"
      "for(long long dx=-r;dx<=r;++dx){long long sx=(long long)x+dx;if(sx<0||sx>=(long long)w)continue;"
      "total+=(unsigned long long)in[(c*h+(unsigned long long)sy)*w+(unsigned long long)sx];++n;}}"
      "out[i]=(unsigned char)(total/n);}";
}

std::string hip_image_filter_source() {
    return
      "extern \"C\" __global__ void q_kernel(unsigned char* out,const unsigned char* in,"
      "const long long* kernel,unsigned int* status,unsigned long long count,"
      "unsigned long long h,unsigned long long w,unsigned long long kh,unsigned long long kw,"
      "long long divisor,long long offset){"
      "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
      "if(i>=count)return;unsigned long long x=i%w,t=i/w,y=t%h,c=t/h;"
      "long long total=0,cy=(long long)(kh/2),cx=(long long)(kw/2);"
      "for(unsigned long long ky=0;ky<kh;++ky){long long sy=(long long)y+(long long)ky-cy;"
      "if(sy<0||sy>=(long long)h)continue;"
      "for(unsigned long long kx=0;kx<kw;++kx){long long sx=(long long)x+(long long)kx-cx;"
      "if(sx<0||sx>=(long long)w)continue;long long p=(long long)in[(c*h+(unsigned long long)sy)*w+(unsigned long long)sx];"
      "long long product=0,next=0;"
      "if(__builtin_mul_overflow(p,kernel[ky*kw+kx],&product)||__builtin_add_overflow(total,product,&next)){status[0]=1u;return;}"
      "total=next;}}"
      "if(total==(-9223372036854775807ll-1ll)&&divisor==-1ll){status[0]=1u;return;}"
      "long long value=total/divisor,next=0;if(__builtin_add_overflow(value,offset,&next)){status[0]=1u;return;}"
      "if(next<0)next=0;if(next>255)next=255;out[i]=(unsigned char)next;}";
}

std::string hip_image_morph_source(int dtype,bool dilate) {
    const auto type=hip_dtype_type(dtype);
    std::ostringstream out;
    out<<"extern \"C\" __global__ void q_kernel("<<type<<"* out,const "<<type
       <<"* in,unsigned long long count,unsigned long long h,unsigned long long w,unsigned long long radius){"
         "unsigned long long i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;"
         "if(i>=count)return;unsigned long long x=i%w,t=i/w,y=t%h,c=t/h;"<<type<<" best=in[i];"
         "long long r=(long long)radius;for(long long dy=-r;dy<=r;++dy){long long sy=(long long)y+dy;"
         "if(sy<0||sy>=(long long)h)continue;for(long long dx=-r;dx<=r;++dx){long long sx=(long long)x+dx;"
         "if(sx<0||sx>=(long long)w)continue;"<<type<<" v=in[(c*h+(unsigned long long)sy)*w+(unsigned long long)sx];"
         "if(v"<<(dilate?">":"<")<<"best)best=v;}}out[i]=best;}";
    return out.str();
}

#ifdef __APPLE__
std::string msl_image_geometry_source(std::size_t bytes,int operation) {
    const auto type=msl_raw_type(bytes);
    std::ostringstream out;
    out<<"#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void q_kernel(device "<<type<<"* out [[buffer(0)]],const device "<<type<<"* in [[buffer(1)]],"
          "constant ulong& count [[buffer(2)]],constant ulong& ih [[buffer(3)]],constant ulong& iw [[buffer(4)]],"
          "constant ulong& oh [[buffer(5)]],constant ulong& ow [[buffer(6)]],constant ulong& p0 [[buffer(7)]],"
          "constant ulong& p1 [[buffer(8)]],uint gid [[thread_position_in_grid]]){"
          " ulong i=ulong(gid);if(i>=count)return;ulong x=i%ow;ulong t=i/ow;ulong y=t%oh;ulong c=t/oh;"
          " ulong sy=0,sx=0;";
    if(operation==1)out<<"sy=p0+y;sx=p1+x;";
    else if(operation==2)out<<"sy=y*ih/oh;sx=x*iw/ow;";
    else if(operation==3)out<<"sy=y;sx=iw-1-x;";
    else if(operation==4)out<<"sy=ih-1-y;sx=x;";
    else if(operation==5)out<<"sy=ih-1-x;sx=y;";
    else if(operation==7)out<<"sy=ih-1-y;sx=iw-1-x;";
    else out<<"sy=x;sx=iw-1-y;";
    out<<"out[i]=in[(c*ih+sy)*iw+sx];}\n";
    return out.str();
}
std::string msl_image_grayscale_source(){
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device uchar* out [[buffer(0)]],const device uchar* in [[buffer(1)]],"
      "constant ulong& pixels [[buffer(2)]],constant ulong& channels [[buffer(3)]],uint gid [[thread_position_in_grid]]){"
      "ulong i=ulong(gid);if(i>=pixels)return;if(channels==1){out[i]=in[i];return;}"
      "float v=0.299f*float(in[i])+0.587f*float(in[pixels+i])+0.114f*float(in[2*pixels+i]);"
      "out[i]=uchar(clamp(round(v),0.0f,255.0f));}\n";
}
std::string msl_image_threshold_source(){
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device uchar* out [[buffer(0)]],const device uchar* in [[buffer(1)]],"
      "constant ulong& count [[buffer(2)]],constant uchar& cutoff [[buffer(3)]],constant uchar& low [[buffer(4)]],"
      "constant uchar& high [[buffer(5)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i<count)out[i]=in[i]>=cutoff?high:low;}\n";
}

std::string msl_image_filter_source(){
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device uchar* out [[buffer(0)]],const device uchar* in [[buffer(1)]],"
      "const device long* coeffs [[buffer(2)]],device atomic_uint* status [[buffer(3)]],"
      "constant ulong& count [[buffer(4)]],constant ulong& h [[buffer(5)]],constant ulong& w [[buffer(6)]],"
      "constant ulong& kh [[buffer(7)]],constant ulong& kw [[buffer(8)]],"
      "constant long& divisor [[buffer(9)]],constant long& offset [[buffer(10)]],"
      "uint gid [[thread_position_in_grid]]){"
      "ulong i=ulong(gid);if(i>=count)return;ulong x=i%w;ulong t=i/w;ulong y=t%h;ulong c=t/h;"
      "long total=0;long cy=long(kh/2),cx=long(kw/2);"
      "for(ulong ky=0;ky<kh;++ky){long sy=long(y)+long(ky)-cy;if(sy<0||sy>=long(h))continue;"
      "for(ulong kx=0;kx<kw;++kx){long sx=long(x)+long(kx)-cx;if(sx<0||sx>=long(w))continue;"
      "long p=long(in[(c*h+ulong(sy))*w+ulong(sx)]);long k=coeffs[ky*kw+kx];"
      "if(p!=0){long hi=0x7fffffffffffffffL/p;long lo=(-0x7fffffffffffffffL-1L)/p;"
      "if(k>hi||k<lo){atomic_store_explicit(status,1u,memory_order_relaxed);return;}}"
      "long product=p*k;if((product>0&&total>0x7fffffffffffffffL-product)||"
      "(product<0&&total<(-0x7fffffffffffffffL-1L)-product)){atomic_store_explicit(status,1u,memory_order_relaxed);return;}"
      "total+=product;}}"
      "if(total==(-0x7fffffffffffffffL-1L)&&divisor==-1){atomic_store_explicit(status,1u,memory_order_relaxed);return;}"
      "long value=total/divisor;"
      "if((offset>0&&value>0x7fffffffffffffffL-offset)||(offset<0&&value<(-0x7fffffffffffffffL-1L)-offset)){"
      "atomic_store_explicit(status,1u,memory_order_relaxed);return;}"
      "value+=offset;value=clamp(value,0L,255L);out[i]=uchar(value);}\n";
}
std::string msl_image_blur_source(){
    return "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void q_kernel(device uchar* out [[buffer(0)]],const device uchar* in [[buffer(1)]],"
      "constant ulong& count [[buffer(2)]],constant ulong& h [[buffer(3)]],constant ulong& w [[buffer(4)]],"
      "constant ulong& radius [[buffer(5)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"
      "ulong x=i%w;ulong t=i/w;ulong y=t%h;ulong c=t/h;ulong total=0,n=0;"
      "long r=long(radius);for(long dy=-r;dy<=r;++dy){long sy=long(y)+dy;if(sy<0||sy>=long(h))continue;"
      "for(long dx=-r;dx<=r;++dx){long sx=long(x)+dx;if(sx<0||sx>=long(w))continue;"
      "total+=ulong(in[(c*h+ulong(sy))*w+ulong(sx)]);++n;}}out[i]=uchar(total/n);}\n";
}

template <typename T>
std::string msl_morph_source_t(const char* type_name,bool dilate){
    (void)sizeof(T);
    std::ostringstream out;
    out<<"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void q_kernel(device "<<type_name<<"* out [[buffer(0)]],const device "<<type_name<<"* in [[buffer(1)]],"
         "constant ulong& count [[buffer(2)]],constant ulong& h [[buffer(3)]],constant ulong& w [[buffer(4)]],"
         "constant ulong& radius [[buffer(5)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"
         "ulong x=i%w;ulong t=i/w;ulong y=t%h;ulong c=t/h;"<<type_name<<" best=in[i];long r=long(radius);"
         "for(long dy=-r;dy<=r;++dy){long sy=long(y)+dy;if(sy<0||sy>=long(h))continue;"
         "for(long dx=-r;dx<=r;++dx){long sx=long(x)+dx;if(sx<0||sx>=long(w))continue;"
         <<type_name<<" v=in[(c*h+ulong(sy))*w+ulong(sx)];if(v"<<(dilate?">":"<")<<"best)best=v;}}out[i]=best;}\n";
    return out.str();
}
#endif

} // namespace

bool compute_image_geometry(
    Buffer* output,const Buffer* input,int dtype,
    std::size_t channels,std::size_t input_height,std::size_t input_width,
    std::size_t output_height,std::size_t output_width,
    int operation,std::size_t parameter0,std::size_t parameter1,
    std::string& error){
    const auto bytes=vision_dtype_width(dtype);
    const auto input_count=channels*input_height*input_width;
    const auto output_count=channels*output_height*output_width;
    if(operation<1||operation>7||!compute_same_device(output,input,error)||
       !compute_buffer_range(input,0,input_count,bytes,error)||
       !compute_buffer_range(output,0,output_count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test)
        return fake_image_geometry(output,input,bytes,channels,input_height,input_width,
                                   output_height,output_width,operation,parameter0,parameter1,error);
#endif
    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_geometry_source(bytes,operation);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(output_count),ih=static_cast<std::uint64_t>(input_height),
             iw=static_cast<std::uint64_t>(input_width),oh=static_cast<std::uint64_t>(output_height),
             ow=static_cast<std::uint64_t>(output_width),p0=static_cast<std::uint64_t>(parameter0),
             p1=static_cast<std::uint64_t>(parameter1);
        void* args[]={&out_ptr,&in_ptr,&n,&ih,&iw,&oh,&ow,&p0,&p1};
        return launch_ptx_1d(output,"image-geometry-"+std::to_string(bytes)+"-"+std::to_string(operation),
                             ptx,output_count,args,error);
    }
    if(output->backend==Backend::Amd){
        auto source=hip_image_geometry_source(bytes,operation);
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(output_count),ih=static_cast<std::uint64_t>(input_height),
             iw=static_cast<std::uint64_t>(input_width),oh=static_cast<std::uint64_t>(output_height),
             ow=static_cast<std::uint64_t>(output_width),p0=static_cast<std::uint64_t>(parameter0),
             p1=static_cast<std::uint64_t>(parameter1);
        void* args[]={&out_ptr,&in_ptr,&n,&ih,&iw,&oh,&ow,&p0,&p1};
        return launch_hip_1d(output,
            "image-geometry-"+std::to_string(bytes)+"-"+std::to_string(operation),
            source,output_count,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        auto source=msl_image_geometry_source(bytes,operation);
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],
            "image-geometry-"+std::to_string(bytes)+"-"+std::to_string(operation),source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(output_count),ih=static_cast<std::uint64_t>(input_height),
             iw=static_cast<std::uint64_t>(input_width),oh=static_cast<std::uint64_t>(output_height),
             ow=static_cast<std::uint64_t>(output_width),p0=static_cast<std::uint64_t>(parameter0),
             p1=static_cast<std::uint64_t>(parameter1);
        return dispatch_metal_1d(output,pipeline,output_count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];[e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBytes:&ih length:8 atIndex:3];[e setBytes:&iw length:8 atIndex:4];
            [e setBytes:&oh length:8 atIndex:5];[e setBytes:&ow length:8 atIndex:6];[e setBytes:&p0 length:8 atIndex:7];
            [e setBytes:&p1 length:8 atIndex:8];
        },error);
    }
#endif
    return real_compute_unavailable(output,"image tensor geometry",dtype,error);
}

bool compute_image_grayscale(
    Buffer* output,const Buffer* input,std::size_t channels,
    std::size_t height,std::size_t width,std::string& error){
    const auto pixels=height*width,input_count=channels*pixels;
    if((channels!=1&&channels!=3&&channels!=4)||
       !compute_same_device(output,input,error)||
       !compute_buffer_range(input,0,input_count,1,error)||
       !compute_buffer_range(output,0,pixels,1,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test)
        return fake_image_grayscale(output,input,channels,height,width);
#endif
    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_grayscale_source();
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(pixels),c=static_cast<std::uint64_t>(channels);
        void* args[]={&out_ptr,&in_ptr,&n,&c};
        return launch_ptx_1d(output,"image-grayscale-u8",ptx,pixels,args,error);
    }
    if(output->backend==Backend::Amd){
        auto source=hip_image_grayscale_source();
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(pixels),c=static_cast<std::uint64_t>(channels);
        void* args[]={&out_ptr,&in_ptr,&n,&c};
        return launch_hip_1d(output,"image-grayscale-u8",source,pixels,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        auto source=msl_image_grayscale_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"image-grayscale-u8",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(pixels),c=static_cast<std::uint64_t>(channels);
        return dispatch_metal_1d(output,pipeline,pixels,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];[e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBytes:&c length:8 atIndex:3];
        },error);
    }
#endif
    return real_compute_unavailable(output,"image grayscale",5,error);
}

bool compute_image_threshold(
    Buffer* output,const Buffer* input,std::size_t count,
    std::uint8_t cutoff,std::uint8_t low,std::uint8_t high,std::string& error){
    if(!compute_same_device(output,input,error)||
       !compute_buffer_range(input,0,count,1,error)||
       !compute_buffer_range(output,0,count,1,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test)
        return fake_image_threshold(output,input,count,cutoff,low,high);
#endif
    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_threshold_source();
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count);
        std::uint32_t c=cutoff,l=low,h=high;
        void* args[]={&out_ptr,&in_ptr,&n,&c,&l,&h};
        return launch_ptx_1d(output,"image-threshold-u8",ptx,count,args,error);
    }
    if(output->backend==Backend::Amd){
        auto source=hip_image_threshold_source();
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count);
        auto c=cutoff,l=low,h=high;
        void* args[]={&out_ptr,&in_ptr,&n,&c,&l,&h};
        return launch_hip_1d(output,"image-threshold-u8",source,count,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        auto source=msl_image_threshold_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"image-threshold-u8",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];[e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBytes:&cutoff length:1 atIndex:3];
            [e setBytes:&low length:1 atIndex:4];[e setBytes:&high length:1 atIndex:5];
        },error);
    }
#endif
    return real_compute_unavailable(output,"image threshold",5,error);
}

bool compute_image_blur(
    Buffer* output,const Buffer* input,std::size_t channels,
    std::size_t height,std::size_t width,std::size_t radius,std::string& error){
    const auto count=channels*height*width;
    if(!compute_same_device(output,input,error)||
       !compute_buffer_range(input,0,count,1,error)||
       !compute_buffer_range(output,0,count,1,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test)return fake_image_blur(output,input,channels,height,width,radius);
#endif
    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_blur_source();
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        void* args[]={&out_ptr,&in_ptr,&n,&h,&w,&r};
        return launch_ptx_1d(output,"image-blur-u8",ptx,count,args,error);
    }
    if(output->backend==Backend::Amd){
        auto source=hip_image_blur_source();
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        void* args[]={&out_ptr,&in_ptr,&n,&h,&w,&r};
        return launch_hip_1d(output,"image-blur-u8",source,count,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        auto source=msl_image_blur_source();
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],"image-blur-u8",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];[e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBytes:&h length:8 atIndex:3];[e setBytes:&w length:8 atIndex:4];
            [e setBytes:&r length:8 atIndex:5];
        },error);
    }
#endif
    return real_compute_unavailable(output,"image blur",5,error);
}

bool compute_image_filter(
    Buffer* output,const Buffer* input,const Buffer* kernel,
    std::size_t channels,std::size_t height,std::size_t width,
    std::size_t kh,std::size_t kw,std::int64_t divisor,std::int64_t offset,
    std::string& error){
    const auto count=channels*height*width,kcount=kh*kw;
    if(divisor==0){error="image filter divisor must not be zero";return false;}
    if(!compute_same_device(output,input,error)||!compute_same_device(output,kernel,error)||
       !compute_buffer_range(input,0,count,1,error)||
       !compute_buffer_range(output,0,count,1,error)||
       !compute_buffer_range(kernel,0,kcount,8,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test)
        return fake_image_filter(output,input,kernel,channels,height,width,kh,kw,divisor,offset,error);
#endif
    std::unique_ptr<Buffer,void(*)(Buffer*)> status(
        acquire_validation_status(output->global_index,error),
        [](Buffer* value){quidra::device::release(value);});
    if(!status)return false;

    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_filter_source();
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer,
             k_ptr=kernel->cuda_pointer,status_ptr=status->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),
             hh=static_cast<std::uint64_t>(height),ww=static_cast<std::uint64_t>(width),
             kh64=static_cast<std::uint64_t>(kh),kw64=static_cast<std::uint64_t>(kw);
        auto div=divisor,off=offset;
        void* args[]={&out_ptr,&in_ptr,&k_ptr,&status_ptr,&n,&hh,&ww,&kh64,&kw64,&div,&off};
        if(!launch_ptx_1d(output,"image-filter-u8-i64",ptx,count,args,error))return false;
    }
    else if(output->backend==Backend::Amd){
        auto source=hip_image_filter_source();
        auto out_ptr=output->pointer,in_ptr=input->pointer,
             k_ptr=kernel->pointer,status_ptr=status->pointer;
        auto n=static_cast<std::uint64_t>(count),
             hh=static_cast<std::uint64_t>(height),ww=static_cast<std::uint64_t>(width),
             kh64=static_cast<std::uint64_t>(kh),kw64=static_cast<std::uint64_t>(kw);
        auto div=divisor,off=offset;
        void* args[]={&out_ptr,&in_ptr,&k_ptr,&status_ptr,&n,&hh,&ww,&kh64,&kw64,&div,&off};
        if(!launch_hip_1d(output,"image-filter-u8-i64",source,count,args,error))return false;
    }
#ifdef __APPLE__
    else if(output->backend==Backend::Metal){
        auto source=msl_image_filter_source();
        auto pipeline=cached_metal_pipeline(
            [output->metal_buffer device],"image-filter-u8-i64",source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),
             hh=static_cast<std::uint64_t>(height),ww=static_cast<std::uint64_t>(width),
             kh64=static_cast<std::uint64_t>(kh),kw64=static_cast<std::uint64_t>(kw);
        auto div=divisor,off=offset;
        if(!dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];
            [e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBuffer:kernel->metal_buffer offset:0 atIndex:2];
            [e setBuffer:status->metal_buffer offset:0 atIndex:3];
            [e setBytes:&n length:8 atIndex:4];[e setBytes:&hh length:8 atIndex:5];
            [e setBytes:&ww length:8 atIndex:6];[e setBytes:&kh64 length:8 atIndex:7];
            [e setBytes:&kw64 length:8 atIndex:8];[e setBytes:&div length:8 atIndex:9];
            [e setBytes:&off length:8 atIndex:10];
        },error))return false;
    }
#endif
    else{
        return real_compute_unavailable(output,"image filter",5,error);
    }

    defer_validation(
        status.release(),"vision.filter integer arithmetic overflow");
    return true;
}

bool compute_image_morphology(
    Buffer* output,const Buffer* input,int dtype,
    std::size_t channels,std::size_t height,std::size_t width,
    std::size_t radius,bool dilate,std::string& error){
    const auto bytes=vision_dtype_width(dtype),count=channels*height*width;
    if(!compute_same_device(output,input,error)||
       !compute_buffer_range(input,0,count,bytes,error)||
       !compute_buffer_range(output,0,count,bytes,error))return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(output->backend==Backend::Test){
        switch(dtype){
            case 1:return fake_image_morphology<std::int64_t>(output,input,channels,height,width,radius,dilate);
            case 2:return fake_image_morphology<std::int8_t>(output,input,channels,height,width,radius,dilate);
            case 3:return fake_image_morphology<std::int16_t>(output,input,channels,height,width,radius,dilate);
            case 4:return fake_image_morphology<std::int32_t>(output,input,channels,height,width,radius,dilate);
            case 5:return fake_image_morphology<std::uint8_t>(output,input,channels,height,width,radius,dilate);
            case 6:return fake_image_morphology<std::uint16_t>(output,input,channels,height,width,radius,dilate);
            case 7:return fake_image_morphology<std::uint32_t>(output,input,channels,height,width,radius,dilate);
            case 8:return fake_image_morphology<std::uint64_t>(output,input,channels,height,width,radius,dilate);
            case 9:return fake_image_morphology<double>(output,input,channels,height,width,radius,dilate);
            case 10:return fake_image_morphology<float>(output,input,channels,height,width,radius,dilate);
            default:error="invalid image morphology dtype";return false;
        }
    }
#endif
    if(output->backend==Backend::Nvidia){
        auto ptx=ptx_image_morphology_source(dtype,dilate);
        auto out_ptr=output->cuda_pointer,in_ptr=input->cuda_pointer;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        void* args[]={&out_ptr,&in_ptr,&n,&h,&w,&r};
        return launch_ptx_1d(output,
            "image-morph-"+std::to_string(dtype)+"-"+(dilate?"d":"e"),
            ptx,count,args,error);
    }
    if(output->backend==Backend::Amd){
        auto source=hip_image_morph_source(dtype,dilate);
        auto out_ptr=output->pointer,in_ptr=input->pointer;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        void* args[]={&out_ptr,&in_ptr,&n,&h,&w,&r};
        return launch_hip_1d(output,
            "image-morph-"+std::to_string(dtype)+"-"+(dilate?"d":"e"),
            source,count,args,error);
    }
#ifdef __APPLE__
    if(output->backend==Backend::Metal){
        std::string source;
        switch(dtype){
            case 2:source=msl_morph_source_t<std::int8_t>("char",dilate);break;
            case 3:source=msl_morph_source_t<std::int16_t>("short",dilate);break;
            case 4:source=msl_morph_source_t<std::int32_t>("int",dilate);break;
            case 5:source=msl_morph_source_t<std::uint8_t>("uchar",dilate);break;
            case 6:source=msl_morph_source_t<std::uint16_t>("ushort",dilate);break;
            case 7:source=msl_morph_source_t<std::uint32_t>("uint",dilate);break;
            case 10:source=msl_morph_source_t<float>("float",dilate);break;
            default:return real_compute_unavailable(output,"image morphology",dtype,error);
        }
        auto key="image-morph-"+std::to_string(dtype)+"-"+(dilate?"d":"e");
        auto pipeline=cached_metal_pipeline([output->metal_buffer device],key,source,error);
        if(!pipeline)return false;
        auto n=static_cast<std::uint64_t>(count),h=static_cast<std::uint64_t>(height),
             w=static_cast<std::uint64_t>(width),r=static_cast<std::uint64_t>(radius);
        return dispatch_metal_1d(output,pipeline,count,[&](id<MTLComputeCommandEncoder> e){
            [e setBuffer:output->metal_buffer offset:0 atIndex:0];[e setBuffer:input->metal_buffer offset:0 atIndex:1];
            [e setBytes:&n length:8 atIndex:2];[e setBytes:&h length:8 atIndex:3];[e setBytes:&w length:8 atIndex:4];
            [e setBytes:&r length:8 atIndex:5];
        },error);
    }
#endif
    return real_compute_unavailable(output,"image morphology",dtype,error);
}
