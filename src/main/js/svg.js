/*
 * Copyright (c) Sandflow Consulting LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * DOM-free renderer from ISD documents to standalone SVG overlays.
 *
 * Scope: horizontal (lrtb) text only, space-based word wrapping, explicit
 * <br> line breaks, colors/fonts/opacity, region origin/extent/padding,
 * displayAlign, paragraph textAlign, wrapOption, lineHeight and single-level
 * paired ruby. Vertical writing modes, bidi text and images are rejected.
 *
 * @module imscSVG
 */

import { byName } from "./styles.js";

/**
 * @typedef {import("./isd").ISD} ISD
 * @typedef {import("./isd").ISDContentElement} ISDContentElement
 */

/**
 * Text measurement callback.
 *
 * @callback MeasureFunction
 * @param {string} text Text to measure (never empty)
 * @param {{fontSize: number, fontFamily: string[], fontWeight: string, fontStyle: string}} style
 *        Computed style of the text
 * @returns {{width: number, ascent: number, descent: number}}
 */

const RUBY = byName.ruby.qname;
const RUBY_POSITION = byName.rubyPosition.qname;
const COLOR = byName.color.qname;
const BACKGROUND_COLOR = byName.backgroundColor.qname;
const FONT_SIZE = byName.fontSize.qname;
const FONT_FAMILY = byName.fontFamily.qname;
const FONT_WEIGHT = byName.fontWeight.qname;
const FONT_STYLE = byName.fontStyle.qname;
const LINE_HEIGHT = byName.lineHeight.qname;
const TEXT_ALIGN = byName.textAlign.qname;
const DISPLAY_ALIGN = byName.displayAlign.qname;
const WRAP_OPTION = byName.wrapOption.qname;
const ORIGIN = byName.origin.qname;
const EXTENT = byName.extent.qname;
const PADDING = byName.padding.qname;
const OVERFLOW = byName.overflow.qname;
const OPACITY = byName.opacity.qname;
const WRITING_MODE = byName.writingMode.qname;
const DIRECTION = byName.direction.qname;

/* monotonic counter so clip IDs stay unique across independently generated SVGs */
let clipIdCounter = 0;

function isFiniteNonNegative(v) {
    return typeof v === "number" && Number.isFinite(v) && v >= 0;
}

function isPositiveFinite(v) {
    return typeof v === "number" && Number.isFinite(v) && v > 0;
}

/* format a pixel coordinate as a short, deterministic string */
function fmt(v) {
    if (!Number.isFinite(v)) {
        throw new Error("Non-finite coordinate generated during layout");
    }
    if (Math.abs(v) < 0.0005 || Object.is(v, -0)) {
        v = 0;
    }
    return String(Number(v.toFixed(3)));
}

