// Vaporwave — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "vaporwave", "name": "Vaporwave", "category": "Retro", "tagline": "Wide mono \u00b7 pink/cyan \u00b7 drift \u00b7 aesthetic"};
const config = {"id": "vaporwave", "tagline": "Wide mono \u00b7 pink/cyan \u00b7 drift \u00b7 aesthetic", "category": "Retro", "font": "spacemono", "grouping": "Phrase", "custom_n": 3, "font_size": 0.09, "sub_wrap_w": 0.85, "pause_gap": 0.5, "tracking": 0.12, "sub_pos": 1, "max_words": 4, "all_caps": true, "style": "None", "color": [1.0, 0.6, 0.9, 1.0], "n_fx": 0, "fx": [], "ts": {"shadow_enabled": true, "shadow_ox": 3.0, "shadow_oy": 0.0}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
