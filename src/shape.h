// shape.h — Shape clip data model (ClipType::Shape).
//
// Lives in the engine core so the desktop app and the iOS shell share ONE path
// representation, ONE keyframe eval, ONE tessellator, and ONE serializer. The
// desktop app draws the pen tool / preset picker / inspector; iOS draws the
// Pencil editor / SwiftUI inspector; both drive the engine through levers and
// both render through the engine's scene compositor (GL on desktop, Metal on
// iOS — the shape shader is a hand-written pass on each side, like the
// compositor itself).
//
// See ARCHITECTURE.md §"Clip data model" for the transform/keyframe conventions
// this mirrors.
#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "keyframe.h"

// A single point on a shape path. Coordinates are in the shape's LOCAL
// normalized space [0,1]² — the path's bounding box maps to the unit square,
// then the clip transform (pos_x/pos_y/scale_x/scale_y/rotation) places it on
// the canvas, exactly like a Background clip. `width` is a per-point stroke
// thickness in fractions of canvas height (from Apple Pencil pressure / mouse
// velocity for freehand; uniform for presets), scaled by the keyframable
// stroke_width_mul at eval time.
struct ShapePoint {
    float x     = 0.5f;
    float y     = 0.5f;
    float width = 0.008f;
};

// A path is an ordered list of points connected by straight segments (a
// polyline). closed=true connects the last point back to the first and
// enables fill; open paths render stroke only. Freehand and preset paths
// share this one representation — presets bake to a sampled ShapePath,
// freehand stores simplified input points (dense enough that straight segments
// read as smooth). Polyline (not spline) keeps path-morph interpolation exact:
// two resampled paths lerp point-for-point with no per-frame curve eval.
struct ShapePath {
    std::vector<ShapePoint> pts;
    bool closed = false;

    bool empty() const { return pts.empty(); }
    int  size()  const { return (int)pts.size(); }

    // Polyline arc length (sum of segment lengths in local space).
    float arc_length() const;
    // Axis-aligned bbox in local space. No-op (leaves outputs untouched) if empty.
    void  bounds(float& minx, float& miny, float& maxx, float& maxy) const;
    // Resample to n evenly-spaced points along the polyline (arc-length). Used
    // for path-morph interpolation: two paths must share a point count to lerp.
    // Preserves `closed`. Returns *this unchanged if n < 2 or empty.
    ShapePath resample(int n) const;
    // Normalise the bbox to [0,1]² (translate + uniform scale, preserving
    // aspect by the larger axis). Presets bake already-normalised; this is for
    // freehand input that may sit anywhere in local space.
    void normalise_bbox();
};

// Visual style — mirrors TextStyle (stroke/glow/gradient) so shape clips
// visually match the rest of the app. All colours RGBA 0..1.
struct ShapeStyle {
    float fill_col[4]    = {1.f, 1.f, 1.f, 1.f};
    bool  fill_on        = true;
    float stroke_col[4]  = {0.f, 0.f, 0.f, 1.f};
    bool  stroke_on      = false;
    float stroke_width   = 0.008f;   // base stroke width (fraction canvas height)
    int   grad_mode      = 0;        // 0=none 1=linear 2=radial 3=hue-cycle
    float grad_col2[4]   = {1.f, 1.f, 1.f, 1.f};
    float grad_angle     = 45.f;     // degrees (linear mode)
    float glow_col[4]    = {0.5f, 0.8f, 1.f, 1.f};
    bool  glow_on        = false;
    float glow_radius    = 0.02f;    // fraction canvas height
    float glow_intensity = 1.f;
};

// Path-morph keyframe: a full ShapePath snapshot at a time. PathPropTrack::eval
// resamples adjacent key paths to a common point count and lerps each point
// (x, y, width) with the same easing curves as scalar keyframes. Times are
// seconds relative to clip.start (same convention as Keyframe.time).
struct PathKeyframe {
    float      time   = 0.f;
    ShapePath  path;
    InterpType interp = InterpType::EaseBoth;
};

struct PathPropTrack {
    std::vector<PathKeyframe> keys;

    bool  empty() const { return keys.empty(); }
    // Evaluate the morph path at relative time t. Returns `base` when empty or
    // when t is outside the key range; otherwise lerps between the surrounding
    // keys (resampled to the larger of the two point counts).
    ShapePath eval(float t, const ShapePath& base) const;
    void  set(float t, const ShapePath& p, InterpType it = InterpType::EaseBoth);
    int   find_nearest(float t, float tol = 0.1f) const;
};

