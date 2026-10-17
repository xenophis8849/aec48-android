// 双档配置接口与处理线程边界自检。用法：aec48_profile
#include "aec48.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> count_allocations{false};
std::atomic<unsigned long> allocation_count{0};
}

#if defined(__GLIBC__) && !defined(__ANDROID__)
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* malloc(std::size_t n) {
    if (count_allocations.load(std::memory_order_relaxed)) allocation_count.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
#endif

void* operator new(std::size_t n) {
    if (count_allocations.load(std::memory_order_relaxed)) allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (count_allocations.load(std::memory_order_relaxed)) allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
enum HookMode { HookIdentity, HookHalf, HookZero };
struct HookState { int calls = 0; HookMode mode = HookIdentity; bool contract_ok = true; };
void hook2(void* opaque, float* re, float* im, int bins, const Aec48SpecSide* side) {
    auto* s = static_cast<HookState*>(opaque);
    ++s->calls;
    if (!side || side->size != static_cast<int>(sizeof(Aec48SpecSide)) || side->near_talk != -1.0f)
        s->contract_ok = false;
    for (int i = 0; i < bins; ++i) {
        if (s->mode == HookHalf) { re[i] *= .5f; im[i] *= .5f; }
        else if (s->mode == HookZero) re[i] = im[i] = 0.f;
    }
}

bool process_one(Aec48* a, int block, int seq) {
    std::array<float, 384> mic{}, ref{}, out{};
    for (int i = 0; i < block; ++i) {
        const int t = seq * block + i;
        ref[i] = 0.08f * std::sin(0.013f * t) + 0.025f * std::sin(0.031f * t);
        mic[i] = 0.4f * ref[(i + block - 11) % block] + 0.015f * std::sin(0.021f * t);
    }
    if (block != static_cast<int>(mic.size()) || aec48_process(a, mic.data(), ref.data(), out.data(), block) != 0) return false;
    for (float x : out) if (!std::isfinite(x)) return false;
    return true;
}
}

int main() {
    const std::string version = aec48_version() ? aec48_version() : "";
    const bool dual = version.find("mask sing-") != std::string::npos && version.find("talk-t2ae10") != std::string::npos;
    const bool talk_only = version.find("mask talk-") != std::string::npos;
    const int own_profile = talk_only ? AEC48_PROFILE_TALK : AEC48_PROFILE_SING;
    bool ok = true;
    ok &= aec48_set_profile(nullptr, AEC48_PROFILE_TALK) == -1;
    ok &= aec48_get_profile(nullptr) == -1;
    Aec48* a = aec48_create(48000, 384);
    if (!a) {
        std::fprintf(stderr, "FAIL create 48000/384\n");
        return 1;
    }
    const int initial = aec48_get_profile(a);
    ok &= initial == (dual ? AEC48_PROFILE_TALK : own_profile);
    ok &= aec48_set_profile(a, -1) == -1;
    ok &= aec48_set_profile(a, 2) == -1;
    if (!dual) {
        ok &= aec48_set_profile(a, 1 - own_profile) == -1;
        ok &= aec48_set_profile(a, own_profile) == 0;
    } else {
        // 命令只在下一个 process 块边界生效；双向快速反切也不应丢失请求。
        allocation_count = 0; count_allocations = true;
        const int gate_on_rc = aec48_set_talk_gate(a, 1, -3.0f);
        const int sing_rc = aec48_set_profile(a, AEC48_PROFILE_SING);
        const bool first_sing_ok = process_one(a, 384, 0);
        count_allocations = false;
        ok &= gate_on_rc == 0 && sing_rc == 0 && first_sing_ok && allocation_count.load() == 0;
        ok &= aec48_get_profile(a) == AEC48_PROFILE_SING;
        allocation_count = 0; count_allocations = true;
        const int gate_off_rc = aec48_set_talk_gate(a, 0, -2.5f); // 唱歌档设置仍保存
        const int talk_rc = aec48_set_profile(a, AEC48_PROFILE_TALK);
        const bool first_talk_ok = process_one(a, 384, 1);
        count_allocations = false;
        ok &= gate_off_rc == 0 && talk_rc == 0 && first_talk_ok && allocation_count.load() == 0;
        ok &= aec48_get_profile(a) == AEC48_PROFILE_TALK;
        allocation_count = 0; count_allocations = true;
        const int flip1_rc = aec48_set_profile(a, AEC48_PROFILE_SING);
        const int flip2_rc = aec48_set_profile(a, AEC48_PROFILE_TALK); // 同一边界前最后一次请求生效
        const bool flipped_ok = process_one(a, 384, 2);
        count_allocations = false;
        ok &= flip1_rc == 0 && flip2_rc == 0 && flipped_ok && allocation_count.load() == 0;
        ok &= aec48_get_profile(a) == AEC48_PROFILE_TALK;
        HookState hs;
        ok &= aec48_set_spec_hook2(a, hook2, &hs) == 0;
        allocation_count = 0; count_allocations = true;
        const bool hook_block_ok = process_one(a, 384, 3);
        count_allocations = false;
        ok &= hook_block_ok && allocation_count.load() == 0;
        ok &= hs.calls > 0 && hs.contract_ok; // hook2 near_talk 契约保持 -1
        ok &= aec48_set_spec_hook2(a, nullptr, nullptr) == 0;

        // 同一序列比较无回调、identity、half、zero；切档和讲话门开关均处于回调期间。
        // 第一个处理块不产帧，之后每个块恰好调用一次 hook2。
        std::array<Aec48*, 4> variants{};
        std::array<HookState, 3> hookStates{{{0, HookIdentity, true}, {0, HookHalf, true}, {0, HookZero, true}}};
        for (Aec48*& h : variants) h = aec48_create(48000, 384);
        bool hookVariantsOK = true;
        for (Aec48* h : variants) hookVariantsOK &= h != nullptr;
        for (int v = 1; v < 4 && hookVariantsOK; ++v)
            hookVariantsOK &= aec48_set_spec_hook2(variants[v], hook2, &hookStates[v - 1]) == 0;
        std::array<std::array<float, 384>, 4> variantOut{};
        std::array<float, 384> vm{}, vr{};
        float maxHalfError = 0.f, maxZero = 0.f;
        constexpr int hookBlocks = 120;
        for (int block = 0; block < hookBlocks && hookVariantsOK; ++block) {
            if (block == 30 || block == 60) {
                const int profile = block == 30 ? AEC48_PROFILE_SING : AEC48_PROFILE_TALK;
                for (Aec48* h : variants) hookVariantsOK &= aec48_set_profile(h, profile) == 0;
            }
            if (block == 24 || block == 48 || block == 88) {
                const int on = block == 48;
                for (Aec48* h : variants) hookVariantsOK &= aec48_set_talk_gate(h, on, 100.f) == 0;
            }
            for (int q = 0; q < 384; ++q) {
                const int t = block * 384 + q;
                vr[q] = .10f * std::sin(.013f * t) + .03f * std::sin(.031f * t);
                vm[q] = .58f * vr[(q + 384 - 11) % 384] + .001f * std::sin(.021f * t);
            }
            for (int v = 0; v < 4; ++v)
                hookVariantsOK &= aec48_process(variants[v], vm.data(), vr.data(), variantOut[v].data(), 384) == 0;
            hookVariantsOK &= std::memcmp(variantOut[0].data(), variantOut[1].data(), 384 * sizeof(float)) == 0;
            for (int q = 0; q < 384; ++q) {
                maxHalfError = std::max(maxHalfError, std::abs(variantOut[2][q] - .5f * variantOut[0][q]));
                maxZero = std::max(maxZero, std::abs(variantOut[3][q]));
            }
            for (const auto& v : variantOut) for (float x : v) hookVariantsOK &= std::isfinite(x);
        }
        for (const HookState& hs2 : hookStates)
            hookVariantsOK &= hs2.calls == hookBlocks - 1 && hs2.contract_ok;
        hookVariantsOK &= maxHalfError <= 1e-6f && maxZero <= 1e-6f;
        for (Aec48* h : variants) if (h) aec48_destroy(h);
        ok &= hookVariantsOK;
        std::printf("%s profile hook: identity byte-equal, half max_abs=%.3g, zero max_abs=%.3g, callbacks=%d/frame after priming\n",
                    hookVariantsOK ? "PASS" : "FAIL", maxHalfError, maxZero, hookStates[0].calls);

        // 独立控制线程随机请求/读取档位，处理线程只检查输出有限。
        Aec48* threaded = aec48_create(48000, 384);
        std::atomic<bool> threadOK{threaded != nullptr};
        if (threaded) {
            std::thread setter([&] {
                unsigned x = 0x4a3c2d1u;
                for (int i = 0; i < 600; ++i) {
                    x = x * 1664525u + 1013904223u;
                    if (aec48_set_profile(threaded, (x >> 31) ? AEC48_PROFILE_TALK : AEC48_PROFILE_SING) != 0)
                        threadOK.store(false, std::memory_order_relaxed);
                    const int active = aec48_get_profile(threaded);
                    if (active != AEC48_PROFILE_SING && active != AEC48_PROFILE_TALK)
                        threadOK.store(false, std::memory_order_relaxed);
                }
            });
            std::array<float, 384> tm{}, tr{}, ty{};
            for (int block = 0; block < 120; ++block) {
                for (int q = 0; q < 384; ++q) {
                    tm[q] = .01f * std::sin((block * 384 + q) * .019f);
                    tr[q] = .04f * std::sin((block * 384 + q) * .011f);
                }
                if (aec48_process(threaded, tm.data(), tr.data(), ty.data(), 384) != 0)
                    threadOK.store(false, std::memory_order_relaxed);
                for (float x : ty) if (!std::isfinite(x)) threadOK.store(false, std::memory_order_relaxed);
            }
            setter.join();
            ok &= threadOK.load(std::memory_order_relaxed);
            aec48_destroy(threaded);
        }

        // 直通时切档不应引入新分支的 OLA 启动衰减；门设置也不影响直通。
        Aec48* plain = aec48_create(48000, 384);
        Aec48* flipping = aec48_create(48000, 384);
        if (!plain || !flipping) ok = false;
        if (plain && flipping) {
            aec48_set_enabled(plain, 0); aec48_set_enabled(flipping, 0);
            std::array<float, 384> m{}, r{}, p{}, f{};
            bool same_bypass = true;
            for (int b = 0; b < 80; ++b) {
                for (int q = 0; q < 384; ++q) {
                    const int t = b * 384 + q;
                    m[q] = .08f * std::sin(.017f * t); r[q] = .04f * std::sin(.023f * t);
                }
                if (b >= 12 && b % 7 == 0) aec48_set_profile(flipping, (b / 7) % 2);
                same_bypass &= aec48_process(plain, m.data(), r.data(), p.data(), 384) == 0;
                same_bypass &= aec48_process(flipping, m.data(), r.data(), f.data(), 384) == 0;
                same_bypass &= !std::memcmp(p.data(), f.data(), p.size() * sizeof(float));
            }
            ok &= same_bypass;
            std::printf("%s bypass profile switches byte-identical\n", same_bypass ? "PASS" : "FAIL");
        }
        aec48_destroy(plain); aec48_destroy(flipping);

        aec48_destroy(a);
        a = aec48_create(48000, 384);
        if (!a) return 1;

        // 短于全音频验收集的时间轴功能测试：10 s talk→sing，20 s sing→talk。
        // 在两次首次切入目标掩码时检查有限输出，并监测请求与处理边界没有 operator new。
        std::array<float, 384> mic{}, ref{}, out{};
        bool switch10_no_alloc = false, switch20_no_alloc = false, long_finite = true;
        for (int block = 0; block < 24 * 48000 / 384; ++block) {
            const bool fade = (block >= 1250 && block < 1254) || (block >= 2500 && block < 2504);
            if (fade) { allocation_count = 0; count_allocations = true; }
            if (block == 10 * 48000 / 384) ok &= aec48_set_profile(a, AEC48_PROFILE_SING) == 0;
            if (block == 20 * 48000 / 384) ok &= aec48_set_profile(a, AEC48_PROFILE_TALK) == 0;
            const int offset = block * 384;
            for (int i = 0; i < 384; ++i) {
                const int t = offset + i;
                ref[i] = .10f * std::sin(.013f * t) + .03f * std::sin(.031f * t);
                mic[i] = .58f * ref[(i + 384 - 11) % 384] + .001f * std::sin(.021f * t);
            }
            const bool process_ok = aec48_process(a, mic.data(), ref.data(), out.data(), 384) == 0;
            if (fade) {
                count_allocations = false;
                const int expected = block < 15 * 48000 / 384 ? AEC48_PROFILE_SING : AEC48_PROFILE_TALK;
                const bool no_alloc = allocation_count.load() == 0;
                ok &= process_ok && no_alloc && aec48_get_profile(a) == expected;
                if (block == 10 * 48000 / 384) switch10_no_alloc = no_alloc;
                else if (block == 20 * 48000 / 384) switch20_no_alloc = no_alloc;
            }
            long_finite &= process_ok;
            for (float x : out) long_finite &= std::isfinite(x);
        }
        ok &= long_finite && switch10_no_alloc && switch20_no_alloc;
    }
    aec48_destroy(a);

    // 空指针及模型档位限制也要在非 48 kHz 实例创建路径之外有明确定义。
    Aec48* b = aec48_create(44100, 256);
    ok &= b != nullptr;
    if (b) {
        ok &= aec48_get_profile(b) == (dual ? AEC48_PROFILE_TALK : own_profile);
        aec48_destroy(b);
    }
    std::printf("%s dual=%d initial=%d model=%s; null/invalid/profile restrictions, boundary switch, quick reverse switch, 10s talk->sing->20s talk finite output, zero operator-new at first profile use/switch, gate API, hook2 near_talk=-1\n",
                ok ? "PASS" : "FAIL", dual, initial, version.c_str());
    return ok ? 0 : 1;
}
