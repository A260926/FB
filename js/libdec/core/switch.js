// SPDX-FileCopyrightText: 2026 r2dec contributors
// SPDX-License-Identifier: BSD-3-Clause

// Recovery of a single, non nested indirect jump (radare2 `switchop`) into a
// structured `switch`. The pass runs before the regular ControlFlow analysis,
// works on the original radare2 graph (block offsets and jump/fail edges) and
// decorates the same session instruction/block objects that core.print uses.
// It produces both:
//   - the structured printing of the switch (reusing the existing print path),
//   - a serializable description of the recovered region (session.switches).
//
// Only the control flow described by the input graph is used. Cases sharing a
// target share one body, fallthrough is decided from graph edges only (never
// from label values or physical addresses), and no graph edge is removed.

import Base from './base.js';
import Scope from './scope.js';
import Long from '../long.js';
import Extra from './extra.js';

var _hex = function(value) {
    return '0x' + Long.from(value, true).toString(16);
};

// The test-suite JSON decoder emits Long objects for every integer; normalize
// the graph once so the recovery logic can use plain numbers consistently.
var _n = function(value) {
    if (value == null) {
        return null;
    }
    return Long.isLong(value) ? Number(value.toString()) : Number(value);
};

var _normalize_graph = function(raw_graph) {
    return {
        blocks: raw_graph.blocks.map(function(rb) {
            var block = {
                offset: _n(rb.offset),
                jump: _n(rb.jump),
                fail: _n(rb.fail),
                switchop: null,
                ops: rb.ops.map(function(op) {
                    return { offset: _n(op.offset), size: _n(op.size), type: op.type };
                })
            };
            if (rb.switchop) {
                block.switchop = {
                    offset: _n(rb.switchop.offset),
                    selector: rb.switchop.selector,
                    defval: _n(rb.switchop.defval),
                    cases: rb.switchop.cases.map(function(c) {
                        return { value: _n(c.value), jump: _n(c.jump) };
                    })
                };
            }
            return block;
        })
    };
};

// ---------------------------------------------------------------------------
// printable scope objects
// ---------------------------------------------------------------------------

var _switch_head = function(address, selector) {
    this.address = address;
    this.toString = function() {
        return Global().printer.theme.flow('switch') + ' (' + selector + ') {';
    };
    this.print = function() {
        var offset = Global().evars.honor.offsets ? Extra.align_address(this.address) : '';
        Global().context.printLine(Global().context.identfy(offset.length, Global().printer.theme.integers(offset)) + this.toString(), this.address);
        Global().context.identIn();
    };
};

