/*
 * aec48 0.5.2 —— 实时全带回声消除（48 kHz / 44.1 kHz，单声道）
 *
 * 用法（每一路采集流一个实例）：
 *   Aec48* a = aec48_create(48000, 384);          // 采样率；每次 process 的样本数（384 的整数倍延迟最低）
 *   每个采集块：aec48_process(a, mic, ref, out, n);  // mic = 麦克风原始采集；ref = 同一时刻扬声器正在播放的最终混音；out = 消除回声后的信号
 *   aec48_destroy(a);
 *
 * 线程：一个实例的 process 只能在同一个线程里调（不加锁、不分配内存、不做文件读写，可以放在音频回调里）；
 *       aec48_set_enabled / aec48_set_spec_hook_enabled 只写一个原子量（不加锁、不分配），可以在任意线程调，包括音频线程；aec48_dump_* 可以在任意线程调（会加锁，不要在音频线程里调）。
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

/* 1 = 回声消除开（默认）；0 = 直通（输出麦克风原声，延迟不变，便于 A/B 对比）。下一块生效。只写原子量，可在音频线程调。
 * 关闭期间线性级与掩码照常运行、只是不用其输出，所以切回开时立即是正常状态，无需重新收敛；代价是关闭时算力不省。 */
AEC48_EXPORT void aec48_set_enabled(Aec48* a, int on);

/* out 相对 mic 的固定延迟（样本数）。 */
AEC48_EXPORT int aec48_latency_samples(const Aec48* a);

/* 录音：把 mic / ref / out 三路分别写成 16 bit wav 到目录 dir（mic.wav / ref.wav / out.wav，已有的覆盖），停止时写 meta.json。
 * 写文件在库内部的后台线程，不阻塞 process。meta.json 的 events 记录音期间两个开关的切换：pos = 切换生效的那一块在录音时间轴（mic.wav 样本序号）上的末尾。三路按"一次 process 调用"整包写入：后台写不及时时，该次调用的三路一起丢弃，
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

/* ———— 频谱回调（0.5 起）：在本库的输出频谱上串接其他处理（例如降噪），不额外增加延迟 ————
 * 本库每处理一帧（每 hop 个样本一帧），在把输出频谱合成回波形之前调用一次回调，回调可原地修改频谱。
 * 帧格式（aec48_spec_format 查询）：
 *   窗长 nfft = 1536、帧移 hop = 384（48 kHz 与 44.1 kHz 都按样本数计）；频点数 bins = nfft/2 + 1 = 769（0 = 直流，768 = 奈奎斯特）。
 *   窗：周期 sqrt-Hann，w[i] = sqrt(0.5·(1 − cos(2π·i/nfft)))，分析与合成同一个窗（window_id = AEC48_WINDOW_SQRT_HANN_PERIODIC）。
 *   频谱 = 加窗帧的不归一化 DFT（负指数，X[k] = Σ w[n]x[n]e^{−j2πkn/nfft}），波形满幅 ±1.0。
 *   合成 = IDFT/nfft，乘窗重叠相加，再除以窗平方的重叠和。回调返回后直流与奈奎斯特的虚部被置 0。
 * 频谱内容：回声消除开时为消除后的输出（近端人声 + 背景噪声，本库不降噪）；aec48_set_enabled(a, 0) 直通时为麦克风原始频谱。两种情况回调都会被调用。
 * 回调在 aec48_process 的调用线程里同步执行：不得分配内存、加锁、读写文件或阻塞；其耗时计入 aec48_stats 的每块耗时。
 * 帧时序：第 1 块不调用；从第 2 块起每块调用一次。第 c 次调用（c 从 0 数）的窗覆盖输入样本 [c·hop − 768, c·hop + 768)，即输入左边补 768 个零后
 *   第 c 个 center=False 的帧（hop = 384、nfft = 1536）。回调不改变 aec48_latency_samples。
 * 录音 meta.json 的 events：pos = 切换生效那一块输入在录音时间轴上的末尾（时间轴含被丢弃的段，与 dropped 同一条）；
 *   out_from = pos + latency − 4·hop 起输出开始变化，out_full = pos + latency − hop 起完全是新状态（中间是重叠相加的过渡）。 */
#define AEC48_WINDOW_SQRT_HANN_PERIODIC 1
typedef void (*aec48_spec_hook)(void* user, float* re, float* im, int bins);

/* 挂上（fn 非空）或摘掉（fn = NULL）频谱回调；user 原样传给回调。
 * 只能在没有 aec48_process 进行中、也没有在录音时调用。返回 0 成功；-1 参数无效；-2 检测到 process 正在进行；-3 正在录音（先 dump_stop）。
 * 挂着回调时开始录音（aec48_dump_start），会另写 out_prehook.wav = 回调之前的输出（与 out.wav 同一时间轴），便于分清问题出在哪一级；meta.json 的 spec_hook 记录挂接与开关状态。 */
AEC48_EXPORT int aec48_set_spec_hook(Aec48* a, aec48_spec_hook fn, void* user);

/* 回调开关：1 = 调用（默认），0 = 跳过（输出与没挂回调时逐样本相同）。只写原子量，可在任意线程（含音频线程）调用，下一块生效；
 * 录音中的切换由处理线程在生效的那一块记进 meta.json 的 events。 */
AEC48_EXPORT void aec48_set_spec_hook_enabled(Aec48* a, int on);

/* 查询帧格式：采样率、窗长、帧移、窗型编号。串接方据此核对自己的格式，对不上就不要挂。 */
AEC48_EXPORT void aec48_spec_format(const Aec48* a, int* sample_rate, int* nfft, int* hop, int* window_id);

/* 版本与模型标识，例如 "aec48 0.1 (lin nlinBc3-dual, mask v59ae20)"。 */
AEC48_EXPORT const char* aec48_version(void);

#ifdef __cplusplus
}
#endif
#endif
