# aec48

实时全带回声消除库（Android，C 接口）。单声道，48 kHz / 44.1 kHz，算法延迟 24 ms。

处理链：神经网络控制的线性自适应滤波器（含在线时延估计）→ 神经网络残余回声抑制，两级共用一次 STFT。模型编进库里，接入只需要一个 `.so` 和一个头文件。

---

## 目录

| 路径 | 内容 |
|---|---|
| `include/aec48.h` | 全部接口（C） |
| `prebuilt/arm64-v8a/libaec48.so`、`prebuilt/armeabi-v7a/libaec48.so` | 预编译库（NDK r28，minSdk 26，c++_static） |
| `src/` | 源码（`aec48.cpp` 接口实现，`aec_chain.h` 处理链，`fb/` 推理内核，`models.cpp` 内嵌模型） |
| `android/src/com/karaoke/aec48/Aec48Uploader.java` | 录音上传工具（可配置上传地址与库名） |
| `example/duplex_example.cpp` | AAudio 全双工接入示例 |
| `test/aec48_check.cpp`、`test/aec48_bench.cpp`、`test/aec48_robust.cpp`、`test/Aec48UploaderTest.java`、`test/vec/` | 自检程序、端上测速程序、接口健壮性测试、上传工具测试、测试向量 |
| `tools/build_android.sh`、`tools/embed_models.py` | 编译脚本、模型内嵌脚本 |
| `third_party/pffft/` | FFT 库（BSD 类许可，见其 `LICENSE.txt`） |
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

### 从源码编译

```sh
ANDROID_NDK=/path/to/ndk tools/build_android.sh      # 产出 prebuilt/arm64-v8a、prebuilt/armeabi-v7a
```

也可以直接把本仓库作为子目录 `add_subdirectory()` 进自己的 CMake 工程（目标名 `aec48`）。

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
- `aec48_set_enabled`、`aec48_dump_start/stop`、`aec48_stats` 可以在任意线程调。
- 多路音频各建一个实例，实例之间互不影响。

---

## 接入要点

1. **麦克风要用原始采集。** 不要用带系统回声消除的采集方式（AAudio / OpenSL 的 `VOICE_COMMUNICATION` 预设、`AudioManager.MODE_IN_COMMUNICATION` 等），否则两层处理叠加。推荐输入预设 `GENERIC` 或 `UNPROCESSED`。
2. **参考信号 `ref` = 扬声器实际播放的最终声音**，也就是所有要播出去的声音（远端人声、背景音乐、提示音……）混完之后、送进播放设备的那一路。没进参考的声音消不掉。
3. **两路不用调用方对齐。** `mic` 与 `ref` 之间的时间差由库在线估计（0 – 约 1 s），只要求：同一台设备、同一采样率、每次样本数相同、按时间顺序连续送入。最简单的做法是在播放回调里同时读麦克风、记下本次播放的样本（见 `example/duplex_example.cpp`）。
4. **调用位置：** 采集之后、后续处理之前（降噪、自动增益、编码、混音等都放在 `out` 之后）。
5. **起步：** 有回声后约 0.5 s 内收敛。回声路径变化（换位置、换路由等）时库在线重新适应，不需要重建实例；切换扬声器 / 听筒 / 耳机等路由的场景尚未专门测试。
6. **单声道。** 立体声采集取一路或先下混。

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
| `meta.json` | `version`、`sample_rate`、`latency_samples`、`samples`、`samples_mic_ref_out`（三个文件的样本数，正常时相等）、`start_unix_ms`（开始录音的时刻）、`write_error`（1 = 有写盘错误）、`enabled_at_start`、`events`（录音期间 `aec48_set_enabled` 的切换：`{"pos": 样本位置, "enabled": 0/1}`）、`dropped` / `dropped_calls` / `dropped_samples`（后台写不及时整包丢弃的位置、次数与样本数，正常为空 / 0 / 0）、`dropped_overflow`（见下，正常为 `null`） |

