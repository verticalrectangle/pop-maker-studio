#!/usr/bin/env python3
"""
RVC voice conversion — system Python, no fairseq.
Usage: python3 vc_infer.py <input.wav> <model.pth> <out.wav> [f0_semitones]
Prints PROGRESS:<float> to stdout.
Requirements: torch torchaudio numpy
"""
import sys, math
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torchaudio

def prog(p): print(f"PROGRESS:{p:.3f}", flush=True)

# ── utils ──────────────────────────────────────────────────────────────────────

def sequence_mask(lengths, max_len=None):
    if max_len is None:
        max_len = int(lengths.max())
    return torch.arange(max_len, device=lengths.device).unsqueeze(0) < lengths.unsqueeze(1)

# ── LayerNorm for [B, C, T] ────────────────────────────────────────────────────

class LayerNorm(nn.Module):
    def __init__(self, channels, eps=1e-5):
        super().__init__()
        self.eps = eps
        self.gamma = nn.Parameter(torch.ones(channels))
        self.beta  = nn.Parameter(torch.zeros(channels))
    def forward(self, x):
        x = x.transpose(1, -1)
        x = F.layer_norm(x, (x.shape[-1],), self.gamma, self.beta, self.eps)
        return x.transpose(1, -1)

# ── Multi-head attention with relative position encoding ──────────────────────

class MultiHeadAttention(nn.Module):
    def __init__(self, channels, out_channels, n_heads, p_dropout=0.,
                 window_size=4, heads_share=True, block_length=None,
                 proximal_bias=False, proximal_init=False):
        super().__init__()
        self.n_heads     = n_heads
        self.k_ch        = channels // n_heads
        self.window_size = window_size
        self.heads_share = heads_share
        self.block_length = block_length
        self.proximal_bias = proximal_bias
        self.drop   = nn.Dropout(p_dropout)
        self.conv_q = nn.Conv1d(channels, channels, 1)
        self.conv_k = nn.Conv1d(channels, channels, 1)
        self.conv_v = nn.Conv1d(channels, channels, 1)
        self.conv_o = nn.Conv1d(channels, out_channels, 1)
        for c in (self.conv_q, self.conv_k, self.conv_v):
            nn.init.xavier_uniform_(c.weight)
        if proximal_init:
            with torch.no_grad():
                self.conv_k.weight.copy_(self.conv_q.weight)
                self.conv_k.bias.copy_(self.conv_q.bias)
        if window_size is not None:
            n_h = 1 if heads_share else n_heads
            std = self.k_ch ** -0.5
            self.emb_rel_k = nn.Parameter(torch.randn(n_h, window_size*2+1, self.k_ch)*std)
            self.emb_rel_v = nn.Parameter(torch.randn(n_h, window_size*2+1, self.k_ch)*std)

    def _rel_emb(self, emb, L):
        pad   = max(L - (self.window_size + 1), 0)
        start = max(self.window_size + 1 - L, 0)
        end   = start + 2*L - 1
        e = F.pad(emb, [0, 0, pad, pad, 0, 0])[:, start:end]
        return e.expand(self.n_heads, -1, -1) if self.heads_share else e

    @staticmethod
    def _rel_to_abs(x):
        b, h, L, _ = x.shape
        x = F.pad(x, [0, L-1])
        x = x.view(b, h, L*(2*L-1))
        x = F.pad(x, [L, 0])
        return x.view(b, h, L, 2*L)[:, :, :, 1:]

    def forward(self, x, c, attn_mask=None):
        B, C, T = x.shape
        Ts = c.shape[2]
        q = self.conv_q(x).view(B, self.n_heads, self.k_ch, T).transpose(2, 3)
        k = self.conv_k(c).view(B, self.n_heads, self.k_ch, Ts).transpose(2, 3)
        v = self.conv_v(c).view(B, self.n_heads, self.k_ch, Ts).transpose(2, 3)
        scale  = self.k_ch ** -0.5
        scores = torch.matmul(q * scale, k.transpose(-2, -1))
        if self.window_size is not None and T == Ts:
            ek = self._rel_emb(self.emb_rel_k, T)
            scores = scores + self._rel_to_abs(torch.einsum('bhld,hmd->bhlm', q*scale, ek))
        if self.proximal_bias and T == Ts:
            r  = torch.arange(T, dtype=torch.float32, device=x.device)
            pb = -torch.log1p(torch.abs(r.unsqueeze(0) - r.unsqueeze(1)))
            scores = scores + pb.unsqueeze(0).unsqueeze(0)
        if self.block_length is not None and T == Ts:
            bm = torch.ones_like(scores).triu(-self.block_length).tril(self.block_length)
            scores = scores.masked_fill(bm == 0, -1e4)
        if attn_mask is not None:
            scores = scores.masked_fill(attn_mask == 0, -1e4)
        w   = self.drop(F.softmax(scores, dim=-1))
        out = torch.matmul(w, v).transpose(2, 3).contiguous().view(B, C, T)
        return self.conv_o(out)

