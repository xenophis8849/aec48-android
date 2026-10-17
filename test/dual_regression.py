#!/usr/bin/env python3
"""Dual-profile byte regression and live profile-switch acoustic check.

Examples:
  python test/dual_regression.py --dual build-host-dual/libaec48.so \
      --baseline-dir build-dual-evidence/baseline --out-dir build-dual-evidence/final
The frozen byte cases intentionally match talk_regression.py. The longer
switch trajectory uses a separate seed and is never used to tune the model.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import time
from pathlib import Path

import numpy as np


SING, TALK = 0, 1
BLOCKS = 80


def bind(path, profile_api=True):
    lib = C.CDLL(str(Path(path).resolve()))
    lib.aec48_create.argtypes = [C.c_int, C.c_int]
    lib.aec48_create.restype = C.c_void_p
    lib.aec48_destroy.argtypes = [C.c_void_p]
    lib.aec48_set_enabled.argtypes = [C.c_void_p, C.c_int]
    if profile_api:
        lib.aec48_set_profile.argtypes = [C.c_void_p, C.c_int]
        lib.aec48_set_profile.restype = C.c_int
        lib.aec48_get_profile.argtypes = [C.c_void_p]
        lib.aec48_get_profile.restype = C.c_int
        lib.aec48_set_talk_gate.argtypes = [C.c_void_p, C.c_int, C.c_float]
        lib.aec48_set_talk_gate.restype = C.c_int
    lib.aec48_process.argtypes = [C.c_void_p, C.POINTER(C.c_float), C.POINTER(C.c_float), C.POINTER(C.c_float), C.c_int]
    lib.aec48_process.restype = C.c_int
    lib.aec48_process_s16.argtypes = [C.c_void_p, C.POINTER(C.c_int16), C.POINTER(C.c_int16), C.POINTER(C.c_int16), C.c_int]
    lib.aec48_process_s16.restype = C.c_int
    lib.aec48_latency_samples.argtypes = [C.c_void_p]
    lib.aec48_latency_samples.restype = C.c_int
    return lib


def ptr(a, dtype):
    return a.ctypes.data_as(C.POINTER(dtype))


def random_case(i):
    # Preserve exactly the input construction used by talk_regression.py.
    rng = np.random.default_rng(202610170001 + i)
    sr, block = [48000, 44100][i % 2], [384, 256, 768][i % 3]
    n = block * BLOCKS
    ref = (rng.normal(size=n) * 10 ** rng.uniform(-4, -1)).astype('float32')
    ref[n // 3:n // 2] = 0
    mic = (np.roll(ref, 710) * .6 + rng.normal(size=n) * .002).astype('float32')
    return sr, block, mic, ref


def run_fixed(lib, profile, sr, block, mic, ref, integer):
    a = lib.aec48_create(sr, block)
    if not a:
        raise RuntimeError(f"create failed ({sr}, {block})")
    try:
        if profile is not None:
            rc = lib.aec48_set_profile(a, profile)
            if rc != 0:
                raise AssertionError(f"fixed profile {profile}: set={rc}")
        lib.aec48_set_enabled(a, 1)
        if integer:
            m = (mic * 32767).astype('int16')
            r = (ref * 32767).astype('int16')
            y = np.zeros_like(m)
            fn, typ = lib.aec48_process_s16, C.c_int16
        else:
            m, r = mic, ref
            y = np.zeros_like(m)
            fn, typ = lib.aec48_process, C.c_float
        for j in range(BLOCKS):
            if j in (30, 45):
                lib.aec48_set_enabled(a, int(j == 45))
            sl = slice(j * block, (j + 1) * block)
            rc = fn(a, ptr(m[sl], typ), ptr(r[sl], typ), ptr(y[sl], typ), block)
            if rc != 0:
                raise AssertionError(f"process returned {rc}")
            if (j + 1) * block >= 384 and profile is not None and lib.aec48_get_profile(a) != profile:
                raise AssertionError(f"fixed profile {profile} did not become active at internal block boundary")
        return y
    finally:
        lib.aec48_destroy(a)


def byte_regression(lib, baseline_dir, out_dir):
    count = 0
    total = 0
    digest = hashlib.sha256()
    comparisons = []
    for profile, name in ((SING, "default"), (TALK, "talk")):
        baseline = Path(baseline_dir) / name / "snapshot.npz"
        if not baseline.is_file():
            raise FileNotFoundError(f"missing改动前基线: {baseline}")
        with np.load(baseline) as old:
            expected = {k: old[k].copy() for k in old.files}
        actual = {}
        for i in range(12):
            sr, block, mic, ref = random_case(i)
            for integer in (False, True):
                key = f"{i}_{integer}"
                y = run_fixed(lib, profile, sr, block, mic, ref, integer)
                actual[key] = y
                if key not in expected or y.tobytes() != expected[key].tobytes():
                    raise AssertionError(f"byte regression: profile={name}, case={key}")
                digest.update(y.tobytes())
                total += y.size
                count += 1
        np.savez(Path(out_dir) / f"{name}_fixed.npz", **actual)
        comparisons.append({"profile": name, "cases": len(actual), "bytes_identical": True})
    return {"cases": count, "samples": total, "sha256": digest.hexdigest(), "profiles": comparisons}


def independent_regression(lib, baseline_dir, out_dir, seed, cases):
    """Final blind set: fresh seeds, compared byte-for-byte to frozen single-profile releases."""
    refs = {SING: bind(Path(baseline_dir) / "default" / "libaec48.so", profile_api=False),
            TALK: bind(Path(baseline_dir) / "talk" / "libaec48.so", profile_api=False)}
    digest = hashlib.sha256()
    count = total = 0
    saved = {}
    for i in range(cases):
        rng = np.random.default_rng(seed + i)
        sr, block = [48000, 44100][i % 2], [384, 256, 768][(i // 2) % 3]
        n = block * BLOCKS
        ref = (rng.normal(size=n) * 10 ** rng.uniform(-4, -1)).astype(np.float32)
        gap0 = (i % 5) * n // 10
        gap1 = min(n, gap0 + n // 7)
        ref[gap0:gap1] = 0
        delay = int(rng.integers(64, min(1536, n // 4)))
        mic = (np.roll(ref, delay) * rng.uniform(.2, .85) + rng.normal(size=n) * .0015).astype(np.float32)
        for profile, label in ((SING, "sing"), (TALK, "talk")):
            for integer in (False, True):
                y = run_fixed(lib, profile, sr, block, mic, ref, integer)
                expected = run_fixed(refs[profile], None, sr, block, mic, ref, integer)
                if y.tobytes() != expected.tobytes():
                    raise AssertionError(f"blind byte regression: seed={seed+i}, profile={label}, int16={integer}")
                key = f"{i}_{label}_{int(integer)}"
                saved[key] = y
                digest.update(y.tobytes())
                count += 1
                total += y.size
    np.savez(Path(out_dir) / "independent_random.npz", **saved)
    return {"seed_start": seed, "seeds": cases, "blocks_per_case": BLOCKS,
            "cases": count, "samples": total, "byte_identical_to_release": True, "sha256": digest.hexdigest()}


def voice_sequence(sr, seconds, seed):
    rng = np.random.default_rng(seed)
    n = sr * seconds
    # 远端单讲：宽带语音带包络驱动，仅用同一 ref 的延迟回声生成 mic。
    white = rng.standard_normal(n).astype(np.float32)
    ref = np.empty(n, dtype=np.float32)
    state = 0.0
    for i, x in enumerate(white):
        state = 0.92 * state + 0.08 * float(x)
        env = 0.55 + 0.45 * math.sin(2 * math.pi * (i / sr) * 2.3) ** 2
        ref[i] = 0.12 * env * state
    delayed = np.zeros_like(ref)
    delayed[521:] = ref[:-521]
    mic = 0.58 * delayed + (rng.standard_normal(n).astype(np.float32) * 0.00015)
    return mic, ref


def run_sequence(lib, mode, mic, ref, sr, block, start_profile, gate_on=True):
    n = len(mic)
    if n % block:
        raise ValueError("input length must be an exact block multiple")
    a = lib.aec48_create(sr, block)
    if not a:
        raise RuntimeError("sequence create failed")
    try:
        if lib.aec48_set_profile(a, start_profile) != 0:
            raise AssertionError("initial profile rejected")
        if lib.aec48_set_talk_gate(a, int(gate_on), -3.0) != 0:
            raise AssertionError("dual talk gate API rejected")
        out = np.empty(n, dtype=np.float32)
        durations = []
        latency = lib.aec48_latency_samples(a)
        block_count = n // block
        at10, at20 = 10 * sr // block, 20 * sr // block
        for j in range(block_count):
            if mode == "switch":
                if j == at10 and lib.aec48_set_profile(a, SING) != 0:
                    raise AssertionError("talk->sing request rejected")
                if j == at20 and lib.aec48_set_profile(a, TALK) != 0:
                    raise AssertionError("sing->talk request rejected")
            elif mode == "fresh" and j == 0:
                pass
            sl = slice(j * block, (j + 1) * block)
            inputs = [ptr(x[sl], C.c_float) for x in (mic, ref, out)]
            t0 = time.perf_counter_ns()
            rc = lib.aec48_process(a, *inputs, block)
            durations.append(time.perf_counter_ns() - t0)
            if rc != 0:
                raise AssertionError(f"process returned {rc}")
            if j == 0 and lib.aec48_get_profile(a) != start_profile:
                raise AssertionError("initial profile did not take effect on first block")
        if not np.isfinite(out).all():
            raise AssertionError("non-finite sequence output")
        return out, latency, np.asarray(durations, dtype=np.float64) / 1e6
    finally:
        lib.aec48_destroy(a)


def aligned_metrics(mic, out, sr, latency, start_s, end_s):
    # out[t + latency] corresponds to mic[t], matching aec48_check's alignment.
    lo = int(start_s * sr) + latency
    hi = int(end_s * sr) + latency
    x = mic[int(start_s * sr):int(end_s * sr)].astype(np.float64)
    y = out[lo:hi].astype(np.float64)
    n = min(x.size, y.size)
    pm, po = float(np.mean(x[:n] ** 2)), float(np.mean(y[:n] ** 2))
    erle = 10 * math.log10((pm + 1e-30) / (po + 1e-30))
    return {"mic_power": pm, "out_power": po, "suppression_db": erle}


def switch_check(lib, out_dir, gate_on=True):
    sr, block, seconds = 48000, 384, 30
    mic, ref = voice_sequence(sr, seconds, 202610170017)
    switched, latency, timing = run_sequence(lib, "switch", mic, ref, sr, block, TALK, gate_on)
    continuous_sing, _, _ = run_sequence(lib, "continuous", mic, ref, sr, block, SING, gate_on)
    continuous_talk, _, _ = run_sequence(lib, "continuous", mic, ref, sr, block, TALK, gate_on)
    results = {}
    for start_s, profile, continuous in ((10, "sing", continuous_sing), (20, "talk", continuous_talk)):
        fresh_mic = mic[start_s * sr:(start_s + 2) * sr]
        fresh_ref = ref[start_s * sr:(start_s + 2) * sr]
        fresh, fresh_latency, _ = run_sequence(lib, "fresh", fresh_mic, fresh_ref, sr, block,
                                                SING if profile == "sing" else TALK, gate_on)
        switched_metric = aligned_metrics(mic, switched, sr, latency, start_s, start_s + 1)
        continuous_metric = aligned_metrics(mic, continuous, sr, latency, start_s, start_s + 1)
        fresh_metric = aligned_metrics(fresh_mic, fresh, sr, fresh_latency, 0, 1)
        lo, hi = start_s * sr + latency, (start_s + 1) * sr + latency
        ys, yc = switched[lo:hi].astype(np.float64), continuous[lo:hi].astype(np.float64)
        n = min(ys.size, yc.size)
        difference_db = 10 * math.log10((float(np.mean((ys[:n] - yc[:n]) ** 2)) + 1e-30) /
                                        (float(np.mean(yc[:n] ** 2)) + 1e-30))
        results[f"{start_s}s_{profile}"] = {
            "switched": switched_metric,
            "continuous_target": continuous_metric,
            "fresh_instance": fresh_metric,
            "delta_vs_continuous_db": switched_metric["suppression_db"] - continuous_metric["suppression_db"],
            "delta_vs_fresh_db": switched_metric["suppression_db"] - fresh_metric["suppression_db"],
            "switched_vs_continuous_waveform_error_db": difference_db,
        }
    at10, at20 = 10 * sr // block, 20 * sr // block
    # Profile change calls take effect on these block boundaries. Four following
    # blocks conservatively cover the configured 2-4 frame crossfade interval.
    fade_idx = np.r_[at10:at10 + 4, at20:at20 + 4]
    steady_idx = np.r_[2 * sr // block:at10 - 4, at10 + 4:at20 - 4, at20 + 4:len(timing)]
    timing_stats = {}
    for label, idx in (("steady", steady_idx), ("fade_peak_window", fade_idx)):
        v = timing[idx]
        timing_stats[label] = {"blocks": int(v.size), "mean_ms": float(v.mean()),
                               "p99_ms": float(np.quantile(v, .99)), "max_ms": float(v.max())}
    np.savez(Path(out_dir) / ("switch_trajectory" + ("" if gate_on else "_gate_off") + ".npz"), mic=mic, ref=ref, switched=switched,
             continuous_sing=continuous_sing, continuous_talk=continuous_talk)
    return {"gate_on": gate_on, "duration_s": seconds, "sample_rate": sr, "block": block, "latency_samples": latency,
            "seed": 202610170017, "post_switch_metrics": results, "timing": timing_stats}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dual", required=True)
    p.add_argument("--baseline-dir", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--seed", type=int, default=202610170041,
                   help="final blind random-set seed start; keep separate from single-case tuning")
    p.add_argument("--random-cases", type=int, default=48)
    args = p.parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    lib = bind(args.dual)
    byte_result = byte_regression(lib, args.baseline_dir, out_dir)
    blind_result = independent_regression(lib, args.baseline_dir, out_dir, args.seed, args.random_cases)
    switch_result = switch_check(lib, out_dir)
    isolated_result = switch_check(lib, out_dir, gate_on=False)
    result = {"dual_library": str(Path(args.dual).resolve()), "fixed_profile_byte_regression": byte_result,
              "independent_random_regression": blind_result, "switch_and_timing": switch_result,
              "switch_gate_off_diagnostic": isolated_result}
    target = out_dir / "dual_regression.json"
    target.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
