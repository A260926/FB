#!/usr/bin/env node
import fs from 'node:fs';
import { compile, renderDrawingFrame } from '../src/index.js';

function main() {
  const [inputPath, secondsText, outputPath] = process.argv.slice(2);
  if (!inputPath || secondsText === undefined || !outputPath) {
    throw new Error('usage: render-drawing.mjs input.ass seconds output.pam');
  }

  const source = fs.readFileSync(inputPath, 'utf8');
  const document = compile(source);
  const width = document.width;
  const height = document.height;
  if (!(width > 0) || !(height > 0)) {
    throw new Error('the ASS file must declare positive PlayResX and PlayResY');
  }

  const seconds = Number(secondsText);
  const frame = renderDrawingFrame(document, seconds, { width, height });
  const header = Buffer.from(
    [
      'P7',
      `WIDTH ${frame.width}`,
      `HEIGHT ${frame.height}`,
      'DEPTH 4',
      'MAXVAL 255',
      'TUPLTYPE RGB_ALPHA',
      'ENDHDR',
      '',
    ].join('\n'),
    'ascii',
  );
  fs.writeFileSync(outputPath, Buffer.concat([header, Buffer.from(frame.data)]));
}

try {
  main();
} catch (error) {
  console.error(error?.message || error);
  process.exitCode = 1;
}
