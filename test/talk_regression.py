#!/usr/bin/env python3
"""Independent deterministic float/s16 byte regression, before/after shared libraries."""
import argparse
import ctypes as C
import hashlib
from pathlib import Path
import numpy as np

p = argparse.ArgumentParser()
p.add_argument('library')
p.add_argument('output')
p.add_argument('--compare')
p.add_argument('--talk-highpass-off', action='store_true',
               help='disable the 0.10 talk high-pass before processing for 0.9 byte comparisons')
a = p.parse_args()
lib = C.CDLL(str(Path(a.library).resolve()))
lib.aec48_create.argtypes = [C.c_int, C.c_int]
lib.aec48_create.restype = C.c_void_p
lib.aec48_destroy.argtypes = [C.c_void_p]
lib.aec48_set_enabled.argtypes = [C.c_void_p, C.c_int]
if a.talk_highpass_off:
    lib.aec48_set_talk_highpass.argtypes = [C.c_void_p, C.c_int]
    lib.aec48_set_talk_highpass.restype = C.c_int
for name, dtype in [('aec48_process', C.c_float), ('aec48_process_s16', C.c_int16)]:
    getattr(lib, name).argtypes = [C.c_void_p] + [C.POINTER(dtype)] * 3 + [C.c_int]
out = {}
for i in range(12):
    # Frozen independent seeds, rates, sizes and gain trajectories; no tuning samples.
    rng = np.random.default_rng(202610170001 + i)
    sr, block = [48000, 44100][i % 2], [384, 256, 768][i % 3]
    n = block * 80
    ref = (rng.normal(size=n) * 10 ** rng.uniform(-4, -1)).astype('float32')
    ref[n // 3:n // 2] = 0
    mic = (np.roll(ref, 710) * .6 + rng.normal(size=n) * .002).astype('float32')
    for integer in (False, True):
        dtype = C.c_int16 if integer else C.c_float
        m = (mic * 32767).astype('int16') if integer else mic
        r = (ref * 32767).astype('int16') if integer else ref
        y = np.zeros_like(m)
        h = lib.aec48_create(sr, block)
        assert h
        if a.talk_highpass_off:
            assert lib.aec48_set_talk_highpass(h, 0) == 0
        for j in range(80):
            if j in (30, 45): lib.aec48_set_enabled(h, int(j == 45))
            sl = slice(j * block, (j + 1) * block)
            args = [x[sl].ctypes.data_as(C.POINTER(dtype)) for x in (m, r, y)]
            assert getattr(lib, 'aec48_process_s16' if integer else 'aec48_process')(h, *args, block) == 0
        lib.aec48_destroy(h)
        out[f'{i}_{integer}'] = y
np.savez(a.output, **out)
if a.compare:
    with np.load(a.compare) as z:
        assert set(z.files) == set(out)
        assert all(z[k].tobytes() == v.tobytes() for k, v in out.items()), 'byte regression'
print(f'PASS {len(out)} cases / {sum(x.size for x in out.values())} samples; SHA256 ' + hashlib.sha256(b''.join(x.tobytes() for x in out.values())).hexdigest())
