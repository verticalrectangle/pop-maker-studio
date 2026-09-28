// Terminal — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "terminal", "name": "Terminal", "category": "Mono", "tagline": "Green CRT mono \u00b7 scanlines \u00b7 hacker"};
const config = {"id": "terminal", "tagline": "Green CRT mono \u00b7 scanlines \u00b7 hacker", "category": "Mono", "font": "vt323", "grouping": "CustomN", "custom_n": 4, "font_size": 0.07, "sub_pos_x": 0.08, "sub_pos_y": 0.85, "sub_wrap_w": 0.85, "pause_gap": 0.3, "sub_pos": 0, "sub_anchor_h": 0, "max_words": 6, "style": "Typewriter", "color": [0.3, 1.0, 0.4, 1.0], "n_fx": 1, "fx": [{"type": "Scanlines", "beat": 0.0}], "ts": {"shadow_enabled": false, "glow_enabled": true, "glow_r": 6.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
