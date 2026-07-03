// ui_geom.cpp — engine-owned storage for UI-produced geometry snapshots
// (canvas transform handles, live-mirror quad, timeline clip rects). The
// desktop UI WRITES these each frame; the engine's IPC dispatcher READS them
// (agents drive the UI via ui_input against these coordinates). Engine owns
// the storage so the accessors link into pms-engine; the app remains free to
// fill or not fill them (headless engine builds simply report invalid/empty).
#include "engine_seams.h"

CanvasHandleGeom g_canvas_handle_geom;
CanvasHandleGeom canvas_handle_geom() { return g_canvas_handle_geom; }

MirrorDebugGeom g_mirror_dbg_geom;
MirrorDebugGeom mirror_debug_geom() { return g_mirror_dbg_geom; }

TLGeom g_tl_geom;
const TLGeom& tl_geom() { return g_tl_geom; }
