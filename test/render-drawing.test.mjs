import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { taskCase } from '../task_support/check.mjs';
import { compile, decompile, renderDrawingFrame } from '../src/index.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EXAMPLE = path.join(ROOT, 'examples', 'render-drawing.mjs');
const FIXTURE = path.join(ROOT, 'task_support', 'drawing.ass');

const STYLE_FORMAT = 'Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding';
const RED_STYLE = 'Style: Draw,Arial,20,&H000000FF,&H0000FF00,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1';
const GREEN_STYLE = 'Style: Green,Arial,20,&H0000FF00,&H0000FF00,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1';

function ass(width, height, dialogues, extraStyles = '') {
  return `[Script Info]
PlayResX: ${width}
PlayResY: ${height}
WrapStyle: 2
[V4+ Styles]
${STYLE_FORMAT}
${RED_STYLE}
${extraStyles}
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
${Array.isArray(dialogues) ? dialogues.join('\n') : dialogues}
`;
}

function dialogue(text, options = {}) {
  const {
    layer = 0,
    start = '0:00:00.00',
    end = '0:00:02.00',
    style = 'Draw',
  } = options;
  return `Dialogue: ${layer},${start},${end},${style},,0,0,0,,${text}`;
}

function frameFromASS(source, seconds = 0.5, width, height) {
  const document = compile(source);
  return renderDrawingFrame(document, seconds, {
    width: width || document.width,
    height: height || document.height,
  });
}

function rgba(frame, x, y) {
  const index = (y * frame.width + x) * 4;
  return [
    frame.data[index],
    frame.data[index + 1],
    frame.data[index + 2],
    frame.data[index + 3],
  ];
}

function expectPixel(check, frame, x, y, expected, label) {
  const actual = rgba(frame, x, y);
  check(
    actual.length === 4 && actual.every((value, index) => value === expected[index]),
    `${label}: pixel ${x},${y} was ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`,
  );
}

function expectTransparent(check, frame, x, y, label) {
  expectPixel(check, frame, x, y, [0, 0, 0, 0], label);
}

