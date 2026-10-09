
namespace {

int gpu_integer_bits(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: case QCORE_DTYPE_UINT64: return 64;
        case QCORE_DTYPE_INT8: case QCORE_DTYPE_UINT8: return 8;
        case QCORE_DTYPE_INT16: case QCORE_DTYPE_UINT16: return 16;
        case QCORE_DTYPE_INT32: case QCORE_DTYPE_UINT32: return 32;
        default: return 0;
    }
}
bool gpu_integer_signed(int dtype) { return dtype>=QCORE_DTYPE_INT64&&dtype<=QCORE_DTYPE_INT32; }
bool gpu_integer_dtype(int dtype) { return dtype>=QCORE_DTYPE_INT64&&dtype<=QCORE_DTYPE_UINT64; }

std::string ptx_integer_kind(int dtype) {
    const auto bits=gpu_integer_bits(dtype);
    if(!bits)return {};
    return std::string(gpu_integer_signed(dtype)?"s":"u")+std::to_string(bits);
}
std::string integer_min_text(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: return "-9223372036854775808";
        case QCORE_DTYPE_INT8: return "-128";
        case QCORE_DTYPE_INT16: return "-32768";
        case QCORE_DTYPE_INT32: return "-2147483648";
        default:return "0";
    }
}
std::string integer_max_text(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: return "9223372036854775807";
        case QCORE_DTYPE_INT8: return "127";
        case QCORE_DTYPE_INT16: return "32767";
        case QCORE_DTYPE_INT32: return "2147483647";
        case QCORE_DTYPE_UINT8: return "255";
        case QCORE_DTYPE_UINT16: return "65535";
        case QCORE_DTYPE_UINT32: return "4294967295";
        case QCORE_DTYPE_UINT64: return "18446744073709551615";
        default:return "0";
    }
}

std::string ptx_load_integer_global(
    int dtype,const char* address,const char* dest,const char* temp) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    if(bits==64){
        o<<" ld.global."<<(sign?"s64":"u64")<<" "<<dest<<",["<<address<<"];\n";
    }else{
        o<<" ld.global."<<(sign?"s":"u")<<bits<<" "<<temp<<",["<<address<<"];\n"
         <<" cvt."<<(sign?"s64.s32":"u64.u32")<<" "<<dest<<","<<temp<<";\n";
    }
    return o.str();
}
std::string ptx_load_integer_param(
    int dtype,const char* parameter,const char* dest,const char* temp) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    if(bits==64){
        o<<" ld.param."<<(sign?"s64":"u64")<<" "<<dest<<",["<<parameter<<"];\n";
    }else{
        o<<" ld.param."<<(sign?"s":"u")<<bits<<" "<<temp<<",["<<parameter<<"];\n"
         <<" cvt."<<(sign?"s64.s32":"u64.u32")<<" "<<dest<<","<<temp<<";\n";
    }
    return o.str();
}
std::string ptx_store_integer_global(
    int dtype,const char* address,const char* source,const char* temp) {
    const auto bits=gpu_integer_bits(dtype);
    std::ostringstream o;
    if(bits==64) o<<" st.global.u64 ["<<address<<"],"<<source<<";\n";
    else{
        o<<" cvt.u32.u64 "<<temp<<","<<source<<";\n"
         <<" st.global.u"<<bits<<" ["<<address<<"],"<<temp<<";\n";
    }
    return o.str();
}

