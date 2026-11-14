/**
 * @license
 * Copyright 2026 Google Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
// Authored fixture for reproducing frame-coordinate observations. Apache-2.0.
import {execFileSync} from 'node:child_process';
import {once} from 'node:events';
import http from 'node:http';

import puppeteer from '../packages/puppeteer-core/lib/esm/puppeteer/puppeteer-core.js';

const image =
  'chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f';
const docker = (...args) => {
  return execFileSync('docker', args, {encoding: 'utf8'}).trim();
};

/**
 * Layout constants for the regression fixtures. Tests derive their expected
 * coordinates from these values.
 */
export const LAYOUT = {
  outer: {left: 120, top: 100, width: 320, height: 220, border: 8, padding: 6},
  inner: {left: 30, top: 20, width: 160, height: 120, border: 4, padding: 3},
  target: {left: 40, top: 30, width: 45, height: 30},
  partial: {
    left: 20,
    top: 15,
    width: 60,
    height: 40,
    rotate: 27,
    scaleX: 1.3,
    scaleY: 0.8,
    originX: 11,
    originY: 13,
  },
};

const identityOuter = 'none';
const identityInner = 'none';
const affineOuter = 'scale(1.5,.8) rotate(12deg)';
const affineInner = 'rotate(-17deg) scale(1.2,.75) skewX(9deg)';

const eventsScript = scenario => {
  if (scenario === 'clip') {
    return `<script>
      window.events={clicks:0,hovers:0,clippedClicks:0,partialClicks:0};
      window.clicks=0;window.hovers=0;
      document.querySelector('#target').addEventListener('click',()=>{window.events.clicks++;window.clicks++;});
      document.querySelector('#target').addEventListener('mouseenter',()=>{window.events.hovers++;window.hovers++;});
      document.querySelector('#clipped').addEventListener('click',()=>window.events.clippedClicks++);
      document.querySelector('#partial').addEventListener('click',()=>window.events.partialClicks++);
    </script>`;
  }
  return `<script>
    window.events={clicks:0,hovers:0,clippedClicks:0,partialClicks:0};
    window.clicks=0;window.hovers=0;
    document.querySelector('#target').addEventListener('click',()=>{window.events.clicks++;window.clicks++;});
    document.querySelector('#target').addEventListener('mouseenter',()=>{window.events.hovers++;window.hovers++;});
  </script>`;
};

function leafPage(scenario) {
  if (scenario === 'clip') {
    return `<style>html,body{margin:0;background:#ddd;overflow:hidden}
      #target{position:absolute;left:${LAYOUT.target.left}px;top:${LAYOUT.target.top}px;width:${LAYOUT.target.width}px;height:${LAYOUT.target.height}px;border:0;padding:0;background:rgb(10,240,60)}
      #clipped{position:absolute;left:-500px;top:20px;width:45px;height:30px;border:0;padding:0;background:rgb(240,20,60)}
      #partial{position:absolute;left:${LAYOUT.partial.left}px;top:${LAYOUT.partial.top}px;width:${LAYOUT.partial.width}px;height:${LAYOUT.partial.height}px;border:0;padding:0;background:rgb(240,20,60);transform-origin:${LAYOUT.partial.originX}px ${LAYOUT.partial.originY}px;transform:rotate(${LAYOUT.partial.rotate}deg) scale(${LAYOUT.partial.scaleX},${LAYOUT.partial.scaleY})}
    </style>
    <button id="target" aria-label="record"></button>
    <button id="clipped" aria-label="clipped"></button>
    <button id="partial" aria-label="partial"></button>
    ${eventsScript(scenario)}`;
  }
  if (scenario === 'scroll') {
    return `<style>html,body{margin:0;background:#ddd;height:520px;width:520px;overflow:auto}
      #target{position:absolute;left:${LAYOUT.target.left}px;top:${LAYOUT.target.top}px;width:${LAYOUT.target.width}px;height:${LAYOUT.target.height}px;border:0;padding:0;background:rgb(10,240,60)}
    </style>
    <button id="target" aria-label="record"></button>
    ${eventsScript(scenario)}`;
  }
  return `<style>html,body{margin:0;background:#ddd}
    #target{position:absolute;left:${LAYOUT.target.left}px;top:${LAYOUT.target.top}px;width:${LAYOUT.target.width}px;height:${LAYOUT.target.height}px;border:0;padding:0;background:rgb(10,240,60)}
  </style>
  <button id="target" aria-label="record"></button>
  ${eventsScript(scenario)}`;
}

