// engine_smoke — the Phase 0 proof artifact: drives the engine through the
// C ABI with NO app, NO socket, NO window. Links against pms-engine only.
// Exercises the full editing loop: new_project → set_format → tracks/clips →
// move/trim/split → FX bricks (effect, multi-fx, audio multi-fx) →
// set_clip_fx/decouple/delete → undo/redo → save v64 → load into a SECOND
// engine instance → structural get_project equivalence → project summary /
// recents queries → event drain. Exit 0 = the engine core is genuinely
// standalone AND its persistence + FX projection round-trip.
#include "../src/pms_engine.h"
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

using json = nlohmann::json;

static const char* k_root = "/tmp/pms-engine-smoke";

static void fail(const std::string& msg) {
    fprintf(stderr, "engine smoke: FAIL — %s\n", msg.c_str());
    exit(1);
}

static void expect(bool cond, const std::string& msg) {
    if (!cond) fail(msg);
}

// Send one lever; return the parsed reply envelope {id, result|error}.
static json cmd_raw(pms_engine* e, const std::string& method, const json& params) {
    json req = {{"id", "s"}, {"method", method}, {"params", params}};
    char* r = pms_command(e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    json reply = json::parse(out, nullptr, false);
    if (reply.is_discarded()) fail(method + ": unparsable reply: " + out.substr(0, 200));
    return reply;
}

// Send one lever; assert success; return the result payload.
static json cmd(pms_engine* e, const std::string& method, const json& params = json::object()) {
    json reply = cmd_raw(e, method, params);
    if (reply.contains("error"))
        fail(method + " errored: " + reply["error"].get<std::string>());
    return reply.value("result", json::object());
}

static bool near_eq(double a, double b) { return std::fabs(a - b) < 1e-3; }

static double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

// Structural projection of get_project(verbose:true): everything that must
// survive a save → load round-trip, floats rounded so representation noise
// can't fail the comparison.
static json structure_of(const json& proj) {
    json s;
    s["format"] = proj.value("format", "");
    json ts = json::array();
    for (const auto& t : proj["tracks"]) {
        json tt;
        tt["name"] = t.value("name", "");
        json cs = json::array();
        for (const auto& c : t["clips"]) {
            json cc;
            cc["type"]  = c.value("type", "");
            cc["start"] = round3(c.value("start", 0.0));
            cc["end"]   = round3(c.value("end", 0.0));
            if (c.contains("fx_type")) cc["fx_type"] = c["fx_type"];
            if (c.contains("fx_chain")) {
                json ch = json::array();
                for (const auto& e : c["fx_chain"]) ch.push_back(e.value("fx_type", ""));
                cc["fx_chain"] = ch;
            }
            cs.push_back(cc);
        }
        tt["clips"] = cs;
        ts.push_back(tt);
    }
    s["tracks"] = ts;
    return s;
}

// Index of the track named `name`, or fail.
static int track_index(const json& proj, const std::string& name) {
    for (int i = 0; i < (int)proj["tracks"].size(); ++i)
        if (proj["tracks"][i].value("name", "") == name) return i;
    fail("no track named '" + name + "'");
    return -1;
}

// Find the first clip of `type` on track ti; returns clip json or fails.
static json find_clip(const json& proj, int ti, const std::string& type) {
    for (const auto& c : proj["tracks"][ti]["clips"])
        if (c.value("type", "") == type) return c;
    fail("no clip of type '" + type + "' on track " + std::to_string(ti) +
         " — tracks: " + proj["tracks"].dump().substr(0, 2000));
    return {};
}

int main() {
    // Keep recents/config writes inside the smoke sandbox, not the real $HOME.
    setenv("HOME", (std::string(k_root) + "/home").c_str(), 1);

    pms_engine* e = pms_create(nullptr, "", k_root);
    if (!e) { fprintf(stderr, "create failed\n"); return 1; }
    if (pms_abi_version() != PMS_ENGINE_ABI) { fprintf(stderr, "abi mismatch\n"); return 1; }
    printf("engine up: abi %u, .pms v%u\n", pms_abi_version(), pms_project_version());

    // ── list_body_fx: the BodyFXInfo manifest over the command channel ───────
    {
        json r = cmd(e, "list_body_fx");
        expect(r.contains("effects") && r["effects"].is_array(),
               "list_body_fx must return an effects array");
        expect(r["effects"].size() >= 10,
               "list_body_fx must return >= 10 effects (got " +
               std::to_string(r["effects"].size()) + ")");
        bool neon = false;
        for (const auto& fx : r["effects"]) {
            expect(fx.contains("name") && fx["name"].is_string() &&
                   !fx["name"].get<std::string>().empty(),
                   "every body FX entry needs a non-empty name");
            expect(fx.contains("params") && fx["params"].is_array(),
                   "every body FX entry needs a params array");
            for (const auto& p : fx["params"])
                expect(p.contains("name") && p.contains("label") &&
                       p.contains("min") && p.contains("max") &&
                       p.contains("default"),
                       "body FX params need name/label/min/max/default");
            if (fx["name"].get<std::string>() == "Neon Outline") neon = true;
        }
        expect(neon, "list_body_fx must contain exactly-named \"Neon Outline\"");
    }

    // ── pms_model_status: real JSON from the C ABI ────────────────────────────
    {
        char* ms = pms_model_status(e);
        expect(ms != nullptr, "pms_model_status must return a string");
        json j = json::parse(ms ? ms : "", nullptr, false);
        pms_free(ms);
        expect(!j.is_discarded(), "pms_model_status must return valid JSON");
        expect(j.contains("models_dir") && j["models_dir"].is_string(),
               "pms_model_status must report models_dir");
        expect(j.contains("models") && j["models"].is_array() &&
               !j["models"].empty(),
               "pms_model_status must report a non-empty models array");
        for (const auto& m : j["models"])
            expect(m.contains("name") && m["name"].is_string() &&
                   m.contains("present") && m["present"].is_boolean(),
                   "every model entry needs name + present");
    }

    // ── External mic-capture injection (pms_submit_mic_block) ────────────────
    // Push 48 kHz interleaved stereo sine blocks through the C ABI; the audio
    // module resamples to 44.1k at push time and counts post-resample frames.
    // get_audio_perf must reflect the accepted count and report zero drops;
    // then overfill the fixed ring (131072 frames) and drops must appear.
    {
        json perf0 = cmd(e, "get_audio_perf");
        expect(perf0.contains("capture_injected_frames") &&
               perf0.contains("capture_dropped_frames"),
               "get_audio_perf must expose capture_injected/dropped_frames");
        expect(perf0["capture_injected_frames"].get<uint64_t>() == 0 &&
               perf0["capture_dropped_frames"].get<uint64_t>() == 0,
               "injection counters must start at zero");

        // Null/zero guards must be no-ops (not crashes).
        pms_submit_mic_block(nullptr, nullptr, 0, 0.0);
        pms_submit_mic_block(e, nullptr, 480, 48000.0);
        std::vector<float> blk(480 * 2);
        pms_submit_mic_block(e, blk.data(), 0, 48000.0);

        const size_t BLK = 480; const int NBLK = 10;  // 4800 frames @ 48k
        double ph = 0.0;
        for (int b = 0; b < NBLK; ++b) {
            for (size_t f = 0; f < BLK; ++f) {
                float s = (float)std::sin(ph);
                ph += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
                blk[f*2] = s; blk[f*2+1] = s;
            }
            pms_submit_mic_block(e, blk.data(), BLK, 48000.0);
        }
        for (int i = 0; i < 3; ++i) pms_tick(e, 1.0 / 60.0);

        json perf = cmd(e, "get_audio_perf");
        uint64_t inj  = perf["capture_injected_frames"].get<uint64_t>();
        uint64_t drop = perf["capture_dropped_frames"].get<uint64_t>();
        // Post-resample count: 4800 * 44100/48000 = 4410 (± a few frames of
        // linear-resampler boundary rounding).
        const double want = (double)(NBLK * BLK) * 44100.0 / 48000.0;
        expect(std::fabs((double)inj - want) <= 8.0,
               "injected frame counter must be ~" + std::to_string((int)want) +
               " post-resample (got " + std::to_string(inj) + ")");
        expect(drop == 0, "no drops expected while the ring has room (got " +
               std::to_string(drop) + ")");

        // Overflow: push far more than the 131072-frame ring capacity without
        // draining. Policy is reject-newest, so drops must be counted.
        std::vector<float> big(4096 * 2, 0.25f);
        for (int b = 0; b < 64; ++b)              // 262144 frames @ 44.1k
            pms_submit_mic_block(e, big.data(), 4096, 44100.0);
        perf = cmd(e, "get_audio_perf");
        expect(perf["capture_dropped_frames"].get<uint64_t>() > 0,
               "overfilling the injected ring must increment the drop counter");
        expect(perf["capture_injected_frames"].get<uint64_t>() > inj,
               "accepted counter must still have grown while filling the ring");
    }

    // ── Project setup ─────────────────────────────────────────────────────────
    cmd(e, "new_project", {{"force", true}});
    cmd(e, "set_format", {{"format", "vertical"}});
    expect(cmd(e, "get_project").value("format", "") == "vertical",
           "slim get_project must report format=vertical");
    cmd(e, "add_track", {{"name", "V"}, {"position", 0}});
    cmd(e, "add_track", {{"name", "T"}, {"position", 1}});

    // ── move_track: track order IS canvas z-order — reorder + round-trip ─────
    {
        json mv = cmd(e, "move_track", {{"from", 0}, {"to", 1}});
        expect(mv["order"].size() == 2 && mv["order"][0] == "T" && mv["order"][1] == "V",
               "move_track 0->1 must yield order [T, V]");
        mv = cmd(e, "move_track", {{"from", 1}, {"to", 0}});
        expect(mv["order"][0] == "V" && mv["order"][1] == "T",
               "move_track back must restore order [V, T]");
        json bad = cmd_raw(e, "move_track", {{"from", 7}, {"to", 0}});
        expect(bad.contains("error"), "move_track with a bad index must be rejected");
    }

    // ── Clips: video-typed (image path — no decode needed) + text ────────────
    std::string fake_png = std::string(k_root) + "/fake_clip.png";
    json r = cmd(e, "add_clip", {{"track", 0}, {"type", "video"}, {"start", 0.0},
                                 {"end", 4.0}, {"text", fake_png}});
    expect(r.value("clip", -1) == 0, "video add_clip should land at index 0");
    r = cmd(e, "add_clip", {{"track", 1}, {"type", "text"}, {"start", 0.0},
                            {"end", 2.0}, {"text", "engine smoke"}});
    expect(r.value("clip", -1) == 0, "text add_clip should land at index 0");

    // ── move / trim / split ───────────────────────────────────────────────────
    cmd(e, "move_clip", {{"track", 1}, {"clip", 0}, {"start", 1.0}});
    cmd(e, "trim_clip", {{"track", 1}, {"clip", 0}, {"start", 1.0}, {"end", 3.0}});
    json proj = cmd(e, "get_project", {{"verbose", true}});
    {
        const json& c = proj["tracks"][1]["clips"][0];
        expect(near_eq(c["start"].get<double>(), 1.0) &&
               near_eq(c["end"].get<double>(), 3.0),
               "text clip should sit at [1,3] after move+trim");
    }
    r = cmd(e, "split_clip", {{"track", 1}, {"clip", 0}, {"time", 2.0}});
    expect(r.value("left_clip", -1) == 0 && r.value("right_clip", -1) == 1,
           "split_clip should report left=0 right=1");

    // ── Canvas transform round-trip (CANVAS_PLAN.md stage 0) ─────────────────
    // The verbose projection must carry the per-clip canvas transform so the
    // iOS editing surface can read back what set_clip_prop(s) wrote.
    {
        proj = cmd(e, "get_project", {{"verbose", true}});
        const json& c = proj["tracks"][0]["clips"][0];
        expect(near_eq(c.value("pos_x", -1.0), 0.5) &&
               near_eq(c.value("pos_y", -1.0), 0.5) &&
               near_eq(c.value("scale_x", -1.0), 1.0) &&
               near_eq(c.value("rotation", -1.0), 0.0) &&
               near_eq(c.value("crop_l", -1.0), 0.0) &&
               c.contains("flip_h") && !c["flip_h"].get<bool>(),
               "verbose projection must carry default canvas transform fields");
        cmd(e, "set_clip_props", {{"ops", json::array({
            {{"track", 0}, {"clip", 0}, {"prop", "pos_x"},    {"value", 0.25}},
            {{"track", 0}, {"clip", 0}, {"prop", "scale_y"},  {"value", 1.5}},
            {{"track", 0}, {"clip", 0}, {"prop", "rotation"}, {"value", 30.0}},
            {{"track", 0}, {"clip", 0}, {"prop", "crop_l"},   {"value", 0.1}},
            {{"track", 0}, {"clip", 0}, {"prop", "flip_h"},   {"value", true}}})}});
        proj = cmd(e, "get_project", {{"verbose", true}});
        const json& c2 = proj["tracks"][0]["clips"][0];
        expect(near_eq(c2.value("pos_x", -1.0), 0.25) &&
               near_eq(c2.value("scale_y", -1.0), 1.5) &&
               near_eq(c2.value("rotation", -1.0), 30.0) &&
               near_eq(c2.value("crop_l", -1.0), 0.1) &&
               c2["flip_h"].get<bool>(),
               "canvas transform props must round-trip through set_clip_props");
        // crop clamp: crop_r is limited to 0.95 - crop_l (engine-side rule the
        // iOS crop UI mirrors).
        cmd(e, "set_clip_prop", {{"track", 0}, {"clip", 0},
                                 {"prop", "crop_r"}, {"value", 0.99}});
        proj = cmd(e, "get_project", {{"verbose", true}});
        expect(near_eq(proj["tracks"][0]["clips"][0].value("crop_r", -1.0), 0.85),
               "crop_r must clamp to 0.95 - crop_l");
        // restore neutral transform so later scene/FX assertions see defaults
        cmd(e, "set_clip_props", {{"ops", json::array({
            {{"track", 0}, {"clip", 0}, {"prop", "pos_x"},    {"value", 0.5}},
            {{"track", 0}, {"clip", 0}, {"prop", "scale_y"},  {"value", 1.0}},
            {{"track", 0}, {"clip", 0}, {"prop", "rotation"}, {"value", 0.0}},
            {{"track", 0}, {"clip", 0}, {"prop", "crop_l"},   {"value", 0.0}},
            {{"track", 0}, {"clip", 0}, {"prop", "crop_r"},   {"value", 0.0}},
            {{"track", 0}, {"clip", 0}, {"prop", "flip_h"},   {"value", false}}})}});
    }

    // ── Effect brick over the video: couples → becomes a MultiFX glass brick
    // snapped to the host span, with the effect windowed in fx_chain.
    r = cmd(e, "add_effect_brick", {{"track", 0}, {"fx_type", "pixelate"},
                                    {"start", 0.0}, {"end", 2.0},
                                    {"params", {{"size", 24.0}}}});
    int fx_ci = r.value("clip", -1);
    expect(fx_ci >= 0, "add_effect_brick must return a clip index");
    expect(r.value("coupled", false), "pixelate brick should auto-couple to the video");
    proj = cmd(e, "get_project", {{"verbose", true}});
    {
        json fx = find_clip(proj, 0, "multi_fx");   // coupled video FX = glass multi_fx
        expect(fx.value("coupled", false), "coupled brick must project coupled=true");
        expect(fx.contains("fx_chain") && fx["fx_chain"].size() == 1,
               "coupled pixelate brick must carry a 1-entry fx_chain");
        const json& e0 = fx["fx_chain"][0];
        expect(e0.value("fx_type", "") == "pixelate",
               "fx_chain[0] must project fx_type='pixelate' (got '" +
               e0.value("fx_type", "") + "')");
        expect(e0.contains("fx_params") &&
               near_eq(e0["fx_params"].value("size", 0.0), 24.0),
               "fx_chain[0].fx_params.size must read back 24");
        expect(e0.contains("rel_start") && e0.contains("rel_end") &&
               near_eq(e0["rel_end"].get<double>(), 2.0),
               "fx_chain entries must carry the rel_start/rel_end window");
    }

    // set_clip_fx must reach INTO a multi_fx brick's chain (the iOS inspector
    // edits coupled bricks, which are always multi_fx).
    cmd(e, "set_clip_fx", {{"track", 0}, {"clip", fx_ci}, {"fx_id", "pixelate"},
                           {"params", {{"size", 40.0}}}});
    proj = cmd(e, "get_project", {{"verbose", true}});
    {
        json fx = find_clip(proj, 0, "multi_fx");
        expect(near_eq(fx["fx_chain"][0]["fx_params"].value("size", 0.0), 40.0),
               "set_clip_fx on a multi_fx brick must update the chained effect's param");
    }

    // ── Decouple lifts the brick onto its own track ───────────────────────────
    int tracks_before = (int)proj["tracks"].size();
    cmd(e, "decouple_fx_brick", {{"track", 0}, {"clip", fx_ci}});
    proj = cmd(e, "get_project", {{"verbose", true}});
    expect((int)proj["tracks"].size() == tracks_before + 1,
           "decouple_fx_brick should lift the brick onto a fresh track");

    // ── Plain (uncoupled) Effect brick: no host on the text track, so it stays
    // ClipType::Effect and must project fx_type + fx_params directly.
    r = cmd(e, "add_effect_brick", {{"track_name", "T"}, {"fx_type", "pixelate"},
                                    {"start", 4.0}, {"end", 6.0},
                                    {"params", {{"size", 24.0}}}});
    int plain_ci = r.value("clip", -1);
    expect(plain_ci >= 0, "plain add_effect_brick must return a clip index");
    expect(!r.value("coupled", true), "text track has no host — brick must stay uncoupled");
    cmd(e, "set_clip_fx", {{"track_name", "T"}, {"clip", plain_ci}, {"fx_id", "pixelate"},
                           {"params", {{"size", 32.0}}}});
    proj = cmd(e, "get_project", {{"verbose", true}});
    {
        json fx = find_clip(proj, track_index(proj, "T"), "effect");
        expect(fx.value("fx_type", "") == "pixelate",
               "effect brick must project fx_type='pixelate' (got '" +
               fx.value("fx_type", "") + "')");
        expect(fx.contains("fx_params") &&
               near_eq(fx["fx_params"].value("size", 0.0), 32.0),
               "fx_params.size must read back 32 after set_clip_fx");
        expect(fx["fx_params"].contains("amount"), "fx_params must include 'amount'");
    }

    // ── Multi-FX chain (incl. body_fx sub-clip) + audio Multi-FX ─────────────
    r = cmd(e, "add_multifx_brick",
            {{"track_name", "V"}, {"start", 2.0}, {"end", 4.0},
             {"effects", json::array({
                 {{"fx_type", "film_grain"}, {"params", {{"intensity", 0.6}}}},
                 {{"fx_type", "body_fx"}, {"body_fx_type", "Neon Outline"}}})}});
    expect(r.value("clip", -1) >= 0, "add_multifx_brick must return a clip index");
    r = cmd(e, "add_audio_multifx_brick",
            {{"track_name", "T"}, {"start", 0.0}, {"end", 2.0},
             {"effects", json::array({
                 {{"fx_type", "audio_reverb"}, {"params", {{"room", 0.7}}}}})}});
    expect(r.value("clip", -1) >= 0, "add_audio_multifx_brick must return a clip index");

    proj = cmd(e, "get_project", {{"verbose", true}});
    {
        json mfx = find_clip(proj, track_index(proj, "V"), "multi_fx");
        expect(mfx.contains("fx_chain") && mfx["fx_chain"].size() == 2,
               "multi_fx brick must project a 2-entry fx_chain");
        const json& e0 = mfx["fx_chain"][0];
        const json& e1 = mfx["fx_chain"][1];
        expect(e0.value("fx_type", "") == "film_grain" &&
               e0.contains("fx_params") &&
               near_eq(e0["fx_params"].value("intensity", 0.0), 0.6),
               "fx_chain[0] must be film_grain with intensity 0.6");
        expect(e0.contains("rel_start") && e0.contains("rel_end"),
               "fx_chain entries must carry rel_start/rel_end");
        expect(e1.value("fx_type", "") == "body_fx" &&
               e1.value("body_fx_type", "") == "Neon Outline" &&
               e1.contains("body_fx_params") && e1["body_fx_params"].size() == 4,
               "fx_chain[1] must be body_fx NeonOutline with 4 params");

        json afx = find_clip(proj, track_index(proj, "T"), "audio_multi_fx");
        expect(afx.contains("fx_chain") && afx["fx_chain"].size() == 1 &&
               afx["fx_chain"][0].value("fx_type", "") == "audio_reverb" &&
               near_eq(afx["fx_chain"][0]["fx_params"].value("room", 0.0), 0.7),
               "audio_multi_fx chain must project audio_reverb with room 0.7");
    }

    // ── delete + undo/redo ────────────────────────────────────────────────────
    // Track 1 now holds: text, text (split), effect brick, audio multi-fx.
    r = cmd(e, "delete_clip", {{"track_name", "T"}, {"clip", 1}});
    expect(r.value("clips_remaining", -1) == 3, "delete_clip should leave 3 clips on track 1");
    r = cmd(e, "undo");
    expect((int)cmd(e, "get_clips", {{"track_name", "T"}}).size() == 4,
           "undo must restore the deleted clip");
    r = cmd(e, "redo");
    expect((int)cmd(e, "get_clips", {{"track_name", "T"}}).size() == 3,
           "redo must re-apply the delete");

    // ── abort_batch rolls back to the begin_batch snapshot ───────────────────
    cmd(e, "begin_batch", {{"label", "smoke compound"}});
    cmd(e, "add_clip", {{"track_name", "T"}, {"type", "text"}, {"start", 5.0},
                        {"end", 6.0}, {"text", "doomed"}});
    r = cmd(e, "abort_batch");
    expect(r.value("aborted", false), "abort_batch must report aborted=true");
    expect((int)cmd(e, "get_clips", {{"track_name", "T"}}).size() == 3,
           "abort_batch must roll back the mid-batch add_clip");

    // ── ScratchFilm clip_style round-trip + Wave 2 serialization fix ────────
    {
        // The text clip on track 1, clip 0 already exists from the setup above.
        cmd(e, "set_clip_prop", {{"track", 1}, {"clip", 0},
                                 {"prop", "clip_style"}, {"value", "scratch"}});
        json p = cmd(e, "get_project", {{"verbose", true}});
        std::string got = p["tracks"][1]["clips"][0].value("clip_style", "");
        expect(got == "scratch",
               "clip_style should be 'scratch', got '" + got + "'");

        // All styles must round-trip (pre-existing bug: Wave 2 styles
        // serialized as "unknown" before the anim_style_str fix).
        const char* styles[] = {"wave", "jitter", "explode", "gravity", "scratch",
                                "scratch-raw",
                                "fade", "glitch", "typewriter", "bounce", "scale",
                                "slide", "stack", "block", "none"};
        for (const char* s : styles) {
            cmd(e, "set_clip_prop", {{"track", 1}, {"clip", 0},
                                     {"prop", "clip_style"}, {"value", s}});
            json pp = cmd(e, "get_project", {{"verbose", true}});
            std::string g = pp["tracks"][1]["clips"][0].value("clip_style", "");
            expect(g == s, "clip_style '" + std::string(s) +
                           "' round-trip failed, got '" + g + "'");
        }
        printf("scratch IPC round-trip: PASS (all 15 styles serialize correctly)\n");
    }

    // ── save → summary/recents → load into a SECOND engine ───────────────────
    std::string pms_path = std::string(k_root) + "/smoke.pms";
    cmd(e, "save_project", {{"path", pms_path}});
    json proj1 = cmd(e, "get_project", {{"verbose", true}});
    expect(proj1.value("format", "") == "vertical", "verbose get_project must report format");

    json sum = cmd(e, "get_project_summary", {{"path", pms_path}});
    expect(sum.value("name", "") == "smoke", "summary name must be the file stem");
    expect(sum.value("format", "") == "vertical", "summary format must round-trip");
    expect(sum.value("clip_count", -1) == 2,
           "summary clip_count must be 2 (video+text), got " +
           std::to_string(sum.value("clip_count", -1)));
    expect(sum.value("fx_count", -1) == 4,
           "summary fx_count must be 4 (2 multifx + effect + audio multifx), got " +
           std::to_string(sum.value("fx_count", -1)));
    expect(sum.value("duration", 0.0) >= 4.0, "summary duration must cover the clips");
    expect(sum.value("fps", 0.0) > 0.0, "summary fps must be positive");
    expect(sum.value("modified_unix", (int64_t)0) > 0, "summary needs modified_unix");
    // The temporary-AppState load must NOT disturb the open project.
    expect(cmd(e, "get_project", {{"verbose", true}}) == proj1,
           "get_project_summary must not mutate the open project");

    json recents = cmd(e, "list_recent_projects");
    {
        bool found = false;
        for (const auto& p : recents["projects"])
            if (p.value("path", "") == pms_path && !p.contains("error")) found = true;
        expect(found, "list_recent_projects must include the saved project");
    }

    pms_destroy(e);

    pms_engine* e2 = pms_create(nullptr, "", k_root);
    if (!e2) { fprintf(stderr, "second create failed\n"); return 1; }
    cmd(e2, "load_project", {{"path", pms_path}});
    json proj2 = cmd(e2, "get_project", {{"verbose", true}});
    json s1 = structure_of(proj1), s2 = structure_of(proj2);
    if (s1 != s2)
        fail("save→load structural mismatch:\n  saved:  " + s1.dump() +
             "\n  loaded: " + s2.dump());
    expect(proj2.value("format", "") == "vertical", "format must survive save→load");

    char* ev = pms_poll_events(e2);
    expect(ev && ev[0] == '[', "pms_poll_events must drain to a JSON array");
    pms_free(ev);
    pms_destroy(e2);

    printf("engine smoke: PASS (ABI %u, .pms v%u)\n",
           pms_abi_version(), pms_project_version());
    return 0;
}
