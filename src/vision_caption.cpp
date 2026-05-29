#include "vision_caption.h"
#include "paths.h"
#include "json.hpp"
#include "stb_image.h"

#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <thread>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <climits>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── GPT-2 byte-level BPE tokenizer ───────────────────────────────────────────

// bytes_to_unicode: maps each byte 0-255 to a unique unicode codepoint.
// Printable ASCII (33-126) and Latin-1 supplements (161-172, 174-255) map to
// themselves.  The remaining 36 bytes map to codepoints starting at 256.
static void init_byte_maps(uint32_t b2u[256],
                            std::unordered_map<uint32_t, uint8_t>& u2b) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        bool direct = (b >= 33 && b <= 126)
                   || (b >= 161 && b <= 172)
                   || (b >= 174 && b <= 255);
        b2u[b] = direct ? (uint32_t)b : (uint32_t)(256 + n++);
        u2b[b2u[b]] = (uint8_t)b;
    }
}

// Encode a unicode codepoint as UTF-8.
static std::string cp_to_utf8(uint32_t cp) {
    if (cp < 0x80)
        return { (char)cp };
    if (cp < 0x800)
        return { (char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F)) };
    if (cp < 0x10000)
        return { (char)(0xE0 | (cp >> 12)),
                 (char)(0x80 | ((cp >> 6) & 0x3F)),
                 (char)(0x80 | (cp & 0x3F)) };
    return {};
}

// Split a UTF-8 string into individual UTF-8 characters.
static std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

// Apply BPE merges to a sequence of UTF-8 character tokens.
static std::vector<std::string> bpe_encode(
        const std::vector<std::string>& chars,
        const std::map<std::pair<std::string,std::string>, int>& merge_rank) {

    if (chars.size() <= 1) return chars;
    std::vector<std::string> word = chars;

    while (word.size() > 1) {
        int best_rank = INT_MAX, best_i = -1;
        for (int i = 0; i < (int)word.size() - 1; i++) {
            auto it = merge_rank.find({word[i], word[i+1]});
            if (it != merge_rank.end() && it->second < best_rank) {
                best_rank = it->second;
                best_i    = i;
            }
        }
        if (best_i < 0) break;
        word[best_i] += word[best_i + 1];
        word.erase(word.begin() + best_i + 1);
    }
    return word;
}

// Pre-tokenise ASCII text into word-level strings in the same way GPT-2 does:
// words (optionally preceded by a space), runs of newlines, lone punctuation.
static std::vector<std::string> pre_tokenize(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        // Newlines / carriage-returns
        if (c == '\n' || c == '\r') {
            std::string tok;
            while (i < text.size() && (text[i] == '\n' || text[i] == '\r'))
                tok += text[i++];
            out.push_back(tok);
            continue;
        }
        // Space followed by alphanumerics → one token with leading space
        if (c == ' ') {
            std::string tok = " ";
            i++;
            while (i < text.size() && (isalpha((unsigned char)text[i])
                                     || isdigit((unsigned char)text[i])))
                tok += text[i++];
            out.push_back(tok);
            continue;
        }
        // Run of alphanumerics (no leading space)
        if (isalpha((unsigned char)c) || isdigit((unsigned char)c)) {
            std::string tok;
            while (i < text.size() && (isalpha((unsigned char)text[i])
                                     || isdigit((unsigned char)text[i])))
                tok += text[i++];
            out.push_back(tok);
            continue;
        }
        // Single character (punctuation, control, etc.)
        out.push_back(std::string(1, c));
        i++;
    }
    return out;
}

// ── Model sessions (lazy singleton) ──────────────────────────────────────────

static const int IMG_SIZE   = 378;
static const int MAX_TOKENS = 64;

// SigLIP normalisation (mean=0.5, std=0.5 → maps [0,255] to [-1,1])
static const float SIGLIP_MEAN = 0.5f;
static const float SIGLIP_STD  = 0.5f;

