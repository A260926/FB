/**
 * Rank contract regression tests for `:nth-child(An+B [of S])` and friends.
 *
 * The reference results in this file are computed from a small hand-written
 * page model. The model never calls into the library's positional
 * pseudo-classes, never touches the compiler internals and never imports
 * `nth-check`. Every expected result is derived from the integer An+B
 * formula alone, after the matching sibling set has been filtered. Each
 * library query is only compared against that independent expectation; one
 * query's result is never used as the answer for another.
 *
 * Run with: `node --test checks/rank-contract.test.mjs`
 */
import assert from "node:assert/strict";
import { test } from "node:test";
import { Comment, Text, isTag } from "domhandler";
import * as DomUtils from "domutils";
import { parseDocument } from "htmlparser2";
import { compile, selectAll } from "../dist/index.js";

/* ------------------------------------------------------------------ */
/* Fixed, readable page seeds. Two structurally different trees reuse  */
/* The same element ids on purpose.                                    */
/* ------------------------------------------------------------------ */

const ALPHA = {
    name: "alpha",
    parents: [
        {
            id: "p1",
            tag: "section",
            cls: ["feed"],
            children: [
                { tag: "i", id: "d1", cls: ["hot", "pick"] },
                { text: "lead-line" },
                { comment: "note-one" },
                { tag: "b", id: "d2", cls: ["ad"] },
                { tag: "i", id: "d3", cls: ["pick", "hot"], marked: true },
                { tag: "b", id: "d4", cls: ["pick", "card"], marked: true },
                { tag: "i", id: "d5", cls: ["pick"] },
                { comment: "note-two" },
                { tag: "b", id: "d6", cls: ["hot"] },
                { tag: "i", id: "d7", cls: ["card"] },
            ],
        },
        {
            id: "p2",
            tag: "div",
            cls: ["wrap"],
            children: [
                { tag: "b", id: "x1", cls: ["ad"] },
                { text: "wrap-gap" },
                { tag: "i", id: "x2", cls: ["pick"] },
                { tag: "b", id: "x3", cls: ["pick"] },
                { comment: "wrap-tail" },
                { tag: "i", id: "x4", cls: ["ad"] },
            ],
        },
        {
            id: "p3",
            tag: "div",
            cls: ["emptyish"],
            children: [
                { text: "only-text" },
                { comment: "only-comment" },
                { tag: "b", id: "z1", cls: ["ad"] },
            ],
        },
    ],
};

const BETA = {
    name: "beta",
    parents: [
        {
            id: "p1",
            tag: "ul",
            cls: ["list"],
            children: [
                { tag: "li", id: "d1", cls: ["pick", "hot"] },
                { comment: "beta-gap" },
                {
                    tag: "div",
                    id: "d2",
                    cls: ["pick", "card"],
                    marked: true,
                },
                { text: "beta-text" },
                { tag: "li", id: "d3", cls: ["ad"] },
                { tag: "li", id: "d4", cls: ["pick"] },
                { tag: "div", id: "d5", cls: ["hot", "card"] },
            ],
        },
        {
            id: "p2",
            tag: "nav",
            cls: ["strip"],
            children: [
                { tag: "span", id: "x1", cls: ["ad"] },
                { text: "strip-gap" },
                { tag: "span", id: "x2", cls: ["pick"] },
                { tag: "span", id: "x3", cls: ["pick"] },
            ],
        },
        {
            id: "p3",
            tag: "aside",
            cls: ["blank"],
            children: [
                { text: "blank-text" },
                { comment: "blank-comment" },
                { tag: "a", id: "z1", cls: ["ad"] },
            ],
        },
    ],
};

/* ------------------------------------------------------------------ */
/* Page construction: one seed drives both the DOM and the model.      */
/* ------------------------------------------------------------------ */

function renderPage(seed) {
    let html = "";
    for (const parent of seed.parents) {
        html += `<${parent.tag} id="${parent.id}" class="${parent.cls.join(" ")}">`;
        for (const child of parent.children) {
            if (child.text !== undefined) {
                html += child.text;
            } else if (child.comment === undefined) {
                const classAttribute = child.cls.length > 0
                    ? ` class="${child.cls.join(" ")}"`
                    : "";
                html += child.marked ? `<${child.tag} id="${child.id}"${classAttribute}><em mark="" id="${child.id}-m"></em></${child.tag}>` : `<${child.tag} id="${child.id}"${classAttribute}></${child.tag}>`;
            } else {
                html += `<!--${child.comment}-->`;
            }
        }
        html += `</${parent.tag}>`;
    }
    return html;
}

