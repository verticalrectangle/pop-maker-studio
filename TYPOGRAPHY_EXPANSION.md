# Typography Expansion — toward 100+ elaborate lyric-video styles

*Design doc. No code yet. This is the plan: what the engine needs, which fonts
to bundle, and a catalogue of 100+ concrete styles inspired by real lyric
videos and motion-typography.*

---

## 0. The honest framing

Adding presets is trivial — `g_typo_presets[]` in `src/typography_presets.h` is a
flat array; the picker grid and the `generate_typography` agent API pick up new
entries automatically. The problem is what a preset can *say*. Today it can set:
grouping, position, font **size** (not face), one base color, all-caps/case,
karaoke on/off, and **one of 8 whole-block animations** (Fade, Glitch,
Typewriter*, Bounce, Scale, Slide, Stack, Block). That's it.

So the real constraint isn't "how many presets can I write" — it's "how many
*distinct* things can the renderer do." Right now the answer is ~8 motions × a
few positions × colors. A hundred rows on top of that is a hundred reskins.

\* "Typewriter" is a misnomer — it's a block fade with a tiny vertical nudge.
There is no actual character-by-character reveal anywhere.

**Plan: invest in a handful of engine primitives that each unlock a whole family
of styles, then author the catalogue against them.** The styles below are tagged
with which primitive they need, so we can ship in waves and watch the catalogue
light up as each primitive lands.

---

## 1. The primitives (engine work that unlocks the catalogue)

Ordered by leverage. Each is a force multiplier — one primitive turns into a
dozen styles.

### P1 — Per-element animation (per-word + per-letter) **[biggest unlock]**
Today every animation produces 3 block scalars (`anim_alpha`, `anim_dx`,
`anim_dy`) applied to the whole text block. The single highest-leverage change is
a per-element path in `render_text_block`: each word (and optionally each glyph)
gets its own `(alpha, dx, dy, scale, rot)` driven by an index-based **stagger**
(`delay = i * step`). This one change enables: true typewriter, word-by-word pop,
letter cascade, wave text, exploding letters, gravity drop, elastic settle,
sequential slide-in, per-word bounce — the entire "kinetic" half of lyric-video
typography. ~Triplicated switch (canvas / overlay_renderer / render) needs the
new path in the GL/preview paths at minimum.

New per-preset fields this implies: `unit` (Block / Word / Letter), `stagger_s`
(delay step), `anim_in` + `anim_out` (separate enter/exit motions), `ease`
(see E1).

### P2 — A real font system (multi-typeface) **[unlocks all "voice" variety]**
Text is hardwired to `g_font_black`. JetBrains Mono is already embedded but
unused for text. We need: (a) a small bundled type library (see §2), (b) a
`font_id` field on `TypographyPreset` + `Clip`, (c) thread the chosen `ImFont*`
through `TextRenderCtx`, (d) update the three render callers. The renderer is
already font-agnostic (`ctx.font`) — only the callers hardcode the face. A script
preset that says "script" should actually *be* a script.

### P3 — `TextStyle` as a preset field **[unlocks stroke/glow/shadow/box per style]**
`TextStyle` (shadow, stroke, glow, background box) is per-clip but **not** part of
a preset — only `neon` and `cyberpunk` get styled, via hardcoded `strcmp(id,...)`.
Make `TextStyle` a member of `TypographyPreset` so every preset can carry its own
outline/glow/shadow/pill. Kills the id-switch and lets ~40 styles below exist.

### E1 — An easing library **[quality multiplier, cheap]**
There's no easing — ramps are linear, with one ad-hoc ease-out-cubic and a
sin·exp spring. Add the standard set (ease-in/out cubic & quint, back/overshoot,
elastic, bounce, expo, anticipate). Per-preset `ease` selector. Cheap to write,
makes everything feel designed instead of linear.

### P4 — Karaoke evolved **[unlocks the "real lyric video" feel]**
Karaoke today swaps a word's color at its timestamp. Add: **fill-wipe** (color
sweeps L→R across the active word over its `[start,end]`), **per-word pop**
(active word scales/lifts), **bouncing-ball** dot, and **gradient sweep**. Driven
by the same `words_cache` timing already loaded.

