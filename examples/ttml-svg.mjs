#!/usr/bin/env node
/*
 * Node example: turn an IMSC/TTML document into a standalone SVG overlay.
 *
 * Usage:
 *   node examples/ttml-svg.mjs input.ttml seconds width height > out.svg
 *
 * Pipeline: the bundled offline SAX parser feeds the public fromXML() entry,
 * generateISD() computes the intermediate document at the given time offset,
 * and renderSVG() lays it out without any DOM, Canvas or browser API.
 *
 * Support scope (see task_support/svg_contract.md):
 *   - horizontal lrtb text only; vertical writing modes and bidi are rejected
 *   - space-based word wrapping, explicit <br/> breaks, xml:space handling
 *   - colors, fonts, opacity, region origin/extent/padding, displayAlign,
 *     paragraph textAlign, wrapOption and lineHeight
 *   - single-level paired ruby (rubyPosition before/after)
 *   - images, nested/non-paired ruby, strokes and shadows are not supported
 *
 * The measurement function below is a deliberately simple, deterministic
 * fixed-pitch stand-in. It lives in this example on purpose: real deployments
 * must supply metrics from an actual font scaler, since renderSVG() never
 * measures text itself.
 */

import fs from "node:fs";

import sax from "../vendor/sax/lib/sax.js";
import { fromXML, generateISD, renderSVG } from "../src/main/js/main.js";

function fail(message) {
    process.stderr.write("ttml-svg: " + message + "\n");
    process.exit(1);
}

const [inputPath, secondsArg, widthArg, heightArg] = process.argv.slice(2);

if (process.argv.length !== 6) {
    fail("usage: node examples/ttml-svg.mjs input.ttml seconds width height");
}

const seconds = Number(secondsArg);
const width = Number(widthArg);
const height = Number(heightArg);

if (!Number.isFinite(seconds) || seconds < 0) {
    fail("seconds must be a finite non-negative number");
}

if (!(Number.isFinite(width) && width > 0) || !(Number.isFinite(height) && height > 0)) {
    fail("width and height must be positive finite pixel values");
}

let ttml;

try {
    ttml = fs.readFileSync(inputPath, "utf8");
} catch (e) {
    fail("cannot read '" + inputPath + "': " + e.message);
}

const errorHandler = {
    info() { },
    warn() { },
    error(msg) {
        fail("document error: " + msg);
    },
    fatal(msg) {
        fail("fatal document error: " + msg);
    },
};

let doc;

try {
    const saxParser = sax.parser(true, { xmlns: true });
    doc = fromXML(ttml, errorHandler, null, saxParser);
} catch (e) {
    fail("parsing failed: " + e.message);
}

let svg;

try {
    const isd = generateISD(doc, seconds, errorHandler);
    svg = renderSVG(isd, { width, height, measure: fixedPitchMeasure });
} catch (e) {
    fail("rendering failed: " + e.message);
}

process.stdout.write(svg);

/*
 * Fixed-pitch stand-in measurement:
 *   advance width = 0.6 em per character, ascent = 0.8 em, descent = 0.2 em.
 * Replace with real font metrics before using this in production.
 */
function fixedPitchMeasure(text, style) {
    const em = style.fontSize;
    return {
        width: 0.6 * em * text.length,
        ascent: 0.8 * em,
        descent: 0.2 * em,
    };
}
