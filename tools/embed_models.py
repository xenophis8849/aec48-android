#!/usr/bin/env python3
"""把 models/ 下的模型文件编成 C 数组，库从内存加载，接入方不用另外放模型文件。
用法：tools/embed_models.py <线性级.gtc> <掩码.gtc> <模型标识> [输出，默认 src/models.cpp]   例：tools/embed_models.py models/lin_nlinBc3.gtc models/mask_v59ae20.gtc "lin nlinBc3-dual, mask v59ae20"
双档版：tools/embed_models.py --dual <lin.gtc> <sing.gtc> <talk.gtc> <模型标识> [输出，默认 src/models_dual.cpp]
0.6 起另有候选版：tools/embed_models.py models/lin_nlinBc3.gtc models/mask_v62be20.gtc "lin nlinBc3-dual, mask v62be20 candidate" src/models_v62b.cpp """
import sys, hashlib
dual = len(sys.argv) > 1 and sys.argv[1] == "--dual"
if dual:
    if len(sys.argv) not in (6, 7):
        raise SystemExit("用法: embed_models.py --dual <lin.gtc> <sing.gtc> <talk.gtc> <模型标识> [输出]")
    lin, sing, talk, mid = sys.argv[2:6]
    out = sys.argv[6] if len(sys.argv) == 7 else "src/models_dual.cpp"
else:
    if len(sys.argv) not in (4, 5):
        raise SystemExit("用法: embed_models.py <lin.gtc> <mask.gtc> <模型标识> [输出]")
    lin, mask, mid = sys.argv[1:4]
    out = sys.argv[4] if len(sys.argv) == 5 else "src/models.cpp"
def arr(name, path):
    b = open(path, "rb").read(); h = hashlib.sha256(b).hexdigest()[:16]
    rows = ",\n".join(",".join(str(x) for x in b[i:i + 32]) for i in range(0, len(b), 32))
    return f"// {path}  {len(b)} 字节  sha256 前 16 位 {h}\nextern const unsigned char {name}[]; alignas(16) const unsigned char {name}[] = {{\n{rows}\n}};\nextern const unsigned long {name}_size; const unsigned long {name}_size = {len(b)};\n"
if dual:
    body = arr("aec48_lin_model", lin) + arr("aec48_sing_mask_model", sing) + arr("aec48_talk_mask_model", talk)
    body += f'extern const char aec48_model_id[]; const char aec48_model_id[] = "{mid}";\n'
else:
    body = arr("aec48_lin_model", lin) + arr("aec48_mask_model", mask)
    body += f'extern const char aec48_model_id[]; const char aec48_model_id[] = "{mid}";\n'
open(out, "w").write("// 自动生成（tools/embed_models.py），不要手改\nextern \"C\" {\n" + body + "}\n")
print(out, "写好")
