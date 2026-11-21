#!/usr/bin/env node
'use strict';

// Align the cues of two WebVTT subtitle files into a bilingual association
// model, or conform one track of a saved model to the other.
//
// Usage:
//   node examples/bilingual.cjs align LEFT.vtt RIGHT.vtt OUT.json
//     Parse both files with the production WebVTT.Parser (UTF-8), assign
//     keys l1, l2, ... / r1, r2, ... in cue order and save the aligned
//     BilingualAlignment model as JSON.
//   node examples/bilingual.cjs conform IN.json GROUP OUT.json [--ripple]
//     Restore a saved model (possibly in another process), conform the
//     given group index and save the updated model.
//
// The output file is only written once parsing, solving and serialization
// have all succeeded; on any error the exit code is non-zero and an
// existing output file keeps its previous bytes.

const fs = require('fs');
const {
  WebVTT,
  VTTCue,
  VTTRegion,
  BilingualAlignment
} = require('../lib/index.js');

const USAGE =
  'Usage:\n' +
  '  node examples/bilingual.cjs align LEFT.vtt RIGHT.vtt OUT.json\n' +
  '  node examples/bilingual.cjs conform IN.json GROUP OUT.json [--ripple]';

// Parse one WebVTT file with the production parser and return its cues as
// {key, cue} entries keyed prefix1, prefix2, ... in callback order.
function parseVttFile(file, prefix) {
  const data = fs.readFileSync(file);
  const parser = new WebVTT.Parser({ VTTCue, VTTRegion });
  const cues = [];
  const errors = [];
  parser.oncue = (cue) => cues.push(cue);
  parser.onparsingerror = (error) => errors.push(error);
  parser.parse(data);
  parser.flush();
  if (errors.length) {
    throw new Error(file + ': ' + errors[0].message);
  }
  return cues.map((cue, index) => ({ key: prefix + (index + 1), cue }));
}

function readModel(file) {
  const document = JSON.parse(fs.readFileSync(file, 'utf8'));
  return BilingualAlignment.fromJSON(document);
}

// Serialize first, then write, so a failure never truncates an existing
// output file.
function writeModel(file, model) {
  const json = JSON.stringify(model.toJSON(), null, 2) + '\n';
  fs.writeFileSync(file, json);
}

function main(argv) {
  const [command, ...args] = argv;
  if (command === 'align') {
    if (args.length !== 3) {
      throw new Error(USAGE);
    }
    const [leftFile, rightFile, outFile] = args;
    const left = parseVttFile(leftFile, 'l');
    const right = parseVttFile(rightFile, 'r');
    writeModel(outFile, BilingualAlignment.align(left, right));
    return;
  }
  if (command === 'conform') {
    let ripple = false;
    const positional = [];
    for (const arg of args) {
      if (arg === '--ripple') {
        ripple = true;
      } else {
        positional.push(arg);
      }
    }
    if (positional.length !== 3) {
      throw new Error(USAGE);
    }
    const [inFile, groupArg, outFile] = positional;
    if (!/^-?\d+$/.test(groupArg)) {
      throw new Error('GROUP must be an integer, got: ' + groupArg);
    }
    const model = readModel(inFile);
    model.conform(Number(groupArg), ripple);
    writeModel(outFile, model);
    return;
  }
  throw new Error(USAGE);
}

try {
  main(process.argv.slice(2));
} catch (error) {
  console.error('bilingual: ' + error.message);
  process.exitCode = 1;
}
