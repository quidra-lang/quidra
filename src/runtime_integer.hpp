#pragma once

// The arbitrary-precision integers of the exact-number runtime.
//
// Owns: BigInt and its arithmetic, the boxed bigint value and its
// conversions to machine integers, the failures of exact numbers (their
// conversions, division by zero, undecided proofs and the EXACT_NUMERIC
// substrate failures) and the allocation helpers. runtime_integer.cpp
// (bigint) and runtime_exact_real.cpp (real, whose rationals are BigInt
// fractions) share them.

#include "runtime_report.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::runtime_integer {

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

// An exact comparison or evaluation the finite proof budget could not
// decide (EXACT_UNPROVEN).
[[noreturn]] inline void exact_unproven(abi::FailureReason reason, unsigned long long line = 0,
                                        unsigned long long column = 0) {
    quidra::runtime::report_failure(reason, {}, line, column);
}

inline constexpr std::uint32_t LIMB_BASE=1000000000U;
inline constexpr int LIMB_DIGITS=9;

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

struct BigIntValue{BigInt value;};

template<class T,class...A>T* managed_new(A&&...args){
    void* memory=quidra_managed_alloc(sizeof(T));
    try{return new(memory)T{std::forward<A>(args)...};}
    catch(...){exact_fail("exact numeric allocation failed");}
}
inline BigIntValue* bi(void*p){return static_cast<BigIntValue*>(p);}
inline void* make_bi(BigInt value){
    return managed_new<BigIntValue>(BigIntValue{std::move(value)});
}
inline char* copy_text(const std::string&text){
    return quidra_runtime_copy_text_bytes(
        text.data(),static_cast<unsigned long long>(text.size()));
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

} // namespace quidra::runtime_integer
