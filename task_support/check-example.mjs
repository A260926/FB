import assert from 'node:assert/strict';
import fs from 'node:fs';
const result = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
assert.ok(Array.isArray(result.frames) && result.frames.length >= 2);
for (const frame of result.frames) {
    assert.ok(Number.isFinite(frame.time));
    assert.ok(Array.isArray(frame.instances) && frame.instances.length === 2);
    for (const instance of frame.instances) {
        assert.ok(Array.isArray(instance.positions) && instance.positions.length >= 9 && instance.positions.length % 3 === 0);
        assert.ok(instance.positions.every(Number.isFinite));
    }
}
console.log(`EXAMPLE_POSES frames=${result.frames.length}`);
