#pragma once

#include <cmath>

namespace fb {

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
        return static_cast<float>(s_[1].step(s_[0].step(x)));
    }

    void process(const float* input, float* output, int n) {
        for (int i = 0; i < n; ++i) output[i] = step(input[i]);
    }
};

}  // namespace fb