# ── Feed-forward network ───────────────────────────────────────────────────────

class FFN(nn.Module):
    def __init__(self, in_ch, out_ch, filter_ch, kernel, p_dropout=0., causal=False):
        super().__init__()
        pad = (kernel - 1) // 2 if not causal else kernel - 1
        self.conv_1 = nn.Conv1d(in_ch,     filter_ch, kernel, padding=pad)
        self.conv_2 = nn.Conv1d(filter_ch, out_ch,    kernel, padding=pad)
        self.drop   = nn.Dropout(p_dropout)
        self.causal = causal
        self.pad    = pad
    def forward(self, x, mask):
        x = self.conv_1(x * mask)
        if self.causal and self.pad: x = x[:, :, :-self.pad]
        x = torch.relu(x)
        x = self.drop(x)
        x = self.conv_2(x * mask)
        if self.causal and self.pad: x = x[:, :, :-self.pad]
        return x * mask

# ── Transformer encoder ────────────────────────────────────────────────────────

class Encoder(nn.Module):
    def __init__(self, hidden, filter_ch, n_heads, n_layers, kernel, p_dropout=0.,
                 window_size=4, proximal_bias=False, proximal_init=False):
        super().__init__()
        self.drop = nn.Dropout(p_dropout)
        self.attn_layers   = nn.ModuleList([
            MultiHeadAttention(hidden, hidden, n_heads, p_dropout, window_size,
                               proximal_bias=proximal_bias, proximal_init=proximal_init)
            for _ in range(n_layers)])
        self.norm_layers_1 = nn.ModuleList([LayerNorm(hidden) for _ in range(n_layers)])
        self.ffn_layers    = nn.ModuleList([
            FFN(hidden, hidden, filter_ch, kernel, p_dropout) for _ in range(n_layers)])
        self.norm_layers_2 = nn.ModuleList([LayerNorm(hidden) for _ in range(n_layers)])
    def forward(self, x, mask):
        am = mask.unsqueeze(2) * mask.unsqueeze(-1)
        for attn, n1, ffn, n2 in zip(self.attn_layers, self.norm_layers_1,
                                      self.ffn_layers,  self.norm_layers_2):
            x = n1(x + self.drop(attn(x, x, am)))
            x = n2(x + self.drop(ffn(x, mask)))
        return x * mask

# ── Phone encoder (enc_p) ──────────────────────────────────────────────────────

class TextEncoder(nn.Module):
    def __init__(self, out_ch, hidden, filter_ch, n_heads, n_layers, kernel, p_dropout,
                 phone_dim=768):
        super().__init__()
        self.emb_phone = nn.Linear(phone_dim, hidden)
        self.lrelu     = nn.LeakyReLU(0.1, inplace=True)
        self.encoder   = Encoder(hidden, filter_ch, n_heads, n_layers, kernel, p_dropout)
        self.proj      = nn.Conv1d(hidden, out_ch * 2, 1)
    def forward(self, phone, lengths):
        x    = self.emb_phone(phone).transpose(1, 2)   # [B, hidden, T]
        x    = self.lrelu(x)
        mask = sequence_mask(lengths, x.shape[2]).unsqueeze(1).to(x.dtype)
        x    = self.encoder(x, mask)
        m, logs = self.proj(x).chunk(2, dim=1)
        return m * mask, logs * mask, mask

