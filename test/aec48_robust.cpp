// 接口健壮性测试（对应 0.1 版审查报告 H-01…M-05 各条的复现方式）。主机上跑：aec48_robust <mic.pcm> <ref.pcm> <临时目录>
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

static std::atomic<long> g_news{0}; static std::atomic<bool> g_count{false};
void* operator new(size_t n) { if (g_count.load(std::memory_order_relaxed)) g_news.fetch_add(1); void* p = malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") { printf("%s  %s%s%s\n", ok ? "通过  " : "不通过", what, detail.empty() ? "" : "：", detail.c_str()); if (!ok) ++fails; }
static std::vector<float> rdf(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET); std::vector<short> v(n); if (fread(v.data(), 2, n, f) != (size_t)n) exit(2); fclose(f); std::vector<float> o(n); for (long i = 0; i < n; ++i) o[i] = v[i] / 32768.f; return o; }
static long rss_kb() { FILE* f = fopen("/proc/self/status", "r"); char l[256]; long v = -1; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6); if (f) fclose(f); return v; }
static long wav_frames(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long n = (ftell(f) - 44) / 2; fclose(f); return n; }
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
      for (int k = 0; k < 20; ++k) { std::string d = tmp; aec48_dump_start(a, d.c_str()); std::this_thread::sleep_for(std::chrono::milliseconds(5 + 7 * (k % 5)));
        if (k % 3 == 0) aec48_set_enabled(a, k % 2); aec48_dump_stop(a);
        long f0 = wav_frames(d + "/mic.wav"), f1 = wav_frames(d + "/ref.wav"), f2 = wav_frames(d + "/out.wav");
        if (!(f0 == f1 && f1 == f2)) { same = false; why = std::to_string(f0) + "/" + std::to_string(f1) + "/" + std::to_string(f2); } }
      go = false; t.join(); aec48_destroy(a);
      check(same, "H-03 并发开始/停止录音 20 次，mic/ref/out 三个文件始终等长", why); }

    // M-01：处理期不分配内存（覆盖时延估计锁定前后）
    { Aec48* a = aec48_create(48000, 384); std::vector<float> o(384); aec48_process(a, &mic[0], &ref[0], o.data(), 384);
      g_news = 0; g_count = true; for (long i = 384; i + 384 <= std::min<long>(n, 384 * 2000); i += 384) aec48_process(a, &mic[i], &ref[i], o.data(), 384); g_count = false;
      check(g_news.load() == 0, "M-01 连续 2000 次 process 期间 operator new 次数为 0", std::to_string(g_news.load())); aec48_destroy(a); }

    // M-02：int16 长度不受限（8192 / 8193 / 20000），与 float 分块一致
    { long M = std::min<long>(n, 20000); std::vector<short> ms(M), rs(M), os(M);
      for (long i = 0; i < M; ++i) { ms[i] = (short)lrintf(mic[i] * 32768.f); rs[i] = (short)lrintf(ref[i] * 32768.f); }
      for (int L : {8192, 8193, 20000}) { if (L > M) continue; Aec48* a = aec48_create(48000, 0); int rc = aec48_process_s16(a, ms.data(), rs.data(), os.data(), L); aec48_destroy(a);
        check(rc == 0, ("M-02 int16 单次 " + std::to_string(L) + " 样本返回 0").c_str(), "rc=" + std::to_string(rc)); } }

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
      long fr = wav_frames(d + "/mic.wav"); check(fr * 2 + 44 <= 60000, "M-03 wav 样本数只计成功写入的部分", std::to_string(fr) + " 帧"); }

    // M-04：不支持的采样率 → NULL（不退出进程）
    check(aec48_create(16000, 0) == nullptr, "M-04 不支持的采样率返回 NULL");

    // M-05：8 个线程同时创建，全部成功且输出一致
    { std::vector<std::vector<float>> outs(8); std::vector<std::thread> th; long M = std::min<long>(n, 48000);
      for (int k = 0; k < 8; ++k) th.emplace_back([&, k] { outs[k] = run(mic, ref, M, 384, 384); });
      for (auto& t : th) t.join(); bool ok = true; for (int k = 1; k < 8; ++k) ok = ok && maxdiff(outs[0], outs[k]) == 0;
      check(ok, "M-05 8 线程并发创建，各实例输出逐样本相同"); }

    printf("%s：不通过 %d 项\n", fails ? "有问题" : "全部通过", fails); return fails ? 1 : 0;
}
