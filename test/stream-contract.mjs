import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { test } from "node:test";

const repoRoot = new URL("..", import.meta.url).pathname;
// The runner points this environment variable at a temporary build of a
// deliberately degraded source tree. Without it, the repository build is used.
const requestedDist = process.env["STREAM_CONTRACT_DIST"] ?? "";
const distEntry =
    requestedDist === "" ? join(repoRoot, "dist", "index.js") : requestedDist;

// `harness/check-target.sh` runs this file directly before the runner has
// built anything, so a missing artifact is produced here with the same
// compiler options as the normal build.
if (!existsSync(distEntry)) {
    const tsc = join(repoRoot, "harness", "node_modules", ".bin", "tsc");
    const build = spawnSync(tsc, [
        "-p",
        join(repoRoot, "harness", "tsconfig.json"),
    ], {
        cwd: repoRoot,
        encoding: "utf8",
        stdio: ["ignore", "pipe", "pipe"],
    });
    if (build.status !== 0 || !existsSync(distEntry)) {
        throw new Error(
            `stream contract: build failed (${build.status}): ${build.stdout}${build.stderr}`,
        );
    }
}
const { Parser, parseDocument, createDocumentStream } = await import(
    pathToFileURL(distEntry).href
);

// ---------------------------------------------------------------------------
// Hand-written finite entity map.
// Expected values only come from these tables plus the documented rules for
// numeric references. Nothing here calls `entities` or the parser under test.
// ---------------------------------------------------------------------------
const HTML_LEGACY = new Map([
    ["amp", "&"],
    ["lt", "<"],
    ["gt", ">"],
    ["quot", '"'],
    ["nbsp", " "],
    ["not", "¬"],
]);
const HTML_NAMED = new Map([
    ...HTML_LEGACY,
    ["apos", "'"],
    ["NotEqualTilde", "≂̸"], // two code points, one reference
]);
const XML_NAMED = new Map([
    ["amp", "&"],
    ["lt", "<"],
    ["gt", ">"],
    ["quot", '"'],
    ["apos", "'"],
]);

const NUMERIC_REF = /^&#(?:([xX])([0-9A-Fa-f]+)|([0-9]+))(;?)$/;
const NAMED_REF = /^&([A-Za-z][A-Za-z0-9]*)(;?)$/;

function decodeReference(source, context) {
    if (context.decodeEntities === false || context.place === "raw") {
        return source;
    }
    const numeric = NUMERIC_REF.exec(source);
    if (numeric !== null) {
        if (context.mode === "xml" && numeric[4] === "") return source;
        const codePoint =
            numeric[1] === undefined
                ? Number.parseInt(numeric[3], 10)
                : Number.parseInt(numeric[2], 16);
        return String.fromCodePoint(codePoint);
    }
    const named = NAMED_REF.exec(source);
    if (named === null) {
        throw new Error(`stream contract model: bad reference ${source}`);
    }
    const [, name, hasSemicolon] = named;
    if (context.mode === "xml") {
        return hasSemicolon === ";"
            ? (XML_NAMED.get(name) ?? source)
            : source;
    }
    if (hasSemicolon === ";") return HTML_NAMED.get(name) ?? source;
    // Semicolonless references are legacy text; in attributes they stay
    // literal when followed by a letter, digit, or `=`.
    if (
        context.place === "attr" &&
        context.hasNext &&
        /[A-Za-z0-9=]/.test(context.nextChar)
    ) {
        return source;
    }
    return HTML_LEGACY.get(name) ?? source;
}

// ---------------------------------------------------------------------------
// Declarative page model
// ---------------------------------------------------------------------------
const literal = (text) => ({ kind: "literal", text });
const reference = (source) => ({ kind: "reference", source });
const element = (name, attributes, children) => ({
    kind: "element",
    name,
    attributes,
    children,
});
const text = (...parts) => ({ kind: "text", parts });
const rcdata = (name, attributes, parts) => ({
    kind: "rcdata",
    name,
    attributes,
    parts,
});
const rawText = (name, attributes, parts) => ({
    kind: "raw",
    name,
    attributes,
    parts,
});
const comment = (data) => ({ kind: "comment", data });
const attr = (name, quote, input) => ({ name, quote, input });

class ModelBuilder {
    constructor(spec) {
        this.spec = spec;
        this.options = spec.options ?? {};
        this.mode = this.options.xmlMode === true ? "xml" : "html";
        this.decodeEntities = this.options.decodeEntities !== false;
        this.html = "";
        this.openEvents = [];
        this.attributeEvents = [];
        this.closeEvents = [];
        this.comments = [];
        this.textSegments = [];
        this.treeChildren = [];
    }

    append(sourceText) {
        this.html += sourceText;
    }

    decodeParts(parts, place) {
        const output = [];
        for (let index = 0; index < parts.length; index++) {
            const part = parts[index];
            if (part.kind === "literal") {
                output.push(part.text);
                continue;
            }
            const next = parts[index + 1];
            const nextChar =
                next === undefined
                    ? undefined
                    : next.kind === "literal"
                      ? next.text[0]
                      : next.source[0];
            output.push(
                decodeReference(part.source, {
                    mode: this.mode,
                    decodeEntities: this.decodeEntities,
                    place,
                    hasNext: next !== undefined,
                    nextChar,
                }),
            );
        }
        return output.join("");
    }

