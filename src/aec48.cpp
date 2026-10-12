// aec48 C 接口实现。整链 = 新线性级（网络估噪声的对角卡尔曼 + 在线时延估计，双滤波）→ CoFi-Lite 掩码，两级共用一套 STFT（1536 / 384）。
// 模型编进库里（models.cpp，tools/embed_models.py 生成），从内存加载。process 内不分配、不加锁、不做文件读写；录音由后台线程写。
#include "aec48.h"
#include "aec_chain.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" { extern const unsigned char aec48_lin_model[]; extern const unsigned long aec48_lin_model_size;
             extern const unsigned char aec48_mask_model[]; extern const unsigned long aec48_mask_model_size; extern const char aec48_model_id[]; }

namespace {
struct Ring {   // 单生产单消费、无锁
    std::vector<float> b; size_t m = 0; std::atomic<size_t> w{0}, r{0};
    void init(size_t pow2) { b.assign(pow2, 0.f); m = pow2 - 1; w = 0; r = 0; }
    size_t size() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
    bool push(const float* x, size_t n) { if (b.size() - size() < n) return false; size_t W = w.load(std::memory_order_relaxed); for (size_t i = 0; i < n; ++i) b[(W + i) & m] = x[i]; w.store(W + n, std::memory_order_release); return true; }
    size_t pop(float* x, size_t n) { size_t s = size(); if (n > s) n = s; size_t R = r.load(std::memory_order_relaxed); for (size_t i = 0; i < n; ++i) x[i] = b[(R + i) & m]; r.store(R + n, std::memory_order_release); return n; }
};
struct Wav {
    FILE* f = nullptr; long n = 0; int sr = 48000;
    bool open(const std::string& p, int sr_) { sr = sr_; n = 0; f = fopen(p.c_str(), "wb"); if (!f) return false; unsigned char h[44] = {0}; fwrite(h, 1, 44, f); return true; }
    void write(const float* x, size_t k) { short s[1024]; while (k) { size_t c = k < 1024 ? k : 1024; for (size_t i = 0; i < c; ++i) { float v = x[i] * 32767.f; s[i] = (short)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : lrintf(v))); } fwrite(s, 2, c, f); n += c; x += c; k -= c; } }
    void close() { if (!f) return; unsigned int d = (unsigned)(n * 2), r = (unsigned)sr, br = (unsigned)sr * 2, c = 36 + d; unsigned short pcm = 1, ch = 1, ba = 2, bits = 16; unsigned int fmt = 16;
        fseek(f, 0, SEEK_SET); fwrite("RIFF", 1, 4, f); fwrite(&c, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
        fwrite(&r, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fclose(f); f = nullptr; }
};
}  // namespace

struct Aec48 {
    ac::Chain chain; int sr = 48000, H = 384, prefill = 0, lat = 0;
    std::vector<float> inM, inR, blkO; int fill = 0; Ring outQ;
    std::atomic<int> enabled{1};
    // 统计
    std::atomic<double> avgMs{0}, maxMs{0}; std::atomic<long> samples{0}, dumpDrop{0}; double accMs = 0, accMax = 0; long accN = 0; std::chrono::steady_clock::time_point lastStat;
    // 录音（dump）
    std::mutex dmu; std::atomic<bool> dumping{false}; std::thread dth; std::condition_variable dcv; Ring dq[3]; Wav wav[3]; std::string ddir;
    std::vector<std::string> events; std::mutex emu; long dumpStart = 0; std::chrono::system_clock::time_point dumpWall;
    std::vector<float> fm, fr, fo; int enabledAtStart = 1;
};

static void block(Aec48* a, const float* m, const float* r) {
    auto t0 = std::chrono::steady_clock::now();
    a->chain.push(m, r, a->blkO.data(), a->enabled.load(std::memory_order_relaxed) ? 2 : 0);
    a->outQ.push(a->blkO.data(), a->H);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    a->accMs += ms; a->accN++; if (ms > a->accMax) a->accMax = ms;
    if (t0 - a->lastStat > std::chrono::seconds(1)) { a->avgMs = a->accMs / a->accN; a->maxMs = a->accMax; a->accMs = 0; a->accN = 0; a->accMax = 0; a->lastStat = t0; }
}

