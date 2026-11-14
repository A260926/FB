/**
 * @license
 * Copyright 2025 Google Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Regression tests for element geometry through nested same-process and
 * cross-process frames whose ancestors use reversible 2D CSS affine
 * transforms.
 *
 * Expected coordinates come from the fixture's own layout constants and CSS
 * transform matrices, or from whole-page pixels; the element handle's
 * `boundingBox()` is never used to validate itself. The tests also read the
 * click and hover events that the target button actually receives.
 */

import assert from 'node:assert/strict';
import test from 'node:test';

import {PNG} from 'pngjs';

import {LAYOUT, openFixture} from './fixture.mjs';

const TARGET_COLOR = {r: 10, g: 240, b: 60};
const OTHER_COLOR = {r: 240, g: 20, b: 60};

// Fixture transforms, mirrored from `harness/fixture.mjs`. These are the
// independent reference used by the assertions.
const OUTER = {
  ...LAYOUT.outer,
  origin: {x: 11, y: 7},
  matrix: multiply(scale(1.5, 0.8), rotate(12)),
};
const INNER = {
  ...LAYOUT.inner,
  origin: {x: 7, y: 5},
  matrix: multiply(rotate(-17), multiply(scale(1.2, 0.75), skewX(9))),
};
const TARGET = LAYOUT.target;
const PARTIAL = LAYOUT.partial;
const PARTIAL_MATRIX = multiply(
  rotate(PARTIAL.rotate),
  scale(PARTIAL.scaleX, PARTIAL.scaleY),
);
const PARTIAL_ORIGIN = {x: PARTIAL.originX, y: PARTIAL.originY};

/** 2D affine matrices in CSS `DOMMatrix` `[a,b,c,d,e,f]` order. */
function multiply(outer, inner) {
  return [
    outer[0] * inner[0] + outer[2] * inner[1],
    outer[1] * inner[0] + outer[3] * inner[1],
    outer[0] * inner[2] + outer[2] * inner[3],
    outer[1] * inner[2] + outer[3] * inner[3],
    outer[0] * inner[4] + outer[2] * inner[5] + outer[4],
    outer[1] * inner[4] + outer[3] * inner[5] + outer[5],
  ];
}

function scale(x, y) {
  return [x, 0, 0, y, 0, 0];
}

function rotate(degrees) {
  const radians = (degrees * Math.PI) / 180;
  return [
    Math.cos(radians),
    Math.sin(radians),
    -Math.sin(radians),
    Math.cos(radians),
    0,
    0,
  ];
}

function skewX(degrees) {
  return [1, 0, Math.tan((degrees * Math.PI) / 180), 1, 0, 0];
}

function apply(matrix, point) {
  return {
    x: matrix[0] * point.x + matrix[2] * point.y + matrix[4],
    y: matrix[1] * point.x + matrix[3] * point.y + matrix[5],
  };
}

/**
 * Maps a point in the leaf document's CSS pixels into the main viewport,
 * composing the fixture's content-box insets, layout offsets and transforms.
 */
function leafPointToMain(point) {
  const middle = mapThroughFrame(INNER, point);
  return mapThroughFrame(OUTER, middle);
}

/** Maps a point in a child viewport through one frame element. */
function mapThroughFrame(frame, point) {
  const contentOrigin = {
    x: frame.border + frame.padding,
    y: frame.border + frame.padding,
  };
  const local = {
    x: contentOrigin.x + point.x - frame.origin.x,
    y: contentOrigin.y + point.y - frame.origin.y,
  };
  const transformed = apply(frame.matrix, local);
  return {
    x: frame.left + frame.origin.x + transformed.x,
    y: frame.top + frame.origin.y + transformed.y,
  };
}

function quadBox(points) {
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (const point of points) {
    left = Math.min(left, point.x);
    top = Math.min(top, point.y);
    right = Math.max(right, point.x);
    bottom = Math.max(bottom, point.y);
  }
  return {x: left, y: top, width: right - left, height: bottom - top};
}

/**
 * Applies an element's own CSS transform: `point` is in the leaf document's
 * coordinates, the transform origin is relative to the element's border box
 * at `(left, top)`.
 */
