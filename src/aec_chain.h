// 处理链的流式包装：线性级（fb/nlin.h）→ 残余回声抑制（fb/cofi.h），两级共用一次 STFT（N=1536 / H=384，sqrt-hann）。
// 每来 H 个样本推一帧：第 k 块到齐后算第 k−2 帧（窗 = 最近 4 块，开头不足补零），算完后第 k−4 块的输出定稿 → 输出比输入晚 3 块（1152 样本）。
// 提示版掩码（参数文件带 hint_v1，0.6 起，如 v62be20）另吃「对齐参考谱」：参考按线性级逐帧时延（lf 帧）挪到回归量起点后的同一 STFT，
//   = 评估里 nlin_bin.py 的第 4 个输出（每跳 j 的样本用第 j 帧的 lf）再做 STFT。窗内最后一跳（第 t+1 跳）的 lf 端上还没算出，用本帧 lf 代替——
//   时延不变时与评估逐样本相同，只在时延改判的那几帧有差别。参考历史按线性级最大时延 K 帧留够。
// mode：0 = 直通（输出麦克风，延迟相同）；1 = 只线性级；2 = 整链。切换不清状态。
// 频谱回调（0.5）：每帧输出频谱在合成前交给 hook 原地修改（各 mode 都调；第 1 块不调，第 2 块起每块一帧，第 c 次调用的窗覆盖输入 [c·H − 768, c·H + 768)，负索引补零）；调完把直流 / 奈奎斯特虚部置 0。
//   回调前一路始终独立合成并推进高通；回调开着时另做一次逆变换，没挂 / 关着复用同一帧。
#pragma once
#include "fb/nlin.h"
#include "fb/cofi.h"
#include "aec48.h"
#include "talk_gate.h"
#include "fb/talk_hpf.h"
#include "pffft.h"
#include <vector>
#include <complex>
#include <cmath>
#include <cstring>

// 双档切档：讲话那一路在唱歌档期间休眠，判断头状态会过时（实测切回后约 1.5–2 s 吃人声）。
// 唤醒时（含从唱歌开流后第一次切到讲话）清零判断头 GRU（掩码主体不动），并让讲话门保持打开 AEC48_SWITCH_WAKE_MS，
// 期间判断头照常推进、只靠掩码压回声；之后门按平滑规则正常收放。4000 ms 是 120 条 × 4 种切档日程按近端真值量到「零新增吃字」的最短值
// （2000 ms 剩 16 窗、3000 ms 剩 2 窗）。模式 1 = 只强开门，3 = 另清门历史，仅供对照。
#ifndef AEC48_SWITCH_WAKE_MODE
#define AEC48_SWITCH_WAKE_MODE 2
#endif
#ifndef AEC48_SWITCH_WAKE_MS
#define AEC48_SWITCH_WAKE_MS 4000
#endif
static_assert(AEC48_SWITCH_WAKE_MODE >= 1 && AEC48_SWITCH_WAKE_MODE <= 3, "invalid switch wake mode");
static_assert(AEC48_SWITCH_WAKE_MS >= 0, "invalid switch wake duration");

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
#ifdef AEC48_DUAL
    fb::Cofi singNet;
    std::vector<float> moOther, accSing, accPSing, outSing, outTalk, preSing, preTalk;
    std::vector<cf> FXOther, FXPrimary, FXMixPre;
    float talkBlend = 1.f;
    int blendPos = 0, blendN = 4;   // 切档交叉淡化：整数进度 0..blendN，blendN = ceil(0.3·sr/H)（48k 38 块 / 44.1k 35 块）
    bool singOLAActive = false, talkOLAActive = false;
    long long talkFrames = 0;
    bool talkWasRunning = false;
    int talkWakeFramesLeft = 0;
