'use strict';

var qs = require('querystring')
  , url = require('url');

const PARSE_LINK_HEADER_MAXLEN = parseInt(process.env.PARSE_LINK_HEADER_MAXLEN) || 2000;
const PARSE_LINK_HEADER_THROW_ON_MAXLEN_EXCEEDED = process.env.PARSE_LINK_HEADER_THROW_ON_MAXLEN_EXCEEDED != null

const DEFAULT_MAX_BYTES = 8192;

// ---------------------------------------------------------------------------
// legacy shallow merge (replaces the `xtend` dependency)
// ---------------------------------------------------------------------------

function extend(target) {
  for (var i = 1; i < arguments.length; i++) {
    var source = arguments[i];
    if (!source) continue;
    var keys = Object.keys(source);
    for (var k = 0; k < keys.length; k++) {
      target[keys[k]] = source[keys[k]];
    }
  }
  return target;
}

function hasRel(x) {
  return x && x.rel;
}

function intoRels (acc, x) {
  function splitRel (rel) {
    acc[rel] = extend({}, x, { rel: rel });
  }

  x.rel.split(/\s+/).forEach(splitRel);

  return acc;
}

function createObjects (acc, p) {
  // rel="next" => 1: rel 2: next
  var m = p.match(/\s*(.+)\s*=\s*"?([^"]+)"?/)
  if (m) acc[m[1]] = m[2];
  return acc;
}

function parseLink(link) {
  try {
    var m         =  link.match(/<?([^>]*)>(.*)/)
      , linkUrl   =  m[1]
      , parts     =  m[2].split(';')
      , parsedUrl =  url.parse(linkUrl)
      , qry       =  qs.parse(parsedUrl.query);

    parts.shift();

    var info = parts
      .reduce(createObjects, {});

    info = extend(qry, info);
    info.url = linkUrl;
    return info;
  } catch (e) {
    return null;
  }
}

function checkHeader(linkHeader){
  if (!linkHeader) return false;

  if (linkHeader.length > PARSE_LINK_HEADER_MAXLEN) {
    if (PARSE_LINK_HEADER_THROW_ON_MAXLEN_EXCEEDED) {
      throw new Error('Input string too long, it should be under ' + PARSE_LINK_HEADER_MAXLEN + ' characters.');
    } else {
        return false;
      }
  }
  return true;
}

// ---------------------------------------------------------------------------
// strict tokenizer / parser used by `entries` and `nextPage`
// ---------------------------------------------------------------------------

function isAsciiWhitespace(ch) {
  return ch === ' ' || ch === '\t' || ch === '\n' ||
         ch === '\v' || ch === '\f' || ch === '\r';
}

function asciiLower(str) {
  var out = '';
  for (var i = 0; i < str.length; i++) {
    var code = str.charCodeAt(i);
    out += code >= 0x41 && code <= 0x5A ? str[i].toLowerCase() : str[i];
  }
  return out;
}

// Split a rel value on ASCII whitespace, lower-casing registered (bare,
// non-URI) relation types while extension URI relations stay untouched.
function splitRels(value) {
  var parts = value.split(/[ \t\n\v\f\r]+/).filter(function (part) {
    return part.length > 0;
  });
  if (parts.length === 0) {
    throw new SyntaxError('Link header rel parameter must not be empty');
  }
  return parts.map(function (part) {
    return part.indexOf(':') === -1 ? asciiLower(part) : part;
  });
}