extern "C" {
const char* aec48_version(void) { static std::string v = std::string("aec48 0.1 (") + aec48_model_id + ")"; return v.c_str(); }

Aec48* aec48_create(int sample_rate, int frames_per_call) {
    if (sample_rate != 48000 && sample_rate != 44100) return nullptr;
    fb::fb_register_model("lin", aec48_lin_model, aec48_lin_model_size); fb::fb_register_model("mask", aec48_mask_model, aec48_mask_model_size);
    auto* a = new Aec48(); a->sr = sample_rate;
    if (!a->chain.load("mem:lin", "mem:mask", sample_rate)) { delete a; return nullptr; }
    a->H = a->chain.H; a->inM.assign(a->H, 0.f); a->inR.assign(a->H, 0.f); a->blkO.assign(a->H, 0.f); a->outQ.init(1 << 15);
    // 块长是 H 的整数倍时输入凑满一块立刻出一块，输出不会欠；否则先垫 H 个零，保证任何块长都不欠（延迟多一块）
    a->prefill = (frames_per_call > 0 && frames_per_call % a->H == 0) ? 0 : a->H;
    std::vector<float> z(a->prefill, 0.f); if (a->prefill) a->outQ.push(z.data(), z.size());
    a->lat = a->chain.latency() + a->prefill; a->lastStat = std::chrono::steady_clock::now();
    a->fm.assign(8192, 0.f); a->fr.assign(8192, 0.f); a->fo.assign(8192, 0.f);
    return a;
}

int aec48_latency_samples(const Aec48* a) { return a ? a->lat : 0; }

void aec48_set_enabled(Aec48* a, int on) {
    if (!a) return; a->enabled = on ? 1 : 0;
    if (a->dumping.load()) { std::lock_guard<std::mutex> lk(a->emu); char b[96]; snprintf(b, sizeof b, "{\"pos\":%ld,\"enabled\":%d}", a->samples.load() - a->dumpStart, on ? 1 : 0); a->events.push_back(b); }
}

int aec48_process(Aec48* a, const float* mic, const float* ref, float* out, int n) {
    if (!a || n <= 0) return -1;
    if (a->dumping.load(std::memory_order_relaxed)) { if (!a->dq[0].push(mic, n) || !a->dq[1].push(ref, n)) a->dumpDrop.fetch_add(1); }
    int i = 0; const int H = a->H;
    // 先把所有输入喂完（凑满一块就处理），再取输出——out 与 mic 可以是同一块内存
    while (i < n) { int c = std::min(H - a->fill, n - i); memcpy(&a->inM[a->fill], mic + i, c * 4); memcpy(&a->inR[a->fill], ref + i, c * 4); a->fill += c; i += c;
        if (a->fill == H) { block(a, a->inM.data(), a->inR.data()); a->fill = 0; } }
    size_t got = a->outQ.pop(out, n); if (got < (size_t)n) memset(out + got, 0, (n - got) * 4);
    if (a->dumping.load(std::memory_order_relaxed)) { if (!a->dq[2].push(out, n)) a->dumpDrop.fetch_add(1); a->dcv.notify_one(); }
    a->samples.fetch_add(n, std::memory_order_relaxed);
    return 0;
}

int aec48_process_s16(Aec48* a, const short* mic, const short* ref, short* out, int n) {
    if (!a || n <= 0) return -1;
    if ((int)a->fm.size() < n) return -2;   // 缓冲在 create 时按 8192 预留；更大的块请拆开调
    for (int i = 0; i < n; ++i) { a->fm[i] = mic[i] / 32768.f; a->fr[i] = ref[i] / 32768.f; }
    int r = aec48_process(a, a->fm.data(), a->fr.data(), a->fo.data(), n);
    for (int i = 0; i < n; ++i) { float v = a->fo[i] * 32767.f; out[i] = (short)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : lrintf(v))); }
    return r;
}

int aec48_dump_start(Aec48* a, const char* dir) {
    if (!a || !dir) return -1; std::lock_guard<std::mutex> lk(a->dmu); if (a->dumping.load()) return 0;
    a->ddir = dir; static const char* nm[3] = {"mic", "ref", "out"};
    for (auto& q : a->dq) if (q.b.empty()) q.init(1 << 19);   // 录音缓冲在第一次录音时才分配（约 6 MB），不录音不占；process 只在 dumping 置位后才写它
    for (int k = 0; k < 3; ++k) { a->dq[k].r.store(a->dq[k].w.load()); if (!a->wav[k].open(a->ddir + "/" + nm[k] + ".wav", a->sr)) { for (int j = 0; j < k; ++j) a->wav[j].close(); return -2; } }
    { std::lock_guard<std::mutex> e(a->emu); a->events.clear(); }
    a->dumpStart = a->samples.load(); a->dumpWall = std::chrono::system_clock::now(); a->dumpDrop = 0; a->enabledAtStart = a->enabled.load(); a->dumping = true;
    a->dth = std::thread([a] { std::vector<float> b(1 << 15); std::mutex m; std::unique_lock<std::mutex> lk(m);
        for (;;) { bool on = a->dumping.load(); size_t any = 0;
            for (int k = 0; k < 3; ++k) { size_t g = a->dq[k].pop(b.data(), b.size()); if (g) { a->wav[k].write(b.data(), g); any += g; } }
            if (!on && !any) break; if (!any) a->dcv.wait_for(lk, std::chrono::milliseconds(50)); } });
    return 0;
}

void aec48_dump_stop(Aec48* a) {
    if (!a) return; std::lock_guard<std::mutex> lk(a->dmu); if (!a->dumping.load()) return;
    a->dumping = false; a->dcv.notify_one(); if (a->dth.joinable()) a->dth.join();
    long n = a->wav[0].n; for (auto& w : a->wav) w.close();
    // 说明文件：分析时对齐用（out 比 mic 晚 latency_samples；enabled 开关切换的位置）
    FILE* f = fopen((a->ddir + "/meta.json").c_str(), "wb");
    if (f) { long long t = std::chrono::duration_cast<std::chrono::milliseconds>(a->dumpWall.time_since_epoch()).count();
        fprintf(f, "{\"version\":\"%s\",\"sample_rate\":%d,\"latency_samples\":%d,\"samples\":%ld,\"start_unix_ms\":%lld,\"dump_dropped_blocks\":%ld,\"enabled_at_start\":%d,\"events\":[",
                aec48_version(), a->sr, a->lat, n, t, a->dumpDrop.load(), a->enabledAtStart);
        std::lock_guard<std::mutex> e(a->emu); for (size_t i = 0; i < a->events.size(); ++i) fprintf(f, "%s%s", i ? "," : "", a->events[i].c_str()); fprintf(f, "]}\n"); fclose(f); }
}

void aec48_stats(const Aec48* a, double o[4]) { if (!a) return; o[0] = a->avgMs; o[1] = a->maxMs; o[2] = (double)a->samples.load(); o[3] = (double)a->dumpDrop.load(); }

void aec48_destroy(Aec48* a) { if (!a) return; aec48_dump_stop(a); delete a; }
}
