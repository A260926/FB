/*
 * Script-data contract tests for the native tokenizer.
 *
 * These tests exercise the HTML "script data" state family through the built
 * `Parser` (and `parseDocument` for DOM identity). All expectations are stated
 * from the input strings themselves — no expectation is derived from a one-shot
 * parse of the same input.
 *
 * The rules under test (HTML tokenizer, "script data" section):
 * - `<!--` enters escaped script data.
 * - In escaped script data, `<` followed by an ASCII letter collects a
 *   temporary word. Only the word `script` followed by ASCII whitespace, `/`
 *   or `>` enters double-escaped script data.
 * - In double-escaped script data, `</script` plus a valid delimiter only
 *   returns to escaped script data; it does not emit a tag.
 * - The comparison is ASCII case-insensitive.
 * - The dash/dash-dash/less-than/`-->` transitions are complete.
 * - Unfinished script text is delivered once at EOF.
 */

import assert from "node:assert/strict";
import { describe, it } from "node:test";

import { parseDocument } from "../dist/index.js";
import { Parser } from "../dist/Parser.js";

/**
 * Observe parser events for one input string.
 * @param html Input document.
 * @param [options] Parser options.
 * @returns Events.
 */
function observe(html, options = {}) {
    let text = "";
    const textParts = [];
    const textRanges = [];
    const opens = [];
    const closes = [];
    let comments = 0;
    let ends = 0;
    let parser;

    parser = new Parser(
        {
            onopentag(name, attribs) {
                opens.push([name, attribs.id ?? ""]);
            },
            ontext(data) {
                textParts.push(data);
                // Inclusive start, exclusive end => endIndex + 1.
                textRanges.push([parser.startIndex, parser.endIndex + 1]);
                text += data;
            },
            onclosetag(name, isImplied) {
                closes.push([name, isImplied, parser.endIndex]);
            },
            oncomment() {
                comments++;
            },
            onend() {
                ends++;
            },
        },
        options,
    );

    parser.end(html);
    return { text, textParts, textRanges, opens, closes, ends, comments };
}

/**
 * Observe a parser fed one UTF-16 code unit at a time.
 * @param html Input document.
 * @param [options] Parser options.
 * @returns Events.
 */
function observeUnitwise(html, options = {}) {
    let text = "";
    const textParts = [];
    const textRanges = [];
    const opens = [];
    const closes = [];
    let comments = 0;
    let ends = 0;
    let parser;

    parser = new Parser(
        {
            onopentag(name, attribs) {
                opens.push([name, attribs.id ?? ""]);
            },
            ontext(data) {
                textParts.push(data);
                textRanges.push([parser.startIndex, parser.endIndex + 1]);
                text += data;
            },
            onclosetag(name, isImplied) {
                closes.push([name, isImplied, parser.endIndex]);
            },
            oncomment() {
                comments++;
            },
            onend() {
                ends++;
            },
        },
        options,
    );

    for (const element of html) {
        parser.write(element);
    }
    parser.end();

    return { text, textParts, textRanges, opens, closes, ends, comments };
}

/**
 * Observe a parser fed two chunks split at one code-unit offset.
 * @param html Input document.
 * @param cut Split offset (in UTF-16 code units).
 * @param [options] Parser options.
 * @returns Events.
 */
function observeSplit(html, cut, options = {}) {
    let text = "";
    const textParts = [];
    const textRanges = [];
    const opens = [];
    const closes = [];
    let comments = 0;
    let ends = 0;
    let parser;

    parser = new Parser(
        {
            onopentag(name, attribs) {
                opens.push([name, attribs.id ?? ""]);
            },
            ontext(data) {
                textParts.push(data);
                textRanges.push([parser.startIndex, parser.endIndex + 1]);
                text += data;
            },
            onclosetag(name, isImplied) {
                closes.push([name, isImplied, parser.endIndex]);
            },
            oncomment() {
                comments++;
            },
            onend() {
                ends++;
            },
        },
        options,
    );

    parser.write(html.slice(0, cut));
    parser.write(html.slice(cut));
    parser.end();

    return { text, textParts, textRanges, opens, closes, ends, comments };
}

/**
 * Return the element nodes of a parsed document, in document order.
 * @param document Document to walk.
 * @returns Elements.
 */
