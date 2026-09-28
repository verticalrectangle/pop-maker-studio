// Apple — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "apple", "name": "Apple", "category": "Clean", "tagline": "Single word \u00b7 massive \u00b7 fades \u00b7 pristine white"};
const config = {"id": "apple", "tagline": "Single word \u00b7 massive \u00b7 fades \u00b7 pristine white", "category": "Clean", "font": null, "grouping": "Word", "custom_n": 1, "font_size": 0.16, "sub_wrap_w": 0.88, "pause_gap": 0.18, "sub_pos": 1, "max_words": 1, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
