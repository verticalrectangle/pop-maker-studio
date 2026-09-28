#pragma once
// Canvas 2D context for Script clips (docs/SCRIPT_API.md §4), implemented on
// Skia: the JS classes Context2D / CanvasGradient / Image for one QuickJS
// runtime, the per-runtime font families, and HarfBuzz text shaping.
//
// The path is stored in device space (points transformed by the CTM when
// added, as the HTML spec defines) and mapped back into the CTM's user space
// at fill/stroke/clip time, so gradients and stroke widths follow the
// transform in effect when the path is painted.
//
// Unknown members throw TypeError: the prototype chain ends in a guard
// object whose exotic get/set hooks reject any name not defined on
// Context2D.prototype, and instances are non-extensible.
//
// Main/GL thread only (the runtime that owns the objects).
#include "quickjs.h"

#include "include/core/SkRefCnt.h"

#include <string>

class SkCanvas;
class SkTypeface;

// New Context2D object (registers the classes on the context's runtime on
// first use). Drawing throws until c2d_begin_frame attaches a canvas.
JSValue c2d_new_context(JSContext* ctx);

// Attach this frame's canvas (cleared) and reset every drawing-state
// attribute to its spec default. Scripts draw in logical px
// (logical_w × logical_h = f.width × f.height); the canvas may be smaller
// (preview) and is scaled to fit. nullptr detaches the canvas (drawing then
// throws; measureText keeps working).
void c2d_begin_frame(JSContext* ctx, JSValueConst c2d, SkCanvas* canvas, int logical_w,
                     int logical_h);

// pms.image(path): decode with stb_image (straight RGBA) into an Image
// object {width, height}. Throws (returns JS_EXCEPTION) on decode failure.
JSValue c2d_new_image(JSContext* ctx, const std::string& path);

// pms.font(family, path): add a face to `family` for this context (several
// files under one family = weights/styles matched like CSS). Idempotent.
bool c2d_register_font(JSContext* ctx, JSValueConst c2d, const std::string& family,
                       const std::string& path, std::string* err);

// Built-in typefaces (embedded): family "Inter" (weights 400/700/900) or
// "Mono" (JetBrains Mono 400). Used by the host for the error card.
sk_sp<SkTypeface> c2d_builtin_typeface(const std::string& family, int weight);

// CSS colour → straight RGBA floats. Accepts #rgb/#rgba/#rrggbb/#rrggbbaa,
// rgb()/rgba()/hsl()/hsla() (comma or space syntax, % channels, / alpha),
// the CSS named colours and "transparent".
bool c2d_parse_color(const char* s, float rgba[4]);
