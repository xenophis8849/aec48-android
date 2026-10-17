// aec48 C 接口实现。整链 = 新线性级（网络估噪声的对角卡尔曼 + 在线时延估计，双滤波）→ CoFi-Lite 掩码，两级共用一套 STFT（1536 / 384）。
// 模型编进库里（models.cpp，tools/embed_models.py 生成），从内存加载。
// process / process_s16：任意长度都在内部按 384 样本块处理、边处理边取输出（支持 out 与 mic 同址）；处理期不分配内存、不加锁、不做文件读写。
//   int16 与 float 走同一个循环（0.3：不再按 4096 内部分段——分段会在声明块长为 384 倍数时插零，见审查复查 R-01）。
// 录音：mic / ref / out 三路按"一次 process 调用"整包写入（三路要么都收、要么都丢，丢的位置记进 meta.json）；写盘在后台线程，
//   直接用文件描述符写，只计真正落盘的字节（写盘出错时 WAV 头、meta 与文件实际内容一致，R-03）。
// 频谱回调（0.5，给串联降噪等后处理用）：每帧输出频谱在合成前交给接入方的回调原地改，库本身不依赖任何后处理库。
//   挂着回调时，处理链同时合成一路「回调前」的输出进 outP（与 outQ 同步进出）；录音开始时挂着回调，就多写 out_prehook.wav。
#include "aec48.h"
#include "aec_chain.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

extern "C" { extern const unsigned char aec48_lin_model[]; extern const unsigned long aec48_lin_model_size;
#ifdef AEC48_DUAL
             extern const unsigned char aec48_sing_mask_model[]; extern const unsigned long aec48_sing_mask_model_size;
             extern const unsigned char aec48_talk_mask_model[]; extern const unsigned long aec48_talk_mask_model_size;
#else
             extern const unsigned char aec48_mask_model[]; extern const unsigned long aec48_mask_model_size;
#endif
             extern const char aec48_model_id[]; }

namespace {
struct Ring {   // 单生产单消费、无锁
    std::vector<float> b; size_t m = 0; std::atomic<size_t> w{0}, r{0};
    void init(size_t pow2) { b.assign(pow2, 0.f); m = pow2 - 1; w = 0; r = 0; }
    size_t size() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }
    size_t space() const { return b.size() - size(); }
    void push(const float* x, size_t n) { size_t W = w.load(std::memory_order_relaxed); for (size_t i = 0; i < n; ++i) b[(W + i) & m] = x[i]; w.store(W + n, std::memory_order_release); }   // 调用方先查 space
    size_t pop(float* x, size_t n) { size_t s = size(); if (n > s) n = s; size_t R = r.load(std::memory_order_relaxed); for (size_t i = 0; i < n; ++i) x[i] = b[(R + i) & m]; r.store(R + n, std::memory_order_release); return n; }
};
inline short toS16(float x) { float v = x * 32767.f; return (short)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : lrintf(v))); }
struct Wav {   // 16 bit 单声道，直接写文件描述符（自带缓冲）。bytes 只计 write() 确认落盘的字节；出错置 err、之后不再写
    int fd = -1; long long bytes = 0; long n = 0; int sr = 48000; bool err = false; std::vector<short> buf; size_t bn = 0;
    bool wr(const void* p, size_t len) {
        const char* c = (const char*)p;
        while (len) { ssize_t w = ::write(fd, c, len); if (w < 0 && errno == EINTR) continue; if (w <= 0) return false; c += w; len -= (size_t)w; bytes += w; }
        return true; }
    void flush() { if (bn && !err && !wr(buf.data(), bn * 2)) err = true; bn = 0; }
    bool open(const std::string& p, int sr_) {   // buf 由 dump_start 预先分配
        sr = sr_; n = 0; bytes = 0; bn = 0; err = false; fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return false;
        unsigned char h[44] = {0}; if (!wr(h, 44)) err = true; return true; }
    void write(const float* x, size_t k) {
        while (k && fd >= 0 && !err) { size_t c = std::min(k, buf.size() - bn); for (size_t i = 0; i < c; ++i) buf[bn + i] = toS16(x[i]); bn += c; x += c; k -= c; if (bn == buf.size()) flush(); } }
    void close() {   // 头里的样本数 = 实际落盘的完整样本；出错时把半个样本的尾巴截掉，文件长度 = 44 + 2·n
        if (fd < 0) return; flush();
        const long long pay = bytes > 44 ? bytes - 44 : 0; n = (long)(pay / 2);
        if (err && bytes >= 44 && ftruncate(fd, 44 + (off_t)n * 2) != 0) err = true;
        unsigned int d = (unsigned)(n * 2), r = (unsigned)sr, br = (unsigned)sr * 2, c = 36 + d; unsigned short pcm = 1, ch = 1, ba = 2, bits = 16; unsigned int fmt = 16;
        unsigned char h[44]; memcpy(h, "RIFF", 4); memcpy(h + 4, &c, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
        memcpy(h + 24, &r, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &d, 4);
        if (bytes >= 44 && pwrite(fd, h, 44, 0) != 44) err = true;   // 头只覆盖已有的 44 字节，不加长文件
        if (::close(fd) != 0) err = true;
        fd = -1; }
};
struct Drop { long pos; long n; };
struct Ev { long pos; unsigned char kind, val; };   // 录音期间的开关事件：kind 0 = 回声消除开关，1 = 频谱回调开关
const int kMaxEv = 1024;
const int kMaxDrops = 1024;   // 丢包位置表：相邻的丢包合并成一段；段数超过上限后只记溢出起点与合计（meta 里 dropped_overflow，R-05）
std::once_flag g_reg;   // 内嵌模型只注册一次（多实例并发创建安全）
const float kZero[384] = {0};
}  // namespace

