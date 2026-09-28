// Fill Wipe — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "karaoke_fill", "name": "Fill Wipe", "category": "Karaoke", "tagline": "Highlight sweeps across each word as it's sung \u00b7 classic karaoke"};
const config = {"id": "karaoke_fill", "tagline": "Highlight sweeps across each word as it's sung \u00b7 classic karaoke", "category": "Karaoke", "font": "anton", "grouping": "Line", "custom_n": 3, "font_size": 0.075, "sub_pos_y": 0.86, "sub_wrap_w": 0.9, "pause_gap": 0.5, "sub_pos": 0, "max_words": 8, "karaoke_mode": 1, "all_caps": true, "karaoke": true, "style": "None", "color": [0.55, 0.55, 0.58, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
