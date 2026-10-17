#!/usr/bin/env python3
"""Export the frozen t2ae10 CoFi body plus e1be12 gru2 head as mask_talk.gtc.

Usage: python tools/export_talk.py [t2ae10.pt] [e1be12.pt] [mask_talk.gtc]
The defaults point at the documented local model mirror.  This script compares
all shared body tensors bit-for-bit before taking the gru2 head from e1be12.
"""
import os
import sys
from pathlib import Path
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
sys.dont_write_bytecode = True
for key in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
    os.environ[key] = "1"

ROOT = Path(__file__).resolve().parents[1]
import os
E5_SRC = Path(os.environ["AEC48_TALK_SRC"])   # 训练侧讲话模型代码目录（含 cofi_aec48.py）
AEC48 = Path(os.environ["AEC48_PY_ROOT"])   # 训练侧 aec48 Python 目录（含 gtcrn_aec48.py、ondevice/cofi_np.py）
ONDEVICE = AEC48 / "ondevice"
sys.path[:0] = [str(E5_SRC), str(ONDEVICE), str(AEC48)]

os.environ.update({
    "AEC48_ARCH": "cofi", "AEC48_COFI": "large", "AEC48_NFFT": "1536",
    "AEC48_HOP": "384", "AEC48_SR": "48000", "AEC48_FEAT_RHO": "1",
    "AEC48_MASK": "res", "AEC48_MASK_FLOOR_DB": "20",
    "AEC48_HINT": "1", "AEC48_FEAT_REF": "1", "AEC48_TALK": "1",
    "AEC48_TALK_HEAD": "gru2", "AEC48_TALK_DETACH": "1",
    "AEC48_WIDTH": "32", "AEC48_DMIC": "1",
})

import numpy as np
import torch
torch.set_num_threads(1)
import gtcrn_aec48 as G
import cofi_np

DEFAULTS = Path(os.environ.get("AEC48_CKPT_DIR", "."))   # 检查点目录
body_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULTS / "aec48_model_talk_t2ae10.pt"
head_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULTS / "aec48_model_talk_e1be12.pt"
out_path = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "models/mask_talk.gtc"

def load_state(path):
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    if "model" not in ckpt:
        raise ValueError(f"checkpoint 缺少 model: {path}")
    return ckpt["model"]

body_sd, head_sd = load_state(body_path), load_state(head_path)
head_keys = {k for k in head_sd if k.startswith("talk_head2.") or k == "talk_head2_v1"}
if not head_keys or "talk_head2.0.weight" not in head_keys:
    raise ValueError("e1be12 中没有 gru2 小出口")
talk_aux = {k for k in set(body_sd) | set(head_sd) if k.startswith("talk_head.") or k == "talk_head2_v1"}
shared = sorted(set(body_sd) & set(head_sd) - head_keys - talk_aux)
body_only = sorted(set(body_sd) - set(head_sd) - talk_aux)
head_only = sorted(set(head_sd) - set(body_sd) - head_keys)
if body_only or head_only:
    raise ValueError(f"两检查点主体键不一致: t2={body_only[:8]}, e1b={head_only[:8]}")
def bit_equal(x, y):
    return x.shape == y.shape and x.dtype == y.dtype and x.detach().cpu().contiguous().numpy().tobytes() == y.detach().cpu().contiguous().numpy().tobytes()
different = [k for k in shared if not bit_equal(body_sd[k], head_sd[k])]
print(f"BODY_COMPARE shared={len(shared)} mismatch={len(different)}")
if different:
    raise ValueError(f"主体权重并非逐位相同: {different[:8]}")

net = G.GTCRN_AEC48()
merged = dict(body_sd)
for k in talk_aux:
    merged.pop(k, None)
for k in head_keys:
    merged[k] = head_sd[k]
net.load_state_dict(merged, strict=True)
net.eval()
params = cofi_np.export_params(net)
for key in head_keys:
    if key.startswith("talk_head2."):
        params[key] = head_sd[key].detach().cpu().numpy().astype(np.float32, copy=True)
params["talk_head2_v1"] = np.ones((1,), np.float32)
cofi_np.save_bin(params, out_path)
print(f"EXPORTED {out_path} bytes={out_path.stat().st_size} tensors={len(params)} features=700+129=829")