### G1 — Gradient + better decoration **[unlocks the "beautiful" half]**
Everything is solid RGBA. Add: vertical/diagonal **gradient fill**, **gradient
stroke**, animated **hue cycle**, **duotone**. Plus real-ish niceties: **tracking
/ letter-spacing** (huge for that wide-caps aesthetic — currently impossible),
**line-leading** control, **skew/italicize** transform, optional **real blur**
for glow (vs the 8-point ring fake).

### Tiering
- **Wave 1 (catalogue doubles immediately):** P2 + P3 + E1. No new motion, but
  real fonts + per-style stroke/glow/box + proper easing already make ~40 of the
  styles below real (all the editorial/script/retro/minimal families).
- **Wave 2 (the kinetic explosion):** P1. Unlocks ~35 motion styles.
- **Wave 3 (polish):** P4 + G1. The karaoke + gradient + tracking families (~25).

---

## 2. The type library (fonts to bundle)

All Google Fonts / OFL — safe to embed the same way Inter + JetBrains Mono are
(`embed_font` CMake target → header byte array → `AddFontFromMemoryTTF`). Group
by role; `font_id` picks one.

**Sans (workhorses + impact)**
- Inter *(have)* — clean UI/lower-third
- **Anton** — ultra-condensed black caps; the single most "lyric video" face
- **Bebas Neue** — tall condensed caps; trailer/poster energy
- **Archivo Black** / **Archivo Expanded** — wide bold editorial
- **Space Grotesk** — techy neo-grotesque
- **Syne** — arty, extended, gallery-poster weird
- **Poppins / Montserrat** — friendly geometric (TikTok-caption default vibe)
- **Clash/General Sans-ish** → use **Sora** or **Hanken Grotesk** as OFL stand-ins

**Mono (code / terminal / glitch)**
- JetBrains Mono *(have)* — code/terminal
- **Space Mono** — retro-future mono with character
- **VT323** — CRT terminal pixel-mono
- **Major Mono Display** — lowercase mono oddity

**Serif (editorial / cinematic / elegant)**
- **Playfair Display** — Vogue/editorial high-contrast
- **Fraunces** — gorgeous variable "old-style with attitude"
- **DM Serif Display** — clean modern display serif
- **Instrument Serif** — light, airy, A24-trailer serif
- **Cormorant / EB Garamond** — literary, delicate
- **Cinzel** — Roman inscriptional caps (epic/cinematic titles)

**Script / handwritten (the "beautiful scripts" you asked for)**
- **Caveat** — casual marker handwriting
- **Dancing Script** — bouncy casual script
- **Great Vibes / Allura** — formal flowing calligraphy
- **Sacramento** — thin monoline signature
- **Permanent Marker** — bold marker scrawl
- **Homemade Apple** — pencil cursive

**Display / novelty (retro, neon, pixel, signage)**
- **Monoton** — retro neon double-line
- **Bungee** — urban vertical-signage
- **Lobster / Pacifico** — retro script-display
- **Righteous** — rounded art-deco
- **Press Start 2P / Silkscreen** — pixel/8-bit
- **Rubik Mono One** — chunky mono-display

That's ~30 faces across 6 roles. We don't need all at once — bundle per wave as
the styles that use them ship. (Atlas size: subset to Latin + common punctuation
to keep memory sane; ImGui shares one atlas.)

---

## 3. The catalogue — 100+ styles

Legend for the **needs** column:
`P1`=per-element motion · `P2`=font · `P3`=TextStyle-in-preset · `P4`=karaoke+ ·
`G1`=gradient/tracking · `E1`=easing · `—`=expressible today.

Format: **Name** — font · motion · color/deco · *inspiration*. (needs)

