#include "aec48.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#ifndef AEC48_SWITCH_WAKE_MS
#define AEC48_SWITCH_WAKE_MS 500
#endif
#ifdef AEC48_TESTING
extern "C" long long aec48_internal_talk_gate_frames(Aec48*);
extern "C" float aec48_internal_talk_gate_sample(Aec48*, long long, int);

static bool wakeSwitchApplySelftest(bool startInSing) {
    Aec48* a = aec48_create(48000, 384); if (!a) return false;
    bool ok = aec48_set_talk_gate(a, 1, 100.f) == 0;
    float mic[384], ref[384], out[384];
    auto process = [&](int block) {
        for (int q = 0; q < 384; ++q) {
            const int n = block * 384 + q;
            // Voiced synthetic near-end input plus an active far-end reference.
            mic[q] = .11f * std::sin(2.f * 3.14159265f * 230.f * n / 48000.f)
                   + .04f * std::sin(2.f * 3.14159265f * 460.f * n / 48000.f);
            ref[q] = .08f * std::sin(2.f * 3.14159265f * 173.f * n / 48000.f);
        }
        return aec48_process(a, mic, ref, out, 384) == 0;
    };
    int block = 0;
    if (startInSing) ok &= aec48_set_profile(a, AEC48_PROFILE_SING) == 0;
    else {
        // Start in talk and make the retained gate state closed while the remote is active.
        for (; block < 40 && ok; ++block) ok &= process(block);
        ok &= aec48_set_profile(a, AEC48_PROFILE_SING) == 0;
    }
    // Talk inference and its gate sleep while near-end speech continues in singing mode.
    for (int i = 0; i < 200 && ok; ++i, ++block) ok &= process(block);
    const long long wakeFrame = aec48_internal_talk_gate_frames(a);
    if (!startInSing) ok &= wakeFrame > 0;
    ok &= aec48_set_profile(a, AEC48_PROFILE_TALK) == 0;
    const int wFrames = (int)std::ceil(AEC48_SWITCH_WAKE_MS * 48000.0 / (1000.0 * 384));
    bool woke = false; int checkedFrames = 0;
    for (int i = 0; i < wFrames + 10 && ok; ++i, ++block) {
        const long long before = aec48_internal_talk_gate_frames(a);
        ok &= process(block);
        const long long after = aec48_internal_talk_gate_frames(a);
        if (after > before) {
            if (!woke) { woke = true; ok &= after == before + 1 && before == wakeFrame; }
            const long long wakeBlock = after - wakeFrame - 1;
            if (wakeBlock >= 0 && wakeBlock < wFrames) {
                // Chain::push applies after this block with frame=talkFrames-1.
                const long long applyFrame = after - 1;
                for (int q = 0; q < 384; ++q) {
                    const float g = aec48_internal_talk_gate_sample(a, applyFrame, q);
                    if (std::abs(g - 1.f) > 1e-6f) { ok = false; break; }
                }
                ++checkedFrames;
            }
        }
    }
    ok &= woke && checkedFrames == wFrames;
    aec48_destroy(a);
    return ok;
}
#endif

static std::vector<float> run(bool bypass, bool gate, float threshold) {
    Aec48* a = aec48_create(48000, 384);
    if (!a) return {};
    aec48_set_enabled(a, !bypass);
    aec48_set_talk_highpass(a, 0); // isolate the pre-existing talk-gate parity check from the new post-gate HPF
    aec48_set_talk_gate(a, gate, threshold);
    std::vector<float> y(384*350), m(384), r(384);
    for (int b=0;b<350;++b) {
        for (int i=0;i<384;++i) { int t=b*384+i; r[i]=.1f*std::sin(t*.17f); m[i]=.07f*std::sin(t*.13f); }
        if (aec48_process(a,m.data(),r.data(),y.data()+b*384,384)) return {};
    }
    aec48_destroy(a); return y;
}
int main() {
    Aec48* a=aec48_create(48000,384); if (!a) return 1;
    const bool talk=std::strstr(aec48_version(),"mask talk-")!=nullptr ||
                    std::strstr(aec48_version(),"mask sing-")!=nullptr; // dual 创建后为讲话档
    aec48_set_talk_highpass(a, 0);
    bool ok=aec48_set_talk_gate(nullptr,1,-2.75f)==-1;
    ok &= aec48_set_talk_gate(a,1,-2.75f)==(talk?0:-1);
    ok &= aec48_set_talk_gate(a,1,std::numeric_limits<float>::infinity())==-1;
    ok &= aec48_set_talk_gate(a,1,std::numeric_limits<float>::quiet_NaN())==-1;
    ok &= aec48_latency_samples(a)==1152;
    aec48_destroy(a);
    auto off=run(false,false,100), closed=run(false,true,100), opened=run(false,true,-100);
    auto pass=run(true,false,100), passGate=run(true,true,100);
    const auto equal=[](const auto& x,const auto& y) { return !x.empty()&&x.size()==y.size()&&!std::memcmp(x.data(),y.data(),x.size()*4); };
    ok &= equal(off,opened)&&equal(pass,passGate);
    if (talk) {
        double e=0, d=0;
        for (size_t i=1152;i<off.size();++i) { e+=double(off[i])*off[i]; double v=closed[i]-off[i]*.01f; d+=v*v; }
        ok &= d<1e-12*(e+1e-30);
    } else ok &= equal(off,closed);
#ifdef AEC48_TESTING
    const bool dual = std::strstr(aec48_version(), "mask sing-") != nullptr;
    if (dual) ok &= wakeSwitchApplySelftest(false) && wakeSwitchApplySelftest(true);
#endif
    std::printf("%s talk=%d: API/NaN/Inf/1152 latency/gate off/threshold/bypass%s\n",ok?"PASS":"FAIL",talk,
#ifdef AEC48_TESTING
                "/sing-to-talk W-window apply"
#else
                ""
#endif
    );
    return ok?0:1;
}
