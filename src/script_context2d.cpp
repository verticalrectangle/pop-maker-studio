// Canvas 2D on Skia for Script clips — see script_context2d.h and
// docs/SCRIPT_API.md §4. Semantics follow the HTML Canvas 2D spec (Blink is
// the tie-breaker); deviations: invalid values throw TypeError/RangeError
// instead of being ignored, so ports fail loudly.
#include "script_context2d.h"

#include "inter_font.h"
#include "mono_font.h"
#include "stb_image.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/ports/SkFontMgr_empty.h"
#include "modules/skshaper/include/SkShaper.h"
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── Colours ──────────────────────────────────────────────────────────────

struct NamedColor { const char* name; uint32_t rgb; };
// CSS Color 4 named colours, sorted by name (binary search).
const NamedColor kNamedColors[] = {
    {"aliceblue",0xf0f8ff},{"antiquewhite",0xfaebd7},{"aqua",0x00ffff},{"aquamarine",0x7fffd4},
    {"azure",0xf0ffff},{"beige",0xf5f5dc},{"bisque",0xffe4c4},{"black",0x000000},
    {"blanchedalmond",0xffebcd},{"blue",0x0000ff},{"blueviolet",0x8a2be2},{"brown",0xa52a2a},
    {"burlywood",0xdeb887},{"cadetblue",0x5f9ea0},{"chartreuse",0x7fff00},{"chocolate",0xd2691e},
    {"coral",0xff7f50},{"cornflowerblue",0x6495ed},{"cornsilk",0xfff8dc},{"crimson",0xdc143c},
    {"cyan",0x00ffff},{"darkblue",0x00008b},{"darkcyan",0x008b8b},{"darkgoldenrod",0xb8860b},
    {"darkgray",0xa9a9a9},{"darkgreen",0x006400},{"darkgrey",0xa9a9a9},{"darkkhaki",0xbdb76b},
    {"darkmagenta",0x8b008b},{"darkolivegreen",0x556b2f},{"darkorange",0xff8c00},
    {"darkorchid",0x9932cc},{"darkred",0x8b0000},{"darksalmon",0xe9967a},{"darkseagreen",0x8fbc8f},
    {"darkslateblue",0x483d8b},{"darkslategray",0x2f4f4f},{"darkslategrey",0x2f4f4f},
    {"darkturquoise",0x00ced1},{"darkviolet",0x9400d3},{"deeppink",0xff1493},
    {"deepskyblue",0x00bfff},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1e90ff},
    {"firebrick",0xb22222},{"floralwhite",0xfffaf0},{"forestgreen",0x228b22},{"fuchsia",0xff00ff},
    {"gainsboro",0xdcdcdc},{"ghostwhite",0xf8f8ff},{"gold",0xffd700},{"goldenrod",0xdaa520},
    {"gray",0x808080},{"green",0x008000},{"greenyellow",0xadff2f},{"grey",0x808080},
    {"honeydew",0xf0fff0},{"hotpink",0xff69b4},{"indianred",0xcd5c5c},{"indigo",0x4b0082},
    {"ivory",0xfffff0},{"khaki",0xf0e68c},{"lavender",0xe6e6fa},{"lavenderblush",0xfff0f5},
    {"lawngreen",0x7cfc00},{"lemonchiffon",0xfffacd},{"lightblue",0xadd8e6},{"lightcoral",0xf08080},
    {"lightcyan",0xe0ffff},{"lightgoldenrodyellow",0xfafad2},{"lightgray",0xd3d3d3},
    {"lightgreen",0x90ee90},{"lightgrey",0xd3d3d3},{"lightpink",0xffb6c1},{"lightsalmon",0xffa07a},
    {"lightseagreen",0x20b2aa},{"lightskyblue",0x87cefa},{"lightslategray",0x778899},
    {"lightslategrey",0x778899},{"lightsteelblue",0xb0c4de},{"lightyellow",0xffffe0},
    {"lime",0x00ff00},{"limegreen",0x32cd32},{"linen",0xfaf0e6},{"magenta",0xff00ff},
    {"maroon",0x800000},{"mediumaquamarine",0x66cdaa},{"mediumblue",0x0000cd},
    {"mediumorchid",0xba55d3},{"mediumpurple",0x9370db},{"mediumseagreen",0x3cb371},
    {"mediumslateblue",0x7b68ee},{"mediumspringgreen",0x00fa9a},{"mediumturquoise",0x48d1cc},
    {"mediumvioletred",0xc71585},{"midnightblue",0x191970},{"mintcream",0xf5fffa},
    {"mistyrose",0xffe4e1},{"moccasin",0xffe4b5},{"navajowhite",0xffdead},{"navy",0x000080},
    {"oldlace",0xfdf5e6},{"olive",0x808000},{"olivedrab",0x6b8e23},{"orange",0xffa500},
    {"orangered",0xff4500},{"orchid",0xda70d6},{"palegoldenrod",0xeee8aa},{"palegreen",0x98fb98},
    {"paleturquoise",0xafeeee},{"palevioletred",0xdb7093},{"papayawhip",0xffefd5},
    {"peachpuff",0xffdab9},{"peru",0xcd853f},{"pink",0xffc0cb},{"plum",0xdda0dd},
    {"powderblue",0xb0e0e6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xff0000},
    {"rosybrown",0xbc8f8f},{"royalblue",0x4169e1},{"saddlebrown",0x8b4513},{"salmon",0xfa8072},
    {"sandybrown",0xf4a460},{"seagreen",0x2e8b57},{"seashell",0xfff5ee},{"sienna",0xa0522d},
    {"silver",0xc0c0c0},{"skyblue",0x87ceeb},{"slateblue",0x6a5acd},{"slategray",0x708090},
    {"slategrey",0x708090},{"snow",0xfffafa},{"springgreen",0x00ff7f},{"steelblue",0x4682b4},
    {"tan",0xd2b48c},{"teal",0x008080},{"thistle",0xd8bfd8},{"tomato",0xff6347},
    {"turquoise",0x40e0d0},{"violet",0xee82ee},{"wheat",0xf5deb3},{"white",0xffffff},
    {"whitesmoke",0xf5f5f5},{"yellow",0xffff00},{"yellowgreen",0x9acd32},
};

std::string trim_lower(const char* s) {
    std::string r(s);
    size_t a = r.find_first_not_of(" \t\n\r\f");
    size_t b = r.find_last_not_of(" \t\n\r\f");
    r = a == std::string::npos ? std::string() : r.substr(a, b - a + 1);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Channel list of a functional notation: "a, b, c[, d]" or "a b c[ / d]".
// Each value keeps its '%' flag.
struct FnArg { double v = 0; bool pct = false; };
bool parse_fn_args(const std::string& inner, std::vector<FnArg>& out) {
    std::string s = inner;
    for (char& c : s) if (c == ',' || c == '/') c = ' ';
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) return false;
        FnArg a;
        a.v = v;
        p = end;
        if (*p == '%') { a.pct = true; ++p; }
        else if (!std::strncmp(p, "deg", 3)) p += 3;
        out.push_back(a);
        if (*p && *p != ' ') return false;
    }
    return true;
}

double hue_to_rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

}  // namespace

bool c2d_parse_color(const char* cs, float rgba[4]) {
    if (!cs) return false;
    std::string s = trim_lower(cs);
    if (s.empty()) return false;
    if (s[0] == '#') {
        size_t n = s.size() - 1;
        if (n != 3 && n != 4 && n != 6 && n != 8) return false;
        int d[8] = {};
        for (size_t i = 0; i < n; ++i) {
            d[i] = hex_digit(s[i + 1]);
            if (d[i] < 0) return false;
        }
        if (n <= 4) {
            for (int i = 0; i < 3; ++i) rgba[i] = (float)(d[i] * 17) / 255.f;
            rgba[3] = n == 4 ? (float)(d[3] * 17) / 255.f : 1.f;
        } else {
            for (int i = 0; i < 3; ++i) rgba[i] = (float)(d[2 * i] * 16 + d[2 * i + 1]) / 255.f;
            rgba[3] = n == 8 ? (float)(d[6] * 16 + d[7]) / 255.f : 1.f;
        }
        return true;
    }
    size_t open = s.find('(');
    if (open != std::string::npos) {
        if (s.back() != ')') return false;
        std::string fn = s.substr(0, open);
        std::vector<FnArg> a;
        if (!parse_fn_args(s.substr(open + 1, s.size() - open - 2), a)) return false;
        if (a.size() != 3 && a.size() != 4) return false;
        double alpha = a.size() == 4 ? (a[3].pct ? a[3].v / 100 : a[3].v) : 1.0;
        if (fn == "rgb" || fn == "rgba") {
            for (int i = 0; i < 3; ++i)
                rgba[i] = (float)clamp01(a[i].pct ? a[i].v / 100 : a[i].v / 255);
        } else if (fn == "hsl" || fn == "hsla") {
            double h = std::fmod(a[0].v, 360.0);
            if (h < 0) h += 360;
            h /= 360;
            double sat = clamp01(a[1].v / 100), l = clamp01(a[2].v / 100);
            if (sat == 0) {
                rgba[0] = rgba[1] = rgba[2] = (float)l;
            } else {
                double q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat;
                double p = 2 * l - q;
                rgba[0] = (float)hue_to_rgb(p, q, h + 1.0 / 3);
                rgba[1] = (float)hue_to_rgb(p, q, h);
                rgba[2] = (float)hue_to_rgb(p, q, h - 1.0 / 3);
            }
        } else {
            return false;
        }
        rgba[3] = (float)clamp01(alpha);
        return true;
    }
    if (s == "transparent") {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.f;
        return true;
    }
    const NamedColor* end = kNamedColors + sizeof(kNamedColors) / sizeof(kNamedColors[0]);
    const NamedColor* it = std::lower_bound(kNamedColors, end, s,
        [](const NamedColor& c, const std::string& k) { return std::strcmp(c.name, k.c_str()) < 0; });
    if (it == end || s != it->name) return false;
    rgba[0] = (float)((it->rgb >> 16) & 0xff) / 255.f;
    rgba[1] = (float)((it->rgb >> 8) & 0xff) / 255.f;
    rgba[2] = (float)(it->rgb & 0xff) / 255.f;
    rgba[3] = 1.f;
    return true;
}

