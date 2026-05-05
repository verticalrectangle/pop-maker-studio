#pragma once

// All visual FX are now CPU pixel operations in video.cpp.
// These stubs keep app_init/app_shutdown call sites unchanged.
void fx_shader_init();
void fx_shader_shutdown();