function transformElement(frame, matrix, origin, point) {
  const local = {
    x: point.x - frame.left - origin.x,
    y: point.y - frame.top - origin.y,
  };
  const transformed = apply(matrix, local);
  return {
    x: frame.left + origin.x + transformed.x,
    y: frame.top + origin.y + transformed.y,
  };
}

function targetQuad() {
  return [
    {x: TARGET.left, y: TARGET.top},
    {x: TARGET.left + TARGET.width, y: TARGET.top},
    {x: TARGET.left + TARGET.width, y: TARGET.top + TARGET.height},
    {x: TARGET.left, y: TARGET.top + TARGET.height},
  ].map(leafPointToMain);
}

function partialQuad() {
  const corners = [
    {x: PARTIAL.left, y: PARTIAL.top},
    {x: PARTIAL.left + PARTIAL.width, y: PARTIAL.top},
    {x: PARTIAL.left + PARTIAL.width, y: PARTIAL.top + PARTIAL.height},
    {x: PARTIAL.left, y: PARTIAL.top + PARTIAL.height},
  ];
  return corners
    .map(point => {
      return transformElement(PARTIAL, PARTIAL_MATRIX, PARTIAL_ORIGIN, point);
    })
    .map(leafPointToMain);
}

function assertClose(actual, expected, tolerance, message) {
  assert.ok(
    Math.abs(actual - expected) <= tolerance,
    `${message}: expected ${expected} +/- ${tolerance}, got ${actual}`,
  );
}

function assertBoxClose(actual, expected, tolerance) {
  assert.ok(actual, 'box should be available');
  assertClose(actual.x, expected.x, tolerance, 'box x');
  assertClose(actual.y, expected.y, tolerance, 'box y');
  assertClose(actual.width, expected.width, tolerance, 'box width');
  assertClose(actual.height, expected.height, tolerance, 'box height');
}

/** Decodes a screenshot and locates pixels of `color`, in CSS pixels. */
function findColor(png, color, deviceScaleFactor) {
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  let count = 0;
  for (let y = 0; y < png.height; y++) {
    for (let x = 0; x < png.width; x++) {
      const index = (y * png.width + x) * 4;
      if (
        png.data[index] === color.r &&
        png.data[index + 1] === color.g &&
        png.data[index + 2] === color.b
      ) {
        count++;
        left = Math.min(left, x);
        right = Math.max(right, x);
        top = Math.min(top, y);
        bottom = Math.max(bottom, y);
      }
    }
  }
  if (count === 0) {
    return {count: 0};
  }
  const s = deviceScaleFactor;
  return {
    count,
    left: left / s,
    top: top / s,
    right: (right + 1) / s,
    bottom: (bottom + 1) / s,
    width: (right - left + 1) / s,
    height: (bottom - top + 1) / s,
  };
}

function decode(data) {
  return PNG.sync.read(Buffer.from(data));
}

function inside(raster, point, tolerance = 1.5) {
  return (
    point.x >= raster.left - tolerance &&
    point.x <= raster.right + tolerance &&
    point.y >= raster.top - tolerance &&
    point.y <= raster.bottom + tolerance
  );
}

async function pageFor(
  fixture,
  {remote, affine = true, scenario = 'baseline', deviceScaleFactor = 1},
) {
  const page = await fixture.newPage();
  await page.setViewport({width: 1000, height: 700, deviceScaleFactor});
  const query = new URLSearchParams({
    remote: remote ? '1' : '0',
    affine: affine ? '1' : '0',
    scenario,
  });
  await page.goto(`${fixture.origin1}/outer?${query}`, {
    waitUntil: 'networkidle0',
  });
  const leaf = page.frames().find(frame => {
    return frame.url().includes('/leaf');
  });
  assert.ok(leaf, 'leaf frame should be present');
  return {page, leaf};
}

async function eventsOf(frame) {
  return await frame.evaluate(() => {
    return window.events;
  });
}

