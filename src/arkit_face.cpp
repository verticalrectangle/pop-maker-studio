#include "arkit_face.h"
#include <mutex>

bool g_face_overlay = false;

static struct {
    std::mutex  mtx;
    ARKitFace3D face;
    double      cam_t = 0.0;      // newest camera frame host time
    double      submit_t = 0.0;   // camera time at the last face submission
} g_arkit;

void arkit_face_note_camera_time(double host_time) {
    std::lock_guard<std::mutex> lk(g_arkit.mtx);
    g_arkit.cam_t = host_time;
}

void arkit_face3d_submit(const ARKitFace3D* f) {
    std::lock_guard<std::mutex> lk(g_arkit.mtx);
    if (!f || !f->valid) { g_arkit.face.valid = false; return; }
    g_arkit.face = *f;
    g_arkit.submit_t = g_arkit.cam_t;
}

bool arkit_face3d_take(ARKitFace3D* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lk(g_arkit.mtx);
    if (!g_arkit.face.valid) return false;
    if (g_arkit.cam_t - g_arkit.submit_t > 0.15) return false;
    *out = g_arkit.face;
    return true;
}
