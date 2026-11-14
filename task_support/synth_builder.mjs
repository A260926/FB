// SPDX-FileCopyrightText: 2026 r2dec contributors
// SPDX-License-Identifier: BSD-3-Clause

// Deterministic builder for the synthetic x86-64 radare2-style function graphs
// used by switch_recovery_test.mjs. Every emitted instruction carries legal
// machine bytes (never executed). Jumps reference block keys; a fixed-point
// layout pass assigns contiguous offsets and resolves every displacement, so
// the graph and the byte listing always agree.
// Only JavaScript built-in modules are required.

import { createHash } from 'node:crypto';

var BASE = 0x2000;

var hex = function(bytes) {
    return Buffer.from(bytes).toString('hex');
};

var i8 = function(value) {
    return [value & 0xff];
};

var i32 = function(value) {
    var b = [];
    for (var i = 0; i < 4; i++) {
        b.push((value >>> (i * 8)) & 0xff);
    }
    return b;
};

var ENCODERS = {
    ret: { size: function() { return 1; }, emit: function() { return [0xc3]; } },
    nop: { size: function() { return 1; }, emit: function() { return [0x90]; } },
    mov: {
        size: function() { return 5; },
        emit: function(op) {
            var regs = { eax: 0xb8, ecx: 0xb9, edx: 0xba, ebx: 0xbb };
            return [regs[op.args[0]]].concat(i32(op.args[1] >>> 0));
        }
    },
    xor: {
        size: function() { return 2; },
        emit: function(op) {
            var pairs = { 'eax,eax': [0x31, 0xc0], 'edx,edx': [0x31, 0xd2], 'ecx,ecx': [0x31, 0xc9] };
            return pairs[op.args.join(',')];
        }
    },
    cmp: {
        size: function() { return 3; },
        emit: function(op) { return [0x83, 0xf8, op.args[1] & 0xff]; }
    },
    test: {
        size: function() { return 2; },
        emit: function(op) {
            var modrm = { 'eax,eax': [0x85, 0xc0], 'edx,edx': [0x85, 0xd2], 'ecx,ecx': [0x85, 0xc9] };
            return modrm[op.args.join(',')];
        }
    },
    add_imm: {
        size: function() { return 3; },
        emit: function(op) { return [0x83, 0xc0, op.args[1] & 0xff]; }
    },
    add_reg: {
        size: function() { return 2; },
        emit: function(op) {
            var pairs = { 'eax,edx': [0x01, 0xd0], 'eax,ecx': [0x01, 0xc8] };
            return pairs[op.args.join(',')];
        }
    },
    inc: {
        size: function() { return 1; },
        emit: function(op) {
            return [{ eax: 0x40, ecx: 0x41, edx: 0x42, ebx: 0x43 }[op.args[0]]];
        }
    },
    jmp: {
        size: function(op) { return op.near ? 5 : 2; },
        emit: function(op, offset, resolve) {
            var to = resolve(op.args[0]);
            if (op.near) {
                return [0xe9].concat(i32(to - (offset + 5)));
            }
            return [0xeb].concat(i8(to - (offset + 2)));
        }
    },
    jcc: {
        size: function() { return 2; },
        emit: function(op, offset, resolve) {
            var cc = { ja: 0x77, je: 0x74, jne: 0x75, jg: 0x7f };
            var to = resolve(op.args[0]);
            return [cc[op.mnem]].concat(i8(to - (offset + 2)));
        }
    },
    ijmp: {
        size: function() { return 7; },
        emit: function(op) {
            return [0xff, 0x24, 0xc5].concat(i32(op.args[0] >>> 0));
        }
    }
};

var OPCODE_TEXT = {
    mov: function(op) { return 'mov ' + op.args[0] + ', ' + op.args[1]; },
    xor: function(op) { return 'xor ' + op.args.join(', '); },
    cmp: function(op) { return 'cmp ' + op.args.join(', '); },
    test: function(op) { return 'test ' + op.args.join(', '); },
    add_imm: function(op) { return 'add ' + op.args.join(', '); },
    add_reg: function(op) { return 'add ' + op.args.join(', '); },
    inc: function(op) { return 'inc ' + op.args[0]; },
    jmp: function(op, resolve) { return 'jmp 0x' + resolve(op.args[0]).toString(16); },
    jcc: function(op, resolve) { return op.mnem + ' 0x' + resolve(op.args[0]).toString(16); },
    ijmp: function(op) { return 'jmp qword [rax*8 + 0x' + op.args[0].toString(16) + ']'; },
    ret: function() { return 'ret'; },
    nop: function() { return 'nop'; }
};