test('global geometry and pixels', async t => {
  const fixture = await openFixture();
  t.after(async () => {
    await fixture.close();
  });
  const expected = quadBox(targetQuad());

  for (const remote of [false, true]) {
    await t.test(`remote=${remote}`, async () => {
      const {page, leaf} = await pageFor(fixture, {remote});
      try {
        assert.equal(
          leaf.client !== leaf.parentFrame().client,
          remote,
          'remote flag should control the process boundary',
        );
        const target = await leaf.waitForSelector('#target');

        // The complete projected border box, not just the element's own
        // rectangle, must match the fixture's composed transforms.
        const box = await target.boundingBox();
        assertBoxClose(box, expected, 1.5);

        const raster = findColor(
          decode(await page.screenshot({captureBeyondViewport: false})),
          TARGET_COLOR,
          1,
        );
        assert.ok(raster.count > 100, 'target should be painted');
        assert.ok(
          raster.left >= box.x - 1.5 &&
            raster.right <= box.x + box.width + 1.5 &&
            raster.top >= box.y - 1.5 &&
            raster.bottom <= box.y + box.height + 1.5,
          'painted target must lie inside the complete projected box',
        );

        // The chosen point must be inside the real visible intersection and
        // must reach the button.
        const point = await target.clickablePoint();
        assert.ok(
          inside(raster, point),
          'clickablePoint should hit the painted target',
        );
        await target.click();
        assert.equal((await eventsOf(leaf)).clicks, 1, 'click should land');
        await target.hover();
        assert.equal((await eventsOf(leaf)).hovers, 1, 'hover should land');

        // Element screenshots use the same complete projection.
        const shot = decode(await target.screenshot({scrollIntoView: false}));
        const marker = findColor(shot, TARGET_COLOR, 1);
        assert.ok(
          marker.count > 100,
          'element screenshot must contain the target',
        );
        assert.ok(
          marker.count / (shot.width * shot.height) > 0.3,
          'element screenshot must be dominated by the target',
        );
        assertClose(shot.width, Math.round(box.width), 1, 'crop width');
        assertClose(shot.height, Math.round(box.height), 1, 'crop height');
      } finally {
        await page.close();
      }
    });
  }
});

test('clipped pointer and offset', async t => {
  const fixture = await openFixture();
  t.after(async () => {
    await fixture.close();
  });

  for (const remote of [false, true]) {
    await t.test(`remote=${remote}`, async () => {
      const {page, leaf} = await pageFor(fixture, {
        remote,
        scenario: 'clip',
      });
      try {
        const clipped = await leaf.$('#clipped');
        const partial = await leaf.$('#partial');
        assert.ok(clipped && partial);

        const originalClick = page.mouse.click.bind(page.mouse);
        const originalMove = page.mouse.move.bind(page.mouse);
        const originalDown = page.mouse.down.bind(page.mouse);
        let sent = 0;
        page.mouse.click = async (...args) => {
          sent++;
          return await originalClick(...args);
        };
        page.mouse.move = async (...args) => {
          sent++;
          return await originalMove(...args);
        };
        page.mouse.down = async (...args) => {
          sent++;
          return await originalDown(...args);
        };
        try {
          // Empty intersection: no mouse actions at all.
          await assert.rejects(clipped.clickablePoint(), /not clickable/);
          await assert.rejects(clipped.click(), /not clickable/);
          await assert.rejects(clipped.hover(), /not clickable/);
          assert.equal(
            sent,
            0,
            'fully clipped element must send no mouse events',
          );
          assert.equal((await eventsOf(leaf)).clippedClicks, 0);

          // A partially clipped, transformed element still resolves.
          const partialBox = await partial.boundingBox();
          assertBoxClose(partialBox, quadBox(partialQuad()), 1.5);
          const raster = findColor(
            decode(await page.screenshot({captureBeyondViewport: false})),
            OTHER_COLOR,
            1,
          );
          assert.ok(raster.count > 100, 'partial target should be painted');
          const point = await partial.clickablePoint();
          assert.ok(
            inside(raster, point),
            'clickablePoint must be inside the visible part',
          );

          // Offsets are local CSS pixels of the untransformed border box.
          const origin = await partial.clickablePoint({x: 0, y: 0});
          // Offset (0, 0) is the element's own untransformed top-left corner.
          const expectedOrigin = leafPointToMain(
            transformElement(PARTIAL, PARTIAL_MATRIX, PARTIAL_ORIGIN, {
              x: PARTIAL.left,
              y: PARTIAL.top,
            }),
          );
          assertClose(origin.x, expectedOrigin.x, 1.5, 'offset 0,0 x');
          assertClose(origin.y, expectedOrigin.y, 1.5, 'offset 0,0 y');

          // An offset inside the untransformed box maps through the same
          // transform and clicks the button.
          await partial.click({offset: {x: 20, y: 15}});
          assert.equal(
            (await eventsOf(leaf)).partialClicks,
            1,
            'offset click should land',
          );

          // An offset clipped away by a frame is rejected without sending any
          // mouse action.
          const before = sent;
          await assert.rejects(
            partial.click({offset: {x: -5000, y: 0}}),
            /outside the visible area/,
          );
          assert.equal(
            sent,
            before,
            'rejected offset must send no mouse events',
          );
          assert.equal((await eventsOf(leaf)).partialClicks, 1);
        } finally {
          page.mouse.click = originalClick;
          page.mouse.move = originalMove;
          page.mouse.down = originalDown;
        }
      } finally {
        await page.close();
      }
    });
  }
});

