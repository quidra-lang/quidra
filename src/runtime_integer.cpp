// Integer arithmetic, storage, conversions and public integer entry points.
// Included only by runtime_exact.cpp; not a separate translation unit.
#ifndef QUIDRA_EXACT_SECTION
#error "exact-runtime fragment requires its aggregate translation unit"
#endif

#if QUIDRA_EXACT_SECTION == 0

// A failure of the exact-number substrate (EXACT_NUMERIC): a provider's
// availability or domain, an internal invariant.
[[noreturn]] inline void exact_fail(const char* message,
                             unsigned long long line=0,
                             unsigned long long column=0) {
    quidra::runtime::report_failure(
        abi::FailureReason::exact_numeric_failure, abi::FailureArgs{.message = message},
        line, column);
}

// A conversion of an exact number that fails (NUMERIC_CONVERSION, subject
// value), to the type that `type` (a conversion type) names.
[[noreturn]] inline void exact_conversion_fail(abi::ConversionReason reason, int type,
                                               unsigned long long line,
                                               unsigned long long column) {
    quidra::runtime::report_conversion(reason, abi::conversion_type_name(type),
                                       abi::ConversionSubject::value, line, column);
}

// A try-conversion of an exact number that declines records why (read by a
// fallible conversion's error block) and returns false.
inline bool exact_conversion_declined(abi::ConversionReason reason) {
    quidra::runtime::record_conversion_reason(reason);
    return false;
}

// An exact division by zero (DIVISION_BY_ZERO).
[[noreturn]] inline void exact_division_by_zero(unsigned long long line = 0,
                                                unsigned long long column = 0) {
    quidra::runtime::report_failure(abi::FailureReason::division_by_zero, {}, line, column);
}

// An exact decision refinement could not prove (EXACT_UNPROVEN): the
// message names the decision, the reason says why.
[[noreturn]] inline void exact_unproven(abi::FailureReason reason, const char* message,
                                        unsigned long long line = 0,
                                        unsigned long long column = 0) {
    quidra::runtime::report_failure(reason, abi::FailureArgs{.message = message}, line, column);
}


#endif

#if QUIDRA_EXACT_SECTION == 2
struct BigInt {
    int sign{0};
    // Little-endian base-1e9 limbs. std::vector keeps a reusable capacity and
    // grows geometrically on mainstream standard-library implementations.
    std::vector<std::uint32_t> limbs;

    BigInt()=default;
    BigInt(int s,std::vector<std::uint32_t> v)
        :sign(s),limbs(std::move(v)){normalize();}

    void normalize(){
        while(!limbs.empty()&&limbs.back()==0)limbs.pop_back();
        if(limbs.empty())sign=0;
        else sign=sign<0?-1:1;
    }

    static BigInt from_u64(std::uint64_t value){
        if(!value)return {};
        std::vector<std::uint32_t> out;
        out.reserve(3);
        while(value){
            out.push_back(static_cast<std::uint32_t>(value%LIMB_BASE));
            value/=LIMB_BASE;
        }
        return BigInt(1,std::move(out));
    }

    static BigInt from_i64(std::int64_t value){
        if(value>=0)return from_u64(static_cast<std::uint64_t>(value));
        auto magnitude=static_cast<std::uint64_t>(-(value+1))+1ULL;
        auto out=from_u64(magnitude);out.sign=-1;return out;
    }

    static BigInt parse(std::string_view text,bool& ok){
        ok=false;
        if(text.empty())return {};
        std::size_t begin=0;int s=1;
        if(text[0]=='+'||text[0]=='-'){
            if(text[0]=='-')s=-1;
            begin=1;
            if(begin==text.size())return {};
        }
        for(std::size_t i=begin;i<text.size();++i)
            if(text[i]<'0'||text[i]>'9')return {};
        while(begin<text.size()&&text[begin]=='0')++begin;
        if(begin==text.size()){ok=true;return {};}
        std::vector<std::uint32_t> limbs;
        limbs.reserve((text.size()-begin+LIMB_DIGITS-1)/LIMB_DIGITS);
        std::size_t end=text.size();
        while(end>begin){
            const std::size_t start=end>=begin+LIMB_DIGITS?end-LIMB_DIGITS:begin;
            std::uint32_t limb=0;
            for(std::size_t i=start;i<end;++i)
                limb=limb*10U+static_cast<unsigned>(text[i]-'0');
            limbs.push_back(limb);
            end=start;
        }
        ok=true;return BigInt(s,std::move(limbs));
    }

    std::string text()const{
        if(!sign)return "0";
        std::ostringstream out;
        if(sign<0)out<<'-';
        out<<limbs.back();
        for(std::size_t i=limbs.size()-1;i>0;--i)
            out<<std::setw(LIMB_DIGITS)<<std::setfill('0')<<limbs[i-1];
        return out.str();
    }
};

