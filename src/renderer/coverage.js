/**
 * Sub-pixel coverage evaluation for ASS drawings.
 *
 * Every pixel is sampled on a fixed 4x4 grid: sample (i, j) of pixel (x, y)
 * sits at output position (x + (i + 0.5) / 4, y + (j + 0.5) / 4). Drawing
 * geometry and clip geometry are both flattened and transformed into output
 * space before sampling, so edges are always intersected in the same sample
 * space instead of multiplying per-pixel coverage ratios.
 *
 * A row mask stores one bit per sample of a pixel, bit index = sy * 4 + sx.
 */

const SUB = 4;
const SUB_SAMPLES = SUB * SUB;
// Cubic curves are flattened until every control point is within 1/128 output
// pixels of the chord. The resulting polyline stays within 1/64 px of the
// curve, as required.
const FLATNESS = 1 / 128;

/**
 * @typedef {{x: number, y: number}} Point
 * @typedef {{a: number, b: number, c: number, d: number, e: number, f: number}} Matrix
 * @typedef {{edges: Array, left: number, top: number, right: number, bottom: number}} Shape
 */

/**
 * Apply an affine matrix to a point.
 * @param {Matrix} m
 * @param {Point} p
 * @returns {Point}
 */
export function transformPoint(m, p) {
  return {
    x: m.a * p.x + m.c * p.y + m.e,
    y: m.b * p.x + m.d * p.y + m.f,
  };
}

/**
 * Compose affine matrices so that `compose(m, n)` applies `n` first, then `m`.
 * @param {Matrix} m
 * @param {Matrix} n
 * @returns {Matrix}
 */
export function compose(m, n) {
  return {
    a: m.a * n.a + m.c * n.b,
    b: m.b * n.a + m.d * n.b,
    c: m.a * n.c + m.c * n.d,
    d: m.b * n.c + m.d * n.d,
    e: m.a * n.e + m.c * n.f + m.e,
    f: m.b * n.e + m.d * n.f + m.f,
  };
}

/**
 * Translation matrix.
 * @param {number} x
 * @param {number} y
 * @returns {Matrix}
 */
export function translation(x, y) {
  return {
    a: 1, b: 0, c: 0, d: 1, e: x, f: y,
  };
}

/**
 * Scale matrix.
 * @param {number} x
 * @param {number} y
 * @returns {Matrix}
 */
export function scaling(x, y) {
  return {
    a: x, b: 0, c: 0, d: y, e: 0, f: 0,
  };
}

/**
 * Clockwise rotation matrix for a y-down coordinate system.
 * @param {number} degrees
 * @returns {Matrix}
 */
export function rotation(degrees) {
  const rad = (degrees * Math.PI) / 180;
  const cos = Math.cos(rad);
  const sin = Math.sin(rad);
  return {
    a: cos, b: sin, c: -sin, d: cos, e: 0, f: 0,
  };
}

function flattenCubic(p0, p1, p2, p3, out, depth) {
  const dx = p3.x - p0.x;
  const dy = p3.y - p0.y;
  const len = Math.hypot(dx, dy);
  let deviation;
  if (len < 1e-12) {
    deviation = Math.max(
      Math.hypot(p1.x - p0.x, p1.y - p0.y),
      Math.hypot(p2.x - p0.x, p2.y - p0.y),
    );
  } else {
    deviation = Math.max(
      Math.abs((p1.x - p0.x) * dy - (p1.y - p0.y) * dx),
      Math.abs((p2.x - p0.x) * dy - (p2.y - p0.y) * dx),
    ) / len;
  }
  if (deviation <= FLATNESS || depth >= 20) {
    out.push(p3);
    return;
  }
  const p01 = { x: (p0.x + p1.x) / 2, y: (p0.y + p1.y) / 2 };
  const p12 = { x: (p1.x + p2.x) / 2, y: (p1.y + p2.y) / 2 };
  const p23 = { x: (p2.x + p3.x) / 2, y: (p2.y + p3.y) / 2 };
  const p012 = { x: (p01.x + p12.x) / 2, y: (p01.y + p12.y) / 2 };
  const p123 = { x: (p12.x + p23.x) / 2, y: (p12.y + p23.y) / 2 };
  const mid = { x: (p012.x + p123.x) / 2, y: (p012.y + p123.y) / 2 };
  flattenCubic(p0, p01, p012, mid, out, depth + 1);
  flattenCubic(mid, p123, p23, p3, out, depth + 1);
}

/**
 * Flatten compiled drawing instructions into closed contours in the coordinate
 * space produced by `matrix`. Every M command starts a new sub-path; every
 * sub-path is implicitly closed.
 * @param {import('../../types/index').DialogueDrawing} drawing
 * @param {Matrix} matrix
 * @returns {Point[][]}
 */
export function flattenInstructions(drawing, matrix) {
  const contours = [];
  let current = null;
  let cursor = { x: 0, y: 0 };
  const instructions = (drawing && drawing.instructions) || [];
  for (let i = 0; i < instructions.length; i++) {
    const { type, points } = instructions[i];
    if (!points || !points.length) continue;
    if (type === 'M') {
      current = [transformPoint(matrix, points[0])];
      contours.push(current);
      cursor = points[0];
      continue;
    }
    if (!current) continue;
    if (type === 'L') {
      for (let j = 0; j < points.length; j++) {
        current.push(transformPoint(matrix, points[j]));
        cursor = points[j];
      }
    } else if (type === 'C') {
      for (let j = 0; j + 2 < points.length; j += 3) {
        const p1 = transformPoint(matrix, points[j]);
        const p2 = transformPoint(matrix, points[j + 1]);
        const p3 = transformPoint(matrix, points[j + 2]);
        flattenCubic(transformPoint(matrix, cursor), p1, p2, p3, current, 0);
        cursor = points[j + 2];
      }
    }
  }
  return contours.map((points) => {
    const out = points.slice();
    if (out.length > 1) {
      const first = out[0];
      const last = out[out.length - 1];
      if (last.x === first.x && last.y === first.y) out.pop();
    }
    return out;
  }).filter((points) => points.length >= 3);
}

