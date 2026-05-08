# FX Pipeline

This document describes how the GPU effects system works, what each effect does internally, and how to add new effects. It is the foundation for a future effects API.

---

## Overview

All visual effects run through a single function: `fx_apply()` in `src/fx_shader.cpp`.

```cpp
uintptr_t fx_apply(uintptr_t src_tex, int slot, int w, int h,
                   const EffectAccum& ea, const CreativeFXAccum& cfx, float t);
```

It takes a source texture (the decoded video frame, already CPU-processed for bg_remove), runs the active effect chain via GLSL fragment shaders, and returns a stable output texture that persists until the next call for that slot. The caller draws this texture — no copy needed.

---

## Architecture

### Pass chain

Effects run as a sequence of fullscreen passes over a **ping-pong pair of FBOs** (`g_pp[2]`). Each pass reads from `cur` (a texture ID) and writes to the other ping-pong slot, then swaps. This means any number of passes can run without allocating new textures.

```
src_tex → [grade+vignette] → [blur H] → [blur V] → [chroma-key] → [glitch]
        → [vhs] → [light-leak] → [datamosh] → g_out[slot]
```

Inactive effects are skipped entirely — if only blur is on, only that pass runs.

### Stable output textures

The ping-pong buffers are scratch space. After the chain, the final result is blitted to `g_out[slot]` — a **per-slot stable texture** that persists until the next `fx_apply` call for that slot. This matters because ImGui's `AddImageQuad` is recorded into a draw list and rendered later; the texture must still be valid at flush time.

`slot` maps to the video system's slot index (0–15). `MAX_VIDEO_TRACKS * 2` slots are pre-allocated.

### Vertex shader

All passes use a single vertex shader that generates a fullscreen triangle from `gl_VertexID` — no VBO, no attributes. Core profile requires a VAO to be bound; `g_vao` is an empty one created at init.

### GL state

`fx_apply` saves `GL_FRAMEBUFFER_BINDING` and `GL_VIEWPORT` before doing anything, and restores them on exit. The save must happen before `pp_ensure` / `out_ensure`, which themselves bind FBOs.

---

## Effect accumulators

Effects come from two structs, populated by scanning the timeline around the current playhead:

### `EffectAccum` — adjustment-layer effects

Collected by `collect_effects(state, t, below_track_idx)`.

| Field | Default | Description |
|---|---|---|
| `brightness` | 0.0 | Additive brightness, −1 to +1 |
| `contrast` | 1.0 | Multiplicative contrast |
| `saturation` | 1.0 | 0 = greyscale, 1 = unchanged, >1 oversaturated |
| `hue` | 0.0 | Hue rotation in degrees |
| `blur` | 0.0 | Gaussian sigma in pixels |
| `vignette` | 0.0 | Vignette strength 0–1 |
| `any_color` | false | True if any color param differs from neutral |
| `any_blur` | false | True if blur > 0.1 |
| `any_vignette` | false | True if vignette > 0.001 |

### `CreativeFXAccum` — creative effects

Collected by `collect_creative_fx(state, t, below_track_idx)`.

| Field | Default | Description |
|---|---|---|
| `chroma_key_on` | false | |
| `chroma_key_r/g/b` | 0/1/0 | Key color (green by default) |
| `chroma_key_threshold` | 0.30 | Match radius in RGB space |
| `chroma_key_softness` | 0.15 | Edge feather width |
| `glitch_on` | false | |
| `glitch_chroma` | 0.0 | RGB channel horizontal split pixels |
| `glitch_jitter` | 0.0 | Row vertical jitter strength |
| `glitch_corruption` | 0.0 | Block corruption amount |
| `glitch_corruption_bleed` | 0.0 | Corruption horizontal bleed |
| `datamosh_on` | false | |
| `datamosh_intensity` | 0.6 | JPEG scan corruption density (0 = off, 1 = heavy) |
| `datamosh_spread` | 0.3 | GPU chroma-bleed width on saturated pixels (0 = off, 1 = wide) |
| `zoom_on` | false | |
| `zoom_strength` | 0.0 | Beat-synced scale spike magnitude |
| `zoom_decay` | 0.15 | Exponential decay rate of punch |
| `zoom_shake` | 0.0 | Random positional shake on each punch |
| `leak_on` | false | |
| `leak_intensity` | 0.0 | Brightness of light flare overlay |
| `leak_speed` | 1.0 | Animation rate multiplier |
| `vhs_on` | false | |
| `vhs_noise` | 0.0 | Luma noise strength |
| `vhs_bleed` | 0.0 | Chroma horizontal bleed in pixels |
| `vhs_tracking` | 0.0 | Tracking error wobble |

