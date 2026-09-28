// Sad Girl — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "sadgirl", "name": "Sad Girl", "category": "Aesthetic", "tagline": "Large single word \u00b7 pastel \u00b7 centered \u00b7 slow"};
const config = {"id": "sadgirl", "tagline": "Large single word \u00b7 pastel \u00b7 centered \u00b7 slow", "category": "Aesthetic", "font": "cormorant", "grouping": "Word", "custom_n": 1, "font_size": 0.16, "pause_gap": 0.2, "sub_pos": 1, "max_words": 1, "text_case": 2, "style": "None", "color": [0.95, 0.75, 0.88, 1.0], "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
