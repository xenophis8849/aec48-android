// 接入示例：AAudio 全双工里怎么调 aec48（只示意调用位置，省略错误处理）。
// 要点：① 麦克风用不带系统回声消除的采集（输入预设 GENERIC / UNPROCESSED，不要 VOICE_COMMUNICATION）；
//       ② ref = 扬声器实际播放的最终混音（所有要播出去的声音混完之后的那一路），每次与 mic 同样多的样本；
//       ③ mic 与 ref 的时间差由库自己估（0 – 约 1 s），调用方不用对齐，但两路必须来自同一台设备、同一采样率。
// 这里在播放回调里先非阻塞读走全部麦克风样本、再播放，读到的麦克风与刚播出的参考按样本数一一配对（同一时钟，锁步）。
#include <aaudio/AAudio.h>
#include <vector>
#include "aec48.h"

static AAudioStream *gIn, *gOut;
static Aec48* gAec;
static std::vector<float> gMic(8192), gRef(8192), gClean(8192);

void app_render_playout(float* dst, int n);          // 由接入方实现：本次要播放的最终混音
void app_send_captured(const float* clean, int n);   // 由接入方实现：消除回声后的采集信号（编码 / 发送 / 录制……）

static aaudio_data_callback_result_t onPlay(AAudioStream*, void*, void* audio, int32_t n) {
    float* out = (float*)audio;
    app_render_playout(out, n);                                   // 要播放的声音
    int got = AAudioStream_read(gIn, gMic.data(), n, 0);          // 非阻塞读麦克风（示意：每次读与播放同样多）
    if (got < n) for (int i = got > 0 ? got : 0; i < n; ++i) gMic[i] = 0.f;
    for (int i = 0; i < n; ++i) gRef[i] = out[i];                 // 参考 = 实际播出的样本
    aec48_process(gAec, gMic.data(), gRef.data(), gClean.data(), n);   // 处理（可在回调里调：不分配、不加锁、不读写文件）
    app_send_captured(gClean.data(), n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool start_duplex() {
    AAudioStreamBuilder* b;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setSampleRate(b, 48000); AAudioStreamBuilder_setChannelCount(b, 1); AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT); AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_GENERIC);
    AAudioStreamBuilder_openStream(b, &gIn);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT); AAudioStreamBuilder_setDataCallback(b, onPlay, nullptr);
    AAudioStreamBuilder_openStream(b, &gOut); AAudioStreamBuilder_delete(b);
    gAec = aec48_create(AAudioStream_getSampleRate(gOut), 0);    // 回调块长不固定，填 0
    if (!gAec) return false;
    AAudioStream_requestStart(gIn); AAudioStream_requestStart(gOut);
    return true;
}

void stop_duplex() {
    AAudioStream_requestStop(gOut); AAudioStream_requestStop(gIn); AAudioStream_close(gOut); AAudioStream_close(gIn);
    aec48_destroy(gAec); gAec = nullptr;
}
