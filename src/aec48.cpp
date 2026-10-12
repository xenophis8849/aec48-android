// aec48 C 接口实现。整链 = 新线性级（网络估噪声的对角卡尔曼 + 在线时延估计，双滤波）→ CoFi-Lite 掩码，两级共用一套 STFT（1536 / 384）。
// 模型编进库里（models.cpp，tools/embed_models.py 生成），从内存加载。
// process / process_s16：任意长度都在内部按 384 样本块处理、边处理边取输出（支持 out 与 mic 同址）；处理期不分配内存、不加锁、不做文件读写。
//   int16 与 float 走同一个循环（0.3：不再按 4096 内部分段——分段会在声明块长为 384 倍数时插零，见审查复查 R-01）。
// 录音：mic / ref / out 三路按"一次 process 调用"整包写入（三路要么都收、要么都丢，丢的位置记进 meta.json）；写盘在后台线程，
//   直接用文件描述符写，只计真正落盘的字节（写盘出错时 WAV 头、meta 与文件实际内容一致，R-03）。
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
#include <new>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

extern "C" { extern const unsigned char aec48_lin_model[]; extern const unsigned long aec48_lin_model_size;
             extern const unsigned char aec48_mask_model[]; extern const unsigned long aec48_mask_model_size; extern const char aec48_model_id[]; }

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
const int kMaxDrops = 1024;   // 丢包位置表：相邻的丢包合并成一段；段数超过上限后只记溢出起点与合计（meta 里 dropped_overflow，R-05）
std::once_flag g_reg;   // 内嵌模型只注册一次（多实例并发创建安全）
const float kZero[384] = {0};
}  // namespace

struct Aec48 {
    ac::Chain chain; int sr = 48000, H = 384, prefill = 0, lat = 0;
    std::vector<float> inM, inR, blkO; int fill = 0; Ring outQ;
    std::atomic<int> enabled{1};
    std::vector<float> tmpO;         // 取输出的中转（H 个，create 时分配）
    // 统计
    std::atomic<double> avgMs{0}, maxMs{0}; std::atomic<long> samples{0}, outPadded{0}; double accMs = 0, accMax = 0; long accN = 0; std::chrono::steady_clock::time_point lastStat;
    // 录音（dump）
    std::mutex dmu; std::atomic<bool> dumping{false}, stopReq{false}; std::atomic<int> busy{0}; std::thread dth; std::condition_variable dcv; std::mutex dcvm;
    Ring dq[3]; Wav wav[3]; std::string ddir; std::atomic<long> dumpPos{0}, dumpDrop{0}; size_t dqCap = 1 << 19; std::vector<float> dbuf; std::atomic<bool> thErr{false};
    Drop drops[kMaxDrops]; int nDrops = 0; long dropSamples = 0, ovFrom = -1, ovCalls = 0, ovSamples = 0;   // 只由 process 线程写；dump_stop 在 busy 归零后读
    std::vector<std::string> events; std::mutex emu; std::chrono::system_clock::time_point dumpWall; int enabledAtStart = 1; std::atomic<bool> lastWriteErr{false};
};

static void block(Aec48* a, const float* m, const float* r) {
    auto t0 = std::chrono::steady_clock::now();
    a->chain.push(m, r, a->blkO.data(), a->enabled.load(std::memory_order_relaxed) ? 2 : 0);
    a->outQ.push(a->blkO.data(), a->H);   // outQ 最多积压 prefill + H，远小于容量
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
    bool rec = false;
    if (a->dumping.load(std::memory_order_acquire)) {
        a->busy.fetch_add(1, std::memory_order_acq_rel);
        if (a->dumping.load(std::memory_order_acquire)) {
            if (a->dq[0].space() >= (size_t)n && a->dq[1].space() >= (size_t)n && a->dq[2].space() >= (size_t)n) rec = true;
            else { noteDrop(a, a->dumpPos.load(), n); a->dumpDrop.fetch_add(1); }
            a->dumpPos.fetch_add(n);
        }
        if (!rec) a->busy.fetch_sub(1, std::memory_order_acq_rel);
    }
    int consumed = 0, produced = 0;
    while (consumed < n) {
        const int c = std::min(H - a->fill, n - consumed);
        in.get(consumed, c, &a->inM[a->fill], &a->inR[a->fill]);   // 先读输入（out 可能与 mic 同址，之后会被覆盖）
        if (rec) { a->dq[0].push(&a->inM[a->fill], c); a->dq[1].push(&a->inR[a->fill], c); }
        a->fill += c; consumed += c;
        if (a->fill == H) { block(a, a->inM.data(), a->inR.data()); a->fill = 0; }
        // 取输出：最多取到已读完的输入位置，保证同址时不覆盖还没读的 mic
        for (;;) { const int want = std::min(consumed - produced, H); if (want <= 0) break;
            const int got = (int)a->outQ.pop(a->tmpO.data(), want); if (!got) break;
            out.put(produced, a->tmpO.data(), got); if (rec) a->dq[2].push(a->tmpO.data(), got); produced += got; }
    }
    if (produced < n) {   // 只在 frames_per_call 声明为 H 的整数倍、实际却传了别的长度时发生：补零、返回 1 并计数
        out.zero(produced, n - produced);
        if (rec) for (int k = produced; k < n; k += 384) a->dq[2].push(kZero, std::min(384, n - k));
        a->outPadded.fetch_add(n - produced);
    }
    if (rec) { a->busy.fetch_sub(1, std::memory_order_acq_rel); a->dcv.notify_one(); }
    a->samples.fetch_add(n, std::memory_order_relaxed);
    return produced < n ? 1 : 0;
}

