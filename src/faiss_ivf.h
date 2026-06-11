#pragma once
// Minimal reader for faiss IndexIVFFlat files (RVC voice .index siblings) —
// no faiss dependency. Supports feature retrieval: blend each query frame
// with its k nearest training-set vectors (RVC's index_rate mechanism),
// which pulls HuBERT features toward the target voice's training data.
#include <string>
#include <vector>

struct FaissIVF {
    int dim    = 0;          // vector dimensionality (768 v2 / 256 v1)
    int ntotal = 0;
    std::vector<float>   centroids;    // [nlist, dim]
    std::vector<float>   vectors;      // [ntotal, dim] grouped by list
    std::vector<int64_t> list_offsets; // nlist+1 prefix sums into vectors
    std::string err;                   // non-empty on load failure
};

// Parse an IndexIVFFlat file written by faiss (old float-vector and new
// byte-codes IndexFlat serialisations both accepted).
FaissIVF faiss_ivf_load(const std::string& path);

// RVC retrieval: for each of the T frames in feats [T, dim], find the k
// nearest stored vectors (searching nprobe closest lists), weight them by
// 1/d² and blend: f = rate·weighted + (1−rate)·f. In place.
void faiss_ivf_blend(const FaissIVF& ix, float* feats, int T,
                     float rate, int k = 8, int nprobe = 4);
