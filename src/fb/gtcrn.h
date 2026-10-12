// 流式推理公共部分：.gtc 参数文件读取（GTC1 格式）、张量、激活函数与 GRU 等基础算子（单帧、单线程、每帧零分配）。
// 权重在 load() 里一次定位，每帧不查名字、不分配内存。
// 激活一律 [通道][频率] 行主序。用法：fb::Gtcrn g; g.load("model.gtc", nfft); g.step(se, sy, out);  se/sy/out = 2F 个 float（实、虚交错）。
#pragma once
// aec48-android：模型从库内嵌数据加载（路径 "mem:<名字>" → fmemopen），其余路径照常 fopen。
#include <cstdio>
#include <cstring>
namespace fb { struct MemModel { const char* name; const unsigned char* p; size_t n; };
inline MemModel* fb_mem_models() { static MemModel m[4] = {}; return m; }
inline void fb_register_model(const char* name, const unsigned char* p, size_t n) { MemModel* m = fb_mem_models(); for (int i = 0; i < 4; ++i) if (!m[i].name || !strcmp(m[i].name, name)) { m[i] = {name, p, n}; return; } }
inline FILE* fb_open_model(const char* path) {
    if (!strncmp(path, "mem:", 4)) { MemModel* m = fb_mem_models(); for (int i = 0; i < 4; ++i) if (m[i].name && !strcmp(m[i].name, path + 4)) return fmemopen((void*)m[i].p, m[i].n, "rb"); return nullptr; }
    return fopen(path, "rb"); } }
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#ifdef GTCRN_PROF
#include <chrono>
#define PT(k) do { auto _n = std::chrono::steady_clock::now(); prof[k] += std::chrono::duration<double, std::milli>(_n - _t).count(); _t = _n; } while (0)
#else
#define PT(k) do {} while (0)
#endif
namespace fb {
struct Ten { std::vector<int> d; std::vector<float> v; };

struct Gtcrn {
    static const int FE = 129, F0 = 65, NB = 33;
    std::map<std::string, Ten> P; int F = 0, W = 0, Wh = 0, NI = 7; bool rho = true, res = true; float floorDb = 20.f;
    double prof[12] = {0};
    static const char* profName(int k) { static const char* n[] = {"特征+ERB+SFE", "en0", "en1", "en2-4", "dp0", "dp1", "de0-2", "de3-4", "iERB+掩码"}; return n[k]; }

    struct CB { const float *w, *b; int O, Ig, K, g; float a; bool th; };
    struct GRU { const float *ih, *hh, *bi, *bh; int I, H; };
    struct GT { const float *p1w, *p1b, *dw, *db, *p2w, *p2b, *tfw, *tfb; float p1a, da; GRU tg; int d, head; std::vector<float> cache, h; };   // cache: [W][2d 槽][NB] 环形
    struct DP { GRU in[2][2], ex[2]; const float *ifw, *ifb, *efw, *efb; std::vector<float> ilw, ilb, elw, elb; float ile, ele; std::vector<float> h; };   // h: [W][NB]
    CB en0c, en1c, de3c, de4c; GT gt_[6]; DP dp_[2]; const float *erb = nullptr, *ierb = nullptr; std::vector<int> es, ee, is, ie;
    // 缓冲（load 里一次分配）
    std::vector<float> x7, xe, x21, en0, en1, enA, enB, enC, xa, xb, sf, h1, hp, h2, ytmp, xi, gi, gh, ev, od, xp, d3, m2, mF;