struct Aec48 {
    ac::Chain chain; int sr = 48000, H = 384, prefill = 0, lat = 0;
    std::vector<float> inM, inR, blkO, blkP; int fill = 0; Ring outQ, outP;   // outP：回调前的输出，与 outQ 同步推、同步取
    std::atomic<int> enabled{1};
    std::atomic<int> talkHighpassOn{1};
    std::atomic<int> requestedProfile{AEC48_PROFILE_TALK}, activeProfile{AEC48_PROFILE_TALK};
    std::atomic<int> talkGateOn{1}; std::atomic<uint32_t> talkThresholdBits{0};
    std::vector<float> tmpO, tmpP;   // 取输出的中转（H 个，create 时分配）
#ifdef AEC48_TESTING
    float* testPreCapture = nullptr;   // 仅自检：当前 process 调用的原始回调前 float 输出
#endif
    ac::SpecHook hook = nullptr; aec48_spec_hook2 hook2 = nullptr; void* hookUser = nullptr; std::atomic<int> hookOn{1}; std::atomic<int> inProc{0};
    int preDrain = 0;   // 摘掉回调后还要接着维护「回调前」那路的块数（重叠相加里残留着被回调改过的帧，4 块后两路才相等）
    // 统计
    std::atomic<double> avgMs{0}, maxMs{0}; std::atomic<long> samples{0}, outPadded{0}; double accMs = 0, accMax = 0; long accN = 0; std::chrono::steady_clock::time_point lastStat;
    // 录音（dump）
    std::mutex dmu; std::atomic<bool> dumping{false}, stopReq{false}; std::atomic<int> busy{0}; std::thread dth; std::condition_variable dcv; std::mutex dcvm;
    Ring dq[4]; Wav wav[4]; int nTr = 3; bool hookAtStart = false; int hookOnAtStart = 1;   // 第 4 路 = out_prehook（录音开始时挂着回调才写）
    std::string ddir; std::atomic<long> dumpPos{0}, dumpDrop{0}; size_t dqCap = 1 << 19; std::vector<float> dbuf; std::atomic<bool> thErr{false};
    Drop drops[kMaxDrops]; int nDrops = 0; long dropSamples = 0, ovFrom = -1, ovCalls = 0, ovSamples = 0;   // 只由 process 线程写；dump_stop 在 busy 归零后读
    // 开关事件（0.5，Codex 对抗审查）：开关函数只写原子量；处理线程在块边界看到变化才记一条（预分配、不加锁、不分配），位置 = 生效那一块在录音时间轴上的末尾
    Ev evs[kMaxEv]; int nEv = 0; long evOverflow = 0; int evLastEn = 1, evLastHo = 1;   // 只由 process 线程写；dump_stop 在 busy 归零后读
    std::chrono::system_clock::time_point dumpWall; int enabledAtStart = 1; std::atomic<bool> lastWriteErr{false};
};

static void noteEv(Aec48* a, long pos, int kind, int val) { if (a->nEv < kMaxEv) a->evs[a->nEv++] = {pos, (unsigned char)kind, (unsigned char)val}; else a->evOverflow++; }

