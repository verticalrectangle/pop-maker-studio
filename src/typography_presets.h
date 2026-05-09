#pragma once
#include "app.h"
#include <cstring>

// ── FX clip descriptor ────────────────────────────────────────────────────────
// Describes an FX clip the typography generator creates above the lyrics track.
struct TypoFXDesc {
    FXType type          = FXType::Glitch;
    float  beat_intensity = 0.f;  // if > 0, first beat-syncable param gets this intensity
};

// ── Typography preset ─────────────────────────────────────────────────────────
struct TypographyPreset {
    const char* id;
    const char* label;
    const char* tagline;
    const char* category; // "Hype", "Aesthetic", "Editorial", "Clean", "Retro"

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
    bool  all_caps     = false;
    bool  karaoke      = false;
    AnimStyle style    = AnimStyle::None;

    // FX clips to auto-generate (max 3)
    TypoFXDesc fx[3]   = {};
    int        n_fx    = 0;
};

// ── Preset catalogue ──────────────────────────────────────────────────────────
static const TypographyPreset g_typo_presets[] = {

    // ── Hype / Club ──────────────────────────────────────────────────────────

    {   "flash",
        "Flash",
        "One word · full-frame · cuts hard · pure energy",
        "Hype",
        SubtitleMode::Word, 1,
        0.18f, 0.5f, 1, 0.5f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        true, false, AnimStyle::Block,
        {}, 0
    },

    {   "strobe",
        "Strobe",
        "Flash but inverts every word · ultra aggressive",
        "Hype",
        SubtitleMode::Word, 1,
        0.18f, 0.5f, 1, 0.5f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        true, false, AnimStyle::Block,
        {}, 0
    },

    {   "rave",
        "Rave",
        "Random positions · beat-reactive · neon chaos",
        "Hype",
        SubtitleMode::Word, 1,
        0.14f, 0.5f, 3, 0.5f, 1, 0.85f,
        {1.f, 0.1f, 0.9f, 1.f},
        true, false, AnimStyle::Scale,
        { {FXType::ChromaticAberration, 0.8f} }, 1
    },

    {   "cyberpunk",
        "Cyberpunk",
        "Monospace · cyan · glitch · bottom-left",
        "Hype",
        SubtitleMode::Word, 1,
        0.12f, 0.08f, 0, 0.88f, 0, 0.9f,
        {0.0f, 1.f, 0.95f, 1.f},
        true, false, AnimStyle::Glitch,
        { {FXType::ChromaticAberration, 0.7f}, {FXType::Scanlines, 0.f} }, 2
    },

    {   "drill",
        "Drill",
        "3 words · top third · yellow on black · aggressive",
        "Hype",
        SubtitleMode::CustomN, 3,
        0.10f, 0.5f, 2, 0.15f, 1, 0.85f,
        {1.f, 0.95f, 0.0f, 1.f},
        true, false, AnimStyle::Block,
        {}, 0
    },

    // ── Aesthetic / Indie ────────────────────────────────────────────────────

    {   "tumblr",
        "Tumblr",
        "Helvetica energy · lowercase · centered · clean",
        "Aesthetic",
        SubtitleMode::Phrase, 3,
        0.08f, 0.5f, 1, 0.5f, 1, 0.80f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "indie2012",
        "Indie 2012",
        "Small · muted · left offset · minimal",
        "Aesthetic",
        SubtitleMode::Line, 3,
        0.06f, 0.12f, 1, 0.5f, 0, 0.75f,
        {0.85f, 0.82f, 0.78f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "sadgirl",
        "Sad Girl",
        "Large single word · pastel · centered · slow",
        "Aesthetic",
        SubtitleMode::Word, 1,
        0.15f, 0.5f, 1, 0.5f, 1, 0.85f,
        {0.95f, 0.75f, 0.88f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "cottagecore",
        "Cottagecore",
        "Warm cream · centered · line grouping · gentle",
        "Aesthetic",
        SubtitleMode::Line, 3,
        0.07f, 0.5f, 1, 0.5f, 1, 0.80f,
        {0.96f, 0.91f, 0.78f, 1.f},
        false, false, AnimStyle::Fade,
        { {FXType::FilmGrain, 0.f} }, 1
    },

    {   "film",
        "Film",
        "Bottom third · sentence · classic subtitle",
        "Aesthetic",
        SubtitleMode::Line, 3,
        0.055f, 0.5f, 0, 0.88f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    // ── Editorial / Bold ─────────────────────────────────────────────────────

    {   "headline",
        "Headline",
        "Massive single word · fills frame · high contrast",
        "Editorial",
        SubtitleMode::Word, 1,
        0.20f, 0.5f, 1, 0.5f, 1, 0.92f,
        {1.f, 1.f, 1.f, 1.f},
        true, false, AnimStyle::Scale,
        {}, 0
    },

    {   "manifesto",
        "Manifesto",
        "Full left flush · stacked · uppercase · maximum",
        "Editorial",
        SubtitleMode::Line, 3,
        0.11f, 0.05f, 1, 0.5f, 0, 0.92f,
        {1.f, 1.f, 1.f, 1.f},
        true, false, AnimStyle::Block,
        {}, 0
    },

    {   "zine",
        "Zine",
        "Mixed case · phrases · lo-fi rotated energy",
        "Editorial",
        SubtitleMode::Phrase, 3,
        0.09f, 0.5f, 1, 0.5f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Glitch,
        {}, 0
    },

    {   "newspaper",
        "Newspaper",
        "Tight centered block · sentences · serif energy",
        "Editorial",
        SubtitleMode::Segment, 3,
        0.07f, 0.5f, 1, 0.5f, 1, 0.75f,
        {1.f, 1.f, 1.f, 1.f},
        true, false, AnimStyle::Block,
        {}, 0
    },

    // ── Clean / Modern ───────────────────────────────────────────────────────

    {   "minimal",
        "Minimal",
        "Small · centered · breathing room · muted",
        "Clean",
        SubtitleMode::Phrase, 3,
        0.055f, 0.5f, 1, 0.5f, 1, 0.75f,
        {0.9f, 0.9f, 0.9f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "spotify",
        "Spotify",
        "Bottom center · clean sans · phrase · white",
        "Clean",
        SubtitleMode::Phrase, 3,
        0.07f, 0.5f, 0, 0.85f, 1, 0.82f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "apple",
        "Apple",
        "Single word · massive · fades · pristine white",
        "Clean",
        SubtitleMode::Word, 1,
        0.16f, 0.5f, 1, 0.5f, 1, 0.88f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Fade,
        {}, 0
    },

    {   "kinetic",
        "Kinetic",
        "Words slide in from left · clean · modern",
        "Clean",
        SubtitleMode::Phrase, 3,
        0.08f, 0.5f, 1, 0.5f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::Slide,
        {}, 0
    },

    {   "karaoke",
        "Karaoke",
        "Line at bottom · word-by-word highlight",
        "Clean",
        SubtitleMode::Line, 3,
        0.065f, 0.5f, 0, 0.88f, 1, 0.85f,
        {0.7f, 0.7f, 0.7f, 1.f},
        false, true, AnimStyle::None,
        {}, 0
    },

    // ── Retro / Textured ─────────────────────────────────────────────────────

    {   "vhs",
        "VHS Lyrics",
        "Bottom third · white · VHS grain · tracking",
        "Retro",
        SubtitleMode::Line, 3,
        0.065f, 0.5f, 0, 0.88f, 1, 0.85f,
        {1.f, 1.f, 1.f, 1.f},
        false, false, AnimStyle::None,
        { {FXType::VHS, 0.f} }, 1
    },

    {   "neon",
        "Neon",
        "Glow · hot pink · beat-reactive · centered",
        "Retro",
        SubtitleMode::Word, 1,
        0.14f, 0.5f, 1, 0.5f, 1, 0.85f,
        {1.f, 0.05f, 0.7f, 1.f},
        true, false, AnimStyle::Scale,
        { {FXType::ChromaticAberration, 0.6f} }, 1
    },

    {   "lofi",
        "Lo-fi",
        "Small · warm · film grain · phrases · chill",
        "Retro",
        SubtitleMode::Phrase, 3,
        0.06f, 0.5f, 1, 0.5f, 1, 0.78f,
        {0.93f, 0.87f, 0.72f, 1.f},
        false, false, AnimStyle::Fade,
        { {FXType::FilmGrain, 0.f} }, 1
    },
};

static const int g_n_typo_presets = (int)(sizeof(g_typo_presets) / sizeof(g_typo_presets[0]));

inline const TypographyPreset* typo_preset_by_id(const char* id) {
    for (int i = 0; i < g_n_typo_presets; ++i)
        if (strcmp(g_typo_presets[i].id, id) == 0) return &g_typo_presets[i];
    return nullptr;
}