    const Ten& T(const std::string& k) const { auto it = P.find(k); if (it == P.end()) { fprintf(stderr, "gtcrn: 缺参数 %s\n", k.c_str()); exit(1); } return it->second; }
    const float* p(const std::string& k) const { return T(k).v.data(); }
    static void rd(void* d, size_t sz, size_t n, FILE* f) { if (fread(d, sz, n, f) != n) { fprintf(stderr, "gtcrn: 参数文件截断\n"); exit(1); } }
    void load(const char* path, int nfft, bool rho_ = true, bool res_ = true, float floor_ = 20.f) {
        FILE* f = fb_open_model(path); if (!f) { perror(path); exit(1); } char mg[4]; int cnt; rd(mg, 1, 4, f); rd(&cnt, 4, 1, f); if (memcmp(mg, "GTC1", 4)) { fprintf(stderr, "gtcrn: 文件格式错\n"); exit(1); }
        for (int i = 0; i < cnt; ++i) { int nl, nd; rd(&nl, 4, 1, f); std::string k(nl, ' '); rd(&k[0], 1, nl, f); rd(&nd, 4, 1, f); Ten t; t.d.resize(nd); if (nd) rd(t.d.data(), 4, nd, f); size_t n = 1; for (int x : t.d) n *= x; t.v.resize(n); if (n) rd(t.v.data(), 4, n, f); P[k] = std::move(t); }
        fclose(f); F = nfft / 2 + 1; rho = rho_; res = res_; floorDb = floor_; NI = rho ? 7 : 6; W = T("en0.w").d[0]; Wh = W / 2;
        if (T("erb_fc").d[1] != F - F0) { fprintf(stderr, "gtcrn: nfft %d 与参数不符（erb_fc %d 列）\n", nfft, T("erb_fc").d[1]); exit(1); }
        if (W > 64) { fprintf(stderr, "gtcrn: 宽 %d 超出栈缓冲上限 64\n", W); exit(1); }
        erb = p("erb_fc"); ierb = p("ierb_fc");
        auto rng = [](const Ten& t, std::vector<int>& s, std::vector<int>& e) { int R = t.d[0], C = t.d[1]; s.assign(R, 0); e.assign(R, 0); for (int r = 0; r < R; ++r) { int a = C, b = 0; for (int c = 0; c < C; ++c) if (t.v[r * C + c] != 0.f) { a = std::min(a, c); b = c + 1; } s[r] = a < b ? a : 0; e[r] = a < b ? b : 0; } };
        rng(T("erb_fc"), es, ee); rng(T("ierb_fc"), is, ie);
        auto cb = [&](const char* n, CB& c) { std::string s(n); const Ten& w = T(s + ".w"); c.w = w.v.data(); c.b = p(s + ".b"); c.O = w.d[0]; c.Ig = w.d[1]; c.K = w.d[2]; c.g = (int)T(s + ".g").v[0]; const Ten& a = T(s + ".a"); c.th = a.v.empty(); c.a = c.th ? 0.f : a.v[0]; if (c.K != 5) { fprintf(stderr, "gtcrn: %s 核长 %d≠5\n", n, c.K); exit(1); } };
        cb("en0", en0c); cb("en1", en1c); cb("de3", de3c); cb("de4", de4c);
        auto gru = [&](const std::string& s, GRU& g) { const Ten& ih = T(s + "ih"); g.ih = ih.v.data(); g.hh = p(s + "hh"); g.bi = p(s + "bi"); g.bh = p(s + "bh"); g.H = ih.d[0] / 3; g.I = ih.d[1]; if (g.H > 64) { fprintf(stderr, "gtcrn: GRU 隐藏 %d 超出上限\n", g.H); exit(1); } };
        const char* gn[6] = {"en2", "en3", "en4", "de0", "de1", "de2"};
        for (int i = 0; i < 6; ++i) { std::string s(gn[i]); GT& g = gt_[i]; g.p1w = p(s + ".p1w"); g.p1b = p(s + ".p1b"); g.p1a = T(s + ".p1a").v[0]; g.dw = p(s + ".dw"); g.db = p(s + ".db"); g.da = T(s + ".da").v[0];
            g.p2w = p(s + ".p2w"); g.p2b = p(s + ".p2b"); gru(s + ".tg", g.tg); g.tfw = p(s + ".tfw"); g.tfb = p(s + ".tfb"); g.d = (int)T(s + ".dil").v[0]; g.head = 0; g.cache.assign((size_t)W * 2 * g.d * NB, 0.f); g.h.assign(W, 0.f); }
        for (int j = 0; j < 2; ++j) { std::string n = "dp" + std::to_string(j) + "."; DP& d = dp_[j];
            for (int gI = 0; gI < 2; ++gI) for (int dir = 0; dir < 2; ++dir) gru(n + (gI ? "ib" : "ia") + (dir ? "r." : "f."), d.in[gI][dir]);
            gru(n + "ea.", d.ex[0]); gru(n + "eb.", d.ex[1]);
            d.ifw = p(n + "ifw"); d.ifb = p(n + "ifb"); d.efw = p(n + "efw"); d.efb = p(n + "efb"); d.ile = T(n + "ile").v[0]; d.ele = T(n + "ele").v[0];
            auto tr = [&](const float* s, std::vector<float>& o) { o.resize((size_t)W * NB); for (int f = 0; f < NB; ++f) for (int c = 0; c < W; ++c) o[c * NB + f] = s[f * W + c]; };   // LN 权重 (33, W) → [W][33]
            tr(p(n + "ilw"), d.ilw); tr(p(n + "ilb"), d.ilb); tr(p(n + "elw"), d.elw); tr(p(n + "elb"), d.elb); d.h.assign((size_t)W * NB, 0.f); }
        const size_t WN = (size_t)W * NB; const int CM = std::max(NI * 3, W);
        x7.assign(NI * F, 0); xe.assign(NI * FE, 0); x21.assign(NI * 3 * FE, 0); en0.assign(W * 65, 0); en1.assign(WN, 0); enA.assign(WN, 0); enB.assign(WN, 0); enC.assign(WN, 0); xa.assign(WN, 0); xb.assign(WN, 0);
        sf.assign(Wh * 3 * NB, 0); h1.assign(WN, 0); hp.assign(WN, 0); h2.assign(Wh * NB, 0); ytmp.assign(WN, 0); xi.assign(WN, 0); gi.assign(3 * WN, 0); gh.assign(3 * WN, 0);
        ev.assign((size_t)CM * 70, 0); od.assign((size_t)CM * 70, 0); xp.assign((size_t)W * 70, 0); d3.assign(W * 65, 0); m2.assign(2 * FE, 0); mF.assign(2 * F, 0);
    }
    void reset() { for (auto& g : gt_) { std::fill(g.cache.begin(), g.cache.end(), 0.f); std::fill(g.h.begin(), g.h.end(), 0.f); g.head = 0; } for (auto& d : dp_) std::fill(d.h.begin(), d.h.end(), 0.f); }

