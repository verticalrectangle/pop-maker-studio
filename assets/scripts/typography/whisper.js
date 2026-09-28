// Whisper — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "whisper", "name": "Whisper", "category": "Aesthetic", "tagline": "Faint thin script \u00b7 low opacity \u00b7 intimate"};
const config = {"id": "whisper", "tagline": "Faint thin script \u00b7 low opacity \u00b7 intimate", "category": "Aesthetic", "font": "sacramento", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.7, "pause_gap": 0.5, "sub_pos": 1, "max_words": 5, "style": "None", "color": [1.0, 1.0, 1.0, 0.8], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
