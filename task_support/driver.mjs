import assert from 'node:assert/strict';
import cases from '../test/scene/instanced-skin.target.mjs';
const groups = ['palette', 'pipeline', 'lifetime'];
assert.ok(Array.isArray(cases) && cases.length > 0 && cases.length < 10000, 'nonempty cases required');
const names = new Set();
for (const c of cases) {
    assert.ok(c && groups.includes(c.group) && typeof c.name === 'string' && c.name.length && typeof c.run === 'function');
    const key = `${c.group}/${c.name}`;
    assert.ok(!names.has(key), 'duplicate case'); names.add(key);
}
let total = 0;
for (const group of groups) {
    let ran = 0, groupChecks = 0;
    for (const c of cases.filter(c => c.group === group)) {
        let checks = 0;
        const check = (condition, message) => { ++checks; assert.ok(condition, message); };
        const result = await c.run(check);
        assert.equal(result, undefined, 'cases do not return skip/status values');
        assert.ok(checks > 0, 'case did not execute any checks');
        groupChecks += checks; ++ran; ++total;
        console.log(`CASE ${group} ${c.name} checks=${checks}`);
    }
    assert.ok(ran > 0 && groupChecks > 0, `empty group ${group}`);
    console.log(`GROUP ${group} cases=${ran} checks=${groupChecks} COMPLETE`);
}
assert.equal(total, cases.length);
console.log(`TARGET_COMPLETE cases=${total}`);