export function FixtureBuilder(name) {
    this.name = name;
    this._blocks = new Map();
    this._order = [];

    this.block = function(key) {
        if (!this._blocks.has(key)) {
            this._blocks.set(key, { key: key, ops: [], jump: null, fail: null, switchop: null });
            this._order.push(key);
        }
        return this._blocks.get(key);
    };

    this.op = function(key, op) {
        this.block(key).ops.push(op);
        return this;
    };

    // jump/fail use block keys; a null key means "no edge" (function exit).
    this.edge = function(key, jump, fail) {
        var block = this.block(key);
        block.jump = jump == null ? null : jump;
        block.fail = fail == null ? null : fail;
        return this;
    };

    this.switchop = function(key, spec) {
        this.block(key).switchop = {
            offset_key: spec.offset_key || key,
            selector: spec.selector,
            defval: spec.defval,
            minval: spec.minval,
            maxval: spec.maxval,
            cases: spec.cases.map(function(c) {
                return { value: c.value, target: c.target };
            })
        };
        return this;
    };

    this.build = function() {
        var blocks = this._order.map(function(key) {
            return this._blocks.get(key);
        }, this);

        // fixed-point layout: an unconditional jmp may need a near (5 byte)
        // encoding once the real displacement is known.
        var layout = new Map();
        var changed = true;
        while (changed) {
            changed = false;
            var cursor = BASE;
            blocks.forEach(function(block) {
                layout.set(block.key, cursor);
                block.ops.forEach(function(op) {
                    cursor += ENCODERS[op.enc].size(op);
                });
            });
            blocks.forEach(function(block) {
                block.ops.forEach(function(op) {
                    if (op.enc !== 'jmp') {
                        return;
                    }
                    var from = layout.get(block.key) + block.ops.slice(0, block.ops.indexOf(op)).reduce(function(acc, o) {
                        return acc + ENCODERS[o.enc].size(o);
                    }, 0);
                    var disp = layout.get(op.args[0]) - (from + 2);
                    var needs_near = disp > 127 || disp < -128;
                    if (needs_near && !op.near) {
                        op.near = true;
                        changed = true;
                    }
                });
            });
        }

        var resolve = function(key) {
            if (!layout.has(key)) {
                throw new Error('unknown block key: ' + key);
            }
            return layout.get(key);
        };

        // final offsets and encodings.
        var image_size = 0;
        blocks.forEach(function(block) {
            var cursor = layout.get(block.key);
            block.ops.forEach(function(op) {
                op.offset = cursor;
                var encoded = ENCODERS[op.enc].emit(op, cursor, resolve);
                op.bytes = hex(encoded);
                op.size = encoded.length;
                op.opcode = OPCODE_TEXT[op.enc](op, resolve);
                cursor += op.size;
                image_size = Math.max(image_size, cursor - BASE);
            });
        });

        // sanity: every conditional jump must fit in its rel8 displacement.
        blocks.forEach(function(block) {
            block.ops.forEach(function(op) {
                if (op.enc === 'jcc') {
                    var disp = resolve(op.args[0]) - (op.offset + 2);
                    if (disp > 127 || disp < -128) {
                        throw new Error('conditional jump out of rel8 range in ' + this.name);
                    }
                }
            }, this);
        }, this);

        var image = Buffer.alloc(image_size);
        var out_blocks = blocks.map(function(block) {
            block.ops.forEach(function(op) {
                Buffer.from(op.bytes, 'hex').copy(image, op.offset - BASE);
            });
            var switchop = null;
            if (block.switchop) {
                switchop = {
                    offset: resolve(block.switchop.offset_key),
                    selector: block.switchop.selector,
                    defval: resolve(block.switchop.defval),
                    minval: block.switchop.minval,
                    maxval: block.switchop.maxval,
                    cases: block.switchop.cases.map(function(c) {
                        return { value: c.value, jump: resolve(c.target) };
                    })
                };
            }
            return {
                offset: layout.get(block.key),
                jump: block.jump == null ? undefined : resolve(block.jump),
                fail: block.fail == null ? undefined : resolve(block.fail),
                switchop: switchop,
                ops: block.ops.map(function(op) {
                    var out = {
                        offset: op.offset,
                        opcode: op.opcode,
                        bytes: op.bytes,
                        size: op.size,
                        type: op.type
                    };
                    if (op.type === 'jmp' || op.type === 'cjmp') {
                        out.jump = resolve(op.args[0]);
                    }
                    return out;
                })
            };
        });

        var sha256 = createHash('sha256').update(image).digest('hex');
        var issue = {
            arch: 'x86',
            bits: 64,
            image: { base: BASE, sha256: sha256, kind: 'legal synthetic x86-64 raw image; never execute' },
            graph: [{ name: this.name, blocks: out_blocks }]
        };
        return { name: this.name, issue: issue, image: image, sha256: sha256, layout: layout };
    };
}

export var O = {
    mov: function(reg, imm) { return { enc: 'mov', type: 'mov', args: [reg, imm] }; },
    xor: function(a, b) { return { enc: 'xor', type: 'xor', args: [a, b] }; },
    cmp: function(reg, imm) { return { enc: 'cmp', type: 'cmp', args: [reg, imm] }; },
    test: function(a, b) { return { enc: 'test', type: 'test', args: [a, b] }; },
    add_imm: function(reg, imm) { return { enc: 'add_imm', type: 'add', args: [reg, imm] }; },
    add_reg: function(a, b) { return { enc: 'add_reg', type: 'add', args: [a, b] }; },
    inc: function(reg) { return { enc: 'inc', type: 'inc', args: [reg] }; },
    jmp: function(key) { return { enc: 'jmp', type: 'jmp', args: [key] }; },
    jcc: function(mnem, key) { return { enc: 'jcc', type: 'cjmp', mnem: mnem, args: [key] }; },
    ijmp: function(table) { return { enc: 'ijmp', type: 'ijmp', args: [table] }; },
    ret: function() { return { enc: 'ret', type: 'ret', args: [] }; },
    nop: function() { return { enc: 'nop', type: 'nop', args: [] }; }
};
