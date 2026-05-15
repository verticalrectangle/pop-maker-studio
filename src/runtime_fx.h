#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct RuntimeFXParam {
    std::string name;
    std::string label;
    float min_val     = 0.f;
    float max_val     = 1.f;
    float default_val = 0.f;
    float curve       = 1.f;  // 1=linear, 0.5=sqrt (UI slider mapping only)
};

struct RuntimeFXDef {
    std::string                 id;
    std::string                 name;
    std::string                 category;
    std::string                 description;
    std::vector<RuntimeFXParam> params;
    std::string                 glsl_body;
    unsigned                    program = 0;  // GLuint; 0 = compile failed
    std::string                 compile_error;
    std::string                 source_hash;  // tracks whether recompile is needed
};

// Call at app init. Loads all effects from dir.
void runtime_fx_init(const std::string& dir);

// Call every frame from app_frame. Checks dir mtime and reloads on change.
void runtime_fx_poll(const std::string& dir);

// Call at app shutdown. Frees GL resources.
void runtime_fx_shutdown();

// Returns all currently loaded definitions (including failed ones).
const std::vector<RuntimeFXDef>& runtime_fx_list();

// Returns nullptr if id not found.
const RuntimeFXDef* runtime_fx_find(const std::string& id);

// Apply a runtime effect: src_tex → returns stable output GL texture id.
// param_values must be in registry param order (missing values default to param.default_val).
// Returns src_tex unchanged on any error.
uintptr_t runtime_fx_apply(const std::string& id,
                            uintptr_t src_tex, int w, int h,
                            const std::vector<float>& param_values,
                            float amount, float t);

// Test-compile a GLSL body with the given params without registering it.
// Returns "" on success, GL compile error on failure.
std::string runtime_fx_validate(const std::string& glsl_body,
                                const std::vector<RuntimeFXParam>& params);
