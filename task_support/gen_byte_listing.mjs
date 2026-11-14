// SPDX-FileCopyrightText: 2026 r2dec contributors
// SPDX-License-Identifier: BSD-3-Clause

// Emits a byte listing for every synthetic switch fixture so the legal bytes
// used by the tests are kept on disk and reviewable. The listing is derived
// only from the same builder that produces the graphs, so they cannot drift.
// Run: node task_support/gen_byte_listing.mjs
// Only JavaScript built-in modules are used.

import { writeFileSync } from 'node:fs';
import { fixtures } from './switch_fixtures.mjs';

var names = Object.keys(fixtures);

var listing = {
    description: 'legal synthetic x86-64 raw images for switch recovery tests; never execute',
    fixtures: {}
};

names.forEach(function(name) {
    var f = fixtures[name]();
    var blocks = [];
    f.issue.graph[0].blocks.forEach(function(block) {
        blocks.push({
            offset: '0x' + block.offset.toString(16),
            ops: block.ops.map(function(op) {
                return {
                    offset: '0x' + op.offset.toString(16),
                    size: op.size,
                    bytes: op.bytes,
                    opcode: op.opcode
                };
            })
        });
    });
    listing.fixtures[name] = {
        base: '0x' + f.issue.image.base.toString(16),
        sha256: f.sha256,
        image_hex: f.image.toString('hex'),
        blocks: blocks
    };
});

var out = new URL('./samples/synthetic_bytes.json', import.meta.url);
writeFileSync(out, JSON.stringify(listing, null, 2) + '\n');
console.log('wrote ' + names.length + ' fixture byte listings to task_support/samples/synthetic_bytes.json');
names.forEach(function(name) {
    console.log('  ' + name + ' sha256=' + listing.fixtures[name].sha256);
});