    // Emit source for a run of parts and record both the inclusive source
    // fragments and the groups that will survive as a single text callback
    // (decoded references become separate callback spans).
    emitParts(parts, place) {
        const fragments = [];
        const groups = [];
        let groupStart = null;
        let groupEnd = null;
        const flush = () => {
            if (groupStart !== null && groupEnd !== null) {
                groups.push([groupStart, groupEnd]);
            }
            groupStart = null;
            groupEnd = null;
        };
        for (let index = 0; index < parts.length; index++) {
            const part = parts[index];
            const start = this.html.length;
            this.append(part.kind === "literal" ? part.text : part.source);
            const end = this.html.length - 1;
            if (end >= start) fragments.push([start, end]);
            let decodes = false;
            if (part.kind === "reference") {
                const next = parts[index + 1];
                const nextChar =
                    next === undefined
                        ? undefined
                        : next.kind === "literal"
                          ? next.text[0]
                          : next.source[0];
                decodes =
                    decodeReference(part.source, {
                        mode: this.mode,
                        decodeEntities: this.decodeEntities,
                        place,
                        hasNext: next !== undefined,
                        nextChar,
                    }) !== part.source;
            }
            if (decodes) {
                flush();
                if (end >= start) groups.push([start, end]);
            } else {
                if (groupStart === null) groupStart = start;
                groupEnd = end;
            }
        }
        flush();
        return { data: this.decodeParts(parts, place), fragments, groups };
    }

    addText(parts, place, parent) {
        const { data, fragments, groups } = this.emitParts(parts, place);
        const segment = {
            data,
            fragments,
            groups,
            start: fragments[0]?.[0],
            end: fragments.at(-1)?.[1],
        };
        this.textSegments.push(segment);
        parent.push({ type: "text", ...segment });
    }

    emitOpen(node, parent) {
        const start = this.html.length;
        this.append(`<${node.name}`);
        const attributes = node.attributes ?? [];
        const attrRecords = [];
        const attribs = {};
        for (const attribute of attributes) {
            this.append(" ");
            const nameStart = this.html.length;
            this.append(attribute.name);
            this.append(`=${attribute.quote}`);
            const {
                data: value,
                fragments,
            } = this.emitParts(attribute.input, "attr");
            const closingQuote = this.html.length;
            this.append(attribute.quote);
            // The library's inclusive `endIndex` for an attribute runs
            // through the delimiter that follows the closing quote (a space,
            // `>` or `/`). The expected source slice is checked below.
            const record = {
                name: attribute.name,
                value,
                quote: attribute.quote,
                start: nameStart,
                endIndex: closingQuote + 1,
                fragments,
            };
            attrRecords.push(record);
            if (!Object.hasOwn(attribs, attribute.name)) {
                attribs[attribute.name] = value;
            }
        }
        this.append(">");
        const end = this.html.length - 1;
        this.openEvents.push({
            name: node.name,
            attribs,
            implied: false,
            start,
            end,
        });
        this.attributeEvents.push(...attrRecords);
        const treeNode = {
            type: this.elementType(node.name),
            name: node.name,
            attribs,
            children: [],
            start,
            end: undefined,
        };
        parent.push(treeNode);
        return { treeNode };
    }

    elementType(name) {
        if (this.mode === "xml") return "tag";
        return name === "script" || name === "style" ? name : "tag";
    }

    emitClose(name, treeNode) {
        const start = this.html.length;
        this.append(`</${name}>`);
        const end = this.html.length - 1;
        this.closeEvents.push({ name, implied: false, start, end });
        treeNode.end = end;
    }

    buildNode(node, parent) {
        switch (node.kind) {
            case "comment": {
                const start = this.html.length;
                this.append(`<!--${node.data}-->`);
                const end = this.html.length - 1;
                this.comments.push({ data: node.data, start, end });
                parent.push({ type: "comment", data: node.data, start, end });
                return;
            }
            case "text": {
                this.addText(node.parts, "text", parent);
                return;
            }
            case "element": {
                const { treeNode } = this.emitOpen(node, parent);
                for (const child of node.children ?? []) {
                    this.buildNode(child, treeNode.children);
                }
                this.emitClose(node.name, treeNode);
                return;
            }
            case "rcdata":
            case "raw": {
                const { treeNode } = this.emitOpen(node, parent);
                this.addText(
                    node.parts,
                    node.kind === "raw" ? "raw" : "text",
                    treeNode.children,
                );
                this.emitClose(node.name, treeNode);
                return;
            }
            default: {
                throw new Error(
                    `stream contract model: unknown node kind ${node.kind}`,
                );
            }
        }
    }

    build() {
        for (const node of this.spec.root) {
            this.buildNode(node, this.treeChildren);
        }
        // Resolve each attribute's inclusive source fragment only once the
        // whole document (and therefore every offset) is known.
        for (const record of this.attributeEvents) {
            record.source = this.html.slice(record.start, record.endIndex + 1);
        }
        return {
            id: this.spec.id,
            options: this.options,
            html: this.html,
            openEvents: this.openEvents,
            attributeEvents: this.attributeEvents,
            closeEvents: this.closeEvents,
            comments: this.comments,
            textSegments: this.textSegments,
            tree: { type: "root", children: mergeAdjacentText(this.treeChildren) },
        };
    }
}

function mergeAdjacentText(children) {
    const output = [];
    for (const child of children) {
        const last = output.at(-1);
        if (child.type === "text" && last?.type === "text") {
            last.data += child.data;
            last.fragments.push(...child.fragments);
            last.groups.push(...child.groups);
            last.end = Math.max(last.end ?? -1, child.end ?? -1);
            continue;
        }
        output.push(
            child.type === "tag"
                ? {
                      ...child,
                      children: mergeAdjacentText(child.children),
                  }
                : child,
        );
    }
    return output;
}

