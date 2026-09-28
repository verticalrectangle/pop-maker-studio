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
- Drawing: an HTML Canvas 2D context implemented on Skia (Ganesh GL, prebuilt static
  Skia fetched by `cmake/Skia.cmake`; text shaped with HarfBuzz), sharing the app's GL
  context (`src/script_context2d.*`, `src/script_gpu.*`). Scripts always draw in project
  canvas pixels (`f.width × f.height`: 1080×1920, 1920×1080 or 1080×1080); the preview
  rasterises the same drawing at its own, smaller size, export and `render_still` at the
  full size.
- The clip framebuffer is then (optionally) run through the clip's post shader (§5),
  converted to straight alpha, and composited like any other layer (clip transform,
  opacity, track order).
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
| `t` | timeline time in seconds on the project frame grid (`frame / fps`, exact in double precision — same clock as `pms.audio` times) |
| `local` | `t` minus the clip's start |
| `frame` | integer timeline frame index |
| `fps` | project frame rate |
| `width`, `height` | canvas size in px for this render (changes with project format / multi-format export) |
| `duration` | clip duration in seconds |
| `exporting` | `true` during export, `false` in preview |

## 3. `pms` global

| member | type | notes |
|---|---|---|
| `pms.canvas` | `Context2D` | §4. Every drawing-state attribute is reset to its spec default (identity transform, `10px sans-serif`, black styles, no clip) and the framebuffer cleared to transparent before each `render`. Drawing outside `render` throws; `measureText` works in `setup` too. |
| `pms.params` | object | the clip's `script_params` JSON (`{}` if empty) |
| `pms.audio` | `Analysis \| null` | audio analysis v2 of the project's master audio, times converted to **timeline seconds** (docs/AUDIO_ANALYSIS.md schema). `null` until analysis exists. |
| `pms.words` | `Word[]` | `{w, t0, t1, line, i, conf}` in timeline seconds (from `pms.audio.words`, empty if none) |
| `pms.lines` | `string[]` | lyric lines |
| `pms.json(path)` | any | parse a JSON file (relative to the script file); cached; watched for reload |
| `pms.image(path)` | `Image` | `{width, height}`; decoded synchronously on first call (stb_image, RGBA, alpha kept), cached per runtime; relative paths resolve against the script file. Call it at module top level for every image a scene uses: decoding then happens once at load instead of stalling the first frame that needs it while scrubbing |
| `pms.face(path)` | `FaceTrack \| null` | per-frame face data for a media file (§3.1). `null` while tracking; the host starts tracking on first request; export blocks until every requested track is ready. |
| `pms.font(family, path)` | void | add a TTF/OTF face to a family (idempotent; several files under one family are matched by weight/style like CSS; a bold request on a regular-only family is synthesised). Built-in families: `"Inter"` (400/700/900; also `sans-serif`, `system-ui`), `"Mono"` (JetBrains Mono 400; also `monospace`) |
| `pms.post(frag, uniforms)` | void | set this frame's post shader (§5). Call inside `render`; if not called, no post pass. |
| `pms.hash(a, b?, c?)` | number in [0,1) | deterministic integer hash, bit-identical to the reference in §7 |
| `pms.log(...args)` | void | to the script log (`get_script_errors` returns the last 200 lines); `console.log/warn/error` alias it |

### 3.1 `FaceTrack`

```ts
{
  width: number, height: number,          // source frame size in px (display orientation)
  fps: number,                            // container frame rate (nominal; use times for VFR)
  times: number[],                        // source seconds per tracked frame (real pts, VFR-safe)
  landmarks: [number, number][][],        // per frame: 478 [x, y] normalised 0..1 in display
                                          // orientation (all zeros on frames without a face)
  eyeOpen: number[],                      // per frame: eyelid-gap ratio (src/face_metrics.h), unsmoothed
  blink: number[],                        // per frame: unified blink 0..1 — max of the calibrated
                                          // geometric term (eyeOpen vs a streaming open-eye baseline)
                                          // and the blendshape term (face_metrics.h face_blink_signal)
  mesh: {                                 // MediaPipe face-mesh connection sets, [a, b] index pairs
    tesselation: [number,number][],       // 2556 edges
    contours: [number,number][],          // lips, eyes, brows, face oval (124 edges)
    irises: [number,number][]             // both irises (8 edges)
  }
}
```

## 4. `Context2D`

Semantics follow the HTML Canvas 2D spec for everything listed (Chrome/Blink is the
tie-breaker); anything not listed does not exist — reading or assigning it throws
`TypeError`, so ports fail loudly. Where the spec silently ignores an invalid value
(unknown colour, font, composite operation, enum), the host throws instead; non-finite
numeric arguments are ignored as the spec says.

- **State:** `save()`, `restore()`; `canvas` → `{width, height}` (logical px).
- **Transform:** `translate(x,y)`, `scale(x,y)`, `rotate(rad)`, `transform(a,b,c,d,e,f)`,
  `setTransform(a,b,c,d,e,f)` / `setTransform({a..f})`, `resetTransform()`,
  `getTransform()` → `{a,b,c,d,e,f}`. Path points are transformed when added (spec).
- **Compositing:** `globalAlpha`; `globalCompositeOperation` — every spec value:
  `source-over|source-in|source-out|source-atop|destination-over|destination-in|
  destination-out|destination-atop|lighter|copy|xor|multiply|screen|overlay|darken|
  lighten|color-dodge|color-burn|hard-light|soft-light|difference|exclusion|hue|
  saturation|color|luminosity` (the unbounded ones affect the whole clip, as specified).
