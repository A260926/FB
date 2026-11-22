"use strict";
const assert=require("node:assert/strict"),fs=require("node:fs"),vm=require("node:vm");
const {stream}=require("./fixture.cjs"),{packets}=require("./transport-fixture.cjs");
const sourceFiles=["buffer.js","decoder.js","ts.js","mpeg1.js","mp2.js","webaudio.js","player.js"];
function setup(sync){
 let now=0;const rendered=[];
 const J={Decoder:{},Demuxer:{},AudioOutput:{},Renderer:{},Source:{},Now:()=>now,Fill:(a,v)=>a.fill(v)};
 const scope=vm.createContext({JSMpeg:J,console,requestAnimationFrame(){return 1;},cancelAnimationFrame(){},performance:{now:()=>now*1000}});
 for(const name of sourceFiles)vm.runInContext(fs.readFileSync("src/"+name,"utf8"),scope,{filename:name});
 // Canvas is only a pixel/time recorder; decoding and all scheduling stay in production classes.
 J.Renderer.Canvas2D=class{resize(w,h){assert.equal(w,16);assert.equal(h,16);}render(y,cr,cb){for(const p of [y,cr,cb])assert.ok(p.length&&p.every(x=>x===128));rendered.push(now);}renderProgress(){}destroy(){}};
 class Source{constructor(){this.streaming=true;this.established=true;this.progress=1;}connect(d){this.destination=d;}start(){}destroy(){}}
 const player=new J.Player("",{source:Source,streaming:true,disableWebAssembly:true,disableGl:true,video:true,audio:false,pauseWhenHidden:false,decodeFirstFrame:false,syncToPTS:sync,loop:false,videoBufferSize:4096});
 const a=stream([1]),b=stream([2]);
 const data=Buffer.concat([packets(256,0xe0,90000,a),packets(256,0xe0,108000,b,1)]);
 for(let at=0;at<data.length;at+=7)player.source.destination.write(data.subarray(at,at+7));
 return {J,player,rendered,tick(t){now=t;player.update();}};
}
// Existing path is a control: the same legal PES and production decoder actually render both frames.
const normal=setup(false);normal.tick(0);normal.tick(0.001);
assert.equal(normal.rendered.length,2,"legal one-frame PES must decode on existing Player path");
console.log("PTS_EXISTING_PATH_CONTROL_OK frames=2 actual_pixels=128");
const timed=setup(true);assert.equal(timed.rendered.length,0);
timed.tick(0);assert.equal(timed.rendered.length,0,"syncToPTS must not render before 100ms wall start");
assert.equal(timed.player.currentTime,0);
timed.tick(0.099);assert.equal(timed.rendered.length,0,"pre-start update must preserve compressed frame");
timed.tick(0.101);assert.equal(timed.rendered.length,1,"first PTS is due after wall start");
timed.tick(0.299);assert.equal(timed.rendered.length,1,"PTS gap must retain previous picture");
timed.tick(0.301);assert.equal(timed.rendered.length,2,"second PTS must render at its own media time");
timed.tick(1);assert.equal(timed.rendered.length,2,"empty update must not render another picture");
console.log("REQUIRED_PTS_PLAYER_OK first=0.101 second=0.301 gap=0.2");