namespace {

// ── Fonts ────────────────────────────────────────────────────────────────

SkFontMgr* font_mgr() {
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
    return mgr.get();
}

sk_sp<SkTypeface> typeface_from_memory(const unsigned char* data, size_t size) {
    return font_mgr()->makeFromData(SkData::MakeWithoutCopy(data, size));
}

// Typefaces loaded from files, shared by every runtime (immutable).
sk_sp<SkTypeface> typeface_from_file(const std::string& path) {
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    sk_sp<SkTypeface> tf = font_mgr()->makeFromFile(path.c_str());
    if (tf) cache.emplace(path, tf);
    return tf;
}

struct Face {
    sk_sp<SkTypeface> tf;
    int weight = 400;
    bool italic = false;
};
using Families = std::unordered_map<std::string, std::vector<Face>>;

Face face_of(sk_sp<SkTypeface> tf) {
    Face f;
    SkFontStyle st = tf->fontStyle();
    f.weight = st.weight();
    f.italic = st.slant() != SkFontStyle::kUpright_Slant;
    f.tf = std::move(tf);
    return f;
}

const std::vector<Face>& builtin_inter() {
    static const std::vector<Face> faces = {
        face_of(typeface_from_memory(inter_regular_ttf, inter_regular_ttf_size)),
        face_of(typeface_from_memory(inter_bold_ttf, inter_bold_ttf_size)),
        face_of(typeface_from_memory(inter_black_ttf, inter_black_ttf_size)),
    };
    return faces;
}

const std::vector<Face>& builtin_mono() {
    static const std::vector<Face> faces = {
        face_of(typeface_from_memory(jetbrains_mono_regular_ttf, jetbrains_mono_regular_ttf_size)),
    };
    return faces;
}

// CSS font matching (weight + style) within one family.
const Face* match_face(const std::vector<Face>& faces, int weight, bool italic) {
    const Face* best = nullptr;
    long best_score = 0;
    for (const Face& f : faces) {
        long score = f.italic == italic ? 0 : 100000;
        int w = f.weight;
        if (weight >= 400 && weight <= 500) {
            if (w >= weight && w <= 500) score += w - weight;
            else if (w < weight) score += 1000 + (weight - w);
            else score += 2000 + (w - weight);
        } else if (weight < 400) {
            score += w <= weight ? weight - w : 1000 + (w - weight);
        } else {
            score += w >= weight ? w - weight : 1000 + (weight - w);
        }
        if (!best || score < best_score) { best = &f; best_score = score; }
    }
    return best;
}

struct FontSpec {
    sk_sp<SkTypeface> tf;
    float size = 10.f;
    bool embolden = false;
    bool synth_italic = false;
};

std::string unquote(std::string s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// "[style] [variant] [weight] [stretch] <size>[/<line-height>] <family>[, <family>…]"
bool parse_font(const Families& fam, const std::string& css, FontSpec& out, std::string& err) {
    const char* p = css.c_str();
    int weight = 400;
    bool italic = false;
    double size = -1;
    std::string rest;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* q = p;
        while (*q && *q != ' ' && *q != '\t') ++q;
        std::string tok(p, q);
        std::string low = trim_lower(tok.c_str());
        if (low == "normal" || low == "small-caps" || low == "ultra-condensed" ||
            low == "extra-condensed" || low == "condensed" || low == "semi-condensed" ||
            low == "semi-expanded" || low == "expanded" || low == "extra-expanded" ||
            low == "ultra-expanded") {
            p = q;
            continue;
        }
        if (low == "italic" || low == "oblique") { italic = true; p = q; continue; }
        if (low == "bold" || low == "bolder") { weight = 700; p = q; continue; }
        if (low == "lighter") { weight = 300; p = q; continue; }
        char* end = nullptr;
        double v = std::strtod(low.c_str(), &end);
        if (end != low.c_str() && *end == '\0') {
            if (v < 1 || v > 1000) { err = "font weight out of range"; return false; }
            weight = (int)v;
            p = q;
            continue;
        }
        if (end != low.c_str()) {
            std::string unit(end);
            size_t slash = unit.find('/');
            if (slash != std::string::npos) unit = unit.substr(0, slash);
            if (unit == "px") size = v;
            else if (unit == "pt") size = v * 4.0 / 3.0;
            else if (unit == "em" || unit == "rem") size = v * 10.0;
            else if (unit == "%") size = v * 0.1;
            else { err = "font size unit must be px, pt, em or %"; return false; }
            rest = q;
            break;
        }
        err = "unrecognised font token '" + tok + "'";
        return false;
    }
    if (size < 0) { err = "font needs a size, e.g. '48px Inter'"; return false; }
    if (!(size > 0)) { err = "font size must be positive"; return false; }
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= rest.size()) {
        size_t comma = rest.find(',', start);
        std::string name = unquote(rest.substr(start, comma == std::string::npos ? std::string::npos
                                                                                 : comma - start));
        if (!name.empty()) names.push_back(name);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (names.empty()) { err = "font needs a family"; return false; }
    for (const std::string& n : names) {
        auto it = fam.find(n);
        if (it == fam.end()) it = fam.find(trim_lower(n.c_str()));
        if (it == fam.end() || it->second.empty()) continue;
        const Face* f = match_face(it->second, weight, italic);
        out.tf = f->tf;
        out.size = (float)size;
        out.embolden = weight >= 600 && f->weight <= 500;
        out.synth_italic = italic && !f->italic;
        return true;
    }
    err = "font family '" + names.front() + "' is not registered (pms.font(family, path))";
    return false;
}

SkFont make_font(const FontSpec& fs) {
    SkFont f(fs.tf, fs.size);
    f.setSubpixel(true);
    f.setLinearMetrics(true);
    f.setHinting(SkFontHinting::kSlight);
    f.setEdging(SkFont::Edging::kAntiAlias);
    f.setEmbolden(fs.embolden);
    if (fs.synth_italic) f.setSkewX(-0.25f);
    return f;
}

// ── Text shaping (HarfBuzz via SkShaper) ─────────────────────────────────

SkShaper* shaper() {
    static std::unique_ptr<SkShaper> s =
        SkShapers::HB::ShapeDontWrapOrReorder(SkUnicodes::ICU::Make(), nullptr);
    return s.get();
}

struct Shaped {
    std::vector<SkGlyphID> glyphs;
    std::vector<SkPoint> pos;
    std::vector<uint32_t> clusters;
    float width = 0;
    SkRect bounds = SkRect::MakeEmpty();  // tight ink bounds, origin at the pen start
};

class CollectRuns final : public SkShaper::RunHandler {
public:
    explicit CollectRuns(Shaped& out) : out_(out) {}
    void beginLine() override {}
    void runInfo(const RunInfo&) override {}
    void commitRunInfo() override {}
    Buffer runBuffer(const RunInfo& ri) override {
        size_t n0 = out_.glyphs.size();
        out_.glyphs.resize(n0 + ri.glyphCount);
        out_.pos.resize(n0 + ri.glyphCount);
        out_.clusters.resize(n0 + ri.glyphCount);
        return {out_.glyphs.data() + n0, out_.pos.data() + n0, nullptr,
                out_.clusters.data() + n0, {x_, 0}};
    }
    void commitRunBuffer(const RunInfo& ri) override { x_ += ri.fAdvance.fX; }
    void commitLine() override {}
    float advance() const { return x_; }

private:
    Shaped& out_;
    float x_ = 0;
};

// Shape one line. Whitespace controls become spaces (spec). letterSpacing is
// added after every cluster, including the last (Blink).
Shaped shape_text(const SkFont& font, std::string text, float spacing) {
    for (char& c : text)
        if (c == '\t' || c == '\n' || c == '\f' || c == '\r') c = ' ';
    Shaped s;
    if (text.empty()) return s;
    CollectRuns runs(s);
    SkShaper::TrivialFontRunIterator fonts(font, text.size());
    SkShaper::TrivialBiDiRunIterator bidi(0, text.size());
    std::unique_ptr<SkShaper::ScriptRunIterator> script =
        SkShapers::HB::ScriptRunIterator(text.c_str(), text.size());
    SkShaper::TrivialLanguageRunIterator lang("en", text.size());
    shaper()->shape(text.c_str(), text.size(), fonts, bidi, *script, lang, nullptr, 0,
                    SK_ScalarInfinity, &runs);
    s.width = runs.advance();
    if (spacing != 0.f && !s.glyphs.empty()) {
        int k = 0;
        for (size_t i = 0; i < s.glyphs.size(); ++i) {
            if (i > 0 && s.clusters[i] != s.clusters[i - 1]) ++k;
            s.pos[i].fX += spacing * (float)k;
        }
        s.width += spacing * (float)(k + 1);
    }
    std::vector<SkRect> gb(s.glyphs.size());
    font.getBounds(s.glyphs, gb, nullptr);
    for (size_t i = 0; i < gb.size(); ++i) {
        if (gb[i].isEmpty()) continue;
        s.bounds.join(gb[i].makeOffset(s.pos[i]));
    }
    return s;
}

sk_sp<SkTextBlob> make_blob(const SkFont& font, const Shaped& s) {
    if (s.glyphs.empty()) return nullptr;
    SkTextBlobBuilder b;
    const auto& run = b.allocRunPos(font, (int)s.glyphs.size());
    std::memcpy(run.glyphs, s.glyphs.data(), s.glyphs.size() * sizeof(SkGlyphID));
    std::memcpy(run.points(), s.pos.data(), s.pos.size() * sizeof(SkPoint));
    return b.make();
}

// ── JS classes ───────────────────────────────────────────────────────────

JSClassID g_c2d_class = 0, g_grad_class = 0, g_img_class = 0, g_guard_class = 0;

struct Stop { float off; SkColor4f color; };

struct Gradient {
    enum Kind { Linear, Radial, Conic } kind = Linear;
    float x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0, angle = 0;
    std::vector<Stop> stops;  // sorted by offset; ties keep insertion order
    sk_sp<SkShader> shader;
    bool dirty = true;
};

struct Image {
    sk_sp<SkImage> image;
};

// fillStyle / strokeStyle: the value as assigned (string or gradient) plus
// the parsed colour for strings.
struct Style {
    JSValue js = JS_UNDEFINED;
    bool gradient = false;
    SkColor4f color = SkColors::kBlack;
};

enum Align { AlignStart, AlignEnd, AlignLeft, AlignRight, AlignCenter };
enum Baseline { BaseAlphabetic, BaseTop, BaseHanging, BaseMiddle, BaseIdeographic, BaseBottom };

struct DrawState {
    SkMatrix ctm;
    Style fill, stroke;
    float alpha = 1.f;
    int comp = 0;  // index into kComposite
    float line_width = 1.f;
    SkPaint::Cap cap = SkPaint::kButt_Cap;
    SkPaint::Join join = SkPaint::kMiter_Join;
    float miter = 10.f;
    std::vector<float> dash;
    float dash_offset = 0.f;
    bool smoothing = true;
    int smoothing_quality = 0;  // low / medium / high
    std::string font_css = "10px sans-serif";
    FontSpec font;
    int align = AlignStart;
    int baseline = BaseAlphabetic;
    float spacing = 0.f;
    std::string spacing_css = "0px";
    SkColor4f shadow_color = {0, 0, 0, 0};
    std::string shadow_css = "rgba(0, 0, 0, 0)";
    float shadow_blur = 0.f, shadow_dx = 0.f, shadow_dy = 0.f;
    float filter_blur = 0.f;  // CSS px (sigma); 0 = none
    std::string filter_css = "none";
};

struct C2D {
    SkCanvas* canvas = nullptr;
    SkMatrix base;              // logical canvas px → surface px
    int logical_w = 0, logical_h = 0;
    DrawState st;
    std::vector<DrawState> stack;
    SkPathBuilder path;  // device space
    Families families;
};

struct Composite { const char* name; SkBlendMode mode; bool unbounded; };
const Composite kComposite[] = {
    {"source-over", SkBlendMode::kSrcOver, false},
    {"source-in", SkBlendMode::kSrcIn, true},
    {"source-out", SkBlendMode::kSrcOut, true},
    {"source-atop", SkBlendMode::kSrcATop, false},
    {"destination-over", SkBlendMode::kDstOver, false},
    {"destination-in", SkBlendMode::kDstIn, true},
    {"destination-out", SkBlendMode::kDstOut, false},
    {"destination-atop", SkBlendMode::kDstATop, true},
    {"lighter", SkBlendMode::kPlus, false},
    {"copy", SkBlendMode::kSrc, true},
    {"xor", SkBlendMode::kXor, false},
    {"multiply", SkBlendMode::kMultiply, false},
    {"screen", SkBlendMode::kScreen, false},
    {"overlay", SkBlendMode::kOverlay, false},
    {"darken", SkBlendMode::kDarken, false},
    {"lighten", SkBlendMode::kLighten, false},
    {"color-dodge", SkBlendMode::kColorDodge, false},
    {"color-burn", SkBlendMode::kColorBurn, false},
    {"hard-light", SkBlendMode::kHardLight, false},
    {"soft-light", SkBlendMode::kSoftLight, false},
    {"difference", SkBlendMode::kDifference, false},
    {"exclusion", SkBlendMode::kExclusion, false},
    {"hue", SkBlendMode::kHue, false},
    {"saturation", SkBlendMode::kSaturation, false},
    {"color", SkBlendMode::kColor, false},
    {"luminosity", SkBlendMode::kLuminosity, false},
};
constexpr int kCompositeCount = (int)(sizeof(kComposite) / sizeof(kComposite[0]));

// ── Helpers ──────────────────────────────────────────────────────────────

C2D* c2d_of(JSContext* ctx, JSValueConst v) {
    C2D* c = (C2D*)JS_GetOpaque(v, g_c2d_class);
    if (!c) JS_ThrowTypeError(ctx, "not a Context2D");
    return c;
}

C2D* drawable(JSContext* ctx, JSValueConst v) {
    C2D* c = c2d_of(ctx, v);
    if (c && !c->canvas) {
        JS_ThrowTypeError(ctx, "pms.canvas can only draw inside render(f)");
        return nullptr;
    }
    return c;
}

// Convert the first n args to doubles. Missing args throw (browsers do).
bool args_num(JSContext* ctx, int argc, JSValueConst* argv, int n, double* out, const char* fn) {
    if (argc < n) {
        JS_ThrowTypeError(ctx, "%s: %d arguments required, %d given", fn, n, argc);
        return false;
    }
    for (int i = 0; i < n; ++i)
        if (JS_ToFloat64(ctx, &out[i], argv[i]) < 0) return false;
    return true;
}

bool all_finite(const double* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

std::string to_string(JSContext* ctx, JSValueConst v, bool* ok) {
    const char* s = JS_ToCString(ctx, v);
    if (!s) { *ok = false; return std::string(); }
    std::string r(s);
    JS_FreeCString(ctx, s);
    *ok = true;
    return r;
}

void state_dup(JSContext* ctx, DrawState& s) {
    s.fill.js = JS_DupValue(ctx, s.fill.js);
    s.stroke.js = JS_DupValue(ctx, s.stroke.js);
}

void state_free(JSRuntime* rt, DrawState& s) {
    JS_FreeValueRT(rt, s.fill.js);
    JS_FreeValueRT(rt, s.stroke.js);
    s.fill.js = s.stroke.js = JS_UNDEFINED;
}

void reset_state(JSContext* ctx, C2D* c) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    for (DrawState& s : c->stack) state_free(rt, s);
    c->stack.clear();
    state_free(rt, c->st);
    c->st = DrawState();
    c->st.fill.js = JS_NewString(ctx, "#000000");
    c->st.stroke.js = JS_NewString(ctx, "#000000");
    std::string err;
    parse_font(c->families, c->st.font_css, c->st.font, err);
    c->path.reset();
}

void apply_ctm(C2D* c) {
    if (c->canvas) c->canvas->setMatrix(SkMatrix::Concat(c->base, c->st.ctm));
}

// The device-space path mapped into the current user space.
bool user_path(C2D* c, SkPathFillType ft, SkPath& out) {
    SkMatrix inv;
    if (!c->st.ctm.invert(&inv)) return false;
    out = c->path.snapshot().makeTransform(inv);
    out.setFillType(ft);
    return true;
}

sk_sp<SkShader> gradient_shader(Gradient* g) {
    if (!g->dirty) return g->shader;
    g->dirty = false;
    g->shader = nullptr;
    if (g->stops.empty()) return nullptr;
    std::vector<SkColor4f> colors;
    std::vector<float> pos;
    for (const Stop& s : g->stops) { colors.push_back(s.color); pos.push_back(s.off); }
    if (colors.size() == 1) { colors.push_back(colors[0]); pos = {0.f, 1.f}; }
    SkGradient grad(SkGradient::Colors(SkSpan<const SkColor4f>(colors.data(), colors.size()),
                                       SkSpan<const float>(pos.data(), pos.size()),
                                       SkTileMode::kClamp),
                    SkGradient::Interpolation{});
    if (g->kind == Gradient::Linear) {
        if (g->x0 == g->x1 && g->y0 == g->y1) return nullptr;
        SkPoint pts[2] = {{g->x0, g->y0}, {g->x1, g->y1}};
        g->shader = SkShaders::LinearGradient(pts, grad);
    } else if (g->kind == Gradient::Radial) {
        if (g->x0 == g->x1 && g->y0 == g->y1 && g->r0 == g->r1) return nullptr;
        g->shader = SkShaders::TwoPointConicalGradient({g->x0, g->y0}, g->r0, {g->x1, g->y1},
                                                       g->r1, grad);
    } else {
        SkMatrix rot = SkMatrix::RotateDeg(g->angle, {g->x0, g->y0});
        g->shader = SkShaders::SweepGradient({g->x0, g->y0}, 0.f, 360.f, grad, &rot);
    }
    return g->shader;
}

// Paint for fill/stroke (style, globalAlpha, composite, line style, filter).
// Returns false when nothing should be drawn (gradient paints nothing).
bool make_paint(C2D* c, bool stroke, SkPaint& p) {
    const DrawState& st = c->st;
    const Style& style = stroke ? st.stroke : st.fill;
    p.setAntiAlias(true);
    if (style.gradient) {
        Gradient* g = (Gradient*)JS_GetOpaque(style.js, g_grad_class);
        sk_sp<SkShader> sh = g ? gradient_shader(g) : nullptr;
        if (!sh) p.setColor4f({0, 0, 0, 0});
        else { p.setShader(std::move(sh)); p.setAlphaf(st.alpha); }
    } else {
        SkColor4f col = style.color;
        col.fA *= st.alpha;
        p.setColor4f(col);
    }
    if (stroke) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(st.line_width);
        p.setStrokeCap(st.cap);
        p.setStrokeJoin(st.join);
        p.setStrokeMiter(st.miter);
        if (!st.dash.empty())
            p.setPathEffect(SkDashPathEffect::Make(
                SkSpan<const float>(st.dash.data(), st.dash.size()), st.dash_offset));
    }
    return true;
}

