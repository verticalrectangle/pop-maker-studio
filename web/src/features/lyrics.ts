import type { App } from "../core/app";
import {
  addTrack, insertClip, makeClip, type Clip, type TextStyle,
} from "../core/project";
import type { Word } from "../media/store";

export interface TypographyPreset {
  name: string;
  style: TextStyle;
  /** Max words per lyric brick. */
  wordsPerBrick: number;
  /** Pause (s) that forces a phrase break. */
  pauseBreak: number;
}

export const PRESETS: Record<string, TypographyPreset> = {
  karaoke: {
    name: "Karaoke Pop",
    wordsPerBrick: 4,
    pauseBreak: 0.6,
    style: {
      fontFamily: "Inter, system-ui, sans-serif", fontSize: 0.09, color: [1, 1, 1, 1], bold: true, italic: false,
      align: "center", strokeColor: [0, 0, 0, 0.9], strokeWidth: 0.008, letterSpacing: 0.02,
      uppercase: true,
    },
  },
  minimal: {
    name: "Minimal",
    wordsPerBrick: 7,
    pauseBreak: 0.8,
    style: {
      fontFamily: "Inter, system-ui, sans-serif", fontSize: 0.055, color: [1, 1, 1, 0.95], bold: false, italic: false,
      align: "center", strokeWidth: 0, letterSpacing: 0.01, uppercase: false,
    },
  },
  punch: {
    name: "Punch",
    wordsPerBrick: 2,
    pauseBreak: 0.4,
    style: {
      fontFamily: "Inter, system-ui, sans-serif", fontSize: 0.14, color: [1, 0.85, 0.2, 1], bold: true, italic: false,
      align: "center", strokeColor: [0, 0, 0, 1], strokeWidth: 0.012, letterSpacing: 0,
      uppercase: true,
      shadow: { dx: 0.004, dy: 0.006, blur: 0.01, color: [0, 0, 0, 0.6] },
    },
  },
};

const LYRICS_TRACK = "Lyrics";

/**
 * Lay lyric bricks on the managed Lyrics track from a word transcript.
 * Timestamps in `words` are source-file seconds; `offset` shifts them to the
 * timeline (audio clip start minus its in_point). Idempotent: clears the
 * track's lyric clips first.
 */
export function layLyrics(
  app: App,
  words: Word[],
  presetName: string,
  offset: number,
): void {
  const preset = PRESETS[presetName] ?? PRESETS["karaoke"]!;
  app.mutate(`Lyrics: ${preset.name}`, () => {
    let track = app.project.tracks.find((t) => t.name === LYRICS_TRACK);
    if (!track) track = addTrack(app.project, LYRICS_TRACK, 0);
    track.clips = track.clips.filter((c) => c.type !== "lyric");

    for (const phrase of groupPhrases(words, preset)) {
      const clip: Clip = makeClip("lyric", phrase.start + offset, phrase.end + offset);
      clip.text = phrase.text;
      clip.textStyle = { ...preset.style };
      clip.props["pos_x"] = 0.5;
      clip.props["pos_y"] = presetName === "minimal" ? 0.85 : 0.5;
      insertClip(track!, clip);
    }
  });
}

interface Phrase { text: string; start: number; end: number }

function groupPhrases(words: Word[], preset: TypographyPreset): Phrase[] {
  const out: Phrase[] = [];
  let cur: Word[] = [];
  const flush = (): void => {
    if (cur.length === 0) return;
    out.push({
      text: cur.map((w) => w.word).join(" "),
      start: cur[0]!.start,
      end: cur[cur.length - 1]!.end,
    });
    cur = [];
  };
  for (let i = 0; i < words.length; i++) {
    const w = words[i]!;
    const prev = cur[cur.length - 1];
    if (prev && (w.start - prev.end > preset.pauseBreak || cur.length >= preset.wordsPerBrick)) flush();
    cur.push(w);
  }
  flush();
  return out;
}

/** Remove lyric bricks (e.g. before re-laying with a different preset). */
export function clearLyrics(app: App): void {
  app.mutate("Clear lyrics", () => {
    const track = app.project.tracks.find((t) => t.name === LYRICS_TRACK);
    if (track) track.clips = track.clips.filter((c) => c.type !== "lyric");
  });
}
