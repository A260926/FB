/**
 * @file Regression tests for no-unsanitized DOM XSS data-flow tracking.
 *
 * These tests exercise the interaction of three layers that the mocha
 * suite only covers in isolation:
 *
 *  - binding-writes:   const/let initializers plus every writing reference
 *                      decide whether a binding may reach a sink, no matter
 *                      where the write sits relative to the sink.
 *  - expression-graph: string concatenation and template interpolation are
 *                      only safe when every value branch is safe; repeated
 *                      references to one safe binding are not a cycle.
 *  - sink-config:      the same values flow into innerHTML, outerHTML and
 *                      insertAdjacentHTML under default and per-sink
 *                      configurations.
 *
 * Expected results are derived from a small deterministic source
 * generator with its own declaration records (see
 * docs/domxss-flow-regressions.md). The generator never calls into lib/ruleHelper.js, so
 * the expectations stay an independent re-derivation of the documented
 * conservative policy instead of a copy of the detector's answers.
 *
 * All sources are inert strings read by ESLint statically; none of the
 * DOM operations in them are ever executed.
 */
"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");
const test = require("node:test");
const { Linter } = require("eslint");

// tools/check-domxss-mutants.cjs points this at a mutated copy of the
// plugin; by default the tests run against the repository sources.
const PLUGIN_ROOT = process.env.DOMXSS_PLUGIN_ROOT
    ? path.resolve(process.env.DOMXSS_PLUGIN_ROOT)
    : path.join(__dirname, "..", "..");
const plugin = require(path.join(PLUGIN_ROOT, "index.js"));

//------------------------------------------------------------------------------
// Deterministic source generator
//------------------------------------------------------------------------------

// Expression nodes understood by the generator. This mirrors the syntax
// the production rules already handle; nothing here extends the grammar.
const lit = (value) => ({ kind: "lit", value });
const input = () => ({ kind: "input" }); // unsanitized external input
const ref = (name) => ({ kind: "ref", name });
const add = (...parts) => ({ kind: "add", parts }); // string concatenation
const tpl = (...parts) => ({ kind: "tpl", parts }); // template interpolations
const esc = (expr) => ({ kind: "esc", expr }); // escapeHTML`...${expr}...`
const unwrap = (expr) => ({ kind: "unwrap", expr }); // unwrapSafeHTML(expr)

// Declaration record: kind is "const" or "let"; writes are the assignment
// expressions that target the binding, in statement order.
const decl = (name, kind, init = null, writes = []) => ({
    name,
    kind,
    init,
    writes: [...writes],
});

const write = (name, expr) => ({ type: "write", name, expr });
const use = (sink, expr) => ({ type: "use", sink, expr });

const SINKS = ["innerHTML", "outerHTML", "insertAdjacentHTML"];

/**
 * Independent expectation model.
 *
 * Re-derives the documented conservative policy from the declaration
 * records alone:
 *  - literals and template text are safe;
 *  - escapeHTML tagged templates and unwrapSafeHTML calls are safe;
 *  - raw external input is unsafe;
 *  - concatenation and interpolation are safe iff every branch is safe;
 *  - a binding reference is safe iff it resolves to a const/let whose
 *    initializer and every writing reference are safe; var declarations,
 *    parameters, unresolvable bindings and reference cycles are unsafe.
 *
 * @param {object} expr Expression node from the generator.
 * @param {Map} decls Declaration records by name.
 * @param {Set} inProgress Bindings currently being resolved (cycle guard).
 * @returns {boolean} Whether the expression is expected to be allowed.
 */
function isSafeExpr(expr, decls, inProgress = new Set()) {
    switch (expr.kind) {
        case "lit":
            return true;
        case "input":
            return false;
        case "esc":
        case "unwrap":
            return true;
        case "add":
        case "tpl":
            return expr.parts.every((part) =>
                isSafeExpr(part, decls, inProgress)
            );
        case "ref": {
            const record = decls.get(expr.name);
            if (!record || record.kind === "var" || record.param) {
                return false;
            }
            if (inProgress.has(expr.name)) {
                return false; // reference cycle
            }
            inProgress.add(expr.name);
            const initSafe =
                record.init == null || isSafeExpr(record.init, decls, inProgress);
            const writesSafe = record.writes.every((w) =>
                isSafeExpr(w, decls, inProgress)
            );
            inProgress.delete(expr.name);
            return initSafe && writesSafe;
        }
        default:
            throw new Error(`unknown expression kind: ${expr.kind}`);
    }
}

