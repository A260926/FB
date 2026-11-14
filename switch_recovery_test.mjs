// SPDX-FileCopyrightText: 2026 r2dec contributors
// SPDX-License-Identifier: BSD-3-Clause

// Switch recovery tests. Every case runs the real production pipeline through
// task_support/harness.mjs (session -> x86 translation -> ControlFlow -> print)
// and then asserts against hand-written edges/paths and printed positions.
// Only JavaScript built-in modules are used.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { decompile } from './task_support/harness.mjs';
import { fixtures } from './task_support/switch_fixtures.mjs';

var HEX = /^0x[0-9a-f]+$/;

var at = function(fixture, key) {
    return '0x' + fixture.layout.get(key).toString(16);
};

// Printed line offsets are bare hex strings (no 0x prefix) in the harness.
var atline = function(fixture, key) {
    return fixture.layout.get(key).toString(16);
};

var edge_set = function(edges) {
    return new Set(edges.map(function(e) {
        return e.from + '->' + (e.to == null ? 'null' : e.to);
    }));
};

var expect_edges = function(expected) {
    return new Set(expected.map(function(e) {
        return e[0] + '->' + (e[1] == null ? 'null' : e[1]);
    }));
};

// SHA + per instruction raw byte consistency, mirroring the offline checks.
var verify_image = function(fixture, check) {
    var issue = fixture.issue;
    check(createHash('sha256').update(fixture.image).digest('hex') === issue.image.sha256, 'image sha256 must match');
    issue.graph[0].blocks.forEach(function(block) {
        block.ops.forEach(function(op) {
            var raw = fixture.image.subarray(
                op.offset - issue.image.base,
                op.offset - issue.image.base + op.size
            ).toString('hex');
            check(raw === op.bytes, 'bytes at 0x' + op.offset.toString(16) + ' must match the graph listing');
        });
    });
};

var validate_structure = function(sw, check) {
    check(HEX.test(sw.dispatch), 'dispatch must be a hex address string');
    check(typeof sw.selector === 'string' && sw.selector.length > 0, 'selector must be a string');
    check(Array.isArray(sw.cases) && sw.cases.length > 0, 'cases must be non empty');
    sw.cases.forEach(function(c) {
        check(Number.isInteger(c.value), 'case value must be an integer');
        check(HEX.test(c.target), 'case target must be a hex address string');
    });
    check(sw.defaultTarget === null || HEX.test(sw.defaultTarget), 'defaultTarget must be null or hex');
    check(sw.join === null || HEX.test(sw.join), 'join must be null or hex');
    check(Array.isArray(sw.edges), 'edges must be an array');
    sw.edges.forEach(function(e) {
        check(HEX.test(e.from), 'edge from must be hex');
        check(e.to === null || HEX.test(e.to), 'edge to must be hex or null for returns');
    });
};

var line_index = function(lines, substring) {
    for (var i = 0; i < lines.length; i++) {
        if (lines[i].str.includes(substring)) {
            return i;
        }
    }
    return -1;
};

var count = function(lines, substring) {
    return lines.filter(function(l) {
        return l.str.includes(substring);
    }).length;
};

