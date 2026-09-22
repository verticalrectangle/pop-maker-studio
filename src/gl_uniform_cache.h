#pragma once
// gl_uniform_cache.h — memoize glGetUniformLocation (program, name) -> location.
//
// The frame path issues hundreds of glGetUniformLocation calls per frame
// (every uniform of every active pass of every layer, plus all global passes),
// and each call is a string lookup inside the driver. Uniform locations are
// stable for a linked program, so cache them process-wide: first call per
// (program, name) hits the driver, the rest are a hash-map lookup.
//
// Correctness: entries go stale if the driver recycles a program id after
// glDeleteProgram. Every program (re)link path in this codebase must call
// uni_cache_clear() after linking (link_prog/link_prog2, link_body_prog,
// compile_def and the runtime_fx reload/shutdown paths that delete programs).
// Links are init/reload-time events; clearing is effectively free.
//
// Threading: the GL context lives on one thread (main/preview, export tick).
// All cache users run there. Not thread-safe by design — do not call off-GL.
#include "gl_compat.h"

#include <string_view>
#include <unordered_map>

struct GlUniKey {
    GLuint prog = 0;
    std::string_view name;
    bool operator==(const GlUniKey& o) const { return prog == o.prog && name == o.name; }
};

struct GlUniKeyHash {
    size_t operator()(GlUniKey const& k) const noexcept {
        size_t h1 = std::hash<GLuint>{}(k.prog);
        size_t h2 = std::hash<std::string_view>{}(k.name);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

inline std::unordered_map<GlUniKey, GLint, GlUniKeyHash>& gl_uni_cache() {
    static std::unordered_map<GlUniKey, GLint, GlUniKeyHash> cache;
    return cache;
}

// Drop-in replacement for glGetUniformLocation with identical semantics
// (including returning -1 for unknown names — cached as such).
inline GLint uni_loc(GLuint prog, const char* name) {
    auto& cache = gl_uni_cache();
    const GlUniKey key{prog, name ? name : ""};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const GLint loc = glGetUniformLocation(prog, name);
    cache.emplace(key, loc);
    return loc;
}

inline void uni_cache_clear() { gl_uni_cache().clear(); }
