// CoFi-Lite 残余回声抑制网络的流式推理（单帧、单线程、每帧零分配）。输入为线性级输出谱与线性回声估计谱，输出逐频点增益后的谱。
// 写法沿用 gtcrn.h：load() 里一次定位权重与分配缓冲；激活一律 [通道][频率] 行主序；GRU 输入投影按频率批量算；tanh/sigmoid 用 Eigen 13/6 有理逼近。
// 用法：fb::Cofi m; m.load("mask.gtc", nfft); m.step(se, sy, out);  se/sy/out = 2F 个 float（实、虚交错）。
#pragma once
#include "gtcrn.h"   // 复用 Ten、ftanh/sigm、pw、sfe、gruGates、gru1、layernorm
namespace fb {
static inline bool gtcHasKey(const char* path, const char* key) {   // 不加载全部数据，只扫名字：用于按参数文件选内核
    FILE* f = fb_open_model(path); if (!f) return false; char mg[4]; int cnt; bool ok = false;
    if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "GTC1", 4) && fread(&cnt, 4, 1, f) == 1)
        for (int i = 0; i < cnt && !ok; ++i) { int nl, nd; if (fread(&nl, 4, 1, f) != 1) break; std::string k(nl, ' '); if (fread(&k[0], 1, nl, f) != (size_t)nl || fread(&nd, 4, 1, f) != 1) break;
            std::vector<int> d(nd); if (nd && fread(d.data(), 4, nd, f) != (size_t)nd) break; long n = 1; for (int x : d) n *= x; ok = (k == key); fseek(f, n * 4, SEEK_CUR); }
    fclose(f); return ok;
}

struct Cofi {
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
    std::vector<float> em, ym, rh, lm, xc0, xc9, xf7, xf21, h1, hd, yt, sk0, sk1, sk2, xf0, xa, xb, xc, xf, gi, gh, yi, z, z2, mF;

