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
| `test/aec48_check.cpp`、`test/vec/` | 自检程序与测试向量 |
| `tools/build_android.sh`、`tools/embed_models.py` | 编译脚本、模型内嵌脚本 |
| `third_party/pffft/` | FFT 库（BSD 类许可，见其 `LICENSE.txt`） |

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
| `Aec48* aec48_create(int sample_rate, int frames_per_call)` | 建实例。`sample_rate` 只支持 48000、44100。`frames_per_call` 是之后每次 `process` 的样本数；是 384 的整数倍时延迟最低（见下表），不固定就填 0。失败返回 NULL。 |
| `int aec48_process(Aec48*, const float* mic, const float* ref, float* out, int n)` | 处理 `n` 个样本。float，满幅 ±1.0。`n` 任意正数；`out` 可以和 `mic` 是同一块内存。返回 0 成功。 |
| `int aec48_process_s16(Aec48*, const short* mic, const short* ref, short* out, int n)` | 16 bit 版，单次 `n` ≤ 8192。 |
| `void aec48_set_enabled(Aec48*, int on)` | 1 = 回声消除开（默认）；0 = 直通（输出麦克风原声，延迟不变）。下一块生效，不清内部状态，切回开时无需重新收敛。 |
| `int aec48_latency_samples(const Aec48*)` | `out` 相对 `mic` 的固定延迟（样本数）。 |
| `int aec48_dump_start(Aec48*, const char* dir)` / `void aec48_dump_stop(Aec48*)` | 录音：把 mic / ref / out 写成 `dir` 下的 `mic.wav`、`ref.wav`、`out.wav`（16 bit 单声道），停止时另写 `meta.json`。见「录音与上传」。 |
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
| `meta.json` | `version`、`sample_rate`、`latency_samples`、`samples`、`start_unix_ms`（开始录音的时刻）、`dump_dropped_blocks`（后台没写及时丢的块，正常应为 0）、`enabled_at_start`、`events`（录音期间 `aec48_set_enabled` 的切换：`{"pos": 样本位置, "enabled": 0/1}`） |

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

失败的文件留在目录里，再调一次 `upload` 会补传；已成功的文件会写 `<文件名>.uploaded` 标记并跳过。同一目录多次上传沿用同一个 `session_id`（存在目录里的 `.session_id`）。需要 `INTERNET` 权限。

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

---

## 性能

| 设备 | ABI | 实时率（单核，越小越省） | 每 8 ms 块耗时 |
|---|---|---|---|
| Galaxy S10（Exynos 9820） | arm64-v8a | 0.032 | 平均 ≈ 0.26 ms |
| Galaxy S10（Exynos 9820） | armeabi-v7a | 0.049 | 平均 0.37 ms，最大 3.7 ms |

库体积：arm64 1.9 MB、armv7 1.7 MB（含模型）。

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

- 当前：`aec48 0.1 (lin nlinBc3-dual, mask v59ae20)`。
- 换模型：把新的模型文件放进 `models/`，运行 `tools/embed_models.py <线性级模型> <掩码模型> "<模型标识>"` 重新生成 `src/models.cpp`，再编译。接口不变。

---

## 已知限制

- 只处理单声道。
- 参考里没有的声音（例如其他 App 直接播放、未经本路混音的声音）消不掉。
- `mic` 与 `ref` 的时间差超过约 1 s 时对不齐。
- 回声特别强且回声路径变化快的段落可能有残留。
- 44.1 kHz 已用测试向量验证，48 kHz 已在真机全双工实时运行验证。