// Device-space → user-space scale for blur radii (shadows / filter are not
// affected by the transform).
float ctm_scale(const SkMatrix& m) {
    float det = m.getScaleX() * m.getScaleY() - m.getSkewX() * m.getSkewY();
    float s = std::sqrt(std::fabs(det));
    return s > 1e-6f ? s : 1.f;
}

// Draw with shadow / filter / composite semantics. `draw` issues the
// geometry with the paint it is given.
template <class F>
void paint_draw(C2D* c, SkPaint p, F&& draw) {
    const DrawState& st = c->st;
    SkCanvas* cv = c->canvas;
    float scale = ctm_scale(st.ctm);
    sk_sp<SkImageFilter> blur;
    if (st.filter_blur > 0.f)
        blur = SkImageFilters::Blur(st.filter_blur / scale, st.filter_blur / scale, nullptr);
    const Composite& comp = kComposite[st.comp];
    bool layered = comp.unbounded;
    if (layered) {
        SkPaint lp;
        lp.setBlendMode(comp.mode);
        cv->saveLayer(nullptr, &lp);
    } else {
        p.setBlendMode(comp.mode);
    }
    bool shadow = st.shadow_color.fA > 0.f &&
                  (st.shadow_blur > 0.f || st.shadow_dx != 0.f || st.shadow_dy != 0.f);
    if (shadow) {
        SkMatrix inv;
        if (st.ctm.invert(&inv)) {
            SkVector off = inv.mapVector(st.shadow_dx, st.shadow_dy);
            float sigma = st.shadow_blur / 2.f / scale;
            SkPaint sp = p;
            sp.setImageFilter(SkImageFilters::DropShadowOnly(off.fX, off.fY, sigma, sigma,
                                                             st.shadow_color, nullptr, blur));
            draw(sp);
        }
    }
    if (blur) p.setImageFilter(blur);
    draw(p);
    if (layered) cv->restore();
}

// ── Path construction (device space) ────────────────────────────────────

void path_move(C2D* c, double x, double y) {
    c->path.moveTo(c->st.ctm.mapPoint({(float)x, (float)y}));
}

