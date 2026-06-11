// faiss_ivf.cpp — IndexIVFFlat reader + retrieval blend, no faiss dependency.
//
// File layout (faiss index_write.cpp):
//   "IwFl" | index_header | nlist u64 | nprobe u64
//   quantizer: "IxF2"/"IxFI"/"IxFl" | index_header | vector(codes)
//   direct_map: type u8 | vector(u64 ids)
//   invlists: "ilar" | nlist u64 | code_size u64 | "full"
//             | vector(sizes u64) | per list: codes then ids
// index_header: d i32 | ntotal i64 | dummy i64 ×2 | is_trained u8
//               | metric i32 | (metric_arg f32 if metric > 1)
// vector(T): count u64 then count elements. Old IndexFlat stores
// vector<float> (count = elements); new stores vector<uint8> (count = bytes).
#include "faiss_ivf.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <algorithm>

namespace {

struct Reader {
    std::ifstream f;
    bool ok = true;

    explicit Reader(const std::string& p) : f(p, std::ios::binary) {
        ok = f.good();
    }
    void raw(void* dst, size_t n) {
        if (!f.read((char*)dst, (std::streamsize)n)) ok = false;
    }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    int32_t  i32() { int32_t  v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    uint8_t  u8()  { uint8_t  v = 0; raw(&v, 1); return v; }

    // index_header → (d, ntotal); fails the reader on nonsense values
    void header(int& d, int64_t& ntotal) {
        d = i32();
        ntotal = (int64_t)u64();
        f.seekg(16, std::ios::cur);   // dummies
        (void)u8();                   // is_trained
        int metric = i32();
        if (metric > 1) f.seekg(4, std::ios::cur);
        if (d <= 0 || d > 8192 || ntotal < 0) ok = false;
    }
};

constexpr uint32_t fcc(const char a[5]) {
    return (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
           ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
}

} // namespace

FaissIVF faiss_ivf_load(const std::string& path) {
    FaissIVF ix;
    Reader r(path);
    if (!r.ok) { ix.err = "cannot open " + path; return ix; }

    if (r.u32() != fcc("IwFl")) { ix.err = "not an IndexIVFFlat file"; return ix; }
    int d = 0; int64_t ntotal = 0;
    r.header(d, ntotal);
    uint64_t nlist = r.u64();
    (void)r.u64();                       // stored nprobe (we use our own)

    // Quantizer: IndexFlat (any metric variant)
    uint32_t qf = r.u32();
    if (qf != fcc("IxF2") && qf != fcc("IxFI") && qf != fcc("IxFl")) {
        ix.err = "unsupported quantizer"; return ix;
    }
    int qd = 0; int64_t qn = 0;
    r.header(qd, qn);
    if (!r.ok || qd != d || (uint64_t)qn != nlist) {
        ix.err = "quantizer header mismatch"; return ix;
    }
    uint64_t cnt = r.u64();
    uint64_t want = (uint64_t)qn * (uint64_t)qd;       // element count
    uint64_t nbytes = (cnt == want) ? cnt * 4          // old: vector<float>
                    : (cnt == want * 4) ? cnt          // new: vector<uint8>
                    : 0;
    if (!nbytes) { ix.err = "unexpected quantizer code count"; return ix; }
    ix.centroids.resize(want);
    r.raw(ix.centroids.data(), nbytes);

    // Direct map
    (void)r.u8();
    uint64_t dm_n = r.u64();
    r.f.seekg((std::streamoff)(dm_n * 8), std::ios::cur);

    // Inverted lists
    if (r.u32() != fcc("ilar")) { ix.err = "unsupported inverted lists"; return ix; }
    uint64_t il_nlist  = r.u64();
    uint64_t code_size = r.u64();
    if (il_nlist != nlist || code_size != (uint64_t)d * 4) {
        ix.err = "inverted list header mismatch"; return ix;
    }
    if (r.u32() != fcc("full")) { ix.err = "only 'full' list format supported"; return ix; }
    uint64_t ns = r.u64();
    if (ns != nlist) { ix.err = "list size count mismatch"; return ix; }
    std::vector<uint64_t> sizes(ns);
    r.raw(sizes.data(), ns * 8);

    ix.vectors.resize((size_t)ntotal * d);
    ix.list_offsets.resize(nlist + 1, 0);
    int64_t pos = 0;
    for (uint64_t l = 0; l < nlist; l++) {
        ix.list_offsets[(size_t)l] = pos;
        uint64_t sz = sizes[(size_t)l];
        if (!sz) continue;
        if (pos + (int64_t)sz > ntotal) { ix.err = "list sizes exceed ntotal"; return ix; }
        r.raw(ix.vectors.data() + (size_t)pos * d, sz * code_size);
        r.f.seekg((std::streamoff)(sz * 8), std::ios::cur);   // skip ids
        pos += (int64_t)sz;
    }
    ix.list_offsets[(size_t)nlist] = pos;

    if (!r.ok || pos != ntotal) { ix.err = "truncated index file"; return ix; }
    ix.dim    = d;
    ix.ntotal = (int)ntotal;
    return ix;
}

void faiss_ivf_blend(const FaissIVF& ix, float* feats, int T,
                     float rate, int k, int nprobe) {
    if (ix.dim <= 0 || ix.ntotal == 0 || rate <= 0.f) return;
    const int D     = ix.dim;
    const int nlist = (int)ix.list_offsets.size() - 1;
    nprobe = std::min(nprobe, nlist);

    std::vector<int>   probe(nprobe);
    std::vector<float> probe_d(nprobe);
    std::vector<std::pair<float, int64_t>> top;   // (dist², row)
    std::vector<float> mixed(D);

    for (int t = 0; t < T; t++) {
        float* q = feats + (size_t)t * D;

        // nprobe closest centroids (insertion into a small sorted list)
        int np = 0;
        for (int l = 0; l < nlist; l++) {
            const float* c = ix.centroids.data() + (size_t)l * D;
            float acc = 0.f;
            for (int j = 0; j < D; j++) { float v = q[j] - c[j]; acc += v * v; }
            int at = np;
            while (at > 0 && probe_d[(size_t)at-1] > acc) at--;
            if (at < nprobe) {
                int last = std::min(np, nprobe - 1);
                for (int m = last; m > at; m--) {
                    probe[(size_t)m] = probe[(size_t)m-1];
                    probe_d[(size_t)m] = probe_d[(size_t)m-1];
                }
                probe[(size_t)at] = l; probe_d[(size_t)at] = acc;
                if (np < nprobe) np++;
            }
        }

        // k nearest vectors within the probed lists
        top.clear();
        for (int p = 0; p < np; p++) {
            int64_t lo = ix.list_offsets[(size_t)probe[(size_t)p]];
            int64_t hi = ix.list_offsets[(size_t)probe[(size_t)p] + 1];
            for (int64_t row = lo; row < hi; row++) {
                const float* v = ix.vectors.data() + (size_t)row * D;
                float acc = 0.f;
                for (int j = 0; j < D; j++) { float x = q[j] - v[j]; acc += x * x; }
                if ((int)top.size() < k) {
                    top.push_back({acc, row});
                    std::push_heap(top.begin(), top.end());
                } else if (acc < top.front().first) {
                    std::pop_heap(top.begin(), top.end());
                    top.back() = {acc, row};
                    std::push_heap(top.begin(), top.end());
                }
            }
        }
        if (top.empty()) continue;

        // weights ∝ 1/d² (RVC), then blend
        float wsum = 0.f;
        std::fill(mixed.begin(), mixed.end(), 0.f);
        for (auto& [dist, row] : top) {
            float w = 1.f / std::fmax(dist * dist, 1e-12f);
            wsum += w;
            const float* v = ix.vectors.data() + (size_t)row * D;
            for (int j = 0; j < D; j++) mixed[(size_t)j] += w * v[j];
        }
        for (int j = 0; j < D; j++)
            q[j] = rate * (mixed[(size_t)j] / wsum) + (1.f - rate) * q[j];
    }
}
