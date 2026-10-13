// 频谱回调（0.5）的自检。用法：aec48_hook <mic.pcm> <ref.pcm> <采样率> [录音目录 | bench]
// 第 4 个参数写 bench：只测耗时——同一段音频（循环到 30 s）不挂 / 挂恒等回调各跑 3 轮交替，报每块（384 样本）平均耗时，不做下面的检查。
// 逐项（任一不过即返回 1）：
//  H-1 格式查询：nfft 1536 / hop 384 / 窗型 1 / 采样率与创建时一致。
//  H-2 帧格式：直通（set_enabled 0）时回调拿到的频谱 = 「最近 4 块麦克风（开头补零）× 周期 sqrt-Hann」的不归一化负指数 DFT（逐帧比，最大相对误差 ≤ 1e-4）。
//  H-3 恒等回调：输出与不挂回调逐样本相同（float 全等）；回调每帧恰好调一次（次数 = 块数 − 1），bins = 769。
//  H-4 线性：回调把频谱乘 0.5 → 输出 = 不挂时 × 0.5（误差 ≤ 1e-6 满幅）；回调置零 → 输出全零。
//  H-5 开关：回调挂着但关掉 → 与不挂逐样本相同；中途打开 → 打开后的输出与「一直开着」相同（从打开那一帧之后、扣掉重叠相加的 3 块过渡）。
//  H-6 挂接约束：录音中挂 / 摘返回 -3；参数无效返回 -1。
//  H-7 录音：挂着 ×0.5 回调录音 → out.wav ≈ 不挂时 × 0.5、out_prehook.wav = 不挂时（int16 差 ≤ 1 LSB），meta.json 有 spec_hook。
//  H-8 开关事件定位：每次 process 送 10 块；在回调里于第 g 帧关掉回声消除、第 f 帧关掉回调（g < f：回调关掉后就不再被调用） → meta 事件 pos 都 = 生效那一块的末尾（(f+3)·384、(g+3)·384）。
//  H-9 同一目录：先挂回调录、再摘掉回调录 → 第二次后目录里不再有 out_prehook.wav。
//  H-11 摘掉、不挂处理 10 块、再挂上后录音：同上。
//  H-10 摘掉再挂上（中间不处理）后录音：out_prehook.wav 仍 = 回调前（Codex 审查 0.5：回调前那路的重叠相加状态不能从回调后的状态拷）。
#include "aec48.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
static std::vector<short> rd(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET); std::vector<short> v(n); if (fread(v.data(), 2, n, f) != (size_t)n) exit(2); fclose(f); return v; }
static int fails = 0;
static void ck(bool ok, const char* name, const char* fmt, double v) { printf("%s %-46s ", ok ? "通过  " : "不通过", name); printf(fmt, v); printf("\n"); if (!ok) ++fails; }

struct Rec { long calls = 0; int badBins = 0; float scale = 1.f; bool zero = false; };
static void hookFn(void* u, float* re, float* im, int bins) {
    Rec* r = (Rec*)u; ++r->calls; if (bins != 769) ++r->badBins;
    for (int b = 0; b < bins; ++b) { if (r->zero) { re[b] = 0; im[b] = 0; } else { re[b] *= r->scale; im[b] *= r->scale; } }
}
// 直通时核对帧格式：保存每帧频谱，事后用朴素 DFT 比
struct Cap { std::vector<std::vector<float>> re, im; long maxFrames = 6; };
struct Tog { Aec48* a = nullptr; long frame = 0, offHookAt = -1, offAecAt = -1; };
static void togFn(void* u, float*, float*, int) { Tog* t = (Tog*)u; if (t->frame == t->offHookAt) aec48_set_spec_hook_enabled(t->a, 0); if (t->frame == t->offAecAt) aec48_set_enabled(t->a, 0); ++t->frame; }
static std::string slurp(const std::string& p) { std::string m; FILE* f = fopen(p.c_str(), "rb"); if (f) { char b[4096]; size_t g; while ((g = fread(b, 1, sizeof b, f))) m.append(b, g); fclose(f); } return m; }
static bool exists(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (f) fclose(f); return f != nullptr; }
static void capFn(void* u, float* re, float* im, int bins) { Cap* c = (Cap*)u; if ((long)c->re.size() < c->maxFrames) { c->re.emplace_back(re, re + bins); c->im.emplace_back(im, im + bins); } }