// evPos ≥ 0：正在录音，本块输入在录音时间轴上结束于 evPos（用来给开关事件定位）
static void block(Aec48* a, const float* m, const float* r, long evPos) {
    auto t0 = std::chrono::steady_clock::now();
    const int en = a->enabled.load(std::memory_order_relaxed), ho = a->hookOn.load(std::memory_order_relaxed);
    if (evPos >= 0) { if (en != a->evLastEn) { noteEv(a, evPos, 0, en); a->evLastEn = en; } if (ho != a->evLastHo) { noteEv(a, evPos, 1, ho); a->evLastHo = ho; } }
    // 讲话版始终取出独立的回调前路，避免重挂时输出队列带入门后 IIR 尾迹。
    // 其他版本仍在挂回调和 OLA 排空期间取出该路。
    const bool pre = a->chain.cnet.hasTalkHead || a->hook || a->hook2 || a->preDrain > 0; if (!a->hook && !a->hook2 && a->preDrain > 0) --a->preDrain;
    const uint32_t thresholdBits = a->talkThresholdBits.load(std::memory_order_relaxed); float threshold; memcpy(&threshold, &thresholdBits, sizeof(threshold));
#ifdef AEC48_DUAL
    const int profile = a->requestedProfile.load(std::memory_order_relaxed);
    a->activeProfile.store(profile, std::memory_order_relaxed);
#else
    const int profile = a->activeProfile.load(std::memory_order_relaxed);
#endif
    a->chain.push(m, r, a->blkO.data(), en ? 2 : 0, a->hook, a->hookUser, ho != 0, pre ? a->blkP.data() : nullptr, a->hook2, a->hookUser,
                  a->talkGateOn.load(std::memory_order_relaxed) != 0, threshold, profile,
                  a->talkHighpassOn.load(std::memory_order_relaxed) != 0);
    if (!pre) memcpy(a->blkP.data(), a->blkO.data(), a->H * 4);
    a->outQ.push(a->blkO.data(), a->H);   // outQ 最多积压 prefill + H，远小于容量
    a->outP.push(a->blkP.data(), a->H);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    a->accMs += ms; a->accN++; if (ms > a->accMax) a->accMax = ms;
    if (t0 - a->lastStat > std::chrono::seconds(1)) { a->avgMs = a->accMs / a->accN; a->maxMs = a->accMax; a->accMs = 0; a->accN = 0; a->accMax = 0; a->lastStat = t0; }
}

// 输入取样 / 输出写出：float 与 int16 两种格式共用 run() 一个循环
struct InF { const float *m, *r; void get(int i, int c, float* dm, float* dr) const { memcpy(dm, m + i, c * 4); memcpy(dr, r + i, c * 4); } };
struct InS { const short *m, *r; void get(int i, int c, float* dm, float* dr) const { for (int k = 0; k < c; ++k) { dm[k] = m[i + k] / 32768.f; dr[k] = r[i + k] / 32768.f; } } };
struct OutF { float* o; void put(int i, const float* x, int c) const { memcpy(o + i, x, c * 4); } void zero(int i, int c) const { memset(o + i, 0, c * 4); } };
struct OutS { short* o; void put(int i, const float* x, int c) const { for (int k = 0; k < c; ++k) o[i + k] = toS16(x[k]); } void zero(int i, int c) const { memset(o + i, 0, c * 2); } };

static void noteDrop(Aec48* a, long pos, long n) {   // 相邻丢包合并；表满后只记溢出
    a->dropSamples += n;
    if (a->nDrops > 0 && a->drops[a->nDrops - 1].pos + a->drops[a->nDrops - 1].n == pos) { a->drops[a->nDrops - 1].n += n; return; }
    if (a->ovFrom < 0 && a->nDrops < kMaxDrops) { a->drops[a->nDrops++] = {pos, n}; return; }
    if (a->ovFrom < 0) a->ovFrom = pos;
    a->ovCalls++; a->ovSamples += n;
}

