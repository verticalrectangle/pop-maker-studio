// shape.cpp — Shape clip implementation. See shape.h for the data model.
#include "shape.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

// ── Easing (mirrors app_state.cpp apply_interp — kept local so the shape
// module stays self-contained for the iOS port's engine split) ────────────────
float apply_interp(float t, InterpType it) {
    switch (it) {
        case InterpType::EaseIn:   return t * t;
        case InterpType::EaseOut:  return t * (2.f - t);
        case InterpType::EaseBoth: return t < 0.5f ? 2.f*t*t : -1.f + (4.f - 2.f*t)*t;
        case InterpType::Hold:     return 0.f;
        default:                   return t;
    }
}

float lerp(float a, float b, float t) { return a + t * (b - a); }

} // namespace

// ── ShapePath ────────────────────────────────────────────────────────────────

float ShapePath::arc_length() const {
    if (pts.size() < 2) return 0.f;
    float len = 0.f;
    int n = (int)pts.size();
    for (int i = 0; i < n - 1; ++i) {
        float dx = pts[i+1].x - pts[i].x;
        float dy = pts[i+1].y - pts[i].y;
        len += std::sqrt(dx*dx + dy*dy);
    }
    if (closed) {
        float dx = pts[0].x - pts[n-1].x;
        float dy = pts[0].y - pts[n-1].y;
        len += std::sqrt(dx*dx + dy*dy);
    }
    return len;
}

