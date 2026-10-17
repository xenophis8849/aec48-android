// 讲话档逐帧远端/近端判断门（与离线参考实现逐样本对齐）。
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace ac {
struct TalkGate {
    int H = 384, sr = 48000, farN = 126, talkN = 32;
    std::vector<unsigned char> farHist, talkHist;
    std::vector<float> gainHist;
    int farAt = 0, talkAt = 0, gainAt = 0, gainCount = 0;
    long long gainTotal = 0;   // 0.8.0 审查：32 位 ABI 上 long 只有 32 位，样本位置须 64 位
    int farCount = 0, talkCount = 0;
    float threshold = -3.0f, currentDb = -40.f;
    double upDb = 32.0, downDb = 40.0 / 18.75;
    bool initialized = false;

    void init(int hop, int sampleRate) {
        H = hop; sr = sampleRate;
        farN = (int)(sr / (double)H) + 1;               // dil(..., 125): 当前帧 + 过去 125 帧
        talkN = (int)(0.25 * sr / H) + 1;              // dil(..., 31): 当前帧 + 过去 31 帧
        farHist.assign(farN, 0); talkHist.assign(talkN, 0);
        gainHist.assign(farN + talkN + 16, 1.f);
        upDb = 40.0 / std::max(10.0 / (1000.0 * H / sr), 1.0);
        downDb = 40.0 / std::max(150.0 / (1000.0 * H / sr), 1.0);
        threshold = -3.0f;
        reset();
    }
    // 清掉休眠前的远端、近端与平滑历史。唤醒保护期内仍由 push() 更新历史。
    void reset() {
        std::fill(farHist.begin(), farHist.end(), 0);
        std::fill(talkHist.begin(), talkHist.end(), 0);
        std::fill(gainHist.begin(), gainHist.end(), 1.f);
        farAt = talkAt = gainAt = gainCount = farCount = talkCount = 0;
        gainTotal = 0; currentDb = -40.f; initialized = false;
    }
    // 唤醒时清门的判决与平滑状态，但保留全局帧号供 OLA 延迟输出对齐。
    void resetHistory() {
        std::fill(farHist.begin(), farHist.end(), 0);
        std::fill(talkHist.begin(), talkHist.end(), 0);
        std::fill(gainHist.begin(), gainHist.end(), 1.f);
        farAt = talkAt = farCount = talkCount = 0;
        currentDb = -40.f; initialized = false;
    }
    void setThreshold(float t) { threshold = t; }
    // 输出块有两帧 OLA 延迟；帧中心插值还会读到唤醒前的三个增益端点。
    void forceRecentOpen(int frames = 2) {
        const int n = std::min(frames, gainCount);
        const int size = (int)gainHist.size();
        for (int i = 1; i <= n; ++i) gainHist[(gainAt - i + size) % size] = 1.f;
    }
    float push(const float* ref, float logit, bool forceOpen = false) {
        double e = 0.0;
        for (int i = 0; i < H; ++i) e += (double)ref[i] * ref[i];
        e = e / H + 1e-15;
        const unsigned char f = e > 1e-7 ? 1 : 0;
        const unsigned char p = logit > threshold ? 1 : 0;
        farCount -= farHist[farAt]; farHist[farAt] = f; farCount += f;
        if (++farAt == farN) farAt = 0;
        talkCount -= talkHist[talkAt]; talkHist[talkAt] = p; talkCount += p;
        if (++talkAt == talkN) talkAt = 0;
        const float target = forceOpen ? 0.f : ((farCount && !talkCount) ? -40.f : 0.f);
        if (forceOpen) { currentDb = 0.f; initialized = true; }
        else if (!initialized) { currentDb = target; initialized = true; }
        else if (target > currentDb) currentDb = (float)std::min<double>(target, currentDb + upDb);
        else if (target < currentDb) currentDb = (float)std::max<double>(target, currentDb - downDb);
        const float g = std::pow(10.f, currentDb / 20.f);
        gainHist[gainAt] = g;
        gainAt = (gainAt + 1) % (int)gainHist.size();
        if (gainCount < (int)gainHist.size()) ++gainCount;
        ++gainTotal;
        return g;
    }
    float gainAtFrame(long long frame) const {
        if (gainCount == 0) return 1.f;
        if (frame < 0) frame = 0;
        const long long oldest = gainTotal - gainCount;
        if (frame < oldest) frame = oldest;
        if (frame >= gainTotal) frame = gainTotal - 1;
        const int start = (gainAt - gainCount + (int)gainHist.size()) % (int)gainHist.size();
        return gainHist[(start + (int)(frame - oldest)) % (int)gainHist.size()];
    }
    float gainAtSample(long long frame, int sample) const {
        if (sample < 0) sample = 0;
        if (sample >= H) sample = H - 1;
        const long long firstSample = (frame - 2) * (long long)H;
        const double pos = (firstSample + sample - H / 2.0) / H;
        if (pos <= 0.0) return gainAtFrame(0);
        const long long lo = (long long)std::floor(pos);
        const float frac = (float)(pos - lo);
        const float a = gainAtFrame(lo), b = gainAtFrame(lo + 1);
        return a + (b - a) * frac;
    }
    // Chain 第 t 帧完成时输出的是原始时间轴 [t−2]H..[t−1]H；插值中心在 jH+H/2。
    void apply(float* out, long long frame, int mode) const {
        if (mode == 0) return;
        for (int q = 0; q < H; ++q) out[q] *= gainAtSample(frame, q);
    }
};
} // namespace ac
