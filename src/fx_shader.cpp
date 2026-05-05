#include "fx_shader.h"

// All visual FX are now CPU pixel operations in video.cpp (applied during MJPEG decode).
// These stubs keep app_init/app_shutdown call sites unchanged.
void fx_shader_init()    {}
void fx_shader_shutdown() {}
