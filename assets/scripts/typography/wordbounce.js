// Word Bounce — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "wordbounce", "name": "Word Bounce", "category": "Kinetic", "tagline": "Each word bounces in on a spring \u00b7 upbeat pop"};
const config = {"id": "wordbounce", "tagline": "Each word bounces in on a spring \u00b7 upbeat pop", "category": "Kinetic", "font": "poppins", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.85, "pause_gap": 0.35, "anim_stagger": 0.07, "sub_pos": 1, "max_words": 5, "anim_unit": 1, "style": "Bounce", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
