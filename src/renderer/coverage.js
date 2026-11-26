// Path coverage helpers.  Coordinates passed to these functions are always in
// output pixel space, so curve flattening tolerance is measured in output
// pixels as required by task_support/render-contract.md.

export const SUBSAMPLE_OFFSETS = [0.125, 0.375, 0.625, 0.875];

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

function distanceToSegmentSquared(point, start, end) {
  const vx = end[0] - start[0];
  const vy = end[1] - start[1];
  const wx = point[0] - start[0];
  const wy = point[1] - start[1];
  const lengthSquared = vx * vx + vy * vy;
  let t = 0;
  if (lengthSquared > 0) {
    t = clamp01((wx * vx + wy * vy) / lengthSquared);
  }
  const dx = point[0] - (start[0] + t * vx);
  const dy = point[1] - (start[1] + t * vy);
  return dx * dx + dy * dy;
}

function cubicIsFlat(p0, p1, p2, p3, toleranceSquared) {
  return distanceToSegmentSquared(p1, p0, p3) <= toleranceSquared
    && distanceToSegmentSquared(p2, p0, p3) <= toleranceSquared;
}

function addFlattenedCubic(p0, p1, p2, p3, toleranceSquared, points, depth) {
  // The depth guard only protects against non-finite/degenerate input.  Normal
  // curves stop much earlier from the geometric flatness test.
  if (depth < 30 && !cubicIsFlat(p0, p1, p2, p3, toleranceSquared)) {
    const p01 = [(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2];
    const p12 = [(p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2];
    const p23 = [(p2[0] + p3[0]) / 2, (p2[1] + p3[1]) / 2];
    const p012 = [(p01[0] + p12[0]) / 2, (p01[1] + p12[1]) / 2];
    const p123 = [(p12[0] + p23[0]) / 2, (p12[1] + p23[1]) / 2];
    const p0123 = [(p012[0] + p123[0]) / 2, (p012[1] + p123[1]) / 2];
    addFlattenedCubic(p0, p01, p012, p0123, toleranceSquared, points, depth + 1);
    addFlattenedCubic(p0123, p123, p23, p3, toleranceSquared, points, depth + 1);
  } else {
    points.push(p3);
  }
}

/**
 * Flatten compiled M/L/C instructions into automatically closed subpaths.
 * @param {Array<{type:string,points:Array<{x:number,y:number}>}>} instructions
 * @param {(x:number,y:number)=>[number,number]} transform
 * @param {number} [tolerance] Maximum output-pixel deviation.
 * @returns {Array<Array<[number,number]>>}
 */
export function flattenInstructions(instructionsInput, transform, tolerance = 1 / 64) {
  const subpaths = [];
  let points = null;
  let current = null;

  const ensurePath = () => {
    if (!points) {
      const origin = transform(0, 0);
      points = [origin];
      current = origin;
      subpaths.push(points);
    }
  };

  const moveTo = (source) => {
    current = transform(source.x, source.y);
    points = [current];
    subpaths.push(points);
  };

  const lineTo = (source) => {
    ensurePath();
    current = transform(source.x, source.y);
    points.push(current);
  };

  const instructions = instructionsInput || [];
  for (let commandIndex = 0; commandIndex < instructions.length; commandIndex++) {
    const command = instructions[commandIndex];
    if (command.type === 'M') {
      command.points.forEach((source, index) => {
        if (index === 0) moveTo(source);
        else lineTo(source);
      });
    } else if (command.type === 'L') {
      command.points.forEach(lineTo);
    } else if (command.type === 'C') {
      if (command.points.length < 3) continue;
      ensurePath();
      // A compiled C command can contain consecutive three-point cubic groups.
      for (let pointIndex = 0; pointIndex + 2 < command.points.length; pointIndex += 3) {
        const p0 = current;
        const p1 = transform(command.points[pointIndex].x, command.points[pointIndex].y);
        const p2 = transform(command.points[pointIndex + 1].x, command.points[pointIndex + 1].y);
        const p3 = transform(command.points[pointIndex + 2].x, command.points[pointIndex + 2].y);
        addFlattenedCubic(p0, p1, p2, p3, tolerance * tolerance, points, 0);
        current = p3;
      }
    }
  }

  return subpaths.filter((path) => path.length >= 2);
}

/**
 * Non-zero winding number using a rightward ray and half-open y crossings.
 * @param {Array<Array<[number,number]>>} subpaths
 * @param {number} x Sample x in output pixels.
 * @param {number} y Sample y in output pixels.
 * @returns {number}
 */
export function windingAt(subpaths, x, y) {
  let winding = 0;
  for (let pathIndex = 0; pathIndex < subpaths.length; pathIndex++) {
    const path = subpaths[pathIndex];
    for (let i = 0; i < path.length; i++) {
      const start = path[i];
      const end = path[(i + 1) % path.length];
      const x1 = start[0];
      const y1 = start[1];
      const x2 = end[0];
      const y2 = end[1];

      if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
        const intersectionX = x1 + ((y - y1) * (x2 - x1)) / (y2 - y1);
        if (intersectionX > x) {
          winding += y2 > y1 ? 1 : -1;
        }
      }
    }
  }
  return winding;
}

export function isInsidePath(subpaths, x, y) {
  return windingAt(subpaths, x, y) !== 0;
}
