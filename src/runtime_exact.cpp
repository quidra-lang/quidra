#include "runtime_internal.hpp"

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
#include <new>
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
    if(!a.sign)return b;if(!b.sign)return a;
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
BigInt add_small(BigInt a,std::uint32_t value){
    if(!value)return a;
    if(a.sign<0)return add(a,BigInt::from_u64(value));
    if(!a.sign)return BigInt::from_u64(value);
    std::uint64_t carry=value;
    std::size_t i=0;
    while(carry&&i<a.limbs.size()){
        const auto total=static_cast<std::uint64_t>(a.limbs[i])+carry;
        a.limbs[i]=static_cast<std::uint32_t>(total%LIMB_BASE);
        carry=total/LIMB_BASE;++i;
    }
    if(carry)a.limbs.push_back(static_cast<std::uint32_t>(carry));
    return a;
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

// Decimal helpers are used only by the certified integer-square-root formatter.
std::string trim_dec(std::string value){
    const auto p=value.find_first_not_of('0');
    return p==std::string::npos?"0":value.substr(p);
}
int cmp_dec(const std::string&a0,const std::string&b0){
    const auto a=trim_dec(a0),b=trim_dec(b0);
    if(a.size()!=b.size())return a.size()<b.size()?-1:1;
    return a==b?0:(a<b?-1:1);
}
std::string add_dec(const std::string&a,const std::string&b){
    std::string out;int carry=0;std::size_t i=a.size(),j=b.size();
    while(i||j||carry){
        int v=carry;if(i)v+=a[--i]-'0';if(j)v+=b[--j]-'0';
        out.push_back(static_cast<char>('0'+v%10));carry=v/10;
    }
    std::reverse(out.begin(),out.end());return trim_dec(out);
}
std::string sub_dec(const std::string&a,const std::string&b){
    std::string out;int borrow=0;std::size_t i=a.size(),j=b.size();
    while(i){
        int v=(a[--i]-'0')-borrow-(j?b[--j]-'0':0);
        if(v<0){v+=10;borrow=1;}else borrow=0;
        out.push_back(static_cast<char>('0'+v));
    }
    while(out.size()>1&&out.back()=='0')out.pop_back();
    std::reverse(out.begin(),out.end());return trim_dec(out);
}
std::string mul_dec_digit(const std::string&a,int d){
    if(d==0||trim_dec(a)=="0")return "0";
    std::string out;int carry=0;
    for(std::size_t i=a.size();i;){
        int v=(a[--i]-'0')*d+carry;
        out.push_back(static_cast<char>('0'+v%10));carry=v/10;
    }
    while(carry){out.push_back(static_cast<char>('0'+carry%10));carry/=10;}
    std::reverse(out.begin(),out.end());return trim_dec(out);
}
std::string isqrt_decimal(std::string input){
    input=trim_dec(std::move(input));
    if(input=="0")return "0";
    if(input.size()%2)input="0"+input;
    std::string root="0",rem="0";
    for(std::size_t i=0;i<input.size();i+=2){
        const int pair=(input[i]-'0')*10+(input[i+1]-'0');
        rem=add_dec(mul_dec_digit(rem,100),std::to_string(pair));
        const auto twenty=mul_dec_digit(root,20);
        int chosen=0;std::string term="0";
        for(int d=9;d>=0;--d){
            auto candidate=mul_dec_digit(add_dec(twenty,std::to_string(d)),d);
            if(cmp_dec(candidate,rem)<=0){chosen=d;term=std::move(candidate);break;}
        }
        rem=sub_dec(rem,term);
        root=add_dec(mul_dec_digit(root,10),std::to_string(chosen));
    }
    return trim_dec(root);
}
bool perfect_square(const BigInt&value,BigInt&root){
    if(value.sign<0)return false;
    const auto r=isqrt_decimal(value.text());
    bool ok=false;auto candidate=BigInt::parse(r,ok);
    if(!ok||cmp(mul(candidate,candidate),value)!=0)return false;
    root=std::move(candidate);return true;
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
        if(s[0]=='-')sign=-1;s.erase(0,1);
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
    Rational,Pi,E,Sqrt,Sin,Cos,Tan,Log,Exp,Pow,Add,Sub,Mul,Div,Neg,Abs
};
struct Real;
using RealPtr=std::shared_ptr<Real>;
struct Real{
    RealKind kind{RealKind::Rational};
    Rational rational;
    RealPtr a,b;
};

std::unordered_map<std::string,std::weak_ptr<Real>> real_intern;

std::string real_key(const RealPtr&value,std::size_t&budget){
    if(!value)return "null";
    if(!budget--)exact_fail("bigreal canonicalization budget exhausted");
    if(value->kind==RealKind::Rational)
        return "q:"+value->rational.num.text()+"/"+value->rational.den.text();
    const auto tag=std::to_string(static_cast<int>(value->kind));
    return tag+"("+real_key(value->a,budget)+","+real_key(value->b,budget)+")";
}
RealPtr intern_real(RealPtr value){
    std::size_t budget=8192;
    const auto key=real_key(value,budget);
    if(const auto found=real_intern.find(key);found!=real_intern.end())
        if(auto existing=found->second.lock())return existing;
    real_intern[key]=value;
    return value;
}
RealPtr rr(Rational rational){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Rational;value->rational=std::move(rational);
    return intern_real(std::move(value));
}
RealPtr special(RealKind kind){
    auto value=std::make_shared<Real>();value->kind=kind;
    return intern_real(std::move(value));
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
bool structurally_equal(const RealPtr&a,const RealPtr&b,std::size_t&budget){
    if(a.get()==b.get())return true;
    if(!a||!b||!budget--)return false;
    if(a->kind!=b->kind)return false;
    if(a->kind==RealKind::Rational)return rat_cmp(a->rational,b->rational)==0;
    return structurally_equal(a->a,b->a,budget)&&
           structurally_equal(a->b,b->b,budget);
}

std::pair<BigInt,BigInt> extract_small_square(BigInt input){
    input=absbi(std::move(input));
    BigInt outside=BigInt::from_u64(1);
    BigInt residual=BigInt::from_u64(1);
    static constexpr std::array<std::uint32_t,25> primes{
        2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97};
    for(auto prime:primes){
        unsigned count=0;
        while(input.sign){
            auto qr=div_small(input,prime);
            if(qr.second)break;
            input=std::move(qr.first);++count;
        }
        for(unsigned i=0;i<count/2;++i)outside=mul_small(outside,prime);
        if(count%2)residual=mul_small(residual,prime);
    }
    residual=mul(residual,input);
    return {std::move(outside),std::move(residual)};
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
    if(is_zero(a))return b;if(is_zero(b))return a;
    return binary(RealKind::Add,std::move(a),std::move(b));
}
RealPtr real_sub(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_sub(a->rational,b->rational));
    if(is_zero(b))return a;
    std::size_t budget=4096;
    if(structurally_equal(a,b,budget))return rr({});
    return binary(RealKind::Sub,std::move(a),std::move(b));
}
bool sqrt_term(const RealPtr&x,Rational&coefficient,RealPtr&radicand){
    coefficient=Rational(BigInt::from_u64(1),BigInt::from_u64(1));
    if(x->kind==RealKind::Sqrt){radicand=x->a;return true;}
    if(x->kind==RealKind::Mul){
        if(x->a&&x->a->kind==RealKind::Rational&&
           x->b&&x->b->kind==RealKind::Sqrt){
            coefficient=x->a->rational;radicand=x->b->a;return true;
        }
        if(x->b&&x->b->kind==RealKind::Rational&&
           x->a&&x->a->kind==RealKind::Sqrt){
            coefficient=x->b->rational;radicand=x->a->a;return true;
        }
    }
    return false;
}

RealPtr real_mul(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_mul(a->rational,b->rational));
    if(is_zero(a)||is_zero(b))return rr({});
    if(is_one(a))return b;if(is_one(b))return a;
    Rational ca,cb;RealPtr ra,rb;
    if(sqrt_term(a,ca,ra)&&sqrt_term(b,cb,rb)){
        std::size_t budget=4096;
        if(structurally_equal(ra,rb,budget)){
            auto coefficient=rr(rat_mul(ca,cb));
            return real_mul(std::move(coefficient),std::move(ra));
        }
    }
    return binary(RealKind::Mul,std::move(a),std::move(b));
}
RealPtr real_div(RealPtr a,RealPtr b){
    if(is_zero(b))exact_fail("real division by zero");
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_div(a->rational,b->rational));
    if(is_zero(a))return rr({});
    if(is_one(b))return a;
    std::size_t budget=4096;
    if(structurally_equal(a,b,budget))
        return rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
    return binary(RealKind::Div,std::move(a),std::move(b));
}
RealPtr real_abs(RealPtr a){
    if(a->kind==RealKind::Rational){
        auto r=a->rational;r.num=absbi(std::move(r.num));return rr(std::move(r));
    }
    return unary(RealKind::Abs,std::move(a));
}
RealPtr simplify_sqrt(RealPtr x){
    if(!x)return x;
    if(x->kind==RealKind::Rational){
        if(x->rational.num.sign<0)
            exact_fail("sqrt is undefined for negative bigreal");
        BigInt rn,rd;
        if(perfect_square(absbi(x->rational.num),rn)&&
           perfect_square(x->rational.den,rd))
            return rr(Rational(std::move(rn),std::move(rd)));

        auto [on,rn2]=extract_small_square(x->rational.num);
        auto [od,rd2]=extract_small_square(x->rational.den);
        const bool extracted=
            cmp(on,BigInt::from_u64(1))!=0||cmp(od,BigInt::from_u64(1))!=0;
        if(extracted){
            auto outside=rr(Rational(std::move(on),std::move(od)));
            auto residual=rr(Rational(std::move(rn2),std::move(rd2)));
            return real_mul(std::move(outside),unary(RealKind::Sqrt,std::move(residual)));
        }
    }
    return unary(RealKind::Sqrt,std::move(x));
}
RealPtr real_math(RealKind kind,RealPtr a){
    if(kind==RealKind::Sin&&(is_zero(a)||a->kind==RealKind::Pi))return rr({});
    if(kind==RealKind::Cos&&is_zero(a))
        return rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
    if(kind==RealKind::Cos&&a->kind==RealKind::Pi)
        return rr(Rational(BigInt::from_i64(-1),BigInt::from_u64(1)));
    if(kind==RealKind::Tan&&(is_zero(a)||a->kind==RealKind::Pi))return rr({});
    if(kind==RealKind::Log&&is_one(a))return rr({});
    if(kind==RealKind::Log&&a->kind==RealKind::E)
        return rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
    if(kind==RealKind::Exp&&is_zero(a))
        return rr(Rational(BigInt::from_u64(1),BigInt::from_u64(1)));
    if(kind==RealKind::Exp&&is_one(a))return special(RealKind::E);
    if(kind==RealKind::Exp&&a->kind==RealKind::Log)return a->a;
    if(kind==RealKind::Log&&a->kind==RealKind::Exp)return a->a;
    if(kind==RealKind::Log&&a->kind==RealKind::Rational&&a->rational.num.sign<=0)
        exact_fail("log requires a positive bigreal");
    return unary(kind,std::move(a));
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

std::string sqrt_rational_decimal(const Rational&r,int significant){
    if(r.num.sign<0)exact_fail("sqrt is undefined for negative bigreal");
    if(!r.num.sign)return "0.0";
    const int sig=significant>0?significant:34;
    const auto whole=divmod(r.num,r.den).first;
    const auto whole_root=isqrt_decimal(whole.text());
    const int integer_digits=whole_root=="0"?1:static_cast<int>(whole_root.size());
    const int fraction_digits=std::max(1,sig-integer_digits)+2;
    auto scaled=mul(r.num,pow10(static_cast<std::size_t>(2*fraction_digits)));
    const auto quotient=divmod(scaled,r.den).first;
    auto root=isqrt_decimal(quotient.text());
    if(static_cast<int>(root.size())<=fraction_digits)
        root=std::string(static_cast<std::size_t>(fraction_digits+1-root.size()),'0')+root;
    std::string integer=root.substr(
        0,root.size()-static_cast<std::size_t>(fraction_digits));
    std::string fraction=root.substr(
        root.size()-static_cast<std::size_t>(fraction_digits));
    const int keep=std::max(1,sig-static_cast<int>(integer=="0"?0:integer.size()));
    if(static_cast<int>(fraction.size())>keep){
        const bool up=fraction[static_cast<std::size_t>(keep)]>='5';
        fraction.resize(static_cast<std::size_t>(keep));
        if(up){
            int i=static_cast<int>(fraction.size())-1;
            while(i>=0&&fraction[static_cast<std::size_t>(i)]=='9'){
                fraction[static_cast<std::size_t>(i)]='0';--i;
            }
            if(i>=0)++fraction[static_cast<std::size_t>(i)];
            else{
                bool ok=false;auto w=BigInt::parse(integer,ok);
                integer=add(w,BigInt::from_u64(1)).text();
            }
        }
    }
    while(fraction.size()>1&&fraction.back()=='0')fraction.pop_back();
    return integer+"."+fraction;
}

const std::string PI_DIGITS=
"3.14159265358979323846264338327950288419716939937510582097494459230781640628620899862803482534211706798214808651328230664709384460955058223172535940812848111745028410270193852";
const std::string E_DIGITS=
"2.71828182845904523536028747135266249775724709369995957496696762772407663035354759457138217852516642742746639193200305992181741359662904357290033429526059563073813232862794349";

std::string constant_text(const std::string&digits,int significant){
    const int sig=significant>0?significant:34;
    const auto dot=digits.find('.');
    if(dot==std::string::npos)return digits;
    const int keep=std::max(1,sig-1);
    if(dot+1+static_cast<std::size_t>(keep)>=digits.size())return digits;
    return digits.substr(0,dot+1+static_cast<std::size_t>(keep));
}
Rational constant_bound(const std::string&digits,bool upper){
    std::string raw=digits;
    const auto dot=raw.find('.');
    const std::size_t fraction=raw.size()-dot-1;
    raw.erase(dot,1);
    bool ok=false;auto n=BigInt::parse(raw,ok);
    if(!ok)exact_fail("internal constant bound failure");
    auto d=pow10(fraction);
    if(upper)n=add(n,BigInt::from_u64(1));
    return Rational(std::move(n),std::move(d));
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
        case RealKind::Pi:return std::acos(-1.0L);
        case RealKind::E:return std::exp(1.0L);
        case RealKind::Sqrt:return std::sqrt(eval_ld(x->a,budget));
        case RealKind::Sin:return std::sin(eval_ld(x->a,budget));
        case RealKind::Cos:return std::cos(eval_ld(x->a,budget));
        case RealKind::Tan:return std::tan(eval_ld(x->a,budget));
        case RealKind::Log:return std::log(eval_ld(x->a,budget));
        case RealKind::Exp:return std::exp(eval_ld(x->a,budget));
        case RealKind::Pow:return std::pow(eval_ld(x->a,budget),eval_ld(x->b,budget));
        case RealKind::Add:return eval_ld(x->a,budget)+eval_ld(x->b,budget);
        case RealKind::Sub:return eval_ld(x->a,budget)-eval_ld(x->b,budget);
        case RealKind::Mul:return eval_ld(x->a,budget)*eval_ld(x->b,budget);
        case RealKind::Div:return eval_ld(x->a,budget)/eval_ld(x->b,budget);
        case RealKind::Neg:return -eval_ld(x->a,budget);
        case RealKind::Abs:return std::fabs(eval_ld(x->a,budget));
    }
    return 0;
}