    const Ten& T(const std::string& k) const { auto it = P.find(k); if (it == P.end()) { fprintf(stderr, "cofi: 缺参数 %s\n", k.c_str()); exit(1); } return it->second; }
    const float* p(const std::string& k) const { return T(k).v.data(); }
    static void die(const char* m) { fprintf(stderr, "cofi: %s\n", m); exit(1); }
    void gru(const std::string& s, GRU& g) { const Ten& ih = T(s + "ih"); g.ih = ih.v.data(); g.hh = p(s + "hh"); g.bi = p(s + "bi"); g.bh = p(s + "bh"); g.H = ih.d[0] / 3; g.I = ih.d[1]; if (g.H > 64) die("GRU 隐藏超出上限 64"); }
    void load(const char* path, int nfft, float floor_ = 20.f) {
        FILE* f = fb_open_model(path); if (!f) { perror(path); exit(1); } char mg[4]; int cnt; Gtcrn::rd(mg, 1, 4, f); Gtcrn::rd(&cnt, 4, 1, f); if (memcmp(mg, "GTC1", 4)) die("文件格式错");
        for (int i = 0; i < cnt; ++i) { int nl, nd; Gtcrn::rd(&nl, 4, 1, f); std::string k(nl, ' '); Gtcrn::rd(&k[0], 1, nl, f); Gtcrn::rd(&nd, 4, 1, f); Ten t; t.d.resize(nd); if (nd) Gtcrn::rd(t.d.data(), 4, nd, f); size_t n = 1; for (int x : t.d) n *= x; t.v.resize(n); if (n) Gtcrn::rd(t.v.data(), 4, n, f); P[k] = std::move(t); }
        fclose(f); T("arch_cofi"); F = nfft / 2 + 1; floorDb = floor_;
        if (T("erb_fc").d[1] != F - F0) { fprintf(stderr, "cofi: nfft %d 与参数不符（erb_fc %d 列）\n", nfft, T("erb_fc").d[1]); exit(1); }
        erb = p("erb_fc"); ierb = p("ierb_fc");
        auto rng = [](const Ten& t, std::vector<int>& s, std::vector<int>& e) { int R = t.d[0], C = t.d[1]; s.assign(R, 0); e.assign(R, 0); for (int r = 0; r < R; ++r) { int a = C, b = 0; for (int c = 0; c < C; ++c) if (t.v[r * C + c] != 0.f) { a = std::min(a, c); b = c + 1; } s[r] = a < b ? a : 0; e[r] = a < b ? b : 0; } };
        rng(T("erb_fc"), es, ee); rng(T("ierb_fc"), is, ie);
        const char* mn[8] = {"ce0", "ce1", "ce2", "fe", "cd0", "cd1", "cd2", "fd"}; size_t mx = 0;
        for (int i = 0; i < 8; ++i) { std::string s(mn[i]); MB& m = mb_[i]; const Ten& p1 = T(s + ".p1w"), &dw = T(s + ".dw");
            m.p1w = p1.v.data(); m.p1b = p(s + ".p1b"); m.p1aw = p(s + ".p1aw"); m.p1ab = p(s + ".p1ab"); m.p1s = p(s + ".p1s"); m.C = p1.d[0]; m.Cin = p1.d[1]; m.Fi = T(s + ".p1aw").d[1];
            m.dw = dw.v.data(); m.db = p(s + ".db"); m.kt = dw.d[1]; m.kf = dw.d[2]; m.pf = m.kf / 2; m.daw = p(s + ".daw"); m.dab = p(s + ".dab"); m.ds = p(s + ".ds"); m.Fo = T(s + ".daw").d[1]; m.de = T(s + ".de").v[0] > 0;
            m.p2w = p(s + ".p2w"); m.p2b = p(s + ".p2b"); gru(s + ".tg", m.tg); m.tfw = p(s + ".tfw"); m.tfb = p(s + ".tfb");
            if (dw.d[0] != m.C || m.kt > 3 || (m.de ? 2 * m.Fi - 1 - 2 * m.pf + m.kf - 1 : (m.Fi + 2 * m.pf - m.kf) / 2 + 1) != m.Fo) { fprintf(stderr, "cofi: %s 形状不符\n", mn[i]); exit(1); }
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
        f2b = p("cpf.f2b"); clw = p("cpf.lw"); clb = p("cpf.lb"); cle = T("cpf.le").v[0]; H = T("cpf.f1w").d[0]; D = T("cpf.f1w").d[1];
        gru("cpf.g1", cg1); gru("cpf.g2", cg2); if (cg1.H + cg2.H != H || D != ir_[0].C * ir_[0].Fn + ir_[2].C * ir_[2].Fn) die("CPF 形状不符"); ch1.assign(cg1.H, 0.f); ch2.assign(cg2.H, 0.f);
        em.assign(F, 0); ym.assign(F, 0); rh.assign(F, 0); lm.assign(2 * FL, 0); xc0.assign(3 * FE, 0); xc9.assign(9 * FE, 0); xf7.assign(7 * FL, 0); xf21.assign(21 * FL, 0);
        h1.assign(mx, 0); hd.assign(mx, 0); yt.assign(mx, 0); sk0.assign(mx, 0); sk1.assign(mx, 0); sk2.assign(mx, 0); xf0.assign(mx, 0); xa.assign(mx, 0); xb.assign(mx, 0); xc.assign(mx, 0); xf.assign(mx, 0);
        gi.assign(3 * mx, 0); gh.assign(3 * mx, 0); yi.assign(mx, 0); z.assign(std::max(H, D), 0); z2.assign(D, 0); mF.assign(F, 0);
    }
    void reset() { for (auto& m : mb_) { std::fill(m.cache.begin(), m.cache.end(), 0.f); std::fill(m.h.begin(), m.h.end(), 0.f); m.head = 0; } for (auto& r : ir_) std::fill(r.h.begin(), r.h.end(), 0.f); std::fill(ch1.begin(), ch1.end(), 0.f); std::fill(ch2.begin(), ch2.end(), 0.f); }

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
                if (!m.de) { for (int f = 0; f < Fo; ++f) { float a = 0; const int b0 = 2 * f - pf; for (int k = 0; k < kf; ++k) { const int i = b0 + k; if (i >= 0 && i < Fi) a += w[k] * r[i]; } yo[f] += a; } }
                else { for (int f = 0; f < Fo; ++f) { float a = 0; const int u = f + pf; for (int k = (u & 1); k < kf; k += 2) { const int i = (u - k) >> 1; if (i >= 0 && i < Fi) a += w[k] * r[i]; } yo[f] += a; } } }   // 聚集式：y[u−pf] = Σ_{2i+k=u} x[i]·w[k]
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
    static inline float res(float m) { const float t = Gtcrn::ftanh(m); return 1.f + (t < 0 ? t : 0.1f * t); }
    void step(const float* se, const float* sy, float* out) {
#ifdef GTCRN_PROF
        auto _t = std::chrono::steady_clock::now();
#endif
        const float LG = 0.4342944819f;   // 1/ln10
        for (int f = 0; f < F; ++f) { const float er = se[2 * f], ei = se[2 * f + 1], yr = sy[2 * f], yi_ = sy[2 * f + 1]; const float pe = er * er + ei * ei + 1e-12f, py = yr * yr + yi_ * yi_ + 1e-12f;
            em[f] = std::sqrt(pe); ym[f] = std::sqrt(py); rh[f] = pe / (pe + py + 1e-9f); }
        const int Fh = F - F0; const float* src[3] = {&em[0], &ym[0], &rh[0]};
        for (int c = 0; c < 3; ++c) { const float* xs = src[c]; float* yc = &xc0[c * FE]; memcpy(yc, xs, F0 * 4); for (int r = 0; r < 64; ++r) { float a = 0; const float* wr = erb + r * Fh; for (int k = es[r]; k < ee[r]; ++k) a += wr[k] * xs[F0 + k]; yc[F0 + r] = a; } }
        for (int i = 0; i < 2 * FE; ++i) xc0[i] = std::log(std::max(xc0[i], 1e-8f)) * LG;
        Gtcrn::sfe(&xc0[0], &xc9[0], 3, FE);
        for (int s = 0; s < 2; ++s) { const float* m = s ? &ym[0] : &em[0]; const float* sp = s ? sy : se; float* o = &xf7[s * 3 * FL];
            for (int f = 0; f < FL; ++f) { const float lnm = std::log(std::max(m[f], 1e-8f)); o[f] = lnm * LG; const float iv = std::exp(-0.7f * std::log(m[f])); o[FL + f] = sp[2 * f] * iv; o[2 * FL + f] = sp[2 * f + 1] * iv; } }
        memcpy(&xf7[6 * FL], &rh[0], FL * 4); Gtcrn::sfe(&xf7[0], &xf21[0], 7, FL); PT(0);
        mb(mb_[0], &xc9[0], &sk0[0]); mb(mb_[1], &sk0[0], &sk1[0]); mb(mb_[2], &sk1[0], &sk2[0]); PT(1);
        mb(mb_[3], &xf21[0], &xf0[0]); PT(2);
        const int nc = ir_[0].C * ir_[0].Fn, nf = ir_[2].C * ir_[2].Fn;
        memcpy(&xc[0], &sk2[0], nc * 4); memcpy(&xf[0], &xf0[0], nf * 4); inter(ir_[0], &xc[0]); inter(ir_[2], &xf[0]); PT(3);
        cpf(&xc[0], nc, &xf[0], nf); PT(4);
        inter(ir_[1], &xc[0]); inter(ir_[3], &xf[0]); PT(3);
        for (int i = 0; i < nc; ++i) xc[i] += sk2[i]; mb(mb_[4], &xc[0], &xa[0]);
        { const int n = mb_[4].C * mb_[4].Fo; for (int i = 0; i < n; ++i) xa[i] += sk1[i]; } mb(mb_[5], &xa[0], &xb[0]);
        { const int n = mb_[5].C * mb_[5].Fo; for (int i = 0; i < n; ++i) xb[i] += sk0[i]; } mb(mb_[6], &xb[0], &xa[0]); PT(5);   // xa = 粗路输出 (1, 129)
        for (int i = 0; i < nf; ++i) xf[i] += xf0[i]; mb(mb_[7], &xf[0], &xb[0]); PT(6);                                         // xb = 细路输出 (1, 65)
        for (int i = 0; i < FE; ++i) xa[i] = res(xa[i]);
        memcpy(&mF[0], &xa[0], F0 * 4); for (int r = 0; r < Fh; ++r) { float s = 0; const float* wr = ierb + r * 64; for (int k = is[r]; k < ie[r]; ++k) s += wr[k] * xa[F0 + k]; mF[F0 + r] = s; }
        for (int f = 0; f < FL; ++f) mF[f] *= res(xb[f]);
        const float lo = floorDb > 0 ? std::pow(10.f, -floorDb / 20.f) : -1e30f;
        for (int f = 0; f < F; ++f) { const float g = std::max(mF[f], lo); out[2 * f] = se[2 * f] * g; out[2 * f + 1] = se[2 * f + 1] * g; }
        PT(7);
    }
};
}