inline int cmp_abs(const BigInt&a,const BigInt&b){
    if(a.limbs.size()!=b.limbs.size())
        return a.limbs.size()<b.limbs.size()?-1:1;
    for(std::size_t i=a.limbs.size();i>0;--i){
        if(a.limbs[i-1]!=b.limbs[i-1])
            return a.limbs[i-1]<b.limbs[i-1]?-1:1;
    }
    return 0;
}
inline int cmp(const BigInt&a,const BigInt&b){
    if(a.sign!=b.sign)return a.sign<b.sign?-1:1;
    if(!a.sign)return 0;
    const int c=cmp_abs(a,b);return a.sign<0?-c:c;
}
inline BigInt neg(BigInt value){value.sign=-value.sign;return value;}
inline BigInt absbi(BigInt value){if(value.sign<0)value.sign=1;return value;}

inline BigInt add_abs(const BigInt&a,const BigInt&b){
    std::vector<std::uint32_t> out;
    out.reserve(std::max(a.limbs.size(),b.limbs.size())+1);
    std::uint64_t carry=0;
    const auto count=std::max(a.limbs.size(),b.limbs.size());
    for(std::size_t i=0;i<count;++i){
        std::uint64_t total=carry;
        if(i<a.limbs.size())total+=a.limbs[i];
        if(i<b.limbs.size())total+=b.limbs[i];
        out.push_back(static_cast<std::uint32_t>(total%LIMB_BASE));
        carry=total/LIMB_BASE;
    }
    if(carry)out.push_back(static_cast<std::uint32_t>(carry));
    return BigInt(1,std::move(out));
}
inline BigInt sub_abs(const BigInt&a,const BigInt&b){
    // |a| >= |b|
    std::vector<std::uint32_t> out(a.limbs.size());
    std::int64_t borrow=0;
    for(std::size_t i=0;i<a.limbs.size();++i){
        std::int64_t value=static_cast<std::int64_t>(a.limbs[i])-borrow-
            (i<b.limbs.size()?static_cast<std::int64_t>(b.limbs[i]):0);
        if(value<0){value+=LIMB_BASE;borrow=1;}else borrow=0;
        out[i]=static_cast<std::uint32_t>(value);
    }
    return BigInt(1,std::move(out));
}
inline BigInt add(const BigInt&a,const BigInt&b){
    if(!a.sign)return b;
    if(!b.sign)return a;
    if(a.sign==b.sign){auto out=add_abs(a,b);out.sign=a.sign;return out;}
    const int c=cmp_abs(a,b);if(!c)return {};
    auto out=c>0?sub_abs(a,b):sub_abs(b,a);
    out.sign=c>0?a.sign:b.sign;return out;
}
inline BigInt sub(const BigInt&a,const BigInt&b){return add(a,neg(b));}
inline BigInt mul_small(const BigInt&a,std::uint32_t factor){
    if(!a.sign||!factor)return {};
    std::vector<std::uint32_t> out;
    out.reserve(a.limbs.size()+1);
    std::uint64_t carry=0;
    for(auto limb:a.limbs){
        const std::uint64_t value=static_cast<std::uint64_t>(limb)*factor+carry;
        out.push_back(static_cast<std::uint32_t>(value%LIMB_BASE));
        carry=value/LIMB_BASE;
    }
    if(carry)out.push_back(static_cast<std::uint32_t>(carry));
    return BigInt(a.sign,std::move(out));
}
inline BigInt mul(const BigInt&a,const BigInt&b){
    if(!a.sign||!b.sign)return {};
    std::vector<std::uint64_t> accum(a.limbs.size()+b.limbs.size(),0);
    for(std::size_t i=0;i<a.limbs.size();++i){
        std::uint64_t carry=0;
        for(std::size_t j=0;j<b.limbs.size();++j){
            const std::size_t k=i+j;
            const std::uint64_t total=
                static_cast<std::uint64_t>(a.limbs[i])*b.limbs[j]+
                accum[k]+carry;
            accum[k]=total%LIMB_BASE;
            carry=total/LIMB_BASE;
        }
        std::size_t k=i+b.limbs.size();
        while(carry){
            const auto total=accum[k]+carry;
            accum[k]=total%LIMB_BASE;
            carry=total/LIMB_BASE;
            ++k;
            if(k==accum.size()&&carry)accum.push_back(0);
        }
    }
    std::vector<std::uint32_t> out;
    out.reserve(accum.size());
    for(auto limb:accum)out.push_back(static_cast<std::uint32_t>(limb));
    return BigInt(a.sign*b.sign,std::move(out));
}
inline std::pair<BigInt,std::uint32_t> div_small(const BigInt&a,std::uint32_t divisor){
    if(!divisor)exact_division_by_zero();
    if(!a.sign)return {{},0};
    std::vector<std::uint32_t> out(a.limbs.size());
    std::uint64_t rem=0;
    for(std::size_t i=a.limbs.size();i>0;--i){
        const std::uint64_t cur=rem*LIMB_BASE+a.limbs[i-1];
        out[i-1]=static_cast<std::uint32_t>(cur/divisor);
        rem=cur%divisor;
    }
    return {BigInt(a.sign,std::move(out)),static_cast<std::uint32_t>(rem)};
}
inline std::pair<BigInt,BigInt> divmod(const BigInt&a,const BigInt&b){
    if(!b.sign)exact_division_by_zero();
    if(!a.sign)return {{},{}};
    const BigInt aa=absbi(a),bb=absbi(b);
    if(cmp_abs(aa,bb)<0)return {{},a};

    // Long division directly in base-1e9 limbs. Decimal text is never used as
    // the arithmetic representation or quotient intermediate.
    std::vector<std::uint32_t> quotient(aa.limbs.size(),0);
    BigInt rem;
    rem.limbs.reserve(bb.limbs.size()+1);
    for(std::size_t i=aa.limbs.size();i>0;--i){
        rem.limbs.insert(rem.limbs.begin(),aa.limbs[i-1]);
        rem.sign=1;
        rem.normalize();
        std::uint32_t lo=0,hi=LIMB_BASE-1,best=0;
        while(lo<=hi){
            const auto mid=lo+static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(hi)-lo)/2);
            const auto candidate=mul_small(bb,mid);
            if(cmp_abs(candidate,rem)<=0){
                best=mid;
                if(mid==LIMB_BASE-1)break;
                lo=mid+1;
            }else{
                if(mid==0)break;
                hi=mid-1;
            }
        }
        quotient[i-1]=best;
        if(best)rem=sub_abs(rem,mul_small(bb,best));
    }
    BigInt q(1,std::move(quotient));
    q.sign=q.sign?a.sign*b.sign:0;
    rem.sign=rem.sign?a.sign:0;
    return {std::move(q),std::move(rem)};
}
inline BigInt gcd(BigInt a,BigInt b){
    a=absbi(std::move(a));b=absbi(std::move(b));
    while(b.sign){auto r=divmod(a,b).second;a=std::move(b);b=std::move(r);}
    return a;
}
inline BigInt pow_small(BigInt base,unsigned exponent){
    auto out=BigInt::from_u64(1);
    while(exponent){
        if(exponent&1U)out=mul(out,base);
        exponent>>=1U;
        if(exponent)base=mul(base,base);
    }
    return out;
}
inline BigInt pow10(std::size_t n){
    BigInt result=BigInt::from_u64(1);
    BigInt base=BigInt::from_u64(10);
    while(n){
        if(n&1U)result=mul(result,base);
        n>>=1U;
        if(n)base=mul(base,base);
    }
    return result;
}

