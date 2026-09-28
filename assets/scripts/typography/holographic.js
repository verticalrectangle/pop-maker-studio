// Holographic — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "holographic", "name": "Holographic", "category": "Gradient", "tagline": "Animated iridescent hue shift \u00b7 Y2K foil"};
const config = {"id": "holographic", "tagline": "Animated iridescent hue shift \u00b7 Y2K foil", "category": "Gradient", "font": "spacegrotesk", "grouping": "Phrase", "custom_n": 3, "font_size": 0.11, "sub_wrap_w": 0.85, "pause_gap": 0.4, "tracking": 0.04, "sub_pos": 1, "max_words": 4, "grad_mode": 3, "all_caps": true, "style": "None", "n_fx": 0, "fx": [], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