static std::vector<float> run(int sr, const std::vector<float>& mic, const std::vector<float>& ref, aec48_spec_hook fn, void* u, int enabled = 1, long hookOnAt = -1) {
    Aec48* a = aec48_create(sr, 384); if (!a) { printf("创建失败\n"); exit(1); }
    aec48_set_enabled(a, enabled);
    if (fn) { aec48_set_spec_hook(a, fn, u); if (hookOnAt >= 0) aec48_set_spec_hook_enabled(a, 0); }
    std::vector<float> out(mic.size(), 0.f); const long n = (long)mic.size();
    for (long i = 0; i + 384 <= n; i += 384) { if (hookOnAt >= 0 && i == hookOnAt) aec48_set_spec_hook_enabled(a, 1); aec48_process(a, &mic[i], &ref[i], &out[i], 384); }
    aec48_destroy(a); return out;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "用法：%s mic.pcm ref.pcm 采样率 [录音目录]\n", argv[0]); return 2; }
    const int sr = atoi(argv[3]); auto m16 = rd(argv[1]), r16 = rd(argv[2]);
    if (argc > 4 && !strcmp(argv[4], "bench")) {
        const long L = 30L * sr / 384 * 384; std::vector<float> bm(L), br(L), bo(L); for (long i = 0; i < L; ++i) { bm[i] = m16[i % m16.size()] / 32768.f; br[i] = r16[i % r16.size()] / 32768.f; }
        double t[2] = {0, 0}; Rec r;
        for (int round = 0; round < 6; ++round) { const int h = round & 1; Aec48* a = aec48_create(sr, 384); if (h) aec48_set_spec_hook(a, hookFn, &r);
            auto t0 = std::chrono::steady_clock::now(); for (long i = 0; i < L; i += 384) aec48_process(a, &bm[i], &br[i], &bo[i], 384);
            t[h] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (L / 384); aec48_destroy(a); }
        printf("%s · 每块平均：不挂 %.3f ms，挂恒等回调 %.3f ms（多 %.3f ms，%+.1f%%）\n", aec48_version(), t[0] / 3, t[1] / 3, (t[1] - t[0]) / 3, 100 * (t[1] - t[0]) / t[0]);
        return 0; }
    long n = (long)std::min(m16.size(), r16.size()); n = std::min(n, 8L * sr); n -= n % 384;   // 8 s 足够
    std::vector<float> mic(n), ref(n); for (long i = 0; i < n; ++i) { mic[i] = m16[i] / 32768.f; ref[i] = r16[i] / 32768.f; }
    const long blocks = n / 384; printf("%s · 采样率 %d · %ld 块\n", aec48_version(), sr, blocks);

    { Aec48* a = aec48_create(sr, 384); int s = 0, N = 0, H = 0, w = 0; aec48_spec_format(a, &s, &N, &H, &w);
      ck(s == sr && N == 1536 && H == 384 && w == AEC48_WINDOW_SQRT_HANN_PERIODIC, "H-1 格式查询", "(nfft %.0f)", N);
      ck(aec48_set_spec_hook(nullptr, hookFn, nullptr) == -1, "H-6 参数无效返回 -1", "%.0f", 0);
      aec48_destroy(a); }

    {   // H-2：直通下回调频谱 = 最近 4 块麦克风加窗的 DFT。第 c 次调用（c 从 0 起）发生在第 c+2 块（从 1 数）到齐时，窗覆盖样本 [(c+2−4)·384, (c+2)·384)
        Cap cap; cap.maxFrames = 6; run(sr, mic, ref, capFn, &cap, 0);
        const int N = 1536, F = 769; double worst = 0;
        for (size_t c = 0; c < cap.re.size(); ++c) {
            const long end = (long)(c + 2) * 384, beg = end - N; double num = 0, den = 0;
            for (int k = 0; k < F; ++k) { double xr = 0, xi = 0;
                for (int q = 0; q < N; ++q) { long t = beg + q; double x = t >= 0 ? mic[t] : 0.0; x *= std::sqrt(0.5 * (1 - std::cos(2 * M_PI * q / N)));
                    xr += x * std::cos(2 * M_PI * (double)k * q / N); xi -= x * std::sin(2 * M_PI * (double)k * q / N); }
                if (k == 0 || k == F - 1) xi = 0;
                num = std::max(num, std::hypot(cap.re[c][k] - xr, cap.im[c][k] - xi)); den = std::max(den, std::hypot(xr, xi)); }
            if (den > 0) worst = std::max(worst, num / den); }
        ck(cap.re.size() == 6 && worst <= 1e-4, "H-2 帧格式 = 加窗不归一化负指数 DFT", "(最大相对误差 %.2e)", worst); }

    auto base = run(sr, mic, ref, nullptr, nullptr);
    {   Rec r; auto o = run(sr, mic, ref, hookFn, &r); long diff = 0; for (long i = 0; i < n; ++i) diff += o[i] != base[i];
        ck(diff == 0, "H-3 恒等回调：输出逐样本相同", "(不同样本 %.0f)", diff);
        ck(r.calls == blocks - 1 && r.badBins == 0, "H-3 每帧调一次、bins = 769", "(调用 %.0f 次)", r.calls); }
    {   Rec r; r.scale = 0.5f; auto o = run(sr, mic, ref, hookFn, &r); double e = 0; for (long i = 0; i < n; ++i) e = std::max(e, (double)std::fabs(o[i] - 0.5f * base[i]));
        ck(e <= 1e-6, "H-4 回调 ×0.5 → 输出 ×0.5", "(最大差 %.2e)", e);
        Rec z; z.zero = true; auto oz = run(sr, mic, ref, hookFn, &z); double mz = 0; for (long i = 0; i < n; ++i) mz = std::max(mz, (double)std::fabs(oz[i]));
        ck(mz == 0, "H-4 回调置零 → 输出全零", "(最大幅度 %.2e)", mz); }
    {   Rec r; r.scale = 0.5f; auto off = run(sr, mic, ref, hookFn, &r, 1, n + 384);   // 关着、一直没打开
        long diff = 0; for (long i = 0; i < n; ++i) diff += off[i] != base[i];
        ck(diff == 0, "H-5 挂着但关掉 = 不挂", "(不同样本 %.0f)", diff);
        const long at = (blocks / 2) * 384; Rec r2; r2.scale = 0.5f; auto sw = run(sr, mic, ref, hookFn, &r2, 1, at);
        Rec r3; r3.scale = 0.5f; auto on = run(sr, mic, ref, hookFn, &r3);
        double e = 0; for (long i = at + 4 * 384; i < n - 384; ++i) e = std::max(e, (double)std::fabs(sw[i] - on[i]));   // 打开那一块起算第 1 帧、扣 3 块重叠相加过渡 + 输出延迟
        ck(e <= 1e-6, "H-5 中途打开后 = 一直开着", "(最大差 %.2e)", e); }

    if (argc > 4) {   // H-6 / H-7：录音
        const std::string d = argv[4]; Aec48* a = aec48_create(sr, 384); Rec r; r.scale = 0.5f;
        aec48_set_spec_hook(a, hookFn, &r);
        int rc0 = aec48_dump_start(a, d.c_str()); int rcHook = aec48_set_spec_hook(a, nullptr, nullptr);
        ck(rc0 == 0 && rcHook == -3, "H-6 录音中摘挂钩返回 -3", "(%.0f)", rcHook);
        std::vector<float> o(n, 0.f); for (long i = 0; i + 384 <= n; i += 384) aec48_process(a, &mic[i], &ref[i], &o[i], 384);
        aec48_dump_stop(a); aec48_destroy(a);
        auto ow = rd((d + "/out.wav").c_str()), pw = rd((d + "/out_prehook.wav").c_str());   // 跳过 44 字节头（22 个 short）
        auto q = [](float x) { float v = x * 32767.f; return (int)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : lrintf(v))); };
        long m = std::min((long)ow.size(), (long)pw.size()) - 22; int eo = 0, ep = 0;
        for (long i = 0; i < m && i < n; ++i) { eo = std::max(eo, std::abs(ow[i + 22] - q(0.5f * base[i]))); ep = std::max(ep, std::abs(pw[i + 22] - q(base[i]))); }
        ck(m == n && eo <= 1, "H-7 录音 out.wav = 回调后", "(最大差 %.0f LSB)", eo);
        ck(m == n && ep <= 1, "H-7 录音 out_prehook.wav = 回调前", "(最大差 %.0f LSB)", ep);
        FILE* f = fopen((d + "/meta.json").c_str(), "rb"); std::string meta; if (f) { char b[4096]; size_t g; while ((g = fread(b, 1, sizeof b, f))) meta.append(b, g); fclose(f); }
        ck(meta.find("\"spec_hook\":{\"attached_at_start\":1") != std::string::npos, "H-7 meta.json 记了 spec_hook", "%.0f", 0);

        {   // H-8：回调在第 f 帧关回调、第 g 帧关回声消除（都是下一块生效）；每次 process 送 10 块，事件仍要落在块上
            const long f = 37, g = 29; Aec48* b = aec48_create(sr, 3840); Tog t; t.a = b; t.offHookAt = f; t.offAecAt = g;
            aec48_set_spec_hook(b, togFn, &t); aec48_dump_start(b, d.c_str());
            std::vector<float> o2(n, 0.f); for (long i = 0; i + 3840 <= n; i += 3840) aec48_process(b, &mic[i], &ref[i], &o2[i], 3840);
            aec48_dump_stop(b); aec48_destroy(b); std::string m = slurp(d + "/meta.json");
            char e1[96], e2[96]; snprintf(e1, sizeof e1, "{\"pos\":%ld,\"spec_hook_enabled\":0}", (f + 3) * 384); snprintf(e2, sizeof e2, "{\"pos\":%ld,\"enabled\":0}", (g + 3) * 384);
            ck(m.find(e1) != std::string::npos && m.find(e2) != std::string::npos && m.find("\"events_overflow\":0") != std::string::npos, "H-8 开关事件落在生效那一块的末尾", "(回调开关 pos %.0f)", (double)(f + 3) * 384);
            if (m.find(e1) == std::string::npos) printf("    meta：%s\n", m.c_str()); }
        {   // H-9：同一目录先挂回调录（上面 H-8 刚录过，有 out_prehook.wav），再不挂回调录
            bool had = exists(d + "/out_prehook.wav"); Aec48* b = aec48_create(sr, 384); aec48_dump_start(b, d.c_str());
            std::vector<float> o2(n, 0.f); for (long i = 0; i + 384 <= n; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_dump_stop(b); aec48_destroy(b);
            ck(had && !exists(d + "/out_prehook.wav"), "H-9 不挂回调再录：旧 out_prehook.wav 已删", "%.0f", 0); }
        {   // H-10：先挂置零回调处理一半 → 摘掉 → 马上挂回 ×1 回调 → 录后一半：out_prehook 应 = 一直不挂时的输出
            Aec48* b = aec48_create(sr, 384); Rec z; z.zero = true; Rec one; aec48_set_spec_hook(b, hookFn, &z);
            const long half = (blocks / 2) * 384; std::vector<float> o2(n, 0.f);
            for (long i = 0; i < half; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_set_spec_hook(b, nullptr, nullptr); aec48_set_spec_hook(b, hookFn, &one); aec48_dump_start(b, d.c_str());
            for (long i = half; i + 384 <= n; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_dump_stop(b); aec48_destroy(b);
            auto pw = rd((d + "/out_prehook.wav").c_str()); int e = 0;
            for (long i = 0; i + 22 < (long)pw.size() && half + i < n; ++i) e = std::max(e, std::abs(pw[i + 22] - q(base[half + i])));
            ck((long)pw.size() - 22 == n - half && e <= 1, "H-10 摘掉再挂上后录音：out_prehook = 回调前", "(最大差 %.0f LSB)", e); }
        {   // H-11：置零回调处理一段 → 摘掉 → 不挂处理 10 块（> 4 块，走「两路本来相等、直接拷」那条路）→ 挂回 ×1 回调 → 录：out_prehook = 回调前
            Aec48* b = aec48_create(sr, 384); Rec z; z.zero = true; Rec one; aec48_set_spec_hook(b, hookFn, &z);
            const long q1 = (blocks / 4) * 384, q2 = q1 + 10 * 384; std::vector<float> o2(n, 0.f);
            for (long i = 0; i < q1; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_set_spec_hook(b, nullptr, nullptr);
            for (long i = q1; i < q2; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_set_spec_hook(b, hookFn, &one); aec48_dump_start(b, d.c_str());
            for (long i = q2; i + 384 <= n; i += 384) aec48_process(b, &mic[i], &ref[i], &o2[i], 384);
            aec48_dump_stop(b); aec48_destroy(b);
            auto pw = rd((d + "/out_prehook.wav").c_str()); int e = 0;
            for (long i = 0; i + 22 < (long)pw.size() && q2 + i < n; ++i) e = std::max(e, std::abs(pw[i + 22] - q(base[q2 + i])));
            ck((long)pw.size() - 22 == n - q2 && e <= 1, "H-11 摘掉 10 块后再挂上：out_prehook = 回调前", "(最大差 %.0f LSB)", e); }
    }
    if (fails) printf("共 %d 项不通过\n", fails); else printf("全部通过\n");
    return fails ? 1 : 0;
}
