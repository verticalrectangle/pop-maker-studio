// Minimal typed surface of the untyped `mp4box` UMD module (mp4box.js).
// Only the pieces this app uses are declared; everything else stays `unknown`.
declare module "mp4box" {
  /** A sample entry's codec-config box (avcC / hvcC / etc.). */
  export interface CodecConfigBox {
    size: number;
    write(stream: DataStream): void;
    [key: string]: unknown;
  }

  export interface MP4SampleDescription {
    avcC?: CodecConfigBox;
    hvcC?: CodecConfigBox;
    getCodec(): string;
    [key: string]: unknown;
  }

  export interface MP4Track {
    id: number;
    type: "video" | "audio" | "hint" | "meta" | string;
    nb_samples: number;
    /** Duration in the track's own timescale. */
    duration: number;
    timescale: number;
    /** Duration in the movie timescale. */
    movie_duration: number;
    movie_timescale: number;
    samples_duration: number;
    codec: string;
    track_width: number;
    track_height: number;
    bitrate: number;
    audio?: {
      sample_rate: number;
      channel_count: number;
      sample_size: number;
      [key: string]: unknown;
    };
    [key: string]: unknown;
  }

  export interface MP4Info {
    /** Duration in the movie timescale. Seconds = duration / timescale. */
    duration: number;
    timescale: number;
    tracks: MP4Track[];
    videoTracks: MP4Track[];
    audioTracks: MP4Track[];
    isFragmented: boolean;
  }

  export interface MP4Sample {
    number: number;
    track_id: number;
    description: MP4SampleDescription;
    data: ArrayBuffer;
    size: number;
    alreadyRead: number;
    /** Keyframe flag. */
    is_sync: boolean;
    /** Decode timestamp in track timescale. */
    dts: number;
    /** Composition timestamp in track timescale. */
    cts: number;
    duration: number;
    timescale: number;
  }

  export interface ISOFile {
    onReady?: (info: MP4Info) => void;
    onSamples?: (trackId: number, user: unknown, samples: MP4Sample[]) => void;
    onError?: (e: string) => void;
    start(): void;
    stop(): void;
    flush(): void;
    appendBuffer(buf: ArrayBuffer): number;
    seek(time: number, complete: boolean): number;
    setExtractionOptions(trackId: number, sampleNum: number): void;
    releaseUsedSamples(trackId: number, sampleNum: number): void;
    getTrackById(id: number): MP4Track & {
      mdia?: {
        minf?: {
          stbl?: { stsd?: { entries: MP4SampleDescription[] } };
        };
      };
    };
  }

  /** mp4box's growable byte writer. `new DataStream()` allocates an empty,
   * dynamic buffer; boxes write themselves into it via `box.write(stream)`. */
  export class DataStream {
    constructor(arrayBuffer?: ArrayBuffer | number, byteOffset?: number, endianness?: boolean);
    buffer: ArrayBuffer;
    position: number;
    endianness: boolean;
    writeUint8(v: number): void;
    writeUint16(v: number): void;
    writeUint32(v: number): void;
    static BIG_ENDIAN: boolean;
    static LITTLE_ENDIAN: boolean;
  }

  export function createFile(keepMdatData?: boolean): ISOFile;

  /** mp4box mutates the ArrayBuffer it receives, tagging it with `fileStart`. */
  export type MP4Buffer = ArrayBuffer & { fileStart: number };
}
