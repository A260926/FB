// Authored fixture for reproducing frame-coordinate observations. Apache-2.0.
import http from 'node:http';
import {once} from 'node:events';
import {execFileSync} from 'node:child_process';
import puppeteer from '../packages/puppeteer-core/lib/esm/puppeteer/puppeteer-core.js';

const image='chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f';
const docker=(...args)=>execFileSync('docker',args,{encoding:'utf8'}).trim();

export async function openFixture() {
  const server = http.createServer((req, res) => {
    const port = server.address().port;
    const url = new URL(req.url, 'http://localhost');
    const affine = url.searchParams.get('affine') === '1';
    const childHost = url.searchParams.get('remote') === '1' ? 'domain2.test' : 'domain1.test';
    const query = url.search;
    res.setHeader('Content-Type', 'text/html; charset=utf-8');
    if (url.pathname === '/outer') {
      res.end(`<style>html,body{margin:0;background:white} iframe{position:absolute;left:120px;top:100px;width:320px;height:220px;border:8px solid #222;padding:6px;transform-origin:0 0;transform:${affine ? 'scale(1.5,.8) rotate(12deg)' : 'none'}}</style><iframe id="outer" src="http://${childHost}:${port}/middle${query}"></iframe>`);
    } else if (url.pathname === '/middle') {
      res.end(`<style>html,body{margin:0;background:#eee} iframe{position:absolute;left:30px;top:20px;width:160px;height:120px;border:4px solid #444;padding:3px;transform-origin:0 0;transform:${affine ? 'matrix(1,.15,.1,.9,0,0)' : 'none'}}</style><iframe id="inner" src="http://domain1.test:${port}/leaf${query}"></iframe>`);
    } else if (url.pathname === '/leaf') {
      res.end(`<style>html,body{margin:0;background:#ddd} #target{position:absolute;left:40px;top:30px;width:45px;height:30px;border:0;padding:0;background:rgb(10,240,60)}</style><button id="target" aria-label="record"></button><script>window.clicks=0;window.hovers=0;target.onclick=()=>window.clicks++;target.onmouseenter=()=>window.hovers++;</script>`);
    } else {
      res.statusCode = 404;
      res.end('not found');
    }
  });
  server.listen(0);
  await once(server, 'listening');
  let browser, container;
  try {
    const address=docker('run','--rm','--entrypoint','getent',image,'hosts','host.docker.internal').split(/\s+/)[0];
    container=docker('run','-d','--rm','-p','127.0.0.1::9222','--entrypoint','/bin/bash',image,'-c',
      'socat TCP4-LISTEN:9222,fork TCP4:127.0.0.1:9223 & exec /headless-shell/headless-shell --no-sandbox --disable-dev-shm-usage --remote-debugging-port=9223 "$@"',
      'userfb','--site-per-process',`--host-resolver-rules=MAP *.test ${address}`);
    const port=docker('port',container,'9222/tcp').split(':').at(-1);
    let response;
    for(let i=0;i<100;i++) {
      response=await fetch(`http://127.0.0.1:${port}/json/version`).catch(()=>null);
      if(response?.ok) break;
      await new Promise(resolve=>setTimeout(resolve,50));
    }
    if(!response?.ok) throw new Error('pinned Chrome endpoint did not start');
    const {webSocketDebuggerUrl}=await response.json();
    const endpoint=new URL(webSocketDebuggerUrl);
    endpoint.hostname='127.0.0.1';
    endpoint.port=port;
    browser=await puppeteer.connect({browserWSEndpoint:endpoint.href});
  } catch (error) {
    if(container) docker('rm','-f',container);
    server.closeAllConnections();
    server.close();
    throw error;
  }
  const page = await browser.newPage();
  await page.bringToFront();
  await page.setViewport({width:1000, height:700, deviceScaleFactor:1});
  return {
    browser, page,
    url: `http://domain1.test:${server.address().port}/outer`,
    async close() {
      browser.disconnect();
      docker('rm','-f',container);
      server.closeAllConnections();
      await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
    },
  };
}