inline bool bigint_try_i64_value(const BigInt& source,int bits,long long& out){
    if(bits<=0||bits>64)return false;
    const auto text=source.text();
    char* end=nullptr;errno=0;
    const auto value=std::strtoll(text.c_str(),&end,10);
    if(errno==ERANGE||!end||*end!='\0')return false;
    if(bits<64){
        const long long low=-(1LL<<(bits-1));
        const long long high=(1LL<<(bits-1))-1;
        if(value<low||value>high)return false;
    }
    out=value;
    return true;
}
inline bool bigint_try_u64_value(
    const BigInt& source,int bits,unsigned long long& out){
    if(bits<=0||bits>64||source.sign<0)return false;
    const auto text=source.text();
    char* end=nullptr;errno=0;
    const auto value=std::strtoull(text.c_str(),&end,10);
    if(errno==ERANGE||!end||*end!='\0')return false;
    if(bits<64&&value>((1ULL<<bits)-1ULL))return false;
    out=value;
    return true;
}


#endif

#if QUIDRA_EXACT_SECTION == 4
struct BigIntValue{BigInt value;};


#endif

#if QUIDRA_EXACT_SECTION == 6
template<class T,class...A>T* managed_new(A&&...args){
    void* memory=quidra_managed_alloc(sizeof(T));
    quidra::runtime::NodeRuntimeOwner partial(memory);
    try {
        auto* result=new(memory)T{std::forward<A>(args)...};
        partial.relinquish();
        return result;
    } catch (...) {
        exact_fail("exact numeric allocation failed");
    }
}
inline BigIntValue* bi(void*p){return static_cast<BigIntValue*>(p);}

#endif

#if QUIDRA_EXACT_SECTION == 8
inline void* make_bi(BigInt value){
    return managed_new<BigIntValue>(BigIntValue{std::move(value)});
}