function elementNodes(document) {
    const result = [];
    const walk = (nodes) => {
        for (const node of nodes) {
            if (node.name === undefined) {
            	continue;
            }

            result.push(node);
            if (node.children) walk(node.children);
        }
    };
    walk(document.children);
    return result;
}

/**
 * `[name, id]` pairs for every element in a document.
 * @param document Document to walk.
 * @returns Elements.
 */
function elements(document) {
    return elementNodes(document).map((node) => [
        node.name,
        node.attribs?.id ?? "",
    ]);
}

const ghostDocument =
    '<script><!--<script></script><p id="ghost">x</p>--></script><p id="real">ok</p>';

describe("script data", () => {
    it("keeps ghost markup inside double-escaped script text", () => {
        const result = observe(ghostDocument);

        /*
         * The first `</script>` (index 20) only leaves double-escaped mode.
         * The ghost paragraph therefore stays text; the final `</script>`
         * (index 51, name ends at 58) closes the script at endIndex 59.
         */
        assert.equal(
            result.text,
            '<!--<script></script><p id="ghost">x</p>-->ok',
        );
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 59],
            ["p", false, 78],
        ]);
        assert.equal(result.ends, 1);
        assert.equal(result.comments, 0);

        const dom = parseDocument(ghostDocument);
        assert.deepEqual(elements(dom), [
            ["script", ""],
            ["p", "real"],
        ]);

        /*
         * Node identity: the only paragraph in the tree is the real one, and
         * the ghost id exists nowhere (no element, attribute or text node id).
         */
        const nodes = elementNodes(dom);
        const script = nodes[0];
        const realParagraph = nodes[1];
        assert.equal(script.name, "script");
        assert.equal(realParagraph.name, "p");
        assert.equal(realParagraph.attribs["id"], "real");
        assert.equal(realParagraph.children.length, 1);
        const [realText] = realParagraph.children;
        assert.equal(realText.type, "text");
        assert.equal(realText.data, "ok");

        // The ghost id must not appear anywhere in the tree.
        assert.equal(
            nodes.some((node) => node.attribs["id"] === "ghost"),
            false,
        );
        assert.equal(realParagraph.attribs["id"] === "ghost", false);

        // The script's single text node holds the ghost markup verbatim.
        assert.equal(script.children.length, 1);
        const [scriptText] = script.children;
        assert.equal(scriptText.type, "text");
        assert.equal(
            scriptText.data,
            '<!--<script></script><p id="ghost">x</p>-->',
        );
    });

    it("closes single-escaped script data at the real end tag", () => {
        const html = '<script><!--</script><p id="real">ok</p>';
        const result = observe(html);

        /*
         * `</script>` starts at index 12; its name ends at 19, so the
         * inclusive endIndex is 20.
         */
        assert.equal(result.text, "<!--ok");
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 20],
            ["p", false, 39],
        ]);
        assert.equal(result.ends, 1);
        assert.deepEqual(elements(parseDocument(html)), [
            ["script", ""],
            ["p", "real"],
        ]);
    });

    it("treats only script plus a valid delimiter as the end tag", () => {
        const html =
            '<script>a</scriptx>b</script->c</SCRIPT ><p id="real">ok</p>';
        const result = observe(html);

        /*
         * `</scriptx>` (bad word) and `</script->` (bad delimiter) stay text.
         * `</SCRIPT >` (case-insensitive, whitespace delimiter) starts at index
         * 31 and ends at 39; the `p#real` element follows at 41.
         */
        assert.equal(result.text, "a</scriptx>b</script->cok");
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 39],
            ["p", false, 59],
        ]);
        assert.equal(result.ends, 1);
        assert.equal(result.comments, 0);
    });

    it("does not enter double-escaped on near words in escaped data", () => {
        const html =
            '<script><!--<scriptx>a</script->b</script ><p id="real">ok</p>';
        const result = observe(html);

        /*
         * Neither `scriptx` nor `script-` is accepted as the temp word, so
         * `</script >` (starts at 33) closes the script, endIndex 41.
         */
        assert.equal(result.text, "<!--<scriptx>a</script->bok");
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 41],
            ["p", false, 61],
        ]);
    });

    it("matches the script keyword case-insensitively", () => {
        const html =
            '<SCRIPT><!--<ScRiPt></ScRiPt>--></SCRIPT><p id="real">ok</p>';
        const result = observe(html);

        /*
         * The mixed-case inner `<ScRiPt>` enters double-escaped, so the
         * following `</ScRiPt>` stays text and the outer close ends at 40.
         */
        assert.equal(result.text, "<!--<ScRiPt></ScRiPt>-->ok");
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 40],
            ["p", false, 59],
        ]);
        assert.equal(result.ends, 1);
    });

    it("leaves double-escaped mode on --> and closes next", () => {
        const html =
            '<script><!--<script>x--><p id="ghost">x</p></script><p id="real">ok</p>';
        const result = observe(html);

        /*
         * `-->` at index 19 ends double-escaped mode, so the ghost markup is
         * text and `</script>` (starts at 43, ends at 51) closes the script.
         */
        assert.equal(result.text, '<!--<script>x--><p id="ghost">x</p>ok');
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 51],
            ["p", false, 70],
        ]);
        assert.equal(result.ends, 1);
    });

    it("leaves escaped mode on --> and closes next", () => {
        const html =
            '<script><!--abc--><p id="ghost">x</p></script><p id="real">ok</p>';
        const result = observe(html);

        /*
         * `-->` ends escaped mode; after it, `<p ...>` is still script text
         * because only an end tag closes raw script data. `</script>` starts
         * at 37 and ends at 45.
         */
        assert.equal(result.text, '<!--abc--><p id="ghost">x</p>ok');
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(result.closes, [
            ["script", false, 45],
            ["p", false, 64],
        ]);
    });

    it("delivers unfinished script text once at EOF", () => {
        const html = "<script><!--<script>\u{1F600}x</script";
        const result = observe(html);

        assert.equal(result.text, "<!--<script>\u{1F600}x</script");
        assert.deepEqual(result.opens, [["script", ""]]);
        /*
         * No explicit close tag; the script is closed implicitly at EOF,
         * reporting the parser's start index (31).
         */
        assert.deepEqual(result.closes, [["script", true, 31]]);
        assert.equal(result.ends, 1);
        // The whole unfinished body is delivered by exactly one ontext.
        assert.deepEqual(result.textParts, ["<!--<script>\u{1F600}x</script"]);
        assert.deepEqual(result.textRanges, [[8, 31]]);
    });

    it("delivers escaped script text at EOF after a surrogate pair", () => {
        const html = "<script><!--<script>\u{1F600}</script>-->";
        const result = observe(html);

        assert.equal(result.text, "<!--<script>\u{1F600}</script>-->");
        assert.deepEqual(result.opens, [["script", ""]]);
        /*
         * The script text covers indices 8..33; the implied close reports the
         * parser's start index (34).
         */
        assert.deepEqual(result.textRanges, [[8, 34]]);
        assert.deepEqual(result.closes, [["script", true, 34]]);
        assert.equal(result.ends, 1);
    });
});

