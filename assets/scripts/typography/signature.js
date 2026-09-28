// Signature — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "signature", "name": "Signature", "category": "Script", "tagline": "Flowing calligraphy \u00b7 fades in like a pen finishing"};
const config = {"id": "signature", "tagline": "Flowing calligraphy \u00b7 fades in like a pen finishing", "category": "Script", "font": "greatvibes", "grouping": "Phrase", "custom_n": 3, "font_size": 0.14, "sub_wrap_w": 0.8, "pause_gap": 0.5, "sub_pos": 1, "max_words": 5, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