#endif

#if QUIDRA_EXACT_SECTION == 10
inline char* copy_text(const std::string&text){
    return quidra_runtime_copy_text_bytes(
        text.data(),static_cast<unsigned long long>(text.size()));
}


#endif

#if QUIDRA_EXACT_SECTION == 12
extern "C" void quidra_bigint_drop(void*p){
    if(p)bi(p)->~BigIntValue();
}


#endif

#if QUIDRA_EXACT_SECTION == 14
extern "C" void* quidra_bigint_literal(const char*text){
    bool ok=false;auto value=BigInt::parse(text?text:"",ok);
    if(!ok)exact_fail("invalid bigint literal");
    return make_bi(std::move(value));
}
extern "C" void* quidra_bigint_parse(const char*text){
    bool ok=false;auto value=BigInt::parse(text?text:"",ok);
    return ok?make_bi(std::move(value)):nullptr;
}

#endif

#if QUIDRA_EXACT_SECTION == 16
extern "C" char* quidra_bigint_text(void*p){
    if(!p)exact_fail("null bigint");
    return copy_text(bi(p)->value.text());
}


#endif

#if QUIDRA_EXACT_SECTION == 18
extern "C" void* quidra_bigint_neg(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    return make_bi(neg(bi(p)->value));
}
extern "C" void* quidra_bigint_binary(
    void*left,void*right,int operation,
    unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigint operand",line,column);
    const auto&a=bi(left)->value;const auto&b=bi(right)->value;
    switch(operation){
        case abi::exact_binary_opcode::add: return make_bi(add(a,b));
        case abi::exact_binary_opcode::subtract: return make_bi(sub(a,b));
        case abi::exact_binary_opcode::multiply: return make_bi(mul(a,b));
        case abi::exact_binary_opcode::divide: return make_bi(divmod(a,b).first);
        case abi::exact_binary_opcode::remainder: return make_bi(divmod(a,b).second);
        default:exact_fail("invalid bigint operation",line,column);
    }
}
extern "C" void* quidra_bigint_pow(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigint power operand",line,column);
    auto base=bi(left)->value;
    auto exponent=bi(right)->value;
    if(exponent.sign<0)
        quidra::runtime::report_failure(abi::FailureReason::negative_integer_exponent,{},
                                        line,column);
    auto result=BigInt::from_u64(1);
    while(exponent.sign){
        auto quotient=div_small(exponent,2);
        if((quotient.second&1U)!=0)result=mul(result,base);
        exponent=std::move(quotient.first);
        if(exponent.sign)base=mul(base,base);
    }
    return make_bi(std::move(result));
}
extern "C" int quidra_bigint_compare(void*left,void*right){
    if(!left||!right)exact_fail("null bigint comparison");
    return cmp(bi(left)->value,bi(right)->value);
}

#endif

#if QUIDRA_EXACT_SECTION == 20
extern "C" void* quidra_bigint_from_i64(long long value){
    return make_bi(BigInt::from_i64(value));
}
extern "C" void* quidra_bigint_from_u64(unsigned long long value){
    return make_bi(BigInt::from_u64(value));
}

// A bigint converts to a fixed-width type unless it is outside its range.

#endif

#if QUIDRA_EXACT_SECTION == 22
extern "C" bool quidra_bigint_try_i64(void*p,int bits,long long*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0;
    if(!bigint_try_i64_value(bi(p)->value,bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}
extern "C" bool quidra_bigint_try_u64(
    void*p,int bits,unsigned long long*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0;
    if(!bigint_try_u64_value(bi(p)->value,bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}


#endif

#if QUIDRA_EXACT_SECTION == 24
extern "C" long long quidra_bigint_to_i64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    long long value=0;
    if(!bigint_try_i64_value(bi(p)->value,bits,value))
        exact_conversion_fail(abi::ConversionReason::out_of_range,
                              abi::integer_conversion_type(bits,true),line,column);
    return value;
}
extern "C" unsigned long long quidra_bigint_to_u64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    unsigned long long value=0;
    if(bi(p)->value.sign<0||!bigint_try_u64_value(bi(p)->value,bits,value))
        exact_conversion_fail(abi::ConversionReason::out_of_range,
                              abi::integer_conversion_type(bits,false),line,column);
    return value;
}

#endif

#if QUIDRA_EXACT_SECTION == 26
extern "C" bool quidra_bigint_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0;
    double value=0;
    // Parse directly into the destination format. A long-double or double
    // intermediate can erase an integer just above a rounding midpoint.
    try{value=std::stod(bi(p)->value.text());}
    catch(...){return exact_conversion_declined(abi::ConversionReason::out_of_range);}
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" bool quidra_bigint_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0F;
    float result=0.0F;
    try{result=std::stof(bi(p)->value.text());}
    catch(...){return exact_conversion_declined(abi::ConversionReason::out_of_range);}
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}

#endif

#if QUIDRA_EXACT_SECTION == 28
extern "C" double quidra_bigint_to_float64(
    void*p,unsigned long long line,unsigned long long column){
    double result=0.0;
    if(!quidra_bigint_try_float64(p,&result))
        exact_conversion_fail(abi::ConversionReason::out_of_range,QCORE_DTYPE_FLOAT64,line,column);
    return result;
}
extern "C" float quidra_bigint_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    float result=0.0F;
    if(!quidra_bigint_try_float32(p,&result))
        exact_conversion_fail(abi::ConversionReason::out_of_range,QCORE_DTYPE_FLOAT32,line,column);
    return result;
}

