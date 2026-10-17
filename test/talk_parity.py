#!/usr/bin/env python3
"""Talk head/gate/full Python chain audit. Uses existing local reference sources only."""
import os
os.environ['CUDA_VISIBLE_DEVICES'] = ''
for k in ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'NUMEXPR_NUM_THREADS'):
    os.environ[k] = '1'
os.environ.update(AEC48_ARCH='cofi', AEC48_COFI='large', AEC48_DMIC='1', AEC48_HINT='1',
                  AEC48_NFFT='1536', AEC48_HOP='384', AEC48_FEAT_RHO='1', AEC48_WIDTH='32',
                  AEC48_FEAT_REF='1', AEC48_MASK='res', AEC48_MASK_FLOOR_DB='20',
                  AEC48_TALK='1', AEC48_TALK_HEAD='gru2', TDE2_A='0.98', NLIN_DUAL='1',
                  NLIN_TDE='2', NLIN_REFNORM='1', NLIN_CAUSAL='1')
import argparse
import ast
import ctypes as C
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
import numpy as np
import torch
torch.set_num_threads(1)
torch.set_num_interop_threads(1)
ROOT = Path(__file__).resolve().parents[1]
import os
TALK_SRC = Path(os.environ['AEC48_TALK_SRC'])   # 训练侧讲话模型代码目录（含 cofi_aec48.py）
GATE_PY = Path(os.environ['AEC48_GATE_PY'])   # 离线门参考实现（gate_wav.py）
TH = float(os.environ.get('AEC48_TALK_TH', '-3.0'))   # 0.8.0 审查：两侧门限一致；--mode gate 的 C 工具用库默认 −3.0，改 TH 只对 chain 模式有效（gate 模式会拒绝）
DATA = Path(os.environ.get('AEC48_CKPT_DIR', '.'))
sys.path.insert(0, os.environ['AEC48_PY_ROOT'])
spec = importlib.util.spec_from_file_location('cofi_aec48', TALK_SRC/'cofi_aec48.py')
cofi = importlib.util.module_from_spec(spec)
sys.modules['cofi_aec48'] = cofi
spec.loader.exec_module(cofi)
import gtcrn_aec48 as G

def gate(ref, logits):
    src = ast.parse((GATE_PY).read_text())
    defs = [x for x in src.body if isinstance(x, ast.FunctionDef)]
    scope = dict(np=np, SR=48000, HOP=384, FARABS=-70)
    exec(compile(ast.Module(body=defs, type_ignores=[]), 'gate_wav.py', 'exec'), scope)
    g = scope['gain'](ref, logits > TH)
    return np.interp(np.arange(len(ref)), np.arange(len(g))*384+192, g)

def stats(v):
    v = np.abs(v).astype(float).reshape(-1)
    return dict(n=len(v), median=float(np.median(v)), p95=float(np.percentile(v,95)),
                p99=float(np.percentile(v,99)), maximum=float(v.max()))

def model():
    net = G.GTCRN_AEC48().eval()
    net.load_state_dict(torch.load(DATA/'aec48_model_talk_e1be12.pt', map_location='cpu', weights_only=False)['model'])
    return net

def pcm(p): return np.fromfile(p, np.int16).astype('float32')/32768
def eratio(x,y): return float(10*np.log10((np.square(x-y).sum()+1e-30)/(np.square(y).sum()+1e-30)))

p = argparse.ArgumentParser()
p.add_argument('--build', default='build-host-talk-test')
p.add_argument('--output', default='build-talk-evidence')
p.add_argument('--mode', choices=['gate','chain','snapshot'], required=True)
a = p.parse_args()
od = ROOT/a.output
od.mkdir(exist_ok=True, parents=True)
if a.mode == 'gate':
    rng = np.random.default_rng(2026101742)
    T = 900
    ref = (rng.normal(size=T*384)*.01).astype('float32')
    ref.reshape(T,384)[50:400] = 0
    ref.reshape(T,384)[650:] *= 1e-5
    logits = rng.normal(-3.5, 1, T).astype('float32')
    logits[100:450] = -9
    logits[450:480] = 0
    path = od/'gate_input.bin'
    with path.open('wb') as f:
        np.array([T],np.int32).tofile(f); ref.tofile(f); logits.tofile(f)
    if TH != -3.0: sys.exit('gate 模式的 C 工具固定用库默认门限 −3.0；AEC48_TALK_TH 非默认时只能跑 chain 模式')
    subprocess.run([str(ROOT/a.build/'aec48_talk_gate'),'--dump',str(path),str(od/'gate_gain.bin')], check=True)
    c = np.fromfile(od/'gate_gain.bin',np.float32)
    py = gate(ref,logits)
    result = stats(c-py)
    result['pass'] = result['maximum'] <= 1e-5
    print(json.dumps(result,indent=2))
    (od/'gate_parity.json').write_text(json.dumps(result,indent=2))
    assert result['pass']