struct MoondreamSessions {
    Ort::Env env { ORT_LOGGING_LEVEL_ERROR, "moondream" };
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> encoder;
    std::unique_ptr<Ort::Session> decoder;

    // Tokenizer
    uint32_t byte_enc[256];
    std::unordered_map<uint32_t, uint8_t> byte_dec;
    std::unordered_map<std::string, int32_t> vocab;
    std::unordered_map<int32_t, std::string> id2str;
    std::map<std::pair<std::string,std::string>, int> merge_rank;

    int32_t eos_id   = -1;
    int32_t image_id = -1;

    // Encoder tensor names (queried at load time)
    std::string enc_in_name;
    std::string enc_out_name;

    // Decoder tensor names
    std::vector<std::string> dec_in_names;
    std::vector<std::string> dec_out_names;

    // Indices into dec_in_names / dec_out_names
    int idx_input_ids    = -1;
    int idx_attn_mask    = -1;
    int idx_img_feat     = -1;
    int idx_use_cache    = -1;
    int idx_logits_out   = -1;

    // KV cache: pairs of (past_input_idx, present_output_idx)
    struct KVPair { int in_idx; int out_idx; };
    std::vector<KVPair> kv_pairs;

    int64_t kv_heads = 32;
    int64_t kv_dim   = 64;
};

static std::unique_ptr<MoondreamSessions> g_sess;
static std::mutex                          g_sess_mu;
static std::string                         g_sess_err;

static bool load_tokenizer(MoondreamSessions& s) {
    std::string tok_path = app_models_dir() + "/moondream-tokenizer.json";
    if (!fs::exists(tok_path)) {
        g_sess_err = "moondream-tokenizer.json not found in models/";
        return false;
    }

    std::ifstream f(tok_path);
    json j; f >> j;

    // Build byte maps
    init_byte_maps(s.byte_enc, s.byte_dec);

    // Vocabulary
    const auto& vocab_j = j["model"]["vocab"];
    for (auto it = vocab_j.begin(); it != vocab_j.end(); ++it) {
        int32_t id = it.value().get<int32_t>();
        s.vocab[it.key()] = id;
        s.id2str[id]      = it.key();
    }

    // Merges
    for (auto& m : j["model"]["merges"]) {
        std::string s1 = m.get<std::string>();
        size_t sp = s1.find(' ');
        if (sp != std::string::npos) {
            std::string a = s1.substr(0, sp), b = s1.substr(sp + 1);
            s.merge_rank[{a, b}] = (int)s.merge_rank.size();
        }
    }

    // Special tokens
    if (j.contains("added_tokens")) {
        for (auto& at : j["added_tokens"]) {
            int32_t id = at.value("id", -1);
            std::string content = at.value("content", "");
            if (content == "<|endoftext|>" || content == "</s>" || content == "<eos>")
                s.eos_id = id;
            if (content == "<image>")
                s.image_id = id;
            // Also add to vocab
            if (id >= 0 && !content.empty()) {
                s.vocab[content] = id;
                s.id2str[id]     = content;
            }
        }
    }
    // Fallback EOS
    if (s.eos_id < 0) {
        auto it = s.vocab.find("<|endoftext|>");
        if (it != s.vocab.end()) s.eos_id = it->second;
    }

    return true;
}

// Tokenise a plain-text string (no special tokens).
static std::vector<int32_t> tokenize_text(const MoondreamSessions& s,
                                           const std::string& text) {
    std::vector<int32_t> ids;
    for (auto& word : pre_tokenize(text)) {
        // Byte-level encode
        std::string encoded;
        for (unsigned char c : word)
            encoded += cp_to_utf8(s.byte_enc[c]);
        // BPE
        auto chars = utf8_chars(encoded);
        auto tokens = bpe_encode(chars, s.merge_rank);
        for (auto& t : tokens) {
            auto it = s.vocab.find(t);
            if (it != s.vocab.end())
                ids.push_back(it->second);
        }
    }
    return ids;
}