std::string ptx_checked_add(
    int dtype,const char* a,const char* b,const char* out,const char* bad,
    std::string_view prefix) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    if(bits<64){
        o<<" add."<<(sign?"s64":"u64")<<" "<<out<<","<<a<<","<<b<<";\n"
         <<" mov.u64 %rd62,"<<integer_max_text(dtype)<<";\n"
         <<" setp.gt."<<(sign?"s64":"u64")<<" %p12,"<<out<<",%rd62;\n"
         <<" @%p12 bra "<<bad<<";\n";
        if(sign){
            o<<" mov.s64 %rd63,"<<integer_min_text(dtype)<<";\n"
             <<" setp.lt.s64 %p13,"<<out<<",%rd63;\n @%p13 bra "<<bad<<";\n";
        }
    }else if(sign){
        o<<" setp.gt.s64 %p12,"<<b<<",0;\n @!%p12 bra "<<prefix<<"_ADD_NEG;\n"
         <<" mov.s64 %rd62,0x7fffffffffffffff;\n sub.s64 %rd62,%rd62,"<<b<<";\n"
         <<" setp.gt.s64 %p13,"<<a<<",%rd62;\n @%p13 bra "<<bad<<";\n"
         <<" bra "<<prefix<<"_ADD_DO;\n"<<prefix<<"_ADD_NEG:\n"
         <<" setp.lt.s64 %p12,"<<b<<",0;\n @!%p12 bra "<<prefix<<"_ADD_DO;\n"
         <<" mov.s64 %rd62,0x8000000000000000;\n sub.s64 %rd62,%rd62,"<<b<<";\n"
         <<" setp.lt.s64 %p13,"<<a<<",%rd62;\n @%p13 bra "<<bad<<";\n"
         <<prefix<<"_ADD_DO:\n add.s64 "<<out<<","<<a<<","<<b<<";\n";
    }else{
        o<<" mov.u64 %rd62,0xffffffffffffffff;\n sub.u64 %rd62,%rd62,"<<b<<";\n"
         <<" setp.gt.u64 %p12,"<<a<<",%rd62;\n @%p12 bra "<<bad<<";\n"
         <<" add.u64 "<<out<<","<<a<<","<<b<<";\n";
    }
    return o.str();
}

std::string ptx_checked_sub(
    int dtype,const char* a,const char* b,const char* out,const char* bad,
    std::string_view prefix) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    if(bits<64){
        if(!sign)
            o<<" setp.lt.u64 %p12,"<<a<<","<<b<<";\n @%p12 bra "<<bad<<";\n";
        o<<" sub."<<(sign?"s64":"u64")<<" "<<out<<","<<a<<","<<b<<";\n";
        if(sign){
            o<<" mov.s64 %rd62,"<<integer_max_text(dtype)<<";\n"
             <<" setp.gt.s64 %p12,"<<out<<",%rd62;\n @%p12 bra "<<bad<<";\n"
             <<" mov.s64 %rd63,"<<integer_min_text(dtype)<<";\n"
             <<" setp.lt.s64 %p13,"<<out<<",%rd63;\n @%p13 bra "<<bad<<";\n";
        }
    }else if(sign){
        o<<" setp.gt.s64 %p12,"<<b<<",0;\n @!%p12 bra "<<prefix<<"_SUB_NEG;\n"
         <<" mov.s64 %rd62,0x8000000000000000;\n add.s64 %rd62,%rd62,"<<b<<";\n"
         <<" setp.lt.s64 %p13,"<<a<<",%rd62;\n @%p13 bra "<<bad<<";\n"
         <<" bra "<<prefix<<"_SUB_DO;\n"<<prefix<<"_SUB_NEG:\n"
         <<" setp.lt.s64 %p12,"<<b<<",0;\n @!%p12 bra "<<prefix<<"_SUB_DO;\n"
         <<" mov.s64 %rd62,0x7fffffffffffffff;\n add.s64 %rd62,%rd62,"<<b<<";\n"
         <<" setp.gt.s64 %p13,"<<a<<",%rd62;\n @%p13 bra "<<bad<<";\n"
         <<prefix<<"_SUB_DO:\n sub.s64 "<<out<<","<<a<<","<<b<<";\n";
    }else{
        o<<" setp.lt.u64 %p12,"<<a<<","<<b<<";\n @%p12 bra "<<bad<<";\n"
         <<" sub.u64 "<<out<<","<<a<<","<<b<<";\n";
    }
    return o.str();
}

