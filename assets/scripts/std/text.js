// pms:text — word layout on measured advances.
//
// layout(ctx, words, {width, lineHeight, space}): flow words (each a string
// or {text, width?}) into lines within `width` px using ctx.measureText when
// a width isn't given. Returns {lines: [{words: [{text, x, w}], width, y}],
// height} with y = line top (lineHeight per line).
//
// fit(ctx, words, box, {max, min}): largest font size in [min, max] that fits
// `words` inside box {w, h} without splitting words. Sets ctx.font size via
// the "<size>px <family>" tail of the current ctx.font. Returns the size.
//
// glyphs(ctx, text, x, y): per-glyph advances [{ch, x, w}] starting at x
// (left-aligned; caller applies align). For per-glyph transforms (flip,
// offset, scramble).

function currentFont(ctx, size) {
  const m = /^(.*?)(\d+(?:\.\d+)?)px\s+(.+)$/.exec(ctx.font || '');
  if (!m) return `${size}px sans-serif`;
  return `${m[1]}${size}px ${m[3]}`;
}

function wordWidth(ctx, word) {
  if (typeof word === 'object' && word !== null && typeof word.width === 'number') {
    return word.width;
  }
  const text = typeof word === 'string' ? word : word.text;
  return ctx.measureText(text).width;
}

export function layout(ctx, words, opts = {}) {
  const width = opts.width !== undefined ? opts.width : 1000;
  const lineHeight = opts.lineHeight !== undefined ? opts.lineHeight : 1.2;
  const space = opts.space !== undefined ? opts.space : null;
  const spaceW = space !== null ? space : ctx.measureText(' ').width;

  // Resolve font size for lineHeight multiplier interpretation.
  const fontM = /(\d+(?:\.\d+)?)px/.exec(ctx.font || '');
  const fontSize = fontM ? parseFloat(fontM[1]) : 16;
  const lh = lineHeight < 4 ? lineHeight * fontSize : lineHeight;

  const lines = [];
  let cur = [];
  let curW = 0;
  const flush = () => {
    if (cur.length === 0) return;
    let x = 0;
    const boxes = cur.map((entry) => {
      const b = { text: entry.text, x, w: entry.w };
      x += entry.w + spaceW;
      return b;
    });
    lines.push({ words: boxes, width: x - spaceW, y: lines.length * lh });
    cur = [];
    curW = 0;
  };
  for (const word of words) {
    const text = typeof word === 'string' ? word : word.text;
    const w = wordWidth(ctx, word);
    const add = cur.length === 0 ? w : curW + spaceW + w;
    if (cur.length > 0 && add > width) flush();
    cur.push({ text, w });
    curW = cur.length === 1 ? w : curW + spaceW + w;
  }
  flush();
  return { lines, height: lines.length * lh, lineHeight: lh };
}

export function fit(ctx, words, box, opts = {}) {
  const max = opts.max !== undefined ? opts.max : 120;
  const min = opts.min !== undefined ? opts.min : 8;
  const saved = ctx.font;
  let lo = min, hi = max;
  // Binary search the largest size that fits (monotonic: bigger = wider).
  for (let i = 0; i < 24; i++) {
    const mid = (lo + hi) / 2;
    ctx.font = currentFont(ctx, mid);
    const laid = layout(ctx, words, opts);
    const widest = laid.lines.reduce((m, l) => Math.max(m, l.width), 0);
    if (widest <= box.w && laid.height <= box.h) lo = mid;
    else hi = mid;
    if (hi - lo < 0.5) break;
  }
  ctx.font = saved;
  return lo;
}

export function glyphs(ctx, text, x, y) {
  const out = [];
  let cx = x;
  for (const ch of String(text)) {
    const w = ctx.measureText(ch).width;
    out.push({ ch, x: cx, y, w });
    cx += w;
  }
  return out;
}