// Decode a token ID to its original byte string.
static std::string decode_token(const MoondreamSessions& s, int32_t id) {
    auto it = s.id2str.find(id);
    if (it == s.id2str.end()) return "";
    // Convert byte-level unicode chars back to bytes
    std::string result;
    auto chars = utf8_chars(it->second);
    for (auto& ch : chars) {
        // Decode UTF-8 to codepoint
        uint32_t cp = 0;
        unsigned char first = (unsigned char)ch[0];
        if (first < 0x80)      cp = first;
        else if (first < 0xE0) cp = ((first & 0x1F) << 6)  | ((unsigned char)ch[1] & 0x3F);
        else if (first < 0xF0) cp = ((first & 0x0F) << 12) | (((unsigned char)ch[1] & 0x3F) << 6) | ((unsigned char)ch[2] & 0x3F);
        auto b = s.byte_dec.find(cp);
        if (b != s.byte_dec.end())
            result += (char)b->second;
        else
            result += ch;  // pass through (shouldn't happen)
    }
    return result;
}

static bool discover_decoder_names(MoondreamSessions& s) {
    Ort::AllocatorWithDefaultOptions alloc;
    size_t n_in  = s.decoder->GetInputCount();
    size_t n_out = s.decoder->GetOutputCount();

    s.dec_in_names.reserve(n_in);
    for (size_t i = 0; i < n_in; i++)
        s.dec_in_names.push_back(s.decoder->GetInputNameAllocated(i, alloc).get());

    s.dec_out_names.reserve(n_out);
    for (size_t i = 0; i < n_out; i++)
        s.dec_out_names.push_back(s.decoder->GetOutputNameAllocated(i, alloc).get());

    // Identify well-known inputs by name substring
    for (int i = 0; i < (int)n_in; i++) {
        const auto& nm = s.dec_in_names[i];
        if (nm == "input_ids")
            s.idx_input_ids = i;
        else if (nm == "attention_mask")
            s.idx_attn_mask = i;
        else if (nm.find("image") != std::string::npos ||
                 nm.find("encoder") != std::string::npos)
            s.idx_img_feat = i;
        else if (nm.find("cache_branch") != std::string::npos ||
                 nm.find("use_cache") != std::string::npos)
            s.idx_use_cache = i;
    }

    // Logits output
    for (int i = 0; i < (int)n_out; i++) {
        if (s.dec_out_names[i].find("logits") != std::string::npos) {
            s.idx_logits_out = i;
            break;
        }
    }
    if (s.idx_logits_out < 0) s.idx_logits_out = 0;  // fallback: first output

    // Identify KV cache pairs: present.N.* output ↔ past_key_values.N.* input
    std::unordered_map<std::string, int> present_out;  // suffix → out_idx
    for (int i = 0; i < (int)n_out; i++) {
        const auto& nm = s.dec_out_names[i];
        if (i == s.idx_logits_out) continue;
        // Strip common prefixes to get the key suffix used for matching
        std::string key = nm;
        for (auto& pfx : {"present.", "past_key_values."}) {
            if (key.rfind(pfx, 0) == 0) { key = key.substr(strlen(pfx)); break; }
        }
        present_out[key] = i;
    }
    for (int i = 0; i < (int)n_in; i++) {
        if (i == s.idx_input_ids || i == s.idx_attn_mask ||
            i == s.idx_img_feat  || i == s.idx_use_cache) continue;
        const auto& nm = s.dec_in_names[i];
        std::string key = nm;
        for (auto& pfx : {"past_key_values.", "present."}) {
            if (key.rfind(pfx, 0) == 0) { key = key.substr(strlen(pfx)); break; }
        }
        auto it = present_out.find(key);
        if (it != present_out.end())
            s.kv_pairs.push_back({i, it->second});
    }

    // Discover KV shape from first KV input
    if (!s.kv_pairs.empty()) {
        auto ti = s.decoder->GetInputTypeInfo(s.kv_pairs[0].in_idx);
        auto sh = ti.GetTensorTypeAndShapeInfo().GetShape();
        // Expected shape: [batch, num_heads, past_len, head_dim]
        if (sh.size() == 4) {
            if (sh[1] > 0) s.kv_heads = sh[1];
            if (sh[3] > 0) s.kv_dim   = sh[3];
        }
    }

    return s.idx_input_ids >= 0;
}

