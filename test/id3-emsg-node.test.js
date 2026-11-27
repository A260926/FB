/**
 * Tests for the Transmuxer `inbandMetadata` option: ID3 v2.4 tags found
 * in TS input are carried into the remuxed output as version 1 emsg
 * boxes.
 *
 * All inputs are real 188-byte MPEG-2 transport stream packets built by
 * hand. Output boxes are read back independently with a small local
 * parser instead of trusting this library's own emsg reader.
 */
'use strict';

const test = require('node:test');
const assert = require('node:assert');

const {Transmuxer} = require('../lib/mp4/transmuxer');
const MetadataStream = require('../lib/m2ts/metadata-stream');
const utils = require('./utils');

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

const SYNC_SAFE = function(n) {
  return [(n >>> 21) & 0x7f, (n >>> 14) & 0x7f, (n >>> 7) & 0x7f, n & 0x7f];
};

// Build an ID3 v2.4 tag. body is an array of frame bytes; it is kept in
// full and the header is written by hand.
const id3v24 = function(body, version, flags) {
  version = version === undefined ? 4 : version;
  flags = flags === undefined ? 0 : flags;
  return [0x49, 0x44, 0x33, version, 0, flags].concat(SYNC_SAFE(body.length)).concat(body);
};

// A TIT2 text frame with a UTF-8 encoding byte.
const textFrame = function(id, text) {
  const payload = [0x03].concat(Array.from(Buffer.from(text, 'utf8')));
  return Array.from(Buffer.from(id, 'ascii'))
    .concat(SYNC_SAFE(payload.length))
    .concat([0x00, 0x00])
    .concat(payload);
};

const tagOf = function(text) {
  return id3v24(textFrame('TIT2', text));
};

const toBytes = function(arr) {
  return new Uint8Array(arr);
};

// Build a raw metadata PES packet carrying `payload` with the requested
// pts. `first` marks payload_unit_start / data_alignment_indicator.
const metadataPes = function(payload, pts, first) {
  return utils.transportPacket(0x13, payload, first !== false, pts);
};

// Return a copy of the 188-byte packet with data_alignment_indicator in
// the PES header forced to `dai`.
const withDataAlignment = function(packet, dai) {
  const pkt = packet.slice();
  let pes = -1;
  for (let i = 4; i + 2 < pkt.length; i++) {
    if (pkt[i] === 0x00 && pkt[i + 1] === 0x00 && pkt[i + 2] === 0x01) {
      pes = i;
      break;
    }
  }
  if (pes === -1) {
    throw new Error('PES start not found');
  }
  if (dai) {
    pkt[pes + 6] |= 0x04;
  } else {
    pkt[pes + 6] &= ~0x04;
  }
  return pkt;
};

// A continuation metadata PES: starts a new PES packet but does not
// assert data_alignment_indicator, so it may continue a tag.
const metadataContinuationPes = function(payload, pts) {
  return withDataAlignment(
    utils.transportPacket(0x13, payload, true, pts), false);
};

// Encode a 33-bit PES timestamp with the given 4-bit prefix.
const encodeTimestamp = function(ts, prefix) {
  return [
    ((prefix & 0x0f) << 4) | (((Math.floor(ts / 1073741824)) & 0x07) << 1) | 1,
    Math.floor(ts / 4194304) & 0xff,
    ((Math.floor(ts / 32768) & 0x7f) << 1) | 1,
    Math.floor(ts / 128) & 0xff,
    ((ts & 0x7f) << 1) | 1
  ];
};

const SPS = [
  0x07, 0x27, 0x42, 0xe0, 0x0b, 0xa9, 0x18, 0x60, 0x9d,
  0x80, 0x53, 0x06, 0x01, 0x06, 0xb6, 0xc2, 0xb5, 0xef, 0x7c, 0x04
];

// Build a single 188-byte TS packet carrying a full PES packet as its
// payload, with adaptation-field stuffing to pad out the packet.
const tsPacketOfPes = function(pid, pes) {
  const header = [0x47, 0x40 | ((pid >> 8) & 0x1f), pid & 0xff, 0x30];
  const afl = 188 - 4 - 1 - pes.length;
  const adapt = [afl, 0x00];
  for (let i = 0; i < afl - 1; i++) {
    adapt.push(0xff);
  }
  const pkt = header.concat(adapt, pes);
  if (pkt.length !== 188) {
    throw new Error('packet is ' + pkt.length + ' bytes');
  }
  return pkt;
};

