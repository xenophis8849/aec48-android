// 处理链的流式包装：线性级（fb/nlin.h）→ 残余回声抑制（fb/cofi.h），两级共用一次 STFT（N=1536 / H=384，sqrt-hann）。
// 每来 H 个样本推一帧：第 k 块到齐后算第 k−2 帧（窗 = 最近 4 块，开头不足补零），算完后第 k−4 块的输出定稿 → 输出比输入晚 3 块（1152 样本）。
// 提示版掩码（参数文件带 hint_v1，0.6 起，如 v62be20）另吃「对齐参考谱」：参考按线性级逐帧时延（lf 帧）挪到回归量起点后的同一 STFT，
//   = 评估里 nlin_bin.py 的第 4 个输出（每跳 j 的样本用第 j 帧的 lf）再做 STFT。窗内最后一跳（第 t+1 跳）的 lf 端上还没算出，用本帧 lf 代替——
//   时延不变时与评估逐样本相同，只在时延改判的那几帧有差别。参考历史按线性级最大时延 K 帧留够。
// mode：0 = 直通（输出麦克风，延迟相同）；1 = 只线性级；2 = 整链。切换不清状态。
// 频谱回调（0.5）：每帧输出频谱在合成前交给 hook 原地修改（各 mode 都调；第 1 块不调，第 2 块起每块一帧，第 c 次调用的窗覆盖输入 [c·H − 768, c·H + 768)，负索引补零）；调完把直流 / 奈奎斯特虚部置 0。
//   pre 非空时另做一路「回调前」的合成（录音用）：回调开着才多一次逆变换，没挂 / 关着直接复用同一帧（只多一次重叠相加）。
#pragma once
#include "fb/nlin.h"
#include "fb/cofi.h"
#include "pffft.h"
#include <vector>
#include <complex>
#include <cmath>
#include <cstring>

namespace ac {
typedef std::complex<float> cf;

struct RFft { int N = 0, M = 0; PFFFT_Setup* s = nullptr; float *in = nullptr, *out = nullptr, *wk = nullptr;
    RFft() = default; RFft(const RFft&) = delete; RFft& operator=(const RFft&) = delete;
    ~RFft() { release(); }
    void release() { if (s) pffft_destroy_setup(s); pffft_aligned_free(in); pffft_aligned_free(out); pffft_aligned_free(wk); s = nullptr; in = out = wk = nullptr; }
    bool init(int n) { release(); N = n; M = n / 2; s = pffft_new_setup(n, PFFFT_REAL); if (!s) return false;
        in = (float*)pffft_aligned_malloc(n * 4); out = (float*)pffft_aligned_malloc(n * 4); wk = (float*)pffft_aligned_malloc(n * 4);
        if (!in || !out || !wk) { release(); return false; } return true; }
    void fwd(const float* x, float* Xr, float* Xi) { memcpy(in, x, N * 4); pffft_transform_ordered(s, in, out, wk, PFFFT_FORWARD);
        Xr[0] = out[0]; Xi[0] = 0.f; Xr[M] = out[1]; Xi[M] = 0.f; for (int k = 1; k < M; ++k) { Xr[k] = out[2 * k]; Xi[k] = out[2 * k + 1]; } }
    void inv(const cf* X, float* x) { in[0] = X[0].real(); in[1] = X[M].real(); for (int k = 1; k < M; ++k) { in[2 * k] = X[k].real(); in[2 * k + 1] = X[k].imag(); }
        pffft_transform_ordered(s, in, out, wk, PFFFT_BACKWARD); const float sc = 1.f / N; for (int q = 0; q < N; ++q) x[q] = out[q] * sc; }
};

typedef void (*SpecHook)(void* user, float* re, float* im, int bins);

struct Chain {
    fb::NlinD lin; fb::Cofi cnet; bool hasMask = false;
    int N = 0, H = 0, F = 0; RFft rf;
    bool hint = false; std::vector<float> rr, xaw, XAr, XAi, sx; long long nIn = 0; int lfAt[4] = {0, 0, 0, 0};   // 提示版：参考环形历史、对齐参考窗 / 谱
    std::vector<float> win, mh, rh, xw, dw, Xr, Xi, Dr, Di, Er, Ei, se, sy, mo, acc, wacc, hr, hi, xp, accP; std::vector<cf> FX;
    double Pg = -1, GA = 0, GT = 0, sr = 48000; long k = 0;
    float lastLin = 0.f, lastMask = 0.f;   // 上一块线性级 / 掩码耗时 ms（统计用）

