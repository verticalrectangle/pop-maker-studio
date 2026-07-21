import { sampleKeyTrack, type KeyTrack, type Interp } from "./keyframes";

/** Clip kinds. fx/bodyfx bricks composite over tracks below; the rest are content. */
export type ClipType =
  | "video" | "audio" | "image" | "text" | "shape"
  | "fx" | "bodyfx" | "camera" | "lyric";

export interface FxSubEffect {
  fxType: string;
  bodyFxType?: string;
  /** Seconds relative to brick start. */
  relStart: number;
  /** 0 = until brick end. */
  relEnd: number;
  params: Record<string, number>;
}

export interface TextStyle {
  fontFamily: string;
  fontSize: number; // fraction of canvas height
  color: [number, number, number, number];
  bgColor?: [number, number, number, number];
  bold: boolean;
  italic: boolean;
  align: "left" | "center" | "right";
  strokeColor?: [number, number, number, number];
  strokeWidth: number; // fraction canvas height
  shadow?: { dx: number; dy: number; blur: number; color: [number, number, number, number] };
  letterSpacing: number;
  uppercase: boolean;
}

export interface ShapePoint { x: number; y: number; w?: number }
export interface ShapeData {
  preset: string;
  params: number[];
  path?: ShapePoint[]; // custom path in local [0,1]^2
  closed: boolean;
  strokeWidth: number;
  fill?: [number, number, number, number];
  stroke?: [number, number, number, number];
  glowRadius: number;
  pathKeys?: { t: number; path: ShapePoint[]; interp: Interp }[];
}

export interface Clip {
  id: number;
  type: ClipType;
  /** Timeline seconds. */
  start: number;
  end: number;
  /** Source-file offset (seconds) where playback begins. */
  inPoint: number;
  speed: number;
  /** MediaStore source id (video/audio/image/camera sources). */
  source?: string;
  text?: string;
  /** Static scalar props; keyframes override per-frame. */
  props: Record<string, number>;
  keyframes: Record<string, KeyTrack>;
  /** Single FX brick params. */
  fx?: { fxType: string; amount: number; params: Record<string, number> };
  /** Ordered multi-FX chain (mutually exclusive with fx). */
  fxChain?: FxSubEffect[];
  /** Host content clip id when a multi-FX brick is coupled/welded. */
  coupledTo?: number;
  textStyle?: TextStyle;
  shape?: ShapeData;
  /** bodyfx: which segmentation mask + status. */
  maskStatus?: "none" | "processing" | "ready";
  /** camera brick: recorded take media ids; last is selected. */
  takes?: string[];
}

export interface Track {
  id: number;
  name: string;
  muted: boolean;
  locked: boolean;
  clips: Clip[]; // kept sorted by start
}

export interface Marker {
  time: number;
  label: string;
  color: string;
}

export interface Project {
  duration: number;
  fps: number;
  width: number;
  height: number;
  bpm: number | null;
  beats: number[];
  /** Master audio source id (transcription/alignment target). */
  audioSource: string;
  tracks: Track[]; // index 0 = top/foreground
  markers: Marker[];
}

let nextId = 1;
export function freshId(): number { return nextId++; }
/** Call after loading a project so new ids never collide. */
export function reseedIds(project: Project): void {
  let max = 0;
  for (const t of project.tracks) for (const c of t.clips) max = Math.max(max, c.id);
  for (const t of project.tracks) max = Math.max(max, t.id);
  nextId = max + 1;
}

export function emptyProject(): Project {
  return {
    duration: 0, fps: 30, width: 1920, height: 1080,
    bpm: null, beats: [], audioSource: "",
    tracks: [], markers: [],
  };
}

export const DEFAULT_PROPS: Record<string, number> = {
  pos_x: 0.5, pos_y: 0.5, scale_x: 1, scale_y: 1, rotation: 0,
  opacity: 1, volume: 1, pan: 0,
  crop_l: 0, crop_t: 0, crop_r: 0, crop_b: 0,
};

