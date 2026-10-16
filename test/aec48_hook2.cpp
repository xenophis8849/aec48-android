// 第 2 版频谱回调自检：合成确定性输入，不依赖外部录音；任一项失败返回 1。
#include "aec48.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#ifdef AEC48_TESTING
extern "C" int aec48_internal_hook2_hint_error(Aec48*, double[2]);
extern "C" int aec48_internal_hook2_hint_exact(Aec48*);
extern "C" int aec48_internal_hook2_scaled_inputs(Aec48*);
extern "C" int aec48_internal_hook2_hint_values(Aec48*, double*, double*, double*);
#endif
static int fails = 0;
static void ck(bool ok, const char* n) { printf("%s %s\n", ok ? "通过  " : "不通过", n); fails += !ok; }
static void one(void* u, float*, float*, int) { if (u) ++*(int*)u; }
struct Rec {
    Aec48* a = nullptr; int block = 0, calls = 0, bad = 0, mode = 2; bool constraints = false;
    std::vector<float> far; float scale = 1.f; double energy = 0, absErr = 0, relErr = 0; int comparisons = 0, bitDiff = 0, scaledFrames = 0, scaledDiff = 0; double conversionErr = 0;
    explicit Rec(int n) : far(n, -1) {}
};
static void two(void* u, float* re, float* im, int bins, const Aec48SpecSide* s) {
    Rec& r = *(Rec*)u; ++r.calls;
    r.bad += !s || bins != 769;
    if (!s) return;
    r.bad += s->size != sizeof(*s) || s->aec_mode != r.mode || !(s->near_talk < 0) || s->resid_bins != bins || !s->resid_echo;
    r.bad += s->far_active != 0 && s->far_active != 1;
    r.far[r.block] = s->far_active;
    if (s->resid_echo) for (int k = 0; k < bins; ++k) { float p = s->resid_echo[k]; r.bad += !std::isfinite(p) || p < 0; if (r.block > 100) r.energy += p; }
    if (r.constraints) { r.bad += aec48_set_spec_hook(r.a, one, nullptr) != -2; r.bad += aec48_set_spec_hook2(r.a, nullptr, nullptr) != -2; }
#ifdef AEC48_TESTING
    int sd = aec48_internal_hook2_scaled_inputs(r.a); if (sd >= 0) { ++r.scaledFrames; r.scaledDiff += sd; }
    int different = aec48_internal_hook2_hint_exact(r.a); if (different >= 0) r.bitDiff += different;
    double normalized[769], raw[769], g2;
    if (aec48_internal_hook2_hint_values(r.a, normalized, raw, &g2) == bins) {
        r.bad += !(g2 > 0) || !std::isfinite(g2);
        for (int k = 0; k < bins; ++k) {
            double expected = normalized[k] / g2;
            r.conversionErr = std::max(r.conversionErr, std::fabs(raw[k] - expected) / std::max(expected, 1e-30));
            if (s->aec_mode == 2) r.bad += s->resid_echo[k] > raw[k] * (1 + 2e-7) + 1e-30;
        }
    }
    double e[2]; if (aec48_internal_hook2_hint_error(r.a, e)) { ++r.comparisons; r.absErr = std::max(r.absErr, e[0]); r.relErr = std::max(r.relErr, e[1]); }
#endif
    for (int k = 0; k < bins; ++k) { re[k] *= r.scale; im[k] *= r.scale; }
}
static std::vector<float> run(int sr, const std::vector<float>& m, const std::vector<float>& x, int kind, Rec& r, int mode = 2) {
    Aec48* a = aec48_create(sr, 384); if (!a) exit(2); r.a = a; r.mode = mode;
    aec48_set_enabled(a, mode != 0);
    if (kind == 1) aec48_set_spec_hook(a, one, nullptr);
    if (kind == 2 || kind == 3) aec48_set_spec_hook2(a, two, &r);
    if (kind == 3) aec48_set_spec_hook_enabled(a, 0);
    std::vector<float> out(m.size());
    for (size_t i = 0; i < m.size(); i += 384) { r.block = i / 384; aec48_process(a, &m[i], &x[i], &out[i], 384); }
    aec48_destroy(a); r.a = nullptr; return out;
}
static bool same(const std::vector<float>& a, const std::vector<float>& b) { return a.size() == b.size() && !memcmp(a.data(), b.data(), a.size() * sizeof(float)); }
static std::string read(const std::string& p) { std::string s; FILE* f = fopen(p.c_str(), "rb"); if (f) { char b[1024]; size_t n; while ((n = fread(b, 1, sizeof b, f))) s.append(b, n); fclose(f); } return s; }
int main() {
    const int B = 384, blocks = 340; unsigned rng = 7;
    std::vector<float> m(B * blocks), x(m.size());
    for (size_t i = 0; i < m.size(); ++i) { rng = rng * 1664525u + 1013904223u; float v = ((int)(rng >> 8) - 8388608) * (0.15f / 8388608);
        // 头尾静音用来核对拖尾，后半段持续参考便于比较残余回声。
        x[i] = (i / B >= 20 && i / B < 40) || i / B >= 220 ? v : 0.f;
        m[i] = (i >= 2 * B ? 0.65f * x[i - 2 * B] : 0.f) + 0.002f * std::sin((float)i * 0.137f); }
    for (int sr : {48000, 44100}) {
        printf("采样率 %d\n", sr); Rec b(blocks), h1(blocks), h2(blocks), off(blocks), pass(blocks);
        auto base = run(sr, m, x, 0, b), v1 = run(sr, m, x, 1, h1), v2 = run(sr, m, x, 2, h2), disabled = run(sr, m, x, 3, off);
        ck(same(base, v1) && same(base, v2) && same(base, disabled), "无回调 / 第 1 版恒等 / 第 2 版恒等 / 关闭第 2 版：float 逐字节相同");
        ck(h2.calls == blocks - 1 && h2.bad == 0 && off.calls == 0, "调用次数、字段、769 个有限非负功率、关闭后不调用");
        int hold = (sr + B - 1) / B; bool far = true;
        for (int k = 1; k < 20; ++k) far &= h2.far[k] == 0;
        for (int k = 20; k < 43 + hold - 1; ++k) far &= h2.far[k] == 1;
        for (int k = 43 + hold - 1; k < 220; ++k) far &= h2.far[k] == 0;
        ck(far, "原始参考静音 / 有声 / 窗尾后 1 秒拖尾（按实际采样率）");
        run(sr, m, x, 2, pass, 0);
        printf("残余功率和：整链 %.9g，直通 %.9g\n", h2.energy, pass.energy);
        ck(pass.bad == 0 && pass.energy > h2.energy * 1.2, "同一输入直通残余功率明显大于整链");
        if (h2.comparisons) {
            ck(h2.comparisons == blocks - 1 && h2.bitDiff == 0 && pass.bitDiff == 0, "候选版直接复用 Cofi 提示功率：每帧每频点逐位一致");
            ck(h2.conversionErr < 2e-7 && pass.conversionErr < 2e-7 && h2.relErr < 2e-7, "候选版提示除以本帧 g² 正确（回乘只容许 float 舍入）");
            printf("候选版 %d 帧：提示功率不同频点 %d，换算最大相对误差 %.9g，回乘最大相对误差 %.9g\n", h2.comparisons, h2.bitDiff, h2.conversionErr, h2.relErr);
        }
        if (h2.scaledFrames) {
            ck(h2.scaledFrames == blocks - 1 && h2.scaledDiff == 0 && pass.scaledDiff == 0, "默认版实际提示输入带功率 = Pe/Px 乘一次本帧 g²（逐位核对）");
            ck(h2.conversionErr < 2e-7 && pass.conversionErr < 2e-7, "默认版实际提示输出除以同一帧 g²，换算正确");
        }
        Rec scaled(blocks); scaled.scale = 0.5f; auto half = run(sr, m, x, 2, scaled); double err = 0;
        for (size_t i = 0; i < half.size(); ++i) err = std::max(err, (double)std::fabs(half[i] - 0.5f * base[i]));
        ck(err <= 1e-6, "hook2 原地修改频谱生效：输出乘 0.5");
        Aec48* a = aec48_create(sr, B); Rec r(blocks); r.a = a; r.constraints = true; int n1 = 0; float out[B];
        ck(aec48_set_spec_hook2(nullptr, two, &r) == -1, "hook2 空实例返回 -1");
        aec48_set_spec_hook(a, one, &n1); aec48_set_spec_hook2(a, two, &r);
        for (int k = 0; k < 30; ++k) { r.block = k; aec48_process(a, &m[k * B], &x[k * B], out, B); }
        ck(n1 == 0 && r.calls == 29 && r.bad == 0, "第 2 版替换第 1 版；process 中两种 setter 均返回 -2");
        int old = r.calls; aec48_set_spec_hook(a, one, &n1); aec48_process(a, m.data(), x.data(), out, B);
        ck(n1 == 1 && r.calls == old, "第 1 版替换第 2 版");
        aec48_set_spec_hook2(a, nullptr, nullptr); aec48_process(a, m.data(), x.data(), out, B);
        ck(n1 == 1 && r.calls == old, "hook2(NULL) 同时摘除第 1 版");
        for (int k = 0; k < 4; ++k) aec48_process(a, m.data(), x.data(), out, B);
        aec48_set_spec_hook2(a, two, &r); r.block = 31; r.mode = 0; aec48_set_enabled(a, 0); aec48_process(a, m.data(), x.data(), out, B);
        ck(r.bad == 0 && r.far[31] == 0, "重挂清空峰值与拖尾；set_enabled 切换到 mode 0");
        r.mode = 2; aec48_set_enabled(a, 1); aec48_process(a, m.data(), x.data(), out, B); ck(r.bad == 0, "set_enabled 切回 mode 2");
        // 关闭期间冻结拖尾，长时间静音也不消耗计数；直接替换现有 hook2 必须重新开始。
        for (int k = 20; k < 24; ++k) { r.block = k; aec48_process(a, &m[k * B], &x[k * B], out, B); }
        int onCalls = r.calls; aec48_set_spec_hook_enabled(a, 0);
        for (int k = 0; k < 180; ++k) aec48_process(a, m.data(), x.data(), out, B);
        aec48_set_spec_hook_enabled(a, 1); r.block = 32; aec48_process(a, m.data(), x.data(), out, B);
        ck(r.calls == onCalls + 1 && r.far[32] == 1, "关闭期间不调用且冻结拖尾，重新打开仍有拖尾");
        aec48_set_spec_hook2(a, two, &r); r.block = 33; aec48_process(a, m.data(), x.data(), out, B);
        ck(r.far[33] == 0, "直接替换现有 hook2 清空历史状态");
        const char* td = getenv("TMPDIR"); std::string tp = std::string(td && td[0] ? td : "/tmp") + "/aec48-hook2-XXXXXX"; std::vector<char> tmp(tp.begin(), tp.end()); tmp.push_back(0); char* dir = mkdtemp(tmp.data());   /* 手机上没有 /tmp：用 TMPDIR */ ck(dir && aec48_dump_start(a, dir) == 0, "hook2 开始录音");
        ck(aec48_set_spec_hook(a, nullptr, nullptr) == -3 && aec48_set_spec_hook2(a, nullptr, nullptr) == -3, "录音中两种 setter 均返回 -3");
        r.constraints = false; aec48_process(a, m.data(), x.data(), out, B); aec48_dump_stop(a);
        std::string d = dir ? dir : "", meta = read(d + "/meta.json");
        ck(meta.find("\"contract_version\":2") != std::string::npos && read(d + "/out_prehook.wav").size() == 44 + B * 2, "hook2 录音保留 out_prehook.wav 并记录契约版本 2");
        aec48_set_spec_hook(a, one, nullptr); aec48_dump_start(a, dir); aec48_process(a, m.data(), x.data(), out, B); aec48_dump_stop(a);
        ck(read(d + "/meta.json").find("\"contract_version\":1") != std::string::npos, "第 1 版录音记录契约版本 1");
        for (int next : {1, 2}) {
            Aec48* c = aec48_create(sr, B); Rec zero(blocks); zero.a = c; zero.scale = 0;
            aec48_set_spec_hook2(c, two, &zero);
            for (int k = 0; k < 64; ++k) { zero.block = k; aec48_process(c, &m[k * B], &x[k * B], out, B); }
            if (next == 1) aec48_set_spec_hook(c, one, nullptr); else aec48_set_spec_hook2(c, two, &zero);
            aec48_dump_start(c, dir);
            for (int k = 64; k < 80; ++k) { zero.block = k; aec48_process(c, &m[k * B], &x[k * B], out, B); }
            aec48_dump_stop(c); aec48_destroy(c); std::string wav = read(d + "/out_prehook.wav"); bool eq = wav.size() == 44 + 16 * B * 2;
            for (int i = 0; eq && i < 16 * B; ++i) { short v; memcpy(&v, wav.data() + 44 + i * 2, 2); float z = base[64 * B + i] * 32767.f; int q = (int)lrintf(std::max(-32768.f, std::min(32767.f, z))); eq &= abs((int)v - q) <= 1; }
            ck(eq, next == 1 ? "修改频谱的 hook2 直接换 hook：回调前录音连续" : "修改频谱的 hook2 直接重挂：回调前录音连续");
        }
        for (const char* f : {"mic.wav", "ref.wav", "out.wav", "out_prehook.wav", "meta.json"}) unlink((d + "/" + f).c_str()); if (dir) rmdir(dir);
        aec48_set_spec_hook(a, nullptr, nullptr); old = r.calls; aec48_process(a, m.data(), x.data(), out, B); ck(r.calls == old, "hook(NULL) 摘除后不再回调"); aec48_destroy(a);
    }
    printf("%s：%d 项不通过\n", fails ? "不通过" : "全部通过", fails); return fails ? 1 : 0;
}
