// Typewriter Glow — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "typeglow", "name": "Typewriter Glow", "category": "Kinetic", "tagline": "Char reveal \u00b7 glow on the newest letter \u00b7 retro computer"};
const config = {"id": "typeglow", "tagline": "Char reveal \u00b7 glow on the newest letter \u00b7 retro computer", "category": "Kinetic", "font": "spacemono", "grouping": "CustomN", "custom_n": 4, "font_size": 0.075, "sub_wrap_w": 0.85, "pause_gap": 0.4, "anim_stagger": 0.04, "sub_pos": 1, "max_words": 6, "anim_unit": 2, "style": "Typewriter", "color": [1.0, 0.8, 0.4, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 6.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