static MoondreamSessions* get_sessions() {
    std::lock_guard<std::mutex> lk(g_sess_mu);
    if (g_sess) return g_sess.get();

    std::string enc_path = app_models_dir() + "/moondream-encoder.onnx";
    std::string dec_path = app_models_dir() + "/moondream-decoder.onnx";

    if (!fs::exists(enc_path) || !fs::exists(dec_path)) {
        g_sess_err = "Moondream2 ONNX models not found in models/";
        return nullptr;
    }

    auto s = std::make_unique<MoondreamSessions>();
    s->opts.SetIntraOpNumThreads((int)std::thread::hardware_concurrency());
    s->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (!load_tokenizer(*s)) return nullptr;

    try {
        s->encoder = std::make_unique<Ort::Session>(s->env, enc_path.c_str(), s->opts);
        s->decoder = std::make_unique<Ort::Session>(s->env, dec_path.c_str(), s->opts);
    } catch (const Ort::Exception& e) {
        g_sess_err = std::string("ONNX load failed: ") + e.what();
        return nullptr;
    }

    // Encoder input/output names
    {
        Ort::AllocatorWithDefaultOptions alloc;
        s->enc_in_name  = s->encoder->GetInputNameAllocated(0, alloc).get();
        s->enc_out_name = s->encoder->GetOutputNameAllocated(0, alloc).get();
    }

    if (!discover_decoder_names(*s)) {
        g_sess_err = "Could not identify input_ids tensor in decoder";
        return nullptr;
    }

    g_sess = std::move(s);
    return g_sess.get();
}

// ── Inference ─────────────────────────────────────────────────────────────────

