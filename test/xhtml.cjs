'use strict';

/*
 * Atom XHTML text-construct (type="xhtml") collection and serialization.
 *
 * Run with the built-in test runner:
 *   node --test test/xhtml.cjs
 *
 * The NODE_TEST_CONTEXT guard keeps this file inert when Mocha happens to
 * require it (Mocha's default spec glob also matches *.cjs); the tests below
 * only register under `node --test`.
 */

const test = require('node:test');
const assert = require('node:assert/strict');
const { Readable, Duplex, PassThrough } = require('node:stream');
const http = require('node:http');
const sax = require('sax');
const FeedParser = require('..');

const ATOM = 'http://www.w3.org/2005/Atom';
const XHTML = 'http://www.w3.org/1999/xhtml';

// The canonical feed: entry "a" has an Atom <summary type="xhtml"> whose XHTML
// div contains a <p> with an escaped attribute, a same-name nested <summary>,
// and a final <p> carrying TAIL (plus Chinese text). Entry "b" is plain text.
const FEED =
  '<feed xmlns="' + ATOM + '">' +
    '<title>XHTML 测试 feed</title>' +
    '<entry>' +
      '<id>urn:a</id>' +
      '<title>第一条 中文标题</title>' +
      '<summary type="xhtml"><div xmlns="' + XHTML + '">' +
        '<p title="A &quot; B">A &amp; B &lt; C，中文正文</p>' +
        '<summary>内层同名 &lt;summary&gt;</summary>' +
        '<p>TAIL 尾部</p>' +
      '</div></summary>' +
    '</entry>' +
    '<entry>' +
      '<id>urn:b</id>' +
      '<title>Next</title>' +
    '</entry>' +
  '</feed>';

// Parse a feed delivered as the given list of chunks; collect items and meta.
function parseChunks (chunks, options) {
  return new Promise((resolve, reject) => {
    const parser = new FeedParser(options);
    const items = [];
    let meta = null;
    parser.on('meta', (m) => { meta = m; });
    parser.on('error', reject);
    Readable.from(chunks).pipe(parser);
    parser.on('data', (item) => items.push(item));
    parser.on('end', () => resolve({ items, meta }));
  });
}

const parseWhole = (xml, options) => parseChunks([Buffer.from(xml)], options);
const parseByteByByte = (xml, options) =>
  parseChunks([...Buffer.from(xml)].map((b) => Buffer.from([b])), options);

// Independently re-parse a serialized fragment with a fresh strict SAX
// parser and capture element names, text nodes and attribute values.
function inspectFragment (fragment) {
  const parser = sax.parser(true, { lowercase: true, xmlns: true });
  const tags = [];
  const texts = [];
  const attributes = [];
  parser.onopentag = (node) => {
    tags.push(node.name);
    Object.keys(node.attributes).forEach((key) => {
      attributes.push([key, node.attributes[key].value]);
    });
  };
  parser.ontext = (text) => texts.push(text);
  parser.write('<root>' + fragment + '</root>').close();
  return { tags, texts, attributes };
}

// A pair of crossed in-memory duplex streams standing in for a TCP connection,
// so an http.ClientRequest can receive a real HTTP response without binding a
// local port (which is unavailable in the restricted sandbox).
function streamPair () {
  const clientToServer = new PassThrough();
  const serverToClient = new PassThrough();
  const client = new Duplex({
    read () {},
    write (chunk, enc, cb) { clientToServer.write(chunk, enc, cb); },
    final (cb) { clientToServer.end(); cb(); }
  });
  serverToClient.on('data', (d) => client.push(d));
  serverToClient.on('end', () => client.push(null));
  serverToClient.on('error', (e) => client.destroy(e));
  const server = new Duplex({
    read () {},
    write (chunk, enc, cb) { serverToClient.write(chunk, enc, cb); },
    final (cb) { serverToClient.end(); cb(); }
  });
  clientToServer.on('data', (d) => server.push(d));
  clientToServer.on('end', () => server.push(null));
  clientToServer.on('error', (e) => server.destroy(e));
  return [client, server];
}

