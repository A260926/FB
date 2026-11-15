import assert from 'node:assert/strict';
import { InstancedSkin } from '../src/scene/instanced-skin.js';
import { MeshInstance } from '../src/scene/mesh-instance.js';
assert.equal(typeof InstancedSkin, 'function');
assert.equal(typeof MeshInstance.prototype.setInstancedSkin, 'function');
