// CoFi-Lite 残余回声抑制网络的流式推理（单帧、单线程、每帧零分配）。输入为线性级输出谱与线性回声估计谱，输出逐频点增益后的谱。
// 写法沿用 gtcrn.h：load() 里一次定位权重与分配缓冲；激活一律 [通道][频率] 行主序；GRU 输入投影按频率批量算；tanh/sigmoid 用 Eigen 13/6 有理逼近。
// 用法：fb::Cofi m; m.load("mask.gtc", nfft); m.step(se, sy, out);  se/sy/out = 2F 个 float（实、虚交错）。
// 提示版（参数文件带 hint_v1，如 v62be20）：m.step(se, sy, sx, out)，sx = 对齐参考谱（与 se 同一电平缩放）；模型内部因果算「残余回声提示」，
//   粗 / 细两路各多一个输入通道。逐帧同式于 Python cofi_np.Hint（= cofi_aec48.residual_echo_hint）：带功率 float 求和、递推 double。
#pragma once
#include "gtcrn.h"   // 复用 Ten、ftanh/sigm、pw、sfe、gruGates、gru1、layernorm
#include "hint_v1.h"
#ifdef COFI_Q8
#include "q8.h"   // CPF 两个全连接用 int8（Arm KleidiAI 量化方式与内核）
#endif
namespace fb {
static inline bool gtcHasKey(const char* path, const char* key) {   // 不加载全部数据，只扫名字：用于按参数文件选内核
    ModelFile mf_(fb_open_model(path)); FILE* f = mf_.f; if (!f) return false; char mg[4]; int cnt; bool ok = false;
    if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "GTC1", 4) && fread(&cnt, 4, 1, f) == 1)
        for (int i = 0; i < cnt && !ok; ++i) { int nl, nd; if (fread(&nl, 4, 1, f) != 1) break; std::string k(nl, ' '); if (fread(&k[0], 1, nl, f) != (size_t)nl || fread(&nd, 4, 1, f) != 1) break;
            std::vector<int> d(nd); if (nd && fread(d.data(), 4, nd, f) != (size_t)nd) break; long n = 1; for (int x : d) n *= x; ok = (k == key); fseek(f, n * 4, SEEK_CUR); }
    mf_.close(); return ok;
}

struct Cofi : HintV1 {
    static const int FE = 129, F0 = 65, FL = 65;
    std::map<std::string, Ten> P; int F = 0; float floorDb = 20.f;
    double prof[10] = {0};
    static const char* profName(int k) { static const char* n[] = {"特征+ERB+SFE", "粗路编码 ce0-2", "细路编码 fe", "Inter-RNN×4", "CPF", "粗路解码 cd0-2", "细路解码 fd", "iERB+掩码"}; return n[k]; }
    typedef Gtcrn::GRU GRU;
    struct MB { const float *p1w, *p1b, *p1aw, *p1ab, *p1s, *dw, *db, *daw, *dab, *ds, *p2w, *p2b, *tfw, *tfb; GRU tg;
                int Cin, C, Fi, Fo, kt, kf, pf; bool de; int head; std::vector<float> cache, h; };   // cache: [C][kt−1 槽][Fi] 环形
    struct IR { GRU g; const float *fw, *fb; std::vector<float> lw, lb; float le; int C, Fn; std::vector<float> h; };   // h: [C][Fn]
    MB mb_[8]; IR ir_[4]; GRU cg1, cg2; const float *f1w, *f1b, *f2w, *f2b, *clw, *clb; float cle; int H = 0, D = 0; std::vector<float> ch1, ch2;
    const float *erb = nullptr, *ierb = nullptr; std::vector<int> es, ee, is, ie;
    std::vector<float> em, ym, rh, lm, xc0, xc9, xf7, xf21, h1, hd, yt, sk0, sk1, sk2, xf0, xa, xb, xc, xf, gi, gh, yi, z, z2, mF; std::vector<float> padr;   // 深度卷积补零缓冲
    // Talk gru2 head: cr2/fr2 输出特征缓存与循环状态，仅带 talk_head2_v1 的参数包启用。
    bool hasTalkHead = false; float lastTalk = -1.f; std::vector<float> talkZ, talkH;
    const float *talkW1 = nullptr, *talkB1 = nullptr, *talkWih = nullptr, *talkWhh = nullptr, *talkBi = nullptr, *talkBh = nullptr, *talkW2 = nullptr, *talkB2 = nullptr;

