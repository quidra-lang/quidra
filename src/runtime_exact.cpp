#include "runtime_internal.hpp"
#include "quidra/native_extension.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" char* quidra_runtime_copy_text_bytes(
    const char*, unsigned long long);

namespace {

[[noreturn]] void exact_fail(const char* message,
                             unsigned long long line=0,
                             unsigned long long column=0) {
    if(line||column)
        std::fprintf(stderr,
            "Quidra runtime error [EXACT_NUMERIC] at %llu:%llu: %s\n",
            line,column,message);
    else
        std::fprintf(stderr,"Quidra runtime error [EXACT_NUMERIC]: %s\n",message);
    std::fflush(stderr);
    std::exit(101);
}

std::unordered_map<std::string,qcore_exact_real_atom_decimal_fn>& exact_atom_providers(){
    static std::unordered_map<std::string,qcore_exact_real_atom_decimal_fn> providers;
    return providers;
}
struct ExactUnaryProvider {
    qcore_exact_real_unary_float64_fn evaluate{};
    qcore_exact_real_unary_flags_fn flags{};
};
std::unordered_map<std::string,ExactUnaryProvider>& exact_unary_providers(){
    static std::unordered_map<std::string,ExactUnaryProvider> providers;
    return providers;
}
std::mutex& exact_atom_provider_mutex(){
    static std::mutex mutex;
    return mutex;
}
std::optional<std::string> exact_atom_decimal(
    const std::string& provider,std::uint32_t opcode){
    qcore_exact_real_atom_decimal_fn callback=nullptr;
    {
        std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
        const auto found=exact_atom_providers().find(provider);
        if(found==exact_atom_providers().end())return std::nullopt;
        callback=found->second;
    }
    if(!callback)return std::nullopt;
    const char* text=callback(opcode);
    if(!text||!*text)return std::nullopt;
    return std::string(text);
}

constexpr std::uint32_t LIMB_BASE=1000000000U;
constexpr int LIMB_DIGITS=9;

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

int cmp_abs(const BigInt&a,const BigInt&b){
    if(a.limbs.size()!=b.limbs.size())
        return a.limbs.size()<b.limbs.size()?-1:1;
    for(std::size_t i=a.limbs.size();i>0;--i){
        if(a.limbs[i-1]!=b.limbs[i-1])
            return a.limbs[i-1]<b.limbs[i-1]?-1:1;
    }
    return 0;
}
int cmp(const BigInt&a,const BigInt&b){
    if(a.sign!=b.sign)return a.sign<b.sign?-1:1;
    if(!a.sign)return 0;
    const int c=cmp_abs(a,b);return a.sign<0?-c:c;
}
BigInt neg(BigInt value){value.sign=-value.sign;return value;}
BigInt absbi(BigInt value){if(value.sign<0)value.sign=1;return value;}

BigInt add_abs(const BigInt&a,const BigInt&b){
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
BigInt sub_abs(const BigInt&a,const BigInt&b){
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
BigInt add(const BigInt&a,const BigInt&b){
    if(!a.sign)return b;
    if(!b.sign)return a;
    if(a.sign==b.sign){auto out=add_abs(a,b);out.sign=a.sign;return out;}
    const int c=cmp_abs(a,b);if(!c)return {};
    auto out=c>0?sub_abs(a,b):sub_abs(b,a);
    out.sign=c>0?a.sign:b.sign;return out;
}
BigInt sub(const BigInt&a,const BigInt&b){return add(a,neg(b));}
BigInt mul_small(const BigInt&a,std::uint32_t factor){
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
BigInt mul(const BigInt&a,const BigInt&b){
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
std::pair<BigInt,std::uint32_t> div_small(const BigInt&a,std::uint32_t divisor){
    if(!divisor)exact_fail("division by zero");
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
std::pair<BigInt,BigInt> divmod(const BigInt&a,const BigInt&b){
    if(!b.sign)exact_fail("division by zero");
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
BigInt gcd(BigInt a,BigInt b){
    a=absbi(std::move(a));b=absbi(std::move(b));
    while(b.sign){auto r=divmod(a,b).second;a=std::move(b);b=std::move(r);}
    return a;
}
BigInt pow_small(BigInt base,unsigned exponent){
    auto out=BigInt::from_u64(1);
    while(exponent){
        if(exponent&1U)out=mul(out,base);
        exponent>>=1U;
        if(exponent)base=mul(base,base);
    }
    return out;
}
BigInt pow10(std::size_t n){
    BigInt result=BigInt::from_u64(1);
    BigInt base=BigInt::from_u64(10);
    while(n){
        if(n&1U)result=mul(result,base);
        n>>=1U;
        if(n)base=mul(base,base);
    }
    return result;
}

struct Rational{
    BigInt num;
    BigInt den{BigInt::from_u64(1)};
    Rational()=default;
    Rational(BigInt n,BigInt d):num(std::move(n)),den(std::move(d)){normalize();}
    void normalize(){
        if(!den.sign)exact_fail("zero rational denominator");
        if(den.sign<0){den.sign=1;num=neg(std::move(num));}
        if(!num.sign){den=BigInt::from_u64(1);return;}
        const auto g=gcd(absbi(num),den);
        if(!(g.sign==1&&g.limbs.size()==1&&g.limbs[0]==1)){
            num=divmod(num,g).first;
            den=divmod(den,g).first;
            if(den.sign<0)den.sign=1;
        }
    }
};
Rational rat_add(const Rational&a,const Rational&b){
    return {add(mul(a.num,b.den),mul(b.num,a.den)),mul(a.den,b.den)};
}
Rational rat_sub(const Rational&a,const Rational&b){
    return {sub(mul(a.num,b.den),mul(b.num,a.den)),mul(a.den,b.den)};
}
Rational rat_mul(const Rational&a,const Rational&b){
    return {mul(a.num,b.num),mul(a.den,b.den)};
}
Rational rat_div(const Rational&a,const Rational&b){
    if(!b.num.sign)exact_fail("real division by zero");
    return {mul(a.num,b.den),mul(a.den,b.num)};
}
int rat_cmp(const Rational&a,const Rational&b){
    return cmp(mul(a.num,b.den),mul(b.num,a.den));
}
bool rat_int(const Rational&r){
    return r.den.sign==1&&r.den.limbs.size()==1&&r.den.limbs[0]==1;
}

bool parse_decimal(std::string_view text,Rational&out){
    if(text.empty())return false;
    std::string s(text);
    const auto ep=s.find_first_of("eE");
    long long exponent=0;
    if(ep!=std::string::npos){
        const std::string es=s.substr(ep+1);
        if(es.empty())return false;
        char* end=nullptr;errno=0;
        exponent=std::strtoll(es.c_str(),&end,10);
        if(errno==ERANGE||!end||*end!='\0'||std::llabs(exponent)>100000)
            return false;
        s.resize(ep);
    }
    int sign=1;
    if(!s.empty()&&(s[0]=='+'||s[0]=='-')){
        if(s[0]=='-')sign=-1;
        s.erase(0,1);
    }
    if(s.empty())return false;
    const auto dot=s.find('.');
    std::size_t fraction=0;
    if(dot!=std::string::npos){
        if(s.find('.',dot+1)!=std::string::npos)return false;
        fraction=s.size()-dot-1;s.erase(dot,1);
    }
    if(s.empty())return false;
    for(char c:s)if(c<'0'||c>'9')return false;
    bool ok=false;auto numerator=BigInt::parse(s,ok);
    if(!ok)return false;
    if(numerator.sign)numerator.sign=sign;
    BigInt denominator=BigInt::from_u64(1);
    const long long scale=static_cast<long long>(fraction)-exponent;
    if(scale>0)denominator=pow10(static_cast<std::size_t>(scale));
    else if(scale<0)numerator=mul(numerator,pow10(static_cast<std::size_t>(-scale)));
    out=Rational(std::move(numerator),std::move(denominator));
    return true;
}

Rational exact_double(double value){
    if(!std::isfinite(value))
        exact_fail("bigreal cannot contain NaN or infinity");
    if(value==0.0)return {};
    const auto bits=std::bit_cast<std::uint64_t>(value);
    const bool negative=(bits>>63U)!=0;
    const std::uint64_t exponent=(bits>>52U)&0x7ffULL;
    const std::uint64_t fraction=bits&((1ULL<<52U)-1ULL);
    std::uint64_t mantissa=0;int power=0;
    if(exponent==0){mantissa=fraction;power=-1022-52;}
    else{mantissa=(1ULL<<52U)|fraction;power=static_cast<int>(exponent)-1023-52;}
    auto numerator=BigInt::from_u64(mantissa);
    auto denominator=BigInt::from_u64(1);
    if(negative)numerator=neg(std::move(numerator));
    const auto two=BigInt::from_u64(2);
    if(power>=0)numerator=mul(numerator,pow_small(two,static_cast<unsigned>(power)));
    else denominator=pow_small(two,static_cast<unsigned>(-power));
    return Rational(std::move(numerator),std::move(denominator));
}

enum class RealKind{
    Rational,Atom,ProviderUnary,Pow,Add,Sub,Mul,Div,Neg
};
struct Real;
using RealPtr=std::shared_ptr<Real>;
struct Real{
    RealKind kind{RealKind::Rational};
    Rational rational;
    std::string provider;
    std::uint32_t opcode{};
    RealPtr a,b;
};

thread_local std::unordered_map<std::string,std::weak_ptr<Real>> real_intern;
thread_local std::size_t real_intern_operations = 0;

std::string real_key(const RealPtr&value,std::size_t&budget){
    if(!value)return "null";
    if(!budget--)exact_fail("bigreal canonicalization budget exhausted");
    if(value->kind==RealKind::Rational)
        return "q:"+value->rational.num.text()+"/"+value->rational.den.text();
    if(value->kind==RealKind::Atom)
        return "atom:"+value->provider+":"+std::to_string(value->opcode);
    if(value->kind==RealKind::ProviderUnary)
        return "unary:"+value->provider+":"+std::to_string(value->opcode)+
               "("+real_key(value->a,budget)+")";
    const auto tag=std::to_string(static_cast<int>(value->kind));
    return tag+"("+real_key(value->a,budget)+","+real_key(value->b,budget)+")";
}
RealPtr intern_real(RealPtr value){
    std::size_t budget=8192;
    const auto key=real_key(value,budget);
    if(const auto found=real_intern.find(key);found!=real_intern.end()){
        if(auto existing=found->second.lock())return existing;
        real_intern.erase(found);
    }
    real_intern[key]=value;
    if((++real_intern_operations & 255U)==0){
        for(auto it=real_intern.begin();it!=real_intern.end();){
            if(it->second.expired())it=real_intern.erase(it);
            else ++it;
        }
    }
    return value;
}
RealPtr rr(Rational rational){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Rational;value->rational=std::move(rational);
    return intern_real(std::move(value));
}
RealPtr atom(std::string provider,std::uint32_t opcode){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Atom;
    value->provider=std::move(provider);
    value->opcode=opcode;
    return intern_real(std::move(value));
}
RealPtr provider_unary(std::string provider,std::uint32_t opcode,RealPtr input){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::ProviderUnary;
    value->provider=std::move(provider);
    value->opcode=opcode;
    value->a=std::move(input);
    return intern_real(std::move(value));
}
std::optional<ExactUnaryProvider> exact_unary_provider(const std::string& provider){
    std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
    const auto found=exact_unary_providers().find(provider);
    if(found==exact_unary_providers().end())return std::nullopt;
    return found->second;
}
std::uint32_t exact_unary_flags(const RealPtr& value){
    if(!value||value->kind!=RealKind::ProviderUnary)return 0;
    const auto provider=exact_unary_provider(value->provider);
    return provider&&provider->flags?provider->flags(value->opcode):0;
}
RealPtr unary(RealKind kind,RealPtr a){
    auto value=std::make_shared<Real>();value->kind=kind;value->a=std::move(a);
    return intern_real(std::move(value));
}
RealPtr binary(RealKind kind,RealPtr a,RealPtr b){
    if(kind==RealKind::Add||kind==RealKind::Mul){
        std::size_t ba=4096,bb=4096;
        if(real_key(a,ba)>real_key(b,bb))std::swap(a,b);
    }
    auto value=std::make_shared<Real>();
    value->kind=kind;value->a=std::move(a);value->b=std::move(b);
    return intern_real(std::move(value));
}
bool is_zero(const RealPtr&x){
    return x&&x->kind==RealKind::Rational&&!x->rational.num.sign;
}
bool is_one(const RealPtr&x){
    return x&&x->kind==RealKind::Rational&&rat_int(x->rational)&&
        x->rational.num.sign==1&&x->rational.num.limbs.size()==1&&
        x->rational.num.limbs[0]==1;
}

bool atom_bounds(const RealPtr& x,Rational& low,Rational& high){
    if(!x||x->kind!=RealKind::Atom)return false;
    const auto decimal=exact_atom_decimal(x->provider,x->opcode);
    if(!decimal||!parse_decimal(*decimal,low))return false;

    std::string text=*decimal;
    const auto ep=text.find_first_of("eE");
    long long exponent=0;
    if(ep!=std::string::npos){
        const auto exponent_text=text.substr(ep+1);
        char* end=nullptr;
        errno=0;
        exponent=std::strtoll(exponent_text.c_str(),&end,10);
        if(errno==ERANGE||!end||*end!='\0')return false;
        text.resize(ep);
    }
    const auto dot=text.find('.');
    const long long fraction=
        dot==std::string::npos?0:
        static_cast<long long>(text.size()-dot-1);
    const long long power=exponent-fraction;
    Rational unit;
    if(power>=0){
        unit=Rational(
            pow10(static_cast<std::size_t>(power)),
            BigInt::from_u64(1));
    }else{
        unit=Rational(
            BigInt::from_u64(1),
            pow10(static_cast<std::size_t>(-power)));
    }
    high=rat_add(low,unit);
    return true;
}

enum class ProvenSign { Negative, Zero, Positive, NonNegative, NonPositive, Unknown };

ProvenSign invert_sign(ProvenSign sign){
    switch(sign){
        case ProvenSign::Negative:return ProvenSign::Positive;
        case ProvenSign::Positive:return ProvenSign::Negative;
        case ProvenSign::NonNegative:return ProvenSign::NonPositive;
        case ProvenSign::NonPositive:return ProvenSign::NonNegative;
        default:return sign;
    }
}

ProvenSign proven_sign(const RealPtr&x,std::size_t&budget){
    if(!x||!budget--)return ProvenSign::Unknown;
    switch(x->kind){
        case RealKind::Rational:
            return x->rational.num.sign<0?ProvenSign::Negative:
                   x->rational.num.sign>0?ProvenSign::Positive:
                                          ProvenSign::Zero;
        case RealKind::Atom:{
            Rational low,high;
            if(!atom_bounds(x,low,high))return ProvenSign::Unknown;
            const Rational zero{};
            if(rat_cmp(low,zero)>0)return ProvenSign::Positive;
            if(rat_cmp(high,zero)<0)return ProvenSign::Negative;
            if(rat_cmp(low,zero)==0&&rat_cmp(high,zero)>0)
                return ProvenSign::NonNegative;
            if(rat_cmp(low,zero)<0&&rat_cmp(high,zero)==0)
                return ProvenSign::NonPositive;
            return ProvenSign::Unknown;
        }
        case RealKind::ProviderUnary:{
            const auto flags=exact_unary_flags(x);
            if(flags&QCORE_EXACT_UNARY_RESULT_POSITIVE)
                return ProvenSign::Positive;
            if(flags&QCORE_EXACT_UNARY_RESULT_NONNEGATIVE)
                return ProvenSign::NonNegative;
            return ProvenSign::Unknown;
        }
        case RealKind::Neg:
            return invert_sign(proven_sign(x->a,budget));
        case RealKind::Mul:
        case RealKind::Div:{
            const auto left=proven_sign(x->a,budget);
            const auto right=proven_sign(x->b,budget);
            if(left==ProvenSign::Zero)return ProvenSign::Zero;
            if(x->kind==RealKind::Mul&&right==ProvenSign::Zero)
                return ProvenSign::Zero;
            if((left==ProvenSign::Positive||left==ProvenSign::Negative)&&
               (right==ProvenSign::Positive||right==ProvenSign::Negative))
                return left==right?ProvenSign::Positive:ProvenSign::Negative;
            return ProvenSign::Unknown;
        }
        case RealKind::Add:{
            const auto left=proven_sign(x->a,budget);
            const auto right=proven_sign(x->b,budget);
            if(left==ProvenSign::Zero)return right;
            if(right==ProvenSign::Zero)return left;
            if((left==ProvenSign::Positive||left==ProvenSign::NonNegative)&&
               (right==ProvenSign::Positive||right==ProvenSign::NonNegative))
                return left==ProvenSign::Positive||right==ProvenSign::Positive
                    ?ProvenSign::Positive:ProvenSign::NonNegative;
            if((left==ProvenSign::Negative||left==ProvenSign::NonPositive)&&
               (right==ProvenSign::Negative||right==ProvenSign::NonPositive))
                return left==ProvenSign::Negative||right==ProvenSign::Negative
                    ?ProvenSign::Negative:ProvenSign::NonPositive;
            return ProvenSign::Unknown;
        }
        case RealKind::Sub:{
            const auto left=proven_sign(x->a,budget);
            const auto right=invert_sign(proven_sign(x->b,budget));
            if(left==ProvenSign::Zero)return right;
            if(right==ProvenSign::Zero)return left;
            if((left==ProvenSign::Positive||left==ProvenSign::NonNegative)&&
               (right==ProvenSign::Positive||right==ProvenSign::NonNegative))
                return left==ProvenSign::Positive||right==ProvenSign::Positive
                    ?ProvenSign::Positive:ProvenSign::NonNegative;
            if((left==ProvenSign::Negative||left==ProvenSign::NonPositive)&&
               (right==ProvenSign::Negative||right==ProvenSign::NonPositive))
                return left==ProvenSign::Negative||right==ProvenSign::Negative
                    ?ProvenSign::Negative:ProvenSign::NonPositive;
            return ProvenSign::Unknown;
        }
        case RealKind::Pow:
            return ProvenSign::Unknown;
    }
    return ProvenSign::Unknown;
}
bool provably_defined(const RealPtr&x,std::size_t&budget){
    if(!x||!budget--)return false;
    switch(x->kind){
        case RealKind::Rational:
            return true;
        case RealKind::Atom:{
            Rational low,high;
            return atom_bounds(x,low,high);
        }
        case RealKind::ProviderUnary:{
            if(!provably_defined(x->a,budget))return false;
            const auto provider=exact_unary_provider(x->provider);
            if(!provider||!provider->evaluate||!provider->flags)return false;
            const auto flags=provider->flags(x->opcode);
            if(flags&QCORE_EXACT_UNARY_TOTAL)return true;
            std::size_t sign_budget=4096;
            const auto sign=proven_sign(x->a,sign_budget);
            if(flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE)
                return sign==ProvenSign::Positive;
            if(flags&QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE)
                return sign==ProvenSign::Zero||
                       sign==ProvenSign::Positive||
                       sign==ProvenSign::NonNegative;
            return false;
        }
        case RealKind::Neg:
            return provably_defined(x->a,budget);
        case RealKind::Add:
        case RealKind::Sub:
        case RealKind::Mul:
            return provably_defined(x->a,budget)&&
                   provably_defined(x->b,budget);
        case RealKind::Div:{
            if(!provably_defined(x->a,budget)||
               !provably_defined(x->b,budget))return false;
            std::size_t sign_budget=4096;
            const auto sign=proven_sign(x->b,sign_budget);
            return sign==ProvenSign::Positive||sign==ProvenSign::Negative;
        }
        case RealKind::Pow:{
            if(!provably_defined(x->a,budget)||
               !provably_defined(x->b,budget))return false;
            std::size_t sign_budget=4096;
            return proven_sign(x->a,sign_budget)==ProvenSign::Positive;
        }
    }
    return false;
}

bool structurally_equal(const RealPtr&a,const RealPtr&b,std::size_t&budget){
    if(a.get()==b.get())return true;
    if(!a||!b||!budget--)return false;
    if(a->kind!=b->kind)return false;
    if(a->kind==RealKind::Rational)return rat_cmp(a->rational,b->rational)==0;
    if(a->kind==RealKind::Atom)
        return a->provider==b->provider&&a->opcode==b->opcode;
    if(a->kind==RealKind::ProviderUnary &&
       (a->provider!=b->provider||a->opcode!=b->opcode))
        return false;
    return structurally_equal(a->a,b->a,budget)&&
           structurally_equal(a->b,b->b,budget);
}

RealPtr real_neg(RealPtr a){
    if(a->kind==RealKind::Rational){
        auto r=a->rational;r.num=neg(std::move(r.num));return rr(std::move(r));
    }
    if(a->kind==RealKind::Neg)return a->a;
    return unary(RealKind::Neg,std::move(a));
}
RealPtr real_add(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_add(a->rational,b->rational));
    if(is_zero(a))return b;
    if(is_zero(b))return a;
    return binary(RealKind::Add,std::move(a),std::move(b));
}
RealPtr real_sub(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_sub(a->rational,b->rational));
    if(is_zero(b))return a;
    std::size_t budget=4096;
    if(structurally_equal(a,b,budget)){
        std::size_t defined_budget=4096;
        if(provably_defined(a,defined_budget))return rr({});
    }
    return binary(RealKind::Sub,std::move(a),std::move(b));
}
RealPtr real_mul(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_mul(a->rational,b->rational));
    if(is_zero(a)){
        std::size_t defined_budget=4096;
        if(provably_defined(b,defined_budget))return rr({});
    }
    if(is_zero(b)){
        std::size_t defined_budget=4096;
        if(provably_defined(a,defined_budget))return rr({});
    }
    if(is_one(a))return b;
    if(is_one(b))return a;
    return binary(RealKind::Mul,std::move(a),std::move(b));
}
RealPtr real_div(RealPtr a,RealPtr b){
    if(is_zero(b))exact_fail("real division by zero");
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_div(a->rational,b->rational));
    if(is_zero(a)){
        std::size_t defined_budget=4096;
        std::size_t sign_budget=4096;
        const auto sign=proven_sign(b,sign_budget);
        if(provably_defined(b,defined_budget)&&
           (sign==ProvenSign::Positive||sign==ProvenSign::Negative))
            return rr({});
    }
    if(is_one(b))return a;
    std::size_t budget=4096;
    if(structurally_equal(a,b,budget)){
        std::size_t defined_budget=4096;
        std::size_t sign_budget=4096;
        const auto sign=proven_sign(a,sign_budget);
        if(provably_defined(a,defined_budget)&&
           (sign==ProvenSign::Positive||sign==ProvenSign::Negative))
            return rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
    }
    return binary(RealKind::Div,std::move(a),std::move(b));
}
struct BigIntValue{BigInt value;};
struct BigRealValue{RealPtr value;};

template<class T,class...A>T* managed_new(A&&...args){
    void* memory=quidra_managed_alloc(sizeof(T));
    try{return new(memory)T{std::forward<A>(args)...};}
    catch(...){exact_fail("exact numeric allocation failed");}
}
BigIntValue* bi(void*p){return static_cast<BigIntValue*>(p);}
BigRealValue* br(void*p){return static_cast<BigRealValue*>(p);}
void* make_bi(BigInt value){
    return managed_new<BigIntValue>(BigIntValue{std::move(value)});
}
void* make_br(RealPtr value){
    return managed_new<BigRealValue>(BigRealValue{std::move(value)});
}
char* copy_text(const std::string&text){
    return quidra_runtime_copy_text_bytes(
        text.data(),static_cast<unsigned long long>(text.size()));
}

std::string rational_decimal(const Rational&r,int significant){
    const int sig=significant>0?significant:34;
    if(!r.num.sign)return "0.0";
    const bool negative=r.num.sign<0;
    const auto absolute=absbi(r.num);
    auto qr=divmod(absolute,r.den);
    std::string whole=qr.first.text();
    BigInt rem=std::move(qr.second);
    std::string fraction;
    const int whole_sig=whole=="0"?0:static_cast<int>(whole.size());
    const int need=std::max(1,sig-whole_sig);
    for(int i=0;i<need+1&&rem.sign;++i){
        rem=mul_small(rem,10);
        auto digit=divmod(rem,r.den);
        fraction.push_back(
            digit.first.sign?static_cast<char>('0'+digit.first.limbs[0]):'0');
        rem=std::move(digit.second);
    }
    if(fraction.empty())fraction="0";
    if(static_cast<int>(fraction.size())>need){
        const bool up=fraction[static_cast<std::size_t>(need)]>='5';
        fraction.resize(static_cast<std::size_t>(need));
        if(up){
            int i=static_cast<int>(fraction.size())-1;
            while(i>=0&&fraction[static_cast<std::size_t>(i)]=='9'){
                fraction[static_cast<std::size_t>(i)]='0';--i;
            }
            if(i>=0)++fraction[static_cast<std::size_t>(i)];
            else{
                bool ok=false;auto w=BigInt::parse(whole,ok);
                whole=add(w,BigInt::from_u64(1)).text();
            }
        }
    }
    while(fraction.size()>1&&fraction.back()=='0')fraction.pop_back();
    return (negative?"-":"")+whole+"."+fraction;
}

long double eval_ld(const RealPtr&x,std::size_t&budget){
    if(!x||!budget--)exact_fail("bigreal evaluation budget exhausted");
    switch(x->kind){
        case RealKind::Rational:{
            try{
                const long double n=std::stold(x->rational.num.text());
                const long double d=std::stold(x->rational.den.text());
                return n/d;
            }catch(...){exact_fail("bigreal magnitude exceeds display evaluator range");}
        }
        case RealKind::Atom:{
            const auto decimal=exact_atom_decimal(x->provider,x->opcode);
            if(!decimal)exact_fail("exact-real provider atom is unavailable");
            try{return std::stold(*decimal);}
            catch(...){exact_fail("exact-real provider atom exceeds display evaluator range");}
        }
        case RealKind::ProviderUnary:{
            const auto provider=exact_unary_provider(x->provider);
            if(!provider||!provider->evaluate)
                exact_fail("exact-real provider unary operation is unavailable");
            const double input=static_cast<double>(eval_ld(x->a,budget));
            double output=0.0;
            if(!provider->evaluate(x->opcode,input,&output)||!std::isfinite(output))
                exact_fail("exact-real provider unary evaluation failed");
            return static_cast<long double>(output);
        }
        case RealKind::Pow:return std::pow(eval_ld(x->a,budget),eval_ld(x->b,budget));
        case RealKind::Add:return eval_ld(x->a,budget)+eval_ld(x->b,budget);
        case RealKind::Sub:return eval_ld(x->a,budget)-eval_ld(x->b,budget);
        case RealKind::Mul:return eval_ld(x->a,budget)*eval_ld(x->b,budget);
        case RealKind::Div:return eval_ld(x->a,budget)/eval_ld(x->b,budget);
        case RealKind::Neg:return -eval_ld(x->a,budget);
    }
    return 0;
}

bool try_eval_ld(const RealPtr& x, std::size_t& budget, long double& out) {
    if (!x || !budget--) return false;
    long double a = 0.0L;
    long double b = 0.0L;
    switch (x->kind) {
        case RealKind::Rational: {
            try {
                const long double n = std::stold(x->rational.num.text());
                const long double d = std::stold(x->rational.den.text());
                if (d == 0.0L) return false;
                out = n / d;
            } catch (...) {
                return false;
            }
            return std::isfinite(out);
        }
        case RealKind::Atom: {
            const auto decimal=exact_atom_decimal(x->provider,x->opcode);
            if(!decimal)return false;
            try{out=std::stold(*decimal);}
            catch(...){return false;}
            return std::isfinite(out);
        }
        case RealKind::ProviderUnary: {
            const auto provider=exact_unary_provider(x->provider);
            if(!provider||!provider->evaluate)return false;
            if(!try_eval_ld(x->a,budget,a))return false;
            double evaluated=0.0;
            if(!provider->evaluate(
                    x->opcode,static_cast<double>(a),&evaluated)||
               !std::isfinite(evaluated))
                return false;
            out=static_cast<long double>(evaluated);
            return true;
        }
        case RealKind::Pow:
            if (!try_eval_ld(x->a, budget, a) ||
                !try_eval_ld(x->b, budget, b)) return false;
            out = std::pow(a, b);
            return std::isfinite(out);
        case RealKind::Add:
            if (!try_eval_ld(x->a, budget, a) ||
                !try_eval_ld(x->b, budget, b)) return false;
            out = a + b;
            return std::isfinite(out);
        case RealKind::Sub:
            if (!try_eval_ld(x->a, budget, a) ||
                !try_eval_ld(x->b, budget, b)) return false;
            out = a - b;
            return std::isfinite(out);
        case RealKind::Mul:
            if (!try_eval_ld(x->a, budget, a) ||
                !try_eval_ld(x->b, budget, b)) return false;
            out = a * b;
            return std::isfinite(out);
        case RealKind::Div:
            if (!try_eval_ld(x->a, budget, a) ||
                !try_eval_ld(x->b, budget, b) || b == 0.0L) return false;
            out = a / b;
            return std::isfinite(out);
        case RealKind::Neg:
            if (!try_eval_ld(x->a, budget, a)) return false;
            out = -a;
            return std::isfinite(out);
    }
    return false;
}

std::string real_text(const RealPtr&x,int significant){
    if(!x)return "0.0";
    if(x->kind==RealKind::Rational)
        return rational_decimal(x->rational,significant);
    if(x->kind==RealKind::Atom){
        const auto decimal=exact_atom_decimal(x->provider,x->opcode);
        if(!decimal)exact_fail("exact-real provider atom is unavailable");
        const int sig=significant>0?significant:34;
        const auto dot=decimal->find('.');
        if(dot==std::string::npos)return *decimal;
        const std::size_t integer_digits=
            (!decimal->empty()&&((*decimal)[0]=='-'||(*decimal)[0]=='+'))
                ?dot-1:dot;
        const std::size_t keep_fraction=
            static_cast<std::size_t>(std::max(1,sig-static_cast<int>(integer_digits)));
        const auto keep=dot+1+keep_fraction;
        return keep<decimal->size()?decimal->substr(0,keep):*decimal;
    }
    const int display_digits=std::min(significant>0?significant:18,18);
    std::size_t budget=4096;
    const auto value=eval_ld(x,budget);
    if(!std::isfinite(value))
        exact_fail("symbolic bigreal expression is not a finite real");
    char buffer[160];
    std::snprintf(buffer,sizeof(buffer),"%.*Lg",display_digits,value);
    std::string out(buffer);
    if(out.find_first_of(".eE")==std::string::npos)out+=".0";
    return out;
}

int real_compare(const RealPtr&a,const RealPtr&b,
                 unsigned long long line,unsigned long long column){
    std::size_t budget=8192;
    if(structurally_equal(a,b,budget)){
        std::size_t defined_budget=8192;
        if(!provably_defined(a,defined_budget))
            exact_fail("bigreal comparison operand domain could not be proven",
                       line,column);
        return 0;
    }
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rat_cmp(a->rational,b->rational);
    const auto atom_vs_rational=[](const RealPtr& atom_value,
                                   const Rational& rational)->std::optional<int>{
        Rational low,high;
        if(!atom_bounds(atom_value,low,high))return std::nullopt;
        if(rat_cmp(high,rational)<0)return -1;
        if(rat_cmp(low,rational)>0)return 1;
        return std::nullopt;
    };
    if(a->kind==RealKind::Atom&&b->kind==RealKind::Rational){
        if(const auto compared=atom_vs_rational(a,b->rational))return *compared;
    }
    if(b->kind==RealKind::Atom&&a->kind==RealKind::Rational){
        if(const auto compared=atom_vs_rational(b,a->rational))return -*compared;
    }
    if(a->kind==RealKind::Atom&&b->kind==RealKind::Atom){
        Rational al,ah,bl,bh;
        if(atom_bounds(a,al,ah)&&atom_bounds(b,bl,bh)){
            if(rat_cmp(ah,bl)<0)return -1;
            if(rat_cmp(al,bh)>0)return 1;
        }
    }
    exact_fail("bigreal comparison could not be proven within the finite proof budget",
               line,column);
}

} // namespace

extern "C" int qcore_exact_real_provider_register(
    const char* provider,
    qcore_exact_real_atom_decimal_fn atom_decimal) {
    if(!provider||!*provider||!atom_decimal)return 0;
    try{
        const std::string name(provider);
        std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
        auto& providers=exact_atom_providers();
        const auto found=providers.find(name);
        if(found!=providers.end())
            return found->second==atom_decimal?1:0;
        providers.emplace(name,atom_decimal);
        return 1;
    }catch(...){
        return 0;
    }
}

extern "C" int qcore_exact_real_provider_register_unary(
    const char* provider,
    qcore_exact_real_unary_float64_fn evaluate,
    qcore_exact_real_unary_flags_fn flags) {
    if(!provider||!*provider||!evaluate||!flags)return 0;
    try{
        const std::string name(provider);
        std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
        auto& providers=exact_unary_providers();
        const auto found=providers.find(name);
        if(found!=providers.end())
            return found->second.evaluate==evaluate&&found->second.flags==flags?1:0;
        providers.emplace(name,ExactUnaryProvider{evaluate,flags});
        return 1;
    }catch(...){
        return 0;
    }
}

extern "C" void* qcore_exact_real_unary(
    const char* provider,std::uint32_t opcode,const void* input){
    if(!provider||!*provider||!input)
        exact_fail("invalid exact-real unary provider call");
    const std::string name(provider);
    const auto registered=exact_unary_provider(name);
    if(!registered||!registered->evaluate||!registered->flags)
        exact_fail("exact-real unary provider is unavailable");
    const auto flags=registered->flags(opcode);
    const auto source=br(const_cast<void*>(input))->value;
    std::size_t sign_budget=4096;
    const auto sign=proven_sign(source,sign_budget);
    if((flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE)&&
       (sign==ProvenSign::Negative||sign==ProvenSign::Zero||
        sign==ProvenSign::NonPositive))
        exact_fail("exact-real unary input is outside the provider domain");
    if((flags&QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE)&&
       sign==ProvenSign::Negative)
        exact_fail("exact-real unary input is outside the provider domain");
    return make_br(provider_unary(name,opcode,source));
}

extern "C" void* qcore_exact_real_atom(
    const char* provider,std::uint32_t opcode){
    if(!provider||!*provider)exact_fail("invalid exact-real provider");
    const std::string name(provider);
    Rational low,high;
    auto value=atom(name,opcode);
    if(!atom_bounds(value,low,high))
        exact_fail("exact-real provider or atom opcode is unavailable");
    return make_br(std::move(value));
}

extern "C" double qcore_exact_real_atom_float64(
    const char* provider,std::uint32_t opcode){
    if(!provider||!*provider)exact_fail("invalid exact-real provider");
    const auto decimal=exact_atom_decimal(provider,opcode);
    if(!decimal)exact_fail("exact-real provider or atom opcode is unavailable");
    try{
        const double value=std::stod(*decimal);
        if(!std::isfinite(value))
            exact_fail("exact-real provider atom is not finite");
        return value;
    }catch(...){
        exact_fail("exact-real provider atom cannot be observed as float");
    }
}

extern "C" void quidra_bigint_drop(void*p){
    if(p)bi(p)->~BigIntValue();
}
extern "C" void quidra_bigreal_drop(void*p){
    if(p)br(p)->~BigRealValue();
}

extern "C" void* quidra_bigint_literal(const char*text){
    bool ok=false;auto value=BigInt::parse(text?text:"",ok);
    if(!ok)exact_fail("invalid bigint literal");
    return make_bi(std::move(value));
}
extern "C" void* quidra_bigint_parse(const char*text){
    bool ok=false;auto value=BigInt::parse(text?text:"",ok);
    return ok?make_bi(std::move(value)):nullptr;
}
extern "C" void* quidra_bigreal_literal(const char*text){
    if(!text)exact_fail("invalid bigreal literal");
    Rational rational;
    if(!parse_decimal(text,rational))exact_fail("invalid bigreal literal");
    return make_br(rr(std::move(rational)));
}
extern "C" void* quidra_bigreal_parse(const char*text){
    Rational rational;
    if(!text||!parse_decimal(text,rational))return nullptr;
    return make_br(rr(std::move(rational)));
}
extern "C" char* quidra_bigint_text(void*p){
    if(!p)exact_fail("null bigint");
    return copy_text(bi(p)->value.text());
}
extern "C" char* quidra_bigreal_text(void*p,int significant){
    if(!p)exact_fail("null bigreal");
    return copy_text(real_text(br(p)->value,significant));
}

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
        case 1:return make_bi(add(a,b));
        case 2:return make_bi(sub(a,b));
        case 3:return make_bi(mul(a,b));
        case 4:return make_bi(divmod(a,b).first);
        case 5:return make_bi(divmod(a,b).second);
        default:exact_fail("invalid bigint operation",line,column);
    }
}
extern "C" void* quidra_bigint_pow(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigint power operand",line,column);
    auto base=bi(left)->value;
    auto exponent=bi(right)->value;
    if(exponent.sign<0)
        exact_fail("bigint exponent must be non-negative",line,column);
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

extern "C" void* quidra_bigreal_neg(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    return make_br(real_neg(br(p)->value));
}
extern "C" void* quidra_bigreal_binary(
    void*left,void*right,int operation,
    unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigreal operand",line,column);
    auto a=br(left)->value,b=br(right)->value;
    switch(operation){
        case 1:return make_br(real_add(std::move(a),std::move(b)));
        case 2:return make_br(real_sub(std::move(a),std::move(b)));
        case 3:return make_br(real_mul(std::move(a),std::move(b)));
        case 4:return make_br(real_div(std::move(a),std::move(b)));
        default:exact_fail("invalid bigreal operation",line,column);
    }
}
extern "C" int quidra_bigreal_compare(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigreal comparison",line,column);
    return real_compare(br(left)->value,br(right)->value,line,column);
}
extern "C" void* quidra_bigreal_pow(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null bigreal operand",line,column);
    auto base=br(left)->value,exponent=br(right)->value;
    if(exponent->kind==RealKind::Rational&&rat_int(exponent->rational)){
        const auto text=exponent->rational.num.text();
        char* end=nullptr;errno=0;
        const long long e=std::strtoll(text.c_str(),&end,10);
        if(errno!=ERANGE&&end&&*end=='\0'&&std::llabs(e)<=100000){
            RealPtr result=rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
            RealPtr factor=base;
            unsigned long long n=e<0
                ?static_cast<unsigned long long>(-(e+1))+1ULL
                :static_cast<unsigned long long>(e);
            while(n){
                if(n&1ULL)result=real_mul(result,factor);
                n>>=1ULL;
                if(n)factor=real_mul(factor,factor);
            }
            if(e<0)result=real_div(
                rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1))),result);
            return make_br(std::move(result));
        }
    }
    return make_br(binary(RealKind::Pow,std::move(base),std::move(exponent)));
}
extern "C" void* quidra_bigint_from_i64(long long value){
    return make_bi(BigInt::from_i64(value));
}
extern "C" void* quidra_bigint_from_u64(unsigned long long value){
    return make_bi(BigInt::from_u64(value));
}
extern "C" void* quidra_bigreal_from_i64(long long value){
    return make_br(rr(Rational(BigInt::from_i64(value),BigInt::from_u64(1))));
}
extern "C" void* quidra_bigreal_from_u64(unsigned long long value){
    return make_br(rr(Rational(BigInt::from_u64(value),BigInt::from_u64(1))));
}
extern "C" void* quidra_bigreal_from_float64(double value){
    return make_br(rr(exact_double(value)));
}
extern "C" void* quidra_bigreal_from_bigint(void*p){
    if(!p)exact_fail("null bigint");
    return make_br(rr(Rational(bi(p)->value,BigInt::from_u64(1))));
}
bool bigint_try_i64_value(const BigInt& source,int bits,long long& out){
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
bool bigint_try_u64_value(
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

extern "C" bool quidra_bigint_try_i64(void*p,int bits,long long*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0;
    return bigint_try_i64_value(bi(p)->value,bits,*out);
}
extern "C" bool quidra_bigint_try_u64(
    void*p,int bits,unsigned long long*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0;
    return bigint_try_u64_value(bi(p)->value,bits,*out);
}
extern "C" void* quidra_bigreal_try_bigint(void*p){
    if(!p)exact_fail("null bigreal");
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        return nullptr;
    return make_bi(value->rational.num);
}
extern "C" bool quidra_bigreal_try_i64(void*p,int bits,long long*out){
    if(!p||!out)exact_fail("null bigreal conversion storage");
    *out=0;
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        return false;
    return bigint_try_i64_value(value->rational.num,bits,*out);
}
extern "C" bool quidra_bigreal_try_u64(
    void*p,int bits,unsigned long long*out){
    if(!p||!out)exact_fail("null bigreal conversion storage");
    *out=0;
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        return false;
    return bigint_try_u64_value(value->rational.num,bits,*out);
}

extern "C" void* quidra_bigreal_to_bigint(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    auto* result=quidra_bigreal_try_bigint(p);
    if(!result)exact_fail("bigreal is not provably an integer",line,column);
    return result;
}

extern "C" long long quidra_bigint_to_i64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    long long value=0;
    if(!bigint_try_i64_value(bi(p)->value,bits,value))
        exact_fail("bigint is outside destination range",line,column);
    return value;
}
extern "C" unsigned long long quidra_bigint_to_u64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    if(bi(p)->value.sign<0)
        exact_fail("negative bigint is outside unsigned destination range",line,column);
    unsigned long long value=0;
    if(!bigint_try_u64_value(bi(p)->value,bits,value))
        exact_fail("bigint is outside destination range",line,column);
    return value;
}
extern "C" bool quidra_bigreal_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null bigreal conversion storage");
    *out=0.0;
    std::size_t budget=4096;
    long double value=0.0L;
    if(!try_eval_ld(br(p)->value,budget,value))return false;
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return false;
    *out=result;
    return true;
}
extern "C" bool quidra_bigreal_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null bigreal conversion storage");
    *out=0.0F;
    double value=0.0;
    if(!quidra_bigreal_try_float64(p,&value))return false;
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))return false;
    *out=result;
    return true;
}
extern "C" bool quidra_bigint_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0;
    long double value=0;
    try{value=std::stold(bi(p)->value.text());}
    catch(...){return false;}
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return false;
    *out=result;
    return true;
}
extern "C" bool quidra_bigint_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0F;
    double value=0.0;
    if(!quidra_bigint_try_float64(p,&value))return false;
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))return false;
    *out=result;
    return true;
}
extern "C" bool quidra_exact_numeric_cast_fits(
    void* p,int source_kind,int target_kind,int bits){
    if(!p)exact_fail("null exact numeric conversion storage");
    if(source_kind!=1&&source_kind!=2)return false;
    if(target_kind==1){
        if(source_kind==1)return true;
        const auto value=br(p)->value;
        return value->kind==RealKind::Rational&&rat_int(value->rational);
    }
    if(target_kind==2)return true;
    if(target_kind==3){
        long long out=0;
        if(source_kind==1)return bigint_try_i64_value(bi(p)->value,bits,out);
        const auto value=br(p)->value;
        return value->kind==RealKind::Rational&&rat_int(value->rational)&&
            bigint_try_i64_value(value->rational.num,bits,out);
    }
    if(target_kind==4){
        unsigned long long out=0;
        if(source_kind==1)return bigint_try_u64_value(bi(p)->value,bits,out);
        const auto value=br(p)->value;
        return value->kind==RealKind::Rational&&rat_int(value->rational)&&
            bigint_try_u64_value(value->rational.num,bits,out);
    }
    if(target_kind==5){
        float out=0.0F;
        return source_kind==1
            ? quidra_bigint_try_float32(p,&out)
            : quidra_bigreal_try_float32(p,&out);
    }
    if(target_kind==6){
        double out=0.0;
        return source_kind==1
            ? quidra_bigint_try_float64(p,&out)
            : quidra_bigreal_try_float64(p,&out);
    }
    return false;
}

extern "C" double quidra_bigreal_to_float64(
    void*p,unsigned long long line,unsigned long long column){
    double result=0.0;
    if(!quidra_bigreal_try_float64(p,&result))
        exact_fail("bigreal is outside float range",line,column);
    return result;
}
extern "C" float quidra_bigreal_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    float result=0.0F;
    if(!quidra_bigreal_try_float32(p,&result))
        exact_fail("bigreal is outside float32 range",line,column);
    return result;
}
extern "C" double quidra_bigint_to_float64(
    void*p,unsigned long long line,unsigned long long column){
    double result=0.0;
    if(!quidra_bigint_try_float64(p,&result))
        exact_fail("bigint is outside float range",line,column);
    return result;
}
extern "C" float quidra_bigint_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    float result=0.0F;
    if(!quidra_bigint_try_float32(p,&result))
        exact_fail("bigint is outside float32 range",line,column);
    return result;
}
