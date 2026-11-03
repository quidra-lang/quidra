// Exact real providers, expressions, proofs and public real entry points.
// Included only by runtime_exact.cpp; not a separate translation unit.
#ifndef QUIDRA_EXACT_SECTION
#error "exact-runtime fragment requires its aggregate translation unit"
#endif

#if QUIDRA_EXACT_SECTION == 1
inline constexpr std::uint32_t LIMB_BASE=1000000000U;
inline constexpr int LIMB_DIGITS=9;


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
// Version-2 providers, by name (the registered struct, copied).
std::unordered_map<std::string,qcore_exact_provider_v2>& exact_providers_v2(){
    static std::unordered_map<std::string,qcore_exact_provider_v2> providers;
    return providers;
}
std::optional<qcore_exact_provider_v2> exact_provider_v2(const std::string& provider){
    std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
    const auto found=exact_providers_v2().find(provider);
    if(found==exact_providers_v2().end())return std::nullopt;
    return found->second;
}
struct ExactPowerProvider {
    std::string name;
    std::uint32_t opcode{};
};
std::optional<ExactPowerProvider>& exact_power_provider_slot(){
    static std::optional<ExactPowerProvider> provider;
    return provider;
}
std::optional<ExactPowerProvider> exact_power_provider(){
    std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
    return exact_power_provider_slot();
}

// Whether a version-2 provider owns an opcode of this arity.
bool v2_owns(const qcore_exact_provider_v2& provider,std::uint32_t opcode,std::size_t arity){
    const char* symbol=provider.spelling(opcode);
    return symbol&&*symbol&&provider.arity(opcode)==arity;
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


#endif

#if QUIDRA_EXACT_SECTION == 3
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
        exact_fail("real cannot contain NaN");
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

// Node kinds. Every value is in one normal form (generic normalization,
// Core-owned), so equal normal forms are one interned node:
//   Rational  an exact rational (rational).
//   Sum       rational, the constant term, plus terms: (term, coefficient)
//             pairs, at least one, in the canonical order (node_order); a
//             term is a monomial (an Atom, Op or Power, or a Product with
//             coefficient 1), and only a term that is not provably defined
//             keeps a zero coefficient.
//   Product   rational, the coefficient, times terms: (base, exponent)
//             pairs in the canonical order, with nonzero rational exponents
//             (one that is not provably nonzero keeps a zero exponent); a
//             base is an Atom, Op, Power or Sum, or a positive rational
//             other than 1 whose exponent lies strictly between 0 and 1 (a
//             radical: 2^(1/2) is √2, and 2^(3/2) is 2√2). A base with a
//             fractional exponent is provably positive. A single factor
//             with exponent 1 is a Product only with a coefficient other
//             than 1, and never of a Sum, whose terms take the coefficient
//             instead.
//   Power     args {base, exponent}: a power whose exponent is not a
//             rational number.
//   Atom      a provider's atom.
//   Op        a provider's operation applied to args.
enum class RealKind{
    Rational,Sum,Product,Power,Atom,Op,Infinity
};
struct Real;
using RealPtr=std::shared_ptr<Real>;
using RealTerms=std::vector<std::pair<RealPtr,Rational>>;
struct Real{
    RealKind kind{RealKind::Rational};
    Rational rational;
    RealTerms terms;
    std::string provider;
    std::uint32_t opcode{};
    std::vector<RealPtr> args;
    std::size_t hash{};
};

// Nodes are interned by structure (hash-consing): operands are interned
// before the nodes that use them, so two nodes are equal exactly when their
// kind, scalar fields and operand nodes are the same. The tables are
// thread-local and hold weak references; expired entries are swept as the
// table is used.
thread_local std::unordered_multimap<std::size_t,std::weak_ptr<Real>> real_intern;
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
    for(const auto& [term,factor]:value.terms){
        hash_combine(seed,std::hash<const Real*>{}(term.get()));
        hash_bigint(seed,factor.num);
        hash_bigint(seed,factor.den);
    }
    for(const auto& argument:value.args)
        hash_combine(seed,std::hash<const Real*>{}(argument.get()));
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
           left.terms.size()==right.terms.size()&&
           std::equal(left.terms.begin(),left.terms.end(),right.terms.begin(),
                      [](const auto& l,const auto& r){
                          return l.first.get()==r.first.get()&&
                                 same_bigint(l.second.num,r.second.num)&&
                                 same_bigint(l.second.den,r.second.den);
                      })&&
           left.args.size()==right.args.size()&&
           std::equal(left.args.begin(),left.args.end(),right.args.begin(),
                      [](const RealPtr& l,const RealPtr& r){return l.get()==r.get();});
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
    real_intern.emplace(value->hash,value);
    if((++real_intern_operations & 255U)==0){
        for(auto it=real_intern.begin();it!=real_intern.end();){
            if(it->second.expired())it=real_intern.erase(it);
            else ++it;
        }
    }
    return value;
}
// Re-intern a foreign immutable DAG using private copies. Interning changes
// each node's cached hash, so it must never mutate a node another thread owns.
RealPtr import_real(const RealPtr& root){
    std::unordered_map<const Real*,RealPtr> copied;
    std::vector<std::pair<const Real*,bool>> pending{{root.get(),false}};
    while(!pending.empty()){
        const auto [source,children_ready]=pending.back();
        pending.pop_back();
        if(copied.contains(source))continue;
        if(!children_ready){
            pending.emplace_back(source,true);
            for(const auto& [term,factor]:source->terms){
                (void)factor;
                if(!copied.contains(term.get()))pending.emplace_back(term.get(),false);
            }
            for(const auto& argument:source->args)
                if(!copied.contains(argument.get()))pending.emplace_back(argument.get(),false);
            continue;
        }
        auto value=std::make_shared<Real>(*source);
        for(auto& [term,factor]:value->terms){
            (void)factor;
            term=copied.at(term.get());
        }
        for(auto& argument:value->args)argument=copied.at(argument.get());
        copied.emplace(source,intern_real(std::move(value)));
    }
    return copied.at(root.get());
}
RealPtr rr(Rational rational){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Rational;value->rational=std::move(rational);
    return intern_real(std::move(value));
}
// An infinity is a distinct node, never a rational with denominator zero.
// The existing rational numerator stores only its sign for structural interning.
RealPtr infinity(int sign){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Infinity;
    value->rational=Rational(BigInt::from_i64(sign<0?-1:1),BigInt::from_u64(1));
    return intern_real(std::move(value));
}
int infinity_sign(const RealPtr& value){
    return value->kind==RealKind::Infinity?value->rational.num.sign:0;
}
// Text parsing is separate from decimal/rational parsing: finite exact values
// must never be rounded through an IEEE value to manufacture an infinity.
int infinity_text_sign(std::string_view text){
    if(text=="inf"||text=="+inf"||text=="∞"||text=="+∞")return 1;
    if(text=="-inf"||text=="-∞")return -1;
    return 0;
}
int arithmetic_sign(const RealPtr& value);
void arithmetic_defined(const RealPtr& value);
std::optional<abi::ConversionReason> real_to_integer(const RealPtr& value,BigInt& out);
RealPtr atom(std::string provider,std::uint32_t opcode){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Atom;
    value->provider=std::move(provider);
    value->opcode=opcode;
    return intern_real(std::move(value));
}
RealPtr power_node(RealPtr base,RealPtr exponent){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Power;
    value->args={std::move(base),std::move(exponent)};
    return intern_real(std::move(value));
}
RealPtr terms_node(RealKind kind,Rational rational,RealTerms terms){
    auto value=std::make_shared<Real>();
    value->kind=kind;
    value->rational=std::move(rational);
    value->terms=std::move(terms);
    return intern_real(std::move(value));
}
RealPtr op_node(std::string provider,std::uint32_t opcode,std::vector<RealPtr> args){
    auto value=std::make_shared<Real>();
    value->kind=RealKind::Op;
    value->provider=std::move(provider);
    value->opcode=opcode;
    value->args=std::move(args);
    return intern_real(std::move(value));
}
std::optional<ExactUnaryProvider> exact_unary_provider(const std::string& provider){
    std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
    const auto found=exact_unary_providers().find(provider);
    if(found==exact_unary_providers().end())return std::nullopt;
    return found->second;
}
// The flags of an operation node: its version-2 provider's, else the
// version-1 unary provider's.
std::uint32_t exact_op_flags(const RealPtr& value){
    if(!value||value->kind!=RealKind::Op)return 0;
    if(const auto provider=exact_provider_v2(value->provider))
        return provider->flags?provider->flags(value->opcode):0;
    if(value->args.size()!=1)return 0;
    const auto provider=exact_unary_provider(value->provider);
    return provider&&provider->flags?provider->flags(value->opcode):0;
}
// A version-1 atom's decimal text, unless a version-2 provider of that name
// is registered (which then owns its atoms).
std::optional<std::string> v1_atom_decimal(const std::string& provider,std::uint32_t opcode){
    if(exact_provider_v2(provider))return std::nullopt;
    return exact_atom_decimal(provider,opcode);
}
// The canonical order of nodes: by kind, then provider name, opcode and
// arguments, then values. It depends on the values only, never on the
// order they were built in, so sums and products have one form.
int kind_rank(RealKind kind){
    switch(kind){
        case RealKind::Rational:return 0;
        case RealKind::Power:return 1;
        case RealKind::Atom:return 2;
        case RealKind::Op:return 3;
        case RealKind::Product:return 4;
        case RealKind::Sum:return 5;
        case RealKind::Infinity:return 6;
    }
    return 6;
}
int node_order(const Real* a,const Real* b);
int terms_order(const RealTerms& a,const RealTerms& b){
    const auto n=std::min(a.size(),b.size());
    for(std::size_t i=0;i<n;++i){
        if(const int c=node_order(a[i].first.get(),b[i].first.get()))return c;
        if(const int c=rat_cmp(a[i].second,b[i].second))return c;
    }
    return a.size()<b.size()?-1:a.size()>b.size()?1:0;
}
int node_order(const Real* a,const Real* b){
    if(a==b)return 0;
    if(kind_rank(a->kind)!=kind_rank(b->kind))
        return kind_rank(a->kind)<kind_rank(b->kind)?-1:1;
    switch(a->kind){
        case RealKind::Infinity:
            return a->rational.num.sign<b->rational.num.sign?-1:
                   a->rational.num.sign>b->rational.num.sign?1:0;
        case RealKind::Rational:
            return rat_cmp(a->rational,b->rational);
        case RealKind::Atom:
        case RealKind::Op:{
            if(const int c=a->provider.compare(b->provider))return c<0?-1:1;
            if(a->opcode!=b->opcode)return a->opcode<b->opcode?-1:1;
            const auto n=std::min(a->args.size(),b->args.size());
            for(std::size_t i=0;i<n;++i)
                if(const int c=node_order(a->args[i].get(),b->args[i].get()))return c;
            return a->args.size()<b->args.size()?-1:a->args.size()>b->args.size()?1:0;
        }
        case RealKind::Power:
            if(const int c=node_order(a->args[0].get(),b->args[0].get()))return c;
            return node_order(a->args[1].get(),b->args[1].get());
        case RealKind::Product:
        case RealKind::Sum:
            if(const int c=terms_order(a->terms,b->terms))return c;
            return rat_cmp(a->rational,b->rational);
    }
    return 0;
}

bool rational_is_one(const Rational& r){
    return rat_int(r)&&r.num.sign==1&&r.num.limbs.size()==1&&r.num.limbs[0]==1;
}
Rational rational_one(){return Rational(BigInt::from_u64(1),BigInt::from_u64(1));}

