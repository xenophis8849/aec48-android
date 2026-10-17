#include "aec48.h"
#include "fb/talk_hpf.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

static std::atomic<bool> countAlloc{false};
static std::atomic<unsigned long> allocs{0};
#if defined(__GLIBC__)
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* malloc(std::size_t n) {
    if (countAlloc.load(std::memory_order_relaxed)) allocs.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
#endif
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
static constexpr double kPi = 3.1415926535897932384626433832795;
static constexpr int kBlock = 384;

static double gain(int sr, double hz) {
    fb::TalkHighpass hp;
    if (!hp.init(sr)) return 0.0;
    const int n = 2 * sr;
    std::vector<float> in(n), out(n);
    for (int i = 0; i < n; ++i) in[i] = (float)std::sin(2.0 * kPi * hz * i / sr);
    int at = 0;
    while (at < n) {
        const int len = std::min(733, n - at);
        hp.process(in.data() + at, out.data() + at, len);
        at += len;
    }
    double ei = 0.0, eo = 0.0;
    for (int i = sr; i < n; ++i) { ei += (double)in[i] * in[i]; eo += (double)out[i] * out[i]; }
    return std::sqrt(eo / ei);
}

static bool isTalkVersion() {
    return std::strstr(aec48_version(), "talk-t2ae10") != nullptr;
}
static bool apiSupported(Aec48* a) {
    const bool expected = isTalkVersion();
    const int rc = aec48_set_talk_highpass(a, 1);
    check(rc == (expected ? 0 : -1), "HPF API availability matches embedded model version");
    return expected;
}

static std::vector<float> makeMic(int sr, int seconds = 2) {
    const int n = (sr * seconds / kBlock) * kBlock;
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / sr;
        v[i] = (float)(.18 * std::sin(2 * kPi * 50 * t) + .11 * std::sin(2 * kPi * 250 * t));
    }
    return v;
}

// hpfSetting: -1 leaves the default, 0/1 selects a setting before the first block.
// toggles are applied at exact 384-sample boundaries.
static std::vector<float> chainRun(int sr, const std::vector<float>& mic, int hpfSetting,
                                   const std::vector<std::pair<int, int>>& toggles = {},
                                   bool sing = false, int profileSwitch = -1) {
    Aec48* a = aec48_create(sr, kBlock);
    if (!a) return {};
    if (hpfSetting >= 0 && aec48_set_talk_highpass(a, hpfSetting) != 0) { aec48_destroy(a); return {}; }
    if (sing && aec48_set_profile(a, AEC48_PROFILE_SING) != 0) { aec48_destroy(a); return {}; }
    if (profileSwitch >= 0 && aec48_set_profile(a, AEC48_PROFILE_SING) != 0) { aec48_destroy(a); return {}; }
    aec48_set_talk_gate(a, 0, -3.0f);
    aec48_set_enabled(a, 1);
    std::vector<float> ref(mic.size(), 0.f), out(mic.size());
    size_t ti = 0;
    const int blocks = (int)(mic.size() / kBlock);
    for (int b = 0; b < blocks; ++b) {
        while (ti < toggles.size() && toggles[ti].first == b) {
            aec48_set_talk_highpass(a, toggles[ti].second);
            ++ti;
        }
        if (profileSwitch == b) aec48_set_profile(a, AEC48_PROFILE_TALK);
        aec48_process(a, mic.data() + (size_t)b * kBlock, ref.data() + (size_t)b * kBlock,
                      out.data() + (size_t)b * kBlock, kBlock);
    }
    aec48_destroy(a);
    return out;
}

static void apiAndRealtime() {
    check(aec48_set_talk_highpass(nullptr, 1) == -1, "null HPF API returns -1");
    Aec48* a = aec48_create(48000, 0);
    if (!a) { check(false, "create instance"); return; }
    const bool has = apiSupported(a);
    if (has) {
        check(aec48_set_talk_highpass(a, 0) == 0, "HPF can be disabled");
        check(aec48_set_talk_highpass(a, 1) == 0, "HPF can be enabled");
    }
    const int n = 4001;
    std::vector<float> mic(n), ref(n), outOn(n), outOff(n);
    for (int i = 0; i < n; ++i) { mic[i] = .03f * std::sin(i * .07f); ref[i] = .02f * std::cos(i * .031f); }
    Aec48* b = aec48_create(48000, 0);
    if (!b) { aec48_destroy(a); check(false, "create bypass comparison instance"); return; }
    aec48_set_enabled(a, 0); aec48_set_enabled(b, 0);
    if (has) { aec48_set_talk_highpass(a, 0); aec48_set_talk_highpass(b, 1); }
    int at = 0, lens[] = {1, 17, 383, 385, 777, 1024, 1414};
    for (int j = 0; at < n; ++j) {
        const int len = std::min(lens[j % 7], n - at);
        aec48_process(a, mic.data() + at, ref.data() + at, outOff.data() + at, len);
        aec48_process(b, mic.data() + at, ref.data() + at, outOn.data() + at, len);
        at += len;
    }
    check(std::memcmp(outOff.data(), outOn.data(), n * sizeof(float)) == 0,
          "HPF toggle does not change disabled echo-processing bypass output");
    aec48_destroy(a); aec48_destroy(b);

    // Check that every process size and HPF setter remain allocation-free.
    a = aec48_create(48000, 0);
    if (!a) { check(false, "create allocation-count instance"); return; }
    std::vector<float> x(4001, .01f), r(4001, .02f), y(4001);
    int p = 0;
    allocs.store(0); countAlloc.store(true, std::memory_order_relaxed);
    for (int j = 0; p < (int)x.size(); ++j) {
        const int len = std::min(lens[j % 7], (int)x.size() - p);
        if (has && j == 2) aec48_set_talk_highpass(a, 0);
        if (has && j == 5) aec48_set_talk_highpass(a, 1);
        aec48_process(a, x.data() + p, r.data() + p, y.data() + p, len);
        p += len;
    }
    countAlloc.store(false, std::memory_order_relaxed);
    check(allocs.load() == 0, "arbitrary process lengths and HPF toggles allocate no memory");
    aec48_destroy(a);
}