taskCase('geometry', (check) => {
  const holeSource = ass(12, 12, dialogue(
    '{\\an7\\pos(0,0)\\p1}m 0 0 l 10 0 10 10 0 10 '
    + 'm 4 4 l 4 8 8 8 8 4',
  ));
  const hole = frameFromASS(holeSource, 0.5, 12, 12);
  expectPixel(check, hole, 1, 1, [255, 0, 0, 255], 'outer fill');
  expectPixel(check, hole, 9, 9, [255, 0, 0, 255], 'opposite winding ring');
  expectTransparent(check, hole, 5, 5, 'reverse inner contour hollows out');
  expectTransparent(check, hole, 10, 10, 'outer bottom-right edge is clipped');

  const sameWindingSource = ass(12, 12, dialogue(
    '{\\an7\\pos(0,0)\\p1}m 0 0 l 10 0 10 10 0 10 '
    + 'm 4 4 l 8 4 8 8 4 8',
  ));
  const sameWinding = frameFromASS(sameWindingSource, 0.5, 12, 12);
  expectPixel(check, sameWinding, 5, 5, [255, 0, 0, 255], 'same winding inner contour stays filled');

  const curveSource = ass(16, 16, dialogue(
    '{\\an7\\pos(0,0)\\p1}'
    + 'm 8 2 '
    + 'b 11.314 2 14 4.686 14 8 '
    + 'b 14 11.314 11.314 14 8 14 '
    + 'b 4.686 14 2 11.314 2 8 '
    + 'b 2 4.686 4.686 2 8 2',
  ));
  const curve = frameFromASS(curveSource, 0.5, 16, 16);
  expectPixel(check, curve, 8, 8, [255, 0, 0, 255], 'curve interior');
  expectPixel(check, curve, 1, 1, [255, 0, 0, 16], 'curve edge uses shared 16-sample coverage');
  expectTransparent(check, curve, 14, 14, 'curve exterior');

  const pScale = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(0,0)\\p2}m 0 0 l 8 0 8 8 0 8',
  )), 0.5, 8, 8);
  expectPixel(check, pScale, 3, 3, [255, 0, 0, 255], 'p2 divides drawing coordinates by two');
  expectTransparent(check, pScale, 4, 4, 'p2 right edge lands at script x/y=4');

  const pbo = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(0,0)\\p1\\pbo2}m 0 0 l 2 0 2 2 0 2',
  )), 0.5, 8, 8);
  expectTransparent(check, pbo, 1, 1, 'pbo offsets local drawing vertically');
  expectPixel(check, pbo, 1, 3, [255, 0, 0, 255], 'pbo-shifted rectangle interior');

  const fsc = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(0,0)\\p1\\fscx200\\fscy50}m 0 0 l 4 0 4 4 0 4',
  )), 0.5, 8, 8);
  expectPixel(check, fsc, 6, 1, [255, 0, 0, 255], 'fscx expands around anchor');
  expectTransparent(check, fsc, 0, 2, 'fscy contracts around anchor');

  const clipRectangle = frameFromASS(ass(16, 16, dialogue(
    '{\\an5\\pos(8,8)\\p1\\clip(2,3,6,7)}m -8 -8 l 8 -8 8 8 -8 8',
  )), 0.5, 16, 16);
  expectPixel(check, clipRectangle, 3, 4, [255, 0, 0, 255], 'rect clip interior');
  expectPixel(check, clipRectangle, 5, 6, [255, 0, 0, 255], 'rect clip upper/right interior');
  expectTransparent(check, clipRectangle, 1, 4, 'outside rect clip');
  expectTransparent(check, clipRectangle, 6, 6, 'rect clip x2 is exclusive');
  expectTransparent(check, clipRectangle, 3, 7, 'rect clip y2 is exclusive');

  const inverseClip = frameFromASS(ass(16, 16, dialogue(
    '{\\an5\\pos(8,8)\\p1\\iclip(2,3,6,7)}m -8 -8 l 8 -8 8 8 -8 8',
  )), 0.5, 16, 16);
  expectPixel(check, inverseClip, 0, 0, [255, 0, 0, 255], 'iclip exterior is painted');
  expectTransparent(check, inverseClip, 3, 4, 'iclip interior is removed');

  const drawingClip = frameFromASS(ass(16, 16, dialogue(
    '{\\an5\\pos(8,8)\\p1\\clip(m 2 3 l 6 3 6 7 2 7)}'
    + 'm -8 -8 l 8 -8 8 8 -8 8',
  )), 0.5, 16, 16);
  expectPixel(check, drawingClip, 3, 4, [255, 0, 0, 255], 'drawing clip interior');
  expectTransparent(check, drawingClip, 8, 8, 'outside drawing clip remains clear');

  const rotation = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(4,4)\\p1\\frz90}m 0 0 l 4 0 4 4 0 4',
  )), 0.5, 8, 8);
  expectPixel(check, rotation, 1, 5, [255, 0, 0, 255], 'clockwise frz moves top-left box below-left');
  expectTransparent(check, rotation, 1, 1, 'rotated box leaves upper-left empty');
  expectTransparent(check, rotation, 6, 5, 'rotated box leaves lower-right empty');

  const orgRotation = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(4,4)\\org(8,4)\\p1\\frz90}m 0 0 l 4 0 4 4 0 4',
  )), 0.5, 8, 8);
  expectPixel(check, orgRotation, 5, 1, [255, 0, 0, 255], 'org changes rotation pivot');
  expectTransparent(check, orgRotation, 1, 5, 'org pivot moves shape away from default result');

  const scaledOutput = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\pos(0,0)\\p1}m 0 0 l 4 0 4 4 0 4',
  )), 0.5, 4, 4);
  expectPixel(check, scaledOutput, 1, 1, [255, 0, 0, 255], 'output dimensions scale script coordinates');
  expectTransparent(check, scaledOutput, 2, 2, 'scaled right edge is at output coordinate two');

  let rejectedPlayRes = false;
  try {
    renderDrawingFrame({ info: {}, dialogues: [] }, 0, { width: 4, height: 4 });
  } catch {
    rejectedPlayRes = true;
  }
  check(rejectedPlayRes, 'missing positive PlayRes must reject');

  let rejectedSize = false;
  try {
    renderDrawingFrame({ info: { PlayResX: 8, PlayResY: 8 }, dialogues: [] }, 0, { width: 0, height: 4 });
  } catch {
    rejectedSize = true;
  }
  check(rejectedSize, 'width outside 1..2048 must reject');
});

