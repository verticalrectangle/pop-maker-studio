// Pop Highlight — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "karaoke_pop", "name": "Pop Highlight", "category": "Karaoke", "tagline": "Active word scales up + brightens \u00b7 modern karaoke"};
const config = {"id": "karaoke_pop", "tagline": "Active word scales up + brightens \u00b7 modern karaoke", "category": "Karaoke", "font": "montserrat", "grouping": "Line", "custom_n": 3, "font_size": 0.07, "sub_pos_y": 0.86, "sub_wrap_w": 0.9, "pause_gap": 0.5, "sub_pos": 0, "max_words": 8, "karaoke_mode": 2, "karaoke": true, "style": "None", "color": [0.6, 0.6, 0.62, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
