// Cascade — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "cascade", "name": "Cascade", "category": "Kinetic", "tagline": "Words drop in top-down \u00b7 staggered \u00b7 Spotify Canvas"};
const config = {"id": "cascade", "tagline": "Words drop in top-down \u00b7 staggered \u00b7 Spotify Canvas", "category": "Kinetic", "font": "archivoblack", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.85, "pause_gap": 0.35, "anim_stagger": 0.06, "sub_pos": 1, "max_words": 5, "anim_unit": 1, "style": "Stack", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
