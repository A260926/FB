/**
 * @license
 * Copyright 2025 Google Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Small 2D affine geometry helpers shared by element coordinate calculations.
 *
 * Matrices use the same component order as CSS `DOMMatrix`:
 * `[a, b, c, d, e, f]` maps `(x, y)` to
 * `(a * x + c * y + e, b * x + d * y + f)`.
 *
 * @internal
 */
export type AffineMatrix = readonly [
  a: number,
  b: number,
  c: number,
  d: number,
  e: number,
  f: number,
];

/**
 * @internal
 */
export interface GeometryPoint {
  x: number;
  y: number;
}

/**
 * @internal
 */
export interface GeometryRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * @internal
 */
export const IDENTITY_MATRIX: AffineMatrix = [1, 0, 0, 1, 0, 0];

/**
 * @internal
 */
export function applyAffine(
  matrix: AffineMatrix,
  point: GeometryPoint,
): GeometryPoint {
  return {
    x: matrix[0] * point.x + matrix[2] * point.y + matrix[4],
    y: matrix[1] * point.x + matrix[3] * point.y + matrix[5],
  };
}

/**
 * Composes two matrices so that the result maps a point through `inner`
 * first and then through `outer`.
 *
 * @internal
 */
export function multiplyAffine(
  outer: AffineMatrix,
  inner: AffineMatrix,
): AffineMatrix {
  return [
    outer[0] * inner[0] + outer[2] * inner[1],
    outer[1] * inner[0] + outer[3] * inner[1],
    outer[0] * inner[2] + outer[2] * inner[3],
    outer[1] * inner[2] + outer[3] * inner[3],
    outer[0] * inner[4] + outer[2] * inner[5] + outer[4],
    outer[1] * inner[4] + outer[3] * inner[5] + outer[5],
  ];
}

/**
 * Returns the four corners of a rectangle, clockwise, without rounding.
 *
 * @internal
 */
export function quadFromRect(rect: GeometryRect): GeometryPoint[] {
  return [
    {x: rect.x, y: rect.y},
    {x: rect.x + rect.width, y: rect.y},
    {x: rect.x + rect.width, y: rect.y + rect.height},
    {x: rect.x, y: rect.y + rect.height},
  ];
}

/**
 * @internal
 */
export function transformQuad(
  quad: readonly GeometryPoint[],
  matrix: AffineMatrix,
): GeometryPoint[] {
  return quad.map(point => {
    return applyAffine(matrix, point);
  });
}

/**
 * @internal
 */
export function quadBoundingBox(
  quad: readonly GeometryPoint[],
): GeometryRect | null {
  if (quad.length === 0) {
    return null;
  }
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (const point of quad) {
    left = Math.min(left, point.x);
    top = Math.min(top, point.y);
    right = Math.max(right, point.x);
    bottom = Math.max(bottom, point.y);
  }
  return {x: left, y: top, width: right - left, height: bottom - top};
}

/**
 * Returns a point inside a convex polygon. The average of the vertices is
 * always inside the polygon, including for reflected or degenerate quads.
 *
 * @internal
 */
export function quadInteriorPoint(
  quad: readonly GeometryPoint[],
): GeometryPoint | null {
  if (quad.length === 0) {
    return null;
  }
  let x = 0;
  let y = 0;
  for (const point of quad) {
    x += point.x;
    y += point.y;
  }
  return {x: x / quad.length, y: y / quad.length};
}

/**
 * Converts the flat, eight-number quad representation used by the DevTools
 * protocol into four clockwise points.
 *
 * @internal
 */
export function protocolQuadToPoints(
  values: readonly number[],
): GeometryPoint[] {
  const points: GeometryPoint[] = [];
  for (let index = 0; index + 1 < values.length; index += 2) {
    points.push({x: values[index]!, y: values[index + 1]!});
  }
  return points;
}

/**
 * Builds an affine matrix that maps a rectangle of `width` by `height`
 * (origin at `(0, 0)`) onto the quad
 * `[topLeft, topRight, bottomRight, bottomLeft]`. Since the quad is the image
 * of that rectangle, this recovers the full composed transform, including
 * nested frames.
 *
 * @internal
 */