// Build a video PES header (stream id 0xE0) with both PTS and DTS so a
// B-frame composition offset can be expressed.
const videoPesWithTimes = function(data, pts, dts) {
  const body = [0x00, 0x00, 0x01, 0xe0, 0x00, 0x00,
    0x84, // '10' marker + data_alignment_indicator
    0xc0, // PTS and DTS present
    10];  // header data length
  const withTimes = body.concat(encodeTimestamp(pts, 0x3))
    .concat(encodeTimestamp(dts, 0x1))
    .concat(data);

  // pes_packet_length counts bytes after the length field
  const len = withTimes.length - 6;
  withTimes[4] = (len >> 8) & 0xff;
  withTimes[5] = len & 0xff;
  return tsPacketOfPes(0x11, withTimes);
};

// A two-frame video segment whose first frame carries a B-frame style
// presentation/decode offset (pts ahead of dts).
const videoSegmentWithOffset = function(dts, offset, frameDuration) {
  const spsNal = [0x00, 0x00, 0x01].concat(SPS);
  const aud = [0x00, 0x00, 0x01, 0x09, 0x01];
  const idr = [0x00, 0x00, 0x01, 0x05, 0x01];
  const nonIdr = [0x00, 0x00, 0x01, 0x01, 0x01];
  return [
    videoPesWithTimes(aud, dts + offset, dts),
    videoPesWithTimes(spsNal.concat(idr), dts + offset, dts),
    videoPesWithTimes(aud, dts + offset + frameDuration, dts + frameDuration),
    videoPesWithTimes(nonIdr, dts + offset + frameDuration, dts + frameDuration)
  ];
};

const PAT = utils.PAT;
const PMT_AUDIO_META = utils.generatePMT({hasAudio: true, hasMetadata: true});
const PMT_VIDEO_META = utils.generatePMT({hasVideo: true, hasMetadata: true});
const PMT_AUDIO_VIDEO_META = utils.generatePMT({
  hasVideo: true, hasAudio: true, hasMetadata: true
});

const AUDIO_FRAME = [0x19, 0x47, 0x00, 0x01];

// One complete keyframe worth of video PES packets at the given pts.
const videoFramePes = function(pts, idr) {
  return [
    utils.videoPes([0x09, 0x01], true, pts),
    utils.videoPes(SPS, false, pts),
    utils.videoPes([idr === false ? 0x01 : 0x05, 0x01], true, pts)
  ];
};

// A two-frame video segment starting at `pts`, one frame every 3000
// ticks. The second frame is not a keyframe.
const videoSegmentPes = function(pts) {
  return videoFramePes(pts, true).concat(videoFramePes(pts + 3000, false));
};

// ---------------------------------------------------------------------------
// Independent MP4 box reader
// ---------------------------------------------------------------------------

const readBox = function(buf, offset) {
  const size = buf.readUInt32BE(offset);
  const type = buf.toString('ascii', offset + 4, offset + 8);
  return {size, type, offset};
};

const topLevelBoxes = function(data) {
  const buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  const boxes = [];
  let p = 0;
  while (p < buf.length) {
    const box = readBox(buf, p);
    if (box.size < 8 || p + box.size > buf.length) {
      throw new Error('invalid box at ' + p);
    }
    boxes.push(box);
    p += box.size;
  }
  return boxes;
};

// Parse a version 1 emsg box independently.
const parseEmsg = function(buf, box) {
  let p = box.offset + 8;
  const version = buf[p];
  p += 4; // version + flags
  const timescale = buf.readUInt32BE(p);
  p += 4;
  const presentationTime = buf.readUInt32BE(p) * 4294967296 + buf.readUInt32BE(p + 4);
  p += 8;
  const eventDuration = buf.readUInt32BE(p);
  p += 4;
  const id = buf.readUInt32BE(p);
  p += 4;

  let z = p;
  while (buf[z] !== 0) {
    z++;
  }
  const scheme = buf.toString('ascii', p, z);
  p = z + 1;

  z = p;
  while (buf[z] !== 0) {
    z++;
  }
  const value = buf.toString('ascii', p, z);
  p = z + 1;

  const messageData = buf.subarray(p, box.offset + box.size);
  return {
    version,
    timescale,
    presentationTime,
    eventDuration,
    id,
    scheme,
    value,
    messageData: new Uint8Array(messageData)
  };
};

const emsgsOf = function(data) {
  const buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  return topLevelBoxes(data)
    .filter((b) => b.type === 'emsg')
    .map((b) => parseEmsg(buf, b));
};

