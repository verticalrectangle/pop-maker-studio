// Love Note — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "lovenote", "name": "Love Note", "category": "Script", "tagline": "Thin signature script \u00b7 drifts up \u00b7 blush"};
const config = {"id": "lovenote", "tagline": "Thin signature script \u00b7 drifts up \u00b7 blush", "category": "Script", "font": "sacramento", "grouping": "Phrase", "custom_n": 3, "font_size": 0.12, "sub_wrap_w": 0.75, "pause_gap": 0.5, "sub_pos": 1, "max_words": 5, "style": "Stack", "color": [1.0, 0.85, 0.9, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