export var cases = [
    // ----------------------------------------------------------------- aliases
    {
        group: 'aliases',
        name: 'shared_case_labels_and_common_tail',
        run: function(check) {
            var f = fixtures.aliases_join();
            verify_image(f, check);
            var r = decompile(f.issue);
            check(r.session.switches.length === 1, 'exactly one recovered switch');
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.dispatch === at(f, 'dispatch'), 'dispatch points at the indirect jump');
            check(sw.selector === 'eax', 'selector comes straight from switchop');
            check(sw.join === at(f, 'join'), 'common tail detected as join');
            check(sw.defaultTarget === at(f, 'gdef'), 'default target preserved');
            check(sw.cases[0].target === sw.cases[1].target, 'values 0 and 1 share one body block');
            check(sw.cases[0].target === at(f, 'g01'), 'shared target is g01');
            check(sw.cases[2].target === at(f, 'g2'), 'value 2 keeps its own target');

            assert.deepEqual(edge_set(sw.edges), expect_edges([
                [at(f, 'g01'), at(f, 'join')],
                [at(f, 'g2'), at(f, 'join')],
                [at(f, 'gdef'), at(f, 'join')]
            ]));
            check(true, 'all three case edges reach the single join');

            var lines = r.lines;
            check(count(lines, 'switch (eax)') === 1, 'one switch statement printed');
            var shared = lines.find(function(l) {
                return l.str.trim() === 'case 0: case 1:';
            });
            check(!!shared, 'aliased values share one case label line');
            check(shared.offset === atline(f, 'g01'), 'shared case line offset is the target block');
            var head = lines.find(function(l) {
                return l.str.includes('switch (');
            });
            check(head.offset === atline(f, 'dispatch'), 'switch head offset is the dispatch instruction');
            check(count(lines, 'eax = 10') === 1, 'shared body printed once');
            check(count(lines, 'eax = 20') === 1, 'each other body printed once');
            check(count(lines, 'eax = 30') === 1, 'default body printed once');
            check(count(lines, 'goto ') === 0, 'join edges use break, not goto');
            check(line_index(lines, 'eax++') > line_index(lines, '}'), 'tail executes once after the switch');
        }
    },
    {
        group: 'aliases',
        name: 'shared_target_without_join_uses_explicit_goto',
        run: function(check) {
            var f = fixtures.aliases_goto();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.join === null, 'a direct-return path means there is no join');
            assert.deepEqual(edge_set(sw.edges), expect_edges([
                [at(f, 'a'), at(f, 'shared')],
                [at(f, 'bb'), at(f, 'shared')],
                [at(f, 'dret'), null],
                [at(f, 'shared'), null]
            ]));
            check(true, 'return edges keep a null destination');

            var lines = r.lines;
            var label = 'label_' + at(f, 'shared');
            check(count(lines, 'goto ' + label) === 2, 'both cases goto the shared block label');
            check(count(lines, label + ':') === 1, 'shared block labeled exactly once');
            check(count(lines, 'eax += 3') === 1, 'shared body is not duplicated');
            check(line_index(lines, 'eax = 9') >= 0, 'default body present');
            check(lines[line_index(lines, 'eax = 9') + 1].str.includes('return'), 'default returns directly');
            check(!sw.edges.some(function(e) {
                return e.from === at(f, 'dret') && e.to !== null;
            }), 'the return is never wired to a join');
        }
    },

    {
        group: 'aliases',
        name: 'default_sharing_a_case_body_uses_one_block',
        run: function(check) {
            var f = fixtures.aliases_default_shared();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.defaultTarget === at(f, 'g1d'), 'default target recorded');
            check(sw.cases[1].target === sw.defaultTarget, 'value 1 and default share one target');
            check(sw.join === at(f, 'join'), 'common tail still detected');

            var lines = r.lines;
            var combined = lines.find(function(l) {
                return l.str.trim() === 'case 1: default:';
            });
            check(!!combined, 'case and default share one label line');
            check(combined.offset === atline(f, 'g1d'), 'combined line offset is the shared block');
            check(count(lines, 'eax = 20') === 1, 'shared body emitted once');
            check(count(lines, 'default:') === 1, 'a single default label in the whole switch');
        }
    },

    // ------------------------------------------------------------ fallthrough
    {
        group: 'fallthrough',
        name: 'fallthrough_follows_graph_edges_not_label_values',
        run: function(check) {
            var f = fixtures.fallthrough_chain();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.join === at(f, 'join'), 'common tail is the join');
            assert.deepEqual(edge_set(sw.edges), expect_edges([
                [at(f, 't0'), at(f, 't2')],
                [at(f, 't2'), at(f, 't1')],
                [at(f, 't1'), at(f, 'join')],
                [at(f, 'dflt'), at(f, 'join')]
            ]));
            check(true, 'physical fallthrough t0->t2 is an explicit edge with no jump op');

            var values = sw.cases.map(function(c) {
                return c.value;
            });
            check(values.join(',') === '5,0,2', 'numeric label order is intentionally not sorted');
            var lines = r.lines;
            var i5 = line_index(lines, 'case 5:');
            var i0 = line_index(lines, 'case 0:');
            var i2 = line_index(lines, 'case 2:');
            check(i5 >= 0 && i0 > i5 && i2 > i0, 'cases print in graph fallthrough order 5 -> 0 -> 2');
            check(lines.slice(i5, i0).every(function(l) {
                return !l.str.includes('break');
            }), 'no break is invented between case 5 and case 0');
            check(lines.slice(i0, i2).every(function(l) {
                return !l.str.includes('break');
            }), 'no break is invented between case 0 and case 2');
            check(line_index(lines, 'break') > i2, 'the real join edge still ends case 2 with a break');
            check(line_index(lines, 'eax++') > line_index(lines, '}'), 'tail stays outside the switch');
        }
    },

    // ------------------------------------------------------------------- exits
    {
        group: 'exits',
        name: 'common_tail_and_early_return_are_kept_distinct',
        run: function(check) {
            var f = fixtures.exits_tail_return();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.join === null, 'direct default return forbids a join');
            assert.deepEqual(edge_set(sw.edges), expect_edges([
                [at(f, 'c0'), at(f, 'tail')],
                [at(f, 'c1'), at(f, 'tail')],
                [at(f, 'dret'), null],
                [at(f, 'tail'), null]
            ]));
            check(true, 'only the return blocks carry null destinations');

            var lines = r.lines;
            check(count(lines, 'goto label_' + at(f, 'tail')) === 2, 'both cases reach the shared tail block');
            check(count(lines, 'eax++') === 1, 'tail body emitted once');
            var default_idx = line_index(lines, 'default:');
            check(lines[default_idx + 1].str.includes('eax = 9'), 'default body');
            check(lines[default_idx + 2].str.includes('return'), 'default returns without touching the tail');
            check(count(lines, 'break') === 0, 'no unconditional break is synthesized');
        }
    },
    {
        group: 'exits',
        name: 'local_if_keeps_both_successors_inside_a_case',
        run: function(check) {
            var f = fixtures.exits_local_if();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.join === at(f, 'join'), 'the tail after the local if is the join');
            var from_l0 = sw.edges.filter(function(e) {
                return e.from === at(f, 'l0');
            });
            check(from_l0.length === 2, 'the conditional block keeps two successors');
            assert.deepEqual(new Set(from_l0.map(function(e) {
                return e.to;
            })), new Set([at(f, 'then'), at(f, 'els')]));
            check(sw.edges.some(function(e) {
                return e.from === at(f, 'then') && e.to === at(f, 'join');
            }), 'taken branch still reaches the join');
            check(sw.edges.some(function(e) {
                return e.from === at(f, 'els') && e.to === at(f, 'join');
            }), 'fall-through branch still reaches the join');

            var lines = r.lines;
            check(line_index(lines, 'if (eax == 0)') >= 0, 'local condition printed');
            check(line_index(lines, 'goto label_' + at(f, 'then')) >= 0, 'taken edge is an explicit goto, not a deleted edge');
            check(count(lines, 'edx = 7') === 1 && count(lines, 'edx = 8') === 1, 'neither branch body is copied or dropped');
            check(line_index(lines, 'eax += edx') > line_index(lines, '}'), 'join printed once after the switch');
        }
    },
    {
        group: 'exits',
        name: 'in_case_loop_keeps_back_edge_without_duplicating_blocks',
        run: function(check) {
            var f = fixtures.exits_loop();
            verify_image(f, check);
            var r = decompile(f.issue);
            var sw = r.session.switches[0];
            validate_structure(sw, check);
            check(sw.join === at(f, 'join'), 'join after the loop region');
            check(sw.edges.some(function(e) {
                return e.from === at(f, 'loop') && e.to === at(f, 'loop');
            }), 'back edge of the loop is preserved');
            check(sw.edges.some(function(e) {
                return e.from === at(f, 'loop') && e.to === at(f, 'after');
            }), 'loop exit edge is preserved');
            check(sw.edges.some(function(e) {
                return e.from === at(f, 'after') && e.to === at(f, 'join');
            }), 'case 1 path reaches the join');

            var lines = r.lines;
            check(line_index(lines, 'do {') >= 0 && line_index(lines, 'while (eax != 3);') >= 0, 'loop rendered structurally');
            check(count(lines, 'eax++') === 1, 'loop body printed once despite the back edge');
            check(line_index(lines, 'case 1:') > line_index(lines, 'while (eax != 3);'), 'next case follows the loop');
            check(count(lines, 'return eax') === 1, 'single function return after the switch');
            check(line_index(lines, 'return eax') > line_index(lines, '}'), 'return sits in the tail outside the switch');
        }
    },
    {
        group: 'exits',
        name: 'function_without_switchop_keeps_legacy_flow',
        run: function(check) {
            var f = fixtures.plain_if();
            verify_image(f, check);
            var r = decompile(f.issue);
            assert.deepEqual(r.session.switches, [], 'no switch recovered without a switchop');
            check(r.session.switchOrder === null, 'block ordering untouched for legacy functions');
            check(count(r.lines, 'switch') === 0, 'no switch keyword printed');
            check(line_index(r.lines, 'if (') >= 0, 'ordinary if/else flow still works');
            check(line_index(r.lines, 'return') >= 0, 'ordinary return still works');
        }
    }
];