function buildPage(seed) {
    const model = {
        name: seed.name,
        parents: seed.parents.map((parent) => ({
            id: parent.id,
            tag: parent.tag,
            cls: [...parent.cls],
            nodes: parent.children.map((child, index) =>
                child.text === undefined
                    ? child.comment === undefined
                      ? {
                            kind: "element",
                            label: child.id,
                            id: child.id,
                            tag: child.tag,
                            cls: [...child.cls],
                            marked: child.marked === true,
                        }
                      : {
                            kind: "comment",
                            label: `${parent.id}/comment#${index}`,
                        }
                    : {
                          kind: "text",
                          label: `${parent.id}/text#${index}`,
                      },
            ),
        })),
    };

    /*
     * A marked element is itself a parent with one direct marker child.
     * Model those nested sibling lists too, so every element the library can
     * rank has a counterpart in the model.
     */
    for (const parent of seed.parents) {
        for (const child of parent.children) {
            if (child.marked) {
                model.parents.push({
                    id: child.id,
                    tag: child.tag,
                    cls: [...child.cls],
                    nodes: [
                        {
                            kind: "element",
                            label: `${child.id}-m`,
                            id: `${child.id}-m`,
                            tag: "em",
                            cls: [],
                            marked: false,
                        },
                    ],
                });
            }
        }
    }

    const document = parseDocument(renderPage(seed));
    const elementsById = new Map();
    for (const element of walkElements(document)) {
        elementsById.set(element.attribs.id, element);
    }
    const parentsById = new Map(
        model.parents.map((parent) => [
            parent.id,
            elementsById.get(parent.id),
        ]),
    );

    return { seed, model, doc: document, elementsById, parentsById };
}

function* walkElements(node) {
    for (const child of node.childNodes) {
        if (!isTag(child)) {
        	continue;
        }

        yield child;
        yield* walkElements(child);
    }
}

/*
 * Library results are collected per modeled parent, over a defensive copy of
 * its live child-node list. Passing the copy keeps `removeSubsets` from
 * mutating the DOM. `selectAll` also walks descendants, so the collected
 * matches are restricted to direct children — the exact sibling domain the
 * model describes; nested parents are evaluated on their own. Evaluating
 * each parent separately makes the contexts match the model's sibling lists.
 */
function actualSelectAllIds(page, parentModel, query) {
    const parentElement = page.parentsById.get(parentModel.id);
    return selectAll(query, [...parentElement.childNodes], {
        cacheResults: false,
    })
        .filter((element) => element.parent === parentElement)
        .map((element) => element.attribs.id);
}

function actualPredicateIds(page, parentModel, predicate) {
    const parentElement = page.parentsById.get(parentModel.id);
    return [...parentElement.childNodes]
        .filter(isTag)
        .filter((element) => predicate(element))
        .map((element) => element.attribs.id);
}

function actualIdsForPage(page, query) {
    return page.model.parents.flatMap((parentModel) =>
        actualSelectAllIds(page, parentModel, query),
    );
}

function actualPredicateForPage(page, predicate) {
    return page.model.parents.flatMap((parentModel) =>
        actualPredicateIds(page, parentModel, predicate),
    );
}

/* ------------------------------------------------------------------ */
/* Independent reference machinery. No positional pseudos, no          */
/* Nth-check, no compiler calls: just set filtering and integer math.  */
/* ------------------------------------------------------------------ */

// Model-level counterparts of the `of S` selectors sent to the library.
const OF_PREDICATES = {
    pick: (node) => node.kind === "element" && node.cls.includes("pick"),
    // `.hot, i`: one element may satisfy both branches yet occupies one slot.
    "hot-or-i": (node) =>
        node.kind === "element" &&
        (node.cls.includes("hot") || node.tag === "i"),
    // `.card:has(> [mark])`: structural condition, not just a class string.
    "card-with-direct-mark": (node) =>
        node.kind === "element" && node.cls.includes("card") && node.marked,
};

