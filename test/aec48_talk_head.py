#!/usr/bin/env python3
"""Standalone float32/double/PyTorch agreement check for the embedded gru2 head."""
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
for key in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"):
    os.environ[key] = "1"
import sys
sys.dont_write_bytecode = True
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import torch
from torch import nn

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = Path(__file__).resolve().parents[1] / "models/mask_talk.gtc"
DEFAULT_CKPT = Path(os.environ.get("AEC48_CKPT_DIR", ".")) / "aec48_model_talk_e1be12.pt"
MODEL = Path(os.environ.get("AEC48_TALK_GTC", DEFAULT_MODEL))
CKPT = Path(os.environ.get("AEC48_TALK_CKPT", DEFAULT_CKPT))
torch.set_num_threads(1)

HARNESS = r'''#include "fb/cofi.h"
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  fb::Cofi c; c.load(argv[1], 1536, 20.f);
  FILE* f = fopen(argv[2], "rb"); int n = 0;
  if (!f || fread(&n, sizeof(int), 1, f) != 1) return 3;
  std::vector<float> x(829); float y;
  for (int t = 0; t < n; ++t) {
    if (fread(x.data(), sizeof(float), x.size(), f) != x.size()) return 4;
    y = c.talkStep(x.data());
    if (fwrite(&y, sizeof(float), 1, stdout) != 1) return 5;
  }
  fclose(f); return 0;
}'''

def tensor_model(state, dtype):
    m = nn.ModuleDict({"fc1": nn.Linear(829, 32), "gru": nn.GRU(32, 32, batch_first=True), "fc2": nn.Linear(32, 1)})
    m["fc1"].weight.data.copy_(state["talk_head2.0.weight"])
    m["fc1"].bias.data.copy_(state["talk_head2.0.bias"])
    for n in ("weight_ih_l0", "weight_hh_l0", "bias_ih_l0", "bias_hh_l0"):
        m["gru"]._parameters[n].data.copy_(state[f"talk_head2.2.{n}"])
    m["fc2"].weight.data.copy_(state["talk_head2.3.weight"])
    m["fc2"].bias.data.copy_(state["talk_head2.3.bias"])
    return m.to(dtype=dtype).eval()

def run_ref(model, x):
    hidden = None; out = []
    with torch.no_grad():
        for row in x:
            z = torch.nn.functional.elu(model["fc1"](torch.from_numpy(row).to(next(model.parameters()).dtype)))[None, None]
            z, hidden = model["gru"](z, hidden)
            out.append(float(model["fc2"](z)[0, 0, 0]))
    return np.asarray(out, dtype=np.float64)

def main():
    ckpt = torch.load(CKPT, map_location="cpu", weights_only=False)["model"]
    rng = np.random.default_rng(731)
    x = rng.normal(0, 0.25, (256, 829)).astype(np.float32)
    x[:, 700:] = rng.uniform(-8.0, -0.2, (len(x), 129)).astype(np.float32)
    ref32 = run_ref(tensor_model(ckpt, torch.float32), x)
    ref64 = run_ref(tensor_model(ckpt, torch.float64), x)
    # Keep inputs and partial output on failure for recovery and audit.
    td = Path(tempfile.mkdtemp(prefix="aec48-talk-head-"))
    cpp = td / "head.cpp"; exe = td / "head"; inp = td / "features.bin"
    cpp.write_text(HARNESS)
    subprocess.run(["c++", "-O3", "-std=c++17", "-Isrc", str(cpp), "-o", str(exe)], cwd=ROOT, check=True)
    with inp.open("wb") as f:
        f.write(struct.pack("i", len(x))); f.write(x.tobytes())
    c = subprocess.run([str(exe), str(MODEL), str(inp)], check=True, stdout=subprocess.PIPE).stdout
    got = np.frombuffer(c, dtype=np.float32).astype(np.float64)
    print(f"ARTIFACTS {td}")
    for label, ref in (("torch_float32", ref32), ("torch_double", ref64)):
        d = np.abs(got - ref)
        print(f"C_vs_{label}: n={len(d)} p50={np.quantile(d,.50):.8g} p95={np.quantile(d,.95):.8g} p99={np.quantile(d,.99):.8g} max={d.max():.8g}")
    if max(np.max(np.abs(got - ref32)), np.max(np.abs(got - ref64))) > 1e-3:
        raise SystemExit("talk head discrepancy exceeds 1e-3")

if __name__ == "__main__": main()
