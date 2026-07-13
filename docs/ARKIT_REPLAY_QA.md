# ARKit Makeup QA — fixture capture, replay, and gates

How to test the native tier-1 makeup path (docs in pms-ios:
`docs/ARKIT_NATIVE_PLAN.md`) without touching the phone for every
iteration. Written for agents; everything here runs from the Linux box via
`ssh macbookpro.local` (no password) except the capture itself.

## The loop in one paragraph

The app records real face geometry (triple-tap), you pull the `.jsonl` off
the phone once, and from then on `arkit-native-replay` re-renders that
exact face — every frame, any look, any intensity — through the real
engine on the Mac and writes PNGs you can look at. Placement bugs, blend
bugs, blink/gaze behavior: verify offline against the wearer's true
geometry, fix, re-render, and only deploy when the PNGs are right. Never
tune makeup art against the canonical head if a fixture exists — the
canonical head's proportions lie (brows, eye shape, jitter are all
per-person).

## 1. Capture a fixture (user action, on device)

Triple-tap the camera preview in RecordView. This toggles the engine
landmark overlay AND records ~6 s (180 frames) of geometry to the app's
Documents: `arkit_capture_<unix-ts>.jsonl`. Have the user perform the
motion that breaks things (blink, look around, talk, tilt).

Each JSONL line is one frame:

```json
{"t": <ARFrame timestamp s>, "w": 1080, "h": 1440,
 "verts":  [1220*3 floats, anchor space, meters],
 "model":  [16], "view": [16], "proj": [16],   // column-major simd
 "eye_l":  [16], "eye_r": [16],                // eyeball transforms
 "blend":  [52]}                               // MediaPipe order, _neutral=0
```

## 2. Pull it (Mac, phone on USB/Wi-Fi)

```sh
ssh macbookpro.local 'xcrun devicectl device info files \
  --device 00008140-0008641C1E90801C \
  --domain-type appDataContainer \
  --domain-identifier xyz.epsilver.popmakerstudio \
  --subdirectory Documents' | grep arkit_capture

ssh macbookpro.local 'xcrun devicectl device copy from \
  --device 00008140-0008641C1E90801C \
  --domain-type appDataContainer \
  --domain-identifier xyz.epsilver.popmakerstudio \
  --source Documents/arkit_capture_<ts>.jsonl \
  --destination /tmp/fixture.jsonl'
```

(The files also appear in the iOS Files app under On My iPhone → Pop Maker
Studio, but devicectl is scriptable.)

## 3. Replay (Mac; build once with ninja)

```sh
cd ~/dev/pop-maker-studio
ninja -C build-mac arkit-native-replay
export PMS_ASSET_ROOT=$HOME/dev/pms-ios/Engine/EngineAssets
export PMS_SHADER_DIR=$HOME/dev/pms-ios/Shaders/msl

# real-face fixture: renders every recorded frame, writes a PNG every 45
./build-mac/arkit-native-replay /tmp/fixture.jsonl /tmp/out <filter_id> <amount>

# synthetic canonical head: scripted neutral/blink/gaze/yaw + assertions
./build-mac/arkit-native-replay tools/arkit_face_canonical.obj /tmp/out 13 1.2

# extra knobs
#   5th arg "r,g,b"           flat skin tone (whitening / tone-adaptation QA)
#   PMS_NATIVE_ATLAS=x.png    force an atlas from models/face/arkit/
#                             (checker.png = alignment; makeup_*.png = plates)
./build-mac/arkit-native-replay tools/arkit_face_canonical.obj /tmp/deep 22 1.3 "96,66,50"
PMS_NATIVE_ATLAS=checker.png ./build-mac/arkit-native-replay tools/arkit_face_canonical.obj /tmp/chk 13 1.0
```

`filter_id` = FaceFilter enum in `src/face_filters.h` (Goth 13, Barbie 14,
CatEye 26, EGirl 22, Doll 23…). Plate looks: force via `PMS_NATIVE_ATLAS`.

## 4. What to look for in the PNGs

- **Checker**: glued to the face at every pose; eye/mouth holes open; no
  swimming. If the checker is off, everything after it is meaningless.
- **Liner/lash**: ON the visible lash line (the hole rim), not above it,
  stopping short of the hole's oversized outer corners.
- **Blink frames**: lid pigment slides down OVER the iris as one piece;
  iris disc disappears behind the lid (depth + stencil). No doubled arcs
  (depth), no strokes lagging one by one (one-euro vertex filter).
- **Gaze frames**: iris follows; still clipped by the aperture.
- **Yaw frames**: no pale "mask edge" past the silhouette (grazing fade).
- **Skin tone runs** (light vs deep `--skin`): pigment adapts — pink stays
  pink but takes the skin's brightness; NO whitening from foundation
  washes; skin texture visible through pigment.
- **Under-eye / bags**: aegyo looks (Doll Pink, Angel, Anime Doll, …) may
  show a sheer bright strip tight under the lash — never a dark trough
  under it. A highlight+crease pair sculpts real bags worse; material-
  aware blend then deepens the trough. Check on a *real* fixture, not the
  canonical head (bags are per-person). If bags pop, kill/soften `el_aegyo`
  and rebake plates → ARKit atlases.

## 5. Gates (run all before pushing engine or atlas changes)

Linux: `cmake --build build --target engine-smoke arkit-map-smoke &&
./build/engine-smoke && ./build/arkit-map-smoke tools/arkit_face_canonical.obj`

Mac (`ninja -C build-mac …`): `engine-smoke`, `arkit-map-smoke`
(tier-2/3 bridge: blink sim, gaze checks, weight bounds),
`metal-render-test` (MP path + Metal validation — it catches real crashes),
`arkit-native-replay` synthetic (asserts pigment placement/blink/gaze).
Then the iOS device build. Ship engine → `origin/dev`, pms-ios (incl.
`Engine/EngineAssets/models/face/arkit/`) → `origin/main`; the user
deploys via the Desktop "Deploy Pop Maker.command" (ssh signing fails:
GUI keychain).

## 6. Atlas iteration (art fixes are texture edits, not engine changes)

`tools/gen_arkit_makeup.py` bakes everything in
`pms-ios/Engine/EngineAssets/models/face/arkit/`:
73 plates (MP-UV art through the TPS-pinned, brow-pinned, bilinear warp;
brow art erased — painted brows can't match real brow hair) + 30 builtin
looks (eye layers painted directly on the hole rims in ARKit UV) +
`checker.png`. Delete `tools/arkit_uv_warp.npz` to force a warp-map
rebuild after correspondence changes. Rebake → replay → look → commit
atlases in pms-ios.

## History / why these exact checks exist

Every check above corresponds to a shipped failure: 8 rounds of 2D-bridge
misalignment (see pms-ios `docs/ARKIT_NATIVE_PLAN.md` for the table),
liner floating above lash lines (warp built from similarity fit only),
phantom brows (TPS dragging the brow zone), leopard freckles
(nearest-neighbor warp), mask edges at profile (no silhouette fade),
doubled liner on closed lids (no depth buffer), twitchy lids (unfiltered
vertex noise), whitening (mid-grey multiply tint), under-eye bags carved
harder by aegyo highlight+crease (RGBA blur muddying the strip into dark
pigment the material blend then deepened). Static neutral-pose
