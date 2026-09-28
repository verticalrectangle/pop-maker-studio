// Neon — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "neon", "name": "Neon", "category": "Retro", "tagline": "Glowing tube letters \u00b7 hot pink \u00b7 centered"};
const config = {"id": "neon", "tagline": "Glowing tube letters \u00b7 hot pink \u00b7 centered", "category": "Retro", "font": "monoton", "grouping": "Word", "custom_n": 1, "font_size": 0.13, "pause_gap": 0.15, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "Scale", "color": [1.0, 0.2, 0.75, 1.0], "n_fx": 1, "fx": [{"type": "ChromaticAberration", "beat": 0.6}], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 14.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