test('ancestor stability and abort', async t => {
  const fixture = await openFixture();
  t.after(async () => {
    await fixture.close();
  });

  for (const remote of [false, true]) {
    await t.test(`remote=${remote}`, async () => {
      const {page, leaf} = await pageFor(fixture, {
        remote,
        scenario: 'stability',
      });
      try {
        const startMotion = async count => {
          await page.evaluate(value => {
            window.startMotion(value);
          }, count);
        };
        const stopMotion = async () => {
          await page.evaluate(() => {
            window.motion.stop = true;
          });
        };
        const motionState = async () => {
          return await page.evaluate(() => {
            return window.motion;
          });
        };

        const originalClick = page.mouse.click.bind(page.mouse);
        let observed = null;
        let clicks = 0;
        page.mouse.click = async (...args) => {
          observed = await motionState();
          clicks++;
          return await originalClick(...args);
        };
        try {
          // Default stability check: wait for the ancestor animation to stop,
          // even though the button's local rectangle never changes.
          await startMotion(40);
          await leaf
            .locator('#target')
            .setWaitForEnabled(false)
            .setEnsureElementIsInTheViewport(false)
            .setTimeout(15000)
            .click();
          assert.ok(observed, 'click should reach the mouse');
          assert.ok(
            observed.steps >= 40,
            `click must wait for ancestor stability; dispatched at step ${observed.steps}`,
          );
          assert.equal((await motionState()).moving, false);
          assert.equal((await eventsOf(leaf)).clicks, 1);
          assert.equal(clicks, 1, 'exactly one mouse click is sent');

          // Disabled stability proceeds immediately during motion.
          observed = null;
          await startMotion(60);
          await leaf
            .locator('#target')
            .setWaitForEnabled(false)
            .setEnsureElementIsInTheViewport(false)
            .setWaitForStableBoundingBox(false)
            .setTimeout(5000)
            .click();
          assert.ok(
            observed && observed.moving,
            'disabled stability must dispatch during motion',
          );
          await stopMotion();

          // AbortSignal cancels the pending wait and sends no mouse events.
          observed = null;
          clicks = 0;
          await startMotion(400);
          const controller = new AbortController();
          const pending = leaf
            .locator('#target')
            .setWaitForEnabled(false)
            .setEnsureElementIsInTheViewport(false)
            .setTimeout(30000)
            .click({signal: controller.signal});
          await new Promise(resolve => {
            return setTimeout(resolve, 300);
          });
          const before = (await eventsOf(leaf)).clicks;
          controller.abort();
          await assert.rejects(pending, /aborted|AbortError/i);
          assert.equal(clicks, 0, 'aborted action must send zero mouse events');
          assert.equal((await eventsOf(leaf)).clicks, before);
          await stopMotion();
        } finally {
          page.mouse.click = originalClick;
        }
      } finally {
        await page.close();
      }
    });
  }
});

