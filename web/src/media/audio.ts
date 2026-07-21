// WebAudio playback engine. Keeps a hidden <audio> element per active clip
// routed through a per-clip gain + stereo panner into the master gain, and
// re-syncs element `currentTime` to the playhead every frame so lipsync stays
// within ~50ms while playing and seeking.
//
// `sync(playhead, playing)` is called every animation frame by the main loop.
// It creates voices for clips entering the playhead window, stops voices for
import { propAt, type Clip, type Track } from "../core/project";
import type { App } from "../core/app";
import type { MediaEntry } from "./store";

const DRIFT_THRESHOLD = 0.05; // seconds; re-seek beyond this

interface Voice {
  clipId: number;
  el: HTMLAudioElement;
  source: MediaElementAudioSourceNode;
  gain: GainNode;
  pan: StereoPannerNode;
  objectUrl: string;
}

/** Resolve the media entry whose audio a clip plays (camera bricks -> last take). */
function clipAudioEntry(app: App, clip: Clip): MediaEntry | undefined {
  if (clip.type === "camera") {
    const takes = clip.takes;
    if (takes && takes.length > 0) return app.media.get(takes[takes.length - 1]!);
    return undefined;
  }
  if (clip.source) return app.media.get(clip.source);
  return undefined;
}

/** A clip is audible if it has a decodable audio source, is in range, and has volume. */
function isAudible(app: App, track: Track, clip: Clip, playhead: number): boolean {
  if (track.muted) return false;
  if (playhead < clip.start || playhead >= clip.end) return false;
  if (clip.type !== "audio" && clip.type !== "video" && clip.type !== "camera") return false;
  const entry = clipAudioEntry(app, clip);
  if (!entry || !entry.hasAudio) return false;
  const vol = propAt(clip, "volume", playhead - clip.start);
  return vol > 0;
}

export class AudioEngine {
  private ctx: AudioContext;
  private master: GainNode;
  private voices = new Map<number, Voice>();
  private objectUrls = new Map<string, string>();

  constructor(private app: App) {
    this.ctx = new AudioContext();
    this.master = this.ctx.createGain();
    this.master.gain.value = 1;
    this.master.connect(this.ctx.destination);
  }

  /** Reconcile active voices with the current playhead. Called every frame. */
  sync(playhead: number, playing: boolean): void {
    if (playing && this.ctx.state === "suspended") void this.ctx.resume();

    const desired = new Set<number>();
    for (const track of this.app.project.tracks) {
      for (const clip of track.clips) {
        if (!isAudible(this.app, track, clip, playhead)) continue;
        desired.add(clip.id);
        this.syncVoice(clip, playhead, playing);
      }
    }

    // Stop voices for clips that have left the window.
    for (const id of this.voices.keys()) {
      if (!desired.has(id)) this.stopVoice(id);
    }
  }

  private syncVoice(clip: Clip, playhead: number, playing: boolean): void {
    let voice: Voice | null | undefined = this.voices.get(clip.id);
    const entry = clipAudioEntry(this.app, clip);
    if (!entry) return;

    if (!voice) {
      voice = this.createVoice(clip.id, entry);
      if (!voice) return;
      this.voices.set(clip.id, voice);
    }

    const localT = playhead - clip.start;
    const expected = clip.inPoint + localT * clip.speed;
    const vol = propAt(clip, "volume", localT);
    const panVal = propAt(clip, "pan", localT);

    // Drift correction: re-seek when the element has drifted past the threshold.
    if (Math.abs(voice.el.currentTime - expected) > DRIFT_THRESHOLD) {
      try {
        voice.el.currentTime = Math.max(0, expected);
      } catch {
        /* element not seekable yet */
      }
    }

    // Playback state follows the transport.
    if (playing) {
      if (voice.el.paused) {
        void voice.el.play().catch(() => {
          /* autoplay rejection; will retry next frame once ctx resumes */
        });
      }
    } else if (!voice.el.paused) {
      voice.el.pause();
    }

    voice.gain.gain.value = vol;
    voice.pan.pan.value = Math.max(-1, Math.min(1, panVal));
  }

  private createVoice(clipId: number, entry: MediaEntry): Voice | null {
    let url = this.objectUrls.get(entry.id);
    if (!url) {
      url = URL.createObjectURL(entry.file);
      this.objectUrls.set(entry.id, url);
    }

    const el = document.createElement("audio");
    el.src = url;
    el.preload = "auto";
    el.muted = false;
    el.style.display = "none";
    document.body.append(el);

    let source: MediaElementAudioSourceNode;
    try {
      source = this.ctx.createMediaElementSource(el);
    } catch {
      // A MediaElementSource can only be created once per element; if the
      // element was reused this throws. Drop the element and bail.
      el.remove();
      return null;
    }
    const gain = this.ctx.createGain();
    const pan = this.ctx.createStereoPanner();
    source.connect(gain).connect(pan).connect(this.master);
    return { clipId, el, source, gain, pan, objectUrl: url };
  }

  private stopVoice(clipId: number): void {
    const voice = this.voices.get(clipId);
    if (!voice) return;
    try {
      voice.el.pause();
    } catch {
      /* ignore */
    }
    try {
      voice.source.disconnect();
    } catch {
      /* ignore */
    }
    try {
      voice.gain.disconnect();
      voice.pan.disconnect();
    } catch {
      /* ignore */
    }
    voice.el.remove();
    this.voices.delete(clipId);
  }

  dispose(): void {
    for (const id of this.voices.keys()) this.stopVoice(id);
    for (const url of this.objectUrls.values()) URL.revokeObjectURL(url);
    this.objectUrls.clear();
    void this.ctx.close();
  }
}
