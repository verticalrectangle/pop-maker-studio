#pragma once
#include "app.h"
#include "text_anim.h"   // EASE_* curve ids
#include <cstring>

// ── FX clip descriptor ────────────────────────────────────────────────────────
// Describes an FX clip the typography generator creates above the lyrics track.
struct TypoFXDesc {
    FXType type          = FXType::Glitch;
    float  beat_intensity = 0.f;  // if > 0, first beat-syncable param gets this intensity
};

// ── Typography preset ─────────────────────────────────────────────────────────
// Authored with designated initializers — only fields that differ from the
// defaults below are listed per preset, so the catalogue stays readable at
// scale. Field ORDER here is the declaration order designated init must follow.
struct TypographyPreset {
    const char* id;
    const char* label;
    const char* tagline;
    const char* category; // Hype/Aesthetic/Editorial/Clean/Retro/Script/Mono/Cinematic/Kinetic/Karaoke/Gradient
    const char* font = nullptr;   // bundled face id (sanitized family name); null/"" = Inter Black

    // Grouping
    SubtitleMode grouping     = SubtitleMode::Phrase;
    int          custom_n     = 3;

    // Typography
    float font_size    = 0.09f;  // fraction of canvas height
    float sub_pos_x    = 0.5f;   // 0=left 1=right
    int   sub_pos      = 1;      // 0=bottom 1=center 2=top 3=custom Y
    float sub_pos_y    = 0.5f;   // only when sub_pos==3
    int   sub_anchor_h = 1;      // 0=left 1=center 2=right
    float sub_wrap_w   = 0.85f;
    float color[4]     = {1.f, 1.f, 1.f, 1.f};
    bool  all_caps     = false;  // legacy upper-case flag; -1 text_case derives from it
    bool  karaoke      = false;
    AnimStyle style    = AnimStyle::None;

    // Grouping tuning (0 = use mode default)
    float pause_gap = 0.f;  // gap threshold in seconds to break a group
    int   max_words = 0;    // hard cap on words per group (0 = no limit)

    // FX clips to auto-generate (max 3)
    TypoFXDesc fx[3]   = {};
    int        n_fx    = 0;

    // Letter case: -1 = derive from all_caps (back-compat); 0=as-typed 1=UPPER 2=lower
    int        text_case = -1;

    // ── Kinetic / styling (Waves 1-3) ────────────────────────────────────────
    int   ease         = 0;     // easing curve (text_anim.h EASE_*); 0 = style default
    float tracking     = 0.f;   // letter-spacing, fraction of font size
    int   anim_unit    = 0;     // 0=block 1=word 2=letter (per-element motion)
    float anim_stagger = 0.06f; // per-element delay step (seconds)
    int   karaoke_mode = 0;     // 0=color 1=fill-wipe 2=pop 3=bouncing-ball
    int   grad_mode    = 0;     // 0=none 1=vertical 2=diagonal 3=hue-cycle
    float grad_col2[4] = {1.f, 1.f, 1.f, 1.f};  // second gradient stop
    float karaoke_highlight_color[4] = {1.f, 0.85f, 0.1f, 1.f};  // active-word colour
    TextStyle ts       = {};    // shadow/stroke/glow/box, travels with the preset
};