function middlePage(scenario, affine, port) {
  const transform = affine ? affineInner : identityInner;
  const overflow =
    scenario === 'scroll' ? 'height:480px;width:480px;overflow:auto' : '';
  return `<style>html,body{margin:0;background:#eee;${overflow}}
    #inner{position:absolute;left:${LAYOUT.inner.left}px;top:${LAYOUT.inner.top}px;width:${LAYOUT.inner.width}px;height:${LAYOUT.inner.height}px;border:${LAYOUT.inner.border}px solid #444;padding:${LAYOUT.inner.padding}px;transform-origin:7px 5px;transform:${transform}}
  </style><iframe id="inner" src="http://domain1.test:${port}/leaf?scenario=${scenario}&affine=${affine ? '1' : '0'}"></iframe>`;
}

function outerPage(scenario, affine, remote, port) {
  const childHost = remote ? 'domain2.test' : 'domain1.test';
  const child = `http://${childHost}:${port}/middle?scenario=${scenario}&affine=${affine ? '1' : '0'}`;
  if (scenario === 'stability') {
    return `<style>html,body{margin:0;background:white}
      #outer{position:absolute;left:${LAYOUT.outer.left}px;top:${LAYOUT.outer.top}px;width:${LAYOUT.outer.width}px;height:${LAYOUT.outer.height}px;border:${LAYOUT.outer.border}px solid #222;padding:${LAYOUT.outer.padding}px;transform-origin:11px 7px;transform:none}
    </style>
    <iframe id="outer" src="${child}"></iframe>
    <script>
      window.motion={moving:false,steps:0,stop:false,count:0};
      window.startMotion=count=>{
        const state=window.motion;
        state.steps=0;state.stop=false;state.count=count;state.moving=true;
        const frame=document.querySelector('#outer');
        const step=()=>{
          if(state.stop){state.moving=false;return;}
          state.steps++;
          frame.style.transform='translateX('+state.steps+'px)';
          if(state.steps<state.count){requestAnimationFrame(step);}else{state.moving=false;}
        };
        requestAnimationFrame(step);
      };
    </script>`;
  }
  if (scenario === 'scroll') {
    return `<style>html,body{margin:0;background:white;height:1800px;width:1800px}
      #host{position:absolute;left:120px;top:100px;border:3px solid blue;padding:7px;transform-origin:13px 17px;transform:rotate(-19deg) scale(1.15,.85)}
      iframe{width:320px;height:220px;border:8px solid #222;padding:6px;transform-origin:0 0;transform:skewX(11deg) scale(1.1,.9)}
    </style>
    <div id="host"><iframe id="outer" src="${child}"></iframe></div>
    <script>
      window.replaceFrame=url=>{
        const host=document.querySelector('#host');
        const frame=document.createElement('iframe');
        frame.id='outer';frame.src=url;
        host.replaceChildren(frame);
      };
    </script>`;
  }
  const transform = affine ? affineOuter : identityOuter;
  return `<style>html,body{margin:0;background:white}
    iframe{position:absolute;left:${LAYOUT.outer.left}px;top:${LAYOUT.outer.top}px;width:${LAYOUT.outer.width}px;height:${LAYOUT.outer.height}px;border:${LAYOUT.outer.border}px solid #222;padding:${LAYOUT.outer.padding}px;transform-origin:7px 5px;transform:${transform}}
  </style><iframe id="outer" src="${child}"></iframe>`;
}

/**
 * Renders one fixture page. Returns `null` for unknown paths.
 */
export function renderPage(pathname, params, port) {
  const scenario = params.get('scenario') ?? 'baseline';
  const affine = params.get('affine') === '1';
  const remote = params.get('remote') === '1';
  if (pathname === '/outer') {
    return outerPage(scenario, affine, remote, port);
  }
  if (pathname === '/middle') {
    return middlePage(scenario, affine, port);
  }
  if (pathname === '/leaf') {
    return leafPage(scenario);
  }
  return null;
}

function startServer() {
  const server = http.createServer((req, res) => {
    const port = server.address().port;
    const url = new URL(req.url, 'http://localhost');
    const body = renderPage(url.pathname, url.searchParams, port);
    res.setHeader('Content-Type', 'text/html; charset=utf-8');
    res.setHeader('Cache-Control', 'no-store');
    if (body === null) {
      res.statusCode = 404;
      res.end('not found');
      return;
    }
    res.end(body);
  });
  return server;
}

/**
 * Serves fixture pages through request interception, so the virtual `.test`
 * origins never need to resolve. Used by the local (non-Docker) mode.
 */