### A. Kinetic per-word (the "words build the line" family)
1. **Word Pop** — Anton · each word scales 0.6→1 with back-ease as its timestamp hits · white · *generic modern lyric video*. (P1,P2,E1)
2. **Cascade** — Archivo Black · words drop in top→down, staggered 0.06s · white · *Spotify Canvas*. (P1,P2,E1)
3. **True Type** — JetBrains Mono · real char-by-char reveal + blinking block cursor · green-on-black · *terminal / hacker*. (P1,P2)
4. **Line Build** — Inter · word-by-word fade left→right, prior words stay · white · *clean caption build*. (P1)
5. **Punch In** — Anton · word slams in oversized then settles to 1 (anticipate) · white + shadow · *trap/hype*. (P1,P2,E1,P3)
6. **Stagger Slide** — Space Grotesk · each word slides from left, staggered · cyan · *tech promo*. (P1,P2,E1)
7. **Word Bounce** — Poppins · each word bounces in on a spring · pastel · *upbeat pop*. (P1,P2,E1)
8. **Rise** — Instrument Serif · words rise + fade from below, slow stagger · cream · *A24 trailer*. (P1,P2,E1)
9. **Drop Cap Drop** — Playfair · first letter huge, rest of word follows · ink · *editorial open*. (P1,P2)
10. **Echo** — Inter · word appears with 2 ghost trails that fade · white · *motion-blur echo*. (P1,P3)

### B. Per-letter motion (anomalies)
11. **Wave** — Poppins · letters ride a sine offset, continuous · white · *playful*. (P1,P2)
12. **Letter Cascade** — Montserrat · letters fall in one-by-one · white · *kinetic type reel*. (P1,P2,E1)
13. **Explode In** — Anton · letters fly in from random offsets to place · white · *impact*. (P1,P2,E1)
14. **Gravity** — Bebas · letters drop from top and bounce to baseline · white · *physics title*. (P1,P2,E1)
15. **Elastic Settle** — Sora · letters overshoot + wobble into place · mint · *springy UI*. (P1,P2,E1)
16. **Typewriter Glow** — Space Mono · char reveal with a glow on the newest char · amber · *retro computer*. (P1,P2,P3)
17. **Jitter** — JetBrains Mono · every letter micro-vibrates constantly · white · *anxious/energetic*. (P1,P2)
18. **Flip In** — Archivo · letters flip in on Y (scale-x 0→1 fake-3D) · white · *split-flap board*. (P1,P2,E1)
19. **Scatter Gather** — Inter · letters start scattered, converge on beat · white · *abstract*. (P1,E1)
20. **Breathe** — Fraunces · whole word gently scales 0.98↔1.02 looping · cream · *ambient/lofi*. (P1,P2,E1)

### C. Karaoke evolved (real lyric-sync)
21. **Fill Wipe** — Anton · active word fills with highlight color L→R over its duration · white→yellow · *classic karaoke*. (P4,P2)
22. **Bouncing Ball** — Poppins · dot hops word-to-word above the line · white · *sing-along*. (P4,P2)
23. **Pop Highlight** — Montserrat · active word scales up + brightens · white/red · *modern karaoke*. (P4,P2,E1)
24. **Gradient Sweep** — Space Grotesk · a gradient band sweeps across the active word · neon · *Vevo lyric*. (P4,G1,P2)
25. **Underline Track** — Inter · animated underline grows under the active word · white · *clean follow*. (P4,P3)
26. **Spoken Reveal** — DM Serif · words are invisible until sung, then stay · cream · *poetry*. (P4,P2)
27. **Dim & Lift** — Inter · inactive words 40% + flat, active 100% + lifted 4px · white · *focus*. (P4)
28. **Color March** — Poppins · each sung word locks to a new palette color · rainbow · *kids/party*. (P4,P2)
29. **Neon Trace** — Monoton · active word's stroke lights up like a tube · pink/cyan · *neon sign*. (P4,P2,P3)
30. **Box Highlighter** — Archivo · a highlighter rectangle slides under each word · black on yellow · *marker note*. (P4,P3)

