# Script clips — API contract

A **Script clip** (`ClipType::Script`) is a timeline clip whose pixels are produced every
frame by a JavaScript module. It is how agents author anything the fixed clip types cannot
express: data-driven motion graphics, audio-reactive HUDs, particle systems, per-glyph
typography, multi-stage "processing" sequences. Preview and export run the same code
through the same GL compositor.

This document is the contract between the runtime (`src/script_*`), the typography library
(`assets/scripts/typography/`), MCP tools, and scene authors. Change it deliberately.

## 1. Model

- Runtime: QuickJS-ng (vendored, `vendor/quickjs-ng/`). One isolated `JSRuntime` +
  `JSContext` per Script clip. ES modules; `import` resolves relative to the importing file,
  plus the built-in module specifiers listed in §6.
- Drawing: a Canvas-2D-shaped context implemented on NanoVG (GL3 backend,
  `vendor/nanovg/`), rendering into the clip's own RGBA framebuffer the size of the
  project canvas (`f.width × f.height`).
- The clip framebuffer is then (optionally) run through the clip's post shader (§5) and
  composited like any other layer (clip transform, opacity, track order).
- **Purity:** `render(f)` must be a pure function of `f` (and loaded data). The host may call
  it in any order (scrubbing), skip frames, or call it twice for the same frame.
  `Math.random` and `Date.now` throw; use `pms.hash`.
- **Hot reload:** the host watches every loaded module file (and `pms.json` files); any
  change rebuilds the runtime. Errors never crash the app: the clip renders a red error
  card in preview, export fails with the error, and `get_script_errors` reports them.

## 2. Module shape

```js
// scene.js
export function setup(env) {}      // optional; once per runtime build. env = {width, height, fps, params}
export function render(f) {}      // required; once per frame the clip is visible
```

`f` (frozen object):

| field | meaning |
|---|---|
| `t` | timeline time in seconds (same clock as `pms.audio` times) |
| `local` | seconds since the clip's start |
| `frame` | integer timeline frame index (`round(t * fps)`) |
| `fps` | project frame rate |
| `width`, `height` | canvas size in px for this render (changes with project format / multi-format export) |
| `duration` | clip duration in seconds |
| `exporting` | `true` during export, `false` in preview |

## 3. `pms` global

| member | type | notes |
|---|---|---|
| `pms.canvas` | `Context2D` | §4. State is reset (identity transform, alpha 1, source-over, no scissor) and the framebuffer cleared to transparent before each `render`. |
| `pms.params` | object | the clip's `script_params` JSON (`{}` if empty) |
| `pms.audio` | `Analysis \| null` | audio analysis v2 of the project's master audio, times converted to **timeline seconds** (docs/AUDIO_ANALYSIS.md schema). `null` until analysis exists. |
| `pms.words` | `Word[]` | `{w, t0, t1, line, i, conf}` in timeline seconds (from `pms.audio.words`, empty if none) |
| `pms.lines` | `string[]` | lyric lines |
| `pms.json(path)` | any | parse a JSON file (relative to the script file); cached; watched for reload |
| `pms.image(path)` | `Image` | `{width, height}`; decoded synchronously on first call (stb_image, RGBA, alpha kept), cached per runtime; relative paths resolve against the script file |
| `pms.face(path)` | `FaceTrack \| null` | per-frame face data for a media file (§3.1). `null` while tracking; the host starts tracking on first request; export blocks until every requested track is ready. |
| `pms.font(family, path)` | void | register a TTF/OTF under a family name (idempotent). Built-in families: `"Inter"`, `"Mono"` |
| `pms.post(frag, uniforms)` | void | set this frame's post shader (§5). Call inside `render`; if not called, no post pass. |
| `pms.hash(a, b?, c?)` | number in [0,1) | deterministic integer hash, bit-identical to the reference in §7 |
| `pms.log(...args)` | void | to the script log (`get_script_errors` returns the tail) |

### 3.1 `FaceTrack`

```ts
{
  width: number, height: number,          // source frame size in px (display orientation)
  times: number[],                        // source seconds per tracked frame (real, VFR-safe)
  landmarks: [number, number][][],        // per frame: 478 [x, y] normalised 0..1 (MediaPipe topology)
  eyeOpen: number[],                      // per frame: eyelid-gap ratio (src/face_metrics.h)
  blink: number[],                        // per frame: max(blendshape blink, 1 - eyeOpen/openBaseline), 0..1
  mesh: { tesselation: [number,number][], contours: [number,number][], irises: [number,number][] }
}
```

## 4. `Context2D`

Semantics follow the HTML Canvas 2D spec for everything listed; anything not listed does not
exist (accessing it throws `TypeError`, so ports fail loudly).

- **State:** `save()`, `restore()`.
- **Transform:** `translate(x,y)`, `scale(x,y)`, `rotate(rad)`, `setTransform(a,b,c,d,e,f)`,
  `resetTransform()`.
