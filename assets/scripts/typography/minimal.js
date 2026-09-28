// Minimal — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "minimal", "name": "Minimal", "category": "Clean", "tagline": "Small \u00b7 centered \u00b7 breathing room \u00b7 muted"};
const config = {"id": "minimal", "tagline": "Small \u00b7 centered \u00b7 breathing room \u00b7 muted", "category": "Clean", "font": null, "grouping": "Phrase", "custom_n": 3, "font_size": 0.055, "sub_wrap_w": 0.75, "pause_gap": 0.4, "sub_pos": 1, "max_words": 5, "style": "None", "color": [0.9, 0.9, 0.9, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