#endif
    fb::TalkHighpass talkHpf;
    fb::TalkHighpass preTalkHpf;
    std::vector<float> hpfScratch, preHpfScratch, preHpfDry, hpfWeights;
    float hpfMix = 1.f, hpfFrom = 1.f;
    int hpfTarget = 1, hpfFadeAt = 0;
    bool hpfStarted = false;
    TalkGate talkGate;
    bool talkFrameReady = false;
    int N = 0, H = 0, F = 0; RFft rf;
    bool hint = false; std::vector<float> rr, xaw, XAr, XAi, sx; long long nIn = 0; int lfAt[4] = {0, 0, 0, 0};   // 提示版：参考环形历史、对齐参考窗 / 谱
    bool sideNetHint = false;
    fb::HintV1 sideHint; std::vector<float> sideResid, sideRhat, sidePowerNorm, sideEr, sideXr, rrSide; std::vector<unsigned char> farWin; int farWinN = 0, farWinAt = 0, farWinCount = 0, sideLfAt[4] = {0, 0, 0, 0}; long long sideBase = 0, sideNIn = 0; double farPeak = 0; bool sideWasOn = false;
    std::vector<float> win, mh, rh, xw, dw, Xr, Xi, Dr, Di, Er, Ei, se, sy, mo, acc, wacc, hr, hi, xp, accP; std::vector<cf> FX;
    double Pg = -1, GA = 0, GT = 0, sr = 48000; long k = 0; float sideLastG = 1.f;
    float lastLin = 0.f, lastMask = 0.f;   // 上一块线性级 / 掩码耗时 ms（统计用）

    bool load(const char* linPath, const char* maskPath, double sampleRate = 48000, const char* singPath = nullptr) {
        sr = sampleRate;
        lin.load(linPath); N = lin.N; H = lin.H; F = lin.F;
        lin.setDual(true, 0.2f, 3.f);                       // 双滤波器
        if (maskPath && maskPath[0]) { if (!fb::gtcHasKey(maskPath, "arch_cofi")) return false; cnet.load(maskPath, N); hasMask = true; }
#ifdef AEC48_DUAL
        if (!singPath || !fb::gtcHasKey(singPath, "arch_cofi")) return false;
        singNet.load(singPath, N);
        if (!cnet.hasTalkHead || !cnet.hint || singNet.hasTalkHead || singNet.hint) return false;
#else
        (void)singPath;
#endif
        if (cnet.hasTalkHead) { lin.TA = 0.98f; lin.TAd = 0.98; }
        if (!rf.init(N)) return false;
        hint = hasMask && cnet.hint;
        rr.assign((size_t)(lin.K + 8) * H, 0.f); xaw.assign(N, 0.f); XAr.assign(F, 0.f); XAi.assign(F, 0.f); sx.assign(2 * F, 0.f);
        rrSide.assign(rr.size(), 0.f);
#ifdef AEC48_DUAL
        sideHint.init(F, N, sr, 48, 12, 0.7f, 0.5f, 0.3f);
#else
        if (!cnet.hint) sideHint.init(F, N, sr, 48, 12, 0.7f, 0.5f, 0.3f);
#endif
        sideResid.assign(F, 0.f); sideRhat.assign(F, 0.f); sidePowerNorm.assign(F, 0.f); sideEr.assign(F, 0.f); sideXr.assign(F, 0.f);
        farWinN = (int)std::ceil(sr / H); farWin.assign(farWinN, 0); farWinAt = farWinCount = 0; farPeak = 0;
        if (cnet.hasTalkHead) talkGate.init(H, (int)sampleRate);
        preHpfDry.assign(H, 0.f);
        if (cnet.hasTalkHead) {
            if (!talkHpf.init(sampleRate) || !preTalkHpf.init(sampleRate)) return false;
            hpfScratch.assign(H, 0.f); preHpfScratch.assign(H, 0.f); hpfWeights.assign(H, 0.f);
        }
        win.resize(N); for (int i = 0; i < N; ++i) win[i] = std::sqrt(0.5f * (1.f - std::cos(2.f * (float)M_PI * i / N)));
        mh.assign(N, 0.f); rh.assign(N, 0.f); xw.resize(N); dw.resize(N); Xr.resize(F); Xi.resize(F); Dr.resize(F); Di.resize(F); Er.resize(F); Ei.resize(F);
        se.resize(2 * F); sy.resize(2 * F); mo.resize(2 * F); FX.resize(N); acc.assign(N, 0.f); wacc.assign(N, 0.f);
        hr.assign(F, 0.f); hi.assign(F, 0.f); xp.assign(N, 0.f); accP.assign(N, 0.f);
#ifdef AEC48_DUAL
        moOther.assign(2 * F, 0.f); FXOther.resize(N); FXPrimary.resize(N); FXMixPre.resize(N);
        accSing.assign(N, 0.f); accPSing.assign(N, 0.f);
        outSing.assign(H, 0.f); outTalk.assign(H, 0.f); preSing.assign(H, 0.f); preTalk.assign(H, 0.f);
#endif
        GA = 1.0 - std::exp(-H / (10.0 * sr)); GT = std::pow(10.0, -25.0 / 20.0);   // 掩码电平归一 τ = 10 s（按实际采样率）
        return true;
    }
    int latency() const { return 3 * H; }
    void resetSideRefs() { sideBase = sideNIn = (long long)k * H; std::fill(rrSide.begin(), rrSide.end(), 0.f); sideLfAt[0] = sideLfAt[1] = sideLfAt[2] = sideLfAt[3] = 0; }
    void resetSide() {
        farPeak = 0; farWinAt = farWinCount = 0; std::fill(farWin.begin(), farWin.end(), 0);
#ifdef AEC48_DUAL
        sideHint.resetHint();
#else
        if (!cnet.hint) sideHint.resetHint();
#endif
        resetSideRefs(); sideWasOn = false;
    }
    void syncPreAcc() { accP = acc;
#ifdef AEC48_DUAL
        accPSing = accSing;
#endif
    }

    // mic / ref：本块 H 个样本（±1 满幅）；out：H 个样本（= 4 块之前那块的处理结果）。
    // hook 非空时每帧调一次（hookOn = false 时跳过）；pre 非空时同时写出「回调前」的 H 个样本（与 out 同一时间轴）。
    void push(const float* mic, const float* ref, float* out, int mode, SpecHook hook = nullptr, void* user = nullptr, bool hookOn = true, float* pre = nullptr, aec48_spec_hook2 hook2 = nullptr, void* user2 = nullptr, bool gateOn = true, float gateThreshold = -3.5f, int profile = AEC48_PROFILE_TALK, bool talkHpfOn = true) {
        talkFrameReady = false;
        if (cnet.hasTalkHead) {
            const int requested = talkHpfOn ? 1 : 0;
            if (!hpfStarted) {
                hpfStarted = true;
                hpfTarget = requested;
                hpfMix = hpfFrom = static_cast<float>(requested);
                hpfFadeAt = 0;
            } else if (requested != hpfTarget) {
                hpfTarget = requested;
                hpfFrom = hpfMix;
                hpfFadeAt = 0;
            }
        }
#ifdef AEC48_DUAL
        float blendStart = talkBlend;
        if (k == 0) { blendN = std::max(1, (int)std::ceil(0.3 * sr / H)); blendPos = profile == AEC48_PROFILE_TALK ? blendN : 0; talkBlend = blendStart = profile == AEC48_PROFILE_TALK ? 1.f : 0.f; }
        else {
            // 约 300 ms 线性淡化；淡化没走完又切回时从当前进度往回走，端点精确为 0 / 1。
            if (profile == AEC48_PROFILE_TALK) { if (blendPos < blendN) ++blendPos; } else if (blendPos > 0) --blendPos;
            talkBlend = blendPos == blendN ? 1.f : blendPos == 0 ? 0.f : (float)blendPos / (float)blendN;
        }
        const float frameBlend = 0.5f * (blendStart + talkBlend);
        const bool runTalk = blendStart > 0.f || talkBlend > 0.f;
        const bool runSing = blendStart < 1.f || talkBlend < 1.f;
        if (k == 0) talkWasRunning = runTalk;
        if (runTalk && !talkWasRunning) {
            // A wake includes the first frame of a stream that starts in sing mode.
#if AEC48_SWITCH_WAKE_MODE >= 2
            cnet.resetTalkHead();
#endif
#if AEC48_SWITCH_WAKE_MODE >= 3
            talkGate.resetHistory();
#endif
            talkGate.forceRecentOpen(3); // frame=talkFrames-1 reads gain frames G-3..G-1 on the first wake block
            // Keep one interpolation endpoint past W: gain samples are joined at frame centers.
            talkWakeFramesLeft = (int)std::ceil(AEC48_SWITCH_WAKE_MS * sr / (1000.0 * H)) + 1;
        } else if (!runTalk) {
            talkWakeFramesLeft = 0;
        }
        talkWasRunning = runTalk;
        const bool wakeHoldOpen = talkWakeFramesLeft > 0;
        if (wakeHoldOpen) --talkWakeFramesLeft;
        fb::Cofi& maskNet = profile == AEC48_PROFILE_TALK ? cnet : singNet;
        fb::Cofi& otherNet = profile == AEC48_PROFILE_TALK ? singNet : cnet;
        hint = runTalk;
        // 休眠的 OLA 尾巴会过期；唤醒时借用正在播放一路的谱合成尾巴，GRU 与门历史不动。
        // mode=0/1 两路输入谱相同，这也保证切档不会使直通音量因空尾巴下降。
        if (runTalk && !talkOLAActive) {
            if (singOLAActive) { memcpy(acc.data(), accSing.data(), N * sizeof(float)); memcpy(accP.data(), accPSing.data(), N * sizeof(float)); }
            else { std::fill(acc.begin(), acc.end(), 0.f); std::fill(accP.begin(), accP.end(), 0.f); }
            talkOLAActive = true;
        }
        if (runSing && !singOLAActive) {
            if (talkOLAActive) { memcpy(accSing.data(), acc.data(), N * sizeof(float)); memcpy(accPSing.data(), accP.data(), N * sizeof(float)); }
            else { std::fill(accSing.begin(), accSing.end(), 0.f); std::fill(accPSing.begin(), accPSing.end(), 0.f); }
            singOLAActive = true;
        }
        if (!runTalk) talkOLAActive = false;
        if (!runSing) singOLAActive = false;
#else
        fb::Cofi& maskNet = cnet;
#endif
        sideNetHint = maskNet.hint;
        const bool sideOn = hook2 && hookOn;
        if (sideOn && !sideWasOn) resetSideRefs();
        sideWasOn = sideOn;
        memmove(mh.data(), mh.data() + H, (N - H) * 4); memcpy(mh.data() + N - H, mic, H * 4);
        memmove(rh.data(), rh.data() + H, (N - H) * 4); memcpy(rh.data() + N - H, ref, H * 4);
#ifdef AEC48_DUAL
        { const size_t RN = rr.size(); for (int q = 0; q < H; ++q) rr[(size_t)((nIn + q) % (long long)RN)] = ref[q]; nIn += H; }
#else
        if (hint) { const size_t RN = rr.size(); for (int q = 0; q < H; ++q) rr[(size_t)((nIn + q) % (long long)RN)] = ref[q]; nIn += H; }
#endif
        if (sideOn) { const size_t RN = rrSide.size(); for (int q = 0; q < H; ++q) rrSide[(size_t)((sideNIn - sideBase + q) % (long long)RN)] = ref[q]; sideNIn += H; }
        ++k;
        if (k < 2) { memset(out, 0, H * 4); if (pre) memset(pre, 0, H * 4); return; }
        for (int q = 0; q < N; ++q) { xw[q] = rh[q] * win[q]; dw[q] = mh[q] * win[q]; }
        rf.fwd(xw.data(), Xr.data(), Xi.data()); rf.fwd(dw.data(), Dr.data(), Di.data());
#ifdef AEC48_DUAL
        // 只换时延估计的平滑系数；NlinD 的时延、滤波器与网络状态均沿用。
        lin.TA = profile == AEC48_PROFILE_TALK ? 0.98f : 0.995f;
        lin.TAd = profile == AEC48_PROFILE_TALK ? 0.98 : 0.995;
#endif
        auto c0 = std::chrono::steady_clock::now();
        lin.step(Xr.data(), Xi.data(), Dr.data(), Di.data(), rh.data() + N - H, Er.data(), Ei.data());   // blk = 窗内最新一跳 = 本块参考
#ifdef AEC48_DUAL
        lfAt[(k - 2) % 4] = lin.lfPrev;
#endif
        auto c1 = std::chrono::steady_clock::now(); lastLin = std::chrono::duration<float, std::milli>(c1 - c0).count();
        if (sideOn) sideLfAt[(k - 2) % 4] = lin.lfPrev;
        // 掩码每帧都跑（含 mode 0 / 1），只按 mode 选输出：A/B 切换时掩码状态始终是新的，切回不需要重新预热；代价是关闭时算力不省
        if (hint) {   // 对齐参考：本帧 t = k − 2，窗 = 输入 [(k−4)H, kH) 的 4 跳（第 t−2 … t+1 跳），第 j 跳取 ref[n − lf_j·H]
            const long t = k - 2; lfAt[t % 4] = lin.lfPrev; const long long n0 = (long long)(k - 4) * H; const size_t RN = rr.size();
            for (int q = 0; q < N; ++q) { const long long n = n0 + q; const long j = (long)(k - 4) + q / H; float v = 0.f;
                if (n >= 0) { const int lf = j > t ? lfAt[t % 4] : lfAt[j % 4]; const long long src = n - (long long)lf * H; if (src >= 0) v = rr[(size_t)(src % (long long)RN)]; }
                xaw[q] = v * win[q]; }
            rf.fwd(xaw.data(), XAr.data(), XAi.data());
        } else if (sideOn) {
            const long t = k - 2; const long long n0 = (long long)(k - 4) * H; const size_t RN = rrSide.size();
            for (int q = 0; q < N; ++q) { const long long n = n0 + q; const long j = (long)(k - 4) + q / H; float v = 0.f;
                if (n >= 0) { const int lf = j > t ? sideLfAt[t % 4] : sideLfAt[j % 4]; const long long src = n - (long long)lf * H; if (src >= sideBase && src < sideNIn) v = rrSide[(size_t)((src - sideBase) % (long long)RN)]; }
                xaw[q] = v * win[q]; }
            rf.fwd(xaw.data(), XAr.data(), XAi.data());
        }
        float sideG = 1.f;
        if (hasMask) {
            const float g = (float)(GT / (std::sqrt(Pg > 0 ? Pg : 1e-8) + 1e-6));
            if (sideOn) sideLastG = sideG = g;
            double p = 0; for (int b = 0; b < F; ++b) p += (b == 0 || b == F - 1 ? 1.0 : 2.0) * ((double)Er[b] * Er[b] + (double)Ei[b] * Ei[b]); p /= (double)N * N * 0.5;
            for (int b = 0; b < F; ++b) { se[2 * b] = Er[b] * g; se[2 * b + 1] = Ei[b] * g; sy[2 * b] = (Dr[b] - Er[b]) * g; sy[2 * b + 1] = (Di[b] - Ei[b]) * g; }
            if (hint) for (int b = 0; b < F; ++b) { sx[2 * b] = XAr[b] * g; sx[2 * b + 1] = XAi[b] * g; }
            maskNet.step(se.data(), sy.data(), maskNet.hint ? sx.data() : nullptr, mo.data());
#ifdef AEC48_DUAL
            if (runTalk && runSing) otherNet.step(se.data(), sy.data(), otherNet.hint ? sx.data() : nullptr, moOther.data());
            if (runTalk) {
                talkGate.setThreshold(gateThreshold);
                talkGate.push(rh.data() + N - 2 * H, cnet.lastTalk, wakeHoldOpen);
                ++talkFrames;
                talkFrameReady = true;
            }
#else
            if (maskNet.hasTalkHead) {
                talkGate.setThreshold(gateThreshold);
                talkGate.push(rh.data() + N - 2 * H, maskNet.lastTalk);
                talkFrameReady = true;
            }
#endif
            const float ig = 1.f / g; for (int b = 0; b < F; ++b) FX[b] = cf(mo[2 * b] * ig, mo[2 * b + 1] * ig);
#ifdef AEC48_DUAL
            if (runTalk && runSing) for (int b = 0; b < F; ++b) FXOther[b] = cf(moOther[2 * b] * ig, moOther[2 * b + 1] * ig);
#endif
            if (Pg < 0) { if (p > 1e-8) Pg = p; } else Pg += GA * (p - Pg);
        }
        if (mode == 1 || (mode == 2 && !hasMask)) { for (int b = 0; b < F; ++b) FX[b] = cf(Er[b], Ei[b]); }
        else if (mode == 0) { for (int b = 0; b < F; ++b) FX[b] = cf(Dr[b], Di[b]); }
#ifdef AEC48_DUAL
        if (runTalk && runSing && mode != 2) for (int b = 0; b < F; ++b) FXOther[b] = FX[b];
#endif
        lastMask = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - c1).count();
        FX[0] = cf(FX[0].real(), 0); FX[F - 1] = cf(FX[F - 1].real(), 0);