bool atom_bounds(const RealPtr& x,Rational& low,Rational& high){
    if(!x||x->kind!=RealKind::Atom)return false;
    const auto decimal=v1_atom_decimal(x->provider,x->opcode);
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

// The sign of a sum and of a product of two values of known sign.
ProvenSign add_signs(ProvenSign left,ProvenSign right){
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
ProvenSign multiply_signs(ProvenSign left,ProvenSign right){
    if(left==ProvenSign::Unknown||right==ProvenSign::Unknown)return ProvenSign::Unknown;
    if(left==ProvenSign::Zero||right==ProvenSign::Zero)return ProvenSign::Zero;
    const bool negative=(left==ProvenSign::Negative||left==ProvenSign::NonPositive)!=
                        (right==ProvenSign::Negative||right==ProvenSign::NonPositive);
    const bool strict=(left==ProvenSign::Positive||left==ProvenSign::Negative)&&
                      (right==ProvenSign::Positive||right==ProvenSign::Negative);
    if(strict)return negative?ProvenSign::Negative:ProvenSign::Positive;
    return negative?ProvenSign::NonPositive:ProvenSign::NonNegative;
}

ProvenSign proven_sign(const RealPtr&x,std::size_t&budget){
    if(!x||!budget--)return ProvenSign::Unknown;
    switch(x->kind){
        case RealKind::Infinity:
            return x->rational.num.sign<0?ProvenSign::Negative:ProvenSign::Positive;
        case RealKind::Rational:
            return x->rational.num.sign<0?ProvenSign::Negative:
                   x->rational.num.sign>0?ProvenSign::Positive:
                                          ProvenSign::Zero;
        case RealKind::Atom:{
            if(const auto provider=exact_provider_v2(x->provider)){
                const auto flags=provider->flags?provider->flags(x->opcode):0U;
                if(flags&QCORE_EXACT_UNARY_RESULT_POSITIVE)return ProvenSign::Positive;
                if(flags&QCORE_EXACT_UNARY_RESULT_NONNEGATIVE)return ProvenSign::NonNegative;
                return ProvenSign::Unknown;
            }
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
        case RealKind::Op:{
            const auto flags=exact_op_flags(x);
            if(flags&QCORE_EXACT_UNARY_RESULT_POSITIVE)
                return ProvenSign::Positive;
            if(flags&QCORE_EXACT_UNARY_RESULT_NONNEGATIVE)
                return ProvenSign::NonNegative;
            return ProvenSign::Unknown;
        }
        case RealKind::Sum:{
            auto sign=x->rational.num.sign<0?ProvenSign::Negative:
                      x->rational.num.sign>0?ProvenSign::Positive:ProvenSign::Zero;
            for(const auto& [term,coefficient]:x->terms){
                auto part=proven_sign(term,budget);
                if(!coefficient.num.sign)return ProvenSign::Unknown;
                if(coefficient.num.sign<0)part=invert_sign(part);
                sign=add_signs(sign,part);
            }
            return sign;
        }
        case RealKind::Product:{
            if(!x->rational.num.sign)return ProvenSign::Unknown;
            auto sign=x->rational.num.sign<0?ProvenSign::Negative:ProvenSign::Positive;
            for(const auto& [base,exponent]:x->terms){
                auto part=proven_sign(base,budget);
                if(!rat_int(exponent)){
                    // A fractional power of a positive base.
                    if(part!=ProvenSign::Positive)return ProvenSign::Unknown;
                    continue;
                }
                const bool odd=exponent.num.sign&&(exponent.num.limbs[0]&1U);
                if(exponent.num.sign<=0&&
                   part!=ProvenSign::Positive&&part!=ProvenSign::Negative)
                    return ProvenSign::Unknown;
                if(!odd){
                    part=part==ProvenSign::Zero?ProvenSign::Zero:
                         part==ProvenSign::Positive||part==ProvenSign::Negative
                             ?ProvenSign::Positive:ProvenSign::NonNegative;
                }
                sign=multiply_signs(sign,part);
            }
            return sign;
        }
        case RealKind::Power:{
            std::size_t sign_budget=budget;
            return proven_sign(x->args[0],sign_budget)==ProvenSign::Positive
                ?ProvenSign::Positive:ProvenSign::Unknown;
        }
    }
    return ProvenSign::Unknown;
}
bool provably_defined(const RealPtr&x,std::size_t&budget){
    if(!x||!budget--)return false;
    switch(x->kind){
        case RealKind::Infinity:
        case RealKind::Rational:
            return true;
        case RealKind::Atom:{
            // A version-2 atom exists once constructed (its provider owns it).
            if(exact_provider_v2(x->provider))return true;
            Rational low,high;
            return atom_bounds(x,low,high);
        }
        case RealKind::Op:{
            for(const auto& argument:x->args)
                if(!provably_defined(argument,budget))return false;
            const auto flags=exact_op_flags(x);
            if(flags&QCORE_EXACT_UNARY_TOTAL)return true;
            for(const auto& argument:x->args){
                std::size_t sign_budget=4096;
                const auto sign=proven_sign(argument,sign_budget);
                if(flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE){
                    if(sign!=ProvenSign::Positive)return false;
                }else if(flags&QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE){
                    if(sign!=ProvenSign::Zero&&sign!=ProvenSign::Positive&&
                       sign!=ProvenSign::NonNegative)
                        return false;
                }else{
                    return false;
                }
            }
            return true;
        }
        case RealKind::Sum:
            for(const auto& [term,coefficient]:x->terms)
                if(!provably_defined(term,budget))return false;
            return true;
        case RealKind::Product:
            for(const auto& [base,exponent]:x->terms){
                if(!provably_defined(base,budget))return false;
                if(exponent.num.sign<=0||!rat_int(exponent)){
                    std::size_t sign_budget=4096;
                    const auto sign=proven_sign(base,sign_budget);
                    if(sign!=ProvenSign::Positive&&sign!=ProvenSign::Negative)return false;
                }
            }
            return true;
        case RealKind::Power:{
            if(!provably_defined(x->args[0],budget)||
               !provably_defined(x->args[1],budget))return false;
            std::size_t sign_budget=4096;
            return proven_sign(x->args[0],sign_budget)==ProvenSign::Positive;
        }
    }
    return false;
}

// Equal normal forms are one node; rationals are compared by value.
bool structurally_equal(const RealPtr&a,const RealPtr&b,std::size_t&){
    if(a.get()==b.get())return true;
    return a&&b&&a->kind==RealKind::Rational&&b->kind==RealKind::Rational&&
           rat_cmp(a->rational,b->rational)==0;
}

// Generic normalization. A value is a constant plus coefficients of
// monomials (its linear form), and a coefficient times integer powers of
// bases (its product form). Sums and products merge their forms in the
// canonical order, combine like terms and equal factors, flatten, and drop
// a term whose coefficient or a factor whose exponent becomes 0 when that
// term is provably defined or that base provably nonzero. A rational
// coefficient distributes over a sum; nothing else is expanded, so the
// node count never grows by normalization.
bool provably_nonzero(const RealPtr& x){
    std::size_t defined_budget=4096,sign_budget=4096;
    if(!provably_defined(x,defined_budget))return false;
    const auto sign=proven_sign(x,sign_budget);
    return sign==ProvenSign::Positive||sign==ProvenSign::Negative;
}
// Merges two term lists in the canonical order, adding the rationals of
// equal nodes; keeps zeros only where drop_zero says no.
template<class DropZero>
RealTerms merge_terms(const RealTerms& left,const RealTerms& right,DropZero&& drop_zero){
    RealTerms out;
    out.reserve(left.size()+right.size());
    std::size_t i=0,j=0;
    const auto push=[&](const RealPtr& node,Rational value){
        if(!value.num.sign&&drop_zero(node))return;
        out.emplace_back(node,std::move(value));
    };
    while(i<left.size()||j<right.size()){
        if(j==right.size()){push(left[i].first,left[i].second);++i;continue;}
        if(i==left.size()){push(right[j].first,right[j].second);++j;continue;}
        const int c=node_order(left[i].first.get(),right[j].first.get());
        if(c<0){push(left[i].first,left[i].second);++i;}
        else if(c>0){push(right[j].first,right[j].second);++j;}
        else{push(left[i].first,rat_add(left[i].second,right[j].second));++i;++j;}
    }
    return out;
}
RealPtr make_product(Rational coefficient,RealTerms factors);
RealPtr scale(const RealPtr& x,const Rational& factor);
// A monomial times a coefficient.
RealPtr scale_monomial(const RealPtr& monomial,const Rational& coefficient){
    if(rational_is_one(coefficient))return monomial;
    if(monomial->kind==RealKind::Product)return make_product(coefficient,monomial->terms);
    return make_product(coefficient,{{monomial,rational_one()}});
}
RealPtr make_sum(Rational constant,RealTerms terms){
    if(terms.empty())return rr(std::move(constant));
    if(!constant.num.sign&&terms.size()==1&&terms[0].second.num.sign)
        return scale_monomial(terms[0].first,terms[0].second);
    return terms_node(RealKind::Sum,std::move(constant),std::move(terms));
}
// r^n for an integer n (r nonzero when n is negative).
Rational rational_power(const Rational& r,const BigInt& n){
    long long e=0;
    if(!bigint_try_i64_value(n,64,e)||std::llabs(e)>100000)
        exact_fail("real power exponent is too large");
    const auto magnitude=static_cast<unsigned>(std::llabs(e));
    Rational out(pow_small(r.num,magnitude),pow_small(r.den,magnitude));
    return e<0?rat_div(rational_one(),out):out;
}
// floor(r) and r - floor(r) of a rational.
std::pair<BigInt,Rational> rational_floor(const Rational& r){
    auto qr=divmod(r.num,r.den);
    auto whole=std::move(qr.first);
    if(qr.second.sign<0)whole=sub(whole,BigInt::from_u64(1));
    return {whole,rat_sub(r,Rational(whole,BigInt::from_u64(1)))};
}
RealPtr make_product(Rational coefficient,RealTerms factors){
    // A rational base keeps the fractional part of its exponent; the integer
    // part moves into the coefficient (√2·√2 is 2, 2^(3/2) is 2√2).
    for(auto it=factors.begin();it!=factors.end();){
        if(it->first->kind!=RealKind::Rational||rat_int(it->second)||!coefficient.num.sign){
            ++it;
            continue;
        }
        auto [whole,fraction]=rational_floor(it->second);
        if(whole.sign)coefficient=rat_mul(coefficient,rational_power(it->first->rational,whole));
        if(!fraction.num.sign){
            it=factors.erase(it);
            continue;
        }
        it->second=std::move(fraction);
        ++it;
    }
    if(factors.empty())return rr(std::move(coefficient));
    if(factors.size()==1&&rational_is_one(factors[0].second)){
        const auto base=factors[0].first;
        if(rational_is_one(coefficient))return base;
        if(base->kind==RealKind::Sum&&coefficient.num.sign)return scale(base,coefficient);
    }
    return terms_node(RealKind::Product,std::move(coefficient),std::move(factors));
}
// The monomial of a product with coefficient 1, and its coefficient.
std::pair<RealPtr,Rational> monomial_of(const RealPtr& x){
    if(x->kind!=RealKind::Product)return {x,rational_one()};
    if(x->terms.size()==1&&rational_is_one(x->terms[0].second))
        return {x->terms[0].first,x->rational};
    return {terms_node(RealKind::Product,rational_one(),x->terms),x->rational};
}
struct LinearForm{Rational constant;RealTerms terms;};
LinearForm linear_form(const RealPtr& x){
    if(x->kind==RealKind::Rational)return {x->rational,{}};
    if(x->kind==RealKind::Sum)return {x->rational,x->terms};
    auto [monomial,coefficient]=monomial_of(x);
    return {Rational(),{{monomial,coefficient}}};
}
struct ProductForm{Rational coefficient;RealTerms factors;};
ProductForm product_form(const RealPtr& x){
    if(x->kind==RealKind::Rational)return {x->rational,{}};
    if(x->kind==RealKind::Product)return {x->rational,x->terms};
    return {rational_one(),{{x,rational_one()}}};
}
RealPtr scale(const RealPtr& x,const Rational& factor){
    if(const auto sign=infinity_sign(x)){
        if(!factor.num.sign)exact_fail("indeterminate real multiplication: zero times infinity");
        return infinity(sign*factor.num.sign);
    }
    if(rational_is_one(factor))return x;
    if(x->kind==RealKind::Rational)return rr(rat_mul(x->rational,factor));
    if(!factor.num.sign){
        std::size_t defined_budget=4096;
        if(provably_defined(x,defined_budget))return rr({});
        auto form=product_form(x);
        return terms_node(RealKind::Product,Rational(),std::move(form.factors));
    }
    if(x->kind==RealKind::Sum){
        RealTerms terms;
        terms.reserve(x->terms.size());
        for(const auto& [term,coefficient]:x->terms)
            terms.emplace_back(term,rat_mul(coefficient,factor));
        return make_sum(rat_mul(x->rational,factor),std::move(terms));
    }
    if(x->kind==RealKind::Product)return make_product(rat_mul(x->rational,factor),x->terms);
    return make_product(factor,{{x,rational_one()}});
}

RealPtr real_neg(RealPtr a){
    return scale(a,Rational(BigInt::from_i64(-1),BigInt::from_u64(1)));
}
RealPtr real_add(RealPtr a,RealPtr b){
    const int ai=infinity_sign(a),bi=infinity_sign(b);
    if(ai||bi){
        if(ai&&bi&&ai!=bi)exact_fail("indeterminate real addition: opposite infinities");
        if(!ai)arithmetic_defined(a);
        if(!bi)arithmetic_defined(b);
        return ai?a:b;
    }
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_add(a->rational,b->rational));
    auto left=linear_form(a);
    auto right=linear_form(b);
    auto terms=merge_terms(left.terms,right.terms,[](const RealPtr& term){
        std::size_t defined_budget=4096;
        return provably_defined(term,defined_budget);
    });
    return make_sum(rat_add(left.constant,right.constant),std::move(terms));
}
RealPtr real_sub(RealPtr a,RealPtr b){
    if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
        return rr(rat_sub(a->rational,b->rational));
    return real_add(std::move(a),real_neg(std::move(b)));
}
RealPtr real_mul(RealPtr a,RealPtr b){
    if(infinity_sign(a)||infinity_sign(b)){
        const int as=arithmetic_sign(a),bs=arithmetic_sign(b);
        if(!as||!bs)exact_fail("indeterminate real multiplication: zero times infinity");
        return infinity(as*bs);
    }
    if(a->kind==RealKind::Rational)return scale(b,a->rational);
    if(b->kind==RealKind::Rational)return scale(a,b->rational);
    auto left=product_form(a);
    auto right=product_form(b);
    auto factors=merge_terms(left.factors,right.factors,[](const RealPtr& base){
        return provably_nonzero(base);
    });
    return make_product(rat_mul(left.coefficient,right.coefficient),std::move(factors));
}
RealPtr real_div(RealPtr a,RealPtr b){
    const int ai=infinity_sign(a),bi=infinity_sign(b);
    if(bi){
        if(ai)exact_fail("indeterminate real division: infinity divided by infinity");
        arithmetic_defined(a);
        return rr({});
    }
    // Exact real has no signed zero: its zero divisor is the positive zero.
    const int bs=arithmetic_sign(b);
    if(ai)return infinity(bs<0?-ai:ai);
    if(!bs){
        const int as=arithmetic_sign(a);
        if(!as)exact_division_by_zero();
        return infinity(as);
    }
    if(b->kind==RealKind::Rational){
        return scale(a,rat_div(rational_one(),b->rational));
    }
    auto form=product_form(b);
    if(!form.coefficient.num.sign)exact_division_by_zero();
    for(auto& [base,exponent]:form.factors)exponent.num=neg(std::move(exponent.num));
    return real_mul(std::move(a),
                    make_product(rat_div(rational_one(),form.coefficient),std::move(form.factors)));
}
RealPtr rational_power_of(RealPtr base,const Rational& exponent,
                          unsigned long long line,unsigned long long column);
std::optional<RealPtr> canonical_power(const RealPtr& base,const RealPtr& exponent);
RealPtr real_pow_generic(RealPtr base,RealPtr exponent,
                         unsigned long long line,unsigned long long column){
    const int bi=infinity_sign(base),ei=infinity_sign(exponent);
    if(ei){
        const int bs=arithmetic_sign(base);
        const auto magnitude=bs<0?real_neg(base):base;
        const int compared=arithmetic_sign(real_sub(magnitude,rr(rational_one())));
        if(!compared)return rr(rational_one());
        return compared*ei>0?infinity(1):rr({});
    }
    if(bi){
        const int es=arithmetic_sign(exponent);
        if(!es)return rr(rational_one());
        int result_sign=1;
        if(bi<0){
            BigInt integer;
            if(const auto reason=real_to_integer(exponent,integer)){
                if(*reason==abi::ConversionReason::not_integral)
                    quidra::runtime::report_failure(abi::FailureReason::negative_base_fractional_exponent,{},line,column);
                exact_unproven(abi::FailureReason::exact_zero_undecided,"exact infinity power exponent could not be proven integral",line,column);
            }
            if(!integer.limbs.empty()&&(integer.limbs[0]&1U))result_sign=-1;
        }
        return es<0?rr({}):infinity(result_sign);
    }
    if(base->kind==RealKind::Rational&&!base->rational.num.sign){
        const int es=arithmetic_sign(exponent);
        if(!es)quidra::runtime::report_failure(abi::FailureReason::zero_power_zero,{},line,column);
        return es<0?infinity(1):rr({});
    }
    if(exponent->kind==RealKind::Rational&&!rat_int(exponent->rational))
        return rational_power_of(std::move(base),exponent->rational,line,column);
    if(exponent->kind!=RealKind::Rational){
        // A power with an exponent that is not rational: defined for a
        // positive base; its refinement belongs to a provider.
        if(base->kind==RealKind::Rational&&base->rational.num.sign<0)
            quidra::runtime::report_failure(abi::FailureReason::negative_base_fractional_exponent,
                                            {},line,column);
        return power_node(std::move(base),std::move(exponent));
    }
    if(exponent->kind==RealKind::Rational&&rat_int(exponent->rational)){
        const auto text=exponent->rational.num.text();
        char* end=nullptr;errno=0;
        const long long e=std::strtoll(text.c_str(),&end,10);
        if(errno!=ERANGE&&end&&*end=='\0'&&std::llabs(e)<=100000){
            RealPtr result=rr(rational_one());
            RealPtr factor=base;
            unsigned long long n=e<0
                ?static_cast<unsigned long long>(-(e+1))+1ULL
                :static_cast<unsigned long long>(e);
            while(n){
                if(n&1ULL)result=real_mul(result,factor);
                n>>=1ULL;
                if(n)factor=real_mul(factor,factor);
            }
            if(e<0)result=real_div(rr(rational_one()),result);
            return result;
        }
    }
    return power_node(std::move(base),std::move(exponent));
}
RealPtr real_pow(RealPtr base,RealPtr exponent,
                 unsigned long long line,unsigned long long column){
    auto result=real_pow_generic(base,exponent,line,column);
    // Exact rational results and integer powers keep the generic path.
    // A provider may normalize a remaining radical without changing its value.
    if(result->kind!=RealKind::Rational&&result->kind!=RealKind::Infinity&&
       exponent->kind==RealKind::Rational&&
       !rat_int(exponent->rational)){
        if(auto canonical=canonical_power(base,exponent))return *canonical;
    }
    return result;
}

#endif

#if QUIDRA_EXACT_SECTION == 5
struct BigRealValue{RealPtr value;};

#endif

#if QUIDRA_EXACT_SECTION == 7
BigRealValue* br(void*p){return static_cast<BigRealValue*>(p);}

#endif

#if QUIDRA_EXACT_SECTION == 9
void* make_br(RealPtr value){
    return managed_new<BigRealValue>(BigRealValue{std::move(value)});
}

#endif

#if QUIDRA_EXACT_SECTION == 11
void release_br(void* handle){
    quidra_managed_release(handle,reinterpret_cast<void*>(&quidra_bigreal_drop));
}

// An argument a provider's domain excludes is an error when it is proven
// outside it; an argument whose sign is not proven is left to refinement.
void check_provider_domain(std::uint32_t flags,const RealPtr& argument){
    std::size_t sign_budget=4096;
    const auto sign=proven_sign(argument,sign_budget);
    if((flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE)&&
       (sign==ProvenSign::Negative||sign==ProvenSign::Zero||
        sign==ProvenSign::NonPositive))
        exact_fail("exact-real operation input is outside the provider domain");
    if((flags&QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE)&&sign==ProvenSign::Negative)
        exact_fail("exact-real operation input is outside the provider domain");
}

// A version-2 provider's opcode applied to args: an atom (no args) or an
// operation, checked against the provider's arity and domain, then given to
// its canonicalization.
RealPtr provider_node(const std::string& name,std::uint32_t opcode,std::vector<RealPtr> args){
    const auto provider=exact_provider_v2(name);
    if(!provider)
        exact_fail(args.empty()?"exact-real provider or atom opcode is unavailable"
                               :"exact-real provider operation is unavailable");
    if(!v2_owns(*provider,opcode,args.size()))
        exact_fail(args.empty()?"exact-real provider or atom opcode is unavailable"
                               :"exact-real provider operation is unavailable");
    if(args.empty())return atom(name,opcode);
    const auto flags=provider->flags?provider->flags(opcode):0U;
    const bool has_infinity=std::any_of(args.begin(),args.end(),[](const RealPtr& a){return infinity_sign(a)!=0;});
    // Existing providers only declare finite domains. They must explicitly
    // opt in and canonicalize infinite inputs before algebra may simplify them.
    if(has_infinity&&!(flags&QCORE_EXACT_UNARY_CANONICALIZES_INFINITY))
        exact_fail("exact-real provider does not support infinity inputs");
    for(const auto& argument:args)check_provider_domain(flags,argument);
    if(provider->canonicalize){
        std::vector<void*> handles;
        handles.reserve(args.size());
        quidra::runtime::NodeRuntimeOwner handles_owner(&handles,+[](void* raw){
            auto& owned=*static_cast<std::vector<void*>*>(raw);
            for(auto& handle:owned){
                auto* value=std::exchange(handle,nullptr);
                if(value)quidra::runtime::cleanup_node_slot(release_br,value);
            }
        });
        for(const auto& argument:args)handles.push_back(make_br(argument));
        void* canonical=provider->canonicalize(
            opcode,const_cast<const void* const*>(handles.data()),
            static_cast<std::uint32_t>(handles.size()));
        quidra::runtime::NodeRuntimeOwner canonical_owner(canonical,release_br);
        for(auto& handle:handles)release_br(std::exchange(handle,nullptr));
        handles_owner.relinquish();
        if(canonical){
            auto value=br(canonical)->value;
            canonical_owner.relinquish();release_br(canonical);
            return value;
        }
    }
    if(has_infinity)exact_fail("exact-real provider did not canonicalize infinity input");
    return op_node(name,opcode,std::move(args));
}

std::optional<RealPtr> canonical_power(const RealPtr& base,const RealPtr& exponent){
    // Canonicalizers can build their result using Core power constructors.
    // Those nested constructors use the generic algorithm rather than
    // re-entering the same provider callback.
    static thread_local bool active=false;
    if(active)return std::nullopt;
    const auto hook=exact_power_provider();
    if(!hook)return std::nullopt;
    const auto provider=exact_provider_v2(hook->name);
    if(!provider||!v2_owns(*provider,hook->opcode,2)||!provider->canonicalize)
        return std::nullopt;
    struct Scope{
        bool& active;
        explicit Scope(bool& value):active(value){active=true;}
        ~Scope(){active=false;}
    } scope(active);
    // provider_node preserves the callback's normal handle ownership,
    // domain checks and cleanup. A declined hook leaves Core's form intact.
    auto value=provider_node(hook->name,hook->opcode,{base,exponent});
    if(value->kind==RealKind::Op&&value->provider==hook->name&&
       value->opcode==hook->opcode&&value->args.size()==2&&
       value->args[0]==base&&value->args[1]==exponent)return std::nullopt;
    return value;
}

// Certified refinement (ExactRefiner): interval arithmetic over dyadic
// rationals m * 2^e, every bound rounded outward, so the exact value always
// lies inside. A decision (a comparison, a conversion, digits) refines at
// increasing precision until the enclosure decides it; the budget is a
// deterministic count of precisions and node visits (abi/exact_codes.hpp),
// never time, so a decision is the same on every run and platform.

// Why a refinement stopped without deciding: the reason of EXACT_UNPROVEN.
struct RefineStop{abi::FailureReason reason;};

constexpr double log2_limb_base=29.897352853986263;  // log2(1e9)

// About the bit length of |m|: within one of it. Rounding to a mantissa
// width needs no more.
long long bit_estimate(const BigInt& m){
    if(!m.sign)return 0;
    const double top=static_cast<double>(m.limbs.back());
    return static_cast<long long>(std::floor(
               std::log2(top)+static_cast<double>(m.limbs.size()-1)*log2_limb_base))+1;
}
// The exact bit length of |m|: the least k with |m| < 2^k.
BigInt shift_left(BigInt m,unsigned long long k);
long long bit_length(const BigInt& m){
    if(!m.sign)return 0;
    long long k=bit_estimate(m);
    const auto magnitude=absbi(m);
    while(k>1&&cmp_abs(shift_left(BigInt::from_u64(1),static_cast<unsigned long long>(k-1)),magnitude)>0)--k;
    while(cmp_abs(shift_left(BigInt::from_u64(1),static_cast<unsigned long long>(k)),magnitude)<=0)++k;
    return k;
}
BigInt shift_left(BigInt m,unsigned long long k){
    constexpr unsigned chunk=29;
    while(k>=chunk&&m.sign){m=mul_small(m,1U<<chunk);k-=chunk;}
    if(k&&m.sign)m=mul_small(m,1U<<k);
    return m;
}
// |m| / 2^k toward zero; inexact says whether a set bit was dropped.
BigInt shift_right_magnitude(BigInt m,unsigned long long k,bool& inexact){
    m=absbi(std::move(m));
    inexact=false;
    constexpr unsigned chunk=29;
    while(k>=chunk&&m.sign){
        auto qr=div_small(m,1U<<chunk);
        inexact=inexact||qr.second!=0;
        m=std::move(qr.first);
        k-=chunk;
    }
    if(k&&m.sign){
        auto qr=div_small(m,1U<<k);
        inexact=inexact||qr.second!=0;
        m=std::move(qr.first);
    }
    return m;
}
enum class Round{down,up};
BigInt shift_right(const BigInt& m,unsigned long long k,Round round){
    bool inexact=false;
    auto q=shift_right_magnitude(m,k,inexact);
    const auto one=BigInt::from_u64(1);
    if(m.sign<0){
        if(inexact&&round==Round::down)q=add(q,one);
        return neg(std::move(q));
    }
    if(inexact&&round==Round::up)q=add(q,one);
    return q;
}

struct Dyadic{BigInt m;long long e{};};

Dyadic dyadic_round(Dyadic x,long long bits,Round round){
    if(!x.m.sign)return {};
    const auto n=bit_estimate(x.m);
    if(n>bits){
        const auto shift=n-bits;
        x.m=shift_right(x.m,static_cast<unsigned long long>(shift),round);
        x.e+=shift;
    }
    return x;
}
int dyadic_sign(const Dyadic& x){return x.m.sign;}
int dyadic_compare(const Dyadic& a,const Dyadic& b){
    if(a.m.sign!=b.m.sign)return a.m.sign<b.m.sign?-1:1;
    if(!a.m.sign)return 0;
    const auto e=std::min(a.e,b.e);
    return cmp(shift_left(a.m,static_cast<unsigned long long>(a.e-e)),
               shift_left(b.m,static_cast<unsigned long long>(b.e-e)));
}
Dyadic dyadic_negate(Dyadic x){x.m=neg(std::move(x.m));return x;}
Dyadic dyadic_add(const Dyadic& a,const Dyadic& b,long long bits,Round round){
    if(!a.m.sign)return dyadic_round(b,bits,round);
    if(!b.m.sign)return dyadic_round(a,bits,round);
    // An operand far below the other's last kept bit is replaced by a bound
    // on the side `round` asks for: zero, or one unit of a bit below it.
    const auto top_a=a.e+bit_estimate(a.m);
    const auto top_b=b.e+bit_estimate(b.m);
    const auto negligible=[&](const Dyadic& big,long long top_big,const Dyadic& small,
                              long long top_small)->std::optional<Dyadic>{
        if(top_big-top_small<=bits+8)return std::nullopt;
        Dyadic bound;
        if((small.m.sign>0)==(round==Round::up)){
            bound.m=BigInt::from_i64(small.m.sign);
            bound.e=top_big-bits-8;
        }
        return dyadic_add(big,bound,bits,round);
    };
    if(auto result=negligible(a,top_a,b,top_b))return *result;
    if(auto result=negligible(b,top_b,a,top_a))return *result;
    const auto e=std::min(a.e,b.e);
    Dyadic sum{add(shift_left(a.m,static_cast<unsigned long long>(a.e-e)),
                   shift_left(b.m,static_cast<unsigned long long>(b.e-e))),e};
    return dyadic_round(std::move(sum),bits,round);
}
Dyadic dyadic_multiply(const Dyadic& a,const Dyadic& b,long long bits,Round round){
    return dyadic_round(Dyadic{mul(a.m,b.m),a.e+b.e},bits,round);
}
// a / b (b nonzero) with `bits` significant bits, rounded as asked.
Dyadic dyadic_divide(const Dyadic& a,const Dyadic& b,long long bits,Round round){
    if(!a.m.sign)return {};
    long long shift=bits+bit_estimate(b.m)-bit_estimate(a.m)+2;
    if(shift<0)shift=0;
    auto qr=divmod(shift_left(a.m,static_cast<unsigned long long>(shift)),b.m);
    auto q=std::move(qr.first);
    if(qr.second.sign){
        const bool positive=a.m.sign==b.m.sign;
        const auto one=BigInt::from_u64(1);
        if(positive&&round==Round::up)q=add(q,one);
        if(!positive&&round==Round::down)q=sub(q,one);
    }
    return dyadic_round(Dyadic{std::move(q),a.e-b.e-shift},bits,round);
}

// A closed interval [lo, hi]; an unbounded one could not be enclosed at
// this precision (a divisor or a domain argument still straddles zero).
struct Interval{Dyadic lo,hi;bool bounded{true};};

Interval unbounded_interval(){Interval out;out.bounded=false;return out;}
Interval point_interval(const Dyadic& value){return {value,value,true};}
Interval rational_interval(const Rational& r,long long bits){
    const Dyadic n{r.num,0},d{r.den,0};
    if(rat_int(r))return {dyadic_round(n,bits,Round::down),dyadic_round(n,bits,Round::up),true};
    return {dyadic_divide(n,d,bits,Round::down),dyadic_divide(n,d,bits,Round::up),true};
}
bool interval_contains_zero(const Interval& x){
    return dyadic_sign(x.lo)<=0&&dyadic_sign(x.hi)>=0;
}
Interval interval_add(const Interval& a,const Interval& b,long long bits){
    if(!a.bounded||!b.bounded)return unbounded_interval();
    return {dyadic_add(a.lo,b.lo,bits,Round::down),dyadic_add(a.hi,b.hi,bits,Round::up),true};
}
Interval interval_multiply(const Interval& a,const Interval& b,long long bits){
    if(!a.bounded||!b.bounded)return unbounded_interval();
    const Dyadic* pairs[4][2]={{&a.lo,&b.lo},{&a.lo,&b.hi},{&a.hi,&b.lo},{&a.hi,&b.hi}};
    Interval out;
    for(int i=0;i<4;++i){
        auto low=dyadic_multiply(*pairs[i][0],*pairs[i][1],bits,Round::down);
        auto high=dyadic_multiply(*pairs[i][0],*pairs[i][1],bits,Round::up);
        if(i==0||dyadic_compare(low,out.lo)<0)out.lo=std::move(low);
        if(i==0||dyadic_compare(high,out.hi)>0)out.hi=std::move(high);
    }
    return out;
}
Interval interval_divide(const Interval& a,const Interval& b,long long bits){
    if(!a.bounded||!b.bounded||interval_contains_zero(b))return unbounded_interval();
    const Dyadic* pairs[4][2]={{&a.lo,&b.lo},{&a.lo,&b.hi},{&a.hi,&b.lo},{&a.hi,&b.hi}};
    Interval out;
    for(int i=0;i<4;++i){
        auto low=dyadic_divide(*pairs[i][0],*pairs[i][1],bits,Round::down);
        auto high=dyadic_divide(*pairs[i][0],*pairs[i][1],bits,Round::up);
        if(i==0||dyadic_compare(low,out.lo)<0)out.lo=std::move(low);
        if(i==0||dyadic_compare(high,out.hi)>0)out.hi=std::move(high);
    }
    return out;
}
Interval interval_power(Interval base,BigInt exponent,long long bits){
    if(!base.bounded)return base;
    const bool negative=exponent.sign<0;
    exponent=absbi(std::move(exponent));
    Interval result=point_interval(Dyadic{BigInt::from_u64(1),0});
    while(exponent.sign){
        auto qr=div_small(exponent,2);
        if(qr.second)result=interval_multiply(result,base,bits);
        exponent=std::move(qr.first);
        if(exponent.sign)base=interval_multiply(base,base,bits);
    }
    if(negative)
        result=interval_divide(point_interval(Dyadic{BigInt::from_u64(1),0}),result,bits);
    return result;
}

// An interval as a provider reads it: both bounds over one exponent.
struct IntervalText{std::string low,high;std::int64_t exponent{};};
IntervalText interval_text(const Interval& x){
    const auto e=std::min(x.lo.e,x.hi.e);
    return {shift_left(x.lo.m,static_cast<unsigned long long>(x.lo.e-e)).text(),
            shift_left(x.hi.m,static_cast<unsigned long long>(x.hi.e-e)).text(),
            static_cast<std::int64_t>(e)};
}
std::optional<Interval> provider_interval(const qcore_exact_interval& out){
    if(!out.low||!out.high)return std::nullopt;
    bool low_ok=false,high_ok=false;
    Interval result;
    result.lo=Dyadic{BigInt::parse(out.low,low_ok),out.exponent};
    result.hi=Dyadic{BigInt::parse(out.high,high_ok),out.exponent};
    if(!low_ok||!high_ok||dyadic_compare(result.lo,result.hi)>0)return std::nullopt;
    return result;
}

// floor(n^(1/k)) for n >= 0 (Newton's method from above).
BigInt integer_root(const BigInt& n,unsigned k){
    if(!n.sign||k==1)return n;
    const auto one=BigInt::from_u64(1);
    const long long bits=bit_estimate(n);
    BigInt x=shift_left(one,static_cast<unsigned long long>(bits/k+2));
    for(;;){
        const auto power=pow_small(x,k-1);
        const auto y=div_small(add(mul_small(x,k-1),divmod(n,power).first),k).first;
        if(cmp(y,x)>=0)return x;
        x=y;
    }
}
// The rational k-th root of r >= 0, when it has one.
std::optional<Rational> exact_root(const Rational& r,unsigned k){
    const auto a=integer_root(r.num,k);
    if(cmp(pow_small(a,k),r.num)!=0)return std::nullopt;
    const auto b=integer_root(r.den,k);
    if(cmp(pow_small(b,k),r.den)!=0)return std::nullopt;
    return Rational(a,b);
}
// The largest root degree whose rational roots Core looks for and whose
// powers it refines generically (by integer roots); larger ones are left
// to providers.
constexpr unsigned max_root_degree=64;

// One precision of a refinement: the enclosure of every node at `bits`,
// each node visited once, counted against the decision's visit budget.
class Refiner{
public:
    Refiner(std::size_t& visits,long long bits):visits_(visits),bits_(bits){}
    Interval enclose(const RealPtr& x){
        if(const auto found=memo_.find(x.get());found!=memo_.end())return found->second;
        if(!visits_||depth_>=abi::exact_refine_max_depth)
            throw RefineStop{abi::FailureReason::exact_budget_exhausted};
        --visits_;
        ++depth_;
        auto result=compute(x);
        --depth_;
        memo_.emplace(x.get(),result);
        return result;
    }
private:
    Interval compute(const RealPtr& x){
        switch(x->kind){
            case RealKind::Infinity:throw RefineStop{abi::FailureReason::exact_no_refinement};
            case RealKind::Rational:return rational_interval(x->rational,bits_);
            case RealKind::Atom:return atom(x);
            case RealKind::Op:return operation(x);
            case RealKind::Sum:{
                auto result=rational_interval(x->rational,bits_);
                for(const auto& [term,coefficient]:x->terms)
                    result=interval_add(result,interval_multiply(rational_interval(coefficient,bits_),
                                                                enclose(term),bits_),bits_);
                return result;
            }
            case RealKind::Product:{
                auto result=rational_interval(x->rational,bits_);
                for(const auto& [base,exponent]:x->terms)
                    result=interval_multiply(result,factor_interval(base,exponent),bits_);
                return result;
            }
            case RealKind::Power:return power(x);
        }
        throw RefineStop{abi::FailureReason::exact_no_refinement};
    }
    Interval atom(const RealPtr& x){
        if(const auto provider=exact_provider_v2(x->provider)){
            qcore_exact_interval out{};
            if(!provider->refine||
               !provider->refine(x->opcode,nullptr,0,static_cast<std::uint32_t>(bits_),&out))
                throw RefineStop{abi::FailureReason::exact_no_refinement};
            const auto interval=provider_interval(out);
            if(!interval)exact_fail("exact-real provider refinement is malformed");
            return *interval;
        }
        // A version-1 atom: its decimal prefix and the next decimal unit.
        Rational low,high;
        if(!atom_bounds(x,low,high))throw RefineStop{abi::FailureReason::exact_no_refinement};
        return {rational_interval(low,bits_).lo,rational_interval(high,bits_).hi,true};
    }
    Interval operation(const RealPtr& x){
        const auto provider=exact_provider_v2(x->provider);
        if(!provider||!provider->refine)throw RefineStop{abi::FailureReason::exact_no_refinement};
        const auto flags=provider->flags?provider->flags(x->opcode):0U;
        std::vector<IntervalText> texts;
        texts.reserve(x->args.size());
        for(const auto& argument:x->args){
            const auto interval=enclose(argument);
            if(!interval.bounded)return unbounded_interval();
            if(flags&(QCORE_EXACT_UNARY_DOMAIN_POSITIVE|QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE)){
                const bool positive=flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE;
                // Outside the domain: the value does not exist.
                if(positive?dyadic_sign(interval.hi)<=0:dyadic_sign(interval.hi)<0)
                    exact_fail("exact-real operation input is outside the provider domain");
                // Not yet inside it: refine further.
                if(positive?dyadic_sign(interval.lo)<=0:dyadic_sign(interval.lo)<0)
                    return unbounded_interval();
            }
            texts.push_back(interval_text(interval));
        }
        std::vector<qcore_exact_interval> args;
        args.reserve(texts.size());
        for(const auto& text:texts)
            args.push_back(qcore_exact_interval{text.low.c_str(),text.high.c_str(),text.exponent});
        qcore_exact_interval out{};
        if(!provider->refine(x->opcode,args.data(),static_cast<std::uint32_t>(args.size()),
                             static_cast<std::uint32_t>(bits_),&out))
            throw RefineStop{abi::FailureReason::exact_no_refinement};
        const auto interval=provider_interval(out);
        if(!interval)exact_fail("exact-real provider refinement is malformed");
        return *interval;
    }
    Interval power(const RealPtr& x){
        // A rational exponent of a Product base; any other exponent needs a
        // provider's refinement.
        if(x->args[1]->kind==RealKind::Rational)
            return factor_interval(x->args[0],x->args[1]->rational);
        const auto hook=exact_power_provider();
        if(!hook)throw RefineStop{abi::FailureReason::exact_no_refinement};
        const auto provider=exact_provider_v2(hook->name);
        if(!provider||!v2_owns(*provider,hook->opcode,2)||!provider->refine)
            throw RefineStop{abi::FailureReason::exact_no_refinement};
        // Non-rational powers refine only once their positive-base domain
        // is certified. The exponent may have either sign.
        const auto base=enclose(x->args[0]);
        if(!base.bounded||dyadic_sign(base.lo)<=0)return unbounded_interval();
        return operation(op_node(hook->name,hook->opcode,x->args));
    }
    // base^exponent: an integer power of the enclosure, or for p/q the
    // enclosure's q-th roots by integer roots (rounded outward), raised to p.
    Interval factor_interval(const RealPtr& base,const Rational& exponent){
        const auto inner=enclose(base);
        if(rat_int(exponent))return interval_power(inner,exponent.num,bits_);
        unsigned long long degree=0;
        if(!bigint_try_u64_value(exponent.den,64,degree)||degree>max_root_degree)
            throw RefineStop{abi::FailureReason::exact_no_refinement};
        if(!inner.bounded||dyadic_sign(inner.lo)<=0)return unbounded_interval();
        const auto k=static_cast<unsigned>(degree);
        Interval roots{root_bound(inner.lo,k,Round::down),root_bound(inner.hi,k,Round::up),true};
        return interval_power(roots,exponent.num,bits_);
    }
    // x^(1/k) for x > 0, rounded down or up, with bits_ fractional bits
    // beyond its leading bit.
    Dyadic root_bound(const Dyadic& x,unsigned k,Round round){
        // floor(x * 2^(k*B)) for B fractional bits of the root.
        const long long top=x.e+bit_estimate(x.m);
        const long long fraction=bits_+2-top/static_cast<long long>(k);
        const long long shift=x.e+static_cast<long long>(k)*fraction;
        BigInt scaled=shift>=0?shift_left(x.m,static_cast<unsigned long long>(shift))
                              :shift_right(x.m,static_cast<unsigned long long>(-shift),Round::down);
        auto root=integer_root(scaled,k);
        if(round==Round::up)root=add(root,BigInt::from_u64(1));
        return Dyadic{std::move(root),-fraction};
    }

    std::size_t& visits_;
    long long bits_;
    std::size_t depth_{};
    std::unordered_map<const Real*,Interval> memo_;
};

// Refines x at each precision of the schedule until `decided` accepts a
// bounded enclosure. False: undecided at the last precision. Throws
// RefineStop when a node cannot be refined or the visit budget runs out.
template<class Decided>
bool refine_until(const RealPtr& x,Decided&& decided){
    std::size_t visits=abi::exact_refine_max_visits;
    for(long long bits=abi::exact_refine_initial_bits;
        bits<=abi::exact_refine_max_bits;bits*=2){
        Refiner refiner(visits,bits);
        const auto interval=refiner.enclose(x);
        if(interval.bounded&&decided(interval))return true;
    }
    return false;
}

// A provider's answer about its own atom or operation node.
int provider_answer(const RealPtr& x,std::uint32_t query){
    if(!x||(x->kind!=RealKind::Atom&&x->kind!=RealKind::Op))return QCORE_EXACT_UNKNOWN;
    const auto provider=exact_provider_v2(x->provider);
    if(!provider||!provider->prove)return QCORE_EXACT_UNKNOWN;
    std::vector<void*> handles;
    handles.reserve(x->args.size());
    quidra::runtime::NodeRuntimeOwner handles_owner(&handles,+[](void* raw){
        auto& owned=*static_cast<std::vector<void*>*>(raw);
        for(auto& handle:owned){
            auto* value=std::exchange(handle,nullptr);
            if(value)quidra::runtime::cleanup_node_slot(release_br,value);
        }
    });
    for(const auto& argument:x->args)handles.push_back(make_br(argument));
    const int answer=provider->prove(x->opcode,const_cast<const void* const*>(handles.data()),
                                     static_cast<std::uint32_t>(handles.size()),query);
    for(auto& handle:handles)release_br(std::exchange(handle,nullptr));
    handles_owner.relinquish();
    return answer;
}

// The sign of x, certified: -1, 0 or 1. Throws RefineStop when it cannot be.
int certified_sign(const RealPtr& x){
    if(x->kind==RealKind::Rational||x->kind==RealKind::Infinity)return x->rational.num.sign;
    int sign=0;
    const bool decided=refine_until(x,[&](const Interval& interval){
        if(dyadic_sign(interval.lo)>0){sign=1;return true;}
        if(dyadic_sign(interval.hi)<0){sign=-1;return true;}
        if(!dyadic_sign(interval.lo)&&!dyadic_sign(interval.hi)){sign=0;return true;}
        return false;
    });
    if(decided)return sign;
    if(provider_answer(x,QCORE_EXACT_QUERY_ZERO)==QCORE_EXACT_YES)return 0;
    if(provider_answer(x,QCORE_EXACT_QUERY_POSITIVE)==QCORE_EXACT_YES)return 1;
    if(provider_answer(x,QCORE_EXACT_QUERY_NEGATIVE)==QCORE_EXACT_YES)return -1;
    throw RefineStop{abi::FailureReason::exact_zero_undecided};
}
// base ^ (p/q) for a fraction p/q: defined for a positive base (and 0 for a
// positive exponent); exact when the base is a rational with a rational
// root, else a factor of a product with that exponent.
RealPtr rational_power_of(RealPtr base,const Rational& exponent,
                          unsigned long long line,unsigned long long column){
    int sign=0;
    if(base->kind==RealKind::Rational){
        sign=base->rational.num.sign;
    }else{
        try{
            sign=certified_sign(base);
        }catch(const RefineStop& stop){
            exact_unproven(stop.reason,"exact power domain could not be proven",line,column);
        }
    }
    if(sign<0)
        quidra::runtime::report_failure(abi::FailureReason::negative_base_fractional_exponent,
                                        {},line,column);
    if(sign==0){
        if(exponent.num.sign<0)return infinity(1);
        return rr({});
    }
    if(base->kind==RealKind::Rational){
        if(rational_is_one(base->rational))return base;
        unsigned long long degree=0;
        if(bigint_try_u64_value(exponent.den,64,degree)&&degree<=max_root_degree){
            if(const auto root=exact_root(base->rational,static_cast<unsigned>(degree)))
                return rr(rational_power(*root,exponent.num));
        }
        return make_product(rational_one(),{{base,exponent}});
    }
    if(base->kind==RealKind::Product)return power_node(std::move(base),rr(exponent));
    return make_product(rational_one(),{{base,exponent}});
}

// Whether x is defined (every divisor and domain argument decided), certified.
void certify_defined(const RealPtr& x){
    std::size_t budget=8192;
    if(provably_defined(x,budget))return;
    if(!refine_until(x,[](const Interval&){return true;}))
        throw RefineStop{abi::FailureReason::exact_zero_undecided};
}

// Keep proof-budget failures on the ordinary exact error channel rather
// than allowing a C++ refinement exception to escape an arithmetic entry.
int arithmetic_sign(const RealPtr& value){
    try{return certified_sign(value);}
    catch(const RefineStop& stop){exact_unproven(stop.reason,"exact arithmetic sign could not be proven");}
}
void arithmetic_defined(const RealPtr& value){
    try{certify_defined(value);}
    catch(const RefineStop& stop){exact_unproven(stop.reason,"exact arithmetic operand could not be proven defined");}
}

// Correct rounding into an IEEE binary format, round to nearest, ties to
// even, subnormals included.
struct IeeeFormat{int precision;int min_exponent;int max_exponent;};
constexpr IeeeFormat binary64_format{53,-1022,1023};
constexpr IeeeFormat binary32_format{24,-126,127};

// sign * (magnitude + δ) * 2^e, where 0 <= δ < 1 and sticky says δ > 0,
// rounded to the format. False when it rounds beyond the largest finite
// value.
bool round_to_format(const BigInt& magnitude,long long e,bool sticky,int sign,
                     const IeeeFormat& format,double& out){
    if(!magnitude.sign){out=sign<0?-0.0:0.0;return true;}
    const long long n=bit_length(magnitude);
    const long long exponent=n-1+e;
    // The weight of the last bit the format keeps for this value.
    const long long last=std::max(exponent-(format.precision-1),
                                  static_cast<long long>(format.min_exponent)-(format.precision-1));
    const auto one=BigInt::from_u64(1);
    BigInt mantissa;
    long long weight=last;
    if(last<=e){
        // Every bit of the magnitude is kept; δ is below half a unit.
        mantissa=shift_left(magnitude,static_cast<unsigned long long>(e-last));
    }else{
        const auto shift=static_cast<unsigned long long>(last-e);
        auto qr=divmod(magnitude,shift_left(one,shift));
        mantissa=std::move(qr.first);
        const int half=cmp(qr.second,shift_left(one,shift-1));
        const bool odd=mantissa.sign&&(mantissa.limbs[0]&1U);
        if(half>0||(half==0&&(sticky||odd)))mantissa=add(mantissa,one);
    }
    if(bit_length(mantissa)>format.precision){
        bool inexact=false;
        mantissa=shift_right_magnitude(mantissa,1,inexact);
        ++weight;
    }
    if(weight+format.precision-1>format.max_exponent)return false;
    unsigned long long bits=0;
    if(!bigint_try_u64_value(mantissa,64,bits))return false;
    const double value=std::ldexp(static_cast<double>(bits),static_cast<int>(weight));
    out=sign<0?-value:value;
    return true;
}
bool rational_to_format(const Rational& r,const IeeeFormat& format,double& out){
    if(!r.num.sign){out=0.0;return true;}
    const auto magnitude=absbi(r.num);
    // A quotient with at least precision + 2 bits, and whether it is exact.
    long long shift=format.precision+2+bit_length(r.den)-bit_length(magnitude);
    if(shift<0)shift=0;
    auto qr=divmod(shift_left(magnitude,static_cast<unsigned long long>(shift)),r.den);
    return round_to_format(qr.first,-shift,qr.second.sign!=0,r.num.sign,format,out);
}
bool dyadic_to_format(const Dyadic& x,const IeeeFormat& format,double& out){
    return round_to_format(absbi(x.m),x.e,false,x.m.sign,format,out);
}
bool same_bits(double a,double b){
    return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);
}

// Why a conversion of a symbolic real to a fixed-width type failed, if it
// did: OUT_OF_RANGE (the value is certainly beyond the format), UNDECIDED.
// Certified: refinement until both ends of the enclosure round to the same
// value; the exact value is between them, so it rounds there too.
std::optional<abi::ConversionReason> real_to_format(const RealPtr& x,const IeeeFormat& format,
                                                    double& out,
                                                    abi::FailureReason* why=nullptr){
    if(const auto sign=infinity_sign(x)){
        out=sign<0?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity();
        return std::nullopt;
    }
    if(x->kind==RealKind::Rational){
        if(rational_to_format(x->rational,format,out))return std::nullopt;
        return abi::ConversionReason::out_of_range;
    }
    bool beyond=false;
    try{
        const bool decided=refine_until(x,[&](const Interval& interval){
            double low=0.0,high=0.0;
            const bool low_finite=dyadic_to_format(interval.lo,format,low);
            const bool high_finite=dyadic_to_format(interval.hi,format,high);
            if(!low_finite&&!high_finite&&
               dyadic_sign(interval.lo)==dyadic_sign(interval.hi)){
                beyond=true;
                return true;
            }
            if(low_finite&&high_finite&&same_bits(low,high)){
                out=low;
                return true;
            }
            return false;
        });
        if(decided)
            return beyond?std::optional<abi::ConversionReason>(abi::ConversionReason::out_of_range)
                         :std::nullopt;
        if(why)*why=abi::FailureReason::exact_zero_undecided;
    }catch(const RefineStop& stop){
        if(why)*why=stop.reason;
    }
    return abi::ConversionReason::undecided;
}

BigInt dyadic_floor(const Dyadic& x){
    if(x.e>=0)return shift_left(x.m,static_cast<unsigned long long>(x.e));
    return shift_right(x.m,static_cast<unsigned long long>(-x.e),Round::down);
}

// A symbolic real as an integer: NOT_INTEGRAL when refinement puts it
// strictly between two consecutive integers, the integer when its provider
// proves it integral and refinement isolates it, else UNDECIDED.
std::optional<abi::ConversionReason> real_to_integer(const RealPtr& x,BigInt& out){
    if(infinity_sign(x))return abi::ConversionReason::out_of_range;
    if(x->kind==RealKind::Rational){
        if(!rat_int(x->rational))return abi::ConversionReason::not_integral;
        out=x->rational.num;
        return std::nullopt;
    }
    const bool integral=provider_answer(x,QCORE_EXACT_QUERY_INTEGRAL)==QCORE_EXACT_YES;
    if(provider_answer(x,QCORE_EXACT_QUERY_INTEGRAL)==QCORE_EXACT_NO)
        return abi::ConversionReason::not_integral;
    bool not_integral=false;
    try{
        const bool decided=refine_until(x,[&](const Interval& interval){
            const auto low=dyadic_floor(interval.lo);
            const auto high=dyadic_floor(interval.hi);
            if(integral){
                // The one integer inside the enclosure.
                const auto low_ceiling=neg(dyadic_floor(dyadic_negate(interval.lo)));
                if(cmp(low_ceiling,high)!=0)return false;
                out=high;
                return true;
            }
            if(cmp(low,high)!=0)return false;
            if(dyadic_compare(interval.lo,Dyadic{low,0})==0)return false;
            not_integral=true;
            return true;
        });
        if(decided)
            return not_integral?std::optional<abi::ConversionReason>(abi::ConversionReason::not_integral)
                               :std::nullopt;
    }catch(const RefineStop&){
    }
    return abi::ConversionReason::undecided;
}

// The text of an exact value (the canonical text). Core lays it out and a
// provider supplies only the symbols of its atoms and operations:
//   a rational alone prints as a finite decimal when it has one (0.5), as
//   an exact fraction otherwise (1/3, -1/3), integral values with .0 (4.0);
//   inside an expression rationals print as integers or fractions;
//   coefficients are juxtaposed (2√3, 3π, π/2, √2/2), with · only between
//   two digits (3·2^(1/3));
//   a power with exponent 1/2 prints with the radical sign (√2, √(2/3)),
//   other powers as base^exponent (2^(1/3), π^2);
//   an operation prints as symbol(arguments);
//   a sum prints its constant first, then its terms in the canonical order
//   (1+√2, π/2-σ).
// A version-1 provider has no symbols: its atoms and operations print in
// the source form of their intrinsics, real.atom("math", 1).

std::string integer_text(const BigInt& value){return value.text();}

// "p/q", or "p" when q is 1.
std::string fraction_text(const Rational& r){
    if(rat_int(r))return integer_text(r.num);
    return integer_text(r.num)+"/"+integer_text(r.den);
}

// The decimal text of a rational alone: finite when its denominator has no
// prime factor but 2 and 5, else the fraction.
std::string rational_text(const Rational& r){
    if(!r.num.sign)return "0.0";
    if(rat_int(r))return integer_text(r.num)+".0";
    BigInt rest=r.den;
    std::size_t twos=0,fives=0;
    for(;;){
        auto qr=div_small(rest,2);
        if(qr.second)break;
        rest=std::move(qr.first);
        ++twos;
    }
    for(;;){
        auto qr=div_small(rest,5);
        if(qr.second)break;
        rest=std::move(qr.first);
        ++fives;
    }
    if(!(rest.sign==1&&rest.limbs.size()==1&&rest.limbs[0]==1))return fraction_text(r);
    const auto scale=std::max(twos,fives);
    const auto scaled=divmod(mul(absbi(r.num),pow10(scale)),r.den).first;
    std::string digits=integer_text(scaled);
    if(digits.size()<=scale)digits.insert(0,scale+1-digits.size(),'0');
    digits.insert(digits.size()-scale,".");
    return (r.num.sign<0?"-":"")+digits;
}

std::string node_text(const RealPtr& x);

bool starts_with_digit(const std::string& text){
    return !text.empty()&&text[0]>='0'&&text[0]<='9';
}
bool ends_with_digit(const std::string& text){
    return !text.empty()&&text.back()>='0'&&text.back()<='9';
}
bool source_form(const std::string& text){return text.rfind("real.",0)==0;}
// Items written side by side, with · where juxtaposition would be ambiguous.
std::string juxtapose(const std::vector<std::string>& items){
    std::string out;
    for(const auto& item:items){
        if(!out.empty()&&((ends_with_digit(out)&&starts_with_digit(item))||
                          source_form(item)||source_form(out)))
            out+="·";
        out+=item;
    }
    return out;
}

std::string provider_symbol(const RealPtr& x){
    if(const auto provider=exact_provider_v2(x->provider)){
        const char* symbol=provider->spelling(x->opcode);
        if(symbol&&*symbol)return symbol;
    }
    return {};
}
std::string quoted_text(const std::string& text){return "\""+text+"\"";}

std::string atom_text(const RealPtr& x){
    auto symbol=provider_symbol(x);
    if(!symbol.empty())return symbol;
    return "real.atom("+quoted_text(x->provider)+", "+std::to_string(x->opcode)+")";
}
std::string operation_text(const RealPtr& x){
    std::string arguments;
    for(std::size_t i=0;i<x->args.size();++i){
        if(i)arguments+=", ";
        arguments+=node_text(x->args[i]);
    }
    auto symbol=provider_symbol(x);
    if(!symbol.empty())return symbol+"("+arguments+")";
    return std::string(x->args.size()==1?"real.unary(":"real.op(")+quoted_text(x->provider)+", "+
           std::to_string(x->opcode)+", "+arguments+")";
}

// A value as an operand of ^ or a factor that would otherwise run into its
// neighbours: parenthesized unless it is one atom, operation, radical or
// non-negative integer.
std::string operand_text(const RealPtr& x){
    if(x->kind==RealKind::Rational){
        if(rat_int(x->rational)&&x->rational.num.sign>=0)return integer_text(x->rational.num);
        return "("+fraction_text(x->rational)+")";
    }
    const auto text=node_text(x);
    if(x->kind==RealKind::Atom||x->kind==RealKind::Op)return text;
    if(x->kind==RealKind::Power&&text.rfind("√",0)==0)return text;
    return "("+text+")";
}
std::string exponent_text(const RealPtr& x){
    if(x->kind==RealKind::Rational&&rat_int(x->rational)&&x->rational.num.sign>=0)
        return integer_text(x->rational.num);
    return operand_text(x);
}
std::string power_text(const RealPtr& base,const RealPtr& exponent){
    if(exponent->kind==RealKind::Rational&&
       !rat_cmp(exponent->rational,Rational(BigInt::from_u64(1),BigInt::from_u64(2)))){
        if(base->kind==RealKind::Rational&&rat_int(base->rational)&&base->rational.num.sign>0)
            return "√"+integer_text(base->rational.num);
        return "√("+(base->kind==RealKind::Rational?fraction_text(base->rational):node_text(base))+")";
    }
    return operand_text(base)+"^"+exponent_text(exponent);
}
// A base raised to a positive integer exponent, as a factor.
std::string factor_text(const RealPtr& base,const BigInt& exponent){
    const bool one=exponent.sign==1&&exponent.limbs.size()==1&&exponent.limbs[0]==1;
    std::string text;
    if(base->kind==RealKind::Sum||base->kind==RealKind::Product)text="("+node_text(base)+")";
    else if(base->kind==RealKind::Power&&!one)text="("+node_text(base)+")";
    else text=node_text(base);
    return one?text:text+"^"+integer_text(exponent);
}
// base^exponent for a positive fraction: the radical sign for 1/2
// (√2, √(2/3), √π), else a power (2^(1/3), π^(3/2)).
std::string fractional_power_text(const RealPtr& base,const Rational& exponent){
    if(!rat_cmp(exponent,Rational(BigInt::from_u64(1),BigInt::from_u64(2)))){
        if(base->kind==RealKind::Rational)
            return rat_int(base->rational)?"√"+integer_text(base->rational.num)
                                          :"√("+fraction_text(base->rational)+")";
        if(base->kind==RealKind::Atom||base->kind==RealKind::Op)return "√"+node_text(base);
        return "√("+node_text(base)+")";
    }
    return operand_text(base)+"^("+fraction_text(exponent)+")";
}
// coefficient * factors (factor, integer exponent), without its sign when
// unsigned is set; negative exponents go below the fraction bar.
std::string product_text(const Rational& coefficient,const RealTerms& factors,bool unsigned_text){
    std::vector<std::string> numerator,denominator;
    const auto magnitude=absbi(coefficient.num);
    for(const auto& [base,exponent]:factors){
        if(!rat_int(exponent)){
            auto absolute_exponent=exponent;
            absolute_exponent.num=absbi(absolute_exponent.num);
            (exponent.num.sign>0?numerator:denominator).push_back(fractional_power_text(base,absolute_exponent));
        }
        else if(exponent.num.sign>0)numerator.push_back(factor_text(base,exponent.num));
        else if(exponent.num.sign<0)denominator.push_back(factor_text(base,absbi(exponent.num)));
        else numerator.push_back(factor_text(base,exponent.num)+"^0");
    }
    const bool unit=magnitude.sign==1&&magnitude.limbs.size()==1&&magnitude.limbs[0]==1;
    if(!unit||numerator.empty())numerator.insert(numerator.begin(),integer_text(magnitude));
    const bool unit_den=coefficient.den.limbs.size()==1&&coefficient.den.limbs[0]==1;
    if(!unit_den)denominator.insert(denominator.begin(),integer_text(coefficient.den));
    std::string text=juxtapose(numerator);
    if(!denominator.empty()){
        const auto below=juxtapose(denominator);
        text+="/"+(denominator.size()>1?"("+below+")":below);
    }
    if(!unsigned_text&&coefficient.num.sign<0)text="-"+text;
    return text;
}
// A coefficient times a monomial (a term of a sum).
std::string term_text(const RealPtr& monomial,const Rational& coefficient,bool unsigned_text){
    if(monomial->kind==RealKind::Product)return product_text(coefficient,monomial->terms,unsigned_text);
    return product_text(coefficient,{{monomial,rational_one()}},unsigned_text);
}

std::string node_text(const RealPtr& x){
    switch(x->kind){
        case RealKind::Infinity:return x->rational.num.sign<0?"-∞":"∞";
        case RealKind::Rational:
            return rat_int(x->rational)?integer_text(x->rational.num)
                                       :fraction_text(x->rational);
        case RealKind::Atom:return atom_text(x);
        case RealKind::Op:return operation_text(x);
        case RealKind::Power:return power_text(x->args[0],x->args[1]);
        case RealKind::Product:return product_text(x->rational,x->terms,false);
        case RealKind::Sum:{
            std::string text;
            if(x->rational.num.sign)text=fraction_text(x->rational);
            for(const auto& [term,coefficient]:x->terms){
                const bool negative=coefficient.num.sign<0;
                if(!text.empty())text+=negative?"-":"+";
                else if(negative)text+="-";
                text+=term_text(term,coefficient,true);
            }
            return text;
        }
    }
    return {};
}

// The canonical text of a value.
std::string real_text(const RealPtr& x){
    if(x->kind==RealKind::Rational)return rational_text(x->rational);
    return node_text(x);
}

// ---- Interpolation formats: frac=N, sig=N, int=N, zero.

// The integer nearest to x * 10^fraction (fraction may be negative), ties
// to even: exact for a rational, certified by refinement otherwise.
BigInt round_scaled_rational(const Rational& r,long long fraction){
    BigInt num=r.num,den=r.den;
    if(fraction>=0)num=mul(num,pow10(static_cast<std::size_t>(fraction)));
    else den=mul(den,pow10(static_cast<std::size_t>(-fraction)));
    auto qr=divmod(absbi(num),den);
    const int half=cmp(mul_small(qr.second,2),den);
    const bool odd=qr.first.sign&&(qr.first.limbs[0]&1U);
    auto q=std::move(qr.first);
    if(half>0||(half==0&&odd))q=add(q,BigInt::from_u64(1));
    return num.sign<0?neg(std::move(q)):q;
}
Rational dyadic_rational(const Dyadic& x){
    if(x.e>=0)return Rational(shift_left(x.m,static_cast<unsigned long long>(x.e)),BigInt::from_u64(1));
    return Rational(x.m,shift_left(BigInt::from_u64(1),static_cast<unsigned long long>(-x.e)));
}
struct DigitsStop{abi::FailureReason reason;};
BigInt round_scaled(const RealPtr& x,long long fraction){
    if(x->kind==RealKind::Rational)return round_scaled_rational(x->rational,fraction);
    BigInt result;
    try{
        const bool decided=refine_until(x,[&](const Interval& interval){
            auto low=round_scaled_rational(dyadic_rational(interval.lo),fraction);
            auto high=round_scaled_rational(dyadic_rational(interval.hi),fraction);
            if(cmp(low,high)!=0)return false;
            result=std::move(low);
            return true;
        });
        if(!decided)throw DigitsStop{abi::FailureReason::exact_zero_undecided};
    }catch(const RefineStop& stop){
        throw DigitsStop{stop.reason};
    }
    return result;
}
// The decimal exponent of a nonzero value: k with 10^k <= |x| < 10^(k+1).
long long decimal_exponent_rational(const Rational& r){
    const auto magnitude=absbi(r.num);
    long long k=static_cast<long long>(integer_text(magnitude).size())-
                static_cast<long long>(integer_text(r.den).size());
    const auto at_least=[&](long long power){  // |r| >= 10^power
        if(power>=0)return cmp(magnitude,mul(r.den,pow10(static_cast<std::size_t>(power))))>=0;
        return cmp(mul(magnitude,pow10(static_cast<std::size_t>(-power))),r.den)>=0;
    };
    while(!at_least(k))--k;
    while(at_least(k+1))++k;
    return k;
}
long long decimal_exponent(const RealPtr& x){
    if(x->kind==RealKind::Rational)return decimal_exponent_rational(x->rational);
    long long result=0;
    try{
        const bool decided=refine_until(x,[&](const Interval& interval){
            if(dyadic_sign(interval.lo)!=dyadic_sign(interval.hi)||!dyadic_sign(interval.lo))
                return false;
            const auto low=decimal_exponent_rational(dyadic_rational(interval.lo));
            const auto high=decimal_exponent_rational(dyadic_rational(interval.hi));
            if(low!=high)return false;
            result=low;
            return true;
        });
        if(!decided)throw DigitsStop{abi::FailureReason::exact_zero_undecided};
    }catch(const RefineStop& stop){
        throw DigitsStop{stop.reason};
    }
    return result;
}
// digits * 10^-fraction as decimal text with `fraction` places.
std::string scaled_text(const BigInt& digits,long long fraction,bool negative){
    std::string text=integer_text(absbi(digits));
    if(fraction<0){
        text.append(static_cast<std::size_t>(-fraction),'0');
    }else if(fraction>0){
        const auto places=static_cast<std::size_t>(fraction);
        if(text.size()<=places)text.insert(0,places+1-text.size(),'0');
        text.insert(text.size()-places,".");
    }
    return (negative?"-":"")+text;
}
std::string pad_integer_part(std::string text,int width,bool zero){
    if(width<0)return text;
    const std::size_t sign=!text.empty()&&text[0]=='-'?1:0;
    auto end=text.find('.');
    if(end==std::string::npos)end=text.size();
    const auto digits=end>sign?end-sign:0;
    if(digits>=static_cast<std::size_t>(width))return text;
    const auto count=static_cast<std::size_t>(width)-digits;
    if(zero)text.insert(sign,count,'0');
    else text.insert(0,count,' ');
    return text;
}
// A sign of x for the text: negative values keep their sign when they
// round to zero, as fixed-width reals do.
bool negative_value(const RealPtr& x){
    if(x->kind==RealKind::Rational)return x->rational.num.sign<0;
    try{
        return certified_sign(x)<0;
    }catch(const RefineStop& stop){
        throw DigitsStop{stop.reason};
    }
}
std::string real_format(const RealPtr& x,int integer_width,int fractional,int significant,
                        bool zero){
    if(infinity_sign(x))return real_text(x);
    std::string text;
    if(fractional>=0){
        const auto digits=round_scaled(x,fractional);
        text=scaled_text(digits,fractional,negative_value(x));
    }else if(significant>0){
        const bool is_zero=x->kind==RealKind::Rational&&!x->rational.num.sign;
        if(is_zero){
            text=scaled_text(BigInt(),significant-1,false);
        }else{
            auto k=decimal_exponent(x);
            auto fraction=static_cast<long long>(significant)-1-k;
            auto digits=round_scaled(x,fraction);
            // Rounding up to the next power of ten adds a digit.
            if(static_cast<long long>(integer_text(absbi(digits)).size())>significant){
                ++k;
                fraction=static_cast<long long>(significant)-1-k;
                digits=round_scaled(x,fraction);
            }
            text=scaled_text(digits,fraction,negative_value(x));
        }
    }else{
        text=real_text(x);
    }
    return pad_integer_part(std::move(text),integer_width,zero);
}

// Comparison: structurally equal values are equal once they are defined;
// otherwise the sign of the difference, certified by refinement.
int real_compare(const RealPtr&a,const RealPtr&b,
                 unsigned long long line,unsigned long long column){
    const int ai=infinity_sign(a),bi=infinity_sign(b);
    if(ai||bi){
        if(!ai)arithmetic_defined(a);
        if(!bi)arithmetic_defined(b);
        return ai<bi?-1:ai>bi?1:0;
    }
    try{
        std::size_t budget=8192;
        if(structurally_equal(a,b,budget)){
            certify_defined(a);
            return 0;
        }
        if(a->kind==RealKind::Rational&&b->kind==RealKind::Rational)
            return rat_cmp(a->rational,b->rational);
        return certified_sign(real_sub(a,b));
    }catch(const RefineStop& stop){
        exact_unproven(stop.reason,"exact comparison could not be proven",line,column);
    }
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
    const auto source=br(const_cast<void*>(input))->value;
    if(exact_provider_v2(name))return make_br(provider_node(name,opcode,{source}));
    if(infinity_sign(source))exact_fail("version-1 exact-real provider does not support infinity inputs");
    const auto registered=exact_unary_provider(name);
    if(!registered||!registered->evaluate||!registered->flags)
        exact_fail("exact-real unary provider is unavailable");
    const auto flags=registered->flags(opcode);
    std::size_t sign_budget=4096;
    const auto sign=proven_sign(source,sign_budget);
    if((flags&QCORE_EXACT_UNARY_DOMAIN_POSITIVE)&&
       (sign==ProvenSign::Negative||sign==ProvenSign::Zero||
        sign==ProvenSign::NonPositive))
        exact_fail("exact-real unary input is outside the provider domain");
    if((flags&QCORE_EXACT_UNARY_DOMAIN_NONNEGATIVE)&&
       sign==ProvenSign::Negative)
        exact_fail("exact-real unary input is outside the provider domain");
    return make_br(op_node(name,opcode,{source}));
}

extern "C" void* qcore_exact_real_atom(
    const char* provider,std::uint32_t opcode){
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!provider||!*provider)exact_fail("invalid exact-real provider");
    const std::string name(provider);
    if(exact_provider_v2(name))return make_br(provider_node(name,opcode,{}));
    Rational low,high;
    auto value=atom(name,opcode);
    if(!atom_bounds(value,low,high))
        exact_fail("exact-real provider or atom opcode is unavailable");
    return make_br(std::move(value));
}

