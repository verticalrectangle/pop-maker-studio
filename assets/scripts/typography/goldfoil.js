// Gold Foil — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "goldfoil", "name": "Gold Foil", "category": "Gradient", "tagline": "Metallic gold gradient + soft glow \u00b7 luxury / awards"};
const config = {"id": "goldfoil", "tagline": "Metallic gold gradient + soft glow \u00b7 luxury / awards", "category": "Gradient", "font": "cinzel", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.85, "pause_gap": 0.5, "tracking": 0.05, "sub_pos": 1, "max_words": 4, "grad_mode": 1, "all_caps": true, "style": "None", "color": [1.0, 0.92, 0.55, 1.0], "grad_col2": [0.7, 0.5, 0.15, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 8.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