// ---------------------------------------------------------------------------
// Pages. Every page is generated from elements, text, and attributes, and the
// builder records the source ranges and the values that must come out.
// ---------------------------------------------------------------------------
// Every page exercises at least one entity or multi-code-point reference and
// one markup transition, so the chunk schedules always straddle something.
const PAGES = [
    {
        id: "html/plain-amp-lt",
        root: [
            element("div", [attr("class", '"', [literal("note")])], [
                text(
                    literal("A "),
                    reference("&amp;"),
                    literal(" B "),
                    reference("&lt;"),
                    literal(" C"),
                ),
                element("span", [], [
                    text(literal("x"), reference("&amp;"), literal("y")),
                ]),
            ]),
        ],
    },
    {
        id: "html/numeric-decimal",
        root: [
            element("div", [], [
                text(
                    literal("d1="),
                    reference("&#65;"),
                    literal(" d2="),
                    reference("&#233;"),
                    literal(" d3="),
                    reference("&#128512;"),
                    literal(" end"),
                ),
            ]),
        ],
    },
    {
        id: "html/numeric-hex",
        root: [
            element("div", [], [
                text(
                    literal("h1="),
                    reference("&#x41;"),
                    literal(" h2="),
                    reference("&#X41;"),
                    literal(" h3="),
                    reference("&#x1F600;"),
                    literal(" h4="),
                    reference("&#x1f600;"),
                ),
            ]),
        ],
    },
    {
        id: "html/two-codepoint",
        root: [
            element("div", [], [
                text(
                    literal("pre "),
                    reference("&NotEqualTilde;"),
                    literal(" mid "),
                    reference("&not;"),
                    literal(" tail"),
                ),
                element("p", [], [text(reference("&NotEqualTilde;"))]),
            ]),
        ],
    },
    {
        id: "html/semicolonless-text",
        root: [
            element("div", [], [
                text(
                    reference("&amp"),
                    literal("x "),
                    reference("&amp"),
                    literal("1 "),
                    reference("&amp"),
                    literal("= "),
                    reference("&amp"),
                    literal(" "),
                    reference("&not"),
                    literal("it=7 "),
                    reference("&#65"),
                    literal("x"),
                ),
            ]),
        ],
    },
    {
        id: "html/attr-legacy-blocked",
        root: [
            element(
                "div",
                [
                    attr("data-one", '"', [reference("&not"), literal("it=7")]),
                    attr("data-two", "'", [reference("&amp"), literal("x")]),
                    attr("data-three", '"', [reference("&amp"), literal("1")]),
                    attr("data-four", '"', [reference("&amp"), literal("=")]),
                    attr("data-ok", '"', [reference("&amp;")]),
                    attr(
                        "data-space",
                        "'",
                        [reference("&amp"), literal(" plain")],
                    ),
                ],
                [text(literal("x"))],
            ),
        ],
    },
    {
        id: "html/attr-entities",
        root: [
            element(
                "div",
                [
                    attr("title", '"', [reference("&apos;"), literal("x")]),
                    attr("note", "'", [reference("&quot;"), reference("&gt;")]),
                    attr(
                        "big",
                        '"',
                        [reference("&#x1F600;"), reference("&#128512;")],
                    ),
                    attr("raw", '"', [reference("&lt;"), reference("&amp;")]),
                    attr("tail", '"', [reference("&amp")]),
                    attr("num", '"', [reference("&#65"), literal("x")]),
                ],
                [text(literal("body"))],
            ),
        ],
    },
    {
        id: "html/rcdata",
        root: [
            element("div", [], [
                rcdata("title", [attr("data-k", '"', [literal("v")])], [
                    literal("A "),
                    reference("&lt;"),
                    literal(" B "),
                    reference("&amp"),
                    literal(" C "),
                    reference("&#65;"),
                    literal(" "),
                    reference("&NotEqualTilde;"),
                    literal(" literal <"),
                ]),
                rcdata("textarea", [], [
                    literal("a<b "),
                    reference("&amp;"),
                    literal(" c "),
                    reference("&lt;"),
                    literal(" d "),
                    reference("&#65;"),
                    literal(" e "),
                    reference("&not;"),
                    literal(" f"),
                    reference("&amp"),
                    literal(" g"),
                ]),
            ]),
        ],
    },
    {
        id: "html/raw",
        root: [
            element("div", [], [
                text(literal("before "), reference("&amp;"), literal(" ")),
                rawText("script", [], [
                    literal('if (a < b) s = "&amp; &lt; &#65;";'),
                ]),
                rawText("style", [], [
                    literal('.x { content: "&amp; <>&#65;" }'),
                ]),
                text(literal(" after "), reference("&lt;")),
            ]),
        ],
    },
    {
        id: "html/comments",
        root: [
            element("div", [], [
                text(literal("start"), reference("&nbsp;")),
                comment("a > b &amp; c"),
                text(literal("mid "), reference("&lt;"), literal(" ")),
                comment("second &not; comment"),
                text(literal("end "), reference("&NotEqualTilde;")),
            ]),
        ],
    },
    {
        id: "html/nested",
        root: [
            element("div", [attr("id", '"', [literal("outer")])], [
                element("section", [], [
                    text(literal("one "), reference("&amp;"), literal(" ")),
                    element("p", [attr("class", "'", [literal("inner")])], [
                        text(literal("two "), reference("&lt;")),
                    ]),
                    text(literal(" three")),
                ]),
                element(
                    "span",
                    [attr("data-v", '"', [reference("&amp;")])],
                    [text(reference("&#x1F600;"), literal(" tail"))],
                ),
            ]),
        ],
    },
    {
        id: "html/decode-off",
        options: { decodeEntities: false },
        root: [
            element("div", [attr("title", '"', [reference("&amp;"), reference("&lt;")])], [
                text(
                    literal("A"),
                    reference("&amp;"),
                    literal("B "),
                    reference("&#65;"),
                    literal(" "),
                    reference("&NotEqualTilde;"),
                ),
                rcdata("textarea", [], [
                    literal("T"),
                    reference("&amp;"),
                    literal("U"),
                ]),
                rawText("script", [], [
                    literal("S"),
                    reference("&amp;"),
                    literal("T"),
                ]),
            ]),
        ],
    },
    {
        id: "xml/entities",
        options: { xmlMode: true },
        root: [
            element(
                "Root",
                [
                    attr("one", '"', [reference("&amp;"), literal("x")]),
                    attr("two", "'", [reference("&lt;")]),
                ],
                [
                    element("Item", [], [
                        text(
                            reference("&amp;"),
                            literal(" "),
                            reference("&#65;"),
                            literal(" "),
                            reference("&#x1F600;"),
                            literal(" "),
                            reference("&NotEqualTilde;"),
                            literal(" "),
                            reference("&amp"),
                            literal(" "),
                            reference("&#65"),
                        ),
                    ]),
                    element("Leaf", [], [
                        text(literal("leaf "), reference("&gt;")),
                    ]),
                ],
            ),
        ],
    },
    {
        id: "xml/decode-off",
        options: { xmlMode: true, decodeEntities: false },
        root: [
            element("Root", [attr("a", '"', [reference("&amp;")])], [
                text(
                    literal("X"),
                    reference("&amp;"),
                    literal("Y"),
                    reference("&#65;"),
                    literal("Z"),
                ),
                element("Child", [], [
                    text(reference("&lt;"), literal(" child")),
                ]),
            ]),
        ],
    },
    {
        id: "html/adjacent-entities",
        root: [
            element("div", [], [
                text(
                    reference("&amp;"),
                    reference("&lt;"),
                    literal("x"),
                    reference("&NotEqualTilde;"),
                    reference("&#65;"),
                ),
                element("p", [], [
                    text(literal("y"), reference("&amp;"), reference("&#x1F600;")),
                ]),
            ]),
        ],
    },
];

