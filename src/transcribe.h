#pragma once
#include "app.h"
#include <string>

enum class PipelineMode {
    Both,        // Demucs + WhisperX
    TranscribeOnly,  // WhisperX on original file (no Demucs)
    SeparateOnly,    // Demucs only, no subtitles
};

// Kick off ml_pipeline.py as a subprocess.
// Writes progress to `status` from a background thread.
// out_words_json / out_vocals_wav are set before the thread reads them.
void transcribe_start(
    const std::string& audio_path,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav,
    PipelineMode       mode = PipelineMode::Both
);

void transcribe_cancel();
bool transcribe_running();