test('frame replacement and scroll', async t => {
  const fixture = await openFixture();
  t.after(async () => {
    await fixture.close();
  });

  for (const remote of [false, true]) {
    await t.test(`remote=${remote}`, async () => {
      const {page, leaf} = await pageFor(fixture, {
        remote,
        scenario: 'scroll',
        deviceScaleFactor: 2,
      });
      try {
        // Scroll each level differently, while keeping the target visible.
        await page.evaluate(() => {
          window.scrollTo(10, 20);
        });
        await leaf.parentFrame().evaluate(() => {
          window.scrollTo(13, 17);
        });
        await leaf.evaluate(() => {
          window.scrollTo(9, 11);
        });
        await new Promise(resolve => {
          return setTimeout(resolve, 200);
        });

        const target = await leaf.waitForSelector('#target');
        const raster = findColor(
          decode(await page.screenshot({captureBeyondViewport: false})),
          TARGET_COLOR,
          2,
        );
        assert.ok(raster.count > 100, 'target should be painted at dsf=2');
        const point = await target.clickablePoint();
        assert.ok(
          inside(raster, point, 2),
          'deviceScaleFactor must not scale mouse coordinates',
        );
        await target.click();
        assert.equal((await eventsOf(leaf)).clicks, 1, 'scrolled click lands');

        // The element crop is in main-document coordinates even though three
        // documents scroll independently.
        const shot = decode(await target.screenshot({scrollIntoView: false}));
        const marker = findColor(shot, TARGET_COLOR, 2);
        assert.ok(
          marker.count > 100,
          'element screenshot must contain the target',
        );
        assert.ok(
          marker.count / (shot.width * shot.height) > 0.3,
          'element screenshot must be dominated by the target',
        );
        const box = await target.boundingBox();
        assert.ok(box);
        assertClose(
          shot.width,
          Math.round(box.width * 2),
          2,
          'dsf doubles crop width',
        );
        assertClose(
          shot.height,
          Math.round(box.height * 2),
          2,
          'dsf doubles crop height',
        );

        // Cross-site frame replacement: re-query and operate again. The new
        // handle must use the new layout and must not inherit the old
        // handle's cached geometry or stability state.
        const oldLeaf = leaf;
        await page.evaluate(url => {
          window.replaceFrame(url);
        }, `${fixture.origin2}/middle?scenario=scroll&affine=1`);
        const newLeaf = await page.waitForFrame(frame => {
          return frame.url().includes('/leaf') && frame !== oldLeaf;
        });
        const newTarget = await newLeaf.waitForSelector('#target');
        assert.ok(newTarget);

        await page.evaluate(() => {
          window.scrollTo(10, 20);
        });
        await newLeaf.evaluate(() => {
          window.scrollTo(9, 11);
        });
        await new Promise(resolve => {
          return setTimeout(resolve, 200);
        });

        const replacedRaster = findColor(
          decode(await page.screenshot({captureBeyondViewport: false})),
          TARGET_COLOR,
          2,
        );
        assert.ok(
          replacedRaster.count > 100,
          'replaced target should be painted',
        );
        const replacedPoint = await newTarget.clickablePoint();
        assert.ok(
          inside(replacedRaster, replacedPoint, 2),
          're-queried handle must use the replaced frame layout',
        );
        await newTarget.click();
        assert.equal(
          (await eventsOf(newLeaf)).clicks,
          1,
          're-queried handle must click after replacement',
        );

        // The old handle must not silently keep acting on stale cached
        // coordinates: either it is gone, or it no longer resolves to the
        // painted position.
        const staleBox = await target.boundingBox().catch(() => {
          return null;
        });
        if (staleBox) {
          assert.ok(
            !(
              staleBox.x === box.x &&
              staleBox.y === box.y &&
              staleBox.width === box.width &&
              staleBox.height === box.height
            ),
            'old handle must not reuse its cached geometry after replacement',
          );
        }
      } finally {
        await page.close();
      }
    });
  }
});