taskCase('compositing', (check) => {
  const alpha = frameFromASS(ass(4, 4, dialogue(
    '{\\an7\\pos(0,0)\\p1\\1a&H80&}m 0 0 l 4 0 4 4 0 4',
  )), 0.5, 4, 4);
  expectPixel(check, alpha, 1, 1, [255, 0, 0, 127], 'primary BBGGRR and alpha byte');
  expectPixel(check, alpha, 2, 2, [255, 0, 0, 127], 'alpha covers the rectangle interior');

  const resetSource = ass(8, 8, dialogue(
    '{\\an7\\pos(0,0)\\p1}m 0 0 l 2 0 2 2 0 2'
    + '{\\rGreen\\p1}m 4 0 l 6 0 6 2 4 2',
  ), GREEN_STYLE);
  const resetFrame = frameFromASS(resetSource, 0.5, 8, 8);
  expectPixel(check, resetFrame, 1, 1, [255, 0, 0, 255], 'initial slice uses Default style');
  expectPixel(check, resetFrame, 5, 1, [0, 255, 0, 255], 'named reset selects Green slice style');

  const overlapSource = ass(8, 8, [
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&H0000FF&}m 0 0 l 8 0 8 8 0 8'),
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&HFF0000&}m 0 0 l 8 0 8 8 0 8'),
  ]);
  const overlap = frameFromASS(overlapSource, 0.5, 8, 8);
  expectPixel(check, overlap, 4, 4, [85, 0, 170, 191], 'same-layer array order uses source-over');

  const reversedSource = ass(8, 8, [
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&HFF0000&}m 0 0 l 8 0 8 8 0 8'),
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&H0000FF&}m 0 0 l 8 0 8 8 0 8'),
  ]);
  const reversed = frameFromASS(reversedSource, 0.5, 8, 8);
  expectPixel(check, reversed, 4, 4, [170, 0, 85, 191], 'reversing same-layer array order changes top color');

  const layerSource = ass(8, 8, [
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&HFF0000&}m 0 0 l 8 0 8 8 0 8', { layer: 0 }),
    dialogue('{\\an7\\pos(0,0)\\p1\\fad(1000,0)\\1c&H0000FF&}m 0 0 l 8 0 8 8 0 8', { layer: 1 }),
  ]);
  const layered = frameFromASS(layerSource, 0.5, 8, 8);
  expectPixel(check, layered, 4, 4, [170, 0, 85, 191], 'higher layer is composited later');

  const tempDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'render-drawing-test-'));
  const pamPath = path.join(tempDirectory, 'generated.pam');
  execFileSync(process.execPath, [EXAMPLE, FIXTURE, '0.5', pamPath], { cwd: ROOT });
  const pam = fs.readFileSync(pamPath);
  fs.rmSync(tempDirectory, { recursive: true, force: true });
  const endHeader = pam.indexOf(Buffer.from('ENDHDR\n'));
  check(endHeader > 0, 'example writes ENDHDR marker');
  const header = pam.subarray(0, endHeader).toString('ascii');
  for (const line of ['P7', 'WIDTH 8', 'HEIGHT 8', 'DEPTH 4', 'MAXVAL 255', 'TUPLTYPE RGB_ALPHA']) {
    check(header.split('\n').includes(line), `PAM header contains ${line}`);
  }
  const pixels = pam.subarray(endHeader + Buffer.byteLength('ENDHDR\n'));
  check(pixels.length === 8 * 8 * 4, 'PAM contains exactly one RGBA byte per script pixel');
  const pamCenter = [
    pixels[(2 * 8 + 2) * 4],
    pixels[(2 * 8 + 2) * 4 + 1],
    pixels[(2 * 8 + 2) * 4 + 2],
    pixels[(2 * 8 + 2) * 4 + 3],
  ];
  check(JSON.stringify(pamCenter) === JSON.stringify([255, 0, 0, 255]), `PAM center was ${JSON.stringify(pamCenter)}`);
  check(pixels.slice(0, 4).every((value) => value === 0), 'PAM corner is transparent black');

  const badOutput = path.join(tempDirectory, 'missing-directory', 'out.pam');
  const failedWrite = spawnSync(process.execPath, [EXAMPLE, FIXTURE, '0.5', badOutput], { cwd: ROOT });
  check(failedWrite.status !== 0, 'PAM write failure exits non-zero');
});