std::string real_text(const RealPtr&x,int significant){
    if(!x)return "0.0";
    if(x->kind==RealKind::Rational)
        return rational_decimal(x->rational,significant);
    if(x->kind==RealKind::Pi)return constant_text(PI_DIGITS,significant);
    if(x->kind==RealKind::E)return constant_text(E_DIGITS,significant);
    if(x->kind==RealKind::Sqrt&&x->a&&x->a->kind==RealKind::Rational)
        return sqrt_rational_decimal(x->a->rational,significant);
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

int compare_constant_to_rational(RealKind kind,const Rational&r){
    const auto&digits=kind==RealKind::Pi?PI_DIGITS:E_DIGITS;
    const auto low=constant_bound(digits,false);
    const auto high=constant_bound(digits,true);
    if(rat_cmp(high,r)<0)return -1;
    if(rat_cmp(low,r)>0)return 1;
    exact_fail("bigreal comparison needs more certified constant digits");
}
int real_compare(const RealPtr&a,const RealPtr&b,
                 unsigned long long line,unsigned long long column){
    std::size_t budget=8192;
    if(structurally_equal(a,b,budget))return 0;
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rat_cmp(a->rational,b->rational);
    if((a->kind==RealKind::Pi||a->kind==RealKind::E)&&
       b->kind==RealKind::Rational)
        return compare_constant_to_rational(a->kind,b->rational);
    if((b->kind==RealKind::Pi||b->kind==RealKind::E)&&
       a->kind==RealKind::Rational)
        return -compare_constant_to_rational(b->kind,a->rational);
    if(a->kind==RealKind::Pi&&b->kind==RealKind::E)return 1;
    if(a->kind==RealKind::E&&b->kind==RealKind::Pi)return -1;
    if(a->kind==RealKind::Sqrt&&a->a&&a->a->kind==RealKind::Rational&&
       b->kind==RealKind::Rational&&b->rational.num.sign>=0)
        return rat_cmp(a->a->rational,rat_mul(b->rational,b->rational));
    if(b->kind==RealKind::Sqrt&&b->a&&b->a->kind==RealKind::Rational&&
       a->kind==RealKind::Rational&&a->rational.num.sign>=0)
        return -rat_cmp(b->a->rational,rat_mul(a->rational,a->rational));
    exact_fail("bigreal comparison could not be proven within the finite proof budget",
               line,column);
}

BigInt round_real(const RealPtr&x,int operation,
                  unsigned long long line,unsigned long long column){
    if(!x||x->kind!=RealKind::Rational)
        exact_fail("bigreal rounding could not be proven exactly",line,column);
    const auto&r=x->rational;
    auto qr=divmod(r.num,r.den);
    auto q=std::move(qr.first);auto rem=std::move(qr.second);
    if(!rem.sign)return q;
    // 1=trunc, 2=round half away from zero, 3=floor, 4=ceil
    if(operation==1)return q;
    if(operation==3&&r.num.sign<0)return sub(q,BigInt::from_u64(1));
    if(operation==4&&r.num.sign>0)return add(q,BigInt::from_u64(1));
    if(operation==2){
        const auto twice=mul_small(absbi(rem),2);
        if(cmp(twice,r.den)>=0)
            return r.num.sign<0?sub(q,BigInt::from_u64(1))
                               :add(q,BigInt::from_u64(1));
    }
    return q;
}

} // namespace

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
    if(std::strcmp(text,"$pi")==0)return make_br(special(RealKind::Pi));
    if(std::strcmp(text,"$e")==0)return make_br(special(RealKind::E));
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
extern "C" void* quidra_bigint_abs(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    return make_bi(absbi(bi(p)->value));
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
extern "C" int quidra_bigint_compare(void*left,void*right){
    if(!left||!right)exact_fail("null bigint comparison");
    return cmp(bi(left)->value,bi(right)->value);
}

extern "C" void* quidra_bigreal_neg(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    return make_br(real_neg(br(p)->value));
}
extern "C" void* quidra_bigreal_abs(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    return make_br(real_abs(br(p)->value));
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
extern "C" void* quidra_bigreal_sqrt(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    return make_br(simplify_sqrt(br(p)->value));
}
extern "C" void* quidra_bigreal_math_unary(
    void*p,int operation,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    RealKind kind=RealKind::Sin;
    switch(operation){
        case 1:kind=RealKind::Sin;break;
        case 2:kind=RealKind::Cos;break;
        case 3:kind=RealKind::Tan;break;
        case 4:kind=RealKind::Log;break;
        case 5:kind=RealKind::Exp;break;
        default:exact_fail("invalid bigreal math operation",line,column);
    }
    return make_br(real_math(kind,br(p)->value));
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
extern "C" void* quidra_bigreal_round(
    void*p,int operation,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    return make_bi(round_real(br(p)->value,operation,line,column));
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
extern "C" void* quidra_bigreal_to_bigint(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    const auto value=br(p)->value;
    if(value->kind!=RealKind::Rational||!rat_int(value->rational))
        exact_fail("bigreal is not provably an integer",line,column);
    return make_bi(value->rational.num);
}

extern "C" long long quidra_bigint_to_i64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    const auto text=bi(p)->value.text();
    char* end=nullptr;errno=0;
    const auto value=std::strtoll(text.c_str(),&end,10);
    if(errno==ERANGE||!end||*end!='\0')
        exact_fail("bigint is outside destination range",line,column);
    if(bits<64){
        const long long low=-(1LL<<(bits-1));
        const long long high=(1LL<<(bits-1))-1;
        if(value<low||value>high)
            exact_fail("bigint is outside destination range",line,column);
    }
    return value;
}
extern "C" unsigned long long quidra_bigint_to_u64(
    void*p,int bits,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    const auto&value=bi(p)->value;
    if(value.sign<0)
        exact_fail("negative bigint is outside unsigned destination range",line,column);
    const auto text=value.text();
    char* end=nullptr;errno=0;
    const auto result=std::strtoull(text.c_str(),&end,10);
    if(errno==ERANGE||!end||*end!='\0')
        exact_fail("bigint is outside destination range",line,column);
    if(bits<64&&result>((1ULL<<bits)-1ULL))
        exact_fail("bigint is outside destination range",line,column);
    return result;
}
extern "C" double quidra_bigreal_to_float64(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigreal",line,column);
    std::size_t budget=4096;
    const auto value=eval_ld(br(p)->value,budget);
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))
        exact_fail("bigreal is outside float range",line,column);
    return result;
}
extern "C" float quidra_bigreal_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    const double value=quidra_bigreal_to_float64(p,line,column);
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))
        exact_fail("bigreal is outside float32 range",line,column);
    return result;
}
extern "C" double quidra_bigint_to_float64(
    void*p,unsigned long long line,unsigned long long column){
    if(!p)exact_fail("null bigint",line,column);
    long double value=0;
    try{value=std::stold(bi(p)->value.text());}
    catch(...){exact_fail("bigint is outside float range",line,column);}
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))
        exact_fail("bigint is outside float range",line,column);
    return result;
}
extern "C" float quidra_bigint_to_float32(
    void*p,unsigned long long line,unsigned long long column){
    const double value=quidra_bigint_to_float64(p,line,column);
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))
        exact_fail("bigint is outside float32 range",line,column);
    return result;
}