extern "C" int qcore_exact_real_provider_register_v2(const qcore_exact_provider_v2* provider){
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!provider||provider->struct_size<sizeof(qcore_exact_provider_v2)||
       !provider->name||!*provider->name||!provider->spelling||!provider->arity)
        return 0;
    try{
        qcore_exact_provider_v2 copy=*provider;
        copy.struct_size=sizeof(qcore_exact_provider_v2);
        const std::string name(provider->name);
        std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
        auto& providers=exact_providers_v2();
        const auto found=providers.find(name);
        if(found!=providers.end()){
            const auto& known=found->second;
            return known.spelling==copy.spelling&&known.arity==copy.arity&&
                   known.flags==copy.flags&&known.canonicalize==copy.canonicalize&&
                   known.prove==copy.prove&&known.refine==copy.refine?1:0;
        }
        // The registered name must outlive the registration: keep the key's.
        auto inserted=providers.emplace(name,copy).first;
        inserted->second.name=inserted->first.c_str();
        return 1;
    }catch(...){
        return 0;
    }
}

extern "C" int qcore_exact_real_power_provider_register_v2(
    const char* name,std::uint32_t opcode){
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!name||!*name)return 0;
    try{
        const auto provider=exact_provider_v2(name);
        if(!provider||!provider->refine||!v2_owns(*provider,opcode,2))return 0;
        std::lock_guard<std::mutex> lock(exact_atom_provider_mutex());
        auto& known=exact_power_provider_slot();
        if(known)return known->name==name&&known->opcode==opcode?1:0;
        known=ExactPowerProvider{std::string(name),opcode};
        return 1;
    }catch(...){return 0;}
}