---

## Effects reference

### Grade + Vignette
**Pass**: single pass, `k_grade_frag`  
**Fires when**: `need_grade || need_vig`

Applies brightness/contrast/saturation/hue and a radial vignette in one pass.

Hue rotation uses a 3×3 Rodrigues matrix computed in the shader — matches the CPU `cpu_apply_grade` path for export/preview parity.

Vignette is a smooth radial falloff from center: `strength * smoothstep(0.3, 0.8, dist)`.

---

### Blur
**Passes**: two passes (horizontal then vertical), `k_blur_frag`  
**Fires when**: `need_blur` (blur > 0.1)

Separable Gaussian implemented as a 1D box blur. `u_dir` is `(1/w, 0)` for H and `(0, 1/h)` for V. `u_sigma` is the pixel radius.

---

### Chroma Key
**Pass**: single pass, `k_chroma_key_frag`  
**Fires when**: `need_chroma`

Computes the Euclidean distance between the pixel's RGB and the key color. Pixels within `threshold` are keyed out (alpha → 0). `softness` controls the edge ramp via `smoothstep`.

---

### Glitch
**Pass**: single pass, `k_glitch_frag`  
**Fires when**: `need_glitch` (chroma ≥ 0.1 or jitter ≥ 0.01)

Two sub-effects in one shader:
- **Chroma split**: R and B channels are sampled at `v_uv ± (u_chroma, 0)`.
- **Row jitter**: rows are grouped into bands; each band gets a random horizontal offset driven by `hash(y_id + time)`. Uses `u_tex_h` as a uniform instead of `textureSize()` — old Mesa/iGPU drivers crash on `textureSize`.

---

### VHS
**Pass**: single pass, `k_vhs_frag`  
**Fires when**: `need_vhs`

Three sub-effects:
- **Luma noise**: `hash(uv + time)` added to luminance.
- **Chroma bleed**: U/V channels smeared right by `u_bleed` pixels.
- **Tracking wobble**: horizontal offset per row driven by `sin(y * freq + time) * u_tracking`.

---

### Light Leak
**Pass**: single pass, `k_leak_frag`  
**Fires when**: `need_leak`

Two procedural light blobs (warm orange + cool magenta) at fixed UV positions, animated by `u_time * u_speed`. Blob intensity falls off with `pow(distance, 3)`. Additive blend over source.

---

### Datamosh
**Implementation**: CPU JPEG scan-data corruption (preview + snapshot) + GPU chroma-bleed pass (GPU pipeline)  
**Fires when**: `datamosh_on && datamosh_intensity > 0.01f`

#### How it works

Datamosh corrupts the actual JPEG-encoded bytes before the decoder sees them. A real JPEG decoder (stb_image) encounters corrupted Huffman entropy data and loses sync mid-scan, producing extreme DCT coefficient values. Because JPEG stores image data in YCbCr color space, corrupted luma (Y) and chroma (Cb/Cr) produce the vivid neon green, pink, and cyan block patterns characteristic of real datamoshed video.

#### CPU corruption — `corrupt_jpeg_buf(buf, sz, intensity, seed)` in `video.cpp`

1. Find the SOS (Start Of Scan) marker `0xFF 0xDA` in the JPEG bitstream.
2. Skip the SOS header (2-byte length + component table).
3. Scatter `(intensity * 20) + 1` random byte replacements into the scan data using an LCG seeded by `seed`.
4. Positions use a u² bias (`t = u*u`) to concentrate hits toward the start of the scan, so Huffman decode errors cascade downward from the top — even vertical distribution of artifacts.

`seed = (uint32_t)(time * 60.f)` — animates on playback, deterministic for the same time position. **No ghost buffer, no GPU state, no feedback loop.** Every frame is independent.

#### Preview path (`decode_proxy_frame` in `video.cpp`)

The proxy is a low-resolution MJPEG file generated by FFmpeg. FFmpeg's MJPEG encoder inserts restart markers (RST0–RST7) approximately every MCU row. Corruption is bounded to the current restart interval, producing distinct horizontal bands of artifact. The raw MJPEG bytes in `s_buf` are corrupted in place before `stbi_load_from_memory`.