#ifdef AEC48_DUAL
        const bool crossing = runTalk && runSing;
        if (crossing) {
            FXOther[0] = cf(FXOther[0].real(), 0); FXOther[F - 1] = cf(FXOther[F - 1].real(), 0);
            for (int b = 0; b < F; ++b) {
                FXPrimary[b] = FX[b];
                const cf& sing = profile == AEC48_PROFILE_SING ? FXPrimary[b] : FXOther[b];
                const cf& talk = profile == AEC48_PROFILE_TALK ? FXPrimary[b] : FXOther[b];
                FX[b] = (1.f - frameBlend) * sing + frameBlend * talk;
                FXMixPre[b] = FX[b];
            }
        }
        auto addSpectrum = [&](const cf* spectrum, std::vector<float>& accumulator, float* scratch) {
            rf.inv(spectrum, scratch);
            for (int q = 0; q < N; ++q) accumulator[q] += scratch[q] * win[q];
        };
#endif
        const bool callHook = hook && hookOn, anyHook = callHook || sideOn;
        if (sideOn) {
            double tot = 0; for (int b = 0; b < F; ++b) tot += (double)Xr[b] * Xr[b] + (double)Xi[b] * Xi[b];
            farPeak = std::max(farPeak, tot); const unsigned char active = (double)tot > farPeak * 1e-5 + 1e-12;
            farWinCount -= farWin[farWinAt] != 0; farWin[farWinAt] = active; farWinCount += active; if (++farWinAt == farWinN) farWinAt = 0;
        }
        if (anyHook) {
#ifdef AEC48_DUAL
            const cf* primary = crossing ? FXPrimary.data() : FX.data();
            if (runTalk) addSpectrum(profile == AEC48_PROFILE_TALK ? primary : FXOther.data(), accP, xp.data());
            if (runSing) addSpectrum(profile == AEC48_PROFILE_SING ? primary : FXOther.data(), accPSing, xp.data());
#else
            rf.inv(FX.data(), xp.data()); for (int q = 0; q < N; ++q) accP[q] += xp[q] * win[q];
#endif
        }   // 回调前的这一帧
        if (callHook) {
            for (int b = 0; b < F; ++b) { hr[b] = FX[b].real(); hi[b] = FX[b].imag(); }
            hook(user, hr.data(), hi.data(), F);
            for (int b = 0; b < F; ++b) FX[b] = cf(hr[b], hi[b]);
            FX[0] = cf(FX[0].real(), 0); FX[F - 1] = cf(FX[F - 1].real(), 0);
        }
        if (sideOn) {
            const float g2 = sideG * sideG;
            // 候选版直接复用 Cofi 在归一电平谱上生成的提示；默认版用同一尺度独立递推。
            // 默认版的独立状态只在 hook2 开启时递推，关闭期间冻结，重新挂接时清零。
            if (maskNet.hint) {
                for (int b = 0; b < F; ++b) { sidePowerNorm[b] = maskNet.hm[b] * maskNet.hm[b]; sideRhat[b] = sidePowerNorm[b] / g2; }
            } else {
                for (int b = 0; b < F; ++b) { sideEr[b] = Er[b] * Er[b] + Ei[b] * Ei[b]; sideXr[b] = XAr[b] * XAr[b] + XAi[b] * XAi[b]; }
                sideHint.hintStepScaledPower(F, sideEr.data(), sideXr.data(), g2);
                for (int b = 0; b < F; ++b) { sidePowerNorm[b] = sideHint.power(b); sideRhat[b] = sideHint.powerUnscaled(b, g2); }
            }
            for (int b = 0; b < F; ++b) {
                const float re = sideRhat[b];
                if (mode == 0) { const float dr = Dr[b] - Er[b], di = Di[b] - Ei[b]; sideResid[b] = dr * dr + di * di + re; }
                else if (mode == 2 && hasMask) { const float e = Er[b] * Er[b] + Ei[b] * Ei[b], f = FX[b].real() * FX[b].real() + FX[b].imag() * FX[b].imag(); sideResid[b] = re * std::min(1.f, f / (e + 1e-20f)); }
                else sideResid[b] = re;
            }
            Aec48SpecSide side = { (int)sizeof(Aec48SpecSide), mode, farWinCount ? 1.f : 0.f, -1.f, sideResid.data(), F };
            for (int b = 0; b < F; ++b) { hr[b] = FX[b].real(); hi[b] = FX[b].imag(); }
            hook2(user2, hr.data(), hi.data(), F, &side);
            for (int b = 0; b < F; ++b) FX[b] = cf(hr[b], hi[b]);
            FX[0] = cf(FX[0].real(), 0); FX[F - 1] = cf(FX[F - 1].real(), 0);
        }
        // 双档过渡：两路分别做 OLA 与讲话门，再对两个时域块作线性混合。
