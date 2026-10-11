#include "indy/cpu/Fpu.hpp"

#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <type_traits>

// The host's rounding mode and exception flags are part of the result. This
// file is also built with strict floating-point semantics (see CMakeLists.txt)
// so the compiler neither folds nor reorders operations across fenv calls.
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace indy::fpu {

namespace {

template <class F> struct Traits;
template <> struct Traits<float> {
    using Bits = uint32_t;
    static constexpr Bits kExponent   = 0x7F80'0000;
    static constexpr Bits kFraction   = 0x007F'FFFF;
    static constexpr Bits kSignalling = 0x0040'0000; // MIPS: top fraction bit set = signalling
    static constexpr Bits kNan        = kDefaultNanS;
};
template <> struct Traits<double> {
    using Bits = uint64_t;
    static constexpr Bits kExponent   = 0x7FF0'0000'0000'0000;
    static constexpr Bits kFraction   = 0x000F'FFFF'FFFF'FFFF;
    static constexpr Bits kSignalling = 0x0008'0000'0000'0000;
    static constexpr Bits kNan        = kDefaultNanD;
};

template <class F> typename Traits<F>::Bits bitsOf(uint64_t raw) {
    return static_cast<typename Traits<F>::Bits>(raw);
}
template <class F> F toFloat(uint64_t raw) { return std::bit_cast<F>(bitsOf<F>(raw)); }
template <class F> uint64_t toRaw(F value) { return std::bit_cast<typename Traits<F>::Bits>(value); }

template <class F> bool isNan(uint64_t raw) {
    const auto b = bitsOf<F>(raw);
    return (b & Traits<F>::kExponent) == Traits<F>::kExponent && (b & Traits<F>::kFraction) != 0;
}
template <class F> bool isSignalling(uint64_t raw) {
    return isNan<F>(raw) && (bitsOf<F>(raw) & Traits<F>::kSignalling) != 0;
}

// Runs host arithmetic under the guest rounding mode (FCR31 bits 1:0) and
// collects the IEEE exceptions it raises as FCR31 cause bits.
class HostFpEnv {
public:
    explicit HostFpEnv(uint32_t roundingMode) : saved_(std::fegetround()) {
        static constexpr int kModes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
        std::fesetround(kModes[roundingMode & 3]);
        std::feclearexcept(FE_ALL_EXCEPT);
    }
    ~HostFpEnv() { std::fesetround(saved_); }
    HostFpEnv(const HostFpEnv&) = delete;
    HostFpEnv& operator=(const HostFpEnv&) = delete;

    [[nodiscard]] uint32_t cause() const {
        const int raised = std::fetestexcept(FE_ALL_EXCEPT);
        uint32_t c = 0;
        if ((raised & FE_INEXACT) != 0)   c |= kInexact;
        if ((raised & FE_UNDERFLOW) != 0) c |= kUnderflow;
        if ((raised & FE_OVERFLOW) != 0)  c |= kOverflow;
        if ((raised & FE_DIVBYZERO) != 0) c |= kDivideByZero;
        if ((raised & FE_INVALID) != 0)   c |= kInvalid;
        return c;
    }

private:
    int saved_;
};

// FS set: denormal results become zero of the same sign (inexact underflow).
template <class F> uint64_t flushDenormal(uint64_t raw, uint32_t fcr31, uint32_t& cause) {
    if ((fcr31 & kFcr31FlushToZero) != 0 && std::fpclassify(toFloat<F>(raw)) == FP_SUBNORMAL) {
        cause |= kUnderflow | kInexact;
        return toRaw<F>(std::signbit(toFloat<F>(raw)) ? F(-0.0) : F(0.0));
    }
    return raw;
}

// Host NaNs use the opposite quiet/signalling convention: never let one
// reach the guest.
template <class F> uint64_t guestResult(F value, uint32_t fcr31, uint32_t& cause) {
    if (std::isnan(value)) {
        cause |= kInvalid;
        return Traits<F>::kNan;
    }
    return flushDenormal<F>(toRaw<F>(value), fcr31, cause);
}

// ADD SUB MUL DIV SQRT ABS NEG (function codes 0..7, MOV excluded).
template <class F> Result arithmetic(uint32_t funct, uint64_t a, uint64_t b, uint32_t fcr31) {
    Result r{.wide = std::is_same_v<F, double>};
    const bool binary = funct <= 0x03;
    if (isNan<F>(a) || (binary && isNan<F>(b))) {
        // Signalling operand: invalid, default NaN. Quiet operands propagate (fs first).
        if (isSignalling<F>(a) || (binary && isSignalling<F>(b))) {
            r.cause = kInvalid;
            r.value = Traits<F>::kNan;
        } else {
            r.value = isNan<F>(a) ? a : b;
        }
        r.value = bitsOf<F>(r.value);
        return r;
    }
    F z{};
    {
        HostFpEnv env(fcr31);
        volatile F x = toFloat<F>(a);
        volatile F y = toFloat<F>(b);
        switch (funct) {
        case 0x00: z = x + y; break;
        case 0x01: z = x - y; break;
        case 0x02: z = x * y; break;
        case 0x03: z = x / y; break;
        case 0x04: z = std::sqrt(F{x}); break;
        case 0x05: z = std::fabs(F{x}); break;
        default:   z = -x; break; // 0x07 NEG
        }
        r.cause = env.cause();
    }
    r.value = guestResult<F>(z, fcr31, r.cause);
    return r;
}

template <class To, class From> Result convertFloat(uint64_t a, uint32_t fcr31) {
    Result r{.wide = std::is_same_v<To, double>};
    if (isNan<From>(a)) {
        r.cause = isSignalling<From>(a) ? kInvalid : 0;
        r.value = Traits<To>::kNan;
        return r;
    }
    To z{};
    {
        HostFpEnv env(fcr31);
        volatile From x = toFloat<From>(a);
        z = static_cast<To>(x);
        r.cause = env.cause();
    }
    r.value = guestResult<To>(z, fcr31, r.cause);
    return r;
}

template <class To, class Int> Result convertInteger(Int value, uint32_t fcr31) {
    Result r{.wide = std::is_same_v<To, double>};
    To z{};
    {
        HostFpEnv env(fcr31);
        volatile Int x = value;
        z = static_cast<To>(x);
        r.cause = env.cause();
    }
    r.value = toRaw<To>(z);
    return r;
}

// Float to W/L with rounding mode `rm`. NaN, infinity and out-of-range values
// are invalid and give the largest positive integer, as the kernel's
// emulation of the R4600's unimplemented-operation trap does.
template <class Int, class From> Result toInteger(uint64_t a, uint32_t rm) {
    using UInt = std::make_unsigned_t<Int>;
    Result r{.wide = sizeof(Int) == 8};
    const From x = toFloat<From>(a);
    const From limit = std::ldexp(From{1}, std::numeric_limits<Int>::digits); // 2^31 or 2^63
    From rounded{};
    if (!std::isnan(x)) {
        HostFpEnv env(rm);
        volatile From v = x;
        rounded = std::nearbyint(From{v}); // never raises inexact
    }
    if (std::isnan(x) || !(rounded >= -limit && rounded < limit)) {
        r.cause = kInvalid;
        r.value = static_cast<UInt>(std::numeric_limits<Int>::max());
        return r;
    }
    if (rounded != x) {
        r.cause = kInexact;
    }
    r.value = static_cast<UInt>(static_cast<Int>(rounded));
    return r;
}

template <class F> Comparison compareAs(uint32_t cond, uint64_t a, uint64_t b) {
    Comparison c;
    if (isNan<F>(a) || isNan<F>(b)) {
        c.condition = (cond & 1) != 0;
        if ((cond & 8) != 0 || isSignalling<F>(a) || isSignalling<F>(b)) {
            c.cause = kInvalid;
        }
        return c;
    }
    const F x = toFloat<F>(a);
    const F y = toFloat<F>(b);
    c.condition = ((cond & 4) != 0 && x < y) || ((cond & 2) != 0 && x == y);
    return c;
}

constexpr Result kUnimplementedResult{.cause = kUnimplemented};

} // namespace

Result operate(uint32_t funct, uint32_t fmt, uint64_t fs, uint64_t ft, uint32_t fcr31) {
    const bool single = fmt == kSingle;
    const bool dbl    = fmt == kDouble;
    const bool isFloat = single || dbl;

    if (funct <= 0x07) {
        if (!isFloat) return kUnimplementedResult;
        if (funct == 0x06) { // MOV: a bit copy, no exceptions
            return Result{.value = single ? (fs & 0xFFFF'FFFF) : fs, .wide = dbl};
        }
        return single ? arithmetic<float>(funct, fs, ft, fcr31) : arithmetic<double>(funct, fs, ft, fcr31);
    }

    if (funct <= 0x0F) {
        // ROUND/TRUNC/CEIL/FLOOR .L (0x08..0x0B) and .W (0x0C..0x0F). The low
        // two bits are the rounding mode in FCR31's own encoding.
        if (!isFloat) return kUnimplementedResult;
        const uint32_t rm = funct & 3;
        if (funct < 0x0C) {
            return single ? toInteger<int64_t, float>(fs, rm) : toInteger<int64_t, double>(fs, rm);
        }
        return single ? toInteger<int32_t, float>(fs, rm) : toInteger<int32_t, double>(fs, rm);
    }

    const auto word = static_cast<int32_t>(static_cast<uint32_t>(fs));
    const auto lng  = static_cast<int64_t>(fs);
    switch (funct) {
    case 0x20: // CVT.S
        if (dbl) return convertFloat<float, double>(fs, fcr31);
        if (fmt == kWord) return convertInteger<float>(word, fcr31);
        if (fmt == kLong) return convertInteger<float>(lng, fcr31);
        return kUnimplementedResult;
    case 0x21: // CVT.D
        if (single) return convertFloat<double, float>(fs, fcr31);
        if (fmt == kWord) return convertInteger<double>(word, fcr31);
        if (fmt == kLong) return convertInteger<double>(lng, fcr31);
        return kUnimplementedResult;
    case 0x24: // CVT.W
        if (!isFloat) return kUnimplementedResult;
        return single ? toInteger<int32_t, float>(fs, fcr31) : toInteger<int32_t, double>(fs, fcr31);
    case 0x25: // CVT.L
        if (!isFloat) return kUnimplementedResult;
        return single ? toInteger<int64_t, float>(fs, fcr31) : toInteger<int64_t, double>(fs, fcr31);
    default:
        return kUnimplementedResult;
    }
}

Comparison compare(uint32_t cond, uint32_t fmt, uint64_t fs, uint64_t ft) {
    if (fmt == kSingle) return compareAs<float>(cond & 0xF, fs, ft);
    if (fmt == kDouble) return compareAs<double>(cond & 0xF, fs, ft);
    return Comparison{.cause = kUnimplemented};
}

} // namespace indy::fpu
