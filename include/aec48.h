/*
 * aec48 0.3 —— 实时全带回声消除（48 kHz / 44.1 kHz，单声道）
 *
 * 用法（每一路采集流一个实例）：
 *   Aec48* a = aec48_create(48000, 384);          // 采样率；每次 process 的样本数（384 的整数倍延迟最低）
 *   每个采集块：aec48_process(a, mic, ref, out, n);  // mic = 麦克风原始采集；ref = 同一时刻扬声器正在播放的最终混音；out = 消除回声后的信号
 *   aec48_destroy(a);
 *
 * 线程：一个实例的 process 只能在同一个线程里调（不加锁、不分配内存、不做文件读写，可以放在音频回调里）；
 *       aec48_set_enabled / aec48_dump_* 可以在任意线程调。
 * 数据：float，满幅 ±1.0，单声道。mic 与 ref 必须是同一个采样率、同样多的样本。
 * 延迟：out 比 mic 晚 aec48_latency_samples() 个样本（48k、块长为 384 的整数倍时 1152 = 24 ms；其他块长 1536 = 32 ms）。
 */
#ifndef AEC48_H
#define AEC48_H
#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define AEC48_EXPORT __attribute__((visibility("default")))
#else
#define AEC48_EXPORT
#endif

typedef struct Aec48 Aec48;

/* sample_rate：48000 或 44100。frames_per_call：之后每次 process 的样本数（会变就填 0）。
 * 失败（参数不支持、内存不足、模型初始化失败）返回 NULL；库不会退出宿主进程。多线程同时创建多个实例是安全的。 */
AEC48_EXPORT Aec48* aec48_create(int sample_rate, int frames_per_call);

/* 处理 n 个样本。mic、ref 输入，out 输出（可与 mic 同一块内存）。n 可以是任意正数（内部按 384 样本分块，长度不受限）。
 * 返回 0：成功，out 的 n 个样本全部有效。
 * 返回 1：create 时声明的 frames_per_call 是 384 的整数倍、本次 n 却不是，输出末尾不足的部分补了零（请按声明的块长调用，或创建时填 0）。
 * 返回 -1：参数无效（空指针或 n ≤ 0）。 */
AEC48_EXPORT int aec48_process(Aec48* a, const float* mic, const float* ref, float* out, int n);

/* int16 版，同上（与 float 版同一处理循环，长度不受限、可同址，输出与 float 版逐样本一致）。返回值同 aec48_process。 */
AEC48_EXPORT int aec48_process_s16(Aec48* a, const short* mic, const short* ref, short* out, int n);

/* 1 = 回声消除开（默认）；0 = 直通（输出麦克风原声，延迟不变，便于 A/B 对比）。下一块生效。
 * 关闭期间线性级与掩码照常运行、只是不用其输出，所以切回开时立即是正常状态，无需重新收敛；代价是关闭时算力不省。 */
AEC48_EXPORT void aec48_set_enabled(Aec48* a, int on);

/* out 相对 mic 的固定延迟（样本数）。 */
AEC48_EXPORT int aec48_latency_samples(const Aec48* a);

/* 录音：把 mic / ref / out 三路分别写成 16 bit wav 到目录 dir（mic.wav / ref.wav / out.wav，已有的覆盖），停止时写 meta.json。
 * 写文件在库内部的后台线程，不阻塞 process。三路按"一次 process 调用"整包写入：后台写不及时时，该次调用的三路一起丢弃，
 * 丢弃位置记在 meta.json 的 dropped 里（相邻的合并成一段），三个文件始终等长、互相对齐（out 比 mic 晚 latency_samples）。
 * 返回 0 成功；-1 参数无效；-2 文件打不开；-3 内存或线程创建失败（录音用到的内存都在这里分配，失败不会影响宿主进程）。
 * aec48_dump_stop 结束并补好 wav 头（可在任意线程调，会等进行中的 process 写完）。 */
AEC48_EXPORT int aec48_dump_start(Aec48* a, const char* dir);
AEC48_EXPORT void aec48_dump_stop(Aec48* a);

/* 上一次录音的写盘结果（在 aec48_dump_stop 之后查）：0 正常；-4 有写盘错误（磁盘满等），meta.json 的 write_error 为 1。
 * 出错时文件保留已经落盘的部分：wav 头与 meta.json 里的样本数 = 文件里实际的样本数（三路可能不等长）。 */
AEC48_EXPORT int aec48_dump_status(const Aec48* a);

/* 最近一秒的统计：[0] 每块平均处理耗时 ms [1] 最大 ms [2] 已处理样本数 [3] 录音丢块数（后台写不及时） */
AEC48_EXPORT void aec48_stats(const Aec48* a, double out4[4]);

AEC48_EXPORT void aec48_destroy(Aec48* a);

/* 版本与模型标识，例如 "aec48 0.1 (lin nlinBc3-dual, mask v59ae20)"。 */
AEC48_EXPORT const char* aec48_version(void);

#ifdef __cplusplus
}
#endif
#endif
