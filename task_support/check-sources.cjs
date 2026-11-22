'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
// Parse and execute the same source files used by the JS playback chain.
const names=['buffer.js','decoder.js','ts.js','mpeg1.js','mp2.js','webaudio.js','player.js'];
const scope={JSMpeg:{Decoder:{},Demuxer:{},AudioOutput:{},Renderer:{},Source:{},Now:()=>0},console};
vm.createContext(scope);
for(const name of names) {
 const source=fs.readFileSync('src/'+name,'utf8');assert.ok(source.trim().length>0,name);
 new vm.Script(source,{filename:name}).runInContext(scope);
}
for(const C of [scope.JSMpeg.Decoder.Base,scope.JSMpeg.Demuxer.TS,scope.JSMpeg.Decoder.MPEG1Video,scope.JSMpeg.Decoder.MP2Audio,scope.JSMpeg.AudioOutput.WebAudio,scope.JSMpeg.Player])assert.equal(typeof C,'function');
console.log('PRODUCTION_SOURCE_LOAD_OK');
