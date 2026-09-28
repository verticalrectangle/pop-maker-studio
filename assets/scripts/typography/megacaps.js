// Mega Caps — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "megacaps", "name": "Mega Caps", "category": "Hype", "tagline": "Enormous condensed caps \u00b7 fills the frame \u00b7 festival"};
const config = {"id": "megacaps", "tagline": "Enormous condensed caps \u00b7 fills the frame \u00b7 festival", "category": "Hype", "font": "bebasneue", "grouping": "CustomN", "custom_n": 2, "font_size": 0.22, "sub_wrap_w": 0.95, "pause_gap": 0.15, "tracking": 0.04, "sub_pos": 1, "max_words": 2, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
