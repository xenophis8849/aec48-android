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
| `src/` | 源码（`aec48.cpp` 接口实现，`aec_chain.h` 处理链，`fb/` 推理内核，`models.cpp` 内嵌模型，`models_v62b.cpp` 候选版内嵌模型） |
| `android/src/com/karaoke/aec48/Aec48Uploader.java` | 录音上传工具（可配置上传地址与库名） |
| `example/duplex_example.cpp` | AAudio 全双工接入示例 |
| `test/aec48_check.cpp`、`test/aec48_bench.cpp`、`test/aec48_robust.cpp`、`test/aec48_oom.cpp`、`test/aec48_hook.cpp`、`test/Aec48UploaderTest.java`、`test/vec/` | 自检程序、端上测速程序、接口健壮性测试、内存分配失败注入（Linux 主机）、频谱回调测试、上传工具测试、测试向量 |
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

### 从源码编译

```sh
ANDROID_NDK=/path/to/ndk tools/build_android.sh      # 产出默认版 prebuilt/<ABI>/ 与候选版 prebuilt/candidate-v62b/<ABI>/（arm64-v8a、armeabi-v7a）
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
| `int aec48_set_spec_hook(Aec48*, aec48_spec_hook fn, void* user)` | 挂上（`fn` 非空）或摘掉（`fn` = NULL）频谱回调，见「⚠ 联动接口：频谱回调」。只能在没有 `process` 进行中、也没在录音时调用。返回 0 成功、−1 参数无效、−2 检测到 `process` 正在进行、−3 正在录音。 |
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

**当前契约版本：1**（库 0.5.2 起；`AEC48_SPEC_CONTRACT_VERSION` = 1）。

### 用途与边界

本库内部按 STFT 逐帧处理。0.5 起可以挂一个回调：每帧输出频谱在合成回波形之前交给回调，回调原地修改。用途是把另一个工作在同一帧格式上的频域处理（例如降噪）直接接在回声消除后面，省掉一次「合成 → 再分析」，**不增加延迟**（整体仍是 `aec48_latency_samples`），也省一次正反变换。本库不依赖、也不链接任何后处理库；双方只靠下面的帧格式约定对接。

```c
int sr, nfft, hop, win;
aec48_spec_format(aec, &sr, &nfft, &hop, &win);      // 串接方据此核对自己的格式，对不上就不要挂
aec48_set_spec_hook(aec, my_spec_fn, my_state);       // 开始处理前挂好
...
void my_spec_fn(void* user, float* re, float* im, int bins) { /* 原地修改 re[0..bins)、im[0..bins) */ }
```

### 帧格式（契约版本 1）

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

### 契约变更记录

契约版本从 0.5.2 补订起正式编号；在此之前的 0.5 / 0.5.1 / 0.5.2 统一算作版本 1（接口签名没变，行为与说明的变化都列在下表）。以后按本节开头的规则：改一项就加一行，并提升版本号。

| 契约版本 | 库版本 | 变更 | 对串接方的影响 |
|---|---|---|---|
| 1 | 0.5 | **【联动】** 新增频谱回调、帧格式查询、回调开关 | 首次提供 |
| 1 | 0.5.1 | **【联动】** 两个开关改为只写原子量，可在音频线程调；录音事件改由处理线程按块记录 | 行为更安全，接口不变；录音事件的 `pos` 含义变了（见 0.5.2） |
| 1 | 0.5.2 | **【联动】说明更正（行为不变）**：① 第 1 块不调用、第 2 块起每块调用（之前误写「前两块不调用」）；② 第 c 次调用的窗覆盖输入 [c·384 − 768, c·384 + 768)，torch 等价写法要先左补 768 个零（之前漏了 `F.pad`）；③ 关掉回调后输出有 3 块过渡才与没挂时相同（之前误写「立即相同」）；④ 录音事件加 `out_from` / `out_full` | 按旧说明做逐帧核对的，帧起点会差 768 个样本；按旧说明做「立即旁路」的，要算上 3 块过渡。训练数据只用帧内容、不受①②影响 |

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
| `meta.json` | `version`、`sample_rate`、`latency_samples`、`samples`、`samples_mic_ref_out`（三个文件的样本数，正常时相等）、`start_unix_ms`（开始录音的时刻）、`write_error`（1 = 有写盘错误）、`enabled_at_start`、`events`（录音期间两个开关的切换：`{"pos": 样本位置, "enabled": 0/1}` 或 `{"pos": 样本位置, "spec_hook_enabled": 0/1}`。由处理线程在切换生效的那一块记录。`pos` = 那一块输入在录音时间轴上的末尾（时间轴含被丢弃的段，与 `dropped` 的 `pos` 同一条；没有丢弃时就是 `mic.wav` 样本序号）。这一块算出的帧当场参与输出合成、且覆盖最近 4 块，所以 `out.wav` 从 `out_from` = `pos + latency − 1536` 开始变化，到 `out_full` = `pos + latency − 384` 起完全是新状态，中间是重叠相加的过渡；两个位置都直接写在事件里。最多记 1024 条，更多的计入 `events_overflow`）、`dropped` / `dropped_calls` / `dropped_samples`（后台写不及时整包丢弃的位置、次数与样本数，正常为空 / 0 / 0）、`dropped_overflow`（见下，正常为 `null`）、`spec_hook`（`attached_at_start`：录音开始时是否挂着回调；挂着时另有 `enabled_at_start` 与 `samples_out_prehook`；录音期间 `aec48_set_spec_hook_enabled` 的切换记在 `events`，形如 `{"pos": 样本位置, "spec_hook_enabled": 0/1}`） |

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

`test/aec48_hook` 检查频谱回调（主机与手机都能跑，全部通过返回 0）：格式查询；直通时回调拿到的频谱与按上表格式自己算的 DFT 一致（相对误差 ≤ 1e−4）；恒等回调输出与不挂时逐样本相同、每帧恰好调用一次；回调乘 0.5 / 置零时输出相应变化；从一开始就关着等于没挂、中途打开后与一直开着一致、运行中关掉后从 `out_full` 起等于没挂（H-12）；录音中挂接返回 −3；挂着回调录音时 `out.wav` / `out_prehook.wav` 分别等于回调后 / 回调前的输出。第 4 个参数写 `bench` 时只测挂与不挂的每块耗时。

```sh
./build-host/aec48_hook test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm 48000 /tmp/aec48_hook_dir   # 目录需已存在
```

上传工具的测试（主机上有 JDK 即可，会在本机起一个临时 HTTP 服务）：

```sh
javac -d /tmp/aec48up android/src/com/karaoke/aec48/Aec48Uploader.java test/Aec48UploaderTest.java && java -cp /tmp/aec48up Aec48UploaderTest
```

---

## 性能

### 测法

`test/aec48_bench`：按真实节奏每 8 ms 送一块（48 kHz、384 样本），用 `taskset` 把进程钉在指定核心上跑 30 s，统计处理线程的 CPU 时间 / 墙钟时间（= 该核心被占用的比例），以及每块处理耗时与超时（单块超过 8 ms）次数。另以全速（不睡眠）跑 10 s 量实时率。按真实节奏时核心会降频，所以占用率比全速实时率高，前者才反映实际运行时的占用。Galaxy S10 息屏进入打盹状态时，系统不允许把进程钉到中 / 大核（`taskset` 报 Invalid argument），测这几档时须点亮屏幕。

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

- 候选版（0.6）：手机上还没测。它每块比默认版多一次 FFT 和 48 个频带的递推，电脑上（x86，全速）每块平均 0.10 vs 0.10 ms、p99 0.20 vs 0.19 ms，分不出差别；手机上请用候选版的 `aec48_bench` 按上面的测法测一遍。
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

- 当前：默认版 `aec48 0.6.0 (lin nlinBc3-dual, mask v59ae20)`；候选版 `aec48 0.6.0 (lin nlinBc3-dual, mask v62be20 candidate)`。
- 0.6 修改（默认版处理结果不变：三个测试向量输出与 0.5.2 逐字节相同；接口、延迟、频谱回调契约不变，契约版本仍为 1）：
  - 新增候选版预编译库 `prebuilt/candidate-v62b/`（见「快速开始 · 候选版」）。掩码内核支持带「残余回声提示」的模型（参数文件带 `hint_v1` 时自动启用），处理链为它多算一路对齐参考频谱（每块多一次 FFT）。
  - 对齐参考：参考按线性级每帧的时延挪齐后做同一 STFT。窗内最后一跳的时延在当帧还没估出，用当帧的时延代替；时延不变时与训练 / 评估的算法逐样本相同，只在时延改判前后一两帧有差别。
  - 核对：掩码内核与训练框架里的模型逐帧比，最大差 5×10⁻⁵（输出幅度 17）；故意喂错对齐参考时差到 1，说明核对能发现输入错误。对齐参考频谱与评估流程逐帧比，时延相同的帧差值能量低 120 dB 以上。
  - 测试：候选版另有三条期望输出（`test/vec/*_v62b.pcm`）；`aec48_robust`、`aec48_hook`、`aec48_oom` 两版都全部通过。
  - `tools/embed_models.py` 可指定输出文件；CMake 加 `AEC48_MODELS` 选内嵌模型；`tools/build_android.sh` 同时编两版。
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