const MODELS = PAGES.map((page) => new ModelBuilder(page).build());

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------
function createObservation() {
    return {
        attributes: [],
        openTags: [],
        closeTags: [],
        comments: [],
        textRuns: [],
        endCount: 0,
        errors: [],
        textOpen: false,
        activeRun: null,
        textCallbackCount: 0,
    };
}

function closeTextRun(observation) {
    observation.textOpen = false;
    observation.activeRun = null;
}

function callbacksFor(parser, observation) {
    return {
        onattribute(name, value, quote) {
            closeTextRun(observation);
            observation.attributes.push({
                name,
                value,
                quote,
                start: parser.startIndex,
                endIndex: parser.endIndex,
            });
        },
        onopentag(name, attribs, isImplied) {
            closeTextRun(observation);
            observation.openTags.push({
                name,
                attribs: { ...attribs },
                implied: isImplied,
                start: parser.startIndex,
                end: parser.endIndex,
            });
        },
        onclosetag(name, isImplied) {
            closeTextRun(observation);
            observation.closeTags.push({
                name,
                implied: isImplied,
                start: parser.startIndex,
                end: parser.endIndex,
            });
        },
        oncomment(data) {
            closeTextRun(observation);
            observation.comments.push({
                data,
                start: parser.startIndex,
                end: parser.endIndex,
            });
        },
        ontext(data) {
            observation.textCallbackCount++;
            if (!observation.textOpen) {
                observation.activeRun = { data: "", intervals: [] };
                observation.textRuns.push(observation.activeRun);
                observation.textOpen = true;
            }
            observation.activeRun.data += data;
            observation.activeRun.intervals.push({
                start: parser.startIndex,
                end: parser.endIndex,
            });
        },
        onend() {
            closeTextRun(observation);
            observation.endCount++;
        },
        onerror(error) {
            observation.errors.push(String(error));
        },
    };
}

