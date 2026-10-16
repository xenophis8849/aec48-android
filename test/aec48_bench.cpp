// 端上测速：按真实节奏（每块 384 样本，48 kHz 下 8 ms）喂数据，量处理线程的 CPU 占用与每块耗时。核心用 taskset 指定。
// 用法：aec48_bench <mic.pcm> <ref.pcm> <采样率> <秒数> [paced=1] [每次块长=384] [hook_compare]
//   输入不够长就循环；paced=0 时不睡、全速跑（量实时率）。输出一行：CPU 占用 = 处理线程 CPU 时间 / 墙钟时间；每块耗时均值 / p99 / 最大；超时 = 单块超过一个块周期的次数；内存 = 建实例前后与跑完后的常驻内存差。
//   设了环境变量 AEC48_DUMP_DIR=<目录> 时同时录音（写盘线程与处理在同一组核心上），末尾多输出一行录音结果（开始返回值、写盘状态、丢弃次数）。
#include "aec48.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
static std::vector<short> rd(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET); std::vector<short> v(n); if (fread(v.data(), 2, n, f) != (size_t)n) exit(2); fclose(f); return v; }
static long rss_kb() { FILE* f = fopen("/proc/self/status", "r"); char l[256]; long v = -1; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6); if (f) fclose(f); return v; }
static double tcpu() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void empty_hook(void*, float*, float*, int) {}
static void empty_hook2(void*, float*, float*, int, const Aec48SpecSide*) {}
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "用法：%s mic.pcm ref.pcm 采样率 秒数 [paced=1]\n", argv[0]); return 2; }
    auto mic = rd(argv[1]), ref = rd(argv[2]); int sr = atoi(argv[3]); double secs = atof(argv[4]); bool paced = argc < 6 || atoi(argv[5]) != 0;
    const int B = argc > 6 ? atoi(argv[6]) : 384;   // 可设 768 等（每次调用处理多块）
    long nIn = (long)std::min(mic.size(), ref.size()) / B * B, nb = (long)(secs * sr / B);
    std::vector<float> m(nIn), r(nIn), o(B); for (long i = 0; i < nIn; ++i) { m[i] = mic[i] / 32768.f; r[i] = ref[i] / 32768.f; }
    if (argc > 7 && !strcmp(argv[7], "hook_compare")) {
        double t[2] = {0, 0};
        for (int round = 0; round < 6; ++round) { const int h = round & 1; Aec48* b = aec48_create(sr, B); if (!b) return 1;
            int rc = h ? aec48_set_spec_hook2(b, empty_hook2, nullptr) : aec48_set_spec_hook(b, empty_hook, nullptr); if (rc) { fprintf(stderr, "挂接回调失败：%d\n", rc); aec48_destroy(b); return 1; }
            auto s = std::chrono::steady_clock::now(); for (long k = 0; k < nb; ++k) { long off = (k * B) % nIn; aec48_process(b, &m[off], &r[off], o.data(), B); }
            t[h] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count() / nb; aec48_destroy(b); }
        const double v1 = t[0] / 3, v2 = t[1] / 3;
        printf("hook_compare · 每块平均：第 1 版空回调 %.6f ms，第 2 版空回调 %.6f ms（多 %.6f ms，%+.1f%%）\n", v1, v2, v2 - v1, 100.0 * (v2 - v1) / v1);
        return 0;
    }
    long r0 = rss_kb(); Aec48* a = aec48_create(sr, B); if (!a) return 1; long r1 = rss_kb();
    const char* dd = getenv("AEC48_DUMP_DIR"); int drc = dd ? aec48_dump_start(a, dd) : 1;   // 设了 AEC48_DUMP_DIR 就边录音边测（录音写盘在库内后台线程）
    std::vector<double> t(nb); int over = 0; const double per = 1000.0 * B / sr;
    auto w0 = std::chrono::steady_clock::now(); double c0 = tcpu(); auto next = w0;
    for (long k = 0; k < nb; ++k) {
        if (paced) { std::this_thread::sleep_until(next); next += std::chrono::nanoseconds((long long)(1e9 * B / sr)); }
        long off = (k * B) % nIn; auto s = std::chrono::steady_clock::now();
        aec48_process(a, &m[off], &r[off], o.data(), B);
        t[k] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count(); if (t[k] > per) ++over;
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count(), cpu = tcpu() - c0; long r2 = rss_kb();
    std::vector<double> s = t; std::sort(s.begin(), s.end()); double mean = 0; for (double x : t) mean += x; mean /= nb;
    printf("{\"paced\":%d,\"sec\":%.1f,\"blocks\":%ld,\"cpu_pct\":%.2f,\"rtf\":%.4f,\"mean_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.3f,\"over\":%d,\"rss_create_kb\":%ld,\"rss_run_kb\":%ld}\n",
           paced, wall, nb, 100.0 * cpu / wall, mean / per, mean, s[(size_t)(0.99 * (nb - 1))], s.back(), over, r1 - r0, r2 - r0);
    if (dd) { double st[4]; aec48_stats(a, st); aec48_dump_stop(a); printf("{\"dump_start\":%d,\"dump_status\":%d,\"dropped_calls\":%.0f}\n", drc, aec48_dump_status(a), st[3]); }
    aec48_destroy(a); return 0;
}