void path_line(C2D* c, double x, double y) {
    SkPoint p = c->st.ctm.mapPoint({(float)x, (float)y});
    if (c->path.isEmpty()) c->path.moveTo(p);
    else c->path.lineTo(p);
}

void ensure_subpath(C2D* c, double x, double y) {
    if (c->path.isEmpty()) path_move(c, x, y);
}

// Append a user-space segment path (starting with its own moveTo) mapped by
// `local` then the CTM; connect to the current point with a line.
void append_segment(C2D* c, SkPathBuilder& seg, const SkMatrix& local) {
    SkMatrix m = c->st.ctm;
    m.preConcat(local);
    SkPath dev = seg.detach(&m);
    c->path.addPath(dev, c->path.isEmpty() ? SkPath::kAppend_AddPathMode
                                           : SkPath::kExtend_AddPathMode);
}

void add_ellipse(C2D* c, double x, double y, double rx, double ry, double rot,
                 double a0, double a1, bool ccw) {
    const double tau = 2 * M_PI;
    double end = a1;
    if (!ccw && a1 - a0 >= tau) end = a0 + tau;
    else if (ccw && a0 - a1 >= tau) end = a0 - tau;
    else if (!ccw && a0 > a1) end = a0 + (tau - std::fmod(a0 - a1, tau));
    else if (ccw && a0 < a1) end = a0 - (tau - std::fmod(a1 - a0, tau));
    double start_deg = a0 * 180.0 / M_PI;
    double sweep_deg = (end - a0) * 180.0 / M_PI;
    SkRect oval = SkRect::MakeLTRB((float)-rx, (float)-ry, (float)rx, (float)ry);
    SkPathBuilder seg;
    if (std::fabs(sweep_deg) >= 360.0 - 1e-9) {
        seg.arcTo(oval, (float)start_deg, (float)(sweep_deg / 2), true);
        seg.arcTo(oval, (float)(start_deg + sweep_deg / 2), (float)(sweep_deg / 2), false);
    } else {
        seg.arcTo(oval, (float)start_deg, (float)sweep_deg, true);
    }
    SkMatrix local = SkMatrix::Translate((float)x, (float)y);
    local.preRotate((float)(rot * 180.0 / M_PI));
    append_segment(c, seg, local);
}

// ── Context2D methods ────────────────────────────────────────────────────

#define C2D_FN(name) \
    JSValue name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)

C2D_FN(m_save) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    DrawState copy = c->st;
    state_dup(ctx, copy);
    c->stack.push_back(std::move(copy));
    if (c->canvas) c->canvas->save();
    return JS_UNDEFINED;
}

C2D_FN(m_restore) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (c->stack.empty()) return JS_UNDEFINED;
    state_free(JS_GetRuntime(ctx), c->st);
    c->st = std::move(c->stack.back());
    c->stack.pop_back();
    if (c->canvas) c->canvas->restore();
    return JS_UNDEFINED;
}

C2D_FN(m_translate) {
    C2D* c = c2d_of(ctx, this_val);
    double v[2];
    if (!c || !args_num(ctx, argc, argv, 2, v, "translate")) return JS_EXCEPTION;
    if (!all_finite(v, 2)) return JS_UNDEFINED;
    c->st.ctm.preTranslate((float)v[0], (float)v[1]);
    apply_ctm(c);
    return JS_UNDEFINED;
}

C2D_FN(m_scale) {
    C2D* c = c2d_of(ctx, this_val);
    double v[2];
    if (!c || !args_num(ctx, argc, argv, 2, v, "scale")) return JS_EXCEPTION;
    if (!all_finite(v, 2)) return JS_UNDEFINED;
    c->st.ctm.preScale((float)v[0], (float)v[1]);
    apply_ctm(c);
    return JS_UNDEFINED;
}

C2D_FN(m_rotate) {
    C2D* c = c2d_of(ctx, this_val);
    double v[1];
    if (!c || !args_num(ctx, argc, argv, 1, v, "rotate")) return JS_EXCEPTION;
    if (!all_finite(v, 1)) return JS_UNDEFINED;
    c->st.ctm.preRotate((float)(v[0] * 180.0 / M_PI));
    apply_ctm(c);
    return JS_UNDEFINED;
}

SkMatrix matrix_of(const double* v) {
    return SkMatrix::MakeAll((float)v[0], (float)v[2], (float)v[4],
                             (float)v[1], (float)v[3], (float)v[5], 0, 0, 1);
}

C2D_FN(m_transform) {
    C2D* c = c2d_of(ctx, this_val);
    double v[6];
    if (!c || !args_num(ctx, argc, argv, 6, v, "transform")) return JS_EXCEPTION;
    if (!all_finite(v, 6)) return JS_UNDEFINED;
    c->st.ctm.preConcat(matrix_of(v));
    apply_ctm(c);
    return JS_UNDEFINED;
}

C2D_FN(m_setTransform) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    double v[6];
    if (argc == 1 && JS_IsObject(argv[0])) {
        static const char* keys[6] = {"a", "b", "c", "d", "e", "f"};
        static const double defs[6] = {1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 6; ++i) {
            JSValue x = JS_GetPropertyStr(ctx, argv[0], keys[i]);
            if (JS_IsUndefined(x)) v[i] = defs[i];
            else if (JS_ToFloat64(ctx, &v[i], x) < 0) { JS_FreeValue(ctx, x); return JS_EXCEPTION; }
            JS_FreeValue(ctx, x);
        }
    } else if (argc == 0) {
        v[0] = v[3] = 1; v[1] = v[2] = v[4] = v[5] = 0;
    } else if (!args_num(ctx, argc, argv, 6, v, "setTransform")) {
        return JS_EXCEPTION;
    }
    if (!all_finite(v, 6)) return JS_UNDEFINED;
    c->st.ctm = matrix_of(v);
    apply_ctm(c);
    return JS_UNDEFINED;
}

C2D_FN(m_resetTransform) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    c->st.ctm.reset();
    apply_ctm(c);
    return JS_UNDEFINED;
}

C2D_FN(m_getTransform) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    const SkMatrix& m = c->st.ctm;
    JSValue o = JS_NewObject(ctx);
    const double vals[6] = {m.getScaleX(), m.getSkewY(), m.getSkewX(),
                            m.getScaleY(), m.getTranslateX(), m.getTranslateY()};
    static const char* keys[6] = {"a", "b", "c", "d", "e", "f"};
    for (int i = 0; i < 6; ++i)
        JS_DefinePropertyValueStr(ctx, o, keys[i], JS_NewFloat64(ctx, vals[i]), JS_PROP_C_W_E);
    return o;
}

C2D_FN(m_beginPath) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    c->path.reset();
    return JS_UNDEFINED;
}

C2D_FN(m_closePath) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (!c->path.isEmpty()) c->path.close();
    return JS_UNDEFINED;
}

C2D_FN(m_moveTo) {
    C2D* c = c2d_of(ctx, this_val);
    double v[2];
    if (!c || !args_num(ctx, argc, argv, 2, v, "moveTo")) return JS_EXCEPTION;
    if (all_finite(v, 2)) path_move(c, v[0], v[1]);
    return JS_UNDEFINED;
}

C2D_FN(m_lineTo) {
    C2D* c = c2d_of(ctx, this_val);
    double v[2];
    if (!c || !args_num(ctx, argc, argv, 2, v, "lineTo")) return JS_EXCEPTION;
    if (all_finite(v, 2)) path_line(c, v[0], v[1]);
    return JS_UNDEFINED;
}

C2D_FN(m_quadraticCurveTo) {
    C2D* c = c2d_of(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "quadraticCurveTo")) return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_UNDEFINED;
    ensure_subpath(c, v[0], v[1]);
    const SkMatrix& m = c->st.ctm;
    c->path.quadTo(m.mapPoint({(float)v[0], (float)v[1]}), m.mapPoint({(float)v[2], (float)v[3]}));
    return JS_UNDEFINED;
}

C2D_FN(m_bezierCurveTo) {
    C2D* c = c2d_of(ctx, this_val);
    double v[6];
    if (!c || !args_num(ctx, argc, argv, 6, v, "bezierCurveTo")) return JS_EXCEPTION;
    if (!all_finite(v, 6)) return JS_UNDEFINED;
    ensure_subpath(c, v[0], v[1]);
    const SkMatrix& m = c->st.ctm;
    c->path.cubicTo(m.mapPoint({(float)v[0], (float)v[1]}), m.mapPoint({(float)v[2], (float)v[3]}),
                    m.mapPoint({(float)v[4], (float)v[5]}));
    return JS_UNDEFINED;
}

C2D_FN(m_arcTo) {
    C2D* c = c2d_of(ctx, this_val);
    double v[5];
    if (!c || !args_num(ctx, argc, argv, 5, v, "arcTo")) return JS_EXCEPTION;
    if (!all_finite(v, 5)) return JS_UNDEFINED;
    if (v[4] < 0) return JS_ThrowRangeError(ctx, "arcTo: negative radius");
    ensure_subpath(c, v[0], v[1]);
    SkMatrix inv;
    if (!c->st.ctm.invert(&inv)) return JS_UNDEFINED;
    SkPoint cur = inv.mapPoint(*c->path.getLastPt());
    SkPathBuilder seg;
    seg.moveTo(cur);
    seg.arcTo({(float)v[0], (float)v[1]}, {(float)v[2], (float)v[3]}, (float)v[4]);
    append_segment(c, seg, SkMatrix::I());
    return JS_UNDEFINED;
}

C2D_FN(m_arc) {
    C2D* c = c2d_of(ctx, this_val);
    double v[5];
    if (!c || !args_num(ctx, argc, argv, 5, v, "arc")) return JS_EXCEPTION;
    bool ccw = argc > 5 && JS_ToBool(ctx, argv[5]) > 0;
    if (!all_finite(v, 5)) return JS_UNDEFINED;
    if (v[2] < 0) return JS_ThrowRangeError(ctx, "arc: negative radius");
    add_ellipse(c, v[0], v[1], v[2], v[2], 0, v[3], v[4], ccw);
    return JS_UNDEFINED;
}

C2D_FN(m_ellipse) {
    C2D* c = c2d_of(ctx, this_val);
    double v[7];
    if (!c || !args_num(ctx, argc, argv, 7, v, "ellipse")) return JS_EXCEPTION;
    bool ccw = argc > 7 && JS_ToBool(ctx, argv[7]) > 0;
    if (!all_finite(v, 7)) return JS_UNDEFINED;
    if (v[2] < 0 || v[3] < 0) return JS_ThrowRangeError(ctx, "ellipse: negative radius");
    add_ellipse(c, v[0], v[1], v[2], v[3], v[4], v[5], v[6], ccw);
    return JS_UNDEFINED;
}

