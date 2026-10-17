# aec48

实时全带回声消除库（Android，C 接口）。单声道，48 kHz / 44.1 kHz，算法延迟 24 ms。

处理链：神经网络控制的线性自适应滤波器（含在线时延估计）→ 神经网络残余回声抑制，两级共用一次 STFT。模型编进库里，接入只需要一个 `.so` 和一个头文件。

> ⚠ 本库有一个与降噪库 ans48 联动的接口（频谱回调，0.5 起），它的契约与改动规则见「⚠ 联动接口：频谱回调」一节。

---

## 目录

| 路径 | 内容 |
|---|---|
| `include/aec48.h` | 全部接口（C） |
| `prebuilt/arm64-v8a/libaec48.so`、`prebuilt/armeabi-v7a/libaec48.so` | 预编译库（NDK r28，minSdk 21，c++_static） |
| `prebuilt/candidate-v62b/<ABI>/libaec48.so` | 候选版预编译库（0.6 起，掩码 v62be20；接口与默认版相同，见「候选版」） |
| `prebuilt/talk/<ABI>/libaec48.so` | 讲话版：t2ae10 掩码、e1be12 gru2 判断头与默认开启的平滑门 |
| `src/` | 源码（`aec48.cpp` 接口实现，`aec_chain.h` 处理链，`fb/` 推理内核，`models.cpp` 内嵌模型，`models_v62b.cpp` 候选版内嵌模型） |
| `android/src/com/karaoke/aec48/Aec48Uploader.java` | 录音上传工具（可配置上传地址与库名） |
| `example/duplex_example.cpp` | AAudio 全双工接入示例 |
| `test/aec48_check.cpp`、`test/aec48_bench.cpp`、`test/aec48_robust.cpp`、`test/aec48_oom.cpp`、`test/aec48_hook.cpp`、`test/aec48_hook2.cpp`、`test/aec48_hint.cpp`、`test/Aec48UploaderTest.java`、`test/vec/` | 自检程序、端上测速程序、接口健壮性测试、内存分配失败注入（Linux 主机）、频谱回调测试、上传工具测试、测试向量 |
| `tools/build_android.sh`、`tools/embed_models.py` | 编译脚本、模型内嵌脚本 |
| `third_party/pffft/` | FFT 库（BSD 类许可，见其 `LICENSE.txt`） |
| `third_party/kleidiai/` | Arm KleidiAI 节选：int8 量化打包与点积矩阵乘内核（Apache-2.0） |
| `LICENSE`、`THIRD_PARTY_NOTICES.md` | 本库许可与第三方声明 |

---

## 快速开始

### 用预编译库

把 `prebuilt/<ABI>/libaec48.so` 放进工程（例如 `app/src/main/jniLibs/<ABI>/`），`include/aec48.h` 加进头文件路径，native 代码里链接 `aec48`。CMake 示例：

```cmake
add_library(aec48 SHARED IMPORTED)
set_target_properties(aec48 PROPERTIES IMPORTED_LOCATION ${AEC48_DIR}/prebuilt/${ANDROID_ABI}/libaec48.so)
target_include_directories(your_audio PRIVATE ${AEC48_DIR}/include)
target_link_libraries(your_audio aec48)
```

### 候选版（0.6 起）

`prebuilt/candidate-v62b/<ABI>/libaec48.so` 换了一个掩码模型（v62be20），用来和默认版对比试用。头文件、接口、延迟、频谱回调契约都与默认版相同，把 `.so` 换成候选版即可，代码不用改。两版的区别：

- 候选版的掩码多用两路输入：参考信号按线性级估出的时延挪齐后的频谱，以及由它与线性级输出算出的残余回声估计。两路都在库内部从已有的 `mic` / `ref` 算出，接入方不需要多提供任何数据。
- 效果：回声为主的时段残留更少（测试向量见「自检」：远端单讲整段消除量 8.2 → 13.0 dB）。
- 区分版本：`aec48_version()` 返回 `aec48 0.6.0 (lin nlinBc3-dual, mask v62be20 candidate)`，录音的 `meta.json` 的 `version` 同此，回传的录音可据此分辨用的是哪一版。
- 库文件大小、内存与默认版基本相同（arm64 2.0 MB；每个实例多约 0.3 MB）。

### 讲话档

将 `prebuilt/talk/<ABI>/libaec48.so` 放入工程即可使用讲话档；主机或父工程构建时选择 `-DAEC48_MODELS=src/models_talk.cpp`。版本标识为 `lin nlinBc3-dual, mask talk-t2ae10 + head e1be12-gru2`。掩码来自 t2a e10，小出口来自 e1b e12（主体 317 个共享张量逐位一致）；时延跟踪系数为 0.98，唱歌档保留 0.995。讲话档用于语音场景，唱歌场景继续选默认或候选档。

讲话门默认开启，默认 logit 门限为 −3.0。可以在音频线程调用：

```c
aec48_set_talk_gate(a, 1, -3.0f);  /* 开门控，设置判断门限 */
aec48_set_talk_gate(a, 0, -3.0f);  /* 关闭门控，保留讲话模型输出 */
```

讲话版成功返回 0；非讲话版、空实例或非有限门限返回 −1。开关和门限通过原子量更新，在处理块边界生效，不加锁、不分配。`aec48_set_enabled(a, 0)` 直通时门不作用，内部模型与门状态继续更新。

48 kHz 下每 384 样本判断一次：原始参考块均方超过绝对 −70 dBFS 视为远端有声，并保留过去 125 帧；判断头 logit 严格大于门限视为近端讲话，并保留过去 31 帧（均含当前帧）。仅在远端有声而未判近端讲话时，目标增益为 −40 dB；其余为 0 dB。dB 域开启每帧最多 +32 dB、关闭每帧最多 −2.13333 dB，初值取第一帧目标。转线性后按帧中心 `t*384+192` 逐样本插值，乘在时域合成输出上；不增加原有 1152 样本延迟。44.1 kHz 下保持长度和斜率随实际采样率换算。

频谱回调发生在时域门之前；第 2 版回调的 `near_talk` 字段仍为 −1，不向联动降噪库发布讲话判断结果。唱歌档的输出不受新增接口影响。

判断头为 32,929 参数，理论额外约 4 MMAC/s（x86 主机实测每块多约 0.006 ms）。判断头与门已与离线参考实现逐帧对齐（logit 最大差 7e-6，门增益最大差 7e-8）；整链输出的合成方式与唱歌档相同（逐帧反归一后合成）。

模型导出：`tools/export_talk.py`（需 PyTorch，把讲话掩码与判断头合成一个参数包），再运行 `tools/embed_models.py models/lin_nlinBc3.gtc models/mask_talk.gtc "lin nlinBc3-dual, mask talk-t2ae10 + head e1be12-gru2" src/models_talk.cpp`。时延系数在讲话链加载后覆盖，通用线性模型文件保持原值。

### 从源码编译

```sh
ANDROID_NDK=/path/to/ndk tools/build_android.sh      # 产出默认、候选、讲话三档，两种 ABI
ANDROID_NDK=/path/to/ndk tools/build_android.sh talk # 只重建讲话档；AEC48_BUILD_JOBS 可设构建并发数，默认 2
```

也可以直接把本仓库作为子目录 `add_subdirectory()` 进自己的 CMake 工程（目标名 `aec48`）；要候选版时先设 `set(AEC48_MODELS src/models_v62b.cpp)` 或在命令行加 `-DAEC48_MODELS=src/models_v62b.cpp`。

