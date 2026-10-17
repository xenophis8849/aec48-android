#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace fb {
// Inspect integer bits through a non-inlined pointer helper. Clang may fold
// an inlined floating-value check to true under -ffast-math.
static_assert(sizeof(float) == sizeof(std::uint32_t) && sizeof(double) == sizeof(std::uint64_t),
              "TalkHighpass requires 32-bit float and 64-bit double");
__attribute__((noinline)) inline bool talkHpfFiniteBits32(const void* p) {
    std::uint32_t bits;
    std::memcpy(&bits, p, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}
__attribute__((noinline)) inline bool talkHpfFiniteBits64(const void* p) {
    std::uint64_t bits;
    std::memcpy(&bits, p, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}
inline bool talkHpfFinite(float x) { return talkHpfFiniteBits32(&x); }
inline bool talkHpfFinite(double x) { return talkHpfFiniteBits64(&x); }

// Causal fourth-order Butterworth high-pass, implemented as two transposed
// direct-form-II biquads. Coefficients are calculated for the actual sample
// rate. The class has fixed storage and process() never allocates.
class TalkHighpass {
    struct Section {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double step(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    } s_[2];

    static double checkedStep(Section& s, double x) {
        if (!talkHpfFinite(x)) x = 0.0;
        if (!talkHpfFinite(s.z1) || !talkHpfFinite(s.z2)) s.z1 = s.z2 = 0.0;
        const double y = s.step(x);
        if (!talkHpfFinite(s.z1) || !talkHpfFinite(s.z2)) s.z1 = s.z2 = 0.0;
        return talkHpfFinite(y) ? y : 0.0;
    }

    float fastStep(float x) {
        return static_cast<float>(s_[1].step(s_[0].step(x)));
    }

    static void design(Section& s, double sampleRate, double q) {
        const double w = 2.0 * 3.1415926535897932384626433832795 * 80.0 / sampleRate;
        const double c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        s.b0 = (1.0 + c) * 0.5 / a0;
        s.b1 = -(1.0 + c) / a0;
        s.b2 = s.b0;
        s.a1 = -2.0 * c / a0;
        s.a2 = (1.0 - alpha) / a0;
        s.z1 = s.z2 = 0.0;
    }

public:
    bool init(double sampleRate) {
        if (!(sampleRate > 160.0)) return false;
        // Butterworth pole Q values for order four.
        design(s_[0], sampleRate, 0.541196100146197);
        design(s_[1], sampleRate, 1.306562964876377);
        return true;
    }

    void reset() {
        for (auto& s : s_) s.z1 = s.z2 = 0.0;
    }

    float step(float x) {
        const float y = static_cast<float>(checkedStep(s_[1], checkedStep(s_[0], x)));
        return talkHpfFinite(y) ? y : 0.f;
    }

    void process(const float* input, float* output, int n) {
        // Fully finite blocks use the original arithmetic path byte for byte.
        // Exceptional blocks repair bad samples and section state immediately.
        bool allFinite = true;
        for (int i = 0; i < n; ++i) allFinite &= talkHpfFinite(input[i]);
        for (const auto& s : s_) {
            allFinite &= talkHpfFinite(s.z1) && talkHpfFinite(s.z2);
        }
        if (allFinite) {
            for (int i = 0; i < n; ++i) output[i] = fastStep(input[i]);
            for (auto& s : s_) if (!talkHpfFinite(s.z1) || !talkHpfFinite(s.z2)) s.z1 = s.z2 = 0.0;
        } else {
            for (int i = 0; i < n; ++i) output[i] = step(input[i]);
        }
    }
};

}  // namespace fb
