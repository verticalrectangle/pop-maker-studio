import type { App } from "../core/app";
import { transcribe } from "../ml/transcribe";
import { analyzeBeats } from "../ml/beats";
import { layLyrics } from "./lyrics";

export type PipelineStage = "idle" | "beats" | "transcribe" | "lyrics" | "done" | "error";

/**
 * The ML pipeline: beat analysis + word transcription of the project's master
 * audio, then automatic lyric brick layout. Mirrors the desktop's
 * trigger_pipeline: re-running reuses entry.transcript (cached on the
 * MediaEntry) and just re-lays the lyrics.
 */
export async function runPipeline(
  app: App,
  presetName: string,
  onStage?: (stage: PipelineStage, detail: string, progress: number) => void,
): Promise<void> {
  const entry = app.project.audioSource
    ? app.media.get(app.project.audioSource)
    : app.media.list().find((m) => m.hasAudio);
  if (!entry) throw new Error("No audio source — import audio or video first");

  try {
    if (!entry.beats) {
      onStage?.("beats", "Analyzing beats…", 0);
      const { bpm, beats, rms } = await analyzeBeats(app, entry);
      entry.bpm = bpm;
      entry.beats = beats;
      entry.rms = rms;
      app.project.bpm = bpm;
      app.project.beats = beats;
    }

    if (!entry.transcript) {
      onStage?.("transcribe", "Transcribing…", 0);
      entry.transcript = await transcribe(app, entry, (msg, p) => onStage?.("transcribe", msg, p));
    }

    onStage?.("lyrics", "Laying lyrics…", 1);
    // Offset: transcript is source-relative; the audio clip's timeline start
    // minus its in_point maps source time → timeline time.
    const audioClip = app.project.tracks
      .flatMap((t) => t.clips)
      .find((c) => c.source === entry.id);
    const offset = audioClip ? audioClip.start - audioClip.inPoint : 0;
    layLyrics(app, entry.transcript, presetName, offset);
    onStage?.("done", "Done", 1);
    app.events.emit("toast", "Lyrics ready");
  } catch (err) {
    onStage?.("error", err instanceof Error ? err.message : String(err), 0);
    throw err;
  }
}
