'use strict';
const assert=require('node:assert/strict');
const WebVTT=require('../lib/vtt.js');
const VTTCue=require('../lib/vttcue.js');
const VTTRegion=require('../lib/vttregion.js');
const parser=new WebVTT.Parser({VTTCue,VTTRegion},WebVTT.StringDecoder());
const cues=[];parser.oncue=c=>cues.push(c);parser.onparsingerror=e=>{throw e;};
parser.parse('WEBVTT\nRegion: id=r width=45% lines=2\n\nsame\n00:01.000 --> 00:02.500 region:r line:auto\n<b>A</b>\n\nsame\n00:03.000 --> 00:04.000 region:r\nB\n\n');parser.flush();
assert.equal(cues.length,2);assert.equal(cues[0].text,'<b>A</b>');assert.equal(cues[0].startTime,1);assert.equal(cues[0].endTime,2.5);assert.equal(cues[0].id,cues[1].id);assert.equal(cues[0].region,cues[1].region);assert.equal(cues[0].region.width,45);
cues[0].hasBeenReset=false;cues[0].startTime=1.25;assert.equal(cues[0].hasBeenReset,true);
console.log('NORMAL cues=2 duplicate-id=same region-shared=true width=45');
console.log('SETTER start=1.25 reset=true text=<b>A</b>');
assert.equal(typeof WebVTT.BilingualAlignment,'undefined');console.log('GAP bilingual-alignment=absent');
try { require('../lib/index.js');console.log('INDEX unexpectedly-loadable');process.exitCode=2; } catch(e) { if(!(e instanceof TypeError))throw e;console.log('INDEX legacy-export=TypeError'); }
