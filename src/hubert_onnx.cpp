// hubert_onnx.cpp — Convert a parsed fairseq HuBERT base PthModel into an ONNX file.
//
// Architecture: feature_extractor (7 conv layers) → feature_projection →
//               pos_conv → 12 transformer layers → final layer norm
//
// Input:  audio   float32 [1, N]   (16 kHz mono, raw PCM)
// Output: features float32 [1, T, 768]
//
// Protobuf is hand-rolled (no protobuf library dependency).
// Weight-norm is resolved at export time.
#include "hubert_onnx.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal protobuf writer (self-contained copy from rvc_onnx.cpp pattern)
// ─────────────────────────────────────────────────────────────────────────────

struct PbBuf {
    std::vector<uint8_t> d;

    void push_bytes(const void* p, size_t n) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
        d.insert(d.end(), b, b + n);
    }

    void varint(uint64_t v) {
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            d.push_back(b);
        } while (v);
    }

    void tag_varint(int field, uint64_t v)  { varint(((uint64_t)field << 3) | 0); varint(v); }
    void tag_i32   (int field, uint32_t v)  { varint(((uint64_t)field << 3) | 5); push_bytes(&v, 4); }
    void tag_bytes (int field, const void* p, size_t n) {
        varint(((uint64_t)field << 3) | 2); varint(n); push_bytes(p, n);
    }
    void tag_string(int field, const std::string& s) { tag_bytes(field, s.data(), s.size()); }
    void tag_msg   (int field, const PbBuf& sub)     { tag_bytes(field, sub.d.data(), sub.d.size()); }
    void append    (const PbBuf& o) { d.insert(d.end(), o.d.begin(), o.d.end()); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ONNX constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int kDtFloat  = 1;   // TensorProto::FLOAT
static constexpr int kDtInt64  = 7;   // TensorProto::INT64
static constexpr int kAttrFloat  = 1;
static constexpr int kAttrInt    = 2;
static constexpr int kAttrInts   = 7;

// ─────────────────────────────────────────────────────────────────────────────
// TensorProto builders
// ─────────────────────────────────────────────────────────────────────────────

static PbBuf make_tensor_float(const std::string& name,
                                const std::vector<int64_t>& dims,
                                const std::vector<float>& data)
{
    PbBuf t;
    for (int64_t d : dims) t.tag_varint(1, (uint64_t)d);
    t.tag_varint(2, kDtFloat);
    t.tag_string(8, name);
    t.tag_bytes(9, data.data(), data.size() * sizeof(float));
    return t;
}

static PbBuf make_tensor_int64(const std::string& name,
                                const std::vector<int64_t>& dims,
                                const std::vector<int64_t>& data)
{
    PbBuf t;
    for (int64_t d : dims) t.tag_varint(1, (uint64_t)d);
    t.tag_varint(2, kDtInt64);
    t.tag_string(8, name);
    t.tag_bytes(9, data.data(), data.size() * sizeof(int64_t));
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Graph builder
// ─────────────────────────────────────────────────────────────────────────────

struct OnnxGraph {
    PbBuf nodes;
    PbBuf initializers;
    PbBuf inputs;
    PbBuf outputs;
    int   ctr = 0;

    std::string uid(const std::string& pfx) {
        return pfx + "_" + std::to_string(ctr++);
    }

    void add_init_f32(const std::string& name, const std::vector<int64_t>& dims,
                      const std::vector<float>& data) {
        initializers.tag_msg(5, make_tensor_float(name, dims, data));
    }
    void add_init_i64(const std::string& name, const std::vector<int64_t>& dims,
                      const std::vector<int64_t>& data) {
        initializers.tag_msg(5, make_tensor_int64(name, dims, data));
    }
    void add_scalar_i64(const std::string& name, int64_t v) { add_init_i64(name, {}, {v}); }
    void add_scalar_f32(const std::string& name, float   v) { add_init_f32(name, {}, {v}); }

    void add_node(const std::string& op,
                  const std::vector<std::string>& ins,
                  const std::vector<std::string>& outs,
                  const std::string& node_name,
                  const std::vector<PbBuf>& attrs = {})
    {
        PbBuf n;
        for (auto& s : ins)  n.tag_string(1, s);
        for (auto& s : outs) n.tag_string(2, s);
        n.tag_string(3, node_name);
        n.tag_string(4, op);
        for (auto& a : attrs) n.tag_msg(5, a);
        nodes.tag_msg(1, n);
    }

    std::string emit(const std::string& op,
                     const std::vector<std::string>& ins,
                     const std::vector<PbBuf>& attrs = {},
                     const std::string& hint = "")
    {
        std::string out = uid(hint.empty() ? op : hint);
        add_node(op, ins, {out}, out + "_n", attrs);
        return out;
    }

    static PbBuf attr_int(const std::string& name, int64_t v) {
        PbBuf a;
        a.tag_string(1, name);
        a.tag_varint(3, (uint64_t)v);
        a.tag_varint(20, kAttrInt);
        return a;
    }
    static PbBuf attr_float(const std::string& name, float v) {
        PbBuf a;
        a.tag_string(1, name);
        uint32_t bits; memcpy(&bits, &v, 4);
        a.tag_i32(4, bits);
        a.tag_varint(20, kAttrFloat);
        return a;
    }
    static PbBuf attr_ints(const std::string& name, const std::vector<int64_t>& v) {
        PbBuf a;
        a.tag_string(1, name);
        PbBuf packed;
        for (int64_t x : v) packed.varint((uint64_t)x);
        a.tag_bytes(8, packed.d.data(), packed.d.size());
        a.tag_varint(20, kAttrInts);
        return a;
    }

    static PbBuf make_value_info(const std::string& name, int dtype,
                                  const std::vector<int64_t>& dims,
                                  const std::vector<std::string>& dyn_names = {})
    {
        PbBuf shape_proto;
        int dyn_ctr = 0;
        for (size_t i = 0; i < dims.size(); i++) {
            int64_t d = dims[i];
            PbBuf dim;
            if (d < 0) {
                std::string sym = (dyn_ctr < (int)dyn_names.size())
                                  ? dyn_names[(size_t)dyn_ctr]
                                  : ("dyn" + std::to_string(dyn_ctr));
                dyn_ctr++;
                dim.tag_string(2, sym);
            } else {
                dim.tag_varint(1, (uint64_t)d);
            }
            shape_proto.tag_msg(1, dim);
        }
        PbBuf tensor_type;
        tensor_type.tag_varint(1, (uint64_t)dtype);
        tensor_type.tag_msg(2, shape_proto);
        PbBuf type_proto;
        type_proto.tag_msg(1, tensor_type);
        PbBuf vi;
        vi.tag_string(1, name);
        vi.tag_msg(2, type_proto);
        return vi;
    }

    void add_input(const std::string& name, int dtype,
                   const std::vector<int64_t>& dims,
                   const std::vector<std::string>& dyn_names = {}) {
        inputs.tag_msg(11, make_value_info(name, dtype, dims, dyn_names));
    }
    void add_output(const std::string& name, int dtype,
                    const std::vector<int64_t>& dims,
                    const std::vector<std::string>& dyn_names = {}) {
        outputs.tag_msg(12, make_value_info(name, dtype, dims, dyn_names));
    }

    std::vector<uint8_t> serialise(const std::string& graph_name) const {
        PbBuf graph;
        graph.append(nodes);
        graph.tag_string(2, graph_name);
        graph.append(initializers);
        graph.append(inputs);
        graph.append(outputs);

        PbBuf opset;
        opset.tag_string(1, "");      // default domain
        opset.tag_varint(2, 17);      // opset version 17

        PbBuf model;
        model.tag_varint(1, 8);       // ir_version = 8
        model.tag_msg(8, opset);
        model.tag_msg(7, graph);
        return model.d;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Weight-norm resolution
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<float> apply_weight_norm(const std::vector<float>& wv,
                                             const std::vector<float>& wg,
                                             const std::vector<int64_t>& shape)
{
    int64_t dim0 = shape[0];
    int64_t rest = 1;
    for (size_t i = 1; i < shape.size(); i++) rest *= shape[i];

    std::vector<float> w(wv.size());
    for (int64_t i = 0; i < dim0; i++) {
        float g_val = wg[(size_t)i];
        const float* src = wv.data() + (size_t)i * rest;
        double norm2 = 0.0;
        for (int64_t k = 0; k < rest; k++) norm2 += (double)src[k] * src[k];
        float scale = (norm2 > 1e-12) ? g_val / (float)std::sqrt(norm2) : 0.f;
        float* dst = w.data() + (size_t)i * rest;
        for (int64_t k = 0; k < rest; k++) dst[k] = src[k] * scale;
    }
    return w;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tensor loader
// ─────────────────────────────────────────────────────────────────────────────

struct TensorLoader {
    const PthModel& m;

    std::vector<float> load(const std::string& name) const {
        auto v = pth_load_tensor(m, name);
        if (v.empty()) throw std::runtime_error("missing tensor: " + name);
        return v;
    }

    std::vector<float> load_wn(const std::string& base) const {
        auto it_v = m.tensors.find(base + ".weight_v");
        auto it_g = m.tensors.find(base + ".weight_g");
        if (it_v != m.tensors.end() && it_g != m.tensors.end()) {
            auto wv = load(base + ".weight_v");
            auto wg = load(base + ".weight_g");
            // weight_g shape is [C_out, 1, 1]; flatten to [C_out]
            std::vector<float> wg_flat(wg.size());
            for (size_t i = 0; i < wg.size(); i++) wg_flat[i] = wg[i];
            return apply_weight_norm(wv, wg_flat, it_v->second.shape);
        }
        return load(base + ".weight");
    }

    std::vector<int64_t> shape(const std::string& name) const {
        auto it = m.tensors.find(name);
        if (it == m.tensors.end()) throw std::runtime_error("no tensor: " + name);
        return it->second.shape;
    }

    std::vector<int64_t> shape_wn(const std::string& base) const {
        auto it_v = m.tensors.find(base + ".weight_v");
        if (it_v != m.tensors.end()) return it_v->second.shape;
        return shape(base + ".weight");
    }

    bool has(const std::string& name) const {
        return m.tensors.count(name) > 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Op shorthand helpers
// ─────────────────────────────────────────────────────────────────────────────

using G = OnnxGraph;

static std::string op_unsqueeze(G& g, const std::string& x, int64_t axis,
                                  const std::string& hint = "unsq")
{
    static int cnt = 0;
    std::string ax = "huq_ax_" + std::to_string(cnt++);
    g.add_init_i64(ax, {1}, {axis});
    return g.emit("Unsqueeze", {x, ax}, {}, hint);
}


static std::string op_transpose(G& g, const std::string& x,
                                  const std::vector<int64_t>& perm,
                                  const std::string& hint = "tp")
{
    return g.emit("Transpose", {x}, {G::attr_ints("perm", perm)}, hint);
}

static std::string op_reshape(G& g, const std::string& x, const std::string& shp,
                               const std::string& hint = "rs")
{
    return g.emit("Reshape", {x, shp}, {}, hint);
}

static std::string op_add(G& g, const std::string& a, const std::string& b,
                           const std::string& hint = "add")
{ return g.emit("Add", {a, b}, {}, hint); }

static std::string op_sub(G& g, const std::string& a, const std::string& b,
                           const std::string& hint = "sub")
{ return g.emit("Sub", {a, b}, {}, hint); }

static std::string op_mul(G& g, const std::string& a, const std::string& b,
                           const std::string& hint = "mul")
{ return g.emit("Mul", {a, b}, {}, hint); }

static std::string op_div(G& g, const std::string& a, const std::string& b,
                           const std::string& hint = "div")
{ return g.emit("Div", {a, b}, {}, hint); }

static std::string op_matmul(G& g, const std::string& a, const std::string& b,
                              const std::string& hint = "mm")
{ return g.emit("MatMul", {a, b}, {}, hint); }

static std::string op_concat(G& g, const std::vector<std::string>& ins,
                              int64_t axis, const std::string& hint = "cat")
{ return g.emit("Concat", ins, {G::attr_int("axis", axis)}, hint); }

static std::string op_softmax(G& g, const std::string& x, int64_t axis,
                               const std::string& h = "sm")
{ return g.emit("Softmax", {x}, {G::attr_int("axis", axis)}, h); }

// Dynamic-axis slice: starts/ends/axes given as tensor names already in the graph.
static std::string op_slice_dyn(G& g, const std::string& x,
                                  const std::string& starts,
                                  const std::string& ends,
                                  const std::string& axes,
                                  const std::string& hint = "sld")
{
    return g.emit("Slice", {x, starts, ends, axes}, {}, hint);
}

// Conv1d wrapper.  weight: [C_out, C_in_per_group, K]
static std::string op_conv1d(G& g,
                               const std::string& x,
                               const std::string& w,
                               const std::string& b,    // "" = no bias
                               int kernel, int stride, int padding,
                               const std::string& hint = "conv",
                               int group = 1)
{
    std::vector<PbBuf> attrs = {
        G::attr_ints("dilations",    {1}),
        G::attr_ints("kernel_shape", {kernel}),
        G::attr_ints("pads",         {padding, padding}),
        G::attr_ints("strides",      {stride}),
        G::attr_int ("group",        group),
    };
    std::vector<std::string> ins = {x, w};
    if (!b.empty()) ins.push_back(b);
    return g.emit("Conv", ins, attrs, hint);
}

// Extract a scalar int64 = shape[dim_idx] of tensor x.
static std::string shape_dim(G& g, const std::string& x, int64_t dim_idx,
                              const std::string& hint = "sdim")
{
    static int cnt = 0;
    std::string base = hint + "_" + std::to_string(cnt++);
    std::string sh   = g.emit("Shape", {x}, {}, base + "_sh");
    std::string idx  = base + "_idx"; g.add_init_i64(idx, {}, {dim_idx});
    return g.emit("Gather", {sh, idx}, {G::attr_int("axis", 0)}, base + "_dim");
}

// Concatenate scalar int64 tensors into a 1-D shape tensor.
static std::string build_shape(G& g, const std::vector<std::string>& scalars,
                                const std::string& hint = "shp")
{
    std::vector<std::string> parts;
    for (auto& s : scalars)
        parts.push_back(op_unsqueeze(g, s, 0, hint + "_uq"));
    return op_concat(g, parts, 0, hint);
}

// ─────────────────────────────────────────────────────────────────────────────
// GELU(x) = 0.5 * x * (1 + erf(x / sqrt(2)))
// ─────────────────────────────────────────────────────────────────────────────
static std::string op_gelu(G& g, const std::string& x, const std::string& hint)
{
    // sqrt2 constant
    std::string sqrt2_c = hint + "_sq2"; g.add_scalar_f32(sqrt2_c, std::sqrt(2.f));
    std::string one_c   = hint + "_1f";  g.add_scalar_f32(one_c,   1.f);
    std::string half_c  = hint + "_hf";  g.add_scalar_f32(half_c,  0.5f);

    auto xd  = op_div(g, x, sqrt2_c, hint + "_div");
    auto erf = g.emit("Erf", {xd}, {}, hint + "_erf");
    auto ep1 = op_add(g, erf, one_c,  hint + "_ep1");
    auto mux = op_mul(g, x,   ep1,    hint + "_mux");
    return op_mul(g, mux, half_c, hint + "_gelu");
}

// ─────────────────────────────────────────────────────────────────────────────
// LayerNorm operating on [..., C] tensors (normalise over last axis).
// gamma, beta: [C] initializer names.
// Input x is treated as having last dim = C.
// Uses ReduceMean / manual variance path (opset-17 compatible everywhere).
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_layer_norm_last(G& g,
                                         const std::string& x,
                                         const std::string& gamma,
                                         const std::string& beta,
                                         const std::string& hint)
{
    auto mn    = g.emit("ReduceMean", {x},
                        {G::attr_ints("axes", {-1}), G::attr_int("keepdims", 1)},
                        hint + "_mean");
    auto diff  = op_sub(g, x, mn, hint + "_diff");
    auto diff2 = op_mul(g, diff, diff, hint + "_diff2");
    auto var   = g.emit("ReduceMean", {diff2},
                        {G::attr_ints("axes", {-1}), G::attr_int("keepdims", 1)},
                        hint + "_var");
    std::string eps_c = hint + "_eps"; g.add_scalar_f32(eps_c, 1e-5f);
    auto var_eps = op_add(g, var, eps_c, hint + "_veps");
    auto std_    = g.emit("Sqrt", {var_eps}, {}, hint + "_std");
    auto normed  = op_div(g, diff, std_,  hint + "_normed");

    // Broadcast gamma/beta: they are [C]; normed may be [..., C].
    // Mul/Add broadcast fine as long as shapes are compatible.
    auto scaled = op_mul(g, normed, gamma, hint + "_sc");
    return op_add(g, scaled, beta, hint + "_ln");
}

// ─────────────────────────────────────────────────────────────────────────────
// Feature extractor: 7 conv layers, each Conv1d → GroupNorm(=InstanceNorm) → GELU
// Input:  x = audio [1, 1, N]  (channel dim added before calling)
// Output: [1, 512, T_conv]
//
// Conv strides: [5,2,2,2,2,2,2]  kernels: [10,3,3,3,3,2,2]
// Layers 0..6: in_ch=[1,512,512,512,512,512,512], out_ch=512
// ─────────────────────────────────────────────────────────────────────────────
static const int kFEStrides[7] = {5, 2, 2, 2, 2, 2, 2};
static const int kFEKernels[7] = {10, 3, 3, 3, 3, 2, 2};

static std::string emit_feature_extractor(G& g, const TensorLoader& tl,
                                           const std::string& x)
{
    // x: [1, 1, N]
    std::string cur = x;
    for (int i = 0; i < 7; i++) {
        std::string h    = "fe" + std::to_string(i);
        std::string cpfx = "feature_extractor.conv_layers." + std::to_string(i) + ".0";
        std::string npfx = "feature_extractor.conv_layers." + std::to_string(i) + ".2";

        int64_t c_out = 512;
        int64_t c_in  = (i == 0) ? 1 : 512;
        int k = kFEKernels[i];
        int s = kFEStrides[i];

        auto conv_w = tl.load(cpfx + ".weight");
        g.add_init_f32(h + "_cw", {c_out, c_in, k}, conv_w);

        // Conv1d, no bias (fairseq feature extractor has no conv bias)
        cur = op_conv1d(g, cur, h + "_cw", "", k, s, 0, h + "_conv");

        // GroupNorm only on layer 0 (mode="default" in fairseq HuBERT base)
        if (i == 0) {
            auto gn_w = tl.load(npfx + ".weight");
            auto gn_b = tl.load(npfx + ".bias");
            g.add_init_f32(h + "_gnw", {c_out}, gn_w);
            g.add_init_f32(h + "_gnb", {c_out}, gn_b);

            PbBuf eps_attr;
            eps_attr.tag_string(1, "epsilon");
            uint32_t eps_bits; float eps_val = 1e-5f; memcpy(&eps_bits, &eps_val, 4);
            eps_attr.tag_i32(4, eps_bits);
            eps_attr.tag_varint(20, kAttrFloat);
            cur = g.emit("InstanceNormalization",
                         {cur, h + "_gnw", h + "_gnb"},
                         {eps_attr},
                         h + "_gn");
        }

        // GELU
        cur = op_gelu(g, cur, h + "_gelu");
    }
    return cur;  // [1, 512, T_conv]
}

// ─────────────────────────────────────────────────────────────────────────────
// Feature projection: LayerNorm(512) → Linear(512→768)
// Input:  x [1, 512, T_conv]
// Output: [1, T_conv, 768]   (already transposed to [B,T,C] for transformer)
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_feature_projection(G& g, const TensorLoader& tl,
                                             const std::string& x)
{
    // Transpose [1, 512, T] → [1, T, 512]
    auto xt = op_transpose(g, x, {0, 2, 1}, "fp_tp0");

    // LayerNorm over last dim (512) — fairseq key: "layer_norm.*"
    auto ln_w = tl.load("layer_norm.weight");
    auto ln_b = tl.load("layer_norm.bias");
    g.add_init_f32("fp_lnw", {512}, ln_w);
    g.add_init_f32("fp_lnb", {512}, ln_b);
    auto xln = emit_layer_norm_last(g, xt, "fp_lnw", "fp_lnb", "fp_ln");

    // Linear(512→768): weight [768, 512] → transpose to [512, 768] for matmul
    // fairseq key: "post_extract_proj.*"
    auto proj_w_raw = tl.load("post_extract_proj.weight");  // [768, 512]
    auto proj_b     = tl.load("post_extract_proj.bias");    // [768]

    // Transpose weight CPU-side: [768,512] → [512,768]
    std::vector<float> proj_w_T(768 * 512);
    for (int i = 0; i < 768; i++)
        for (int j = 0; j < 512; j++)
            proj_w_T[(size_t)j * 768 + i] = proj_w_raw[(size_t)i * 512 + j];

    g.add_init_f32("fp_pw", {512, 768}, proj_w_T);
    g.add_init_f32("fp_pb", {768},      proj_b);

    // [1, T, 512] @ [512, 768] → [1, T, 768]
    auto mm  = op_matmul(g, xln, "fp_pw", "fp_mm");
    return op_add(g, mm, "fp_pb", "fp_proj");   // [1, T, 768]
}

// ─────────────────────────────────────────────────────────────────────────────
// Positional conv: grouped Conv1d(768,768,k=128,pad=64,groups=16) → GELU
// Input:  x [1, T, 768]
// Output: [1, T, 768]  (added to x as residual by caller)
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_pos_conv(G& g, const TensorLoader& tl,
                                  const std::string& x,     // [1, T, 768]
                                  const std::string& T_sc)  // scalar int64
{
    // Resolve weight norm: weight_v [768, 48, 128], weight_g [768, 1, 1]
    auto pos_w = tl.load_wn("encoder.pos_conv.0");
    auto pos_b = tl.load("encoder.pos_conv.0.bias");

    g.add_init_f32("pc_w", {768, 48, 128}, pos_w);
    g.add_init_f32("pc_b", {768},           pos_b);

    // Transpose input [1, T, 768] → [1, 768, T]
    auto xt = op_transpose(g, x, {0, 2, 1}, "pc_tp0");

    // Conv1d(groups=16, kernel=128, padding=64) → output [1, 768, T+1] (padding adds 1 extra)
    // Actually with padding=64 on both sides and kernel=128:
    //   L_out = L_in + 2*64 - (128 - 1) = L_in + 128 - 127 = L_in + 1
    // So we need to slice off the last frame.
    auto conv_out = op_conv1d(g, xt, "pc_w", "pc_b", 128, 1, 64, "pc_conv", 16);
    // conv_out: [1, 768, T+1]

    // Slice to [1, 768, T]: axis=2, start=0, end=T (dynamic)
    // We build the slice tensors dynamically.
    std::string zero_c = "pc_z0";   g.add_scalar_i64(zero_c, 0LL);
    std::string one_c  = "pc_one";  g.add_scalar_i64(one_c,  1LL);
    std::string big_c  = "pc_big";  g.add_scalar_i64(big_c,  INT64_MAX);
    std::string ax2_c  = "pc_ax2";  g.add_init_i64(ax2_c, {1}, {2LL});

    // starts=[0], ends=[T], axes=[2]
    auto starts_t = op_unsqueeze(g, zero_c, 0, "pc_sl_st");
    auto ends_t   = op_unsqueeze(g, T_sc,   0, "pc_sl_en");
    auto sliced   = op_slice_dyn(g, conv_out, starts_t, ends_t, ax2_c, "pc_sl");
    // sliced: [1, 768, T]

    // GELU
    auto gelu_out = op_gelu(g, sliced, "pc_gelu");

    // Transpose back [1, 768, T] → [1, T, 768]
    return op_transpose(g, gelu_out, {0, 2, 1}, "pc_tp1");
}

// ─────────────────────────────────────────────────────────────────────────────
// Self-attention (manual, 12 heads, dim=768, head_dim=64)
// Input x: [1, T, 768]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_self_attention(G& g, const TensorLoader& tl,
                                        const std::string& x,    // [1, T, 768]
                                        const std::string& T_sc, // scalar int64
                                        int layer,
                                        const std::string& hint)
{
    std::string lpfx = "encoder.layers." + std::to_string(layer) + ".self_attn";
    constexpr int H  = 12;
    constexpr int D  = 768;
    constexpr int Dh = 64;  // D / H

    // Load and transpose projection weights (PyTorch stores [out, in])
    auto load_proj_T = [&](const std::string& key, const std::string& init_name) {
        auto w_raw = tl.load(key);  // [D, D]
        std::vector<float> w_T(D * D);
        for (int i = 0; i < D; i++)
            for (int j = 0; j < D; j++)
                w_T[(size_t)j * D + i] = w_raw[(size_t)i * D + j];
        g.add_init_f32(init_name, {D, D}, w_T);
    };

    load_proj_T(lpfx + ".q_proj.weight", hint + "_Wq");
    load_proj_T(lpfx + ".k_proj.weight", hint + "_Wk");
    load_proj_T(lpfx + ".v_proj.weight", hint + "_Wv");
    load_proj_T(lpfx + ".out_proj.weight", hint + "_Wo");

    g.add_init_f32(hint + "_bq", {D}, tl.load(lpfx + ".q_proj.bias"));
    g.add_init_f32(hint + "_bk", {D}, tl.load(lpfx + ".k_proj.bias"));
    g.add_init_f32(hint + "_bv", {D}, tl.load(lpfx + ".v_proj.bias"));
    g.add_init_f32(hint + "_bo", {D}, tl.load(lpfx + ".out_proj.bias"));

    // Q = x @ Wq + bq  [1, T, D]
    auto Q = op_add(g, op_matmul(g, x, hint + "_Wq", hint + "_Q_mm"), hint + "_bq", hint + "_Q");
    auto K = op_add(g, op_matmul(g, x, hint + "_Wk", hint + "_K_mm"), hint + "_bk", hint + "_K");
    auto V = op_add(g, op_matmul(g, x, hint + "_Wv", hint + "_V_mm"), hint + "_bv", hint + "_V");

    // Reshape [1, T, D] → [1, T, H, Dh] → Transpose → [1, H, T, Dh]
    std::string one_c  = hint + "_1";  g.add_scalar_i64(one_c,  1LL);
    std::string H_c    = hint + "_H";  g.add_scalar_i64(H_c,    (int64_t)H);
    std::string Dh_c   = hint + "_Dh"; g.add_scalar_i64(Dh_c,   (int64_t)Dh);

    auto q4shp = build_shape(g, {one_c, T_sc, H_c, Dh_c}, hint + "_q4shp");
    auto Q4    = op_transpose(g, op_reshape(g, Q, q4shp, hint + "_Qrs"), {0,2,1,3}, hint + "_Qt");
    auto K4    = op_transpose(g, op_reshape(g, K, q4shp, hint + "_Krs"), {0,2,1,3}, hint + "_Kt");
    auto V4    = op_transpose(g, op_reshape(g, V, q4shp, hint + "_Vrs"), {0,2,1,3}, hint + "_Vt");
    // Q4, K4, V4: [1, H, T, Dh]

    // Scale scores
    float scale = 1.f / std::sqrt((float)Dh);
    std::string sc_c = hint + "_sc"; g.add_scalar_f32(sc_c, scale);
    auto Q4s = op_mul(g, Q4, sc_c, hint + "_Qs");

    // scores = Q @ K^T:  [1,H,T,Dh] @ [1,H,Dh,T] → [1,H,T,T]
    auto K4T   = op_transpose(g, K4, {0,1,3,2}, hint + "_KT");
    auto scores = op_matmul(g, Q4s, K4T, hint + "_sc_mm");  // [1,H,T,T]

    // Softmax over last axis
    auto attn = op_softmax(g, scores, -1, hint + "_sm");  // [1,H,T,T]

    // out = attn @ V:  [1,H,T,T] @ [1,H,T,Dh] → [1,H,T,Dh]
    auto aout  = op_matmul(g, attn, V4, hint + "_aout");
    // Transpose [1,H,T,Dh] → [1,T,H,Dh]
    auto aoutT = op_transpose(g, aout, {0,2,1,3}, hint + "_aoutT");
    // Reshape [1,T,H,Dh] → [1,T,D]
    std::string D_c = hint + "_D"; g.add_scalar_i64(D_c, (int64_t)D);
    auto merged_shp = build_shape(g, {one_c, T_sc, D_c}, hint + "_mshp");
    auto merged     = op_reshape(g, aoutT, merged_shp, hint + "_merged");  // [1,T,768]

    // Output projection
    auto out_mm = op_matmul(g, merged, hint + "_Wo", hint + "_oproj_mm");
    return op_add(g, out_mm, hint + "_bo", hint + "_oproj");
}

// ─────────────────────────────────────────────────────────────────────────────
// FFN for transformer: GELU(Linear(768→3072)) → Linear(3072→768)
// Input x: [1, T, 768]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_ffn(G& g, const TensorLoader& tl,
                              const std::string& x,
                              int layer,
                              const std::string& hint)
{
    std::string lpfx = "encoder.layers." + std::to_string(layer);

    // fc1: [3072, 768] → transpose to [768, 3072]
    auto fc1_w_raw = tl.load(lpfx + ".fc1.weight");  // [3072, 768]
    std::vector<float> fc1_w_T(768 * 3072);
    for (int i = 0; i < 3072; i++)
        for (int j = 0; j < 768; j++)
            fc1_w_T[(size_t)j * 3072 + i] = fc1_w_raw[(size_t)i * 768 + j];
    g.add_init_f32(hint + "_fc1w", {768, 3072}, fc1_w_T);
    g.add_init_f32(hint + "_fc1b", {3072}, tl.load(lpfx + ".fc1.bias"));

    // fc2: [768, 3072] → transpose to [3072, 768]
    auto fc2_w_raw = tl.load(lpfx + ".fc2.weight");  // [768, 3072]
    std::vector<float> fc2_w_T(3072 * 768);
    for (int i = 0; i < 768; i++)
        for (int j = 0; j < 3072; j++)
            fc2_w_T[(size_t)j * 768 + i] = fc2_w_raw[(size_t)i * 3072 + j];
    g.add_init_f32(hint + "_fc2w", {3072, 768}, fc2_w_T);
    g.add_init_f32(hint + "_fc2b", {768},  tl.load(lpfx + ".fc2.bias"));

    // [1,T,768] @ [768,3072] + bias → GELU
    auto h1   = op_add(g, op_matmul(g, x, hint + "_fc1w", hint + "_mm1"), hint + "_fc1b", hint + "_h1");
    auto h1g  = op_gelu(g, h1, hint + "_ffngelu");
    // [1,T,3072] @ [3072,768] + bias
    auto h2   = op_add(g, op_matmul(g, h1g, hint + "_fc2w", hint + "_mm2"), hint + "_fc2b", hint + "_h2");
    return h2;  // [1, T, 768]
}

// ─────────────────────────────────────────────────────────────────────────────
// Single transformer layer (pre-norm)
// Input x: [1, T, 768]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_transformer_layer(G& g, const TensorLoader& tl,
                                           const std::string& x,
                                           const std::string& T_sc,
                                           int layer)
{
    std::string h    = "tl" + std::to_string(layer);
    std::string lpfx = "encoder.layers." + std::to_string(layer);

    // Self-attn pre-norm
    auto ln1_w = tl.load(lpfx + ".self_attn_layer_norm.weight");
    auto ln1_b = tl.load(lpfx + ".self_attn_layer_norm.bias");
    g.add_init_f32(h + "_ln1w", {768}, ln1_w);
    g.add_init_f32(h + "_ln1b", {768}, ln1_b);
    auto x_ln1 = emit_layer_norm_last(g, x, h + "_ln1w", h + "_ln1b", h + "_ln1");

    // Self-attention
    auto attn_out = emit_self_attention(g, tl, x_ln1, T_sc, layer, h + "_sa");

    // Residual
    auto res1 = op_add(g, x, attn_out, h + "_res1");

    // FFN pre-norm
    auto ln2_w = tl.load(lpfx + ".final_layer_norm.weight");
    auto ln2_b = tl.load(lpfx + ".final_layer_norm.bias");
    g.add_init_f32(h + "_ln2w", {768}, ln2_w);
    g.add_init_f32(h + "_ln2b", {768}, ln2_b);
    auto x_ln2 = emit_layer_norm_last(g, res1, h + "_ln2w", h + "_ln2b", h + "_ln2");

    // FFN
    auto ffn_out = emit_ffn(g, tl, x_ln2, layer, h + "_ffn");

    // Residual
    return op_add(g, res1, ffn_out, h + "_res2");
}

// ─────────────────────────────────────────────────────────────────────────────
// hubert_to_onnx — main entry point
// ─────────────────────────────────────────────────────────────────────────────

std::string hubert_to_onnx(const PthModel& m, const std::string& out_path)
{
    if (!m.err.empty()) return "PthModel has error: " + m.err;

    try {
        TensorLoader tl{m};
        OnnxGraph g;

        // ── Graph I/O declarations ────────────────────────────────────────────
        g.add_input("audio",    kDtFloat, {1, -1}, {"N"});
        g.add_output("features", kDtFloat, {1, -1, 768}, {"T"});

        // ── Add channel dim: [1, N] → [1, 1, N] ──────────────────────────────
        auto audio_3d = op_unsqueeze(g, "audio", 1, "au3d");

        // ── Feature extractor (7 conv layers) ────────────────────────────────
        // Output: [1, 512, T_conv]
        auto feat_conv = emit_feature_extractor(g, tl, audio_3d);

        // ── Feature projection: LayerNorm(512) → Linear(512→768) ─────────────
        // Output: [1, T_conv, 768]
        auto feat_proj = emit_feature_projection(g, tl, feat_conv);

        // ── Get T (sequence length after conv) ────────────────────────────────
        auto T_sc = shape_dim(g, feat_proj, 1, "T");

        // ── Positional conv ───────────────────────────────────────────────────
        auto pos_enc = emit_pos_conv(g, tl, feat_proj, T_sc);
        // x = x + pos_enc  [1, T, 768]
        auto x = op_add(g, feat_proj, pos_enc, "pos_add");

        // ── 12 Transformer layers ─────────────────────────────────────────────
        for (int i = 0; i < 12; i++) {
            x = emit_transformer_layer(g, tl, x, T_sc, i);
        }

        // ── Final layer norm ──────────────────────────────────────────────────
        auto enc_ln_w = tl.load("encoder.layer_norm.weight");
        auto enc_ln_b = tl.load("encoder.layer_norm.bias");
        g.add_init_f32("enc_lnw", {768}, enc_ln_w);
        g.add_init_f32("enc_lnb", {768}, enc_ln_b);
        x = emit_layer_norm_last(g, x, "enc_lnw", "enc_lnb", "enc_ln");

        // ── Rename final output to "features" ─────────────────────────────────
        g.add_node("Identity", {x}, {"features"}, "features_id");

        // ── Serialise ─────────────────────────────────────────────────────────
        auto bytes = g.serialise("hubert_base");

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) return "Cannot open for writing: " + out_path;
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  (std::streamsize)bytes.size());
        if (!ofs) return "Write error: " + out_path;

        return "";  // success

    } catch (const std::exception& e) {
        return std::string("hubert_to_onnx error: ") + e.what();
    }
}
