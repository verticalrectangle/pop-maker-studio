#!/usr/bin/env python3
"""
Patches shaders to add u_strength uniform and mix with original.
Run from repo root: python3 tools/patch_strength_shaders.py
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(ROOT, "shaders")

def patch(name, old, new):
    path = os.path.join(SHADERS, f"{name}.glsl")
    with open(path) as f:
        src = f.read()
    if old not in src:
        print(f"  SKIP {name}: pattern not found")
        return
    src = src.replace(old, new, 1)
    with open(path, "w") as f:
        f.write(src)
    print(f"  patched {name}")

def insert_uniform(name, uniform_line="uniform float u_strength;"):
    path = os.path.join(SHADERS, f"{name}.glsl")
    with open(path) as f:
        src = f.read()
    if uniform_line in src:
        print(f"  SKIP {name}: already has {uniform_line}")
        return src
    lines = src.split("\n")
    # Find the last uniform declaration
    last_uniform = -1
    for i, line in enumerate(lines):
        if line.strip().startswith("uniform "):
            last_uniform = i
    if last_uniform == -1:
        print(f"  WARN {name}: no uniform found")
        return src
    lines.insert(last_uniform + 1, uniform_line)
    result = "\n".join(lines)
    with open(path, "w") as f:
        f.write(result)
    return result

# ── Shaders needing u_strength + mix(orig.rgb, result, u_strength) ──────────

# ascii_art: already has orig = texture(u_tex, v_uv)
insert_uniform("ascii_art")
patch("ascii_art",
    "    frag = vec4(clamp(result, 0.0, 1.0), orig.a);\n}",
    "    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);\n}")

# barrel_warp: no orig, just warps uv
insert_uniform("barrel_warp")
patch("barrel_warp",
    "    frag = texture(u_tex, clamp(uv, 0.0, 1.0));\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    vec4 warped = texture(u_tex, clamp(uv, 0.0, 1.0));\n    frag = vec4(mix(orig.rgb, warped.rgb, u_strength), orig.a);\n}")

# bit_crush: col = texture(u_tex, v_uv)
insert_uniform("bit_crush")
patch("bit_crush",
    "    frag = vec4(clamp(crushed, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, crushed, u_strength), 0.0, 1.0), col.a);\n}")

# comic_dots: col is cell color (not orig). Add orig separately.
insert_uniform("comic_dots")
patch("comic_dots",
    "    frag = vec4(clamp(result, 0.0, 1.0), 1.0);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);\n}")

# contour_map: col = texture(u_tex, v_uv)
insert_uniform("contour_map")
patch("contour_map",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# crosshatch: col = texture(u_tex, v_uv)
insert_uniform("crosshatch")
patch("crosshatch",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# crt: barrel-warped, scanlines always on. Need to save orig before warp check.
insert_uniform("crt")
patch("crt",
    "void main() {\n    // Barrel warp",
    "void main() {\n    vec4 orig = texture(u_tex, v_uv);\n    // Barrel warp")
patch("crt",
    "    frag = col;\n}",
    "    col.rgb = mix(orig.rgb, col.rgb, u_strength);\n    frag = col;\n}")

# daguerreotype: col = texture(u_tex, v_uv) — plate + scratch, result is toned
insert_uniform("daguerreotype")
patch("daguerreotype",
    "    frag = vec4(clamp(toned + plate, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, toned + plate, u_strength), 0.0, 1.0), col.a);\n}")

# dither_bayer: col = texture(u_tex, v_uv)
insert_uniform("dither_bayer")
patch("dither_bayer",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# dna_helix: col = texture(u_tex, v_uv)
insert_uniform("dna_helix")
patch("dna_helix",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# duotone: col = texture(u_tex, v_uv)
insert_uniform("duotone")
patch("duotone",
    "    frag = vec4(mix(shadow, highlight, luma), col.a);\n}",
    "    vec3 duotone_result = mix(shadow, highlight, luma);\n    frag = vec4(mix(col.rgb, duotone_result, u_strength), col.a);\n}")

# echo_trails: col = texture(u_tex, v_uv). result.rgb is screen-blended echoes.
insert_uniform("echo_trails")
patch("echo_trails",
    "    frag = vec4(clamp(result.rgb, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result.rgb, u_strength), 0.0, 1.0), col.a);\n}")

# ice_crystal: samples refracted UV, adds bright borders. Add orig.
insert_uniform("ice_crystal")
patch("ice_crystal",
    "    frag = vec4(clamp(ice_tint, 0.0, 1.0), 1.0);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(orig.rgb, ice_tint, u_strength), 0.0, 1.0), orig.a);\n}")

# infrared_film: col = texture(u_tex, v_uv)
insert_uniform("infrared_film")
patch("infrared_film",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# kaleidoscope: just samples mirrored uv. Add orig.
insert_uniform("kaleidoscope")
patch("kaleidoscope",
    "    frag = texture(u_tex, uv);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    vec4 effect = texture(u_tex, uv);\n    frag = vec4(mix(orig.rgb, effect.rgb, u_strength), orig.a);\n}")

# kodachrome: col = texture(u_tex, v_uv)
insert_uniform("kodachrome")
patch("kodachrome",
    "    frag = vec4(clamp(sat, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, sat, u_strength), 0.0, 1.0), col.a);\n}")

# lomo: col = texture(u_tex, v_uv). Result is sat*vig.
insert_uniform("lomo")
patch("lomo",
    "    frag = vec4(sat * vig, col.a);\n}",
    "    vec3 lomo_result = sat * vig;\n    frag = vec4(mix(col.rgb, lomo_result, u_strength), col.a);\n}")

# mirror_tunnel: just warps. Add orig.
insert_uniform("mirror_tunnel")
patch("mirror_tunnel",
    "    frag = texture(u_tex, clamp(uv, 0.0, 1.0));\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    vec4 effect = texture(u_tex, clamp(uv, 0.0, 1.0));\n    frag = vec4(mix(orig.rgb, effect.rgb, u_strength), orig.a);\n}")

# neon_edge_glow: col = texture(u_tex, v_uv) — inside main.
insert_uniform("neon_edge_glow")
patch("neon_edge_glow",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# night_vision: col = texture(u_tex, v_uv). Result is green phosphor.
insert_uniform("night_vision")
patch("night_vision",
    "    frag = vec4(g * 0.15, g, g * 0.08, col.a);\n}",
    "    vec3 nv_result = vec3(g * 0.15, g, g * 0.08);\n    frag = vec4(mix(col.rgb, nv_result, u_strength), col.a);\n}")

# oil_paint: already has orig = texture(u_tex, v_uv).rgb, alpha needs separate fetch.
insert_uniform("oil_paint")
patch("oil_paint",
    "    frag = vec4(clamp(result + (result - orig) * (u_sharpness * 0.05), 0.0, 1.0), 1.0);\n}",
    "    vec3 oil_result = clamp(result + (result - orig) * (u_sharpness * 0.05), 0.0, 1.0);\n    frag = vec4(mix(orig, oil_result, u_strength), 1.0);\n}")

# pointillist: no orig (renders dots on white paper). Add orig.
insert_uniform("pointillist")
patch("pointillist",
    "    frag = vec4(clamp(result, 0.0, 1.0), 1.0);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);\n}")

# posterize: col = texture(u_tex, v_uv)
insert_uniform("posterize")
patch("posterize",
    "    frag = vec4(p, col.a);\n}",
    "    frag = vec4(mix(col.rgb, p, u_strength), col.a);\n}")

# risograph: col1 is misregistered — add true orig.
insert_uniform("risograph")
patch("risograph",
    "    frag = vec4(clamp(result, 0.0, 1.0), col1.a);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);\n}")

# stained_glass: no v_uv orig — add it.
insert_uniform("stained_glass")
patch("stained_glass",
    "    frag = vec4(clamp(result, 0.0, 1.0), 1.0);\n}",
    "    vec4 orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);\n}")

# super8_film: col is warped (gate weave). Add exact orig.
insert_uniform("super8_film")
patch("super8_film",
    "    frag = vec4(clamp(warm, 0.0, 1.0), col.a);\n}",
    "    vec4 exact_orig = texture(u_tex, v_uv);\n    frag = vec4(clamp(mix(exact_orig.rgb, warm, u_strength), 0.0, 1.0), exact_orig.a);\n}")

# technicolor: col = texture(u_tex, v_uv)
insert_uniform("technicolor")
patch("technicolor",
    "    frag = vec4(clamp(sat, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, sat, u_strength), 0.0, 1.0), col.a);\n}")

# thermal_map: col = texture(u_tex, v_uv)
insert_uniform("thermal_map")
patch("thermal_map",
    "    frag = vec4(clamp(thermal * scan, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, thermal * scan, u_strength), 0.0, 1.0), col.a);\n}")

# vintage_negative: col = texture(u_tex, v_uv)
insert_uniform("vintage_negative")
patch("vintage_negative",
    "    frag = vec4(clamp(neg + g, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, neg + g, u_strength), 0.0, 1.0), col.a);\n}")

# warhol_pop: col = texture(u_tex, v_uv)
insert_uniform("warhol_pop")
patch("warhol_pop",
    "    frag = vec4(clamp(result, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);\n}")

# watercolor: orig = texture(u_tex, v_uv) at bottom
insert_uniform("watercolor")
patch("watercolor",
    "    frag = vec4(clamp(wash, 0.0, 1.0), orig.a);\n}",
    "    frag = vec4(clamp(mix(orig.rgb, wash, u_strength), 0.0, 1.0), orig.a);\n}")

# x_ray: col = texture(u_tex, v_uv)
insert_uniform("x_ray")
patch("x_ray",
    "    frag = vec4(clamp(xray, 0.0, 1.0), col.a);\n}",
    "    frag = vec4(clamp(mix(col.rgb, xray, u_strength), 0.0, 1.0), col.a);\n}")

# zone_system_bw: col = texture(u_tex, v_uv)
insert_uniform("zone_system_bw")
patch("zone_system_bw",
    "    frag = vec4(result, col.a);\n}",
    "    frag = vec4(mix(col.rgb, result, u_strength), col.a);\n}")

# ── Inverted-param fixes (rename mix_orig->strength, flip semantics) ────────

# gradient_map: mix_orig -> strength, flip direction
path = os.path.join(SHADERS, "gradient_map.glsl")
with open(path) as f:
    src = f.read()
src = src.replace("uniform float u_mix_orig;", "uniform float u_strength;")
src = src.replace("mix(mapped, col.rgb, u_mix_orig)", "mix(col.rgb, mapped, u_strength)")
with open(path, "w") as f:
    f.write(src)
print("  patched gradient_map (inverted param)")

# pixel_mosaic: mix_orig -> strength, flip direction
path = os.path.join(SHADERS, "pixel_mosaic.glsl")
with open(path) as f:
    src = f.read()
src = src.replace("uniform float u_mix_orig;", "uniform float u_strength;")
src = src.replace("mix(c.rgb, orig.rgb, u_mix_orig)", "mix(orig.rgb, c.rgb, u_strength)")
with open(path, "w") as f:
    f.write(src)
print("  patched pixel_mosaic (inverted param)")

# pencil_sketch: mix_orig -> strength, flip direction
path = os.path.join(SHADERS, "pencil_sketch.glsl")
with open(path) as f:
    src = f.read()
src = src.replace("uniform float u_mix_orig;", "uniform float u_strength;")
src = src.replace("mix(result, orig.rgb, u_mix_orig)", "mix(orig.rgb, result, u_strength)")
with open(path, "w") as f:
    f.write(src)
print("  patched pencil_sketch (inverted param)")

# mirror_fold: blend -> strength, flip direction
path = os.path.join(SHADERS, "mirror_fold.glsl")
with open(path) as f:
    src = f.read()
src = src.replace("uniform float u_blend;", "uniform float u_strength;")
src = src.replace("frag = mix(fold, orig, u_blend);", "frag = mix(orig, fold, u_strength);")
with open(path, "w") as f:
    f.write(src)
print("  patched mirror_fold (inverted param)")

print("\nAll shaders patched.")