- **Compositing:** `globalAlpha`; `globalCompositeOperation` ∈ `'source-over'`, `'lighter'`
  (additive), `'multiply'`, `'screen'`, `'destination-out'`.
- **Styles:** `fillStyle`, `strokeStyle` accept CSS colours (`#rgb`, `#rgba`, `#rrggbb`,
  `#rrggbbaa`, `rgb()`, `rgba()`, CSS named colours) or a `CanvasGradient`.
  `createLinearGradient(x0,y0,x1,y1)`, `createRadialGradient(x0,y0,r0,x1,y1,r1)` with
  **any number** of `addColorStop(offset, colour)` stops (host rasterises multi-stop
  gradients to a lookup texture).
- **Lines:** `lineWidth`, `lineCap` (`butt|round|square`), `lineJoin` (`miter|round|bevel`),
  `miterLimit`.
- **Paths:** `beginPath()`, `closePath()`, `moveTo`, `lineTo`, `quadraticCurveTo`,
  `bezierCurveTo`, `arc(x,y,r,a0,a1,ccw?)`, `ellipse(x,y,rx,ry,rot,a0,a1,ccw?)`,
  `rect(x,y,w,h)`, `fill(rule?)` with `rule` ∈ `'nonzero'|'evenodd'`, `stroke()`.
  Paths with tens of thousands of segments must stay interactive.
- **Clipping:** `clip(rule?)` supports any path (stencil-based, intersects with the current
  clip, restored by `restore()`), including `'evenodd'` for "everything except this box".
- **Rects:** `fillRect`, `strokeRect`, `clearRect`.
- **Images:** `drawImage(img, dx, dy)`, `drawImage(img, dx, dy, dw, dh)`,
  `drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh)`; `imageSmoothingEnabled` (false = nearest
  sampling, pixel-exact blocks); `imageSmoothingQuality` accepted and ignored.
- **Text:** `font` (CSS shorthand subset: `"[weight] <size>px <family>"`), `textAlign`
  (`left|right|center|start|end`), `textBaseline` (`alphabetic|top|middle|bottom`),
  `letterSpacing` (`"<n>px"` or `"<n>em"`), `fillText(s,x,y)`, `strokeText(s,x,y)`,
  `measureText(s)` → `{width, actualBoundingBoxAscent, actualBoundingBoxDescent}`
  (measured from the font's real advances/kerning).

## 5. Post shader

`pms.post(frag, uniforms)` — `frag` is a GLSL 330 core fragment shader body:

```glsl
// provided by the host (do not redeclare):
// uniform sampler2D u_tex;   // this clip's framebuffer, premultiplied RGBA
// uniform vec2 u_res;        // framebuffer size in px
// in vec2 v_uv;              // 0..1, origin top-left (y down, matching the canvas)
// out vec4 fragColor;
uniform float u_amount;        // author-declared uniforms
void main() { fragColor = texture(u_tex, v_uv); }
```

`uniforms` maps names to `number` or arrays of length 2–4 (vec2–vec4). Programs are compiled
once per distinct `frag` string and cached; compile errors surface like script errors.

## 6. Built-in modules

- `pms:rhythm` — audio helpers over `pms.audio`, with a visual lead (`LEAD = 0.012` s):
  `lastIndex(arr, t)`, `pulse(kind, t, decay, minStrength)`, `lastHit(kind, t, minStrength)`,
  `eventPulse(times, t, decay)`, `envAt(values, t)`, `beatInfo(t)`,
  `progress(weights, t0, t1, opts)` (music-driven progress: cumulative hit energy + trickle,
  normalised to reach 1 at `t1`), `hash`, `clamp01`, `easeInOutCubic`.
- `pms:text` — layout on measured advances: `layout(ctx, words, {width, lineHeight, space})`
  → per-word boxes; `fit(ctx, words, box, {max, min})` → largest font size that fits without
  splitting words; glyph iteration for per-glyph transforms (flip, offset, scramble).
- `pms:typography/<preset>` — the ported typography presets (see `assets/scripts/typography/`).

## 7. Reference hash

```js
function hash(a, b = 0, c = 0) {
  let h = Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul(b | 0, 0x165667b1) ^ Math.imul(c | 0, 0x9e3779b1);
  h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}
```

## 8. Clip fields, IPC and MCP

- `Clip::script_path` (entry module; absolute, or relative to the project file),
  `Clip::script_params` (JSON string). Standard transform/opacity/fade fields apply.
- IPC / MCP: `add_script_clip {track, start, duration, path, params?}`,
  `set_script_clip {clip, path?, params?}`, `get_script_errors {clip?}`,
  `render_still {t, path, format?}` (renders the full composite at time `t`).

## 9. Performance budget

Scene scripts of the size of the reference project (≈2k blocks + ≈1k particle strokes +
a HUD of ≈3k segments) must render in ≤ 8 ms per frame at 1080×1920 on the preview path.
The host caches the last rendered frame per clip and skips `render` when `t`, canvas size,
params, and module/data versions are unchanged.
