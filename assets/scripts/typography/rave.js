// Rave — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "rave", "name": "Rave", "category": "Hype", "tagline": "Random positions \u00b7 beat-reactive \u00b7 neon chaos"};
const config = {"id": "rave", "tagline": "Random positions \u00b7 beat-reactive \u00b7 neon chaos", "category": "Hype", "font": "bungee", "grouping": "Word", "custom_n": 1, "font_size": 0.14, "sub_pos_y": 0.5, "pause_gap": 0.12, "sub_pos": 3, "max_words": 1, "all_caps": true, "style": "Scale", "color": [1.0, 0.1, 0.9, 1.0], "n_fx": 1, "fx": [{"type": "ChromaticAberration", "beat": 0.8}], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