export function affineFromQuad(
  quad: readonly GeometryPoint[],
  width: number,
  height: number,
): AffineMatrix {
  const topLeft = quad[0]!;
  const topRight = quad[1] ?? topLeft;
  const bottomLeft = quad[3] ?? topLeft;
  const sx = width === 0 ? 0 : (topRight.x - topLeft.x) / width;
  const sy = width === 0 ? 0 : (topRight.y - topLeft.y) / width;
  const tx = height === 0 ? 0 : (bottomLeft.x - topLeft.x) / height;
  const ty = height === 0 ? 0 : (bottomLeft.y - topLeft.y) / height;
  return [sx, sy, tx, ty, topLeft.x, topLeft.y];
}

/**
 * @internal
 */
function signedArea(polygon: readonly GeometryPoint[]): number {
  let area = 0;
  for (let index = 0; index < polygon.length; index++) {
    const current = polygon[index]!;
    const next = polygon[(index + 1) % polygon.length]!;
    area += current.x * next.y - next.x * current.y;
  }
  return area / 2;
}

/**
 * @internal
 */
function cross(ax: number, ay: number, bx: number, by: number): number {
  return ax * by - ay * bx;
}

/**
 * Returns true if the point lies inside (or on the boundary of) a convex
 * polygon. The tolerance is expressed in CSS pixels.
 *
 * @internal
 */
export function pointInConvexPolygon(
  point: GeometryPoint,
  polygon: readonly GeometryPoint[],
  tolerance = 0,
): boolean {
  if (polygon.length < 3) {
    return false;
  }
  const positive = signedArea(polygon) >= 0;
  for (let index = 0; index < polygon.length; index++) {
    const a = polygon[index]!;
    const b = polygon[(index + 1) % polygon.length]!;
    const value = cross(b.x - a.x, b.y - a.y, point.x - a.x, point.y - a.y);
    if (positive ? value < -tolerance : value > tolerance) {
      return false;
    }
  }
  return true;
}

/**
 * Intersects two convex polygons using Sutherland-Hodgman clipping. The
 * result is empty when the polygons do not overlap.
 *
 * @internal
 */
export function clipConvexPolygon(
  subject: readonly GeometryPoint[],
  clip: readonly GeometryPoint[],
): GeometryPoint[] {
  if (subject.length < 3 || clip.length < 3) {
    return [];
  }
  const positive = signedArea(clip) >= 0;
  let result: GeometryPoint[] = [...subject];
  for (let index = 0; index < clip.length; index++) {
    if (result.length < 3) {
      return [];
    }
    const a = clip[index]!;
    const b = clip[(index + 1) % clip.length]!;
    const inside = (point: GeometryPoint): boolean => {
      const value = cross(b.x - a.x, b.y - a.y, point.x - a.x, point.y - a.y);
      return positive ? value >= 0 : value <= 0;
    };
    const input = result;
    result = [];
    for (let pointIndex = 0; pointIndex < input.length; pointIndex++) {
      const current = input[pointIndex]!;
      const previous = input[(pointIndex + input.length - 1) % input.length]!;
      const currentInside = inside(current);
      const previousInside = inside(previous);
      if (currentInside !== previousInside) {
        const edgeX = b.x - a.x;
        const edgeY = b.y - a.y;
        const segmentX = current.x - previous.x;
        const segmentY = current.y - previous.y;
        const denominator = cross(edgeX, edgeY, segmentX, segmentY);
        if (denominator !== 0) {
          const numerator = cross(
            edgeX,
            edgeY,
            previous.x - a.x,
            previous.y - a.y,
          );
          const t = -numerator / denominator;
          result.push({
            x: previous.x + segmentX * t,
            y: previous.y + segmentY * t,
          });
        }
      }
      if (currentInside) {
        result.push(current);
      }
    }
  }
  return result.length >= 3 ? result : [];
}