三路按"一次 `process` 调用"整包写入：后台写不及时时，这次调用的三路一起丢弃，并记进 `dropped`（`pos` 为录音时间轴上的样本位置，`n` 为丢的样本数；紧挨着的几次丢弃合并成一段），所以三个文件始终等长、互相对齐。`dropped` 最多记 1024 段；更多时 `dropped_overflow` 给出 `{"from_pos": 第一个没记下的位置, "calls": 之后的丢弃次数, "samples": 之后丢的样本数}`——`from_pos` 之前的时间轴可以按 `dropped` 完整还原，之后只知道合计。`aec48_dump_stop` 可以在任意线程调用，它会等正在进行的那次 `process` 写完再收尾。写盘出错（如磁盘满）时 `aec48_dump_status` 返回 −4，已落盘的部分保留，wav 头与 `meta.json` 里的样本数和文件实际内容一致。建议每次录音使用一个新目录。

### 上传（可配置）

`Aec48Uploader` 把一个录音目录里的 4 个文件逐个上传。需要配置两项：

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

通过标准：整段消除量与期望相差 ≤ 1 dB，且与期望输出的差值能量低 ≥ 20 dB。不同 CPU 的浮点差会让自适应滤波器走出略有不同的轨迹，所以不要求逐样本相同。期望输出与写出的输出都已按延迟挪齐到 mic 的时间轴。测试向量取自公开的 AEC Challenge 盲测集（远端单讲），int16 原始 PCM，单声道。

设置环境变量 `AEC48_DUMP_DIR=<目录>` 时，自检会同时验证录音功能（中途切一次直通，`meta.json` 里应有一条 `events`）。

`test/aec48_robust` 检查接口边界与故障路径（主机与手机都能跑，全部通过返回 0）：反复创建 / 销毁不漏内存；超长单次调用与分块处理逐样本一致；原地处理（`out` = `mic`）；录音中途从另一线程开始 / 停止时三个文件等长、wav 头与文件一致；处理期不分配内存；int16 超过 8192 样本；int16 大块（4608、12288，含原地）与 float 384 块逐样本一致；声明块长与实际不符时返回 1；写盘失败时 `aec48_dump_status` = −4 且文件长度、wav 头、`meta.json` 三者一致；录音缓冲分配失败返回 −3 且进程不退出；丢弃超过 1024 段时的合并与溢出记录；不支持的采样率返回 NULL；多线程并发创建。主机版用替换 `operator new` 的方式做分配计数与故障注入，第一项会先确认这种替换能拦到库内部的分配。

```sh
./build-host/aec48_robust test/vec/03ra86gHhkWD_mic.pcm test/vec/03ra86gHhkWD_ref.pcm /tmp/aec48_robust_dir
```

也可以用 ThreadSanitizer 构建跑这个测试查线程竞态（`-DCMAKE_CXX_FLAGS=-fsanitize=thread` 等，0.3 无告警）。

上传工具的测试（主机上有 JDK 即可，会在本机起一个临时 HTTP 服务）：

```sh
javac -d /tmp/aec48up android/src/com/karaoke/aec48/Aec48Uploader.java test/Aec48UploaderTest.java && java -cp /tmp/aec48up Aec48UploaderTest
```

---

## 性能

### 测法

`test/aec48_bench`：按真实节奏每 8 ms 送一块（48 kHz、384 样本），用 `taskset` 把进程钉在指定核心上跑 30 s，统计处理线程的 CPU 时间 / 墙钟时间（= 该核心被占用的比例），以及每块处理耗时与超时（单块超过 8 ms）次数。另以全速（不睡眠）跑 10 s 量实时率。按真实节奏时核心会降频，所以占用率比全速实时率高，前者才反映实际运行时的占用。

```sh
adb shell 'cd /data/local/tmp/aec48 && LD_LIBRARY_PATH=. taskset <核心掩码> ./aec48_bench 03ra86gHhkWD_mic.pcm 03ra86gHhkWD_ref.pcm 48000 30 1'
```

### 结果（0.3，48 kHz，每块 8 ms，回声消除开）

