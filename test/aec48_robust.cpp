// 接口健壮性测试（对应 0.1 版审查报告 H-01…M-05、0.2 复查 R-01…R-05 各条的复现方式）。主机上跑：aec48_robust <mic.pcm> <ref.pcm> <临时目录>
// R-06（模型解析中途失败时关流）靠 RAII 保证，这里没有动态注入；上传器（R-04）的测试在 Aec48UploaderTest.java。
// 每项打印"通过 / 不通过"，全部通过返回 0。
#include "aec48.h"
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void aec48_internal_set_dump_capacity(Aec48* a, unsigned long pow2);   // 库内部测试钩子（不在头文件里）

static std::atomic<long> g_news{0}; static std::atomic<bool> g_count{false}; static std::atomic<size_t> g_failSize{0};   // g_failSize：这个大小的分配一律失败（故障注入）
void* operator new(size_t n) { if (g_count.load(std::memory_order_relaxed)) g_news.fetch_add(1); if (n && n == g_failSize.load()) throw std::bad_alloc(); void* p = malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") { printf("%s  %s%s%s\n", ok ? "通过  " : "不通过", what, detail.empty() ? "" : "：", detail.c_str()); if (!ok) ++fails; }
static std::vector<float> rdf(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET); std::vector<short> v(n); if (fread(v.data(), 2, n, f) != (size_t)n) exit(2); fclose(f); std::vector<float> o(n); for (long i = 0; i < n; ++i) o[i] = v[i] / 32768.f; return o; }
static long rss_kb() { FILE* f = fopen("/proc/self/status", "r"); char l[256]; long v = -1; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6); if (f) fclose(f); return v; }
static long wav_frames(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = (ftell(f) - 44) / 2; fclose(f); return n; }
static long wav_hdr(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return -1; unsigned char h[44]; long v = -1; if (fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 36, "data", 4)) { unsigned d; memcpy(&d, h + 40, 4); v = d / 2; } fclose(f); return v; }   // WAV 头里声明的样本数
static long fsize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }
static long jnum(const std::string& s, const std::string& key, size_t from = 0) { size_t i = s.find("\"" + key + "\":", from); return i == std::string::npos ? -999 : atol(s.c_str() + i + key.size() + 3); }
static std::string slurp(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return ""; std::string s; char b[4096]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) s.append(b, k); fclose(f); return s; }
// 用块长 blk 处理整段，返回输出
static std::vector<float> run(const std::vector<float>& m, const std::vector<float>& r, long n, int blk, int fpc, bool on = true, bool inplace = false) {
    Aec48* a = aec48_create(48000, fpc); aec48_set_enabled(a, on); std::vector<float> o(n);
    std::vector<float> buf;
    for (long i = 0; i < n; i += blk) { int c = (int)std::min<long>(blk, n - i);
        if (inplace) { buf.assign(m.begin() + i, m.begin() + i + c); aec48_process(a, buf.data(), &r[i], buf.data(), c); memcpy(&o[i], buf.data(), c * 4); }
        else aec48_process(a, &m[i], &r[i], &o[i], c); }
    aec48_destroy(a); return o;
}
static double maxdiff(const std::vector<float>& a, const std::vector<float>& b) { double d = 0; for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, (double)std::fabs(a[i] - b[i])); return d; }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "用法：%s mic.pcm ref.pcm 临时目录\n", argv[0]); return 2; }
    auto mic = rdf(argv[1]), ref = rdf(argv[2]); std::string tmp = argv[3]; long n = (long)std::min(mic.size(), ref.size()) / 384 * 384;
    printf("%s\n", aec48_version());

    // 前提：本程序替换的 operator new 能拦到库内部的分配（M-01 与下面的故障注入都靠它）
    { g_news = 0; g_count = true; Aec48* a = aec48_create(48000, 384); g_count = false; aec48_destroy(a);
      check(g_news.load() > 0, "前提：测试程序能拦到库内部的内存分配", "创建一次 operator new " + std::to_string(g_news.load()) + " 次"); }

    // H-01：反复创建 / 销毁，内存不随次数线性增长
    { for (int i = 0; i < 20; ++i) aec48_destroy(aec48_create(48000, 384)); long r0 = rss_kb();
      for (int i = 0; i < 600; ++i) aec48_destroy(aec48_create(48000, 384)); long r1 = rss_kb();
      check(r1 - r0 < 2048, "H-01 600 次创建/销毁后常驻内存增长 < 2 MB", std::to_string(r1 - r0) + " KB（0.1 版约 5.5 MB / 200 次）"); }

    // H-02：一次传 65536 个样本 = 分成 ≤384 的小块；关 AEC + 恒定 0.25 输入（审查原复现），以及开 AEC 的真实录音
    { const long L = 65536; std::vector<float> c(L, 0.25f), z(L, 0.f);
      auto big = run(c, z, L, (int)L, 0, false), small = run(c, z, L, 384, 0, false);
      check(maxdiff(big, small) < 1e-6, "H-02 关 AEC：单次 65536 样本 = 分块处理", "最大差 " + std::to_string(maxdiff(big, small)));
      long M = std::min<long>(n, 4 * 65536 / 384 * 384);
      auto b2 = run(mic, ref, M, (int)M, 0), s2 = run(mic, ref, M, 384, 0);
      check(maxdiff(b2, s2) < 1e-6, ("H-02 开 AEC：单次整段（" + std::to_string(M) + " 样本）= 分块处理").c_str(), "最大差 " + std::to_string(maxdiff(b2, s2))); }

    // 原地处理（out = mic）与不同址一致，含奇数块长
    for (int blk : {1, 383, 385, 441, 8192}) { long M = std::min<long>(n, 96000); auto a = run(mic, ref, M, blk, 0), b = run(mic, ref, M, blk, 0, true, true);
      check(maxdiff(a, b) == 0, ("原地处理 块长 " + std::to_string(blk)).c_str()); }

    // H-03：录音中途从另一线程开始 / 停止，三路文件等长；开关事件写进 meta
    { Aec48* a = aec48_create(48000, 0); std::atomic<bool> go{true}; std::vector<float> o(4096);
      std::thread t([&] { long i = 0; while (go) { int c = 1 + (int)(i % 4096); long off = (i * 997) % (n - 4096); aec48_process(a, &mic[off], &ref[off], o.data(), c); ++i; } });
      bool same = true; std::string why;
      for (int k = 0; k < 20; ++k) { std::string d = tmp; int rc = aec48_dump_start(a, d.c_str()); std::this_thread::sleep_for(std::chrono::milliseconds(5 + 7 * (k % 5)));
        if (k % 3 == 0) aec48_set_enabled(a, k % 2); aec48_dump_stop(a);
        long f0 = wav_frames(d + "/mic.wav"), f1 = wav_frames(d + "/ref.wav"), f2 = wav_frames(d + "/out.wav"), h0 = wav_hdr(d + "/mic.wav");
        if (rc != 0 || f0 <= 0 || h0 != f0 || !(f0 == f1 && f1 == f2)) { same = false; why = "rc " + std::to_string(rc) + "，" + std::to_string(f0) + "/" + std::to_string(f1) + "/" + std::to_string(f2) + "，头 " + std::to_string(h0); } }
      go = false; t.join(); aec48_destroy(a);
      check(same, "H-03 并发开始/停止录音 20 次：每次开始成功、有内容、WAV 头与文件一致、三个文件等长", why); }

    // M-01：处理期不分配内存（覆盖时延估计锁定前后）
    { Aec48* a = aec48_create(48000, 384); std::vector<float> o(384); aec48_process(a, &mic[0], &ref[0], o.data(), 384);
      g_news = 0; g_count = true; for (long i = 384; i + 384 <= std::min<long>(n, 384 * 2000); i += 384) aec48_process(a, &mic[i], &ref[i], o.data(), 384); g_count = false;
      check(g_news.load() == 0, "M-01 连续 2000 次 process 期间 operator new 次数为 0", std::to_string(g_news.load())); aec48_destroy(a); }

    // M-02：int16 长度不受限（8192 / 8193 / 20000），与 float 分块一致
    { long M = std::min<long>(n, 20000); std::vector<short> ms(M), rs(M), os(M);
      for (long i = 0; i < M; ++i) { ms[i] = (short)lrintf(mic[i] * 32768.f); rs[i] = (short)lrintf(ref[i] * 32768.f); }
      for (int L : {8192, 8193, 20000}) { if (L > M) continue; Aec48* a = aec48_create(48000, 0); int rc = aec48_process_s16(a, ms.data(), rs.data(), os.data(), L); aec48_destroy(a);
        check(rc == 0, ("M-02 int16 单次 " + std::to_string(L) + " 样本返回 0").c_str(), "rc=" + std::to_string(rc)); } }

    // R-01：int16 低延迟大块（声明块长 = 实际块长 = 384 的整数倍）与 float 384 块逐样本一致（差 ≤ 1 LSB）、返回 0、延迟相同；含原地处理
    { long M = std::min<long>(n, 48000 * 6) / 36864 * 36864; std::vector<short> ms(M), rs(M);   // 36864 = 384、4608、12288 的公倍数
      for (long i = 0; i < M; ++i) { ms[i] = (short)lrintf(mic[i] * 32768.f); rs[i] = (short)lrintf(ref[i] * 32768.f); }
      std::vector<float> mf(M), rf(M); for (long i = 0; i < M; ++i) { mf[i] = ms[i] / 32768.f; rf[i] = rs[i] / 32768.f; }
      auto base = run(mf, rf, M, 384, 384); int latB; { Aec48* b = aec48_create(48000, 384); latB = aec48_latency_samples(b); aec48_destroy(b); }
      for (int L : {384, 4608, 12288}) for (int inplace = 0; inplace < 2; ++inplace) {
        Aec48* a = aec48_create(48000, L); int lat = aec48_latency_samples(a); std::vector<short> o(M); int bad = 0; long worst = 0;
        for (long i = 0; i + L <= M; i += L) { int rc; if (inplace) { memcpy(&o[i], &ms[i], L * 2); rc = aec48_process_s16(a, &o[i], &rs[i], &o[i], L); } else rc = aec48_process_s16(a, &ms[i], &rs[i], &o[i], L); if (rc != 0) ++bad; }
        aec48_destroy(a);
        for (long i = 0; i < M; ++i) { float v = base[i] * 32767.f; long q = (long)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : lrintf(v))); worst = std::max(worst, std::labs(q - o[i])); }
        check(bad == 0 && worst <= 1 && lat == latB, ("R-01 int16 声明并传 " + std::to_string(L) + " 样本" + (inplace ? "（原地）" : "") + "：返回 0、与 float 384 块一致、延迟相同").c_str(),
              "非 0 返回 " + std::to_string(bad) + " 次，最大差 " + std::to_string(worst) + " LSB，延迟 " + std::to_string(lat) + " vs " + std::to_string(latB)); } }

    // 返回 1：声明 384 的整数倍却传了别的长度
    { Aec48* a = aec48_create(48000, 384); std::vector<float> o(500); int rc = aec48_process(a, &mic[0], &ref[0], o.data(), 500); aec48_destroy(a);
      check(rc == 1, "声明块长 384、实际传 500 → 返回 1（输出末尾补零）", "rc=" + std::to_string(rc)); }

    // M-03：写盘失败（文件大小上限）→ dump_status = -4，meta 标 write_error
    { signal(SIGXFSZ, SIG_IGN); rlimit old; getrlimit(RLIMIT_FSIZE, &old); Aec48* a = aec48_create(48000, 384); std::string d = tmp;
      aec48_dump_start(a, d.c_str()); rlimit lim = old; lim.rlim_cur = 60000; setrlimit(RLIMIT_FSIZE, &lim);
      std::vector<float> o(384); for (long i = 0; i + 384 <= std::min<long>(n, 48000 * 3); i += 384) aec48_process(a, &mic[i], &ref[i], o.data(), 384);
      lim.rlim_cur = old.rlim_cur; setrlimit(RLIMIT_FSIZE, &lim); aec48_dump_stop(a); int st = aec48_dump_status(a); aec48_destroy(a);
      std::string meta = slurp(d + "/meta.json");
      check(st == -4 && meta.find("\"write_error\":1") != std::string::npos, "M-03 写盘失败 → dump_status = -4、meta.json write_error = 1", "status=" + std::to_string(st));
      bool ok = true; std::string why; static const char* nm[3] = {"mic", "ref", "out"}; size_t at = meta.find("\"samples_mic_ref_out\":[");
      for (int k = 0; k < 3; ++k) { std::string w = d + "/" + nm[k] + ".wav"; long fs = fsize(w), hd = wav_hdr(w), mt = at == std::string::npos ? -999 : atol(meta.c_str() + (k == 0 ? at + 23 : meta.find(",", k == 1 ? at : meta.find(",", at) + 1) + 1));
        if (!(fs <= 60000 && hd >= 0 && fs == 44 + 2 * hd && mt == hd)) { ok = false; why += std::string(nm[k]) + "：文件 " + std::to_string(fs) + " 字节、头 " + std::to_string(hd) + "、meta " + std::to_string(mt) + "；"; } }
      check(ok, "R-03 写盘失败后：文件长度 = 44 + 2×WAV 头样本数 = 44 + 2×meta 样本数（三路）", why); }

    // R-02：录音缓冲分配失败 → dump_start 返回 -3、不结束进程；之后照常处理、照常能录
    for (size_t sz : {(size_t)(1 << 15) * 4, (size_t)(1 << 15) * 2, (size_t)(1 << 19) * 4}) {
      Aec48* a = aec48_create(48000, 384); std::string d = tmp; std::vector<float> o(384);
      g_failSize = sz; int rc = aec48_dump_start(a, d.c_str()); g_failSize = 0;
      for (long i = 0; i + 384 <= std::min<long>(n, 48000); i += 384) aec48_process(a, &mic[i], &ref[i], o.data(), 384);
      aec48_dump_stop(a); int rc2 = aec48_dump_start(a, d.c_str()); for (long i = 0; i + 384 <= std::min<long>(n, 48000); i += 384) aec48_process(a, &mic[i], &ref[i], o.data(), 384); aec48_dump_stop(a);
      long fr = wav_frames(d + "/mic.wav"); aec48_destroy(a);
      check(rc == -3 && rc2 == 0 && fr > 0, ("R-02 录音缓冲（" + std::to_string(sz) + " 字节）分配失败 → -3，进程不退出，之后能正常录").c_str(), "rc " + std::to_string(rc) + " / " + std::to_string(rc2) + "，再录 " + std::to_string(fr) + " 帧"); }

    // R-05：丢包超过 1024 段：相邻丢包合并、超出部分记 dropped_overflow；文件样本数 + 丢弃样本数 = 送入总数，各段之和 + 溢出 = 丢弃合计
    { Aec48* a = aec48_create(48000, 0); aec48_internal_set_dump_capacity(a, 1024); std::string d = tmp; int rc = aec48_dump_start(a, d.c_str()); std::vector<float> o(2048); long total = 0;
      for (int k = 0; k < 1400; ++k) { long off = (k * 4096L) % (n - 4096); aec48_process(a, &mic[off], &ref[off], o.data(), 2048); total += 2048;   // 比队列大，必丢
        aec48_process(a, &mic[off], &ref[off], o.data(), 64); total += 64; if (k % 2 == 0) aec48_process(a, &mic[off], &ref[off], o.data(), 2048), total += 2048; usleep(200); }
      aec48_dump_stop(a); aec48_destroy(a); std::string meta = slurp(d + "/meta.json");
      long ds = jnum(meta, "dropped_samples"), calls = jnum(meta, "dropped_calls"), fr = wav_frames(d + "/mic.wav"), segs = 0, sum = 0, ovs = 0; bool adj = false; long lastEnd = -1;
      size_t i0 = meta.find("\"dropped\":["), i1 = meta.find("]", i0);
      for (size_t p = meta.find("{\"pos\":", i0); p != std::string::npos && p < i1; p = meta.find("{\"pos\":", p + 1)) { long pos = jnum(meta, "pos", p), nn = jnum(meta, "n", p); if (pos == lastEnd) adj = true; lastEnd = pos + nn; sum += nn; ++segs; }
      size_t ov = meta.find("\"dropped_overflow\":{"); if (ov != std::string::npos) ovs = jnum(meta, "samples", ov);
      bool ok = rc == 0 && calls >= 1400 && fr + ds == total && sum + ovs == ds && !adj && (ov == std::string::npos ? segs <= 1024 : segs == 1024);
      check(ok, "R-05 丢包 >1024 段：位置表合并相邻段、溢出有标记、样本账对得上",
            "丢包调用 " + std::to_string(calls) + "，段 " + std::to_string(segs) + (ov != std::string::npos ? "（有溢出）" : "（无溢出）") + "，文件 " + std::to_string(fr) + " + 丢弃 " + std::to_string(ds) + " vs 送入 " + std::to_string(total) + "，段和 " + std::to_string(sum) + " + 溢出 " + std::to_string(ovs)); }

    // M-04：不支持的采样率 → NULL（不退出进程）
    check(aec48_create(16000, 0) == nullptr, "M-04 不支持的采样率返回 NULL");

    // M-05：8 个线程同时创建，全部成功且输出一致
    { std::vector<std::vector<float>> outs(8); std::vector<std::thread> th; long M = std::min<long>(n, 48000);
      for (int k = 0; k < 8; ++k) th.emplace_back([&, k] { outs[k] = run(mic, ref, M, 384, 384); });
      for (auto& t : th) t.join(); bool ok = true; for (int k = 1; k < 8; ++k) ok = ok && maxdiff(outs[0], outs[k]) == 0;
      check(ok, "M-05 8 线程并发创建，各实例输出逐样本相同"); }

    printf("%s：不通过 %d 项\n", fails ? "有问题" : "全部通过", fails); return fails ? 1 : 0;
}