#### Snapshot / export path (`video_apply_datamosh` in `video.cpp`)

`stbi_write_jpg` produces no restart markers. Corrupting a full-resolution JPEG (e.g. 1080×1920 ≈ 2 MB scan) with u²-biased hits near byte 0 would cascade through the entire image → solid black frame.

Fix: `video_apply_datamosh` works at proxy scale (~540 px wide):
1. Nearest-neighbour downscale RGBA → small RGB.
2. Encode to JPEG with `stbi_write_jpg_to_func`.
3. Corrupt with `corrupt_jpeg_buf`.
4. Decode back with `stbi_load_from_memory` (if NULL, skip — image unchanged).
5. Nearest-neighbour upscale corrupted result back to original dimensions.

Working at small scale gives the same JPEG scan density that corruption was tuned for. The nearest-neighbour upscale makes the block artifacts larger, which reads well at full resolution.

Called from `gl_render_vid_clip` in `render.cpp` after `video_decode_frame_at`, before GL texture upload. This is the same function used by both the GL snapshot and the GL-based export path.

#### GPU spread pass (`k_datamosh_frag` in `fx_shader.cpp`)

After the CPU corruption is uploaded to GL, a single GPU pass adds horizontal chroma bleed:

```glsl
float lo    = min(col.r, min(col.g, col.b));
float hi    = max(col.r, max(col.g, col.b));
float matte = smoothstep(0.25, 0.75, hi - lo);   // high saturation = artifact pixel
float bleed = matte * u_spread * 40.0 / u_tex_w;
float r     = texture(u_tex, v_uv + vec2( bleed,       0.0)).r;
float b     = texture(u_tex, v_uv - vec2( bleed * 0.6, 0.0)).b;
frag = vec4(r, col.g, b, 1.0);
```

Channel dominance (`hi - lo`) serves as a matte: neon/saturated pixels (JPEG artifacts) bleed their red and blue channels horizontally. Neutral skin tones and grays have near-zero matte and are unaffected. This selectively smears only the corrupted regions, producing a pseudo chroma-key layered bleed.

**Fires when**: `cfx.datamosh_on && cfx.datamosh_spread > 0.01f`

---

## Adding a new effect

### 1. Add parameters to the accumulator

For a creative effect, add fields to `CreativeFXAccum` in `src/app.h`:

```cpp
bool  my_fx_on        = false;
float my_fx_strength  = 0.5f;
```

For an adjustment-style effect (stacks with grade/blur), extend `EffectAccum`.

### 2. Collect it from the timeline

In `collect_creative_fx()` in `src/app.cpp`, add a case to the switch:

```cpp
case FXType::MyFX:
    acc.my_fx_on       = true;
    acc.my_fx_strength = fmaxf(acc.my_fx_strength, cl.fx_my_strength);
    break;
```

Use `fmaxf` to accumulate when multiple clips overlap (strongest wins). Set fixed fields like `clip_start` by assignment.

### 3. Add a clip property

In the `Clip` struct (`src/app.h`), add the stored parameter:

```cpp
float fx_my_strength = 0.5f;
```

Also add to `FXType` enum and to serialization in `src/project.cpp`.

### 4. Write the fragment shader

Add a `static const char* k_my_fx_frag` string in `src/fx_shader.cpp`. All shaders share the same vertex shader — inputs are just `in vec2 v_uv` and the standard uniforms. Use `u_tex` for single-texture passes. Multi-texture passes bind additional samplers manually.

Avoid `textureSize()` — use explicit `uniform float u_tex_w / u_tex_h` instead.

### 5. Register the program

In `g_prog`, add a `GLuint my_fx = 0` field. In `fx_shader_init()`:

```cpp
g_prog.my_fx = link_prog(k_my_fx_frag);
```

Also add to the null-check array and shutdown loop.

### 6. Add the pass to `fx_apply()`

After the existing passes, add:

```cpp
bool need_my_fx = cfx.my_fx_on && cfx.my_fx_strength > 0.01f;

// ... in the pass chain:
if (need_my_fx) {
    GLuint p = g_prog.my_fx;
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, "u_strength"), cfx.my_fx_strength);
    run1(p);
}
```

