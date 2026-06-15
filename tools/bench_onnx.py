import os, time, subprocess, sys, statistics, json

def rss_mb(pid=None):
    import resource
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0

# ---- Benchmark 2: representative HuBERT-base encoder, torch eager vs ORT ----
import numpy as np
import torch, torch.nn as nn
torch.set_num_threads(4)

D, H, FF, L = 768, 12, 3072, 12   # HuBERT base
class Enc(nn.Module):
    def __init__(s):
        super().__init__()
        s.layers = nn.ModuleList([nn.TransformerEncoderLayer(D, H, FF, batch_first=True, activation='gelu', norm_first=False) for _ in range(L)])
    def forward(s, x):
        for l in s.layers: x = l(x)
        return x

m = Enc().eval()
T = 150  # ~3 s of audio at 50 fps (HuBERT frame rate)
x = torch.randn(1, T, D)

def timeit(fn, n=30, warm=5):
    for _ in range(warm): fn()
    ts=[]
    for _ in range(n):
        t=time.perf_counter(); fn(); ts.append((time.perf_counter()-t)*1000)
    return statistics.median(ts), min(ts)

with torch.no_grad():
    torch_med, torch_min = timeit(lambda: m(x))

# export to ONNX
onnx_path="/tmp/enc.onnx"
with torch.no_grad():
    torch.onnx.export(m, x, onnx_path, input_names=['x'], output_names=['y'],
                      dynamic_axes={'x':{1:'T'},'y':{1:'T'}}, opset_version=17)

import onnxruntime as ort
so = ort.SessionOptions(); so.intra_op_num_threads=4; so.graph_optimization_level=ort.GraphOptimizationLevel.ORT_ENABLE_ALL
sess = ort.InferenceSession(onnx_path, so, providers=['CPUExecutionProvider'])
xnp = x.numpy()
ort_med, ort_min = timeit(lambda: sess.run(None, {'x':xnp}))

# numerical parity
with torch.no_grad(): y_torch = m(x).numpy()
y_ort = sess.run(None, {'x':xnp})[0]
max_abs = float(np.max(np.abs(y_torch - y_ort)))

print(json.dumps({
  "model":"HuBERT-base encoder (12 layers, d=768, 12 heads, ffn=3072)",
  "input":"[1,150,768] (~3s @ 50fps)",
  "torch_eager_ms_median": round(torch_med,2), "torch_eager_ms_min": round(torch_min,2),
  "ort_ms_median": round(ort_med,2), "ort_ms_min": round(ort_min,2),
  "speedup_median": round(torch_med/ort_med,2),
  "max_abs_diff": max_abs,
  "onnx_size_mb": round(os.path.getsize(onnx_path)/1e6,1),
}, indent=2))