// Arbitrary-precision integers as words (abi::bare_integer_layout). The
// generated code takes the inline fast paths; these entry points take every
// other case, and each gives the values, texts and failures of the boxed
// entry points above.
namespace {

namespace word_layout = abi::bare_integer_layout;

bool word_boxed(long long word) {
    return (static_cast<unsigned long long>(word) &
            static_cast<unsigned long long>(word_layout::boxed_bit)) != 0;
}
BigIntValue* word_box(long long word) {
    return reinterpret_cast<BigIntValue*>(static_cast<std::uintptr_t>(
        static_cast<unsigned long long>(word) &
        ~static_cast<unsigned long long>(word_layout::boxed_bit)));
}
long long word_small(long long word) { return word >> 1; }
long long small_word(long long value) {
    return static_cast<long long>(static_cast<unsigned long long>(value) << 1);
}
bool small_inline(long long value) {
    return value >= word_layout::inline_min && value <= word_layout::inline_max;
}

// The value of a word: the boxed integer itself, or the inline value.
class WordValue {
public:
    explicit WordValue(long long word) {
        if (word_boxed(word)) {
            value_ = &word_box(word)->value;
        } else {
            storage_ = BigInt::from_i64(word_small(word));
            value_ = &storage_;
        }
    }
    const BigInt& get() const { return *value_; }

private:
    BigInt storage_;
    const BigInt* value_{};
};

// `value` as an int64, when it fits.
bool bigint_exact_i64(const BigInt& value, long long& out) {
    unsigned long long magnitude = 0;
    for (std::size_t i = value.limbs.size(); i > 0; --i) {
        const unsigned long long limb = value.limbs[i - 1];
        if (magnitude > (~0ULL - limb) / LIMB_BASE) return false;
        magnitude = magnitude * LIMB_BASE + limb;
    }
    constexpr unsigned long long positive_limit = 9223372036854775807ULL;
    if (value.sign >= 0) {
        if (magnitude > positive_limit) return false;
        out = static_cast<long long>(magnitude);
        return true;
    }
    if (magnitude > positive_limit + 1ULL) return false;
    out = magnitude == positive_limit + 1ULL
        ? static_cast<long long>(-9223372036854775807LL - 1)
        : -static_cast<long long>(magnitude);
    return true;
}

long long box_word(BigInt value) {
    auto* box = static_cast<BigIntValue*>(make_bi(std::move(value)));
    return static_cast<long long>(reinterpret_cast<std::uintptr_t>(box) |
                                  static_cast<std::uintptr_t>(word_layout::boxed_bit));
}

// The canonical word of `value`: inline when it is within the inline range.
long long canonical_word(BigInt value) {
    long long small = 0;
    if (bigint_exact_i64(value, small) && small_inline(small)) return small_word(small);
    return box_word(std::move(value));
}

long long word_from_i64(long long value) {
    return small_inline(value) ? small_word(value) : box_word(BigInt::from_i64(value));
}

bool small_fits_signed(long long value, int bits) {
    if (bits <= 0 || bits > 64) return false;
    if (bits == 64) return true;
    const long long low = -(1LL << (bits - 1));
    const long long high = (1LL << (bits - 1)) - 1;
    return value >= low && value <= high;
}
bool small_fits_unsigned(long long value, int bits) {
    if (bits <= 0 || bits > 64 || value < 0) return false;
    if (bits == 64) return true;
    return static_cast<unsigned long long>(value) <= ((1ULL << bits) - 1ULL);
}

} // namespace

