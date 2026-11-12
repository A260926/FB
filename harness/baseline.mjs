// Current-behaviour witness only; this file does not implement target geometry.
import assert from 'node:assert/strict';
import {PNG} from 'pngjs';
import {openFixture} from './fixture.mjs';

function marker(buffer, allowEmpty=false) {
  const png = PNG.sync.read(Buffer.from(buffer));
  let left = png.width, top = png.height, right = -1, bottom = -1, count = 0;
  for (let y=0;y<png.height;y++) for(let x=0;x<png.width;x++) {
    const i=(y*png.width+x)*4;
    if(png.data[i]===10 && png.data[i+1]===240 && png.data[i+2]===60) {
      count++; left=Math.min(left,x);right=Math.max(right,x);top=Math.min(top,y);bottom=Math.max(bottom,y);
    }
  }
  if(allowEmpty && count===0) return {count:0};
  assert(count>100, 'rendered target marker missing');
  return {x:(left+right+1)/2,y:(top+bottom+1)/2,width:right-left+1,height:bottom-top+1,count};
}

const f=await openFixture();
try {
  console.log('BROWSER', await f.browser.version());
  for (const remote of [0,1]) for (const affine of [0,1]) {
    await f.page.goto(`${f.url}?remote=${remote}&affine=${affine}`, {waitUntil:'networkidle0'});
    const leaf = f.page.frames().find(frame => frame.url().includes('/leaf'));
    assert(leaf, 'leaf frame missing');
    assert.equal(leaf.client !== leaf.parentFrame().client, Boolean(remote), 'expected frame process boundary');
    const element = await leaf.$('#target');
    assert(element);
    const raster = marker(await f.page.screenshot());
    const reported = await element.boundingBox();
    assert(reported);
    const point = await element.clickablePoint();
    await element.click();
    const clicks = await leaf.evaluate(()=>window.clicks);
    const delta = Math.hypot(point.x-raster.x,point.y-raster.y);
    if(!affine) {
      assert.equal(clicks,1);
      assert(delta<=1);
      assert.equal(reported.width,raster.width);
      console.log(`BASELINE identity remote=${remote} click=1 raster-aligned`);
    } else {
      assert.equal(clicks,0,'baseline must continue to witness the original affine miss');
      assert(delta>20);
      const crop = marker(await element.screenshot({scrollIntoView:false}),true).count;
      // The original crop cuts out most of the painted target (at most 25%).
      assert(crop < raster.count/4);
      console.log(`BASELINE affine remote=${remote} click=0 coordinate-error=${delta.toFixed(2)} crop-marker=${crop}/${raster.count}`);
    }
    console.log(JSON.stringify({remote,affine,oopif:leaf.client!==leaf.parentFrame().client,raster,reported,point}));
    await element.dispose();
  }
  await f.page.close();
  f.page=await f.browser.newPage();
  await f.page.bringToFront();
  await f.page.setViewport({width:1000,height:700,deviceScaleFactor:1});
  await f.page.goto(`${f.url}?remote=0&affine=0`,{waitUntil:'networkidle0'});
  const leaf=f.page.frames().find(frame=>frame.url().includes('/leaf'));
  const originalClick=f.page.mouse.click.bind(f.page.mouse);
  let observed;
  f.page.mouse.click=async (...args)=>{
    observed=await f.page.evaluate(()=>({moving:window.moving,steps:window.steps}));
    return originalClick(...args);
  };
  await f.page.evaluate(()=>{
    window.moving=true;window.steps=0;
    const frame=document.querySelector('#outer');
    function move(){
      frame.style.transform=`translateX(${++window.steps}px)`;
      if(window.steps<40) requestAnimationFrame(move); else window.moving=false;
    }
    requestAnimationFrame(move);
  });
  try {
    await leaf.locator('#target').setWaitForEnabled(false).setEnsureElementIsInTheViewport(false).setTimeout(5000).click();
    assert(observed?.moving && observed.steps<40, 'baseline must witness ancestor motion being ignored');
    console.log(`BASELINE locator dispatched-during-ancestor-motion step=${observed.steps}/40`);
  } finally {f.page.mouse.click=originalClick;}
  console.log('BASELINE normal-and-defect-witness PASS');
} finally { await f.close(); }