export function makeClip(type: ClipType, start: number, end: number): Clip {
  return {
    id: freshId(), type, start, end, inPoint: 0, speed: 1,
    props: { ...DEFAULT_PROPS }, keyframes: {},
  };
}

export function clipDuration(c: Clip): number { return c.end - c.start; }

/** Effective scalar prop at clip-local time t (seconds since clip start). */
export function propAt(c: Clip, name: string, t: number): number {
  const kt = c.keyframes[name];
  const base = c.props[name] ?? DEFAULT_PROPS[name] ?? 0;
  return kt ? sampleKeyTrack(kt, t, base) : base;
}

export function addTrack(project: Project, name: string, position = 0): Track {
  const track: Track = { id: freshId(), name, muted: false, locked: false, clips: [] };
  project.tracks.splice(Math.min(position, project.tracks.length), 0, track);
  return track;
}

export function findTrack(project: Project, trackId: number): Track | undefined {
  return project.tracks.find((t) => t.id === trackId);
}

export function findClip(project: Project, clipId: number): { track: Track; clip: Clip } | undefined {
  for (const track of project.tracks) {
    const clip = track.clips.find((c) => c.id === clipId);
    if (clip) return { track, clip };
  }
  return undefined;
}

export function insertClip(track: Track, clip: Clip): void {
  const i = track.clips.findIndex((c) => c.start > clip.start);
  if (i === -1) track.clips.push(clip);
  else track.clips.splice(i, 0, clip);
}

export function recomputeDuration(project: Project): void {
  let d = 0;
  for (const t of project.tracks) for (const c of t.clips) d = Math.max(d, c.end);
  project.duration = d;
}

/** Split a clip at timeline times; returns resulting clips left-to-right. */
export function splitClip(project: Project, clipId: number, times: number[]): Clip[] {
  const found = findClip(project, clipId);
  if (!found) return [];
  const { track, clip } = found;
  const cuts = times.filter((t) => t > clip.start && t < clip.end).sort((a, b) => a - b);
  if (cuts.length === 0) return [clip];
  const out: Clip[] = [];
  let prevStart = clip.start;
  let prevIn = clip.inPoint;
  const mk = (s: number, e: number, inPt: number): Clip => ({
    ...clip,
    id: freshId(),
    start: s, end: e, inPoint: inPt,
    props: { ...clip.props },
    keyframes: Object.fromEntries(
      Object.entries(clip.keyframes).map(([k, v]) => [k, { interp: v.interp, keys: v.keys.map((kk) => ({ ...kk })) }]),
    ),
    fx: clip.fx ? { ...clip.fx, params: { ...clip.fx.params } } : undefined,
    fxChain: clip.fxChain?.map((f) => ({ ...f, params: { ...f.params } })),
  });
  for (const cut of cuts) {
    out.push(mk(prevStart, cut, prevIn));
    prevIn += (cut - prevStart) / clip.speed;
    prevStart = cut;
  }
  out.push(mk(prevStart, clip.end, prevIn));
  track.clips = track.clips.filter((c) => c.id !== clipId);
  for (const c of out) insertClip(track, c);
  recomputeDuration(project);
  return out;
}

/** Serialize/deserialize (.pms-compatible subset). */
export function serializeProject(project: Project): string {
  return JSON.stringify({ version: 1, project }, null, 2);
}

export function deserializeProject(json: string): Project {
  const data: unknown = JSON.parse(json);
  if (data && typeof data === "object" && "project" in data) {
    const candidate: unknown = data.project;
    if (candidate && typeof candidate === "object" && "tracks" in candidate && Array.isArray(candidate.tracks)) {
      // Shape validated above; Project has no runtime schema yet, so one named cast.
      const p = candidate as Project;
      p.tracks ??= [];
      p.markers ??= [];
      reseedIds(p);
      return p;
    }
  }
  throw new Error("Not a pop-maker-studio project file");
}
