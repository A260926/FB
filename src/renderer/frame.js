import {
  compose,
  drawingShape,
  rectangleRowMask,
  rectangleShape,
  rotation,
  scaling,
  shapeRowMask,
  translation,
} from './coverage.js';

const MAX_SIZE = 2048;
const SUB_SAMPLES = 16;
const FULL_MASK = 0xffff;

function clamp01(value) {
  if (!(value > 0)) return 0;
  if (value > 1) return 1;
  return value;
}

function parseAlpha(value) {
  if (typeof value === 'number') {
    return Number.isFinite(value) ? value : 0;
  }
  const alpha = Number.parseInt(value, 16);
  return Number.isFinite(alpha) ? alpha : 0;
}

function colorOf(tag) {
  const hex = typeof tag.c1 === 'string' ? tag.c1.padStart(6, '0') : '000000';
  return {
    // ASS colours are stored as BBGGRR.
    b: Number.parseInt(hex.slice(0, 2), 16) / 255,
    g: Number.parseInt(hex.slice(2, 4), 16) / 255,
    r: Number.parseInt(hex.slice(4, 6), 16) / 255,
  };
}

function effectiveTag(styles, slice, fragment) {
  const style = styles[slice.style] || styles.Default;
  return Object.assign({}, style ? style.tag : {}, fragment.tag);
}

function fail(message) {
  throw new Error(`renderDrawingFrame: ${message}`);
}

function validateUnsupported(tag, text) {
  if (text) {
    fail('text is not supported, only drawing dialogues can be rendered');
  }
  ['xbord', 'ybord', 'xshad', 'yshad'].forEach((key) => {
    if (tag[key]) fail(`${key} (border/shadow) is not supported`);
  });
  if (tag.blur) fail('blur is not supported');
  ['frx', 'fry'].forEach((key) => {
    if (tag[key]) fail(`${key} (perspective rotation) is not supported`);
  });
  ['fax', 'fay'].forEach((key) => {
    if (tag[key]) fail(`${key} (skew) is not supported`);
  });
  if (tag.t !== undefined) fail('t animations are not supported');
}

function fadeFactor(fade, localMs, durationMs) {
  if (!fade) return 1;
  if (fade.type === 'fad') {
    const tin = fade.t1;
    const tout = fade.t2;
    const fadeIn = tin > 0 ? clamp01(localMs / tin) : 1;
    const fadeOut = tout > 0 ? clamp01((durationMs - localMs) / tout) : 1;
    return Math.min(fadeIn, fadeOut);
  }
  if (fade.type === 'fade') {
    const { a1, a2, a3, t1, t2, t3, t4 } = fade;
    const o1 = 1 - parseAlpha(a1) / 255;
    const o2 = 1 - parseAlpha(a2) / 255;
    const o3 = 1 - parseAlpha(a3) / 255;
    if (localMs < t1) return o1;
    if (localMs < t2) {
      if (t2 <= t1) return o2;
      return o1 + ((o2 - o1) * (localMs - t1)) / (t2 - t1);
    }
    if (localMs < t3) return o2;
    if (localMs < t4) {
      if (t4 <= t3) return o3;
      return o2 + ((o3 - o2) * (localMs - t3)) / (t4 - t3);
    }
    return o3;
  }
  return 1;
}

function moveAnchor(move, localMs, durationMs) {
  let { t1, t2 } = move;
  const { x1, y1, x2, y2 } = move;
  if (t1 === t2) {
    if (t1 === 0) {
      t2 = durationMs;
    } else {
      return localMs < t1 ? { x: x1, y: y1 } : { x: x2, y: y2 };
    }
  }
  const fraction = t2 > t1 ? clamp01((localMs - t1) / (t2 - t1)) : 1;
  return {
    x: x1 + (x2 - x1) * fraction,
    y: y1 + (y2 - y1) * fraction,
  };
}