function parseFormula(input) {
    const rule = input.replace(/\s+/g, "");
    if (rule === "odd") return [2, 1];
    if (rule === "even") return [2, 0];

    const nthMatch = rule.match(/^([+-]?\d*)n(?:([+-]\d+))?$/);
    if (nthMatch) {
        const coefficient =
            nthMatch[1] === "" || nthMatch[1] === "+"
                ? 1
                : nthMatch[1] === "-"
                  ? -1
                  : Number(nthMatch[1]);
        const offset =
            nthMatch[2] === undefined ? 0 : Number(nthMatch[2]);
        return [coefficient, offset];
    }

    if (/^[+-]?\d+$/.test(rule)) {
        return [0, Number(rule)];
    }

    throw new Error(`reference cannot parse formula ${input}`);
}

/*
 * `rank` is the zero-based position inside the already-filtered sibling set,
 * counted either from the start or from the end.
 */
function formulaMatches(input, rank) {
    const [coefficient, offset] = parseFormula(input);
    if (coefficient === 0) return rank + 1 === offset;
    const step = (rank + 1 - offset) / coefficient;
    return Number.isSafeInteger(step) && step >= 0;
}

function expectedIdsForParent(parentModel, check) {
    if (
        check.outer &&
        !(
            parentModel.tag === check.outer.tag &&
            parentModel.cls.includes(check.outer.cls)
        )
    ) {
        return [];
    }

    const elements = parentModel.nodes.filter(
        (node) => node.kind === "element",
    );

    let pool;
    if (check.of) {
        const ofPredicate = OF_PREDICATES[check.of];
        pool = elements.filter((node) => ofPredicate(node));
    } else if (check.kind === "of-type" || check.kind === "last-of-type") {
        pool = elements.filter((node) => node.tag === check.typeTag);
    } else {
        pool = elements;
    }

    const isFromEnd =
        check.kind === "last-child" || check.kind === "last-of-type";

    return elements
        .filter((node) => {
            // The candidate itself has to belong to the filtered set.
            if (check.of && !OF_PREDICATES[check.of](node)) return false;
            if (
                (check.kind === "of-type" ||
                    check.kind === "last-of-type") &&
                node.tag !== check.typeTag
            ) {
                return false;
            }

            const forwardIndex = pool.indexOf(node);
            if (forwardIndex === -1) return false;
            const rank = isFromEnd
                ? pool.length - 1 - forwardIndex
                : forwardIndex;
            return formulaMatches(check.formula, rank);
        })
        .map((node) => node.id);
}

function expectedIdsForPage(page, check) {
    return page.model.parents.flatMap((parent) =>
        expectedIdsForParent(parent, check),
    );
}

/* ------------------------------------------------------------------ */
/* Checks. The `css` string is the only thing the library sees; the     */
/* Remaining fields describe the same expectation for the model.       */
/* ------------------------------------------------------------------ */

const PICK_CHECKS = [
    {
        css: ":nth-child(1 of .pick)",
        kind: "child",
        formula: "1",
        of: "pick",
    },
    {
        css: ":nth-child(2 of .pick)",
        kind: "child",
        formula: "2",
        of: "pick",
    },
    {
        css: ":nth-child(0n+2 of .pick)",
        kind: "child",
        formula: "0n+2",
        of: "pick",
    },
    {
        css: ":nth-child(2n+1 of .pick)",
        kind: "child",
        formula: "2n+1",
        of: "pick",
    },
    {
        css: ":nth-child(-n+2 of .pick)",
        kind: "child",
        formula: "-n+2",
        of: "pick",
    },
];

const REVERSE_CHECKS = [
    {
        css: ":nth-last-child(1 of .pick)",
        kind: "last-child",
        formula: "1",
        of: "pick",
    },
    {
        css: ":nth-last-child(2 of .pick)",
        kind: "last-child",
        formula: "2",
        of: "pick",
    },
    {
        css: ":nth-last-child(-n+2 of .pick)",
        kind: "last-child",
        formula: "-n+2",
        of: "pick",
    },
    {
        css: ":nth-last-child(2n+1 of .pick)",
        kind: "last-child",
        formula: "2n+1",
        of: "pick",
    },
    { css: ":nth-last-child(1)", kind: "last-child", formula: "1" },
    { css: ":nth-last-child(2)", kind: "last-child", formula: "2" },
];

const BRANCH_CHECKS = [
    {
        css: ":nth-child(2n of .hot, i)",
        kind: "child",
        formula: "2n",
        of: "hot-or-i",
    },
    {
        css: ":nth-child(2n+1 of .hot, i)",
        kind: "child",
        formula: "2n+1",
        of: "hot-or-i",
    },
];