- **Styles:** `fillStyle`, `strokeStyle` accept CSS colours (`#rgb`, `#rgba`, `#rrggbb`,
  `#rrggbbaa`, `rgb()/rgba()/hsl()/hsla()` in comma or space syntax, the CSS named colours,
  `transparent`) or a `CanvasGradient`: `createLinearGradient(x0,y0,x1,y1)`,
  `createRadialGradient(x0,y0,r0,x1,y1,r1)`, `createConicGradient(angle,x,y)`, any number
  of `addColorStop(offset, colour)` (interpolated unpremultiplied, per spec).
- **Lines:** `lineWidth`, `lineCap` (`butt|round|square`), `lineJoin` (`miter|round|bevel`),
  `miterLimit`, `setLineDash(segments)`, `getLineDash()`, `lineDashOffset`.
- **Paths:** `beginPath()`, `closePath()`, `moveTo`, `lineTo`, `quadraticCurveTo`,
  `bezierCurveTo`, `arcTo(x1,y1,x2,y2,r)`, `arc(x,y,r,a0,a1,ccw?)`,
  `ellipse(x,y,rx,ry,rot,a0,a1,ccw?)`, `rect(x,y,w,h)`, `roundRect(x,y,w,h,radii)`,
  `fill(rule?)` with `rule` ∈ `'nonzero'|'evenodd'`, `stroke()`, `isPointInPath(x,y,rule?)`
  (device coordinates). Paths with tens of thousands of segments stay interactive.
- **Clipping:** `clip(rule?)` — any path, anti-aliased, intersects with the current clip,
  restored by `restore()`; `'evenodd'` gives "everything except this box".
- **Rects:** `fillRect`, `strokeRect`, `clearRect`.
- **Images:** `drawImage(img, dx, dy)`, `drawImage(img, dx, dy, dw, dh)`,
  `drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh)` with `img` from `pms.image()`;
  `imageSmoothingEnabled` (false = nearest, pixel-exact blocks); `imageSmoothingQuality`
  `low` (bilinear, default) | `medium` (bilinear + nearest mip) | `high` (trilinear).
- **Shadows / filter:** `shadowColor`, `shadowBlur`, `shadowOffsetX/Y` (device px, not
  transformed, as specified); `filter` ∈ `'none' | 'blur(<n>px)'`.
- **Text:** `font` (CSS shorthand: `[style] [variant] [weight] [stretch] <size>[/lh] <family>[, …]`,
  size in px/pt/em/%; the first registered family wins, an unregistered list throws),
  `textAlign` (`start|end|left|right|center`), `textBaseline`
  (`alphabetic|top|hanging|middle|ideographic|bottom`), `letterSpacing` (`"<n>px"`,
  `"<n>em"` resolved against the current font, `"<n>pt"`), `fillText(s,x,y,maxWidth?)`,
  `strokeText(s,x,y,maxWidth?)`, `measureText(s)` → `{width, actualBoundingBoxLeft/Right/
  Ascent/Descent, fontBoundingBoxAscent/Descent, emHeightAscent/Descent,
  alphabeticBaseline, hangingBaseline, ideographicBaseline}`. Text is shaped with
  HarfBuzz (kerning, ligatures), positioned with subpixel precision and linear metrics;
  `letterSpacing` is added after every cluster including the last (Blink).

## 5. Post shader

`pms.post(frag, uniforms)` — `frag` is a GLSL 330 core fragment shader body:

```glsl
// provided by the host (do not redeclare):
// uniform sampler2D u_tex;   // this clip's framebuffer, premultiplied RGBA, row 0 = top
// uniform vec2 u_res;        // logical canvas size in px (f.width, f.height)
// in vec2 v_uv;              // 0..1, origin top-left (y down, matching the canvas)
// out vec4 fragColor;        // premultiplied RGBA (the host converts to straight alpha)
uniform float u_amount;        // author-declared uniforms
void main() { fragColor = texture(u_tex, v_uv); }
```

`uniforms` maps names to `number` or arrays of length 2–4 (vec2–vec4). Programs are compiled
once per distinct `frag` string and cached; compile errors surface like script errors
(line numbers refer to `frag`). Sample through `v_uv`/`u_res` rather than `gl_FragCoord`:
the preview framebuffer is smaller than `u_res`.

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
  `Clip::script_params` (JSON string). Standard transform/opacity/fade fields apply; the
  layer covers the canvas at `pos 0.5/0.5, scale 1`.
- IPC / MCP: `add_script_clip {track, start, duration, path, params?}`,
  `set_script_clip {track, clip, path?, params?}`,
  `get_script_errors {track?, clip?}` → `{clips: [{clip: "track:clip", errors: [{message,
  file, line}], log, render_ms, flush_ms, builds}]}` (`builds` counts runtime (re)builds:
  1 + hot reloads/edits), `render_still {t, path, format?}` (the full
  composite at time `t`, rendered like export: face tracks are waited for),
  `set_fps {fps}` (the frame grid `f.frame` counts on).
- Export treats a script error as fatal: the render stops with
  `Error — script clip <track:clip>: <message>` as its stage.

## 9. Performance budget

Scene scripts of the size of the reference project (≈2k blocks + ≈1k particle strokes +
a HUD of ≈3k segments) must stay within ≈ 8 ms of main-thread CPU per frame (JS + Skia
recording + flush submit, both clips) on the preview path; `get_perf_stats` reports it as
the `script` stage and `get_script_errors` per clip. The host skips `render` when `t`,
canvas/surface size, params, audio analysis and module/data files are unchanged, and
re-renders while a requested face track is still building.