template <class In, class Out> static int run(Aec48* a, const In& in, const Out& out, int n) {
    const int H = a->H;
    // 录音：本次调用整包决定收或丢（三路剩余空间都够 n 才收；空间只会被消费方变大，所以之后的写入一定成功）
    // 录音中：整次调用都占着 busy（丢包的调用也要给开关事件定位，dump_stop 等它结束再读事件表）
    bool rec = false, hold = false; long base = -1;
    if (a->dumping.load(std::memory_order_acquire)) {
        a->busy.fetch_add(1, std::memory_order_acq_rel);
        if (a->dumping.load(std::memory_order_acquire)) {
            hold = true; base = a->dumpPos.load();
            bool sp = true; for (int t = 0; t < a->nTr; ++t) sp = sp && a->dq[t].space() >= (size_t)n;
            if (sp) rec = true;
            else { noteDrop(a, base, n); a->dumpDrop.fetch_add(1); }
            a->dumpPos.fetch_add(n);
        } else a->busy.fetch_sub(1, std::memory_order_acq_rel);
    }
    int consumed = 0, produced = 0;
    while (consumed < n) {
        const int c = std::min(H - a->fill, n - consumed);
        in.get(consumed, c, &a->inM[a->fill], &a->inR[a->fill]);   // 先读输入（out 可能与 mic 同址，之后会被覆盖）
        if (rec) { a->dq[0].push(&a->inM[a->fill], c); a->dq[1].push(&a->inR[a->fill], c); }
        a->fill += c; consumed += c;
        if (a->fill == H) { block(a, a->inM.data(), a->inR.data(), hold ? base + consumed : -1); a->fill = 0; }
        // 取输出：最多取到已读完的输入位置，保证同址时不覆盖还没读的 mic
        for (;;) { const int want = std::min(consumed - produced, H); if (want <= 0) break;
            const int got = (int)a->outQ.pop(a->tmpO.data(), want); if (!got) break;
            a->outP.pop(a->tmpP.data(), got);   // 与 outQ 同步：两边推的量始终相同
#ifdef AEC48_TESTING
            if (a->testPreCapture) memcpy(a->testPreCapture + produced, a->tmpP.data(), got * sizeof(float));
#endif
            out.put(produced, a->tmpO.data(), got); if (rec) { a->dq[2].push(a->tmpO.data(), got); if (a->nTr == 4) a->dq[3].push(a->tmpP.data(), got); } produced += got; }
    }
    if (produced < n) {   // 只在 frames_per_call 声明为 H 的整数倍、实际却传了别的长度时发生：补零、返回 1 并计数
        out.zero(produced, n - produced);
#ifdef AEC48_TESTING
        if (a->testPreCapture) memset(a->testPreCapture + produced, 0, (n - produced) * sizeof(float));
#endif
        if (rec) for (int k = produced; k < n; k += 384) for (int t = 2; t < a->nTr; ++t) a->dq[t].push(kZero, std::min(384, n - k));
        a->outPadded.fetch_add(n - produced);
    }
    if (hold) { a->busy.fetch_sub(1, std::memory_order_acq_rel); if (rec) a->dcv.notify_one(); }
    a->samples.fetch_add(n, std::memory_order_relaxed);
    return produced < n ? 1 : 0;
}