C2D_FN(m_rect) {
    C2D* c = c2d_of(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "rect")) return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_UNDEFINED;
    const SkMatrix& m = c->st.ctm;
    float x = (float)v[0], y = (float)v[1], w = (float)v[2], h = (float)v[3];
    c->path.moveTo(m.mapPoint({x, y}));
    c->path.lineTo(m.mapPoint({x + w, y}));
    c->path.lineTo(m.mapPoint({x + w, y + h}));
    c->path.lineTo(m.mapPoint({x, y + h}));
    c->path.close();
    c->path.moveTo(m.mapPoint({x, y}));
    return JS_UNDEFINED;
}

// radii: number | number[1..4] | {x, y}[1..4] (CSS corner order).
bool parse_radii(JSContext* ctx, JSValueConst v, SkVector out[4]) {
    std::vector<SkVector> r;
    auto one = [&](JSValueConst x) -> bool {
        SkVector p;
        if (JS_IsObject(x) && !JS_IsArray(x)) {
            double a = 0, b = 0;
            JSValue ax = JS_GetPropertyStr(ctx, x, "x"), by = JS_GetPropertyStr(ctx, x, "y");
            bool ok = JS_ToFloat64(ctx, &a, ax) == 0 && JS_ToFloat64(ctx, &b, by) == 0;
            JS_FreeValue(ctx, ax);
            JS_FreeValue(ctx, by);
            if (!ok) return false;
            p = {(float)a, (float)b};
        } else {
            double a = 0;
            if (JS_ToFloat64(ctx, &a, x) < 0) return false;
            p = {(float)a, (float)a};
        }
        if (p.fX < 0 || p.fY < 0) { JS_ThrowRangeError(ctx, "roundRect: negative radius"); return false; }
        r.push_back(p);
        return true;
    };
    if (JS_IsArray(v)) {
        int64_t n = 0;
        if (JS_GetLength(ctx, v, &n) < 0) return false;
        if (n < 1 || n > 4) { JS_ThrowRangeError(ctx, "roundRect: 1 to 4 radii"); return false; }
        for (int64_t i = 0; i < n; ++i) {
            JSValue x = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            bool ok = one(x);
            JS_FreeValue(ctx, x);
            if (!ok) return false;
        }
    } else if (!one(v)) {
        return false;
    }
    // CSS order → Skia (UL, UR, LR, LL).
    switch (r.size()) {
        case 1: out[0] = out[1] = out[2] = out[3] = r[0]; break;
        case 2: out[0] = out[2] = r[0]; out[1] = out[3] = r[1]; break;
        case 3: out[0] = r[0]; out[1] = out[3] = r[1]; out[2] = r[2]; break;
        default: out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3]; break;
    }
    return true;
}

C2D_FN(m_roundRect) {
    C2D* c = c2d_of(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "roundRect")) return JS_EXCEPTION;
    SkVector radii[4] = {};
    if (argc > 4 && !JS_IsUndefined(argv[4]) && !parse_radii(ctx, argv[4], radii))
        return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_UNDEFINED;
    SkRRect rr;
    rr.setRectRadii(SkRect::MakeXYWH((float)v[0], (float)v[1], (float)v[2], (float)v[3]).makeSorted(),
                    radii);
    SkPathBuilder seg;
    seg.addRRect(rr);
    append_segment(c, seg, SkMatrix::I());
    path_move(c, v[0], v[1]);
    return JS_UNDEFINED;
}

bool fill_rule(JSContext* ctx, int argc, JSValueConst* argv, SkPathFillType& ft) {
    ft = SkPathFillType::kWinding;
    if (argc < 1 || JS_IsUndefined(argv[0])) return true;
    bool ok = false;
    std::string r = to_string(ctx, argv[0], &ok);
    if (!ok) return false;
    if (r == "evenodd") ft = SkPathFillType::kEvenOdd;
    else if (r != "nonzero") {
        JS_ThrowTypeError(ctx, "fill rule must be 'nonzero' or 'evenodd'");
        return false;
    }
    return true;
}

C2D_FN(m_fill) {
    C2D* c = drawable(ctx, this_val);
    SkPathFillType ft;
    if (!c || !fill_rule(ctx, argc, argv, ft)) return JS_EXCEPTION;
    SkPath p;
    SkPaint paint;
    if (!user_path(c, ft, p) || !make_paint(c, false, paint)) return JS_UNDEFINED;
    paint_draw(c, paint, [&](const SkPaint& pp) { c->canvas->drawPath(p, pp); });
    return JS_UNDEFINED;
}

C2D_FN(m_stroke) {
    C2D* c = drawable(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    SkPath p;
    SkPaint paint;
    if (!user_path(c, SkPathFillType::kWinding, p) || !make_paint(c, true, paint))
        return JS_UNDEFINED;
    paint_draw(c, paint, [&](const SkPaint& pp) { c->canvas->drawPath(p, pp); });
    return JS_UNDEFINED;
}

C2D_FN(m_clip) {
    C2D* c = drawable(ctx, this_val);
    SkPathFillType ft;
    if (!c || !fill_rule(ctx, argc, argv, ft)) return JS_EXCEPTION;
    SkPath p;
    if (!user_path(c, ft, p)) {
        c->canvas->clipRect(SkRect::MakeEmpty());
        return JS_UNDEFINED;
    }
    c->canvas->clipPath(p, SkClipOp::kIntersect, true);
    return JS_UNDEFINED;
}

C2D_FN(m_isPointInPath) {
    C2D* c = c2d_of(ctx, this_val);
    double v[2];
    if (!c || !args_num(ctx, argc, argv, 2, v, "isPointInPath")) return JS_EXCEPTION;
    SkPathFillType ft;
    if (!fill_rule(ctx, argc - 2, argv + 2, ft)) return JS_EXCEPTION;
    if (!all_finite(v, 2)) return JS_FALSE;
    SkPath p = c->path.snapshot();
    p.setFillType(ft);
    return JS_NewBool(ctx, p.contains((float)v[0], (float)v[1]));
}

C2D_FN(m_fillRect) {
    C2D* c = drawable(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "fillRect")) return JS_EXCEPTION;
    if (!all_finite(v, 4) || v[2] == 0 || v[3] == 0) return JS_UNDEFINED;
    SkPaint paint;
    if (!make_paint(c, false, paint)) return JS_UNDEFINED;
    SkRect r = SkRect::MakeXYWH((float)v[0], (float)v[1], (float)v[2], (float)v[3]).makeSorted();
    paint_draw(c, paint, [&](const SkPaint& pp) { c->canvas->drawRect(r, pp); });
    return JS_UNDEFINED;
}

C2D_FN(m_strokeRect) {
    C2D* c = drawable(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "strokeRect")) return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_UNDEFINED;
    SkPaint paint;
    if (!make_paint(c, true, paint)) return JS_UNDEFINED;
    float x = (float)v[0], y = (float)v[1], w = (float)v[2], h = (float)v[3];
    SkPathBuilder b;
    if (w == 0 || h == 0) {
        b.moveTo(x, y).lineTo(x + w, y + h);
    } else {
        b.moveTo(x, y).lineTo(x + w, y).lineTo(x + w, y + h).lineTo(x, y + h).close();
    }
    SkPath p = b.detach();
    paint_draw(c, paint, [&](const SkPaint& pp) { c->canvas->drawPath(p, pp); });
    return JS_UNDEFINED;
}

C2D_FN(m_clearRect) {
    C2D* c = drawable(ctx, this_val);
    double v[4];
    if (!c || !args_num(ctx, argc, argv, 4, v, "clearRect")) return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_UNDEFINED;
    SkPaint p;
    p.setAntiAlias(true);
    p.setBlendMode(SkBlendMode::kClear);
    c->canvas->drawRect(
        SkRect::MakeXYWH((float)v[0], (float)v[1], (float)v[2], (float)v[3]).makeSorted(), p);
    return JS_UNDEFINED;
}

SkSamplingOptions sampling_of(const DrawState& st) {
    if (!st.smoothing) return SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
    if (st.smoothing_quality == 0) return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
    if (st.smoothing_quality == 1) return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest);
    return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
}