taskCase('timeline', (check) => {
  const moveSource = ass(8, 8, dialogue(
    '{\\an7\\p1\\move(0,0,8,0,0,1000)}m 0 0 l 2 0 2 2 0 2',
  ));
  const moveHalf = frameFromASS(moveSource, 0.5, 8, 8);
  expectPixel(check, moveHalf, 4, 1, [255, 0, 0, 255], 'move interpolates from the dialogue-relative clock');
  expectPixel(check, moveHalf, 5, 1, [255, 0, 0, 255], 'second moved interior column');
  expectTransparent(check, moveHalf, 0, 1, 'move start has advanced at half time');
  const moveAfter = frameFromASS(moveSource, 1.5, 8, 8);
  expectTransparent(check, moveAfter, 0, 1, 'move clamps to endpoint after t2');

  const wholeDurationMove = frameFromASS(ass(8, 8, dialogue(
    '{\\an7\\p1\\move(0,0,8,0,0,0)}m 0 0 l 2 0 2 2 0 2',
  )), 0.5, 8, 8);
  expectPixel(check, wholeDurationMove, 2, 1, [255, 0, 0, 255], 't1=t2=0 uses the whole dialogue duration');
  expectPixel(check, wholeDurationMove, 3, 1, [255, 0, 0, 255], 'whole-duration second column');

  const relativeSource = ass(8, 8, dialogue(
    '{\\an7\\p1\\move(0,0,8,0,0,1000)}m 0 0 l 2 0 2 2 0 2',
    { start: '0:00:01.00', end: '0:00:03.00' },
  ));
  const relativeMove = frameFromASS(relativeSource, 1.5, 8, 8);
  expectPixel(check, relativeMove, 4, 1, [255, 0, 0, 255], 'move time is relative to dialogue start');

  const fadSource = ass(4, 4, dialogue(
    '{\\an7\\pos(0,0)\\p1\\fad(1000,1000)}m 0 0 l 4 0 4 4 0 4',
  ));
  expectTransparent(check, frameFromASS(fadSource, 0, 4, 4), 1, 1, 'fad factor zero at start');
  expectPixel(check, frameFromASS(fadSource, 0.5, 4, 4), 1, 1, [255, 0, 0, 128], 'fad half-in');
  expectPixel(check, frameFromASS(fadSource, 1, 4, 4), 1, 1, [255, 0, 0, 255], 'fad plateau middle');
  expectPixel(check, frameFromASS(fadSource, 1.5, 4, 4), 1, 1, [255, 0, 0, 128], 'fad half-out');
  expectTransparent(check, frameFromASS(fadSource, 2, 4, 4), 1, 1, 'end time is excluded');

  const fadeSource = ass(4, 4, dialogue(
    '{\\an7\\pos(0,0)\\p1\\fade(0,128,255,0,1000,1000,2000)}m 0 0 l 4 0 4 4 0 4',
  ));
  expectPixel(check, frameFromASS(fadeSource, 0.5, 4, 4), 1, 1, [255, 0, 0, 191], 'four-time fade interpolates alpha bytes');
  expectPixel(check, frameFromASS(fadeSource, 1, 4, 4), 1, 1, [255, 0, 0, 127], 'fade plateau uses middle alpha');
  expectPixel(check, frameFromASS(fadeSource, 1.5, 4, 4), 1, 1, [255, 0, 0, 64], 'fade interpolates toward final alpha');
  expectTransparent(check, frameFromASS(fadeSource, 2, 4, 4), 1, 1, 'fade dialogue right endpoint is inactive');

  for (const unsupportedTag of [
    '\\bord1',
    '\\shad1',
    '\\blur1',
    '\\frx1',
    '\\fry1',
    '\\fax1',
    '\\fay1',
    '\\t(\\frz45)',
  ]) {
    const document = compile(ass(4, 4, dialogue(
      `{\\an7\\pos(0,0)\\p1${unsupportedTag}}m 0 0 l 4 0 4 4 0 4`,
    )));
    let threw = false;
    try {
      renderDrawingFrame(document, 0.5, { width: 4, height: 4 });
    } catch {
      threw = true;
    }
    check(threw, `active ${unsupportedTag} must throw`);
    const inactive = renderDrawingFrame(document, 2, { width: 4, height: 4 });
    check(inactive.data.every((value) => value === 0), `inactive ${unsupportedTag} does not block output`);
  }

  const textDocument = compile(ass(4, 4, dialogue('plain text')));
  let textThrew = false;
  try {
    renderDrawingFrame(textDocument, 0.5, { width: 4, height: 4 });
  } catch {
    textThrew = true;
  }
  check(textThrew, 'active non-empty text must throw');

  const immutableSource = ass(8, 8, dialogue(
    '{\\an7\\pos(1,1)\\p1}m 0 0 l 4 0 4 4 0 4',
  ));
  const document = compile(immutableSource);
  const documentSnapshot = JSON.stringify(document);
  const first = renderDrawingFrame(document, 0.5, { width: 8, height: 8 });
  const firstSnapshot = Array.from(first.data);
  const repeated = renderDrawingFrame(document, 0.5, { width: 8, height: 8 });
  const later = renderDrawingFrame(document, 1.9, { width: 8, height: 8 });
  const rightEnd = renderDrawingFrame(document, 2, { width: 8, height: 8 });
  const earlier = renderDrawingFrame(document, 0.1, { width: 8, height: 8 });
  check(first.data !== repeated.data && first.data !== later.data, 'every call returns independent pixel storage');
  check(JSON.stringify(Array.from(repeated.data)) === JSON.stringify(firstSnapshot), 'repeated query is identical');
  check(JSON.stringify(Array.from(first.data)) === JSON.stringify(firstSnapshot), 'older frame is not rewritten');
  check(JSON.stringify(document) === documentSnapshot, 'rendering does not mutate compiled document');
  expectPixel(check, earlier, 2, 2, [255, 0, 0, 255], 'reverse-order query still renders');
  expectPixel(check, later, 2, 2, [255, 0, 0, 255], 'frame remains active immediately before end');
  expectTransparent(check, rightEnd, 2, 2, 'right endpoint is inactive');

  const roundtripDocument = compile(decompile(document));
  const roundtrip = renderDrawingFrame(roundtripDocument, 0.5, { width: 8, height: 8 });
  check(roundtrip.data.length === first.data.length
    && roundtrip.data.every((value, index) => value === first.data[index]),
  'decompile/compile roundtrip produces the same frame');
});