`run1(p)` is the helper lambda that draws a fullscreen pass, advances `cur`, and flips `pslot`.

### 7. Add UI in screen_studio

Add sliders in the effect clip inspector panel in `src/ui/screen_studio.cpp`, inside the block for `FXType::MyFX`.

---

## Future API direction

### Registered effect descriptors

The current system is a hand-wired chain: each effect is a hardcoded if-block in `fx_apply()`. An API would let effects register themselves and be composed dynamically.

**Effect descriptor struct** (sketch):

```cpp
struct FXDescriptor {
    const char*  name;
    const char*  frag_src;        // GLSL source
    bool       (*is_active)(const CreativeFXAccum&);
    void       (*set_uniforms)(GLuint prog, const CreativeFXAccum&, int w, int h, float t);
    bool         needs_cpu_pre;   // true if a cpu_apply fn must run before GL upload
    void       (*cpu_apply)(VideoFrame*, const CreativeFXAccum&, float t); // optional
};
```

**Registration**:
```cpp
fx_register(my_descriptor);
```

**`fx_apply` becomes a loop** over registered descriptors rather than a fixed chain.

**Shader hot-reload**: since shaders are strings, recompiling on file change during development would be straightforward — store source paths alongside `frag_src`.

---

### MCP effects authoring

This document is the spec for an MCP-powered effect authoring workflow. An LLM connected to the codebase via MCP can implement a complete new effect end-to-end, given only a plain-language description, because each effect is a small, well-scoped set of changes across a fixed set of files:

| File | What changes |
|---|---|
| `src/app.h` | `Clip` fields (`fx_*`), `CreativeFXAccum` fields, `FXType` enum entry |
| `src/app.cpp` | `collect_creative_fx` case |
| `src/fx_shader.cpp` | GLSL fragment shader string, program slot, `fx_apply` pass |
| `src/project.cpp` | Serialization read/write (version bump) |
| `src/ui/screen_studio.cpp` | Inspector sliders for the effect clip |
| `src/video.h` + `video.cpp` | Only for CPU effects (like datamosh) that modify pixel data before GL upload |

**What an LLM needs to produce a new GPU effect:**
1. A GLSL fragment shader that reads `u_tex` and outputs `frag`. Uniforms: `u_time`, `u_tex_w`, `u_tex_h`, plus any effect-specific floats.
2. Parameter names and ranges (e.g. `strength 0–1`, `speed 0–5`).
3. Accumulation strategy for overlapping clips (`fmaxf` for most, `+=` for additive like brightness).

**What an LLM needs to produce a CPU effect:**
1. A C++ function `void cpu_apply(VideoFrame*, float param, float t)` operating on RGBA pixel data.
2. Whether it replaces or modifies pixels in-place.
3. Any resolution considerations (see datamosh downscale approach for effects sensitive to JPEG scale).

**Prompt template for requesting a new effect:**

```
Add a new effect to Pop Maker Studio called "<name>".
Visual description: <what it should look like>.
Parameters: <list name, type, range, default>.
Implementation type: GPU shader / CPU pixel / hybrid.
```

The LLM reads FX.md for the exact change list and wiring pattern, reads the current state of the 6 files above, writes all changes, builds, and reports any errors. No manual wiring required beyond the changes listed in "Adding a new effect."

**Current gap before this is fully automatic**: the `FXType` enum and `project.cpp` version bump are the most fragile steps — an LLM must read the current version number and enum values carefully. These are good candidates to automate via a code-generation script that keeps them in sync with a JSON effect registry.

---

## Known constraints

- Datamosh is a hybrid effect: CPU JPEG corruption (preview + snapshot/export) + GPU chroma-bleed spread pass. The CPU stage is stateless — no ghost buffer, no cross-track shared state. The GPU stage runs in the normal pass chain.
- Datamosh corruption density is calibrated for the proxy MJPEG scale (~540 px wide). The snapshot path explicitly downscales before corrupting and upscales after to match the same density.
- `bg_remove` (background removal) is CPU-only — it reads PNG alpha masks generated offline and cannot move to the GPU until mask generation is GPU-accelerated.
- Slot count is `MAX_VIDEO_TRACKS * 2` (currently 16). Adding tracks beyond 8 requires bumping this constant.
- All shaders target GLSL 3.30 core for compatibility with old integrated GPUs. No `textureSize`, no `gl_FragDepth`, no integer atomics.