### 最小调用

```c
#include "aec48.h"

Aec48* aec = aec48_create(48000, 0);            // 采样率；每次 process 的样本数（不固定填 0）

// 每个音频块：
aec48_process(aec, mic, ref, out, n);           // mic：麦克风采集；ref：同一时刻扬声器播放的声音；out：消除回声后的信号

aec48_destroy(aec);
```

---

## 接口

所有函数见 `include/aec48.h`。

| 函数 | 说明 |
|---|---|
| `Aec48* aec48_create(int sample_rate, int frames_per_call)` | 建实例。`sample_rate` 只支持 48000、44100。`frames_per_call` 是之后每次 `process` 的样本数；是 384 的整数倍时延迟最低（见下表），不固定就填 0。失败（参数不支持、内存不足、模型初始化失败）返回 NULL，库不会退出宿主进程；多线程同时创建是安全的。 |
| `int aec48_process(Aec48*, const float* mic, const float* ref, float* out, int n)` | 处理 `n` 个样本。float，满幅 ±1.0。`n` 任意正数，长度不受限（内部按 384 样本分块）；`out` 可以和 `mic` 是同一块内存。返回 0：成功；1：创建时声明的块长是 384 的整数倍、本次 `n` 却不是，输出末尾补了零（按声明块长调用即可避免）；−1：参数无效。 |
| `int aec48_process_s16(Aec48*, const short* mic, const short* ref, short* out, int n)` | 16 bit 版，长度不受限，返回值同上。 |
| `void aec48_set_enabled(Aec48*, int on)` | 1 = 回声消除开（默认）；0 = 直通（输出麦克风原声，延迟不变）。下一块生效。关闭期间线性级和残余抑制照常运行、只是不用其输出，所以切回开时立即是正常状态；代价是关闭时算力不省。 |
| `int aec48_latency_samples(const Aec48*)` | `out` 相对 `mic` 的固定延迟（样本数）。 |
| `int aec48_dump_start(Aec48*, const char* dir)` / `void aec48_dump_stop(Aec48*)` | 录音：把 mic / ref / out 写成 `dir` 下的 `mic.wav`、`ref.wav`、`out.wav`（16 bit 单声道），停止时另写 `meta.json`。start 返回 0 成功、−1 参数无效、−2 文件打不开、−3 内存或线程创建失败。见「录音与上传」。 |
| `int aec48_dump_status(const Aec48*)` | 上一次录音的写盘结果（stop 之后查）：0 正常，−4 有写盘错误。 |
| `void aec48_stats(const Aec48*, double out4[4])` | 最近一秒：[0] 每块平均处理耗时 ms，[1] 最大 ms，[2] 已处理样本数，[3] 录音丢块数。 |
| `int aec48_set_spec_hook(Aec48*, aec48_spec_hook fn, void* user)` | 挂上（`fn` 非空）或摘掉（`fn` = NULL）第 1 版频谱回调，见「⚠ 联动接口：频谱回调」。只能在没有 `process` 进行中、也没在录音时调用。返回 0 成功、−1 参数无效、−2 检测到 `process` 正在进行、−3 正在录音。 |
| `int aec48_set_spec_hook2(Aec48*, aec48_spec_hook2 fn, void* user)` | 挂上或摘掉第 2 版频谱回调；与 `aec48_set_spec_hook` 二选一，调用约束与返回码相同。 |
| `void aec48_set_spec_hook_enabled(Aec48*, int on)` | 回调开关：1 = 调用（默认），0 = 跳过（下一块起不再调用；运行中关掉时，输出要经 3 块重叠相加过渡才与没挂回调时逐样本相同，即录音事件的 out_from 到 out_full；从一开始就关着则始终相同。重新打开同样有 3 块过渡）。任意线程（含音频线程）。 |
| `void aec48_spec_format(const Aec48*, int* sample_rate, int* nfft, int* hop, int* window_id)` | 查询回调里频谱的帧格式。 |
| `void aec48_destroy(Aec48*)` | 释放（录音中会先停止录音）。 |
| `const char* aec48_version(void)` | 版本与模型标识。 |

### 延迟

| 采样率 | `frames_per_call` 是 384 的整数倍 | 其他（含 0） |
|---|---|---|
| 48 kHz | 1152 样本 = 24.0 ms | 1536 样本 = 32.0 ms |
| 44.1 kHz | 1152 样本 = 26.1 ms | 1536 样本 = 34.8 ms |

内部按 384 样本一块处理。块长不是 384 的整数倍时，库多垫一块零，保证任何块长都不会缺输出。

### 线程

- 一个实例的 `process` 只在同一个线程里调用。`process` 内不分配内存、不加锁、不读写文件，可以直接在音频回调里调。
- `aec48_set_enabled`、`aec48_set_spec_hook_enabled` 只写一个原子量（不加锁、不分配内存），可以在任意线程调，包括音频线程。
- `aec48_dump_start/stop`、`aec48_stats` 可以在任意线程调；`dump_start/stop` 会加锁、分配或等待，不要在音频线程里调。
- `aec48_set_spec_hook` 不能和 `process` 同时调用（在开始处理前挂好；运行中要临时停用回调，用 `aec48_set_spec_hook_enabled`）。
- 多路音频各建一个实例，实例之间互不影响。

---

## 接入要点

1. **麦克风要用原始采集。** 不要用带系统回声消除的采集方式（AAudio / OpenSL 的 `VOICE_COMMUNICATION` 预设、`AudioManager.MODE_IN_COMMUNICATION` 等），否则两层处理叠加。推荐输入预设 `GENERIC` 或 `UNPROCESSED`。
2. **参考信号 `ref` = 扬声器实际播放的最终声音**，也就是所有要播出去的声音（远端人声、背景音乐、提示音……）混完之后、送进播放设备的那一路。没进参考的声音消不掉。
3. **两路不用调用方对齐。** `mic` 与 `ref` 之间的时间差由库在线估计（0 – 约 1 s），只要求：同一台设备、同一采样率、每次样本数相同、按时间顺序连续送入。最简单的做法是在播放回调里同时读麦克风、记下本次播放的样本（见 `example/duplex_example.cpp`）。
4. **调用位置：** 采集之后、后续处理之前（降噪、自动增益、编码、混音等都放在 `out` 之后）。工作在同一帧格式上的频域处理（如降噪）也可以用频谱回调串在库内部，不额外增加延迟，见「⚠ 联动接口：频谱回调」。
5. **起步：** 有回声后约 0.5 s 内收敛。回声路径变化（换位置、换路由等）时库在线重新适应，不需要重建实例；切换扬声器 / 听筒 / 耳机等路由的场景尚未专门测试。
6. **单声道。** 立体声采集取一路或先下混。

---

## ⚠ 联动接口：频谱回调（与降噪库串接）

> **这一节是跨项目的接口契约。** 降噪库 ans48 的「频谱档」按这里的帧格式训练模型、按这里的时序逐帧处理，再挂在本库的频谱回调上运行。
> **本节任何内容的改动（帧格式、调用时序、线程要求、开关语义、录音元数据）都会影响 ans48，必须：**
> 1. 提升下面的契约版本（`AEC48_SPEC_CONTRACT_VERSION`，头文件里有同名宏）；
> 2. 在下面的「契约变更记录」和「版本与模型」里用 **【联动】** 标出，写清改了什么、对串接方有什么影响；
> 3. 通知 ans48 一侧核对后再发布。
>
> 只改说明、不改行为的更正同样记一条（串接方可能已按旧说明实现）。

