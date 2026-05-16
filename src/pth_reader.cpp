// pth_reader.cpp — parse PyTorch .pth (zip+pickle) without Python or libtorch.
//
// Format overview:
//   .pth file is a zip archive named "{model}/..."
//   ├── data.pkl         — pickle stream describing the weight dict + config
//   ├── data/0           — raw tensor bytes for storage "0"
//   ├── data/1           — raw tensor bytes for storage "1"
//   └── ...
//
// Pickle opcodes used by PyTorch (protocol 2):
//   PROTO, GLOBAL, BINPUT/LONG_BINPUT, BINGET/LONG_BINGET,
//   MARK, TUPLE/TUPLE1/TUPLE2/TUPLE3, EMPTY_TUPLE,
//   REDUCE, BINPERSID, BININT1/BININT2/BININT, BINUNICODE,
//   EMPTY_DICT, SETITEMS, SETITEM, EMPTY_LIST, APPENDS, APPEND,
//   NEWTRUE, NEWFALSE, NONE, BINFLOAT, LIST, DICT, POP, POP_MARK, STOP
//
// The top-level object is an OrderedDict with keys:
//   'weight'  → dict of name → tensor (via _rebuild_tensor_v2 + BINPERSID)
//   'config'  → list of architecture params (ints, strings, nested lists)
//   'sr', 'f0', 'version', 'epoch', 'step', … (ignored)

#include "pth_reader.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

// ── Pickle value type ─────────────────────────────────────────────────────────

struct Val {
    enum Kind {
        None = 0, Bool, Int, Float, Str,
        List,    // also used for tuples
        Dict,    // vector of (key,val) pairs
        Global,  // module.name reference
        Storage, // persistent storage descriptor
        Tensor,  // _rebuild_tensor_v2 result
        Mark,    // internal mark sentinel
    };
    Kind kind = None;

    bool        b = false;
    int64_t     i = 0;
    double      f = 0.0;
    std::string s;

    // Global: s = name, extra = module
    std::string extra;

    // List / Dict
    std::vector<Val>                 items;
    std::vector<std::pair<Val, Val>> pairs;

    // Storage descriptor (from BINPERSID)
    // s      = storage_class ("HalfStorage", "FloatStorage", …)
    // extra  = file_idx ("0", "1", …)
    // i      = num_elements

    // Tensor (from _rebuild_tensor_v2)
    // extra  = dtype ("f16", "f32", …)
    // s      = storage_file
    // items  = shape (as Val::Int)
    // pairs[0].first = stride packed into pairs for reuse
    int64_t storage_offset = 0;
    // shape and stride stored in items / secondary_items to avoid collision:
    std::vector<int64_t> shape, stride;

    static Val make_none()              { Val v; v.kind = None; return v; }
    static Val make_bool(bool b_)       { Val v; v.kind = Bool; v.b = b_; return v; }
    static Val make_int(int64_t i_)     { Val v; v.kind = Int;  v.i = i_; return v; }
    static Val make_float(double f_)    { Val v; v.kind = Float; v.f = f_; return v; }
    static Val make_str(std::string s_) { Val v; v.kind = Str;  v.s = std::move(s_); return v; }
    static Val make_mark()              { Val v; v.kind = Mark; return v; }
    static Val make_list(std::vector<Val> it) { Val v; v.kind = List; v.items = std::move(it); return v; }
    static Val make_dict() { Val v; v.kind = Dict; return v; }
};

// ── Pickle VM ─────────────────────────────────────────────────────────────────

struct Unpickler {
    const uint8_t*   data;
    size_t           pos, size;
    std::vector<Val> stack;
    std::unordered_map<int, Val> memo;
    int              memo_next = 0; // for MEMOIZE (protocol 4)
    std::string      err;

    // ── Low-level reads ───────────────────────────────────────────────────────

    bool ok() const { return err.empty() && pos <= size; }

    uint8_t read1() {
        if (pos >= size) { err = "unexpected EOF"; return 0; }
        return data[pos++];
    }
    int32_t read_i32_le() {
        int32_t v = 0; memcpy(&v, data + pos, 4); pos += 4; return v;
    }
    uint16_t read_u16_le() {
        uint16_t v = 0; memcpy(&v, data + pos, 2); pos += 2; return v;
    }
    double read_f64_be() {
        uint8_t buf[8]; memcpy(buf, data + pos, 8); pos += 8;
        std::reverse(buf, buf + 8);
        double v; memcpy(&v, buf, 8); return v;
    }
    std::string read_bytes(size_t n) {
        std::string s((const char*)data + pos, n); pos += n; return s;
    }
    std::string read_line() {
        std::string s;
        while (pos < size && data[pos] != '\n') s += (char)data[pos++];
        ++pos; // skip '\n'
        return s;
    }