static void talkChainFadeAndState(int sr) {
    if (!isTalkVersion()) return;
    const auto mic = makeMic(sr, 1);
    const auto dry = chainRun(sr, mic, 0);
    const auto steadyOn = chainRun(sr, mic, 1);
    const auto defaultOn = chainRun(sr, mic, -1);
    const std::vector<std::pair<int, int>> toggles = {{20, 0}, {22, 1}, {23, 0}, {24, 1}, {64, 0}, {66, 1}};
    const auto changing = chainRun(sr, mic, -1, toggles);
    check(dry.size() == mic.size() && steadyOn.size() == mic.size() && changing.size() == mic.size(),
          "talk chain runs for HPF comparison");
    if (dry.size() != mic.size() || steadyOn.size() != mic.size() || changing.size() != mic.size()) return;
    check(std::memcmp(steadyOn.data(), defaultOn.data(), dry.size() * sizeof(float)) == 0,
          "talk HPF defaults on exactly like explicit on");

    // The helper is the expected wet branch. Compare the production's per-sample
    // linear mix, including quick reversals while a prior 4-block ramp is active.
    fb::TalkHighpass hp;
    hp.init(sr);
    std::vector<float> wet(dry.size());
    hp.process(dry.data(), wet.data(), (int)dry.size());
    std::vector<float> expected(dry.size());
    float from = 1.f, mix = 1.f;
    int target = 1, fadeAt = 0, toggleAt = 0;
    double worst = 0;
    for (int b = 0; b < (int)(dry.size() / kBlock); ++b) {
        while (toggleAt < (int)toggles.size() && toggles[toggleAt].first == b) {
            const int next = toggles[toggleAt++].second;
            if (next != target) { target = next; from = mix; fadeAt = 0; }
        }
        for (int q = 0; q < kBlock; ++q) {
            float w = mix;
            if (fadeAt < 4 * kBlock) {
                ++fadeAt;
                w = from + (static_cast<float>(target) - from) * (static_cast<float>(fadeAt) / (4.f * kBlock));
                mix = w;
            } else mix = w = static_cast<float>(target);
            const size_t i = (size_t)b * kBlock + q;
            expected[i] = w >= 1.f ? wet[i] : (w > 0.f ? (1.f - w) * dry[i] + w * wet[i] : dry[i]);
            worst = std::max(worst, std::fabs((double)changing[i] - expected[i]));
        }
    }
    check(worst <= 2e-6, "talk HPF matches exact 4-block sample-wise fade through rapid reversals");
    const size_t afterSettledReenable = (size_t)(66 + 4) * kBlock;
    check(std::memcmp(changing.data() + afterSettledReenable, steadyOn.data() + afterSettledReenable,
                      (dry.size() - afterSettledReenable) * sizeof(float)) == 0,
          "HPF state continues updating while off and matches always-on after re-enable fade");

    double steadyStep = 0.0, transitionStep = 0.0;
    const int radius = sr / 100;
    for (size_t i = 1; i < changing.size(); ++i) {
        const double d = std::fabs((double)changing[i] - changing[i - 1]);
        bool near = false;
        for (const auto& t : toggles) near |= i >= (size_t)t.first * kBlock - radius && i <= (size_t)t.first * kBlock + radius;
        if (i > 4 * kBlock && i + 4 * kBlock < changing.size()) {
            if (near) transitionStep = std::max(transitionStep, d);
            else steadyStep = std::max(steadyStep, d);
        }
    }
    const double steadyRatio = steadyStep > 0.0 ? transitionStep / steadyStep : 1e30;
    std::printf("%d Hz chain: max fade error %.3g, steady adjacent peak %.6g, switch-window peak %.6g, ratio %.3f\n",
                sr, worst, steadyStep, transitionStep, steadyRatio);
    check(steadyStep > 1e-7 && transitionStep <= 2.0 * steadyStep,
          "HPF toggles stay below twice the steady adjacent-sample difference in each ±10 ms window");
}

