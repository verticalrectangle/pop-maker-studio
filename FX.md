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
| `datamosh_intensity` | 0.6 | Fraction of noise field that shows ghost |
| `datamosh_decay` | 0.08 | How fast ghost tracks source (restoring force) |
| `datamosh_block_size` | 16 | Noise scale in pixels — small = fine grain, large = blobs |
| `datamosh_clip_start` | −1 | Timeline start of active datamosh clip; ghost resets on change |
| `datamosh_bleedback` | 0.0 | Subject reasserts at clip tail (0 = off) |
| `datamosh_clip_duration` | 0.0 | Used to compute bleedback ramp |
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
**Passes**: three steps  
**Fires when**: `need_datamosh`

#### How it works

Datamosh simulates a P-frame-only MPEG decoder: old reference frames corrupt new ones. The key is a **persistent ghost texture** (`g_ghost`) that slowly accumulates source frames over time.

**Step 1 — display pass** (`k_datamosh_frag`):  
A smooth two-octave value noise field decides, per pixel, whether to show ghost or source:

```
n = vnoise(uv * scale + time*0.08) * 0.65
  + vnoise(uv * scale * 2.1 + time*0.13) * 0.35

output = (n < intensity) ? ghost[uv] : source[uv]
```

Ghost is sampled at the **same UV as source** — no displacement. The datamosh effect comes entirely from the ghost containing genuinely old content. Moving subjects leave their old position visible in ghost regions while their new position shows through in clean regions.

`block_size` controls noise scale: small → fine grain, large → big organic blobs.

**Step 2 — ghost update** (`k_datamosh_update_frag`):  
```
new_ghost = mix(ghost, source, decay)
```
`decay` (0.01–0.5) is the restoring force. Low decay = ghost holds old content for many seconds. High decay = ghost quickly converges to source = effect fades. Default 0.08 = ghost half-life of ~8 seconds at 60fps.

**Step 3 — ghost commit**:  
New ghost blitted to `g_ghost.fbo`. The ghost texture is unbound from its sampler unit before this blit to avoid a GL feedback loop (same texture as both FBO attachment and sampler input is undefined behavior).

#### Ghost lifecycle

`ghost_ensure()` manages one global ghost buffer, keyed by `cfx.datamosh_clip_start` (the timeline position of the active datamosh effect clip). When the key changes (different clip active, or first activation) the ghost resets and is seeded with the current source frame. This means:

- First frame after activation: ghost = source → no visible effect yet
- After a few seconds of playback: ghost has genuinely old content → effect is dramatic
- Scrubbing within the same clip: ghost persists, shows whatever it accumulated

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

The current system is a hand-wired chain: each effect is a hardcoded if-block in `fx_apply()`. An API would let effects register themselves and be composed dynamically.

**Effect descriptor struct** (sketch):

```cpp
struct FXDescriptor {
    const char*  name;
    const char*  frag_src;        // GLSL source
    bool       (*is_active)(const CreativeFXAccum&);
    void       (*set_uniforms)(GLuint prog, const CreativeFXAccum&, int w, int h, float t);
    bool         needs_ghost;     // true for stateful effects like datamosh
};
```

**Registration**:
```cpp
fx_register(my_descriptor);
```

**`fx_apply` becomes a loop** over registered descriptors rather than a fixed chain.

**Per-effect ghost buffers** would replace the single `g_ghost`, keyed by effect ID + clip_start. This allows multiple stateful effects simultaneously.

**Shader hot-reload**: since shaders are strings, recompiling on file change during development would be straightforward — store source paths alongside `frag_src`.

---

## Known constraints

- One datamosh ghost buffer globally — two simultaneous datamosh clips on different tracks share state (last writer wins).
- `bg_remove` (background removal) is CPU-only — it reads PNG alpha masks generated offline and cannot move to the GPU until mask generation is GPU-accelerated.
- Slot count is `MAX_VIDEO_TRACKS * 2` (currently 16). Adding tracks beyond 8 requires bumping this constant.
- All shaders target GLSL 3.30 core for compatibility with old integrated GPUs. No `textureSize`, no `gl_FragDepth`, no integer atomics.