C2D_FN(m_drawImage) {
    C2D* c = drawable(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (argc != 3 && argc != 5 && argc != 9)
        return JS_ThrowTypeError(ctx, "drawImage takes 3, 5 or 9 arguments, %d given", argc);
    Image* im = (Image*)JS_GetOpaque(argv[0], g_img_class);
    if (!im || !im->image) return JS_ThrowTypeError(ctx, "drawImage: source must be a pms.image()");
    double v[8];
    if (!args_num(ctx, argc - 1, argv + 1, argc - 1, v, "drawImage")) return JS_EXCEPTION;
    float iw = (float)im->image->width(), ih = (float)im->image->height();
    double sx = 0, sy = 0, sw = iw, sh = ih, dx, dy, dw, dh;
    if (argc == 3) { dx = v[0]; dy = v[1]; dw = iw; dh = ih; }
    else if (argc == 5) { dx = v[0]; dy = v[1]; dw = v[2]; dh = v[3]; }
    else { sx = v[0]; sy = v[1]; sw = v[2]; sh = v[3]; dx = v[4]; dy = v[5]; dw = v[6]; dh = v[7]; }
    double all[8] = {sx, sy, sw, sh, dx, dy, dw, dh};
    if (!all_finite(all, 8)) return JS_UNDEFINED;
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return JS_UNDEFINED;
    // Clip the source rect to the image, shrinking the destination to match.
    double kx = dw / sw, ky = dh / sh;
    if (sx < 0) { dx -= sx * kx; dw += sx * kx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy * ky; dh += sy * ky; sh += sy; sy = 0; }
    if (sx + sw > iw) { double o = sx + sw - iw; sw -= o; dw -= o * kx; }
    if (sy + sh > ih) { double o = sy + sh - ih; sh -= o; dh -= o * ky; }
    if (sw <= 0 || sh <= 0) return JS_UNDEFINED;
    SkRect src = SkRect::MakeXYWH((float)sx, (float)sy, (float)sw, (float)sh);
    SkRect dst = SkRect::MakeXYWH((float)dx, (float)dy, (float)dw, (float)dh);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setAlphaf(c->st.alpha);
    SkSamplingOptions samp = sampling_of(c->st);
    paint_draw(c, paint, [&](const SkPaint& pp) {
        c->canvas->drawImageRect(im->image.get(), src, dst, samp, &pp,
                                 SkCanvas::kStrict_SrcRectConstraint);
    });
    return JS_UNDEFINED;
}

// ── Text ─────────────────────────────────────────────────────────────────

struct TextLayout {
    Shaped shaped;
    float ax = 0, ay = 0;  // alignment offsets applied to the pen origin
    SkFontMetrics metrics{};
    float em_ascent = 0, em_descent = 0;
};

TextLayout layout_text(C2D* c, const std::string& s) {
    TextLayout tl;
    SkFont font = make_font(c->st.font);
    tl.shaped = shape_text(font, s, c->st.spacing);
    font.getMetrics(&tl.metrics);
    float asc = -tl.metrics.fAscent, desc = tl.metrics.fDescent;
    float em = c->st.font.size;
    tl.em_ascent = asc + desc > 0 ? asc * em / (asc + desc) : em;
    tl.em_descent = em - tl.em_ascent;
    float w = tl.shaped.width;
    switch (c->st.align) {
        case AlignCenter: tl.ax = -w / 2; break;
        case AlignRight: case AlignEnd: tl.ax = -w; break;
        default: tl.ax = 0; break;
    }
    switch (c->st.baseline) {
        case BaseTop: tl.ay = tl.em_ascent; break;
        case BaseHanging: tl.ay = asc * 0.8f; break;
        case BaseMiddle: tl.ay = (tl.em_ascent - tl.em_descent) / 2; break;
        case BaseIdeographic: tl.ay = -desc; break;
        case BaseBottom: tl.ay = -tl.em_descent; break;
        default: tl.ay = 0; break;
    }
    return tl;
}

JSValue draw_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, bool stroke) {
    C2D* c = drawable(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (argc < 3) return JS_ThrowTypeError(ctx, "%s needs (text, x, y)", stroke ? "strokeText" : "fillText");
    bool ok = false;
    std::string s = to_string(ctx, argv[0], &ok);
    if (!ok) return JS_EXCEPTION;
    double v[3] = {0, 0, INFINITY};
    if (!args_num(ctx, 2, argv + 1, 2, v, "fillText")) return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &v[2], argv[3]) < 0)
        return JS_EXCEPTION;
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || std::isnan(v[2]) || v[2] <= 0)
        return JS_UNDEFINED;
    TextLayout tl = layout_text(c, s);
    SkFont font = make_font(c->st.font);
    sk_sp<SkTextBlob> blob = make_blob(font, tl.shaped);
    if (!blob) return JS_UNDEFINED;
    SkPaint paint;
    if (!make_paint(c, stroke, paint)) return JS_UNDEFINED;
    float sx = 1.f;
    if (std::isfinite(v[2]) && tl.shaped.width > v[2]) sx = (float)v[2] / tl.shaped.width;
    SkCanvas* cv = c->canvas;
    if (sx != 1.f) {
        cv->save();
        cv->translate((float)v[0], (float)v[1]);
        cv->scale(sx, 1.f);
        cv->translate((float)-v[0], (float)-v[1]);
    }
    float x = (float)v[0] + tl.ax, y = (float)v[1] + tl.ay;
    paint_draw(c, paint, [&](const SkPaint& pp) { cv->drawTextBlob(blob, x, y, pp); });
    if (sx != 1.f) cv->restore();
    return JS_UNDEFINED;
}

C2D_FN(m_fillText) { return draw_text(ctx, this_val, argc, argv, false); }
C2D_FN(m_strokeText) { return draw_text(ctx, this_val, argc, argv, true); }

C2D_FN(m_measureText) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "measureText needs (text)");
    bool ok = false;
    std::string s = to_string(ctx, argv[0], &ok);
    if (!ok) return JS_EXCEPTION;
    TextLayout tl = layout_text(c, s);
    const SkRect& b = tl.shaped.bounds;
    float asc = -tl.metrics.fAscent, desc = tl.metrics.fDescent;
    bool empty = b.isEmpty();
    const struct { const char* k; double v; } fields[] = {
        {"width", tl.shaped.width},
        {"actualBoundingBoxLeft", empty ? 0.0 : -(b.fLeft + tl.ax)},
        {"actualBoundingBoxRight", empty ? 0.0 : b.fRight + tl.ax},
        {"actualBoundingBoxAscent", empty ? 0.0 : -(b.fTop + tl.ay)},
        {"actualBoundingBoxDescent", empty ? 0.0 : b.fBottom + tl.ay},
        {"fontBoundingBoxAscent", asc - tl.ay},
        {"fontBoundingBoxDescent", desc + tl.ay},
        {"emHeightAscent", tl.em_ascent - tl.ay},
        {"emHeightDescent", tl.em_descent + tl.ay},
        {"alphabeticBaseline", -tl.ay},
        {"hangingBaseline", asc * 0.8 - tl.ay},
        {"ideographicBaseline", -desc - tl.ay},
    };
    JSValue o = JS_NewObject(ctx);
    for (const auto& f : fields)
        JS_DefinePropertyValueStr(ctx, o, f.k, JS_NewFloat64(ctx, f.v), JS_PROP_C_W_E);
    return o;
}

// ── Gradients ────────────────────────────────────────────────────────────

JSValue new_gradient(JSContext* ctx, Gradient* g) {
    JSValue o = JS_NewObjectClass(ctx, (int)g_grad_class);
    if (JS_IsException(o)) { delete g; return o; }
    JS_SetOpaque(o, g);
    return o;
}

C2D_FN(m_createLinearGradient) {
    double v[4];
    if (!c2d_of(ctx, this_val) || !args_num(ctx, argc, argv, 4, v, "createLinearGradient"))
        return JS_EXCEPTION;
    if (!all_finite(v, 4)) return JS_ThrowTypeError(ctx, "createLinearGradient: non-finite argument");
    Gradient* g = new Gradient();
    g->kind = Gradient::Linear;
    g->x0 = (float)v[0]; g->y0 = (float)v[1]; g->x1 = (float)v[2]; g->y1 = (float)v[3];
    return new_gradient(ctx, g);
}

C2D_FN(m_createRadialGradient) {
    double v[6];
    if (!c2d_of(ctx, this_val) || !args_num(ctx, argc, argv, 6, v, "createRadialGradient"))
        return JS_EXCEPTION;
    if (!all_finite(v, 6)) return JS_ThrowTypeError(ctx, "createRadialGradient: non-finite argument");
    if (v[2] < 0 || v[5] < 0) return JS_ThrowRangeError(ctx, "createRadialGradient: negative radius");
    Gradient* g = new Gradient();
    g->kind = Gradient::Radial;
    g->x0 = (float)v[0]; g->y0 = (float)v[1]; g->r0 = (float)v[2];
    g->x1 = (float)v[3]; g->y1 = (float)v[4]; g->r1 = (float)v[5];
    return new_gradient(ctx, g);
}

C2D_FN(m_createConicGradient) {
    double v[3];
    if (!c2d_of(ctx, this_val) || !args_num(ctx, argc, argv, 3, v, "createConicGradient"))
        return JS_EXCEPTION;
    if (!all_finite(v, 3)) return JS_ThrowTypeError(ctx, "createConicGradient: non-finite argument");
    Gradient* g = new Gradient();
    g->kind = Gradient::Conic;
    g->angle = (float)(v[0] * 180.0 / M_PI);
    g->x0 = (float)v[1]; g->y0 = (float)v[2];
    return new_gradient(ctx, g);
}

JSValue g_addColorStop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Gradient* g = (Gradient*)JS_GetOpaque(this_val, g_grad_class);
    if (!g) return JS_ThrowTypeError(ctx, "not a CanvasGradient");
    if (argc < 2) return JS_ThrowTypeError(ctx, "addColorStop needs (offset, colour)");
    double off = 0;
    if (JS_ToFloat64(ctx, &off, argv[0]) < 0) return JS_EXCEPTION;
    if (!(off >= 0 && off <= 1)) return JS_ThrowRangeError(ctx, "addColorStop: offset outside 0..1");
    bool ok = false;
    std::string cs = to_string(ctx, argv[1], &ok);
    if (!ok) return JS_EXCEPTION;
    float rgba[4];
    if (!c2d_parse_color(cs.c_str(), rgba))
        return JS_ThrowTypeError(ctx, "addColorStop: invalid colour '%s'", cs.c_str());
    Stop st{(float)off, {rgba[0], rgba[1], rgba[2], rgba[3]}};
    auto it = std::upper_bound(g->stops.begin(), g->stops.end(), st.off,
                               [](float o, const Stop& s) { return o < s.off; });
    g->stops.insert(it, st);
    g->dirty = true;
    return JS_UNDEFINED;
}

// ── Accessors ────────────────────────────────────────────────────────────

enum Prop {
    P_fillStyle, P_strokeStyle, P_globalAlpha, P_globalCompositeOperation, P_lineWidth,
    P_lineCap, P_lineJoin, P_miterLimit, P_lineDashOffset, P_imageSmoothingEnabled,
    P_imageSmoothingQuality, P_font, P_textAlign, P_textBaseline, P_letterSpacing,
    P_shadowColor, P_shadowBlur, P_shadowOffsetX, P_shadowOffsetY, P_filter, P_canvas,
};

