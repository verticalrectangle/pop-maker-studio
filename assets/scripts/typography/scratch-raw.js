// Scratch Raw — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "scratch-raw", "name": "Scratch Raw", "category": "Retro", "tagline": "Scratchy font \u00b7 per-frame boil \u00b7 raw analog energy"};
const config = {"id": "scratch-raw", "tagline": "Scratchy font \u00b7 per-frame boil \u00b7 raw analog energy", "category": "Retro", "font": "scratchl", "grouping": "Phrase", "custom_n": 3, "font_size": 0.14, "tracking": 0.0, "anim_stagger": 0.0, "sub_pos": 0, "anim_unit": 2, "all_caps": true, "style": "ScratchRaw", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
