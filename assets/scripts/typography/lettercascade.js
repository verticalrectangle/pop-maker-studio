// Letter Cascade — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "lettercascade", "name": "Letter Cascade", "category": "Kinetic", "tagline": "Letters fall in one by one \u00b7 kinetic type reel"};
const config = {"id": "lettercascade", "tagline": "Letters fall in one by one \u00b7 kinetic type reel", "category": "Kinetic", "font": "montserrat", "grouping": "CustomN", "custom_n": 2, "font_size": 0.11, "sub_wrap_w": 0.9, "pause_gap": 0.3, "anim_stagger": 0.03, "sub_pos": 1, "max_words": 2, "anim_unit": 2, "all_caps": true, "style": "Stack", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