### D. Editorial / serif (the "beautiful" magazine family)
31. **Vogue** — Playfair · slow fade, wide tracking, centered · white · *fashion magazine*. (P2,G1)
32. **Pull Quote** — Fraunces Italic · big quotation marks, fade · ink · *feature article*. (P2,P3)
33. **Masthead** — Playfair Black · giant centered caps, hairline rule above/below · white · *newspaper*. (P2,P3,G1)
34. **Byline** — EB Garamond · small italic lower-third · grey · *credits/attribution*. (P2)
35. **Serif Drop** — DM Serif · line fades up from below, generous leading · cream · *A24*. (P2,G1)
36. **Letterpress** — Cinzel · caps with inset shadow (emboss fake) · stone · *cinematic title*. (P2,P3)
37. **Editorial Split** — Archivo Expanded · word on left, translation/echo small on right · b/w · *layout art*. (P1,P2)
38. **Thin & Wide** — Cormorant · very thin, very tracked-out caps, slow · white · *luxury/perfume ad*. (P2,G1)
39. **Numbered List** — Fraunces · big numeral + serif line · ink · *listicle/countdown*. (P2)
40. **Manifesto** — Instrument Serif · left-aligned paragraph builds line by line · cream · *spoken word*. (P1,P2)

### E. Script / handwritten (the scripts you asked for)
41. **Signature** — Great Vibes · flowing script fades in like a pen finishing · white · *romantic ballad*. (P2,E1)
42. **Handwrite** — Caveat · words appear as if being written, left→right · ink · *vlog/personal*. (P1,P2)
43. **Love Note** — Sacramento · thin signature script, gentle drift up · blush · *love song*. (P2,E1)
44. **Marker** — Permanent Marker · bold scrawl, slight rotation per line · black · *raw/punk*. (P2,P1)
45. **Calligraphy** — Allura · formal hand, large, centered, glow · gold · *wedding/elegant*. (P2,P3)
46. **Pencil** — Homemade Apple · faint cursive, paper-grain bg · graphite · *diary/indie*. (P2,P3)
47. **Brush** — Caveat Bold · thick brush words pop in on beat · red · *streetwear drop*. (P1,P2,E1)
48. **Doodle** — Caveat · words with little hand-drawn underlines/circles · multi · *scrapbook*. (P2,P3)

### F. Mono / code / glitch (terminal + cyber)
49. **Terminal** — VT323 · green CRT text, scanlines FX, blinking cursor · green · *hacker movie*. (P2,P3,fx)
50. **Matrix** — JetBrains Mono · letters cycle random glyphs then resolve · green · *digital rain*. (P1,P2)
51. **Datamosh** — Space Mono · glitch jitter + chromatic split on beat · cyan/red · *glitch art*. (P2,P3,fx)
52. **Compile** — JetBrains Mono · `> ` prompt prefix, char reveal, syntax-color words · multi · *dev aesthetic*. (P1,P2,G1)
53. **Pixel** — Press Start 2P · chunky 8-bit caps, hard cut · white · *retro game*. (P2)
54. **ASCII Wipe** — Major Mono · letters de-resolve into symbols on exit · grey · *experimental*. (P1,P2)
55. **Redacted** — JetBrains Mono · words start as ████ then reveal · white/black · *classified*. (P1,P2,P3)
56. **Glitch Caps** — Space Grotesk · RGB-split + slice offset, hard · white · *cyberpunk* (upgrade of current). (P2,P3,fx)

### G. Hype / trap / impact (energy family)
57. **Flash** *(have, keep)* — Anton · one word full-frame, hard cut · white. (P2)
58. **Shake** — Anton · word vibrates hard then settles, screen-fills · white · *808 drop*. (P1,P2,E1)
59. **Zoom Punch** — Archivo Black · word zooms from 3× to 1, blur-in · white · *hype intro*. (P1,P2,E1)
60. **Strobe** *(have, upgrade)* — Anton · word inverts b/w per beat · b/w. (P4)
61. **Stamp** — Bebas · word slams down rotated then straightens · red · *approved/impact*. (P1,P2,E1,P3)
62. **3D Extrude** — Archivo Black · faux-3D layered shadow offset, pops · yellow · *MrBeast thumbnail*. (P2,P3)
63. **Outline Only** — Anton · hollow stroked caps, fill flashes on beat · white outline · *streetwear*. (P2,P3,P4)
64. **Mega Caps** — Bebas · enormous tracked caps fill the frame, slow push-in · white · *festival visuals*. (P1,P2,G1)
65. **Slam Stack** — Anton · words stack vertically, each slams in · white · *rap multi-line*. (P1,P2,E1)

