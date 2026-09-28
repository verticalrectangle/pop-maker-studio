// 70s Groovy — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "groovy", "name": "70s Groovy", "category": "Retro", "tagline": "Warm retro script \u00b7 gentle \u00b7 funk/soul"};
const config = {"id": "groovy", "tagline": "Warm retro script \u00b7 gentle \u00b7 funk/soul", "category": "Retro", "font": "lobster", "grouping": "Phrase", "custom_n": 3, "font_size": 0.12, "sub_wrap_w": 0.8, "pause_gap": 0.45, "sub_pos": 1, "max_words": 4, "style": "None", "color": [1.0, 0.7, 0.3, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": 2.0, "shadow_oy": 2.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