function escapeText(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function escapeAttr(s) {
    return escapeText(s).replace(/"/g, "&quot;").replace(/'/g, "&apos;");
}

function colorToRgba(c) {
    const alpha = c[3] / 255;
    if (alpha >= 1) {
        return "rgb(" + c[0] + "," + c[1] + "," + c[2] + ")";
    }
    return "rgba(" + c[0] + "," + c[1] + "," + c[2] + "," + fmt(alpha) + ")";
}

function fontFamilyAttr(families) {
    return families
        .map((f) => f.replace(/^['"]|['"]$/g, ""))
        .map((f) => (f.includes(" ") ? "'" + f + "'" : f))
        .join(", ");
}

/**
 * Renders an ISD object into a standalone SVG string.
 *
 * @param {ISD} isd ISD returned by generateISD()
 * @param {{width: number, height: number, measure: MeasureFunction}} options
 *        Rendering surface size and text measurement callback
 * @returns {string} Complete SVG document
 */
export function renderSVG(isd, options) {

    if (isd === null || typeof isd !== "object" || !Array.isArray(isd.contents)) {
        throw new Error("renderSVG: an ISD object is required");
    }

    if (options === null || typeof options !== "object") {
        throw new Error("renderSVG: options object is required");
    }

    if (!isPositiveFinite(options.width) || !isPositiveFinite(options.height)) {
        throw new Error("renderSVG: width and height must be positive finite pixel values");
    }

    if (typeof options.measure !== "function") {
        throw new Error("renderSVG: options.measure must be a function");
    }

    const ctx = {
        width: options.width,
        height: options.height,
        measure: options.measure,
        clipId: "imsc-svg-clip-" + (++clipIdCounter),
        clipCount: 0,
        defs: [],
        body: [],
    };

    for (const region of isd.contents) {
        if (region.kind !== "region") {
            throw new Error("renderSVG: unexpected top-level ISD element '" + region.kind + "'");
        }
        ctx.body.push(renderRegion(ctx, region));
    }

    let out = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + fmt(ctx.width) +
        "\" height=\"" + fmt(ctx.height) + "\" viewBox=\"0 0 " + fmt(ctx.width) + " " + fmt(ctx.height) + "\">";

    if (ctx.defs.length > 0) {
        out += "<defs>" + ctx.defs.join("") + "</defs>";
    }

    out += ctx.body.join("") + "</svg>";

    return out;
}

function lengthPx(ctx, computed) {
    if (computed === null || typeof computed.toUsedLength !== "function") {
        throw new Error("renderSVG: expected a computed length in the ISD");
    }
    return computed.toUsedLength(ctx.width, ctx.height);
}

/* pixel value of a computed font size against the rendering surface */
function fontSizePx(ctx, styleAttrs) {

    const fs = styleAttrs[FONT_SIZE];

    if (fs === null || fs === undefined || typeof fs.toUsedLength !== "function") {
        throw new Error("renderSVG: computed fontSize is missing from ISD style");
    }

    const px = fs.toUsedLength(ctx.width, ctx.height);

    if (!isPositiveFinite(px)) {
        throw new Error("renderSVG: computed fontSize must be a positive finite value");
    }

    return px;
}

function measureText(ctx, text, styleAttrs) {

    const style = {
        fontSize: fontSizePx(ctx, styleAttrs),
        fontFamily: styleAttrs[FONT_FAMILY],
        fontWeight: styleAttrs[FONT_WEIGHT],
        fontStyle: styleAttrs[FONT_STYLE],
    };

    let m;
    try {
        m = ctx.measure(text, style);
    } catch (e) {
        throw new Error("renderSVG: measure() callback failed: " + (e && e.message ? e.message : e));
    }

    if (m === null || typeof m !== "object" ||
        !isFiniteNonNegative(m.width) ||
        !isFiniteNonNegative(m.ascent) ||
        !isFiniteNonNegative(m.descent)) {
        throw new Error("renderSVG: measure('" + text + "') must return finite non-negative width, ascent and descent");
    }

    if (text.length > 0 && m.ascent + m.descent <= 0) {
        throw new Error("renderSVG: measure('" + text + "') must return ascent + descent > 0 for non-empty text");
    }

    return { width: m.width, ascent: m.ascent, descent: m.descent };
}

/* ------------------------------------------------------------------ */
/* Region layout                                                       */
/* ------------------------------------------------------------------ */

function renderRegion(ctx, region) {

    const sa = region.styleAttrs;

    const writingMode = sa[WRITING_MODE];

    if (writingMode !== "lrtb" && writingMode !== "lr") {
        throw new Error("renderSVG: only horizontal lrtb writing mode is supported (found '" + writingMode + "')");
    }

    const origin = sa[ORIGIN];
    const extent = sa[EXTENT];

    const regionX = lengthPx(ctx, origin.w);
    const regionY = lengthPx(ctx, origin.h);
    const regionW = lengthPx(ctx, extent.w);
    const regionH = lengthPx(ctx, extent.h);

    /* padding is already ordered [top, right, bottom, left] for lrtb */
    const padding = sa[PADDING];
    const padTop = lengthPx(ctx, padding[0]);
    const padRight = lengthPx(ctx, padding[1]);
    const padBottom = lengthPx(ctx, padding[2]);
    const padLeft = lengthPx(ctx, padding[3]);

    const contentX = regionX + padLeft;
    const contentY = regionY + padTop;
    const contentW = Math.max(0, regionW - padLeft - padRight);
    const contentH = Math.max(0, regionH - padTop - padBottom);

    const paragraphs = collectParagraphs(region);

    /* lay out every paragraph first so the final block height is known */
    const blocks = [];
    let blockHeight = 0;

    for (const p of paragraphs) {
        const block = layoutParagraph(ctx, p, contentW);
        blocks.push(block);
        blockHeight += block.height;
    }

    const displayAlign = sa[DISPLAY_ALIGN];
    let blockOffsetY;

    if (displayAlign === "center") {
        blockOffsetY = Math.max(0, (contentH - blockHeight) / 2);
    } else if (displayAlign === "after") {
        blockOffsetY = Math.max(0, contentH - blockHeight);
    } else {
        blockOffsetY = 0;
    }

    const out = [];

    const bg = sa[BACKGROUND_COLOR];
    if (bg && bg[3] > 0) {
        out.push("<rect x=\"" + fmt(regionX) + "\" y=\"" + fmt(regionY) +
            "\" width=\"" + fmt(regionW) + "\" height=\"" + fmt(regionH) +
            "\" fill=\"" + colorToRgba(bg) + "\"/>");
    }

    let cursorY = contentY + blockOffsetY;

    for (const block of blocks) {
        out.push(renderBlock(block, contentX, cursorY, contentW));
        cursorY += block.height;
    }

    const groupAttrs = [];

    if (sa[OVERFLOW] === "hidden") {
        const clipId = ctx.clipId + "-" + (++ctx.clipCount);
        ctx.defs.push("<clipPath id=\"" + clipId + "\"><rect x=\"" + fmt(regionX) +
            "\" y=\"" + fmt(regionY) + "\" width=\"" + fmt(regionW) +
            "\" height=\"" + fmt(regionH) + "\"/></clipPath>");
        groupAttrs.push("clip-path=\"url(#" + clipId + ")\"");
    }

    const opacity = sa[OPACITY];
    if (opacity !== undefined && opacity !== 1) {
        groupAttrs.push("opacity=\"" + fmt(opacity) + "\"");
    }

    return (groupAttrs.length > 0 ? "<g " + groupAttrs.join(" ") + ">" : "<g>") +
        out.join("") + "</g>";
}

function collectParagraphs(element) {

    const out = [];

    (function walk(e) {
        for (const child of e.contents ?? []) {
            if (child.kind === "p") {
                out.push(child);
            } else if (child.kind === "body" || child.kind === "div") {
                walk(child);
            } else if (child.kind === "image") {
                throw new Error("renderSVG: images are not supported");
            } else {
                throw new Error("renderSVG: unsupported ISD element '" + child.kind + "' in region flow");
            }
        }
    })(element);

    return out;
}

/* ------------------------------------------------------------------ */
/* Paragraph flow: tokens, line breaking, line metrics                 */
/* ------------------------------------------------------------------ */

function layoutParagraph(ctx, p, contentW) {

    const direction = p.styleAttrs[DIRECTION];
    if (direction === "rtl") {
        throw new Error("renderSVG: bidirectional/rtl text is not supported");
    }

    const runs = [];
    buildRuns(ctx, p, runs);

    const explicitLineHeight = p.styleAttrs[LINE_HEIGHT];
    const lineHeightPx = (explicitLineHeight === "normal" || explicitLineHeight === undefined) ?
        null :
        lengthPx(ctx, explicitLineHeight);

    const emptyLineMetrics = measureText(ctx, "M", p.styleAttrs);

    /* line breaking */
    const lines = [newLine()];

    for (const run of runs) {

        const cur = lines[lines.length - 1];

        if (run.type === "br") {
            trimLineEnd(cur);
            lines.push(newLine());
            continue;
        }

        if (run.type === "space") {
            if (!run.preserve && cur.items.length === 0) {
                /* collapsed leading spaces are discarded */
                continue;
            }
            cur.items.push(run);
            cur.width += run.width;
            continue;
        }

        /* word or ruby atom */
        const trailingSpace = trailingIsSpace(cur) ? cur.items[cur.items.length - 1] : null;
        const canBreak = trailingSpace !== null &&
            run.wrap === "wrap" &&
            trailingSpace.wrap === "wrap";

        if (canBreak && cur.width + run.width > contentW) {
            /* break after the pending space: collapsed spaces disappear,
             * preserved spaces remain at the end of the current line */
            trimLineEnd(cur);
            const fresh = newLine();
            fresh.items.push(run);
            fresh.width = run.width;
            lines.push(fresh);
        } else {
            if (cur.items.length === 0) {
                /* a single oversized atom gets its own line and may overflow */
                cur.items.push(run);
                cur.width = run.width;
            } else {
                cur.items.push(run);
                cur.width += run.width;
            }
        }
    }

    trimLineEnd(lines[lines.length - 1]);

    /* metrics and final geometry of each line */
    let height = 0;

    for (const line of lines) {
        computeLineMetrics(ctx, line, lineHeightPx, emptyLineMetrics);
        line.top = height;
        height += line.height;
    }

    return { lines, height, textAlign: p.styleAttrs[TEXT_ALIGN] };
}

function newLine() {
    return { items: [], width: 0 };
}

function trailingIsSpace(line) {
    const last = line.items[line.items.length - 1];
    return last && last.type === "space";
}

/* remove collapsed spaces at a line end; xml:space="preserve" spaces stay */
function trimLineEnd(line) {
    while (trailingIsSpace(line)) {
        const last = line.items[line.items.length - 1];
        if (last.preserve) {
            break;
        }
        line.width -= line.items.pop().width;
    }
}

function buildRuns(ctx, element, out) {
    for (const child of element.contents ?? []) {

        if (child.kind === "br") {
            out.push({ type: "br" });
            continue;
        }

        if (child.kind !== "span") {
            throw new Error("renderSVG: unsupported ISD element '" + child.kind + "' in paragraph");
        }

        const ruby = child.styleAttrs[RUBY];

        if (ruby === "delimiter") {
            /* ruby delimiters are never rendered */
            continue;
        }

        if (ruby === "container") {
            out.push(buildRubyRun(ctx, child));
            continue;
        }

        if (ruby === "baseContainer" || ruby === "textContainer") {
            throw new Error("renderSVG: nested or non-paired ruby is not supported");
        }

        if (ruby === "base" || ruby === "text") {
            throw new Error("renderSVG: ruby base/text must be enclosed in a ruby container");
        }

        if (ruby !== "none" && ruby !== undefined && ruby !== null) {
            throw new Error("renderSVG: unsupported ruby role '" + ruby + "'");
        }

        if ("text" in child && child.text !== null && child.text.length > 0) {
            appendTextRuns(ctx, child, out);
        }

        if ("contents" in child) {
            buildRuns(ctx, child, out);
        }
    }
}

function appendTextRuns(ctx, span, out) {

    const preserve = span.space === "preserve";
    const fsPx = fontSizePx(ctx, span.styleAttrs);
    const wrap = span.styleAttrs[WRAP_OPTION];

    for (const token of tokenize(span.text, preserve)) {
        if (token.space) {
            if (preserve) {
                /* each preserved space is kept as its own run and never collapsed */
                for (let i = 0; i < token.text.length; i++) {
                    out.push(makeSpaceRun(ctx, span.styleAttrs, fsPx, wrap, true));
                }
            } else {
                /* default whitespace collapses, including across adjacent spans */
                const prev = out.length > 0 ? out[out.length - 1] : null;
                if (prev && prev.type === "space" && !prev.preserve) {
                    prev.width = Math.max(prev.width, measureText(ctx, " ", span.styleAttrs).width);
                } else {
                    out.push(makeSpaceRun(ctx, span.styleAttrs, fsPx, wrap, false));
                }
            }
        } else {
            const m = measureText(ctx, token.text, span.styleAttrs);
            out.push({
                type: "word",
                text: token.text,
                width: m.width,
                ascent: m.ascent,
                descent: m.descent,
                fontSizePx: fsPx,
                styleAttrs: span.styleAttrs,
                wrap,
            });
        }
    }
}

function makeSpaceRun(ctx, styleAttrs, fsPx, wrap, preserve) {
    const m = measureText(ctx, " ", styleAttrs);
    return {
        type: "space",
        text: " ",
        width: m.width,
        preserve,
        fontSizePx: fsPx,
        styleAttrs,
        wrap,
    };
}

function tokenize(text, preserve) {
    const out = [];
    /* any XML whitespace character is a break opportunity; only U+0020 is
     * ever emitted, with one run per character when xml:space="preserve" */
    const re = preserve ? /([ \t\r\n]+)|([^ \t\r\n]+)/g : /(\s+)|(\S+)/g;
    let m;
    while ((m = re.exec(text)) !== null) {
        out.push({ space: m[1] !== undefined, text: m[0] });
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Ruby                                                                */
/* ------------------------------------------------------------------ */

function buildRubyRun(ctx, container) {

    let base = null;
    let text = null;

    for (const child of container.contents ?? []) {
        const role = child.styleAttrs[RUBY];
        if (role === "delimiter") {
            continue;
        }
        if (role === "container" || role === "baseContainer" || role === "textContainer") {
            throw new Error("renderSVG: nested ruby containers are not supported");
        }
        if (role === "base") {
            if (base !== null) {
                throw new Error("renderSVG: a ruby container must contain exactly one base");
            }
            base = child;
        } else if (role === "text") {
            if (text !== null) {
                throw new Error("renderSVG: a ruby container must contain exactly one ruby text");
            }
            text = child;
        } else {
            throw new Error("renderSVG: unsupported child of ruby container");
        }
    }

    if (base === null || text === null) {
        throw new Error("renderSVG: a ruby container requires one base and one text");
    }

    const position = text.styleAttrs[RUBY_POSITION];

    if (position !== "before" && position !== "after") {
        throw new Error("renderSVG: only rubyPosition 'before' and 'after' are supported (found '" + position + "')");
    }

    const baseSegs = collectRubySegments(ctx, base);
    const textSegs = collectRubySegments(ctx, text);

    if (baseSegs.length === 0 || textSegs.length === 0) {
        throw new Error("renderSVG: ruby base and text must both contain text");
    }

    const baseWidth = baseSegs.reduce((s, seg) => s + seg.width, 0);
    const textWidth = textSegs.reduce((s, seg) => s + seg.width, 0);

    return {
        type: "ruby",
        position,
        width: Math.max(baseWidth, textWidth),
        baseWidth,
        textWidth,
        baseSegs,
        textSegs,
        wrap: container.styleAttrs[WRAP_OPTION] ?? "wrap",
    };
}

function collectRubySegments(ctx, elem) {

    const segs = [];

    function pushText(child) {
        if ("text" in child && child.text !== null && child.text.length > 0) {
            const m = measureText(ctx, child.text, child.styleAttrs);
            segs.push({
                text: child.text,
                width: m.width,
                ascent: m.ascent,
                descent: m.descent,
                fontSizePx: fontSizePx(ctx, child.styleAttrs),
                styleAttrs: child.styleAttrs,
            });
        }
    }

    (function walk(e) {
        pushText(e);
        for (const child of e.contents ?? []) {
            if (child.kind === "br") {
                throw new Error("renderSVG: line breaks inside ruby are not supported");
            }
            if (child.kind !== "span") {
                throw new Error("renderSVG: unsupported element inside ruby");
            }
            const role = child.styleAttrs[RUBY];
            if (role && role !== "none") {
                throw new Error("renderSVG: nested ruby is not supported");
            }
            walk(child);
        }
    })(elem);

    return segs;
}

/* ------------------------------------------------------------------ */
/* Line metrics                                                        */
/* ------------------------------------------------------------------ */

function computeLineMetrics(ctx, line, lineHeightPx, emptyLineMetrics) {

    let coreAscent = 0;
    let coreDescent = 0;
    let beforeReserve = 0;
    let afterReserve = 0;
    let beforeMaxAscent = 0;
    let afterMaxAscent = 0;
    let hasContent = false;

    for (const item of line.items) {
        if (item.type === "space") {
            continue;
        }
        hasContent = true;

        if (item.type === "word") {
            coreAscent = Math.max(coreAscent, item.ascent);
            coreDescent = Math.max(coreDescent, item.descent);
        } else {
            /* ruby atom */
            for (const seg of item.baseSegs) {
                coreAscent = Math.max(coreAscent, seg.ascent);
                coreDescent = Math.max(coreDescent, seg.descent);
            }

            let annHeight = 0;
            let annMaxAscent = 0;

            for (const seg of item.textSegs) {
                annHeight = Math.max(annHeight, seg.ascent + seg.descent);
                annMaxAscent = Math.max(annMaxAscent, seg.ascent);
            }

            if (item.position === "before") {
                beforeReserve = Math.max(beforeReserve, annHeight);
                beforeMaxAscent = Math.max(beforeMaxAscent, annMaxAscent);
            } else {
                afterReserve = Math.max(afterReserve, annHeight);
                afterMaxAscent = Math.max(afterMaxAscent, annMaxAscent);
            }
        }
    }

    if (!hasContent) {
        coreAscent = emptyLineMetrics.ascent;
        coreDescent = emptyLineMetrics.descent;
    }

    const metricHeight = coreAscent + coreDescent;
    const coreHeight = Math.max(metricHeight, lineHeightPx ?? 0);
    const halfLeading = (coreHeight - metricHeight) / 2;

    line.coreAscent = coreAscent;
    line.coreDescent = coreDescent;
    line.coreHeight = coreHeight;
    line.halfLeading = halfLeading;
    line.beforeReserve = beforeReserve;
    line.afterReserve = afterReserve;
    line.beforeMaxAscent = beforeMaxAscent;
    line.afterMaxAscent = afterMaxAscent;
    line.height = beforeReserve + coreHeight + afterReserve;
}

/* ------------------------------------------------------------------ */
/* SVG emission                                                        */
/* ------------------------------------------------------------------ */

function renderBlock(block, contentX, blockTop, contentW) {

    const out = [];

    for (const line of block.lines) {

        let lineX = contentX;

        if (block.textAlign === "center") {
            lineX = contentX + (contentW - line.width) / 2;
        } else if (block.textAlign === "end") {
            lineX = contentX + (contentW - line.width);
        }

        const coreTop = blockTop + line.top + line.beforeReserve;
        const coreBaseline = coreTop + line.halfLeading + line.coreAscent;
        const coreBottom = coreTop + line.coreHeight;
        /* annotation layers fill their reserve band and share one baseline
         * derived from the layer's maximum ascent */
        const beforeBaseline = coreTop - line.beforeReserve + line.beforeMaxAscent;
        const afterBaseline = coreBottom + line.afterMaxAscent;

        let cursor = 0;

        for (const item of line.items) {
            if (item.type === "space") {
                cursor += item.width;
            } else if (item.type === "word") {
                out.push(renderText(item.text, item.styleAttrs, item.fontSizePx, lineX + cursor, coreBaseline));
                cursor += item.width;
            } else {
                out.push(renderRuby(item, lineX + cursor, coreBaseline, beforeBaseline, afterBaseline));
                cursor += item.width;
            }
        }
    }

    return out.join("");
}

function renderRuby(item, atomX, coreBaseline, beforeBaseline, afterBaseline) {

    const out = [];
    const baseOffset = (item.width - item.baseWidth) / 2;
    const textOffset = (item.width - item.textWidth) / 2;
    const annotationBaseline = item.position === "before" ? beforeBaseline : afterBaseline;

    let x = atomX + baseOffset;
    for (const seg of item.baseSegs) {
        out.push(renderText(seg.text, seg.styleAttrs, seg.fontSizePx, x, coreBaseline));
        x += seg.width;
    }

    x = atomX + textOffset;
    for (const seg of item.textSegs) {
        out.push(renderText(seg.text, seg.styleAttrs, seg.fontSizePx, x, annotationBaseline));
        x += seg.width;
    }

    return out.join("");
}

function renderText(text, styleAttrs, fontSize, x, y) {

    const attrs = [
        "x=\"" + fmt(x) + "\"",
        "y=\"" + fmt(y) + "\"",
        "font-family=\"" + escapeAttr(fontFamilyAttr(styleAttrs[FONT_FAMILY])) + "\"",
        "font-size=\"" + fmt(fontSize) + "\"",
        "font-weight=\"" + escapeAttr(String(styleAttrs[FONT_WEIGHT])) + "\"",
        "font-style=\"" + escapeAttr(String(styleAttrs[FONT_STYLE])) + "\"",
        "fill=\"" + colorToRgba(styleAttrs[COLOR]) + "\"",
    ];

    return "<text " + attrs.join(" ") + ">" + escapeText(text) + "</text>";
}
