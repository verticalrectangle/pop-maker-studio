#pragma once
// In-app agent harness — chat loop against an OpenAI-compatible API
// (DeepSeek by default), dispatching tool calls to the app's own IPC socket.
// See AGENT_HARNESS.md. UI lives in ui/panel_agent.cpp.
#include <string>
#include <vector>

enum class AgentRole { User, Assistant, Tool, Error, Info, Image };

struct AgentRow {
    AgentRole   role;
    std::string text;          // user/assistant text, or one-line tool summary
    std::string detail;        // full tool args + result (expand on click)
    bool        streaming = false;  // assistant row still receiving deltas
};

struct AgentConfig {
    std::string base_url = "https://api.deepseek.com";
    std::string model    = "deepseek-chat";
    bool        vision   = false;  // ship snapshot images to the model
};

// All functions are safe to call from the UI thread.
void agent_send(const std::string& user_text);  // no-op while a turn runs
bool agent_running();
void agent_stop();                               // cancel the current turn
std::vector<AgentRow> agent_rows_snapshot();     // copy of the transcript
void agent_clear();                              // wipe transcript + history
void agent_shutdown();                           // join worker (app exit)

AgentConfig agent_get_config();
void        agent_set_config(const AgentConfig& cfg);

// DeepSeek/API key via the Secret Service (secret-tool subprocess).
// The key never touches the settings file.
bool agent_key_available();                      // secret-tool binary exists
bool agent_key_present();                        // a key is stored
bool agent_key_store(const std::string& key);
bool agent_key_clear();
