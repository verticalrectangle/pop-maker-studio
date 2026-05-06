# rembg Model Benchmarks

**Test frame:** `bench_frame.jpg` — 540×960  
**Timed runs per model:** 5  
**CPU:** 12 threads  
**Processing pipeline:** 2× supersample → model → Lanczos downsample → σ=0.7 Gaussian


## Results (sorted fastest → slowest)

| Model | Median | Min | Max | vs. u2netp | Notes |
|-------|--------|-----|-----|------------|-------|
| `u2netp` | 0.267s | 0.251s | 0.288s | baseline | Fastest pruned model (current default) |
| `u2net` | 0.487s | 0.408s | 0.496s | 1.8× | Full U²-Net — more accurate, 2× slower |
| `u2net_human_seg` | 0.493s | 0.436s | 0.516s | 1.8× | U²-Net fine-tuned on human portrait data |
| `silueta` | 0.617s | 0.579s | 0.695s | 2.3× | Compact person-specific model |
| `isnet-general-use` | 1.170s | 1.011s | 1.261s | 4.4× | ISNet — newer architecture, strong on clothing edges |
| `birefnet-general-lite` | 8.759s | 8.213s | 10.682s | 32.8× | BiRefNet-lite — SOTA quality at reduced cost |
| `birefnet-general` | 16.296s | 15.674s | 16.544s | 61.0× | BiRefNet general — current SOTA for matting |
| `birefnet-portrait` | 16.763s | 16.215s | 17.770s | 62.8× | BiRefNet fine-tuned for portraits |

## Visual outputs

Preview images (person composited on black) saved alongside this file:

- `u2netp_preview.png` — `u2netp`
- `u2net_preview.png` — `u2net`
- `u2net_human_seg_preview.png` — `u2net_human_seg`
- `silueta_preview.png` — `silueta`
- `isnet_general_use_preview.png` — `isnet-general-use`
- `birefnet_general_lite_preview.png` — `birefnet-general-lite`
- `birefnet_general_preview.png` — `birefnet-general`
- `birefnet_portrait_preview.png` — `birefnet-portrait`

## Quality notes

_(Based on visual inspection of preview images — same frame, same 2× supersample pipeline)_

| Model | Edge quality | Hair detail | Clothing edges | Artefacts |
|-------|-------------|-------------|----------------|-----------|
| `u2netp` | Good | Good | Gaps near body ⚠️ | Occasional missing strips |
| `u2net` | Good | Good | Better than u2netp | Minor fringe |
| `u2net_human_seg` | Good | Good | Best of U²-Net family | Clean shoulder/body boundary |
| `silueta` | Good | Good | Similar to u2net | Slightly soft |
| `isnet-general-use` | Very good | Very good | Good | Slight dark halo on some frames |
| `birefnet-general-lite` | Very good | Very good | Very good | None visible — but 33× slower |
| `birefnet-general` | Excellent | Excellent | Excellent | None — GPU required for real use |
| `birefnet-portrait` | Excellent | Excellent | Excellent | None — GPU required for real use |

## Recommendation

**For CPU preview (current use case):** switch default from `u2netp` → `u2net_human_seg`.  
Same architecture (U²-Net), fine-tuned on human portrait data, only 1.8× slower (0.49s vs 0.27s/frame),  
and noticeably cleaner clothing/shoulder boundaries — directly fixes the gap artifact.

**If you add GPU support later:** `birefnet-general` or `birefnet-portrait` are in a different league quality-wise but need CUDA to be practical.