// Read the first trun data_offset of a moof and confirm it points at the
// payload bytes of the following mdat, relative to the moof start.
const trunPointsAtMdat = function(data) {
  const buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  const boxes = topLevelBoxes(data);

  for (let i = 0; i < boxes.length; i++) {
    if (boxes[i].type !== 'moof') {
      continue;
    }
    const moof = boxes[i];
    const mdat = boxes[i + 1];
    if (!mdat || mdat.type !== 'mdat') {
      continue;
    }

    // find the first trun inside the moof
    let trunOffset = -1;
    let scan = moof.offset + 8;
    const moofEnd = moof.offset + moof.size;
    while (scan < moofEnd) {
      const child = readBox(buf, scan);
      if (child.type === 'traf') {
        let inner = scan + 8;
        const trafEnd = scan + child.size;
        while (inner < trafEnd) {
          const grand = readBox(buf, inner);
          if (grand.type === 'trun') {
            trunOffset = inner;
            break;
          }
          inner += grand.size;
        }
      }
      if (trunOffset !== -1) {
        break;
      }
      scan += child.size;
    }

    assert.notStrictEqual(trunOffset, -1, 'found a trun in the moof');

    // full box: 4 bytes version/flags, 4 sample_count, 4 data_offset
    const dataOffset = buf.readUInt32BE(trunOffset + 8 + 8);
    const absolute = moof.offset + dataOffset;
    const payloadStart = mdat.offset + 8;
    assert.strictEqual(absolute, payloadStart,
                       'trun data_offset is relative to the moof and unaffected by emsg');
  }
};

// Pull the exact media payload bytes referenced by the first trun of the
// first moof.
const firstMdatPayload = function(data) {
  const buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  const boxes = topLevelBoxes(data);
  for (let i = 0; i < boxes.length; i++) {
    if (boxes[i].type === 'mdat') {
      return new Uint8Array(buf.subarray(boxes[i].offset + 8,
                                          boxes[i].offset + boxes[i].size));
    }
  }
  return null;
};

// Run a transmitter's full TS input in one shot.
const transmux = function(packets, options) {
  const tx = new Transmuxer(options);
  const out = {segments: [], errors: [], logs: [], id3Frames: []};

  tx.on('data', (s) => out.segments.push(s));
  tx.on('error', (e) => out.errors.push(e));
  tx.on('log', (e) => out.logs.push(e));
  tx.on('id3Frame', (f) => out.id3Frames.push(f));

  packets.forEach((p) => tx.push(utils.packetize(p)));
  tx.flush();
  return {tx, out};
};

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