    // ── Stack helpers ─────────────────────────────────────────────────────────

    Val pop() {
        if (stack.empty()) { err = "pop from empty stack"; return Val::make_none(); }
        Val v = std::move(stack.back()); stack.pop_back(); return v;
    }

    std::vector<Val> pop_to_mark() {
        std::vector<Val> out;
        while (!stack.empty() && stack.back().kind != Val::Mark) {
            out.push_back(std::move(stack.back())); stack.pop_back();
        }
        if (!stack.empty()) stack.pop_back(); // consume mark
        std::reverse(out.begin(), out.end());
        return out;
    }

    void memo_put(int k) {
        if (!stack.empty()) memo[k] = stack.back();
    }

    // ── Persistent load (BINPERSID) ───────────────────────────────────────────
    // PyTorch encodes storage as a tuple:
    //   ('storage', StorageClass, file_idx_str, device_str, num_elements)
    Val persistent_load(const Val& key) {
        if (key.kind != Val::List || key.items.size() < 5) return Val::make_none();
        const auto& items = key.items;
        // items[0] = 'storage', items[1] = Global(torch, XxxStorage),
        // items[2] = file_idx, items[3] = device (ignored), items[4] = num_elements
        Val v;
        v.kind = Val::Storage;
        if (items[1].kind == Val::Global) v.s = items[1].s; // class name
        if (items[2].kind == Val::Str)    v.extra = items[2].s; // file index
        if (items[4].kind == Val::Int)    v.i = items[4].i;
        return v;
    }

    // ── REDUCE dispatch ───────────────────────────────────────────────────────
    Val reduce(const Val& func, const Val& args) {
        if (func.kind != Val::Global) return Val::make_none();

        const auto& fn = func.s; // name part

        // _rebuild_tensor_v2(storage, offset, shape, stride, requires_grad, hooks)
        if (fn == "_rebuild_tensor_v2") {
            if (args.kind != Val::List || args.items.size() < 4) return Val::make_none();
            const Val& stor  = args.items[0];
            Val t;
            t.kind = Val::Tensor;
            if (stor.kind == Val::Storage) {
                // dtype from storage class
                const std::string& cls = stor.s;
                if      (cls == "HalfStorage")   t.extra = "f16";
                else if (cls == "FloatStorage")  t.extra = "f32";
                else if (cls == "DoubleStorage") t.extra = "f64";
                else if (cls == "LongStorage")   t.extra = "i64";
                else if (cls == "IntStorage")    t.extra = "i32";
                else if (cls == "ShortStorage")  t.extra = "i16";
                else if (cls == "ByteStorage")   t.extra = "u8";
                else                             t.extra = "f32";
                t.s = stor.extra; // file index
                t.i = stor.i;     // num_elements (storage size)
            }
            if (args.items[1].kind == Val::Int) t.storage_offset = args.items[1].i;
            const Val& shp = args.items[2];
            if (shp.kind == Val::List)
                for (auto& d : shp.items)
                    if (d.kind == Val::Int) t.shape.push_back(d.i);
            const Val& str = args.items[3];
            if (str.kind == Val::List)
                for (auto& d : str.items)
                    if (d.kind == Val::Int) t.stride.push_back(d.i);
            return t;
        }

        // OrderedDict or dict — just return a Dict
        if (fn == "OrderedDict") {
            Val d = Val::make_dict();
            // args may be empty tuple, or list of pairs
            if (args.kind == Val::List)
                for (auto& it : args.items)
                    if (it.kind == Val::List && it.items.size() == 2)
                        d.pairs.push_back({it.items[0], it.items[1]});
            return d;
        }

        // Anything else we don't know how to call — return None
        return Val::make_none();
    }