elif a.mode == 'snapshot':
    lib=C.CDLL(str((ROOT/a.build/'libaec48.so').resolve()))
    ptr=C.POINTER(C.c_float)
    lib.aec48_create.argtypes=[C.c_int,C.c_int]; lib.aec48_create.restype=C.c_void_p
    lib.aec48_destroy.argtypes=[C.c_void_p]
    lib.aec48_process.argtypes=[C.c_void_p,ptr,ptr,ptr,C.c_int]
    lib.aec48_set_enabled.argtypes=[C.c_void_p,C.c_int]
    lib.aec48_set_talk_gate.argtypes=[C.c_void_p,C.c_int,C.c_float]
    snap=lib.aec48_internal_talk_snapshot
    snap.argtypes=[C.c_void_p]+[ptr]*7
    name='03ra86gHhkWD'
    m,r=[pcm(ROOT/'test/vec'/f'{name}_{k}.pcm') for k in ('mic','ref')]
    n=min(len(m),len(r))//384*384
    m,r=m[:n],r[:n]
    outputs=[]; zs=[]; ls=[]; spectra=[]
    for enabled in (True,False):
        h=lib.aec48_create(48000,384); assert h
        assert lib.aec48_set_talk_gate(h,int(enabled),TH)==0
        y=np.zeros(n,np.float32)
        buffers=[np.zeros(829,np.float32),np.zeros(1,np.float32)]+[np.zeros(1538,np.float32) for _ in range(5)]
        for t in range(0,n,384):
            assert lib.aec48_process(h,*[x[t:t+384].ctypes.data_as(ptr) for x in (m,r,y)],384)==0
            if enabled and snap(h,*[b.ctypes.data_as(ptr) for b in buffers]):
                zs.append(buffers[0].copy()); ls.append(buffers[1][0]); spectra.append(np.stack(buffers[2:]).copy())
        lib.aec48_destroy(h); outputs.append(y)
    z=np.stack(zs); logits=np.array(ls); ss=np.stack(spectra)
    net=model()
    results={}
    for dtype in (torch.float32,torch.float64):
        head=net.talk_head2.to(dtype)
        x=torch.from_numpy(z).to(dtype)[None]
        with torch.no_grad():
            x=head[1](head[0](x)); x=head[2](x)[0]; py=head[3](x)[0,:,0].numpy()
        results[str(dtype)]=stats(py-logits)
        assert results[str(dtype)]['maximum']<=1e-3
    net.float()
    captured=[]
    net.talk_head2[0].register_forward_pre_hook(lambda _,args: captured.append(args[0].detach().numpy()[0].copy()))
    tx=lambda x: torch.from_numpy(x.reshape(len(x),769,2).transpose(1,0,2).copy())[None]
    with torch.no_grad(): net(tx(ss[:,0]),tx(ss[:,1]),tx(ss[:,2]))
    results['same_spectra_talk_input']=stats(captured[0]-z)
    results['same_spectra_logit']=stats(net.last_talk[0].numpy()-logits)
    g=gate(r[:len(logits)*384],logits)
    usable=n-1152
    expected=outputs[1][1152:]*g[:usable]
    results['integrated_gate_output']=stats(outputs[0][1152:]-expected)
    assert results['integrated_gate_output']['maximum']<=1e-5
    np.savez(od/'talk_snapshot.npz',z=z,logits=logits,se=ss[:,0],sy=ss[:,1],sx=ss[:,2])
    print(json.dumps(results,indent=2))
    (od/'head_parity.json').write_text(json.dumps(results,indent=2))