extern "C" void* qcore_exact_real_node(
    const char* provider,std::uint32_t opcode,const void* const* args,std::uint32_t n){
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!provider||!*provider||(n&&!args))exact_fail("invalid exact-real provider call");
    std::vector<RealPtr> arguments;
    arguments.reserve(n);
    for(std::uint32_t i=0;i<n;++i){
        if(!args[i])exact_fail("invalid exact-real provider call");
        arguments.push_back(br(const_cast<void*>(args[i]))->value);
    }
    return make_br(provider_node(provider,opcode,std::move(arguments)));
}

extern "C" void* qcore_exact_real_infinity(int sign){
    if(sign!=-1&&sign!=1)exact_fail("infinity sign must be -1 or 1");
    return make_br(infinity(sign));
}
extern "C" int qcore_exact_real_infinity_sign(const void* value){
    if(!value)exact_fail("null exact real");
    return infinity_sign(br(const_cast<void*>(value))->value);
}
extern "C" void* qcore_exact_real_rational(const char* numerator,const char* denominator){
    bool numerator_ok=false,denominator_ok=false;
    auto n=BigInt::parse(numerator?numerator:"",numerator_ok);
    auto d=BigInt::parse(denominator?denominator:"",denominator_ok);
    if(!numerator_ok||!denominator_ok)exact_fail("invalid exact rational text");
    if(!d.sign)exact_division_by_zero();
    return make_br(rr(Rational(std::move(n),std::move(d))));
}
extern "C" void* qcore_exact_real_add(const void* left,const void* right){
    if(!left||!right)exact_fail("null real operand");
    return make_br(real_add(br(const_cast<void*>(left))->value,br(const_cast<void*>(right))->value));
}
extern "C" void* qcore_exact_real_sub(const void* left,const void* right){
    if(!left||!right)exact_fail("null real operand");
    return make_br(real_sub(br(const_cast<void*>(left))->value,br(const_cast<void*>(right))->value));
}
extern "C" void* qcore_exact_real_mul(const void* left,const void* right){
    if(!left||!right)exact_fail("null real operand");
    return make_br(real_mul(br(const_cast<void*>(left))->value,br(const_cast<void*>(right))->value));
}
extern "C" void* qcore_exact_real_div(const void* left,const void* right){
    if(!left||!right)exact_fail("null real operand");
    return make_br(real_div(br(const_cast<void*>(left))->value,br(const_cast<void*>(right))->value));
}
extern "C" void* qcore_exact_real_pow(const void* base,const void* exponent){
    if(!base||!exponent)exact_fail("null real operand");
    return make_br(real_pow(br(const_cast<void*>(base))->value,br(const_cast<void*>(exponent))->value,0,0));
}
extern "C" void* qcore_exact_real_neg(const void* value){
    if(!value)exact_fail("null real operand");
    return make_br(real_neg(br(const_cast<void*>(value))->value));
}
extern "C" int qcore_exact_real_is_node(const void* value,const char* provider,std::uint32_t opcode){
    if(!value||!provider)return 0;
    const auto& node=br(const_cast<void*>(value))->value;
    return (node->kind==RealKind::Op||node->kind==RealKind::Atom)&&
           node->provider==provider&&node->opcode==opcode;
}
extern "C" int qcore_exact_real_rational_parts(
    const void* value,const char** numerator,const char** denominator){
    if(numerator)*numerator=nullptr;
    if(denominator)*denominator=nullptr;
    if(!value||!numerator||!denominator||numerator==denominator)return 0;
    const auto& node=br(const_cast<void*>(value))->value;
    if(node->kind!=RealKind::Rational)return 0;
    try{
        std::string new_numerator=node->rational.num.text();
        std::string new_denominator=node->rational.den.text();
        thread_local std::string numerator_text;
        thread_local std::string denominator_text;
        numerator_text.swap(new_numerator);
        denominator_text.swap(new_denominator);
        *numerator=numerator_text.c_str();
        *denominator=denominator_text.c_str();
        return 1;
    }catch(const std::bad_alloc&){
        return 0;
    }
}
extern "C" void* qcore_exact_real_argument(const void* value,std::uint32_t index){
    if(!value)return nullptr;
    const auto& node=br(const_cast<void*>(value))->value;
    if(node->kind!=RealKind::Op||index>=node->args.size())return nullptr;
    return make_br(node->args[index]);
}
extern "C" void* qcore_exact_real_retain(const void* value){
    if(!value)return nullptr;
    auto* handle=const_cast<void*>(value);
    quidra_managed_retain(handle);
    return handle;
}
extern "C" void qcore_exact_real_release(void* value){
    if(value)quidra_managed_release(value,reinterpret_cast<void*>(&quidra_bigreal_drop));
}

