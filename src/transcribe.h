#pragma once
#include "app.h"
#include <string>

// Kick off the ml_pipeline.py subprocess (Demucs + WhisperX).
// Writes progress updates to `status` from a background thread.
// On completion fills `out_words_json` with the path to the JSON file
// and `out_vocals_wav` with the extracted vocals path.
void transcribe_start(
    const std::string& audio_path,
    const std::string& python_path,
    const std::string& pipeline_script,
    PipelineStatus&    status,          // written from bg thread — read from UI thread
    std::string&       out_words_json,
    std::string&       out_vocals_wav
);

void transcribe_cancel();
bool transcribe_running();
