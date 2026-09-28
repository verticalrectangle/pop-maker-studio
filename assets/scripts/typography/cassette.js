// Cassette — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "cassette", "name": "Cassette", "category": "Retro", "tagline": "Mono caps \u00b7 bracketed \u00b7 mixtape J-card"};
const config = {"id": "cassette", "tagline": "Mono caps \u00b7 bracketed \u00b7 mixtape J-card", "category": "Retro", "font": "spacemono", "grouping": "Line", "custom_n": 3, "font_size": 0.06, "sub_pos_y": 0.88, "pause_gap": 0.6, "tracking": 0.05, "sub_pos": 0, "sub_anchor_h": 1, "max_words": 8, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
