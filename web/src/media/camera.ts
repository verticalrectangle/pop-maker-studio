// Camera brick support: live preview via getUserMedia and take recording via
// MediaRecorder (vp9/webm). A recorded take is wrapped in a File, registered in
// the MediaStore as a video entry, and appended to the camera clip's `takes`
// list through a single undoable mutation.
import { findClip } from "../core/project";
import type { App } from "../core/app";

let currentStream: MediaStream | null = null;
let currentRecorder: MediaRecorder | null = null;
let currentChunks: Blob[] = [];
let currentTakeResolve: ((take: string | null) => void) | null = null;

/** Preferred mime for recorded takes; vp9/webm has the widest browser support. */
function recorderMime(): string {
  const candidates = [
    "video/webm;codecs=vp9,opus",
    "video/webm;codecs=vp8,opus",
    "video/webm",
  ];
  for (const c of candidates) {
    if (typeof MediaRecorder !== "undefined" && MediaRecorder.isTypeSupported(c)) return c;
  }
  return "video/webm";
}

/** Start the live camera preview and return the resulting MediaStream. */
export async function startCameraPreview(_app: App): Promise<MediaStream> {
  stopCameraPreview();
  const stream = await navigator.mediaDevices.getUserMedia({
    video: { width: { ideal: 1280 }, height: { ideal: 720 } },
    audio: true,
  });
  currentStream = stream;
  return stream;
}

/** Stop the live camera preview (and any in-flight recording). */
export function stopCameraPreview(): void {
  if (currentRecorder && currentRecorder.state !== "inactive") {
    try {
      currentRecorder.stop();
    } catch {
      /* ignore */
    }
  }
  currentRecorder = null;
  currentChunks = [];
  if (currentStream) {
    for (const track of currentStream.getTracks()) {
      try {
        track.stop();
      } catch {
        /* ignore */
      }
    }
    currentStream = null;
  }
}

/**
 * Begin recording a take from `stream`. Resolves with the new media entry id
 * once recording is finalized (via stopCameraPreview or a stream end). Returns
 * `null` if recording could not be started. The take is appended to the camera
 * clip's `takes` array as one undo step.
 */
export async function recordTake(app: App, clipId: number, stream: MediaStream): Promise<void> {
  if (typeof MediaRecorder === "undefined") return;
  const mime = recorderMime();
  let recorder: MediaRecorder;
  try {
    recorder = new MediaRecorder(stream, { mimeType: mime });
  } catch {
    return;
  }

  currentRecorder = recorder;
  currentChunks = [];

  const takeId = await new Promise<string | null>((resolve) => {
    currentTakeResolve = resolve;
    recorder.ondataavailable = (e) => {
      if (e.data && e.data.size > 0) currentChunks.push(e.data);
    };
    recorder.onstop = () => {
      const chunks = currentChunks;
      const resolveFn = currentTakeResolve;
      currentChunks = [];
      currentTakeResolve = null;
      if (chunks.length === 0) {
        resolveFn?.(null);
        return;
      }
      const blob = new Blob(chunks, { type: mime });
      const file = new File([blob], `take-${Date.now()}.webm`, { type: mime });
      const entry = app.media.add({
        file,
        kind: "video",
        duration: 0, // duration filled in lazily by the bin/decoder on demand
        width: 0,
        height: 0,
        fps: 0,
        hasAudio: true,
        hasVideo: true,
      });
      app.events.emit("media:added", entry);
      resolveFn?.(entry.id);
    };
    recorder.onerror = () => {
      currentTakeResolve?.(null);
      currentTakeResolve = null;
    };
    try {
      recorder.start();
    } catch {
      currentTakeResolve = null;
      resolve(null);
    }
  });

  if (takeId) appendTake(app, clipId, takeId);
}

/** Append a recorded take id to the camera clip's `takes` list (one undo step). */
function appendTake(app: App, clipId: number, takeId: string): void {
  app.mutate("Record take", () => {
    const found = findClip(app.project, clipId);
    if (!found) return;
    const clip = found.clip;
    clip.takes = [...(clip.takes ?? []), takeId];
  });
}