var _case_line = function(address, labels, is_default) {
    this.address = address;
    this.toString = function() {
        var t = Global().printer.theme;
        var parts = labels.map(function(value) {
            return t.flow('case') + ' ' + t.integers(value.toString()) + ':';
        });
        if (is_default) {
            parts.push(t.flow('default') + ':');
        }
        return parts.join(' ');
    };
    this.print = function() {
        Global().context.printLine(Global().context.identfy() + this.toString(), this.address);
    };
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

var _find_instruction = function(instructions, location) {
    for (var i = 0; i < instructions.length; i++) {
        if (instructions[i].location.eq(location)) {
            return instructions[i];
        }
    }
    return null;
};

// Splits the single merged block produced by the pre-analysis into one block
// per raw graph block, so the generic ControlFlow pass can only build scopes
// that stay inside a raw block and can never merge two case bodies.
var _split_session_blocks = function(session, raw_blocks) {
    var block_by_offset = new Map();
    var split_blocks = [];
    var current = session.blocks[0];
    block_by_offset.set(raw_blocks[0].offset, current);
    split_blocks.push(current);
    for (var index = 1; index < raw_blocks.length; index++) {
        var rb = raw_blocks[index];
        var local_index = current.instructions.findIndex(function(instr) {
            return instr.location.eq(rb.offset);
        });
        if (local_index < 0) {
            continue;
        }
        var next = current.split(local_index);
        block_by_offset.set(rb.offset, next);
        split_blocks.push(next);
        current = next;
    }
    session.blocks = split_blocks;
    return block_by_offset;
};

var _build_raw_map = function(raw_blocks) {
    var by_offset = new Map();
    var entries = new Set();
    for (var i = 0; i < raw_blocks.length; i++) {
        var rb = raw_blocks[i];
        by_offset.set(rb.offset, rb);
        entries.add(rb.offset);
    }
    return { by_offset: by_offset, entries: entries };
};

// Successors of a raw block following radare2 graph semantics.
var _raw_successors = function(rb, entries) {
    var last = rb.ops[rb.ops.length - 1];
    if (last.type === 'ret') {
        return { exits: true, conditional: false, targets: [] };
    }
    if (rb.jump != null) {
        var targets = [rb.jump];
        if (rb.fail != null) {
            targets.push(rb.fail);
        }
        return { exits: false, conditional: rb.fail != null, targets: targets };
    }
    var fall = last.offset + last.size;
    return { exits: false, conditional: false, targets: entries.has(fall) ? [fall] : [] };
};

// Forward walk of the raw graph from `start_offsets`; blocks at `cut` are not
// entered but edges towards them are still recorded.
var _explore = function(raw_map, start_offsets, cut) {
    var region = new Set();
    var edges = [];
    var seen = new Set();
    var exits = [];
    var stack = start_offsets.slice();

    var add_edge = function(from, to) {
        var key = _hex(from) + '->' + (to == null ? 'null' : _hex(to));
        if (seen.has(key)) {
            return;
        }
        seen.add(key);
        edges.push({ from: from, to: to });
    };

    while (stack.length) {
        var offset = stack.pop();
        if (region.has(offset)) {
            continue;
        }
        region.add(offset);
        var rb = raw_map.by_offset.get(offset);
        if (!rb) {
            continue;
        }
        var succ = _raw_successors(rb, raw_map.entries);
        if (succ.exits || !succ.targets.length) {
            add_edge(offset, null);
            exits.push(offset);
            continue;
        }
        for (var i = 0; i < succ.targets.length; i++) {
            var to = succ.targets[i];
            add_edge(offset, to);
            if (cut != null && to === cut) {
                continue;
            }
            if (raw_map.by_offset.has(to) && !region.has(to)) {
                stack.push(to);
            }
        }
    }
    return { region: region, edges: edges, exits: exits };
};

// Common post-dominator of the case targets; a direct-return path excludes any
// join candidate, in which case null is returned. EXIT is a virtual node for
// every function return.
var _find_join = function(raw_map, start_offsets) {
    var EXIT = -1;
    var explored = _explore(raw_map, start_offsets, null);
    var nodes = Array.from(explored.region);
    var succ_of = new Map();
    nodes.forEach(function(offset) {
        var rb = raw_map.by_offset.get(offset);
        var succ = _raw_successors(rb, raw_map.entries);
        var targets = succ.targets.filter(function(to) {
            return explored.region.has(to);
        });
        if (!targets.length || succ.exits) {
            targets = targets.concat([EXIT]);
        }
        succ_of.set(offset, Array.from(new Set(targets)));
    });

    var all = new Set(nodes);
    all.add(EXIT);
    var postdom = new Map();
    nodes.forEach(function(offset) {
        postdom.set(offset, new Set(all));
    });
    postdom.set(EXIT, new Set([EXIT]));

    var changed = true;
    while (changed) {
        changed = false;
        nodes.forEach(function(offset) {
            var common = new Set(all);
            succ_of.get(offset).forEach(function(to) {
                var next = postdom.get(to);
                common = new Set(Array.from(common).filter(function(x) {
                    return next.has(x);
                }));
            });
            common.add(offset);
            var previous = postdom.get(offset);
            if (previous.size !== common.size || Array.from(common).some(function(x) {
                return !previous.has(x);
            })) {
                postdom.set(offset, common);
                changed = true;
            }
        });
    }

    var distance = new Map();
    var queue = start_offsets.slice();
    queue.forEach(function(offset) {
        distance.set(offset, 0);
    });
    while (queue.length) {
        var offset = queue.shift();
        succ_of.get(offset).forEach(function(to) {
            if (to !== EXIT && !distance.has(to)) {
                distance.set(to, distance.get(offset) + 1);
                queue.push(to);
            }
        });
    }

    var starts = new Set(start_offsets);
    var shared = null;
    start_offsets.forEach(function(offset) {
        var pd = postdom.get(offset);
        shared = shared == null ? new Set(pd) : new Set(Array.from(shared).filter(function(x) {
            return pd.has(x);
        }));
    });
    var candidates = Array.from(shared).filter(function(offset) {
        return offset !== EXIT && !starts.has(offset);
    });
    if (!candidates.length) {
        return null;
    }
    return candidates.reduce(function(best, offset) {
        var d = distance.get(offset);
        var db = distance.get(best);
        if (d < db || (d === db && offset < best)) {
            return offset;
        }
        return best;
    });
};

var _build_groups = function(switchop) {
    var default_target = switchop.defval != null ? switchop.defval : null;
    var groups = new Map();
    var order = [];
    for (var i = 0; i < switchop.cases.length; i++) {
        var c = switchop.cases[i];
        if (!groups.has(c.jump)) {
            groups.set(c.jump, { target: c.jump, values: [], is_default: false });
            order.push(c.jump);
        }
        groups.get(c.jump).values.push(c.value);
    }
    if (default_target != null && !groups.has(default_target)) {
        groups.set(default_target, { target: default_target, values: [], is_default: true });
        order.push(default_target);
    } else if (default_target != null) {
        groups.get(default_target).is_default = true;
    }
    return order.map(function(k) {
        return groups.get(k);
    });
};

// Natural case order from graph edges only, following the single unconditional
// successor while it is the target of a not-yet-emitted group.
var _order_groups = function(groups, raw_map, region) {
    var group_targets = new Set();
    groups.forEach(function(group) {
        group_targets.add(group.target);
    });
    var ordered = [];
    var placed = new Set();
    var current = groups.length ? groups[0].target : null;

    while (current != null) {
        if (placed.has(current) || !group_targets.has(current)) {
            break;
        }
        var group = groups.filter(function(g) {
            return g.target === current;
        })[0];
        ordered.push(group);
        placed.add(current);

        var rb = raw_map.by_offset.get(current);
        var succ = rb ? _raw_successors(rb, raw_map.entries) : { targets: [] };
        var next = null;
        if (!succ.conditional && succ.targets.length === 1 && region.has(succ.targets[0]) &&
            group_targets.has(succ.targets[0]) && !placed.has(succ.targets[0])) {
            next = succ.targets[0];
        }
        current = next;
    }

    groups.forEach(function(group) {
        if (!placed.has(group.target)) {
            ordered.push(group);
        }
    });
    return ordered;
};

// Structured emission order. Targets reached only through an explicit branch
// (conditional taken edge, goto) are deferred so natural fallthrough stays
// adjacent; the common tail and outer blocks come last.
var _build_emission_order = function(raw_blocks, raw_map, region, ordered_groups, dispatch_offset, deferred) {
    var rank = new Map();
    var next_rank = 0;
    var assign = function(offset) {
        if (!rank.has(offset)) {
            rank.set(offset, next_rank++);
        }
    };

    var dispatch_index = raw_blocks.findIndex(function(rb) {
        return rb.offset === dispatch_offset;
    });
    raw_blocks.forEach(function(rb, index) {
        if (index <= dispatch_index && !region.has(rb.offset)) {
            assign(rb.offset);
        }
    });

    var visit = function(offset) {
        if (rank.has(offset) || !region.has(offset)) {
            return;
        }
        assign(offset);
        var rb = raw_map.by_offset.get(offset);
        if (!rb) {
            return;
        }
        var succ = _raw_successors(rb, raw_map.entries);
        succ.targets.forEach(function(to) {
            if (region.has(to) && !deferred.has(to)) {
                visit(to);
            }
        });
    };
    ordered_groups.forEach(function(group) {
        visit(group.target);
    });
    raw_blocks.forEach(function(rb) {
        if (deferred.has(rb.offset) && region.has(rb.offset)) {
            visit(rb.offset);
        }
    });
    raw_blocks.forEach(function(rb) {
        assign(rb.offset);
    });
    return rank;
};

// ---------------------------------------------------------------------------
// main recovery
// ---------------------------------------------------------------------------

var SwitchRecovery = function(session) {
    session.switches = [];
    session.switchOrder = null;
    session.switchRegion = null;
    if (!session.rawGraph || !session.rawGraph.blocks || !session.rawGraph.blocks.some(function(rb) {
        return rb.switchop;
    })) {
        return;
    }
    var raw_graph = _normalize_graph(session.rawGraph);
    session.switchGraph = raw_graph;
    var block_by_offset = _split_session_blocks(session, raw_graph.blocks);

    for (var bi = 0; bi < raw_graph.blocks.length; bi++) {
        var raw_block = raw_graph.blocks[bi];
        if (!raw_block.switchop) {
            continue;
        }
        var switchop = raw_block.switchop;
        var dispatch_offset = switchop.offset != null ? switchop.offset : raw_block.offset;
        var dispatch_instr = _find_instruction(session.instructions, dispatch_offset);
        if (!dispatch_instr) {
            continue;
        }

        var default_target = switchop.defval != null ? switchop.defval : null;
        var groups = _build_groups(switchop);
        if (!groups.length) {
            continue;
        }
        // Every case target and the dispatch must map to a session block, else
        // leave this function to the generic flow instead of recovering half a
        // switch.
        var recoverable = block_by_offset.has(dispatch_offset) && groups.every(function(group) {
            return block_by_offset.has(group.target);
        });
        if (!recoverable) {
            continue;
        }

        var raw_map = _build_raw_map(raw_graph.blocks);
        var starts = groups.map(function(group) {
            return group.target;
        });

        var join = _find_join(raw_map, starts);
        var explored = _explore(raw_map, starts, join);
        var region = explored.region;
        session.switchRegion = region;

        var edges_out = explored.edges.map(function(edge) {
            return {
                from: _hex(edge.from),
                to: edge.to == null ? null : _hex(edge.to)
            };
        });

        var ordered_groups = _order_groups(groups, raw_map, region);

        // Decide how every unconditional terminator is rendered. Conditional
        // jumps are left to ControlFlow, which now builds only contained scopes.
        var deferred = new Set();
        raw_graph.blocks.forEach(function(rb) {
            if (!region.has(rb.offset)) {
                return;
            }
            var succ = _raw_successors(rb, raw_map.entries);
            if (succ.conditional) {
                // taken branch becomes an explicit labeled goto target.
                deferred.add(succ.targets[0]);
                return;
            }
            succ.targets.forEach(function(to) {
                if (join != null && to === join) {
                    return;
                }
                var group_index = ordered_groups.findIndex(function(g) {
                    return g.target === rb.offset;
                });
                var next_group = group_index >= 0 && group_index + 1 < ordered_groups.length ?
                    ordered_groups[group_index + 1] : null;
                var natural = next_group && succ.targets.length === 1 && to === next_group.target;
                if (!natural) {
                    deferred.add(to);
                }
            });
        });

        session.switchOrder = _build_emission_order(raw_graph.blocks, raw_map, region, ordered_groups, dispatch_offset, deferred);

        // The bounds check guarding the dispatch is redundant once the switch
        // (with its default target) describes both outcomes.
        if (default_target != null) {
            raw_graph.blocks.forEach(function(rb) {
                if (rb.offset !== dispatch_offset && rb.fail === dispatch_offset && rb.jump === default_target) {
                    var guard = _find_instruction(session.instructions, rb.ops[rb.ops.length - 1].offset);
                    if (guard) {
                        guard.code = null;
                        guard.setBadJump();
                    }
                }
            });
        }

        // Indirect jump: replaced by the switch head, its edges become cases.
        dispatch_instr.code = null;
        dispatch_instr.valid = true;
        dispatch_instr.setBadJump();
        block_by_offset.get(dispatch_offset).addExtraHead(
            new _switch_head(Long.from(dispatch_offset, true), switchop.selector));

        ordered_groups.forEach(function(group) {
            block_by_offset.get(group.target).addExtraHead(
                new _case_line(Long.from(group.target, true), group.values, group.is_default));
        });

        // Terminator handling for raw blocks headed by a case (and any other
        // unconditional block inside the region).
        raw_graph.blocks.forEach(function(rb) {
            if (!region.has(rb.offset)) {
                return;
            }
            var succ = _raw_successors(rb, raw_map.entries);
            if (succ.conditional || succ.exits || !succ.targets.length) {
                return;
            }
            var last_op = rb.ops[rb.ops.length - 1];
            var term = _find_instruction(session.instructions, last_op.offset);
            if (!term || !term.jump) {
                return;
            }
            var group_index = ordered_groups.findIndex(function(g) {
                return g.target === rb.offset;
            });
            var next_group = group_index >= 0 && group_index + 1 < ordered_groups.length ?
                ordered_groups[group_index + 1] : null;
            var natural = next_group && succ.targets.length === 1 && succ.targets[0] === next_group.target;
            if (join != null && succ.targets.indexOf(join) >= 0) {
                // real edge towards the common tail: the case ends here.
                term.code = Base.break();
                term.setBadJump();
            } else if (natural) {
                // graph fallthrough into the next case: no synthetic break.
                term.code = null;
                term.setBadJump();
            }
            // every other unconditional jump stays for ControlFlow, which turns
            // it into an explicit (address based) goto without dropping edges.
        });

        // Closing brace on the last *printed* region block (highest emission
        // rank): every region block, including deferred goto/local-if targets,
        // stays inside the switch; only the common tail prints outside it.
        var anchor_offset = null;
        region.forEach(function(offset) {
            if (anchor_offset == null || session.switchOrder.get(offset) > session.switchOrder.get(anchor_offset)) {
                anchor_offset = offset;
            }
        });
        var anchor_raw = raw_map.by_offset.get(anchor_offset);
        var brace_offset = anchor_raw.ops[anchor_raw.ops.length - 1].offset;
        block_by_offset.get(anchor_offset).addExtraTail(
            new Scope.brace(Long.from(brace_offset, true)));

        session.switches.push({
            dispatch: _hex(dispatch_offset),
            selector: switchop.selector,
            cases: switchop.cases.map(function(c) {
                return { value: c.value, target: _hex(c.jump) };
            }),
            defaultTarget: default_target == null ? null : _hex(default_target),
            join: join == null ? null : _hex(join),
            edges: edges_out
        });
    }
};

// After ControlFlow: order blocks per the structured layout and give every
// label inside the recovered region an explicit address based name.
SwitchRecovery.orderBlocks = function(session) {
    if (!session.switchOrder || !session.blocks || !session.switchGraph) {
        return;
    }
    var rank = session.switchOrder;
    var owner = new Map();
    session.switchGraph.blocks.forEach(function(rb) {
        rb.ops.forEach(function(op) {
            owner.set(op.offset, rb.offset);
        });
    });
    var rank_of = function(block) {
        var offset = owner.get(Number(block.bounds.low.toString()));
        if (offset == null || !rank.has(offset)) {
            return Number.MAX_SAFE_INTEGER;
        }
        return rank.get(offset);
    };
    session.blocks.sort(function(a, b) {
        var ra = rank_of(a);
        var rb = rank_of(b);
        if (ra !== rb) {
            return ra - rb;
        }
        return a.bounds.low.lt(b.bounds.low) ? -1 : (a.bounds.low.eq(b.bounds.low) ? 0 : 1);
    });

    var region = session.switchRegion;
    var rename = function(label) {
        if (!label || !label.address || !region.has(Number(label.address.toString()))) {
            return;
        }
        label.name = 'label_' + _hex(label.address);
    };
    session.instructions.forEach(function(instr) {
        rename(instr.label);
        var codes = [];
        if (instr.code) {
            codes = instr.code.composed ? instr.code.composed : [instr.code];
        }
        codes.forEach(function(code) {
            if (code && code.value && Long.isLong(code.value.address)) {
                rename(code.value);
            }
        });
    });
};

export default SwitchRecovery;