    // ── Main dispatch loop ────────────────────────────────────────────────────
    Val run() {
        while (ok() && pos < size) {
            uint8_t op = read1();
            switch (op) {
            case 0x80: read1(); break; // PROTO — skip version byte
            case 0x95: pos += 8; break; // FRAME (protocol 4) — skip 8-byte length
            case 0x94: { // MEMOIZE (protocol 4) — memo stack top with auto index
                if (!stack.empty()) memo[memo_next++] = stack.back();
                break;
            }
            case 0x93: { // STACK_GLOBAL (protocol 4) — pop name then module
                Val nm = pop(); Val mod = pop();
                Val v; v.kind = Val::Global;
                v.extra = (mod.kind == Val::Str) ? mod.s : "";
                v.s     = (nm.kind  == Val::Str) ? nm.s  : "";
                stack.push_back(std::move(v));
                break;
            }
            case 0x92: { // NEWOBJ_EX — class(*args, **kwargs); treat as NEWOBJ
                Val kwargs = pop(); Val args = pop(); Val cls = pop();
                stack.push_back(reduce(cls, args));
                (void)kwargs;
                break;
            }

            case 'c': { // GLOBAL module\nname\n
                std::string mod = read_line();
                std::string nm  = read_line();
                Val v; v.kind = Val::Global; v.extra = mod; v.s = nm;
                stack.push_back(std::move(v));
                break;
            }

            case 'q': memo_put(read1()); break;                        // BINPUT
            case 'r': memo_put(read_i32_le()); break;                  // LONG_BINPUT
            case 'h': stack.push_back(memo[read1()]); break;           // BINGET
            case 'j': stack.push_back(memo[read_i32_le()]); break;     // LONG_BINGET

            case ')': stack.push_back(Val::make_list({})); break;      // EMPTY_TUPLE
            case 0x85: { Val a = pop(); stack.push_back(Val::make_list({a})); break; } // TUPLE1
            case 0x86: { Val b = pop(), a = pop(); stack.push_back(Val::make_list({a,b})); break; } // TUPLE2
            case 0x87: { Val c = pop(), b = pop(), a = pop(); stack.push_back(Val::make_list({a,b,c})); break; } // TUPLE3
            case 't': { auto it = pop_to_mark(); stack.push_back(Val::make_list(it)); break; } // TUPLE

            case '(': stack.push_back(Val::make_mark()); break;        // MARK

            case 'R': { Val args = pop(); Val fn = pop(); stack.push_back(reduce(fn, args)); break; } // REDUCE
            case 'Q': { Val key = pop(); stack.push_back(persistent_load(key)); break; }              // BINPERSID

            case 'K': stack.push_back(Val::make_int(read1())); break;         // BININT1
            case 'M': stack.push_back(Val::make_int(read_u16_le())); break;   // BININT2
            case 'J': stack.push_back(Val::make_int(read_i32_le())); break;   // BININT

            case 'X': { // BINUNICODE: 4-byte LE length + UTF-8
                uint32_t n = 0; memcpy(&n, data + pos, 4); pos += 4;
                stack.push_back(Val::make_str(read_bytes(n)));
                break;
            }
            case 'U': { // SHORT_BINSTRING
                uint8_t n = read1();
                stack.push_back(Val::make_str(read_bytes(n)));
                break;
            }
            case 'T': { // BINSTRING
                int32_t n = read_i32_le();
                stack.push_back(Val::make_str(read_bytes(n < 0 ? 0 : n)));
                break;
            }

            case '}': stack.push_back(Val::make_dict()); break;        // EMPTY_DICT
            case 'u': { // SETITEMS — pop pairs to mark, add to dict at stack top
                auto it = pop_to_mark();
                Val& d = stack.back();
                for (size_t k = 0; k + 1 < it.size(); k += 2)
                    d.pairs.push_back({it[k], it[k+1]});
                break;
            }
            case 's': { // SETITEM
                Val val = pop(), key = pop();
                stack.back().pairs.push_back({key, val});
                break;
            }

            case ']': stack.push_back(Val::make_list({})); break;      // EMPTY_LIST
            case 'e': { // APPENDS
                auto it = pop_to_mark();
                for (auto& x : it) stack.back().items.push_back(std::move(x));
                break;
            }
            case 'a': { Val item = pop(); stack.back().items.push_back(std::move(item)); break; } // APPEND

            case 'l': { auto it = pop_to_mark(); stack.push_back(Val::make_list(it)); break; } // LIST
            case 'd': { // DICT from mark
                auto it = pop_to_mark();
                Val dv = Val::make_dict();
                for (size_t k = 0; k + 1 < it.size(); k += 2)
                    dv.pairs.push_back({it[k], it[k+1]});
                stack.push_back(std::move(dv));
                break;
            }

            case 0x88: stack.push_back(Val::make_bool(true));  break;  // NEWTRUE
            case 0x89: stack.push_back(Val::make_bool(false)); break;  // NEWFALSE
            case 'N': stack.push_back(Val::make_none()); break;        // NONE
            case 'G': stack.push_back(Val::make_float(read_f64_be())); break; // BINFLOAT

            case '0': if (!stack.empty()) stack.pop_back(); break;     // POP
            case '1': pop_to_mark(); break;                             // POP_MARK

            case 'b': { // BUILD — apply __setstate__; for OrderedDict, merge pairs
                Val state = pop();
                if (!stack.empty() && stack.back().kind == Val::Dict && state.kind == Val::Dict)
                    for (auto& p : state.pairs) stack.back().pairs.push_back(p);
                break;
            }
            case 'o': { // OBJ — pop to mark (args) then pop class
                auto args = pop_to_mark();
                Val cls = pop();
                stack.push_back(reduce(cls, Val::make_list(std::move(args))));
                break;
            }
            case 0x81: { // NEWOBJ — cls(*args)
                Val args = pop(); Val cls = pop();
                stack.push_back(reduce(cls, args));
                break;
            }
            case 'i': { // INST — module\nname\n + MARK args
                read_line(); read_line();
                pop_to_mark();
                stack.push_back(Val::make_none());
                break;
            }
            case 0x82: pos += 1; stack.push_back(Val::make_none()); break; // EXT1
            case 0x83: pos += 2; stack.push_back(Val::make_none()); break; // EXT2
            case 0x84: pos += 4; stack.push_back(Val::make_none()); break; // EXT4
            case 'p': { // PUT n\n — text memo
                std::string key = read_line();
                memo[std::stoi(key)] = stack.empty() ? Val::make_none() : stack.back();
                break;
            }
            case 'g': { // GET n\n
                std::string key = read_line();
                stack.push_back(memo[std::stoi(key)]);
                break;
            }
            case 'F': { // FLOAT n\n
                std::string s = read_line();
                stack.push_back(Val::make_float(std::stod(s)));
                break;
            }
            case 'I': { // INT n\n
                std::string s = read_line();
                if (s == "00") stack.push_back(Val::make_bool(false));
                else if (s == "01") stack.push_back(Val::make_bool(true));
                else stack.push_back(Val::make_int(std::stoll(s)));
                break;
            }
            case 'S': { // STRING 'val'\n
                std::string s = read_line();
                // strip surrounding quotes
                if (s.size() >= 2 && (s[0]=='\'' || s[0]=='"'))
                    s = s.substr(1, s.size()-2);
                stack.push_back(Val::make_str(s));
                break;
            }
            case 'L': { // LONG n\n
                std::string s = read_line();
                if (!s.empty() && s.back() == 'L') s.pop_back();
                stack.push_back(Val::make_int(std::stoll(s)));
                break;
            }
            case 0x8a: { // LONG1 — 1-byte length + that many bytes little-endian
                uint8_t n = read1();
                int64_t v = 0; memcpy(&v, data+pos, std::min((size_t)n, sizeof(v)));
                pos += n;
                stack.push_back(Val::make_int(v));
                break;
            }
            case 0x8b: { // LONG4 — 4-byte LE length + bytes
                int32_t n = read_i32_le();
                int64_t v = 0; memcpy(&v, data+pos, std::min((size_t)n, sizeof(v)));
                pos += n;
                stack.push_back(Val::make_int(v));
                break;
            }
            case 0x8c: { // SHORT_BINUNICODE (1-byte len)
                uint8_t n = read1();
                stack.push_back(Val::make_str(read_bytes(n)));
                break;
            }
            case 0x8d: { // BINUNICODE8 (8-byte len)
                int64_t n = 0; memcpy(&n, data+pos, 8); pos += 8;
                stack.push_back(Val::make_str(read_bytes((size_t)n)));
                break;
            }
            case 0x8e: { // BYTEARRAY8
                int64_t n = 0; memcpy(&n, data+pos, 8); pos += 8;
                pos += (size_t)n; // skip bytes
                stack.push_back(Val::make_none());
                break;
            }
            case 0x96: { // BYTEARRAY8 (protocol 5)
                uint64_t n = 0; memcpy(&n, data+pos, 8); pos += 8;
                pos += (size_t)n;
                stack.push_back(Val::make_none());
                break;
            }

            case '.': // STOP
                return stack.empty() ? Val::make_none() : stack.back();

            default:
                // Unknown opcode — skip (rare in practice for PyTorch .pth)
                break;
            }
        }
        return stack.empty() ? Val::make_none() : stack.back();
    }
};

