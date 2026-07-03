// pms_engine.h — the C ABI over the Pop Maker Studio engine (pms-engine
// static lib). This is the surface the iOS Swift shell consumes and the
// headless test target proves; it deliberately matches the contract published
// in the pms-ios repo (Engine/include/pms_engine.h there is the same file,
// kept in sync by hand until the engine build exports it).
//
// Desktop notes:
//  - `graphics_device` is the platform graphics handle: MTLDevice* on iOS,
//    ignored (pass null) on desktop GL where the caller owns the context.
//  - pms_render is not implemented yet (Phase 2 — RenderSurface seam); the
//    desktop app still renders through its own loop.
//  - pms_poll_events currently drains a minimal engine event queue; the
//    full event formalization tracks docs/IOS_PORT_PLAN.md Phase 0.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pms_engine pms_engine;

pms_engine* pms_create(void* graphics_device,
                       const char* asset_root,
                       const char* state_root);
void        pms_destroy(pms_engine*);

// Advance clocks / pump worker results. Call once per frame.
void pms_tick(pms_engine*, double dt_seconds);

// Execute one lever (same JSON protocol as the desktop IPC socket / agent
// tools; see docs/LEVERS.md in pms-ios). Returns a malloc'd JSON string —
// free with pms_free. Thread: main.
char* pms_command(pms_engine*, const char* json_request);

// Drain pending engine events as a JSON array string (malloc'd; pms_free).
char* pms_poll_events(pms_engine*);

void pms_free(char*);

#define PMS_ENGINE_ABI 1
uint32_t pms_abi_version(void);
uint32_t pms_project_version(void);

#ifdef __cplusplus
}
#endif