const char* kAlignNames[] = {"start", "end", "left", "right", "center"};
const char* kBaselineNames[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
const char* kCapNames[] = {"butt", "round", "square"};
const char* kJoinNames[] = {"miter", "round", "bevel"};
const char* kQualityNames[] = {"low", "medium", "high"};

int index_of(const char* const* names, int n, const std::string& v) {
    for (int i = 0; i < n; ++i)
        if (v == names[i]) return i;
    return -1;
}

JSValue prop_get(JSContext* ctx, JSValueConst this_val, int magic) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    const DrawState& st = c->st;
    switch (magic) {
        case P_fillStyle: return JS_DupValue(ctx, st.fill.js);
        case P_strokeStyle: return JS_DupValue(ctx, st.stroke.js);
        case P_globalAlpha: return JS_NewFloat64(ctx, st.alpha);
        case P_globalCompositeOperation: return JS_NewString(ctx, kComposite[st.comp].name);
        case P_lineWidth: return JS_NewFloat64(ctx, st.line_width);
        case P_lineCap: return JS_NewString(ctx, kCapNames[(int)st.cap]);
        case P_lineJoin: return JS_NewString(ctx, kJoinNames[(int)st.join]);
        case P_miterLimit: return JS_NewFloat64(ctx, st.miter);
        case P_lineDashOffset: return JS_NewFloat64(ctx, st.dash_offset);
        case P_imageSmoothingEnabled: return JS_NewBool(ctx, st.smoothing);
        case P_imageSmoothingQuality: return JS_NewString(ctx, kQualityNames[st.smoothing_quality]);
        case P_font: return JS_NewString(ctx, st.font_css.c_str());
        case P_textAlign: return JS_NewString(ctx, kAlignNames[st.align]);
        case P_textBaseline: return JS_NewString(ctx, kBaselineNames[st.baseline]);
        case P_letterSpacing: return JS_NewString(ctx, st.spacing_css.c_str());
        case P_shadowColor: return JS_NewString(ctx, st.shadow_css.c_str());
        case P_shadowBlur: return JS_NewFloat64(ctx, st.shadow_blur);
        case P_shadowOffsetX: return JS_NewFloat64(ctx, st.shadow_dx);
        case P_shadowOffsetY: return JS_NewFloat64(ctx, st.shadow_dy);
        case P_filter: return JS_NewString(ctx, st.filter_css.c_str());
        case P_canvas: {
            JSValue o = JS_NewObject(ctx);
            int w = c->logical_w, h = c->logical_h;
            JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, w), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, h), JS_PROP_C_W_E);
            return o;
        }
    }
    return JS_UNDEFINED;
}

bool set_number(JSContext* ctx, JSValueConst val, float& out, bool (*valid)(double)) {
    double d = 0;
    if (JS_ToFloat64(ctx, &d, val) < 0) return false;
    if (valid(d)) out = (float)d;
    return true;
}

JSValue prop_set(JSContext* ctx, JSValueConst this_val, JSValueConst val, int magic) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    DrawState& st = c->st;
    auto finite = [](double d) { return std::isfinite(d); };
    auto positive = [](double d) { return std::isfinite(d) && d > 0; };
    auto non_negative = [](double d) { return std::isfinite(d) && d >= 0; };
    auto enum_str = [&](const char* const* names, int n, const char* what, int& out) -> bool {
        bool ok = false;
        std::string s = to_string(ctx, val, &ok);
        if (!ok) return false;
        int i = index_of(names, n, s);
        if (i < 0) { JS_ThrowTypeError(ctx, "invalid %s '%s'", what, s.c_str()); return false; }
        out = i;
        return true;
    };
    switch (magic) {
        case P_fillStyle:
        case P_strokeStyle: {
            Style& style = magic == P_fillStyle ? st.fill : st.stroke;
            if (JS_GetOpaque(val, g_grad_class)) {
                JS_FreeValue(ctx, style.js);
                style.js = JS_DupValue(ctx, val);
                style.gradient = true;
                return JS_UNDEFINED;
            }
            if (!JS_IsString(val))
                return JS_ThrowTypeError(ctx, "fillStyle/strokeStyle must be a CSS colour or CanvasGradient");
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            float rgba[4];
            if (!c2d_parse_color(s.c_str(), rgba))
                return JS_ThrowTypeError(ctx, "invalid colour '%s'", s.c_str());
            JS_FreeValue(ctx, style.js);
            style.js = JS_DupValue(ctx, val);
            style.gradient = false;
            style.color = {rgba[0], rgba[1], rgba[2], rgba[3]};
            return JS_UNDEFINED;
        }
        case P_globalAlpha: {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, val) < 0) return JS_EXCEPTION;
            if (d >= 0 && d <= 1) st.alpha = (float)d;
            return JS_UNDEFINED;
        }
        case P_globalCompositeOperation: {
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            for (int i = 0; i < kCompositeCount; ++i)
                if (s == kComposite[i].name) { st.comp = i; return JS_UNDEFINED; }
            return JS_ThrowTypeError(ctx, "unsupported globalCompositeOperation '%s'", s.c_str());
        }
        case P_lineWidth:
            return set_number(ctx, val, st.line_width, positive) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_miterLimit:
            return set_number(ctx, val, st.miter, positive) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_lineDashOffset:
            return set_number(ctx, val, st.dash_offset, finite) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_lineCap: {
            int i = 0;
            if (!enum_str(kCapNames, 3, "lineCap", i)) return JS_EXCEPTION;
            st.cap = (SkPaint::Cap)i;
            return JS_UNDEFINED;
        }
        case P_lineJoin: {
            int i = 0;
            if (!enum_str(kJoinNames, 3, "lineJoin", i)) return JS_EXCEPTION;
            st.join = (SkPaint::Join)i;
            return JS_UNDEFINED;
        }
        case P_imageSmoothingEnabled: {
            int b = JS_ToBool(ctx, val);
            if (b < 0) return JS_EXCEPTION;
            st.smoothing = b != 0;
            return JS_UNDEFINED;
        }
        case P_imageSmoothingQuality:
            return enum_str(kQualityNames, 3, "imageSmoothingQuality", st.smoothing_quality)
                       ? JS_UNDEFINED : JS_EXCEPTION;
        case P_font: {
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            FontSpec fs;
            std::string err;
            if (!parse_font(c->families, s, fs, err))
                return JS_ThrowTypeError(ctx, "font '%s': %s", s.c_str(), err.c_str());
            st.font = fs;
            st.font_css = s;
            return JS_UNDEFINED;
        }
        case P_textAlign:
            return enum_str(kAlignNames, 5, "textAlign", st.align) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_textBaseline:
            return enum_str(kBaselineNames, 6, "textBaseline", st.baseline) ? JS_UNDEFINED
                                                                            : JS_EXCEPTION;
        case P_letterSpacing: {
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            std::string low = trim_lower(s.c_str());
            char* end = nullptr;
            double v = std::strtod(low.c_str(), &end);
            std::string unit = end ? std::string(end) : std::string();
            double px;
            if (end == low.c_str()) px = NAN;
            else if (unit == "px" || (unit.empty() && v == 0)) px = v;
            else if (unit == "em") px = v * st.font.size;
            else if (unit == "pt") px = v * 4.0 / 3.0;
            else px = NAN;
            if (!std::isfinite(px))
                return JS_ThrowTypeError(ctx, "letterSpacing '%s': use '<n>px' or '<n>em'", s.c_str());
            st.spacing = (float)px;
            st.spacing_css = s;
            return JS_UNDEFINED;
        }
        case P_shadowColor: {
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            float rgba[4];
            if (!c2d_parse_color(s.c_str(), rgba))
                return JS_ThrowTypeError(ctx, "invalid shadowColor '%s'", s.c_str());
            st.shadow_color = {rgba[0], rgba[1], rgba[2], rgba[3]};
            st.shadow_css = s;
            return JS_UNDEFINED;
        }
        case P_shadowBlur:
            return set_number(ctx, val, st.shadow_blur, non_negative) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_shadowOffsetX:
            return set_number(ctx, val, st.shadow_dx, finite) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_shadowOffsetY:
            return set_number(ctx, val, st.shadow_dy, finite) ? JS_UNDEFINED : JS_EXCEPTION;
        case P_filter: {
            bool ok = false;
            std::string s = to_string(ctx, val, &ok);
            if (!ok) return JS_EXCEPTION;
            std::string low = trim_lower(s.c_str());
            if (low == "none") { st.filter_blur = 0; st.filter_css = s; return JS_UNDEFINED; }
            double px = 0;
            char tail[8] = {};
            if (std::sscanf(low.c_str(), "blur(%lfpx%7s", &px, tail) == 2 && std::string(tail) == ")" &&
                px >= 0) {
                st.filter_blur = (float)px;
                st.filter_css = s;
                return JS_UNDEFINED;
            }
            return JS_ThrowTypeError(ctx, "filter '%s': supported values are 'none' and 'blur(<n>px)'",
                                     s.c_str());
        }
        case P_canvas:
            return JS_ThrowTypeError(ctx, "canvas is read-only");
    }
    return JS_UNDEFINED;
}

C2D_FN(m_setLineDash) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx, "setLineDash needs an array");
    int64_t n = 0;
    if (JS_GetLength(ctx, argv[0], &n) < 0) return JS_EXCEPTION;
    std::vector<float> d;
    for (int64_t i = 0; i < n; ++i) {
        JSValue x = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        double v = 0;
        int r = JS_ToFloat64(ctx, &v, x);
        JS_FreeValue(ctx, x);
        if (r < 0) return JS_EXCEPTION;
        if (!std::isfinite(v) || v < 0) return JS_UNDEFINED;  // spec: ignore
        d.push_back((float)v);
    }
    if (d.size() % 2) d.insert(d.end(), d.begin(), d.end());
    bool all_zero = std::all_of(d.begin(), d.end(), [](float x) { return x == 0; });
    c->st.dash = all_zero ? std::vector<float>() : d;
    return JS_UNDEFINED;
}

C2D_FN(m_getLineDash) {
    C2D* c = c2d_of(ctx, this_val);
    if (!c) return JS_EXCEPTION;
    JSValue a = JS_NewArray(ctx);
    for (size_t i = 0; i < c->st.dash.size(); ++i)
        JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i, JS_NewFloat64(ctx, c->st.dash[i]),
                                     JS_PROP_C_W_E);
    return a;
}

// ── Class plumbing ───────────────────────────────────────────────────────

void c2d_finalizer(JSRuntime* rt, JSValueConst val) {
    C2D* c = (C2D*)JS_GetOpaque(val, g_c2d_class);
    if (!c) return;
    for (DrawState& s : c->stack) state_free(rt, s);
    state_free(rt, c->st);
    delete c;
}

void c2d_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark) {
    C2D* c = (C2D*)JS_GetOpaque(val, g_c2d_class);
    if (!c) return;
    JS_MarkValue(rt, c->st.fill.js, mark);
    JS_MarkValue(rt, c->st.stroke.js, mark);
    for (const DrawState& s : c->stack) {
        JS_MarkValue(rt, s.fill.js, mark);
        JS_MarkValue(rt, s.stroke.js, mark);
    }
}

void grad_finalizer(JSRuntime*, JSValueConst val) {
    delete (Gradient*)JS_GetOpaque(val, g_grad_class);
}

void img_finalizer(JSRuntime*, JSValueConst val) {
    delete (Image*)JS_GetOpaque(val, g_img_class);
}

