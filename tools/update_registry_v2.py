#!/usr/bin/env python3
"""
Restructure registry for v2 slider system:
  - Remove all 'strength' params that were added in the last commit
  - Add system-level 'amount' field to each effect (min=0, max=1, default=1)
  - Fix param ranges (raise mins to perceptual floor)
  - Add 'curve' field where nonlinear mapping helps
  - Fix inverted mix_orig params (gradient_map, pixel_mosaic, pencil_sketch, mirror_fold)
  - Bump project_version to 22
"""
import json, math

with open('effects/registry.json') as f:
    reg = json.load(f)

effects_list = reg['effects']

# ── Step 1: Remove 'strength' params that were added as the first param ──────
# We added 'strength' as first param to 33 effects, and renamed mix_orig/blend
# to 'strength' in 4 effects. Clean all of that up.
for e in effects_list:
    # Remove any param named 'strength' that was NOT originally in the effect
    # (We know the 4 inverted effects had mix_orig/blend renamed to strength)
    eid = e['id']
    params = e['params']

    # Remove strength if it's there (was added by us, not original)
    e['params'] = [p for p in params if p['name'] != 'strength']

# ── Step 2: Fix inverted params (restore correct naming and semantics) ────────
# gradient_map, pixel_mosaic, pencil_sketch should have mix_orig back
# mirror_fold should have blend back
# These params were renamed to 'strength' in the previous commit.
# After step 1 they've been removed -- we need to add them back correctly.

effects = {e['id']: e for e in effects_list}

# gradient_map: add strength param (replaces mix_orig, correct direction)
# shader does: mix(col.rgb, mapped, u_strength) after our revert → now needs re-check
# Actually after git revert, gradient_map.glsl has u_mix_orig back.
# Let's keep the original param name for now — we'll handle direction in shader.
# For now: restore mix_orig with default=0 for backward compat, or rename to strength
# with correct semantics. Decision: rename to 'strength', default=1, correct direction.
# The shader will be patched below.

def add_param_first(eid, param):
    effects[eid]['params'].insert(0, param)

def ensure_param(eid, param):
    """Add param if not already present."""
    if not any(p['name'] == param['name'] for p in effects[eid]['params']):
        effects[eid]['params'].insert(0, param)

# ── Step 3: Add system-level 'amount' to every effect ────────────────────────
# This is NOT in the params[] list — it's a top-level field on each effect.
for e in effects_list:
    e['amount_default'] = e.get('amount_default', 1.0)

# ── Step 4: Fix param ranges and add curves ───────────────────────────────────
# curve: power applied to normalized [0,1] slider before mapping to [min,max]
#   curve=1.0: linear (default)
#   curve=0.5: sqrt — slider moves quickly into visible range (good for intensity)
#   curve=2.0: quadratic — more resolution at low end (good for precise small values)
# min/max: the PHYSICAL visible range (min should be perceptual floor, not 0)

def set_param(eid, name, **kwargs):
    """Update fields on a named param."""
    for p in effects[eid]['params']:
        if p['name'] == name:
            p.update(kwargs)
            return
    print(f"  WARN: {eid}.{name} not found")

def clamp01(v): return max(0.0, min(1.0, v))

# ── Intensity/blur type params that are invisible at 0 → raise min ─────────
# frosted_glass
set_param('frosted_glass', 'blur',  min=0.003, max=0.05,  default=0.018, curve=0.5)
set_param('frosted_glass', 'noise', min=0.0,   max=1.0,   default=0.4,   curve=0.5)

# watercolor
set_param('watercolor', 'bleeding', min=0.003, max=0.05, default=0.018, curve=0.5)
set_param('watercolor', 'saturation', min=0.8, max=2.5,  default=1.4,   curve=0.5)

# oil_paint
set_param('oil_paint', 'radius',    min=2.0,   max=8.0,  default=4.0,   curve=0.5)
set_param('oil_paint', 'sharpness', min=0.0,   max=15.0, default=6.0,   curve=0.5)

# tilt_shift
set_param('tilt_shift', 'blur_radius', min=2.0, max=30.0, default=12.0, curve=0.5)

# long_exposure — glow invisible at very low values
set_param('long_exposure', 'glow',  min=0.1, max=2.0, default=1.0, curve=0.5)
set_param('long_exposure', 'trail', min=0.01, max=0.2, default=0.06, curve=0.5)

# starburst_spike — length at 0 produces no stars
set_param('starburst_spike', 'length', min=0.05, max=0.6, default=0.25, curve=0.5)

# echo_trails — offset at very low values is invisible
set_param('echo_trails', 'offset', min=0.005, max=0.1, default=0.025, curve=0.5)

# neon_edge_glow — glow at low values hard to see
set_param('neon_edge_glow', 'glow', min=0.2, max=2.0, default=0.8, curve=0.5)

# glitter_dust — sparkle at low values
set_param('glitter_dust', 'sparkle', min=0.2, max=2.0, default=1.0, curve=0.5)
set_param('glitter_dust', 'size',    min=0.2, max=2.0, default=0.8, curve=0.5)

# ice_crystal — refract at 0 is passthrough
set_param('ice_crystal', 'refract', min=0.01, max=0.15, default=0.04, curve=0.5)

# raindrop_refract — refract_str at 0 is invisible
set_param('raindrop_refract', 'refract_str', min=0.1, max=2.0, default=1.2, curve=0.5)

# rgb_split_wave — amplitude at very low values invisible
set_param('rgb_split_wave', 'amplitude', min=0.003, max=0.05, default=0.018, curve=0.5)

# film halation style — glow effects
if 'film_halation' in effects:
    pass  # handled separately if it's a generated effect