const MARKED_CHECKS = [
    {
        css: ":nth-child(1 of .card:has(> [mark]))",
        kind: "child",
        formula: "1",
        of: "card-with-direct-mark",
    },
    {
        css: ":nth-last-child(1 of .card:has(> [mark]))",
        kind: "last-child",
        formula: "1",
        of: "card-with-direct-mark",
    },
    {
        css: "section.feed > :nth-child(1 of .card:has(> [mark]))",
        kind: "child",
        formula: "1",
        of: "card-with-direct-mark",
        outer: { tag: "section", cls: "feed" },
    },
];

const PLAIN_CHECKS = [
    { css: ":nth-child(2)", kind: "child", formula: "2" },
    { css: ":nth-child(3)", kind: "child", formula: "3" },
    { css: ":nth-child(4)", kind: "child", formula: "4" },
    { css: ":nth-child(6)", kind: "child", formula: "6" },
];

const TYPE_CHECKS = [
    { css: "i:nth-of-type(2)", kind: "of-type", formula: "2", typeTag: "i" },
    { css: "b:nth-of-type(2)", kind: "of-type", formula: "2", typeTag: "b" },
    {
        css: "li:nth-of-type(2)",
        kind: "of-type",
        formula: "2",
        typeTag: "li",
    },
    {
        css: "div:nth-of-type(2)",
        kind: "of-type",
        formula: "2",
        typeTag: "div",
    },
    {
        css: "i:nth-last-of-type(1)",
        kind: "last-of-type",
        formula: "1",
        typeTag: "i",
    },
    {
        css: "b:nth-last-of-type(1)",
        kind: "last-of-type",
        formula: "1",
        typeTag: "b",
    },
];

const OUTER_CHECKS = [
    {
        css: "section.feed > :nth-child(2 of .pick)",
        kind: "child",
        formula: "2",
        of: "pick",
        outer: { tag: "section", cls: "feed" },
    },
    {
        css: "ul.list > :nth-last-child(1 of .pick)",
        kind: "last-child",
        formula: "1",
        of: "pick",
        outer: { tag: "ul", cls: "list" },
    },
];

const ALL_CHECKS = [
    ...PICK_CHECKS,
    ...REVERSE_CHECKS,
    ...BRANCH_CHECKS,
    ...MARKED_CHECKS,
    ...PLAIN_CHECKS,
    ...TYPE_CHECKS,
    ...OUTER_CHECKS,
];

/* ------------------------------------------------------------------ */
/* Assertion helpers with full diagnostics on failure.                 */
/* ------------------------------------------------------------------ */

function snapshot(page) {
    return page.model.parents.map((parent) => ({
        id: parent.id,
        tag: parent.tag,
        cls: parent.cls,
        nodes: parent.nodes.map((node) =>
            node.kind === "element"
                ? {
                      kind: node.kind,
                      id: node.id,
                      tag: node.tag,
                      cls: node.cls,
                      marked: node.marked,
                  }
                : { kind: node.kind, label: node.label },
        ),
    }));
}

function failureReport(page, css, expected, actual, phase) {
    return JSON.stringify(
        {
            phase: phase ?? "static",
            page: page.name,
            selector: css,
            expectedIds: expected,
            actualIds: actual,
            tree: snapshot(page),
        },
        null,
        2,
    );
}

function assertSelectAll(page, check, phase) {
    const expected = expectedIdsForPage(page, check);
    const actual = actualIdsForPage(page, check.css);
    assert.deepEqual(
        actual,
        expected,
        failureReport(page, check.css, expected, actual, phase),
    );
}

function assertCompiledPredicate(page, check, phase) {
    const expected = expectedIdsForPage(page, check);
    const predicate = compile(check.css, { cacheResults: false });
    const actual = actualPredicateForPage(page, predicate);
    assert.deepEqual(
        actual,
        expected,
        failureReport(page, check.css, expected, actual, phase),
    );
}

/* ------------------------------------------------------------------ */
/* Rank-model: independent model vs. selectAll and compiled predicates */
/* ------------------------------------------------------------------ */

