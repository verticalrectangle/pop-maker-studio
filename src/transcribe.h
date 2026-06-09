#pragma once
#include "app.h"
#include <string>
#include <vector>
// PipelineMode is defined in app.h so AppState can hold last_pipeline_mode.

// Kick off ml_pipeline.py as a subprocess.
// Writes progress to `status` from a background thread.
// out_words_json / out_vocals_wav are set before the thread reads them.
// clip_in / clip_dur (seconds): when clip_dur > 0, only the source region
// [clip_in, clip_in+clip_dur] is decoded, separated, and transcribed.
// Whisper timestamps in the output JSON are shifted back to be source-relative
// so that apply_subtitle_mode's tl_offset logic works unchanged.
void transcribe_start(
    const std::string& audio_path,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav,
    PipelineMode       mode      = PipelineMode::Both,
    double             proxy_fps = 0.0,
    float              clip_in   = 0.f,
    float              clip_dur  = 0.f
);

void transcribe_cancel();
bool transcribe_running();

// Search a media file for a spoken query using chunked Whisper inference.
// Decodes and transcribes 5-minute windows until the query is found, then stops.
// Returns timestamps relative to the source file.
// buffer_sec: extra seconds to collect past the match end before stopping.
struct TranscribeSearchResult {
    bool        found   = false;
    float       start   = 0.f;  // source-relative start of matched region
    float       end     = 0.f;  // source-relative end of matched region
    std::string excerpt;        // words around the match
    std::string error;
};
TranscribeSearchResult transcribe_search(
    const std::string&              path,
    const std::vector<std::string>& query_words,
    float                           buffer_sec = 60.f
);

struct SearchCandidate {
    float       start = 0.f;
    float       end   = 0.f;
    float       score = 0.f;   // 0..1 fraction of query words matched
    std::string excerpt;
};

struct SearchStatus {
    bool        running       = false;
    float       progress      = 0.f;
    float       current_sec   = 0.f;
    float       total_sec     = 0.f;
    std::string message;
    // result fields — valid when running == false and error is empty
    bool        found         = false;
    float       start         = 0.f;
    float       end           = 0.f;
    std::string excerpt;
    std::string error;
    // On no-match, the top-N closest fuzzy hits in the accumulated transcript
    // (sorted by score descending).  Lets the caller / human disambiguate or
    // re-query rather than getting a blank failure.
    std::vector<SearchCandidate> candidates;
};

// Kick off a background search (returns immediately). Poll transcribe_search_status().
void transcribe_search_start(
    const std::string&              path,
    const std::vector<std::string>& query_words,
    float                           buffer_sec = 60.f
);
SearchStatus transcribe_search_status();
bool         transcribe_search_running();
