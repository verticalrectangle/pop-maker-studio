// Scratch — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "scratch", "name": "Scratch", "category": "Retro", "tagline": "Scratched-on-film \u00b7 per-frame carve \u00b7 raw analog energy"};
const config = {"id": "scratch", "tagline": "Scratched-on-film \u00b7 per-frame carve \u00b7 raw analog energy", "category": "Retro", "font": "archivoblack", "grouping": "Phrase", "custom_n": 3, "font_size": 0.12, "tracking": 0.02, "anim_stagger": 0.0, "sub_pos": 0, "anim_unit": 2, "all_caps": true, "style": "ScratchFilm", "n_fx": 0, "fx": [], "ts": {}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
