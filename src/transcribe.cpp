#include "transcribe.h"
#include <thread>
#include <atomic>
#include <cstdio>
#include <sstream>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

static std::thread       g_thread;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_cancel{false};

// Parse progress markers written by ml_pipeline.py to stderr:
//   [MODEL]      loading models
//   [SEPARATE]   demucs stem separation
//   [TRANSCRIBE] whisperx transcription
//   [ALIGN]      forced alignment
//   [DONE]       finished
static void parse_line(const std::string& line, PipelineStatus& status) {
    if (line.find("[MODEL]") != std::string::npos) {
        status.stage    = PipelineStage::Extract;
        status.progress = 0.05f;
        status.message  = line.substr(line.find(']') + 2);
    } else if (line.find("[SEPARATE]") != std::string::npos) {
        status.stage    = PipelineStage::Extract;
        status.progress = 0.25f;
        status.message  = line.substr(line.find(']') + 2);
    } else if (line.find("[TRANSCRIBE]") != std::string::npos) {
        status.stage    = PipelineStage::Transcribe;
        status.progress = 0.55f;
        status.message  = line.substr(line.find(']') + 2);
    } else if (line.find("[ALIGN]") != std::string::npos) {
        status.stage    = PipelineStage::Align;
        status.progress = 0.80f;
        status.message  = line.substr(line.find(']') + 2);
    } else if (line.find("[DONE]") != std::string::npos) {
        status.stage    = PipelineStage::Done;
        status.progress = 1.0f;
        status.message  = line.substr(line.find(']') + 2);
    } else if (line.find("[ERROR]") != std::string::npos) {
        status.stage   = PipelineStage::Error;
        status.error   = line;
    }
}

void transcribe_start(
    const std::string& audio_path,
    const std::string& python_path,
    const std::string& pipeline_script,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav)
{
    if (g_running.load()) return;

    g_cancel.store(false);
    g_running.store(true);

    g_thread = std::thread([&]() {
        // Build output paths alongside the audio file
        fs::path audio(audio_path);
        fs::path outdir = audio.parent_path() / audio.stem();
        fs::create_directories(outdir);

        std::string json_path  = (outdir / (audio.stem().string() + ".json")).string();
        std::string vocals_out = (outdir / "vocals.wav").string();

        out_words_json = json_path;
        out_vocals_wav = vocals_out;

        // Build command
        std::ostringstream cmd;
        cmd << "\"" << python_path << "\" "
            << "\"" << pipeline_script << "\" "
            << "\"" << audio_path << "\" "
            << "--output \"" << json_path << "\" "
            << "--vocals-out \"" << vocals_out << "\" "
            << "2>&1";  // merge stderr into stdout so we can read progress

        status.stage    = PipelineStage::Extract;
        status.progress = 0.01f;
        status.message  = "Starting pipeline…";

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            status.stage = PipelineStage::Error;
            status.error = "Failed to launch ml_pipeline.py";
            g_running.store(false);
            return;
        }

        char buf[1024];
        while (fgets(buf, sizeof(buf), pipe)) {
            if (g_cancel.load()) {
                pclose(pipe);
                status.stage   = PipelineStage::Idle;
                status.message = "Cancelled";
                g_running.store(false);
                return;
            }
            std::string line(buf);
            // strip trailing newline
            if (!line.empty() && line.back() == '\n') line.pop_back();
            parse_line(line, status);
        }

        int ret = pclose(pipe);
        if (ret != 0 && status.stage != PipelineStage::Done) {
            status.stage = PipelineStage::Error;
            status.error = "Pipeline exited with code " + std::to_string(ret);
        }

        g_running.store(false);
    });

    g_thread.detach();
}

void transcribe_cancel() {
    g_cancel.store(true);
}

bool transcribe_running() {
    return g_running.load();
}
