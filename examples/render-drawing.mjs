#!/usr/bin/env node
import fs from 'node:fs';
import process from 'node:process';
import { compile, renderDrawingFrame } from '../src/index.js';

/**
 * Render one drawing-only ASS frame to a P7 PAM file.
 *
 * Usage: node examples/render-drawing.mjs input.ass seconds output.pam
 */

function usage(message) {
  if (message) process.stderr.write(`${message}\n`);
  process.stderr.write('usage: render-drawing.mjs input.ass seconds output.pam\n');
  process.exit(2);
}

const [input, secondsArg, output] = process.argv.slice(2);
if (!input || !secondsArg || !output) usage();
const seconds = Number(secondsArg);
if (!Number.isFinite(seconds)) usage(`invalid seconds: ${secondsArg}`);

let source;
try {
  source = fs.readFileSync(input, 'utf8');
} catch (error) {
  process.stderr.write(`cannot read ${input}: ${error.message}\n`);
  process.exit(1);
}

const document = compile(source);
if (!(document.width > 0) || !(document.height > 0)) {
  process.stderr.write('the script needs a positive PlayResX and PlayResY\n');
  process.exit(1);
}

const frame = renderDrawingFrame(document, seconds, {
  width: document.width,
  height: document.height,
});

const header = [
  'P7',
  `WIDTH ${frame.width}`,
  `HEIGHT ${frame.height}`,
  'DEPTH 4',
  'MAXVAL 255',
  'TUPLTYPE RGB_ALPHA',
  'ENDHDR',
  '',
].join('\n');

try {
  fs.writeFileSync(output, Buffer.concat([
    Buffer.from(header, 'ascii'),
    Buffer.from(frame.data.buffer, frame.data.byteOffset, frame.data.byteLength),
  ]));
} catch (error) {
  process.stderr.write(`cannot write ${output}: ${error.message}\n`);
  process.exit(1);
}
