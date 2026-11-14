import {taskCase} from '../task_support/check.mjs';
import {parse} from '../task_support/parse.mjs';
import {fromXML, generateISD, renderSVG} from '../src/main/js/main.js';
import assert from 'node:assert/strict';
import sax from '../vendor/sax/lib/sax.js';

/*
 * Deterministic measurement: every glyph advances 0.6 em, ascent 0.8 em,
 * descent 0.2 em, so every coordinate below can be computed by hand.
 */
function measure(text, style) {
    const em = style.fontSize;
    return {
        width: 0.6 * em * text.length,
        ascent: 0.8 * em,
        descent: 0.2 * em,
    };
}

const ERROR_HANDLER = {info() {}, warn() {}, error() {}, fatal() {}};

function render(ttml, seconds, width = 200, height = 100, measureFn = measure) {
    const doc = parse(ttml);
    const isd = generateISD(doc, seconds, ERROR_HANDLER);
    return {isd, svg: renderSVG(isd, {width, height, measure: measureFn})};
}

function parseAttrs(attrString) {
    const attrs = {};
    const re = /([\w-]+)="([^"]*)"/g;
    let m;
    while ((m = re.exec(attrString)) !== null) {
        attrs[m[1]] = m[2];
    }
    return attrs;
}

function texts(svg) {
    const out = [];
    const re = /<text ([^>]*)>([^<]*)<\/text>/g;
    let m;
    while ((m = re.exec(svg)) !== null) {
        out.push({...parseAttrs(m[1]), content: m[2]});
    }
    return out;
}

function findTexts(svg, content) {
    return texts(svg).filter((t) => t.content === content);
}

function rects(svg) {
    const out = [];
    const re = /<rect ([^>]*)\/>/g;
    let m;
    while ((m = re.exec(svg)) !== null) {
        out.push(parseAttrs(m[1]));
    }
    return out;
}

function close(a, b, eps = 0.001) {
    return Math.abs(Number(a) - Number(b)) <= eps;
}

/* ------------------------------------------------------------------ */
/* inline-wrap: cross-color words, breaks, spaces, wrapOption          */
/* ------------------------------------------------------------------ */

const WRAP_TTML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" tts:extent="200px 100px">
<head><layout>
<region xml:id="rw" tts:origin="10px 10px" tts:extent="90px 80px"/>
<region xml:id="rn" tts:origin="120px 10px" tts:extent="40px 80px"/>
</layout></head>
<body><div>
<p region="rw" begin="1s" end="5s" tts:fontSize="10px">One <span tts:color="red">Wor</span><span tts:color="blue">ld</span> TwoThree</p>
<p region="rw" begin="1s" end="5s" tts:fontSize="10px">  a   b  </p>
<p region="rn" begin="1s" end="5s" tts:fontSize="10px" tts:wrapOption="noWrap">aaa bbb</p>
</div></body>
</tt>`;

taskCase('inline-wrap', (check) => {
    const {svg} = render(WRAP_TTML, 2);
    const all = texts(svg);

    const one = findTexts(svg, 'One')[0];
    const wor = findTexts(svg, 'Wor')[0];
    const ld = findTexts(svg, 'ld')[0];
    const twoThree = findTexts(svg, 'TwoThree')[0];

    /* the color-split word stays on one line and the segments stay contiguous */
    check(wor !== undefined && ld !== undefined, 'both colored spans of "World" are rendered');
    check(wor.y === ld.y, 'a word split across colored spans is not broken at the color boundary');
    check(close(Number(ld.x), Number(wor.x) + 18), 'colored segments are contiguous with no break space');
    check(wor.fill === 'rgb(255,0,0)' && ld.fill === 'rgb(0,0,255)', 'each segment keeps its own color');

    /* automatic wrapping happened after the complete word */
    check(one.y === '18' && wor.y === '18', 'first line baseline follows region origin and font metrics');
    check(twoThree.y === '28', 'the following word wraps to a second 10px-high line');
    check(Number(twoThree.x) === 10, 'wrapped line starts at the region content edge');

    /* whitespace collapses to one U+0020 and line-edge spaces disappear */
    const a = findTexts(svg, 'a')[0];
    const b = findTexts(svg, 'b')[0];
    check(a !== undefined && b !== undefined, 'collapsed paragraph keeps both words');
    check(close(Number(b.x), Number(a.x) + 12), 'collapsed whitespace contributes exactly one space width');
    check(a.y === '38', 'second paragraph follows the first with no extra paragraph gap');

    /* noWrap: an over-wide line is kept on a single line and may overflow */
    const aaa = findTexts(svg, 'aaa')[0];
    const bbb = findTexts(svg, 'bbb')[0];
    check(aaa.y === bbb.y, 'noWrap paragraphs only break at br elements');
    check(Number(bbb.x) + 18 > 120 + 40, 'noWrap overflow extends past the region rectangle');

    check(all.every((t) => Number(t['font-size']) > 0), 'every text run carries a concrete font size');
});

/* ------------------------------------------------------------------ */
/* ruby-layout: wider annotation, before/after positions, atom wraps   */
/* ------------------------------------------------------------------ */

const RUBY_TTML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" tts:extent="200px 100px">
<head><layout>
<region xml:id="rr" tts:origin="0px 0px" tts:extent="200px 100px"/>
<region xml:id="rw" tts:origin="0px 50px" tts:extent="25px 50px"/>
</layout></head>
<body><div>
<p region="rr" begin="1s" end="5s" tts:fontSize="10px">
<span tts:ruby="container"><span tts:ruby="delimiter">(</span><span tts:ruby="base">X</span><span tts:ruby="text" tts:rubyPosition="after">wide</span><span tts:ruby="delimiter">)</span></span>
<span tts:ruby="container"><span tts:ruby="base">AB</span><span tts:ruby="text" tts:rubyPosition="before">z</span></span>
</p>
<p region="rw" begin="1s" end="5s" tts:fontSize="10px">aa <span tts:ruby="container"><span tts:ruby="base">WWW</span><span tts:ruby="text" tts:rubyPosition="after">zzzz</span></span></p>
</div></body>
</tt>`;