/**
 * Emit the source text for a generated expression.
 *
 * @param {object} expr Expression node from the generator.
 * @returns {string} JavaScript source for the expression.
 */
function emitExpr(expr) {
    switch (expr.kind) {
        case "lit":
            return JSON.stringify(expr.value);
        case "input":
            return "input";
        case "ref":
            return expr.name;
        case "add":
            return expr.parts.map((part) => `(${emitExpr(part)})`).join(" + ");
        case "tpl":
            return `\`${expr.parts
                .map((part) => `\${${emitExpr(part)}}`)
                .join("")}\``;
        case "esc":
            return `escapeHTML\`safe \${${emitExpr(expr.expr)}}\``;
        case "unwrap":
            return `unwrapSafeHTML(${emitExpr(expr.expr)})`;
        default:
            throw new Error(`unknown expression kind: ${expr.kind}`);
    }
}

/**
 * Build a straight-line program: one declaration per line, then the given
 * write/use statements in order, one per line, all starting at column 0.
 *
 * @param {Array} declList Declaration records.
 * @param {Array} statements write() and use() records in statement order.
 * @returns {object} The source text, the use sites with their line numbers,
 *   and the declaration map with all writes attached.
 */
function buildProgram(declList, statements) {
    const decls = new Map(declList.map((d) => [d.name, d]));
    const lines = [];
    for (const d of declList) {
        lines.push(
            d.init == null
                ? `${d.kind} ${d.name};`
                : `${d.kind} ${d.name} = ${emitExpr(d.init)};`
        );
    }
    const uses = [];
    for (const statement of statements) {
        if (statement.type === "write") {
            const record = decls.get(statement.name);
            assert.ok(record, `write to undeclared binding ${statement.name}`);
            record.writes.push(statement.expr);
            lines.push(`${statement.name} = ${emitExpr(statement.expr)};`);
        } else {
            const line = lines.length + 1;
            if (statement.sink === "insertAdjacentHTML") {
                lines.push(
                    `el.insertAdjacentHTML("beforeend", ${emitExpr(statement.expr)});`
                );
            } else {
                lines.push(`el.${statement.sink} = ${emitExpr(statement.expr)};`);
            }
            uses.push({ sink: statement.sink, expr: statement.expr, line });
        }
    }
    return { source: lines.join("\n"), uses, decls };
}

/**
 * Derive the expected reports for a built program from the declaration
 * records. Every unsafe use produces exactly one report at its own line,
 * column 1, under the rule that owns the sink.
 *
 * @param {object} program Result of buildProgram().
 * @returns {Array} Expected report descriptors in source order.
 */
function expectedReports(program) {
    const expected = [];
    for (const u of program.uses) {
        if (!isSafeExpr(u.expr, program.decls)) {
            expected.push({
                line: u.line,
                column: 1,
                ruleId:
                    u.sink === "insertAdjacentHTML"
                        ? "no-unsanitized/method"
                        : "no-unsanitized/property",
                sink: u.sink,
            });
        }
    }
    return expected;
}

//------------------------------------------------------------------------------
// Linting helpers
//------------------------------------------------------------------------------

/**
 * Run both no-unsanitized rules over a source string with a fresh Linter.
 *
 * @param {string} source Source text to lint.
 * @param {object} ruleOptions Map of rule name ("property"/"method") to an
 *   array of rule options, replacing the default "error" shorthand.
 * @returns {Array} ESLint messages, fatals asserted away.
 */
function lint(source, ruleOptions = {}) {
    const linter = new Linter();
    const rules = {
        "no-unsanitized/property": "error",
        "no-unsanitized/method": "error",
    };
    for (const [name, options] of Object.entries(ruleOptions)) {
        rules[`no-unsanitized/${name}`] = ["error", ...options];
    }
    const messages = linter.verify(source, [
        {
            plugins: { "no-unsanitized": plugin },
            languageOptions: { ecmaVersion: 2022 },
            rules,
        },
    ]);
    assert.equal(
        messages.filter((m) => m.fatal).length,
        0,
        `fatal parse errors for:\n${source}\n${JSON.stringify(messages)}`
    );
    return messages;
}

