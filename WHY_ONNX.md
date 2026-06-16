# WHY_ONNX — or, How We Run RVC Voice Conversion in a C++ Video Editor With Zero Python

> **Thesis:** Pop Maker Studio does real neural voice conversion — RVC, HuBERT/ContentVec, RMVPE pitch, FAISS retrieval — inside a native C++ app that ships **no Python, no PyTorch, no libtorch, not one `.py` at runtime**. We read PyTorch `.pth` checkpoints directly in C++, hand-emit ONNX protobuf at the wire level, and run everything on ONNX Runtime.
>
> This document explains why that was the right call, benchmarks it against the "just ship PyTorch" alternative, and walks through the genuinely hard engineering that made it numerically correct.

---

## TL;DR — the numbers

Measured on this machine (Ryzen 5 5600X, CPU inference, 4 intra-op threads, `onnxruntime 1.26`, `torch 2.12+cpu`). Benchmark scripts are reproducible — see [Appendix A](#appendix-a--benchmark-methodology).

### Inference: ONNX Runtime vs PyTorch eager

A HuBERT-base-shaped encoder (12 transformer layers, d=768, 12 heads, ffn=3072) over ~3 s of audio (`[1, 150, 768]`):

| Runtime | Median latency | Best | Relative |
|---|---:|---:|---:|
| PyTorch eager (CPU, 4 threads) | 116.5 ms | 111.1 ms | 1.00× |
| **ONNX Runtime (CPU, 4 threads, `ORT_ENABLE_ALL`)** | **83.4 ms** | **79.9 ms** | **1.40× faster** |

Numerical parity: **max abs difference 3.3e-6** — i.e. bit-for-bit equivalent within float32 rounding. We aren't trading correctness for speed; ORT is just a better executor of the same math.

### The part that actually matters: you don't ship a 750 MB Python runtime

| Metric | Ship PyTorch | **Ship ONNX Runtime** | Win |
|---|---:|---:|---:|
| Cold import / init | 877 ms | **76 ms** | **11.5× faster** |
| Peak RSS just to load the lib | 229 MB | **43 MB** | **5.3× lighter** |
| On-disk runtime | 753 MB (torch pkg) + a Python interpreter | **22.6 MB** (`libonnxruntime.so`) | **~33× smaller** |
| Languages in the shipped product | C++ **and** Python | **C++ only** | the two-language problem, deleted |

The shipped binary's dependency on the entire ML stack is a single 22.6 MB shared object:

```
$ ldd build/pop-maker-studio | grep -iE "python|torch|c10|caffe"
NONE — no python/torch/libtorch linked
```

That's the whole pitch. A creative desktop app cannot reasonably bundle a 750 MB PyTorch install + a CPython interpreter + a venv and call it a feature. ONNX Runtime is one `.so` you link like any other native library, it starts in 76 ms instead of nearly a second, and it runs the inference *faster*.

---

## Why PyTorch eager is the wrong thing to ship for inference

This isn't a knock on PyTorch — it's the best *research/training* framework there is. But for *deploying inference in a native app*, its design goals are working against you:

1. **Eager mode pays per-op dispatch overhead.** Every operation is dispatched through PyTorch's dynamic dispatcher (device/dtype/autograd keys) before it reaches an ATen kernel. That machinery is what makes PyTorch flexible and debuggable in research; in a tight inference loop it's pure tax. A static, pre-compiled graph has none of it.

2. **You ship the entire training stack to do inference.** Autograd, the optimizer zoo, the dispatcher, training-only CUDA kernels, the full ATen surface — all of it is in that 753 MB, none of it runs when you're just doing a forward pass. ONNX Runtime is an *inference* engine; it carries only what inference needs.

3. **ONNX Runtime optimizes the graph; eager can't.** ORT does ahead-of-time, whole-graph work that eager fundamentally cannot: constant folding, dead-node elimination, **operator fusion** (LayerNorm, attention, Conv chains collapsed into single fused kernels), and static memory planning / buffer reuse. That's where our 1.40× comes from — and the fusion advantage *grows* with transformer depth, which is exactly the shape of HuBERT and RVC.

4. **The two-language problem.** Shipping PyTorch means shipping Python: a venv, an interpreter, packaging headaches, version skew, and a startup cost users feel. The ONNX/ORT path is "one native lib, one IR file" — the IR (`.onnx`) is a portable, versioned, self-describing graph. (`torch.compile`/TorchScript exist, but they don't remove the Python+torch dependency from the deployed artifact; ONNX does.)

So the strategy: **convert once, in C++, to ONNX; run forever on ORT.** The catch is that "convert once" turned out to be the hard part — because we refused to even use Python for the conversion.

---

## The architecture

Two phases, both Python-free:

```
EXPORT (one-time, pure C++)
  hubert_base.pt (ContentVec) ──[pth_reader]──[hand-rolled ONNX writer]──> hubert.onnx
  <voice>.pth (RVC)           ──[pth_reader]──[hand-rolled ONNX writer]──> <voice>.onnx + sidecar.json

INFERENCE (per conversion, ONNX Runtime)
  ffmpeg → 16kHz mono
        ├─ RMVPE.onnx ──────────────► F0 (pitch, Hz)
        └─ HuBERT.onnx ─► features ─► FAISS .index retrieve ─► repeat-interleave ×2 ─┐
                                                                                      ▼
                                          RVC.onnx (enc_p → flow⁻¹ → NSF decoder) ─► audio → ffmpeg
```

| File | Lines | Job |
|---|---:|---|
| `src/pth_reader.cpp` | 760 | A from-scratch **pickle VM** + `.pth` (zip) parser — reads PyTorch checkpoints with no libtorch |
| `src/hubert_onnx.cpp` | 888 | HuBERT/ContentVec content-encoder → ONNX (export + the hand-rolled protobuf writer) |
| `src/rvc_onnx.cpp` | 1432 | RVC `SynthesizerTrnMsNSFsid` → ONNX (the whole VITS+NSF graph, by hand) |
| `src/rmvpe_onnx.cpp` | 188 | RMVPE pitch estimator — Slaney log-mel front-end (FFTW) + argmax/cents decode |
| `src/vc_onnx.cpp` | 430 | Orchestration: decode → RMVPE → HuBERT → FAISS → interleave → RVC → encode |
| `src/faiss_ivf.h` | — | A from-scratch `IndexIVFFlat` parser (FAISS retrieval, no faiss dependency) |
| `tools/export_hubert.cpp` | 88 | The C++ HuBERT→ONNX exporter CLI |

~3,800 lines of C++ standing in for what's normally "pip install torch onnx onnxruntime, write a 40-line Python export script." We did the opposite, and here's why each piece is genuinely cool.

---

## Cool thing #1: a complete pickle VM in C++ (`pth_reader.cpp`)

A `.pth` file is a zip containing `data.pkl` (a Python **pickle** stream) plus raw tensor storage blobs (`data/0`, `data/1`, …). To read it without libtorch you have to interpret pickle bytecode — so we wrote a stack-machine interpreter for it (`Unpickler`, `pth_reader.cpp:88-434`).

It implements the opcodes PyTorch actually emits across **pickle protocols 2–5**: `PROTO`, `FRAME`, `MEMOIZE`, `STACK_GLOBAL`, `NEWOBJ_EX`, `GLOBAL`, `BINPUT`/`LONG_BINPUT`, `BINGET`, `TUPLE1/2/3`, `REDUCE`, `BINPERSID`, the `BININT*` family, `BINUNICODE*`, dict/list builders, `BUILD`, `OBJ`, `NEWOBJ`, `LONG1/4`, and `BINFLOAT`. Endianness is handled per-opcode — ints and raw float storage are little-endian, but `BINFLOAT` is **big-endian IEEE-754 double**, so it byte-reverses (`read_f64_be`, `:110`). Getting that wrong is a silent corruption, not a crash.

The two load-bearing special cases (`reduce`, `:163`):
- `torch._utils._rebuild_tensor_v2(storage, offset, shape, stride, …)` → reconstructs a tensor's **metadata**: it maps the storage class to a dtype (`FloatStorage→f32`, `HalfStorage→f16`, `LongStorage→i64`, …) and captures `storage_offset`, `shape`, `stride` (`:169-198`).
- Persistent load (`:149`) decodes the `('storage', StorageClass, file_idx, …)` tuple that points a tensor at its `data/{n}` blob.

Crucially it loads tensors **lazily** — the pickle pass recovers only metadata; the float bytes are `mmap`/`fread` on demand (`pth_load_tensor`, `:648`). And because most RVC weights ship as float16, there's a hand-written **half→float converter that correctly handles denormals and inf/nan** (`f16_to_f32`, `:630`) — not the lossy approximation people usually reach for. There's even a **stride-aware non-contiguous path** (`:718-748`) that walks multi-dimensional indices through the recovered strides when a tensor isn't densely packed.

> This is the unglamorous, load-bearing foundation: you cannot convert a model you cannot read, and "read PyTorch's serialization format from scratch in C++" is a real piece of systems work. It took follow-up fixes for protocol-4 opcodes (`FRAME`/`MEMOIZE`/`STACK_GLOBAL`, commit `aa1f73e`), the `OBJ`/`NEWOBJ` opcodes (`257db44`), and a fallback to the `model` key for fairseq checkpoints (`036e04c`) — i.e. it was hardened against the actual zoo of real checkpoints, not a toy.

---

## Cool thing #2: hand-emitted ONNX protobuf, at the wire level

There is **no protobuf library and no ONNX C++ API** anywhere in the exporters. ONNX is a protobuf schema (`ModelProto` → `GraphProto` → `NodeProto`/`TensorProto`/…), and we serialize it by writing protobuf wire bytes directly.

The entire serializer is one little struct (`PbBuf`, `rvc_onnx.cpp:27-53`):

```cpp
void varint(uint64_t v){ do { uint8_t b=v&0x7F; v>>=7; if(v) b|=0x80; d.push_back(b);} while(v); }
// tag byte = (field_number << 3) | wire_type
void tag_varint(int field,uint64_t v){ varint(((uint64_t)field<<3)|0); varint(v); }      // wire 0
void tag_i32   (int field,uint32_t v){ varint(((uint64_t)field<<3)|5); push_bytes(&v,4);} // wire 5
void tag_bytes (int field,const void*p,size_t n){ varint(((uint64_t)field<<3)|2); varint(n); push_bytes(p,n);} // wire 2
```

On top of that, an `OnnxGraph` assembles the model imperatively in SSA form. `emit(op, inputs, attrs, hint)` mints a unique output name and appends a `NodeProto`; a library of shorthands (`op_conv1d`, `op_matmul`, `op_transpose`, `op_slice`, `op_gather`, `op_softmax`, `emit_layer_norm`, `op_convtranspose1d`, …) builds the graph node by node. Weights are baked in as graph **initializers** with their raw little-endian float bytes in `TensorProto.raw_data` (field 9). Repeated fields are emitted packed or non-packed exactly per spec; dynamic dimensions become `dim_param` strings; there's even a hand-built `TENSOR`-typed attribute for `ConstantOfShape`. RVC exports at opset 14, HuBERT at opset 17.

> Think about what this means: the team encoded the ONNX spec — wire types, field numbers, varint LEB128, packed-repeated rules — *from the manual*, by hand, and it produces files that ONNX Runtime, Netron, and third-party RVC tools all load natively. That's the kind of thing most people assume "you need the library for."

### The war story: `AttributeProto.f` is field **2**, not field 4

This is the canonical hazard of hand-rolled protobuf, and it's instructive. Float attributes (`LeakyRelu alpha`, `RandomNormalLike scale`, …) were being written to protobuf **field 4**. The real field number for `AttributeProto.f` is **2**.

A wrong field number in protobuf doesn't error — it's *forward-compatible by design*. The float landed in a field nobody reads, got silently dropped, and **every float attribute in every exported model defaulted to 0.0**. The symptoms:
- `LeakyRelu(alpha=0.1)` → `alpha=0` → a plain ReLU everywhere in the network.
- `RandomNormalLike(scale=0.1)` → `scale=0` → ONNX Runtime asserts and dies.

The fix (`rvc_onnx.cpp:163`, mirrored in `hubert_onnx.cpp`):

```cpp
static PbBuf attr_float(const std::string& name, float v){
    PbBuf a; a.tag_string(1, name);
    uint32_t bits; memcpy(&bits, &v, 4);
    a.tag_i32(2, bits);          // AttributeProto.f is field 2   ← the one-character war
    a.tag_varint(20, kAttrFloat);
    return a;
}
```

No stack trace, no error, total numerical breakage — found by isolating each stage and diffing against reference. This is exactly the failure mode the hand-rolled approach invites, and exactly the kind of bug that proves the team actually understood the wire format well enough to debug it at the byte level (commit `1088094`).

---

## Cool thing #3: numerical parity with PyTorch on the genuinely fiddly bits

Re-implementing a model graph by hand means re-implementing every numerical subtlety *exactly*, or it produces "fluent gibberish" — output that's clearly the right kind of thing and completely wrong. These are the traps that were found and fixed (all in/around commit `1088094`), and each one is a little lesson in how these architectures actually work:

**Post-norm transformer placement (HuBERT).** HuBERT-base is **post-norm** (`layer_norm_first=False`): the encoder LayerNorm runs *before* the 12 layers, and inside each layer the LN comes *after* the residual add (`x = ln(x + sublayer(x))`, `hubert_onnx.cpp:784-800`). The first cut built a pre-norm stack. Pre- vs post-norm changes the activation statistics fed into every downstream layer — the features come out subtly off and the RVC decoder turns them into garbage. The placement of that *final* per-layer norm is the whole ballgame.

**Weight-norm over the *correct* dim.** PyTorch `weight_norm` reparametrizes `w = g · v/‖v‖`, where the norm is over **all dims except `dim`**. RVC's convs use `dim=0` (per-output-channel; `weight_g` is `[C_out,1,1]`), but fairseq's HuBERT `pos_conv` uses `dim=2` (per-kernel-tap; `weight_g` is `[1,1,128]`). Resolving a `[1,1,128]` `weight_g` with the dim-0 code reads out of bounds *and* normalizes wrong. So there are **two** resolvers picked by matching the `weight_g` shape (`hubert_onnx.cpp:331-346`): `apply_weight_norm` (dim-0) and `apply_weight_norm_lastdim` (last dim). Knowing that fairseq and RVC normalize over different axes is the kind of detail you only get from reading both source trees.

**The `×√hidden` embedding scale (VITS).** RVC's text encoder scales the summed phone+pitch embedding by `√hidden_channels` before the encoder (`rvc_onnx.cpp:820-822`):

```cpp
// x = x * sqrt(hidden_channels)  (RVC scales embeddings before the encoder)
std::string sqh_c = "ep_sqh"; g.add_scalar_f32(sqh_c, std::sqrt((float)hid));
auto emb_s = op_mul(g, emb_b, sqh_c, "ep_scale");
```

Drop that one multiply and every LayerNorm/attention statistic downstream is off by a constant `√192 ≈ 13.9`. Silent, catastrophic, one line.

**`emb_pitch`, sampling, and the relative-attention value term.** The first RVC graph was missing the coarse-pitch embedding (`emb_pitch`, gathered over bins 1..255 and added to the phone embedding), the relative-position **value** term `emb_rel_v` in attention (it had `emb_rel_k` but not `emb_rel_v`), and it took the *mean* `z = m_p` instead of **sampling** `z = m_p + exp(logs_p)·noise` with RVC's `0.66666` temperature. Each omission is plausible-looking and individually wrong; together they're the difference between "a voice" and "noise that almost sounds like a voice."

**The NSF source, rebuilt as ONNX ops.** The decoder's `SineGen` excitation — `CumSum` phase accumulation → `Mod 1.0` → `Sin·0.1`, a voiced mask at `f0 > 10 Hz`, and noise excitation for unvoiced frames — is reconstructed *entirely in the ONNX graph* (`rvc_onnx.cpp:1100-1165`). That's a signal-processing module expressed as a pure dataflow graph, which is genuinely elegant.

> The throughline: every one of these is a place where "it runs and produces audio" and "it produces *the right* audio" are separated by a single subtle numerical fact about the architecture. Getting all of them right, in a hand-written C++ graph, validated by ear and by diff — that's the work. That's the part to be proud of.

---

## Cool thing #4: reverse-engineering the training register from the weights

This one's just clever. RVC voices are fine-tuned, and an embedding row only receives gradient for the **pitch bins it actually saw during training**. So you can recover a voice's vocal range *from the weights alone*, with no audio and no synthesis: diff the model's `enc_p.emb_pitch` against the pretrained base (`models/rvc_pitch_bases.bin`, magic `PMSPB1\n`), take the per-bin norm of the difference, smooth over 9 bins, threshold at `0.3·peak`, and you have a 64-hex-char `register_mask` of "which pitches this voice was trained on" (`rvc_onnx.cpp:1359-1410`).

At convert time the caller scores the *source's* voiced frames against that mask for octave shifts `{0, +12, −12}` and picks the best fit (`vc_onnx.cpp:316-381`) — **automatic octave matching**, derived from a gradient-coverage signal baked into an embedding table. I have not seen anyone else do this (commit `b8ef6f0`).

---

## Cool thing #5: the inference path is lean and interop-correct

`vc_onnx.cpp` runs ONNX Runtime the way you want it run in a real-time app:

- **Sessions:** `SetIntraOpNumThreads(4)`, `ORT_ENABLE_ALL` graph optimization, and an *optional* CUDA provider wrapped in try/catch so a GPU is used when present and CPU-only machines fall through silently (`make_opts`, `:79-86`). The 360 MB RMVPE session is cached in a function-local `static` behind a mutex so it loads **once** across conversions (`rmvpe_onnx.cpp:134-145`).
- **Ecosystem interop:** the exporter emits the canonical RVC/w-okada ONNX input signature (`phone, phone_lengths, pitch, pitchf, ds, rnd`), so third-party MoeSS / w-okada exports run in our engine *and* our exports run in theirs (commit `d564234`). At load time we **probe the session's input names** to accept any subset, and we recover the model's sample rate purely from output length on a tiny `T=8` probe run (`tgt_sr = M·100/Tp`, using RVC's fixed 100 fps invariant) — no sidecar needed for sr.
- **FAISS retrieval, from scratch:** voice models ship a sibling `.index` (an `IndexIVFFlat` of training features). `faiss_ivf.h` parses it with no faiss dependency and blends each HuBERT frame toward its k=8 nearest training vectors at `index_rate=0.75` *before* the ×2 interleave — the "retrieval" that makes RVC sound like the target instead of a generic vocoder (commit `4d50c4c`).

---

## So, why ONNX? — the honest summary

Because the alternative is shipping a 750 MB Python+PyTorch runtime into a desktop creative tool to do *forward passes*, and that's absurd when:

- ONNX Runtime runs the same math **1.4× faster** (and the gap widens with depth, via fusion),
- to **3e-6** numerical parity,
- starting **11.5× faster**,
- in **5.3× less RAM**,
- as a single **22.6 MB** native `.so` with **zero** Python/torch in the linked binary.

The price was doing the conversion ourselves — a pickle VM, a wire-level ONNX writer, and pixel-perfect re-implementations of weight-norm axes, post-norm placement, the √hidden scale, VITS sampling, NSF source generation, and FAISS retrieval, all debugged down to a single protobuf field number. That price bought a feature that genuinely could not ship any other way: **neural voice conversion in a native app that a normal person can actually install.**

That's the why.

---

## Appendix A — benchmark methodology

All numbers above are reproducible on this machine.

- **Inference / parity:** build a 12-layer post-norm `TransformerEncoder` (d=768, 12 heads, ffn=3072) — HuBERT-base's shape — in PyTorch; time eager CPU inference over `[1,150,768]` (median of 30 runs after 5 warmups, `torch.set_num_threads(4)`); `torch.onnx.export` it at opset 17; time `onnxruntime` with `intra_op_num_threads=4` and `ORT_ENABLE_ALL`; diff outputs for `max_abs_diff`. (`tools/bench_onnx.py` — run `python3 tools/bench_onnx.py`)
- **Startup / RSS:** import each library in an isolated subprocess; `perf_counter` around the import; `getrusage(RUSAGE_SELF).ru_maxrss` for peak RSS.
- **Footprint:** recursive `du` of each package dir; `ls -l` of `libonnxruntime.so`; `ldd` of the shipped binary.

Caveats, stated honestly: this is **CPU** inference of a *representative* encoder, not the exact production HuBERT/RVC weights, and the 1.4× is a conservative single-model figure — ORT's fusion advantage compounds across the deeper RVC graph. The point of the inference number isn't "ORT is 1.4× faster"; it's "ORT is *faster* **and** an order of magnitude lighter to ship." The deployment numbers are the headline; the speed is a bonus.

## Appendix B — the war-story changelog

The commits where this got real:

- `9a4d466` — one-time `.pth` → ONNX export; C++ ONNX runtime for voice conversion (the bet)
- `1ea88c3` / `27ed847` — eliminate Python from RVC voice model export
- `3900e72` — C++ HuBERT base → ONNX exporter
- `036e04c` — `pth_reader` falls back to the `model` key for fairseq/HuBERT
- `aa1f73e` — pickle protocol-4 opcodes (`FRAME`, `MEMOIZE`, `STACK_GLOBAL`)
- `257db44` — correct `OBJ` opcode, add `NEWOBJ (0x81)`
- `256986c` — correct fairseq tensor key names in feature projection
- `2f2e270` — GroupNorm only on layer 0 of the HuBERT feature extractor
- **`1088094` — "voice convert actually converts"** (the master war story: protobuf field-2, post-norm HuBERT, weight-norm dims, `emb_pitch`/√hidden scale, VITS sampling, RMVPE)
- `4d50c4c` — FAISS index retrieval with no faiss dependency
- `d564234` — standard RVC ONNX signature; ecosystem interop; sr probing
- `b8ef6f0` — auto-octave transpose via `emb_pitch` fine-tune-diff register detection
