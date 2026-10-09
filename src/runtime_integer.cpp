#include "runtime_integer.hpp"
#include "quidra/abi/exact_codes.hpp"
#include "quidra/abi/runtime_entry_points.hpp"

#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The ABI this runtime shares with generated code (include/quidra/abi).
namespace abi = quidra::abi;

using namespace quidra::runtime_integer;

extern "C" void quidra_bigint_drop(void*p){
    if(p)bi(p)->~BigIntValue();
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
extern "C" char* quidra_bigint_text(void*p){
    if(!p)exact_fail("null bigint");
    return copy_text(bi(p)->value.text());
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
extern "C" void* quidra_bigint_from_i64(long long value){
    return make_bi(BigInt::from_i64(value));
}
extern "C" void* quidra_bigint_from_u64(unsigned long long value){
    return make_bi(BigInt::from_u64(value));
}

// A bigint converts to a fixed-width type unless it is outside its range.
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
extern "C" bool quidra_bigint_try_float64(void*p,double*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0;
    long double value=0;
    try{value=std::stold(bi(p)->value.text());}
    catch(...){return exact_conversion_declined(abi::ConversionReason::out_of_range);}
    const double result=static_cast<double>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
extern "C" bool quidra_bigint_try_float32(void*p,float*out){
    if(!p||!out)exact_fail("null bigint conversion storage");
    *out=0.0F;
    double value=0.0;
    if(!quidra_bigint_try_float64(p,&value))return false;
    const float result=static_cast<float>(value);
    if(!std::isfinite(result))return exact_conversion_declined(abi::ConversionReason::out_of_range);
    *out=result;
    return true;
}
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
    double wide = 0.0;
    if (!quidra_int_try_float64(value, &wide)) return false;
    const float result = static_cast<float>(wide);
    if (!std::isfinite(result)) return false;
    *out = result;
    return true;
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
