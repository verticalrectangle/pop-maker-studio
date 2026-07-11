// stb_impl.cpp — the stb_image_write implementation, always compiled (the
// snapshot/export JPEG writer is referenced from ungated code; render.cpp
// which used to host it is now media-gated for headless/iOS).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
