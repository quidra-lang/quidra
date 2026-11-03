// Binary32 operations for bfloat storage on devices that flush denormals.
inline constexpr const char* msl_soft_float_body = R"MSL(
// qsoft_begin
#ifndef QUIDRA_SOFT_FLOAT
#define QUIDRA_SOFT_FLOAT
ulong qjam(ulong m,int n){if(n<=0)return m;if(n>=64)return ulong(m!=0);return (m>>n)|ulong((m&((1ul<<n)-1ul))!=0);}
ulong qnearest(ulong m,int n){if(n<=0)return m<<(-n);if(n>64)return 0;if(n==64)return ulong(m>0x8000000000000000ul);ulong v=m>>n,r=m&((1ul<<n)-1ul),h=1ul<<(n-1);return v+ulong(r>h||(r==h&&(v&1ul)));}
uint qpack(ulong m,int e,uint sign){
 if(m==0)return sign;
 int top=0;for(ulong v=m;v>1;v>>=1)++top;
 int exponent=top+e;
 if(exponent>127)return sign|0x7f800000u;
 if(exponent< -126){ulong v=qnearest(m,-149-e);return sign|uint(v);}
 ulong v=qnearest(m,top-23);if(v>=0x1000000ul){v>>=1;++exponent;}
 if(exponent>127)return sign|0x7f800000u;
 return sign|(uint(exponent+127)<<23)|(uint(v)&0x7fffffu);
}
ulong qmant(uint b){return ulong(b&0x7fffffu)|((b&0x7f800000u)?0x800000ul:0ul);}
int qexp(uint b){int e=int((b>>23)&255u);return e?e-150:-149;}
uint qadd_bits(uint a,uint b){
 uint aa=a&0x7fffffffu,bb=b&0x7fffffffu,sa=a&0x80000000u,sb=b&0x80000000u;
 if(aa>=0x7f800000u||bb>=0x7f800000u){if(aa>0x7f800000u||bb>0x7f800000u||(aa==bb&&sa!=sb))return 0x7fc00000u;return aa==0x7f800000u?a:b;}
 if(aa==0&&bb==0)return sa&sb;
 if(aa==bb&&sa!=sb)return 0u;
 if(aa<bb){uint t=a;a=b;b=t;t=aa;aa=bb;bb=t;t=sa;sa=sb;sb=t;}
 int e=qexp(a);ulong x=qmant(a)<<3,y=qjam(qmant(b)<<3,e-qexp(b));
 return qpack(sa==sb?x+y:x-y,e-3,sa);
}
uint qmul_bits(uint a,uint b){
 uint aa=a&0x7fffffffu,bb=b&0x7fffffffu,s=(a^b)&0x80000000u;
 if(aa>=0x7f800000u||bb>=0x7f800000u){if(aa>0x7f800000u||bb>0x7f800000u||aa==0||bb==0)return 0x7fc00000u;return s|0x7f800000u;}
 return qpack(qmant(a)*qmant(b),qexp(a)+qexp(b),s);
}
uint qdiv_bits(uint a,uint b){
 uint aa=a&0x7fffffffu,bb=b&0x7fffffffu,s=(a^b)&0x80000000u;
 if(aa>0x7f800000u||bb>0x7f800000u||(aa==0&&bb==0)||(aa==0x7f800000u&&bb==0x7f800000u))return 0x7fc00000u;
 if(aa==0x7f800000u||bb==0)return s|0x7f800000u;
 if(aa==0||bb==0x7f800000u)return s;
 ulong am=qmant(a),bm=qmant(b);int ae=qexp(a),be=qexp(b);
 while(am<0x800000ul){am<<=1;--ae;}while(bm<0x800000ul){bm<<=1;--be;}
 ulong numerator=am<<32,denominator=bm,v=numerator/denominator;
 v|=ulong(numerator%denominator!=0);
 return qpack(v,ae-be-32,s);
}
struct qf {
 uint bits;
 qf():bits(0){}
 qf(float v):bits(as_type<uint>(v)){}
 operator float() const{return as_type<float>(bits);}
};
qf qfromraw(uint b){qf v;v.bits=b;return v;}
uint qraw(qf v){return v.bits;}
bool qnan(qf v){return (v.bits&0x7fffffffu)>0x7f800000u;}
bool qfinite(qf v){return (v.bits&0x7f800000u)!=0x7f800000u;}
qf qabs(qf v){return qfromraw(v.bits&0x7fffffffu);}
qf operator-(qf v){return qfromraw(v.bits^0x80000000u);}
qf operator+(qf a,qf b){return qfromraw(qadd_bits(a.bits,b.bits));}
qf operator-(qf a,qf b){return qfromraw(qadd_bits(a.bits,b.bits^0x80000000u));}
qf operator*(qf a,qf b){return qfromraw(qmul_bits(a.bits,b.bits));}
qf operator/(qf a,qf b){return qfromraw(qdiv_bits(a.bits,b.bits));}
bool operator==(qf a,qf b){return ((a.bits|b.bits)&0x7fffffffu)==0||(a.bits==b.bits&&(a.bits&0x7fffffffu)<=0x7f800000u);}
bool operator!=(qf a,qf b){return !(a==b);}
bool operator<(qf a,qf b){if((a.bits&0x7fffffffu)>0x7f800000u||(b.bits&0x7fffffffu)>0x7f800000u||a==b)return false;uint x=(a.bits&0x80000000u)?~a.bits:(a.bits|0x80000000u),y=(b.bits&0x80000000u)?~b.bits:(b.bits|0x80000000u);return x<y;}
bool operator>(qf a,qf b){return b<a;}
bool operator<=(qf a,qf b){return a==b||a<b;}
bool operator>=(qf a,qf b){return a==b||a>b;}
#define QSOFT_MIXED(OP,RESULT) RESULT operator OP(qf a,float b){return a OP qf(b);} RESULT operator OP(float a,qf b){return qf(a) OP b;}
QSOFT_MIXED(+,qf) QSOFT_MIXED(-,qf) QSOFT_MIXED(*,qf) QSOFT_MIXED(/,qf)
QSOFT_MIXED(==,bool) QSOFT_MIXED(!=,bool) QSOFT_MIXED(<,bool) QSOFT_MIXED(>,bool) QSOFT_MIXED(<=,bool) QSOFT_MIXED(>=,bool)
#undef QSOFT_MIXED
thread qf& operator+=(thread qf& a,qf b){a=a+b;return a;}
qf qtrunc(qf a){uint m=a.bits&0x7fffffffu;int e=int(m>>23)-127;if(e<0)return qfromraw(a.bits&0x80000000u);if(e>=23)return a;return qfromraw(a.bits&~((1u<<(23-e))-1u));}
qf qpow(qf a,qf b){
 uint magnitude=a.bits&0x7fffffffu;
 if((b.bits&0x7fffffffu)==0)return qf(1.0f);
 if(b.bits==0x40000000u)return a*a;
 if(b.bits==0x40400000u)return (a*a)*a;
 uint sign=0;float bv=float(b);
 if((a.bits&0x80000000u)&&qtrunc(b)==b&&fabs(bv)<16777216.0f&&(int(bv)&1))sign=0x80000000u;
 if(magnitude==0)return qfromraw(sign|(b<0.0f?0x7f800000u:0u));
 float av=float(a),native=precise::pow(av,bv);uint raw=as_type<uint>(native);
 if((magnitude&0x7f800000u)!=0&&((raw&0x7f800000u)!=0||(raw&0x7fffffffu)>0x7f800000u))return qfromraw(raw);
 ulong mant=qmant(a.bits);int top=0;for(ulong v=mant;v>1;v>>=1)++top;
 float normalized=as_type<float>(0x3f800000u|(uint(mant<<(23-top))&0x7fffffu));
 float logarithm=precise::log2(normalized)+float(top+qexp(a.bits));
 float power=logarithm*bv;
 if(power>128.0f)return qfromraw(sign|0x7f800000u);
 if(power< -151.0f)return qfromraw(sign);
 int exponent=int(floor(power));float fraction=precise::exp2(power-float(exponent));
 uint fbits=as_type<uint>(fraction);return qfromraw(qpack(qmant(fbits),qexp(fbits)+exponent,sign));
}
struct qf2 {qf x,y;qf2(){} qf2(qf a,qf b):x(a),y(b){} qf2(qf v):x(v),y(v){} qf2(float2 v):x(v.x),y(v.y){} };
qf2 operator-(qf2 v){return qf2(-v.x,-v.y);}
bool2 qnan(qf2 v){return bool2(qnan(v.x),qnan(v.y));}
bool2 qfinite(qf2 v){return bool2(qfinite(v.x),qfinite(v.y));}
qf2 qpair(ulong b){return qf2(qfromraw(uint(b)),qfromraw(uint(b>>32)));}
#endif
// qsoft_end
)MSL";