    // ---- 基本算子 ----
    // tanh：Eigen 的 13/6 次有理逼近（全实数轴最大绝对误差 3.3e-7，本机实测）；无 exp、可向量化。sigmoid = 0.5·tanh(x/2)+0.5（误差 2.3e-7）。
    // 10-08 vivo 实测 GRU 每帧约一万次 exp/tanh 是 DPGRNN 的主要耗时；限幅同时避免 -ffast-math 下 exp 溢出成 NaN（x86 实测过）。
    static inline float ftanh(float x) {
        x = std::min(7.90531110763549805f, std::max(-7.90531110763549805f, x)); const float x2 = x * x;
        float p = x2 * -2.76076847742355e-16f + 2.00018790482477e-13f; p = p * x2 + -8.60467152213735e-11f; p = p * x2 + 5.12229709037114e-08f; p = p * x2 + 1.48572235717979e-05f; p = p * x2 + 6.37261928875436e-04f; p = p * x2 + 4.89352455891786e-03f; p = p * x;
        float q = x2 * 1.19825839466702e-06f + 1.18534705686654e-04f; q = q * x2 + 2.26843463243900e-03f; q = q * x2 + 4.89352518554385e-03f; return p / q; }
    static inline float sigm(float x) { return 0.5f * ftanh(0.5f * x) + 0.5f; }
    static inline void prelu(float* x, int n, float a) { for (int i = 0; i < n; ++i) { float v = x[i]; x[i] = std::max(v, 0.f) + a * std::min(v, 0.f); } }
    // y[O][N] = w[O][I] · x[I][N] + b
    static void pw(const float* __restrict w, const float* __restrict b, const float* __restrict x, float* __restrict y, int O, int I, int N) {
        for (int o = 0; o < O; ++o) { float* __restrict yo = y + o * N; const float bv = b[o]; for (int n = 0; n < N; ++n) yo[n] = bv;
            const float* wo = w + o * I; for (int i = 0; i < I; ++i) { const float wv = wo[i]; const float* __restrict xi = x + i * N; for (int n = 0; n < N; ++n) yo[n] += wv * xi[n]; } }
    }
    static void sfe(const float* x, float* y, int C, int N) {
        for (int c = 0; c < C; ++c) { const float* xc = x + c * N; float* y0 = y + (c * 3) * N; float* y1 = y0 + N; float* y2 = y1 + N;
            y0[0] = 0; memcpy(y0 + 1, xc, (N - 1) * 4); memcpy(y1, xc, N * 4); memcpy(y2, xc + 1, (N - 1) * 4); y2[N - 1] = 0; }
    }
    // 频率向 (1,5) 卷积、步长 2、补 2：补边后拆偶相 E[m]=xp[2m]、奇相 O[m]=xp[2m+1]；y[f] = w0E[f]+w1O[f]+w2E[f+1]+w3O[f+1]+w4E[f+2]
    void convF(const CB& c, const float* x, int Fi, float* y) {
        const int I = c.Ig * c.g, Fo = (Fi - 1) / 2 + 1, Ne = Fo + 2, Og = c.O / c.g;
        for (int i = 0; i < I; ++i) { const float* xs = x + i * Fi; float* E = &ev[i * Ne]; float* Od = &od[i * Ne];
            for (int m = 0; m < Ne; ++m) { int a = 2 * m - 2, b = 2 * m - 1; E[m] = (a >= 0 && a < Fi) ? xs[a] : 0.f; Od[m] = (b >= 0 && b < Fi) ? xs[b] : 0.f; } }
        for (int o = 0; o < c.O; ++o) { float* __restrict yo = y + o * Fo; const float bv = c.b[o]; for (int f = 0; f < Fo; ++f) yo[f] = bv; const int g0 = (o / Og) * c.Ig;
            for (int i = 0; i < c.Ig; ++i) { const float* wk = c.w + (o * c.Ig + i) * 5; const float w0 = wk[0], w1 = wk[1], w2 = wk[2], w3 = wk[3], w4 = wk[4];
                const float* __restrict E = &ev[(g0 + i) * Ne]; const float* __restrict Od = &od[(g0 + i) * Ne];
                for (int f = 0; f < Fo; ++f) yo[f] += w0 * E[f] + w1 * Od[f] + w2 * E[f + 1] + w3 * Od[f + 1] + w4 * E[f + 2]; } }
        const int n = c.O * Fo; if (c.th) for (int i = 0; i < n; ++i) y[i] = ftanh(y[i]); else prelu(y, n, c.a);
    }
    // 频率向 (1,5) 反卷积、步长 2、裁 2（聚集式）：q[m+1]=x[m]（两侧补 0）；y[2m] = w0q[m+2]+w2q[m+1]+w4q[m]，y[2m+1] = w1q[m+2]+w3q[m+1]
    void deconvF(const CB& c, const float* x, int Fi, float* y) {
        const int I = c.Ig * c.g, Fo = 2 * Fi - 1, Np = Fi + 2, Og = c.O / c.g;
        for (int i = 0; i < I; ++i) { float* q = &xp[i * Np]; q[0] = 0; memcpy(q + 1, x + i * Fi, Fi * 4); q[Fi + 1] = 0; }
        float* Ye = &ev[0]; float* Yo = &od[0];
        for (int o = 0; o < c.O; ++o) { const float bv = c.b[o]; for (int m = 0; m < Fi; ++m) Ye[m] = bv; for (int m = 0; m < Fi - 1; ++m) Yo[m] = bv; const int g0 = (o / Og) * c.Ig;
            for (int i = 0; i < c.Ig; ++i) { const float* wk = c.w + (o * c.Ig + i) * 5; const float w0 = wk[0], w1 = wk[1], w2 = wk[2], w3 = wk[3], w4 = wk[4]; const float* __restrict q = &xp[(g0 + i) * Np];
                for (int m = 0; m < Fi; ++m) Ye[m] += w0 * q[m + 2] + w2 * q[m + 1] + w4 * q[m];
                for (int m = 0; m < Fi - 1; ++m) Yo[m] += w1 * q[m + 2] + w3 * q[m + 1]; }
            float* yo = y + o * Fo; for (int m = 0; m < Fi - 1; ++m) { yo[2 * m] = Ye[m]; yo[2 * m + 1] = Yo[m]; } yo[2 * (Fi - 1)] = Ye[Fi - 1]; }
        const int n = c.O * Fo; if (c.th) for (int i = 0; i < n; ++i) y[i] = ftanh(y[i]); else prelu(y, n, c.a);
    }
    // GRU 门（批 N，[通道][N] 布局）：gi/gh = (3H)×N 已含偏置；h [H][N] 原地更新
    static void gruGates(const float* __restrict gi, const float* __restrict gh, float* __restrict h, int H, int N) {
        for (int j = 0; j < H; ++j) { const float *ir = gi + j * N, *iz = gi + (H + j) * N, *in = gi + (2 * H + j) * N, *hr = gh + j * N, *hz = gh + (H + j) * N, *hn = gh + (2 * H + j) * N; float* hj = h + j * N;
            for (int n = 0; n < N; ++n) { float r = sigm(ir[n] + hr[n]), z = sigm(iz[n] + hz[n]), nn = ftanh(in[n] + r * hn[n]); hj[n] = (1 - z) * nn + z * hj[n]; } }
    }
    // 单样本 GRU（TRA 与 intra 递推用）：h[H] 原地；gx = 已算好的输入投影（含 bi），为空则现算
    static void gru1(const GRU& g, const float* x, const float* gx, float* h) {
        float a[192], b[192]; const int H = g.H, H3 = 3 * H;
        if (!gx) { for (int r = 0; r < H3; ++r) { float s = g.bi[r]; const float* w = g.ih + r * g.I; for (int i = 0; i < g.I; ++i) s += w[i] * x[i]; a[r] = s; } gx = a; }
        for (int r = 0; r < H3; ++r) { float s = g.bh[r]; const float* w = g.hh + r * H; for (int i = 0; i < H; ++i) s += w[i] * h[i]; b[r] = s; }
        for (int j = 0; j < H; ++j) { float r = sigm(gx[j] + b[j]), z = sigm(gx[H + j] + b[H + j]), nn = ftanh(gx[2 * H + j] + r * b[2 * H + j]); h[j] = (1 - z) * nn + z * h[j]; }
    }
    void gt(GT& g, const float* x, float* y) {
        const int L = 2 * g.d;
        sfe(x, &sf[0], Wh, NB); pw(g.p1w, g.p1b, &sf[0], &h1[0], W, Wh * 3, NB); prelu(&h1[0], W * NB, g.p1a);
        // 深度卷积：时间抽头 = 环形缓存里最老的槽（t−2d）、其后第 d 个槽（t−d）、当前帧；频率 3 抽头补 1（内部向量化 + 两端单算）
        const int s0 = g.head, s1 = (g.head + g.d) % L;
        for (int c = 0; c < W; ++c) { const float* r0 = &g.cache[((size_t)c * L + s0) * NB]; const float* r1 = &g.cache[((size_t)c * L + s1) * NB]; const float* r2 = &h1[c * NB]; const float* w = g.dw + c * 9; float* yo = &hp[c * NB]; const float bv = g.db[c];
            for (int f = 1; f < NB - 1; ++f) yo[f] = bv + w[0] * r0[f - 1] + w[1] * r0[f] + w[2] * r0[f + 1] + w[3] * r1[f - 1] + w[4] * r1[f] + w[5] * r1[f + 1] + w[6] * r2[f - 1] + w[7] * r2[f] + w[8] * r2[f + 1];
            yo[0] = bv + w[1] * r0[0] + w[2] * r0[1] + w[4] * r1[0] + w[5] * r1[1] + w[7] * r2[0] + w[8] * r2[1];
            yo[NB - 1] = bv + w[0] * r0[NB - 2] + w[1] * r0[NB - 1] + w[3] * r1[NB - 2] + w[4] * r1[NB - 1] + w[6] * r2[NB - 2] + w[7] * r2[NB - 1];
            memcpy(&g.cache[((size_t)c * L + s0) * NB], r2, NB * 4); }   // 最老的槽被当前帧覆盖
        g.head = (g.head + 1) % L;
        prelu(&hp[0], W * NB, g.da); pw(g.p2w, g.p2b, &hp[0], &h2[0], Wh, W, NB);
        float zt[64], at[64];
        for (int c = 0; c < Wh; ++c) { float a = 0; const float* hc = &h2[c * NB]; for (int f = 0; f < NB; ++f) a += hc[f] * hc[f]; zt[c] = a * (1.f / NB); }
        gru1(g.tg, zt, nullptr, &g.h[0]);
        for (int c = 0; c < Wh; ++c) { float a = g.tfb[c]; const float* w = g.tfw + c * W; for (int i = 0; i < W; ++i) a += w[i] * g.h[i]; at[c] = sigm(a); }
        for (int c = 0; c < Wh; ++c) { const float* hc = &h2[c * NB]; float* y0 = y + (2 * c) * NB; const float s = at[c]; for (int f = 0; f < NB; ++f) y0[f] = hc[f] * s; memcpy(y + (2 * c + 1) * NB, x + (Wh + c) * NB, NB * 4); }
    }
    static void layernorm(float* x, int n, const float* w, const float* b, float eps) {
        double m = 0, v = 0; for (int i = 0; i < n; ++i) m += x[i]; m /= n; for (int i = 0; i < n; ++i) { double t = x[i] - m; v += t * t; } v /= n;
        const float is = (float)(1.0 / std::sqrt(v + eps)), mf = (float)m; for (int i = 0; i < n; ++i) x[i] = (x[i] - mf) * is * w[i] + b[i];
    }
    // DPGRNN（[W][NB] 布局）：x 进、out 出
    void dp(DP& d, const float* x, float* out) {
        const size_t WN = (size_t)W * NB;
        for (int gI = 0; gI < 2; ++gI) for (int dir = 0; dir < 2; ++dir) { const GRU& g = d.in[gI][dir]; const int H = g.H;
            pw(g.ih, g.bi, x + gI * Wh * NB, &gi[0], 3 * H, Wh, NB);                     // 输入投影一次算完 33 步：(3H)×NB
            float h[64] = {0}, gx[192];
            for (int s = 0; s < NB; ++s) { const int f = dir ? NB - 1 - s : s; for (int r = 0; r < 3 * H; ++r) gx[r] = gi[r * NB + f]; gru1(g, nullptr, gx, h);
                for (int j = 0; j < H; ++j) ytmp[(gI * Wh + dir * H + j) * NB + f] = h[j]; } }
        pw(d.ifw, d.ifb, &ytmp[0], &xi[0], W, W, NB); layernorm(&xi[0], (int)WN, &d.ilw[0], &d.ilb[0], d.ile);
        for (size_t i = 0; i < WN; ++i) xi[i] += x[i];                                     // intra_out
        for (int gI = 0; gI < 2; ++gI) { const GRU& g = d.ex[gI]; const int H = g.H; float* hg = &d.h[gI * Wh * NB];
            pw(g.ih, g.bi, &xi[gI * Wh * NB], &gi[0], 3 * H, Wh, NB); pw(g.hh, g.bh, hg, &gh[0], 3 * H, H, NB); gruGates(&gi[0], &gh[0], hg, H, NB); }
        pw(d.efw, d.efb, &d.h[0], out, W, W, NB); layernorm(out, (int)WN, &d.elw[0], &d.elb[0], d.ele);
        for (size_t i = 0; i < WN; ++i) out[i] += xi[i];
    }
    void step(const float* se, const float* sy, float* out) {
#ifdef GTCRN_PROF
        auto _t = std::chrono::steady_clock::now();
#endif
        for (int f = 0; f < F; ++f) { const float er = se[2 * f], ei = se[2 * f + 1], yr = sy[2 * f], yi = sy[2 * f + 1]; const float pe = er * er + ei * ei + 1e-12f, py = yr * yr + yi * yi + 1e-12f;
            x7[f] = std::sqrt(pe); x7[F + f] = er; x7[2 * F + f] = ei; x7[3 * F + f] = std::sqrt(py); x7[4 * F + f] = yr; x7[5 * F + f] = yi; if (rho) x7[6 * F + f] = pe / (pe + py + 1e-9f); }
        const int Fh = F - F0;
        for (int c = 0; c < NI; ++c) { const float* xc = &x7[c * F]; float* yc = &xe[c * FE]; memcpy(yc, xc, F0 * 4); for (int r = 0; r < 64; ++r) { float a = 0; const float* wr = erb + r * Fh; for (int k = es[r]; k < ee[r]; ++k) a += wr[k] * xc[F0 + k]; yc[F0 + r] = a; } }
        sfe(&xe[0], &x21[0], NI, FE); PT(0);
        convF(en0c, &x21[0], FE, &en0[0]); PT(1); convF(en1c, &en0[0], 65, &en1[0]); PT(2);
        gt(gt_[0], &en1[0], &enA[0]); gt(gt_[1], &enA[0], &enB[0]); gt(gt_[2], &enB[0], &enC[0]); PT(3);
        dp(dp_[0], &enC[0], &xa[0]); PT(4); dp(dp_[1], &xa[0], &xb[0]); PT(5);
        const size_t WN = (size_t)W * NB;
        for (size_t i = 0; i < WN; ++i) xb[i] += enC[i]; gt(gt_[3], &xb[0], &xa[0]);
        for (size_t i = 0; i < WN; ++i) xa[i] += enB[i]; gt(gt_[4], &xa[0], &xb[0]);
        for (size_t i = 0; i < WN; ++i) xb[i] += enA[i]; gt(gt_[5], &xb[0], &xa[0]); PT(6);
        for (size_t i = 0; i < WN; ++i) xa[i] += en1[i];
        deconvF(de3c, &xa[0], NB, &d3[0]); for (int i = 0; i < W * 65; ++i) d3[i] += en0[i];
        deconvF(de4c, &d3[0], 65, &m2[0]); PT(7);                                          // (2, 129)
        for (int c = 0; c < 2; ++c) { const float* mc = &m2[c * FE]; float* o = &mF[c * F]; memcpy(o, mc, F0 * 4); for (int r = 0; r < Fh; ++r) { float s = 0; const float* wr = ierb + r * 64; for (int k = is[r]; k < ie[r]; ++k) s += wr[k] * mc[F0 + k]; o[F0 + r] = s; } }
        const float lo = std::pow(10.f, -floorDb / 20.f);
        for (int f = 0; f < F; ++f) { float m0 = mF[f], m1 = mF[F + f]; if (res) m0 = 1.f + (m0 < 0 ? m0 : 0.1f * m0);
            if (floorDb > 0) { const float mag = std::sqrt(m0 * m0 + m1 * m1 + 1e-12f); const float sc = std::max(mag, lo) / mag; m0 *= sc; m1 *= sc; }
            const float er = se[2 * f], ei = se[2 * f + 1]; out[2 * f] = er * m0 - ei * m1; out[2 * f + 1] = ei * m0 + er * m1; }
        PT(8);
    }
};
}
