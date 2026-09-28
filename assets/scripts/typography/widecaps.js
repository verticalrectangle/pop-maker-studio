// Wide Caps — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "widecaps", "name": "Wide Caps", "category": "Clean", "tagline": "Letter-spaced thin caps \u00b7 minimal brand"};
const config = {"id": "widecaps", "tagline": "Letter-spaced thin caps \u00b7 minimal brand", "category": "Clean", "font": "spacegrotesk", "grouping": "Phrase", "custom_n": 3, "font_size": 0.07, "sub_wrap_w": 0.85, "pause_gap": 0.45, "tracking": 0.22, "sub_pos": 1, "max_words": 4, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