extern "C" double qcore_exact_real_atom_float64(
    const char* provider,std::uint32_t opcode){
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    if(!provider||!*provider)exact_fail("invalid exact-real provider");
    void* handle=qcore_exact_real_atom(provider,opcode);
    const auto value=br(handle)->value;
    release_br(handle);
    // The correctly rounded value, certified by refinement.
    double result=0.0;
    auto why=abi::FailureReason::exact_zero_undecided;
    if(const auto reason=real_to_format(value,binary64_format,result,&why)){
        if(*reason==abi::ConversionReason::out_of_range)
            exact_conversion_fail(*reason,QCORE_DTYPE_FLOAT64,0,0);
        exact_unproven(why,"exact rounding could not be proven");
    }
    return result;
}

#endif

#if QUIDRA_EXACT_SECTION == 13
extern "C" void quidra_bigreal_drop(void*p){
    if(p)br(p)->~BigRealValue();
}

#endif

#if QUIDRA_EXACT_SECTION == 15
extern "C" void* quidra_bigreal_literal(const char*text){
    if(!text)exact_fail("invalid real literal");
    if(const auto sign=infinity_text_sign(text))return make_br(infinity(sign));
    Rational rational;
    if(!parse_decimal(text,rational))exact_fail("invalid real literal");
    return make_br(rr(std::move(rational)));
}
// A decimal, or a fraction p/q with an optional sign: the text `real`
// prints for rationals. Symbolic text is not parseable.
static bool parse_fraction(std::string_view text,Rational& out){
    const auto slash=text.find('/');
    if(slash==std::string_view::npos)return false;
    bool numerator_ok=false,denominator_ok=false;
    auto numerator=BigInt::parse(text.substr(0,slash),numerator_ok);
    const auto denominator_text=text.substr(slash+1);
    if(denominator_text.empty()||denominator_text[0]=='+'||denominator_text[0]=='-')return false;
    auto denominator=BigInt::parse(denominator_text,denominator_ok);
    if(!numerator_ok||!denominator_ok||!denominator.sign)return false;
    out=Rational(std::move(numerator),std::move(denominator));
    return true;
}
extern "C" void* quidra_bigreal_parse(const char*text){
    if(text)if(const auto sign=infinity_text_sign(text))return make_br(infinity(sign));
    Rational rational;
    if(!text||!(parse_decimal(text,rational)||parse_fraction(text,rational)))return nullptr;
    return make_br(rr(std::move(rational)));
}
// The text of a real: its canonical text, or with significant > 0 that many
// significant digits.