elif a.mode == 'chain':
    # Python original full linear stage, original talk mask and published /g wave synthesis.
    sys.path.insert(0,str(ROOT.parent/'aec48/dproto'))
    os.environ['NLINBIN_CK'] = str(DATA/'nlin/nlinBc3_best.pt')
    import nlin_bin as NB
    import nlin as NL
    original_stft = torch.stft
    torch.stft = lambda *v, **kw: original_stft(*v, **{**kw,'pad_mode':kw.get('pad_mode','constant')})
    ck = torch.load(os.environ['NLINBIN_CK'],map_location='cpu',weights_only=False)
    lin = NL.NLin(ck['kind']).eval(); lin.load_state_dict(ck['model'])
    net = model()
    results = {}
    for name in ('00t5hG031EmZ','03ra86gHhkWD'):
        m,r = [pcm(ROOT/'test/vec'/f'{name}_{k}.pcm') for k in ('mic','ref')]
        n = min(len(m),len(r))//384*384
        m,r = m[:n],r[:n]
        os.environ['NLINBIN_SPEC'] = str(od/f'{name}_E.npy')
        os.environ['NLINBIN_SPEC_D'] = str(od/f'{name}_D.npy')
        with torch.no_grad(): _,lf = NB.run(m,r,lin)
        E,D = [np.load(od/f'{name}_{k}.npy') for k in ('E','D')]
        t = np.arange(n); src = t-lf[np.minimum(t//384,len(lf)-1)]*384
        xa = np.where(src>=0,r[np.maximum(src,0)],0)
        # nlin_bin CLI quantizes aligned reference before the mask evaluation.
        xa = (np.clip(xa,-1,1)*32767).astype('int16').astype('float32')/32768
        w = torch.hann_window(1536).sqrt()
        XA = torch.stft(torch.from_numpy(xa),1536,384,1536,w,center=True,pad_mode='constant',return_complex=True).T.numpy()
        pg=-1.; gs=[]; ga=1-np.exp(-384/(10*48000))
        for frame in E:
            gs.append(10**(-25/20)/(np.sqrt(pg if pg>0 else 1e-8)+1e-6))
            power=np.abs(frame.astype('complex128'))**2
            power=(power[0]+power[-1]+2*power[1:-1].sum())/(1536**2*.5)
            if pg<0:
                if power>1e-8: pg=power
            else: pg+=ga*(power-pg)
        gs=np.array(gs,np.float32)
        tensor=lambda x: torch.view_as_real(torch.from_numpy(x.T.copy()).contiguous())[None]
        gt=torch.from_numpy(gs)[None,None,:,None]
        with torch.no_grad(): so=net(tensor(E)*gt,tensor(D-E)*gt,tensor(XA)*gt)
        sc=torch.view_as_complex(so[0].permute(1,0,2).contiguous()).T
        wave=torch.istft(sc,1536,384,1536,w,length=n).numpy()/np.repeat(gs,384)[:n]
        logits=net.last_talk[0].numpy()
        gain=gate(r,logits)
        py=wave*gain
        # Also isolate the documented synthesis difference: divide each spectral frame by g first.
        wave_c=torch.istft(sc/torch.from_numpy(gs)[None,:],1536,384,1536,w,length=n).numpy()*gain
        lib=C.CDLL(str((ROOT/a.build/'libaec48.so').resolve()))
        lib.aec48_create.argtypes=[C.c_int,C.c_int];lib.aec48_create.restype=C.c_void_p
        lib.aec48_process.argtypes=[C.c_void_p]+[C.POINTER(C.c_float)]*3+[C.c_int]
        lib.aec48_destroy.argtypes=[C.c_void_p]
        lib.aec48_set_talk_gate.argtypes=[C.c_void_p,C.c_int,C.c_float]; lib.aec48_set_talk_gate.restype=C.c_int
        h=lib.aec48_create(48000,384); assert h
        assert lib.aec48_set_talk_gate(h,1,TH)==0   # 0.8.1 复审：C 侧门限与 Python 参照一致
        y=np.zeros(n+1536,np.float32)
        mm=np.pad(m,(0,1536)); rr=np.pad(r,(0,1536))
        for t in range(0,len(y),384):
            ps=[x[t:t+384].ctypes.data_as(C.POINTER(C.c_float)) for x in (mm,rr,y)]
            assert lib.aec48_process(h,*ps,384)==0
        lib.aec48_destroy(h)
        c=y[1152:1152+n]
        result=dict(samples=n,full_reference_error_db=eratio(c,py),
                    common_unflushed_error_db=eratio(c[:-1152],py[:-1152]),
                    spectrum_unscale_reference_error_db=eratio(c,wave_c),
                    c_peak=float(np.abs(c).max()), python_peak=float(np.abs(py).max()))
        result['pass']=result['full_reference_error_db']<=-30
        results[name]=result
        np.savez(od/f'{name}_chain.npz',c=c,python=py,python_spectrum_unscale=wave_c,logits=logits)
        print(name,json.dumps(result),flush=True)
    (od/'chain_parity.json').write_text(json.dumps(results,indent=2))
    # Failure remains an audit result; never alter reference definitions to make it pass.
