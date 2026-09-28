// Disco — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "disco", "name": "Disco", "category": "Retro", "tagline": "Rounded deco caps \u00b7 bounce \u00b7 party"};
const config = {"id": "disco", "tagline": "Rounded deco caps \u00b7 bounce \u00b7 party", "category": "Retro", "font": "righteous", "grouping": "Word", "custom_n": 1, "font_size": 0.15, "pause_gap": 0.12, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "Bounce", "color": [1.0, 0.85, 0.3, 1.0], "n_fx": 0, "fx": [], "ts": {"glow_enabled": true, "glow_r": 8.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