function createParser(options) {
    const observation = createObservation();
    let parser;
    // Callbacks read `startIndex`/`endIndex` from the live parser when the
    // event fires. The getters are only consulted after assignment.
    const liveIndices = {
        get startIndex() {
            return parser.startIndex;
        },
        get endIndex() {
            return parser.endIndex;
        },
    };
    const handler = callbacksFor(liveIndices, observation);
    parser = new Parser(handler, options);
    return { parser, observation };
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------
function unionIntervals(intervals) {
    const sorted = intervals
        .filter(({ start, end }) => start <= end)
        .map(({ start, end }) => [start, end])
        .sort((first, second) => first[0] - second[0] || first[1] - second[1]);
    const result = [];
    for (const [start, end] of sorted) {
        const last = result.at(-1);
        if (last !== undefined && start <= last[1] + 1) {
            last[1] = Math.max(last[1], end);
        } else {
            result.push([start, end]);
        }
    }
    return result;
}

function rangesFrom(intervals) {
    return unionIntervals(
        intervals.map(({ start, end }) => ({ start, end })),
    );
}

function recordFailure(failures, aspect, message) {
    failures.push({ aspect, message });
}

function compareFields(failures, aspect, label, expectedList, actualList, keys) {
    if (expectedList.length !== actualList.length) {
        recordFailure(
            failures,
            aspect,
            `${label}: ${aspect} count ${actualList.length} != ${expectedList.length}`,
        );
        return;
    }
    for (let index = 0; index < expectedList.length; index++) {
        for (const key of keys) {
            const expected = expectedList[index][key];
            const actual = actualList[index][key];
            if (JSON.stringify(actual) !== JSON.stringify(expected)) {
                recordFailure(
                    failures,
                    aspect,
                    `${label}: ${aspect}[${index}].${key} ${JSON.stringify(actual)} != ${JSON.stringify(expected)}`,
                );
            }
        }
    }
}

function compareObservation(model, observation, label) {
    const failures = [];
    const html = model.html;
    if (observation.errors.length > 0) {
        recordFailure(failures, "errors", `${label}: parser errors ${observation.errors.join("; ")}`);
    }
    if (observation.endCount !== 1) {
        recordFailure(
            failures,
            "end",
            `${label}: onend fired ${observation.endCount} times`,
        );
    }
    compareFields(
        failures,
        "attribute-events",
        label,
        model.attributeEvents,
        observation.attributes,
        ["name", "value", "quote", "start", "endIndex"],
    );
    // The reported position must cover the complete attribute source
    // fragment. This never derives a position from the decoded value length.
    if (observation.attributes.length === model.attributeEvents.length) {
        for (let index = 0; index < model.attributeEvents.length; index++) {
            const expected = model.attributeEvents[index];
            const actual = observation.attributes[index];
            const actualSource = html.slice(actual.start, actual.endIndex + 1);
            if (actualSource !== expected.source) {
                recordFailure(
                    failures,
                    "attribute-source",
                    `${label}: attribute[${index}] source ${JSON.stringify(actualSource)} != ${JSON.stringify(expected.source)}`,
                );
            }
        }
    }
    compareFields(
        failures,
        "open-tags",
        label,
        model.openEvents,
        observation.openTags,
        ["name", "attribs", "implied", "start", "end"],
    );
    compareFields(
        failures,
        "close-tags",
        label,
        model.closeEvents,
        observation.closeTags,
        ["name", "implied", "start", "end"],
    );
    compareFields(
        failures,
        "comments",
        label,
        model.comments,
        observation.comments,
        ["data", "start", "end"],
    );
    if (observation.textRuns.length !== model.textSegments.length) {
        recordFailure(
            failures,
            "text-segments",
            `${label}: text runs ${observation.textRuns.length} != ${model.textSegments.length}`,
        );
        return failures;
    }
    for (let index = 0; index < model.textSegments.length; index++) {
        const expected = model.textSegments[index];
        const run = observation.textRuns[index];
        if (run.data !== expected.data) {
            recordFailure(
                failures,
                "text-values",
                `${label}: text[${index}] ${JSON.stringify(run.data)} != ${JSON.stringify(expected.data)}`,
            );
        }
        // Inclusive ranges are checked as the union of non-empty intervals of
        // the same text run.
        const actualCoverage = rangesFrom(run.intervals);
        const expectedCoverage = rangesFrom(
            expected.fragments.map(([start, end]) => ({ start, end })),
        );
        if (JSON.stringify(actualCoverage) !== JSON.stringify(expectedCoverage)) {
            recordFailure(
                failures,
                "text-ranges",
                `${label}: text[${index}] coverage ${JSON.stringify(actualCoverage)} != ${JSON.stringify(expectedCoverage)}`,
            );
        }
        // An individual callback must not straddle a decoded reference and
        // its surrounding literal text. References that decode to two code
        // points may emit an additional zero-width span, so only non-empty
        // spans are inspected.
        for (const interval of run.intervals) {
            if (interval.start > interval.end) continue;
            const contained = expected.groups.some(
                ([start, end]) =>
                    interval.start >= start && interval.end <= end,
            );
            if (!contained) {
                recordFailure(
                    failures,
                    "text-ranges",
                    `${label}: text[${index}] interval ${interval.start}..${interval.end} crosses a decoded reference`,
                );
            }
        }
    }
    return failures;
}

// ---------------------------------------------------------------------------
// Chunk schedules
// ---------------------------------------------------------------------------
function splitAt(html, cut) {
    return [html.slice(0, cut), html.slice(cut)];
}

function unitChunks(html) {
    const chunks = [];
    for (let index = 0; index < html.length; index++) {
        chunks.push(html.slice(index, index + 1));
    }
    return chunks;
}

function seededRandom(seed) {
    let state = seed >>> 0;
    return () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let value = state;
        value = Math.imul(value ^ (value >>> 15), value | 1);
        value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
        return ((value ^ (value >>> 14)) >>> 0) / 0x1_0000_0000;
    };
}

function seededCuts(html, seed, count) {
    const random = seededRandom(seed);
    const cuts = new Set();
    while (cuts.size < Math.min(count, html.length - 1)) {
        cuts.add(1 + Math.floor(random() * (html.length - 1)));
    }
    const points = [...cuts].sort((first, second) => first - second);
    const chunks = [];
    let previous = 0;
    for (const point of points) {
        chunks.push(html.slice(previous, point));
        previous = point;
    }
    chunks.push(html.slice(previous));
    return chunks;
}

function schedulesFor(html) {
    const schedules = [
        { name: "single", chunks: [html] },
        { name: "units", chunks: unitChunks(html) },
    ];
    for (let cut = 1; cut < html.length; cut++) {
        schedules.push({
            name: `cut-${cut}`,
            chunks: splitAt(html, cut),
        });
    }
    for (const seed of [1, 2, 3, 4, 5, 6]) {
        schedules.push({
            name: `seeded-${seed}`,
            chunks: seededCuts(html, seed, 4),
        });
    }
    return schedules;
}

function parseInChunks(html, chunks, options) {
    const { parser, observation } = createParser(options);
    for (const chunk of chunks) {
        parser.write(chunk);
    }
    parser.end();
    return observation;
}

