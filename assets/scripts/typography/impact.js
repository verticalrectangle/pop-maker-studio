// Impact — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "impact", "name": "Impact", "category": "Hype", "tagline": "Heavy bold \u00b7 slams to full size \u00b7 pop"};
const config = {"id": "impact", "tagline": "Heavy bold \u00b7 slams to full size \u00b7 pop", "category": "Hype", "font": "archivoblack", "grouping": "Word", "custom_n": 1, "font_size": 0.17, "pause_gap": 0.1, "sub_pos": 1, "max_words": 1, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": 4.0, "shadow_oy": 4.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