// ── Config extraction ─────────────────────────────────────────────────────────

static int64_t val_int(const Val& v, int64_t def = 0) {
    if (v.kind == Val::Int)   return v.i;
    if (v.kind == Val::Float) return (int64_t)v.f;
    return def;
}

static std::string val_str(const Val& v, const std::string& def = "") {
    if (v.kind == Val::Str) return v.s;
    return def;
}

static std::vector<int> val_int_list(const Val& v) {
    std::vector<int> out;
    if (v.kind != Val::List) return out;
    for (auto& it : v.items) out.push_back((int)val_int(it));
    return out;
}

// Parse the config list from the pickle value.
// PyTorch saves config as a Python list of positional args to SynthesizerTrnMsNSFsid:
//   [spec_channels, segment_size, inter_channels, hidden_channels,
//    filter_channels, n_heads, n_layers, kernel_size, p_dropout,
//    resblock_str, resblock_kernel_sizes, resblock_dilation_sizes,
//    upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
//    n_speakers, gin_channels, sr]
static RvcConfig parse_config(const Val& cfg) {
    RvcConfig c;
    if (cfg.kind != Val::List) return c;
    const auto& it = cfg.items;
    auto ig = [&](size_t idx) -> int64_t {
        return idx < it.size() ? val_int(it[idx]) : 0;
    };
    auto sg = [&](size_t idx) -> std::string {
        return idx < it.size() ? val_str(it[idx]) : "";
    };
    auto lg = [&](size_t idx) -> std::vector<int> {
        return idx < it.size() ? val_int_list(it[idx]) : std::vector<int>{};
    };

    if (it.size() < 18) return c;

    c.spec_channels          = (int)ig(0);
    c.segment_size           = (int)ig(1);
    c.inter_channels         = (int)ig(2);
    c.hidden_channels        = (int)ig(3);
    c.filter_channels        = (int)ig(4);
    c.n_heads                = (int)ig(5);
    c.n_layers               = (int)ig(6);
    c.kernel_size            = (int)ig(7);
    c.p_dropout              = (float)ig(8);
    c.resblock               = sg(9);

    c.resblock_kernel_sizes  = lg(10);
    if (c.resblock_kernel_sizes.empty()) c.resblock_kernel_sizes = {3,7,11};

    // resblock_dilation_sizes is a list of lists
    if (it[11].kind == Val::List) {
        c.resblock_dilation_sizes.clear();
        for (auto& sub : it[11].items)
            c.resblock_dilation_sizes.push_back(val_int_list(sub));
    }

    c.upsample_rates         = lg(12);
    if (c.upsample_rates.empty()) c.upsample_rates = {10,10,2,2};

    c.upsample_initial_channel = (int)ig(13);
    c.upsample_kernel_sizes  = lg(14);
    if (c.upsample_kernel_sizes.empty()) c.upsample_kernel_sizes = {16,16,4,4};

    c.n_speakers             = (int)ig(15);
    c.gin_channels           = (int)ig(16);
    c.sr                     = (int)ig(17);

    return c;
}