function alignmentParts(alignment) {
  const an = Number.isInteger(alignment) && alignment >= 1 && alignment <= 9
    ? alignment
    : 2;
  return {
    column: (an - 1) % 3, // 0 left, 1 centre, 2 right
    row: Math.floor((an - 1) / 3), // 0 bottom, 1 middle, 2 top
  };
}

function anchorPosition(document, dialogue, alignment) {
  const { column, row } = alignmentParts(alignment);
  const { width: scriptWidth, height: scriptHeight } = document;
  const margin = dialogue.margin || { left: 0, right: 0, vertical: 0 };
  let x;
  if (column === 0) {
    x = margin.left;
  } else if (column === 2) {
    x = scriptWidth - margin.right;
  } else {
    x = (margin.left + (scriptWidth - margin.right)) / 2;
  }
  let y;
  if (row === 0) {
    y = scriptHeight - margin.vertical;
  } else if (row === 2) {
    y = margin.vertical;
  } else {
    y = scriptHeight / 2;
  }
  return { x, y };
}

function fragmentTransform(tag, anchor, anchorPos, org, sx, sy) {
  const p = tag.p === undefined ? 1 : tag.p;
  const pScale = Math.pow(2, p - 1);
  const pbo = tag.pbo || 0;
  const fscx = tag.fscx === undefined ? 100 : tag.fscx;
  const fscy = tag.fscy === undefined ? 100 : tag.fscy;
  const frz = tag.frz || 0;

  // Local drawing coordinates: divide by the drawing scale, then offset y.
  let matrix = compose(translation(0, pbo), scaling(1 / pScale, 1 / pScale));
  // Move the anchor of the dialogue box to the local origin.
  matrix = compose(translation(-anchor.x, -anchor.y), matrix);
  // Static scaling around the local origin.
  matrix = compose(scaling(fscx / 100, fscy / 100), matrix);
  // Place at the anchor position.
  matrix = compose(translation(anchorPos.x, anchorPos.y), matrix);
  // Clockwise rotation around the anchor, or around `org` when present.
  if (frz) {
    const centre = org || anchorPos;
    matrix = compose(
      translation(centre.x, centre.y),
      compose(rotation(frz), compose(translation(-centre.x, -centre.y), matrix)),
    );
  }
  // Finally the script-to-output canvas scale.
  return compose(scaling(sx, sy), matrix);
}

function dialogueBox(dialogue, styles) {
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (let s = 0; s < dialogue.slices.length; s++) {
    const slice = dialogue.slices[s];
    const style = styles[slice.style] || styles.Default;
    for (let f = 0; f < slice.fragments.length; f++) {
      const fragment = slice.fragments[f];
      const { drawing } = fragment;
      if (!drawing || !drawing.instructions || !drawing.instructions.length) continue;
      if (!Number.isFinite(drawing.minX) || !Number.isFinite(drawing.minY)) continue;
      const tag = Object.assign({}, style ? style.tag : {}, fragment.tag);
      const p = tag.p === undefined ? 1 : tag.p;
      const pScale = Math.pow(2, p - 1);
      // The dialogue box is the union of the p-scaled control-point ranges;
      // `pbo` only offsets the drawing itself.
      const x0 = drawing.minX / pScale;
      const x1 = (drawing.minX + drawing.width) / pScale;
      const y0 = drawing.minY / pScale;
      const y1 = (drawing.minY + drawing.height) / pScale;
      if (x0 < left) left = x0;
      if (x1 > right) right = x1;
      if (y0 < top) top = y0;
      if (y1 > bottom) bottom = y1;
    }
  }
  if (!(left <= right && top <= bottom)) return null;
  return {
    minX: left, minY: top, maxX: right, maxY: bottom, width: right - left, height: bottom - top,
  };
}

function anchorFromBox(box, alignment) {
  const { column, row } = alignmentParts(alignment);
  let x = box.minX + box.width / 2;
  if (column === 0) x = box.minX;
  if (column === 2) x = box.maxX;
  let y = box.minY + box.height / 2;
  if (row === 0) y = box.maxY;
  if (row === 2) y = box.minY;
  return { x, y };
}

