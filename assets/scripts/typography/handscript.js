// Handscript — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "handscript", "name": "Handscript", "category": "Script", "tagline": "Bouncy casual script \u00b7 personal vlog energy"};
const config = {"id": "handscript", "tagline": "Bouncy casual script \u00b7 personal vlog energy", "category": "Script", "font": "dancingscript", "grouping": "Phrase", "custom_n": 3, "font_size": 0.11, "sub_wrap_w": 0.8, "pause_gap": 0.45, "sub_pos": 1, "max_words": 5, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