// ── pth_open ─────────────────────────────────────────────────────────────────

PthModel pth_open(const std::string& path) {
    PthModel m;

    // Create temp dir
    std::string tmpdir = "/tmp/pms_pth_XXXXXX";
    if (!mkdtemp(tmpdir.data())) { m.err = "mkdtemp failed"; return m; }
    m.tmpdir = tmpdir;

    // Extract the zip
    std::string cmd = "unzip -q \"" + path + "\" -d \"" + tmpdir + "\" 2>&1";
    if (system(cmd.c_str()) != 0) { // NOLINT
        m.err = "Failed to extract .pth zip: " + path;
        return m;
    }

    // Find the prefix (first subdir)
    std::string prefix;
    for (auto& e : fs::directory_iterator(tmpdir)) {
        if (e.is_directory()) { prefix = e.path().string() + "/"; break; }
    }
    if (prefix.empty()) { m.err = "No directory found in .pth zip"; return m; }

    // Read data.pkl
    std::string pkl_path = prefix + "data.pkl";
    std::ifstream pf(pkl_path, std::ios::binary);
    if (!pf) { m.err = "data.pkl not found in " + path; return m; }
    std::vector<uint8_t> pkl_data((std::istreambuf_iterator<char>(pf)), {});

    // Run pickle VM
    Unpickler up;
    up.data = pkl_data.data();
    up.pos  = 0;
    up.size = pkl_data.size();
    Val root = up.run();
    if (!up.err.empty()) { m.err = "Pickle error: " + up.err; return m; }

    // Root should be an OrderedDict or Dict with keys 'weight', 'config', etc.
    // Find 'weight' and 'config' among top-level pairs.
    const Val* weight_val = nullptr;
    const Val* config_val = nullptr;

    const Val* model_val = nullptr; // fairseq/HuBERT format uses 'model' key
    auto scan_pairs = [&](const std::vector<std::pair<Val,Val>>& pairs) {
        for (auto& p : pairs) {
            if (p.first.kind == Val::Str) {
                if (p.first.s == "weight") weight_val = &p.second;
                if (p.first.s == "model")  model_val  = &p.second;
                if (p.first.s == "config") config_val = &p.second;
            }
        }
    };

    if (root.kind == Val::Dict) {
        scan_pairs(root.pairs);
    } else if (root.kind == Val::List) {
        // Sometimes an OrderedDict is returned as a List of pairs
        for (auto& it : root.items)
            if (it.kind == Val::Dict) { scan_pairs(it.pairs); break; }
    }

    // Parse config
    if (config_val) {
        m.config = parse_config(*config_val);
    }

    // Fall back to 'model' key if no 'weight' found (fairseq/HuBERT format)
    if (!weight_val && model_val) weight_val = model_val;

    // Parse weight dict into TensorMeta
    if (weight_val && weight_val->kind == Val::Dict) {
        for (auto& p : weight_val->pairs) {
            if (p.first.kind != Val::Str) continue;
            if (p.second.kind != Val::Tensor) continue;
            const std::string& name = p.first.s;
            const Val& tv = p.second;

            TensorMeta tm;
            tm.dtype           = tv.extra;
            tm.storage_offset  = tv.storage_offset;
            tm.storage_file    = tv.s;
            tm.num_elements    = tv.i;
            tm.shape           = tv.shape;
            tm.stride          = tv.stride;

            m.tensors[name] = std::move(tm);
        }
    }

    // Infer phone_dim from emb_phone weight shape
    auto it = m.tensors.find("enc_p.emb_phone.weight");
    if (it != m.tensors.end() && it->second.shape.size() >= 2)
        m.config.phone_dim = (int)it->second.shape[1];

    return m;
}

