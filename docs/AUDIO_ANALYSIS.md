# Audio analysis v2

One analysis per audio file, cached as `<audio stem>_analysis.json` next to the audio (or in the
project cache dir when that is not writable). C++ type: `AudioAnalysis` (`src/audio_analysis.h`);
live copy: `AppState::audio_analysis`. Consumers: script clips (`pms.audio`), codegen FX beat
modulation, MCP `get_audio_analysis`, the style expander.

All times are **source-file seconds**. Script clips see timeline seconds (runtime maps through the
audio clip playing the file: `timeline = source − in_point + clip.start`).

## JSON schema

```jsonc
{
  "version": 2,
  "source": "/abs/path/song.flac",
  "duration": 204.52,
  "bpm": 98.36,
  "beats": [0.16, 0.78],                // ascending
  "downbeats": [1.39, 3.83],            // subset of beats
  "hits": {                             // per kind: onsets with strength 0..1
    "kick":  [{"t": 0.17, "s": 0.84}],  // drums stem, 30–150 Hz band flux
    "snare": [],                        // drums stem, 180 Hz–4 kHz band flux
    "hat":   [],                        // drums stem, 7–16 kHz band flux
    "bass":  [],                        // bass stem onset strength
    "other": [],                        // other stem onset strength (synths, guitars, keys)
    "vocal": []                         // vocals stem onset strength
  },
  "fps": 60,                            // envelope + spectrum frame rate
  "env": { "mix": [], "drums": [], "bass": [], "other": [], "vocals": [] },  // RMS per frame, 0..1
  "spectrum_bands": 32,
  "spectrum": [[0, 12, 99]],            // per frame: log-mel bands, 0..99
  "lines": ["He imagined that this was an ability he shared with most other people"],
  "words": [{"w": "He", "line": 0, "i": 0, "t0": 88.44, "t1": 88.48, "conf": 0.70}],
  "stems": {"drums": "…/drums.wav", "bass": "…", "other": "…", "vocals": "…"}
}
```

The reference implementation (Python/librosa, the Talking Heads project in
`~/Projects/seen-and-not-seen/pipeline/audio.py`, output `public/timeline.json`) produces this
schema minus `version/source/spectrum_bands/stems`, with times relative to its clip; the C++
loader accepts it.

## Algorithm requirements

| Field | Requirement |
|---|---|
| stems | 4-stem separation (htdemucs, ONNX, STFT/iSTFT in C++). Without the model: `drums/bass/other` empty and hits fall back to band flux on the instrumental (`original − vocals`). |
| beats, bpm | Beat tracking on the **mix** (never the vocal stem). |
| downbeats | Bar phase ∈ {0..3} maximising Σ over beats of (bass-stem onset strength at the beat / max) + (1 − cosine similarity of beat-synchronous chroma of bass+other between consecutive beats). Downbeats = beats at that phase. |
| hits.kick/snare/hat | Half-wave-rectified spectral flux (dB) of the drums stem restricted to the band; peak picking with local max ±3 frames, local mean ±12 frames, delta = 0.5 × quantile(norm, q) with q = 0.90/0.90/0.80, minimum spacing 120/120/70 ms (10 ms hop). |
| hits.bass/other/vocal | Onset strength of the stem, same peak picking, q = 0.85, spacing 100/80/80 ms. |
| strength `s` | `peak / p90(all picked peak heights of that kind)`, clipped to 1. Never divide by the global max. |
| env | RMS per video frame (frame length 2048), divided by its 98th percentile, clipped to 1. |
| spectrum | Mel power spectrum, 32 bands 30 Hz–16 kHz, n_fft 4096, hop = sr/fps, dB relative to max, mapped (dB+70)/70 → 0..99. |
| words | Forced alignment (wav2vec2 CTC) of the given lyric lines inside per-line windows. Words with mean confidence < 0.5 get `t0 = max(t0, t1 − (0.07·chars + 0.05))`: a low-confidence path parks on the word across breaths/backing vocals and drags its start early; its end stays reliable. |

## Parity acceptance (reference song: "Seen and Not Seen", 1:28–2:03)

Against the reference `timeline.json`: beats ±20 ms; same downbeat phase; per kind ≥ 85 % of
reference hits with `s ≥ 0.5` matched within 30 ms; ≥ 90 % of word starts within 80 ms.
