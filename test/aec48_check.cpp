// 接入自检：用库处理一段测试录音（int16 原始 PCM，单声道），报处理耗时、整段回声消除量；给了参考输出就比对。
// 用法：aec48_check <mic.pcm> <ref.pcm> <采样率> [期望输出.pcm] [每次块长=384] [写出输出.pcm]
// 期望输出 / 写出的输出都是「已按库的延迟往前挪齐」的（第 i 个样本对应 mic 第 i 个样本），所以不同块长（延迟不同）也能直接比。
// 主机（x86）与手机（adb push 后在 /data/local/tmp 跑）都能编。比对口径：不同 CPU 的浮点差会让自适应滤波器走出略有不同的轨迹，
// 所以不要求逐样本相同——要求整段消除量与期望相差 ≤1 dB、差值能量比期望输出低 ≥20 dB。
#include "aec48.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <chrono>
static std::vector<short> rd(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET); std::vector<short> v(n); if (fread(v.data(), 2, n, f) != (size_t)n) exit(2); fclose(f); return v; }
static double erle(const std::vector<short>& m, const std::vector<short>& o, long lat, long skip) {
    double a = 0, b = 0; for (long i = skip; i + lat < (long)o.size() && i < (long)m.size(); ++i) { a += (double)m[i] * m[i]; b += (double)o[i + lat] * o[i + lat]; } return 10 * log10((a + 1) / (b + 1)); }
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "用法：%s mic.pcm ref.pcm 采样率 [期望输出.pcm] [块长] [写出.pcm]\n", argv[0]); return 2; }
    int sr = atoi(argv[3]), blk = argc > 5 ? atoi(argv[5]) : 384; auto mic = rd(argv[1]), ref = rd(argv[2]); long n = (long)std::min(mic.size(), ref.size());
    Aec48* a = aec48_create(sr, blk); if (!a) { fprintf(stderr, "aec48_create 失败\n"); return 1; }
    printf("%s · 采样率 %d · 块长 %d · 延迟 %d 样本（%.1f ms）\n", aec48_version(), sr, blk, aec48_latency_samples(a), 1000.0 * aec48_latency_samples(a) / sr);
    if (getenv("AEC48_DUMP_DIR")) printf("测试录音 → %s（rc %d）\n", getenv("AEC48_DUMP_DIR"), aec48_dump_start(a, getenv("AEC48_DUMP_DIR")));   // 自检录音功能
    std::vector<short> out(n, 0); auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i + blk <= n; i += blk) { aec48_process_s16(a, &mic[i], &ref[i], &out[i], blk); if (getenv("AEC48_DUMP_DIR") && i < n / 2 && i + blk >= n / 2) aec48_set_enabled(a, 0); }   // 录音自检时中途关一次，验 meta 事件
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); double st[4]; aec48_stats(a, st);
    long lat = aec48_latency_samples(a), skip = 2L * sr; double e = erle(mic, out, lat, skip);
    std::vector<short> al(n, 0); for (long i = 0; i + lat < n; ++i) al[i] = out[i + lat];   // 挪齐到 mic 的时间轴
    printf("处理 %.1f s 音频用时 %.2f s（实时率 %.3f）；最近一秒每块均 %.3f ms、最大 %.3f ms\n", (double)n / sr, sec, sec / ((double)n / sr), st[0], st[1]);
    printf("整段回声消除（跳过前 2 s）：%.2f dB\n", e);
    int rc = 0;
    if (argc > 4 && argv[4][0] && argv[4][0] != '-') { auto exp = rd(argv[4]); double ee = erle(mic, exp, 0, skip), d = 0, p = 0; long m = std::min((long)exp.size(), n);
        for (long i = skip; i < m - lat; ++i) { double x = (double)al[i] - exp[i]; d += x * x; p += (double)exp[i] * exp[i]; }
        double rel = 10 * log10((d + 1) / (p + 1)); bool ok = fabs(e - ee) <= 1.0 && rel <= -20; rc = ok ? 0 : 1;
        printf("对期望输出：消除量 %.2f vs %.2f dB（差 %+.2f），差值能量 %.1f dB → %s\n", e, ee, e - ee, rel, ok ? "通过" : "不通过"); }
    if (argc > 6) { FILE* f = fopen(argv[6], "wb"); fwrite(al.data(), 2, n, f); fclose(f); }
    aec48_dump_stop(a); aec48_destroy(a); return rc;
}