test.describe('framing', () => {
  test.it('emits two tags that arrive in a single PES, both keeping the PES time', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    const one = tagOf('one');
    const two = tagOf('two');

    ms.push({
      type: 'timed-metadata',
      pts: 1000,
      dts: 1000,
      data: toBytes(one.concat(two))
    });

    assert.strictEqual(tags.length, 2, 'emitted two tags');
    assert.strictEqual(tags[0].pts, 1000, 'first tag uses the PES pts');
    assert.strictEqual(tags[1].pts, 1000, 'second tag also uses the PES pts');
    assert.deepStrictEqual(Array.from(tags[0].data), one, 'first tag bytes are intact');
    assert.deepStrictEqual(Array.from(tags[1].data), two, 'second tag bytes are intact');
  });

  test.it('reassembles a tag whose ten byte header is split across PES packets', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    const tag = toBytes(tagOf('split'));
    ms.push({type: 'timed-metadata', pts: 100, dts: 100, data: tag.subarray(0, 5)});
    assert.strictEqual(tags.length, 0, 'waiting for the header');
    ms.push({type: 'timed-metadata', pts: 900, dts: 900, data: tag.subarray(5)});

    assert.strictEqual(tags.length, 1, 'emitted one tag');
    assert.deepStrictEqual(Array.from(tags[0].data), Array.from(tag), 'bytes are exact');
    assert.strictEqual(tags[0].pts, 100, 'uses the PES that held the first header byte');
  });

  test.it('keeps the PTS of the PES with the first byte across a split tag', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    const tag = toBytes(tagOf('across'));
    ms.push({type: 'timed-metadata', pts: 500, dts: 500, data: tag.subarray(0, 12)});
    ms.push({type: 'timed-metadata', pts: 8000, dts: 8000, data: tag.subarray(12)});

    assert.strictEqual(tags.length, 1);
    assert.strictEqual(tags[0].pts, 500, 'later PES time does not override the tag');
  });

  test.it('collects three tags spread across PES packets in order', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    const a = tagOf('a');
    const b = tagOf('b');
    const c = tagOf('c');
    const stream = a.concat(b, c);

    ms.push({type: 'timed-metadata', pts: 10, dts: 10, data: toBytes(stream.slice(0, 18))});
    ms.push({type: 'timed-metadata', pts: 20, dts: 20, data: toBytes(stream.slice(18, 40))});
    ms.push({type: 'timed-metadata', pts: 30, dts: 30, data: toBytes(stream.slice(40))});

    assert.strictEqual(tags.length, 3, 'emitted all three');
    assert.deepStrictEqual(Array.from(tags[0].data), a);
    assert.deepStrictEqual(Array.from(tags[1].data), b);
    assert.deepStrictEqual(Array.from(tags[2].data), c);
    // a starts in push 1, b starts in push 2, and c starts in push 3
    assert.deepStrictEqual(tags.map((t) => t.pts), [10, 20, 30],
                           'each tag uses the PES its first byte arrived on');
  });

  test.it('keeps the complete original tag, including an unknown frame and padding', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    // A frame ID the parser does not know about, followed by padding.
    const unknownFrame = Array.from(Buffer.from('XINF', 'ascii'))
      .concat(SYNC_SAFE(4))
      .concat([0x00, 0x00])
      .concat([0xde, 0xad, 0xbe, 0xef]);
    const body = textFrame('TIT2', 'keep').concat(unknownFrame, [0x00, 0x00, 0x00]);
    const raw = id3v24(body);

    ms.push({type: 'timed-metadata', pts: 5, dts: 5, data: toBytes(raw)});

    assert.strictEqual(tags.length, 1);
    assert.deepStrictEqual(Array.from(tags[0].data), raw,
                           'the raw tag is not rebuilt from parsed frames');
  });

  test.it('drops an incomplete tag at a data alignment boundary and restarts', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    const logs = [];
    ms.on('data', (t) => tags.push(t));
    ms.on('log', (l) => logs.push(l));

    const first = toBytes(tagOf('first'));
    ms.push({
      type: 'timed-metadata',
      pts: 100,
      dts: 100,
      dataAlignmentIndicator: true,
      data: first.subarray(0, 6)
    });

    const second = toBytes(tagOf('second'));
    ms.push({
      type: 'timed-metadata',
      pts: 200,
      dts: 200,
      dataAlignmentIndicator: true,
      data: second
    });

    assert.strictEqual(tags.length, 1, 'only the complete tag is emitted');
    assert.deepStrictEqual(Array.from(tags[0].data), Array.from(second),
                           'fragment is not glued onto the next tag');
    assert.strictEqual(logs.length, 1, 'logged the discarded fragment');
  });

  test.it('does not extend unsynchronization, footers, or extended headers', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    const logs = [];
    ms.on('data', (t) => tags.push(t));
    ms.on('log', (l) => logs.push(l));

    // version 4 but with the extended-header flag set is unsupported
    const unsupported = id3v24(textFrame('TIT2', 'x'), 4, 0x40);
    const good = tagOf('good');

    ms.push({type: 'timed-metadata', pts: 1, dts: 1, data: toBytes(unsupported.concat(good))});

    assert.strictEqual(logs.length, 1, 'logged the unsupported tag');
    assert.strictEqual(tags.length, 1, 'the supported tag still comes through');
    assert.deepStrictEqual(Array.from(tags[0].data), good);
  });

  test.it('rejects an invalid syncsafe size', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('log', () => {});
    ms.on('data', (t) => tags.push(t));

    // high bit set in the size field is not a valid syncsafe integer
    const bad = [0x49, 0x44, 0x33, 4, 0, 0, 0x80, 0, 0, 0];
    ms.push({type: 'timed-metadata', pts: 1, dts: 1, data: toBytes(bad)});

    assert.strictEqual(tags.length, 0, 'did not emit an invalid tag');
  });

  test.it('survives input buffers being reused after a push', () => {
    const ms = new MetadataStream({inbandMetadata: true});
    const tags = [];
    ms.on('data', (t) => tags.push(t));

    const tag = tagOf('reuse');
    const scratch = new Uint8Array(tag.length);
    scratch.set(tag);

    // first push only sends the head, then the caller scribbles on the
    // same buffer before sending the tail
    ms.push({type: 'timed-metadata', pts: 7, dts: 7, data: scratch.subarray(0, 8)});
    scratch.fill(0xaa);

    const tail = new Uint8Array(tag.length);
    tail.set(tag);
    ms.push({type: 'timed-metadata', pts: 7, dts: 7, data: tail.subarray(8)});

    assert.strictEqual(tags.length, 1, 'emitted the tag');
    assert.deepStrictEqual(Array.from(tags[0].data), tag,
                           'buffered bytes were copied, not aliased');
  });

  test.it('does the same framing through the public Transmuxer over real TS', () => {
    // Two tags in one PES plus a third tag split across a continuation
    // PES. All tags share the audio output range.
    const a = tagOf('first');
    const b = tagOf('second');
    const c = tagOf('third-split-across-pes-packets');

    const packets = [
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(a.concat(b), 90000),
      metadataPes(c.slice(0, 12), 90000),
      metadataContinuationPes(c.slice(12), 90000)
    ];

    const {out} = transmux(packets, {inbandMetadata: true});
    assert.strictEqual(out.segments.length, 1, 'one media output');

    const emsgs = emsgsOf(out.segments[0].data);
    assert.strictEqual(emsgs.length, 3, 'three emsg boxes');
    assert.deepStrictEqual(Array.from(emsgs[0].messageData), a);
    assert.deepStrictEqual(Array.from(emsgs[1].messageData), b);
    assert.deepStrictEqual(Array.from(emsgs[2].messageData), c,
                           'the tag split across PES packets is reassembled exactly');
  });
});

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