**当前契约版本：2**（库 0.7.0 起；`AEC48_SPEC_CONTRACT_VERSION` = 2）。版本 2 包含版本 1 的全部帧格式、时序和回调行为，并增加 `aec48_set_spec_hook2` 与逐帧状态信息；版本 1 回调行为保持不变。

### 用途与边界

本库内部按 STFT 逐帧处理。0.5 起可以挂一个回调：每帧输出频谱在合成回波形之前交给回调，回调原地修改。用途是把另一个工作在同一帧格式上的频域处理（例如降噪）直接接在回声消除后面，省掉一次「合成 → 再分析」，**不增加延迟**（整体仍是 `aec48_latency_samples`），也省一次正反变换。本库不依赖、也不链接任何后处理库；双方只靠下面的帧格式约定对接。

```c
int sr, nfft, hop, win;
aec48_spec_format(aec, &sr, &nfft, &hop, &win);      // 串接方据此核对自己的格式，对不上就不要挂
aec48_set_spec_hook(aec, my_spec_fn, my_state);       // 开始处理前挂好
...
void my_spec_fn(void* user, float* re, float* im, int bins) { /* 原地修改 re[0..bins)、im[0..bins) */ }
```

### 帧格式（契约版本 1 与 2 共用）

| 项 | 值 |
|---|---|
| 窗长 `nfft` / 帧移 `hop` | 1536 / 384 样本（48 kHz 与 44.1 kHz 都按样本数计；48 kHz 下每 8 ms 一帧） |
| 频点数 `bins` | `nfft/2 + 1` = 769（0 = 直流，768 = 奈奎斯特） |
| 窗（`window_id` = `AEC48_WINDOW_SQRT_HANN_PERIODIC` = 1） | 周期 sqrt-Hann：`w[i] = sqrt(0.5·(1 − cos(2π·i/nfft)))`；分析与合成同一个窗 |
| 频谱 | 加窗帧的不归一化 DFT，负指数：`X[k] = Σ w[n]·x[n]·e^{−j2πkn/nfft}`，波形满幅 ±1.0。等价于 `torch.stft(F.pad(x, (768, 0)), n_fft=1536, hop_length=384, window=torch.hann_window(1536, periodic=True).sqrt(), center=False, return_complex=True)` |
| 合成 | `IDFT / nfft`，乘窗重叠相加，再除以窗平方的重叠和；回调返回后直流与奈奎斯特的虚部被置 0 |
| 内容 | 回声消除开：消除后的输出（近端人声 + 背景噪声；本库不降噪）。`aec48_set_enabled(a, 0)` 直通：麦克风原始频谱。两种情况回调都会被调用 |
| 时机 | 在 `aec48_process` 的调用线程里同步执行。第 1 块不调用；从第 2 块起每块调用一次，第 c 次调用（c 从 0 数）的窗覆盖输入样本 [c·384 − 768, c·384 + 768)，负索引补零——即输入左边补 768 个零后、`center=False` 的第 c 帧（上一行 torch 写法里的 `F.pad`）|

### 线程、时序与开关

- 回调在音频线程里执行：不得分配内存、加锁、读写文件或阻塞。回调耗时计入 `aec48_stats` 的每块耗时。
- 回调需逐帧因果：拿到第 t 帧就给出第 t 帧的结果，不能等后面的帧，否则就不再是「不增加延迟」。
- 挂着回调时库多维护一路「回调前」的输出（见「录音」的 `out_prehook.wav`）：回调开着时多一次逆变换，关着时只多一次重叠相加。不挂回调时没有这份开销（摘掉后再维护 4 块，保证马上重新挂上时状态也是对的）。开销实测见「版本与模型」0.5.1。
- 运行中临时停用：`aec48_set_spec_hook_enabled(a, 0)`（可在音频线程调），下一块起不再调用回调；输出经 3 块重叠相加过渡（`out_from` 到 `out_full`）后与没挂回调时逐样本相同。重新打开同样有 3 块过渡。需要「立即」旁路的场景（例如回调出错时）要算上这段过渡。

### 契约版本 2：逐帧状态回调

版本 2 新增 `aec48_set_spec_hook2`。它与 `aec48_set_spec_hook` 互斥：挂上其中一个会摘掉另一个，传 `fn = NULL` 摘掉当前回调。挂接约束、返回码、开关、帧时序、线程要求、帧格式和录音行为均与版本 1 相同。`side` 及其指向的数据只在本次回调执行期间有效；需要留存时由调用方复制。`aec48_spec_hook` 的参数和逐帧行为不变。

```c
#define AEC48_SPEC_CONTRACT_VERSION 2
#define AEC48_SIDE_MODE_PASSTHROUGH 0
#define AEC48_SIDE_MODE_LINEAR      1
#define AEC48_SIDE_MODE_FULL        2
typedef struct Aec48SpecSide {
    int size;                  /* 结构体字节数，便于以后向后追加字段 */
    int aec_mode;              /* 本帧处理模式：直通 0、只线性级 1、整链 2 */
    float far_active;          /* 参考端远端活动标志，本版为 0 或 1 */
    float near_talk;           /* 近端说话概率；本版暂不提供，固定为负值 */
    const float* resid_echo;   /* 每频点残余回声功率，尺度同 re²+im² */
    int resid_bins;            /* resid_echo 的元素数，与 bins 相同（当前 769） */
} Aec48SpecSide;
typedef void (*aec48_spec_hook2)(void* user, float* re, float* im, int bins,
                                 const Aec48SpecSide* side);
int aec48_set_spec_hook2(Aec48* a, aec48_spec_hook2 fn, void* user);
```

`size` 为 `sizeof(Aec48SpecSide)`，用于识别结构体长度，后续版本只在末尾追加字段。`aec_mode` 是处理链本帧的模式：0 直通、1 只线性级、2 整链；与处理链内部模式一致，`aec48_set_enabled(a, 0)` 对应 0，开启对应 2。`far_active` 是本帧原始参考的加窗频谱总功率活动标志：令 `tot = Σ(|Xr[k]|² + |Xi[k]|²)`，超过 hook2 挂接以来的最大 `tot × 1e−5 + 1e−12` 即为有声；最近 `ceil(1 秒 / hop)` 帧（含本帧）出现过有声则为 1，否则为 0。48 kHz 时窗口为 125 帧，44.1 kHz 时为 115 帧。峰值从挂上 hook2 起统计，只在 hook2 已挂且开关开启时更新；开关关闭时峰值与拖尾计数冻结（不更新、不清零），每次挂上新的 hook2 时清零。单位为无量纲，本版只取 0 或 1，不是概率。

