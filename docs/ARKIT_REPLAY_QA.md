# ARKit makeup QA — capture, replay, gates

How to judge and tune an ARKit makeup look (architecture: pms-ios
`docs/ARKIT_NATIVE_PLAN.md`) on a real face without touching the phone for
every iteration. Everything below runs from the Linux box via
`ssh macbookpro.local` except the capture itself.

## 1. Capture (on the phone)

Record screen, front camera, **triple-tap the preview**: the face overlay
toggles on and 10 s (300 frames at 30 fps) are recorded to the app's
Documents as `arkit_capture_<unix-ts>/` — `frames.jsonl` plus `fNNNN.jpg`,
the exact portrait BGRA frame the engine received for each ARFrame. A hint
reports "Fixture saved". Script: hold still ~2 s, blink, smile, look around,
turn the head both ways, talk.

`frames.jsonl`, one ARFrame per line: `t`, `w`, `h`, `verts` (1220×3,
anchor space, m), `model` / `view` / `proj` / `eye_l` / `eye_r` (column-major
4×4), `blend` (52, MediaPipe order), `exposure` {`duration`, `offset`},
`light` {`dir`, `intensity`, `ambient`, `kelvin`, `sh`}, `img`.

## 2. Pull it to the Mac (phone on USB/Wi-Fi, no unlock needed)

```sh
ssh macbookpro.local 'xcrun devicectl device info files \
  --device 00008140-0008641C1E90801C --domain-type appDataContainer \
  --domain-identifier xyz.epsilver.popmakerstudio --subdirectory Documents' | grep arkit_capture
ssh macbookpro.local 'xcrun devicectl device copy from \
  --device 00008140-0008641C1E90801C --domain-type appDataContainer \
  --domain-identifier xyz.epsilver.popmakerstudio \
  --source Documents/arkit_capture_<ts> --destination ~/arkit_fixtures/arkit_capture_<ts>'
```

Keep fixtures out of `/tmp` (wiped on reboot).

## 3. Replay

```sh
cd ~/dev/pop-maker-studio
scripts/build_mac.sh        # headless engine (= the iOS configuration) + gates in build-mac/
export PMS_ASSET_ROOT=$HOME/dev/pms-ios/Engine/EngineAssets
export PMS_SHADER_DIR=$HOME/dev/pms-ios/Shaders/msl
mkdir -p /tmp/rev
./build-mac/arkit-native-replay ~/arkit_fixtures/arkit_capture_<ts> /tmp/rev egirl 1.0 --raw
```

Every recorded frame renders (the vertex filter runs in time); PNGs are
written for every 15th frame (`--every N`) plus the max-blink, max-jaw-open,
max-smile and max-yaw frames (`--frames i,j` picks exact frames). `--raw`
adds the untouched frame as `fNNNN_raw.png` for before/after review. The
tool fails if the look does not report `applied` on a picked frame.

## 4. Iterate

- Colors, amounts, liner/lash shape: edit the look JSON in pms-ios
  `Engine/EngineAssets/models/face/arkit/<id>.json` and re-run the replay —
  no rebuild.
- Mask geometry (blush/shadow placement, lip region, freckles, highlights):
  `tools/gen_arkit_makeup.py --assets <pms-ios>/Engine/EngineAssets`
  (numpy + Pillow; runs on Linux), then replay.
- Renderer: `src/arkit_makeup.mm`; `ninja -C build-mac arkit-native-replay`.

## 5. What to look for

- **Placement**: liner and lash roots sit on the real upper lash line — also
  with lowered lids / gaze down, where ARKit's rim rides onto the lid (no
  bare lid between ink and eye) — through blink frames, and the liner flows
  into the wing as one stroke; the fringe moves with the lid and never
  hooks up the lid; lipstick follows the wearer's real lip edge
  (plus the look's overline, smooth, no JPEG-block steps) and never paints
  teeth, tongue or the skin around a small mouth; blush sits on the apples;
  brows untouched.
- **Skin**: pores and shading visible through blush and smoothing; no seam
  at the mesh edge; eyes, brows and lips are never smoothed.
- **Light**: lip gloss is broken up by the lips' texture (no white sticker),
  never a stripe along the mouth seam; the nose tip catches the shine.
- **Hard frames**: yaw (no pigment past the silhouette), smile (lips and
  blush stretch with the skin), blink.

## 6. Gates (before pushing engine or look changes)

Linux: `cmake --build build --target engine-smoke && ./build/engine-smoke`.

Mac: `scripts/build_mac.sh --run` — `engine-smoke`, then
`arkit-native-replay synth` (E-Girl on the canonical head: pigment only
inside the face, blink moves the eye makeup and not the lips, yaw keeps it
attached, `face_overlay` renders, an unknown look reports `look_missing`),
then `metal-render-test`. Then the iOS device build. Ship engine →
`origin/dev`, pms-ios (incl. `Engine/EngineAssets/models/face/arkit/`) →
`origin/main`.
