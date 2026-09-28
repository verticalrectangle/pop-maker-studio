// Zoom Punch — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "zoompunch", "name": "Zoom Punch", "category": "Kinetic", "tagline": "Words punch in oversized then settle \u00b7 hype intro"};
const config = {"id": "zoompunch", "tagline": "Words punch in oversized then settle \u00b7 hype intro", "category": "Kinetic", "font": "archivoblack", "grouping": "CustomN", "custom_n": 2, "font_size": 0.14, "sub_wrap_w": 0.9, "pause_gap": 0.25, "anim_stagger": 0.04, "sub_pos": 1, "max_words": 2, "anim_unit": 1, "all_caps": true, "style": "Scale", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": 3.0, "shadow_oy": 3.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
