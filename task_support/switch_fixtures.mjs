// SPDX-FileCopyrightText: 2026 r2dec contributors
// SPDX-License-Identifier: BSD-3-Clause

// Synthetic, never-executed x86-64 function graphs for switch recovery tests.
// Each fixture is built with task_support/synth_builder.mjs so every op carries
// legal bytes and the graph jump/fail/switchop edges match the encoding.

import { FixtureBuilder, O } from './synth_builder.mjs';

// --- aliases: shared case labels -------------------------------------------

function build_aliases_join() {
    var b = new FixtureBuilder('aliases_join');
    b.op('guard', O.cmp('eax', 2)).op('guard', O.jcc('ja', 'gdef'));
    b.edge('guard', 'gdef', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    b.switchop('dispatch', {
        selector: 'eax', defval: 'gdef', minval: 0, maxval: 2,
        cases: [{ value: 0, target: 'g01' }, { value: 1, target: 'g01' }, { value: 2, target: 'g2' }]
    });
    b.op('g01', O.mov('eax', 10)).op('g01', O.jmp('join'));
    b.edge('g01', 'join');
    b.op('g2', O.mov('eax', 20)).op('g2', O.jmp('join'));
    b.edge('g2', 'join');
    b.op('gdef', O.mov('eax', 30)).op('gdef', O.jmp('join'));
    b.edge('gdef', 'join');
    b.op('join', O.add_imm('eax', 1)).op('join', O.ret());
    return b.build();
}

function build_aliases_goto() {
    var b = new FixtureBuilder('aliases_goto');
    b.op('guard', O.cmp('eax', 1)).op('guard', O.jcc('ja', 'dret'));
    b.edge('guard', 'dret', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    b.switchop('dispatch', {
        selector: 'eax', defval: 'dret', minval: 0, maxval: 1,
        cases: [{ value: 0, target: 'a' }, { value: 1, target: 'bb' }]
    });
    b.op('a', O.mov('eax', 1)).op('a', O.jmp('shared'));
    b.edge('a', 'shared');
    // bb is laid out after a but jumps to shared: no natural fallthrough.
    b.op('bb', O.mov('eax', 2)).op('bb', O.jmp('shared'));
    b.edge('bb', 'shared');
    b.op('dret', O.mov('eax', 9)).op('dret', O.ret());
    b.op('shared', O.add_imm('eax', 3)).op('shared', O.ret());
    return b.build();
}

// --- aliases: default lands on the same block as an explicit case -----------

function build_aliases_default_shared() {
    var b = new FixtureBuilder('aliases_default_shared');
    b.op('guard', O.cmp('eax', 1)).op('guard', O.jcc('ja', 'g1d'));
    b.edge('guard', 'g1d', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    // defval points at g1d, the same block reached by value 1.
    b.switchop('dispatch', {
        selector: 'eax', defval: 'g1d', minval: 0, maxval: 1,
        cases: [{ value: 0, target: 'g0' }, { value: 1, target: 'g1d' }]
    });
    b.op('g0', O.mov('eax', 10)).op('g0', O.jmp('join'));
    b.edge('g0', 'join');
    b.op('g1d', O.mov('eax', 20)).op('g1d', O.jmp('join'));
    b.edge('g1d', 'join');
    b.op('join', O.add_imm('eax', 1)).op('join', O.ret());
    return b.build();
}

// --- fallthrough: graph order differs from numeric label order --------------

function build_fallthrough_chain() {
    var b = new FixtureBuilder('fallthrough_chain');
    b.op('guard', O.cmp('eax', 5)).op('guard', O.jcc('ja', 'dflt'));
    b.edge('guard', 'dflt', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    // values are deliberately out of the fallthrough order 5 -> 0 -> 2.
    b.switchop('dispatch', {
        selector: 'eax', defval: 'dflt', minval: 0, maxval: 5,
        cases: [{ value: 5, target: 't0' }, { value: 0, target: 't2' }, { value: 2, target: 't1' }]
    });
    // t0 physically falls through into t2 (no jump edge, contiguous blocks).
    b.op('t0', O.mov('eax', 100));
    b.op('t2', O.mov('eax', 200)).op('t2', O.jmp('t1'));
    b.edge('t2', 't1');
    b.op('t1', O.mov('eax', 50)).op('t1', O.jmp('join'));
    b.edge('t1', 'join');
    b.op('dflt', O.mov('eax', 77)).op('dflt', O.jmp('join'));
    b.edge('dflt', 'join');
    b.op('join', O.add_imm('eax', 1)).op('join', O.ret());
    return b.build();
}

// --- exits: common tail, early return, local if, in-case loop ---------------

function build_exits_tail_return() {
    var b = new FixtureBuilder('exits_tail_return');
    b.op('guard', O.cmp('eax', 1)).op('guard', O.jcc('ja', 'dret'));
    b.edge('guard', 'dret', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    b.switchop('dispatch', {
        selector: 'eax', defval: 'dret', minval: 0, maxval: 1,
        cases: [{ value: 0, target: 'c0' }, { value: 1, target: 'c1' }]
    });
    b.op('c0', O.mov('eax', 1)).op('c0', O.jmp('tail'));
    b.edge('c0', 'tail');
    b.op('c1', O.mov('eax', 2)).op('c1', O.jmp('tail'));
    b.edge('c1', 'tail');
    // default returns directly: it must never be wired to the common tail.
    b.op('dret', O.mov('eax', 9)).op('dret', O.ret());
    b.op('tail', O.add_imm('eax', 1)).op('tail', O.ret());
    return b.build();
}

function build_exits_local_if() {
    var b = new FixtureBuilder('exits_local_if');
    b.op('guard', O.cmp('eax', 1)).op('guard', O.jcc('ja', 'dflt'));
    b.edge('guard', 'dflt', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    b.switchop('dispatch', {
        selector: 'eax', defval: 'dflt', minval: 0, maxval: 1,
        cases: [{ value: 0, target: 'l0' }, { value: 1, target: 'l1' }]
    });
    // local if inside case 0: je then, fall through to els; both reach join.
    b.op('l0', O.mov('eax', 1)).op('l0', O.test('eax', 'eax')).op('l0', O.jcc('je', 'then'));
    b.edge('l0', 'then', 'els');
    b.op('els', O.mov('edx', 8)).op('els', O.jmp('join'));
    b.edge('els', 'join');
    b.op('then', O.mov('edx', 7)).op('then', O.jmp('join'));
    b.edge('then', 'join');
    b.op('l1', O.mov('eax', 2)).op('l1', O.jmp('join'));
    b.edge('l1', 'join');
    b.op('dflt', O.mov('eax', 9)).op('dflt', O.jmp('join'));
    b.edge('dflt', 'join');
    b.op('join', O.add_reg('eax', 'edx')).op('join', O.ret());
    return b.build();
}

function build_exits_loop() {
    var b = new FixtureBuilder('exits_loop');
    b.op('guard', O.cmp('eax', 1)).op('guard', O.jcc('ja', 'dflt'));
    b.edge('guard', 'dflt', 'dispatch');
    b.op('dispatch', O.ijmp(0x3100));
    b.switchop('dispatch', {
        selector: 'eax', defval: 'dflt', minval: 0, maxval: 1,
        cases: [{ value: 0, target: 'loop' }, { value: 1, target: 'after' }]
    });
    // back edge inside case 0: do { eax++ } while (eax != 3);
    b.op('loop', O.inc('eax')).op('loop', O.cmp('eax', 3)).op('loop', O.jcc('jne', 'loop'));
    b.edge('loop', 'loop', 'after');
    b.op('after', O.mov('edx', 5)).op('after', O.jmp('join'));
    b.edge('after', 'join');
    b.op('dflt', O.mov('eax', 99)).op('dflt', O.jmp('join'));
    b.edge('dflt', 'join');
    b.op('join', O.nop()).op('join', O.ret());
    return b.build();
}

// --- control: plain function without a switchop -----------------------------

function build_plain_if() {
    var b = new FixtureBuilder('plain_if');
    b.op('entry', O.cmp('eax', 0)).op('entry', O.jcc('je', 'bb'));
    b.edge('entry', 'bb', 'aa');
    b.op('aa', O.mov('eax', 1)).op('aa', O.jmp('cc'));
    b.edge('aa', 'cc');
    b.op('bb', O.mov('eax', 2)).op('bb', O.jmp('cc'));
    b.edge('bb', 'cc');
    b.op('cc', O.ret());
    return b.build();
}

export var fixtures = {
    aliases_join: build_aliases_join,
    aliases_goto: build_aliases_goto,
    aliases_default_shared: build_aliases_default_shared,
    fallthrough_chain: build_fallthrough_chain,
    exits_tail_return: build_exits_tail_return,
    exits_local_if: build_exits_local_if,
    exits_loop: build_exits_loop,
    plain_if: build_plain_if
};