static void dualProfiles() {
    if (std::strstr(aec48_version(), "sing-v59ae20") == nullptr) return;
    const int sr = 48000;
    const auto mic = makeMic(sr, 1);
    const auto singOff = chainRun(sr, mic, 0, {}, true);
    const auto singOn = chainRun(sr, mic, 1, {}, true);
    check(singOff.size() == singOn.size() && std::memcmp(singOff.data(), singOn.data(), singOff.size() * sizeof(float)) == 0,
          "dual singing profile is unchanged when HPF setting changes");
    const auto switchedOff = chainRun(sr, mic, 0, {}, false, 20);
    const auto switchedOn = chainRun(sr, mic, 1, {}, false, 20);
    double talkDifference = 0.0;
    const size_t from = 40u * kBlock;
    for (size_t i = from; i < switchedOff.size(); ++i)
        talkDifference = std::max(talkDifference, std::fabs((double)switchedOff[i] - switchedOn[i]));
    check(talkDifference > 1e-4, "dual profile fade enters HPF-filtered talk lane");
}

struct SpectrumCapture { std::vector<float> values; int badSide = 0; };
static void captureSpectrum(void* user, float* re, float* im, int bins, const Aec48SpecSide* side) {
    SpectrumCapture& c = *static_cast<SpectrumCapture*>(user);
    c.badSide += !side || side->size != sizeof(*side) || side->near_talk != -1.f;
    for (int k = 0; k < bins; ++k) { c.values.push_back(re[k]); c.values.push_back(im[k]); }
}
static std::vector<float> spectra(int sr, const std::vector<float>& mic, bool toggle, int* badSide) {
    Aec48* a = aec48_create(sr, kBlock);
    if (!a) return {};
    if (aec48_set_talk_highpass(a, 0) != 0) { aec48_destroy(a); return {}; }
    aec48_set_enabled(a, 1); aec48_set_talk_gate(a, 0, -3.f);
    SpectrumCapture cap; cap.values.reserve((mic.size() / kBlock - 1) * 769u * 2u);
    if (aec48_set_spec_hook2(a, captureSpectrum, &cap) != 0) { aec48_destroy(a); return {}; }
    std::vector<float> ref(mic.size(), 0.f), out(mic.size());
    for (size_t b = 0; b < mic.size() / kBlock; ++b) {
        if (toggle && b == 20) aec48_set_talk_highpass(a, 1);
        if (toggle && b == 40) aec48_set_talk_highpass(a, 0);
        aec48_process(a, mic.data() + b * kBlock, ref.data() + b * kBlock,
                      out.data() + b * kBlock, kBlock);
    }
    if (badSide) *badSide = cap.badSide;
    aec48_destroy(a);
    return cap.values;
}
static void hookContract() {
    if (!isTalkVersion()) return;
    const int sr = 48000;
    const auto mic = makeMic(sr, 1);
    int badOff = 0, badToggle = 0;
    const auto stableOff = spectra(sr, mic, false, &badOff);
    const auto toggled = spectra(sr, mic, true, &badToggle);
    check(stableOff.size() == toggled.size() && !stableOff.empty() &&
          std::memcmp(stableOff.data(), toggled.data(), stableOff.size() * sizeof(float)) == 0,
          "HPF switching leaves hook2 spectra bit-for-bit unchanged");
    check(badOff == 0 && badToggle == 0, "hook2 side size and near_talk contract remain unchanged");
}

int main() {
    apiAndRealtime();
    bool response = true;
    for (int sr : {48000, 44100}) {
        const double g50 = gain(sr, 50), g80 = gain(sr, 80), g100 = gain(sr, 100), g300 = gain(sr, 300);
        const double g3000 = gain(sr, 3000), g10000 = gain(sr, 10000), g20000 = gain(sr, 20000);
        const double db50 = 20 * std::log10(g50), db100 = 20 * std::log10(g100);
        const double db80 = 20 * std::log10(g80), db300 = 20 * std::log10(g300);
        const double db3000 = 20 * std::log10(g3000), db10000 = 20 * std::log10(g10000), db20000 = 20 * std::log10(g20000);
        std::printf("%d Hz: 50=%+.3f dB 80=%+.3f dB 100=%+.3f dB 300=%+.4f dB 3k=%+.4f dB 10k=%+.4f dB 20k=%+.4f dB\n",
                    sr, db50, db80, db100, db300, db3000, db10000, db20000);
        response &= db50 <= -15.0 && db80 >= -3.4 && db80 <= -2.6 && db100 >= -2.0 && db300 >= -0.1 && db3000 >= -0.1 &&
                    db10000 >= -0.1 && db20000 >= -0.1;
    }
    check(response, "4th-order 80 Hz response meets 48 kHz / 44.1 kHz limits");
    if (isTalkVersion()) {
        talkChainFadeAndState(48000);
        talkChainFadeAndState(44100);
    }
    dualProfiles();
    hookContract();
    return failures ? 1 : 0;
}