#endif

#if QUIDRA_EXACT_SECTION == 17
static std::string real_text_or_digits(const RealPtr& value,int significant){
    try{
        return significant>0?real_format(value,-1,-1,significant,false):real_text(value);
    }catch(const DigitsStop& stop){
        exact_unproven(stop.reason,"exact digits could not be proven");
    }
}
extern "C" char* quidra_bigreal_text(void*p,int significant){
    if(!p)exact_fail("null real");
    return copy_text(real_text_or_digits(br(p)->value,significant));
}


#endif

#if QUIDRA_EXACT_SECTION == 19
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
    return make_br(real_pow(br(left)->value,br(right)->value,line,column));
}

#endif

#if QUIDRA_EXACT_SECTION == 21
extern "C" void* quidra_bigreal_from_i64(long long value){
    return make_br(rr(Rational(BigInt::from_i64(value),BigInt::from_u64(1))));
}
extern "C" void* quidra_bigreal_from_u64(unsigned long long value){
    return make_br(rr(Rational(BigInt::from_u64(value),BigInt::from_u64(1))));
}
extern "C" void* quidra_bigreal_from_float64(double value){
    if(std::isinf(value))return make_br(infinity(std::signbit(value)?-1:1));
    return make_br(rr(exact_double(value)));
}
extern "C" void* quidra_bigreal_from_bigint(void*p){
    if(!p)exact_fail("null bigint");
    return make_br(rr(Rational(bi(p)->value,BigInt::from_u64(1))));
}
// A real converts to an integer when it is one: a rational exactly, a
// symbolic value through refinement (NOT_INTEGRAL when it lies strictly
// between two integers, UNDECIDED when that cannot be shown).

