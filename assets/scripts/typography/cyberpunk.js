// Cyberpunk — generated from src/typography_presets.h by tools/gen_typography_presets.py. Do not edit.
import { renderPreset } from './lib/typography.js';
export const meta = {"id": "cyberpunk", "name": "Cyberpunk", "category": "Mono", "tagline": "Monospace \u00b7 cyan \u00b7 glitch \u00b7 bottom-left"};
const config = {"id": "cyberpunk", "tagline": "Monospace \u00b7 cyan \u00b7 glitch \u00b7 bottom-left", "category": "Mono", "font": "spacemono", "grouping": "CustomN", "custom_n": 3, "font_size": 0.12, "sub_pos_x": 0.08, "sub_pos_y": 0.88, "sub_wrap_w": 0.9, "pause_gap": 0.2, "sub_pos": 0, "sub_anchor_h": 0, "max_words": 3, "all_caps": true, "style": "Glitch", "color": [0.0, 1.0, 0.95, 1.0], "n_fx": 2, "fx": [{"type": "ChromaticAberration", "beat": 0.7}, {"type": "Scanlines", "beat": 0.0}], "ts": {"stroke_enabled": true, "stroke_w": 1.5}};
export function setup(env) { renderPreset.setup(env, config); }
export function render(f) { renderPreset(f, config); }