void ShapePath::bounds(float& minx, float& miny, float& maxx, float& maxy) const {
    if (pts.empty()) return;
    minx = maxx = pts[0].x;
    miny = maxy = pts[0].y;
    for (auto& p : pts) {
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
}

ShapePath ShapePath::resample(int n) const {
    ShapePath out;
    out.closed = closed;
    if (n < 2 || pts.empty()) { out.pts = pts; return out; }

    float total = arc_length();
    if (total <= 1e-6f) {
        // Degenerate (all points coincident): just replicate the first point.
        out.pts.resize((size_t)n, pts.front());
        return out;
    }

    // Walk the polyline, emitting a point every total/(n-1) of arc length.
    // For closed paths the closing segment is included in `total` but we do
    // NOT emit a final point == first (the caller renders closure implicitly).
    out.pts.reserve((size_t)n);
    out.pts.push_back(pts[0]);
    int seg = 0;
    float seg_start = 0.f;
    float seg_len = 0.f;
    int seg_count = (int)pts.size() - 1;
    if (closed) seg_count = (int)pts.size();  // include wrap segment

    auto seg_end_pt = [&](int s) -> const ShapePoint& {
        return pts[(s + 1) % (int)pts.size()];
    };

    for (int i = 1; i < n - (closed ? 0 : 0); ++i) {
        float target = total * (float)i / (float)(n - 1);
        // Advance segments until we straddle `target`.
        while (seg < seg_count) {
            float dx = seg_end_pt(seg).x - pts[seg].x;
            float dy = seg_end_pt(seg).y - pts[seg].y;
            seg_len = std::sqrt(dx*dx + dy*dy);
            if (seg_start + seg_len >= target || seg_len <= 1e-8f) break;
            seg_start += seg_len;
            ++seg;
        }
        if (seg >= seg_count) { out.pts.push_back(pts.back()); continue; }
        float denom = seg_len > 1e-8f ? seg_len : 1e-8f;
        float f = (target - seg_start) / denom;
        if (f < 0.f) f = 0.f; if (f > 1.f) f = 1.f;
        const ShapePoint& a = pts[seg];
        const ShapePoint& b = seg_end_pt(seg);
        ShapePoint p;
        p.x = lerp(a.x, b.x, f);
        p.y = lerp(a.y, b.y, f);
        p.width = lerp(a.width, b.width, f);
        out.pts.push_back(p);
    }
    if (!closed) out.pts.push_back(pts.back());
    return out;
}

void ShapePath::normalise_bbox() {
    if (pts.empty()) return;
    float minx, miny, maxx, maxy;
    bounds(minx, miny, maxx, maxy);
    float w = maxx - minx, h = maxy - miny;
    float s = w > h ? w : h;
    if (s <= 1e-6f) s = 1.f;
    for (auto& p : pts) {
        p.x = (p.x - minx) / s;
        p.y = (p.y - miny) / s;
    }
}

// ── PathPropTrack ────────────────────────────────────────────────────────────

ShapePath PathPropTrack::eval(float t, const ShapePath& base) const {
    if (keys.empty()) return base;
    if ((int)keys.size() == 1 || t <= keys.front().time) return keys.front().path;
    if (t >= keys.back().time) return keys.back().path;
    for (int i = 0; i < (int)keys.size() - 1; ++i) {
        if (t >= keys[i].time && t < keys[i+1].time) {
            if (keys[i].interp == InterpType::Hold) return keys[i].path;
            float alpha = (t - keys[i].time) / (keys[i+1].time - keys[i].time);
            alpha = apply_interp(alpha, keys[i].interp);
            const ShapePath& a = keys[i].path;
            const ShapePath& b = keys[i+1].path;
            // Resample both to the larger count so point counts match, then
            // lerp each point. Closed-ness must agree; if it differs we follow
            // the FROM key (the morph visibly opens/closes at the next key).
            int na = a.size(), nb = b.size();
            int n = na > nb ? na : nb;
            if (n < 2) return a;
            ShapePath ra = a.resample(n);
            ShapePath rb = b.resample(n);
            ShapePath out;
            out.closed = a.closed;
            out.pts.reserve((size_t)n);
            for (int k = 0; k < n; ++k) {
                ShapePoint p;
                p.x     = lerp(ra.pts[k].x,     rb.pts[k].x,     alpha);
                p.y     = lerp(ra.pts[k].y,     rb.pts[k].y,     alpha);
                p.width = lerp(ra.pts[k].width, rb.pts[k].width, alpha);
                out.pts.push_back(p);
            }
            return out;
        }
    }
    return keys.back().path;
}

void PathPropTrack::set(float t, const ShapePath& p, InterpType it) {
    for (auto& k : keys) {
        if (std::fabs(k.time - t) < 0.02f) { k.path = p; k.interp = it; return; }
    }
    keys.push_back({t, p, it});
    std::sort(keys.begin(), keys.end(),
              [](const PathKeyframe& a, const PathKeyframe& b){ return a.time < b.time; });
}

int PathPropTrack::find_nearest(float t, float tol) const {
    int best = -1; float bd = tol;
    for (int i = 0; i < (int)keys.size(); ++i) {
        float d = std::fabs(keys[i].time - t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ── ColorPropTrack ───────────────────────────────────────────────────────────

void ColorPropTrack::eval(float t, const float base[4], float out[4]) const {
    if (keys.empty()) { for (int i = 0; i < 4; ++i) out[i] = base[i]; return; }
    const ColorKeyframe* ka = &keys.front();
    const ColorKeyframe* kb = nullptr;
    float alpha = 0.f;
    if ((int)keys.size() == 1 || t <= keys.front().time) {
        // clamp to first key
    } else if (t >= keys.back().time) {
        ka = &keys.back();
    } else {
        for (int i = 0; i < (int)keys.size() - 1; ++i) {
            if (t >= keys[i].time && t < keys[i+1].time) {
                ka = &keys[i];
                if (keys[i].interp != InterpType::Hold) {
                    kb = &keys[i+1];
                    alpha = (t - keys[i].time) / (keys[i+1].time - keys[i].time);
                    alpha = apply_interp(alpha, keys[i].interp);
                }
                break;
            }
        }
    }
    for (int i = 0; i < 4; ++i)
        out[i] = kb ? lerp(ka->v[i], kb->v[i], alpha) : ka->v[i];
}

void ColorPropTrack::set(float t, const float v[4], InterpType it) {
    for (auto& k : keys) {
        if (std::fabs(k.time - t) < 0.02f) {
            for (int i = 0; i < 4; ++i) k.v[i] = v[i];
            k.interp = it;
            return;
        }
    }
    ColorKeyframe k;
    k.time = t;
    for (int i = 0; i < 4; ++i) k.v[i] = v[i];
    k.interp = it;
    keys.push_back(k);
    std::sort(keys.begin(), keys.end(),
              [](const ColorKeyframe& a, const ColorKeyframe& b){ return a.time < b.time; });
}

void ColorPropTrack::remove_at(float t, float tol) {
    int i = find_nearest(t, tol);
    if (i >= 0) keys.erase(keys.begin() + i);
}

int ColorPropTrack::find_nearest(float t, float tol) const {
    int best = -1; float bd = tol;
    for (int i = 0; i < (int)keys.size(); ++i) {
        float d = std::fabs(keys[i].time - t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ── Colour slot table ────────────────────────────────────────────────────────

const char* const kShapeColorProps[] = {"fill_col", "stroke_col", "grad_col2", "glow_col"};
const int kShapeColorPropCount = 4;

float* shape_style_color_slot(ShapeStyle& s, const char* name) {
    if (!strcmp(name, "fill_col"))   return s.fill_col;
    if (!strcmp(name, "stroke_col")) return s.stroke_col;
    if (!strcmp(name, "grad_col2"))  return s.grad_col2;
    if (!strcmp(name, "glow_col"))   return s.glow_col;
    return nullptr;
}
const float* shape_style_color_slot(const ShapeStyle& s, const char* name) {
    return shape_style_color_slot(const_cast<ShapeStyle&>(s), name);
}

// ── Kaleidoscope replication ─────────────────────────────────────────────────

namespace {

void replicate_verts(const std::vector<ShapeVertex>& in,
                     std::vector<ShapeVertex>& out,
                     float cx, float cy, int fold, bool reflect) {
    out.reserve(in.size() * (size_t)fold);
    const float step = 2.f * 3.14159265f / (float)fold;
    for (int i = 0; i < fold; ++i) {
        bool mirror = reflect && (i & 1);
        float c = std::cos(step * (float)i), s = std::sin(step * (float)i);
        for (int j = 0; j < (int)in.size(); ++j) {
            ShapeVertex v = in[j];
            float dx = v.x - cx, dy = v.y - cy;
            if (mirror) dy = -dy;               // reflect across the local x-axis
            v.x = cx + dx * c - dy * s;
            v.y = cy + dx * s + dy * c;
            out.push_back(v);
            // Reflection flips winding: emit mirrored triangles in reverse
            // vertex order to keep every output triangle CCW.
            if (mirror && (j % 3) == 2)
                std::swap(out[out.size() - 2], out[out.size() - 1]);
        }
    }
}

} // namespace

ShapeGeometry shape_radial_replicate(const ShapeGeometry& g,
                                     float cx, float cy,
                                     int fold, bool reflect) {
    if (fold <= 1) return g;
    ShapeGeometry out;
    replicate_verts(g.fill,   out.fill,   cx, cy, fold, reflect);
    replicate_verts(g.stroke, out.stroke, cx, cy, fold, reflect);
    return out;
}

// ── Presets ──────────────────────────────────────────────────────────────────

namespace {

ShapePath make_polygon(int sides, float r = 0.5f, float cx = 0.5f, float cy = 0.5f,
                       float rot = -3.14159265f / 2.f) {
    ShapePath p;
    p.closed = true;
    if (sides < 3) sides = 3;
    p.pts.reserve((size_t)sides);
    for (int i = 0; i < sides; ++i) {
        float a = rot + (float)i * 2.f * 3.14159265f / (float)sides;
        ShapePoint pt;
        pt.x = cx + std::cos(a) * r;
        pt.y = cy + std::sin(a) * r;
        pt.width = 0.008f;
        p.pts.push_back(pt);
    }
    return p;
}

ShapePath make_star(int points, float outer, float inner) {
    ShapePath p;
    p.closed = true;
    if (points < 3) points = 3;
    int n = points * 2;
    p.pts.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        float a = -3.14159265f / 2.f + (float)i * 3.14159265f / (float)points;
        float r = (i & 1) ? inner : outer;
        ShapePoint pt;
        pt.x = 0.5f + std::cos(a) * r;
        pt.y = 0.5f + std::sin(a) * r;
        pt.width = 0.008f;
        p.pts.push_back(pt);
    }
    return p;
}

ShapePath make_heart() {
    // Sample the classic heart parametric, normalised to [0,1]².
    ShapePath p;
    p.closed = true;
    int n = 48;
    p.pts.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        float t = (float)i * 2.f * 3.14159265f / (float)n;
        float x = 16.f * std::pow(std::sin(t), 3);
        float y = 13.f*std::cos(t) - 5.f*std::cos(2*t) - 2.f*std::cos(3*t) - std::cos(4*t);
        ShapePoint pt;
        pt.x = (x / 32.f) + 0.5f;
        pt.y = (-y / 32.f) + 0.5f;   // flip y (canvas y-down)
        pt.width = 0.008f;
        p.pts.push_back(pt);
    }
    p.normalise_bbox();
    return p;
}

ShapePath make_circle(int n = 48) {
    return make_polygon(n, 0.5f, 0.5f, 0.5f, 0.f);
}

ShapePath make_square() {
    ShapePath p;
    p.closed = true;
    p.pts = {{0.05f, 0.05f, 0.008f}, {0.95f, 0.05f, 0.008f},
             {0.95f, 0.95f, 0.008f}, {0.05f, 0.95f, 0.008f}};
    return p;
}

ShapePath make_triangle() {
    return make_polygon(3, 0.5f, 0.5f, 0.42f);
}

ShapePath make_diamond() {
    ShapePath p;
    p.closed = true;
    p.pts = {{0.5f, 0.02f, 0.008f}, {0.98f, 0.5f, 0.008f},
             {0.5f, 0.98f, 0.008f}, {0.02f, 0.5f, 0.008f}};
    return p;
}

ShapePath make_cross() {
    // Plus sign as a 12-point polygon.
    float a = 0.15f, b = 0.35f;  // arm half-width, arm extent
    ShapePath p;
    p.closed = true;
    p.pts = {
        {0.5f-b, 0.5f-a, 0.008f}, {0.5f-a, 0.5f-a, 0.008f},
        {0.5f-a, 0.5f-b, 0.008f}, {0.5f-a, 0.5f+b, 0.008f},
        {0.5f-b, 0.5f+b, 0.008f}, {0.5f-b, 0.5f+a, 0.008f},
        {0.5f+b, 0.5f+a, 0.008f}, {0.5f+b, 0.5f+b, 0.008f},
        {0.5f+a, 0.5f+b, 0.008f}, {0.5f+a, 0.5f-b, 0.008f},
        {0.5f+b, 0.5f-b, 0.008f}, {0.5f+b, 0.5f-a, 0.008f},
    };
    return p;
}

ShapePath make_arrow(float stem, float head) {
    // Right-pointing arrow. stem = stem length fraction (0..1 of body),
    // head = head length fraction (0..1 of total width).
    float sy = 0.4f;  // stem half-height
    float hy = 0.5f;  // head half-height
    float sx0 = 0.05f;
    float sx1 = 0.05f + (0.9f - head) * stem;
    float hx0 = sx1;
    float hx1 = 0.95f;
    ShapePath p;
    p.closed = true;
    p.pts = {
        {sx0, 0.5f - sy, 0.008f}, {sx1, 0.5f - sy, 0.008f},
        {hx0,  0.5f - hy, 0.008f}, {hx1, 0.5f, 0.008f},
        {hx0,  0.5f + hy, 0.008f}, {sx1, 0.5f + sy, 0.008f},
        {sx0, 0.5f + sy, 0.008f},
    };
    return p;
}

ShapePath make_lightning(float jag) {
    if (jag < 0.02f) jag = 0.18f;
    ShapePath p;
    p.closed = true;
    // A zigzag bolt outline.
    float cx = 0.5f;
    p.pts = {
        {cx - 0.06f, 0.02f, 0.008f},
        {cx + 0.10f, 0.30f, 0.008f},
        {cx + 0.02f, 0.30f, 0.008f},
        {cx + 0.14f + jag, 0.55f, 0.008f},
        {cx + 0.04f, 0.55f, 0.008f},
        {cx + 0.12f, 0.98f, 0.008f},
        {cx - 0.10f, 0.62f, 0.008f},
        {cx - 0.02f, 0.62f, 0.008f},
        {cx - 0.14f - jag, 0.38f, 0.008f},
        {cx - 0.04f, 0.38f, 0.008f},
    };
    return p;
}

ShapePath make_burst(int points, float inner) {
    // Like a star but with sharp, narrow spikes — smaller inner ratio default.
    if (inner < 0.05f) inner = 0.18f;
    return make_star(points, 0.5f, inner);
}

} // namespace

const char* shape_preset_name(ShapePreset p) {
    switch (p) {
        case ShapePreset::Circle:     return "circle";
        case ShapePreset::Square:     return "square";
        case ShapePreset::Triangle:   return "triangle";
        case ShapePreset::Star:       return "star";
        case ShapePreset::Heart:      return "heart";
        case ShapePreset::Polygon:    return "polygon";
        case ShapePreset::Hexagon:    return "hexagon";
        case ShapePreset::Burst:      return "burst";
        case ShapePreset::Arrow:      return "arrow";
        case ShapePreset::Lightning:  return "lightning";
        case ShapePreset::Diamond:    return "diamond";
        case ShapePreset::Cross:      return "cross";
    }
    return "circle";
}

bool shape_preset_from_name(const std::string& s, ShapePreset& out) {
    static const char* names[] = {
        "circle","square","triangle","star","heart","polygon",
        "hexagon","burst","arrow","lightning","diamond","cross"
    };
    for (int i = 0; i < 12; ++i) {
        if (s == names[i]) { out = (ShapePreset)i; return true; }
    }
    return false;
}

ShapePath shape_preset_bake(ShapePreset p, const std::vector<float>& params) {
    auto pf = [&](int i, float d) { return (i < (int)params.size()) ? params[i] : d; };
    switch (p) {
        case ShapePreset::Circle:    return make_circle();
        case ShapePreset::Square:    return make_square();
        case ShapePreset::Triangle:  return make_triangle();
        case ShapePreset::Diamond:   return make_diamond();
        case ShapePreset::Cross:     return make_cross();
        case ShapePreset::Heart:     return make_heart();
        case ShapePreset::Star:      return make_star((int)pf(0, 5),  0.5f, pf(1, 0.22f));
        case ShapePreset::Burst:     return make_burst((int)pf(0, 12), pf(1, 0.18f));
        case ShapePreset::Polygon:   return make_polygon((int)pf(0, 5));
        case ShapePreset::Hexagon:   return make_polygon(6);
        case ShapePreset::Arrow:     return make_arrow(pf(0, 0.7f), pf(1, 0.25f));
        case ShapePreset::Lightning: return make_lightning(pf(0, 0.18f));
    }
    return make_circle();
}

// ── Tessellation ─────────────────────────────────────────────────────────────
//
// Fill: ear-clipping triangulation of the (possibly closed) polygon in canvas
// space. Open paths produce no fill. Stroke: a triangle strip per segment,
// extruded along the segment normal by half the (scaled) width. stroke_length
// reveals only the first fraction of the arc length; fill alpha is faded in
// over the last 40% of the reveal so the fill appears as the stroke completes.

namespace {

struct Vec2 { float x, y; };

// Transform a local [0,1] path point into canvas pixel space.
Vec2 to_canvas(float lx, float ly, int canvas_w, int canvas_h,
               float cx, float cy, float hw, float hh, float cos_r, float sin_r) {
    // Local [0,1] → centred [-1,1] → scaled by half-size → rotated → translated.
    float px = (lx - 0.5f) * 2.f * hw;
    float py = (ly - 0.5f) * 2.f * hh;
    float rx = px * cos_r - py * sin_r;
    float ry = px * sin_r + py * cos_r;
    return { cx + rx, cy + ry };
}

// Cross product of (b-a) × (c-a) — positive = CCW in y-down canvas space.
float cross(const Vec2& a, const Vec2& b, const Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Ear-clip a simple polygon (CCW or CW) into triangles. Handles convex and
// concave. Returns empty for < 3 vertices.
std::vector<ShapeVertex> ear_clip(const std::vector<Vec2>& poly,
                                  const std::vector<Vec2>& uv) {
    std::vector<ShapeVertex> out;
    int n = (int)poly.size();
    if (n < 3) return out;
    out.reserve((size_t)(n - 2) * 3);
    // Work on indices; detect winding and reverse if CW so the clipper's
    // "convex vertex" test is consistent.
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    float signed_area = 0.f;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        signed_area += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
    }
    bool ccw = signed_area > 0.f;
    // In y-down canvas space, CCW (screen sense) is negative signed area; we
    // want the clipper to treat "convex" as the ear tip, so normalise to the
    // clipper's expectation: iterate so that cross(a,b,c) > 0 at convex tips.
    auto convex = [&](int a, int b, int c) {
        float cr = cross(poly[a], poly[b], poly[c]);
        return ccw ? cr > 0.f : cr < 0.f;
    };
    auto point_in_tri = [&](int a, int b, int c, const Vec2& p) {
        float d1 = cross(poly[a], poly[b], p);
        float d2 = cross(poly[b], poly[c], p);
        float d3 = cross(poly[c], poly[a], p);
        bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(neg && pos);
    };

    int guard = 0;
    while ((int)idx.size() > 3 && guard++ < n * n) {
        bool found = false;
        int m = (int)idx.size();
        for (int i = 0; i < m; ++i) {
            int prev = idx[(i + m - 1) % m];
            int cur  = idx[i];
            int nxt  = idx[(i + 1) % m];
            if (!convex(prev, cur, nxt)) continue;
            bool any_inside = false;
            for (int k = 0; k < m; ++k) {
                int v = idx[k];
                if (v == prev || v == cur || v == nxt) continue;
                if (point_in_tri(prev, cur, nxt, poly[v])) { any_inside = true; break; }
            }
            if (any_inside) continue;
            // Ear found — emit triangle and clip the tip.
            out.push_back({poly[prev].x, poly[prev].y, uv[prev].x, uv[prev].y});
            out.push_back({poly[cur].x,  poly[cur].y,  uv[cur].x,  uv[cur].y});
            out.push_back({poly[nxt].x,  poly[nxt].y,  uv[nxt].x,  uv[nxt].y});
            idx.erase(idx.begin() + i);
            found = true;
            break;
        }
        if (!found) break;  // degenerate / self-intersecting — bail with what we have
    }
    if ((int)idx.size() == 3) {
        out.push_back({poly[idx[0]].x, poly[idx[0]].y, uv[idx[0]].x, uv[idx[0]].y});
        out.push_back({poly[idx[1]].x, poly[idx[1]].y, uv[idx[1]].x, uv[idx[1]].y});
        out.push_back({poly[idx[2]].x, poly[idx[2]].y, uv[idx[2]].x, uv[idx[2]].y});
    }
    return out;
}

} // namespace

ShapeGeometry shape_tessellate(const ShapePath& path,
                               float stroke_length, float width_mul,
                               float base_stroke_width,
                               int canvas_w, int canvas_h,
                               float cx, float cy, float hw, float hh,
                               float cos_r, float sin_r)
{
    ShapeGeometry g;
    int n = path.size();
    if (n < 2 || hw <= 0.f || hh <= 0.f) return g;

    // Project all path points to canvas space; keep local [0,1] as UVs for
    // gradient/glow shading.
    std::vector<Vec2> cv(n), uv(n);
    for (int i = 0; i < n; ++i) {
        cv[i] = to_canvas(path.pts[i].x, path.pts[i].y,
                          canvas_w, canvas_h, cx, cy, hw, hh, cos_r, sin_r);
        uv[i] = {path.pts[i].x, path.pts[i].y};
    }

    // ── Fill (closed paths only) ─────────────────────────────────────────────
    if (path.closed && n >= 3) {
        // Fade fill alpha in over the last 40% of the stroke reveal so the fill
        // appears as the stroke completes. The shader still receives full-alpha
        // verts; we encode the fade as a uniform upstream. Here we just emit
        // the geometry — the draw-on reveal gates the fill pass at the call
        // site (scene_add_shape multiplies fill alpha by the fade).
        g.fill = ear_clip(cv, uv);
    }

    // ── Stroke (triangle strip per segment) ──────────────────────────────────
    // Each segment becomes a quad (2 triangles). Width at each point =
    // (base_stroke_width + point.width) * width_mul, in canvas px = frac * h.
    auto seg_count = path.closed ? n : n - 1;
    float total_len = 0.f;
    std::vector<float> seg_len((size_t)seg_count, 0.f);
    for (int i = 0; i < seg_count; ++i) {
        int j = (i + 1) % n;
        float dx = cv[j].x - cv[i].x;
        float dy = cv[j].y - cv[i].y;
        seg_len[i] = std::sqrt(dx*dx + dy*dy);
        total_len += seg_len[i];
    }
    float reveal = total_len * fmaxf(0.f, fminf(1.f, stroke_length));

    float px_h = (float)canvas_h;  // width is a fraction of canvas height
    auto width_px = [&](int i) {
        float w = (base_stroke_width + path.pts[i].width) * width_mul * px_h;
        if (w < 1.f) w = 1.f;   // at least 1px so a 0-width stroke still shows
        return w * 0.5f;
    };

    float walked = 0.f;
    for (int i = 0; i < seg_count; ++i) {
        int j = (i + 1) % n;
        float sl = seg_len[i];
        if (sl <= 1e-3f) continue;

        // Skip segments fully past the reveal point.
        if (walked >= reveal) break;

        // Segment may be partially revealed — clip the endpoint.
        Vec2 a = cv[i], b = cv[j];
        float seg_reveal = reveal - walked;
        if (seg_reveal < sl) {
            float f = seg_reveal / sl;
            b.x = a.x + (b.x - a.x) * f;
            b.y = a.y + (b.y - a.y) * f;
        }
        walked += sl;

        // Normal to the segment (perpendicular, y-down).
        float nx = -(b.y - a.y);
        float ny =  (b.x - a.x);
        float nl = std::sqrt(nx*nx + ny*ny);
        if (nl < 1e-3f) continue;
        nx /= nl; ny /= nl;

        float wa = width_px(i);
        float wb = width_px(j);

        // Quad: a-n, a+n, b-n, b+n → two triangles.
        Vec2 a_n = {a.x + nx * wa, a.y + ny * wa};
        Vec2 a_p = {a.x - nx * wa, a.y - ny * wa};
        Vec2 b_n = {b.x + nx * wb, b.y + ny * wb};
        Vec2 b_p = {b.x - nx * wb, b.y - ny * wb};
        Vec2 uva = uv[i], uvb = uv[j];

        // Triangle 1: a_n, a_p, b_n
        g.stroke.push_back({a_n.x, a_n.y, uva.x, uva.y});
        g.stroke.push_back({a_p.x, a_p.y, uva.x, uva.y});
        g.stroke.push_back({b_n.x, b_n.y, uvb.x, uvb.y});
        // Triangle 2: a_p, b_p, b_n
        g.stroke.push_back({a_p.x, a_p.y, uva.x, uva.y});
        g.stroke.push_back({b_p.x, b_p.y, uvb.x, uvb.y});
        g.stroke.push_back({b_n.x, b_n.y, uvb.x, uvb.y});
    }

    return g;
}
