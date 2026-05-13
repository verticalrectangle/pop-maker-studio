#include "transcribe.h"
#include "globals.h"
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

static std::string find_pipeline_python() {
    static const char* alts[] = {
        "/home/alexis/dev/song2subs/venv/bin/python3",
        nullptr
    };
    for (const char** a = alts; *a; a++)
        if (fs::exists(*a)) return *a;
    return "python3";
}

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
        status.stage = PipelineStage::Error;
        status.error = line;
    }
}

void transcribe_start(
    const std::string& audio_path,
    PipelineStatus&    status,
    std::string&       out_words_json,
    std::string&       out_vocals_wav,
    PipelineMode       mode)
{
    if (g_running.load()) return;
    g_cancel.store(false);
    g_running.store(true);

    g_thread = std::thread([&, mode]() {
        fs::path audio(audio_path);
        fs::path outdir = audio.parent_path() / audio.stem();
        fs::create_directories(outdir);

        std::string words_json = (outdir / (audio.stem().string() + "_words.json")).string();
        std::string vocals_out = (outdir / "vocals.wav").string();

        out_words_json = words_json;
        out_vocals_wav = vocals_out;

        const char* mode_str = (mode == PipelineMode::SeparateOnly)   ? "separate"   :
                               (mode == PipelineMode::TranscribeOnly)  ? "transcribe" :
                                                                          "both";

        std::string py = find_pipeline_python();

        std::ostringstream cmd;
        cmd << "\"" << py                   << "\" "
            << "\"" << g_pipeline_script    << "\" "
            << "\"" << audio_path           << "\" "
            << "--outdir \"" << outdir.string() << "\" "
            << "--mode " << mode_str
            << " 2>&1";

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
            if (!line.empty() && line.back() == '\n') line.pop_back();
            status.raw_line = line;
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

void transcribe_cancel() { g_cancel.store(true); }
bool transcribe_running() { return g_running.load(); }