#ifdef AEC48_DUAL
        if (crossing) {
            for (int b = 0; b < F; ++b) {
                const cf before = FXMixPre[b], after = FX[b];
                if (after == before) continue;  // identity hook 必须逐位保留两路原谱。
                if (after == cf(0.f, 0.f)) { FXPrimary[b] = FXOther[b] = cf(0.f, 0.f); continue; }
                if (std::norm(before) > 1e-24f) {
                    const cf gain = after / before;
                    FXPrimary[b] *= gain; FXOther[b] *= gain;
                } else {
                    // 混合谱接近零时复数比值不稳定；两路都采用回调结果。
                    FXPrimary[b] = FXOther[b] = after;
                }
            }
        }
        const cf* primaryPost = crossing ? FXPrimary.data() : FX.data();
        if (runTalk) addSpectrum(profile == AEC48_PROFILE_TALK ? primaryPost : FXOther.data(), acc, xw.data());
        if (runSing) addSpectrum(profile == AEC48_PROFILE_SING ? primaryPost : FXOther.data(), accSing, xw.data());
        if (!anyHook) {
            if (runTalk) addSpectrum(profile == AEC48_PROFILE_TALK ? primaryPost : FXOther.data(), accP, xp.data());
            if (runSing) addSpectrum(profile == AEC48_PROFILE_SING ? primaryPost : FXOther.data(), accPSing, xp.data());
        }
        for (int q = 0; q < N; ++q) wacc[q] += win[q] * win[q];
        for (int q = 0; q < H; ++q) {
            if (runTalk) outTalk[q] = wacc[q] > 1e-8f ? acc[q] / wacc[q] : 0.f;
            if (runSing) outSing[q] = wacc[q] > 1e-8f ? accSing[q] / wacc[q] : 0.f;
            if (runTalk) preTalk[q] = wacc[q] > 1e-8f ? accP[q] / wacc[q] : 0.f;
            if (runSing) preSing[q] = wacc[q] > 1e-8f ? accPSing[q] / wacc[q] : 0.f;
        }
        if (runTalk && gateOn) {
            talkGate.apply(outTalk.data(), talkFrames - 1, mode);
            talkGate.apply(preTalk.data(), talkFrames - 1, mode);
        }
        // Filter the talk lane before the profile crossfade. When the talk
        // lane sleeps in dual mode, feed the live singing lane through the
        // filter state so a later talk transition starts with current state.
        if (cnet.hasTalkHead) {
            const float* hpfInput = runTalk ? outTalk.data() : outSing.data();
            talkHpf.process(hpfInput, hpfScratch.data(), H);
            const float* preInput = runTalk ? preTalk.data() : preSing.data();
            preTalkHpf.process(preInput, preHpfScratch.data(), H);
            float* talkLane = runTalk ? outTalk.data() : nullptr;
            float* preLane = runTalk ? preTalk.data() : nullptr;
            for (int q = 0; q < H; ++q) {
                float w = hpfMix;
                if (hpfFadeAt < 4 * H) {
                    ++hpfFadeAt;
                    w = hpfFrom + (static_cast<float>(hpfTarget) - hpfFrom) * (static_cast<float>(hpfFadeAt) / (4.f * H));
                    hpfMix = w;
                } else hpfMix = w = static_cast<float>(hpfTarget);
                hpfWeights[q] = w;
                if (talkLane) {
                    const float dry = talkLane[q], wet = hpfScratch[q];
                    if (mode != 0 && w > 0.f) talkLane[q] = w >= 1.f ? wet : (1.f - w) * dry + w * wet;
                }
                if (preLane && mode != 0 && w > 0.f) {
                    const float dry = preLane[q], wet = preHpfScratch[q];
                    preLane[q] = w >= 1.f ? wet : (1.f - w) * dry + w * wet;
                }
            }
        }
        if (crossing && mode == 0) { memcpy(out, outTalk.data(), H * sizeof(float)); if (pre) memcpy(pre, preTalk.data(), H * sizeof(float)); }
        else if (crossing) for (int q = 0; q < H; ++q) {
            const float w = blendStart + (talkBlend - blendStart) * ((q + 1.f) / H);
            out[q] = (1.f - w) * outSing[q] + w * outTalk[q];
            if (pre) pre[q] = (1.f - w) * preSing[q] + w * preTalk[q];
        }
        else if (runTalk) { memcpy(out, outTalk.data(), H * sizeof(float)); if (pre) memcpy(pre, preTalk.data(), H * sizeof(float)); }
        else { memcpy(out, outSing.data(), H * sizeof(float)); if (pre) memcpy(pre, preSing.data(), H * sizeof(float)); }
        if (runTalk) { memmove(acc.data(), acc.data() + H, (N - H) * 4); memset(acc.data() + N - H, 0, H * 4); }
        if (runSing) { memmove(accSing.data(), accSing.data() + H, (N - H) * 4); memset(accSing.data() + N - H, 0, H * 4); }
        if (runTalk) { memmove(accP.data(), accP.data() + H, (N - H) * 4); memset(accP.data() + N - H, 0, H * 4); }
        if (runSing) { memmove(accPSing.data(), accPSing.data() + H, (N - H) * 4); memset(accPSing.data() + N - H, 0, H * 4); }
