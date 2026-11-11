const assert = require('node:assert/strict');
const W = require('../dist/waveform-data.cjs.js');
const a = new Float32Array([0.5, -0.5, 1]);
const b = new Float32Array([0.25, -0.25, -1]);
const audio = { numberOfChannels: 2, length: 3, sampleRate: 48000, getChannelData: c => [a,b][c] };
W.createFromAudio({audio_buffer:audio, scale:2,bits:8,split_channels:true,disable_worker:true}, (e,w)=>{
  if(e) throw e;
  assert.equal(w.channels,2); assert.equal(w.length,2);
  assert.deepEqual(w.channel(0).min_array(),[-64,127]);
  assert.deepEqual(w.channel(0).max_array(),[63,127]);
  assert.deepEqual(w.channel(1).min_array(),[-32,-127]);
  assert.deepEqual(w.channel(1).max_array(),[31,-127]);
  assert.deepEqual(w.resample({scale:4}).channel(1).min_array(),[-127]);
  assert.deepEqual(w.slice({startIndex:1,endIndex:2}).channel(0).max_array(),[127]);
  assert.deepEqual([...a],[0.5,-0.5,1]);
  console.log('PASS: stereo 8-bit full/tail bins, resample, slice, source unchanged');
});