### H. Retro / vintage (era families)
66. **VHS** — VT323 · wobble + chroma bleed + scanlines · white · *80s home video*. (P2,P3,fx)
67. **Neon Sign** — Monoton · glowing tube letters, flicker on entry · pink · *bar sign*. (P2,P3,E1)
68. **Chrome 80s** — Bebas · gradient chrome fill + outline · silver/blue · *Miami/synthwave*. (P2,G1,P3)
69. **Vaporwave** — Space Mono · wide tracking, pink/cyan gradient, drift · *aesthetic*. (P2,G1)
70. **70s Groovy** — Lobster · warm script, gentle wave · orange/brown · *funk/soul*. (P1,P2)
71. **Film Title** — Cinzel · caps fade up over letterbox bars · cream · *opening credits*. (P2,P3)
72. **Grindhouse** — Anton · distressed, jitter, film-grain FX · yellow · *exploitation poster*. (P2,P3,fx)
73. **Polaroid Caption** — Caveat · handwritten on a white strip at bottom · ink · *memory/nostalgia*. (P2,P3)
74. **Disco** — Righteous · letters cycle hue, slight bounce · rainbow · *dance/party*. (P1,P2,G1)
75. **Cassette** — Space Mono · J-card label look, mono caps, bracketed · white · *mixtape*. (P2,P3)

### I. Minimal / aesthetic / indie (quiet families)
76. **Tumblr** *(have, keep)* — Inter · lowercase, centered, soft fade · white. (—)
77. **A24** — Instrument Serif · tiny centered serif, very slow fade, lots of space · cream · *indie film*. (P2,E1)
78. **Lower Third** — Inter · clean name/title bottom-left, slide-in bar · white · *documentary*. (P1,P3)
79. **Wide Caps** — Archivo · letter-spaced thin caps, fade · white · *minimal brand*. (P2,G1)
80. **Centered Serif** — Cormorant · delicate centered line, breathe · white · *poetry/ambient*. (P1,P2)
81. **Sticky Note** — Caveat · words on a small colored square, settle · ink/yellow · *casual*. (P2,P3,E1)
82. **Subtitle Clean** — Inter · Netflix-style bottom subtitle, instant · white + shadow · *dialogue*. (P3)
83. **Whisper** — Sacramento · faint thin script, very low opacity, drift · white · *intimate*. (P2)
84. **Margin Note** — EB Garamond Italic · small, off to the side, fade · grey · *annotation*. (P2)

### J. Gradient / color (chromatic families)
85. **Sunset** — Poppins · vertical orange→pink gradient fill, slow · *warm pop*. (G1,P2)
86. **Holographic** — Space Grotesk · animated iridescent hue shift · multi · *Y2K/foil*. (G1,P2)
87. **Duotone** — Anton · two-color split fill on a diagonal · pink/blue · *poster*. (G1,P2)
88. **Rainbow March** — Montserrat · each word a spectrum color in order · rainbow · *pride/fun*. (P4,P2)
89. **Gold Foil** — Cinzel · metallic gradient + soft glow · gold · *luxury/awards*. (G1,P2,P3)
90. **Neon Outline** — Bebas · transparent fill, glowing gradient stroke, pulse · cyan/magenta · *nightlife*. (G1,P2,P3,E1)
91. **Frost** — Inter · white text, subtle blue gradient, soft blur edge · *cool/winter*. (G1,P2)
92. **Ember** — Anton · fill flickers orange→red like fire, per letter · *intense*. (P1,G1,P2)