    bool load(const char* linPath, const char* maskPath, double sampleRate = 48000) {
        sr = sampleRate;
        lin.load(linPath); N = lin.N; H = lin.H; F = lin.F;
        lin.setDual(true, 0.2f, 3.f);                       // 双滤波器
        if (maskPath && maskPath[0]) { if (!fb::gtcHasKey(maskPath, "arch_cofi")) return false; cnet.load(maskPath, N); hasMask = true; }
        if (!rf.init(N)) return false;
        hint = hasMask && cnet.hint;
        if (hint) { rr.assign((size_t)(lin.K + 8) * H, 0.f); xaw.assign(N, 0.f); XAr.assign(F, 0.f); XAi.assign(F, 0.f); sx.assign(2 * F, 0.f); }
        win.resize(N); for (int i = 0; i < N; ++i) win[i] = std::sqrt(0.5f * (1.f - std::cos(2.f * (float)M_PI * i / N)));
        mh.assign(N, 0.f); rh.assign(N, 0.f); xw.resize(N); dw.resize(N); Xr.resize(F); Xi.resize(F); Dr.resize(F); Di.resize(F); Er.resize(F); Ei.resize(F);
        se.resize(2 * F); sy.resize(2 * F); mo.resize(2 * F); FX.resize(N); acc.assign(N, 0.f); wacc.assign(N, 0.f);
        hr.assign(F, 0.f); hi.assign(F, 0.f); xp.assign(N, 0.f); accP.assign(N, 0.f);
        GA = 1.0 - std::exp(-H / (10.0 * sr)); GT = std::pow(10.0, -25.0 / 20.0);   // 掩码电平归一 τ = 10 s（按实际采样率）
        return true;
    }
    int latency() const { return 3 * H; }

