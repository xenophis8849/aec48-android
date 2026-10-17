// 讲话门独立对账工具。
// 用法：aec48_talk_gate --dump <input> <output>
// 输入：uint32 T，float32 ref[T*384]，float32 logit[T]；输出：float32 gain[T*384]，按 gate_wav.py 帧中心插值。
#include "../src/talk_gate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--selftest") {
        ac::TalkGate gate; gate.init(384, 48000); float ref[384]; std::fill(ref, ref + 384, 0.001f);
        const float first = gate.push(ref, -3.0f);
        const float second = gate.push(ref, std::nextafter(-3.0f, INFINITY));
        const float expectedFirst = 0.01f, expectedSecond = std::pow(10.f, -8.f / 20.f);
        if (std::abs(first - expectedFirst) > 1e-7f || std::abs(second - expectedSecond) > 1e-7f) {
            std::fprintf(stderr, "boundary failed first=%.9g second=%.9g\n", first, second); return 1;
        }
        std::printf("PASS strict logit threshold, remote-active frame, 32 dB opening step\n"); return 0;
    }
    if (argc != 4 || std::string(argv[1]) != "--dump") {
        std::fprintf(stderr, "usage: aec48_talk_gate --dump input output | --selftest\n"); return 2;
    }
    FILE* in = std::fopen(argv[2], "rb"); if (!in) { std::perror(argv[2]); return 2; }
    uint32_t T = 0;
    if (std::fread(&T, sizeof(T), 1, in) != 1 || T == 0 || T > 10000000) { std::fprintf(stderr, "invalid T\n"); std::fclose(in); return 2; }
    std::vector<float> ref((size_t)T * 384), logit(T), gain(T), out((size_t)T * 384);
    if (std::fread(ref.data(), sizeof(float), ref.size(), in) != ref.size() || std::fread(logit.data(), sizeof(float), T, in) != T) {
        std::fprintf(stderr, "truncated input\n"); std::fclose(in); return 2;
    }
    std::fclose(in);
    ac::TalkGate gate; gate.init(384, 48000);
    for (uint32_t t = 0; t < T; ++t) gain[t] = gate.push(ref.data() + (size_t)t * 384, logit[t]);
    for (uint32_t t = 0; t < T; ++t) {
        const float left = gain.front(), right = gain.back();
        for (int q = 0; q < 384; ++q) {
            const double pos = ((double)t * 384 + q - 192.0) / 384.0;
            if (pos <= 0.0) out[(size_t)t * 384 + q] = left;
            else if (pos >= (double)T - 1) out[(size_t)t * 384 + q] = right;
            else { const uint32_t lo = (uint32_t)std::floor(pos); const float f = (float)(pos - lo); out[(size_t)t * 384 + q] = gain[lo] + (gain[lo + 1] - gain[lo]) * f; }
        }
    }
    FILE* f = std::fopen(argv[3], "wb"); if (!f) { std::perror(argv[3]); return 2; }
    const size_t n = std::fwrite(out.data(), sizeof(float), out.size(), f); const int err = std::fclose(f);
    if (n != out.size() || err) { std::fprintf(stderr, "output write failed\n"); return 2; }
    std::printf("T=%u samples=%zu threshold_logit=-3.0\n", T, out.size()); return 0;
}