function prepareClip(clip, sx, sy) {
  if (!clip) return null;
  if (clip.dots) {
    return { kind: 'rect', rect: rectangleShape(clip.dots, sx, sy), inverse: !!clip.inverse };
  }
  if (clip.drawing) {
    const scale = clip.scale === undefined ? 1 : clip.scale;
    // A drawing clip is scaled by its own `\clip(scale, ...)` factor only; it
    // stays in script coordinates and ignores the dialogue's p/pos/move/frz.
    const factor = Math.pow(2, scale - 1);
    const matrix = scaling(sx / factor, sy / factor);
    return {
      kind: 'shape', shape: drawingShape(clip.drawing, matrix), inverse: !!clip.inverse,
    };
  }
  return null;
}

function prepareDialogue(document, dialogue, index, localMs, durationMs, sx, sy) {
  const styles = document.styles || {};
  const fade = fadeFactor(dialogue.fade, localMs, durationMs);
  const box = dialogueBox(dialogue, styles);
  const { alignment } = dialogue;
  const anchor = box ? anchorFromBox(box, alignment) : { x: 0, y: 0 };
  let anchorPos = dialogue.pos ? { x: dialogue.pos.x, y: dialogue.pos.y } : null;
  if (!anchorPos) {
    anchorPos = dialogue.move
      ? moveAnchor(dialogue.move, localMs, durationMs)
      : anchorPosition(document, dialogue, alignment);
  }
  const org = dialogue.org ? { x: dialogue.org.x, y: dialogue.org.y } : null;
  const fragments = [];
  for (let s = 0; s < dialogue.slices.length; s++) {
    const slice = dialogue.slices[s];
    for (let f = 0; f < slice.fragments.length; f++) {
      const fragment = slice.fragments[f];
      const tag = effectiveTag(styles, slice, fragment);
      validateUnsupported(tag, fragment.text);
      const { drawing } = fragment;
      if (!drawing || !drawing.instructions || !drawing.instructions.length) continue;
      const matrix = fragmentTransform(tag, anchor, anchorPos, org, sx, sy);
      const shape = drawingShape(drawing, matrix);
      if (!shape.edges.length) continue;
      const color = colorOf(tag);
      const opacity = (1 - parseAlpha(tag.a1) / 255) * fade;
      if (!(opacity > 0)) continue;
      fragments.push({
        shape,
        r: color.r * opacity,
        g: color.g * opacity,
        b: color.b * opacity,
        a: opacity,
      });
    }
  }
  return {
    layer: dialogue.layer,
    index,
    fragments,
    clip: prepareClip(dialogue.clip, sx, sy),
  };
}

function validateSize(value, name) {
  if (!Number.isInteger(value) || value < 1 || value > MAX_SIZE) {
    fail(`${name} must be an integer in [1, ${MAX_SIZE}]`);
  }
}

function clipMaskForRow(clip, row, width) {
  if (!clip) return null;
  if (clip.kind === 'rect') {
    const rectMask = rectangleRowMask(clip.rect, row, width);
    if (clip.inverse) {
      for (let i = 0; i < width; i++) rectMask[i] ^= FULL_MASK;
    }
    return rectMask;
  }
  const shapeMask = shapeRowMask(clip.shape, row, width);
  if (clip.inverse) {
    for (let i = 0; i < width; i++) shapeMask[i] ^= FULL_MASK;
  }
  return shapeMask;
}

/**
 * Render a compiled ASS document into a straight-alpha RGBA frame.
 * @param {import('../../types/index').CompiledASS} document
 * @param {number} seconds
 * @param {{width: number, height: number}} options
 * @returns {{width: number, height: number, data: Uint8ClampedArray}}
 */