// Structural tokenizer. Quoted strings and angle-quoted URIs are scanned as
// whole tokens, so ';', ',' and '<' inside them never split an entry.
function tokenize(input) {
  var tokens = [];
  var n = input.length;
  var i = 0;

  while (i < n) {
    var c = input[i];

    if (isAsciiWhitespace(c)) { i++; continue; }
    if (c === ',') { tokens.push({ type: 'comma' }); i++; continue; }
    if (c === ';') { tokens.push({ type: 'semi' }); i++; continue; }
    if (c === '=') { tokens.push({ type: 'eq' }); i++; continue; }

    if (c === '<') {
      var end = i + 1;
      while (end < n && input[end] !== '>') end++;
      if (end === n) {
        throw new SyntaxError('Unclosed angle bracket in Link header');
      }
      tokens.push({ type: 'uri', value: input.slice(i + 1, end) });
      i = end + 1;
      continue;
    }

    if (c === '"') {
      var j = i + 1;
      var raw = '';
      var closed = false;
      while (j < n) {
        var ch = input[j];
        if (ch === '\\') {
          if (j + 1 >= n) {
            throw new SyntaxError('Unclosed quoted string in Link header');
          }
          raw += input[j + 1]; // escaped char delivered as-is
          j += 2;
          continue;
        }
        if (ch === '"') { closed = true; break; }
        raw += ch;
        j++;
      }
      if (!closed) {
        throw new SyntaxError('Unclosed quoted string in Link header');
      }
      tokens.push({ type: 'qstr', value: raw });
      i = j + 1;
      continue;
    }

    // bare token
    var start = i;
    while (i < n) {
      var t = input[i];
      if (isAsciiWhitespace(t) || t === ';' || t === ',' ||
          t === '=' || t === '"' || t === '<' || t === '>') {
        break;
      }
      i++;
    }
    if (i === start) {
      throw new SyntaxError('Invalid character in Link header: ' + c);
    }
    tokens.push({ type: 'token', value: input.slice(start, i) });
  }

  return tokens;
}

function parseTokens(tokens, ctx) {
  var p = 0;

  function peek() { return tokens[p]; }

  while (p < tokens.length) {
    var uriTok = tokens[p];
    if (!uriTok || uriTok.type !== 'uri') {
      throw new SyntaxError('Link entry must begin with "<URI>"');
    }
    p++;

    var target = uriTok.value;
    if (target === '') {
      throw new SyntaxError('Link URI must not be empty');
    }
    ctx.byteCount += Buffer.byteLength(target, 'utf8');

    var params = [];
    var rels = null;
    var anchorSeen = false;

    while (peek() && peek().type === 'semi') {
      p++;

      var nameTok = peek();
      if (!nameTok || nameTok.type !== 'token') {
        throw new SyntaxError('Expected link parameter name after ";"');
      }
      p++;
      var name = asciiLower(nameTok.value);

      var eqTok = peek();
      if (!eqTok || eqTok.type !== 'eq') {
        throw new SyntaxError('Expected "=" after link parameter "' + name + '"');
      }
      p++;

      var valTok = peek();
      if (!valTok || (valTok.type !== 'token' && valTok.type !== 'qstr')) {
        throw new SyntaxError('Expected value for link parameter "' + name + '"');
      }
      p++;

      if (name === 'rel') {
        if (rels !== null) {
          throw new SyntaxError('Duplicate rel parameter in Link header entry');
        }
        rels = splitRels(valTok.value);
        rels.forEach(function (rel) {
          ctx.byteCount += Buffer.byteLength(rel, 'utf8');
        });
      } else {
        if (name === 'anchor') {
          if (anchorSeen) {
            throw new SyntaxError('Duplicate anchor parameter in Link header entry');
          }
          anchorSeen = true;
        }
        params.push([name, valTok.value]);
        ctx.byteCount += Buffer.byteLength(valTok.value, 'utf8');
      }
    }

    ctx.entries.push({ target: target, rels: rels || [], params: params });

    if (peek()) {
      if (peek().type !== 'comma') {
        throw new SyntaxError('Expected "," between Link header entries');
      }
      p++;
      ctx.byteCount += 1; // logical joining comma
      if (!peek()) {
        throw new SyntaxError('Trailing comma in Link header');
      }
    }
  }
}

