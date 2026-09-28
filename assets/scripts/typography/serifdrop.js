// Serif Drop — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "serifdrop", "name": "Serif Drop", "category": "Editorial", "tagline": "Clean display serif \u00b7 rises from below \u00b7 A24"};
const config = {"id": "serifdrop", "tagline": "Clean display serif \u00b7 rises from below \u00b7 A24", "category": "Editorial", "font": "dmserifdisplay", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.8, "pause_gap": 0.5, "sub_pos": 1, "max_words": 5, "style": "Stack", "color": [0.98, 0.96, 0.92, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
