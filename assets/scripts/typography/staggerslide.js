// Stagger Slide — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "staggerslide", "name": "Stagger Slide", "category": "Kinetic", "tagline": "Each word slides in from the left \u00b7 tech promo"};
const config = {"id": "staggerslide", "tagline": "Each word slides in from the left \u00b7 tech promo", "category": "Kinetic", "font": "spacegrotesk", "grouping": "Phrase", "custom_n": 3, "font_size": 0.085, "sub_wrap_w": 0.85, "pause_gap": 0.35, "anim_stagger": 0.06, "sub_pos": 1, "max_words": 5, "anim_unit": 1, "style": "Slide", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