std::string ptx_checked_mul(
    int dtype,const char* a,const char* b,const char* out,const char* bad) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    if(bits<64){
        o<<" mul.lo."<<(sign?"s64":"u64")<<" "<<out<<","<<a<<","<<b<<";\n"
         <<" mov.u64 %rd62,"<<integer_max_text(dtype)<<";\n"
         <<" setp.gt."<<(sign?"s64":"u64")<<" %p12,"<<out<<",%rd62;\n"
         <<" @%p12 bra "<<bad<<";\n";
        if(sign){
            o<<" mov.s64 %rd63,"<<integer_min_text(dtype)<<";\n"
             <<" setp.lt.s64 %p13,"<<out<<",%rd63;\n @%p13 bra "<<bad<<";\n";
        }
    }else if(sign){
        o<<" mul.lo.s64 "<<out<<","<<a<<","<<b<<";\n"
         <<" mul.hi.s64 %rd61,"<<a<<","<<b<<";\n"
         <<" setp.lt.s64 %p12,"<<out<<",0;\n"
         <<" selp.s64 %rd62,-1,0,%p12;\n"
         <<" setp.ne.s64 %p13,%rd61,%rd62;\n @%p13 bra "<<bad<<";\n";
    }else{
        o<<" mul.lo.u64 "<<out<<","<<a<<","<<b<<";\n"
         <<" mul.hi.u64 %rd61,"<<a<<","<<b<<";\n"
         <<" setp.ne.u64 %p12,%rd61,0;\n @%p12 bra "<<bad<<";\n";
    }
    return o.str();
}