test("rank-model", async (t) => {
    const alpha = buildPage(ALPHA);
    const beta = buildPage(BETA);
    const pages = [alpha, beta];

    await t.test(
        "plain nth-child counts element siblings only",
        () => {
            for (const page of pages) {
                for (const check of PLAIN_CHECKS) {
                    assertSelectAll(page, check);
                }
            }
        },
    );

    await t.test(
        "of-list filters siblings before numbering and guards the candidate",
        () => {
            for (const page of pages) {
                for (const check of PICK_CHECKS) {
                    assertSelectAll(page, check);
                    assertCompiledPredicate(page, check);
                }
            }

            /*
             * D2 sits at the raw second element slot in alpha/p1 but is
             * excluded from `.pick`; it must never be selected.
             */
            const p1Model = alpha.model.parents.find(
                (parent) => parent.id === "p1",
            );
            const secondRaw = actualSelectAllIds(alpha, p1Model, ":nth-child(2 of .pick)");
            assert.deepEqual(secondRaw, ["d3"]);
            assert.ok(!secondRaw.includes("d2"));
        },
    );

    await t.test(
        "overlapping of-branches occupy a single slot",
        () => {
            for (const page of pages) {
                for (const check of BRANCH_CHECKS) {
                    assertSelectAll(page, check);
                    assertCompiledPredicate(page, check);
                }
            }

            /*
             * In alpha/p1, d1 and d3 match both `.hot` and `i`; the union
             * still lists them once each, in document order.
             */
            const union = actualIdsForPage(
                alpha,
                ":nth-child(2n+1 of .hot, i)",
            );
            const expectedUnion = expectedIdsForPage(alpha, BRANCH_CHECKS[1]);
            assert.deepEqual(union, expectedUnion);
            assert.equal(new Set(union).size, union.length);
        },
    );

    await t.test(
        "compound of-selector combines a direct child marker with the outer scope",
        () => {
            for (const page of pages) {
                for (const check of MARKED_CHECKS) {
                    assertSelectAll(page, check);
                    assertCompiledPredicate(page, check);
                }
            }
            for (const check of OUTER_CHECKS) {
                for (const page of pages) {
                    assertSelectAll(page, check);
                }
            }
        },
    );

    await t.test(
        "reverse ranks count from the last sibling",
        () => {
            for (const page of pages) {
                for (const check of REVERSE_CHECKS) {
                    assertSelectAll(page, check);
                    assertCompiledPredicate(page, check);
                }
            }
        },
    );

    await t.test(
        "nth-child and nth-of-type have separate counting domains",
        () => {
            for (const page of pages) {
                for (const check of TYPE_CHECKS) {
                    assertSelectAll(page, check);
                    assertCompiledPredicate(page, check);
                }
            }

            /*
             * Second element overall in alpha/p1 is the b#d2; the second
             * `<b>` is d4. Mixed tags make the domains differ.
             */
            const alphaP1 = alpha.model.parents.find(
                (parent) => parent.id === "p1",
            );
            assert.deepEqual(
                actualSelectAllIds(alpha, alphaP1, ":nth-child(2)"),
                ["d2"],
            );
            assert.deepEqual(
                actualSelectAllIds(alpha, alphaP1, "b:nth-of-type(2)"),
                ["d4"],
            );
        },
    );

    await t.test(
        "coefficients cover positive, negative and zero slopes",
        () => {
            const coefficientChecks = [
                PICK_CHECKS[2], // 0n+2
                PICK_CHECKS[3], // 2n+1
                PICK_CHECKS[4], // -n+2
                BRANCH_CHECKS[0], // 2n
                REVERSE_CHECKS[2], // -n+2 from the end
                REVERSE_CHECKS[3], // 2n+1 from the end
            ];
            for (const page of pages) {
                for (const check of coefficientChecks) {
                    assertSelectAll(page, check);
                }
            }
        },
    );

    await t.test(
        "boundaries: no matching siblings and a single matching sibling",
        () => {
            const emptyPick = {
                css: ":nth-child(1 of .pick)",
                kind: "child",
                formula: "1",
                of: "pick",
            };
            const emptyPickLast = {
                css: ":nth-last-child(1 of .pick)",
                kind: "last-child",
                formula: "1",
                of: "pick",
            };

            for (const page of pages) {
                const emptyParent = page.model.parents.find(
                    (parent) => parent.id === "p3",
                );
                assert.deepEqual(
                    expectedIdsForParent(emptyParent, emptyPick),
                    [],
                );
                assert.deepEqual(
                    expectedIdsForParent(emptyParent, emptyPickLast),
                    [],
                );
                assert.deepEqual(
                    actualSelectAllIds(page, emptyParent, emptyPick.css),
                    [],
                );
                assert.deepEqual(
                    actualSelectAllIds(page, emptyParent, emptyPickLast.css),
                    [],
                );

                /*
                 * The marked-card selector has exactly one matching sibling
                 * in p1; forward and reverse ranks must both point at it.
                 */
                const singleParent = page.model.parents.find(
                    (parent) => parent.id === "p1",
                );
                assert.deepEqual(
                    expectedIdsForParent(singleParent, MARKED_CHECKS[0]),
                    singleParent.cls.includes("feed") ? ["d4"] : ["d2"],
                );
                assert.deepEqual(
                    expectedIdsForParent(singleParent, MARKED_CHECKS[1]),
                    singleParent.cls.includes("feed") ? ["d4"] : ["d2"],
                );
            }
        },
    );

    await t.test(
        "compiled predicates agree with the model node by node",
        () => {
            for (const page of pages) {
                for (const check of ALL_CHECKS) {
                    assertCompiledPredicate(page, check);
                }
            }
        },
    );

    await t.test(
        "reused element ids stay isolated while the two trees run interleaved",
        () => {
            const alphaIds = new Set(
                [...walkElements(alpha.doc)].map(
                    (element) => element.attribs.id,
                ),
            );
            const betaIds = new Set(
                [...walkElements(beta.doc)].map(
                    (element) => element.attribs.id,
                ),
            );
            const shared = new Set([...alphaIds].filter((id) => betaIds.has(id)));
            assert.ok(shared.has("d1"));
            assert.ok(shared.has("x1"));

            for (const [index, check] of ALL_CHECKS.entries()) {
                const ordered =
                    index % 2 === 0 ? [alpha, beta] : [beta, alpha];
                for (const page of ordered) {
                    const expected = expectedIdsForPage(page, check);
                    const actual = actualIdsForPage(page, check.css);
                    assert.deepEqual(
                        actual,
                        expected,
                        failureReport(page, check.css, expected, actual),
                    );
                }
            }
        },
    );
});

