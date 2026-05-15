// rvc_onnx.cpp — Convert a parsed RVC PthModel into an ONNX protobuf file.
//
// Architecture: enc_p → flow (reverse) → dec
// Protobuf is hand-rolled (no protobuf library dependency).
// All weight tensors are resolved (weight-norm applied) before embedding.
//
// See rvc_onnx.h for I/O spec.
#include "rvc_onnx.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cassert>
#include <numeric>
#include <climits>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal protobuf writer
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

    // tag helpers: wire type 0=varint, 1=fixed64, 2=LEN, 5=fixed32
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
static constexpr int kAttrFloat  = 1; // AttributeProto type enum
static constexpr int kAttrInt    = 2;
static constexpr int kAttrFloats = 6;
static constexpr int kAttrInts   = 7;

// ─────────────────────────────────────────────────────────────────────────────
// TensorProto builders
// ─────────────────────────────────────────────────────────────────────────────

static PbBuf make_tensor_float(const std::string& name,
                                const std::vector<int64_t>& dims,
                                const std::vector<float>& data)
{
    PbBuf t;
    // dims: field 1, repeated non-packed int64 (each tagged separately)
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
    PbBuf nodes;        // GraphProto field 1, repeated NodeProto
    PbBuf initializers; // GraphProto field 5
    PbBuf inputs;       // GraphProto field 11
    PbBuf outputs;      // GraphProto field 12
    int   ctr = 0;

    std::string uid(const std::string& pfx) {
        return pfx + "_" + std::to_string(ctr++);
    }

    // ── Initializers ─────────────────────────────────────────────────────────

    void add_init_f32(const std::string& name, const std::vector<int64_t>& dims,
                      const std::vector<float>& data) {
        initializers.tag_msg(5, make_tensor_float(name, dims, data));
    }
    void add_init_i64(const std::string& name, const std::vector<int64_t>& dims,
                      const std::vector<int64_t>& data) {
        initializers.tag_msg(5, make_tensor_int64(name, dims, data));
    }
    // Scalar initializers (0-D tensors)
    void add_scalar_i64(const std::string& name, int64_t v) { add_init_i64(name, {}, {v}); }
    void add_scalar_f32(const std::string& name, float   v) { add_init_f32(name, {}, {v}); }

    // ── Nodes ─────────────────────────────────────────────────────────────────

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

    // Single-output node; returns output name.
    std::string emit(const std::string& op,
                     const std::vector<std::string>& ins,
                     const std::vector<PbBuf>& attrs = {},
                     const std::string& hint = "")
    {
        std::string out = uid(hint.empty() ? op : hint);
        add_node(op, ins, {out}, out + "_n", attrs);
        return out;
    }

    // ── Attribute builders ────────────────────────────────────────────────────

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
        // packed int64: field 8, wire LEN
        PbBuf packed;
        for (int64_t x : v) packed.varint((uint64_t)x);
        a.tag_bytes(8, packed.d.data(), packed.d.size());
        a.tag_varint(20, kAttrInts);
        return a;
    }

    // ── ValueInfoProto (graph I/O type declarations) ──────────────────────────

    static PbBuf make_value_info(const std::string& name, int dtype,
                                  const std::vector<int64_t>& dims)
    {
        PbBuf shape_proto;
        int dyn_ctr = 0;
        for (int64_t d : dims) {
            PbBuf dim;
            if (d < 0) {
                dim.tag_string(2, "dyn" + std::to_string(dyn_ctr++));
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

    void add_input(const std::string& name, int dtype, const std::vector<int64_t>& dims) {
        inputs.tag_msg(11, make_value_info(name, dtype, dims));
    }
    void add_output(const std::string& name, int dtype, const std::vector<int64_t>& dims) {
        outputs.tag_msg(12, make_value_info(name, dtype, dims));
    }

    // ── Serialise ─────────────────────────────────────────────────────────────

    std::vector<uint8_t> serialise(const std::string& graph_name) const {
        PbBuf graph;
        graph.append(nodes);
        graph.tag_string(2, graph_name);
        graph.append(initializers);
        graph.append(inputs);
        graph.append(outputs);

        PbBuf opset;
        opset.tag_string(1, "");       // default domain
        opset.tag_varint(2, 14);       // opset version 14

        PbBuf model;
        model.tag_varint(1, 8);        // ir_version = 8
        model.tag_msg(8, opset);
        model.tag_msg(7, graph);
        return model.d;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Weight-norm resolution
// W_eff[i,:,:] = weight_g[i,0,0] / ||weight_v[i,:,:]||₂ × weight_v[i,:,:]
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

    // Load with optional weight-norm. If weight_v/weight_g exist, resolve norm.
    // Otherwise fall back to .weight.
    std::vector<float> load_wn(const std::string& base) const {
        auto it_v = m.tensors.find(base + ".weight_v");
        auto it_g = m.tensors.find(base + ".weight_g");
        if (it_v != m.tensors.end() && it_g != m.tensors.end()) {
            auto wv = load(base + ".weight_v");
            auto wg = load(base + ".weight_g");
            return apply_weight_norm(wv, wg, it_v->second.shape);
        }
        return load(base + ".weight");
    }

    std::vector<int64_t> shape(const std::string& name) const {
        auto it = m.tensors.find(name);
        if (it == m.tensors.end()) throw std::runtime_error("no tensor: " + name);
        return it->second.shape;
    }

    // Shape of a weight-norm layer (prefers weight_v shape).
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
// All return the name of the single output tensor.
// ─────────────────────────────────────────────────────────────────────────────

using G = OnnxGraph;

// Unsqueeze (opset 13+: axes as input tensor)
static std::string op_unsqueeze(G& g, const std::string& x, int64_t axis,
                                  const std::string& hint = "unsq")
{
    static int cnt = 0;
    std::string ax = "uq_ax_" + std::to_string(cnt++);
    g.add_init_i64(ax, {1}, {axis});
    return g.emit("Unsqueeze", {x, ax}, {}, hint);
}

static std::string op_squeeze(G& g, const std::string& x, int64_t axis,
                               const std::string& hint = "sq")
{
    static int cnt = 0;
    std::string ax = "sq_ax_" + std::to_string(cnt++);
    g.add_init_i64(ax, {1}, {axis});
    return g.emit("Squeeze", {x, ax}, {}, hint);
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

static std::string op_gather(G& g, const std::string& data, const std::string& idx,
                              int64_t axis, const std::string& hint = "gat")
{ return g.emit("Gather", {data, idx}, {G::attr_int("axis", axis)}, hint); }

static std::string op_concat(G& g, const std::vector<std::string>& ins,
                              int64_t axis, const std::string& hint = "cat")
{ return g.emit("Concat", ins, {G::attr_int("axis", axis)}, hint); }

static std::string op_tanh   (G& g, const std::string& x, const std::string& h = "tanh")
{ return g.emit("Tanh",    {x}, {}, h); }

static std::string op_relu   (G& g, const std::string& x, const std::string& h = "relu")
{ return g.emit("Relu",    {x}, {}, h); }

static std::string op_sigmoid(G& g, const std::string& x, const std::string& h = "sig")
{ return g.emit("Sigmoid", {x}, {}, h); }

static std::string op_leaky_relu(G& g, const std::string& x, float alpha,
                                  const std::string& h = "lr")
{ return g.emit("LeakyRelu", {x}, {G::attr_float("alpha", alpha)}, h); }

static std::string op_softmax(G& g, const std::string& x, int64_t axis,
                               const std::string& h = "sm")
{ return g.emit("Softmax", {x}, {G::attr_int("axis", axis)}, h); }

// Slice with static int64 starts/ends/axes tensors.
// Use INT64_MAX (0x7FFFFFFFFFFFFFFF) for "to end" in an axis.
static std::string op_slice(G& g, const std::string& x,
                              const std::vector<int64_t>& starts,
                              const std::vector<int64_t>& ends,
                              const std::vector<int64_t>& axes,
                              const std::string& hint = "sl")
{
    static int cnt = 0;
    std::string b = hint + "_sls_" + std::to_string(cnt++);
    std::string sn = b + "_s"; g.add_init_i64(sn, {(int64_t)starts.size()}, starts);
    std::string en = b + "_e"; g.add_init_i64(en, {(int64_t)ends.size()},   ends);
    std::string an = b + "_a"; g.add_init_i64(an, {(int64_t)axes.size()},   axes);
    return g.emit("Slice", {x, sn, en, an}, {}, hint);
}

// Conv1d wrapper. weight: [C_out, C_in_per_group, K]
static std::string op_conv1d(G& g,
                               const std::string& x,
                               const std::string& w,
                               const std::string& b,    // "" = no bias
                               int kernel, int dilation, int padding,
                               const std::string& hint = "conv",
                               int stride = 1)
{
    std::vector<PbBuf> attrs = {
        G::attr_ints("dilations",    {dilation}),
        G::attr_ints("kernel_shape", {kernel}),
        G::attr_ints("pads",         {padding, padding}),
        G::attr_ints("strides",      {stride}),
    };
    std::vector<std::string> ins = {x, w};
    if (!b.empty()) ins.push_back(b);
    return g.emit("Conv", ins, attrs, hint);
}

// ConvTranspose1d. weight layout: [C_in, C_out/groups, K] (PyTorch convention).
static std::string op_convtranspose1d(G& g,
                                       const std::string& x,
                                       const std::string& w,
                                       const std::string& b,    // "" = no bias
                                       int kernel, int stride, int padding,
                                       const std::string& hint = "deconv")
{
    std::vector<PbBuf> attrs = {
        G::attr_ints("kernel_shape", {kernel}),
        G::attr_ints("pads",         {padding, padding}),
        G::attr_ints("strides",      {stride}),
    };
    std::vector<std::string> ins = {x, w};
    if (!b.empty()) ins.push_back(b);
    return g.emit("ConvTranspose", ins, attrs, hint);
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
// LayerNorm operating on [B, C, T] tensors (normalise over C).
// gamma, beta: [C] initializer names.
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_layer_norm(G& g,
                                    const std::string& x,
                                    const std::string& gamma,
                                    const std::string& beta,
                                    const std::string& hint)
{
    // [B, C, T] → [B, T, C]
    auto xt   = op_transpose(g, x, {0, 2, 1}, hint + "_tp0");

    // Mean and variance over last axis (C)
    auto mn   = g.emit("ReduceMean", {xt},
                        {G::attr_ints("axes", {-1}), G::attr_int("keepdims", 1)},
                        hint + "_mean");
    auto diff  = op_sub(g, xt, mn, hint + "_diff");
    auto diff2 = op_mul(g, diff, diff, hint + "_diff2");
    auto var   = g.emit("ReduceMean", {diff2},
                         {G::attr_ints("axes", {-1}), G::attr_int("keepdims", 1)},
                         hint + "_var");
    std::string eps_c = hint + "_eps"; g.add_scalar_f32(eps_c, 1e-5f);
    auto var_eps  = op_add(g, var, eps_c, hint + "_veps");
    auto std_     = g.emit("Sqrt", {var_eps}, {}, hint + "_std");
    auto normed   = op_div(g, diff, std_, hint + "_normed");

    // [B, T, C] → [B, C, T]
    auto normed_bct = op_transpose(g, normed, {0, 2, 1}, hint + "_tp1");

    // Broadcast gamma/beta [C] → [1, C, 1]
    auto gam1 = op_unsqueeze(g, gamma, 0, hint + "_gu0");
    auto gam2 = op_unsqueeze(g, gam1,  2, hint + "_gu1");
    auto bet1 = op_unsqueeze(g, beta,  0, hint + "_bu0");
    auto bet2 = op_unsqueeze(g, bet1,  2, hint + "_bu1");

    auto scaled = op_mul(g, normed_bct, gam2, hint + "_sc");
    return op_add(g, scaled, bet2, hint + "_ln");
}

// ─────────────────────────────────────────────────────────────────────────────
// _rel_emb: compute relative position embedding for one attention layer.
// emb_name: initializer [1, 2*ws+1, k_ch] = [1, 21, 96]
// T_scalar: scalar int64 for current sequence length.
// Returns: [n_heads, 2T-1, k_ch]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_rel_emb(G& g, const std::string& emb_name,
                                  const std::string& T,   // scalar int64
                                  int n_heads, int k_ch,
                                  const std::string& hint)
{
    // ws=10, ws1=11  (window_size and window_size+1)
    std::string ws1_c = hint + "_ws1"; g.add_scalar_i64(ws1_c, 11LL);
    std::string zero_c = hint + "_0";  g.add_scalar_i64(zero_c, 0LL);
    std::string one_c  = hint + "_1";  g.add_scalar_i64(one_c,  1LL);
    std::string two_c  = hint + "_2";  g.add_scalar_i64(two_c,  2LL);

    // pad_amt = max(T - 11, 0)
    auto T_m11  = op_sub(g, T, ws1_c, hint + "_Tm11");
    auto padamt = g.emit("Max", {T_m11, zero_c}, {}, hint + "_padamt");

    // start = max(11 - T, 0)
    auto w1_mT  = op_sub(g, ws1_c, T, hint + "_w1mT");
    auto start  = g.emit("Max", {w1_mT, zero_c}, {}, hint + "_start");

    // end = start + 2*T - 1
    auto two_T  = op_mul(g, two_c, T, hint + "_2T");
    auto two_Tm1 = op_sub(g, two_T, one_c, hint + "_2Tm1");
    auto end_v  = op_add(g, start, two_Tm1, hint + "_end");

    // Pad emb [1, 21, k_ch] on dim 1 by padamt on both sides.
    // pads = [0, padamt, 0, 0, padamt, 0]  (start/end for each of 3 dims)
    auto pads_t = build_shape(g, {zero_c, padamt, zero_c, zero_c, padamt, zero_c},
                               hint + "_pads");
    auto emb_padded = g.emit("Pad", {emb_name, pads_t}, {}, hint + "_padded");
    // emb_padded: [1, 21 + 2*padamt, k_ch]

    // Slice dim 1: [start, end_v), dims 0 and 2 take full extent.
    std::string kch_c = hint + "_kch"; g.add_scalar_i64(kch_c, (int64_t)k_ch);
    auto starts_t = build_shape(g, {zero_c, start, zero_c},   hint + "_sl_st");
    auto ends_t   = build_shape(g, {one_c,  end_v, kch_c},    hint + "_sl_en");
    std::string axes_n = hint + "_ax012"; g.add_init_i64(axes_n, {3}, {0LL, 1LL, 2LL});
    auto sliced = g.emit("Slice", {emb_padded, starts_t, ends_t, axes_n},
                          {}, hint + "_sliced");
    // sliced: [1, 2T-1, k_ch]

    // Tile to [n_heads, 2T-1, k_ch]
    std::string nh_c = hint + "_nh"; g.add_scalar_i64(nh_c, (int64_t)n_heads);
    auto tile_reps = build_shape(g, {nh_c, one_c, one_c}, hint + "_treps");
    return g.emit("Tile", {sliced, tile_reps}, {}, hint + "_ek");
}

// ─────────────────────────────────────────────────────────────────────────────
// _rel_to_abs: [1, H, T, 2T-1] → [1, H, T, T]  (correct VITS version)
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_rel_to_abs(G& g, const std::string& x,
                                    const std::string& T,   // scalar int64
                                    int n_heads,
                                    const std::string& hint)
{
    std::string one_c  = hint + "_1"; g.add_scalar_i64(one_c,  1LL);
    std::string two_c  = hint + "_2"; g.add_scalar_i64(two_c,  2LL);
    std::string zero_c = hint + "_0"; g.add_scalar_i64(zero_c, 0LL);
    std::string H_c    = hint + "_H"; g.add_scalar_i64(H_c, (int64_t)n_heads);

    // Step 1: pad last dim by 1 → [1, H, T, 2T]  (static pads)
    std::string pad1n = hint + "_p1"; g.add_init_i64(pad1n, {8}, {0,0,0,0,0,0,0,1});
    auto x1 = g.emit("Pad", {x, pad1n}, {}, hint + "_s1");

    // Step 2: Reshape → [1, H, 2T²]
    auto two_T   = op_mul(g, two_c, T, hint + "_2T");
    auto two_T2  = op_mul(g, two_T, T, hint + "_2T2");
    auto rs2_shp = build_shape(g, {one_c, H_c, two_T2}, hint + "_rs2");
    auto x2 = op_reshape(g, x1, rs2_shp, hint + "_s2");

    // Step 3: pad last dim by T-1 → [1, H, 2T²+T-1]
    auto T_m1 = op_sub(g, T, one_c, hint + "_Tm1");
    auto pads3 = build_shape(g, {zero_c, zero_c, zero_c, zero_c, zero_c, T_m1},
                              hint + "_pads3");
    auto x3 = g.emit("Pad", {x2, pads3}, {}, hint + "_s3");

    // Step 4: Reshape → [1, H, T+1, 2T-1]
    auto T_p1    = op_add(g, T, one_c, hint + "_Tp1");
    auto two_Tm1 = op_sub(g, two_T, one_c, hint + "_2Tm1");
    auto rs4_shp = build_shape(g, {one_c, H_c, T_p1, two_Tm1}, hint + "_rs4");
    auto x4 = op_reshape(g, x3, rs4_shp, hint + "_s4");

    // Step 5: Slice [:, :, :T, (T-1):]  → [1, H, T, T]
    // starts=[0,0,0,T-1], ends=[1,H,T,2T-1], axes=[0,1,2,3]
    auto st5 = build_shape(g, {zero_c, zero_c, zero_c, T_m1},      hint + "_st5");
    auto en5 = build_shape(g, {one_c,  H_c,   T,      two_Tm1},    hint + "_en5");
    std::string ax5 = hint + "_ax5"; g.add_init_i64(ax5, {4}, {0LL, 1LL, 2LL, 3LL});
    return g.emit("Slice", {x4, st5, en5, ax5}, {}, hint + "_s5");
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-head Attention (enc_p encoder)
// window_size=10, heads_share=true, no attention mask (all-ones x_mask)
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_attention(G& g, const TensorLoader& tl,
                                   const std::string& x,   // [1, hidden, T]
                                   const std::string& T,   // scalar int64
                                   const std::string& pfx, // PyTorch key prefix
                                   int hidden, int n_heads,
                                   const std::string& hint)
{
    int k_ch = hidden / n_heads;

    // Load weights
    auto W_q = tl.load(pfx + ".conv_q.weight");
    auto b_q = tl.load(pfx + ".conv_q.bias");
    auto W_k = tl.load(pfx + ".conv_k.weight");
    auto b_k = tl.load(pfx + ".conv_k.bias");
    auto W_v = tl.load(pfx + ".conv_v.weight");
    auto b_v = tl.load(pfx + ".conv_v.bias");
    auto W_o = tl.load(pfx + ".conv_o.weight");
    auto b_o = tl.load(pfx + ".conv_o.bias");
    auto emb_rk = tl.load(pfx + ".emb_rel_k");

    // Register initializers
    g.add_init_f32(hint + "_Wq", {hidden, hidden, 1}, W_q);
    g.add_init_f32(hint + "_bq", {hidden},             b_q);
    g.add_init_f32(hint + "_Wk", {hidden, hidden, 1}, W_k);
    g.add_init_f32(hint + "_bk", {hidden},             b_k);
    g.add_init_f32(hint + "_Wv", {hidden, hidden, 1}, W_v);
    g.add_init_f32(hint + "_bv", {hidden},             b_v);
    g.add_init_f32(hint + "_Wo", {hidden, hidden, 1}, W_o);
    g.add_init_f32(hint + "_bo", {hidden},             b_o);
    g.add_init_f32(hint + "_erk", {1, 21, (int64_t)k_ch}, emb_rk);

    // Q, K, V projections
    auto q = op_conv1d(g, x, hint + "_Wq", hint + "_bq", 1, 1, 0, hint + "_Q");
    auto k = op_conv1d(g, x, hint + "_Wk", hint + "_bk", 1, 1, 0, hint + "_K");
    auto v = op_conv1d(g, x, hint + "_Wv", hint + "_bv", 1, 1, 0, hint + "_V");
    // q, k, v: [1, hidden, T]

    // Reshape heads
    std::string one_c = hint + "_1"; g.add_scalar_i64(one_c, 1LL);
    std::string nh_c  = hint + "_H"; g.add_scalar_i64(nh_c,  (int64_t)n_heads);
    std::string kc_c  = hint + "_K"; g.add_scalar_i64(kc_c,  (int64_t)k_ch);

    auto qshp = build_shape(g, {one_c, nh_c, kc_c, T}, hint + "_qshp");
    auto q4   = op_reshape(g, q, qshp, hint + "_q4");  // [1, H, k_ch, T]
    auto k4   = op_reshape(g, k, qshp, hint + "_k4");
    auto v4   = op_reshape(g, v, qshp, hint + "_v4");

    // Transpose to [1, H, T, k_ch]
    auto qt = op_transpose(g, q4, {0,1,3,2}, hint + "_qt");
    auto kt = op_transpose(g, k4, {0,1,3,2}, hint + "_kt");
    auto vt = op_transpose(g, v4, {0,1,3,2}, hint + "_vt");

    // Scale q
    float scale = 1.f / std::sqrt((float)k_ch);
    std::string sc_c = hint + "_sc"; g.add_scalar_f32(sc_c, scale);
    auto qt_s = op_mul(g, qt, sc_c, hint + "_qts");

    // scores = qt_s @ kt^T  → [1, H, T, T]
    auto kt_T  = op_transpose(g, kt, {0,1,3,2}, hint + "_ktT");  // [1,H,k_ch,T]
    auto scores = op_matmul(g, qt_s, kt_T, hint + "_scores");     // [1,H,T,T]

    // Relative position bias
    auto ek    = emit_rel_emb(g, hint + "_erk", T, n_heads, k_ch, hint + "_re");
    // ek: [H, 2T-1, k_ch] → unsqueeze to [1, H, 2T-1, k_ch]
    auto ek4   = op_unsqueeze(g, ek, 0, hint + "_ek4");           // [1, H, 2T-1, k_ch]
    auto ek4T  = op_transpose(g, ek4, {0,1,3,2}, hint + "_ekT");  // [1, H, k_ch, 2T-1]
    // rel_scores = qt_s @ ek4T → [1, H, T, 2T-1]
    auto rel_sc = op_matmul(g, qt_s, ek4T, hint + "_relsc");
    auto rel_abs = emit_rel_to_abs(g, rel_sc, T, n_heads, hint + "_r2a");

    auto sc_biased = op_add(g, scores, rel_abs, hint + "_scb");
    auto weights   = op_softmax(g, sc_biased, -1, hint + "_sm"); // [1, H, T, T]

    // out = weights @ vt → [1, H, T, k_ch]
    auto aout  = op_matmul(g, weights, vt, hint + "_aout");
    // Transpose: [1, H, k_ch, T]
    auto aoutT = op_transpose(g, aout, {0,1,3,2}, hint + "_aoutT");

    // Reshape: [1, hidden, T]
    std::string hc = hint + "_hid"; g.add_scalar_i64(hc, (int64_t)hidden);
    auto oshp = build_shape(g, {one_c, hc, T}, hint + "_oshp");
    auto merged = op_reshape(g, aoutT, oshp, hint + "_merged");

    // Output projection
    return op_conv1d(g, merged, hint + "_Wo", hint + "_bo", 1, 1, 0, hint + "_oproj");
}

// ─────────────────────────────────────────────────────────────────────────────
// FFN
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_ffn(G& g, const TensorLoader& tl,
                              const std::string& x,
                              const std::string& pfx,
                              int hidden, int filter_ch, int kernel,
                              const std::string& hint)
{
    int pad = kernel / 2;
    auto W1 = tl.load(pfx + ".conv_1.weight");
    auto b1 = tl.load(pfx + ".conv_1.bias");
    auto W2 = tl.load(pfx + ".conv_2.weight");
    auto b2 = tl.load(pfx + ".conv_2.bias");

    g.add_init_f32(hint + "_W1", {filter_ch, hidden,    kernel}, W1);
    g.add_init_f32(hint + "_b1", {filter_ch},                    b1);
    g.add_init_f32(hint + "_W2", {hidden,    filter_ch, kernel}, W2);
    g.add_init_f32(hint + "_b2", {hidden},                       b2);

    auto h  = op_conv1d(g, x, hint + "_W1", hint + "_b1", kernel, 1, pad, hint + "_c1");
    auto hr = op_relu(g, h, hint + "_relu");
    return op_conv1d(g, hr, hint + "_W2", hint + "_b2", kernel, 1, pad, hint + "_c2");
}

// ─────────────────────────────────────────────────────────────────────────────
// TextEncoder (enc_p)
// Returns {m_p, logs_p} each [1, inter_channels, T]
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<std::string, std::string>
emit_enc_p(G& g, const TensorLoader& tl,
           const std::string& phone,   // [1, T, phone_dim]
           const std::string& x_mask, // [1, 1, T]
           const std::string& T,      // scalar int64
           const RvcConfig& cfg)
{
    int hid   = cfg.hidden_channels;
    int inter = cfg.inter_channels;
    int filt  = cfg.filter_channels;
    int nh    = cfg.n_heads;
    int nl    = cfg.n_layers;
    int ks    = cfg.kernel_size;
    int pdim  = cfg.phone_dim;

    // emb_phone: Linear(phone_dim → hidden)
    // Weight stored as [hidden, phone_dim] → transpose to [phone_dim, hidden] for right-mul.
    {
        auto W_raw = tl.load("enc_p.emb_phone.weight");  // [hid, pdim]
        auto b_ep  = tl.load("enc_p.emb_phone.bias");    // [hid]

        // Transpose to [pdim, hid]
        std::vector<float> W_T((size_t)pdim * hid);
        for (int i = 0; i < hid; i++)
            for (int j = 0; j < pdim; j++)
                W_T[(size_t)j * hid + i] = W_raw[(size_t)i * pdim + j];

        g.add_init_f32("ep_Wt", {pdim, hid}, W_T);
        g.add_init_f32("ep_b",  {hid},        b_ep);
    }

    // phone [1, T, pdim] @ W_T [pdim, hid] → [1, T, hid]
    auto emb   = op_matmul(g, phone, "ep_Wt", "ep_mm");
    auto emb_b = op_add(g, emb, "ep_b", "ep_add");
    // Transpose to [1, hid, T]
    auto xt    = op_transpose(g, emb_b, {0, 2, 1}, "ep_tp");
    auto xlr   = op_leaky_relu(g, xt, 0.1f, "ep_lr");

    // Transformer layers
    std::string x = xlr;
    for (int i = 0; i < nl; i++) {
        std::string h   = "epl" + std::to_string(i);
        std::string ap  = "enc_p.encoder.attn_layers."   + std::to_string(i);
        std::string n1p = "enc_p.encoder.norm_layers_1." + std::to_string(i);
        std::string fp  = "enc_p.encoder.ffn_layers."    + std::to_string(i);
        std::string n2p = "enc_p.encoder.norm_layers_2." + std::to_string(i);

        auto gam1 = tl.load(n1p + ".gamma");
        auto bet1 = tl.load(n1p + ".beta");
        auto gam2 = tl.load(n2p + ".gamma");
        auto bet2 = tl.load(n2p + ".beta");

        g.add_init_f32(h + "_g1", {hid}, gam1);
        g.add_init_f32(h + "_b1", {hid}, bet1);
        g.add_init_f32(h + "_g2", {hid}, gam2);
        g.add_init_f32(h + "_b2", {hid}, bet2);

        auto attn = emit_attention(g, tl, x, T, ap, hid, nh, h + "_at");
        auto res1 = op_add(g, x, attn, h + "_r1");
        auto n1   = emit_layer_norm(g, res1, h + "_g1", h + "_b1", h + "_ln1");

        auto ffn  = emit_ffn(g, tl, n1, fp, hid, filt, ks, h + "_ff");
        auto res2 = op_add(g, n1, ffn, h + "_r2");
        x         = emit_layer_norm(g, res2, h + "_g2", h + "_b2", h + "_ln2");
    }

    // proj: Conv1d(hid → 2*inter, k=1) → split to m_p, logs_p
    {
        auto W_proj = tl.load("enc_p.proj.weight");  // [2*inter, hid, 1]
        auto b_proj = tl.load("enc_p.proj.bias");    // [2*inter]
        g.add_init_f32("ep_pw", {2*inter, hid, 1}, W_proj);
        g.add_init_f32("ep_pb", {2*inter},          b_proj);
    }
    auto proj  = op_conv1d(g, x, "ep_pw", "ep_pb", 1, 1, 0, "ep_proj");

    // Split along axis 1 at inter
    static constexpr int64_t kBig = INT64_MAX;
    auto m_p    = op_slice(g, proj, {0, 0,     0}, {1, inter,   kBig}, {0, 1, 2}, "m_p");
    auto logs_p = op_slice(g, proj, {0, inter, 0}, {1, 2*inter, kBig}, {0, 1, 2}, "logs_p");

    // Mask multiply (all-ones → identity)
    auto m_pm    = op_mul(g, m_p,    x_mask, "m_pm");
    auto logs_pm = op_mul(g, logs_p, x_mask, "logs_pm");
    return {m_pm, logs_pm};
}

// ─────────────────────────────────────────────────────────────────────────────
// WaveNet (WN) inside ResidualCouplingLayer
// x: [1, half_ch, T];  x_mask: [1, 1, T];  g_cond: [1, gin_ch, 1]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_wn(G& g, const TensorLoader& tl,
                             const std::string& x,
                             const std::string& x_mask,
                             const std::string& g_cond,
                             const std::string& pfx,   // e.g. "flow.flows.0.enc"
                             int ch,                   // half = 96
                             const std::string& hint)
{
    // cond_layer: weight-normed Conv1d [n_layers*2*ch, gin_ch, 1]
    auto cond_w = tl.load_wn(pfx + ".cond_layer");
    auto cond_sh = tl.shape_wn(pfx + ".cond_layer");
    int gin_ch  = (int)cond_sh[1];
    int n_lay   = (int)(cond_sh[0] / (2 * ch));

    g.add_init_f32(hint + "_cw", cond_sh, cond_w);
    auto g_proj = op_conv1d(g, g_cond, hint + "_cw", "", 1, 1, 0, hint + "_gp");
    // g_proj: [1, n_lay*2*ch, 1]
    (void)gin_ch;

    // Initialise result = zeros(x)
    std::string zero_f = hint + "_zf"; g.add_scalar_f32(zero_f, 0.f);
    auto result = op_mul(g, x, zero_f, hint + "_r0");

    std::string cur = x;
    for (int i = 0; i < n_lay; i++) {
        std::string lh   = hint + "_wl" + std::to_string(i);
        std::string ipfx = pfx + ".in_layers."       + std::to_string(i);
        std::string rpfx = pfx + ".res_skip_layers." + std::to_string(i);

        // in_layers: weight-normed Conv1d [2*ch, ch, k] dilation
        auto in_w  = tl.load_wn(ipfx);
        auto in_b  = tl.load(ipfx + ".bias");
        auto in_sh = tl.shape_wn(ipfx);
        int  in_k  = (int)in_sh[2];
        int  in_d  = 1;   // standard WN uses dilation=1 per layer unless multirate
        int  in_p  = in_k / 2;
        g.add_init_f32(lh + "_iw", in_sh, in_w);
        g.add_init_f32(lh + "_ib", {in_sh[0]}, in_b);

        auto cx  = op_mul(g, cur, x_mask, lh + "_xm");
        auto h   = op_conv1d(g, cx, lh + "_iw", lh + "_ib", in_k, in_d, in_p, lh + "_h");
        // h: [1, 2*ch, T]

        // Add g_cond slice for this layer
        int64_t ls = (int64_t)i * 2 * ch;
        int64_t le = ls + 2 * ch;
        auto gs = op_slice(g, g_proj, {0, ls, 0}, {1, le, 1}, {0, 1, 2}, lh + "_gs");
        auto hg = op_add(g, h, gs, lh + "_hg");

        // Gated activation
        auto ht   = op_slice(g, hg, {0, 0,  0}, {1, ch,    INT64_MAX}, {0,1,2}, lh + "_ht");
        auto hs   = op_slice(g, hg, {0, ch, 0}, {1, 2*ch,  INT64_MAX}, {0,1,2}, lh + "_hs");
        auto acts = op_mul(g, op_tanh(g, ht, lh + "_th"), op_sigmoid(g, hs, lh + "_sg"),
                            lh + "_acts");
        // acts: [1, ch, T]

        // res_skip_layers: weight-normed Conv1d
        auto rs_w  = tl.load_wn(rpfx);
        auto rs_b  = tl.load(rpfx + ".bias");
        auto rs_sh = tl.shape_wn(rpfx);
        g.add_init_f32(lh + "_rw", rs_sh, rs_w);
        g.add_init_f32(lh + "_rb", {rs_sh[0]}, rs_b);
        auto rs = op_conv1d(g, acts, lh + "_rw", lh + "_rb", 1, 1, 0, lh + "_rs");
        // rs: [1, 2*ch or ch, T]

        bool is_last = (i == n_lay - 1);
        if (!is_last) {
            auto res_part  = op_slice(g, rs, {0, 0,  0}, {1, ch,   INT64_MAX}, {0,1,2}, lh + "_rp");
            auto skip_part = op_slice(g, rs, {0, ch, 0}, {1, 2*ch, INT64_MAX}, {0,1,2}, lh + "_sp");
            auto xr = op_add(g, cur, res_part, lh + "_xr");
            cur     = op_mul(g, xr, x_mask, lh + "_xrm");
            result  = op_add(g, result, skip_part, lh + "_racc");
        } else {
            result = op_add(g, result, rs, lh + "_racc");
        }
    }
    return op_mul(g, result, x_mask, hint + "_wout");
}

// ─────────────────────────────────────────────────────────────────────────────
// ResidualCouplingLayer reverse (mean_only=True inference)
// x: [1, 192, T] → [1, 192, T]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_coupling_rev(G& g, const TensorLoader& tl,
                                      const std::string& x,
                                      const std::string& x_mask,
                                      const std::string& g_cond,
                                      const std::string& pfx,
                                      int hidden,
                                      const std::string& hint)
{
    int half = hidden / 2;

    auto x0 = op_slice(g, x, {0, 0,    0}, {1, half,   INT64_MAX}, {0,1,2}, hint + "_x0");
    auto x1 = op_slice(g, x, {0, half, 0}, {1, hidden, INT64_MAX}, {0,1,2}, hint + "_x1");

    // pre: Conv1d [hidden, half, 1]
    {
        auto pw = tl.load(pfx + ".pre.weight");
        auto pb = tl.load(pfx + ".pre.bias");
        g.add_init_f32(hint + "_pw", {hidden, half, 1}, pw);
        g.add_init_f32(hint + "_pb", {hidden},           pb);
    }
    auto hpre = op_conv1d(g, x0, hint + "_pw", hint + "_pb", 1, 1, 0, hint + "_pre");
    auto hprem = op_mul(g, hpre, x_mask, hint + "_prem");

    // WN
    auto hwn = emit_wn(g, tl, hprem, x_mask, g_cond, pfx + ".enc", hidden, hint + "_wn");

    // post: Conv1d [half, hidden, 1], no weight norm, mean_only output=half
    {
        auto pw = tl.load(pfx + ".post.weight");
        auto pb = tl.load(pfx + ".post.bias");
        g.add_init_f32(hint + "_postw", {half, hidden, 1}, pw);
        g.add_init_f32(hint + "_postb", {half},             pb);
    }
    auto m   = op_conv1d(g, hwn, hint + "_postw", hint + "_postb", 1, 1, 0, hint + "_m");
    auto mm  = op_mul(g, m, x_mask, hint + "_mm");

    // Reverse: x1_new = (x1 - m) * x_mask
    auto x1s = op_sub(g, x1, mm, hint + "_x1s");
    auto x1n = op_mul(g, x1s, x_mask, hint + "_x1n");

    return op_concat(g, {x0, x1n}, 1, hint + "_out");
}

// ─────────────────────────────────────────────────────────────────────────────
// Flow (ResidualCouplingBlock) in reverse
// Order: Flip, CL6_rev, Flip, CL4_rev, Flip, CL2_rev, Flip, CL0_rev
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_flow_reverse(G& g, const TensorLoader& tl,
                                      const std::string& x,
                                      const std::string& x_mask,
                                      const std::string& g_cond,
                                      int hidden,
                                      const std::string& hint)
{
    // Channel flip index: [hidden-1, hidden-2, ..., 0]
    std::vector<int64_t> flip_idx((size_t)hidden);
    for (int i = 0; i < hidden; i++) flip_idx[(size_t)i] = hidden - 1 - i;
    g.add_init_i64("fl_idx", {(int64_t)hidden}, flip_idx);

    auto flip = [&](const std::string& inp, const std::string& h) {
        return op_gather(g, inp, "fl_idx", 1, h + "_flip");
    };

    // Coupling layer PyTorch indices: 0, 2, 4, 6
    // Reverse order: 6, 4, 2, 0
    int cl_idx[4] = {6, 4, 2, 0};
    std::string cur = x;
    for (int iter = 0; iter < 4; iter++) {
        std::string lh = hint + "_f" + std::to_string(iter);
        cur = flip(cur, lh);
        std::string pfx = "flow.flows." + std::to_string(cl_idx[iter]);
        cur = emit_coupling_rev(g, tl, cur, x_mask, g_cond, pfx, hidden, lh + "_cl");
    }
    return cur;
}

// ─────────────────────────────────────────────────────────────────────────────
// ResBlock1
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_resblock1(G& g, const TensorLoader& tl,
                                   const std::string& x,
                                   const std::string& pfx,
                                   int /*ch*/, int kernel,
                                   const std::vector<int>& dilations,
                                   const std::string& hint)
{
    std::string cur = x;
    for (int j = 0; j < 3; j++) {
        std::string lh  = hint + "_j" + std::to_string(j);
        std::string c1p = pfx + ".convs1." + std::to_string(j);
        std::string c2p = pfx + ".convs2." + std::to_string(j);
        int dil = dilations[(size_t)j];
        int pad = (kernel - 1) * dil / 2;
        int p2  = kernel / 2;  // conv2 padding (dilation=1)

        auto c1_w = tl.load_wn(c1p);
        auto c1_b = tl.load(c1p + ".bias");
        auto c2_w = tl.load_wn(c2p);
        auto c2_b = tl.load(c2p + ".bias");
        auto c1_sh = tl.shape_wn(c1p);
        auto c2_sh = tl.shape_wn(c2p);

        g.add_init_f32(lh + "_c1w", c1_sh, c1_w);
        g.add_init_f32(lh + "_c1b", {c1_sh[0]}, c1_b);
        g.add_init_f32(lh + "_c2w", c2_sh, c2_w);
        g.add_init_f32(lh + "_c2b", {c2_sh[0]}, c2_b);

        auto xa  = op_leaky_relu(g, cur, 0.1f, lh + "_lr1");
        auto h1  = op_conv1d(g, xa, lh + "_c1w", lh + "_c1b", kernel, dil, pad, lh + "_h1");
        auto h1r = op_leaky_relu(g, h1, 0.1f, lh + "_lr2");
        auto h2  = op_conv1d(g, h1r, lh + "_c2w", lh + "_c2b", kernel, 1, p2, lh + "_h2");
        cur = op_add(g, cur, h2, lh + "_out");
    }
    return cur;
}

// ─────────────────────────────────────────────────────────────────────────────
// GeneratorNSF decoder
// z: [1, inter_channels, T];  g_cond: [1, gin_ch, 1];  f0: [1, T]
// Returns: audio [1, 1, M]
// ─────────────────────────────────────────────────────────────────────────────
static std::string emit_decoder(G& g, const TensorLoader& tl,
                                  const std::string& z,
                                  const std::string& g_cond,
                                  const std::string& f0,
                                  const std::string& T,
                                  const RvcConfig& cfg)
{
    int inter  = cfg.inter_channels;       // 192
    int upinit = cfg.upsample_initial_channel; // 512
    int sr     = cfg.sr;                   // 40000
    int gin_ch = cfg.gin_channels;         // 256

    // Total upsampling factor: 10*10*2*2 = 400
    int upp = 1;
    for (int r : cfg.upsample_rates) upp *= r;

    // ── f0 upsampling: repeat_interleave(upp) ────────────────────────────────
    // f0: [1, T] → [1, 1, T, 1] → Expand [1, 1, T, upp] → Reshape [1, 1, T*upp]
    //   → Transpose → [1, T*400, 1]
    std::string one_c = "dc_1"; g.add_scalar_i64(one_c, 1LL);
    std::string upp_c = "dc_u"; g.add_scalar_i64(upp_c, (int64_t)upp);

    auto f0_3 = op_unsqueeze(g, f0,   1, "f0_3d");   // [1, 1, T]
    auto f0_4 = op_unsqueeze(g, f0_3, 3, "f0_4d");   // [1, 1, T, 1]
    auto T_upp = op_mul(g, T, upp_c, "T_upp");        // T * 400
    auto exp_shp = build_shape(g, {one_c, one_c, T, upp_c}, "f0_eshp");
    auto f0_exp  = g.emit("Expand", {f0_4, exp_shp}, {}, "f0_exp");  // [1,1,T,upp]
    auto f0_rs_shp = build_shape(g, {one_c, one_c, T_upp}, "f0_rsshp");
    auto f0_ri   = op_reshape(g, f0_exp, f0_rs_shp, "f0_ri");        // [1,1,T*upp]
    auto f0_up   = op_transpose(g, f0_ri, {0, 2, 1}, "f0_up");       // [1,T*400,1]

    // ── SineGen ──────────────────────────────────────────────────────────────
    // phase = CumSum(f0_up / sr, axis=1) % 1.0
    std::string sr_c = "sg_sr"; g.add_scalar_f32(sr_c, (float)sr);
    auto freq    = op_div(g, f0_up, sr_c, "sg_freq");   // [1,T*400,1]
    // CumSum axis=1: axis is a 1-D int64 scalar
    std::string cs_ax = "cs_ax"; g.add_scalar_i64(cs_ax, 1LL);
    auto phase   = g.emit("CumSum", {freq, cs_ax}, {G::attr_int("exclusive", 0)}, "sg_phase");
    // Mod by 1.0 (fmod mode)
    std::string one_f = "sg_1f"; g.add_scalar_f32(one_f, 1.f);
    auto pmod    = g.emit("Mod", {phase, one_f}, {G::attr_int("fmod", 1)}, "sg_pmod");
    // rad = pmod * 2*pi; sin; scale by 0.1
    std::string twopi_c = "sg_2pi"; g.add_scalar_f32(twopi_c, 6.283185307179586f);
    auto rad     = op_mul(g, pmod, twopi_c, "sg_rad");
    auto sine    = g.emit("Sin", {rad}, {}, "sg_sin");            // [1,T*400,1]
    std::string amp_c = "sg_amp"; g.add_scalar_f32(amp_c, 0.1f);
    auto sine_s  = op_mul(g, sine, amp_c, "sg_sins");
    // voiced/unvoiced mask: uv = float(f0_up > 0)
    std::string zero_f = "sg_zf"; g.add_scalar_f32(zero_f, 0.f);
    auto uv_b    = g.emit("Greater", {f0_up, zero_f}, {}, "sg_uvb");
    auto uv      = g.emit("Cast", {uv_b}, {G::attr_int("to", kDtFloat)}, "sg_uv");
    auto sine_wavs = op_mul(g, sine_s, uv, "sg_wavs");            // [1,T*400,1]

    // ── SourceModuleHnNSF ─────────────────────────────────────────────────────
    // l_linear: weight [1,1], bias [1]
    {
        auto llw = tl.load("dec.m_source.l_linear.weight");
        auto llb = tl.load("dec.m_source.l_linear.bias");
        g.add_init_f32("llw", {1, 1}, llw);
        g.add_init_f32("llb", {1},    llb);
    }
    // Linear on [1, T*400, 1]: MatMul with [1,1] + bias [1] → Tanh
    auto har_lin  = op_matmul(g, sine_wavs, "llw", "har_mm");
    auto har_add  = op_add(g, har_lin, "llb", "har_add");
    auto har_tanh = op_tanh(g, har_add, "har_tanh");
    // Transpose to [1, 1, T*400]
    auto har = op_transpose(g, har_tanh, {0, 2, 1}, "har_tp");   // [1, 1, T*400]

    // ── GeneratorNSF forward ──────────────────────────────────────────────────
    // conv_pre: Conv1d(inter→upinit, k=7, pad=3)
    {
        auto cpw = tl.load("dec.conv_pre.weight");
        auto cpb = tl.load("dec.conv_pre.bias");
        g.add_init_f32("dc_cpw", {upinit, inter, 7}, cpw);
        g.add_init_f32("dc_cpb", {upinit},            cpb);
    }
    auto xd = op_conv1d(g, z, "dc_cpw", "dc_cpb", 7, 1, 3, "dc_pre");

    // cond: Conv1d(gin_ch→upinit, k=1)
    {
        auto cw = tl.load("dec.cond.weight");
        auto cb = tl.load("dec.cond.bias");
        g.add_init_f32("dc_cw", {upinit, gin_ch, 1}, cw);
        g.add_init_f32("dc_cb", {upinit},             cb);
    }
    auto cond_out = op_conv1d(g, g_cond, "dc_cw", "dc_cb", 1, 1, 0, "dc_cond");
    xd = op_add(g, xd, cond_out, "dc_xcond");

    // noise_conv parameters (given in spec)
    struct NcInfo { int ch_out; int k, stride, pad; };
    // ch_out follows the up-stage ch_out (upinit/2, /4, /8, /16)
    const NcInfo nc_info[4] = {
        {upinit/2,  80, 40, 20},
        {upinit/4,   8,  4,  2},
        {upinit/8,   4,  2,  1},
        {upinit/16,  1,  1,  0}
    };

    int ch_cur = upinit;
    for (int i = 0; i < (int)cfg.upsample_rates.size(); i++) {
        std::string lh = "dc_up" + std::to_string(i);
        int stride = cfg.upsample_rates[(size_t)i];
        int kup    = cfg.upsample_kernel_sizes[(size_t)i];
        int pad_up = (kup - stride) / 2;
        int ch_out = ch_cur / 2;

        // ConvTranspose1d: weight_v shape [C_in, C_out, K]
        {
            std::string upfx = "dec.ups." + std::to_string(i);
            auto uw   = tl.load_wn(upfx);
            auto ub   = tl.load(upfx + ".bias");
            auto ush  = tl.shape_wn(upfx);
            // ush = [C_in, C_out, K];  bias = [C_out] = ush[1]
            g.add_init_f32(lh + "_uw", ush, uw);
            g.add_init_f32(lh + "_ub", {ush[1]}, ub);
        }

        xd = op_leaky_relu(g, xd, 0.1f, lh + "_lr");
        xd = op_convtranspose1d(g, xd, lh + "_uw", lh + "_ub", kup, stride, pad_up, lh + "_up");
        // xd: [1, ch_out, T * product_of_ups_so_far]

        // noise_conv[i] on har [1, 1, T*400]
        {
            std::string ncpfx = "dec.noise_convs." + std::to_string(i);
            auto nw = tl.load(ncpfx + ".weight");
            auto nb = tl.load(ncpfx + ".bias");
            const NcInfo& ni = nc_info[i];
            g.add_init_f32(lh + "_nw", {ni.ch_out, 1, ni.k}, nw);
            g.add_init_f32(lh + "_nb", {ni.ch_out},           nb);
            auto nc_out = op_conv1d(g, har, lh + "_nw", lh + "_nb",
                                     ni.k, 1, ni.pad, lh + "_nc", ni.stride);
            xd = op_add(g, xd, nc_out, lh + "_xn");
        }

        // ResBlocks
        int n_rb = (int)cfg.resblock_kernel_sizes.size();
        std::string rb_sum;
        for (int rb = 0; rb < n_rb; rb++) {
            std::string rbh  = lh + "_rb" + std::to_string(rb);
            int rb_idx       = i * n_rb + rb;
            std::string rbpfx = "dec.resblocks." + std::to_string(rb_idx);
            int rk = cfg.resblock_kernel_sizes[(size_t)rb];
            auto& rds = cfg.resblock_dilation_sizes[(size_t)rb];
            auto rb_out = emit_resblock1(g, tl, xd, rbpfx, ch_out, rk,
                                          {rds[0], rds[1], rds[2]}, rbh);
            rb_sum = (rb == 0) ? rb_out
                               : op_add(g, rb_sum, rb_out, lh + "_rbs" + std::to_string(rb));
        }
        std::string nrb_c = lh + "_nrb"; g.add_scalar_f32(nrb_c, (float)n_rb);
        xd = op_div(g, rb_sum, nrb_c, lh + "_rbavg");
        ch_cur = ch_out;
    }
    // ch_cur = 32 after 4 stages

    xd = op_leaky_relu(g, xd, 0.1f, "dc_lrpost");
    // conv_post: Conv1d(ch_cur→1, k=7, pad=3), no bias
    {
        auto pw = tl.load("dec.conv_post.weight");
        g.add_init_f32("dc_postw", {1, (int64_t)ch_cur, 7}, pw);
    }
    auto audio_raw = op_conv1d(g, xd, "dc_postw", "", 7, 1, 3, "dc_post");
    return op_tanh(g, audio_raw, "dc_tanh");   // [1, 1, M]
}

// ─────────────────────────────────────────────────────────────────────────────
// pth_to_onnx — main entry point
// ─────────────────────────────────────────────────────────────────────────────

std::string pth_to_onnx(const PthModel& m, const std::string& out_path)
{
    if (!m.err.empty()) return "PthModel has error: " + m.err;

    try {
        const RvcConfig& cfg = m.config;
        TensorLoader tl{m};
        OnnxGraph g;

        // ── Graph inputs ──────────────────────────────────────────────────────
        g.add_input("phone", kDtFloat, {1, -1, cfg.phone_dim});
        g.add_input("f0",    kDtFloat, {1, -1});
        g.add_input("sid",   kDtInt64, {1});

        // ── Graph output ──────────────────────────────────────────────────────
        g.add_output("audio", kDtFloat, {1, 1, -1});

        // ── T: sequence length ────────────────────────────────────────────────
        // phone: [1, T, phone_dim]; T is dim 1.
        auto T_scalar = shape_dim(g, "phone", 1, "T");

        // ── x_mask: all-ones [1, 1, T] ───────────────────────────────────────
        {
            std::string m1 = "xm_1"; g.add_scalar_i64(m1, 1LL);
            auto shp = build_shape(g, {m1, m1, T_scalar}, "xm_shp");
            // ConstantOfShape with value = 1.0 float
            PbBuf val_a;
            val_a.tag_string(1, "value");
            // AttributeProto.t field (field 5 in proto3, or build as tensor attribute)
            // type=TENSOR=4, t=field 5
            PbBuf tp;
            tp.tag_varint(1, 1);  // dims = [1] (one-element tensor required by spec)
            tp.tag_varint(2, kDtFloat);
            tp.tag_string(8, "xm_val");
            float one_f = 1.f;
            tp.tag_bytes(9, &one_f, sizeof(float));
            val_a.tag_msg(5, tp);
            val_a.tag_varint(20, 4);  // AttributeProto type = TENSOR
            g.add_node("ConstantOfShape", {shp}, {"x_mask"}, "xmask_n", {val_a});
        }

        // ── Speaker embedding ─────────────────────────────────────────────────
        {
            auto ew = tl.load("emb_g.weight");
            g.add_init_f32("emb_g_w", {cfg.n_speakers, cfg.gin_channels}, ew);
        }
        // sid: [1] int64 → scalar
        auto sid_sc  = op_squeeze(g, "sid", 0, "sid_sc");
        auto g_emb   = op_gather(g, "emb_g_w", sid_sc, 0, "g_emb");    // [gin_ch]
        auto g_emb2  = op_unsqueeze(g, g_emb,  0, "g_emb2");           // [1, gin_ch]
        auto g_cond  = op_unsqueeze(g, g_emb2, 2, "g_cond");           // [1, gin_ch, 1]

        // ── TextEncoder ───────────────────────────────────────────────────────
        auto [m_p, logs_p] = emit_enc_p(g, tl, "phone", "x_mask", T_scalar, cfg);
        (void)logs_p;  // not used at inference (mean-only sampling)
        std::string z = m_p;  // z = m_p  (deterministic, noise-free)

        // ── Flow (reverse) ────────────────────────────────────────────────────
        z = emit_flow_reverse(g, tl, z, "x_mask", g_cond, cfg.inter_channels, "fl");

        // ── Decoder ───────────────────────────────────────────────────────────
        auto audio_out = emit_decoder(g, tl, z, g_cond, "f0", T_scalar, cfg);
        // audio_out: [1, 1, M]

        // ── Rename final output to "audio" ────────────────────────────────────
        g.add_node("Identity", {audio_out}, {"audio"}, "audio_id");

        // ── Serialise ─────────────────────────────────────────────────────────
        auto bytes = g.serialise("rvc_vits");

        {
            std::ofstream ofs(out_path, std::ios::binary);
            if (!ofs) return "Cannot open for writing: " + out_path;
            ofs.write(reinterpret_cast<const char*>(bytes.data()),
                      (std::streamsize)bytes.size());
            if (!ofs) return "Write error: " + out_path;
        }

        // ── Sidecar JSON ──────────────────────────────────────────────────────
        {
            std::string jp = out_path;
            auto dot = jp.rfind('.');
            if (dot != std::string::npos) jp = jp.substr(0, dot);
            jp += ".json";
            std::ofstream jf(jp);
            if (!jf) return "Cannot write sidecar JSON: " + jp;
            jf << "{\"target_sr\":" << cfg.sr
               << ",\"phone_dim\":" << cfg.phone_dim << "}";
        }

        return "";  // success

    } catch (const std::exception& e) {
        return std::string("pth_to_onnx error: ") + e.what();
    }
}
