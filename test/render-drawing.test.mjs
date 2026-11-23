import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { compile, decompile, renderDrawingFrame } from '../src/index.js';
import { taskCase } from '../task_support/check.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));

const HEAD = `[Script Info]
PlayResX: 16
PlayResY: 16
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, Outline, Shadow, Alignment, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H000000FF,0,0,7,0,0,0
Style: Alternate,Arial,20,&H00FF0000,0,0,7,0,0,0
[Events]
Format: Layer, Start, End, Style, Text
`;

function script(dialogues) {
  return `${HEAD}${dialogues.join('\n')}\n`;
}

function dialogue(layer, start, end, text, style = 'Default') {
  return `Dialogue: ${layer},${start},${end},${style},${text}`;
}

const t = (ms) => {
  const seconds = ms / 1000;
  const whole = Math.floor(seconds);
  const centis = Math.round((seconds - whole) * 100);
  return `0:00:${String(whole).padStart(2, '0')}.${String(centis).padStart(2, '0')}`;
};

function render(text, seconds, size = 16) {
  const document = compile(text);
  return renderDrawingFrame(document, seconds, { width: size, height: size });
}

function pixel(frame, x, y) {
  const offset = (y * frame.width + x) * 4;
  return Array.from(frame.data.slice(offset, offset + 4));
}

function alphaAt(frame, x, y) {
  return pixel(frame, x, y)[3];
}

function equal(a, b) {
  return a.length === b.length && a.every((value, index) => value === b[index]);
}

function everyPixel(frame, predicate, message) {
  for (let y = 0; y < frame.height; y++) {
    for (let x = 0; x < frame.width; x++) {
      assert.ok(predicate(pixel(frame, x, y), x, y), `${message} at (${x}, ${y})`);
    }
  }
}

const CIRCLE = 'm 8 4 b 10.209 4 12 5.791 12 8 b 12 10.209 10.209 12 8 12 '
  + 'b 5.791 12 4 10.209 4 8 b 4 5.791 5.791 4 8 4';