taskCase('ruby-layout', (check) => {
    const {svg} = render(RUBY_TTML, 2);

    const baseX = findTexts(svg, 'X')[0];
    const wide = findTexts(svg, 'wide')[0];
    const baseAB = findTexts(svg, 'AB')[0];
    const z = findTexts(svg, 'z')[0];

    /* annotation wider than its base: the wider layer wins the footprint,
     * both layers are independently centered */
    check(wide['font-size'] === '5' && baseX['font-size'] === '10', 'ruby text uses its computed half font size');
    check(close(Number(wide.x), 0), 'wider annotation starts at the atom origin');
    check(close(Number(baseX.x), 3), 'narrower base is centered within the annotation width');
    check(close(Number(baseAB.x), 18) && close(Number(z.x), 22.5), 'wider base is centered and narrow annotation is offset');

    /* reserves on both sides drive the 20px line and the annotation baselines */
    check(baseX.y === '13' && baseAB.y === '13', 'base text shares the core baseline after before/after reserves');
    check(wide.y === '19', 'after annotation baseline sits in the after reserve band');
    check(z.y === '4', 'before annotation baseline sits in the before reserve band');

    /* delimiters produce no glyphs */
    check(findTexts(svg, '(').length === 0 && findTexts(svg, ')').length === 0, 'ruby delimiters are not rendered');

    /* a ruby atom that does not fit moves to the next line as one group */
    const www = findTexts(svg, 'WWW')[0];
    const zzzz = findTexts(svg, 'zzzz')[0];
    const aa = findTexts(svg, 'aa')[0];
    check(aa.y === '58', 'plain word stays on the first line of the narrow region');
    check(www.y === '68', 'ruby base moved wholesale to the second line');
    check(zzzz.y === '74', 'ruby annotation moved with its base to the second line');
    check(close(Number(zzzz.x), 3), 'moved atom keeps its centered annotation offset');
});

/* ------------------------------------------------------------------ */
/* region-output: geometry, alignment, clips, timing, repeat renders   */
/* ------------------------------------------------------------------ */

const REGION_TTML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" tts:extent="200px 100px">
<head><layout>
<region xml:id="top" tts:origin="10px 10px" tts:extent="100px 40px" tts:padding="5px 10px"
        tts:backgroundColor="#0000ff" tts:displayAlign="center"/>
<region xml:id="bottom" tts:origin="10px 60px" tts:extent="100px 30px"
        tts:backgroundColor="#00ff00" tts:displayAlign="after" tts:opacity="0.5"/>
</layout></head>
<body><div>
<p region="top" begin="1s" end="3s" tts:fontSize="10px" tts:textAlign="center">Hi</p>
<p region="bottom" begin="3s" end="5s" tts:fontSize="10px">Yo</p>
</div></body>
</tt>`;

const PERCENT_TTML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" tts:extent="200px 100px">
<head><layout><region xml:id="rp" tts:origin="25% 50%" tts:extent="50% 50%"/></layout></head>
<body region="rp"><div><p begin="1s" end="5s" tts:fontSize="10px">x</p></div></body>
</tt>`;