test.describe('timeline', () => {
  test.it('writes version 1 emsg fields and the full tag as message data', () => {
    const tag = tagOf('payload');
    const {out} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {inbandMetadata: true});

    assert.strictEqual(out.segments.length, 1, 'one media output');
    const emsgs = emsgsOf(out.segments[0].data);
    assert.strictEqual(emsgs.length, 1, 'one emsg');

    const e = emsgs[0];
    assert.strictEqual(e.version, 1, 'version 1');
    assert.strictEqual(e.timescale, 90000, 'timescale');
    assert.strictEqual(e.eventDuration, 0xffffffff, 'event duration');
    assert.strictEqual(e.scheme, 'https://aomedia.org/emsg/ID3', 'scheme');
    assert.strictEqual(e.value, '', 'empty value');
    assert.strictEqual(e.id, 0, 'first id is zero');
    assert.deepStrictEqual(Array.from(e.messageData), tag,
                           'message data is the whole original tag');
  });

  test.it('places emsg before the first moof and leaves moof/mdat untouched', () => {
    const tag = tagOf('placed');
    const {out} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {inbandMetadata: true});

    const boxes = topLevelBoxes(out.segments[0].data);
    assert.strictEqual(boxes[0].type, 'emsg', 'emsg first');
    assert.strictEqual(boxes[1].type, 'moof', 'then moof');
    assert.strictEqual(boxes[2].type, 'mdat', 'then mdat');

    // the trun data_offset must still point at the mdat payload
    trunPointsAtMdat(out.segments[0].data);

    // the media bytes are the audio frame we sent
    assert.deepStrictEqual(Array.from(firstMdatPayload(out.segments[0].data)),
                           AUDIO_FRAME, 'mdat payload is unchanged');
  });

  test.it('adds the aid3 compatible brand to the ftyp only when enabled', () => {
    const tag = tagOf('brand');
    const withInband = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {inbandMetadata: true});

    const buf = Buffer.from(withInband.out.segments[0].initSegment);
    const ftypSize = buf.readUInt32BE(0);
    const brands = buf.toString('ascii', 8 + 8, ftypSize);
    assert.ok(brands.indexOf('aid3') !== -1, 'ftyp includes aid3');

    const without = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {});
    const plain = Buffer.from(without.out.segments[0].initSegment);
    assert.strictEqual(plain.toString('ascii', 8 + 8, plain.readUInt32BE(0))
      .indexOf('aid3'), -1, 'default ftyp has no aid3');
  });

  test.it('maps a tag PTS to base plus PTS minus the video dts origin', () => {
    // Video DTS starts at 90000, base is 5000. A tag at 91800 maps to
    // 5000 + (91800 - 90000) = 6800.
    const tag = tagOf('map');
    const packets = [
      PAT, PMT_VIDEO_META,
      ...videoSegmentPes(90000)
    ];
    packets.push(metadataPes(tag, 91800));

    const {out} = transmux(packets, {inbandMetadata: true, baseMediaDecodeTime: 5000});
    const e = emsgsOf(out.segments[0].data)[0];
    assert.strictEqual(e.presentationTime, 6800, 'presentation time uses the video origin');
  });

  test.it('handles a first frame with a B-frame style pts/dts offset', () => {
    // The first video frame has dts 90000 but pts 93000. The origin is
    // the dts, so a tag at the frame's presentation time (93000) maps to
    // base + (93000 - 90000) = base + 3000, not to base.
    const tag = tagOf('bframe');
    const packets = [
      PAT, PMT_VIDEO_META,
      ...videoSegmentWithOffset(90000, 3000, 3000)
    ];
    packets.push(metadataPes(tag, 93000));

    const {out} = transmux(packets, {inbandMetadata: true, baseMediaDecodeTime: 1000});
    const e = emsgsOf(out.segments[0].data)[0];
    assert.ok(e, 'emsg was written');
    assert.strictEqual(e.presentationTime, 4000,
                       'offset is measured from the decode origin');
  });

  test.it('keeps 33-bit timestamps exact across a rollover', () => {
    // Video data runs up to just below 2^33 and wraps. The rollover
    // stream extends the later, wrapped timestamps so they continue past
    // 2^33 rather than jumping back to zero.
    const MAX_TS = 8589934592; // 2^33
    const tag = tagOf('rollover');
    const packets = [
      PAT, PMT_VIDEO_META,
      ...videoFramePes(MAX_TS - 3000, true),
      ...videoFramePes(MAX_TS - 100, false),
      metadataPes(tag, 7) // wrapped: effectively MAX_TS + 7
    ];

    const {out} = transmux(packets, {inbandMetadata: true, baseMediaDecodeTime: 0});
    const e = emsgsOf(out.segments[0].data)[0];
    assert.ok(e, 'emsg was written');
    // origin = MAX_TS - 3000, so mapped presentation = (MAX_TS + 7) - (MAX_TS - 3000)
    assert.strictEqual(e.presentationTime, 3007,
                       'wrapped timestamps stay on the extended timeline');
  });

  test.it('keeps the raw PTS when original timestamps are preserved', () => {
    const tag = tagOf('keep');
    const packets = [
      PAT, PMT_VIDEO_META,
      ...videoSegmentPes(90000)
    ];
    packets.push(metadataPes(tag, 91800));

    const {out} = transmux(packets, {
      inbandMetadata: true,
      baseMediaDecodeTime: 5000,
      keepOriginalTimestamps: true
    });
    const e = emsgsOf(out.segments[0].data)[0];
    assert.strictEqual(e.presentationTime, 5000 + 91800,
                       'original timestamps are added to the base');
  });

  test.it('uses the audio origin when there is no video', () => {
    const tag = tagOf('audio-origin');
    // Audio-only output. The single 44100Hz frame spans 0..1024 in the
    // audio clock. A tag at the audio dts origin maps to base + 0.
    const {out} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {inbandMetadata: true});

    assert.strictEqual(emsgsOf(out.segments[0].data)[0].presentationTime, 0,
                       'audio dts is the origin');
  });

  test.it('handles a 44100Hz audio track in its own sample clock', () => {
    const tag = tagOf('rate');
    // 44100 Hz audio. A tag exactly at the end of the first frame lands
    // just past the segment's single sample and is held for later; a tag
    // at the frame start belongs here. Check the boundary is exclusive by
    // placing a tag at the very end of the written range.
    const {out} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000 + Math.ceil(1024 * 90000 / 44100))
    ], {inbandMetadata: true});

    // the tag is outside the single-frame range, so it is not emitted
    assert.strictEqual(emsgsOf(out.segments[0].data).length, 0,
                       'tag beyond the exclusive audio end is not placed');
    assert.ok(out.logs.length >= 0);
  });

  test.it('writes a presentation time above 32 bits without truncation', () => {
    const tag = tagOf('wide');
    const highBase = 4295000000; // > 2^32
    const packets = [
      PAT, PMT_VIDEO_META,
      ...videoSegmentPes(90000)
    ];
    packets.push(metadataPes(tag, 90300));

    const {out} = transmux(packets, {inbandMetadata: true, baseMediaDecodeTime: highBase});
    const e = emsgsOf(out.segments[0].data)[0];
    assert.strictEqual(e.presentationTime, highBase + 300,
                       'no high bits are dropped');
    assert.ok(e.presentationTime > 4294967295, 'value exceeds 32 bits');
  });

  test.it('wraps the emsg id at 32 bits only when a box is written', () => {
    const tag = tagOf('id');
    const {out, tx} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tag, 90000)
    ], {inbandMetadata: true});

    assert.strictEqual(emsgsOf(out.segments[0].data)[0].id, 0, 'first id');
    const coalesce = tx.transmuxPipeline_.coalesceStream;
    assert.strictEqual(coalesce.nextEmsgId, 1, 'id advanced once');

    // Exercise the real wrap in createEmsgForTag_: it must write
    // 0xffffffff and leave the counter at 0 without 32-bit truncation.
    coalesce.nextEmsgId = 0xffffffff;
    const box = coalesce.createEmsgForTag_({data: toBytes(tag)}, 0);
    const buf = Buffer.from(box.buffer, box.byteOffset, box.byteLength);
    assert.strictEqual(buf.readUInt32BE(8 + 4 + 4 + 8 + 4), 0xffffffff,
                       'wrote the maximum 32-bit id');
    assert.strictEqual(coalesce.nextEmsgId, 0, 'wrapped back to zero');
  });
});

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