SceneResult caption_frame(const std::string& jpeg_path) {
    MoondreamSessions* s = get_sessions();
    if (!s) return {};

    // ── Load and preprocess image ────────────────────────────────────────────
    int w, h, ch;
    uint8_t* raw = stbi_load(jpeg_path.c_str(), &w, &h, &ch, 3);
    if (!raw) return {};

    // Bilinear resize to IMG_SIZE × IMG_SIZE → CHW float32 with SigLIP norm
    std::vector<float> img_data(3 * IMG_SIZE * IMG_SIZE);
    float xs = (float)w / IMG_SIZE, ys = (float)h / IMG_SIZE;
    for (int dy = 0; dy < IMG_SIZE; dy++) {
        float fy = (dy + 0.5f) * ys - 0.5f;
        int y0 = std::max(0, std::min((int)fy,     h - 1));
        int y1 = std::max(0, std::min((int)fy + 1, h - 1));
        float wy = fy - (int)fy;
        for (int dx = 0; dx < IMG_SIZE; dx++) {
            float fx = (dx + 0.5f) * xs - 0.5f;
            int x0 = std::max(0, std::min((int)fx,     w - 1));
            int x1 = std::max(0, std::min((int)fx + 1, w - 1));
            float wx = fx - (int)fx;
            for (int c = 0; c < 3; c++) {
                float v = (1-wy)*((1-wx)*raw[(y0*w+x0)*3+c] + wx*raw[(y0*w+x1)*3+c])
                        +    wy *((1-wx)*raw[(y1*w+x0)*3+c] + wx*raw[(y1*w+x1)*3+c]);
                img_data[c * IMG_SIZE * IMG_SIZE + dy * IMG_SIZE + dx] =
                    (v / 255.f - SIGLIP_MEAN) / SIGLIP_STD;
            }
        }
    }
    stbi_image_free(raw);

    auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── Run encoder ──────────────────────────────────────────────────────────
    int64_t enc_shape[] = {1, 3, IMG_SIZE, IMG_SIZE};
    Ort::Value enc_in = Ort::Value::CreateTensor<float>(
        mem_info, img_data.data(), img_data.size(), enc_shape, 4);

    const char* enc_in_names[]  = { s->enc_in_name.c_str() };
    const char* enc_out_names[] = { s->enc_out_name.c_str() };

    std::vector<Ort::Value> enc_out;
    try {
        enc_out = s->encoder->Run(Ort::RunOptions{nullptr},
                                   enc_in_names, &enc_in, 1,
                                   enc_out_names, 1);
    } catch (const Ort::Exception& e) {
        g_sess_err = std::string("Encoder run failed: ") + e.what();
        return {};
    }

    // Image features: [1, num_patches, hidden_dim]
    auto img_feat_shape = enc_out[0].GetTensorTypeAndShapeInfo().GetShape();
    float* img_feat_ptr  = enc_out[0].GetTensorMutableData<float>();
    size_t img_feat_size = 1;
    for (auto d : img_feat_shape) img_feat_size *= (size_t)d;

    // ── Build prompt token IDs ───────────────────────────────────────────────
    // Moondream prompt: <image>\n\nQuestion: {q}\n\nAnswer:
    static const std::string QUESTION =
        "\n\nQuestion: Describe what is happening in this scene in one sentence.\n\nAnswer:";

    std::vector<int32_t> text_ids = tokenize_text(*s, QUESTION);

    // Prepend <image> token if we know its ID
    std::vector<int32_t> prompt_ids;
    if (s->image_id >= 0)
        prompt_ids.push_back(s->image_id);
    prompt_ids.insert(prompt_ids.end(), text_ids.begin(), text_ids.end());

    if (prompt_ids.empty()) return {};

    // ── Autoregressive decode ────────────────────────────────────────────────
    // Number of image "tokens" as seen by the decoder's attention mask
    int64_t num_img_tokens = (img_feat_shape.size() >= 2) ? img_feat_shape[1] : 0;

    // Convert prompt_ids to int64
    std::vector<int64_t> input_ids_data(prompt_ids.begin(), prompt_ids.end());

    // Build named input/output vectors for the decoder
    int n_dec_in  = (int)s->dec_in_names.size();
    int n_dec_out = (int)s->dec_out_names.size();

    std::vector<const char*> dec_in_cnames(n_dec_in),  dec_out_cnames(n_dec_out);
    for (int i = 0; i < n_dec_in;  i++) dec_in_cnames[i]  = s->dec_in_names[i].c_str();
    for (int i = 0; i < n_dec_out; i++) dec_out_cnames[i] = s->dec_out_names[i].c_str();

    // KV cache: start with empty tensors (past_len = 0)
    int n_kv = (int)s->kv_pairs.size();
    std::vector<std::vector<float>> kv_data(n_kv * 2);  // key and value separate
    // (will be populated from decoder outputs after each step)

    std::string output_text;

    static float s_kv_dummy = 0.f;  // backing for zero-size KV tensors

    for (int step = 0; step < MAX_TOKENS; step++) {
        bool prefill = (step == 0);

        int64_t text_len = (int64_t)input_ids_data.size();
        int64_t past_len = (!kv_data.empty() && !kv_data[0].empty())
                           ? (int64_t)(kv_data[0].size() / (s->kv_heads * s->kv_dim))
                           : 0LL;
        int64_t full_len = num_img_tokens + past_len + text_len;

        // Assemble decoder inputs in the same order as dec_in_names.
        // We build each tensor inline so no Ort::Value needs to be null.
        std::vector<int64_t> attn_data(full_len, 1LL);
        uint8_t cache_val = prefill ? 0u : 1u;

        std::vector<Ort::Value> dec_inputs;
        dec_inputs.reserve(n_dec_in);

        for (int i = 0; i < n_dec_in; i++) {
            if (i == s->idx_input_ids) {
                int64_t sh[] = {1, text_len};
                dec_inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                    mem_info, input_ids_data.data(), (size_t)text_len, sh, 2));
            } else if (i == s->idx_attn_mask) {
                int64_t sh[] = {1, full_len};
                dec_inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                    mem_info, attn_data.data(), (size_t)full_len, sh, 2));
            } else if (i == s->idx_img_feat) {
                dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                    mem_info, img_feat_ptr, img_feat_size,
                    img_feat_shape.data(), img_feat_shape.size()));
            } else if (i == s->idx_use_cache) {
                // Scalar bool tensor (0 dimensions)
                dec_inputs.push_back(Ort::Value::CreateTensor(
                    mem_info, &cache_val, sizeof(cache_val),
                    nullptr, 0,
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
            } else {
                // KV cache input
                int k = -1;
                for (int j = 0; j < n_kv; j++)
                    if (s->kv_pairs[j].in_idx == i) { k = j; break; }
                if (k >= 0) {
                    std::vector<float>& buf = kv_data[k];
                    size_t expected = (size_t)(s->kv_heads * past_len * s->kv_dim);
                    if (buf.size() != expected) buf.assign(expected, 0.f);
                    float* ptr = buf.empty() ? &s_kv_dummy : buf.data();
                    int64_t kv_sh[] = {1, s->kv_heads, past_len, s->kv_dim};
                    dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                        mem_info, ptr, buf.size(), kv_sh, 4));
                } else {
                    // Unknown optional input — provide a single-element zero float
                    int64_t sh[] = {1};
                    dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                        mem_info, &s_kv_dummy, 1, sh, 1));
                }
            }
        }

        // Run decoder
        std::vector<Ort::Value> dec_outs;
        try {
            dec_outs = s->decoder->Run(Ort::RunOptions{nullptr},
                                        dec_in_cnames.data(), dec_inputs.data(), n_dec_in,
                                        dec_out_cnames.data(), n_dec_out);
        } catch (const Ort::Exception& e) {
            // Tolerate failures after we've got some output
            (void)e;
            break;
        }

        // Extract next-token from logits [1, seq, vocab]
        float* logits     = dec_outs[s->idx_logits_out].GetTensorMutableData<float>();
        auto   l_shape    = dec_outs[s->idx_logits_out].GetTensorTypeAndShapeInfo().GetShape();
        int64_t vocab_sz  = l_shape.back();
        int64_t last_row  = (l_shape.size() >= 3) ? (l_shape[1] - 1) : 0;
        float*  last_logits = logits + last_row * vocab_sz;

        int32_t next_id = (int32_t)(std::max_element(last_logits, last_logits + vocab_sz)
                                    - last_logits);

        // Update KV cache from present tensors
        for (int k = 0; k < n_kv; k++) {
            int out_idx = s->kv_pairs[k].out_idx;
            auto  sh    = dec_outs[out_idx].GetTensorTypeAndShapeInfo().GetShape();
            float* ptr  = dec_outs[out_idx].GetTensorMutableData<float>();
            size_t sz   = 1; for (auto d : sh) sz *= (size_t)d;
            kv_data[k].assign(ptr, ptr + sz);
            if (sh.size() == 4) { s->kv_heads = sh[1]; s->kv_dim = sh[3]; }
        }

        if (next_id == s->eos_id || next_id == 0) break;

        std::string tok = decode_token(*s, next_id);
        output_text += tok;

        // Next step: single token
        input_ids_data = {(int64_t)next_id};
    }

    // Trim leading/trailing whitespace
    size_t a = output_text.find_first_not_of(" \t\n\r");
    size_t b = output_text.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return { true, output_text.substr(a, b - a + 1) };
}
