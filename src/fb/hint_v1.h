// hint_v1 残余回声提示的共享状态与递推。
#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fb {
struct HintV1 {
    bool hint = false; int hNB = 0, hK = 0; float hDec = 0, hUp = 0, hDn = 0;
    std::vector<int> hb; std::vector<float> hcnt, hdk, hPe, hPx, hPxs, hHist, hm, xm;
    int hHead = 0, hFill = 0; double hPeak = 0; bool hSeen = false, hInit = false;
    std::vector<double> hNe, hL, hOut;

    void init(int F, int N, double sr, int nb, int K, float dec, float up, float dn, const int* bands = nullptr) {
        hint = true; hNB = nb; hK = K; hDec = dec; hUp = up; hDn = dn;
        hb.resize(F); hcnt.assign(hNB, 0.f);
        if (bands) for (int f = 0; f < F; ++f) hb[f] = bands[f];
        else {
            const double lo = 21.4 * std::log10(1.0 + 0.00437 * 40.0);
            const double hi = 21.4 * std::log10(1.0 + 0.00437 * (sr * 0.5));
            std::vector<double> edges(hNB + 1);
            for (int b = 0; b <= hNB; ++b) edges[b] = lo + (hi - lo) * b / hNB;
            for (int f = 0; f < F; ++f) {
                const double hz = (double)f * sr / N;
                const double e = 21.4 * std::log10(1.0 + 0.00437 * hz);
                int l = 0, r = hNB + 1; while (l < r) { const int m = (l + r) / 2; if (edges[m] <= e) l = m + 1; else r = m; }
                int b = l - 1;
                hb[f] = std::max(0, std::min(hNB - 1, b));
            }
        }
        for (int f = 0; f < F; ++f) hcnt[hb[f]] += 1.f;
        for (auto& c : hcnt) c = std::max(c, 1.f);
        hdk.assign(std::max(hK, 1), 1.f);
        for (int k = 1; k < hK; ++k) hdk[k] = (float)std::pow((double)hDec, (double)k);
        hPe.assign(hNB, 0); hPx.assign(hNB, 0); hPxs.assign(hNB, 0);
        hHist.assign((size_t)std::max(hK - 1, 1) * hNB, 0); hNe.assign(hNB, 0); hL.assign(hNB, 0); hOut.assign(hNB, 0);
        hm.assign(F, 0); xm.assign(F, 0);
    }
    void resetHint() {
        hHead = hFill = 0; hPeak = 0; hSeen = hInit = false;
        std::fill(hNe.begin(), hNe.end(), 0.0); std::fill(hL.begin(), hL.end(), 0.0);
    }
    // em/xm 为输入谱幅度；hm 输出带内均摊后的功率平方根。
    void hintStep(int F, const float* em, const float* xmIn) {
        std::fill(hPe.begin(), hPe.end(), 0.f); std::fill(hPx.begin(), hPx.end(), 0.f);
        for (int f = 0; f < F; ++f) { hPe[hb[f]] += em[f] * em[f]; hPx[hb[f]] += xmIn[f] * xmIn[f]; }
        hintFinish(F);
    }
    // 接收未缩放的逐频点功率，供联动接口直接使用。
    void hintStepPower(int F, const float* pe, const float* px) {
        std::fill(hPe.begin(), hPe.end(), 0.f); std::fill(hPx.begin(), hPx.end(), 0.f);
        for (int f = 0; f < F; ++f) { hPe[hb[f]] += pe[f]; hPx[hb[f]] += px[f]; }
        hintFinish(F);
    }
    // 输入为未缩放功率，但按 g² 在归一电平尺度上递推；不创建逐帧临时对象。
    void hintStepScaledPower(int F, const float* pe, const float* px, float g2) {
        std::fill(hPe.begin(), hPe.end(), 0.f); std::fill(hPx.begin(), hPx.end(), 0.f);
        for (int f = 0; f < F; ++f) { hPe[hb[f]] += pe[f] * g2; hPx[hb[f]] += px[f] * g2; }
        hintFinish(F);
    }
    float power(int f) const { return hOut[hb[f]] / hcnt[hb[f]]; }
    float powerUnscaled(int f, float g2) const { return power(f) / g2; }
    void hintFinish(int F) {
        float tot = 0; for (int b = 0; b < hNB; ++b) { hPxs[b] = hPx[b]; tot += hPx[b]; }
        for (int k = 1; k <= hFill; ++k) { const float* q = &hHist[(size_t)((hHead - k + (hK - 1)) % (hK - 1)) * hNB]; const float d = hdk[k]; for (int b = 0; b < hNB; ++b) hPxs[b] = std::max(hPxs[b], q[b] * d); }
        if (hK > 1) { memcpy(&hHist[(size_t)hHead * hNB], &hPx[0], hNB * 4); hHead = (hHead + 1) % (hK - 1); hFill = std::min(hFill + 1, hK - 1); }
        hPeak = std::max(hPeak, (double)tot); const bool a = (double)tot > hPeak * 1e-5 + 1e-12;
        if (!a) { for (int b = 0; b < hNB; ++b) hNe[b] = hSeen ? std::min(hNe[b] * 1.001, (double)hPe[b]) : (double)hPe[b]; hSeen = true; }
        for (int b = 0; b < hNB; ++b) {
            const double p = hPe[b], sx = hPxs[b], lr = 10.0 * std::log10(std::max(p - hNe[b], 1e-14) / (sx + 1e-14)), l = hL[b];
            double nl = lr < l ? l + hDn * (lr - l) : std::min(lr, l + (double)hUp); if (!hInit) nl = lr;
            if (a) hL[b] = nl;
            hOut[b] = 0;
        }
        if (a) hInit = true;
        if (a && hInit) for (int b = 0; b < hNB; ++b) hOut[b] = std::pow(10.0, hL[b] / 10.0) * (double)hPxs[b];
        for (int f = 0; f < F; ++f) hm[f] = std::sqrt((float)hOut[hb[f]] / hcnt[hb[f]] + 1e-16f);
    }
};
}
