// Pencil — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "pencil", "name": "Pencil", "category": "Script", "tagline": "Faint cursive \u00b7 diary \u00b7 indie"};
const config = {"id": "pencil", "tagline": "Faint cursive \u00b7 diary \u00b7 indie", "category": "Script", "font": "homemadeapple", "grouping": "Phrase", "custom_n": 3, "font_size": 0.085, "sub_wrap_w": 0.75, "pause_gap": 0.5, "sub_pos": 1, "max_words": 4, "style": "None", "color": [0.9, 0.9, 0.92, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
