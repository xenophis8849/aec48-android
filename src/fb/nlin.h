// 线性级流式推理：在线时延估计 + 参考电平归一 + 神经网络估噪声的逐频点对角卡尔曼滤波（单帧、单线程、每帧零分配）。
// 双滤波：同一回归量另跑一套经典卡尔曼，逐频点取误差小者。参数与常数全部来自 .gtc 文件。
// 域：STFT N=1536 / H=384，sqrt-hann。每帧输入参考谱 X、麦克风谱 D（各 F=769 复数）与本帧窗内最新一跳的参考时域样本；输出线性级输出谱 E = D − Ŷ。
#pragma once
#include "gtcrn.h"   // 复用 Ten
#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
namespace fb {

#ifndef NLIN_TDE_T
#define NLIN_TDE_T double   // 时延估计累加精度：double = 与 Python（float64）逐帧一致（核对版）；10-10 手机/测速版用 float（build_chain_d.sh），盲测 800 条 1 条分歧（近端单讲，消除量无差）
#endif
typedef NLIN_TDE_T tdT;
struct NlinD {
    static constexpr int LC = 8;                 // 抽头数编译期固定（load 时核对）
    int N = 0, H = 0, L = 0, NB = 0, HID = 0, F = 0; float AK = 0;
    // 时延估计常数
    int K = 0, TW = 0, HOLD = 0, SC = 0, ENVHOLD = 0, NG = 0, TOPG = 0, MARGF = 0, NBIN = 0, NBND = 0; float TA = 0, THR = 0, RATIO = 0, AENV = 0, ENVDOM = 0, FLOOR0 = 0; bool FULLSTART = true;
    std::vector<int> bins, edg, bS, bE, gS, gE; tdT TAd = 0, AENVd = 0, THRd = 0, RATIOd = 0, ENVDOMd = 0, FLOOR0d = 0;
    std::vector<float> ih, hh, bi, bh, ow, ob, ihT, hhT, owT;   // *T = 转置 [输入][行]
    // —— 状态 ——
    int RX = 0; long t = 0;
    std::vector<float> rxr, rxi;                 // 参考谱环 [RX][F]
    std::vector<float> xbr, xbi; std::vector<tdT> pxr;   // 抽稀频点环 [RX][NBIN]、抽稀能量环 [RX]；包络去均值环 xhd [RX][NBND]
    std::vector<tdT> dbr, dbi, Sr, Si, Sxx, Sdd, bx, bd, xm, dm, Renv, coh, gsc, xhd;
    tdT floor_ = 0; int nact = 0, cur = -1, cand = -1, cn = 0, ecn = 0, ecand = -1;
    double gpk = 0, gcur = 1, gdec = 0; int gnact = 0, gnst = 0; long glast = -1000000000L; float gprev = 1.f; int gch = 0;
    int lfPrev = 0, lch = 0;
    std::vector<float> Wr, Wi, P, tmp;           // [L][F]
    // 10-10 双滤波器（nlin.dual_select）：经典版（c = q = 1）同回归量、同复位另跑一套，逐频点比一阶平滑误差功率，经典版小 DUAL_MARGIN dB 以上才取经典版
    bool dual = false; double dualAd = 0, dualMd = 0;
    std::vector<float> Wcr, Wci, Pc, pec, Ecr, Eci, oneB; std::vector<double> sEn, sEc;   // 平滑误差功率用 double（= Python lfilter float64；近门槛处选路才与规格一致）
    std::vector<float> qF, cF, pe, Yr, Yi, Er_, Ei_, pu, den, ft, h, gx, gh, o, cB, qB, xr0, xi0;
    std::vector<char> inc;                       // 本次决策要算相干度的时延
    std::vector<const float*> tr, ti;            // 本帧各抽头的参考谱行
    std::vector<float> zero;
    double prof[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // 时延估计 / 卡尔曼+网络 / 帧数 / [先验误差, 特征+网络, 更新]                  // 时延估计 / 卡尔曼+网络 / 帧数

    static void rd(void* d, size_t sz, size_t n, FILE* f) { if (fread(d, sz, n, f) != n) { fprintf(stderr, "nlin: 参数文件截断\n"); fb_fail(); } }
    void load(const char* path) {
        std::map<std::string, Ten> Pm;
        FILE* f = fb_open_model(path); if (!f) { perror(path); fb_fail(); } char mg[4]; int cnt; rd(mg, 1, 4, f); rd(&cnt, 4, 1, f); if (memcmp(mg, "GTC1", 4)) { fprintf(stderr, "nlin: 文件格式错\n"); fb_fail(); }
        for (int i = 0; i < cnt; ++i) { int nl, nd; rd(&nl, 4, 1, f); std::string k(nl, ' '); rd(&k[0], 1, nl, f); rd(&nd, 4, 1, f); Ten tt; tt.d.resize(nd); if (nd) rd(tt.d.data(), 4, nd, f); size_t n = 1; for (int x : tt.d) n *= x; tt.v.resize(n); if (n) rd(tt.v.data(), 4, n, f); Pm[k] = std::move(tt); }
        fclose(f);
        auto T = [&](const char* k) -> const Ten& { auto it = Pm.find(k); if (it == Pm.end()) { fprintf(stderr, "nlin: 缺参数 %s\n", k); fb_fail(); } return it->second; };
        const auto& c = T("nlin.cfg").v; N = (int)c[0]; H = (int)c[1]; L = (int)c[2]; NB = (int)c[3]; HID = (int)c[4]; AK = c[5]; F = N / 2 + 1;
        if (L != LC) { fprintf(stderr, "nlin: 抽头数 %d≠编译期 %d\n", L, LC); fb_fail(); }
        const auto& idx = T("nlin.idx").v; if ((int)idx.size() != F) { fprintf(stderr, "nlin: idx 长度 %zu≠F\n", idx.size()); fb_fail(); }
        bS.assign(NB, F); bE.assign(NB, 0); for (int k = 0; k < F; ++k) { int b = (int)idx[k]; bS[b] = std::min(bS[b], k); bE[b] = std::max(bE[b], k + 1); }
        ih = T("gru.ih").v; hh = T("gru.hh").v; bi = T("gru.bi").v; bh = T("gru.bh").v; ow = T("out.w").v; ob = T("out.b").v;
        if ((int)ih.size() != 3 * HID * 5 * NB || (int)ow.size() != 2 * NB * HID) { fprintf(stderr, "nlin: 网络形状不符\n"); fb_fail(); }
        auto tp = [](const std::vector<float>& w, int R, int C) { std::vector<float> o((size_t)R * C); for (int r = 0; r < R; ++r) for (int c = 0; c < C; ++c) o[(size_t)c * R + r] = w[(size_t)r * C + c]; return o; };
        ihT = tp(ih, 3 * HID, 5 * NB); hhT = tp(hh, 3 * HID, HID); owT = tp(ow, 2 * NB, HID);
        const auto& p = T("tde.par").v; K = (int)p[0]; TA = p[1]; TW = (int)p[2]; THR = p[3]; RATIO = p[4]; HOLD = (int)p[5]; SC = (int)p[6]; AENV = p[7]; ENVDOM = p[8]; ENVHOLD = (int)p[9];
        auto d6 = [](float v) { return (tdT)(std::round((double)v * 1e6) / 1e6); };   // 参数文件是 float32：还原成 Python 里的十进制常数（0.995 等）
        TAd = d6(TA); AENVd = d6(AENV); THRd = d6(THR); RATIOd = d6(RATIO); ENVDOMd = d6(ENVDOM);
        FULLSTART = p[10] != 0; FLOOR0 = p[11]; FLOOR0d = d6(FLOOR0); NG = (int)p[12]; TOPG = (int)p[13]; MARGF = (int)p[14];
        for (float v : T("tde.bins").v) bins.push_back((int)v); for (float v : T("tde.edg").v) edg.push_back((int)v); NBIN = (int)bins.size(); NBND = (int)edg.size() - 1;
        gS.clear(); gE.clear(); { int q = NBIN / NG, r = NBIN % NG, s = 0; for (int g = 0; g < NG; ++g) { int n = q + (g < r ? 1 : 0); gS.push_back(s); gE.push_back(s + n); s += n; } }   // = np.array_split
        RX = K + L + 4;   // 时延峰最大 K−1 帧，回归量最远 = 峰 − MARGF + L − 1 帧前
        gdec = std::pow(0.99946688, H / 256.0);
        reset();
    }
    void reset() {
        t = 0; rxr.assign((size_t)RX * F, 0); rxi.assign((size_t)RX * F, 0); xbr.assign((size_t)RX * NBIN, 0); xbi.assign((size_t)RX * NBIN, 0); pxr.assign(RX, 0); xhd.assign((size_t)RX * NBND, 0);
        Sr.assign((size_t)K * NBIN, 0); Si.assign((size_t)K * NBIN, 0); Sxx.assign((size_t)K * NBIN, 0); Sdd.assign(NBIN, 0); bx.assign(NBND, 0); bd.assign(NBND, 0); xm.assign(NBND, 0); dm.assign(NBND, 0);
        dbr.assign(NBIN, 0); dbi.assign(NBIN, 0); Renv.assign(K, 0); inc.assign(K, 0); coh.assign(NBIN, 0); gsc.assign(K, 0); floor_ = FLOOR0d; nact = 0; cur = -1; cand = -1; cn = 0; ecn = 0; ecand = -1;
        gpk = 0; gcur = 1; gnact = 0; gnst = 0; glast = -1000000000L; gprev = 1.f; gch = 0; lfPrev = 0; lch = 0;
        Wr.assign((size_t)L * F, 0); Wi.assign((size_t)L * F, 0); P.assign((size_t)L * F, 1.f); tmp.assign((size_t)L * F, 0);
        qF.assign(F, 0); cF.assign(F, 0); pe.assign(F, 1e-4f); Yr.assign(F, 0); Yi.assign(F, 0); Er_.assign(F, 0); Ei_.assign(F, 0); pu.assign(F, 0); den.assign(F, 0); xr0.assign(F, 0); xi0.assign(F, 0);
        ft.assign(5 * NB, 0); h.assign(HID, 0); gx.assign(3 * HID, 0); gh.assign(3 * HID, 0); o.assign(2 * NB, 0); cB.assign(NB, 0); qB.assign(NB, 0);
        tr.assign(L, nullptr); ti.assign(L, nullptr); zero.assign(F, 0);
        Wcr.assign((size_t)L * F, 0); Wci.assign((size_t)L * F, 0); Pc.assign((size_t)L * F, 1.f); pec.assign(F, 1e-4f); Ecr.assign(F, 0); Eci.assign(F, 0); sEn.assign(F, 0); sEc.assign(F, 0); oneB.assign(NB, 1.f);
    }
    int lag() const { return cur; }

    // ---- 参考因果电平归一（nlin.ref_gain）：返回本帧增益 ----
    float refGain(const float* blk) {
        double pb = 0; for (int i = 0; i < H; ++i) pb += (double)blk[i] * blk[i]; pb /= H;
        gpk = std::max(gpk * gdec, pb); if (pb > 1e-6) ++gnact;
        if (gnact >= 40 && gpk > 0) {
            double gn = 0.31622776601 / std::sqrt(gpk), ddb = 20 * std::log10(gn / gcur); bool early = gnst == 1 && gnact == 125 && std::fabs(ddb) > 6;
            if (gnst == 0 || early || (std::fabs(ddb) > 8 && t - glast > 250)) { gcur = gn; ++gnst; glast = t; }
        }
        return (float)gcur;
    }

    // ---- 结合版时延估计（tde2.track_h）：返回本帧时延峰（帧，未锁定 −1）。X/D 为原始（未归一）谱 ----
    int tde(const float* Xr, const float* Xi, const float* Dr, const float* Di) {
        const int r0 = (int)(t % RX); float* xb_r = &xbr[(size_t)r0 * NBIN]; float* xb_i = &xbi[(size_t)r0 * NBIN]; tdT px = 0;
        for (int j = 0; j < NBIN; ++j) { int b = bins[j]; xb_r[j] = Xr[b]; xb_i[j] = Xi[b]; px += (tdT)Xr[b] * Xr[b] + (tdT)Xi[b] * Xi[b]; }
        pxr[r0] = px;
        if (px < 10 * floor_) floor_ = (tdT)0.999 * floor_ + (tdT)0.001 * px;
        const tdT thr = 30 * floor_ + (tdT)1e-12; const bool on = px > thr;
        for (int j = 0; j < NBIN; ++j) { dbr[j] = Dr[bins[j]]; dbi[j] = Di[bins[j]]; }   // 本帧麦克风抽稀频点：取一次，供下面 K 个时延共用
        const int nk = (int)std::min<long>(K, t + 1); bool any = false;
        for (int k = 0; k < nk; ++k) {
            const int rk = (int)((t - k) % RX); if (!(pxr[rk] > thr)) continue; any = true;
            const float* ar = &xbr[(size_t)rk * NBIN]; const float* ai = &xbi[(size_t)rk * NBIN]; tdT* sr = &Sr[(size_t)k * NBIN]; tdT* si = &Si[(size_t)k * NBIN]; tdT* sx = &Sxx[(size_t)k * NBIN];
            const tdT a = TAd, b = 1 - TAd;
            const tdT* __restrict dbr_ = dbr.data(); const tdT* __restrict dbi_ = dbi.data();
            for (int j = 0; j < NBIN; ++j) { const tdT dr = dbr_[j], di = dbi_[j];
                const tdT xr_ = ar[j], xi_ = ai[j]; sr[j] = a * sr[j] + b * (dr * xr_ + di * xi_); si[j] = a * si[j] + b * (di * xr_ - dr * xi_); sx[j] = a * sx[j] + b * (xr_ * xr_ + xi_ * xi_); }
        }
        if (any) for (int j = 0; j < NBIN; ++j) { const tdT dr = dbr[j], di = dbi[j]; Sdd[j] = TAd * Sdd[j] + (1 - TAd) * (dr * dr + di * di); }
        // 包络：24 个对数间隔频带的 ln 能量
        for (int q = 0; q < NBND; ++q) { tdT sx = 0, sd = 0; for (int b = edg[q]; b < edg[q + 1]; ++b) { sx += (tdT)Xr[b] * Xr[b] + (tdT)Xi[b] * Xi[b]; sd += (tdT)Dr[b] * Dr[b] + (tdT)Di[b] * Di[b]; } bx[q] = std::log(sx + (tdT)1e-12); bd[q] = std::log(sd + (tdT)1e-12); }
        if (t == 0) { xm = bx; dm = bd; }
        tdT* xh0 = &xhd[(size_t)r0 * NBND];
        for (int q = 0; q < NBND; ++q) { xm[q] = (tdT)0.985 * xm[q] + (tdT)0.015 * bx[q]; dm[q] = (tdT)0.985 * dm[q] + (tdT)0.015 * bd[q]; xh0[q] = bx[q] - xm[q]; bd[q] -= dm[q]; }
        if (on) {
            for (int k = 0; k < K; ++k) { tdT s = 0; if (k <= t) { const tdT* xq = &xhd[(size_t)((t - k) % RX) * NBND]; for (int q = 0; q < NBND; ++q) s += xq[q] * bd[q]; } Renv[k] = AENVd * Renv[k] + (1 - AENVd) * s; }
            ++nact;
        }
        if (t % SC == 0 && nact >= TW) {
            int pk[3], np_ = 0;   // 包络前 3 个局部峰（同值取下标小者，= Python 稳定排序）
            const int kmax = (int)std::min<long>(K, t);   // 只认 k < t 的峰：k ≥ t 的行没有真数据（k = t 是第 0 帧去均值 = 舍入噪尘），= tde2.py 的 TDE2_NODUST=1
            // 且峰值须 > 1e-6：麦克风数字静音时 bd − dm 是 EMA 舍入残渣，Python 里出现 1e-13 级假峰（真峰 ≥ 1e-2 量级），同 TDE2_NODUST=1
            for (int k = 0; k < kmax; ++k) {
                if (!(Renv[k] > (tdT)1e-6 && (k == 0 || Renv[k] >= Renv[k - 1]) && (k == K - 1 || Renv[k] >= Renv[k + 1]))) continue;
                int pos = np_; while (pos > 0 && Renv[k] > Renv[pk[pos - 1]]) --pos;
                if (pos < 3) { for (int z = std::min(np_, 2); z > pos; --z) pk[z] = pk[z - 1]; pk[pos] = k; np_ = std::min(np_ + 1, 3); }
            }
            std::fill(inc.begin(), inc.end(), 0); int ncs = 0;   // inc 在 reset() 里按 K 预分配，处理期不再分配   // 成员缓冲（10-09：static thread_local 在 Android 9（vivo，API 28）上段错误——ELF TLS 要 API 29+）
            if (cur < 0 && FULLSTART) { std::fill(inc.begin(), inc.end(), 1); ncs = K; }
            else { auto addc = [&](int c) { for (int j = std::max(0, c - 2); j < std::min(K, c + 3); ++j) if (!inc[j]) { inc[j] = 1; ++ncs; } }; for (int z = 0; z < np_; ++z) addc(pk[z]); if (cur >= 0) addc(cur); }
            if (ncs) {
                int best = -1; tdT sbest = -1;
                for (int k = 0; k < K; ++k) {
                    if (!inc[k]) continue; const tdT* sr = &Sr[(size_t)k * NBIN]; const tdT* si = &Si[(size_t)k * NBIN]; const tdT* sx = &Sxx[(size_t)k * NBIN];
                    tdT gv[16];
                    for (int g = 0; g < NG; ++g) { tdT s = 0; for (int j = gS[g]; j < gE[g]; ++j) s += (sr[j] * sr[j] + si[j] * si[j]) / (sx[j] * Sdd[j] + (tdT)1e-20); gv[g] = s / (gE[g] - gS[g]); }
                    std::sort(gv, gv + NG); tdT s = 0; for (int g = NG - TOPG; g < NG; ++g) s += gv[g]; s /= TOPG; gsc[k] = s;
                    if (s > sbest) { sbest = s; best = k; }
                }
                const bool strong = sbest >= THRd / 2;
                { static const long dbA = getenv("TDEDBG") ? atol(getenv("TDEDBG")) : -1; if (dbA >= 0 && t >= dbA && t < dbA + 8) fprintf(stderr, "C t=%ld best=%d sbest=%.6g cur=%d np=%d pk0=%d R0=%.6g px=%.6g floor=%.6g nact=%d\n", t, best, (double)sbest, cur, np_, np_ ? pk[0] : -1, np_ ? (double)Renv[pk[0]] : 0.0, (double)pxr[r0], (double)floor_, nact); }
                const bool edom = np_ >= 1 && Renv[pk[0]] > 0 && (np_ < 2 || Renv[pk[0]] > ENVDOMd * Renv[pk[1]]);
                if (cur < 0) { if (sbest >= THRd) cur = best; else if (nact >= 60 && edom && !strong) cur = pk[0]; }
                else if (strong) {
                    const tdT scur = inc[cur] ? gsc[cur] : (tdT)0;
                    if (best != cur && sbest > RATIOd * scur) { cn = std::abs(best - cand) <= 1 ? cn + SC : SC; cand = best; if (cn >= (std::abs(best - cur) >= 2 ? HOLD : 2 * HOLD)) { cur = best; cn = 0; } }
                    else cn = std::max(0, cn - SC);
                }
                else if (edom && std::abs(pk[0] - cur) >= 2) { ecn = std::abs(pk[0] - ecand) <= 1 ? ecn + SC : SC; ecand = pk[0]; if (ecn >= ENVHOLD) { cur = pk[0]; ecn = 0; } }
                else ecn = std::max(0, ecn - SC);
            }
        }
        return cur;
    }

    static inline float sigm(float x) { return 1.f / (1.f + expf(-x)); }

    void kf(std::vector<float>& Wr_, std::vector<float>& Wi_, std::vector<float>& P_, const std::vector<float>& pe_, const std::vector<float>& Er, const std::vector<float>& Ei, const float* cb, const float* qb, float g) {
        const float A2 = AK * AK, Aq = 1 - AK * AK;
        std::fill(den.begin(), den.end(), 0.f);
        for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; float* wr = &Wr_[(size_t)l * F]; float* wi = &Wi_[(size_t)l * F]; float* p = &P_[(size_t)l * F];
            for (int b = 0; b < NB; ++b) { const float qa = qb[b] * Aq;
                for (int k = bS[b]; k < bE[b]; ++k) { const float a = xr[k] * g, c = xi[k] * g; const float pn = A2 * p[k] + qa * (wr[k] * wr[k] + wi[k] * wi[k]) + 1e-8f; p[k] = pn; den[k] += pn * (a * a + c * c); } } }
        for (int b = 0; b < NB; ++b) for (int k = bS[b]; k < bE[b]; ++k) den[k] = 1.f / (den[k] + cb[b] * pe_[k] + 1e-8f);
        for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; float* wr = &Wr_[(size_t)l * F]; float* wi = &Wi_[(size_t)l * F]; float* p = &P_[(size_t)l * F];
            for (int k = 0; k < F; ++k) { const float a = xr[k] * g, c = xi[k] * g; const float iv = den[k], kk = p[k] * iv;
                const float gr = Er[k] * a + Ei[k] * c, gi = Ei[k] * a - Er[k] * c; wr[k] = AK * wr[k] + kk * gr; wi[k] = AK * wi[k] + kk * gi;   // W 的 A 缩放挪到这一趟（少写一遍 W）
                p[k] = std::max(p[k] * (1.f - p[k] * (a * a + c * c) * iv), 1e-10f); } }
    }
    void kfc(float g) {   // 经典版（c = q = 1）卡尔曼更新：与 kf(…, 1, 1, g) 逐式相同，不分频带、整段平铺循环（向量化更好）
        const float A2 = AK * AK, Aq = 1 - AK * AK;
        std::fill(den.begin(), den.end(), 0.f);
        for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; const float* wr = &Wcr[(size_t)l * F]; const float* wi = &Wci[(size_t)l * F]; float* p = &Pc[(size_t)l * F];
            for (int k = 0; k < F; ++k) { const float a = xr[k] * g, c = xi[k] * g; const float pn = A2 * p[k] + Aq * (wr[k] * wr[k] + wi[k] * wi[k]) + 1e-8f; p[k] = pn; den[k] += pn * (a * a + c * c); } }
        for (int k = 0; k < F; ++k) den[k] = 1.f / (den[k] + pec[k] + 1e-8f);
        for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; float* wr = &Wcr[(size_t)l * F]; float* wi = &Wci[(size_t)l * F]; float* p = &Pc[(size_t)l * F];
            for (int k = 0; k < F; ++k) { const float a = xr[k] * g, c = xi[k] * g; const float iv = den[k], kk = p[k] * iv;
                const float gr = Ecr[k] * a + Eci[k] * c, gi = Eci[k] * a - Ecr[k] * c; wr[k] = AK * wr[k] + kk * gr; wi[k] = AK * wi[k] + kk * gi;
                p[k] = std::max(p[k] * (1.f - p[k] * (a * a + c * c) * iv), 1e-10f); } }
    }
    void setDual(bool on, float tau = 0.2f, float marginDb = 3.f) { dual = on; dualAd = std::exp(-(double)H / (48000.0 * tau)); dualMd = std::pow(10.0, marginDb / 10.0); }
    // ---- 一帧：X/D 参考与麦克风谱，blk = 参考最新一跳时域；输出 E ----
    void step(const float* Xr, const float* Xi, const float* Dr, const float* Di, const float* blk, float* Eor, float* Eoi) {
        auto c0 = std::chrono::steady_clock::now();
        const int r0 = (int)(t % RX); memcpy(&rxr[(size_t)r0 * F], Xr, F * 4); memcpy(&rxi[(size_t)r0 * F], Xi, F * 4);
        const float g = refGain(blk);
        const int pk = tde(Xr, Xi, Dr, Di); const int lf = pk < 0 ? 0 : std::max(pk - MARGF, 0);
        auto c1 = std::chrono::steady_clock::now();
        // 时延变化：小位移抽头跟着平移（新进抽头 W=0、P=1），首次采用或 ≥L 整体复位（nlin.regress 的 sidx）
        const int dl = t == 0 ? 0 : lf - lfPrev; lfPrev = lf;
        if (dl != 0) {
            ++lch; const bool rs = lch == 1 || std::abs(dl) >= L;
            if (rs) { for (auto* v : {&Wr, &Wi, &Wcr, &Wci}) std::fill(v->begin(), v->end(), 0.f); std::fill(P.begin(), P.end(), 1.f); std::fill(Pc.begin(), Pc.end(), 1.f); }
            else for (float* A_ : {Wr.data(), Wi.data(), P.data(), Wcr.data(), Wci.data(), Pc.data()}) {
                if (!dual && (A_ == Wcr.data() || A_ == Wci.data() || A_ == Pc.data())) continue;
                const float fill = (A_ == P.data() || A_ == Pc.data()) ? 1.f : 0.f; memcpy(tmp.data(), A_, (size_t)L * F * 4);
                for (int l = 0; l < L; ++l) { const int s = l + dl; if (s >= 0 && s < L) memcpy(A_ + (size_t)l * F, &tmp[(size_t)s * F], F * 4); else std::fill(A_ + (size_t)l * F, A_ + (size_t)(l + 1) * F, fill); }
            }
        }
        // 参考增益变化：W、P 反向缩放；首次步进且 ≥10 dB 时 P 复位为 1
        if (t > 0) { const float rr = g / gprev;
            if (rr != 1.f) { ++gch; const bool big = gch == 1 && (rr >= 3.1623f || rr <= 1.f / 3.1623f); const float q = 1.f / std::fabs(rr), q2 = q * q;
                for (size_t i = 0; i < Wr.size(); ++i) { Wr[i] *= q; Wi[i] *= q; P[i] = big ? 1.f : P[i] * q2; }
                if (dual) for (size_t i = 0; i < Wcr.size(); ++i) { Wcr[i] *= q; Wci[i] *= q; Pc[i] = big ? 1.f : Pc[i] * q2; } } }
        gprev = g;
        // 回归量：第 l 抽头 = 第 t−lf−l 帧参考谱 × 本帧增益（越界置零）
        for (int l = 0; l < L; ++l) { const long s = t - lf - l; if (s < 0) { tr[l] = ti[l] = zero.data(); } else { tr[l] = &rxr[(size_t)(s % RX) * F]; ti[l] = &rxi[(size_t)(s % RX) * F]; } }
        // ---- core：先验误差 ----
        std::fill(Yr.begin(), Yr.end(), 0.f); std::fill(Yi.begin(), Yi.end(), 0.f); std::fill(pu.begin(), pu.end(), 0.f);
        for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; const float* wr = &Wr[(size_t)l * F]; const float* wi = &Wi[(size_t)l * F];
            for (int k = 0; k < F; ++k) { const float a = xr[k] * g, b = xi[k] * g; Yr[k] += wr[k] * a - wi[k] * b; Yi[k] += wr[k] * b + wi[k] * a; pu[k] += a * a + b * b; } }
        for (int k = 0; k < F; ++k) { const float er = Dr[k] - Yr[k], ei = Di[k] - Yi[k]; Er_[k] = er; Ei_[k] = ei; pe[k] = 0.9f * pe[k] + 0.1f * (er * er + ei * ei); }
        if (dual) {   // 经典版先验误差（同回归量）
            std::fill(Ecr.begin(), Ecr.end(), 0.f); std::fill(Eci.begin(), Eci.end(), 0.f);
            for (int l = 0; l < L; ++l) { const float* xr = tr[l]; const float* xi = ti[l]; const float* wr = &Wcr[(size_t)l * F]; const float* wi = &Wci[(size_t)l * F];
                for (int k = 0; k < F; ++k) { const float a = xr[k] * g, b = xi[k] * g; Ecr[k] += wr[k] * a - wi[k] * b; Eci[k] += wr[k] * b + wi[k] * a; } }
            for (int k = 0; k < F; ++k) { const float er = Dr[k] - Ecr[k], ei = Di[k] - Eci[k]; Ecr[k] = er; Eci[k] = ei; pec[k] = 0.9f * pec[k] + 0.1f * (er * er + ei * ei); }
        }
        auto cA = std::chrono::steady_clock::now();
        // ---- 网络：频带特征 → GRUCell → c、q ----
        // 10-09 提速：频带和用 float（= Python 的 float32 矩阵乘）；GRU/输出层权重已转置成 [输入][行]，按列累加（每步 192/64 个连续行，NEON 友好）
        for (int b = 0; b < NB; ++b) {
            float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0; const float* x0r = tr[0]; const float* x0i = ti[0];
            for (int k = bS[b]; k < bE[b]; ++k) { const float a = x0r[k] * g, c = x0i[k] * g; s0 += a * a + c * c; s1 += pu[k]; s2 += Dr[k] * Dr[k] + Di[k] * Di[k]; s3 += Er_[k] * Er_[k] + Ei_[k] * Ei_[k]; s4 += Yr[k] * Yr[k] + Yi[k] * Yi[k]; }
            const float inv = 1.f / (bE[b] - bS[b]); const float sv[5] = {s0, s1, s2, s3, s4};
            for (int j = 0; j < 5; ++j) ft[j * NB + b] = (log10f(sv[j] * inv + 1e-8f) + 3.f) * (1.f / 3.f);
        }
        auto cF_ = std::chrono::steady_clock::now();
        const int I = 5 * NB, G3 = 3 * HID;
        { float* __restrict ax = gx.data(); float* __restrict ah = gh.data(); memcpy(ax, bi.data(), G3 * 4); memcpy(ah, bh.data(), G3 * 4);
          for (int i = 0; i < I; ++i) { const float v = ft[i]; const float* __restrict w = &ihT[(size_t)i * G3]; for (int r = 0; r < G3; ++r) ax[r] += w[r] * v; }
          for (int i = 0; i < HID; ++i) { const float v = h[i]; const float* __restrict w = &hhT[(size_t)i * G3]; for (int r = 0; r < G3; ++r) ah[r] += w[r] * v; } }
        auto cG_ = std::chrono::steady_clock::now();
        for (int j = 0; j < HID; ++j) { const float rg = sigm(gx[j] + gh[j]), zg = sigm(gx[HID + j] + gh[HID + j]), ng = tanhf(gx[2 * HID + j] + rg * gh[2 * HID + j]); h[j] = (1 - zg) * ng + zg * h[j]; }
        { float* __restrict ao = o.data(); memcpy(ao, ob.data(), 2 * NB * 4); for (int i = 0; i < HID; ++i) { const float v = h[i]; const float* __restrict w = &owT[(size_t)i * 2 * NB]; for (int r = 0; r < 2 * NB; ++r) ao[r] += w[r] * v; } }
        for (int b = 0; b < NB; ++b) { cB[b] = expf(std::min(8.f, std::max(-8.f, o[b]))); qB[b] = expf(std::min(8.f, std::max(-8.f, o[NB + b]))); }
        auto cB_ = std::chrono::steady_clock::now();
        // ---- 对角卡尔曼更新（网络版；双滤波器时经典版 c = q = 1 再更新一次）----
        kf(Wr, Wi, P, pe, Er_, Ei_, cB.data(), qB.data(), g);
        if (dual) {
            kfc(g);
            for (int k = 0; k < F; ++k) { const double en = (double)Er_[k] * Er_[k] + (double)Ei_[k] * Ei_[k], ec = (double)Ecr[k] * Ecr[k] + (double)Eci[k] * Eci[k];
                sEn[k] = dualAd * sEn[k] + (1 - dualAd) * en; sEc[k] = dualAd * sEc[k] + (1 - dualAd) * ec;
                if (!(sEn[k] <= sEc[k] * dualMd)) { Er_[k] = Ecr[k]; Ei_[k] = Eci[k]; } }
        }
        memcpy(Eor, Er_.data(), F * 4); memcpy(Eoi, Ei_.data(), F * 4);
        auto c2 = std::chrono::steady_clock::now();
        prof[0] += std::chrono::duration<double, std::milli>(c1 - c0).count(); prof[1] += std::chrono::duration<double, std::milli>(c2 - c1).count(); prof[2] += 1; prof[3] += std::chrono::duration<double, std::milli>(cA - c1).count(); prof[4] += std::chrono::duration<double, std::milli>(cB_ - cA).count(); prof[5] += std::chrono::duration<double, std::milli>(c2 - cB_).count(); prof[6] += std::chrono::duration<double, std::milli>(cF_ - cA).count(); prof[7] += std::chrono::duration<double, std::milli>(cG_ - cF_).count();
        ++t;
    }
};
}  // namespace fb