taskCase('region-output', (check) => {
    const at2 = render(REGION_TTML, 2);
    const at4 = render(REGION_TTML, 4);

    /* document header and forbidden constructs */
    check(at2.svg.startsWith('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">'),
        'SVG exposes xmlns, width, height and viewBox');
    check(!at2.svg.includes('foreignObject') && !at2.svg.includes('<script'), 'no foreignObject or scripts are emitted');

    /* region background rectangles and clip paths follow region geometry */
    const bg = rects(at2.svg).filter((r) => r.fill !== undefined);
    check(bg.some((r) => r.x === '10' && r.y === '10' && r.width === '100' && r.height === '40' && r.fill === 'rgb(0,0,255)'),
        'region background is painted over the region rectangle');
    const clips = [...at2.svg.matchAll(/<clipPath id="([^"]+)"><rect ([^/]*)\/><\/clipPath>/g)];
    check(clips.length === 2, 'each visible region gets a clipPath');
    check(clips[0][1] !== clips[1][1], 'clip IDs are unique within one SVG');
    const clipRect = parseAttrs(clips[0][2]);
    check(clipRect.x === '10' && clipRect.y === '10' && clipRect.width === '100' && clipRect.height === '40',
        'clip path matches the region rectangle for overflow=hidden');

    /* displayAlign center + textAlign center with padding deducted */
    const hi = findTexts(at2.svg, 'Hi')[0];
    check(close(Number(hi.x), 54) && hi.y === '33', 'center display/text alignment uses the padded content area');

    /* temporal activity differs between offsets */
    check(findTexts(at2.svg, 'Hi').length === 1 && findTexts(at2.svg, 'Yo').length === 0, 'only the active paragraph is present at 2s');
    check(findTexts(at4.svg, 'Yo').length === 1 && findTexts(at4.svg, 'Hi').length === 0, 'only the active paragraph is present at 4s');
    const yo = findTexts(at4.svg, 'Yo')[0];
    check(yo.x === '10' && yo.y === '88', 'after displayAlign pins the block to the bottom of the content area');
    check(/<g [^>]*opacity="0.5"/.test(at4.svg), 'region opacity is emitted on the region group');

    /* repeated renders never share IDs or mutate coordinates/ISD */
    const before = JSON.stringify(at2.isd.contents);
    const again = renderSVG(at2.isd, {width: 200, height: 100, measure});
    check(JSON.stringify(at2.isd.contents) === before, 'rendering does not mutate the ISD');
    check(findTexts(again, 'Hi')[0].y === '33', 'repeated render produces identical coordinates');
    const idsA = [...at2.svg.matchAll(/id="([^"]+)"/g)].map((m) => m[1]);
    const idsB = [...again.matchAll(/id="([^"]+)"/g)].map((m) => m[1]);
    check(idsA.every((id) => !idsB.includes(id)), 'each generated SVG uses fresh, non-colliding IDs');

    /* ComputedLengths resolve against the canvas, not the region box */
    const small = render(PERCENT_TTML, 2, 200, 100).svg;
    const large = render(PERCENT_TTML, 2, 400, 200).svg;
    check(findTexts(small, 'x')[0].x === '50' && findTexts(large, 'x')[0].x === '100',
        'percentage origins scale with the canvas dimensions');
    check(findTexts(small, 'x')[0]['font-size'] === '10' && findTexts(large, 'x')[0]['font-size'] === '20',
        'px lengths are relative to the root container extent and scale with the canvas');

    /* contract validation */
    assert.throws(() => renderSVG(at2.isd, {width: 0, height: 100, measure}), /positive finite/);
    assert.throws(() => renderSVG(at2.isd, {width: 10, height: 10, measure: () => ({width: 1, ascent: 0, descent: 0})}),
        /ascent \+ descent/);
    const verticalDoc = parse(`
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" tts:extent="200px 100px">
<head><layout><region xml:id="v" tts:writingMode="tblr"/></layout></head>
<body region="v"><div><p begin="1s" end="5s" tts:fontSize="10px">x</p></div></body>
</tt>`);
    const verticalISD = generateISD(verticalDoc, 2, ERROR_HANDLER);
    assert.throws(() => renderSVG(verticalISD, {width: 200, height: 100, measure}), /horizontal lrtb/);

    /* the public entry must be usable through the offline SAX adapter in Node */
    const doc = fromXML(REGION_TTML, ERROR_HANDLER, null, sax.parser(true, {xmlns: true}));
    const svg = renderSVG(generateISD(doc, 2, ERROR_HANDLER), {width: 200, height: 100, measure});
    check(findTexts(svg, 'Hi').length === 1, 'fromXML with the bundled SAX parser feeds renderSVG in Node');
});