void pth_close(PthModel& m) {
    if (!m.tmpdir.empty()) {
        std::string cmd = "rm -rf \"" + m.tmpdir + "\"";
        system(cmd.c_str()); // NOLINT
        m.tmpdir.clear();
    }
}

// ── Tensor loading ────────────────────────────────────────────────────────────

// Convert float16 bit pattern to float32.
static float f16_to_f32(uint16_t h) {
    uint32_t sign     = (h >> 15) & 1;
    uint32_t exp      = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exp == 0) {
        if (mantissa == 0) { uint32_t bits = sign << 31; float f; memcpy(&f, &bits, 4); return f; }
        // Denormal
        while (!(mantissa & 0x400)) { mantissa <<= 1; exp--; }
        exp++; mantissa &= 0x3FF;
    } else if (exp == 31) {
        uint32_t bits = (sign << 31) | 0x7F800000 | (mantissa << 13);
        float f; memcpy(&f, &bits, 4); return f;
    }
    uint32_t bits = (sign << 31) | ((exp + 112) << 23) | (mantissa << 13);
    float f; memcpy(&f, &bits, 4); return f;
}

std::vector<float> pth_load_tensor(const PthModel& m, const std::string& name) {
    auto it = m.tensors.find(name);
    if (it == m.tensors.end()) return {};
    const TensorMeta& tm = it->second;

    if (m.tmpdir.empty()) return {};

    // Find the data file
    std::string data_file;
    for (auto& e : fs::directory_iterator(m.tmpdir)) {
        if (!e.is_directory()) continue;
        std::string dp = e.path().string() + "/data/" + tm.storage_file;
        if (fs::exists(dp)) { data_file = dp; break; }
    }
    if (data_file.empty()) return {};

    // Compute tensor element count from shape
    int64_t n_elems = 1;
    for (int64_t d : tm.shape) n_elems *= d;
    if (n_elems == 0) return {};

    // Element size
    size_t elem_bytes = 2; // f16 default
    if (tm.dtype == "f32") elem_bytes = 4;
    else if (tm.dtype == "f64") elem_bytes = 8;
    else if (tm.dtype == "i64") elem_bytes = 8;
    else if (tm.dtype == "i32") elem_bytes = 4;
    else if (tm.dtype == "i16") elem_bytes = 2;
    else if (tm.dtype == "u8")  elem_bytes = 1;

    // Read raw bytes from storage file starting at offset
    std::ifstream f(data_file, std::ios::binary);
    if (!f) return {};

    size_t byte_offset = tm.storage_offset * elem_bytes;
    f.seekg((std::streamoff)byte_offset);

    // For strided tensors, we need to handle non-contiguous storage.
    // Most RVC weights are contiguous; check by comparing stride to
    // expected contiguous stride.
    bool contiguous = true;
    if (!tm.stride.empty() && !tm.shape.empty()) {
        int64_t expected = 1;
        for (int d = (int)tm.shape.size() - 1; d >= 0; d--) {
            if (tm.stride[d] != expected) { contiguous = false; break; }
            expected *= tm.shape[d];
        }
    }

    std::vector<float> out(n_elems);

    if (contiguous) {
        // Fast path: read contiguous block and convert
        if (tm.dtype == "f16") {
            std::vector<uint16_t> raw(n_elems);
            f.read((char*)raw.data(), n_elems * 2);
            for (int64_t i = 0; i < n_elems; i++) out[i] = f16_to_f32(raw[i]);
        } else if (tm.dtype == "f32") {
            f.read((char*)out.data(), n_elems * 4);
        } else if (tm.dtype == "i64") {
            std::vector<int64_t> raw(n_elems);
            f.read((char*)raw.data(), n_elems * 8);
            for (int64_t i = 0; i < n_elems; i++) out[i] = (float)raw[i];
        } else if (tm.dtype == "i32") {
            std::vector<int32_t> raw(n_elems);
            f.read((char*)raw.data(), n_elems * 4);
            for (int64_t i = 0; i < n_elems; i++) out[i] = (float)raw[i];
        } else {
            // Unknown dtype — zero
        }
    } else {
        // Non-contiguous: read entire storage and index manually
        size_t storage_bytes = tm.num_elements * elem_bytes;
        f.seekg(0);
        std::vector<uint8_t> raw(storage_bytes);
        f.read((char*)raw.data(), (std::streamsize)storage_bytes);

        // Iterate over all multi-dimensional indices using stride layout
        std::vector<int64_t> idx(tm.shape.size(), 0);
        for (int64_t flat = 0; flat < n_elems; flat++) {
            // Compute strided offset
            int64_t soff = tm.storage_offset;
            for (size_t d = 0; d < idx.size(); d++) soff += idx[d] * tm.stride[d];
            soff *= (int64_t)elem_bytes;

            if (tm.dtype == "f16") {
                uint16_t h; memcpy(&h, raw.data() + soff, 2);
                out[flat] = f16_to_f32(h);
            } else if (tm.dtype == "f32") {
                float fv; memcpy(&fv, raw.data() + soff, 4); out[flat] = fv;
            } else if (tm.dtype == "i64") {
                int64_t iv; memcpy(&iv, raw.data() + soff, 8); out[flat] = (float)iv;
            }

            // Increment multi-dimensional index
            for (int d = (int)tm.shape.size() - 1; d >= 0; d--) {
                if (++idx[d] < tm.shape[d]) break;
                idx[d] = 0;
            }
        }
    }

    return out;
}

std::unordered_map<std::string, std::vector<float>>
pth_load_all(PthModel& m) {
    std::unordered_map<std::string, std::vector<float>> out;
    out.reserve(m.tensors.size());
    for (auto& p : m.tensors)
        out[p.first] = pth_load_tensor(m, p.first);
    return out;
}