function strictEntries(header, options) {
  var opts = options || {};
  var maxBytes = opts.maxBytes != null ? opts.maxBytes : DEFAULT_MAX_BYTES;
  if (typeof maxBytes !== 'number' || !Number.isFinite(maxBytes)) {
    throw new TypeError('maxBytes must be a finite number');
  }

  var headers = Array.isArray(header) ? header : [header];
  var ctx = { entries: [], byteCount: 0 };
  var started = false;

  for (var h = 0; h < headers.length; h++) {
    var value = headers[h];
    if (typeof value !== 'string') {
      throw new TypeError('Link header value must be a string');
    }
    if (value === '') continue;

    if (started) ctx.byteCount += 1; // logical comma between header strings
    started = true;

    parseTokens(tokenize(value), ctx);
  }

  if (ctx.byteCount > maxBytes) {
    throw new RangeError(
      'Link header field values exceed the ' + maxBytes + ' UTF-8 byte limit'
    );
  }

  // Return fresh arrays/objects so callers can never mutate parser state.
  return ctx.entries.map(function (entry) {
    return {
      target: entry.target,
      rels: entry.rels.slice(),
      params: entry.params.map(function (pair) { return [pair[0], pair[1]]; })
    };
  });
}

// ---------------------------------------------------------------------------
// nextPage: pick the in-context `next` link relative to the response URL
// ---------------------------------------------------------------------------

function withoutFragment(u) {
  var copy = new URL(u.href);
  copy.hash = '';
  return copy;
}

function nextPage(header, responseURL, options) {
  var entries = strictEntries(header, options);

  var response = new URL(responseURL);
  if (response.protocol !== 'http:' && response.protocol !== 'https:') {
    throw new TypeError('responseURL must be an http or https URL');
  }
  var responseKey = withoutFragment(response).href;

  var candidates = new Map();

  entries.forEach(function (entry) {
    var isNext = entry.rels.some(function (rel) {
      return rel.toLowerCase() === 'next';
    });
    if (!isNext) return;

    var base;
    var anchorPair = null;
    for (var i = 0; i < entry.params.length; i++) {
      if (entry.params[i][0] === 'anchor') { anchorPair = entry.params[i]; break; }
    }

    if (anchorPair === null) {
      // missing anchor binds the link to this response
      base = new URL(responseURL);
    } else {
      var anchor = withoutFragment(new URL(anchorPair[1], responseURL));
      if (anchor.href !== responseKey) {
        // different context (query string participates in the comparison):
        // ignore the whole entry
        return;
      }
      base = anchor;
    }

    var target = withoutFragment(new URL(entry.target, base.href));

    if (target.protocol !== 'http:' && target.protocol !== 'https:') {
      throw new Error('next link must use the http or https scheme: ' + target.href);
    }
    if (target.username !== '' || target.password !== '') {
      throw new Error('next link must not contain a username or password: ' + target.href);
    }
    var sameOrigin = target.protocol === response.protocol &&
      target.hostname === response.hostname &&
      target.port === response.port;
    if (!sameOrigin) {
      throw new Error('next link must share the response origin: ' + target.href);
    }
    if (target.href === responseKey) {
      throw new Error('next link resolves back to the response URL itself: ' + target.href);
    }

    // several entries pointing at the same canonical URL count once
    if (!candidates.has(target.href)) {
      candidates.set(target.href, target);
    }
  });

  if (candidates.size === 0) return null;
  if (candidates.size > 1) {
    throw new Error('Ambiguous next links, multiple distinct candidates: ' +
      Array.from(candidates.keys()).join(', '));
  }

  return Array.from(candidates.values())[0].href;
}

// ---------------------------------------------------------------------------
// default export: unchanged rel-keyed, lenient legacy API
// ---------------------------------------------------------------------------

function parseLinkHeader (linkHeader) {
  if (!checkHeader(linkHeader)) return null;

  return linkHeader.split(/,\s*</)
   .map(parseLink)
   .filter(hasRel)
   .reduce(intoRels, {});
}

parseLinkHeader.entries = strictEntries;
parseLinkHeader.nextPage = nextPage;

module.exports = parseLinkHeader;
