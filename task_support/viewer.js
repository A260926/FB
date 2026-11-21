'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
function create() {
  const c = vm.createContext({console});
  c.window = c;
  c.requestAnimationFrame = () => { throw Error('headless viewer does not schedule animation'); };
  c.setInterval = () => { throw Error('headless viewer does not start browser timers'); };
  const html = fs.readFileSync(path.join(root, 'VoxelSpace.html'), 'utf8');
  let scripts = 0, startups = 0;
  for (const m of html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script\s*>/gi)) {
    let code = m[2], filename = 'VoxelSpace.html:inline';
    const src = /\bsrc\s*=\s*["']([^"']+)["']/i.exec(m[1]);
    if (src) {
      filename = path.resolve(root, src[1]);
      if (!filename.startsWith(root + path.sep)) throw Error('external script must be local');
      code = fs.readFileSync(filename, 'utf8');
    } else {
      code = code.replace(/\bInit\(\);\s*$/, () => { ++startups; return ''; });
    }
    vm.runInContext(code, c, {filename, timeout: 10000});
    ++scripts;
  }
  if (!scripts || startups !== 1 || typeof c.Render !== 'function' || typeof c.DrawBackground !== 'function')
    throw Error('expected production Render and one trailing Init startup');
  return c;
}
function runViewer(o = {}) {
  const c = o.session || create();
  const width = o.width ?? 16, height = o.height ?? 12;
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1) throw Error('invalid framebuffer');
  if (o.map) c.map = o.map;
  if (o.camera) Object.assign(c.camera, o.camera);
  const buffer = new ArrayBuffer(width * height * 4);
  Object.assign(c.screendata, {canvas:{width,height},bufarray:buffer,buf8:new Uint8Array(buffer),buf32:new Uint32Array(buffer)});
  if (o.background !== undefined) c.screendata.backgroundcolor = o.background >>> 0;
  if (o.configure) o.configure(c);
  if (o.collide) c.UpdateCamera();
  vm.runInContext('DrawBackground(); Render();', c, {timeout: 10000});
  return {color:Uint32Array.from(c.screendata.buf32),camera:{...c.camera},session:c};
}
module.exports = {runViewer};