### K. Cinematic / structural (title + caption families)
93. **Opening Credits** — Cinzel · name fades in center, fades out, letterboxed · cream · *film open*. (P2,P3)
94. **End Roll** — EB Garamond · lines scroll up slowly · grey · *credits*. (P1,P2)
95. **Chapter Card** — DM Serif · "Chapter I" big + subtitle, fade · ink · *narrative*. (P2)
96. **Quote Card** — Fraunces Italic · centered quote + small attribution below · cream · *inspirational*. (P1,P2)
97. **News Lower-Third** — Archivo · two-tier bar (headline + ticker), slide · white/red · *broadcast*. (P1,P3)
98. **Countdown** — Bebas · huge numeral swaps 3·2·1 with scale-punch · white · *intro/hype*. (P1,P2,E1)
99. **Letterbox Sub** — Inter · caption sits in the lower black bar, clean · white · *cinematic dialogue*. (P3)
100. **Title Stack** — Anton + Instrument Serif · bold line + thin serif subtitle, staggered · white · *poster lockup*. (P1,P2)

### Bonus / wildcards (typographic anomalies)
101. **Liquid** — Poppins · letters wobble like they're underwater (per-letter sine + scale) · aqua · *dreamy*. (P1,P2)
102. **Magnetic** — Space Grotesk · letters repel from the playhead/cursor then snap back · white · *interactive feel*. (P1)
103. **Shatter Out** — Anton · on exit, letters crack apart and fall · white · *dramatic cut*. (P1,P2,E1)
104. **Mirror** — Archivo · word + a faded reflection beneath · white · *glossy*. (P2,P3)
105. **Ransom Note** — mixed (alternating faces per word/letter) · cut-out colors · *punk collage* (needs per-element font — stretch). (P1,P2,G1)

---

## 4. Authoring + infrastructure notes

- **Switch to designated initializers** for `g_typo_presets[]` before adding 100
  rows. The current positional aggregate init (with an optional trailing
  `text_case`) is unmaintainable at scale — one missed comma shifts every field.
- **Kill the id-string special-casing.** `neon`/`cyberpunk` styling and
  `strobe`/`rave` behavior are hardcoded by `strcmp(id,...)` in
  `panel_animation.cpp`. Folding `TextStyle` (P3) and per-element params (P1) into
  the struct removes these and makes presets fully data-driven.
- **The animation switch is triplicated** (`canvas.cpp`, `overlay_renderer.cpp`,
  `render.cpp`). Before P1, consider extracting it into one shared function so
  preview == export stays true with a single edit. The ffmpeg `drawtext` fallback
  in `render.cpp` can't do per-letter motion at all — it should degrade to a
  block fade for kinetic presets (the GL overlay export is the real WYSIWYG path).
- **Categories:** the picker filters by `category`. With 100 styles, add the new
  families as categories (Kinetic, Script, Editorial, Retro, Mono, Gradient,
  Cinematic, Karaoke) so the grid stays navigable. Consider a search box.
- **Font atlas budget:** subset each bundled face to Latin + punctuation; lazy-add
  faces per wave rather than loading all 30 up front.
- **Agent API:** every new `id` is automatically callable via
  `generate_typography(preset=...)`. Worth grouping the ids by family in the
  method's hint text so the agent can pick sensibly.

## 5. Suggested rollout

1. **Wave 1 — "make presets real":** P2 (fonts) + P3 (TextStyle in preset) + E1
   (easing) + designated initializers. Ship the Editorial, Script, Retro,
   Minimal, Gradient-lite families (~45 styles) — *zero new motion code*, all
   expressible once a preset can pick a face + carry a stroke/glow/box.
2. **Wave 2 — "kinetic":** P1 (per-element + stagger). Unlocks Kinetic, Per-letter,
   Hype, and the anomalies (~40 styles).
3. **Wave 3 — "lyric-grade":** P4 (karaoke evolved) + G1 (gradient/tracking).
   The Karaoke family + the gradient-heavy styles (~25) + tracking retrofit across
   everything.

After all three, the catalogue is 100+ and — more importantly — they're 100+
*different* things, not 100 fades.
