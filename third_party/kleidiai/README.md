# KleidiAI（节选）

来源：<https://gitlab.arm.com/kleidi/kleidiai>，标签 `v1.32.0`（提交 `c80157210987eedaab2509638ef5e15950c53a8c`），Apache-2.0（见 `LICENSES/Apache-2.0.txt`）。

只取本库用到的文件，未作修改：

- `kai/kai_common.h`
- `kai/ukernels/matmul/pack/kai_lhs_quant_pack_qai8dxp_f32.{c,h}`：输入动态量化（非对称 int8，每行一个比例和零点）并打包
- `kai/ukernels/matmul/pack/kai_rhs_pack_nxk_qsi8cxp_qsi8cx_neon.{c,h}`：权重（按行对称 int8）打包
- `kai/ukernels/matmul/matmul_clamp_f32_qai8dxp_qsi8cxp/kai_matmul_clamp_f32_qai8dxp1x8_qsi8cxp4x8_1x4_neon_dotprod.{c,h}`：int8 点积矩阵乘内核（arm64，需 CPU 支持点积指令）