async function installLocalRouting(page) {
  await page.setRequestInterception(true);
  page.on('request', request => {
    const url = new URL(request.url());
    const body = renderPage(
      url.pathname,
      url.searchParams,
      Number(url.port || 80),
    );
    if (body === null) {
      request
        .respond({status: 404, contentType: 'text/plain', body: 'not found'})
        .catch(() => {});
      return;
    }
    request
      .respond({status: 200, contentType: 'text/html; charset=utf-8', body})
      .catch(() => {});
  });
}

/**
 * Opens the fixture. By default the pinned Chrome image is started in Docker,
 * matching the task's public entry. Pass `{local: true}` or set
 * `HARNESS_LOCAL_BROWSER` to launch a local browser with request
 * interception instead.
 */
export async function openFixture(options = {}) {
  const local = options.local ?? Boolean(process.env['HARNESS_LOCAL_BROWSER']);
  const localExecutable =
    options.executablePath ?? process.env['PUPPETEER_EXECUTABLE_PATH'];
  if (local) {
    const browser = await puppeteer.launch({
      headless: 'shell',
      // Pipe transport avoids needing a debugging port in restricted
      // environments; the Docker fixture keeps the default WebSocket path.
      pipe: options.pipe ?? process.env['HARNESS_LOCAL_PIPE'] !== '0',
      ...(localExecutable ? {executablePath: localExecutable} : {}),
      args: [
        '--no-sandbox',
        '--disable-dev-shm-usage',
        '--disable-gpu',
        '--site-per-process',
        // Extra flags are handy on hosts where the browser cannot create
        // Mach services or listening sockets (for example a restricted CI
        // sandbox). They are not needed with the Docker fixture.
        ...(process.env['HARNESS_LOCAL_ARGS'] ?? '')
          .split(/\s+/)
          .filter(Boolean),
      ],
    });
    const newPage = async () => {
      const page = await browser.newPage();
      await page.bringToFront();
      await page.setViewport({width: 1000, height: 700, deviceScaleFactor: 1});
      await installLocalRouting(page);
      return page;
    };
    const page = await newPage();
    return {
      browser,
      page,
      newPage,
      origin1: 'http://domain1.test',
      origin2: 'http://domain2.test',
      url: 'http://domain1.test/outer',
      async close() {
        await browser.close();
      },
    };
  }

  const server = startServer();
  server.listen(0);
  await once(server, 'listening');
  const viewportPort = server.address().port;
  let browser, container;
  try {
    const address = docker(
      'run',
      '--rm',
      '--entrypoint',
      'getent',
      image,
      'hosts',
      'host.docker.internal',
    ).split(/\s+/)[0];
    container = docker(
      'run',
      '-d',
      '--rm',
      '-p',
      '127.0.0.1::9222',
      '--entrypoint',
      '/bin/bash',
      image,
      '-c',
      'socat TCP4-LISTEN:9222,fork TCP4:127.0.0.1:9223 & exec /headless-shell/headless-shell --no-sandbox --disable-dev-shm-usage --remote-debugging-port=9223 "$@"',
      'userfb',
      '--site-per-process',
      `--host-resolver-rules=MAP *.test ${address}`,
    );
    const port = docker('port', container, '9222/tcp').split(':').at(-1);
    let response;
    for (let i = 0; i < 100; i++) {
      response = await fetch(`http://127.0.0.1:${port}/json/version`).catch(
        () => {
          return null;
        },
      );
      if (response?.ok) {
        break;
      }
      await new Promise(resolve => {
        return setTimeout(resolve, 50);
      });
    }
    if (!response?.ok) {
      throw new Error('pinned Chrome endpoint did not start');
    }
    const {webSocketDebuggerUrl} = await response.json();
    const endpoint = new URL(webSocketDebuggerUrl);
    endpoint.hostname = '127.0.0.1';
    endpoint.port = port;
    browser = await puppeteer.connect({browserWSEndpoint: endpoint.href});
  } catch (error) {
    if (container) {
      docker('rm', '-f', container);
    }
    server.closeAllConnections();
    server.close();
    throw error;
  }
  const newPage = async () => {
    const page = await browser.newPage();
    await page.bringToFront();
    await page.setViewport({width: 1000, height: 700, deviceScaleFactor: 1});
    return page;
  };
  const page = await newPage();
  return {
    browser,
    page,
    newPage,
    origin1: `http://domain1.test:${viewportPort}`,
    origin2: `http://domain2.test:${viewportPort}`,
    url: `http://domain1.test:${viewportPort}/outer`,
    async close() {
      browser.disconnect();
      docker('rm', '-f', container);
      server.closeAllConnections();
      await new Promise((resolve, reject) => {
        return server.close(error => {
          return error ? reject(error) : resolve();
        });
      });
    },
  };
}