// ---------------------------------------------------------------------------
// DOM comparison
// ---------------------------------------------------------------------------
function normalizeDom(node) {
    if (node.type === "root") {
        return {
            type: "root",
            children: node.children.map((child) => normalizeDom(child)),
        };
    }
    if (node.type === "text") {
        return {
            type: "text",
            data: node.data,
            startIndex: node.startIndex,
            endIndex: node.endIndex,
        };
    }
    if (node.type === "comment") {
        return {
            type: "comment",
            data: node.data,
            startIndex: node.startIndex,
            endIndex: node.endIndex,
        };
    }
    return {
        type: node.type,
        name: node.name,
        attribs: { ...node.attribs },
        children: node.children.map((child) => normalizeDom(child)),
        startIndex: node.startIndex,
        endIndex: node.endIndex,
    };
}

function normalizeModelTree(node) {
    if (node.type === "root") {
        return {
            type: "root",
            children: node.children.map((child) => normalizeModelTree(child)),
        };
    }
    if (node.type === "text") {
        return {
            type: "text",
            data: node.data,
            startIndex: node.start,
            endIndex: node.end,
        };
    }
    if (node.type === "comment") {
        return {
            type: "comment",
            data: node.data,
            startIndex: node.start,
            endIndex: node.end,
        };
    }
    return {
        type: node.type,
        name: node.name,
        attribs: { ...node.attribs },
        children: node.children.map((child) => normalizeModelTree(child)),
        startIndex: node.start,
        endIndex: node.end,
    };
}

// ---------------------------------------------------------------------------
// Pause scheduling. Pause inside one selected callback, enqueue the remaining
// chunks plus `end`, and resume only from outside the parser call stack.
// ---------------------------------------------------------------------------
function applyPauseSchedule(html, options, target) {
    return new Promise((resolve) => {
        const observation = createObservation();
        const chunkSize = 7;
        const chunks = [];
        for (let index = 0; index < html.length; index += chunkSize) {
            chunks.push(html.slice(index, index + chunkSize));
        }
        let parser;
        let paused = false;
        let nextChunk = 0;
        let queuedFrom = 0;
        let textCallbacksSeen = 0;
        const liveIndices = {
            get startIndex() {
                return parser.startIndex;
            },
            get endIndex() {
                return parser.endIndex;
            },
        };
        const callbacks = callbacksFor(liveIndices, observation);
        const pauseIfSelected = (kind, name) => {
            if (paused) return;
            const matches =
                target.kind === kind &&
                (target.name === undefined || target.name === name) &&
                (target.occurrence === undefined ||
                    (kind === "text"
                        ? textCallbacksSeen
                        : observation.openTags.length) === target.occurrence);
            if (!matches) return;
            paused = true;
            parser.pause();
            queuedFrom = nextChunk;
        };

        callbacks.onopentag = (name, attribs, isImplied) => {
            observation.openTags.push({
                name,
                attribs: { ...attribs },
                implied: isImplied,
                start: parser.startIndex,
                end: parser.endIndex,
            });
            pauseIfSelected("open", name);
        };
        callbacks.ontext = (data) => {
            textCallbacksSeen++;
            if (!observation.textOpen) {
                observation.activeRun = { data: "", intervals: [] };
                observation.textRuns.push(observation.activeRun);
                observation.textOpen = true;
            }
            observation.activeRun.data += data;
            observation.activeRun.intervals.push({
                start: parser.startIndex,
                end: parser.endIndex,
            });
            pauseIfSelected("text", undefined);
        };

        parser = new Parser(callbacks, options);

        // Write chunks until the selected callback pauses or input runs out.
        while (!paused && nextChunk < chunks.length) {
            parser.write(chunks[nextChunk++]);
        }
        if (!paused) {
            parser.end();
            resolve({ observation, paused: false });
            return;
        }
        // Queue the remainder and `end` (all while paused), then resume from
        // a later macrotask.
        while (queuedFrom < chunks.length) {
            parser.write(chunks[queuedFrom++]);
        }
        parser.end();
        // Resume from a macrotask outside the parser stack. The observation
        // is complete synchronously once `resume` returns; a fixed number of
        // extra turns drains any queued microtasks without sleeping.
        setTimeout(() => {
            parser.resume();
            setTimeout(() => resolve({ observation, paused: true }), 0);
        }, 0);
    });
}

function comparePausedObservation(baseline, paused, label) {
    const failures = [];
    const observed = paused.observation;
    // After resume the observation must be structurally identical to an
    // unsplit run: no duplicated tokens, no missing end, no reordered text.
    if (observed.errors.length > 0) {
        recordFailure(
            failures,
            "pause-errors",
            `${label}: parser errors ${observed.errors.join("; ")}`,
        );
    }
    if (observed.endCount !== 1) {
        recordFailure(
            failures,
            "pause-end-count",
            `${label}: onend fired ${observed.endCount} times after resume`,
        );
    }
    if (paused.observation.textRuns.map((run) => run.data).join("") !==
        baseline.textRuns.map((run) => run.data).join("")) {
        recordFailure(
            failures,
            "pause-text",
            `${label}: text after resume differs from the unsplit run`,
        );
    }
    // Attribute values, tag lists and comment contents must be identical.
    if (
        JSON.stringify(
            observed.attributes.map(({ name, value, quote }) => ({
                name,
                value,
                quote,
            })),
        ) !==
        JSON.stringify(
            baseline.attributes.map(({ name, value, quote }) => ({
                name,
                value,
                quote,
            })),
        )
    ) {
        recordFailure(
            failures,
            "pause-attributes",
            `${label}: attributes after resume differ`,
        );
    }
    if (JSON.stringify(observed.openTags.map(({ name, implied }) => ({ name, implied }))) !==
        JSON.stringify(baseline.openTags.map(({ name, implied }) => ({ name, implied })))) {
        recordFailure(failures, "pause-open-tags", `${label}: open tags after resume differ`);
    }
    if (JSON.stringify(observed.closeTags.map(({ name, implied }) => ({ name, implied }))) !==
        JSON.stringify(baseline.closeTags.map(({ name, implied }) => ({ name, implied })))) {
        recordFailure(failures, "pause-close-tags", `${label}: close tags after resume differ`);
    }
    if (JSON.stringify(observed.comments.map(({ data }) => data)) !==
        JSON.stringify(baseline.comments.map(({ data }) => data))) {
        recordFailure(failures, "pause-comments", `${label}: comments after resume differ`);
    }
    return failures;
}