# spin_blur — angle at low values barely visible
set_param('spin_blur', 'angle', min=0.01, max=0.3, default=0.08, curve=0.5)

# golden_hour — glow and warmth at low values subtle
set_param('golden_hour', 'glow_str', min=0.1, max=1.0, default=0.7, curve=0.5)
set_param('golden_hour', 'warmth',   min=0.1, max=1.0, default=0.8, curve=0.5)

# desert_gold — warmth at 0 = barely noticeable
set_param('desert_gold', 'warmth', min=0.1, max=1.0, default=0.7, curve=0.5)

# ripple — amplitude at very low values invisible
set_param('ripple', 'amplitude', min=0.005, max=0.1, default=0.035, curve=0.5)

# wave_warp — amplitude at low values
set_param('wave_warp', 'amplitude', min=0.008, max=0.12, default=0.08, curve=0.5)

# dna_helix — wave_amp and line_width
set_param('dna_helix', 'wave_amp',   min=0.15, max=1.0,  default=0.5,  curve=0.5)
set_param('dna_helix', 'line_width', min=0.02, max=0.15, default=0.05, curve=0.5)

# barrel_warp — k1 near 0 = tiny warp
set_param('barrel_warp', 'k1', min=0.05, max=1.0, default=0.4, curve=0.5)
set_param('barrel_warp', 'k2', min=0.0,  max=0.5, default=0.1, curve=0.5)

# bit_crush — at high levels barely any crushing
set_param('bit_crush', 'levels', min=2.0, max=16.0, default=5.0, curve=0.5)

# mirror_tunnel — depth at 1 = minimal effect
set_param('mirror_tunnel', 'depth', min=2, max=12, default=5, curve=0.5)

# crosshatch — thickness at low values barely visible lines
set_param('crosshatch', 'thickness', min=0.15, max=0.8, default=0.5, curve=0.5)

# ── Curves for nonlinear params (no min change needed, just better slider feel) ─
set_param('infrared_film', 'glow', min=0.05, max=1.0, default=0.4, curve=0.5)
set_param('kodachrome', 'saturation', min=0.8, max=3.0, default=1.5, curve=0.5)
set_param('lomo', 'vignette', min=0.1, max=1.0, default=0.75, curve=0.5)
set_param('lomo', 'saturation', min=0.8, max=2.0, default=1.6, curve=0.5)
set_param('neon_sign', 'glow_radius', min=2.0, max=20.0, default=8.0, curve=0.5)
set_param('neon_sign', 'edge_str', min=1.0, max=10.0, default=4.0, curve=0.5)
set_param('thermal_map', 'contrast', min=0.8, max=3.0, default=1.4, curve=0.5)
set_param('super8_film', 'grain', min=0.05, max=1.5, default=0.5, curve=0.5)
set_param('zone_system_bw', 'contrast', min=0.8, max=3.0, default=1.6, curve=0.5)
set_param('crt', 'curvature', min=0.05, max=1.0, default=0.35, curve=0.5)
set_param('golden_hour', 'vignette', min=0.1, max=2.0, default=0.5, curve=0.5)
set_param('daguerreotype', 'vignette', min=0.1, max=2.0, default=0.8, curve=0.5)
set_param('daguerreotype', 'scratch', min=0.02, max=1.0, default=0.3, curve=0.5)

# liquid_chrome
set_param('liquid_chrome', 'flow', min=0.1, max=1.0, default=0.5, curve=0.5)
set_param('liquid_chrome', 'metallic', min=0.1, max=1.0, default=0.7, curve=0.5)

# ── Trim default curves for any param with curve not set ─────────────────────
for e in effects_list:
    for p in e['params']:
        if 'curve' not in p:
            p['curve'] = 1.0
        # Ensure fmt field
        if 'fmt' not in p:
            p['fmt'] = '%.2f'

# ── Fix inverted params: restore correct shader semantics ─────────────────────
# gradient_map: u_mix_orig — shader does mix(mapped, col.rgb, u_mix_orig)
# mix_orig=0 → gradient effect, mix_orig=1 → original. That's inverted.
# Rename to strength and flip: mix(col.rgb, mapped, u_strength), default=1
for e in effects_list:
    if e['id'] == 'gradient_map':
        for p in e['params']:
            if p['name'] == 'mix_orig':
                p['name'] = 'strength'
                p['label'] = 'Strength'
                p['default'] = 1.0
                p['min'] = 0.0
                p['max'] = 1.0
                p['curve'] = 1.0
    if e['id'] in ('pixel_mosaic', 'pencil_sketch'):
        for p in e['params']:
            if p['name'] == 'mix_orig':
                p['name'] = 'strength'
                p['label'] = 'Strength'
                p['default'] = 1.0
                p['min'] = 0.0
                p['max'] = 1.0
                p['curve'] = 1.0
    if e['id'] == 'mirror_fold':
        for p in e['params']:
            if p['name'] == 'blend':
                p['name'] = 'strength'
                p['label'] = 'Strength'
                p['default'] = 1.0
                p['min'] = 0.0
                p['max'] = 1.0
                p['curve'] = 1.0

# ── Step 5: Bump project version ─────────────────────────────────────────────
reg['project_version'] = 22

with open('effects/registry.json', 'w') as f:
    json.dump(reg, f, indent=2)
print(f"Registry updated: {len(effects_list)} effects, version {reg['project_version']}")

# Print summary of what changed
for e in effects_list:
    for p in e['params']:
        if p.get('curve', 1.0) != 1.0:
            print(f"  {e['id']}.{p['name']}: curve={p['curve']}, [{p['min']}, {p['max']}]")