#endif

#if QUIDRA_EXACT_SECTION == 23
extern "C" void* quidra_bigreal_try_bigint(void*p){
    if(!p)exact_fail("null real");
    BigInt integer;
    if(const auto reason=real_to_integer(br(p)->value,integer)){
        exact_conversion_declined(*reason);
        return nullptr;
    }
    return make_bi(std::move(integer));
}
extern "C" bool quidra_bigreal_try_i64(void*p,int bits,long long*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0;
    BigInt integer;
    if(const auto reason=real_to_integer(br(p)->value,integer))
        return exact_conversion_declined(*reason);
    if(!bigint_try_i64_value(integer,bits,*out))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    return true;
}
extern "C" bool quidra_bigreal_try_u64(
    void*p,int bits,unsigned long long*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0;
    BigInt integer;
    if(const auto reason=real_to_integer(br(p)->value,integer))
        return exact_conversion_declined(*reason);
    if(!bigint_try_u64_value(integer,bits,*out))
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
// A real converts to real64 or real32 as the correctly rounded value (to
// nearest, ties to even): a rational exactly, a symbolic value once
// refinement certifies the rounding. A value beyond the finite range is
// OUT_OF_RANGE; a rounding refinement cannot certify is UNDECIDED.

#endif

#if QUIDRA_EXACT_SECTION == 25
extern "C" bool quidra_bigreal_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0.0;
    double result=0.0;
    if(const auto reason=real_to_format(br(p)->value,binary64_format,result))
        return exact_conversion_declined(*reason);
    *out=result;
    return true;
}
extern "C" bool quidra_bigreal_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null real conversion storage");
    *out=0.0F;
    double result=0.0;
    if(const auto reason=real_to_format(br(p)->value,binary32_format,result))
        return exact_conversion_declined(*reason);
    *out=static_cast<float>(result);
    return true;
}
// Whether an exact array element converts (an array cast's validator); a
// declined element records why, as the try-conversions do.

#endif

#if QUIDRA_EXACT_SECTION == 27
extern "C" bool quidra_exact_numeric_cast_fits(
    void* p,int source_kind,int target_kind,int bits){
    if(!p)exact_fail("null exact numeric conversion storage");
    if(source_kind!=1&&source_kind!=2)return false;
    if(target_kind==1){
        if(source_kind==1)return true;
        BigInt integer;
        if(const auto reason=real_to_integer(br(p)->value,integer))
            return exact_conversion_declined(*reason);
        return true;
    }
    if(target_kind==7){
        if(source_kind==1)return bi(p)->value.sign>=0;
        BigInt integer;
        if(const auto reason=real_to_integer(br(p)->value,integer))
            return exact_conversion_declined(*reason);
        return integer.sign>=0||exact_conversion_declined(abi::ConversionReason::out_of_range);
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

// Round directly to the requested half format: rounding first to binary64
// can turn an exact value next to a half midpoint into a tie.
extern "C" bool quidra_half_try_real(
    long long numerator,long long denominator,int format,double* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0.0;
    const auto value=denominator==0
        ? br(reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator)))->value
        : rr(small_rational(numerator,denominator));
    const IeeeFormat target=format==0?IeeeFormat{11,-14,15}:IeeeFormat{8,-126,127};
    double result=0.0;
    if(const auto reason=real_to_format(value,target,result))
        return exact_conversion_declined(*reason);
    *out=result;
    return true;
}

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
    quidra::runtime::NodeRuntimeOwner partial(handle,release_br);
    if(!handle||!out)exact_fail("null real");
    const auto& value=br(handle)->value;
    if(value->kind==RealKind::Rational&&rational_small(value->rational,out)){
        partial.relinquish();
        quidra_managed_release(handle,real_drop_callback());
        return;
    }
    out->numerator=static_cast<long long>(reinterpret_cast<std::uintptr_t>(handle));
    out->denominator=0;
    partial.relinquish();
}
extern "C" bool quidra_real_node_cleanup_active(){
    return quidra::runtime::node_failure_active();
}
extern "C" void quidra_real_node_unary(const char* provider,int opcode,
    long long n,long long d,abi::ExactRealValue* out){
    auto* input=quidra_real_promote(n,d);
    quidra::runtime::NodeRuntimeOwner input_owner(input,release_br);
    auto* node=qcore_exact_real_unary(provider,opcode,input);
    quidra::runtime::NodeRuntimeOwner node_owner(node,release_br);
    input_owner.relinquish();release_br(input);
    node_owner.relinquish();quidra_real_demote(node,out);
}
extern "C" void quidra_real_node_binary(long long an,long long ad,
    long long bn,long long bd,int opcode,unsigned long long line,
    unsigned long long column,abi::ExactRealValue* out){
    auto* a=quidra_real_promote(an,ad);
    quidra::runtime::NodeRuntimeOwner a_owner(a,release_br);
    auto* b=quidra_real_promote(bn,bd);
    quidra::runtime::NodeRuntimeOwner b_owner(b,release_br);
    auto* result=quidra_bigreal_binary(a,b,opcode,line,column);
    quidra::runtime::NodeRuntimeOwner result_owner(result,release_br);
    a_owner.relinquish();release_br(a);
    b_owner.relinquish();release_br(b);
    result_owner.relinquish();quidra_real_demote(result,out);
}
extern "C" int quidra_real_node_compare(long long an,long long ad,
    long long bn,long long bd,unsigned long long line,unsigned long long column){
    auto* a=quidra_real_promote(an,ad);
    quidra::runtime::NodeRuntimeOwner a_owner(a,release_br);
    auto* b=quidra_real_promote(bn,bd);
    quidra::runtime::NodeRuntimeOwner b_owner(b,release_br);
    const auto result=quidra_bigreal_compare(a,b,line,column);
    a_owner.relinquish();release_br(a);
    b_owner.relinquish();release_br(b);
    return result;
}
extern "C" void quidra_real_node_pow(long long an,long long ad,
    long long bn,long long bd,unsigned long long line,unsigned long long column,
    abi::ExactRealValue* out){
    auto* a=quidra_real_promote(an,ad);
    quidra::runtime::NodeRuntimeOwner a_owner(a,release_br);
    auto* b=quidra_real_promote(bn,bd);
    quidra::runtime::NodeRuntimeOwner b_owner(b,release_br);
    auto* result=quidra_bigreal_pow(a,b,line,column);
    quidra::runtime::NodeRuntimeOwner result_owner(result,release_br);
    a_owner.relinquish();release_br(a);
    b_owner.relinquish();release_br(b);
    result_owner.relinquish();quidra_real_demote(result,out);
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
    return copy_text(real_text_or_digits(rr(small_rational(numerator,denominator)),significant));
}
extern "C" char* quidra_real_format(long long numerator,long long denominator,int integer_width,
                                    int fractional,int significant,int zero){
    const auto value=denominator==0
        ? br(reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator)))->value
        : rr(small_rational(numerator,denominator));
    try{
        return copy_text(real_format(value,integer_width,fractional,significant,zero!=0));
    }catch(const DigitsStop& stop){
        exact_unproven(stop.reason,"exact digits could not be proven");
    }
}
// A small rational is decided: it is an integer exactly when its
// denominator is 1 (else NOT_INTEGRAL), and it converts to a fixed-width
// type unless it is outside its range.
// The correctly rounded value of a small rational. When numerator and
// denominator are exact in the format's significand, one division of the
// format rounds correctly; otherwise the general rounding does.
extern "C" bool quidra_real_small_try_float64(
    long long numerator,long long denominator,double* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0.0;
    constexpr long long exact_limit=1LL<<53;
    if(numerator>=-exact_limit&&numerator<=exact_limit&&denominator<=exact_limit){
        *out=static_cast<double>(numerator)/static_cast<double>(denominator);
        return true;
    }
    double result=0.0;
    if(!rational_to_format(small_rational(numerator,denominator),binary64_format,result))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" bool quidra_real_small_try_float32(
    long long numerator,long long denominator,float* out){
    if(!out)exact_fail("null real conversion storage");
    *out=0.0F;
    constexpr long long exact_limit=1LL<<24;
    if(numerator>=-exact_limit&&numerator<=exact_limit&&denominator<=exact_limit){
        *out=static_cast<float>(numerator)/static_cast<float>(denominator);
        return true;
    }
    double result=0.0;
    if(!rational_to_format(small_rational(numerator,denominator),binary32_format,result))
        return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=static_cast<float>(result);
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

// real.op: generated code pushes the arguments (small or general values)
// onto a list, then applies the provider's operation to it.
extern "C" void* quidra_real_op_push(void* arguments,long long numerator,long long denominator){
    auto* list=arguments?static_cast<std::vector<RealPtr>*>(arguments)
                        :new std::vector<RealPtr>();
    quidra::runtime::NodeRuntimeOwner partial(arguments?nullptr:list,[](void* raw){
        delete static_cast<std::vector<RealPtr>*>(raw);
    });
    if(denominator==0){
        auto* handle=reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator));
        if(!handle)exact_fail("null real");
        list->push_back(br(handle)->value);
    }else{
        list->push_back(rr(small_rational(numerator,denominator)));
    }
    partial.relinquish();
    return list;
}
extern "C" void quidra_real_op_drop(void* arguments){
    delete static_cast<std::vector<RealPtr>*>(arguments);
}
extern "C" void* quidra_real_op_apply(const char* provider,unsigned int opcode,void* arguments){
    quidra::runtime::NodeRuntimeOwner partial(arguments,quidra_real_op_drop);
    quidra::runtime_parallel::reject_inside_parallel_body(__func__);
    std::unique_ptr<std::vector<RealPtr>> list(static_cast<std::vector<RealPtr>*>(arguments));
    partial.relinquish();
    if(!provider||!*provider||!list)exact_fail("invalid exact-real provider call");
    return make_br(provider_node(provider,opcode,std::move(*list)));
}