describe("script data across write boundaries", () => {
    const samples = [
        {
            name: "double-escaped with a ghost paragraph",
            html: ghostDocument,
            /*
             * Script body starts after `<script>` and runs to the final
             * `</script>`; `ok` is the text of `p#real`.
             */
            scriptText: '<!--<script></script><p id="ghost">x</p>-->',
            closes: [
                ["script", false, 59],
                ["p", false, 78],
            ],
        },
        {
            name: "temp word and close tag near misses",
            html: '<script><!--<scriptx>a</script->b</script ><p id="real">ok</p>',
            scriptText: "<!--<scriptx>a</script->b",
            closes: [
                ["script", false, 41],
                ["p", false, 61],
            ],
        },
        {
            name: "surrogate pair inside double-escaped text",
            html: '<script>\u{1F600}a<!--<script></script>--></script><p id="real">ok</p>',
            scriptText: "\u{1F600}a<!--<script></script>-->",
            closes: [
                ["script", false, 43],
                ["p", false, 62],
            ],
        },
    ];

    for (const sample of samples) {
        it(`keeps state for every single cut: ${sample.name}`, () => {
            // These expectations are literal; they are not taken from a parse.
            const expectedText = `${sample.scriptText}ok`;

            for (let cut = 1; cut < sample.html.length; cut++) {
                const run = observeSplit(sample.html, cut);
                assert.equal(
                    run.text,
                    expectedText,
                    `text differs at cut ${cut} of "${sample.name}"`,
                );
                assert.deepEqual(
                    run.opens,
                    [
                        ["script", ""],
                        ["p", "real"],
                    ],
                    `opens differ at cut ${cut} of "${sample.name}"`,
                );
                assert.deepEqual(
                    run.closes,
                    sample.closes,
                    `closes differ at cut ${cut} of "${sample.name}"`,
                );
                assert.equal(run.ends, 1, `end count differs at cut ${cut}`);
                assert.equal(
                    run.comments,
                    0,
                    `comments differ at cut ${cut}`,
                );
                /*
                 * Text may be split, but its concatenation covers exactly the
                 * same characters as the literal expectation.
                 */
                assert.equal(
                    run.textParts.join(""),
                    expectedText,
                    `text coverage differs at cut ${cut}`,
                );
            }

            /*
             * The same input written one UTF-16 code unit at a time must agree
             * with the literal expectations too.
             */
            const unitwise = observeUnitwise(sample.html);
            assert.equal(unitwise.text, expectedText, "unitwise text");
            assert.deepEqual(
                unitwise.opens,
                [
                    ["script", ""],
                    ["p", "real"],
                ],
                "unitwise opens",
            );
            assert.deepEqual(unitwise.closes, sample.closes, "unitwise closes");
            assert.equal(unitwise.ends, 1, "unitwise end count");
            assert.equal(unitwise.textParts.join(""), expectedText);
        });
    }

    it("cuts inside the temp word, the end tag, and a surrogate pair", () => {
        /*
         * `<script>` ends at 7; the inner `<script` starts at 12 (word at 13);
         * the surrogate pair occupies 20..21; the first `</script>` starts at
         * 22 and ends at 30; the final one starts at 34 and ends at 42.
         */
        const html = "<script><!--<script>\u{1F600}</script>--></script>";
        const expectedText = "<!--<script>\u{1F600}</script>-->";
        const expectedCloses = [["script", false, 42]];

        const cuts = {
            "inside the temporary word": 16,
            "inside the end tag": 27,
            "between surrogate halves": 21,
        };

        for (const [description, cut] of Object.entries(cuts)) {
            const run = observeSplit(html, cut);
            assert.equal(run.text, expectedText, `text differs ${description}`);
            assert.equal(
                run.textParts.join(""),
                expectedText,
                `joined text differs ${description}`,
            );
            assert.deepEqual(
                run.opens,
                [["script", ""]],
                `opens differ ${description}`,
            );
            assert.deepEqual(
                run.closes,
                expectedCloses,
                `close differs ${description}`,
            );
            assert.equal(run.ends, 1, `end count differs ${description}`);
        }

        // The same input written one UTF-16 unit at a time must agree too.
        const unitwise = observeUnitwise(html);
        assert.equal(unitwise.text, expectedText);
        assert.deepEqual(unitwise.opens, [["script", ""]]);
        assert.deepEqual(unitwise.closes, expectedCloses);
        assert.equal(unitwise.ends, 1);
    });

    it("produces the same ranges when written one code unit at a time", () => {
        const html =
            '<script><!--<script>\u{1F600}</script>--></script><p id="real">ok</p>';
        // Literal expectations: the script body plus the real paragraph text.
        const expectedText = "<!--<script>\u{1F600}</script>-->ok";
        const expectedCloses = [
            ["script", false, 42],
            ["p", false, 61],
        ];
        const unitwise = observeUnitwise(html);

        assert.equal(unitwise.text, expectedText);
        assert.deepEqual(unitwise.opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(unitwise.closes, expectedCloses);
        assert.equal(unitwise.ends, 1);
        // Concatenated text covers exactly the same characters.
        assert.equal(unitwise.textParts.join(""), expectedText);
        assert.equal(unitwise.textParts.join(""), `${html.slice(8, 34)}ok`);
    });
});

describe("script data pause and reset", () => {
    it("advances script state and end notification once after resume", () => {
        const firstChunk = ghostDocument.slice(0, 30);
        const secondChunk = ghostDocument.slice(30);

        let text = "";
        const opens = [];
        const closes = [];
        let ends = 0;
        let isPaused = false;

        const parser = new Parser({
            onopentag(name, attribs) {
                opens.push([name, attribs.id ?? ""]);
            },
            ontext(data) {
                text += data;
                if (!isPaused) {
                    isPaused = true;
                    parser.pause();
                }
            },
            onclosetag(name, isImplied) {
                closes.push([name, isImplied]);
            },
            onend() {
                ends++;
            },
        });

        parser.write(firstChunk);
        const textAtPause = text;
        // Queue more input and `end()` while paused; neither may run early.
        parser.write(secondChunk);
        parser.end();
        assert.equal(ends, 0, "must not finish while paused");
        assert.equal(text, textAtPause, "no text while paused");

        parser.resume();
        parser.resume();

        assert.equal(text, '<!--<script></script><p id="ghost">x</p>-->ok');
        assert.deepEqual(opens, [
            ["script", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(closes, [
            ["script", false],
            ["p", false],
        ]);
        assert.equal(ends, 1, "onend must fire exactly once");
    });

    it("stops processing after end while paused, then resumes once", () => {
        let text = "";
        let ends = 0;
        let isPaused = false;
        const parser = new Parser({
            ontext(data) {
                text += data;
                if (!isPaused) {
                    isPaused = true;
                    parser.pause();
                }
            },
            onend() {
                ends++;
            },
        });

        parser.write("<script><!--<script>");
        parser.write("x</script");
        parser.end();
        assert.equal(ends, 0);

        parser.resume();
        assert.equal(ends, 1);
        // Both chunks are processed; the partial `</script` at EOF is text.
        assert.equal(text, "<!--<script>x</script");
    });

    it("does not carry an unfinished escaped state across reset", () => {
        let text = "";
        const opened = [];
        const closed = [];
        let ends = 0;
        let resets = 0;

        const parser = new Parser({
            onopentag(name) {
                opened.push(name);
            },
            ontext(data) {
                text += data;
            },
            onclosetag(name) {
                closed.push(name);
            },
            onreset() {
                resets++;
                // Discard everything the first document emitted.
                text = "";
                opened.length = 0;
                closed.length = 0;
            },
            onend() {
                ends++;
            },
        });

        parser.write("<script><!--<script>");
        parser.reset();
        parser.write('<p id="real">ok</p>');
        parser.end();

        assert.equal(resets, 1);
        assert.equal(text, "ok");
        assert.deepEqual(opened, ["p"]);
        assert.deepEqual(closed, ["p"]);
        assert.equal(ends, 1);
    });
});

describe("non-script special tags keep their behavior", () => {
    it("treats style as RAWTEXT", () => {
        const html =
            '<style><!--<script></script><p id="ghost">x</p>--></style><p id="real">ok</p>';
        const result = observe(html);

        /*
         * RAWTEXT ignores the script-data escape rules; everything up to the
         * first `</style>` is text, including the ghost markup.
         */
        assert.equal(
            result.text,
            '<!--<script></script><p id="ghost">x</p>-->ok',
        );
        assert.deepEqual(result.opens, [
            ["style", ""],
            ["p", "real"],
        ]);
        assert.deepEqual(elements(parseDocument(html)), [
            ["style", ""],
            ["p", "real"],
        ]);
    });

    it("treats title and textarea as RCDATA", () => {
        for (const tag of ["title", "textarea"]) {
            const html = `<${tag}><!--<script></script><p id="ghost">x</p>--></${tag}><p id="real">ok</p>`;
            const result = observe(html);

            assert.equal(
                result.text,
                '<!--<script></script><p id="ghost">x</p>-->ok',
                `text for ${tag}`,
            );
            assert.deepEqual(
                result.opens,
                [
                    [tag, ""],
                    ["p", "real"],
                ],
                `opens for ${tag}`,
            );
            assert.deepEqual(
                elements(parseDocument(html)),
                [
                    [tag, ""],
                    ["p", "real"],
                ],
                `elements for ${tag}`,
            );
        }
    });

    it("keeps plaintext running to EOF", () => {
        const html = '<plaintext><p id="ghost">x</p>';
        const result = observe(html);

        assert.equal(result.text, '<p id="ghost">x</p>');
        assert.deepEqual(result.opens, [["plaintext", ""]]);
        assert.deepEqual(result.closes, [["plaintext", true, 30]]);
    });

    it("keeps XML mode treating script like an ordinary element", () => {
        const html = '<script><p id="ghost">x</p></script><p id="real">ok</p>';
        const result = observe(html, { xmlMode: true });

        // In XML the `p#ghost` is a real element inside `script`.
        assert.deepEqual(result.opens, [
            ["script", ""],
            ["p", "ghost"],
            ["p", "real"],
        ]);
        assert.equal(result.text, "xok");
        assert.deepEqual(elements(parseDocument(html, { xmlMode: true })), [
            ["script", ""],
            ["p", "ghost"],
            ["p", "real"],
        ]);
    });
});