    const Ten& T(const std::string& k) const { auto it = P.find(k); if (it == P.end()) { fprintf(stderr, "cofi: 缺参数 %s\n", k.c_str()); fb_fail(); } return it->second; }
    const float* p(const std::string& k) const { return T(k).v.data(); }
    static void die(const char* m) { fprintf(stderr, "cofi: %s\n", m); fb_fail(); }
    void gru(const std::string& s, GRU& g) { const Ten& ih = T(s + "ih"); g.ih = ih.v.data(); g.hh = p(s + "hh"); g.bi = p(s + "bi"); g.bh = p(s + "bh"); g.H = ih.d[0] / 3; g.I = ih.d[1]; if (g.H > 64) die("GRU 隐藏超出上限 64"); }
    void load(const char* path, int nfft, float floor_ = 20.f) {
        ModelFile mf_(fb_open_model(path)); FILE* f = mf_.f; if (!f) { perror(path); fb_fail(); } char mg[4]; int cnt; Gtcrn::rd(mg, 1, 4, f); Gtcrn::rd(&cnt, 4, 1, f); if (memcmp(mg, "GTC1", 4)) die("文件格式错");
        for (int i = 0; i < cnt; ++i) { int nl, nd; Gtcrn::rd(&nl, 4, 1, f); std::string k(nl, ' '); Gtcrn::rd(&k[0], 1, nl, f); Gtcrn::rd(&nd, 4, 1, f); Ten t; t.d.resize(nd); if (nd) Gtcrn::rd(t.d.data(), 4, nd, f); size_t n = 1; for (int x : t.d) n *= x; t.v.resize(n); if (n) Gtcrn::rd(t.v.data(), 4, n, f); P[k] = std::move(t); }
        mf_.close(); T("arch_cofi"); F = nfft / 2 + 1; floorDb = floor_;
        hint = P.count("hint_v1") > 0; hasTalkHead = P.count("talk_head2_v1") > 0;
        if (hasTalkHead) {
            const Ten& w1 = T("talk_head2.0.weight"); const Ten& wih = T("talk_head2.2.weight_ih_l0");
            if (w1.d.size() != 2 || w1.d[0] != 32 || w1.d[1] != 829 || wih.d.size() != 2 || wih.d[0] != 96 || wih.d[1] != 32 || T("talk_head2.2.weight_hh_l0").v.size() != 96 * 32 || T("talk_head2.3.weight").v.size() != 32) die("talk_head2 形状不符");
            talkW1 = p("talk_head2.0.weight"); talkB1 = p("talk_head2.0.bias"); talkWih = p("talk_head2.2.weight_ih_l0"); talkWhh = p("talk_head2.2.weight_hh_l0"); talkBi = p("talk_head2.2.bias_ih_l0"); talkBh = p("talk_head2.2.bias_hh_l0"); talkW2 = p("talk_head2.3.weight"); talkB2 = p("talk_head2.3.bias"); talkZ.assign(829, 0.f); talkH.assign(32, 0.f);
        }
        if (hint) { const Ten& hbT = T("hint_band"); const Ten& hp = T("hint_par"); if ((int)hbT.v.size() != F || hp.v.size() < 5) die("hint 参数形状不符");
            const int nb = (int)hp.v[0], K = (int)hp.v[1]; if (nb <= 0 || K < 1) die("hint 参数无效");
            std::vector<int> bands(F); for (int f = 0; f < F; ++f) { bands[f] = (int)hbT.v[f]; if (bands[f] < 0 || bands[f] >= nb) die("hint 带号越界"); }
            init(F, nfft, 48000.0, nb, K, hp.v[2], hp.v[3], hp.v[4], bands.data()); }
        const int NC = hint ? 4 : 3, NFc = hint ? 8 : 7;   // 粗路输入通道（e、ŷ、ρ[、提示]）/ 细路（e×3、ŷ×3、ρ[、提示]）
        if (T("ce0.p1w").d[1] != 3 * NC || T("fe.p1w").d[1] != 3 * NFc) die(hint ? "提示版输入通道数不符" : "输入通道数不符（参数文件是否带提示却缺 hint_v1？）");
        if (T("erb_fc").d[1] != F - F0) { fprintf(stderr, "cofi: nfft %d 与参数不符（erb_fc %d 列）\n", nfft, T("erb_fc").d[1]); fb_fail(); }
        erb = p("erb_fc"); ierb = p("ierb_fc");
        auto rng = [](const Ten& t, std::vector<int>& s, std::vector<int>& e) { int R = t.d[0], C = t.d[1]; s.assign(R, 0); e.assign(R, 0); for (int r = 0; r < R; ++r) { int a = C, b = 0; for (int c = 0; c < C; ++c) if (t.v[r * C + c] != 0.f) { a = std::min(a, c); b = c + 1; } s[r] = a < b ? a : 0; e[r] = a < b ? b : 0; } };
        rng(T("erb_fc"), es, ee); rng(T("ierb_fc"), is, ie);
        const char* mn[8] = {"ce0", "ce1", "ce2", "fe", "cd0", "cd1", "cd2", "fd"}; size_t mx = 0;
        for (int i = 0; i < 8; ++i) { std::string s(mn[i]); MB& m = mb_[i]; const Ten& p1 = T(s + ".p1w"), &dw = T(s + ".dw");
            m.p1w = p1.v.data(); m.p1b = p(s + ".p1b"); m.p1aw = p(s + ".p1aw"); m.p1ab = p(s + ".p1ab"); m.p1s = p(s + ".p1s"); m.C = p1.d[0]; m.Cin = p1.d[1]; m.Fi = T(s + ".p1aw").d[1];
            m.dw = dw.v.data(); m.db = p(s + ".db"); m.kt = dw.d[1]; m.kf = dw.d[2]; m.pf = m.kf / 2; m.daw = p(s + ".daw"); m.dab = p(s + ".dab"); m.ds = p(s + ".ds"); m.Fo = T(s + ".daw").d[1]; m.de = T(s + ".de").v[0] > 0;
            m.p2w = p(s + ".p2w"); m.p2b = p(s + ".p2b"); gru(s + ".tg", m.tg); m.tfw = p(s + ".tfw"); m.tfb = p(s + ".tfb");
            if (dw.d[0] != m.C || m.kt > 3 || (m.de ? 2 * m.Fi - 1 - 2 * m.pf + m.kf - 1 : (m.Fi + 2 * m.pf - m.kf) / 2 + 1) != m.Fo) { fprintf(stderr, "cofi: %s 形状不符\n", mn[i]); fb_fail(); }
            m.head = 0; m.cache.assign((size_t)m.C * std::max(m.kt - 1, 1) * m.Fi, 0.f); m.h.assign(m.tg.H, 0.f); mx = std::max(mx, (size_t)std::max(m.C, m.Cin) * std::max(m.Fi, m.Fo)); }
        const char* rn[4] = {"cr1", "cr2", "fr1", "fr2"};
        for (int i = 0; i < 4; ++i) { std::string s(rn[i]); IR& r = ir_[i]; gru(s + ".g", r.g); r.fw = p(s + ".fw"); r.fb = p(s + ".fb"); r.le = T(s + ".le").v[0]; const Ten& lw = T(s + ".lw"); r.Fn = lw.d[0]; r.C = lw.d[1];
            if (r.g.H != r.C || r.g.I != r.C) die("Inter-RNN 形状不符");
            r.lw.resize(r.C * r.Fn); r.lb.resize(r.C * r.Fn); const float* lb = p(s + ".lb"); for (int fq = 0; fq < r.Fn; ++fq) for (int c = 0; c < r.C; ++c) { r.lw[c * r.Fn + fq] = lw.v[fq * r.C + c]; r.lb[c * r.Fn + fq] = lb[fq * r.C + c]; }   // (F,C) → [C][F]
            r.h.assign((size_t)r.C * r.Fn, 0.f); }
        f1w = p("cpf.f1w"); f1b = p("cpf.f1b"); f2w = p("cpf.f2w");
#ifdef COFI_W16
        { const Ten& t1 = T("cpf.f1w"); f1h.assign(t1.v.begin(), t1.v.end()); const Ten& t2 = T("cpf.f2w"); f2h.assign(t2.v.begin(), t2.v.end()); }
#endif
        f2b = p("cpf.f2b");
#ifdef COFI_Q8
        q1.init(T("cpf.f1w").v.data(), T("cpf.f1b").v.data(), T("cpf.f1w").d[0], T("cpf.f1w").d[1]); q2.init(T("cpf.f2w").v.data(), T("cpf.f2b").v.data(), T("cpf.f2w").d[0], T("cpf.f2w").d[1]);
        qcat.assign(T("cpf.f1w").d[1], 0.f);
#endif
        clw = p("cpf.lw"); clb = p("cpf.lb"); cle = T("cpf.le").v[0]; H = T("cpf.f1w").d[0]; D = T("cpf.f1w").d[1];
        gru("cpf.g1", cg1); gru("cpf.g2", cg2); if (cg1.H + cg2.H != H || D != ir_[0].C * ir_[0].Fn + ir_[2].C * ir_[2].Fn) die("CPF 形状不符"); ch1.assign(cg1.H, 0.f); ch2.assign(cg2.H, 0.f);
        em.assign(F, 0); ym.assign(F, 0); rh.assign(F, 0); lm.assign(2 * FL, 0); xc0.assign(NC * FE, 0); xc9.assign(3 * NC * FE, 0); xf7.assign(NFc * FL, 0); xf21.assign(3 * NFc * FL, 0);
        h1.assign(mx, 0); hd.assign(mx, 0); yt.assign(mx, 0); sk0.assign(mx, 0); sk1.assign(mx, 0); sk2.assign(mx, 0); xf0.assign(mx, 0); xa.assign(mx, 0); xb.assign(mx, 0); xc.assign(mx, 0); xf.assign(mx, 0);
        gi.assign(3 * mx, 0); gh.assign(3 * mx, 0); yi.assign(mx, 0); z.assign(std::max(H, D), 0); z2.assign(D, 0); padr.assign(2 * mx + 16, 0); mF.assign(F, 0);
    }
    void resetTalkHead() { if (hasTalkHead) { std::fill(talkH.begin(), talkH.end(), 0.f); lastTalk = -1.f; } }
    void reset() { for (auto& m : mb_) { std::fill(m.cache.begin(), m.cache.end(), 0.f); std::fill(m.h.begin(), m.h.end(), 0.f); m.head = 0; } for (auto& r : ir_) std::fill(r.h.begin(), r.h.end(), 0.f); std::fill(ch1.begin(), ch1.end(), 0.f); std::fill(ch2.begin(), ch2.end(), 0.f);
        if (hint) resetHint(); resetTalkHead(); }
    float talkStep(const float* feat) {
        if (!hasTalkHead) return lastTalk = -1.f;
        if (feat != talkZ.data()) memcpy(talkZ.data(), feat, 829 * sizeof(float)); float pre[32], gi[96], gh[96];
        for (int o = 0; o < 32; ++o) { float a = talkB1[o]; const float* w = talkW1 + (size_t)o * 829; for (int i = 0; i < 829; ++i) a += w[i] * talkZ[i]; pre[o] = a > 0.f ? a : std::expm1(a); }
        for (int g = 0; g < 96; ++g) { float a = talkBi[g], b = talkBh[g]; const float* wi = talkWih + (size_t)g * 32; const float* wh = talkWhh + (size_t)g * 32; for (int i = 0; i < 32; ++i) { a += wi[i] * pre[i]; b += wh[i] * talkH[i]; } gi[g] = a; gh[g] = b; }
        Gtcrn::gruGates(gi, gh, talkH.data(), 32, 1); float logit = talkB2[0]; for (int i = 0; i < 32; ++i) logit += talkW2[i] * talkH[i]; return lastTalk = logit;
    }