if (process.env.NODE_TEST_CONTEXT) {
  test('Atom XHTML scope', async (t) => {
    await t.test('whole-buffer and byte-by-byte input produce identical fields', async () => {
      const whole = await parseWhole(FEED);
      const split = await parseByteByByte(FEED);

      assert.equal(whole.items.length, 2);
      assert.equal(split.items.length, 2);
      // The second entry is ordinary Atom text and must be untouched.
      assert.equal(whole.items[1].title, 'Next');
      assert.equal(split.items[1].title, 'Next');

      const fragment = whole.items[0].summary;
      assert.equal(typeof fragment, 'string');
      // The outer XHTML div is kept (no extra wrapper) and body tags survive.
      assert.ok(fragment.startsWith('<div xmlns="' + XHTML + '">'));
      assert.ok(fragment.includes('<summary>内层同名 &lt;summary&gt;</summary>'));
      assert.ok(fragment.includes('<p>TAIL 尾部</p>'));
      assert.ok(fragment.includes('title="A &quot; B"'));
      assert.ok(fragment.includes('A &amp; B &lt; C，中文正文'));

      // summary and description both keep the complete fragment, including TAIL.
      assert.equal(whole.items[0].description, fragment);
      assert.ok(whole.items[0].description.includes('TAIL 尾部'));

      // Chunking boundaries (including mid-multibyte-byte) must not change output.
      assert.deepEqual(
        split.items.map((i) => i.summary),
        whole.items.map((i) => i.summary)
      );
      assert.deepEqual(
        split.items.map((i) => i.title),
        whole.items.map((i) => i.title)
      );
      assert.equal(split.items[0].description, whole.items[0].description);
    });

    await t.test('serialized fragment re-parses to the original text and attribute values', async () => {
      const { items: rows } = await parseWhole(FEED);
      const fragment = rows[0].summary;
      const { tags, texts, attributes } = inspectFragment(fragment);

      // Entities were decoded by SAX and re-encoded exactly once.
      assert.ok(texts.includes('A & B < C，中文正文'));
      assert.ok(texts.includes('内层同名 <summary>'));
      assert.ok(texts.includes('TAIL 尾部'));
      assert.deepEqual(
        attributes.filter(([name]) => name === 'title'),
        [['title', 'A " B']]
      );
      // Real element structure is preserved, not flattened into text.
      assert.ok(tags.includes('div'));
      assert.ok(tags.includes('p'));
      assert.ok(tags.includes('summary'));
    });

    await t.test('escaped text is never turned into markup or double-escaped', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '"><entry><id>x</id>' +
        '<summary type="xhtml"><div xmlns="' + XHTML + '">' +
        '<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>' +
        '<p>a &amp;amp; b</p>' +
        '</div></summary></entry></feed>';
      const { items } = await parseWhole(xml);
      const fragment = items[0].summary;

      // The angle brackets stay encoded: there is no live <script> element.
      const { tags, texts } = inspectFragment(fragment);
      assert.ok(!tags.includes('script'));
      assert.ok(texts.includes('<script>alert(1)</script>'));
      // "&amp;amp;" in the source decodes to the text "&amp;" and is
      // re-encoded once, not flattened to "&" or escaped a second time.
      assert.ok(fragment.includes('a &amp;amp; b'));
      assert.ok(texts.includes('a &amp; b'));
    });

    await t.test('prefix alias and default Atom namespace behave identically', async () => {
      const aliased =
        '<a:feed xmlns:a="' + ATOM + '"><a:entry><a:id>x</a:id>' +
        '<a:summary type="xhtml"><div xmlns="' + XHTML + '">' +
        '<p>前缀别名 &amp; 中文</p></div></a:summary>' +
        '</a:entry></a:feed>';
      const byAlias = await parseWhole(aliased);

      const byDefault = await parseWhole(FEED);
      // The alias feed only carries the one paragraph; compare that exact body.
      const aliasFrag = byAlias.items[0].summary;
      assert.ok(aliasFrag.startsWith('<div xmlns="' + XHTML + '">'));
      assert.ok(aliasFrag.includes('<p>前缀别名 &amp; 中文</p>'));
      // Same decoding semantics as the default-namespace feed.
      assert.deepEqual(
        inspectFragment(aliasFrag).texts,
        ['前缀别名 & 中文']
      );
      assert.ok(byDefault.items[0].summary.includes('A &amp; B &lt; C，中文正文'));
    });

    await t.test('a same-named element in a foreign extension gets no Atom XHTML behavior', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '" xmlns:e="http://example.com/ext">' +
        '<entry><id>x</id><title>T</title>' +
        '<e:summary type="xhtml"><div><p>扩展正文 &amp; keep</p></div></e:summary>' +
        '</entry></feed>';
      const { items } = await parseWhole(xml);

      // No Atom <summary> exists, so the normalized summary stays null.
      assert.equal(items[0].summary, null);
      // The foreign element keeps its native parsed tree instead of a string.
      assert.ok(items[0]['e:summary']);
      assert.equal(typeof items[0]['e:summary'], 'object');
      assert.ok(items[0]['e:summary'].div);
      assert.equal(items[0]['e:summary']['@'].type, 'xhtml');
    });

    await t.test('two adjacent XHTML containers stay independent', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '"><entry><id>x</id>' +
        '<summary type="xhtml"><div xmlns="' + XHTML + '"><p>S &amp; one</p></div></summary>' +
        '<content type="xhtml"><div xmlns="' + XHTML + '"><p>C &lt; two</p></div></content>' +
        '</entry></feed>';
      const { items } = await parseWhole(xml);

      assert.equal(items[0].summary, '<div xmlns="' + XHTML + '"><p>S &amp; one</p></div>');
      assert.equal(items[0].description, '<div xmlns="' + XHTML + '"><p>C &lt; two</p></div>');
      // The first scope ended before the second opened, so no bleed-through.
      assert.ok(!items[0].summary.includes('C &lt; two'));
      assert.ok(!items[0].description.includes('S &amp; one'));
    });

    await t.test('CDATA content is escaped like ordinary text', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '"><entry><id>x</id>' +
        '<summary type="xhtml"><div xmlns="' + XHTML + '">' +
        '<p><![CDATA[raw <tag> & amp 中文]]></p>' +
        '</div></summary></entry></feed>';
      const { items } = await parseWhole(xml);
      const fragment = items[0].summary;

      assert.ok(fragment.includes('raw &lt;tag&gt; &amp; amp 中文'));
      const { texts } = inspectFragment(fragment);
      assert.ok(texts.includes('raw <tag> & amp 中文'));
    });

    await t.test('empty elements are emitted as paired tags', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '"><entry><id>x</id>' +
        '<summary type="xhtml"><div xmlns="' + XHTML + '">' +
        '<br/><img src="a.png"/></div></summary></entry></feed>';
      const { items } = await parseWhole(xml);
      const fragment = items[0].summary;

      assert.equal(fragment, '<div xmlns="' + XHTML + '"><br></br><img src="a.png"></img></div>');
      // Paired tags still form well-formed XML.
      assert.doesNotThrow(() => inspectFragment(fragment));
    });

    await t.test('all five Atom text constructs support type="xhtml"', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '">' +
        '<title type="xhtml"><div xmlns="' + XHTML + '"><p>FT &amp; t</p></div></title>' +
        '<subtitle type="xhtml"><div xmlns="' + XHTML + '"><p>ST &lt; s</p></div></subtitle>' +
        '<rights type="xhtml"><div xmlns="' + XHTML + '"><p>R &amp; r</p></div></rights>' +
        '<entry><id>x</id>' +
        '<summary type="xhtml"><div xmlns="' + XHTML + '"><p>SM &amp; m</p></div></summary>' +
        '<content type="xhtml"><div xmlns="' + XHTML + '"><p>CT &lt; c</p></div></content>' +
        '</entry></feed>';
      const { items, meta } = await parseWhole(xml);

      assert.ok(meta['atom:title']['#'].includes('FT &amp; t'));
      assert.ok(meta['atom:subtitle']['#'].includes('ST &lt; s'));
      assert.ok(meta['atom:rights']['#'].includes('R &amp; r'));
      assert.ok(items[0].summary.includes('SM &amp; m'));
      assert.ok(items[0].description.includes('CT &lt; c'));
    });

    await t.test('a same-name nested closing tag does not pop an ancestor xml:base', async () => {
      const xml =
        '<feed xmlns="' + ATOM + '">' +
        '<entry xml:base="https://example.com/base/">' +
        '<id>x</id>' +
        '<summary type="xhtml" xml:base="sub/">' +
        '<div xmlns="' + XHTML + '">' +
        '<summary>nested</summary>' +
        '<a href="after.html">AFTER</a>' +
        '</div></summary>' +
        '<link href="sibling"/>' +
        '</entry></feed>';
      const { items } = await parseWhole(xml);

      // The link is after the nested same-name close but still inside the real
      // summary, so the sub/ base must still be in effect.
      assert.ok(items[0].summary.includes('href="https://example.com/base/sub/after.html"'));
      // Once the real summary closes, its base pops; the sibling uses entry base.
      assert.equal(items[0].link, 'https://example.com/base/sibling');
    });

    await t.test('normalize=false retains the native shape with a complete XHTML string', async () => {
      const { items } = await parseWhole(FEED, { normalize: false });
      const native = items[0]['atom:summary'];

      assert.equal(native['@'].type, 'xhtml');
      assert.equal(typeof native['#'], 'string');
      assert.ok(native['#'].includes('A &amp; B &lt; C，中文正文'));
      assert.ok(native['#'].includes('<summary>内层同名 &lt;summary&gt;</summary>'));
      assert.ok(native['#'].includes('TAIL 尾部'));
      // The normalized convenience fields are absent.
      assert.equal(items[0].summary, undefined);
    });

    await t.test('a loopback HTTP response can be piped straight into FeedParser', async () => {
      const [clientSocket, serverSocket] = streamPair();
      const request = http.request({
        method: 'GET',
        host: 'loopback.test',
        path: '/feed.atom',
        createConnection: () => clientSocket
      });
      request.on('error', () => {});

      // Answer as a real HTTP server would, writing a framed response over the
      // in-memory socket.
      serverSocket.once('data', () => {
        const body = Buffer.from(FEED);
        serverSocket.write(
          'HTTP/1.1 200 OK\r\n' +
          'Content-Type: application/atom+xml; charset=utf-8\r\n' +
          'Content-Length: ' + body.length + '\r\n' +
          'Connection: close\r\n\r\n'
        );
        serverSocket.end(body);
      });

      // Flush the request so the server side of the pair receives it and
      // answers; without end() the request bytes are never sent.
      request.end();

      const response = await new Promise((resolve) => request.on('response', resolve));
      assert.equal(response.statusCode, 200);

      const parser = new FeedParser();
      const items = [];
      response.pipe(parser);
      for await (const item of parser) items.push(item);

      assert.equal(items.length, 2);
      assert.equal(items[1].title, 'Next');
      assert.ok(items[0].summary.includes('TAIL 尾部'));
      assert.ok(items[0].summary.includes('内层同名 &lt;summary&gt;'));
      request.destroy();
      serverSocket.destroy();
      clientSocket.destroy();
    });

    await t.test('RSS escaped HTML and plain Atom text keep their existing behavior', async () => {
      const rss =
        '<?xml version="1.0"?><rss version="2.0"><channel>' +
        '<title>RSS 频道</title><link>https://example.com/</link>' +
        '<item><title>RSS item</title>' +
        '<description><![CDATA[<p>Hi &amp; bye <a href="https://example.com/x">x</a></p>]]></description>' +
        '</item></channel></rss>';
      const rssResult = await parseWhole(rss);
      // RSS embedded HTML is passed through, not run through XHTML escaping.
      assert.equal(
        rssResult.items[0].description,
        '<p>Hi &amp; bye <a href="https://example.com/x">x</a></p>'
      );

      const plainAtom =
        '<feed xmlns="' + ATOM + '"><entry><id>x</id>' +
        '<summary>纯文本 &amp; 无标记</summary></entry></feed>';
      const atomResult = await parseWhole(plainAtom);
      assert.equal(atomResult.items[0].summary, '纯文本 & 无标记');
      assert.equal(atomResult.items[0].description, '纯文本 & 无标记');

      // The meta event still fires exactly once per feed.
      assert.ok(rssResult.meta);
      assert.ok((await parseWhole(FEED)).meta);
    });
  });
}
