import { flattenInstructions, isInsidePath, SUBSAMPLE_OFFSETS } from './coverage.js';

function finiteNumber(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function clamp01(value) {
  return clamp(value, 0, 1);
}

function isNonzero(value) {
  if (value === undefined || value === null || value === '') return false;
  const number = Number(value);
  return Number.isFinite(number) && number !== 0;
}

function hexByte(value, start) {
  const text = String(value === undefined || value === null ? '00' : value);
  const byte = Number.parseInt(text.slice(start, start + 2), 16);
  return Number.isFinite(byte) ? byte : 0;
}

function mergedTag(document, slice, fragment) {
  const style = document.styles && document.styles[slice.style];
  const styleTag = (style && style.tag) || {};
  return Object.assign({}, styleTag, fragment.tag || {});
}

function assertSupportedDialogue(document, dialogue) {
  const slices = dialogue.slices || [];
  for (let sliceIndex = 0; sliceIndex < slices.length; sliceIndex++) {
    const slice = slices[sliceIndex];
    const fragments = slice.fragments || [];
    for (let fragmentIndex = 0; fragmentIndex < fragments.length; fragmentIndex++) {
      const fragment = fragments[fragmentIndex];
      if (typeof fragment.text === 'string' && fragment.text.length > 0) {
        throw new Error(
          'renderDrawingFrame only supports drawing-only dialogues; text is not supported',
        );
      }
      const tag = mergedTag(document, slice, fragment);
      const unsupportedNumbers = [
        ['bord', tag.bord],
        ['xbord', tag.xbord],
        ['ybord', tag.ybord],
        ['shad', tag.shad],
        ['xshad', tag.xshad],
        ['yshad', tag.yshad],
        ['blur', tag.blur],
        ['be', tag.be],
        ['frx', tag.frx],
        ['fry', tag.fry],
        ['fax', tag.fax],
        ['fay', tag.fay],
      ];
      for (let effectIndex = 0; effectIndex < unsupportedNumbers.length; effectIndex++) {
        const [name, value] = unsupportedNumbers[effectIndex];
        if (isNonzero(value)) {
          throw new Error(`renderDrawingFrame does not support active \\${name} effect (${value})`);
        }
      }
      if (Array.isArray(tag.t) ? tag.t.length > 0 : Boolean(tag.t)) {
        throw new Error('renderDrawingFrame does not support active \\t animations');
      }
    }
  }
}

function pDivisor(tag) {
  const p = finiteNumber(tag.p, 0);
  return p >= 1 ? 2 ** (p - 1) : 1;
}

function alignmentParts(alignment) {
  const an = [1, 2, 3, 4, 5, 6, 7, 8, 9].includes(alignment) ? alignment : 2;
  const column = (an - 1) % 3;
  const row = Math.floor((an - 1) / 3);
  return { an, column, row };
}

function defaultPosition(dialogue, playWidth, playHeight) {
  const margin = dialogue.margin || {};
  const left = finiteNumber(margin.left);
  const right = finiteNumber(margin.right);
  const vertical = finiteNumber(margin.vertical);
  const { column, row } = alignmentParts(dialogue.alignment);

  let x;
  if (column === 0) x = left;
  else if (column === 1) x = (left + playWidth - right) / 2;
  else x = playWidth - right;

  let y;
  if (row === 0) y = playHeight - vertical;
  else if (row === 1) y = playHeight / 2;
  else y = vertical;

  return { x, y };
}

function movePosition(move, localMilliseconds, durationMilliseconds) {
  let progress;
  if (move.t1 === 0 && move.t2 === 0) {
    progress = durationMilliseconds > 0
      ? clamp01(localMilliseconds / durationMilliseconds)
      : 1;
  } else if (move.t1 === move.t2) {
    progress = localMilliseconds < move.t1 ? 0 : 1;
  } else {
    progress = clamp01((localMilliseconds - move.t1) / (move.t2 - move.t1));
  }
  return {
    x: move.x1 + (move.x2 - move.x1) * progress,
    y: move.y1 + (move.y2 - move.y1) * progress,
  };
}

function fadeAlphaAt(fade, time) {
  const lerp = (from, to, progress) => from + (to - from) * progress;
  if (time < fade.t1) return fade.a1;
  if (time < fade.t2) {
    return lerp(fade.a1, fade.a2, (time - fade.t1) / (fade.t2 - fade.t1));
  }
  if (time < fade.t3) return fade.a2;
  if (time < fade.t4) {
    return lerp(fade.a2, fade.a3, (time - fade.t3) / (fade.t4 - fade.t3));
  }
  return fade.a3;
}

function fadeFactor(dialogue, localMilliseconds, durationMilliseconds) {
  const { fade } = dialogue;
  if (!fade) return 1;
  if (fade.type === 'fad') {
    const fadeIn = finiteNumber(fade.t1) === 0
      ? 1
      : clamp01(localMilliseconds / finiteNumber(fade.t1));
    const fadeOut = finiteNumber(fade.t2) === 0
      ? 1
      : clamp01((durationMilliseconds - localMilliseconds) / finiteNumber(fade.t2));
    return Math.min(fadeIn, fadeOut);
  }
  const alpha = clamp(fadeAlphaAt(fade, localMilliseconds), 0, 255);
  return clamp01(1 - alpha / 255);
}

function fragmentBounds(drawing, divisor) {
  if (Number.isFinite(drawing.minX) && Number.isFinite(drawing.minY)
    && Number.isFinite(drawing.width) && Number.isFinite(drawing.height)) {
    return {
      minX: drawing.minX / divisor,
      minY: drawing.minY / divisor,
      maxX: (drawing.minX + drawing.width) / divisor,
      maxY: (drawing.minY + drawing.height) / divisor,
    };
  }
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  const commands = drawing.instructions || [];
  for (let commandIndex = 0; commandIndex < commands.length; commandIndex++) {
    const points = commands[commandIndex].points || [];
    for (let pointIndex = 0; pointIndex < points.length; pointIndex++) {
      const point = points[pointIndex];
      minX = Math.min(minX, point.x / divisor);
      minY = Math.min(minY, point.y / divisor);
      maxX = Math.max(maxX, point.x / divisor);
      maxY = Math.max(maxY, point.y / divisor);
    }
  }
  return { minX, minY, maxX, maxY };
}

function getSubpathBounds(subpaths) {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (let pathIndex = 0; pathIndex < subpaths.length; pathIndex++) {
    const path = subpaths[pathIndex];
    for (let pointIndex = 0; pointIndex < path.length; pointIndex++) {
      const x = path[pointIndex][0];
      const y = path[pointIndex][1];
      minX = Math.min(minX, x);
      minY = Math.min(minY, y);
      maxX = Math.max(maxX, x);
      maxY = Math.max(maxY, y);
    }
  }
  return { minX, minY, maxX, maxY };
}

function sampleMayIntersectBounds(bounds, sampleX, sampleY) {
  return sampleY >= bounds.minY
    && sampleY < bounds.maxY
    && sampleX < bounds.maxX;
}

function prepareClipTest(clip, scaleX, scaleY) {
  if (!clip) return null;
  const inverse = Boolean(clip.inverse);

  if (clip.dots) {
    const x1 = finiteNumber(clip.dots.x1) * scaleX;
    const y1 = finiteNumber(clip.dots.y1) * scaleY;
    const x2 = finiteNumber(clip.dots.x2) * scaleX;
    const y2 = finiteNumber(clip.dots.y2) * scaleY;
    return (sampleX, sampleY) => {
      const inside = sampleX >= x1 && sampleX < x2 && sampleY >= y1 && sampleY < y2;
      return inverse ? !inside : inside;
    };
  }

  if (clip.drawing && clip.drawing.instructions) {
    // The compiled clip scale is the same kind of exponent as \p unless it is
    // already sub-pixel scale 1.  Keep the conversion isolated here.
    const clipScale = finiteNumber(clip.scale, 1);
    const divisor = clipScale >= 1 ? 2 ** (clipScale - 1) : 1;
    const subpaths = flattenInstructions(
      clip.drawing.instructions,
      (x, y) => [x / divisor * scaleX, y / divisor * scaleY],
    );
    const bounds = getSubpathBounds(subpaths);
    return (sampleX, sampleY) => {
      let inside = false;
      if (sampleMayIntersectBounds(bounds, sampleX, sampleY)) {
        inside = isInsidePath(subpaths, sampleX, sampleY);
      }
      return inverse ? !inside : inside;
    };
  }

  return null;
}

function prepareDialogue(document, dialogue, seconds, metrics) {
  const { playWidth, playHeight, scaleX, scaleY } = metrics;
  const localMilliseconds = (seconds - dialogue.start) * 1000;
  const durationMilliseconds = (dialogue.end - dialogue.start) * 1000;
  const opacityFade = fadeFactor(dialogue, localMilliseconds, durationMilliseconds);

  const position = dialogue.move
    ? movePosition(dialogue.move, localMilliseconds, durationMilliseconds)
    : (dialogue.pos || defaultPosition(dialogue, playWidth, playHeight));

  // First pass: union of p-scaled control point boxes from every fragment.
  let box = null;
  const fragmentData = [];
  const slices = dialogue.slices || [];
  for (let sliceIndex = 0; sliceIndex < slices.length; sliceIndex++) {
    const slice = slices[sliceIndex];
    const fragments = slice.fragments || [];
    for (let fragmentIndex = 0; fragmentIndex < fragments.length; fragmentIndex++) {
      const fragment = fragments[fragmentIndex];
      if (!fragment.drawing || !fragment.drawing.instructions
        || !fragment.drawing.instructions.length) continue;
      const tag = mergedTag(document, slice, fragment);
      const divisor = pDivisor(tag);
      const bounds = fragmentBounds(fragment.drawing, divisor);
      if (![bounds.minX, bounds.minY, bounds.maxX, bounds.maxY].every(Number.isFinite)) continue;
      box = box
        ? {
          minX: Math.min(box.minX, bounds.minX),
          minY: Math.min(box.minY, bounds.minY),
          maxX: Math.max(box.maxX, bounds.maxX),
          maxY: Math.max(box.maxY, bounds.maxY),
        }
        : bounds;
      fragmentData.push({ slice, fragment, tag, divisor });
    }
  }

  if (!box) return null;

  const { column, row } = alignmentParts(dialogue.alignment);
  let anchorX;
  if (column === 0) anchorX = box.minX;
  else if (column === 1) anchorX = (box.minX + box.maxX) / 2;
  else anchorX = box.maxX;

  let anchorY;
  if (row === 0) anchorY = box.maxY;
  else if (row === 1) anchorY = (box.minY + box.maxY) / 2;
  else anchorY = box.minY;
  const clipTest = prepareClipTest(dialogue.clip, scaleX, scaleY);

  const fragments = [];
  for (let itemIndex = 0; itemIndex < fragmentData.length; itemIndex++) {
    const item = fragmentData[itemIndex];
    const { tag, divisor } = item;
    const pbo = finiteNumber(tag.pbo);
    const fscx = finiteNumber(tag.fscx, 100) / 100;
    const fscy = finiteNumber(tag.fscy, 100) / 100;
    const angle = finiteNumber(tag.frz) * Math.PI / 180;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const opacity = clamp01((1 - hexByte(tag.a1, 0) / 255) * opacityFade);
    const color = {
      r: hexByte(tag.c1, 4),
      g: hexByte(tag.c1, 2),
      b: hexByte(tag.c1, 0),
    };

    const transform = (sourceX, sourceY) => {
      const localX = (sourceX / divisor - anchorX) * fscx;
      const localY = (sourceY / divisor + pbo - anchorY) * fscy;
      let scriptX;
      let scriptY;
      if (dialogue.org) {
        const relativeX = position.x + localX - dialogue.org.x;
        const relativeY = position.y + localY - dialogue.org.y;
        scriptX = dialogue.org.x + cos * relativeX - sin * relativeY;
        scriptY = dialogue.org.y + sin * relativeX + cos * relativeY;
      } else {
        scriptX = position.x + cos * localX - sin * localY;
        scriptY = position.y + sin * localX + cos * localY;
      }
      return [scriptX * scaleX, scriptY * scaleY];
    };

    const subpaths = flattenInstructions(item.fragment.drawing.instructions, transform);
    if (subpaths.length && opacity > 0) {
      fragments.push({ subpaths, bounds: getSubpathBounds(subpaths), color, opacity });
    }
  }

  if (!fragments.length) return null;
  return { clipTest, fragments };
}

/**
 * Render one transparent RGBA frame from a compiled ASS document.
 * @param {import('../../types/index.js').CompiledASS} document Result of compile().
 * @param {number} seconds Frame time in seconds.
 * @param {{width:number,height:number}} options Output dimensions.
 * @returns {{width:number,height:number,data:Uint8ClampedArray}}
 */
export function renderDrawingFrame(document, seconds, options = {}) {
  if (!document || typeof document !== 'object') {
    throw new TypeError('renderDrawingFrame requires a compiled ASS document');
  }
  const { width, height } = options;
  if (!Number.isInteger(width) || width < 1 || width > 2048) {
    throw new RangeError('width must be an integer from 1 to 2048');
  }
  if (!Number.isInteger(height) || height < 1 || height > 2048) {
    throw new RangeError('height must be an integer from 1 to 2048');
  }
  if (!Number.isFinite(seconds)) {
    throw new RangeError('seconds must be a finite number');
  }

  const playWidthValue = document.info && document.info.PlayResX != null
    ? document.info.PlayResX
    : document.width;
  const playHeightValue = document.info && document.info.PlayResY != null
    ? document.info.PlayResY
    : document.height;
  const playWidth = finiteNumber(playWidthValue, Number.NaN);
  const playHeight = finiteNumber(playHeightValue, Number.NaN);
  if (!(playWidth > 0) || !(playHeight > 0)) {
    throw new RangeError('compiled ASS document must declare positive PlayResX and PlayResY');
  }

  const scaleX = width / playWidth;
  const scaleY = height / playHeight;
  const data = new Uint8ClampedArray(width * height * 4);

  const active = [];
  const dialogues = Array.isArray(document.dialogues) ? document.dialogues : [];
  dialogues.forEach((dialogue, index) => {
    if (seconds >= dialogue.start && seconds < dialogue.end) {
      assertSupportedDialogue(document, dialogue);
      active.push({ dialogue, index });
    }
  });
  active.sort((a, b) => a.dialogue.layer - b.dialogue.layer || a.index - b.index);

  const prepared = active
    .map(({ dialogue }) => prepareDialogue(document, dialogue, seconds, {
      playWidth,
      playHeight,
      scaleX,
      scaleY,
      width,
      height,
    }))
    .filter(Boolean);

  for (let pixelY = 0; pixelY < height; pixelY++) {
    for (let pixelX = 0; pixelX < width; pixelX++) {
      let sumPremultipliedR = 0;
      let sumPremultipliedG = 0;
      let sumPremultipliedB = 0;
      let sumAlpha = 0;

      for (let sampleYIndex = 0; sampleYIndex < 4; sampleYIndex++) {
        const sampleY = pixelY + SUBSAMPLE_OFFSETS[sampleYIndex];
        for (let sampleXIndex = 0; sampleXIndex < 4; sampleXIndex++) {
          const sampleX = pixelX + SUBSAMPLE_OFFSETS[sampleXIndex];
          let alpha = 0;
          let premultipliedR = 0;
          let premultipliedG = 0;
          let premultipliedB = 0;

          for (let rendererIndex = 0; rendererIndex < prepared.length; rendererIndex++) {
            const renderer = prepared[rendererIndex];
            if (renderer.clipTest && !renderer.clipTest(sampleX, sampleY)) continue;
            const rendererFragments = renderer.fragments;
            for (let fragmentIndex = 0; fragmentIndex < rendererFragments.length; fragmentIndex++) {
              const fragment = rendererFragments[fragmentIndex];
              if (!sampleMayIntersectBounds(fragment.bounds, sampleX, sampleY)) continue;
              if (!isInsidePath(fragment.subpaths, sampleX, sampleY)) continue;
              const sourceAlpha = fragment.opacity;
              const inverseAlpha = 1 - sourceAlpha;
              premultipliedR = fragment.color.r * sourceAlpha + premultipliedR * inverseAlpha;
              premultipliedG = fragment.color.g * sourceAlpha + premultipliedG * inverseAlpha;
              premultipliedB = fragment.color.b * sourceAlpha + premultipliedB * inverseAlpha;
              alpha = sourceAlpha + alpha * inverseAlpha;
            }
          }

          sumPremultipliedR += premultipliedR;
          sumPremultipliedG += premultipliedG;
          sumPremultipliedB += premultipliedB;
          sumAlpha += alpha;
        }
      }

      const pixelIndex = (pixelY * width + pixelX) * 4;
      const alphaByte = Math.round(clamp(sumAlpha / 16 * 255, 0, 255));
      if (alphaByte === 0 || sumAlpha === 0) {
        data[pixelIndex] = 0;
        data[pixelIndex + 1] = 0;
        data[pixelIndex + 2] = 0;
        data[pixelIndex + 3] = 0;
      } else {
        // The 1/16 sample count cancels between average premultiplied color
        // and average alpha, so divide the sums directly.
        data[pixelIndex] = Math.round(clamp(sumPremultipliedR / sumAlpha, 0, 255));
        data[pixelIndex + 1] = Math.round(clamp(sumPremultipliedG / sumAlpha, 0, 255));
        data[pixelIndex + 2] = Math.round(clamp(sumPremultipliedB / sumAlpha, 0, 255));
        data[pixelIndex + 3] = alphaByte;
      }
    }
  }

  return { width, height, data };
}