/**
 * Assert that linting a source produces exactly the expected reports:
 * same count, and matching rule id, sink line and column for each.
 *
 * @param {string} source Source text to lint.
 * @param {Array} expected Expected report descriptors in source order.
 * @param {string} label Label for assertion diagnostics.
 * @param {object} ruleOptions Optional rule option overrides.
 */
function assertFlow(source, expected, label, ruleOptions = {}) {
    const messages = lint(source, ruleOptions);
    const actual = [...messages].sort(
        (a, b) => a.line - b.line || a.column - b.column
    );
    assert.equal(
        actual.length,
        expected.length,
        `${label}: expected ${expected.length} report(s), got ${actual.length}\n` +
            `--- source ---\n${source}\n--- messages ---\n${JSON.stringify(actual, null, 1)}`
    );
    for (let i = 0; i < expected.length; i++) {
        const where = `${label} report #${i + 1}`;
        assert.equal(actual[i].ruleId, expected[i].ruleId, `${where} ruleId`);
        assert.equal(actual[i].line, expected[i].line, `${where} line`);
        assert.equal(
            actual[i].column,
            expected[i].column,
            `${where} column`
        );
        assert.ok(
            actual[i].message.includes(expected[i].sink),
            `${where} message should name sink ${expected[i].sink}: ${actual[i].message}`
        );
    }
}

//------------------------------------------------------------------------------
// binding-writes
//------------------------------------------------------------------------------

