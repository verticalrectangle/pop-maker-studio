// Standalone test for pth_reader — compile with:
//   g++ -std=c++17 -I../src tools/test_pth_reader.cpp src/pth_reader.cpp -o /tmp/test_pth && /tmp/test_pth <model.pth>
#include "pth_reader.h"
#include <cstdio>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_pth <model.pth>\n");
        return 1;
    }

    printf("Opening %s …\n", argv[1]);
    PthModel m = pth_open(argv[1]);
    if (!m.err.empty()) { fprintf(stderr, "Error: %s\n", m.err.c_str()); return 1; }

    printf("\nConfig:\n");
    printf("  spec_channels        = %d\n", m.config.spec_channels);
    printf("  inter_channels       = %d\n", m.config.inter_channels);
    printf("  hidden_channels      = %d\n", m.config.hidden_channels);
    printf("  filter_channels      = %d\n", m.config.filter_channels);
    printf("  n_heads              = %d\n", m.config.n_heads);
    printf("  n_layers             = %d\n", m.config.n_layers);
    printf("  kernel_size          = %d\n", m.config.kernel_size);
    printf("  resblock             = %s\n", m.config.resblock.c_str());
    printf("  upsample_initial_ch  = %d\n", m.config.upsample_initial_channel);
    printf("  n_speakers           = %d\n", m.config.n_speakers);
    printf("  gin_channels         = %d\n", m.config.gin_channels);
    printf("  sr                   = %d\n", m.config.sr);
    printf("  phone_dim            = %d\n", m.config.phone_dim);
    printf("  upsample_rates       = [");
    for (int r : m.config.upsample_rates) printf("%d,", r); printf("]\n");
    printf("  upsample_kernel_sizes= [");
    for (int k : m.config.upsample_kernel_sizes) printf("%d,", k); printf("]\n");
    printf("  resblock_kernels     = [");
    for (int k : m.config.resblock_kernel_sizes) printf("%d,", k); printf("]\n");

    printf("\n%zu tensors:\n", m.tensors.size());

    // Sort by name for consistent output
    std::vector<std::string> keys;
    for (auto& p : m.tensors) keys.push_back(p.first);
    std::sort(keys.begin(), keys.end());

    size_t total_params = 0;
    for (auto& k : keys) {
        const TensorMeta& tm = m.tensors[k];
        int64_t n = 1; for (int64_t d : tm.shape) n *= d;
        total_params += (size_t)n;
        printf("  %-60s  dtype=%-4s  shape=[", k.c_str(), tm.dtype.c_str());
        for (size_t i = 0; i < tm.shape.size(); i++) {
            printf("%lld", (long long)tm.shape[i]);
            if (i + 1 < tm.shape.size()) printf(",");
        }
        printf("]\n");
    }
    printf("Total params: %zu  (~%.1f MB as f16, ~%.1f MB as f32)\n",
           total_params,
           total_params * 2.0 / 1024 / 1024,
           total_params * 4.0 / 1024 / 1024);

    // Load a small tensor to verify data reading
    printf("\nLoading enc_p.emb_phone.weight to verify data read…\n");
    std::vector<float> w = pth_load_tensor(m, "enc_p.emb_phone.weight");
    if (w.empty()) { printf("  FAILED\n"); }
    else {
        float mn = *std::min_element(w.begin(), w.end());
        float mx = *std::max_element(w.begin(), w.end());
        float sum = 0.f; for (float f : w) sum += f;
        printf("  size=%zu  min=%.4f  max=%.4f  mean=%.6f\n",
               w.size(), mn, mx, sum / w.size());
    }

    // Load speaker embedding
    printf("Loading emb_g.weight to verify speaker embedding…\n");
    w = pth_load_tensor(m, "emb_g.weight");
    if (w.empty()) { printf("  FAILED or not present\n"); }
    else {
        printf("  size=%zu  first 4 values: %.4f %.4f %.4f %.4f\n",
               w.size(), w[0], w[1], w[2], w[3]);
    }

    pth_close(m);
    printf("\nDone. Temp dir cleaned up.\n");
    return 0;
}