extern "C" {
const char* aec48_version(void) { static std::string v = std::string("aec48 0.4.3 (") + aec48_model_id + ")"; return v.c_str(); }

Aec48* aec48_create(int sample_rate, int frames_per_call) {
    if (sample_rate != 48000 && sample_rate != 44100) return nullptr;
    Aec48* a = nullptr;
    try {
        std::call_once(g_reg, [] { fb::fb_register_model("lin", aec48_lin_model, aec48_lin_model_size); fb::fb_register_model("mask", aec48_mask_model, aec48_mask_model_size); });
        a = new Aec48(); a->sr = sample_rate;
        if (!a->chain.load("mem:lin", "mem:mask", sample_rate)) { delete a; return nullptr; }
        a->H = a->chain.H; a->inM.assign(a->H, 0.f); a->inR.assign(a->H, 0.f); a->blkO.assign(a->H, 0.f); a->outQ.init(1 << 13);
        // 块长是 H 的整数倍时输入凑满一块立刻出一块，输出不会欠；否则先垫 H 个零，保证任何块长都不欠（延迟多一块）
        a->prefill = (frames_per_call > 0 && frames_per_call % a->H == 0) ? 0 : a->H;
        if (a->prefill) a->outQ.push(kZero, a->prefill);
        a->lat = a->chain.latency() + a->prefill; a->lastStat = std::chrono::steady_clock::now();
        a->tmpO.assign(a->H, 0.f);
        return a;
    } catch (...) { delete a; return nullptr; }   // 内存不足 / 模型解析失败：返回 NULL，不退出宿主进程
}

int aec48_latency_samples(const Aec48* a) { return a ? a->lat : 0; }

void aec48_set_enabled(Aec48* a, int on) {
    if (!a) return; a->enabled = on ? 1 : 0;
    if (a->dumping.load()) { std::lock_guard<std::mutex> lk(a->emu); char b[96]; snprintf(b, sizeof b, "{\"pos\":%ld,\"enabled\":%d}", a->dumpPos.load(), on ? 1 : 0); a->events.push_back(b); }
}

int aec48_process(Aec48* a, const float* mic, const float* ref, float* out, int n) {
    if (!a || !mic || !ref || !out || n <= 0) return -1;
    return run(a, InF{mic, ref}, OutF{out}, n);
}

int aec48_process_s16(Aec48* a, const short* mic, const short* ref, short* out, int n) {
    if (!a || !mic || !ref || !out || n <= 0) return -1;
    return run(a, InS{mic, ref}, OutS{out}, n);
}

int aec48_dump_start(Aec48* a, const char* dir) {
    if (!a || !dir) return -1; std::lock_guard<std::mutex> lk(a->dmu); if (a->dumping.load()) return 0;
    try {
        // 录音用到的缓冲全在这里（调用线程）分配：失败由下面的 catch 返回 -3，写盘线程里不再分配（R-02）。第一次录音时才分配（约 6 MB），不录音不占
        for (auto& q : a->dq) if (q.b.size() != a->dqCap) q.init(a->dqCap);
        for (auto& q : a->dq) q.r.store(q.w.load());
        if (a->dbuf.empty()) a->dbuf.assign(1 << 15, 0.f);
        for (auto& w : a->wav) if (w.buf.empty()) w.buf.assign(1 << 15, 0);
        a->ddir = dir; static const char* nm[3] = {"mic", "ref", "out"};
        for (int k = 0; k < 3; ++k) if (!a->wav[k].open(a->ddir + "/" + nm[k] + ".wav", a->sr)) { for (int j = 0; j < k; ++j) a->wav[j].close(); return -2; }
        { std::lock_guard<std::mutex> e(a->emu); a->events.clear(); }
        a->dumpWall = std::chrono::system_clock::now(); a->dumpPos = 0; a->dumpDrop = 0; a->nDrops = 0; a->dropSamples = 0; a->ovFrom = -1; a->ovCalls = 0; a->ovSamples = 0; a->thErr = false; a->enabledAtStart = a->enabled.load(); a->stopReq = false; a->lastWriteErr = false;
        a->dth = std::thread([a] {
            try {   // 写盘线程不分配内存；万一抛异常也只记错误、不让异常逃出线程（否则 std::terminate 结束宿主）
                float* b = a->dbuf.data(); const size_t bs = a->dbuf.size();
                for (;;) {
                    const bool stop = a->stopReq.load(std::memory_order_acquire); size_t any = 0;
                    for (int k = 0; k < 3; ++k) { size_t g = a->dq[k].pop(b, bs); if (g) { a->wav[k].write(b, g); any += g; } }
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
    bool werr = a->thErr.load(); for (auto& w : a->wav) { w.close(); werr = werr || w.err; }
    // 说明文件：分析时对齐用（out 比 mic 晚 latency_samples；enabled 开关切换的位置；整包丢弃的位置）
    FILE* f = fopen((a->ddir + "/meta.json").c_str(), "wb");
    if (f) { long long t = std::chrono::duration_cast<std::chrono::milliseconds>(a->dumpWall.time_since_epoch()).count();
        fprintf(f, "{\"version\":\"%s\",\"sample_rate\":%d,\"latency_samples\":%d,\"samples\":%ld,\"samples_mic_ref_out\":[%ld,%ld,%ld],\"start_unix_ms\":%lld,\"write_error\":%d,\"enabled_at_start\":%d,\"events\":[",
                aec48_version(), a->sr, a->lat, a->wav[0].n, a->wav[0].n, a->wav[1].n, a->wav[2].n, t, werr ? 1 : 0, a->enabledAtStart);
        { std::lock_guard<std::mutex> e(a->emu); for (size_t i = 0; i < a->events.size(); ++i) fprintf(f, "%s%s", i ? "," : "", a->events[i].c_str()); }
        // 整包丢弃（写盘跟不上）：pos = 录音时间轴上的样本位置，n = 丢的样本数（三路同时丢，文件里直接缺这一段；相邻丢包已合并成一段）。
        // 段数超过 1024 时 dropped_overflow 给出第一个没记下的位置与之后的合计：from_pos 之前的时间轴可完整恢复，之后不能逐段恢复
        fprintf(f, "],\"dropped\":[");
        for (int i = 0; i < a->nDrops; ++i) fprintf(f, "%s{\"pos\":%ld,\"n\":%ld}", i ? "," : "", a->drops[i].pos, a->drops[i].n);
        fprintf(f, "],\"dropped_calls\":%ld,\"dropped_samples\":%ld,\"dropped_overflow\":", a->dumpDrop.load(), a->dropSamples);
        if (a->ovFrom < 0) fprintf(f, "null}\n"); else fprintf(f, "{\"from_pos\":%ld,\"calls\":%ld,\"samples\":%ld}}\n", a->ovFrom, a->ovCalls, a->ovSamples);
        if (ferror(f)) werr = true;
        if (fclose(f) != 0) werr = true; } else werr = true;
    a->lastWriteErr = werr;
}

int aec48_dump_status(const Aec48* a) { return !a ? -1 : (a->lastWriteErr.load() ? -4 : 0); }

void aec48_stats(const Aec48* a, double o[4]) { if (!a || !o) return; o[0] = a->avgMs; o[1] = a->maxMs; o[2] = (double)a->samples.load(); o[3] = (double)a->dumpDrop.load(); }

void aec48_destroy(Aec48* a) { if (!a) return; aec48_dump_stop(a); delete a; }

// 仅供仓库自带的健壮性测试用（不在头文件里）：改下一次 dump_start 的录音队列容量（2 的幂），用来稳定复现"丢包超过上限"。
__attribute__((visibility("default"))) void aec48_internal_set_dump_capacity(Aec48* a, unsigned long pow2) { if (a && pow2 >= 1024 && !(pow2 & (pow2 - 1))) a->dqCap = pow2; }
}