// ── Preset catalogue ──────────────────────────────────────────────────────────
static const TypographyPreset g_typo_presets[] = {

    // ── Hype / Club ──────────────────────────────────────────────────────────

    {   .id="flash", .label="Flash",
        .tagline="One word · full-frame · cuts hard · pure energy",
        .category="Hype", .font="anton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.18f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::None, .pause_gap=0.08f, .max_words=1 },

    {   .id="strobe", .label="Strobe",
        .tagline="Flash but inverts every word · ultra aggressive",
        .category="Hype", .font="anton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.18f, .sub_pos=1,
        .all_caps=true, .pause_gap=0.08f, .max_words=1 },

    {   .id="rave", .label="Rave",
        .tagline="Random positions · beat-reactive · neon chaos",
        .category="Hype", .font="bungee",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.14f, .sub_pos=3, .sub_pos_y=0.5f,
        .color={1.f, 0.1f, 0.9f, 1.f}, .all_caps=true, .style=AnimStyle::Scale,
        .pause_gap=0.12f, .max_words=1,
        .fx={ {FXType::ChromaticAberration, 0.8f} }, .n_fx=1 },

    {   .id="impact", .label="Impact",
        .tagline="Heavy bold · slams to full size · pop",
        .category="Hype", .font="archivoblack",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.17f, .sub_pos=1,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.10f, .max_words=1,
        .ease=EASE_OUT_BACK,
        .ts={ .shadow_enabled=true, .shadow_ox=4.f, .shadow_oy=4.f, .shadow_col={0.f,0.f,0.f,0.85f} } },

    {   .id="megacaps", .label="Mega Caps",
        .tagline="Enormous condensed caps · fills the frame · festival",
        .category="Hype", .font="bebasneue",
        .grouping=SubtitleMode::CustomN, .custom_n=2,
        .font_size=0.22f, .sub_pos=1, .sub_wrap_w=0.95f,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.15f, .max_words=2,
        .tracking=0.04f },

    {   .id="cyberpunk", .label="Cyberpunk",
        .tagline="Monospace · cyan · glitch · bottom-left",
        .category="Mono", .font="spacemono",
        .grouping=SubtitleMode::CustomN, .custom_n=3,
        .font_size=0.12f, .sub_pos_x=0.08f, .sub_pos=0, .sub_pos_y=0.88f, .sub_anchor_h=0, .sub_wrap_w=0.9f,
        .color={0.0f, 1.f, 0.95f, 1.f}, .all_caps=true, .style=AnimStyle::Glitch,
        .pause_gap=0.20f, .max_words=3,
        .fx={ {FXType::ChromaticAberration, 0.7f}, {FXType::Scanlines, 0.f} }, .n_fx=2,
        .ts={ .stroke_enabled=true, .stroke_w=1.5f, .stroke_col={0.f,1.f,1.f,0.8f} } },

    {   .id="drill", .label="Drill",
        .tagline="3 words · top third · yellow on black · aggressive",
        .category="Hype", .font="archivoblack",
        .grouping=SubtitleMode::CustomN, .custom_n=3,
        .font_size=0.10f, .sub_pos=2, .sub_pos_y=0.15f,
        .color={1.f, 0.95f, 0.0f, 1.f}, .all_caps=true, .pause_gap=0.25f, .max_words=3 },

    {   .id="stamp", .label="Stamp",
        .tagline="Slams down rotated · red · approved energy",
        .category="Hype", .font="bebasneue",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.16f, .sub_pos=1,
        .color={0.92f, 0.12f, 0.12f, 1.f}, .all_caps=true, .style=AnimStyle::Scale,
        .pause_gap=0.12f, .max_words=1, .ease=EASE_OUT_BACK,
        .ts={ .stroke_enabled=true, .stroke_w=2.f, .stroke_col={0.f,0.f,0.f,1.f} } },

    // ── Aesthetic / Indie ────────────────────────────────────────────────────

    {   .id="tumblr", .label="Tumblr",
        .tagline="Helvetica energy · lowercase · centered · clean",
        .category="Aesthetic",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.08f, .sub_pos=1, .sub_wrap_w=0.80f,
        .style=AnimStyle::Fade, .pause_gap=0.40f, .max_words=6, .text_case=2 },

    {   .id="indie2012", .label="Indie 2012",
        .tagline="Small · muted · left offset · minimal",
        .category="Aesthetic",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.06f, .sub_pos_x=0.12f, .sub_pos=1, .sub_anchor_h=0, .sub_wrap_w=0.75f,
        .color={0.85f, 0.82f, 0.78f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.50f, .max_words=5, .text_case=2 },

    {   .id="sadgirl", .label="Sad Girl",
        .tagline="Large single word · pastel · centered · slow",
        .category="Aesthetic", .font="cormorant",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.16f, .sub_pos=1,
        .color={0.95f, 0.75f, 0.88f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.20f, .max_words=1, .text_case=2, .ease=EASE_OUT_CUBIC },

    {   .id="cottagecore", .label="Cottagecore",
        .tagline="Warm cream · serif · gentle film grain",
        .category="Aesthetic", .font="ebgaramond",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.075f, .sub_pos=1, .sub_wrap_w=0.80f,
        .color={0.96f, 0.91f, 0.78f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.60f, .max_words=6,
        .fx={ {FXType::FilmGrain, 0.f} }, .n_fx=1, .text_case=2, .ease=EASE_OUT_CUBIC },

    {   .id="film", .label="Film",
        .tagline="Bottom third · sentence · classic subtitle",
        .category="Aesthetic",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.055f, .sub_pos=0, .sub_pos_y=0.88f,
        .style=AnimStyle::Fade, .pause_gap=0.70f, .max_words=8 },

    {   .id="a24", .label="A24",
        .tagline="Tiny centered serif · slow fade · lots of air",
        .category="Aesthetic", .font="instrumentserif",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.07f, .sub_pos=1, .sub_wrap_w=0.7f,
        .color={0.97f, 0.95f, 0.9f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.6f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    {   .id="whisper", .label="Whisper",
        .tagline="Faint thin script · low opacity · intimate",
        .category="Aesthetic", .font="sacramento",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.7f,
        .color={1.f, 1.f, 1.f, 0.8f}, .style=AnimStyle::Fade,
        .pause_gap=0.5f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    // ── Editorial / Serif ────────────────────────────────────────────────────

    {   .id="headline", .label="Headline",
        .tagline="Massive single word · fills frame · high contrast",
        .category="Editorial", .font="archivoblack",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.20f, .sub_pos=1, .sub_wrap_w=0.92f,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.12f, .max_words=1 },

    {   .id="manifesto", .label="Manifesto",
        .tagline="Full left flush · stacked · uppercase · maximum",
        .category="Editorial", .font="archivoblack",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.11f, .sub_pos_x=0.05f, .sub_pos=1, .sub_anchor_h=0, .sub_wrap_w=0.92f,
        .all_caps=true, .style=AnimStyle::None, .pause_gap=0.45f, .max_words=5 },

    {   .id="vogue", .label="Vogue",
        .tagline="High-contrast serif · wide tracking · fashion",
        .category="Editorial", .font="playfairdisplay",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.8f,
        .all_caps=true, .style=AnimStyle::Fade, .pause_gap=0.5f, .max_words=5,
        .ease=EASE_OUT_CUBIC, .tracking=0.12f, .ts={ .shadow_enabled=false } },

    {   .id="pullquote", .label="Pull Quote",
        .tagline="Italic serif · feature-article energy",
        .category="Editorial", .font="fraunces",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.085f, .sub_pos=1, .sub_wrap_w=0.8f,
        .color={0.97f, 0.96f, 0.93f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.6f, .max_words=8, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    {   .id="serifdrop", .label="Serif Drop",
        .tagline="Clean display serif · rises from below · A24",
        .category="Editorial", .font="dmserifdisplay",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.8f,
        .color={0.98f, 0.96f, 0.92f, 1.f}, .style=AnimStyle::Stack,
        .pause_gap=0.5f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    {   .id="thinwide", .label="Thin & Wide",
        .tagline="Delicate tracked caps · luxury · slow",
        .category="Editorial", .font="cormorant",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.09f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::Fade, .pause_gap=0.6f, .max_words=4,
        .ease=EASE_OUT_CUBIC, .tracking=0.2f, .ts={ .shadow_enabled=false } },

    {   .id="letterpress", .label="Letterpress",
        .tagline="Roman caps · inset shadow · cinematic stone",
        .category="Editorial", .font="cinzel",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.09f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={0.88f, 0.85f, 0.8f, 1.f}, .all_caps=true, .style=AnimStyle::Fade,
        .pause_gap=0.6f, .max_words=5, .ease=EASE_OUT_CUBIC, .tracking=0.06f,
        .ts={ .shadow_enabled=true, .shadow_ox=-1.f, .shadow_oy=-1.f, .shadow_col={1.f,1.f,1.f,0.25f} } },

    {   .id="newspaper", .label="Newspaper",
        .tagline="Tight centered serif block · sentences",
        .category="Editorial", .font="playfairdisplay",
        .grouping=SubtitleMode::Segment, .custom_n=3,
        .font_size=0.07f, .sub_pos=1, .sub_wrap_w=0.75f,
        .all_caps=true, .style=AnimStyle::None, .pause_gap=0.80f, .max_words=10,
        .ts={ .shadow_enabled=false } },

    {   .id="zine", .label="Zine",
        .tagline="Mixed case · phrases · lo-fi rotated energy",
        .category="Editorial", .font="archivoblack",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.09f, .sub_pos=1,
        .style=AnimStyle::Glitch, .pause_gap=0.35f, .max_words=5 },

    // ── Script / Handwritten ─────────────────────────────────────────────────

    {   .id="signature", .label="Signature",
        .tagline="Flowing calligraphy · fades in like a pen finishing",
        .category="Script", .font="greatvibes",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.14f, .sub_pos=1, .sub_wrap_w=0.8f,
        .style=AnimStyle::Fade, .pause_gap=0.5f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    {   .id="lovenote", .label="Love Note",
        .tagline="Thin signature script · drifts up · blush",
        .category="Script", .font="sacramento",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.12f, .sub_pos=1, .sub_wrap_w=0.75f,
        .color={1.f, 0.85f, 0.9f, 1.f}, .style=AnimStyle::Stack,
        .pause_gap=0.5f, .max_words=5, .ease=EASE_OUT_CUBIC, .ts={ .shadow_enabled=false } },

    {   .id="marker", .label="Marker",
        .tagline="Bold scrawl · raw · slight punk energy",
        .category="Script", .font="permanentmarker",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.8f,
        .style=AnimStyle::Fade, .pause_gap=0.4f, .max_words=5 },

    {   .id="calligraphy", .label="Calligraphy",
        .tagline="Formal hand · large · gold · soft glow · wedding",
        .category="Script", .font="allura",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.15f, .sub_pos=1, .sub_wrap_w=0.8f,
        .color={1.f, 0.9f, 0.55f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.6f, .max_words=4, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=10.f, .glow_col={1.f,0.85f,0.4f,0.5f} } },

    {   .id="pencil", .label="Pencil",
        .tagline="Faint cursive · diary · indie",
        .category="Script", .font="homemadeapple",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.085f, .sub_pos=1, .sub_wrap_w=0.75f,
        .color={0.9f, 0.9f, 0.92f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.5f, .max_words=4, .ts={ .shadow_enabled=false } },

    {   .id="brush", .label="Brush",
        .tagline="Thick brush hand · pops on the beat · streetwear",
        .category="Script", .font="caveat",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.16f, .sub_pos=1,
        .color={0.95f, 0.2f, 0.2f, 1.f}, .style=AnimStyle::Scale,
        .pause_gap=0.15f, .max_words=1, .ease=EASE_OUT_BACK },

    {   .id="handscript", .label="Handscript",
        .tagline="Bouncy casual script · personal vlog energy",
        .category="Script", .font="dancingscript",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.8f,
        .style=AnimStyle::Fade, .pause_gap=0.45f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    // ── Clean / Modern ───────────────────────────────────────────────────────

    {   .id="minimal", .label="Minimal",
        .tagline="Small · centered · breathing room · muted",
        .category="Clean",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.055f, .sub_pos=1, .sub_wrap_w=0.75f,
        .color={0.9f, 0.9f, 0.9f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.40f, .max_words=5, .ts={ .shadow_enabled=false } },

    {   .id="spotify", .label="Spotify",
        .tagline="Bottom center · clean sans · phrase · white",
        .category="Clean",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.07f, .sub_pos=0, .sub_pos_y=0.85f, .sub_wrap_w=0.82f,
        .style=AnimStyle::Fade, .pause_gap=0.35f, .max_words=6 },

    {   .id="apple", .label="Apple",
        .tagline="Single word · massive · fades · pristine white",
        .category="Clean",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.16f, .sub_pos=1, .sub_wrap_w=0.88f,
        .style=AnimStyle::Fade, .pause_gap=0.18f, .max_words=1, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=false } },

    {   .id="widecaps", .label="Wide Caps",
        .tagline="Letter-spaced thin caps · minimal brand",
        .category="Clean", .font="spacegrotesk",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.07f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::Fade, .pause_gap=0.45f, .max_words=4,
        .tracking=0.22f, .ts={ .shadow_enabled=false } },

    {   .id="kinetic", .label="Kinetic",
        .tagline="Words slide in from left · clean · modern",
        .category="Clean", .font="spacegrotesk",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.08f, .sub_pos=1,
        .style=AnimStyle::Slide, .pause_gap=0.30f, .max_words=5, .ease=EASE_OUT_CUBIC },

    {   .id="subtitleclean", .label="Subtitle",
        .tagline="Netflix-style bottom caption · clean · shadow",
        .category="Clean",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.05f, .sub_pos=0, .sub_pos_y=0.9f, .sub_wrap_w=0.9f,
        .style=AnimStyle::None, .pause_gap=0.7f, .max_words=10,
        .ts={ .shadow_enabled=true, .shadow_ox=2.f, .shadow_oy=2.f, .shadow_col={0.f,0.f,0.f,0.9f} } },

    {   .id="karaoke", .label="Karaoke",
        .tagline="Line at bottom · word-by-word highlight",
        .category="Karaoke",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.065f, .sub_pos=0, .sub_pos_y=0.88f,
        .color={0.7f, 0.7f, 0.7f, 1.f}, .karaoke=true, .style=AnimStyle::None,
        .pause_gap=0.50f, .max_words=8 },

    // ── Retro / Textured ─────────────────────────────────────────────────────

    {   .id="vhs", .label="VHS Lyrics",
        .tagline="CRT mono · bottom third · VHS grain · tracking",
        .category="Retro", .font="vt323",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.075f, .sub_pos=0, .sub_pos_y=0.88f,
        .style=AnimStyle::None, .pause_gap=0.60f, .max_words=7,
        .fx={ {FXType::VHS, 0.f} }, .n_fx=1, .text_case=2 },

    {   .id="neon", .label="Neon",
        .tagline="Glowing tube letters · hot pink · centered",
        .category="Retro", .font="monoton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.13f, .sub_pos=1,
        .color={1.f, 0.2f, 0.75f, 1.f}, .all_caps=true, .style=AnimStyle::Scale,
        .pause_gap=0.15f, .max_words=1,
        .fx={ {FXType::ChromaticAberration, 0.6f} }, .n_fx=1, .ease=EASE_OUT_BACK,
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=14.f, .glow_col={1.f,0.2f,0.8f,0.8f} } },

    {   .id="neonsign", .label="Neon Sign",
        .tagline="Cyan tube glow · flickers on · bar sign",
        .category="Retro", .font="monoton",
        .grouping=SubtitleMode::Phrase, .custom_n=2,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={0.3f, 0.95f, 1.f, 1.f}, .all_caps=true, .style=AnimStyle::Fade,
        .pause_gap=0.4f, .max_words=3,
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=14.f, .glow_col={0.3f,0.9f,1.f,0.8f} } },

    {   .id="vaporwave", .label="Vaporwave",
        .tagline="Wide mono · pink/cyan · drift · aesthetic",
        .category="Retro", .font="spacemono",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.09f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={1.f, 0.6f, 0.9f, 1.f}, .all_caps=true, .style=AnimStyle::Fade,
        .pause_gap=0.5f, .max_words=4, .tracking=0.12f,
        .ts={ .shadow_enabled=true, .shadow_ox=3.f, .shadow_oy=0.f, .shadow_col={0.3f,0.9f,1.f,0.7f} } },

    {   .id="groovy", .label="70s Groovy",
        .tagline="Warm retro script · gentle · funk/soul",
        .category="Retro", .font="lobster",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.12f, .sub_pos=1, .sub_wrap_w=0.8f,
        .color={1.f, 0.7f, 0.3f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.45f, .max_words=4, .ease=EASE_OUT_CUBIC,
        .ts={ .shadow_enabled=true, .shadow_ox=2.f, .shadow_oy=2.f, .shadow_col={0.4f,0.15f,0.f,0.6f} } },

    {   .id="disco", .label="Disco",
        .tagline="Rounded deco caps · bounce · party",
        .category="Retro", .font="righteous",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.15f, .sub_pos=1,
        .color={1.f, 0.85f, 0.3f, 1.f}, .all_caps=true, .style=AnimStyle::Bounce,
        .pause_gap=0.12f, .max_words=1,
        .ts={ .glow_enabled=true, .glow_r=8.f, .glow_col={1.f,0.85f,0.3f,0.5f} } },

    {   .id="cassette", .label="Cassette",
        .tagline="Mono caps · bracketed · mixtape J-card",
        .category="Retro", .font="spacemono",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.06f, .sub_pos=0, .sub_pos_y=0.88f, .sub_anchor_h=1,
        .all_caps=true, .style=AnimStyle::None, .pause_gap=0.6f, .max_words=8,
        .tracking=0.05f },

    {   .id="pixel", .label="Pixel",
        .tagline="Chunky 8-bit caps · hard cut · retro game",
        .category="Mono", .font="pressstart2p",
        .grouping=SubtitleMode::CustomN, .custom_n=2,
        .font_size=0.06f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::None, .pause_gap=0.2f, .max_words=3 },

    {   .id="terminal", .label="Terminal",
        .tagline="Green CRT mono · scanlines · hacker",
        .category="Mono", .font="vt323",
        .grouping=SubtitleMode::CustomN, .custom_n=4,
        .font_size=0.07f, .sub_pos_x=0.08f, .sub_pos=0, .sub_pos_y=0.85f, .sub_anchor_h=0, .sub_wrap_w=0.85f,
        .color={0.3f, 1.f, 0.4f, 1.f}, .style=AnimStyle::Typewriter,
        .pause_gap=0.3f, .max_words=6,
        .fx={ {FXType::Scanlines, 0.f} }, .n_fx=1,
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=6.f, .glow_col={0.3f,1.f,0.4f,0.5f} } },

    {   .id="lofi", .label="Lo-fi",
        .tagline="Small · warm · film grain · phrases · chill",
        .category="Retro", .font="spacemono",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.06f, .sub_pos=1, .sub_wrap_w=0.78f,
        .color={0.93f, 0.87f, 0.72f, 1.f}, .style=AnimStyle::Fade,
        .pause_gap=0.45f, .max_words=5,
        .fx={ {FXType::FilmGrain, 0.f} }, .n_fx=1, .text_case=2, .ts={ .shadow_enabled=false } },

    // ── Kinetic — per-word ───────────────────────────────────────────────────

    {   .id="wordpop", .label="Word Pop",
        .tagline="Each word pops to size as it's sung · modern lyric video",
        .category="Kinetic", .font="anton",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.35f, .max_words=5,
        .ease=EASE_OUT_BACK, .anim_unit=1, .anim_stagger=0.05f },

    {   .id="cascade", .label="Cascade",
        .tagline="Words drop in top-down · staggered · Spotify Canvas",
        .category="Kinetic", .font="archivoblack",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.85f,
        .style=AnimStyle::Stack, .pause_gap=0.35f, .max_words=5,
        .ease=EASE_OUT_CUBIC, .anim_unit=1, .anim_stagger=0.06f },

    {   .id="linebuild", .label="Line Build",
        .tagline="Word-by-word fade left to right · clean caption build",
        .category="Kinetic",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.08f, .sub_pos=1, .sub_wrap_w=0.8f,
        .style=AnimStyle::Fade, .pause_gap=0.35f, .max_words=6,
        .anim_unit=1, .anim_stagger=0.08f },

    {   .id="staggerslide", .label="Stagger Slide",
        .tagline="Each word slides in from the left · tech promo",
        .category="Kinetic", .font="spacegrotesk",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.085f, .sub_pos=1, .sub_wrap_w=0.85f,
        .style=AnimStyle::Slide, .pause_gap=0.35f, .max_words=5,
        .ease=EASE_OUT_CUBIC, .anim_unit=1, .anim_stagger=0.06f },

    {   .id="wordbounce", .label="Word Bounce",
        .tagline="Each word bounces in on a spring · upbeat pop",
        .category="Kinetic", .font="poppins",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.85f,
        .style=AnimStyle::Bounce, .pause_gap=0.35f, .max_words=5,
        .anim_unit=1, .anim_stagger=0.07f },

    {   .id="rise", .label="Rise",
        .tagline="Words rise and fade from below · slow · A24 trailer",
        .category="Kinetic", .font="instrumentserif",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.8f,
        .color={0.98f, 0.96f, 0.92f, 1.f}, .style=AnimStyle::Stack,
        .pause_gap=0.5f, .max_words=5, .ease=EASE_OUT_CUBIC,
        .anim_unit=1, .anim_stagger=0.1f, .ts={ .shadow_enabled=false } },

    {   .id="zoompunch", .label="Zoom Punch",
        .tagline="Words punch in oversized then settle · hype intro",
        .category="Kinetic", .font="archivoblack",
        .grouping=SubtitleMode::CustomN, .custom_n=2,
        .font_size=0.14f, .sub_pos=1, .sub_wrap_w=0.9f,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.25f, .max_words=2,
        .ease=EASE_OUT_BACK, .anim_unit=1, .anim_stagger=0.04f,
        .ts={ .shadow_enabled=true, .shadow_ox=3.f, .shadow_oy=3.f, .shadow_col={0.f,0.f,0.f,0.8f} } },

    {   .id="slamstack", .label="Slam Stack",
        .tagline="Words stack and each slams in · rap multi-line",
        .category="Kinetic", .font="anton",
        .grouping=SubtitleMode::CustomN, .custom_n=3,
        .font_size=0.11f, .sub_pos_x=0.06f, .sub_pos=1, .sub_anchor_h=0, .sub_wrap_w=0.5f,
        .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.3f, .max_words=3,
        .ease=EASE_OUT_BACK, .anim_unit=1, .anim_stagger=0.08f },

    // ── Kinetic — per-letter / anomalies ─────────────────────────────────────

    {   .id="truetype", .label="True Type",
        .tagline="Real char-by-char reveal · terminal · green",
        .category="Kinetic", .font="vt323",
        .grouping=SubtitleMode::CustomN, .custom_n=5,
        .font_size=0.08f, .sub_pos_x=0.08f, .sub_pos=0, .sub_pos_y=0.85f, .sub_anchor_h=0, .sub_wrap_w=0.85f,
        .color={0.3f, 1.f, 0.4f, 1.f}, .style=AnimStyle::Fade, .pause_gap=0.4f, .max_words=8,
        .anim_unit=2, .anim_stagger=0.035f, .ts={ .shadow_enabled=false } },

    {   .id="wave", .label="Wave",
        .tagline="Letters ride a continuous sine · playful",
        .category="Kinetic", .font="poppins",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.85f,
        .style=AnimStyle::WaveText, .pause_gap=0.4f, .max_words=4,
        .anim_unit=2, .anim_stagger=0.f },

    {   .id="lettercascade", .label="Letter Cascade",
        .tagline="Letters fall in one by one · kinetic type reel",
        .category="Kinetic", .font="montserrat",
        .grouping=SubtitleMode::CustomN, .custom_n=2,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.9f,
        .all_caps=true, .style=AnimStyle::Stack, .pause_gap=0.3f, .max_words=2,
        .ease=EASE_OUT_CUBIC, .anim_unit=2, .anim_stagger=0.03f },

    {   .id="explode", .label="Explode In",
        .tagline="Letters fly in from everywhere to place · impact",
        .category="Kinetic", .font="anton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.15f, .sub_pos=1,
        .all_caps=true, .style=AnimStyle::Explode, .pause_gap=0.2f, .max_words=1,
        .anim_unit=2, .anim_stagger=0.015f },

    {   .id="gravity", .label="Gravity",
        .tagline="Letters drop from above and bounce to the baseline",
        .category="Kinetic", .font="bebasneue",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.17f, .sub_pos=1,
        .all_caps=true, .style=AnimStyle::Gravity, .pause_gap=0.2f, .max_words=1,
        .anim_unit=2, .anim_stagger=0.04f },

    {   .id="elastic", .label="Elastic",
        .tagline="Letters overshoot and wobble into place · springy",
        .category="Kinetic", .font="sora",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.14f, .sub_pos=1,
        .color={0.7f, 1.f, 0.85f, 1.f}, .style=AnimStyle::Scale, .pause_gap=0.2f, .max_words=1,
        .ease=EASE_OUT_ELASTIC, .anim_unit=2, .anim_stagger=0.035f },

    {   .id="typeglow", .label="Typewriter Glow",
        .tagline="Char reveal · glow on the newest letter · retro computer",
        .category="Kinetic", .font="spacemono",
        .grouping=SubtitleMode::CustomN, .custom_n=4,
        .font_size=0.075f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={1.f, 0.8f, 0.4f, 1.f}, .style=AnimStyle::Fade, .pause_gap=0.4f, .max_words=6,
        .anim_unit=2, .anim_stagger=0.04f,
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=6.f, .glow_col={1.f,0.7f,0.3f,0.5f} } },

    {   .id="jitter", .label="Jitter",
        .tagline="Every letter micro-vibrates · anxious energy",
        .category="Kinetic", .font="majormono",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.09f, .sub_pos=1, .sub_wrap_w=0.85f,
        .style=AnimStyle::Jitter, .pause_gap=0.35f, .max_words=4,
        .anim_unit=2, .anim_stagger=0.f },

    {   .id="liquid", .label="Liquid",
        .tagline="Letters wobble like they're underwater · dreamy",
        .category="Kinetic", .font="poppins",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={0.6f, 0.95f, 1.f, 1.f}, .style=AnimStyle::WaveText, .pause_gap=0.4f, .max_words=4,
        .anim_unit=2, .anim_stagger=0.f, .ts={ .shadow_enabled=false } },

    // ── Karaoke evolved ──────────────────────────────────────────────────────

    {   .id="karaoke_fill", .label="Fill Wipe",
        .tagline="Highlight sweeps across each word as it's sung · classic karaoke",
        .category="Karaoke", .font="anton",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.075f, .sub_pos=0, .sub_pos_y=0.86f, .sub_wrap_w=0.9f,
        .color={0.55f, 0.55f, 0.58f, 1.f}, .all_caps=true, .karaoke=true, .style=AnimStyle::None,
        .pause_gap=0.5f, .max_words=8, .karaoke_mode=1 },

    {   .id="karaoke_pop", .label="Pop Highlight",
        .tagline="Active word scales up + brightens · modern karaoke",
        .category="Karaoke", .font="montserrat",
        .grouping=SubtitleMode::Line, .custom_n=3,
        .font_size=0.07f, .sub_pos=0, .sub_pos_y=0.86f, .sub_wrap_w=0.9f,
        .color={0.6f, 0.6f, 0.62f, 1.f}, .karaoke=true, .style=AnimStyle::None,
        .pause_gap=0.5f, .max_words=8, .karaoke_mode=2 },

    {   .id="neon_trace", .label="Neon Trace",
        .tagline="Active word's tube lights up as you sing · neon sign",
        .category="Karaoke", .font="monoton",
        .grouping=SubtitleMode::Line, .custom_n=2,
        .font_size=0.085f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={0.4f, 0.2f, 0.4f, 1.f}, .all_caps=true, .karaoke=true, .style=AnimStyle::None,
        .pause_gap=0.5f, .max_words=5, .karaoke_mode=1,
        .karaoke_highlight_color={1.f, 0.3f, 0.85f, 1.f},
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=10.f, .glow_col={1.f,0.3f,0.85f,0.5f} } },

    // ── Gradient / chromatic ─────────────────────────────────────────────────

    {   .id="sunset", .label="Sunset",
        .tagline="Warm orange-to-pink gradient fill · slow · warm pop",
        .category="Gradient", .font="poppins",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={1.f, 0.75f, 0.25f, 1.f}, .style=AnimStyle::Fade, .pause_gap=0.4f, .max_words=4,
        .ease=EASE_OUT_CUBIC, .grad_mode=1, .grad_col2={1.f, 0.25f, 0.55f, 1.f},
        .ts={ .shadow_enabled=false } },

    {   .id="holographic", .label="Holographic",
        .tagline="Animated iridescent hue shift · Y2K foil",
        .category="Gradient", .font="spacegrotesk",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.11f, .sub_pos=1, .sub_wrap_w=0.85f,
        .all_caps=true, .style=AnimStyle::Fade, .pause_gap=0.4f, .max_words=4,
        .tracking=0.04f, .grad_mode=3, .ts={ .shadow_enabled=false } },

    {   .id="duotone", .label="Duotone",
        .tagline="Two-colour diagonal split fill · poster",
        .category="Gradient", .font="anton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.18f, .sub_pos=1,
        .color={1.f, 0.3f, 0.6f, 1.f}, .all_caps=true, .style=AnimStyle::Scale, .pause_gap=0.15f, .max_words=1,
        .ease=EASE_OUT_BACK, .grad_mode=2, .grad_col2={0.3f, 0.5f, 1.f, 1.f},
        .ts={ .shadow_enabled=false } },

    {   .id="goldfoil", .label="Gold Foil",
        .tagline="Metallic gold gradient + soft glow · luxury / awards",
        .category="Gradient", .font="cinzel",
        .grouping=SubtitleMode::Phrase, .custom_n=3,
        .font_size=0.10f, .sub_pos=1, .sub_wrap_w=0.85f,
        .color={1.f, 0.92f, 0.55f, 1.f}, .all_caps=true, .style=AnimStyle::Fade,
        .pause_gap=0.5f, .max_words=4, .ease=EASE_OUT_CUBIC, .tracking=0.05f,
        .grad_mode=1, .grad_col2={0.7f, 0.5f, 0.15f, 1.f},
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=8.f, .glow_col={1.f,0.8f,0.35f,0.4f} } },

    {   .id="chrome80s", .label="Chrome 80s",
        .tagline="Silver-to-blue chrome gradient · synthwave",
        .category="Gradient", .font="bebasneue",
        .grouping=SubtitleMode::CustomN, .custom_n=2,
        .font_size=0.16f, .sub_pos=1, .sub_wrap_w=0.9f,
        .color={0.95f, 0.95f, 1.f, 1.f}, .all_caps=true, .style=AnimStyle::Scale,
        .pause_gap=0.2f, .max_words=2, .ease=EASE_OUT_BACK,
        .grad_mode=1, .grad_col2={0.3f, 0.45f, 0.9f, 1.f},
        .ts={ .stroke_enabled=true, .stroke_w=2.f, .stroke_col={0.1f,0.1f,0.3f,1.f} } },

    {   .id="ember", .label="Ember",
        .tagline="Fill glows orange-to-red like fire · intense",
        .category="Gradient", .font="anton",
        .grouping=SubtitleMode::Word, .custom_n=1,
        .font_size=0.17f, .sub_pos=1,
        .color={1.f, 0.85f, 0.3f, 1.f}, .all_caps=true, .style=AnimStyle::Scale,
        .pause_gap=0.15f, .max_words=1, .ease=EASE_OUT_BACK,
        .grad_mode=1, .grad_col2={0.85f, 0.12f, 0.05f, 1.f},
        .ts={ .shadow_enabled=false, .glow_enabled=true, .glow_r=10.f, .glow_col={1.f,0.4f,0.1f,0.5f} } },
};

static const int g_n_typo_presets = (int)(sizeof(g_typo_presets) / sizeof(g_typo_presets[0]));

inline const TypographyPreset* typo_preset_by_id(const char* id) {
    for (int i = 0; i < g_n_typo_presets; ++i)
        if (strcmp(g_typo_presets[i].id, id) == 0) return &g_typo_presets[i];
    return nullptr;
}