/* ------------------------------------------------------------------ */
/* Rank-contexts: one compiled object, a tree that changes underneath.  */
/* CacheResults:false keeps every evaluation live; its meaning is not   */
/* Changed here.                                                       */
/* ------------------------------------------------------------------ */

test("rank-contexts", async (t) => {
    /*
     * Two structurally different trees reuse the same element ids. The same
     * compiled query objects are evaluated against both, interleaved.
     */
    const alpha = buildPage(ALPHA);
    const beta = buildPage(BETA);
    const pages = [alpha, beta];

    const alphaP1 = alpha.model.parents.find((parent) => parent.id === "p1");
    const alphaP1Element = alpha.parentsById.get("p1");
    const betaP1 = beta.model.parents.find((parent) => parent.id === "p1");

    const queries = {
        pickSecond: compile(":nth-child(2 of .pick)", {
            cacheResults: false,
        }),
        pickLast: compile(":nth-last-child(1 of .pick)", {
            cacheResults: false,
        }),
        plainSecond: compile(":nth-child(2)", { cacheResults: false }),
        markedCard: compile(":nth-child(1 of .card:has(> [mark]))", {
            cacheResults: false,
        }),
    };

    const liveChecks = [
        {
            name: "pickSecond",
            css: ":nth-child(2 of .pick)",
            kind: "child",
            formula: "2",
            of: "pick",
        },
        {
            name: "pickLast",
            css: ":nth-last-child(1 of .pick)",
            kind: "last-child",
            formula: "1",
            of: "pick",
        },
        {
            name: "plainSecond",
            css: ":nth-child(2)",
            kind: "child",
            formula: "2",
        },
        {
            name: "markedCard",
            css: ":nth-child(1 of .card:has(> [mark]))",
            kind: "child",
            formula: "1",
            of: "card-with-direct-mark",
        },
    ];

    function assertLive(page, phase) {
        for (const check of liveChecks) {
            const expected = expectedIdsForPage(page, check);
            const predicate = queries[check.name];

            const viaSelectAll = page.model.parents.flatMap((parentModel) =>
                actualSelectAllIds(page, parentModel, predicate),
            );
            assert.deepEqual(
                viaSelectAll,
                expected,
                failureReport(page, check.css, expected, viaSelectAll, phase),
            );

            const viaPredicate = actualPredicateForPage(page, predicate);
            assert.deepEqual(
                viaPredicate,
                expected,
                failureReport(page, check.css, expected, viaPredicate, phase),
            );
        }
    }

    let alternation = 0;

    function assertBothLive(phase) {
        // Interleave the trees so a shared compiled object cannot bind to one.
        const ordered = alternation++ % 2 === 0 ? pages : pages.toReversed();
        for (const page of ordered) {
            assertLive(page, phase);
        }
    }

    await t.test("both trees match their models before any mutation", () => {
        assertBothLive("initial");
    });

    await t.test(
        "an alpha class change shifts alpha ranks while reused-id beta stays put",
        () => {
            const d3Model = alphaP1.nodes.find((node) => node.id === "d3");
            d3Model.cls = d3Model.cls.filter((name) => name !== "pick");
            alpha.elementsById.get("d3").attribs.class = "hot";

            assertBothLive("after-alpha-class-change");

            // Alpha d3 dropped out of `.pick`, so alpha's second pick is d4.
            assert.deepEqual(
                expectedIdsForParent(alphaP1, liveChecks[0]),
                ["d4"],
            );
            /*
             * Beta still has three picks (d1, d2, d4); its reused ids must
             * not inherit alpha's change.
             */
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[0]),
                ["d2"],
            );
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[1]),
                ["d4"],
            );
        },
    );

    await t.test(
        "a beta class change is picked up by the same compiled object",
        () => {
            // Remove `pick` from beta's d4, leaving d1 and d2 as its picks.
            const betaD4 = betaP1.nodes.find((node) => node.id === "d4");
            betaD4.cls = betaD4.cls.filter((name) => name !== "pick");
            beta.elementsById.get("d4").attribs.class = "";

            assertBothLive("after-beta-class-change");

            /*
             * Beta's second and last pick now both resolve to d2; alpha is
             * untouched by this edit.
             */
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[0]),
                ["d2"],
            );
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[1]),
                ["d2"],
            );
            assert.deepEqual(
                expectedIdsForParent(alphaP1, liveChecks[0]),
                ["d4"],
            );
        },
    );

    await t.test(
        "moving an alpha sibling changes alpha ranks only",
        () => {
            const d5Index = alphaP1.nodes.findIndex((node) => node.id === "d5");
            const [d5Model] = alphaP1.nodes.splice(d5Index, 1);
            alphaP1.nodes.unshift(d5Model);

            const d5Element = alpha.elementsById.get("d5");
            DomUtils.prependChild(alphaP1Element, d5Element);
            assert.equal(alphaP1Element.childNodes[0], d5Element);

            assertBothLive("after-alpha-sibling-move");

            /*
             * Element order in alpha p1 starts d5, d1, d2, ... so the plain
             * second element is d1, while filtered `.pick` ranks shifted too.
             */
            assert.deepEqual(
                expectedIdsForParent(alphaP1, liveChecks[2]),
                ["d1"],
            );
            assert.deepEqual(
                expectedIdsForParent(alphaP1, liveChecks[0]),
                ["d1"],
            );
            // Beta's sibling order never changed; its second element is d2.
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[2]),
                ["d2"],
            );
        },
    );

    await t.test(
        "non-element inserts into alpha never take element positions",
        () => {
            const before = snapshot(alpha);
            const elementOrderBefore = alphaP1.nodes
                .filter((node) => node.kind === "element")
                .map((node) => node.id);

            alphaP1.nodes.unshift({
                kind: "comment",
                label: "p1/inserted-comment",
            }, { kind: "text", label: "p1/inserted-text" });
            DomUtils.prependChild(alphaP1Element, new Text("inserted-text"));
            DomUtils.prependChild(
                alphaP1Element,
                new Comment("inserted-comment"),
            );

            const elementOrderAfter = alphaP1.nodes
                .filter((node) => node.kind === "element")
                .map((node) => node.id);
            assert.deepEqual(elementOrderAfter, elementOrderBefore);
            assert.deepEqual(snapshot(alpha).slice(1), before.slice(1));

            assertBothLive("after-alpha-non-element-insert");

            /*
             * Non-elements now precede every element, yet plain element
             * numbering has to produce the same answer as before the insert.
             */
            assert.deepEqual(
                actualIdsForPage(alpha, ":nth-child(2)"),
                expectedIdsForPage(alpha, liveChecks[2]),
            );
            // Beta received no inserts; its plain second element is stable.
            assert.deepEqual(
                expectedIdsForParent(betaP1, liveChecks[2]),
                ["d2"],
            );
        },
    );
});
