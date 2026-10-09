#include "runtime_integer.hpp"
#include "runtime_parallel.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
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

// The ABI this runtime shares with generated code (include/quidra/abi).
namespace abi = quidra::abi;

using namespace quidra::runtime_integer;

namespace {

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
    if(!b.num.sign)exact_division_by_zero();
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
        exact_fail("real cannot contain NaN or infinity");
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
    // The node's place in creation order within its thread's table. Add
    // and Mul put the earlier operand first, so equal sums and products are
    // one node whichever operand order built them.
    std::uint64_t serial{};
    std::size_t hash{};
};

// Nodes are interned by structure (hash-consing): operands are interned
// before the nodes that use them, so two nodes are equal exactly when their
// kind, scalar fields and operand nodes are the same. The tables are
// thread-local and hold weak references; expired entries are swept as the
// table is used.
thread_local std::unordered_multimap<std::size_t,std::weak_ptr<Real>> real_intern;
thread_local std::uint64_t real_serial = 0;
thread_local std::size_t real_intern_operations = 0;

void hash_combine(std::size_t& seed,std::size_t value){
    seed^=value+0x9e3779b97f4a7c15ULL+(seed<<6U)+(seed>>2U);
}
void hash_bigint(std::size_t& seed,const BigInt& value){
    hash_combine(seed,std::hash<int>{}(value.sign));
    for(const auto limb:value.limbs)hash_combine(seed,std::hash<std::uint32_t>{}(limb));
}
std::size_t shallow_hash(const Real& value){
    std::size_t seed=std::hash<int>{}(static_cast<int>(value.kind));
    hash_bigint(seed,value.rational.num);
    hash_bigint(seed,value.rational.den);
    hash_combine(seed,std::hash<std::string>{}(value.provider));
    hash_combine(seed,std::hash<std::uint32_t>{}(value.opcode));
    hash_combine(seed,std::hash<const Real*>{}(value.a.get()));
    hash_combine(seed,std::hash<const Real*>{}(value.b.get()));
    return seed;
}
bool same_bigint(const BigInt& left,const BigInt& right){
    return left.sign==right.sign&&left.limbs==right.limbs;
}
bool shallow_equal(const Real& left,const Real& right){
    return left.kind==right.kind&&
           same_bigint(left.rational.num,right.rational.num)&&
           same_bigint(left.rational.den,right.rational.den)&&
           left.provider==right.provider&&left.opcode==right.opcode&&
           left.a.get()==right.a.get()&&left.b.get()==right.b.get();
}
RealPtr intern_real(RealPtr value){
    value->hash=shallow_hash(*value);
    const auto range=real_intern.equal_range(value->hash);
    for(auto it=range.first;it!=range.second;){
        if(auto existing=it->second.lock()){
            if(shallow_equal(*existing,*value))return existing;
            ++it;
        }else{
            it=real_intern.erase(it);
        }
    }
    value->serial=++real_serial;
    real_intern.emplace(value->hash,value);
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
    if((kind==RealKind::Add||kind==RealKind::Mul)&&b->serial<a->serial)
        std::swap(a,b);
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
    if(is_zero(b))exact_division_by_zero();
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
struct BigRealValue{RealPtr value;};
BigRealValue* br(void*p){return static_cast<BigRealValue*>(p);}
void* make_br(RealPtr value){
    return managed_new<BigRealValue>(BigRealValue{std::move(value)});
}

std::string rational_decimal(const Rational&r,int significant){
    const int sig=significant>0?significant:abi::exact_real_display_digits;
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
    if(!x||!budget--)exact_unproven(abi::FailureReason::evaluation_budget_exhausted);
    switch(x->kind){
        case RealKind::Rational:{
            try{
                const long double n=std::stold(x->rational.num.text());
                const long double d=std::stold(x->rational.den.text());
                return n/d;
            }catch(...){exact_fail("real magnitude exceeds display evaluator range");}
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

// A rational observed as a long double: numerator and denominator are read
// from their decimal text and divided once.
bool rational_ld(const Rational& rational, long double& out) {
    try {
        const long double n = std::stold(rational.num.text());
        const long double d = std::stold(rational.den.text());
        if (d == 0.0L) return false;
        out = n / d;
    } catch (...) {
        return false;
    }
    return std::isfinite(out);
}

bool try_eval_ld(const RealPtr& x, std::size_t& budget, long double& out) {
    if (!x || !budget--) return false;
    long double a = 0.0L;
    long double b = 0.0L;
    switch (x->kind) {
        case RealKind::Rational:
            return rational_ld(x->rational, out);
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
        const int sig=significant>0?significant:abi::exact_real_display_digits;
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
        exact_fail("symbolic real expression is not a finite real");
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
            exact_unproven(abi::FailureReason::domain_unproven,line,column);
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
    exact_unproven(abi::FailureReason::comparison_unproven,line,column);
}

} // namespace

extern "C" int qcore_exact_real_provider_register(
    const char* provider,
    qcore_exact_real_atom_decimal_fn atom_decimal) {
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
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
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
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
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
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
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
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
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!provider||!*provider)exact_fail("invalid exact-real provider");
    const auto decimal=exact_atom_decimal(provider,opcode);
    if(!decimal)exact_fail("exact-real provider or atom opcode is unavailable");
    try{
        const double value=std::stod(*decimal);
        if(!std::isfinite(value))
            exact_fail("exact-real provider atom is not finite");
        return value;
    }catch(...){
        exact_fail("exact-real provider atom cannot be observed as real64");
    }
}
extern "C" void quidra_bigreal_drop(void*p){
    if(p)br(p)->~BigRealValue();
}
extern "C" void* quidra_bigreal_literal(const char*text){
    if(!text)exact_fail("invalid real literal");
    Rational rational;
    if(!parse_decimal(text,rational))exact_fail("invalid real literal");
    return make_br(rr(std::move(rational)));
}
extern "C" void* quidra_bigreal_parse(const char*text){
    Rational rational;
    if(!text||!parse_decimal(text,rational))return nullptr;
    return make_br(rr(std::move(rational)));
}
extern "C" char* quidra_bigreal_text(void*p,int significant){
    if(!p)exact_fail("null real");
    return copy_text(real_text(br(p)->value,significant));
}

extern "C" void* quidra_bigreal_neg(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null real",line,column);
    return make_br(real_neg(br(p)->value));
}
extern "C" void* quidra_bigreal_binary(
    void*left,void*right,int operation,
    unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null real operand",line,column);
    auto a=br(left)->value,b=br(right)->value;
    switch(operation){
        case abi::exact_binary_opcode::add: return make_br(real_add(std::move(a),std::move(b)));
        case abi::exact_binary_opcode::subtract: return make_br(real_sub(std::move(a),std::move(b)));
        case abi::exact_binary_opcode::multiply: return make_br(real_mul(std::move(a),std::move(b)));
        case abi::exact_binary_opcode::divide: return make_br(real_div(std::move(a),std::move(b)));
        default:exact_fail("invalid real operation",line,column);
    }
}
extern "C" int quidra_bigreal_compare(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null real comparison",line,column);
    return real_compare(br(left)->value,br(right)->value,line,column);
}
extern "C" void* quidra_bigreal_pow(
    void*left,void*right,unsigned long long line,unsigned long long column){
    if(!left||!right)exact_fail("null real operand",line,column);
    auto base=br(left)->value,exponent=br(right)->value;
    // 0 ^ e is undefined for e <= 0 (a zero base is decided when it is a
    // rational, the form every exact zero value takes).
    if(base->kind==RealKind::Rational&&base->rational.num.sign==0&&
       exponent->kind==RealKind::Rational&&exponent->rational.num.sign<=0)
        quidra::runtime::report_failure(
            exponent->rational.num.sign==0?abi::FailureReason::zero_power_zero
                                          :abi::FailureReason::zero_power_negative,
            {},line,column);
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
// Why a real is not an integer: a rational that is not one is decided
// (NOT_INTEGRAL); a symbolic value is not decided (UNDECIDED).
static abi::ConversionReason real_integer_reason(const RealPtr& value){
    return value->kind==RealKind::Rational?abi::ConversionReason::not_integral
                                          :abi::ConversionReason::undecided;
}

extern "C" void* quidra_bigreal_try_bigint(void*p){
    if(!p)exact_fail("null real");
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational)){
        exact_conversion_declined(real_integer_reason(value));
        return nullptr;
    }
    return make_bi(value->rational.num);
}
extern "C" bool quidra_bigreal_try_i64(void*p,int bits,long long*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0;
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        return exact_conversion_declined(real_integer_reason(value));
    if(!bigint_try_i64_value(value->rational.num,bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}
extern "C" bool quidra_bigreal_try_u64(
    void*p,int bits,unsigned long long*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0;
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        return exact_conversion_declined(real_integer_reason(value));
    if(!bigint_try_u64_value(value->rational.num,bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}

extern "C" void* quidra_bigreal_to_bigint(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null real",line,column);
    auto* result=quidra_bigreal_try_bigint(p);
    if(!result)
        exact_conversion_fail(quidra::runtime::last_conversion_reason(),
                              abi::conversion_type_integer,line,column);
    return result;
}
// A real converts to real64 when it evaluates within the budget to a finite
// value there. A rational that does not is outside the range (OUT_OF_RANGE);
// a symbolic value that does not evaluate is not decided (UNDECIDED).
extern "C" bool quidra_bigreal_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0.0;
    std::size_t budget=4096;
    long double value=0.0L;
    const auto& real=br(p)->value;
    if(!try_eval_ld(real,budget,value))
        return exact_conversion_declined(real->kind==RealKind::Rational
                                             ?abi::ConversionReason::out_of_range
                                             :abi::ConversionReason::undecided);
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" bool quidra_bigreal_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0.0F;
    double value=0.0;
    if(!quidra_bigreal_try_float64(p,&value))return false;
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
// Whether an exact array element converts (an array cast's validator); a
// declined element records why, as the try-conversions do.
extern "C" bool quidra_exact_numeric_cast_fits(
    void* p,int source_kind,int target_kind,int bits){
    if(!p)exact_fail("null exact numeric conversion storage");
    if(source_kind!=1&&source_kind!=2)return false;
    if(target_kind==1){
        if(source_kind==1)return true;
        const auto value=br(p)->value;
        if(value->kind==RealKind::Rational&&rat_int(value->rational))return true;
        return exact_conversion_declined(real_integer_reason(value));
    }
    if(target_kind==7){
        if(source_kind==1)return bi(p)->value.sign>=0;
        const auto value=br(p)->value;
        return value->kind==RealKind::Rational&&rat_int(value->rational)&&
            value->rational.num.sign>=0;
    }
    if(target_kind==2)return true;
    if(target_kind==3){
        long long out=0;
        if(source_kind==1)return quidra_bigint_try_i64(p,bits,&out);
        return quidra_bigreal_try_i64(p,bits,&out);
    }
    if(target_kind==4){
        unsigned long long out=0;
        if(source_kind==1)return quidra_bigint_try_u64(p,bits,&out);
        return quidra_bigreal_try_u64(p,bits,&out);
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
        exact_conversion_fail(quidra::runtime::last_conversion_reason(),QCORE_DTYPE_FLOAT64,
                              line,column);
    return result;
}
extern "C" float quidra_bigreal_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    float result=0.0F;
    if(!quidra_bigreal_try_float32(p,&result))
        exact_conversion_fail(quidra::runtime::last_conversion_reason(),QCORE_DTYPE_FLOAT32,
                              line,column);
    return result;
}

// Small rationals (abi::ExactRealValue with a nonzero denominator). The
// functions below serve generated code's fast paths: they never allocate a
// managed value, and a small result is written in canonical form. A
// general value (denominator 0) holds a managed node, which the functions
// above operate on; quidra_real_promote and quidra_real_demote convert
// between the two forms.
namespace {

constexpr long long small_numerator_min=std::numeric_limits<long long>::min()+1;

long long small_gcd(long long a,long long b){
    auto x=static_cast<unsigned long long>(a<0?-a:a);
    auto y=static_cast<unsigned long long>(b<0?-b:b);
    while(y){const auto t=x%y;x=y;y=t;}
    return static_cast<long long>(x);
}
bool small_add(long long a,long long b,long long& out){
    constexpr auto min=std::numeric_limits<long long>::min();
    constexpr auto max=std::numeric_limits<long long>::max();
    if((b>0&&a>max-b)||(b<0&&a<min-b))return false;
    out=a+b;
    return true;
}
bool small_mul(long long a,long long b,long long& out){
    constexpr auto min=std::numeric_limits<long long>::min();
    constexpr auto max=std::numeric_limits<long long>::max();
    if(a==0||b==0){out=0;return true;}
    if(a>0){
        if((b>0&&a>max/b)||(b<0&&b<min/a))return false;
    }else{
        if((b>0&&a<min/b)||(b<0&&a<max/b))return false;
    }
    out=a*b;
    return true;
}
// n/d with d > 0 in lowest terms, or false when it is not small.
bool small_result(long long n,long long d,abi::ExactRealValue* out){
    if(d<=0||n<small_numerator_min)return false;
    if(n==0){out->numerator=0;out->denominator=1;return true;}
    const auto g=small_gcd(n,d);
    out->numerator=n/g;
    out->denominator=d/g;
    return true;
}
Rational small_rational(long long n,long long d){
    return Rational(BigInt::from_i64(n),BigInt::from_i64(d));
}
// A rational in small form, if it has one.
bool rational_small(const Rational& rational,abi::ExactRealValue* out){
    long long n=0,d=0;
    if(!bigint_try_i64_value(rational.num,64,n)||n<small_numerator_min)return false;
    if(!bigint_try_i64_value(rational.den,64,d)||d<=0)return false;
    out->numerator=n;
    out->denominator=d;
    return true;
}
void* real_drop_callback(){
    return reinterpret_cast<void*>(&quidra_bigreal_drop);
}

} // namespace

extern "C" void* quidra_real_promote(long long numerator,long long denominator){
    if(denominator==0){
        auto* handle=reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator));
        if(!handle)exact_fail("null real");
        quidra_managed_retain(handle);
        return handle;
    }
    return make_br(rr(small_rational(numerator,denominator)));
}
extern "C" void quidra_real_demote(void* handle,abi::ExactRealValue* out){
    if(!handle||!out)exact_fail("null real");
    const auto& value=br(handle)->value;
    if(value->kind==RealKind::Rational&&rational_small(value->rational,out)){
        quidra_managed_release(handle,real_drop_callback());
        return;
    }
    out->numerator=static_cast<long long>(reinterpret_cast<std::uintptr_t>(handle));
    out->denominator=0;
}
extern "C" bool quidra_real_small_binary(
    long long left_numerator,long long left_denominator,
    long long right_numerator,long long right_denominator,
    int operation,abi::ExactRealValue* out){
    if(!out)exact_fail("null real");
    if(left_denominator<=0||right_denominator<=0)return false;
    switch(operation){
        case abi::exact_binary_opcode::add:
        case abi::exact_binary_opcode::subtract:{
            long long right=right_numerator;
            if(operation==abi::exact_binary_opcode::subtract)right=-right;
            const auto g=small_gcd(left_denominator,right_denominator);
            const auto left_scale=right_denominator/g;
            const auto right_scale=left_denominator/g;
            long long denominator=0,left_part=0,right_part=0,numerator=0;
            if(!small_mul(left_denominator,left_scale,denominator))return false;
            if(!small_mul(left_numerator,left_scale,left_part))return false;
            if(!small_mul(right,right_scale,right_part))return false;
            if(!small_add(left_part,right_part,numerator))return false;
            return small_result(numerator,denominator,out);
        }
        case abi::exact_binary_opcode::multiply:
        case abi::exact_binary_opcode::divide:{
            long long right_n=right_numerator,right_d=right_denominator;
            if(operation==abi::exact_binary_opcode::divide){
                // Division by zero takes the general path, which reports it.
                if(right_numerator==0)return false;
                right_n=right_numerator<0?-right_denominator:right_denominator;
                right_d=right_numerator<0?-right_numerator:right_numerator;
            }
            const auto g1=left_numerator==0?1:small_gcd(left_numerator,right_d);
            const auto g2=right_n==0?1:small_gcd(right_n,left_denominator);
            long long numerator=0,denominator=0;
            if(!small_mul(left_numerator/g1,right_n/g2,numerator))return false;
            if(!small_mul(left_denominator/g2,right_d/g1,denominator))return false;
            return small_result(numerator,denominator,out);
        }
        default:
            return false;
    }
}
extern "C" char* quidra_real_small_text(
    long long numerator,long long denominator,int significant){
    return copy_text(rational_decimal(small_rational(numerator,denominator),significant));
}
// A small rational is decided: it is an integer exactly when its
// denominator is 1 (else NOT_INTEGRAL), and it converts to a fixed-width
// type unless it is outside its range.
extern "C" bool quidra_real_small_try_float64(
    long long numerator,long long denominator,double* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0.0;
    long double value=0.0L;
    if(!rational_ld(small_rational(numerator,denominator),value))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" bool quidra_real_small_try_float32(
    long long numerator,long long denominator,float* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0.0F;
    double value=0.0;
    if(!quidra_real_small_try_float64(numerator,denominator,&value))return false;
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" double quidra_real_small_to_float64(
    long long numerator,long long denominator,
    unsigned long long line,unsigned long long column){
    double result=0.0;
    if(!quidra_real_small_try_float64(numerator,denominator,&result))
        exact_conversion_fail(abi::ConversionReason::out_of_range,QCORE_DTYPE_FLOAT64,line,column);
    return result;
}
extern "C" float quidra_real_small_to_float32(
    long long numerator,long long denominator,
    unsigned long long line,unsigned long long column){
    float result=0.0F;
    if(!quidra_real_small_try_float32(numerator,denominator,&result))
        exact_conversion_fail(abi::ConversionReason::out_of_range,QCORE_DTYPE_FLOAT32,line,column);
    return result;
}
extern "C" bool quidra_real_small_try_i64(
    long long numerator,long long denominator,int bits,long long* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0;
    if(denominator!=1)return exact_conversion_declined(abi::ConversionReason::not_integral);
    if(!bigint_try_i64_value(BigInt::from_i64(numerator),bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}
extern "C" bool quidra_real_small_try_u64(
    long long numerator,long long denominator,int bits,unsigned long long* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0;
    if(denominator!=1)return exact_conversion_declined(abi::ConversionReason::not_integral);
    if(!bigint_try_u64_value(BigInt::from_i64(numerator),bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}
extern "C" void* quidra_real_small_try_bigint(long long numerator,long long denominator){
    if(denominator!=1){
        exact_conversion_declined(abi::ConversionReason::not_integral);
        return nullptr;
    }
    return make_bi(BigInt::from_i64(numerator));
}
extern "C" void* quidra_real_small_to_bigint(
    long long numerator,long long denominator,
    unsigned long long line,unsigned long long column){
    auto* result=quidra_real_small_try_bigint(numerator,denominator);
    if(!result)
        exact_conversion_fail(abi::ConversionReason::not_integral,abi::conversion_type_integer,
                              line,column);
    return result;
}
extern "C" bool quidra_real_small_cast_fits(
    long long numerator,long long denominator,int target_kind,int bits){
    if(target_kind==1)
        return denominator==1||exact_conversion_declined(abi::ConversionReason::not_integral);
    if(target_kind==7){
        if(denominator!=1)return exact_conversion_declined(abi::ConversionReason::not_integral);
        return numerator>=0||exact_conversion_declined(abi::ConversionReason::out_of_range);
    }
    if(target_kind==2)return true;
    if(target_kind==3){
        long long out=0;
        return quidra_real_small_try_i64(numerator,denominator,bits,&out);
    }
    if(target_kind==4){
        unsigned long long out=0;
        return quidra_real_small_try_u64(numerator,denominator,bits,&out);
    }
    if(target_kind==5){
        float out=0.0F;
        return quidra_real_small_try_float32(numerator,denominator,&out);
    }
    if(target_kind==6){
        double out=0.0;
        return quidra_real_small_try_float64(numerator,denominator,&out);
    }
    return false;
}
