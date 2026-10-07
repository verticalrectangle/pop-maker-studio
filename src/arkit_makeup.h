#pragma once
// arkit_makeup.h — the ARKit TrueDepth makeup renderer (Metal, iOS).
//
// A look is data: models/face/arkit/<look_id>.json (colors, amounts, liner and
// lash shape) plus the two mask atlases it names, painted in ARKit's own UV
// layout by tools/gen_arkit_makeup.py. Rendering uses ARKit's own mesh, camera
// and light estimate (arkit_face.h); see pms-ios docs/ARKIT_NATIVE_PLAN.md.
#import <Metal/Metal.h>
#include <string>

// Encode `look_id` at strength `amount` (1 = as designed; the record UI goes
// to 2) over `src`, writing the composite into `dst` (same size, BGRA8Unorm,
// not aliasing src). `time_s` is the frame's capture time (vertex filter).
// Returns false — dst untouched — when nothing was drawn. *status is one of
// "applied", "no_face", "look_missing", "pso_failed".
bool arkit_makeup_encode(id<MTLDevice> dev, id<MTLCommandBuffer> cb,
                         id<MTLTexture> src, id<MTLTexture> dst,
                         const std::string& look_id, float amount,
                         double time_s, const char** status);
