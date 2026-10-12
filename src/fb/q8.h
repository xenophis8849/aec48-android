// int8 矩阵 × 向量（掩码网络 CPF 两个全连接用），量化方式与内核取自 Arm KleidiAI（third_party/kleidiai，Apache-2.0）：
//   权重：按行对称 int8（每行一个比例）；输入：每次调用动态量化成非对称 int8（整行一个比例 + 零点，kai_run_lhs_quant_pack_qai8dxp_f32）；
//   输出 y = 偏置 + float(Σ xq·wq − 零点·Σwq) × (权重比例 × 输入比例倒数)。
// 两条计算路径结果一致（整数部分精确，收尾同一式子）：
//   · arm64 且 CPU 支持点积指令：KleidiAI 内核 matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod；
//   · 其他（ARMv8.0 如 Cortex-A53、armv7、x86）：同一个输入量化打包函数 + 整数点积（NEON smull+sadalp / 标量）。
// 点积指令只认系统声明（HWCAP_ASIMDDP）。不能按核心型号推断：Cortex-A55 / A75 的点积指令是可选配置（Arm 官方说明），
//   型号对得上也可能没有，误用会触发非法指令、结束宿主进程。系统不声明时（含部分旧内核）走后备路径，结果相同、稍慢。
// KleidiAI 的源文件不加 -ffast-math 编译（量化的取整与比例须各平台一致）。
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <string>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif
#include "kai/ukernels/matmul/pack/kai_lhs_quant_pack_qai8dxp_f32.h"
#if defined(__aarch64__)
#include "kai/ukernels/matmul/pack/kai_rhs_pack_nxk_qsi8cxp_qsi8cx_neon.h"
#include "kai/ukernels/matmul/matmul_clamp_f32_qai8dxp_qsi8cxp/kai_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod.h"
#endif

namespace q8 {
inline int pad16(int c) { return (c + 15) & ~15; }
inline size_t kRound(size_t k) { return (k + 31) / 32 * 32; }   // = KleidiAI kai_k_roundedup

#if defined(__aarch64__)
inline int kernelChoice() {   // 0 = 后备路径；8 = KleidiAI 1x8 内核
    static const int v = [] {
        if (getenv("Q8_NODOT")) return 0;
        bool dot = false;
#if defined(__linux__)
        dot = (getauxval(AT_HWCAP) & (1UL << 20)) != 0;   // HWCAP_ASIMDDP：只认系统声明
#endif
        return dot ? 8 : 0;
    }();
    return v;
}
#else
inline int kernelChoice() { return 0; }
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
inline int32_t dotNeon(const int8_t* a, const int8_t* b, int n) {   // ARMv8.0 / armv7：smull + sadalp（每 16 字节）
    int32x4_t acc = vdupq_n_s32(0);
    for (int i = 0; i < n; i += 16) { const int8x16_t x = vld1q_s8(a + i), y = vld1q_s8(b + i);
        int16x8_t p = vmull_s8(vget_low_s8(x), vget_low_s8(y)); acc = vpadalq_s16(acc, p);
        p = vmull_s8(vget_high_s8(x), vget_high_s8(y)); acc = vpadalq_s16(acc, p); }
#if defined(__aarch64__)
    return vaddvq_s32(acc);
#else
    int32x2_t t = vadd_s32(vget_low_s32(acc), vget_high_s32(acc)); return vget_lane_s32(vpadd_s32(t, t), 0);
#endif
}
#endif
inline int32_t dotScalar(const int8_t* a, const int8_t* b, int n) { int32_t s = 0; for (int i = 0; i < n; ++i) s += (int32_t)a[i] * b[i]; return s; }

struct QMat {
    int R = 0, C = 0, Cp = 0, kern = 0; size_t mr = 1, kr = 4, sr = 1;
    std::vector<int8_t> w; std::vector<float> sc, bias; std::vector<int32_t> rsum;   // 后备路径用：行补零到 16 的倍数
    std::vector<uint8_t> rhsP, lhsP;                                                  // KleidiAI 打包后的权重；输入打包缓冲（每次调用复用）
    void init(const float* W, const float* b, int R_, int C_) {
        R = R_; C = C_; Cp = pad16(C); kern = kernelChoice();
        w.assign((size_t)R * Cp, 0); sc.assign(R, 0.f); bias.assign(b, b + R); rsum.assign(R, 0);
        for (int r = 0; r < R; ++r) { const float* wr = W + (size_t)r * C; float m = 0; for (int i = 0; i < C; ++i) m = std::max(m, std::fabs(wr[i]));
            const float s = m > 0 ? m / 127.f : 1.f; sc[r] = s; const float inv = 1.f / s; int8_t* q = &w[(size_t)r * Cp]; int32_t sum = 0;
            for (int i = 0; i < C; ++i) { long v = lrintf(wr[i] * inv); q[i] = (int8_t)(v > 127 ? 127 : (v < -127 ? -127 : v)); sum += q[i]; } rsum[r] = sum; }
#if defined(__aarch64__)
        if (kern) {
            mr = kai_get_mr_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod(); kr = kai_get_kr_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod(); sr = kai_get_sr_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod();
            const size_t nr = kai_get_nr_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod();
            std::vector<int8_t> wc((size_t)R * C); for (int r = 0; r < R; ++r) memcpy(&wc[(size_t)r * C], &w[(size_t)r * Cp], C);
            rhsP.assign(kai_get_rhs_packed_size_rhs_pack_nxk_qsi8cxp_qsi8cx_neon(R, C, nr, kr, sr), 0);
            kai_rhs_pack_qsi8cx_params pp; pp.lhs_zero_point = 1; pp.scale_multiplier = 1.f;
            kai_run_rhs_pack_nxk_qsi8cxp_qsi8cx_neon(1, R, C, nr, kr, sr, wc.data(), bias.data(), sc.data(), rhsP.data(), 0, &pp);
        }
#endif
        lhsP.assign(kai_get_lhs_packed_size_lhs_quant_pack_qai8dxp_f32(1, C, mr, kr, sr), 0);
    }
};

// y[r] = bias[r] + Σ_i W[r][i]·x[i]（量化后）。处理期不分配内存。
inline void gemv(QMat& m, const float* x, float* y) {
    kai_run_lhs_quant_pack_qai8dxp_f32(1, m.C, m.mr, m.kr, m.sr, 0, x, m.C * sizeof(float), m.lhsP.data());
#if defined(__aarch64__)
    if (m.kern == 8) { kai_run_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod(1, m.R, m.C, m.lhsP.data(), m.rhsP.data(), y, m.R * sizeof(float), sizeof(float), -FLT_MAX, FLT_MAX); return; }
#endif
    // 后备路径：mr = 1 时打包结果 = 按顺序的 int8[kRound(C)]（C 之后是末值的复制，乘补零的权重为 0）+ int32 偏移（= −零点）+ float 比例倒数
    const int8_t* xq = (const int8_t*)m.lhsP.data(); const size_t K = kRound(m.C);
    int32_t off; float rc; memcpy(&off, m.lhsP.data() + K, 4); memcpy(&rc, m.lhsP.data() + K + 4, 4);
    for (int r = 0; r < m.R; ++r) {
        const int8_t* w = &m.w[(size_t)r * m.Cp];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        int32_t s = dotNeon(w, xq, m.Cp);
#else
        int32_t s = dotScalar(w, xq, m.Cp);
#endif
        s += m.rsum[r] * off;
        const float k = m.sc[r] * rc; const volatile float t = (float)s * k; y[r] = t + m.bias[r];   // 同内核收尾：先乘后加，不融合成 FMA
    }
}
}  // namespace q8
