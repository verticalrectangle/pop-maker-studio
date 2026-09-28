// Pixel — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "pixel", "name": "Pixel", "category": "Mono", "tagline": "Chunky 8-bit caps \u00b7 hard cut \u00b7 retro game"};
const config = {"id": "pixel", "tagline": "Chunky 8-bit caps \u00b7 hard cut \u00b7 retro game", "category": "Mono", "font": "pressstart2p", "grouping": "CustomN", "custom_n": 2, "font_size": 0.06, "sub_wrap_w": 0.85, "pause_gap": 0.2, "sub_pos": 1, "max_words": 3, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