JSValue img_get(JSContext* ctx, JSValueConst this_val, int magic) {
    Image* im = (Image*)JS_GetOpaque(this_val, g_img_class);
    if (!im) return JS_ThrowTypeError(ctx, "not an Image");
    return JS_NewInt32(ctx, magic == 0 ? im->image->width() : im->image->height());
}

bool atom_is_symbol(JSContext* ctx, JSAtom atom) {
    JSValue v = JS_AtomToValue(ctx, atom);
    bool sym = JS_IsSymbol(v);
    JS_FreeValue(ctx, v);
    return sym;
}

// Names the engine itself probes (promise resolution, JSON, primitive
// conversion) resolve to undefined instead of throwing.
bool engine_probe(const char* name) {
    static const char* probes[] = {"then", "toJSON", "toString", "valueOf", "constructor",
                                   "inspect", nullptr};
    for (int i = 0; probes[i]; ++i)
        if (!std::strcmp(name, probes[i])) return true;
    return false;
}

JSValue guard_get(JSContext* ctx, JSValueConst, JSAtom atom, JSValueConst) {
    if (atom_is_symbol(ctx, atom)) return JS_UNDEFINED;
    const char* name = JS_AtomToCString(ctx, atom);
    if (!name) return JS_EXCEPTION;
    if (engine_probe(name)) { JS_FreeCString(ctx, name); return JS_UNDEFINED; }
    JS_ThrowTypeError(ctx, "Context2D has no member '%s' (docs/SCRIPT_API.md §4)", name);
    JS_FreeCString(ctx, name);
    return JS_EXCEPTION;
}

int guard_set(JSContext* ctx, JSValueConst, JSAtom atom, JSValueConst, JSValueConst, int) {
    const char* name = JS_AtomToCString(ctx, atom);
    JS_ThrowTypeError(ctx, "Context2D has no member '%s' (docs/SCRIPT_API.md §4)",
                      name ? name : "?");
    if (name) JS_FreeCString(ctx, name);
    return -1;
}

JSClassExoticMethods g_guard_exotic = [] {
    JSClassExoticMethods m;
    std::memset(&m, 0, sizeof(m));
    m.get_property = guard_get;
    m.set_property = guard_set;
    return m;
}();

#define PROP(name) JS_CGETSET_MAGIC_DEF(#name, prop_get, prop_set, P_##name)

const JSCFunctionListEntry kC2DProto[] = {
    JS_CFUNC_DEF("save", 0, m_save),
    JS_CFUNC_DEF("restore", 0, m_restore),
    JS_CFUNC_DEF("translate", 2, m_translate),
    JS_CFUNC_DEF("scale", 2, m_scale),
    JS_CFUNC_DEF("rotate", 1, m_rotate),
    JS_CFUNC_DEF("transform", 6, m_transform),
    JS_CFUNC_DEF("setTransform", 6, m_setTransform),
    JS_CFUNC_DEF("resetTransform", 0, m_resetTransform),
    JS_CFUNC_DEF("getTransform", 0, m_getTransform),
    JS_CFUNC_DEF("createLinearGradient", 4, m_createLinearGradient),
    JS_CFUNC_DEF("createRadialGradient", 6, m_createRadialGradient),
    JS_CFUNC_DEF("createConicGradient", 3, m_createConicGradient),
    JS_CFUNC_DEF("beginPath", 0, m_beginPath),
    JS_CFUNC_DEF("closePath", 0, m_closePath),
    JS_CFUNC_DEF("moveTo", 2, m_moveTo),
    JS_CFUNC_DEF("lineTo", 2, m_lineTo),
    JS_CFUNC_DEF("quadraticCurveTo", 4, m_quadraticCurveTo),
    JS_CFUNC_DEF("bezierCurveTo", 6, m_bezierCurveTo),
    JS_CFUNC_DEF("arcTo", 5, m_arcTo),
    JS_CFUNC_DEF("arc", 6, m_arc),
    JS_CFUNC_DEF("ellipse", 8, m_ellipse),
    JS_CFUNC_DEF("rect", 4, m_rect),
    JS_CFUNC_DEF("roundRect", 5, m_roundRect),
    JS_CFUNC_DEF("fill", 1, m_fill),
    JS_CFUNC_DEF("stroke", 0, m_stroke),
    JS_CFUNC_DEF("clip", 1, m_clip),
    JS_CFUNC_DEF("isPointInPath", 3, m_isPointInPath),
    JS_CFUNC_DEF("fillRect", 4, m_fillRect),
    JS_CFUNC_DEF("strokeRect", 4, m_strokeRect),
    JS_CFUNC_DEF("clearRect", 4, m_clearRect),
    JS_CFUNC_DEF("drawImage", 9, m_drawImage),
    JS_CFUNC_DEF("fillText", 4, m_fillText),
    JS_CFUNC_DEF("strokeText", 4, m_strokeText),
    JS_CFUNC_DEF("measureText", 1, m_measureText),
    JS_CFUNC_DEF("setLineDash", 1, m_setLineDash),
    JS_CFUNC_DEF("getLineDash", 0, m_getLineDash),
    PROP(fillStyle), PROP(strokeStyle), PROP(globalAlpha), PROP(globalCompositeOperation),
    PROP(lineWidth), PROP(lineCap), PROP(lineJoin), PROP(miterLimit), PROP(lineDashOffset),
    PROP(imageSmoothingEnabled), PROP(imageSmoothingQuality), PROP(font), PROP(textAlign),
    PROP(textBaseline), PROP(letterSpacing), PROP(shadowColor), PROP(shadowBlur),
    PROP(shadowOffsetX), PROP(shadowOffsetY), PROP(filter), PROP(canvas),
};

const JSCFunctionListEntry kGradProto[] = {
    JS_CFUNC_DEF("addColorStop", 2, g_addColorStop),
};

const JSCFunctionListEntry kImgProto[] = {
    JS_CGETSET_MAGIC_DEF("width", img_get, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("height", img_get, nullptr, 1),
};

void register_classes(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!g_c2d_class) {
        JS_NewClassID(rt, &g_c2d_class);
        JS_NewClassID(rt, &g_grad_class);
        JS_NewClassID(rt, &g_img_class);
        JS_NewClassID(rt, &g_guard_class);
    }
    if (JS_IsRegisteredClass(rt, g_c2d_class)) return;
    JSClassDef c2d{};
    c2d.class_name = "CanvasRenderingContext2D";
    c2d.finalizer = c2d_finalizer;
    c2d.gc_mark = c2d_mark;
    JS_NewClass(rt, g_c2d_class, &c2d);
    JSClassDef grad{};
    grad.class_name = "CanvasGradient";
    grad.finalizer = grad_finalizer;
    JS_NewClass(rt, g_grad_class, &grad);
    JSClassDef img{};
    img.class_name = "Image";
    img.finalizer = img_finalizer;
    JS_NewClass(rt, g_img_class, &img);
    JSClassDef guard{};
    guard.class_name = "Context2DGuard";
    guard.exotic = &g_guard_exotic;
    JS_NewClass(rt, g_guard_class, &guard);
}

}  // namespace

JSValue c2d_new_context(JSContext* ctx) {
    register_classes(ctx);
    JSValue guard = JS_NewObjectProtoClass(ctx, JS_NULL, g_guard_class);
    JSValue proto = JS_NewObjectProto(ctx, guard);
    JS_FreeValue(ctx, guard);
    JS_SetPropertyFunctionList(ctx, proto, kC2DProto, sizeof(kC2DProto) / sizeof(kC2DProto[0]));
    JS_SetClassProto(ctx, g_c2d_class, proto);
    JSValue gp = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gp, kGradProto, sizeof(kGradProto) / sizeof(kGradProto[0]));
    JS_SetClassProto(ctx, g_grad_class, gp);
    JSValue ip = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ip, kImgProto, sizeof(kImgProto) / sizeof(kImgProto[0]));
    JS_SetClassProto(ctx, g_img_class, ip);

    JSValue obj = JS_NewObjectClass(ctx, (int)g_c2d_class);
    if (JS_IsException(obj)) return obj;
    C2D* c = new C2D();
    c->families["Inter"] = builtin_inter();
    c->families["sans-serif"] = builtin_inter();
    c->families["system-ui"] = builtin_inter();
    c->families["Mono"] = builtin_mono();
    c->families["monospace"] = builtin_mono();
    JS_SetOpaque(obj, c);
    reset_state(ctx, c);
    JS_PreventExtensions(ctx, obj);
    return obj;
}

void c2d_begin_frame(JSContext* ctx, JSValueConst c2d, SkCanvas* canvas, int logical_w,
                     int logical_h) {
    C2D* c = (C2D*)JS_GetOpaque(c2d, g_c2d_class);
    if (!c) return;
    reset_state(ctx, c);
    c->canvas = canvas;
    c->logical_w = logical_w;
    c->logical_h = logical_h;
    c->base.reset();
    if (!canvas || logical_w <= 0 || logical_h <= 0) return;
    SkISize px = canvas->getBaseLayerSize();
    c->base.setScale((float)px.width() / (float)logical_w, (float)px.height() / (float)logical_h);
    canvas->setMatrix(c->base);
}

JSValue c2d_new_image(JSContext* ctx, const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return JS_ThrowTypeError(ctx, "pms.image: cannot decode '%s'", path.c_str());
    }
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(SkPixmap(info, px, (size_t)w * 4));
    stbi_image_free(px);
    if (!image) return JS_ThrowTypeError(ctx, "pms.image: cannot wrap '%s'", path.c_str());
    JSValue o = JS_NewObjectClass(ctx, (int)g_img_class);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, new Image{std::move(image)});
    return o;
}

bool c2d_register_font(JSContext* ctx, JSValueConst c2d, const std::string& family,
                       const std::string& path, std::string* err) {
    (void)ctx;
    C2D* c = (C2D*)JS_GetOpaque(c2d, g_c2d_class);
    if (!c) { if (err) *err = "no Context2D"; return false; }
    if (family.empty()) { if (err) *err = "empty family name"; return false; }
    sk_sp<SkTypeface> tf = typeface_from_file(path);
    if (!tf) { if (err) *err = "cannot load font '" + path + "'"; return false; }
    std::vector<Face>& faces = c->families[family];
    for (const Face& f : faces)
        if (f.tf == tf) return true;
    faces.push_back(face_of(tf));
    return true;
}

sk_sp<SkTypeface> c2d_builtin_typeface(const std::string& family, int weight) {
    const std::vector<Face>& faces = family == "Mono" ? builtin_mono() : builtin_inter();
    const Face* f = match_face(faces, weight, false);
    return f ? f->tf : nullptr;
}