extern "C" long long quidra_int_add(long long left, long long right) {
    if (!word_boxed(left) && !word_boxed(right))
        return word_from_i64(word_small(left) + word_small(right));
    return canonical_word(add(WordValue(left).get(), WordValue(right).get()));
}
extern "C" long long quidra_int_sub(long long left, long long right) {
    if (!word_boxed(left) && !word_boxed(right))
        return word_from_i64(word_small(left) - word_small(right));
    return canonical_word(sub(WordValue(left).get(), WordValue(right).get()));
}
extern "C" long long quidra_int_mul(long long left, long long right) {
    if (!word_boxed(left) && !word_boxed(right)) {
        const long long a = word_small(left), b = word_small(right);
        constexpr long long half = 1LL << 31;
        if (a > -half && a < half && b > -half && b < half) return word_from_i64(a * b);
    }
    return canonical_word(mul(WordValue(left).get(), WordValue(right).get()));
}
extern "C" long long quidra_int_div(
    long long left, long long right, unsigned long long line, unsigned long long column) {
    (void)line;
    (void)column;
    const WordValue a(left), b(right);
    return canonical_word(divmod(a.get(), b.get()).first);
}
extern "C" long long quidra_int_rem(
    long long left, long long right, unsigned long long line, unsigned long long column) {
    (void)line;
    (void)column;
    const WordValue a(left), b(right);
    return canonical_word(divmod(a.get(), b.get()).second);
}
extern "C" long long quidra_int_neg(
    long long value, unsigned long long line, unsigned long long column) {
    (void)line;
    (void)column;
    if (!word_boxed(value)) return word_from_i64(-word_small(value));
    return canonical_word(neg(word_box(value)->value));
}
extern "C" long long quidra_int_pow(
    long long left, long long right, unsigned long long line, unsigned long long column) {
    auto base = WordValue(left).get();
    auto exponent = WordValue(right).get();
    if (exponent.sign < 0)
        quidra::runtime::report_failure(abi::FailureReason::negative_integer_exponent, {}, line,
                                        column);
    if (base.sign == 0 && exponent.sign == 0)
        quidra::runtime::report_failure(abi::FailureReason::zero_power_zero, {}, line, column);
    auto result = BigInt::from_u64(1);
    while (exponent.sign) {
        auto quotient = div_small(exponent, 2);
        if ((quotient.second & 1U) != 0) result = mul(result, base);
        exponent = std::move(quotient.first);
        if (exponent.sign) base = mul(base, base);
    }
    return canonical_word(std::move(result));
}
extern "C" long long quidra_int_factorial(
    long long value, unsigned long long line, unsigned long long column) {
    const WordValue operand(value);
    const auto& bound = operand.get();
    if (bound.sign < 0)
        quidra::runtime::report_failure(abi::FailureReason::factorial_negative, {}, line, column);
    const auto one = BigInt::from_u64(1);
    auto result = one;
    for (auto factor = BigInt::from_u64(2); cmp(factor, bound) <= 0; factor = add(factor, one))
        result = mul(result, factor);
    return canonical_word(std::move(result));
}

namespace {
unsigned long long factorial_limit(int bits, bool is_signed,
                                   unsigned long long line, unsigned long long column) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        exact_fail("invalid integer factorial width", line, column);
    if (is_signed) return (1ULL << (bits - 1)) - 1ULL;
    return bits == 64 ? ~0ULL : (1ULL << bits) - 1ULL;
}

unsigned long long fixed_factorial(unsigned long long value, unsigned long long limit,
                                  unsigned long long line, unsigned long long column) {
    unsigned long long result = 1;
    for (unsigned long long factor = 2; factor <= value; ++factor) {
        // Every supported width overflows by 21!, before the loop counter
        // can wrap, including an input of UINT64_MAX.
        if (result > limit / factor)
            quidra::runtime::report_failure(abi::FailureReason::integer_overflow, {}, line, column);
        result *= factor;
    }
    return result;
}
} // namespace

extern "C" long long quidra_integer_factorial_signed(
    long long value, int bits, unsigned long long line, unsigned long long column) {
    if (value < 0)
        quidra::runtime::report_failure(abi::FailureReason::factorial_negative, {}, line, column);
    return static_cast<long long>(fixed_factorial(static_cast<unsigned long long>(value),
                                                factorial_limit(bits, true, line, column),
                                                line, column));
}

extern "C" unsigned long long quidra_integer_factorial_unsigned(
    unsigned long long value, int bits, unsigned long long line, unsigned long long column) {
    return fixed_factorial(value, factorial_limit(bits, false, line, column), line, column);
}

extern "C" int quidra_int_compare(long long left, long long right) {
    if (!word_boxed(left) && !word_boxed(right))
        return (left > right) - (left < right);
    return cmp(WordValue(left).get(), WordValue(right).get());
}
extern "C" char* quidra_int_text(long long value) {
    if (!word_boxed(value)) return copy_text(std::to_string(word_small(value)));
    return copy_text(word_box(value)->value.text());
}
extern "C" bool quidra_int_parse(const char* text, long long* out) {
    if (!out) exact_fail("null bigint parse storage");
    *out = 0;
    bool ok = false;
    auto value = BigInt::parse(text ? text : "", ok);
    if (!ok) return false;
    *out = canonical_word(std::move(value));
    return true;
}
namespace {
// The boxes of integer literals live for the whole process. Package code on
// threads that exit handlers stop and join (warm-ups) may still read them
// while the process exits, after static destruction has begun, so the
// registry is allocated once and never destroyed: no box is freed at exit,
// and every box stays reachable from it for leak checkers.
struct IntLiteralBoxes {
    std::mutex mutex;
    std::vector<BigIntValue*> boxes;

