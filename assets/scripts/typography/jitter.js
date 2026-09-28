// Jitter — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "jitter", "name": "Jitter", "category": "Kinetic", "tagline": "Every letter micro-vibrates \u00b7 anxious energy"};
const config = {"id": "jitter", "tagline": "Every letter micro-vibrates \u00b7 anxious energy", "category": "Kinetic", "font": "majormono", "grouping": "Phrase", "custom_n": 3, "font_size": 0.09, "sub_wrap_w": 0.85, "pause_gap": 0.35, "anim_stagger": 0.0, "sub_pos": 1, "max_words": 4, "anim_unit": 2, "style": "Jitter", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
