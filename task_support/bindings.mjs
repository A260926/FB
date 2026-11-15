import assert from 'node:assert/strict';
import { SceneInstanceCollector } from '../src/render/SceneInstanceCollector.js';
import * as t3d from '../src/main.js';
assert.equal(typeof SceneInstanceCollector,'function');
assert.equal(t3d.SceneInstanceCollector,SceneInstanceCollector);