extern "C" {
const char* aec48_version(void) { static std::string v = std::string("aec48 0.10.1 (") + aec48_model_id + ")"; return v.c_str(); }

Aec48* aec48_create(int sample_rate, int frames_per_call) {
    if (sample_rate != 48000 && sample_rate != 44100) return nullptr;
    Aec48* a = nullptr;
    try {
        std::call_once(g_reg, [] {
            fb::fb_register_model("lin", aec48_lin_model, aec48_lin_model_size);
#ifdef AEC48_DUAL
            fb::fb_register_model("sing_mask", aec48_sing_mask_model, aec48_sing_mask_model_size);
            fb::fb_register_model("talk_mask", aec48_talk_mask_model, aec48_talk_mask_model_size);
#else
            fb::fb_register_model("mask", aec48_mask_model, aec48_mask_model_size);
#endif
        });
        a = new Aec48(); a->sr = sample_rate;
        { const float th = -3.0f; uint32_t bits; memcpy(&bits, &th, sizeof(bits)); a->talkThresholdBits.store(bits, std::memory_order_relaxed); }
#ifdef AEC48_DUAL
        if (!a->chain.load("mem:lin", "mem:talk_mask", sample_rate, "mem:sing_mask")) { delete a; return nullptr; }
#else
        if (!a->chain.load("mem:lin", "mem:mask", sample_rate)) { delete a; return nullptr; }
        const int ownProfile = a->chain.cnet.hasTalkHead ? AEC48_PROFILE_TALK : AEC48_PROFILE_SING;
        a->requestedProfile.store(ownProfile, std::memory_order_relaxed);
        a->activeProfile.store(ownProfile, std::memory_order_relaxed);
#endif
        a->H = a->chain.H; a->inM.assign(a->H, 0.f); a->inR.assign(a->H, 0.f); a->blkO.assign(a->H, 0.f); a->outQ.init(1 << 13); a->outP.init(1 << 13); a->blkP.assign(a->H, 0.f);
        // 块长是 H 的整数倍时输入凑满一块立刻出一块，输出不会欠；否则先垫 H 个零，保证任何块长都不欠（延迟多一块）
        a->prefill = (frames_per_call > 0 && frames_per_call % a->H == 0) ? 0 : a->H;
        if (a->prefill) { a->outQ.push(kZero, a->prefill); a->outP.push(kZero, a->prefill); }
        a->lat = a->chain.latency() + a->prefill; a->lastStat = std::chrono::steady_clock::now();
        a->tmpO.assign(a->H, 0.f); a->tmpP.assign(a->H, 0.f);
        return a;
    } catch (...) { delete a; return nullptr; }   // 内存不足 / 模型解析失败：返回 NULL，不退出宿主进程
}

int aec48_latency_samples(const Aec48* a) { return a ? a->lat : 0; }

void aec48_set_enabled(Aec48* a, int on) { if (a) a->enabled.store(on ? 1 : 0, std::memory_order_relaxed); }   // 只写原子量：可在音频线程调（录音事件由处理线程在生效的那一块记）

int aec48_set_profile(Aec48* a, int profile) {
    if (!a || (profile != AEC48_PROFILE_SING && profile != AEC48_PROFILE_TALK)) return -1;
#ifndef AEC48_DUAL
    if (profile != a->activeProfile.load(std::memory_order_relaxed)) return -1;
#endif
    a->requestedProfile.store(profile, std::memory_order_relaxed);
    return 0;
}

int aec48_get_profile(const Aec48* a) { return a ? a->activeProfile.load(std::memory_order_relaxed) : -1; }

int aec48_set_talk_gate(Aec48* a, int on, float threshold_logit) {
    uint32_t bits; memcpy(&bits, &threshold_logit, sizeof(bits));
    if (!a || !a->chain.cnet.hasTalkHead || (bits & 0x7f800000u) == 0x7f800000u) return -1;
    a->talkThresholdBits.store(bits, std::memory_order_relaxed);
    a->talkGateOn.store(on ? 1 : 0, std::memory_order_relaxed);
    return 0;
}

int aec48_set_talk_highpass(Aec48* a, int on) {
    if (!a || !a->chain.cnet.hasTalkHead) return -1;
    a->talkHighpassOn.store(on ? 1 : 0, std::memory_order_relaxed);
    return 0;
}

int aec48_process(Aec48* a, const float* mic, const float* ref, float* out, int n) {
    if (!a || !mic || !ref || !out || n <= 0) return -1;
    a->inProc.fetch_add(1, std::memory_order_acq_rel); int r = run(a, InF{mic, ref}, OutF{out}, n); a->inProc.fetch_sub(1, std::memory_order_acq_rel); return r;
}

int aec48_process_s16(Aec48* a, const short* mic, const short* ref, short* out, int n) {
    if (!a || !mic || !ref || !out || n <= 0) return -1;
    a->inProc.fetch_add(1, std::memory_order_acq_rel); int r = run(a, InS{mic, ref}, OutS{out}, n); a->inProc.fetch_sub(1, std::memory_order_acq_rel); return r;
}

int aec48_dump_start(Aec48* a, const char* dir) {
    if (!a || !dir) return -1; std::lock_guard<std::mutex> lk(a->dmu); if (a->dumping.load()) return 0;
    try {
        // 录音用到的缓冲全在这里（调用线程）分配：失败由下面的 catch 返回 -3，写盘线程里不再分配（R-02）。第一次录音时才分配（约 6 MB），不录音不占
        for (auto& q : a->dq) if (q.b.size() != a->dqCap) q.init(a->dqCap);
        for (auto& q : a->dq) q.r.store(q.w.load());
        if (a->dbuf.empty()) a->dbuf.assign(1 << 15, 0.f);
        for (auto& w : a->wav) if (w.buf.empty()) w.buf.assign(1 << 15, 0);
        a->hookAtStart = a->hook != nullptr || a->hook2 != nullptr; a->hookOnAtStart = a->hookOn.load(); a->evLastHo = a->hookOnAtStart; a->nTr = a->hookAtStart ? 4 : 3;
        // 同一目录上次挂着回调录过：删掉旧的回调前音轨，免得被当成本次的（Codex 审查 0.5）；删不掉（不是「本来就没有」）按文件打不开处理
        if (!a->hookAtStart && ::unlink((std::string(dir) + "/out_prehook.wav").c_str()) != 0 && errno != ENOENT) return -2;
        a->ddir = dir; static const char* nm[4] = {"mic", "ref", "out", "out_prehook"};
        for (int k = 0; k < a->nTr; ++k) if (!a->wav[k].open(a->ddir + "/" + nm[k] + ".wav", a->sr)) { for (int j = 0; j < k; ++j) a->wav[j].close(); return -2; }
        a->nEv = 0; a->evOverflow = 0;
        a->dumpWall = std::chrono::system_clock::now(); a->dumpPos = 0; a->dumpDrop = 0; a->nDrops = 0; a->dropSamples = 0; a->ovFrom = -1; a->ovCalls = 0; a->ovSamples = 0; a->thErr = false; a->enabledAtStart = a->enabled.load(); a->evLastEn = a->enabledAtStart; a->stopReq = false; a->lastWriteErr = false;
        a->dth = std::thread([a] {
            try {   // 写盘线程不分配内存；万一抛异常也只记错误、不让异常逃出线程（否则 std::terminate 结束宿主）
                float* b = a->dbuf.data(); const size_t bs = a->dbuf.size();
                for (;;) {
                    const bool stop = a->stopReq.load(std::memory_order_acquire); size_t any = 0;
                    for (int k = 0; k < a->nTr; ++k) { size_t g = a->dq[k].pop(b, bs); if (g) { a->wav[k].write(b, g); any += g; } }
                    if (stop && !any) break;   // stopReq 只在没有进行中的写入后置位，此时三路剩余量相同，排空即对齐
                    if (!any) { std::unique_lock<std::mutex> l(a->dcvm); a->dcv.wait_for(l, std::chrono::milliseconds(50)); }
                }
            } catch (...) { a->thErr = true; } });
        a->dumping.store(true, std::memory_order_release);
        return 0;
    } catch (...) { for (auto& w : a->wav) w.close(); return -3; }   // 内存 / 线程创建失败
}

void aec48_dump_stop(Aec48* a) {
    if (!a) return; std::lock_guard<std::mutex> lk(a->dmu); if (!a->dumping.load()) return;
    a->dumping.store(false, std::memory_order_release);
    while (a->busy.load(std::memory_order_acquire) > 0) std::this_thread::yield();   // 等进行中的那次 process 把三路写完
    a->stopReq.store(true, std::memory_order_release); a->dcv.notify_one(); if (a->dth.joinable()) a->dth.join();
    bool werr = a->thErr.load(); for (int k = 0; k < a->nTr; ++k) { a->wav[k].close(); werr = werr || a->wav[k].err; }
    // 说明文件：分析时对齐用（out 比 mic 晚 latency_samples；enabled 开关切换的位置；整包丢弃的位置）
    FILE* f = fopen((a->ddir + "/meta.json").c_str(), "wb");
    if (f) { long long t = std::chrono::duration_cast<std::chrono::milliseconds>(a->dumpWall.time_since_epoch()).count();
        fprintf(f, "{\"version\":\"%s\",\"sample_rate\":%d,\"latency_samples\":%d,\"samples\":%ld,\"samples_mic_ref_out\":[%ld,%ld,%ld],\"start_unix_ms\":%lld,\"write_error\":%d,\"enabled_at_start\":%d,",
                aec48_version(), a->sr, a->lat, a->wav[0].n, a->wav[0].n, a->wav[1].n, a->wav[2].n, t, werr ? 1 : 0, a->enabledAtStart);
        // 频谱回调：录音开始时挂没挂、开没开；挂着时 out_prehook.wav = 回调前的输出（与 out 同一时间轴）
        if (a->hookAtStart) fprintf(f, "\"spec_hook\":{\"attached_at_start\":1,\"contract_version\":%d,\"enabled_at_start\":%d,\"samples_out_prehook\":%ld},", a->hook2 ? 2 : 1, a->hookOnAtStart, a->wav[3].n);
        else fprintf(f, "\"spec_hook\":{\"attached_at_start\":0,\"contract_version\":0},");
        fprintf(f, "\"events\":[");
        // pos = 开关生效的那一块输入在录音时间轴上的末尾（时间轴含被丢弃的段，与 dropped 的 pos 同一条）。
        // 这一块算出的帧当场参与输出合成、且覆盖最近 4 块，所以输出从 out_from = pos + latency − 4H 开始变化，到 out_full = pos + latency − H 起完全是新状态（Codex 复审 0.5.1）
        const long L4 = (long)a->lat - 4L * a->H, L1 = (long)a->lat - a->H;
        for (int i = 0; i < a->nEv; ++i) fprintf(f, "%s{\"pos\":%ld,\"%s\":%d,\"out_from\":%ld,\"out_full\":%ld}", i ? "," : "", a->evs[i].pos, a->evs[i].kind ? "spec_hook_enabled" : "enabled", a->evs[i].val, a->evs[i].pos + L4, a->evs[i].pos + L1);
        // 整包丢弃（写盘跟不上）：pos = 录音时间轴上的样本位置，n = 丢的样本数（三路同时丢，文件里直接缺这一段；相邻丢包已合并成一段）。
        // 段数超过 1024 时 dropped_overflow 给出第一个没记下的位置与之后的合计：from_pos 之前的时间轴可完整恢复，之后不能逐段恢复
        fprintf(f, "],\"events_overflow\":%ld,\"dropped\":[", a->evOverflow);
        for (int i = 0; i < a->nDrops; ++i) fprintf(f, "%s{\"pos\":%ld,\"n\":%ld}", i ? "," : "", a->drops[i].pos, a->drops[i].n);
        fprintf(f, "],\"dropped_calls\":%ld,\"dropped_samples\":%ld,\"dropped_overflow\":", a->dumpDrop.load(), a->dropSamples);
        if (a->ovFrom < 0) fprintf(f, "null}\n"); else fprintf(f, "{\"from_pos\":%ld,\"calls\":%ld,\"samples\":%ld}}\n", a->ovFrom, a->ovCalls, a->ovSamples);
        if (ferror(f)) werr = true;
        if (fclose(f) != 0) werr = true; } else werr = true;
    a->lastWriteErr = werr;
}

int aec48_dump_status(const Aec48* a) { return !a ? -1 : (a->lastWriteErr.load() ? -4 : 0); }

void aec48_stats(const Aec48* a, double o[4]) { if (!a || !o) return; o[0] = a->avgMs; o[1] = a->maxMs; o[2] = (double)a->samples.load(); o[3] = (double)a->dumpDrop.load(); }

int aec48_set_spec_hook(Aec48* a, aec48_spec_hook fn, void* user) {
    if (!a) return -1;
    if (a->inProc.load(std::memory_order_acquire) > 0) return -2;   // 正在 process：拒绝（尽力检测，约定上调用方保证不并发）
    std::lock_guard<std::mutex> lk(a->dmu); if (a->dumping.load()) return -3;   // 录音中不换挂钩：out_prehook 的有无在录音开始时定
    if (fn && !a->hook && !a->hook2 && a->preDrain == 0) a->chain.syncPreAcc();   // 已 ≥4 块没挂回调：两路的重叠相加状态本来就相等
    if (!fn && (a->hook || a->hook2)) a->preDrain = 4;
    a->hook = fn; a->hook2 = nullptr; a->hookUser = fn ? user : nullptr; return 0;
}

int aec48_set_spec_hook2(Aec48* a, aec48_spec_hook2 fn, void* user) {
    if (!a) return -1;
    if (a->inProc.load(std::memory_order_acquire) > 0) return -2;
    std::lock_guard<std::mutex> lk(a->dmu); if (a->dumping.load()) return -3;
    if (fn) { a->chain.resetSide(); if (!a->hook && !a->hook2 && a->preDrain == 0) a->chain.syncPreAcc(); }
    if (!fn && (a->hook || a->hook2)) a->preDrain = 4;
    a->hook2 = fn; a->hook = nullptr; a->hookUser = fn ? user : nullptr; return 0;
}

void aec48_set_spec_hook_enabled(Aec48* a, int on) { if (a) a->hookOn.store(on ? 1 : 0, std::memory_order_relaxed); }   // 同 set_enabled：只写原子量

void aec48_spec_format(const Aec48* a, int* sample_rate, int* nfft, int* hop, int* window_id) {
    if (sample_rate) *sample_rate = a ? a->sr : 0;
    if (nfft) *nfft = a ? a->chain.N : 0;
    if (hop) *hop = a ? a->chain.H : 0;
    if (window_id) *window_id = a ? AEC48_WINDOW_SQRT_HANN_PERIODIC : 0;
}

void aec48_destroy(Aec48* a) { if (!a) return; aec48_dump_stop(a); delete a; }

// 仅供仓库自带的健壮性测试用（不在头文件里）：改下一次 dump_start 的录音队列容量（2 的幂），用来稳定复现"丢包超过上限"。
__attribute__((visibility("default"))) void aec48_internal_set_dump_capacity(Aec48* a, unsigned long pow2) { if (a && pow2 >= 1024 && !(pow2 & (pow2 - 1))) a->dqCap = pow2; }
#ifdef AEC48_TESTING
// 仅供自检取出一次 process 的原始回调前样本；调用者保证缓冲区长度足够。
__attribute__((visibility("default"))) void aec48_internal_set_prehook_capture(Aec48* a, float* output) {
    if (a) a->testPreCapture = output;
}
// 仅供讲话版逐帧对账：829 维 talk 输入、logit，以及 769 个复数频点的交错复谱数组。
// 每个复谱数组长度为 1538 float；无完整 talk 帧时返回 0。
__attribute__((visibility("default"))) int aec48_internal_talk_snapshot(Aec48* a, float* talkInput829, float* logit1,
        float* se, float* sy, float* sx, float* e, float* d) {
    if (!a || !talkInput829 || !logit1 || !se || !sy || !sx || !e || !d || !a->chain.cnet.hasTalkHead || !a->chain.talkFrameReady) return 0;
    const auto& c = a->chain;
    memcpy(talkInput829, c.cnet.talkZ.data(), 829 * sizeof(float)); *logit1 = c.cnet.lastTalk;
    memcpy(se, c.se.data(), 2 * c.F * sizeof(float)); memcpy(sy, c.sy.data(), 2 * c.F * sizeof(float));
    memcpy(sx, c.sx.data(), 2 * c.F * sizeof(float));
    for (int f = 0; f < c.F; ++f) { e[2 * f] = c.Er[f]; e[2 * f + 1] = c.Ei[f]; d[2 * f] = c.Dr[f]; d[2 * f + 1] = c.Di[f]; }
    return c.F;
}
// 仅供仓库内测试：返回最近一帧按掩码归一尺度估计的提示功率、换算后的未缩放功率与 g²。
__attribute__((visibility("default"))) int aec48_internal_hook2_hint_values(Aec48* a, double* normalized, double* unscaled, double* g2) {
    if (!a || !normalized || !unscaled || !g2 || a->chain.F <= 0) return 0;
    *g2 = (double)(a->chain.sideLastG * a->chain.sideLastG);
    for (int f = 0; f < a->chain.F; ++f) { normalized[f] = a->chain.sidePowerNorm[f]; unscaled[f] = a->chain.sideRhat[f]; }
    return a->chain.F;
}
// 仅供仓库内默认版测试：核对递推器实际收到的带内功率与处理链本帧 g² 输入。
__attribute__((visibility("default"))) int aec48_internal_hook2_scaled_inputs(Aec48* a) {
    if (!a || a->chain.sideNetHint) return -1;
    const auto& c = a->chain; if (c.sideHint.hNB > 48) return -1;
    float pe[48] = {}, px[48] = {}; const float g2 = c.sideLastG * c.sideLastG;
    for (int f = 0; f < c.F; ++f) { const int b = c.sideHint.hb[f]; pe[b] += (c.Er[f] * c.Er[f] + c.Ei[f] * c.Ei[f]) * g2; px[b] += (c.XAr[f] * c.XAr[f] + c.XAi[f] * c.XAi[f]) * g2; }
    int n = 0; for (int b = 0; b < c.sideHint.hNB; ++b) { n += memcmp(&pe[b], &c.sideHint.hPe[b], sizeof(float)) != 0; n += memcmp(&px[b], &c.sideHint.hPx[b], sizeof(float)) != 0; }
    return n;
}
// 仅供仓库内候选版测试：逐 float 位比较实际保存的归一提示功率与 Cofi hm²。
__attribute__((visibility("default"))) int aec48_internal_hook2_hint_exact(Aec48* a) {
    if (!a || !a->chain.sideNetHint) return -1;
    int n = 0; for (int f = 0; f < a->chain.F; ++f) { const float p = a->chain.cnet.hm[f] * a->chain.cnet.hm[f]; n += memcmp(&a->chain.sidePowerNorm[f], &p, sizeof(float)) != 0; }
    return n;
}
__attribute__((visibility("default"))) int aec48_internal_hook2_hint_error(Aec48* a, double out2[2]) {
    if (!a || !out2 || !a->chain.sideNetHint) return 0;
    double ma = 0, mp = 0;
    const float g2f = a->chain.sideLastG * a->chain.sideLastG;
    for (int f = 0; f < a->chain.F; ++f) {
        const double p = (double)(a->chain.sideRhat[f] * g2f);
        const double q = (double)a->chain.sidePowerNorm[f];
        ma = std::max(ma, std::fabs(p - q)); mp = std::max(mp, std::max(p, q));
    }
    out2[0] = ma; out2[1] = mp > 0 ? ma / mp : 0; return 1;
}
#endif
}