#else
        rf.inv(FX.data(), xw.data());
        for (int q = 0; q < N; ++q) { acc[q] += xw[q] * win[q]; wacc[q] += win[q] * win[q]; }
        if (!anyHook) for (int q = 0; q < N; ++q) accP[q] += xw[q] * win[q];       // 回调没调：回调前 = 回调后
        for (int q = 0; q < H; ++q) out[q] = wacc[q] > 1e-8f ? acc[q] / wacc[q] : 0.f;
        if (cnet.hasTalkHead && gateOn) talkGate.apply(out, k - 2, mode);
        if (cnet.hasTalkHead) {
            talkHpf.process(out, hpfScratch.data(), H);
            for (int q = 0; q < H; ++q) {
                float w = hpfMix;
                if (hpfFadeAt < 4 * H) {
                    ++hpfFadeAt;
                    w = hpfFrom + (static_cast<float>(hpfTarget) - hpfFrom) * (static_cast<float>(hpfFadeAt) / (4.f * H));
                    hpfMix = w;
                } else hpfMix = w = static_cast<float>(hpfTarget);
                hpfWeights[q] = w;
                if (mode != 0 && w >= 1.f) out[q] = hpfScratch[q];
                else if (mode != 0 && w > 0.f) out[q] = (1.f - w) * out[q] + w * hpfScratch[q];
            }
        }
        for (int q = 0; q < H; ++q) preHpfDry[q] = wacc[q] > 1e-8f ? accP[q] / wacc[q] : 0.f;
        if (cnet.hasTalkHead && gateOn) talkGate.apply(preHpfDry.data(), k - 2, mode);
        if (cnet.hasTalkHead) {
            preTalkHpf.process(preHpfDry.data(), preHpfScratch.data(), H);
            for (int q = 0; q < H; ++q) {
                const float w = hpfWeights[q];
                if (mode != 0 && w >= 1.f) preHpfDry[q] = preHpfScratch[q];
                else if (mode != 0 && w > 0.f) preHpfDry[q] = (1.f - w) * preHpfDry[q] + w * preHpfScratch[q];
            }
        }
        if (pre) memcpy(pre, preHpfDry.data(), H * sizeof(float));
        memmove(acc.data(), acc.data() + H, (N - H) * 4); memset(acc.data() + N - H, 0, H * 4);
        memmove(accP.data(), accP.data() + H, (N - H) * 4); memset(accP.data() + N - H, 0, H * 4);
#endif
        memmove(wacc.data(), wacc.data() + H, (N - H) * 4); memset(wacc.data() + N - H, 0, H * 4);
    }
};
}  // namespace ac