// A contextual constant's value in a fixed-width real type: the correctly
// rounded value of the exact value, certified by refinement, returned as a
// double (exact for real32, which the caller narrows without rounding).
extern "C" double quidra_real_round(long long numerator,long long denominator,int single,
                                    unsigned long long line,unsigned long long column){
    const auto value=denominator==0
        ? br(reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator)))->value
        : rr(small_rational(numerator,denominator));
    double result=0.0;
    auto why=abi::FailureReason::exact_zero_undecided;
    if(const auto reason=real_to_format(value,single?binary32_format:binary64_format,result,&why)){
        if(*reason==abi::ConversionReason::out_of_range)
            exact_conversion_fail(*reason,single?QCORE_DTYPE_FLOAT32:QCORE_DTYPE_FLOAT64,line,column);
        exact_unproven(why,"exact rounding could not be proven",line,column);
    }
    return result;
}


namespace {
BigInt loop_word_integer(long long word) {
    const auto bits=static_cast<unsigned long long>(word);
    if((bits&abi::bare_integer_layout::boxed_bit)==0)
        return BigInt::from_i64(word>>1);
    return bi(reinterpret_cast<void*>(static_cast<std::uintptr_t>(
        bits&~static_cast<unsigned long long>(abi::bare_integer_layout::boxed_bit))))->value;
}
BigInt rational_loop_count(const Rational& start,const Rational& end,const Rational& step,
                           bool descending,bool start_inclusive,bool end_inclusive,
                           unsigned long long line,unsigned long long column) {
    if(!step.num.sign)
        quidra::runtime::report_failure(abi::FailureReason::for_step_zero,{},line,column);
    if((step.num.sign<0)!=descending) {
        const auto text=rational_text(step);
        quidra::runtime::report_failure(descending?abi::FailureReason::for_step_descending:
            abi::FailureReason::for_step_ascending,
            abi::FailureArgs{.step_text=text},line,column);
    }
    const auto distance=descending?rat_sub(start,end):rat_sub(end,start);
    if(distance.num.sign<0 || (!distance.num.sign&&!end_inclusive)) return {};
    const Rational amount(absbi(step.num),step.den);
    const auto ratio=rat_div(distance,amount);
    auto qr=divmod(ratio.num,ratio.den);
    auto count=std::move(qr.first);
    if(end_inclusive || qr.second.sign) count=add(count,BigInt::from_u64(1));
    if(!start_inclusive && count.sign) count=sub(count,BigInt::from_u64(1));
    return count;
}
IeeeFormat loop_format(int format) {
    switch(format) {
        case 0:return {11,-14,15};
        case 1:return {8,-126,127};
        case 2:return binary32_format;
        default:return binary64_format;
    }
}
}

// All three doubles are exact widenings of already materialized operands.
// The count uses their dyadic values, before any candidate is rounded.
extern "C" long long quidra_fixed_real_loop_count(
    double start,double end,double step,bool descending,bool start_inclusive,
    bool end_inclusive,unsigned long long line,unsigned long long column) {
    return quidra_int_adopt(make_bi(rational_loop_count(exact_double(start),exact_double(end),
        exact_double(step),descending,start_inclusive,end_inclusive,line,column)));
}
extern "C" double quidra_fixed_real_loop_value(
    double start,double step,long long multiplier,int format,bool native_fma) {
    const auto multiplier_integer=loop_word_integer(multiplier);
    if(!multiplier_integer.sign&&start==0.0) return start;
    if(native_fma&&(format==2||format==3)) {
        unsigned long long j=0;
        if(!bigint_try_u64_value(multiplier_integer,64,j))
            exact_fail("a proved real loop multiplier did not fit its counter");
        return format==2?static_cast<double>(std::fma(static_cast<float>(j),
            static_cast<float>(step),static_cast<float>(start))):std::fma(static_cast<double>(j),step,start);
    }
    const auto candidate=rat_add(exact_double(start),rat_mul(exact_double(step),
        Rational(multiplier_integer,BigInt::from_u64(1))));
    double result=0.0;
    if(!rational_to_format(candidate,loop_format(format),result))
        exact_fail("an in-range real loop candidate did not fit its format");
    // A zero starting value is already materialized. Its first candidate
    // preserves that sign; cancellation of nonzero values is positive zero.

    return result;
}

namespace {
RealPtr loop_real(long long numerator,long long denominator) {
    return denominator==0?br(reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator)))->value:
        rr(small_rational(numerator,denominator));
}
BigInt floor_nonnegative(const Rational& value) {
    return divmod(value.num,value.den).first;
}
BigInt certified_loop_floor(const RealPtr& ratio,unsigned long long line,
                           unsigned long long column) {
    if(ratio->kind==RealKind::Rational) return floor_nonnegative(ratio->rational);
    BigInt floor;
    try {
        const bool decided=refine_until(ratio,[&](const Interval& interval) {
            if(dyadic_sign(interval.lo)<0) return false;
            auto low=floor_nonnegative(dyadic_rational(interval.lo));
            auto high=floor_nonnegative(dyadic_rational(interval.hi));
            if(cmp(low,high)==0) {floor=std::move(low);return true;}
            if(cmp(add(low,BigInt::from_u64(1)),high)==0) {
                auto boundary=rr(Rational(high,BigInt::from_u64(1)));
                std::size_t budget=8192;
                if(structurally_equal(ratio,boundary,budget)) {
                    certify_defined(ratio);floor=std::move(high);return true;
                }
            }
            return false;
        });
        if(decided) return floor;
    } catch(const RefineStop& stop) {
        exact_unproven(stop.reason,"exact loop count could not be proven",line,column);
    }
    exact_unproven(abi::FailureReason::exact_zero_undecided,
        "exact loop count could not be proven",line,column);
}
BigInt exact_loop_count(const RealPtr& start,const RealPtr& end,const RealPtr& step,
                       bool descending,bool start_inclusive,bool end_inclusive,
                       unsigned long long line,unsigned long long column) {
    const auto zero=rr({});
    const int sign=real_compare(step,zero,line,column);
    if(!sign) quidra::runtime::report_failure(abi::FailureReason::for_step_zero,{},line,column);
    if((sign<0)!=descending) {
        const auto text=real_text(step);
        quidra::runtime::report_failure(descending?abi::FailureReason::for_step_descending:
            abi::FailureReason::for_step_ascending,abi::FailureArgs{.step_text=text},line,column);
    }
    const auto distance=descending?real_sub(start,end):real_sub(end,start);
    const auto ordering=real_compare(distance,zero,line,column);
    if(ordering<0 || (!ordering&&!end_inclusive)) return {};
    const auto amount=descending?real_neg(step):step;
    const auto ratio=real_div(distance,amount);
    auto floor=certified_loop_floor(ratio,line,column);
    auto count=floor;
    if(end_inclusive || real_compare(ratio,rr(Rational(floor,BigInt::from_u64(1))),line,column)!=0)
        count=add(count,BigInt::from_u64(1));
    if(!start_inclusive&&count.sign) count=sub(count,BigInt::from_u64(1));
    return count;
}
void loop_store_real(const RealPtr& value,abi::ExactRealValue* out) {
    if(value->kind==RealKind::Rational&&rational_small(value->rational,out)) return;
    out->numerator=static_cast<long long>(reinterpret_cast<std::uintptr_t>(make_br(value)));
    out->denominator=0;
}
}
// A managed int array: first numerator, stride numerator, denominator,
// count, general flag, machine-counter flag. Its ordinary array drop releases every integer word.
extern "C" void* quidra_exact_loop_plan(
    long long sn,long long sd,long long en,long long ed,long long tn,long long td,
    bool descending,bool start_inclusive,bool end_inclusive,
    unsigned long long line,unsigned long long column) {
    const auto start=loop_real(sn,sd),end=loop_real(en,ed),step=loop_real(tn,td);
    const bool rational=start->kind==RealKind::Rational&&end->kind==RealKind::Rational&&
                        step->kind==RealKind::Rational;
    auto count=rational?rational_loop_count(start->rational,end->rational,step->rational,
        descending,start_inclusive,end_inclusive,line,column):
        exact_loop_count(start,end,step,descending,start_inclusive,end_inclusive,line,column);
    std::array<BigInt,6> fields{};
    fields[3]=std::move(count);
    if(rational) {
        auto denominator=mul(divmod(start->rational.den,
            gcd(start->rational.den,step->rational.den)).first,step->rational.den);
        fields[0]=mul(start->rational.num,divmod(denominator,start->rational.den).first);
        fields[1]=mul(step->rational.num,divmod(denominator,step->rational.den).first);
        if(!start_inclusive) fields[0]=add(fields[0],fields[1]);
        fields[2]=std::move(denominator);
        const auto after=add(fields[0],mul(fields[3],fields[1]));
        long long a=0,b=0,z=0;
        if(bigint_try_i64_value(fields[0],64,a)&&bigint_try_i64_value(fields[1],64,b)&&
           bigint_try_i64_value(after,64,z)) fields[5]=BigInt::from_u64(1);
    } else fields[4]=BigInt::from_u64(1);
    struct PlanOwner {
        std::array<long long,6> words{};
        void* array{};
        bool transferred{};
        ~PlanOwner() {
            if(transferred) return;
            for(const auto word:words)
                if(static_cast<unsigned long long>(word)&abi::bare_integer_layout::boxed_bit)
                    quidra_managed_release(reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                        static_cast<unsigned long long>(word)&~static_cast<unsigned long long>(abi::bare_integer_layout::boxed_bit))),
                        reinterpret_cast<void*>(&quidra_bigint_drop));
            quidra_managed_release(array,nullptr);
        }
    } owner;
    for(std::size_t i=0;i<fields.size();++i) owner.words[i]=quidra_int_adopt(make_bi(std::move(fields[i])));
    owner.array=quidra_managed_alloc(abi::array_layout::payload_offset+6*sizeof(long long));
    auto* array=static_cast<long long*>(owner.array);
    *array=6;
    quidra_init_create(array,6,sizeof(long long),abi::array_layout::payload_offset,1);
    auto* payload=reinterpret_cast<long long*>(reinterpret_cast<unsigned char*>(array)+abi::array_layout::payload_offset);
    std::copy(owner.words.begin(),owner.words.end(),payload);
    owner.transferred=true;
    return array;
}
extern "C" void quidra_exact_from_scaled(long long numerator,long long denominator,
                                         abi::ExactRealValue* out) {
    if((static_cast<unsigned long long>(numerator)&abi::bare_integer_layout::boxed_bit)==0&&
       (static_cast<unsigned long long>(denominator)&abi::bare_integer_layout::boxed_bit)==0&&denominator>0) {
        const auto n=numerator>>1,d=denominator>>1;
        auto a=n<0?0ULL-static_cast<unsigned long long>(n):static_cast<unsigned long long>(n);
        auto b=static_cast<unsigned long long>(d);
        while(b) {const auto remainder=a%b;a=b;b=remainder;}
        out->numerator=n/static_cast<long long>(a);
        out->denominator=d/static_cast<long long>(a);
        return;
    }
    loop_store_real(rr(Rational(loop_word_integer(numerator),loop_word_integer(denominator))),out);
}
extern "C" void quidra_exact_loop_value(long long sn,long long sd,long long tn,long long td,
                                        long long multiplier,abi::ExactRealValue* out) {
    loop_store_real(real_add(loop_real(sn,sd),real_mul(loop_real(tn,td),
        rr(Rational(loop_word_integer(multiplier),BigInt::from_u64(1))))),out);
}
extern "C" bool quidra_exact_scaled_convert(long long numerator,long long denominator,int format,
                                              double* out) {
    *out=0.0;
    if((format==2||format==3)&&
       (static_cast<unsigned long long>(numerator)&abi::bare_integer_layout::boxed_bit)==0&&
       (static_cast<unsigned long long>(denominator)&abi::bare_integer_layout::boxed_bit)==0&&denominator>0) {
        const auto n=numerator>>1,d=denominator>>1;
        const long long limit=format==2?16777216LL:9007199254740992LL;
        if(n>= -limit&&n<=limit&&d<=limit) {
            *out=format==2?static_cast<double>(static_cast<float>(n)/static_cast<float>(d)):
                static_cast<double>(n)/static_cast<double>(d);
            return true;
        }
    }
    return rational_to_format(Rational(loop_word_integer(numerator),loop_word_integer(denominator)),
                              loop_format(format),*out);
}
extern "C" int quidra_exact_scaled_compare(long long numerator,long long denominator,
                                           long long constant_n,long long constant_d) {
    if((static_cast<unsigned long long>(numerator)&abi::bare_integer_layout::boxed_bit)==0&&
       (static_cast<unsigned long long>(denominator)&abi::bare_integer_layout::boxed_bit)==0) {
        const auto product=[](long long value,long long positive,long long& out) {
            if(positive<=0 || value>std::numeric_limits<long long>::max()/positive ||
               value<std::numeric_limits<long long>::min()/positive) return false;
            out=value*positive;return true;
        };
        long long left=0,right=0;
        if(product(numerator>>1,constant_d,left)&&product(constant_n,denominator>>1,right))
            return left<right?-1:left>right?1:0;
    }
    return rat_cmp(Rational(loop_word_integer(numerator),loop_word_integer(denominator)),
                   small_rational(constant_n,constant_d));
}

// Detached values have an ordinary C++ owner, independent of every thread's
// managed allocation table. Attach creates a canonical managed value on the
// joining thread and consumes that detached owner.
extern "C" void quidra_real_detach(long long numerator,long long denominator,abi::ExactRealValue* out){
    if(!out)exact_fail("null real transfer");
    if(denominator!=0){*out={numerator,denominator};return;}
    auto* handle=reinterpret_cast<void*>(static_cast<std::uintptr_t>(numerator));
    if(!handle)exact_fail("null real transfer");
    auto value=std::make_unique<BigRealValue>(BigRealValue{br(handle)->value});
    quidra_managed_release(handle,real_drop_callback());
    out->numerator=static_cast<long long>(reinterpret_cast<std::uintptr_t>(value.release()));
    out->denominator=0;
}
extern "C" void quidra_real_attach(long long numerator,long long denominator,abi::ExactRealValue* out){
    if(!out)exact_fail("null real transfer");
    if(denominator!=0){*out={numerator,denominator};return;}
    std::unique_ptr<BigRealValue> value(reinterpret_cast<BigRealValue*>(static_cast<std::uintptr_t>(numerator)));
    if(!value)exact_fail("null real transfer");
    quidra_real_demote(make_br(import_real(value->value)),out);
}
extern "C" void quidra_real_discard_detached(long long numerator,long long denominator){
    if(denominator==0)delete reinterpret_cast<BigRealValue*>(static_cast<std::uintptr_t>(numerator));
}

#endif