taskCase('geometry', (check) => {
  // A reversed inner contour punches a hole; a co-directional one stays filled.
  // The an7 anchor places the drawing's top-left corner at `\pos`, so drawing
  // coordinates that start at the origin map straight to output pixels.
  const hole = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 6 0 6 6 0 6 m 1 1 l 1 5 5 5 5 1'),
  ]), 0.5);
  check(alphaAt(hole, 3, 3) === 0, 'reversed inner contour must carve a hole');
  check(equal(pixel(hole, 3, 3), [0, 0, 0, 0]), 'hole RGB stays zero');
  check(alphaAt(hole, 0, 3) === 255, 'left ring pixel is opaque');
  check(alphaAt(hole, 5, 3) === 255, 'right ring pixel is opaque');
  check(alphaAt(hole, 3, 0) === 255, 'top ring pixel is opaque');
  check(alphaAt(hole, 6, 3) === 0, 'outside the outer contour is transparent');
  check(alphaAt(hole, 1, 1) === 0, 'the hole corner is empty');
  const filled = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 6 0 6 6 0 6 m 1 1 l 5 1 5 5 1 5'),
  ]), 0.5);
  check(alphaAt(filled, 3, 3) === 255, 'same-direction inner contour stays filled');
  check(alphaAt(filled, 1, 1) === 255, 'same-direction contour covers its corner');

  // A cubic circle; the box is 4..12, and `\pos(4,4)` puts that box corner at
  // output (4,4), so the circle centre lands on (8,8).
  const curve = render(script([
    dialogue(0, t(0), t(2000), `{\\an7\\pos(4,4)\\p1}${CIRCLE}`),
  ]), 0.5);
  check(equal(pixel(curve, 8, 8), [255, 0, 0, 255]), 'curve centre is opaque red');
  check(alphaAt(curve, 4, 4) === 0, 'curve bounding-box corner is empty');
  check(alphaAt(curve, 3, 8) === 0, 'left of the circle is empty');
  check(alphaAt(curve, 4, 8) === 255, 'inside the left edge is opaque');
  check(alphaAt(curve, 11, 8) === 255, 'inside the right edge is opaque');
  check(alphaAt(curve, 12, 8) === 0, 'right of the circle is empty');
  check(alphaAt(curve, 12, 12) === 0, 'bottom-right of the circle is empty');
  // The top row crosses the curve diagonally: coverage rises from the empty
  // corner through two partial pixels to the fully covered top of the circle.
  check(alphaAt(curve, 4, 4) === 0, 'curve corner pixel is fully empty');
  check(alphaAt(curve, 5, 4) === 48, 'curve edge pixel covers 3 of 16 samples');
  check(alphaAt(curve, 6, 4) === 175, 'curve edge pixel covers 11 of 16 samples');
  check(alphaAt(curve, 7, 4) === 255, 'curve top becomes fully covered');
  check(alphaAt(curve, 5, 5) === 239, 'diagonal curve pixel covers 15 of 16 samples');

  // `p` scale divides drawing coordinates by 2^(p-1); `pbo` shifts the local y
  // of the drawing after that division, so the box anchor follows the drawing.
  const p2 = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p2}m 2 2 l 16 2 16 16 2 16'),
  ]), 0.5);
  check(alphaAt(p2, 0, 0) === 255, 'p2 drawing top-left is anchored at pos');
  check(alphaAt(p2, 3, 3) === 255, 'p2 interior is filled');
  check(alphaAt(p2, 6, 6) === 255, 'p2 interior ends before 7');
  check(alphaAt(p2, 7, 7) === 0, 'p2 drawing is 7x7 after scaling');
  check(alphaAt(p2, 8, 8) === 0, 'p2 drawing never reaches 8');

  // Drawing edges and clip edges are intersected at the same sub-samples.
  // The drawing stops at x=3.25 and the clip at x=3.75: only the sample column
  // at x=3.125 counts, i.e. 4 of 16 samples -> round(4 / 16 * 255) = 64.
  const intersect = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\clip(0,0,3.75,16)\\p1}m 0 0 l 3.25 0 3.25 16 0 16'),
  ]), 0.5);
  check(alphaAt(intersect, 3, 4) === 64, 'drawing/clip edges intersect per sub-sample');
  check(alphaAt(intersect, 2, 4) === 255, 'well inside both edges is opaque');
  check(alphaAt(intersect, 4, 4) === 0, 'outside both edges is empty');

  // A drawing clip keeps script coordinates; it is not moved or rotated with
  // the dialogue. The drawing is a 12x12 square centred on `\pos(4,4)`, so it
  // spans -2..10 after the 45 degree rotation and still covers the clip area.
  const clipped = render(script([
    dialogue(0, t(0), t(2000), '{\\an5\\pos(4,4)\\frz45\\clip(0,0,6,6)\\p1}m -6 -6 l 6 -6 6 6 -6 6'),
  ]), 0.5);
  check(alphaAt(clipped, 0, 0) === 255, 'script-space clip starts at the origin');
  check(alphaAt(clipped, 5, 5) === 255, 'script-space clip covers its interior');
  check(alphaAt(clipped, 6, 6) === 0, 'script-space clip is half-open at 6,6');
  check(alphaAt(clipped, 6, 1) === 0, 'script-space clip does not follow the drawing');

  // A drawing clip is scaled by its own `\clip(scale, ...)` factor only, the
  // same 2^(scale-1) division as `p`: `\clip(2, m 1 1 l 2 1 2 2 1 2)` covers
  // script coordinates 0.5..1, i.e. 4 of 16 samples of pixel (0,0).
  const scaledClip = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\clip(2,m 1 1 l 2 1 2 2 1 2)\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]), 0.5);
  check(alphaAt(scaledClip, 0, 0) === 64, 'clip(2,...) covers 0.5..1 of pixel 0,0');
  check(alphaAt(scaledClip, 1, 0) === 0, 'clip(2,...) stops after 1');
  check(alphaAt(scaledClip, 0, 1) === 0, 'clip(2,...) stops below 1');

  // An inverted rectangle clip keeps the complement of [2,6)x[2,6).
  const inverted = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\iclip(2,2,6,6)\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]), 0.5);
  check(alphaAt(inverted, 3, 3) === 0, 'iclip removes the rectangle interior');
  check(alphaAt(inverted, 1, 3) === 255, 'iclip keeps the left side');
  check(alphaAt(inverted, 6, 3) === 255, 'iclip rectangle is half-open on the right');
  check(alphaAt(inverted, 7, 7) === 255, 'iclip keeps the far corner');

  // fscx/fscy scale the drawing around the local origin after anchoring.
  const scaled = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\fscx50\\fscy50\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]), 0.5);
  check(alphaAt(scaled, 3, 3) === 255, 'fscx/fscy shrink the drawing');
  check(alphaAt(scaled, 4, 4) === 0, 'scaled drawing ends at 4');

  // The public entry is pure: it never rewrites the compiled document, and
  // every call returns a fresh buffer.
  const source = script([
    dialogue(0, t(0), t(2000), '{\\an7\\move(0,0,8,0,0,0)\\fad(500,500)\\p1}m 0 0 l 8 0 8 8 0 8 m 1 1 l 1 7 7 7 7 1'),
  ]);
  const document = compile(source);
  const before = JSON.stringify(document);
  const first = renderDrawingFrame(document, 0.25, { width: 16, height: 16 });
  const second = renderDrawingFrame(document, 0.25, { width: 16, height: 16 });
  check(JSON.stringify(document) === before, 'rendering must not mutate the document');
  check(first.data !== second.data, 'each call returns an independent array');
  first.data[0] = 99;
  check(second.data[0] === 0, 'frames do not share storage');

  // Recompiling the decompiled source produces identical frames.
  const roundTrip = compile(decompile(JSON.parse(before)));
  for (const seconds of [0, 0.25, 0.75, 1.5, 1.999]) {
    const a = renderDrawingFrame(document, seconds, { width: 16, height: 16 });
    const b = renderDrawingFrame(roundTrip, seconds, { width: 16, height: 16 });
    check(Buffer.compare(Buffer.from(a.data), Buffer.from(b.data)) === 0,
      `decompile/compile round trip matches at ${seconds}s`);
  }
  const endpoint = renderDrawingFrame(document, 2, { width: 16, height: 16 });
  check(endpoint.data.every((value) => value === 0), 'right endpoint is inactive');

  // The example CLI writes a P7 PAM whose payload is this very frame.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ass-render-'));
  try {
    const input = path.join(dir, 'input.ass');
    const output = path.join(dir, 'output.pam');
    const exampleScript = script([
      dialogue(0, t(0), t(2000), '{\\an7\\pos(2,2)\\p1}m 0 0 l 4 0 4 4 0 4'),
    ]);
    fs.writeFileSync(input, exampleScript);
    execFileSync(process.execPath, [
      path.join(ROOT, 'examples/render-drawing.mjs'), input, '0.5', output,
    ], { cwd: ROOT, stdio: 'pipe' });
    const pam = fs.readFileSync(output);
    const marker = Buffer.from('ENDHDR\n');
    const end = pam.indexOf(marker);
    check(end > 0, 'example writes the PAM ENDHDR marker');
    const header = pam.subarray(0, end).toString('latin1').split('\n');
    check(header[0] === 'P7', 'PAM starts with P7');
    check(header.includes('WIDTH 16'), 'PAM width follows PlayRes');
    check(header.includes('HEIGHT 16'), 'PAM height follows PlayRes');
    check(header.includes('DEPTH 4'), 'PAM depth is 4');
    check(header.includes('MAXVAL 255'), 'PAM maxval is 255');
    check(header.includes('TUPLTYPE RGB_ALPHA'), 'PAM tuple type is RGB_ALPHA');
    check(pam.length === end + marker.length + 16 * 16 * 4, 'PAM payload length matches');
    const expected = render(exampleScript, 0.5);
    const payload = pam.subarray(end + marker.length);
    check(Buffer.compare(payload, Buffer.from(expected.data)) === 0,
      'PAM payload is the rendered frame');
    check(equal(Array.from(payload.slice((2 * 16 + 2) * 4, (2 * 16 + 2) * 4 + 4)), [255, 0, 0, 255]),
      'example drawing centre is opaque red');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

taskCase('compositing', (check) => {
  // Two half-transparent drawings. `\pos(4,4)` shifts the second one so the
  // 4..8 overlap is composited source-over in premultiplied space.
  const overlap = script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\1a&H80&\\p1}m 0 0 l 8 0 8 8 0 8'),
    dialogue(0, t(0), t(2000), '{\\an7\\pos(4,4)\\1c&HFF0000&\\1a&H80&\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]);
  const blended = render(overlap, 0.5);
  // opacity o = 127/255: alpha = 2o - o^2 -> 191, straight red = (1-o)/(2-o)
  // -> 85, straight blue = 1/(2-o) -> 170.
  check(equal(pixel(blended, 6, 6), [85, 0, 170, 191]), 'semi-transparent overlay blends');
  check(equal(pixel(blended, 2, 2), [255, 0, 0, 127]), 'single overlay keeps its alpha');
  check(equal(pixel(blended, 10, 10), [0, 0, 255, 127]), 'second overlay outside the first');
  check(equal(pixel(blended, 2, 12), [0, 0, 0, 0]), 'transparent RGB stays zero');

  // Same layer: later dialogue in the array sits on top.
  const sameLayer = render(script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 8 0 8 8 0 8'),
    dialogue(0, t(0), t(2000), '{\\an7\\pos(4,4)\\1c&HFF0000&\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]), 0.5);
  check(equal(pixel(sameLayer, 6, 6), [0, 0, 255, 255]), 'later same-layer dialogue wins');
  check(equal(pixel(sameLayer, 2, 2), [255, 0, 0, 255]), 'earlier same-layer dialogue below');

  // A higher layer wins even when it appears earlier in the array.
  const layered = render(script([
    dialogue(1, t(0), t(2000), '{\\an7\\pos(4,4)\\1c&HFF0000&\\p1}m 0 0 l 8 0 8 8 0 8'),
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]), 0.5);
  check(equal(pixel(layered, 6, 6), [0, 0, 255, 255]), 'higher layer composites on top');
  check(equal(pixel(layered, 2, 2), [255, 0, 0, 255]), 'lower layer is covered by the top');

  // Style defaults and named resets provide the static tag values.
  const reset = render(script([
    dialogue(0, t(0), t(2000),
      '{\\an7\\pos(0,0)\\p1}m 0 0 l 4 0 4 4 0 4{\\rAlternate\\p1}m 4 4 l 8 4 8 8 4 8'),
  ]), 0.5);
  check(equal(pixel(reset, 1, 1), [255, 0, 0, 255]), 'drawing before reset uses Default');
  check(equal(pixel(reset, 5, 5), [0, 0, 255, 255]), 'named reset switches to Alternate');

  // Static fragment overrides are inherited by the following fragments.
  const inherited = render(script([
    dialogue(0, t(0), t(2000),
      '{\\an7\\pos(0,0)\\p1}m 0 0 l 4 0 4 4 0 4{\\1c&HFF0000&\\p1}m 4 4 l 8 4 8 8 4 8'),
  ]), 0.5);
  check(equal(pixel(inherited, 1, 1), [255, 0, 0, 255]), 'first fragment keeps the main colour');
  check(equal(pixel(inherited, 5, 5), [0, 0, 255, 255]), 'override applies to the next fragment');

  // Active non-drawing content outside the interval must not block output.
  const inactive = script([
    dialogue(0, t(0), t(1000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 4 0 4 4 0 4'),
    dialogue(0, t(1000), t(2000), 'plain text is out of scope'),
    dialogue(0, t(1000), t(2000), '{\\bord2\\p1}m 0 0 l 4 0 4 4 0 4'),
  ]);
  const inactiveFrame = render(inactive, 0.5);
  check(alphaAt(inactiveFrame, 1, 1) === 255, 'active drawing renders while inactive text is ignored');
  check(alphaAt(inactiveFrame, 13, 13) === 0, 'inactive effects do not render');

  // Active unsupported effects and text are reported explicitly.
  const failures = [
    script([dialogue(0, t(0), t(2000), '{\\p1}m 0 0 l 4 0 4 4 0 4{\\p0}hello')]),
    script([dialogue(0, t(0), t(2000), '{\\bord2\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\shad2\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\blur2\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\frx20\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\fry20\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\fax20\\p1}m 0 0 l 4 0 4 4 0 4')]),
    script([dialogue(0, t(0), t(2000), '{\\t(0,500,\\fs30)\\p1}m 0 0 l 4 0 4 4 0 4')]),
  ];
  for (let i = 0; i < failures.length; i++) {
    let threw = false;
    try {
      render(failures[i], 0.5);
    } catch (error) {
      threw = /not supported/.test(error.message);
    }
    check(threw, `unsupported active content ${i} throws`);
  }

  // The output is scaled from PlayRes to width/height; a script without a
  // positive PlayRes, or an invalid size, is rejected.
  const tallScript = `[Script Info]
PlayResX: 8
PlayResY: 16
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, Outline, Shadow, Alignment, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H000000FF,0,0,7,0,0,0
[Events]
Format: Layer, Start, End, Style, Text
${dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 4 0 4 4 0 4')}
`;
  const tallDocument = compile(tallScript);
  const scaledFrame = renderDrawingFrame(tallDocument, 0.5, { width: 16, height: 32 });
  check(scaledFrame.width === 16 && scaledFrame.height === 32, 'output size follows the request');
  check(alphaAt(scaledFrame, 0, 0) === 255, 'drawing scales with the canvas');
  check(alphaAt(scaledFrame, 7, 7) === 255, 'scaled drawing keeps its corner');
  check(alphaAt(scaledFrame, 8, 8) === 0, 'scaled drawing keeps its extent');
  for (const size of [{ width: 0, height: 8 }, { width: 8, height: 8.5 }, { width: 2049, height: 8 }]) {
    let threw = false;
    try {
      renderDrawingFrame(tallDocument, 0.5, size);
    } catch (error) {
      threw = true;
    }
    check(threw, `invalid size ${JSON.stringify(size)} is rejected`);
  }
  const noRes = compile(`[Script Info]
[Events]
Format: Layer, Start, End, Style, Text
${dialogue(0, t(0), t(2000), '{\\p1}m 0 0 l 4 0 4 4 0 4')}
`);
  let threwNoRes = false;
  try {
    renderDrawingFrame(noRes, 0.5, { width: 8, height: 8 });
  } catch (error) {
    threwNoRes = /PlayRes/.test(error.message);
  }
  check(threwNoRes, 'a missing PlayRes is rejected');
});

taskCase('timeline', (check) => {
  // `move` interpolates on the dialogue's own clock; t1 = t2 = 0 spans it all.
  const moving = script([
    dialogue(0, t(0), t(2000), '{\\an7\\move(0,0,8,0,0,0)\\p1}m 0 0 l 2 0 2 2 0 2'),
  ]);
  const startFrame = render(moving, 0);
  check(alphaAt(startFrame, 0, 0) === 255, 'move starts at the first position');
  check(alphaAt(startFrame, 4, 0) === 0, 'move has not advanced at the start');
  const midFrame = render(moving, 1);
  check(alphaAt(midFrame, 4, 0) === 255, 'move is halfway at the midpoint');
  check(alphaAt(midFrame, 0, 0) === 0, 'the first position is left behind');
  const nearEnd = render(moving, 1.75);
  check(alphaAt(nearEnd, 7, 0) === 255, 'move approaches the last position');

  // An explicit move interval clamps outside [t1, t2).
  const clamped = script([
    dialogue(0, t(0), t(2000), '{\\an7\\move(0,0,8,0,500,1500)\\p1}m 0 0 l 2 0 2 2 0 2'),
  ]);
  check(alphaAt(render(clamped, 0.25), 0, 0) === 255, 'move clamps before t1');
  check(alphaAt(render(clamped, 0.25), 4, 0) === 0, 'move is still at the start before t1');
  check(alphaAt(render(clamped, 1), 4, 0) === 255, 'move interpolates between t1 and t2');
  check(alphaAt(render(clamped, 1.75), 8, 0) === 255, 'move clamps after t2');
  check(alphaAt(render(clamped, 1.75), 4, 0) === 0, 'move leaves the middle after t2');

  // fad(fade-in, fade-out) runs relative to the dialogue start.
  const fading = script([
    dialogue(0, t(1000), t(3000), '{\\an7\\pos(0,0)\\fad(1000,1000)\\p1}m 0 0 l 4 0 4 4 0 4'),
  ]);
  check(alphaAt(render(fading, 0.5), 1, 1) === 0, 'dialogue is inactive before its start');
  check(alphaAt(render(fading, 1.25), 1, 1) === 64, 'fade-in is quarter way at 250ms');
  check(alphaAt(render(fading, 1.5), 1, 1) === 128, 'fade-in is halfway at 500ms');
  check(alphaAt(render(fading, 2), 1, 1) === 255, 'fully opaque in the middle');
  check(alphaAt(render(fading, 2.5), 1, 1) === 128, 'fade-out is halfway at 500ms left');
  check(alphaAt(render(fading, 2.75), 1, 1) === 64, 'fade-out is quarter way with 250ms left');
  check(alphaAt(render(fading, 3), 1, 1) === 0, 'dialogue is inactive at its right endpoint');

  // A three-value `fade` walks through its alpha plateaus.
  const blockFade = script([
    dialogue(0, t(0), t(2000),
      '{\\an7\\pos(0,0)\\fade(0,255,0,0,500,1500,2000)\\p1}m 0 0 l 4 0 4 4 0 4'),
  ]);
  check(alphaAt(render(blockFade, 0.25), 1, 1) === 128, 'fade ramp reaches half opacity');
  check(alphaAt(render(blockFade, 1), 1, 1) === 0, 'fade plateau uses the middle alpha');
  check(alphaAt(render(blockFade, 1.75), 1, 1) === 128, 'fade ramp back to the last alpha');
  check(alphaAt(render(blockFade, 1.999), 1, 1) === 254, 'fade approaches the final alpha');

  // The right endpoint of [start, end) is exclusive for every dialogue.
  const endpointScript = script([
    dialogue(0, t(0), t(2000), '{\\an7\\pos(0,0)\\p1}m 0 0 l 8 0 8 8 0 8'),
  ]);
  const endpoint = render(endpointScript, 2);
  check(endpoint.data.every((value) => value === 0), 'right endpoint renders nothing');
  const justBefore = render(endpointScript, 1.999);
  check(alphaAt(justBefore, 4, 4) === 255, 'just before the right endpoint is active');
  everyPixel(endpoint, (value) => value.every((channel) => channel === 0),
    'inactive frame is fully transparent');
});