| 设备（SoC） | 核心 | ABI | 核心占用 | 每块耗时 平均 / p99 / 最大（ms） | 超时块 | 全速实时率 |
|---|---|---|---|---|---|---|
| Galaxy S10 SM-G9730（骁龙 855） | 中核 Kryo 485 Gold 2.42 GHz（A76 级） | arm64-v8a | 14.7% | 1.17 / 1.52 / 1.9 | 0 | 0.034 |
| | | armeabi-v7a | 19.2% | 1.53 / 1.96 / 2.2 | 0 | 0.048 |
| | 小核 Kryo 485 Silver 1.79 GHz（A55 级） | arm64-v8a | 46.8% | 3.89 / 6.28 / 17.2 | 16 / 3750 | 0.178 |
| | | armeabi-v7a | 62.7% | 5.16 / 7.12 / 19.0 | 26 / 3750 | 0.257 |
| | 不绑核（系统调度） | arm64-v8a | 46.3% | 3.82 / 6.16 / 16.3 | 14 / 3750 | 0.029 |
| | | armeabi-v7a | 62.1% | 5.11 / 7.49 / 15.6 | 30 / 3750 | 0.041 |
| vivo V1934A（Helio P65） | 大核 Cortex-A75 2.0 GHz | arm64-v8a | 19.5% | 1.51 / 1.59 / 1.8 | 0 | 0.099 |
| | | armeabi-v7a | 23.7% | 1.82 / 1.95 / 2.9 | 0 | 0.115 |
| | 小核 Cortex-A55 1.7 GHz | arm64-v8a | 50.6% | 3.97 / 4.62 / 5.5 | 0 | 0.273 |
| | | armeabi-v7a | 64.7% | 5.12 / 5.57 / 6.3 | 0 | 0.354 |

- 内存：每个实例常驻约 5.3–5.7 MB；开录音（`aec48_dump_start`）后另加约 6 MB 录音缓冲（第一次录音时分配）。内嵌模型在库文件里：arm64 1.9 MB、armv7 1.7 MB。
- S10 的超大核（Kryo 485 Prime）普通进程不能用 `taskset` 绑定，未单独测；"不绑核"一行全速时实时率 0.029，说明全速时被调度到大核，而按真实节奏时负载低、被调度在小核上（占用与小核一行相同）。
- 小核上平均耗时约 3.9 ms（每块预算 8 ms），偶有单块超过 8 ms（S10 小核约 0.4%）。处理放在音频回调里时，建议给处理线程留至少一块的缓冲，或让处理线程跑在大 / 中核上（例如使用 Android 性能提示接口 `APerformanceHint`、提高线程优先级）。
- 64 位比 32 位省 15–25%，优先用 arm64-v8a。
- 边录音边处理（`AEC48_DUMP_DIR` 设给 `aec48_bench`，写盘线程与处理绑在同一个小核上，按真实节奏 300 s）：S10 超时块 0.46% → 0.57%、平均耗时 3.95 → 4.13 ms；vivo 超时块 9 → 3 次（无差别）；两台都没有丢弃，三个文件等长。
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
| 端上推理 | 手写 C++（无推理框架依赖），单帧、每帧零分配；FFT 用 pffft（NEON） |
| 训练 | PyTorch；回声模拟数据管线 |

---

## 版本与模型

- 当前：`aec48 0.3 (lin nlinBc3-dual, mask v59ae20)`。
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
- 换模型：把新的模型文件放进 `models/`，运行 `tools/embed_models.py <线性级模型> <掩码模型> "<模型标识>"` 重新生成 `src/models.cpp`，再编译。接口不变。

---

## 许可

本库源码、模型权重与预编译库不开源，见 [`LICENSE`](LICENSE)。第三方部分（pffft、测试向量）按各自条款，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

**集成方注意：** `libaec48.so` 静态链接了 pffft，分发带这个库的 App 时，要在 App 的开源许可 / 第三方声明页面里附上 `THIRD_PARTY_NOTICES.md` 中 pffft 那一段。

---

## 已知限制

- 只处理单声道。
- 参考里没有的声音（例如其他 App 直接播放、未经本路混音的声音）消不掉。
- `mic` 与 `ref` 的时间差超过约 1 s 时对不齐。
- 回声特别强且回声路径变化快的段落可能有残留。
- 44.1 kHz 已用测试向量验证，48 kHz 已在真机全双工实时运行验证。44.1 kHz 下内部时间常数沿用与训练时相同的写法（按帧数计，换算成时间比 48 kHz 长约 9%），效果是否与 48 kHz 等价未单独评估。
- 只提供 C 接口；Java / Kotlin 直接调用需要自行写 JNI 包装。`example/duplex_example.cpp` 是调用位置示意，省略了错误处理，不能直接用于生产。