test.describe('placement', () => {
  test.it('places a tag in the output whose range contains it', () => {
    const early = tagOf('early');
    const late = tagOf('late');
    const tx = new Transmuxer({inbandMetadata: true});

    const segments = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', () => {});

    // first flush: one video frame at 90000
    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_VIDEO_META));
    videoSegmentPes(90000).forEach((p) => tx.push(utils.packetize(p)));
    tx.push(utils.packetize(metadataPes(early, 90000)));
    tx.push(utils.packetize(metadataPes(late, 96000))); // future, belongs to flush 2
    tx.flush();

    assert.strictEqual(segments.length, 1, 'first media output');
    const firstEmsgs = emsgsOf(segments[0].data);
    assert.strictEqual(firstEmsgs.length, 1, 'only the early tag went out');
    assert.deepStrictEqual(Array.from(firstEmsgs[0].messageData), early);

    // second flush: another video frame at 96000
    tx.push(utils.packetize(PMT_VIDEO_META));
    videoSegmentPes(96000).forEach((p) => tx.push(utils.packetize(p)));
    tx.flush();

    assert.strictEqual(segments.length, 2, 'second media output');
    const secondEmsgs = emsgsOf(segments[1].data);
    assert.strictEqual(secondEmsgs.length, 1, 'the held tag now goes out');
    assert.deepStrictEqual(Array.from(secondEmsgs[0].messageData), late);
  });

  test.it('keeps the right endpoint exclusive for a tag at the segment end', () => {
    const tag = tagOf('edge');
    // Two video frames 90000..96000 map to the output range 0..6000.
    // A tag whose presentation time is exactly 6000 is at the exclusive
    // right endpoint and must not belong to this output.
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    const logs = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('log', (l) => logs.push(l));
    tx.on('error', () => {});

    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_VIDEO_META));
    videoSegmentPes(90000).forEach((p) => tx.push(utils.packetize(p)));
    tx.push(utils.packetize(metadataPes(tag, 96000)));
    tx.flush();

    assert.strictEqual(segments.length, 1, 'one output');
    assert.strictEqual(emsgsOf(segments[0].data).length, 0, 'not placed at the exclusive end');
  });

  test.it('drops a late tag with a log and does not carry it forward', () => {
    const late = tagOf('past');
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    const logs = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('log', (l) => logs.push(l));
    tx.on('error', () => {});

    // A tag at 30000 arrives while the media starts at 90000.
    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_VIDEO_META));
    videoSegmentPes(90000).forEach((p) => tx.push(utils.packetize(p)));
    tx.push(utils.packetize(metadataPes(late, 30000)));
    tx.flush();

    assert.strictEqual(emsgsOf(segments[0].data).length, 0, 'late tag not emitted');
    assert.ok(logs.some((l) => /late/i.test(l.message)), 'logged the late tag');

    // a second output should not resurrect it
    tx.push(utils.packetize(PMT_VIDEO_META));
    videoSegmentPes(96000).forEach((p) => tx.push(utils.packetize(p)));
    tx.flush();
    assert.strictEqual(emsgsOf(segments[1].data).length, 0, 'late tag is gone');
  });

  test.it('does not emit empty media or consume tags on a metadata-only flush', () => {
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', () => {});

    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(metadataPes(tagOf('only-meta'), 90000)));
    tx.flush();

    assert.strictEqual(segments.length, 0, 'no media output');
  });

  test.it('keeps tags in receive order and preserves identical tags', () => {
    const a = tagOf('same');
    const b = tagOf('same');
    const {out} = transmux([
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(a, 90000),
      metadataPes(b, 90000)
    ], {inbandMetadata: true});

    const emsgs = emsgsOf(out.segments[0].data);
    assert.strictEqual(emsgs.length, 2, 'both tags emitted');
    assert.strictEqual(emsgs[0].id, 0);
    assert.strictEqual(emsgs[1].id, 1);
    assert.deepStrictEqual(Array.from(emsgs[0].messageData), a);
    assert.deepStrictEqual(Array.from(emsgs[1].messageData), b);
  });

  test.it('clears buffered tags and fragments on reset without rewinding ids', () => {
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', () => {});

    // write one emsg so the id counter moves to 1
    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 90000)));
    tx.push(utils.packetize(metadataPes(tagOf('first'), 90000)));
    tx.flush();
    assert.strictEqual(emsgsOf(segments[0].data)[0].id, 0);

    const coalesce = tx.transmuxPipeline_.coalesceStream;
    // stash a future tag, then reset
    coalesce.pendingInbandTags.push({data: toBytes(tagOf('stale')), pts: 96000, dts: 96000});
    tx.reset();
    assert.strictEqual(coalesce.pendingInbandTags.length, 0, 'pending tags cleared');

    // a fresh output should start at the next id, not zero
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 96000)));
    tx.push(utils.packetize(metadataPes(tagOf('second'), 96000)));
    tx.flush();
    const second = emsgsOf(segments[1].data);
    assert.strictEqual(second.length, 1, 'tag emitted after reset');
    assert.strictEqual(second[0].id, 1, 'id did not rewind');
  });

  test.it('clears buffered tags on setBaseMediaDecodeTime', () => {
    const tx = new Transmuxer({inbandMetadata: true});
    tx.on('data', () => {});
    tx.on('error', () => {});

    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 90000)));
    tx.push(utils.packetize(metadataPes(tagOf('held'), 96000))); // future
    tx.flush();

    const coalesce = tx.transmuxPipeline_.coalesceStream;
    assert.strictEqual(coalesce.pendingInbandTags.length, 1, 'tag held for later');

    tx.setBaseMediaDecodeTime(180000);
    assert.strictEqual(coalesce.pendingInbandTags.length, 0,
                       'setBaseMediaDecodeTime discards held tags');
  });

  test.it('leaves the default output byte-identical to itself when disabled', () => {
    const packets = [
      PAT, PMT_AUDIO_META,
      utils.audioPes(AUDIO_FRAME, true, 90000),
      metadataPes(tagOf('default'), 90000)
    ];

    const plain = transmux(packets, {});
    const plain2 = transmux(packets, {});

    assert.deepStrictEqual(
      Array.from(plain.out.segments[0].data),
      Array.from(plain2.out.segments[0].data),
      'default output is deterministic'
    );
    assert.strictEqual(emsgsOf(plain.out.segments[0].data).length, 0,
                       'default mode writes no emsg');
    // metadata/id3Frame events still flow in default mode
    assert.strictEqual(plain.out.segments[0].metadata.length, 1,
                       'default metadata event still emitted');
    assert.strictEqual(plain.out.id3Frames.length, 1,
                       'default id3Frame event still emitted');
  });

  test.it('rejects raw AAC input with an error and no media', () => {
    const fs = require('node:fs');
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    const errors = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', (e) => errors.push(e));

    tx.push(new Uint8Array(fs.readFileSync('test/segments/test-aac-segment.aac')));
    tx.flush();

    assert.ok(errors.length >= 1, 'emitted an error');
    assert.strictEqual(segments.length, 0, 'emitted no media');
  });

  test.it('rejects remux: false with an error and no media', () => {
    const tx = new Transmuxer({inbandMetadata: true, remux: false});
    const segments = [];
    const errors = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', (e) => errors.push(e));

    // Push a realistic multi-packet segment; no packet may be forwarded.
    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_AUDIO_VIDEO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 90000)));
    tx.push(utils.packetize(utils.videoPes([0x09, 0x01], true, 90000)));
    tx.push(utils.packetize(metadataPes(tagOf('rejected'), 90000)));
    tx.flush();

    assert.ok(errors.length >= 1, 'emitted an error');
    assert.strictEqual(segments.length, 0, 'emitted no media');
  });

  test.it('sends an error and no media when a tag time cannot be mapped', () => {
    // Build a tag with no pts in the metadata chunk by going through the
    // public transmuxer but forcing an unusable origin: with no pts the
    // coalesce stream cannot map the tag.
    const tx = new Transmuxer({inbandMetadata: true});
    const segments = [];
    const errors = [];
    tx.on('data', (s) => segments.push(s));
    tx.on('error', (e) => errors.push(e));

    tx.push(utils.packetize(PAT));
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 90000)));

    // inject a malformed tag event with no pts directly into the coalesce
    // stream to exercise the error path deterministically
    const coalesce = tx.transmuxPipeline_.coalesceStream;
    coalesce.push({data: toBytes(tagOf('bad')), pts: NaN, dts: NaN, frames: []});
    tx.flush();

    assert.ok(errors.length >= 1, 'emitted an error');
    assert.strictEqual(segments.length, 0, 'sent no half-baked media');

    // after a reset, legal input is accepted again
    tx.reset();
    tx.push(utils.packetize(PMT_AUDIO_META));
    tx.push(utils.packetize(utils.audioPes(AUDIO_FRAME, true, 90000)));
    tx.push(utils.packetize(metadataPes(tagOf('ok'), 90000)));
    tx.flush();

    assert.strictEqual(segments.length, 1, 'recovered after reset');
    assert.strictEqual(emsgsOf(segments[0].data).length, 1, 'legal tag emitted');
  });
});