    void keep(BigIntValue* value) {
        std::lock_guard<std::mutex> lock(mutex);
        boxes.push_back(value);
    }
};

IntLiteralBoxes& int_literal_boxes() {
    static auto* const registry = new IntLiteralBoxes;
    return *registry;
}
} // namespace

extern "C" long long quidra_int_literal(const char* text) {
    bool ok = false;
    auto value = BigInt::parse(text ? text : "", ok);
    if (!ok) exact_fail("invalid bigint literal");
    long long small = 0;
    if (bigint_exact_i64(value, small) && small_inline(small)) return small_word(small);
    // A literal's box lives for the rest of the process (int_literal_boxes):
    // it is not managed storage, so retaining and releasing it do nothing.
    auto* box = new (std::nothrow) BigIntValue{std::move(value)};
    if (!box) exact_fail("exact numeric allocation failed");
    try {
        int_literal_boxes().keep(box);
    } catch (...) {
        delete box;
        exact_fail("exact numeric allocation failed");
    }
    return static_cast<long long>(reinterpret_cast<std::uintptr_t>(box) |
                                  static_cast<std::uintptr_t>(word_layout::boxed_bit));
}
extern "C" long long quidra_int_from_i64(long long value) {
    return word_from_i64(value);
}
extern "C" long long quidra_int_from_u64(unsigned long long value) {
    constexpr auto limit = static_cast<unsigned long long>(word_layout::inline_max);
    if (value <= limit) return small_word(static_cast<long long>(value));
    return box_word(BigInt::from_u64(value));
}
extern "C" long long quidra_int_to_i64_checked(
    long long value, int bits, unsigned long long line, unsigned long long column) {
    long long out = 0;
    if (!quidra_int_try_i64(value, bits, &out))
        exact_conversion_fail(abi::ConversionReason::out_of_range,
                              abi::integer_conversion_type(bits, true), line, column);
    return out;
}
extern "C" unsigned long long quidra_int_to_u64_checked(
    long long value, int bits, unsigned long long line, unsigned long long column) {
    const bool negative = word_boxed(value) ? word_box(value)->value.sign < 0
                                            : word_small(value) < 0;
    unsigned long long out = 0;
    if (negative || !quidra_int_try_u64(value, bits, &out))
        exact_conversion_fail(abi::ConversionReason::out_of_range,
                              abi::integer_conversion_type(bits, false), line, column);
    return out;
}
extern "C" bool quidra_int_try_i64(long long value, int bits, long long* out) {
    if (!out) exact_fail("null bigint conversion storage");
    *out = 0;
    if (!word_boxed(value)) {
        const long long small = word_small(value);
        if (!small_fits_signed(small, bits)) return false;
        *out = small;
        return true;
    }
    return bigint_try_i64_value(word_box(value)->value, bits, *out);
}
extern "C" bool quidra_int_try_u64(long long value, int bits, unsigned long long* out) {
    if (!out) exact_fail("null bigint conversion storage");
    *out = 0;
    if (!word_boxed(value)) {
        const long long small = word_small(value);
        if (!small_fits_unsigned(small, bits)) return false;
        *out = static_cast<unsigned long long>(small);
        return true;
    }
    return bigint_try_u64_value(word_box(value)->value, bits, *out);
}
extern "C" bool quidra_int_try_float64(long long value, double* out) {
    if (!out) exact_fail("null bigint conversion storage");
    *out = 0.0;
    if (!word_boxed(value)) {
        *out = static_cast<double>(word_small(value));
        return true;
    }
    return quidra_bigint_try_float64(word_box(value), out);
}
extern "C" bool quidra_int_try_float32(long long value, float* out) {
    if (!out) exact_fail("null bigint conversion storage");
    *out = 0.0F;
    if (!word_boxed(value)) {
        *out = static_cast<float>(word_small(value));
        return true;
    }
    return quidra_bigint_try_float32(word_box(value), out);
}
extern "C" double quidra_int_to_float64(
    long long value, unsigned long long line, unsigned long long column) {
    double result = 0.0;
    if (!quidra_int_try_float64(value, &result))
        exact_conversion_fail(abi::ConversionReason::out_of_range, QCORE_DTYPE_FLOAT64, line,
                              column);
    return result;
}
extern "C" float quidra_int_to_float32(
    long long value, unsigned long long line, unsigned long long column) {
    float result = 0.0F;
    if (!quidra_int_try_float32(value, &result))
        exact_conversion_fail(abi::ConversionReason::out_of_range, QCORE_DTYPE_FLOAT32, line,
                              column);
    return result;
}
extern "C" bool quidra_int_cast_fits(long long value, int target_kind, int bits) {
    if (!word_boxed(value)) {
        const long long small = word_small(value);
        switch (target_kind) {
            case 1: case 2: case 5: case 6: return true;
            case 3: return small_fits_signed(small, bits);
            case 4: return small_fits_unsigned(small, bits);
            case 7: return small >= 0;
            default: return false;
        }
    }
    if (target_kind == 7) return word_box(value)->value.sign >= 0;
    return quidra_exact_numeric_cast_fits(word_box(value), 1, target_kind, bits);
}
extern "C" long long quidra_nat_sub(
    long long left, long long right, unsigned long long line, unsigned long long column) {
    auto difference = sub(WordValue(left).get(), WordValue(right).get());
    if (difference.sign < 0)
        quidra::runtime::report_failure(abi::FailureReason::nat_negative, {}, line, column);
    return canonical_word(std::move(difference));
}
extern "C" long long quidra_int_detach(long long value) {
    // The box moves to plain storage, which no thread's managed table owns,
    // and the managed box is released by the thread that owns it.
    if (!word_boxed(value)) return value;
    auto* box = new (std::nothrow) BigIntValue{word_box(value)->value};
    if (!box) exact_fail("exact numeric allocation failed");
    quidra_managed_release(word_box(value), reinterpret_cast<void*>(&quidra_bigint_drop));
    return static_cast<long long>(reinterpret_cast<std::uintptr_t>(box) |
                                  static_cast<std::uintptr_t>(word_layout::boxed_bit));
}
extern "C" long long quidra_int_attach(long long value) {
    if (!word_boxed(value)) return value;
    auto* box = word_box(value);
    const long long word = box_word(std::move(box->value));
    delete box;
    return word;
}
// Keep the existing sequence for machine-integer ranges. Larger ranges
// draw uniform base-1e9 limbs and reject the excess tail without bias.
extern "C" long long quidra_random_integer(void* generator,long long start,long long end,
    unsigned long long line,unsigned long long column){
    const WordValue left(start),right(end);
    const auto& low=left.get();const auto& high=right.get();
    if(cmp(low,high)>=0){
        quidra_runtime_fail_at("INVALID_RANDOM_RANGE","invalid random range",line,column);
        std::abort();
    }
    long long small_low=0,small_high=0;
    if(bigint_exact_i64(low,small_low)&&bigint_exact_i64(high,small_high))
        return word_from_i64(quidra_random_int(generator,small_low,small_high));
    const auto span=sub(high,low);
    const auto bounded=[&](std::uint64_t bound){
        const auto threshold=(std::uint64_t{0}-bound)%bound;
        std::uint64_t value=0;
        do{value=quidra_random_bits(generator);}while(value<threshold);
        return static_cast<std::uint32_t>(value%bound);
    };
    BigInt sample;
    do{
        std::vector<std::uint32_t> limbs(span.limbs.size());
        for(std::size_t i=0;i<limbs.size();++i)
            limbs[i]=bounded(i+1==limbs.size()?std::uint64_t(span.limbs.back())+1:LIMB_BASE);
        sample=BigInt(1,std::move(limbs));
    }while(cmp(sample,span)>=0);
    return canonical_word(add(low,sample));
}

extern "C" long long quidra_int_adopt(void* big_integer) {
    if (!big_integer) exact_fail("null bigint");
    long long small = 0;
    if (bigint_exact_i64(bi(big_integer)->value, small) && small_inline(small)) {
        quidra_managed_release(big_integer, reinterpret_cast<void*>(&quidra_bigint_drop));
        return small_word(small);
    }
    return static_cast<long long>(reinterpret_cast<std::uintptr_t>(big_integer) |
                                  static_cast<std::uintptr_t>(word_layout::boxed_bit));
}

// Called once at entry by the typed for header. A valid step does not
// allocate text; a failing arbitrary-precision step retains every digit.
extern "C" void quidra_for_step_check(long long step, bool descending,
    unsigned long long line, unsigned long long column) {
    const auto sign=quidra_int_compare(step,0);
    if(sign==0)
        quidra::runtime::report_failure(abi::FailureReason::for_step_zero,{},line,column);
    if((sign<0)==descending) return;
    const auto text=WordValue(step).get().text();
    quidra::runtime::report_failure(descending?abi::FailureReason::for_step_descending:
        abi::FailureReason::for_step_ascending,abi::FailureArgs{.step_text=text},line,column);
}

#endif
