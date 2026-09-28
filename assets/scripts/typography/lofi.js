// Lo-fi — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "lofi", "name": "Lo-fi", "category": "Retro", "tagline": "Small \u00b7 warm \u00b7 film grain \u00b7 phrases \u00b7 chill"};
const config = {"id": "lofi", "tagline": "Small \u00b7 warm \u00b7 film grain \u00b7 phrases \u00b7 chill", "category": "Retro", "font": "spacemono", "grouping": "Phrase", "custom_n": 3, "font_size": 0.06, "sub_wrap_w": 0.78, "pause_gap": 0.45, "sub_pos": 1, "max_words": 5, "text_case": 2, "style": "None", "color": [0.93, 0.87, 0.72, 1.0], "n_fx": 1, "fx": [{"type": "FilmGrain", "beat": 0.0}], "ts": {"shadow_enabled": false}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