# ── WaveNet residual stack (used in flow) ─────────────────────────────────────

class WN(nn.Module):
    def __init__(self, hidden, kernel, dilation_rate, n_layers, gin_channels=0, p_dropout=0):
        super().__init__()
        self.n_layers     = n_layers
        self.gin_channels = gin_channels
        self.drop         = nn.Dropout(p_dropout)
        self.in_layers    = nn.ModuleList()
        self.res_skip_layers = nn.ModuleList()
        if gin_channels:
            self.cond_layer = nn.Conv1d(gin_channels, 2*hidden*n_layers, 1)
        for i in range(n_layers):
            d = dilation_rate ** i
            self.in_layers.append(
                nn.Conv1d(hidden, 2*hidden, kernel, dilation=d, padding=(kernel-1)*d//2))
            out_ch = 2*hidden if i < n_layers-1 else hidden
            self.res_skip_layers.append(nn.Conv1d(hidden, out_ch, 1))
    def forward(self, x, x_mask, g=None):
        result = torch.zeros_like(x)
        if g is not None and self.gin_channels:
            g = self.cond_layer(g)
        H = x.size(1)
        for i in range(self.n_layers):
            h = self.drop(self.in_layers[i](x * x_mask))
            if g is not None and self.gin_channels:
                h = h + g[:, i*2*H:(i+1)*2*H]
            acts = torch.tanh(h[:, :H]) * torch.sigmoid(h[:, H:])
            rs   = self.res_skip_layers[i](acts)
            if i < self.n_layers - 1:
                x      = (x + rs[:, :H]) * x_mask
                result = result + rs[:, H:]
            else:
                result = result + rs
        return result * x_mask

# ── Normalizing flow ──────────────────────────────────────────────────────────

class ResidualCouplingLayer(nn.Module):
    def __init__(self, ch, hidden, kernel, dilation, n_layers, gin_ch=0, mean_only=False):
        super().__init__()
        self.half      = ch // 2
        self.mean_only = mean_only
        self.pre  = nn.Conv1d(self.half, hidden, 1)
        self.enc  = WN(hidden, kernel, dilation, n_layers, gin_channels=gin_ch)
        out_ch    = self.half * (1 + (not mean_only))
        self.post = nn.Conv1d(hidden, out_ch, 1)
        nn.init.zeros_(self.post.weight); nn.init.zeros_(self.post.bias)
    def forward(self, x, x_mask, g=None, reverse=False):
        x0, x1 = x[:, :self.half], x[:, self.half:]
        stats  = self.post(self.enc(self.pre(x0) * x_mask, x_mask, g=g)) * x_mask
        m      = stats[:, :self.half]
        logs   = torch.zeros_like(m) if self.mean_only else stats[:, self.half:]
        x1     = (m + x1 * torch.exp(logs)) * x_mask if not reverse else \
                 ((x1 - m) * torch.exp(-logs)) * x_mask
        return torch.cat([x0, x1], 1)

class ResidualCouplingBlock(nn.Module):
    def __init__(self, ch, hidden, kernel, dilation, n_flows, gin_ch=0):
        super().__init__()
        self.flows = nn.ModuleList([
            ResidualCouplingLayer(ch, hidden, kernel, dilation, 4, gin_ch, mean_only=True)
            for _ in range(n_flows)])
    def forward(self, x, x_mask, g=None, reverse=False):
        flows = self.flows if not reverse else list(reversed(self.flows))
        for f in flows:
            x = f(x, x_mask, g=g, reverse=reverse)
            x = torch.flip(x, [1])
        return x

# ── NSF-HiFiGAN decoder ───────────────────────────────────────────────────────

class SineGen(nn.Module):
    def __init__(self, sr, harmonic_num=0, sine_amp=0.1, noise_std=0.003, voiced_threshold=0):
        super().__init__()
        self.sr        = sr
        self.n_harm    = harmonic_num + 1
        self.sine_amp  = sine_amp
        self.noise_std = noise_std
        self.vth       = voiced_threshold
    @torch.no_grad()
    def forward(self, f0, upp):
        # f0: [B, T, 1]  upp: total upsampling factor
        B, T, _ = f0.shape
        f0_up   = f0.repeat_interleave(upp, dim=1)          # [B, T*upp, 1]
        phase   = torch.cumsum(f0_up / self.sr, dim=1) % 1.0
        rad     = phase * 2 * math.pi
        harms   = torch.stack([torch.sin(rad * (i+1)) for i in range(self.n_harm)], dim=-1) * self.sine_amp
        uv      = (f0_up > self.vth).float()
        noise   = torch.randn_like(harms) * self.noise_std
        return harms * uv + noise * (1 - uv), uv, noise

class SourceModuleHnNSF(nn.Module):
    def __init__(self, sr, harmonic_num=0, sine_amp=0.1, add_noise_std=0.003, voiced_threshold=0):
        super().__init__()
        self.l_sin_gen = SineGen(sr, harmonic_num, sine_amp, add_noise_std, voiced_threshold)
        self.l_linear  = nn.Linear(harmonic_num + 1, 1)
        self.l_tanh    = nn.Tanh()
    def forward(self, f0, upp):
        sine_wavs, uv, _ = self.l_sin_gen(f0, upp)
        return self.l_tanh(self.l_linear(sine_wavs)), None, None

class ResBlock1(nn.Module):
    def __init__(self, ch, k=3, d=(1, 3, 5)):
        super().__init__()
        self.convs1 = nn.ModuleList([nn.Conv1d(ch, ch, k, dilation=di, padding=(k-1)*di//2) for di in d])
        self.convs2 = nn.ModuleList([nn.Conv1d(ch, ch, k, padding=(k-1)//2) for _ in d])
    def forward(self, x):
        for c1, c2 in zip(self.convs1, self.convs2):
            x = x + c2(F.leaky_relu(c1(F.leaky_relu(x, 0.1)), 0.1))
        return x

class ResBlock2(nn.Module):
    def __init__(self, ch, k=3, d=(1, 3)):
        super().__init__()
        self.convs = nn.ModuleList([nn.Conv1d(ch, ch, k, dilation=di, padding=(k-1)*di//2) for di in d])
    def forward(self, x):
        for c in self.convs:
            x = x + c(F.leaky_relu(x, 0.1))
        return x

class GeneratorNSF(nn.Module):
    def __init__(self, initial_channel, resblock, resblock_kernel_sizes,
                 resblock_dilation_sizes, upsample_rates, upsample_initial_channel,
                 upsample_kernel_sizes, gin_channels, sr, is_half=False):
        super().__init__()
        self.num_kernels   = len(resblock_kernel_sizes)
        self.num_upsamples = len(upsample_rates)
        self.upp           = math.prod(upsample_rates)
        self.f0_upsamp     = nn.Upsample(scale_factor=self.upp)
        self.m_source      = SourceModuleHnNSF(sr)
        self.conv_pre      = nn.Conv1d(initial_channel, upsample_initial_channel, 7, 1, 3)
        RB   = ResBlock1 if resblock == '1' else ResBlock2
        ch   = upsample_initial_channel
        self.ups        = nn.ModuleList()
        self.noise_convs = nn.ModuleList()
        self.resblocks  = nn.ModuleList()
        for i, (u, k) in enumerate(zip(upsample_rates, upsample_kernel_sizes)):
            self.ups.append(nn.ConvTranspose1d(ch, ch//2, k, u, (k-u)//2))
            ch //= 2
            if i + 1 < len(upsample_rates):
                stride = math.prod(upsample_rates[i+1:])
                self.noise_convs.append(nn.Conv1d(1, ch, stride*2, stride, stride//2))
            else:
                self.noise_convs.append(nn.Conv1d(1, ch, 1))
            for k2, d2 in zip(resblock_kernel_sizes, resblock_dilation_sizes):
                self.resblocks.append(RB(ch, k2, d2))
        self.conv_post = nn.Conv1d(ch, 1, 7, 1, 3, bias=False)
        if gin_channels:
            self.cond = nn.Conv1d(gin_channels, upsample_initial_channel, 1)
    def forward(self, x, f0, g=None):
        f0_up = self.f0_upsamp(f0.unsqueeze(1)).transpose(1, 2)   # [B, T*upp, 1]
        har, _, _ = self.m_source(f0_up, self.upp)
        har = har.transpose(1, 2)                                   # [B, 1, T*upp]
        x   = self.conv_pre(x)
        if g is not None and hasattr(self, 'cond'):
            x = x + self.cond(g)
        for i in range(self.num_upsamples):
            x  = self.ups[i](F.leaky_relu(x, 0.1))
            x  = x + self.noise_convs[i](har)
            xs = None
            for j in range(self.num_kernels):
                rb = self.resblocks[i * self.num_kernels + j]
                xs = rb(x) if xs is None else xs + rb(x)
            x = xs / self.num_kernels
        return torch.tanh(self.conv_post(F.leaky_relu(x)))

# ── Full RVC synthesizer ──────────────────────────────────────────────────────

class SynthesizerTrnMsNSFsid(nn.Module):
    def __init__(self, spec_channels, segment_size, inter_channels, hidden_channels,
                 filter_channels, n_heads, n_layers, kernel_size, p_dropout,
                 resblock, resblock_kernel_sizes, resblock_dilation_sizes,
                 upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
                 n_speakers, gin_channels, sr, phone_dim=768, **kw):
        super().__init__()
        self.gin_channels = gin_channels
        self.enc_p = TextEncoder(inter_channels, hidden_channels, filter_channels,
                                 n_heads, n_layers, kernel_size, p_dropout, phone_dim)
        self.flow  = ResidualCouplingBlock(inter_channels, hidden_channels, 5, 1, 4, gin_channels)
        self.dec   = GeneratorNSF(inter_channels, resblock, resblock_kernel_sizes,
                                  resblock_dilation_sizes, upsample_rates,
                                  upsample_initial_channel, upsample_kernel_sizes,
                                  gin_channels, int(sr))
        if n_speakers >= 1:
            self.emb_g = nn.Embedding(max(n_speakers, 1), gin_channels)

    @torch.no_grad()
    def infer(self, phone, phone_lengths, nsff0, sid=None):
        g = self.emb_g(sid).unsqueeze(-1) if hasattr(self, 'emb_g') and sid is not None else None
        m_p, logs_p, x_mask = self.enc_p(phone, phone_lengths)
        z_p = (m_p + torch.exp(logs_p) * torch.randn_like(m_p) * 0.66666) * x_mask
        z   = self.flow(z_p, x_mask, g=g, reverse=True)
        return self.dec((z * x_mask), nsff0, g=g)

# ── YIN pitch estimator ────────────────────────────────────────────────────────

def yin_f0(x, sr, hop=160, fmin=50.0, fmax=1100.0, threshold=0.10):
    W        = 1024
    x        = x.astype(np.float64)
    n_frames = max(1, (len(x) - W) // hop + 1)
    f0       = np.zeros(n_frames)
    min_tau  = max(1, int(sr / fmax))
    max_tau  = min(W // 2 - 1, int(sr / fmin))
    for i in range(n_frames):
        frame = x[i*hop : i*hop + W]
        if len(frame) < W:
            frame = np.pad(frame, (0, W - len(frame)))
        diff = np.zeros(W // 2)
        for tau in range(1, W // 2):
            d = frame[:W-tau] - frame[tau:]
            diff[tau] = float(np.dot(d, d))
        cmndf   = np.ones(W // 2)
        running = 0.0
        for tau in range(1, W // 2):
            running += diff[tau]
            cmndf[tau] = diff[tau] * tau / running if running > 1e-10 else 1.0
        for tau in range(max(min_tau, 2), max_tau):
            if cmndf[tau] < threshold and cmndf[tau] <= cmndf[tau - 1]:
                denom = cmndf[tau-1] - 2*cmndf[tau] + cmndf[tau+1] if tau+1 < W//2 else 0.0
                adj   = 0.5*(cmndf[tau-1]-cmndf[tau+1])/denom if abs(denom) > 1e-10 else 0.0
                f0[i] = sr / (tau + float(np.clip(adj, -0.5, 0.5)))
                break
    return f0

# ── HuBERT feature extractor (torchaudio, no fairseq) ────────────────────────

_hubert_model = None

def extract_hubert(wav16k_np):
    global _hubert_model
    if _hubert_model is None:
        bundle       = torchaudio.pipelines.HUBERT_BASE
        _hubert_model = bundle.get_model()
        _hubert_model.eval()
    wav = torch.from_numpy(wav16k_np).unsqueeze(0)
    with torch.no_grad():
        features, _ = _hubert_model.extract_features(wav)
    return features[-1]   # [1, T', 768]

# ── Audio I/O ──────────────────────────────────────────────────────────────────

def load_mono(path, sr):
    wav, orig_sr = torchaudio.load(path)
    if wav.shape[0] > 1:
        wav = wav.mean(0, keepdim=True)
    if orig_sr != sr:
        wav = torchaudio.functional.resample(wav, orig_sr, sr)
    return wav.squeeze(0).numpy()

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 4:
        print("Usage: vc_infer.py <in.wav> <model.pth> <out.wav> [semitones]", file=sys.stderr)
        sys.exit(1)

    in_wav, model_pth, out_wav = sys.argv[1], sys.argv[2], sys.argv[3]
    semitones = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    prog(0.0)

    # Load checkpoint
    cpt = torch.load(model_pth, map_location='cpu', weights_only=False)
    cfg = cpt['config']
    tgt_sr = int(cfg[-1])

    # Detect phone dim from saved weights (768 for v2, 256 for v1)
    phone_dim = 768
    w = cpt.get('weight', {})
    if 'enc_p.emb_phone.weight' in w:
        phone_dim = w['enc_p.emb_phone.weight'].shape[1]

    net_g = SynthesizerTrnMsNSFsid(*cfg, phone_dim=phone_dim)
    net_g.eval()
    net_g.load_state_dict(w, strict=False)

    prog(0.1)

    # Load audio at 16 kHz for HuBERT
    wav16 = load_mono(in_wav, 16000)

    # Extract HuBERT features and repeat-interleave x2 (RVC convention)
    prog(0.2)
    feats = extract_hubert(wav16)                        # [1, T', 768]
    feats = feats.repeat_interleave(2, dim=1)            # [1, 2T', 768]

    prog(0.5)

    # Extract F0 at target sample rate
    wav_tgt = load_mono(in_wav, tgt_sr)
    f0 = yin_f0(wav_tgt, tgt_sr)
    if semitones:
        voiced = f0 > 0
        f0[voiced] *= 2 ** (semitones / 12.0)

    # Interpolate F0 to match feature frame count
    T = feats.shape[1]
    f0 = np.interp(np.linspace(0, len(f0)-1, T), np.arange(len(f0)), f0)
    nsff0 = torch.from_numpy(f0.astype(np.float32)).unsqueeze(0)    # [1, T]

    prog(0.7)

    # Run inference
    lengths = torch.LongTensor([T])
    sid     = torch.LongTensor([0])
    audio   = net_g.infer(feats, lengths, nsff0, sid).squeeze().numpy()

    prog(0.9)

    # Save — resample to 44100 for the app
    audio_t = torch.from_numpy(audio).unsqueeze(0)
    if tgt_sr != 44100:
        audio_t = torchaudio.functional.resample(audio_t, tgt_sr, 44100)
    torchaudio.save(out_wav, audio_t, 44100)

    prog(1.0)

if __name__ == '__main__':
    main()