    // mic / ref：本块 H 个样本（±1 满幅）；out：H 个样本（= 4 块之前那块的处理结果）。
    // hook 非空时每帧调一次（hookOn = false 时跳过）；pre 非空时同时写出「回调前」的 H 个样本（与 out 同一时间轴）。
    void push(const float* mic, const float* ref, float* out, int mode, SpecHook hook = nullptr, void* user = nullptr, bool hookOn = true, float* pre = nullptr) {
        memmove(mh.data(), mh.data() + H, (N - H) * 4); memcpy(mh.data() + N - H, mic, H * 4);
        memmove(rh.data(), rh.data() + H, (N - H) * 4); memcpy(rh.data() + N - H, ref, H * 4);
        if (hint) { const size_t RN = rr.size(); for (int q = 0; q < H; ++q) rr[(size_t)((nIn + q) % (long long)RN)] = ref[q]; nIn += H; }
        ++k;
        if (k < 2) { memset(out, 0, H * 4); if (pre) memset(pre, 0, H * 4); return; }
        for (int q = 0; q < N; ++q) { xw[q] = rh[q] * win[q]; dw[q] = mh[q] * win[q]; }
        rf.fwd(xw.data(), Xr.data(), Xi.data()); rf.fwd(dw.data(), Dr.data(), Di.data());
        auto c0 = std::chrono::steady_clock::now();
        lin.step(Xr.data(), Xi.data(), Dr.data(), Di.data(), rh.data() + N - H, Er.data(), Ei.data());   // blk = 窗内最新一跳 = 本块参考
        auto c1 = std::chrono::steady_clock::now(); lastLin = std::chrono::duration<float, std::milli>(c1 - c0).count();
        // 掩码每帧都跑（含 mode 0 / 1），只按 mode 选输出：A/B 切换时掩码状态始终是新的，切回不需要重新预热；代价是关闭时算力不省
        if (hint) {   // 对齐参考：本帧 t = k − 2，窗 = 输入 [(k−4)H, kH) 的 4 跳（第 t−2 … t+1 跳），第 j 跳取 ref[n − lf_j·H]
            const long t = k - 2; lfAt[t % 4] = lin.lfPrev; const long long n0 = (long long)(k - 4) * H; const size_t RN = rr.size();
            for (int q = 0; q < N; ++q) { const long long n = n0 + q; const long j = (long)(k - 4) + q / H; float v = 0.f;
                if (n >= 0) { const int lf = j > t ? lfAt[t % 4] : lfAt[j % 4]; const long long src = n - (long long)lf * H; if (src >= 0) v = rr[(size_t)(src % (long long)RN)]; }
                xaw[q] = v * win[q]; }
            rf.fwd(xaw.data(), XAr.data(), XAi.data());
        }
        if (hasMask) {
            const float g = (float)(GT / (std::sqrt(Pg > 0 ? Pg : 1e-8) + 1e-6));
            double p = 0; for (int b = 0; b < F; ++b) p += (b == 0 || b == F - 1 ? 1.0 : 2.0) * ((double)Er[b] * Er[b] + (double)Ei[b] * Ei[b]); p /= (double)N * N * 0.5;
            for (int b = 0; b < F; ++b) { se[2 * b] = Er[b] * g; se[2 * b + 1] = Ei[b] * g; sy[2 * b] = (Dr[b] - Er[b]) * g; sy[2 * b + 1] = (Di[b] - Ei[b]) * g; }
            if (hint) for (int b = 0; b < F; ++b) { sx[2 * b] = XAr[b] * g; sx[2 * b + 1] = XAi[b] * g; }
            cnet.step(se.data(), sy.data(), hint ? sx.data() : nullptr, mo.data());
            const float ig = 1.f / g; for (int b = 0; b < F; ++b) FX[b] = cf(mo[2 * b] * ig, mo[2 * b + 1] * ig);
            if (Pg < 0) { if (p > 1e-8) Pg = p; } else Pg += GA * (p - Pg);
        }
        if (mode == 1 || (mode == 2 && !hasMask)) { for (int b = 0; b < F; ++b) FX[b] = cf(Er[b], Ei[b]); }
        else if (mode == 0) { for (int b = 0; b < F; ++b) FX[b] = cf(Dr[b], Di[b]); }
        lastMask = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - c1).count();
        FX[0] = cf(FX[0].real(), 0); FX[F - 1] = cf(FX[F - 1].real(), 0);
        const bool callHook = hook && hookOn;
        if (pre && callHook) { rf.inv(FX.data(), xp.data()); for (int q = 0; q < N; ++q) accP[q] += xp[q] * win[q]; }   // 回调前的这一帧
        if (callHook) {
            for (int b = 0; b < F; ++b) { hr[b] = FX[b].real(); hi[b] = FX[b].imag(); }
            hook(user, hr.data(), hi.data(), F);
            for (int b = 0; b < F; ++b) FX[b] = cf(hr[b], hi[b]);
            FX[0] = cf(FX[0].real(), 0); FX[F - 1] = cf(FX[F - 1].real(), 0);
        }
        rf.inv(FX.data(), xw.data());
        for (int q = 0; q < N; ++q) { acc[q] += xw[q] * win[q]; wacc[q] += win[q] * win[q]; }
        if (pre && !callHook) for (int q = 0; q < N; ++q) accP[q] += xw[q] * win[q];       // 回调没调：回调前 = 回调后
        for (int q = 0; q < H; ++q) out[q] = wacc[q] > 1e-8f ? acc[q] / wacc[q] : 0.f;
        if (pre) for (int q = 0; q < H; ++q) pre[q] = wacc[q] > 1e-8f ? accP[q] / wacc[q] : 0.f;
        memmove(acc.data(), acc.data() + H, (N - H) * 4); memset(acc.data() + N - H, 0, H * 4);
        if (pre) { memmove(accP.data(), accP.data() + H, (N - H) * 4); memset(accP.data() + N - H, 0, H * 4); }
        memmove(wacc.data(), wacc.data() + H, (N - H) * 4); memset(wacc.data() + N - H, 0, H * 4);
    }
};
}  // namespace ac
