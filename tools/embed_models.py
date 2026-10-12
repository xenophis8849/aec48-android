#!/usr/bin/env python3
"""把 models/ 下的两个模型文件编成 src/models.cpp（C 数组），库从内存加载，接入方不用另外放模型文件。
用法：tools/embed_models.py <线性级.gtc> <掩码.gtc> <模型标识>   例：tools/embed_models.py models/lin_nlinBc3.gtc models/mask_v59ae20.gtc "lin nlinBc3-dual, mask v59ae20" """
import sys, hashlib
lin, mask, mid = sys.argv[1], sys.argv[2], sys.argv[3]
def arr(name, path):
    b = open(path, "rb").read(); h = hashlib.sha256(b).hexdigest()[:16]
    rows = ",\n".join(",".join(str(x) for x in b[i:i + 32]) for i in range(0, len(b), 32))
    return f"// {path}  {len(b)} 字节  sha256 前 16 位 {h}\nextern const unsigned char {name}[]; alignas(16) const unsigned char {name}[] = {{\n{rows}\n}};\nextern const unsigned long {name}_size; const unsigned long {name}_size = {len(b)};\n"
open("src/models.cpp", "w").write("// 自动生成（tools/embed_models.py），不要手改\nextern \"C\" {\n" + arr("aec48_lin_model", lin) + arr("aec48_mask_model", mask) +
    f'extern const char aec48_model_id[]; const char aec48_model_id[] = "{mid}";\n}}\n')
print("src/models.cpp 写好")