// ---------------------------------------------------------------------------
// The test
// ---------------------------------------------------------------------------
function verifyModel(model, failures) {
    for (const schedule of schedulesFor(model.html)) {
        const observation = parseInChunks(
            model.html,
            schedule.chunks,
            model.options,
        );
        const issues = compareObservation(
            model,
            observation,
            `${model.id} [${schedule.name}]`,
        );
        for (const issue of issues) {
            failures.push({
                page: model.id,
                schedule: schedule.name,
                aspect: issue.aspect,
                message: issue.message,
                detail: issue.detail,
            });
        }
    }
}

async function verifyTrees(model, failures) {
    const parseOptions = {
        ...model.options,
        withStartIndices: true,
        withEndIndices: true,
    };
    const expected = normalizeModelTree(model.tree);
    let document;
    try {
        document = parseDocument(model.html, parseOptions);
    } catch (error) {
        failures.push({
            page: model.id,
            schedule: "parseDocument",
            aspect: "tree",
            message: `parseDocument threw: ${error.message}`,
        });
        return;
    }
    try {
        assert.deepEqual(normalizeDom(document), expected);
    } catch (error) {
        failures.push({
            page: model.id,
            schedule: "parseDocument",
            aspect: "tree-fields",
            message: `parseDocument tree mismatch: ${String(error.message).split("\n").slice(0, 6).join(" ")}`,
        });
    }

    // Streaming DOM construction, fed one UTF-16 unit at a time.
    const result = await new Promise((resolve) => {
        let calls = 0;
        let outcome = null;
        const parser = createDocumentStream(
            (error, streamDocument) => {
                calls++;
                outcome = { error, document: streamDocument };
            },
            parseOptions,
        );
        for (const chunk of unitChunks(model.html)) parser.write(chunk);
        parser.end();
        queueMicrotask(() => resolve({ calls, outcome }));
    });
    if (result.calls !== 1) {
        failures.push({
            page: model.id,
            schedule: "createDocumentStream",
            aspect: "count",
            message: `createDocumentStream callback fired ${result.calls} times`,
        });
        return;
    }
    if (result.outcome?.error) {
        failures.push({
            page: model.id,
            schedule: "createDocumentStream",
            aspect: "error",
            message: `createDocumentStream reported ${result.outcome.error.message}`,
        });
        return;
    }
    try {
        assert.deepEqual(normalizeDom(result.outcome.document), expected);
    } catch (error) {
        failures.push({
            page: model.id,
            schedule: "createDocumentStream",
            aspect: "tree-fields",
            message: `createDocumentStream tree mismatch: ${String(error.message).split("\n").slice(0, 6).join(" ")}`,
        });
    }
}

async function verifyPause(model, failures) {
    const baseline = parseInChunks(model.html, [model.html], model.options);
    // Pause inside the first open-tag callback and inside the first, second,
    // and last text callbacks. Every page has both an opening tag and text.
    // Text callback counts depend on the chunking, so the counts come from
    // the unsplit baseline observation.
    const openName = model.openEvents[0].name;
    const targets = [
        { kind: "open", name: openName, occurrence: 1 },
        { kind: "text", occurrence: 1 },
        { kind: "text", occurrence: Math.min(2, baseline.textCallbackCount) },
        { kind: "text", occurrence: baseline.textCallbackCount },
    ];
    for (const target of targets) {
        const label = `${model.id} pause-on-${target.kind}-${target.occurrence}${target.name ? `-${target.name}` : ""}`;
        const paused = await applyPauseSchedule(model.html, model.options, target);
        if (!paused.paused) {
            failures.push({
                page: model.id,
                schedule: "pause",
                aspect: "pause-not-triggered",
                message: `${label}: selected callback never fired`,
            });
            continue;
        }
        const issues = comparePausedObservation(baseline, paused, label);
        for (const issue of issues) failures.push({ page: model.id, schedule: "pause", ...issue });
    }
}

