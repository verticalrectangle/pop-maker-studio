// Newspaper — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "newspaper", "name": "Newspaper", "category": "Editorial", "tagline": "Tight centered serif block \u00b7 sentences"};
const config = {"id": "newspaper", "tagline": "Tight centered serif block \u00b7 sentences", "category": "Editorial", "font": "playfairdisplay", "grouping": "Segment", "custom_n": 3, "font_size": 0.07, "sub_wrap_w": 0.75, "pause_gap": 0.8, "sub_pos": 1, "max_words": 10, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