// ── Keyframable colours ──────────────────────────────────────────────────────
// RGBA keyframe on a ShapeStyle colour slot — same conventions as Keyframe
// (time relative to clip.start, same easing curves, tracks clamp outside the
// key range). ColorPropTracks live on the Clip (app.h) keyed by slot name; the
// style struct itself stays plain data so the inspector edits it directly.
struct ColorKeyframe {
    float      time   = 0.f;
    float      v[4]   = {1.f, 1.f, 1.f, 1.f};
    InterpType interp = InterpType::EaseBoth;
};

struct ColorPropTrack {
    std::vector<ColorKeyframe> keys;  // always sorted by time

    bool empty() const { return keys.empty(); }
    // Evaluate into out[4] (RGBA). Copies base when the track is empty;
    // otherwise clamps to the nearest key outside the range and lerps (with
    // the key's easing) inside it — same semantics as PropTrack::eval.
    void eval(float t, const float base[4], float out[4]) const;
    void set(float t, const float v[4], InterpType it = InterpType::EaseBoth);
    void remove_at(float t, float tol = 0.05f);
    int  find_nearest(float t, float tol = 0.1f) const;
};

// The ShapeStyle colour slots that accept a ColorPropTrack, in stable order
// (serialization, IPC validation, and the inspector all iterate this table).
extern const char* const kShapeColorProps[];  // fill_col / stroke_col / grad_col2 / glow_col
extern const int kShapeColorPropCount;
// Mutable pointer to the named slot's 4 floats; nullptr when unknown.
float* shape_style_color_slot(ShapeStyle& s, const char* name);
const float* shape_style_color_slot(const ShapeStyle& s, const char* name);

// ── Presets ──────────────────────────────────────────────────────────────────
// The preset is the STARTING shape baked into a path at creation. It is NOT
// stored on the clip — the path is the source of truth and is freely editable
// afterward. `params` are preset-specific knobs (point counts, inner ratios);
// see shape_preset_bake for the per-preset param layout.
enum class ShapePreset {
    Circle, Square, Triangle, Star, Heart, Polygon, Hexagon,
    Burst, Arrow, Lightning, Diamond, Cross
};
const char* shape_preset_name(ShapePreset p);
bool        shape_preset_from_name(const std::string& s, ShapePreset& out);

// Bake a preset to a normalised ShapePath.
//   Star/Burst:    params[0] = point count (>=3), params[1] = inner RADIUS in
//                  unit-box fractions (outer = 0.5; default 0.22 → a classic
//                  star. Values approaching 0.5 read as a wobbly polygon).
//   Polygon/Hex:   params[0] = side count (>=3)
//   Arrow:         params[0] = stem fraction (0..1), params[1] = head fraction (0..1)
//   Lightning:     params[0] = jag amplitude (0..0.5)
//   Circle/Square/Triangle/Heart/Diamond/Cross: no params.
// Unrecognised/missing params take preset defaults.
ShapePath shape_preset_bake(ShapePreset p, const std::vector<float>& params);

// ── Tessellation ─────────────────────────────────────────────────────────────
// Produce triangle lists (canvas-space pixels) for the fill and the stroke of
// a path. stroke_length (0..1) reveals only the first fraction of the stroke
// arc length (draw-on animation); fill alpha is faded in over the last half so
// the fill appears as the stroke completes. width_mul scales every per-point
// width. base_stroke_width is added to every point's width (so a preset with a
// uniform per-point width of 0 still shows the style's base width).
//
// Output vertices are (x, y, u, v) where u/v carry the LOCAL [0,1] position
// (post-transform-undo) for gradient/glow shading. Triangles are CCW in canvas
// space (y-down, matching the scene FBO).
struct ShapeVertex { float x, y, u, v; };
struct ShapeGeometry {
    std::vector<ShapeVertex> fill;
    std::vector<ShapeVertex> stroke;
};

ShapeGeometry shape_tessellate(const ShapePath& path,
                               float stroke_length,   // 0..1 reveal
                               float width_mul,        // global multiplier
                               float base_stroke_width,// style base (canvas frac)
                               int   canvas_w, int canvas_h,
                               float cx, float cy,     // centre px
                               float hw, float hh,     // half-size px
                               float cos_r, float sin_r);

// ── Kaleidoscope replication ─────────────────────────────────────────────────
// Replicate tessellated geometry `fold` times around canvas-space centre
// (cx,cy): replica i is rotated by i·360°/fold about the centre, and when
// `reflect` is set the odd replicas are mirrored first (dihedral kaleidoscope
// — the classic mirror-symmetric bloom). Per-vertex u/v are preserved so every
// replica keeps the original gradient/glow shading. Reflection flips triangle
// winding, so each reflected triangle's vertex order is swapped to keep the
// CCW invariant. fold <= 1 returns `g` unchanged (cheap early-out).
ShapeGeometry shape_radial_replicate(const ShapeGeometry& g,
                                     float cx, float cy,
                                     int fold, bool reflect);