test("stream acquisition contract", async (t) => {
    const failures = [];

    for (const model of MODELS) {
        await t.test(`${model.id}: chunked callback contract`, () => {
            const perModel = [];
            verifyModel(model, perModel);
            if (perModel.length > 0) {
                failures.push(...perModel);
                const first = perModel[0];
                assert.fail(
                    `${first.page} [${first.schedule}] ${first.aspect}: ${first.message}`,
                );
            }
        });
    }

    for (const model of MODELS) {
        await t.test(`${model.id}: DOM trees and ranges`, async () => {
            const perModel = [];
            await verifyTrees(model, perModel);
            if (perModel.length > 0) {
                failures.push(...perModel);
                const first = perModel[0];
                assert.fail(
                    `${first.page} [${first.schedule}] ${first.aspect}: ${first.message}`,
                );
            }
        });
    }

    for (const model of MODELS) {
        await t.test(`${model.id}: pause, queued end, and resume`, async () => {
            const perModel = [];
            await verifyPause(model, perModel);
            if (perModel.length > 0) {
                failures.push(...perModel);
                const first = perModel[0];
                assert.fail(
                    `${first.page} pause ${first.aspect}: ${first.message}`,
                );
            }
        });
    }

    await t.test("reset starts a new document at offset zero", () => {
        const { parser, observation } = createParser({});

        parser.end('<div id="first">A&amp;B</div>');
        assert.equal(observation.endCount, 1);
        assert.equal(observation.textRuns.at(-1).data, "A&B");

        // Remember how many events belong to the first document, then reset.
        const firstAttributeCount = observation.attributes.length;
        const firstTextRunCount = observation.textRuns.length;
        const firstOpenCount = observation.openTags.length;
        const firstCloseCount = observation.closeTags.length;
        parser.reset();

        // A second model whose IDs and text differ from the first document.
        // The raw callbacks stay attached, so the same observation keeps
        // collecting; every event after the reset must use indices that
        // start from zero again.
        const secondHtml = '<section id="second">C&#x1F600;D</section>';
        parser.end(secondHtml);
        assert.equal(observation.endCount, 2);

        const secondAttributes = observation.attributes.slice(
            firstAttributeCount,
        );
        assert.equal(secondAttributes.length, 1);
        assert.equal(secondAttributes[0].name, "id");
        assert.equal(secondAttributes[0].value, "second");
        assert.equal(secondAttributes[0].start, secondHtml.indexOf("id"));
        assert.equal(
            secondAttributes[0].endIndex,
            secondHtml.indexOf(">"),
        );

        const secondOpen = observation.openTags.slice(firstOpenCount);
        assert.equal(secondOpen.length, 1);
        assert.equal(secondOpen[0].name, "section");
        assert.equal(secondOpen[0].attribs.id, "second");
        assert.equal(secondOpen[0].start, 0);
        assert.equal(secondOpen[0].end, secondHtml.indexOf(">"));

        const secondTextRuns = observation.textRuns.slice(firstTextRunCount);
        assert.equal(secondTextRuns.length, 1);
        assert.equal(secondTextRuns[0].data, "C😀D");
        // The first text callback starts right after the open tag, and the
        // numeric reference keeps its own source interval.
        assert.equal(secondTextRuns[0].intervals[0].start, secondHtml.indexOf(">") + 1);
        assert.equal(
            secondTextRuns[0].intervals.at(-1).end,
            secondHtml.indexOf("</") - 1,
        );

        const secondClose = observation.closeTags.slice(firstCloseCount);
        assert.equal(secondClose.length, 1);
        assert.equal(secondClose[0].name, "section");
        assert.equal(secondClose[0].start, secondHtml.indexOf("</"));
        assert.equal(secondClose[0].end, secondHtml.length - 1);

        // The first document's events were left untouched by the reset.
        const firstOpen = observation.openTags.find(
            (entry) => entry.name === "div" && entry.start === 0,
        );
        assert.ok(firstOpen !== undefined);
        assert.equal(observation.textRuns[0].data, "A&B");
    });

    await t.test("queued end survives a pause and fires exactly once", () => {
        const events = [];
        let endCount = 0;
        let parser;
        parser = new Parser({
            onopentag(name) {
                events.push(`open:${name}`);
                parser.pause();
                // Enqueue the remaining input and `end` while paused, from
                // inside the callback; resume happens outside the call stack.
                parser.write("A&amp;B</div>");
                parser.end();
            },
            ontext(data) {
                events.push(`text:${data}`);
            },
            onclosetag(name) {
                events.push(`close:${name}`);
            },
            onend() {
                endCount++;
                events.push("end");
            },
            onerror(error) {
                events.push(`error:${error.message}`);
            },
        });
        parser.write("<div>");
        assert.equal(endCount, 0, "onend must wait for resume");
        parser.resume();
        assert.equal(endCount, 1, "queued end must fire exactly once");
        assert.deepEqual(
            events.filter((event) => event.startsWith("text:")),
            ["text:A", "text:&", "text:B"],
        );
        assert.equal(events.filter((event) => event === "end").length, 1);
        assert.equal(events.at(-1), "end");
    });

    await t.test("resume continues writing without a queued end", () => {
        // Pause mid-text, resume, and only then write the rest plus `end`.
        // This is the path where `end` is not pending during the pause; the
        // end notification must still fire exactly once when it arrives.
        const events = [];
        let endCount = 0;
        let parser;
        let resumed = false;
        parser = new Parser({
            ontext(data) {
                events.push(data);
                if (!resumed && data.includes("&")) {
                    resumed = true;
                    parser.pause();
                }
            },
            onclosetag(name) {
                events.push(`/${name}`);
            },
            onend() {
                endCount++;
            },
        });
        parser.write("<p>A&amp;");
        assert.equal(endCount, 0);
        parser.resume();
        assert.equal(endCount, 0, "no end was queued yet");
        parser.write("B</p>");
        parser.end();
        assert.equal(endCount, 1);
        assert.equal(events.filter((event) => !event.startsWith("/")).join(""), "A&B");
        assert.deepEqual(events.filter((event) => event.startsWith("/")), ["/p"]);
    });

    await t.test("pause before the first write still ends once on resume", () => {
        const events = [];
        let endCount = 0;
        let parser;
        parser = new Parser({
            ontext(data) {
                events.push(data);
            },
            onend() {
                endCount++;
            },
        });
        parser.pause();
        parser.write("<p>a");
        parser.write("&amp;b</p>");
        parser.end();
        assert.equal(endCount, 0);
        parser.resume();
        assert.equal(endCount, 1);
        assert.equal(events.join(""), "a&b");
    });

    // The runner consumes the TAP/`test:summary` counts; this marker is for
    // the direct check-entry invocation in harness/check-target.sh.
    if (failures.length === 0) {
        process.stdout.write("STREAM_MODEL_OK\n");
    }
});