`resid_echo` 指向 `resid_bins` 个非负有限浮点数，当前 `resid_bins = bins = 769`。每项是预计回调收到的频谱中该频点的残余回声功率，单位为未归一化 DFT 的频点幅度平方；尺度与 `re[k]² + im[k]²` 相同。提示在掩码电平归一后的谱上估计，输出已换算回回调频谱的尺度：有提示模型时直接复用 Cofi 本帧的 `hm`（幅度），取 `hm²` 为提示功率（保留模型的数值地板），再除以本帧增益 `g` 的平方；`g` 与本帧送进掩码前的电平归一增益相同。无提示模型时用独立实例对 `Pe = |E|²`、`Px = |XA|²` 同乘增益平方后估计，再除以增益平方。无掩码时增益取 1。算法使用 48 个 ERB 带，范围 40 Hz 至采样率一半、`K = 12`、衰减 0.7、每帧上升 0.5 dB、下降系数 0.3，带内功率均摊到频点。无提示模型参数时也使用同一组固定参数；频率 `f` 的 ERB 为 `21.4·log10(1 + 0.00437f)`，在 `erb(40)` 到 `erb(sr/2)` 间等分 49 个边界；频点 `k` 的频率为 `k·sr/nfft`，按 `searchsorted(right) − 1` 分带并夹在 0 至 47。最终值按本帧实际交给回调的频谱计算：整链且有掩码时 `R̂e[k] × min(1, |FX[k]|²/(|E[k]|² + 1e−20))`；只线性级或整链无掩码时为 `R̂e[k]`；直通时为 `|D[k] − E[k]|² + R̂e[k]`。`D` 是麦克风频谱，`E` 是线性级输出，`FX` 是掩码输出。只挂版本 1 回调或 hook2 开关关闭时，不计算这些附加信息。默认版独立提示在 hook2 关闭期间冻结，重新挂接时清零；候选版直接复用模型提示，提示状态始终随模型运行。默认版首次启用或重新开启 hook2 时，从当时起缓存对齐参考历史，缺少的历史补零；候选版复用处理链已维护的参考历史。`near_talk` 固定为 `−1.0f`，表示当前版本不提供近端说话概率。

录音时挂任一版本的回调都会写 `out_prehook.wav`。`meta.json` 的 `spec_hook` 记录中包含 `contract_version`（1 或 2），标明录音开始时使用的回调契约。

### 契约变更记录

契约版本从 0.5.2 补订起正式编号；在此之前的 0.5 / 0.5.1 / 0.5.2 统一算作版本 1（接口签名没变，行为与说明的变化都列在下表）。以后按本节开头的规则：改一项就加一行，并提升版本号。

| 契约版本 | 库版本 | 变更 | 对串接方的影响 |
|---|---|---|---|
| 1 | 0.5 | **【联动】** 新增频谱回调、帧格式查询、回调开关 | 首次提供 |
| 1 | 0.5.1 | **【联动】** 两个开关改为只写原子量，可在音频线程调；录音事件改由处理线程按块记录 | 行为更安全，接口不变；录音事件的 `pos` 含义变了（见 0.5.2） |
| 1 | 0.5.2 | **【联动】说明更正（行为不变）**：① 第 1 块不调用、第 2 块起每块调用（之前误写「前两块不调用」）；② 第 c 次调用的窗覆盖输入 [c·384 − 768, c·384 + 768)，torch 等价写法要先左补 768 个零（之前漏了 `F.pad`）；③ 关掉回调后输出有 3 块过渡才与没挂时相同（之前误写「立即相同」）；④ 录音事件加 `out_from` / `out_full` | 按旧说明做逐帧核对的，帧起点会差 768 个样本；按旧说明做「立即旁路」的，要算上 3 块过渡。训练数据只用帧内容、不受①②影响 |
| 2 | 0.7.0 | **【联动】** 新增 `aec48_set_spec_hook2`、`Aec48SpecSide` 与远端活动、处理模式、残余回声功率字段；`meta.json` 记录 `contract_version` | 第 1 版接口及行为不变；不重编的旧用户不受影响。重编时版本检查应由 `!= 1` 改为 `< 1`（只用第 1 版回调）或 `< 2`（用 hook2），以兼容后续契约版本 |

---

## 录音与上传

### 录音

```c
aec48_dump_start(aec, "/data/user/0/<包名>/files/aec_rec/20261012_101500");   // 目录需已存在
...
aec48_dump_stop(aec);
```

写文件在库内部的后台线程，不阻塞 `process`。目录里会有：

| 文件 | 内容 |
|---|---|
| `mic.wav` | 麦克风输入 |
| `ref.wav` | 参考输入 |
| `out.wav` | 输出（比 mic 晚 `latency_samples` 个样本） |
| `out_prehook.wav` | 仅在**录音开始时挂着频谱回调**才有：回调之前的输出（与 `out.wav` 同一时间轴），用来分清问题出在回声消除还是串接的处理 |
| `meta.json` | `version`、`sample_rate`、`latency_samples`、`samples`、`samples_mic_ref_out`（三个文件的样本数，正常时相等）、`start_unix_ms`（开始录音的时刻）、`write_error`（1 = 有写盘错误）、`enabled_at_start`、`events`（录音期间两个开关的切换：`{"pos": 样本位置, "enabled": 0/1}` 或 `{"pos": 样本位置, "spec_hook_enabled": 0/1}`。由处理线程在切换生效的那一块记录。`pos` = 那一块输入在录音时间轴上的末尾（时间轴含被丢弃的段，与 `dropped` 的 `pos` 同一条；没有丢弃时就是 `mic.wav` 样本序号）。这一块算出的帧当场参与输出合成、且覆盖最近 4 块，所以 `out.wav` 从 `out_from` = `pos + latency − 1536` 开始变化，到 `out_full` = `pos + latency − 384` 起完全是新状态，中间是重叠相加的过渡；两个位置都直接写在事件里。最多记 1024 条，更多的计入 `events_overflow`）、`dropped` / `dropped_calls` / `dropped_samples`（后台写不及时整包丢弃的位置、次数与样本数，正常为空 / 0 / 0）、`dropped_overflow`（见下，正常为 `null`）、`spec_hook`（`attached_at_start`：录音开始时是否挂着回调；挂着时另有 `enabled_at_start`、`samples_out_prehook` 与 `contract_version`（1 或 2）；录音期间 `aec48_set_spec_hook_enabled` 的切换记在 `events`） |

各路（三路；挂着回调时四路）按"一次 `process` 调用"整包写入：后台写不及时时，这次调用的各路一起丢弃，并记进 `dropped`（`pos` 为录音时间轴上的样本位置，`n` 为丢的样本数；紧挨着的几次丢弃合并成一段），所以三个文件始终等长、互相对齐。`dropped` 最多记 1024 段；更多时 `dropped_overflow` 给出 `{"from_pos": 第一个没记下的位置, "calls": 之后的丢弃次数, "samples": 之后丢的样本数}`——`from_pos` 之前的时间轴可以按 `dropped` 完整还原，之后只知道合计。`aec48_dump_stop` 可以在任意线程调用，它会等正在进行的那次 `process` 写完再收尾。写盘出错（如磁盘满）时 `aec48_dump_status` 返回 −4，已落盘的部分保留，wav 头与 `meta.json` 里的样本数和文件实际内容一致。建议每次录音使用一个新目录。

### 上传（可配置）

`Aec48Uploader` 把一个录音目录里的 4 个文件逐个上传；有 `out_prehook.wav` 时一并上传（track 名 `out_prehook`），没有不算失败。需要配置两项：

| 配置 | 含义 |
|---|---|
| `uploadUrl` | 接收地址（HTTP / HTTPS） |
| `dbName` | 库名，原样放进每个请求的 `db` 字段，接收端据此决定存到哪个库 |

```java
Aec48Uploader up = new Aec48Uploader(uploadUrl, dbName, Build.MANUFACTURER + " " + Build.MODEL, BuildConfig.VERSION_NAME);
new Thread(() -> {
    int failed = up.upload(recDir, null, (track, ok, detail) -> Log.i("aec48", track + " " + ok + " " + detail));
}).start();                                     // 会做网络请求，不能在主线程
```