export function renderDrawingFrame(document, seconds, options = {}) {
  if (!document || !Array.isArray(document.dialogues)) {
    fail('a compiled document is required');
  }
  if (typeof seconds !== 'number' || !Number.isFinite(seconds)) {
    fail('seconds must be a finite number');
  }
  const { width, height } = options;
  validateSize(width, 'width');
  validateSize(height, 'height');
  const { width: scriptWidth, height: scriptHeight } = document;
  if (!(Number.isFinite(scriptWidth) && scriptWidth > 0)
    || !(Number.isFinite(scriptHeight) && scriptHeight > 0)) {
    fail('the document must have a positive PlayResX and PlayResY');
  }
  const sx = width / scriptWidth;
  const sy = height / scriptHeight;
  const data = new Uint8ClampedArray(width * height * 4);

  const active = [];
  for (let i = 0; i < document.dialogues.length; i++) {
    const dialogue = document.dialogues[i];
    if (seconds >= dialogue.start && seconds < dialogue.end) {
      active.push({ dialogue, index: i });
    }
  }
  if (!active.length) return { width, height, data };

  const prepared = [];
  for (let i = 0; i < active.length; i++) {
    const { dialogue, index } = active[i];
    const localMs = (seconds - dialogue.start) * 1000;
    const durationMs = (dialogue.end - dialogue.start) * 1000;
    prepared.push(prepareDialogue(
      document,
      dialogue,
      index,
      localMs,
      durationMs,
      sx,
      sy,
    ));
  }
  prepared.sort((a, b) => a.layer - b.layer || a.index - b.index);

  for (let row = 0; row < height; row++) {
    const rowData = [];
    let hasContent = false;
    for (let p = 0; p < prepared.length; p++) {
      const entry = prepared[p];
      const masks = [];
      const clipMask = clipMaskForRow(entry.clip, row, width);
      for (let f = 0; f < entry.fragments.length; f++) {
        const mask = shapeRowMask(entry.fragments[f].shape, row, width);
        masks.push(clipMask ? mask.map((value, i) => value & clipMask[i]) : mask);
        if (!hasContent && mask.some((value) => value)) hasContent = true;
      }
      rowData.push(masks);
    }
    if (!hasContent) continue;

    for (let px = 0; px < width; px++) {
      let union = 0;
      for (let p = 0; p < rowData.length; p++) {
        for (let f = 0; f < rowData[p].length; f++) union |= rowData[p][f][px];
      }
      if (!union) continue;

      let accR = 0;
      let accG = 0;
      let accB = 0;
      let accA = 0;
      for (let bit = 0; bit < SUB_SAMPLES; bit++) {
        if (!(union & (1 << bit))) continue;
        let pr = 0;
        let pg = 0;
        let pb = 0;
        let pa = 0;
        for (let p = 0; p < prepared.length; p++) {
          const fragments = prepared[p].fragments;
          const masks = rowData[p];
          for (let f = 0; f < fragments.length; f++) {
            if (!(masks[f][px] & (1 << bit))) continue;
            const src = fragments[f];
            const inv = 1 - src.a;
            pr = src.r + pr * inv;
            pg = src.g + pg * inv;
            pb = src.b + pb * inv;
            pa = src.a + pa * inv;
          }
        }
        accR += pr;
        accG += pg;
        accB += pb;
        accA += pa;
      }
      const alpha = accA / SUB_SAMPLES;
      const offset = (row * width + px) * 4;
      if (!(alpha > 0)) {
        data[offset] = 0;
        data[offset + 1] = 0;
        data[offset + 2] = 0;
        data[offset + 3] = 0;
      } else {
        const red = (accR / SUB_SAMPLES) / alpha;
        const green = (accG / SUB_SAMPLES) / alpha;
        const blue = (accB / SUB_SAMPLES) / alpha;
        data[offset] = Math.round(Math.min(1, Math.max(0, red)) * 255);
        data[offset + 1] = Math.round(Math.min(1, Math.max(0, green)) * 255);
        data[offset + 2] = Math.round(Math.min(1, Math.max(0, blue)) * 255);
        data[offset + 3] = Math.round(Math.min(1, Math.max(0, alpha)) * 255);
      }
    }
  }
  return { width, height, data };
}
