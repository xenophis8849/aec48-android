// hint_v1 固定电平缩放自检：比较未缩放功率路径与掩码使用的幅度路径，容许浮点舍入及固定地板。
#include "fb/hint_v1.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    const int F = 769, N = 1536; bool ok = true;
    for (int sr : {48000, 44100}) for (float g : {0.37f, 1.f}) {
        const float g2 = g * g;
        fb::HintV1 raw, scaled, independent; independent.init(F, N, sr, 48, 12, 0.7f, 0.5f, 0.3f); raw.init(F, N, sr, 48, 12, 0.7f, 0.5f, 0.3f); scaled.init(F, N, sr, 48, 12, 0.7f, 0.5f, 0.3f);
        std::vector<float> pe(F), px(F), em(F), xm(F); double maxAbs = 0, maxRel = 0, normAbs = 0, normRel = 0; bool floorOk = true;
        for (int t = 0; t < 300; ++t) {
            for (int f = 0; f < F; ++f) {
                const bool silence = t >= 180 && t < 220;
                pe[f] = silence ? 0.f : 0.1f + 0.02f * (float)((f * 7 + t * 3) % 31);
                px[f] = silence ? 0.f : 0.2f + 0.03f * (float)((f * 11 + t * 5) % 23);
                em[f] = std::sqrt(pe[f] * g2 + 1e-12f); xm[f] = std::sqrt(px[f] * g2 + 1e-12f);
            }
            raw.hintStepPower(F, pe.data(), px.data()); independent.hintStepScaledPower(F, pe.data(), px.data(), g2); scaled.hintStep(F, em.data(), xm.data());
            for (int f = 0; f < F; ++f) {
                const int b = raw.hb[f]; const double a = raw.hOut[b] / raw.hcnt[b] * g2, c = (double)scaled.hm[f] * scaled.hm[f];
                const double old = raw.power(f), norm = independent.powerUnscaled(f, g2), de = std::fabs(old - norm);
                normAbs = std::max(normAbs, de); if (std::max(old, norm) > 1e-10) normRel = std::max(normRel, de / std::max(old, norm)); else floorOk = floorOk && de < 2e-14;
                const double e = std::fabs(a - c); maxAbs = std::max(maxAbs, e); if (std::max(a, c) > 1e-10) maxRel = std::max(maxRel, e / std::max(a, c)); else ok = ok && e < 2e-14;
            }
        }
        const bool pass = maxRel < 2e-6 && normRel < 2e-6 && floorOk && (g != 1.f || normAbs == 0); ok = ok && pass;
        std::printf("%s %d Hz 固定 g=%.2f 输入缩放：最大绝对差 %.9g，最大相对差 %.9g\n", pass ? "通过" : "不通过", sr, g, maxAbs, maxRel);
        std::printf("%s %d Hz 默认独立提示固定 g=%.2f 换算回原尺度：最大绝对差 %.9g，最大相对差 %.9g\n", pass ? "通过" : "不通过", sr, g, normAbs, normRel);
    }
    return ok ? 0 : 1;
}