/**
 * Build an edge list plus bounds for a contour set.
 * @param {Point[][]} contours
 * @returns {Shape}
 */
export function buildShape(contours) {
  const edges = [];
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (let c = 0; c < contours.length; c++) {
    const points = contours[c];
    for (let i = 0; i < points.length; i++) {
      const a = points[i];
      const b = points[(i + 1) % points.length];
      if (a.x < left) left = a.x;
      if (a.x > right) right = a.x;
      if (a.y < top) top = a.y;
      if (a.y > bottom) bottom = a.y;
      if (a.y === b.y) continue;
      edges.push({
        x0: a.x,
        y0: a.y,
        x1: b.x,
        y1: b.y,
        minY: Math.min(a.y, b.y),
        maxY: Math.max(a.y, b.y),
        dir: b.y > a.y ? 1 : -1,
      });
    }
  }
  return {
    edges, left, top, right, bottom,
  };
}

/**
 * Build a shape for a compiled drawing.
 * @param {import('../../types/index').DialogueDrawing} drawing
 * @param {Matrix} matrix
 * @returns {Shape}
 */
export function drawingShape(drawing, matrix) {
  return buildShape(flattenInstructions(drawing, matrix));
}

/**
 * Build a shape for a script-space rectangle clip in output space.
 * @param {{x1: number, y1: number, x2: number, y2: number}} rect
 * @param {number} scaleX
 * @param {number} scaleY
 * @returns {{x1: number, y1: number, x2: number, y2: number}}
 */
export function rectangleShape(rect, scaleX, scaleY) {
  return {
    x1: Math.min(rect.x1, rect.x2) * scaleX,
    x2: Math.max(rect.x1, rect.x2) * scaleX,
    y1: Math.min(rect.y1, rect.y2) * scaleY,
    y2: Math.max(rect.y1, rect.y2) * scaleY,
  };
}

function rowCrossings(edges, y) {
  const crossings = [];
  for (let i = 0; i < edges.length; i++) {
    const edge = edges[i];
    // Half-open interval [minY, maxY): horizontal edges never cross.
    if (!(edge.minY <= y && y < edge.maxY)) continue;
    const t = (y - edge.y0) / (edge.y1 - edge.y0);
    crossings.push({
      x: edge.x0 + t * (edge.x1 - edge.x0),
      dir: edge.dir,
    });
  }
  crossings.sort((a, b) => a.x - b.x);
  return crossings;
}

/**
 * Compute a per-pixel sample mask for one row of a shape.
 * @param {Shape} shape
 * @param {number} row
 * @param {number} width
 * @returns {Uint16Array}
 */
export function shapeRowMask(shape, row, width) {
  const mask = new Uint16Array(width);
  if (!shape.edges.length) return mask;
  if (row + 1 <= shape.top || row >= shape.bottom) return mask;
  const startPx = Math.max(0, Math.floor(shape.left) - 1);
  const endPx = Math.min(width, Math.ceil(shape.right) + 1);
  if (endPx <= startPx) return mask;
  for (let sy = 0; sy < SUB; sy++) {
    const y = row + (sy + 0.5) / SUB;
    if (!(shape.top <= y && y < shape.bottom)) continue;
    const crossings = rowCrossings(shape.edges, y);
    if (!crossings.length) continue;
    // Count crossings strictly to the right of a sample: start with the full
    // winding, then remove edges the ray has already passed.
    let total = 0;
    for (let i = 0; i < crossings.length; i++) total += crossings[i].dir;
    let index = 0;
    let winding = total;
    while (index < crossings.length && crossings[index].x <= startPx) {
      winding -= crossings[index].dir;
      index++;
    }
    for (let px = startPx; px < endPx; px++) {
      let bits = 0;
      for (let sx = 0; sx < SUB; sx++) {
        const x = px + (sx + 0.5) / SUB;
        while (index < crossings.length && crossings[index].x <= x) {
          winding -= crossings[index].dir;
          index++;
        }
        if (winding !== 0) bits |= 1 << (sy * SUB + sx);
      }
      mask[px] |= bits;
    }
  }
  return mask;
}

/**
 * Compute a per-pixel sample mask for one row of a rectangle clip.
 * The rectangle is half-open: [x1, x2) x [y1, y2).
 * @param {{x1: number, y1: number, x2: number, y2: number}} rect
 * @param {number} row
 * @param {number} width
 * @returns {Uint16Array}
 */
export function rectangleRowMask(rect, row, width) {
  const mask = new Uint16Array(width);
  const startPx = Math.max(0, Math.floor(rect.x1) - 1);
  const endPx = Math.min(width, Math.ceil(rect.x2) + 1);
  if (endPx <= startPx) return mask;
  for (let sy = 0; sy < SUB; sy++) {
    const y = row + (sy + 0.5) / SUB;
    if (!(y >= rect.y1 && y < rect.y2)) continue;
    for (let px = startPx; px < endPx; px++) {
      let bits = 0;
      for (let sx = 0; sx < SUB; sx++) {
        const x = px + (sx + 0.5) / SUB;
        if (x >= rect.x1 && x < rect.x2) bits |= 1 << (sy * SUB + sx);
      }
      mask[px] |= bits;
    }
  }
  return mask;
}

export { SUB, SUB_SAMPLES };