std::string ptx_integer_binary_source(int dtype,int operation,int scalar_side) {
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    const auto kind=ptx_integer_kind(dtype);
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n\n.visible .entry q_kernel(\n"
      " .param .u64 p_out,\n .param .u64 p_left,\n .param .u64 p_right,\n"
      <<" .param ."<<kind<<" p_scalar,\n"
      " .param .u64 p_count,\n .param .u64 p_status)\n{\n"
      " .reg .pred %p<16>;\n .reg .b32 %r<20>;\n .reg .b64 %rd<64>;\n"
      " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_left];\n"
      " ld.param.u64 %rd3,[p_right];\n ld.param.u64 %rd5,[p_count];\n"
      " ld.param.u64 %rd6,[p_status];\n"
      " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
      " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
      " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
      <<" mul.lo.u64 %rd8,%rd7,"<<(bits/8)<<";\n"
      " add.u64 %rd9,%rd2,%rd8;\n add.u64 %rd10,%rd3,%rd8;\n"
      " add.u64 %rd11,%rd1,%rd8;\n";
    o<<ptx_load_integer_global(dtype,"%rd9","%rd20","%r5");
    if(scalar_side==abi::scalar_side::none) o<<ptx_load_integer_global(dtype,"%rd10","%rd21","%r6");
    else o<<ptx_load_integer_param(dtype,"p_scalar","%rd21","%r6");
    const char* a=scalar_side==abi::scalar_side::left?"%rd21":"%rd20";
    const char* b=scalar_side==abi::scalar_side::left?"%rd20":"%rd21";
    if(operation==abi::tensor_binary_opcode::add)o<<ptx_checked_add(dtype,a,b,"%rd30","BAD","BIN");
    else if(operation==abi::tensor_binary_opcode::subtract)o<<ptx_checked_sub(dtype,a,b,"%rd30","BAD","BIN");
    else if(operation==abi::tensor_binary_opcode::multiply)o<<ptx_checked_mul(dtype,a,b,"%rd30","BAD");
    else if(operation==abi::tensor_binary_opcode::divide){
        o<<" setp.eq."<<(sign?"s64":"u64")<<" %p2,"<<b<<",0;\n @%p2 bra BAD;\n";
        if(sign){
            o<<" mov.s64 %rd62,0x8000000000000000;\n"
              " setp.eq.s64 %p3,"<<a<<",%rd62;\n setp.eq.s64 %p4,"<<b<<",-1;\n"
              " and.pred %p5,%p3,%p4;\n @%p5 bra BAD;\n";
        }
        o<<" div."<<(sign?"s64":"u64")<<" %rd30,"<<a<<","<<b<<";\n";
    }else{
        o<<" setp.eq."<<(sign?"s64":"u64")<<" %p2,"<<b<<",0;\n @%p2 bra BAD;\n";
        if(sign){
            o<<" mov.s64 %rd62,0x8000000000000000;\n"
              " setp.eq.s64 %p3,"<<a<<",%rd62;\n setp.eq.s64 %p4,"<<b<<",-1;\n"
              " and.pred %p5,%p3,%p4;\n @!%p5 bra REM_DO;\n mov.s64 %rd30,0;\n bra STORE;\nREM_DO:\n";
        }
        o<<" rem."<<(sign?"s64":"u64")<<" %rd30,"<<a<<","<<b<<";\n";
    }
    o<<"STORE:\n"<<ptx_store_integer_global(dtype,"%rd11","%rd30","%r12")
      <<" bra DONE;\nBAD:\n mov.u32 %r10,1;\n atom.global.exch.b32 %r11,[%rd6],%r10;\n"
        "DONE:\n ret;\n}\n";
    return o.str();
}

std::string ptx_integer_unary_source(int dtype) {
    const auto bits=gpu_integer_bits(dtype);
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n\n.visible .entry q_kernel(\n"
      " .param .u64 p_out,\n .param .u64 p_in,\n .param .u64 p_count,\n .param .u64 p_status)\n{\n"
      " .reg .pred %p<4>;\n .reg .b32 %r<12>;\n .reg .b64 %rd<32>;\n"
      " ld.param.u64 %rd1,[p_out];ld.param.u64 %rd2,[p_in];ld.param.u64 %rd5,[p_count];"
      "ld.param.u64 %rd6,[p_status];mov.u32 %r1,%ctaid.x;mov.u32 %r2,%ntid.x;"
      "mov.u32 %r3,%tid.x;mad.lo.s32 %r4,%r1,%r2,%r3;cvt.u64.u32 %rd7,%r4;"
      "setp.ge.u64 %p1,%rd7,%rd5;@%p1 bra DONE;"
      <<"mul.lo.u64 %rd8,%rd7,"<<(bits/8)<<";add.u64 %rd9,%rd2,%rd8;add.u64 %rd10,%rd1,%rd8;";
    o<<ptx_load_integer_global(dtype,"%rd9","%rd20","%r5");
    if(bits==64)o<<" mov.s64 %rd21,0x8000000000000000;setp.eq.s64 %p2,%rd20,%rd21;@%p2 bra BAD;";
    else o<<" mov.s64 %rd21,"<<integer_min_text(dtype)<<";setp.eq.s64 %p2,%rd20,%rd21;@%p2 bra BAD;";
    o<<" neg.s64 %rd30,%rd20;\n"<<ptx_store_integer_global(dtype,"%rd10","%rd30","%r6")
      <<"bra DONE;BAD:mov.u32 %r10,1;atom.global.exch.b32 %r11,[%rd6],%r10;DONE:ret;}\n";
    return o.str();
}

std::string ptx_integer_cast_source(int source_dtype,int target_dtype) {
    const auto sbits=gpu_integer_bits(source_dtype);
    const bool ss=gpu_integer_signed(source_dtype);
    const bool target_int=gpu_integer_dtype(target_dtype);
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n\n.visible .entry q_kernel(\n"
      " .param .u64 p_out,\n .param .u64 p_in,\n .param .u64 p_count,\n .param .u64 p_status)\n{\n"
      " .reg .pred %p<8>;\n .reg .b32 %r<16>;\n .reg .b64 %rd<40>;\n .reg .f32 %f<4>;\n .reg .f64 %fd<4>;\n"
      "ld.param.u64 %rd1,[p_out];ld.param.u64 %rd2,[p_in];ld.param.u64 %rd5,[p_count];"
      "ld.param.u64 %rd6,[p_status];mov.u32 %r1,%ctaid.x;mov.u32 %r2,%ntid.x;mov.u32 %r3,%tid.x;"
      "mad.lo.s32 %r4,%r1,%r2,%r3;cvt.u64.u32 %rd7,%r4;setp.ge.u64 %p1,%rd7,%rd5;@%p1 bra DONE;"
      <<"mul.lo.u64 %rd8,%rd7,"<<(sbits/8)<<";add.u64 %rd9,%rd2,%rd8;";
    o<<ptx_load_integer_global(source_dtype,"%rd9","%rd20","%r5");
    if(target_int){
        const auto tbits=gpu_integer_bits(target_dtype);
        const bool ts=gpu_integer_signed(target_dtype);
        if(ss&&ts){
            if(tbits<64){
                o<<"mov.s64 %rd30,"<<integer_min_text(target_dtype)<<";setp.lt.s64 %p2,%rd20,%rd30;@%p2 bra BAD;"
                  "mov.s64 %rd31,"<<integer_max_text(target_dtype)<<";setp.gt.s64 %p3,%rd20,%rd31;@%p3 bra BAD;";
            }
        }else if(ss&&!ts){
            o<<"setp.lt.s64 %p2,%rd20,0;@%p2 bra BAD;";
            if(tbits<64)
                o<<"mov.u64 %rd31,"<<integer_max_text(target_dtype)<<";setp.gt.u64 %p3,%rd20,%rd31;@%p3 bra BAD;";
        }else if(!ss&&ts){
            o<<"mov.u64 %rd31,"<<integer_max_text(target_dtype)<<";setp.gt.u64 %p3,%rd20,%rd31;@%p3 bra BAD;";
        }else if(tbits<64){
            o<<"mov.u64 %rd31,"<<integer_max_text(target_dtype)<<";setp.gt.u64 %p3,%rd20,%rd31;@%p3 bra BAD;";
        }
        const auto out_bytes=tbits/8;
        o<<"mul.lo.u64 %rd10,%rd7,"<<out_bytes<<";add.u64 %rd11,%rd1,%rd10;"
         <<ptx_store_integer_global(target_dtype,"%rd11","%rd20","%r6");
    }else{
        const auto out_bytes=target_dtype==QCORE_DTYPE_FLOAT32?4:8;
        o<<"mul.lo.u64 %rd10,%rd7,"<<out_bytes<<";add.u64 %rd11,%rd1,%rd10;";
        if(target_dtype==QCORE_DTYPE_FLOAT32){
            o<<"cvt.rn.f32."<<(ss?"s64":"u64")<<" %f1,%rd20;st.global.f32 [%rd11],%f1;";
        }else{
            o<<"cvt.rn.f64."<<(ss?"s64":"u64")<<" %fd1,%rd20;st.global.f64 [%rd11],%fd1;";
        }
    }
    o<<"bra DONE;BAD:mov.u32 %r10,1;atom.global.exch.b32 %r11,[%rd6],%r10;DONE:ret;}\n";
    return o.str();
}



std::string ptx_integer_gather_backward_source(int dtype) {
    const auto bits=gpu_integer_bits(dtype);
    std::ostringstream o;
    o<<".version 6.0\n.target sm_30\n.address_size 64\n\n.visible .entry q_kernel(\n"
      " .param .u64 p_out,\n .param .u64 p_grad,\n .param .u64 p_offsets,\n"
      " .param .u64 p_positions,\n .param .u64 p_count,\n .param .u64 p_status)\n{\n"
      " .reg .pred %p<16>;\n .reg .b32 %r<20>;\n .reg .b64 %rd<64>;\n"
      " ld.param.u64 %rd1,[p_out];\n ld.param.u64 %rd2,[p_grad];\n"
      " ld.param.u64 %rd3,[p_offsets];\n ld.param.u64 %rd4,[p_positions];\n"
      " ld.param.u64 %rd5,[p_count];\n ld.param.u64 %rd6,[p_status];\n"
      " mov.u32 %r1,%ctaid.x;\n mov.u32 %r2,%ntid.x;\n mov.u32 %r3,%tid.x;\n"
      " mad.lo.s32 %r4,%r1,%r2,%r3;\n cvt.u64.u32 %rd7,%r4;\n"
      " setp.ge.u64 %p1,%rd7,%rd5;\n @%p1 bra DONE;\n"
      " mul.lo.u64 %rd8,%rd7,8;\n add.u64 %rd9,%rd3,%rd8;\n"
      " ld.global.u64 %rd10,[%rd9];\n add.u64 %rd11,%rd9,8;\n"
      " ld.global.u64 %rd12,[%rd11];\n mov.u64 %rd30,0;\n"
      "LOOP:\n setp.ge.u64 %p2,%rd10,%rd12;\n @%p2 bra STORE;\n"
      " mul.lo.u64 %rd13,%rd10,8;\n add.u64 %rd14,%rd4,%rd13;\n"
      " ld.global.u64 %rd15,[%rd14];\n"
      <<" mul.lo.u64 %rd16,%rd15,"<<(bits/8)<<";\n"
      " add.u64 %rd17,%rd2,%rd16;\n";
    o<<ptx_load_integer_global(dtype,"%rd17","%rd20","%r5")
     <<ptx_checked_add(dtype,"%rd30","%rd20","%rd30","BAD","SCAT");
    o<<" add.u64 %rd10,%rd10,1;\n bra LOOP;\nSTORE:\n"
      <<" mul.lo.u64 %rd18,%rd7,"<<(bits/8)<<";\n add.u64 %rd19,%rd1,%rd18;\n"
      <<ptx_store_integer_global(dtype,"%rd19","%rd30","%r7")
      <<" bra DONE;\nBAD:\n mov.u32 %r10,1;\n"
        " atom.global.exch.b32 %r11,[%rd6],%r10;\nDONE:\n ret;\n}\n";
    return o.str();
}

#ifdef __APPLE__
std::string msl_integer_type(int dtype) {
    switch(dtype){
        case QCORE_DTYPE_INT64: return "long"; case QCORE_DTYPE_INT8: return "char"; case QCORE_DTYPE_INT16: return "short"; case QCORE_DTYPE_INT32: return "int";
        case QCORE_DTYPE_UINT8: return "uchar"; case QCORE_DTYPE_UINT16: return "ushort"; case QCORE_DTYPE_UINT32: return "uint"; case QCORE_DTYPE_UINT64: return "ulong";
        default:return {};
    }
}
std::string msl_integer_helpers(int dtype) {
    const auto t=msl_integer_type(dtype);
    const auto bits=gpu_integer_bits(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    o<<"inline bool q_add("<<t<<" a,"<<t<<" b,thread "<<t<<"& r){";
    if(bits<64){
        o<<(sign?"long":"ulong")<<" q=("<<(sign?"long":"ulong")<<")a+("<<(sign?"long":"ulong")<<")b;"
         <<"if(q<("<<(sign?"long":"ulong")<<")"<<integer_min_text(dtype)
         <<"||q>("<<(sign?"long":"ulong")<<")"<<integer_max_text(dtype)<<")return true;r=("<<t<<")q;return false;}";
    }else if(sign){
        o<<"if((b>0&&a>long(0x7fffffffffffffff)-b)||(b<0&&a<long(0x8000000000000000)-b))return true;"
          "r=a+b;return false;}";
    }else{
        o<<"if(a>ulong(0xffffffffffffffff)-b)return true;r=a+b;return false;}";
    }
    o<<"inline bool q_sub("<<t<<" a,"<<t<<" b,thread "<<t<<"& r){";
    if(bits<64){
        if(sign)
            o<<"long q=long(a)-long(b);if(q<long("<<integer_min_text(dtype)<<")||q>long("<<integer_max_text(dtype)<<"))return true;";
        else
            o<<"if(a<b)return true;ulong q=ulong(a)-ulong(b);";
        o<<"r=("<<t<<")q;return false;}";
    }else if(sign){
        o<<"if((b>0&&a<long(0x8000000000000000)+b)||(b<0&&a>long(0x7fffffffffffffff)+b))return true;"
          "r=a-b;return false;}";
    }else{
        o<<"if(a<b)return true;r=a-b;return false;}";
    }
    o<<"inline bool q_mul("<<t<<" a,"<<t<<" b,thread "<<t<<"& r){";
    if(bits<64){
        o<<(sign?"long":"ulong")<<" q=("<<(sign?"long":"ulong")<<")a*("<<(sign?"long":"ulong")<<")b;"
         <<"if(q<("<<(sign?"long":"ulong")<<")"<<integer_min_text(dtype)
         <<"||q>("<<(sign?"long":"ulong")<<")"<<integer_max_text(dtype)<<")return true;r=("<<t<<")q;return false;}";
    }else if(sign){
        o<<"if(a==0||b==0){r=0;return false;}if((a==-1&&b==long(0x8000000000000000))||(b==-1&&a==long(0x8000000000000000)))return true;"
          "if(a>0){if((b>0&&a>long(0x7fffffffffffffff)/b)||(b<0&&b<long(0x8000000000000000)/a))return true;}"
          "else{if((b>0&&a<long(0x8000000000000000)/b)||(b<0&&a<long(0x7fffffffffffffff)/b))return true;}"
          "r=a*b;return false;}";
    }else{
        o<<"if(b!=0&&a>ulong(0xffffffffffffffff)/b)return true;r=a*b;return false;}";
    }
    return o.str();
}
std::string msl_integer_binary_source(int dtype,int operation,int scalar_side) {
    const auto t=msl_integer_type(dtype);
    const bool sign=gpu_integer_signed(dtype);
    std::ostringstream o;
    o<<"#include <metal_stdlib>\nusing namespace metal;\n"<<msl_integer_helpers(dtype)
     <<"kernel void q_kernel(device "<<t<<"* out [[buffer(0)]],const device "<<t<<"* left [[buffer(1)]],"
       "const device "<<t<<"* right [[buffer(2)]],constant "<<t<<"& scalar [[buffer(3)]],"
       "constant ulong& count [[buffer(4)]],device atomic_uint* status [[buffer(5)]],"
       "uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"<<t<<" a=left[i],b=";
    if(scalar_side==abi::scalar_side::none)o<<"right[i];";else o<<"scalar;";
    if(scalar_side==abi::scalar_side::left)o<<t<<" q=a;a=b;b=q;";
    o<<t<<" r=0;bool bad=false;";
    if(operation==abi::tensor_binary_opcode::add)o<<"bad=q_add(a,b,r);";
    else if(operation==abi::tensor_binary_opcode::subtract)o<<"bad=q_sub(a,b,r);";
    else if(operation==abi::tensor_binary_opcode::multiply)o<<"bad=q_mul(a,b,r);";
    else if(operation==abi::tensor_binary_opcode::divide){
        o<<"if(b==0)bad=true;";
        if(sign)o<<"else if(a==("<<t<<")"<<integer_min_text(dtype)<<"&&b==("<<t<<")-1)bad=true;";
        o<<"else r=a/b;";
    }else{
        o<<"if(b==0)bad=true;else{";
        if(sign)o<<"if(a==("<<t<<")"<<integer_min_text(dtype)<<"&&b==("<<t<<")-1)r=0;else ";
        o<<"r=a%b;}";
    }
    o<<"if(bad){atomic_store_explicit(status,1u,memory_order_relaxed);return;}out[i]=r;}\n";
    return o.str();
}
std::string msl_integer_unary_source(int dtype) {
    const auto t=msl_integer_type(dtype);
    return "#include <metal_stdlib>\nusing namespace metal;\nkernel void q_kernel(device "+t+
      "* out [[buffer(0)]],const device "+t+"* in [[buffer(1)]],constant ulong& count [[buffer(2)]],"
      "device atomic_uint* status [[buffer(3)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"
      +t+" v=in[i];if(v==("+t+")"+integer_min_text(dtype)+"){atomic_store_explicit(status,1u,memory_order_relaxed);return;}"
      "out[i]=("+t+")(-v);}\n";
}
std::string msl_integer_cast_source(int source_dtype,int target_dtype) {
    const auto src=msl_integer_type(source_dtype);
    const bool ss=gpu_integer_signed(source_dtype);
    std::string dst=gpu_integer_dtype(target_dtype)?msl_integer_type(target_dtype):"float";
    std::ostringstream o;
    o<<"#include <metal_stdlib>\nusing namespace metal;\nkernel void q_kernel(device "<<dst
     <<"* out [[buffer(0)]],const device "<<src<<"* in [[buffer(1)]],constant ulong& count [[buffer(2)]],"
       "device atomic_uint* status [[buffer(3)]],uint gid [[thread_position_in_grid]]){ulong i=ulong(gid);if(i>=count)return;"
     <<src<<" v=in[i];";
    if(gpu_integer_dtype(target_dtype)){
        const bool ts=gpu_integer_signed(target_dtype);const auto tb=gpu_integer_bits(target_dtype);
        if(ss&&ts&&tb<64)o<<"if(long(v)<long("<<integer_min_text(target_dtype)<<")||long(v)>long("<<integer_max_text(target_dtype)<<")){atomic_store_explicit(status,1u,memory_order_relaxed);return;}";
        else if(ss&&!ts){
            o<<"if(v<0){atomic_store_explicit(status,1u,memory_order_relaxed);return;}";
            if(tb<64)o<<"if(ulong(v)>ulong("<<integer_max_text(target_dtype)<<")){atomic_store_explicit(status,1u,memory_order_relaxed);return;}";
        }else if(!ss&&ts)o<<"if(ulong(v)>ulong("<<integer_max_text(target_dtype)<<")){atomic_store_explicit(status,1u,memory_order_relaxed);return;}";
        else if(tb<64)o<<"if(ulong(v)>ulong("<<integer_max_text(target_dtype)<<")){atomic_store_explicit(status,1u,memory_order_relaxed);return;}";
    }
    o<<"out[i]=("<<dst<<")v;}\n";return o.str();
}

std::string msl_integer_gather_backward_source(int dtype) {
    const auto t=msl_integer_type(dtype);
    std::ostringstream o;
    o<<"#include <metal_stdlib>\nusing namespace metal;\n"<<msl_integer_helpers(dtype)
     <<"kernel void q_kernel(device "<<t<<"* out [[buffer(0)]],const device "<<t
     <<"* grad [[buffer(1)]],const device ulong* offsets [[buffer(2)]],"
       "const device ulong* positions [[buffer(3)]],constant ulong& count [[buffer(4)]],"
       "device atomic_uint* status [[buffer(5)]],uint gid [[thread_position_in_grid]]){"
       "ulong i=ulong(gid);if(i>=count)return;"<<t<<" total=0;"
       "for(ulong p=offsets[i];p<offsets[i+1];++p){"<<t
     <<" next=0;if(q_add(total,grad[positions[p]],next)){"
       "atomic_store_explicit(status,1u,memory_order_relaxed);return;}total=next;}"
       "out[i]=total;}\n";
    return o.str();
}

#endif

} // namespace