test("binding-writes", async (t) => {
    await t.test("const/let initialization and writes decide safety", () => {
        const program = buildProgram(
            [
                decl("cSafe", "const", lit("static")),
                decl("lSafe", "let", lit("a"), [lit("b")]),
                decl("lTainted", "let", lit("a"), [input()]),
                decl("cRaw", "const", input()),
            ],
            [
                write("lSafe", lit("b")),
                write("lTainted", input()),
                use("innerHTML", ref("cSafe")),
                use("innerHTML", ref("lSafe")),
                use("innerHTML", ref("lTainted")),
                use("innerHTML", ref("cRaw")),
            ]
        );
        assertFlow(
            program.source,
            expectedReports(program),
            "init+writes decide safety"
        );
    });

    await t.test("a write after the sink still taints the binding", () => {
        const program = buildProgram(
            [decl("x", "let", lit("ok"))],
            [use("innerHTML", ref("x")), write("x", input())]
        );
        const expected = expectedReports(program);
        assert.equal(expected.length, 1, "generator must predict one report");
        assertFlow(program.source, expected, "late write");
    });

    await t.test("uninitialized let with only safe writes is allowed", () => {
        const program = buildProgram(
            [decl("y", "let")],
            [write("y", lit("a")), write("y", lit("b")), use("outerHTML", ref("y"))]
        );
        assertFlow(program.source, expectedReports(program), "uninit let");
    });

    await t.test("safe rewrite chains stay allowed", () => {
        const program = buildProgram(
            [decl("r", "let", lit("a"))],
            [
                write("r", lit("b")),
                use("innerHTML", ref("r")),
                write("r", add(lit("c"), lit("!"))),
            ]
        );
        const expected = expectedReports(program);
        assert.equal(
            expected.length,
            0,
            "generator must predict a fully safe program"
        );
        assertFlow(program.source, expected, "safe rewrites");
    });

    await t.test("handwritten conservative counterexamples", () => {
        // var declarations are conservatively rejected.
        assertFlow(
            'var v = "ok";\nel.innerHTML = v;',
            [
                {
                    line: 2,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "var binding"
        );
        // Function parameters are not traceable values.
        assertFlow(
            "function f(p) {\nel.innerHTML = p;\n}",
            [
                {
                    line: 2,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "function parameter"
        );
        // Bindings that cannot be resolved stay unsafe.
        assertFlow(
            "el.innerHTML = notDeclared;",
            [
                {
                    line: 1,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "unresolvable binding"
        );
        // Reference cycles between declarations are rejected.
        assertFlow(
            "let a = b;\nlet b = a;\nel.innerHTML = a;",
            [
                {
                    line: 3,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "two-binding cycle"
        );
        // A binding that references itself is a degenerate cycle.
        assertFlow(
            "let s = s;\nel.innerHTML = s;",
            [
                {
                    line: 2,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "self reference"
        );
        // A write that reads its own binding is a cycle through the write.
        assertFlow(
            'let w = "a";\nw = w + "?";\nel.innerHTML = w;',
            [
                {
                    line: 3,
                    column: 1,
                    ruleId: "no-unsanitized/property",
                    sink: "innerHTML",
                },
            ],
            "self-referencing write"
        );
    });
});

//------------------------------------------------------------------------------
// expression-graph
//------------------------------------------------------------------------------

test("expression-graph", async (t) => {
    // Shared declaration records for the graph tests: constants, an
    // unsanitized input captured raw, sanitized wrappers and a binding
    // tainted only through a later write.
    const graphDecls = () => [
        decl("k0", "const", lit("a")),
        decl("k1", "const", add(ref("k0"), lit("b"))),
        decl("s0", "const", esc(input())),
        decl("u0", "const", unwrap(input())),
        decl("r0", "const", input()),
        decl("t0", "let", lit("a"), [input()]),
    ];
    const graphStatements = (uses) => [write("t0", input()), ...uses];

    await t.test("concatenation and interpolation of safe values pass", () => {
        const program = buildProgram(
            graphDecls(),
            graphStatements([
                use("innerHTML", add(ref("k0"), ref("k0"))), // repeated safe ref
                use("innerHTML", tpl(ref("k0"), ref("k0"))), // repeated in template
                use("innerHTML", add(ref("k0"), ref("k1"), ref("s0"))),
                use("innerHTML", tpl(ref("k0"), ref("u0"))),
                use("innerHTML", add(tpl(ref("k0"), ref("s0")), ref("u0"), ref("k1"))),
                use("innerHTML", tpl(esc(input()), add(ref("k0"), ref("u0")))),
                use("innerHTML", ref("k1")),
            ])
        );
        const expected = expectedReports(program);
        assert.equal(
            expected.length,
            0,
            "generator must predict a fully safe program"
        );
        assertFlow(program.source, expected, "safe expression graph");
    });

    await t.test("one unsafe branch poisons the whole combination", () => {
        const program = buildProgram(
            graphDecls(),
            graphStatements([
                use("innerHTML", add(ref("s0"), input())), // sanitized + raw
                use("innerHTML", add(ref("k0"), ref("r0"))),
                use("innerHTML", tpl(ref("u0"), ref("t0"))), // write-tainted part
                use("innerHTML", tpl(ref("k0"), add(ref("k0"), input()))),
                use("innerHTML", add(ref("r0"), ref("r0"))), // repeated unsafe ref
                use("innerHTML", ref("r0")),
            ])
        );
        const expected = expectedReports(program);
        assert.equal(
            expected.length,
            program.uses.length,
            "generator must predict every use to be reported"
        );
        assertFlow(program.source, expected, "poisoned expression graph");
    });

    await t.test("mixed graph across rotating sinks", () => {
        const uses = [
            use("innerHTML", add(ref("k0"), ref("s0"))),
            use("outerHTML", add(ref("k0"), ref("r0"))),
            use("insertAdjacentHTML", tpl(ref("u0"), ref("k1"))),
            use("innerHTML", tpl(ref("s0"), ref("t0"))),
            use("outerHTML", add(ref("k1"), ref("u0"), ref("k0"))),
            use("insertAdjacentHTML", add(esc(input()), input())),
        ];
        const program = buildProgram(graphDecls(), graphStatements(uses));
        assertFlow(program.source, expectedReports(program), "rotating sinks");
    });
});

//------------------------------------------------------------------------------
// sink-config
//------------------------------------------------------------------------------

test("sink-config", async (t) => {
    await t.test("the same values through innerHTML, outerHTML and insertAdjacentHTML", () => {
        const values = [
            lit("plain"), // constant: must pass everywhere
            input(), // raw input: reported everywhere
            esc(input()), // fully sanitized: must pass everywhere
            unwrap(input()), // sanitizer call: must pass everywhere
            add(esc(input()), input()), // sanitized + raw: reported everywhere
            ref("cLit"), // safe constant binding: must pass
            ref("lTaint"), // write-tainted binding: reported everywhere
        ];
        const statements = [write("lTaint", input())];
        for (const value of values) {
            for (const sink of SINKS) {
                statements.push(use(sink, value));
            }
        }
        const program = buildProgram(
            [
                decl("cLit", "const", lit("static")),
                decl("lTaint", "let", lit("ok"), [input()]),
            ],
            statements
        );
        const expected = expectedReports(program);
        // raw input, sanitized+raw and the tainted binding, once per sink.
        assert.equal(expected.length, 3 * 3, "generator expectation count");
        assertFlow(program.source, expected, "three-sink matrix");
    });

    await t.test("the position argument cannot mask the HTML argument", () => {
        const messages = lint(
            'el.insertAdjacentHTML("beforeend", input);\n' +
                'el.insertAdjacentHTML("beforeend", "static");\n' +
                'const pos = "beforeend";\n' +
                "el.insertAdjacentHTML(pos, input);"
        );
        assert.equal(messages.length, 2, JSON.stringify(messages, null, 1));
        for (const m of messages) {
            assert.equal(m.ruleId, "no-unsanitized/method");
            assert.match(m.message, /insertAdjacentHTML/);
            assert.match(m.message, /argument 1/);
        }
        assert.deepEqual(
            messages.map((m) => [m.line, m.column]),
            [
                [1, 1],
                [4, 1],
            ]
        );
    });

    await t.test("per-sink escape overrides decide the same binding differently", () => {
        const source =
            "const v = blessProperty(input);\n" +
            "const w = blessMethod(input);\n" +
            "el.innerHTML = v;\n" +
            "el.innerHTML = w;\n" +
            'el.insertAdjacentHTML("beforeend", v);\n' +
            'el.insertAdjacentHTML("beforeend", w);';

        // Default configuration: neither custom sanitizer is known.
        const defaults = lint(source);
        assert.equal(defaults.length, 4, JSON.stringify(defaults, null, 1));

        // A fresh Linter per configuration so no state leaks between runs.
        const propertyLinter = lint(source, {
            property: [{}, { innerHTML: { escape: { methods: ["blessProperty"] } } }],
        });
        // innerHTML accepts v (blessProperty) but still reports w; the
        // method rule keeps its defaults and reports both calls.
        assert.equal(propertyLinter.length, 3, JSON.stringify(propertyLinter, null, 1));
        assert.ok(
            propertyLinter.every(
                (m) =>
                    !(
                        m.ruleId === "no-unsanitized/property" &&
                        m.line === 3
                    )
            ),
            "blessProperty binding must pass innerHTML"
        );
        assert.ok(
            propertyLinter.some(
                (m) => m.ruleId === "no-unsanitized/property" && m.line === 4
            ),
            "blessMethod binding must still fail innerHTML"
        );

        const methodLinter = lint(source, {
            method: [
                {},
                { insertAdjacentHTML: { escape: { methods: ["blessMethod"] } } },
            ],
        });
        // insertAdjacentHTML accepts w (blessMethod) but still reports v;
        // the property rule keeps its defaults and reports both assignments.
        assert.equal(methodLinter.length, 3, JSON.stringify(methodLinter, null, 1));
        assert.ok(
            methodLinter.every(
                (m) => !(m.ruleId === "no-unsanitized/method" && m.line === 6)
            ),
            "blessMethod binding must pass insertAdjacentHTML"
        );
        assert.ok(
            methodLinter.some(
                (m) => m.ruleId === "no-unsanitized/method" && m.line === 5
            ),
            "blessProperty binding must still fail insertAdjacentHTML"
        );
    });

    await t.test("variableTracing disabled follows the existing option", () => {
        const source = 'const c = "ok";\nel.innerHTML = c;\nel.innerHTML = "ok";';
        // With tracing on (default) the constant binding is allowed.
        assert.equal(lint(source).length, 0);
        // With tracing off the identifier is rejected; the literal passes.
        const messages = lint(source, {
            property: [{ variableTracing: false }],
        });
        assert.equal(messages.length, 1, JSON.stringify(messages, null, 1));
        assert.equal(messages[0].ruleId, "no-unsanitized/property");
        assert.equal(messages[0].line, 2);
        assert.equal(messages[0].column, 1);
    });
});