    // AffinePReLU：y = aw·x + ab + (x>0 ? x : s_c·x)，x [C][N] 原地
    static void aprelu(float* x, int C, int N, const float* aw, const float* ab, const float* s) {
        for (int c = 0; c < C; ++c) { float* xc = x + c * N; const float* w = aw + c * N; const float* b = ab + c * N; const float sc = s[c];
            for (int n = 0; n < N; ++n) { const float v = xc[n]; xc[n] = w[n] * v + b[n] + (v > 0 ? v : sc * v); } }
    }
    // MB：PW(1×1)+BN+AffinePReLU → 深度 (kt,kf) 时间因果、频率步长 2（卷积或反卷积）+BN+AffinePReLU → PW+BN → TRA
    void mb(MB& m, const float* x, float* y) {
        const int C = m.C, Fi = m.Fi, Fo = m.Fo, kt = m.kt, kf = m.kf, pf = m.pf, L = kt - 1;
        Gtcrn::pw(m.p1w, m.p1b, x, &h1[0], C, m.Cin, Fi); aprelu(&h1[0], C, Fi, m.p1aw, m.p1ab, m.p1s);
        for (int c = 0; c < C; ++c) {
            const float* rows[3]; for (int t = 0; t < L; ++t) rows[t] = &m.cache[((size_t)c * L + (m.head + t) % L) * Fi]; rows[L] = &h1[c * Fi];   // 时间抽头 t = 0..kt−1（最老 → 当前）
            float* yo = &hd[c * Fo]; const float bv = m.db[c]; for (int f = 0; f < Fo; ++f) yo[f] = bv;
            for (int t = 0; t < kt; ++t) { const float* r = rows[t]; const float* w = m.dw + (c * kt + t) * kf;
                // 输入先补零（左 PL、右补足），内层不再逐点判边界（结果与原式相同：越界项原来跳过、现在乘 0）
                const int PL = kf; float* q = &padr[0]; memset(q, 0, (Fi + 2 * kf + 2) * 4); memcpy(q + PL, r, Fi * 4);
                if (!m.de) { for (int f = 0; f < Fo; ++f) { float a = 0; const float* qq = q + PL + 2 * f - pf; for (int k = 0; k < kf; ++k) a += w[k] * qq[k]; yo[f] += a; } }
                else { for (int f = 0; f < Fo; ++f) { float a = 0; const int u = f + pf; for (int k = (u & 1); k < kf; k += 2) a += w[k] * q[PL + ((u - k) >> 1)]; yo[f] += a; } } }   // 聚集式：y[u−pf] = Σ_{2i+k=u} x[i]·w[k]
            if (L > 0) { memcpy(&m.cache[((size_t)c * L + m.head) * Fi], &h1[c * Fi], Fi * 4); } }   // 最老的槽被当前帧覆盖
        if (L > 0) m.head = (m.head + 1) % L;
        aprelu(&hd[0], C, Fo, m.daw, m.dab, m.ds); Gtcrn::pw(m.p2w, m.p2b, &hd[0], y, C, C, Fo);
        float zt[64], at[64];
        for (int c = 0; c < C; ++c) { float a = 0; const float* yc = y + c * Fo; for (int f = 0; f < Fo; ++f) a += yc[f] * yc[f]; zt[c] = a / Fo; }
        Gtcrn::gru1(m.tg, zt, nullptr, &m.h[0]); const int Hh = m.tg.H;
        for (int c = 0; c < C; ++c) { float a = m.tfb[c]; const float* w = m.tfw + c * Hh; for (int i = 0; i < Hh; ++i) a += w[i] * m.h[i]; at[c] = Gtcrn::sigm(a); }
        for (int c = 0; c < C; ++c) { float* yc = y + c * Fo; const float s = at[c]; for (int f = 0; f < Fo; ++f) yc[f] *= s; }
    }
    // Inter-RNN：每个频点一条沿时间的 GRU(C→C)（批量算）→ FC → LN(F,C) → 残差。x 原地
    void inter(IR& r, float* x) {
        const int C = r.C, N = r.Fn, n = C * N;
        Gtcrn::pw(r.g.ih, r.g.bi, x, &gi[0], 3 * C, C, N); Gtcrn::pw(r.g.hh, r.g.bh, &r.h[0], &gh[0], 3 * C, C, N); Gtcrn::gruGates(&gi[0], &gh[0], &r.h[0], C, N);
        Gtcrn::pw(r.fw, r.fb, &r.h[0], &yi[0], C, C, N);   // fc 作用在通道维：y[c][f] = Σ_i fw[c][i] h[i][f] + fb[c]
        Gtcrn::layernorm(&yi[0], n, &r.lw[0], &r.lb[0], r.le); for (int i = 0; i < n; ++i) x[i] += yi[i];
    }
    // CPF：两路摊平拼接（[C][F] 顺序 = torch 的 c·F+f）→ FC → LN → ELU → 两组 GRU → FC → 拆回 + 残差
#ifdef COFI_W16
    std::vector<__fp16> f1h, f2h;   // 实验：CPF 两个全连接层权重半精度存储（读内存减半）
#endif
#ifdef COFI_Q8
    q8::QMat q1, q2; std::vector<float> qcat;
    void cpf(float* a, int na, float* b, int nb) {
        memcpy(&qcat[0], a, na * 4); memcpy(&qcat[na], b, nb * 4);
        q8::gemv(q1, &qcat[0], &z[0]);
        Gtcrn::layernorm(&z[0], H, clw, clb, cle); for (int h = 0; h < H; ++h) z[h] = z[h] > 0 ? z[h] : std::expm1(z[h]);
        Gtcrn::gru1(cg1, &z[0], nullptr, &ch1[0]); Gtcrn::gru1(cg2, &z[cg1.H], nullptr, &ch2[0]);
        for (int h = 0; h < cg1.H; ++h) z[h] = ch1[h]; for (int h = 0; h < cg2.H; ++h) z[cg1.H + h] = ch2[h];
        q8::gemv(q2, &z[0], &z2[0]);
        for (int i = 0; i < na; ++i) a[i] += z2[i]; for (int i = 0; i < nb; ++i) b[i] += z2[na + i];
    }
#else
    void cpf(float* a, int na, float* b, int nb) {
#ifdef COFI_W16
        for (int h = 0; h < H; ++h) { float s = f1b[h]; const __fp16* w = &f1h[(size_t)h * D];
#else
        for (int h = 0; h < H; ++h) { float s = f1b[h]; const float* w = f1w + (size_t)h * D;
#endif
            for (int i = 0; i < na; ++i) s += (float)w[i] * a[i]; const auto* w2 = w + na; for (int i = 0; i < nb; ++i) s += (float)w2[i] * b[i]; z[h] = s; }
        Gtcrn::layernorm(&z[0], H, clw, clb, cle); for (int h = 0; h < H; ++h) z[h] = z[h] > 0 ? z[h] : std::expm1(z[h]);
        Gtcrn::gru1(cg1, &z[0], nullptr, &ch1[0]); Gtcrn::gru1(cg2, &z[cg1.H], nullptr, &ch2[0]);
        for (int h = 0; h < cg1.H; ++h) z[h] = ch1[h]; for (int h = 0; h < cg2.H; ++h) z[cg1.H + h] = ch2[h];
#ifdef COFI_W16
        for (int d = 0; d < D; ++d) { float s = f2b[d]; const __fp16* w = &f2h[(size_t)d * H]; for (int h = 0; h < H; ++h) s += (float)w[h] * z[h]; z2[d] = s; }
#else
        for (int d = 0; d < D; ++d) { float s = f2b[d]; const float* w = f2w + (size_t)d * H; for (int h = 0; h < H; ++h) s += w[h] * z[h]; z2[d] = s; }
#endif
        for (int i = 0; i < na; ++i) a[i] += z2[i]; for (int i = 0; i < nb; ++i) b[i] += z2[na + i];
    }
#endif
    static inline float res(float m) { const float t = Gtcrn::ftanh(m); return 1.f + (t < 0 ? t : 0.1f * t); }
    void step(const float* se, const float* sy, float* out) { step(se, sy, nullptr, out); }
    void step(const float* se, const float* sy, const float* sx, float* out) {
#ifdef GTCRN_PROF
        auto _t = std::chrono::steady_clock::now();
#endif
        const float LG = 0.4342944819f;   // 1/ln10
        for (int f = 0; f < F; ++f) { const float er = se[2 * f], ei = se[2 * f + 1], yr = sy[2 * f], yi_ = sy[2 * f + 1]; const float pe = er * er + ei * ei + 1e-12f, py = yr * yr + yi_ * yi_ + 1e-12f;
            em[f] = std::sqrt(pe); ym[f] = std::sqrt(py); rh[f] = pe / (pe + py + 1e-9f); }
        if (hint) { if (!sx) die("提示版需要对齐参考谱 sx"); for (int f = 0; f < F; ++f) xm[f] = std::sqrt(sx[2 * f] * sx[2 * f] + sx[2 * f + 1] * sx[2 * f + 1] + 1e-12f); HintV1::hintStep(F, em.data(), xm.data()); }
        const int NC = hint ? 4 : 3, NFc = hint ? 8 : 7;
        const int Fh = F - F0; const float* src[4] = {&em[0], &ym[0], &rh[0], hm.data()};   // 默认版 hm 为空（NC = 3 不读第四路），不能写 &hm[0]
        for (int c = 0; c < NC; ++c) { const float* xs = src[c]; float* yc = &xc0[c * FE]; memcpy(yc, xs, F0 * 4); for (int r = 0; r < 64; ++r) { float a = 0; const float* wr = erb + r * Fh; for (int k = es[r]; k < ee[r]; ++k) a += wr[k] * xs[F0 + k]; yc[F0 + r] = a; } }
        for (int i = 0; i < 2 * FE; ++i) xc0[i] = std::log(std::max(xc0[i], 1e-8f)) * LG;
        if (hint) for (int i = 3 * FE; i < 4 * FE; ++i) xc0[i] = std::log(std::max(xc0[i], 1e-8f)) * LG;   // 提示通道取对数；ρ 不取
        Gtcrn::sfe(&xc0[0], &xc9[0], NC, FE);
        for (int s = 0; s < 2; ++s) { const float* m = s ? &ym[0] : &em[0]; const float* sp = s ? sy : se; float* o = &xf7[s * 3 * FL];
            for (int f = 0; f < FL; ++f) { const float lnm = std::log(std::max(m[f], 1e-8f)); o[f] = lnm * LG; const float iv = std::exp(-0.7f * std::log(m[f])); o[FL + f] = sp[2 * f] * iv; o[2 * FL + f] = sp[2 * f + 1] * iv; } }
        memcpy(&xf7[6 * FL], &rh[0], FL * 4);
        if (hint) for (int f = 0; f < FL; ++f) xf7[7 * FL + f] = std::log(std::max(hm[f], 1e-8f)) * LG;
        Gtcrn::sfe(&xf7[0], &xf21[0], NFc, FL); PT(0);
        mb(mb_[0], &xc9[0], &sk0[0]); mb(mb_[1], &sk0[0], &sk1[0]); mb(mb_[2], &sk1[0], &sk2[0]); PT(1);
        mb(mb_[3], &xf21[0], &xf0[0]); PT(2);
        const int nc = ir_[0].C * ir_[0].Fn, nf = ir_[2].C * ir_[2].Fn;
        memcpy(&xc[0], &sk2[0], nc * 4); memcpy(&xf[0], &xf0[0], nf * 4); inter(ir_[0], &xc[0]); inter(ir_[2], &xf[0]); PT(3);
        cpf(&xc[0], nc, &xf[0], nf); PT(4);
        inter(ir_[1], &xc[0]); inter(ir_[3], &xf[0]); PT(3);
        if (hasTalkHead) { memcpy(talkZ.data(), xc.data(), nc * 4); memcpy(talkZ.data() + nc, xf.data(), nf * 4); }
        for (int i = 0; i < nc; ++i) xc[i] += sk2[i]; mb(mb_[4], &xc[0], &xa[0]);
        { const int n = mb_[4].C * mb_[4].Fo; for (int i = 0; i < n; ++i) xa[i] += sk1[i]; } mb(mb_[5], &xa[0], &xb[0]);
        { const int n = mb_[5].C * mb_[5].Fo; for (int i = 0; i < n; ++i) xb[i] += sk0[i]; } mb(mb_[6], &xb[0], &xa[0]); PT(5);   // xa = 粗路输出 (1, 129)
        for (int i = 0; i < nf; ++i) xf[i] += xf0[i]; mb(mb_[7], &xf[0], &xb[0]); PT(6);                                         // xb = 细路输出 (1, 65)
        for (int i = 0; i < FE; ++i) xa[i] = res(xa[i]);
        memcpy(&mF[0], &xa[0], F0 * 4); for (int r = 0; r < Fh; ++r) { float s = 0; const float* wr = ierb + r * 64; for (int k = is[r]; k < ie[r]; ++k) s += wr[k] * xa[F0 + k]; mF[F0 + r] = s; }
        for (int f = 0; f < FL; ++f) mF[f] *= res(xb[f]);
        const float lo = floorDb > 0 ? std::pow(10.f, -floorDb / 20.f) : -1e30f;
        if (hasTalkHead) {
            float* ol = talkZ.data() + 700; const int Fh = F - F0;
            for (int f = 0; f < F0; ++f) ol[f] = std::log10(std::max(em[f] * std::max(mF[f], lo), 1e-8f));
            for (int r = 0; r < 64; ++r) { float a = 0.f; const float* wr = erb + r * Fh; for (int k = es[r]; k < ee[r]; ++k) a += wr[k] * em[F0 + k] * std::max(mF[F0 + k], lo); ol[F0 + r] = std::log10(std::max(a, 1e-8f)); }
            talkStep(talkZ.data());
        }
        for (int f = 0; f < F; ++f) { const float g = std::max(mF[f], lo); out[2 * f] = se[2 * f] * g; out[2 * f + 1] = se[2 * f + 1] * g; }
        PT(7);
    }
};
}
