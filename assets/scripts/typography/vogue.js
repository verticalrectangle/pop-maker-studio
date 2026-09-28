// Vogue — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "vogue", "name": "Vogue", "category": "Editorial", "tagline": "High-contrast serif \u00b7 wide tracking \u00b7 fashion"};
const config = {"id": "vogue", "tagline": "High-contrast serif \u00b7 wide tracking \u00b7 fashion", "category": "Editorial", "font": "playfairdisplay", "grouping": "Phrase", "custom_n": 3, "font_size": 0.1, "sub_wrap_w": 0.8, "pause_gap": 0.5, "tracking": 0.12, "sub_pos": 1, "max_words": 5, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