四个文件缺任何一个都计为失败（不完整的录音不会报成功）。失败的文件留在目录里，再调一次 `upload` 会补传；已成功的文件写 `<文件名>.uploaded` 标记（内容为 `session_id`、文件大小与内容 CRC32），三者都与这次一致才跳过：同一会话重试只补传失败的文件，换 `session_id` 或同一目录重新录音都会重新上传。同一目录多次上传沿用同一个 `session_id`（存在目录里的 `.session_id`）。需要 `INTERNET` 权限。

**接收端请求格式**（每个文件一个请求）：

```
POST <uploadUrl>
Content-Type: multipart/form-data

字段：
  db           库名（dbName）
  session_id   本次录音的编号（同一录音目录的 4 个文件相同）
  track        mic | ref | out | meta
  device       设备型号
  app_version  App 版本号
  file         文件本体（mic/ref/out 为 wav，meta 为 json）
```

返回 HTTP 2xx 视为成功。

---

## 自检

`test/aec48_check` 用库处理一段录音，报耗时与整段回声消除量；给了期望输出就比对。主机上：

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host
./build-host/aec48_check test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm 48000 test/vec/03ra86gHhkWD_expected48k.pcm 384
```

手机上（`tools/build_android.sh` 同时编出 `build-android-<ABI>/aec48_check`）：

```sh
adb push build-android-arm64-v8a/libaec48.so build-android-arm64-v8a/aec48_check test/vec/* /data/local/tmp/aec48/
adb shell 'cd /data/local/tmp/aec48 && chmod +x aec48_check && LD_LIBRARY_PATH=. ./aec48_check 03ra86gHhkWD_mic.pcm 03ra86gHhkWD_ref.pcm 48000 03ra86gHhkWD_expected48k.pcm 384'
```

| 测试向量 | 采样率 | 期望整段消除量 |
|---|---|---|
| `03ra86gHhkWD` | 48 kHz | 8.15 dB |
| `00t5hG031EmZ` | 48 kHz | 7.62 dB |
| `03ra86gHhkWD_*441` | 44.1 kHz（块长 441） | 7.77 dB |

候选版用 `_v62b` 结尾的期望输出（主机上按 `-DAEC48_MODELS=src/models_v62b.cpp` 另编一份，例如 `build-host-v62b`）：

```sh
cmake -S . -B build-host-v62b -DCMAKE_BUILD_TYPE=Release -DAEC48_MODELS=src/models_v62b.cpp && cmake --build build-host-v62b
./build-host-v62b/aec48_check test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm 48000 test/vec/03ra86gHhkWD_expected48k_v62b.pcm 384
```

| 测试向量（候选版） | 采样率 | 期望整段消除量 |
|---|---|---|
| `03ra86gHhkWD`（`_expected48k_v62b`） | 48 kHz | 12.97 dB |
| `00t5hG031EmZ`（`_expected48k_v62b`） | 48 kHz | 8.05 dB |
| `03ra86gHhkWD_*441`（`_expected441_v62b`） | 44.1 kHz（块长 441） | 10.26 dB |

通过标准：整段消除量与期望相差 ≤ 1 dB，且与期望输出的差值能量低 ≥ 20 dB。不同 CPU 的浮点差会让自适应滤波器走出略有不同的轨迹，所以不要求逐样本相同。期望输出与写出的输出都已按延迟挪齐到 mic 的时间轴。测试向量取自公开的 AEC Challenge 盲测集（远端单讲），int16 原始 PCM，单声道。

设置环境变量 `AEC48_DUMP_DIR=<目录>` 时，自检会同时验证录音功能（中途切一次直通，`meta.json` 里应有一条 `events`）。

`test/aec48_robust` 检查接口边界与故障路径（主机与手机都能跑，全部通过返回 0）：反复创建 / 销毁不漏内存；超长单次调用与分块处理逐样本一致；原地处理（`out` = `mic`）；录音中途从另一线程开始 / 停止时三个文件等长、wav 头与文件一致；处理期不分配内存；int16 超过 8192 样本；int16 大块（4608、12288，含原地）与 float 384 块逐样本一致；声明块长与实际不符时返回 1；写盘失败时 `aec48_dump_status` = −4 且文件长度、wav 头、`meta.json` 三者一致；录音缓冲分配失败返回 −3 且进程不退出；丢弃超过 1024 段时的合并与溢出记录；不支持的采样率返回 NULL；多线程并发创建。主机版用替换 `operator new` 的方式做分配计数与故障注入，第一项会先确认这种替换能拦到库内部的分配。

```sh
./build-host/aec48_robust test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm /tmp/aec48_robust_dir
./build-host/aec48_oom      # 建实例时每一次 malloc 逐次失败：都返回 NULL、不崩溃（Linux 主机）
```

也可以用 ThreadSanitizer 构建跑这个测试查线程竞态（`-DCMAKE_CXX_FLAGS=-fsanitize=thread` 等，0.3、0.5 无告警）。

`test/aec48_hook` 检查第 1 版频谱回调；`test/aec48_hook2` 检查第 2 版字段、远端活动时序、残余回声功率、版本互换与录音约束。`test/aec48_hint` 用固定增益的多帧输入核对共享提示算法；加 `-DAEC48_TESTING=ON` 构建时，候选版测试直接核对复用的模型提示功率，要求逐位一致（换算回未缩放尺度时允许浮点舍入误差）。默认版另逐位检查实际带内输入只乘一次本帧 `g²`，并以固定增益检查独立提示先缩放再换算回来的结果与未缩放口径一致（浮点误差内）；无掩码的 `g = 1` 也核对。私有观测口不进对外头文件，默认构建关闭。主机编译后分别运行，全部通过返回 0。

```sh
./build-host/aec48_hook test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm 48000 /tmp/aec48_hook_dir   # 目录需已存在
./build-host/aec48_hook2
./build-host/aec48_hint
```

上传工具的测试（主机上有 JDK 即可，会在本机起一个临时 HTTP 服务）：

```sh
javac -d /tmp/aec48up android/src/com/karaoke/aec48/Aec48Uploader.java test/Aec48UploaderTest.java && java -cp /tmp/aec48up Aec48UploaderTest
```

---

## 性能

### 测法

`test/aec48_bench`：按真实节奏每 8 ms 送一块（48 kHz、384 样本），用 `taskset` 把进程钉在指定核心上跑，统计处理线程的 CPU 时间 / 墙钟时间（= 该核心被占用的比例），以及每块处理耗时与超时次数。末尾增加 `hook_compare` 可在主机上不休眠地交替测第 1 版与第 2 版空回调各 3 轮，复用输入秒数与块长，输出两者每块平均耗时、差值和百分比：`./build-host/aec48_bench mic.pcm ref.pcm 48000 30 0 384 hook_compare`。Galaxy S10 息屏进入打盹状态时，系统不允许把进程钉到中 / 大核（`taskset` 报 Invalid argument），测这几档时须点亮屏幕。

```sh
adb shell 'cd /data/local/tmp/aec48 && LD_LIBRARY_PATH=. taskset <核心掩码> ./aec48_bench 03ra86gHhkWD_mic.pcm 03ra86gHhkWD_ref.pcm 48000 30 1'
```

### 结果（0.4.1，48 kHz，每块 8 ms，回声消除开）

| 设备（SoC） | 核心 | ABI | 核心占用 | 每块耗时 平均 / p99 / 最大（ms） | 超时块 | 全速实时率 |
|---|---|---|---|---|---|---|
| Galaxy S10 SM-G9730（骁龙 855） | 中核 Kryo 485 Gold 2.42 GHz（A76 级） | arm64-v8a | 13.1% | 1.01 / 1.42 / 1.8 | 0 | 0.031 |
| | | armeabi-v7a | 19.0% | 1.51 / 1.85 / 2.9 | 0 | 0.049 |
| | 小核 Kryo 485 Silver 1.79 GHz（A55 级） | arm64-v8a | 41.7% | 3.42 / 4.23 / 10.8 | 2 / 3750 | 0.151 |
| | | armeabi-v7a | 62.7% | 5.10 / 5.89 / 10.0 | 6 / 3750 | 0.233 |
| | 不绑核（系统调度） | arm64-v8a | 42.2% | 3.40 / 4.05 / 12.9 | 3 / 3750 | 0.027 |
| | | armeabi-v7a | 61.7% | 5.00 / 5.87 / 9.0 | 9 / 3750 | 0.041 |
| vivo V1934A（Helio P65，系统不声明点积指令，走后备路径） | 大核 Cortex-A75 2.0 GHz | arm64-v8a | 17.5% | 1.36 / 1.45 / 3.3 | 0 | 0.085 |
| | | armeabi-v7a | 23.1% | 1.80 / 1.97 / 2.1 | 0 | 0.105 |
| | 小核 Cortex-A55 1.7 GHz | arm64-v8a | 49.7% | 3.90 / 5.38 / 7.5 | 0 | 0.249 |
| | | armeabi-v7a | 61.6% | 4.88 / 5.50 / 6.9 | 0 | 0.338 |

- 候选版：每块比默认版多一次 FFT 和 48 个频带的递推。电脑上（x86，全速）每块平均 0.10 vs 0.10 ms，分不出差别；S10 小核 armeabi-v7a 全速（`hook_compare`，挂第 1 版空回调）每块 2.50–2.60 vs 默认版 2.15–2.20 ms（约 +15%）。按真实节奏的占用率表尚未对候选版单独测。
- 第 2 版回调（0.7.0，`hook_compare`：同一进程交替挂第 1 版 / 第 2 版空回调各 3 轮，全速，S10 小核 armeabi-v7a，4 次测量）：默认版多 0.42–0.50 ms / 块（+20–23%），候选版多 0.04–0.07 ms / 块（+2–3%）。默认版的掩码不带残余回声提示，挂第 2 版回调时要另算对齐参考频谱和提示递推；候选版直接复用模型自己的提示。挂第 2 版回调后两版每块总耗时相近（2.56–2.69 ms）。不挂第 2 版回调时没有这部分开销。电脑上（x86，单核全速）：默认版 +10.1 µs / 块（+11.3%），候选版 +1.4 µs / 块（+1.5%）。
- 内存：每个实例常驻约 5.3–6.0 MB；开录音（`aec48_dump_start`）后另加约 6 MB 录音缓冲（第一次录音时分配）。内嵌模型在库文件里：arm64 1.9 MB、armv7 1.7 MB。
- S10 的超大核（Kryo 485 Prime）普通进程不能用 `taskset` 绑定，未单独测；"不绑核"一行全速时实时率 0.027，说明全速时被调度到大核，而按真实节奏时负载低、被调度在小核上（占用与小核一行相同）。
- 小核上平均耗时约 3.4–3.9 ms（每块预算 8 ms），偶有单块超过 8 ms（S10 小核约 0.1%）。处理放在音频回调里时，建议给处理线程留至少一块的缓冲，或让处理线程跑在大 / 中核上（例如使用 Android 性能提示接口 `APerformanceHint`、提高线程优先级）。
- 64 位比 32 位省 15–25%，优先用 arm64-v8a。
- 边录音边处理（`AEC48_DUMP_DIR` 设给 `aec48_bench`，写盘线程与处理绑在同一个小核上，按真实节奏 300 s）：S10 超时块 0.46% → 0.57%、平均耗时 3.95 → 4.13 ms；vivo 超时块 9 → 3 次（无差别）；两台都没有丢弃，三个文件等长。
- 与 0.3 相比（同设备同测法，arm64）：小核占用 S10 46.8% → 41.7%、vivo 50.6% → 49.7%；中 / 大核 14.7% → 13.1%、19.5% → 17.5%。全速每块耗时 S10 小核 1.41 → 1.21 ms、vivo 小核 2.22 → 1.99 ms。S10 各行为 0.4 实测（S10 系统声明点积指令，0.4.1 在它上面的执行路径与 0.4 相同）。vivo 小核按节奏的占用单次波动较大（同一版本两次 49.7% / 56.9%），以全速耗时比较更可靠。
- 与 0.2 相比（同设备同测法）：小核占用 S10 53.9% → 46.8%、vivo 56.5% → 50.6%；中 / 大核占用 15.4% → 14.7%、23.0% → 19.5%（arm64）。

---

## 研发过程

### 目标与约束

- 48 kHz / 44.1 kHz 原生全带处理，不降采样，运行时不引入重采样。
- 算法延迟 ≤ 32 ms。
- 手机端整链算力与参考基线（阿里通义开源 JAEC，16 kHz）同一量级：不超过其约 5 倍（本库处理全带，频宽是其 3 倍）。
- 效果优先级：不错消（不伤近端人声、不丢内容）＞ 不漏消（回声消除不留大段残余）＞ 客观分数 ＞ 细节音质。

### 迭代流程

每一轮：造训练数据 → 训练 → 用同一套评估尺子全量评估 → 与上一版及参考基线逐条配对比较（按测试条自助抽样给 95% 范围，区分"确实更好 / 确实更差 / 在波动内"）→ 人工试听 → 决定下一轮。尺子判为更差的版本不进入试听。

- **训练数据：** 回声全部按构造模拟（参考 → 喇叭频响与非线性 → 实测房间冲激响应 → 设备时延，可选叠加设备播放链的时变处理），近端为说话、唱歌、器乐，标签按构造干净。线性级输出用与端上同一份推理代码生成，训练与推理同口径。
- **评估（黑盒，只看输入 → 输出）：**
  - 公开集：ICASSP 2023 AEC Challenge 盲测集（远端单讲 / 双讲 / 近端单讲）与 AECMOS；
  - 漏消：3 s 窗消除量 < 3 dB 的占比、长段漏消、收敛到 6 dB 的用时，按"回声为主"的窗统计；
  - 近端保护：近端真值已知的拼接测试集（说话、唱歌），统计误压、内容丢失（"挖坑"）、静态音色偏差、齿音损伤；
  - 资源：手机上逐帧流式实测，不按整段推理折算。

### 技术路线迭代

| 阶段 | 方案 | 结果与转向原因 |
|---|---|---|
| 1 | 现成模型 LocalVQE（16 kHz）+ 自写高频线性支路 | 真机实时跑通，消除 24–28 dB；16 kHz 模型不满足全带要求，转自训 48 kHz |
| 2 | 自训 48 kHz 残余回声抑制网络（GTCRN 骨干）+ 学习型分块频域卡尔曼线性级 | 掩码路线可行；分块频域、逐频点独立更新这一族线性级在 2 kHz 以上有结构性上限，消不深 |
| 3 | 子带 STFT 域多抽头 RLS 线性级（双滤波、在线时延估计）+ GTCRN 掩码，逐轮调整数据与损失；掩码窗 16 → 32 ms 提高频率分辨率 | 消除深度与近端保护逐步达标，双讲区分能力随频率分辨率提升；但整链算力约为基线 20–30 倍 |
| 4 | 掩码骨干横比 GTCRN / UL-UNAS / CoFi-Lite（同算力约 50 M 次乘加/秒），选 CoFi-Lite；手写端上推理 | CoFi-Lite 音色偏差最小；算力瓶颈仍在线性级 |
| 5（当前） | 线性级换神经网络控制的轻量卡尔曼（见下），两级共用一次 STFT，掩码直接使用线性级复谱 | 整链算力降到基线约 3–5 倍，算法延迟 24 ms |

### 当前技术栈

| 部分 | 实现 |
|---|---|
| 分析 / 合成 | STFT 1536 点、跳 384（48 kHz 下 32 ms / 8 ms），sqrt-hann，两级共用一次 |
| 时延估计 | 多时延相干度与能量包络结合的在线估计，范围 0 – 约 1 s，带切换确认 |
| 线性级 | 每频点 8 抽头的对角卡尔曼滤波；噪声参数由小网络（32 个频带、GRU）逐帧估计；同时并行一套经典卡尔曼，逐频点取误差小者；参考信号因果电平归一 |
| 残余回声抑制 | CoFi-Lite（约 30.5 万参数）：输入线性级输出谱与线性回声估计谱，ERB 粗路 + 低频细路，输出逐频点增益（下限 −20 dB），相位不变 |
| 端上推理 | 手写 C++（无推理框架依赖），单帧、每帧零分配；FFT 用 pffft（NEON）；掩码网络中间的两个全连接层用 int8（Arm KleidiAI 的量化方式与点积内核，CPU 不支持点积指令时走算法相同的后备路径） |
| 训练 | PyTorch；回声模拟数据管线 |

---

## 版本与模型

- 当前：默认版 `aec48 0.7.0 (lin nlinBc3-dual, mask v59ae20)`；候选版 `aec48 0.7.0 (lin nlinBc3-dual, mask v62be20 candidate)`。
- 0.7.0 修改 **【联动】**（默认版、候选版处理结果不变）：频谱回调契约升至 2，增加 `aec48_set_spec_hook2` 与 `Aec48SpecSide`，每帧提供处理模式、远端活动和残余回声功率；近端说话概率暂不提供。第 1 版回调行为不变，旧二进制不受影响；重编时契约检查应接受 `AEC48_SPEC_CONTRACT_VERSION >= 1`。录音 `meta.json` 的 `spec_hook` 增加 `contract_version`。
- 0.6 修改（默认版处理结果不变：三个测试向量输出与 0.5.2 逐字节相同；接口、延迟、频谱回调契约不变，契约版本仍为 1）：
  - 新增候选版预编译库 `prebuilt/candidate-v62b/`（见「快速开始 · 候选版」）。掩码内核支持带「残余回声提示」的模型（参数文件带 `hint_v1` 时自动启用），处理链为它多算一路对齐参考频谱（每块多一次 FFT）。
  - 对齐参考：参考按线性级每帧的时延挪齐后做同一 STFT。窗内最后一跳的时延在当帧还没估出，用当帧的时延代替；时延不变时与训练 / 评估的算法逐样本相同，只在时延改判前后一两帧有差别。
  - 核对：掩码内核与训练框架里的模型逐帧比，最大差 5×10⁻⁵（输出幅度 17）；故意喂错对齐参考时差到 1，说明核对能发现输入错误。对齐参考频谱与评估流程逐帧比，时延相同的帧差值能量低 120 dB 以上。
  - 测试：候选版另有三条期望输出（`test/vec/*_v62b.pcm`）；`aec48_robust`、`aec48_hook`、`aec48_oom` 两版都全部通过。
  - `tools/embed_models.py` 可指定输出文件；CMake 加 `AEC48_MODELS` 选内嵌模型；`tools/build_android.sh` 同时编两版。
  - 独立审查后修正：默认版掩码每帧对空缓冲取了 `&hm[0]`（不读它，普通构建无影响；开 `_GLIBCXX_ASSERTIONS` 等 STL 检查的构建会中止），改为不取下标；父工程先 `set(AEC48_MODELS …)` 再 `add_subdirectory` 时，原写法会被同名缓存项覆盖回默认模型，现保留父工程的设置（两种接入方式都已实测编出对应模型）。
- 0.5.2 修改 **【联动】**（按 0.5.1 的独立复审；处理结果不变，只改说明与录音元数据；契约变更见联动接口一节）：
  - 帧时序说明更正：第 1 块不调用回调、从第 2 块起每块一次（0.5 / 0.5.1 误写为「前两块不调用」）；第 c 次调用的窗覆盖输入 [c·384 − 768, c·384 + 768)，torch 等价写法要先左补 768 个零。
  - 录音事件的位置说明更正并补字段：`pos` 所在的时间轴含被丢弃的段；输出从 `out_from` = `pos + latency − 1536` 就开始变化（0.5.1 误写为 `pos + latency`），到 `out_full` = `pos + latency − 384` 起完全切换，两个位置直接写进每条事件。
  - 测试：`aec48_hook` 加 H-12（按实际输出核对 `out_from` / `out_full`，不垫块与垫一块两种）；`aec48_robust` 的 M-06 在输入短于 450 块时循环取块（之前会越界读）。
  - 之后补订 **【联动】**（复审第三轮，库二进制不变）：关掉回调的说明更正为「输出经 3 块过渡后才与没挂时相同」（之前写成立即相同）；头文件加契约版本宏 `AEC48_SPEC_CONTRACT_VERSION`（= 1），README 把频谱回调整理成「⚠ 联动接口」专节并加契约变更记录；`aec48_hook` 的 H-8 / H-12 按输入长度取切换帧、输入不够时跳过（之前短输入会越界读）。
- 0.5.1 修改 **【联动】**（按 0.5 的独立审查）：
  - `aec48_set_enabled` / `aec48_set_spec_hook_enabled` 改为只写原子量。0.5 及更早在录音中调用会加锁并分配内存（`aec48_set_enabled` 从 0.1 起如此），在音频线程里调用有卡顿风险。录音事件改由处理线程在块边界记进预分配的表，`pos` 精确到生效的那一块（以前是调用开关时那次 `process` 的末尾），新增 `events_overflow`。
  - 「回调前」那路只在挂着回调时维护（0.5 不挂时也有一次拷贝），摘掉后再维护 4 块：0.5 在摘掉后马上重新挂上时，从回调后的状态拷贝，`out_prehook.wav` 开头 3 块会偏。
  - 同一目录先挂回调录、再不挂回调录时，`aec48_dump_start` 删掉上次的 `out_prehook.wav`；`Aec48Uploader` 只在本次 `meta.json` 记了挂着回调时才传它。
  - 开销（小核 cpu0 全速，每块平均）：不挂回调时与 0.4.3 交替各 4 轮，S10 1.240 / 1.242 ms、vivo 1.979 / 1.975 ms，分不出差别；挂恒等回调比不挂多 0.03–0.07 ms（S10 +2.5%、vivo +3.5%，`aec48_hook … bench` 单次读数，有 ±1–2% 抖动）。
  - 测试：`aec48_hook` 加 H-8（多块一次的调用里切换，事件落在生效块）、H-9（同目录重录）、H-10 / H-11（摘掉后马上 / 隔 10 块再挂上）；`aec48_robust` 加 M-06（录音中拨开关 + 处理，`operator new` 次数为 0）；上传工具测试加遗留文件一项。不挂回调时输出仍与 0.4.3 逐字节相同。
- 0.5 修改 **【联动】**（模型不变）：新增频谱回调 `aec48_set_spec_hook` / `aec48_set_spec_hook_enabled` / `aec48_spec_format`，见「⚠ 联动接口：频谱回调」；挂着回调时录音另写 `out_prehook.wav`，`meta.json` 加 `spec_hook`；`Aec48Uploader` 有该文件时一并上传。不挂回调时处理结果与 0.4.3 逐字节相同（三个测试向量 × 块长 384 / 480 / 1024，主机）。新增 `test/aec48_hook`（主机、S10、vivo 的 arm64 与 S10 的 armv7 全部通过）。
- 0.4.3 修改：pffft 在建 FFT 时没有检查内存分配失败，内存不足时 `aec48_create` 会使宿主进程崩溃（应返回 NULL）。已补上检查（对 pffft 的修改见 THIRD_PARTY_NOTICES）。新增 `test/aec48_oom`（Linux 主机）：让建实例过程中的每一次 `malloc` 逐次失败，确认都返回 NULL、不崩溃。`aec48_robust` 替换的是 `operator new`，拦不到 C 代码里的 `malloc`，所以之前没测出来。处理结果与 0.4.2 相同。
- 0.4.2 修改：编译时把源码目录映射成 `.`（`-ffile-prefix-map`），预编译库里不再带构建机的绝对路径（0.4.1 及更早的库里，pffft 的断言写进了源文件的绝对路径）；线性级 sigmoid 的参数限到 ±80，避免个别编译器 / 数学库组合下 `expf` 溢出取到错值（正常输入不触及，输出不变）。
- 0.4.1 修改：点积指令只按系统声明（`HWCAP_ASIMDDP`）判断。0.4 在系统不声明时还会按 `/proc/cpuinfo` 的核心型号推断，但 Cortex-A55 / A75 的点积指令是可选配置，型号对得上也可能没有，误用会触发非法指令并结束宿主进程；现在系统不声明的设备一律走后备路径（输出相同，掩码网络全连接层稍慢，见性能表 vivo 一行）。
- 0.4 修改：
  - 掩码网络中间两个全连接层改用 int8：Arm KleidiAI 的量化方式（输入每帧动态非对称量化、权重按行对称量化）。CPU 支持点积指令时用 KleidiAI 的点积内核，不支持时（Cortex-A53 等、armv7、x86）走算法相同的后备路径，各设备结果一致。点积指令的判断方式见 0.4.1。效果在 AEC Challenge 盲测集与近端说话 / 唱歌测试集上与 0.3 无差别（漏消、误消、近端误压、齿音、AECMOS 各项都在测试波动内）；三个测试向量与期望输出的差值能量为 −47～−53 dB（浮点舍入量级之外的部分来自量化）。小核每块耗时约降 10–15%，见上面的性能表。
  - 最低系统版本从 Android 8.0（API 26）降到 Android 5.0（API 21）：内嵌模型的内存读取改用各版本都有的 `funopen`（0.3 的 `fmemopen` 要 API 23，且 API 26 构建引用了 Android 7.0 才有的 `__write_chk`，在 Android 6.0 上加载失败）。已在 Android 6.0 真机上通过自检与健壮性测试。
  - `aec48_bench` 可边录音边测（`AEC48_DUMP_DIR`）。
- 0.3 修改（模型不变；三个测试向量结果与 0.2 相同，差值能量在浮点舍入范围内）：
  - 提速：掩码网络的逐点卷积改为 4 输出通道 × 4 频点一组在寄存器里累加，深度卷积先补零再算（去掉逐点边界判断）。小核每块耗时约降 10–20%，见上面的性能表。
  - `aec48_process_s16` 不再按 4096 样本内部分段：0.2 在声明块长为 384 的整数倍且单次超过 4096 样本时（如 4608、12288）会插入零样本、打乱时间对齐。现在与 float 版走同一处理循环，输出逐样本一致。
  - 录音写盘直接写文件描述符、只计确认落盘的字节：写盘出错时 wav 头、`meta.json` 与文件实际内容一致（0.2 会多报缓冲里没写出去的部分）。
  - 录音用到的缓冲全部在 `aec48_dump_start` 里分配，分配失败返回 −3；写盘线程不再分配内存，也不会让异常逃出线程结束宿主进程。
  - 丢弃位置表合并相邻段、上限 1024 段，超出时 `meta.json` 给出 `dropped_overflow` 与 `dropped_samples`（0.2 超过 256 条后静默截断）。
  - 模型加载中途失败时关闭模型流（0.2 会留下未关闭的流）。
  - 上传工具：已上传标记绑定 `session_id` 与文件内容 CRC32（0.2 只看大小与修改时间，换会话会被旧标记跳过）。
  - 测试：`aec48_robust` 增加上述各项；`aec48_check` 检查 `process_s16` 返回值；新增 `test/Aec48UploaderTest.java`（主机 Java 即可运行）。
- 0.2 修改（模型与算法输出不变，三个测试向量结果与 0.1 相同）：
  - FFT 资源随实例释放（0.1 每次创建 / 销毁泄漏约 25 KB）。
  - `aec48_process` 长度不受限：0.1 单次超过 32768 样本会静默丢失输出；现在内部分块、边处理边取输出，同址处理安全。
  - 录音三路整包写入、整包丢弃并记录位置，三个文件始终等长（0.1 中途开始 / 停止或写不及时可能错位）。
  - 处理期不再分配内存（0.1 时延估计首次决策时有一次分配）。
  - int16 接口取消 8192 样本上限。
  - 写盘错误可查（`aec48_dump_status`、`meta.json` 的 `write_error`），样本计数只计成功写入的部分。
  - 初始化失败统一返回 NULL / 错误码，不再退出宿主进程；内嵌模型只注册一次，多线程并发创建安全。
  - 关闭回声消除时残余抑制也照常运行，切回开时无需预热。
  - 上传工具：缺文件计为失败；已上传标记与文件大小、修改时间绑定。
  - 新增 `test/aec48_robust`。
- 换模型：把新的模型文件放进 `models/`，运行 `tools/embed_models.py <线性级模型> <掩码模型> "<模型标识>" [输出文件]` 重新生成 `src/models.cpp`（或另一个文件，再用 `-DAEC48_MODELS=` 选它），再编译。接口不变。

---

## 许可

本库源码、模型权重与预编译库不开源，见 [`LICENSE`](LICENSE)。第三方部分（pffft、KleidiAI、测试向量）按各自条款，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

**集成方注意：** `libaec48.so` 静态链接了 pffft 和 KleidiAI，分发带这个库的 App 时，要在 App 的开源许可 / 第三方声明页面里附上 `THIRD_PARTY_NOTICES.md` 中 pffft 那一段，以及 KleidiAI 的版权声明和 Apache License 2.0 全文。

---

## 已知限制

- 只处理单声道。
- 参考里没有的声音（例如其他 App 直接播放、未经本路混音的声音）消不掉。
- `mic` 与 `ref` 的时间差超过约 1 s 时对不齐。
- 回声特别强且回声路径变化快的段落可能有残留。
- 44.1 kHz 已用测试向量验证，48 kHz 已在真机全双工实时运行验证。44.1 kHz 下内部时间常数沿用与训练时相同的写法（按帧数计，换算成时间比 48 kHz 长约 9%），效果是否与 48 kHz 等价未单独评估。
- 只提供 C 接口；Java / Kotlin 直接调用需要自行写 JNI 包装。`example/duplex_example.cpp` 是调用位置示意，省略了错误处理，不能直接用于生产。
