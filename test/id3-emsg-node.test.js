'use strict';

/**
 * End to end tests for the Transmuxer `inbandMetadata` option.
 *
 * Every transport stream used here is constructed in this file from
 * individual 188 byte MPEG-2 TS packets (no external fixtures or npm
 * packages). emsg/moof boxes are parsed with the local readers below
 * rather than with the library's own emsg parser, so the assertions do
 * not validate the output against itself.
 */

const {describe, it} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {Transmuxer} = require('../lib/mp4/transmuxer');
const MetadataStream = require('../lib/m2ts/metadata-stream');

// ---------------------------------------------------------------------------
// ID3 v2.4 tag construction
// ---------------------------------------------------------------------------

function syncsafe(n) {
  return [
    Math.floor(n / 0x200000) & 0x7f,
    Math.floor(n / 0x4000) & 0x7f,
    Math.floor(n / 0x80) & 0x7f,
    n & 0x7f
  ];
}

// Build a minimal, spec legal ID3v2.4 tag: version 4, revision 0, no
// flags, one frame. The complete original bytes are returned so tests
// can compare them against emsg message_data.
function id3v24(frameId, payload) {
  const frameHeader = [];

  for (let i = 0; i < frameId.length; i++) {
    frameHeader.push(frameId.charCodeAt(i));
  }
  const frame = frameHeader
    .concat(syncsafe(payload.length), [0x00, 0x00])
    .concat(Array.from(payload));
  const bytes = [0x49, 0x44, 0x33, 0x04, 0x00, 0x00]
    .concat(syncsafe(frame.length), frame);

  return new Uint8Array(bytes);
}

// Build a raw header for negative framing tests. sizeBytes must each be
// legal 7-bit values when syncSafe is true.
function id3Header(version, flags, sizeBytes) {
  return new Uint8Array([0x49, 0x44, 0x33, version, 0x00, flags]
    .concat(sizeBytes));
}

// ---------------------------------------------------------------------------
// 188 byte transport stream construction
// ---------------------------------------------------------------------------

const TS_PACKET_LENGTH = 188;
const SYNC_BYTE = 0x47;
const PID_PAT = 0x0000;
const PID_PMT = 0x0010;
const PID_VIDEO = 0x0011;
const PID_AUDIO = 0x0012;
const PID_METADATA = 0x0013;

function continuityState() {
  return { counters: {} };
}

function tsPacket(pid, payloadUnitStart, payload, state) {
  const packet = new Uint8Array(TS_PACKET_LENGTH);

  packet[0] = SYNC_BYTE;
  packet[1] = (payloadUnitStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);
  packet[2] = pid & 0xff;

  const counter = (state.counters[pid] || 0) & 0x0f;

  state.counters[pid] = counter + 1;

  let offset = 4;

  if (payload.length <= 184) {
    // adaptation field (stuffing) followed by payload
    const stuffing = 184 - payload.length;

    packet[3] = 0x30 | counter;
    packet[4] = stuffing - 1;
    if (stuffing - 1 > 0) {
      packet[5] = 0x00;
      for (let i = 6; i < 4 + stuffing; i++) {
        packet[i] = 0xff;
      }
    }
    offset = 4 + stuffing;
  } else {
    // payload only, fills the packet
    packet[3] = 0x10 | counter;
  }

  packet.set(payload.subarray(0, TS_PACKET_LENGTH - offset), offset);
  return packet;
}

function crc32Mpeg2(bytes) {
  let crc = 0xffffffff;

  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] << 24;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80000000) ?
        ((crc << 1) ^ 0x04c11db7) >>> 0 :
        (crc << 1) >>> 0;
    }
  }
  return crc >>> 0;
}

function patPacket() {
  // one program pointing at PMT pid 0x0010
  const packet = new Uint8Array(TS_PACKET_LENGTH);
  const payload = [
    0x00, // pointer field
    0x00, 0xb0, 0x11, // table id 0, section length 17
    0x00, 0x01, // program number
    0xc1, 0x00, 0x00, // version, current next, section numbers
    0x00, 0x01, 0x00, 0x10, // program 1 -> pmt pid
    0x00, 0x00, 0x00, 0x00 // crc
  ];

  packet[0] = SYNC_BYTE;
  packet[1] = 0x40;
  packet[2] = PID_PAT;
  packet[3] = 0x10;
  packet.set(payload, 4);
  return packet;
}

function pmtPacket(state, kinds) {
  const streamTypes = {
    video: [0x1b, PID_VIDEO],
    audio: [0x0f, PID_AUDIO],
    meta: [0x15, PID_METADATA]
  };
  const section = [
    0x02, 0x00, 0x00, // table id, section length placeholder
    0x00, 0x01, // program number
    0xc1, // version 0, current next indicator
    0x00, 0x00, // section / last section number
    0xe0, kinds.indexOf('video') >= 0 ? PID_VIDEO : PID_AUDIO, // PCR pid
    0xf0, 0x00 // program info length 0
  ];

  kinds.forEach(function(kind) {
    const entry = streamTypes[kind];

    section.push(entry[0], (entry[1] >> 8) & 0x1f, entry[1] & 0xff, 0xf0, 0x00);
  });

  const sectionLength = (section.length - 3) + 4; // trailing CRC32

  section[1] = 0xb0 | ((sectionLength >> 8) & 0x0f);
  section[2] = sectionLength & 0xff;

  const crc = crc32Mpeg2(section);

  section.push(
    (crc >>> 24) & 0xff,
    (crc >>> 16) & 0xff,
    (crc >>> 8) & 0xff,
    crc & 0xff
  );

  return tsPacket(PID_PMT, true, new Uint8Array([0x00].concat(section)), state);
}

// Encode a 33 bit PTS/DTS into the five PES timestamp bytes. Pure
// arithmetic (no bitwise operators) so the full 2^33 range encodes
// correctly; round-trips through the library's PES parser.
function encodeTimestamp(prefix, value) {
  return [
    prefix | 0x01 | ((Math.floor(value / 0x40000000) & 0x07) * 2),
    Math.floor(value / 0x400000) & 0xff,
    (Math.floor(value / 0x4000) & 0xff) | 0x01,
    Math.floor(value / 0x80) & 0xff,
    ((value % 0x80) * 2) | 0x01
  ];
}

// Packetize one PES into real 188 byte TS packets, splitting across as
// many packets as the payload needs.
function pesTsPackets(pid, streamId, data, options, state) {
  const pts = options.pts;
  const dts = options.dts === undefined ? pts : options.dts;
  const hasDts = dts !== pts;
  const headerDataLength = hasDts ? 10 : 5;
  const header = [0x00, 0x00, 0x01, streamId];
  const packetLength = 3 + headerDataLength + data.length;

  header.push((packetLength >> 8) & 0xff, packetLength & 0xff);
  header.push(0x80 | (options.alignment ? 0x04 : 0x00),
    hasDts ? 0xc0 : 0x80, headerDataLength);
  if (hasDts) {
    header.push.apply(header, encodeTimestamp(0x30, pts));
    header.push.apply(header, encodeTimestamp(0x10, dts));
  } else {
    header.push.apply(header, encodeTimestamp(0x20, pts));
  }

  const pes = new Uint8Array(header.length + data.length);

  pes.set(header, 0);
  pes.set(data, header.length);

  const packets = [];
  let position = 0;
  let first = true;

  while (position < pes.length) {
    packets.push(tsPacket(pid, first, pes.subarray(position), state));
    position += Math.min(pes.length - position, 184);
    first = false;
  }
  return packets;
}

// ADTS header for an AAC-LC, 44.1kHz, stereo frame.
function adtsHeader(payloadLength) {
  const frameLength = payloadLength + 7;

  return new Uint8Array([
    0xff, 0xf1, 0x10,
    0xb0 | ((frameLength >> 11) & 0x03),
    (frameLength >> 3) & 0xff,
    ((frameLength & 0x07) << 5) | 0x1f,
    0x00
  ]);
}

function adtsFrame(payload) {
  const header = adtsHeader(payload.length);
  const frame = new Uint8Array(header.length + payload.length);

  frame.set(header, 0);
  frame.set(payload, header.length);
  return frame;
}

const SPS_BYTES = [
  0x27, 0x42, 0xe0, 0x0b, 0xa9, 0x18, 0x60, 0x9d,
  0x80, 0x53, 0x06, 0x01, 0x06, 0xb6, 0xc2, 0xb5,
  0xef, 0x7c, 0x04
];

// One access unit: AUD, SPS, PPS and an IDR or non-IDR slice.
function h264Frame(idr) {
  return new Uint8Array([
    0x00, 0x00, 0x01, 0x09, 0x10, // access unit delimiter
    0x00, 0x00, 0x01, 0x07
  ].concat(SPS_BYTES, [
    0x00, 0x00, 0x01, 0x08, 0x01, // picture parameter set
    0x00, 0x00, 0x01, idr ? 0x05 : 0x01, 0x88, 0x80, 0x40
  ]));
}

// Assemble a complete transport stream from declarative parts.
function buildTransportStream(parts, options) {
  const state = continuityState();
  const kinds = (options && options.kinds) || ['video', 'audio', 'meta'];
  const packets = [patPacket(), pmtPacket(state, kinds)];

  parts.forEach(function(part) {
    if (part.kind === 'video') {
      packets.push.apply(packets, pesTsPackets(
        PID_VIDEO, 0xe0, part.data, {
          pts: part.pts,
          dts: part.dts,
          alignment: true
        }, state));
    } else if (part.kind === 'audio') {
      packets.push.apply(packets, pesTsPackets(
        PID_AUDIO, 0xc0, part.data, {
          pts: part.pts,
          dts: part.dts,
          alignment: true
        }, state));
    } else if (part.kind === 'meta') {
      // a metadata part may be split into several PES packets to
      // exercise tags that span PES boundaries, including a split header
      const chunks = part.chunks || [{
        bytes: part.data,
        pts: part.pts,
        alignment: true
      }];

      chunks.forEach(function(chunk, index) {
        packets.push.apply(packets, pesTsPackets(
          PID_METADATA, 0x06, chunk.bytes, {
            pts: chunk.pts,
            dts: chunk.pts,
            alignment: chunk.alignment !== undefined ?
              chunk.alignment : index === 0
          }, state));
      });
    }
  });

  const result = new Uint8Array(packets.length * TS_PACKET_LENGTH);

  packets.forEach(function(packet, index) {
    result.set(packet, index * TS_PACKET_LENGTH);
  });
  return result;
}

function videoPart(dts, pts, idr) {
  return {kind: 'video', data: h264Frame(idr !== false), pts: pts, dts: dts};
}

function audioPart(pts, payload) {
  return {
    kind: 'audio',
    data: adtsFrame(payload || new Uint8Array([0xde, 0xad, 0xbe, 0xef])),
    pts: pts,
    dts: pts
  };
}

function metaPart(tag, pts, chunks) {
  return {kind: 'meta', data: tag, pts: pts, chunks: chunks};
}

// ---------------------------------------------------------------------------
// Independent ISO BMFF box readers (do not use the library emsg parser)
// ---------------------------------------------------------------------------

function readUint32(uint8, offset) {
  const view = new DataView(uint8.buffer, uint8.byteOffset + offset, 4);

  return view.getUint32(0);
}

function readUint64(uint8, offset) {
  const high = readUint32(uint8, offset);
  const low = readUint32(uint8, offset + 4);

  return (high * Math.pow(2, 32)) + low;
}

function readTopLevelBoxes(uint8) {
  const boxes = [];
  let offset = 0;

  while (offset < uint8.byteLength) {
    const size = readUint32(uint8, offset);
    const type = String.fromCharCode.apply(null,
      uint8.subarray(offset + 4, offset + 8));

    assert.ok(size >= 8, 'box size is at least the header size');
    assert.ok(offset + size <= uint8.byteLength, 'box stays in bounds');
    boxes.push({type: type, size: size, offset: offset});
    offset += size;
  }
  return boxes;
}

function readCString(uint8, offset) {
  let end = offset;

  while (uint8[end] !== 0x00) {
    end++;
  }
  return {
    value: Buffer.from(
      uint8.buffer, uint8.byteOffset + offset, end - offset
    ).toString('ascii'),
    next: end + 1
  };
}

// Parse a version 1 emsg box directly from its bytes.
function readEmsgV1(uint8, box) {
  const start = box.offset;

  assert.equal(String.fromCharCode.apply(null,
    uint8.subarray(start + 4, start + 8)), 'emsg');
  assert.equal(uint8[start + 8], 0x01, 'emsg version is 1');
  assert.deepEqual(
    Array.from(uint8.subarray(start + 9, start + 12)),
    [0, 0, 0],
    'emsg flags are zero');

  let offset = start + 12;
  const timescale = readUint32(uint8, offset);

  offset += 4;
  const presentationTime = readUint64(uint8, offset);
  offset += 8;
  const eventDuration = readUint32(uint8, offset);
  offset += 4;
  const id = readUint32(uint8, offset);
  offset += 4;

  const scheme = readCString(uint8, offset);

  offset = scheme.next;
  const value = readCString(uint8, offset);
  offset = value.next;

  return {
    timescale: timescale,
    presentationTime: presentationTime,
    eventDuration: eventDuration,
    id: id,
    schemeIdUri: scheme.value,
    value: value.value,
    messageData: uint8.subarray(offset, start + box.size)
  };
}

// Walk the children of a container box.
function childBoxes(uint8, container) {
  const children = [];
  let offset = container.offset + 8;
  const end = container.offset + container.size;

  while (offset < end) {
    const size = readUint32(uint8, offset);
    const type = String.fromCharCode.apply(null,
      uint8.subarray(offset + 4, offset + 8));

    children.push({type: type, size: size, offset: offset});
    offset += size;
  }
  return children;
}

// Read the tfdt base media decode time and every trun data offset and
// sample size out of a moof box.
function readMoofInfo(uint8, moofBox) {
  const moofChildren = childBoxes(uint8, moofBox);
  const traf = moofChildren.find(function(box) {
    return box.type === 'traf';
  });

  assert.ok(traf, 'moof contains a traf box');
  const trafChildren = childBoxes(uint8, traf);
  const tfdt = trafChildren.find(function(box) {
    return box.type === 'tfdt';
  });
  const trun = trafChildren.find(function(box) {
    return box.type === 'trun';
  });

  assert.ok(tfdt && trun, 'traf contains tfdt and trun');
  assert.equal(uint8[tfdt.offset + 8], 0x01, 'tfdt is version 1 (64 bit)');

  const baseMediaDecodeTime = readUint64(uint8, tfdt.offset + 12);
  const trunPayload = trun.offset + 8;
  const sampleCount = readUint32(uint8, trunPayload + 4);
  const dataOffset = readUint32(uint8, trunPayload + 8);
  const sampleSizes = [];
  const hasCompositionOffset = (uint8[trunPayload + 2] & 0x08) !== 0;
  // trun entries start after the 12 byte trun full-box payload header
  let entry = trunPayload + 12;

  for (let i = 0; i < sampleCount; i++) {
    const duration = readUint32(uint8, entry);
    const size = readUint32(uint8, entry + 4);

    sampleSizes.push({duration: duration, size: size});
    entry += hasCompositionOffset ? 16 : 8;
  }

  // the first sample's composition offset is the 12th byte of its
  // 16-byte trun entry, i.e. after its duration and size fields
  const firstCompositionOffset = hasCompositionOffset ?
    readUint32(uint8, trunPayload + 12 + 8) : 0;

  // read every sample's composition offset so callers can pick the
  // video trun regardless of moof ordering
  const compositionOffsets = [];

  if (hasCompositionOffset) {
    for (let i = 0; i < sampleCount; i++) {
      compositionOffsets.push(readUint32(uint8, trunPayload + 12 + (i * 16) + 12));
    }
  }

  return {
    baseMediaDecodeTime: baseMediaDecodeTime,
    dataOffset: dataOffset,
    sampleCount: sampleCount,
    sampleSizes: sampleSizes,
    firstCompositionOffset: firstCompositionOffset,
    compositionOffsets: compositionOffsets
  };
}

function readFtypBrands(uint8) {
  const size = readUint32(uint8, 0);

  assert.equal(String.fromCharCode.apply(null,
    uint8.subarray(4, 8)), 'ftyp');
  const brands = [];

  // major brand at 8, minor version at 12, then compatible brands
  brands.push(String.fromCharCode.apply(null, uint8.subarray(8, 12)));
  for (let offset = 16; offset < size; offset += 4) {
    brands.push(String.fromCharCode.apply(null,
      uint8.subarray(offset, offset + 4)));
  }
  return brands;
}

// ---------------------------------------------------------------------------
// Transmuxer harness
// ---------------------------------------------------------------------------

function createHarness(options) {
  const transmuxer = new Transmuxer(options);
  const harness = {
    transmuxer: transmuxer,
    data: [],
    errors: [],
    logs: [],
    id3Frames: []
  };

  transmuxer.on('data', function(event) {
    harness.data.push(event);
  });
  transmuxer.on('error', function(error) {
    harness.errors.push(error);
  });
  transmuxer.on('log', function(log) {
    harness.logs.push(log);
  });
  transmuxer.on('id3Frame', function(frame) {
    harness.id3Frames.push(frame);
  });
  return harness;
}

function emsgBoxes(event) {
  return readTopLevelBoxes(event.data).filter(function(box) {
    return box.type === 'emsg';
  });
}

// ===========================================================================
// framing: MetadataStream tag delimiting in inband mode
// ===========================================================================

describe('framing', function() {
  it('delimits two concatenated tags from one PES and keeps the PES PTS',
    function() {
      const stream = new MetadataStream({inbandMetadata: true});
      const tags = [];

      stream.on('data', function(tag) {
        tags.push(tag);
      });

      const first = id3v24('TXXX', new Uint8Array([0x03, 0x00, 0x6b]));
      const second = id3v24('PRIV', new Uint8Array([1, 2, 3, 4, 5, 6]));
      const combined = new Uint8Array(first.length + second.length);

      combined.set(first, 0);
      combined.set(second, first.length);

      stream.push({
        type: 'timed-metadata',
        pts: 12345,
        dts: 12345,
        dataAlignmentIndicator: true,
        data: combined
      });

      assert.equal(tags.length, 2);
      assert.equal(tags[0].pts, 12345);
      assert.equal(tags[1].pts, 12345, 'the later tag inherits the same PES time');
      assert.deepEqual(Array.from(tags[0].data), Array.from(first));
      assert.deepEqual(Array.from(tags[1].data), Array.from(second));
    });

  it('reassembles a tag split across PES packets and keeps the first PES PTS',
    function() {
      const stream = new MetadataStream({inbandMetadata: true});
      const tags = [];

      stream.on('data', function(tag) {
        tags.push(tag);
      });

      const payload = new Uint8Array(40);

      for (let i = 0; i < payload.length; i++) {
        payload[i] = i;
      }
      const tag = id3v24('PRIV', payload);

      stream.push({
        type: 'timed-metadata',
        pts: 1000,
        dts: 1000,
        dataAlignmentIndicator: true,
        data: tag.subarray(0, 17)
      });
      stream.push({
        type: 'timed-metadata',
        pts: 2000,
        dts: 2000,
        dataAlignmentIndicator: false,
        data: tag.subarray(17)
      });

      assert.equal(tags.length, 1);
      assert.equal(tags[0].pts, 1000, 'later PES PTS must not override the tag');
      assert.deepEqual(Array.from(tags[0].data), Array.from(tag));
    });

  it('reassembles a tag when even the 10 byte header is split', function() {
    const stream = new MetadataStream({inbandMetadata: true});
    const tags = [];

    stream.on('data', function(tag) {
      tags.push(tag);
    });

    const tag = id3v24('PRIV', new Uint8Array([7, 7, 7]));

    stream.push({
      type: 'timed-metadata',
      pts: 500,
      dts: 500,
      dataAlignmentIndicator: true,
      data: tag.subarray(0, 4)
    });
    stream.push({
      type: 'timed-metadata',
      pts: 600,
      dts: 600,
      dataAlignmentIndicator: false,
      data: tag.subarray(4, 9)
    });
    stream.push({
      type: 'timed-metadata',
      pts: 700,
      dts: 700,
      dataAlignmentIndicator: false,
      data: tag.subarray(9)
    });

    assert.equal(tags.length, 1);
    assert.equal(tags[0].pts, 500);
    assert.deepEqual(Array.from(tags[0].data), Array.from(tag));
  });

  it('discards an incomplete fragment on alignment and logs, without bleeding bytes',
    function() {
      const stream = new MetadataStream({inbandMetadata: true});
      const tags = [];
      const logs = [];

      stream.on('data', function(tag) {
        tags.push(tag);
      });
      stream.on('log', function(log) {
        logs.push(log);
      });

      const incomplete = id3v24('PRIV', new Uint8Array([1, 1, 1, 1, 1, 1]));
      const fresh = id3v24('PRIV', new Uint8Array([2, 2, 2]));

      stream.push({
        type: 'timed-metadata',
        pts: 100,
        dts: 100,
        dataAlignmentIndicator: true,
        data: incomplete.subarray(0, 7)
      });
      assert.equal(tags.length, 0);

      stream.push({
        type: 'timed-metadata',
        pts: 900,
        dts: 900,
        dataAlignmentIndicator: true,
        data: fresh
      });

      assert.equal(tags.length, 1);
      assert.equal(tags[0].pts, 900);
      assert.deepEqual(Array.from(tags[0].data), Array.from(fresh));
      assert.equal(logs.length, 1, 'the discarded fragment is reported');
    });

  it('only accepts version 4, flags zero and syncsafe size headers', function() {
    const stream = new MetadataStream({inbandMetadata: true});
    const tags = [];
    const logs = [];

    stream.on('data', function(tag) {
      tags.push(tag);
    });
    stream.on('log', function(log) {
      logs.push(log);
    });

    const rejected = [
      id3Header(0x03, 0x00, [0, 0, 0, 0]), // wrong version
      id3Header(0x04, 0x80, [0, 0, 0, 0]), // unsynchronisation flag
      id3Header(0x04, 0x40, [0, 0, 0, 0]), // extended header flag
      id3Header(0x04, 0x10, [0, 0, 0, 0]), // footer flag
      id3Header(0x04, 0x00, [0x80, 0, 0, 0]) // non syncsafe size
    ];

    rejected.forEach(function(header, index) {
      stream.push({
        type: 'timed-metadata',
        pts: index,
        dts: index,
        dataAlignmentIndicator: true,
        data: header
      });
    });

    assert.equal(tags.length, 0);
    assert.equal(logs.length, rejected.length);

    const valid = id3v24('PRIV', new Uint8Array([5]));

    stream.push({
      type: 'timed-metadata',
      pts: 99,
      dts: 99,
      dataAlignmentIndicator: true,
      data: valid
    });
    assert.equal(tags.length, 1);
    assert.deepEqual(Array.from(tags[0].data), Array.from(valid));
  });

  it('keeps the complete original bytes including unknown frames', function() {
    const stream = new MetadataStream({inbandMetadata: true});
    const tags = [];

    stream.on('data', function(tag) {
      tags.push(tag);
    });

    const tag = id3v24('ZZZZ', new Uint8Array([0x0a, 0x0b, 0x0c, 0x0d]));

    stream.push({
      type: 'timed-metadata',
      pts: 333,
      dts: 333,
      dataAlignmentIndicator: true,
      data: tag
    });

    assert.equal(tags.length, 1);
    assert.deepEqual(Array.from(tags[0].data), Array.from(tag),
      'raw tag bytes are retained, not rebuilt from parsed frames');
    assert.deepEqual(Array.from(tags[0].data.subarray(0, 10)),
      [0x49, 0x44, 0x33, 0x04, 0x00, 0x00].concat(syncsafe(tag.length - 10)));
  });

  it('copies input bytes so reused buffers cannot rewrite emitted tags',
    function() {
      const stream = new MetadataStream({inbandMetadata: true});
      const tags = [];

      stream.on('data', function(tag) {
        tags.push(tag);
      });

      const tag = id3v24('PRIV', new Uint8Array([1, 2, 3, 4, 5]));
      const reusable = new Uint8Array(tag);

      stream.push({
        type: 'timed-metadata',
        pts: 100,
        dts: 100,
        dataAlignmentIndicator: true,
        data: reusable
      });
      reusable.fill(0xff);

      assert.deepEqual(Array.from(tags[0].data), Array.from(tag));
    });

  it('delimits tags spanning real 188 byte TS packets through the public Transmuxer',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const tag = id3v24('PRIV', new Uint8Array(200).fill(0x7f));
      // force the tag across two PES packets, with the split inside the
      // 10 byte ID3 header
      const ts = buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(tag, 90000, [
          {bytes: tag.subarray(0, 4), pts: 90000, alignment: true},
          {bytes: tag.subarray(4), pts: 91000, alignment: false}
        ])
      ]);

      harness.transmuxer.push(ts);
      harness.transmuxer.flush();

      assert.equal(harness.errors.length, 0);
      const boxes = emsgBoxes(harness.data[0]);

      assert.equal(boxes.length, 1);
      const emsg = readEmsgV1(harness.data[0].data, boxes[0]);

      assert.deepEqual(Array.from(emsg.messageData), Array.from(tag));
      assert.equal(emsg.presentationTime, 0);
    });
});

// ===========================================================================
// timeline: timestamp mapping onto the generated media axis
// ===========================================================================

describe('timeline', function() {
  it('places tags on the media timeline using the video DTS origin and base',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const atOrigin = id3v24('PRIV', new Uint8Array([1]));
      const atFirstPts = id3v24('PRIV', new Uint8Array([2]));
      const ts = buildTransportStream([
        // first frame carries a B-frame style PTS offset
        videoPart(90000, 93000, true),
        videoPart(93000, 96000, false),
        audioPart(90000),
        metaPart(atOrigin, 90000),
        metaPart(atFirstPts, 93000)
      ]);

      harness.transmuxer.push(ts);
      harness.transmuxer.flush();

      assert.equal(harness.errors.length, 0);
      const event = harness.data[0];
      const boxes = emsgBoxes(event);

      assert.equal(boxes.length, 2);
      const first = readEmsgV1(event.data, boxes[0]);
      const second = readEmsgV1(event.data, boxes[1]);

      assert.equal(first.timescale, 90000);
      assert.equal(first.schemeIdUri, 'https://aomedia.org/emsg/ID3');
      assert.equal(first.value, '');
      assert.equal(first.eventDuration, 0xffffffff);
      assert.equal(first.presentationTime, 0, 'base 0 + tag pts 90000 - origin dts 90000');
      assert.equal(first.id, 0);
      assert.equal(second.presentationTime, 3000);
      assert.equal(second.id, 1);
      assert.deepEqual(Array.from(first.messageData), Array.from(atOrigin));
      assert.deepEqual(Array.from(second.messageData), Array.from(atFirstPts));

      // the first video sample really does carry a composition offset,
      // proving the B-frame PTS shift is on the media axis. Audio is
      // unshifted, so select the trun that contains the offset.
      const moofs = readTopLevelBoxes(event.data).filter(function(box) {
        return box.type === 'moof';
      });
      const videoInfo = moofs.map(function(moof) {
        return readMoofInfo(event.data, moof);
      }).find(function(info) {
        return info.compositionOffsets.length > 0;
      });

      assert.ok(videoInfo, 'a video trun with composition offsets exists');
      assert.equal(videoInfo.compositionOffsets[0], 3000);
    });

  it('preserves values above 32 bits across a 33 bit timestamp rollover',
    function() {
      const harness = createHarness({
        inbandMetadata: true,
        keepOriginalTimestamps: true
      });
      const rolloverBoundary = Math.pow(2, 33);
      const highTag = id3v24('PRIV', new Uint8Array([9]));
      const lowTag = id3v24('PRIV', new Uint8Array([8]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(rolloverBoundary - 100000, rolloverBoundary - 100000, true),
        audioPart(rolloverBoundary - 100000),
        metaPart(highTag, rolloverBoundary - 100000)
      ]));
      harness.transmuxer.flush();

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 90000, true),
        audioPart(90000),
        metaPart(lowTag, 90000)
      ]));
      harness.transmuxer.flush();

      assert.equal(harness.errors.length, 0);
      assert.equal(harness.data.length, 2);
      const boxes = emsgBoxes(harness.data[1]);

      assert.equal(boxes.length, 1);
      const emsg = readEmsgV1(harness.data[1].data, boxes[0]);
      const expected = rolloverBoundary + 90000;

      assert.ok(expected > Math.pow(2, 32));
      assert.equal(emsg.presentationTime, expected);
      assert.ok(emsg.presentationTime <= Number.MAX_SAFE_INTEGER);
    });

  it('adds a non-zero base media decode time to every presentation time',
    function() {
      const base = 900000;
      const harness = createHarness({
        inbandMetadata: true,
        baseMediaDecodeTime: base
      });
      const tag = id3v24('PRIV', new Uint8Array([7]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        videoPart(93000, 96000, false),
        audioPart(90000),
        metaPart(tag, 93000)
      ]));
      harness.transmuxer.flush();

      const emsg = readEmsgV1(
        harness.data[0].data, emsgBoxes(harness.data[0])[0]);

      assert.equal(emsg.presentationTime, base + 3000);
    });

  it('supports a 44.1kHz audio-only segment without treating samples as 90kHz',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const tag = id3v24('PRIV', new Uint8Array([6]));

      harness.transmuxer.push(buildTransportStream([
        audioPart(90000),
        metaPart(tag, 90000)
      ], {kinds: ['audio', 'meta']}));
      harness.transmuxer.flush();

      assert.equal(harness.data[0].type, 'audio');
      const emsg = readEmsgV1(
        harness.data[0].data, emsgBoxes(harness.data[0])[0]);

      assert.equal(emsg.presentationTime, 0);

      const moof = readTopLevelBoxes(harness.data[0].data).find(function(box) {
        return box.type === 'moof';
      });
      const info = readMoofInfo(harness.data[0].data, moof);

      // the audio trun runs on the AAC sample clock: every sample is
      // 1024 samples long, never a 90kHz tick count
      assert.equal(info.sampleCount, 1);
      assert.equal(info.sampleSizes[0].duration, 1024);
      // the init segment mdhd timescale is the 44.1kHz sample rate,
      // proving the track sample clock is not the 90kHz emsg clock
      const initBoxes = readTopLevelBoxes(harness.data[0].initSegment);
      const moov = initBoxes.find(function(box) {
        return box.type === 'moov';
      });
      let timescaleFound = false;

      (function walk(container) {
        childBoxes(harness.data[0].initSegment, container)
          .forEach(function(child) {
            if (child.type === 'mdhd') {
              timescaleFound =
                readUint32(harness.data[0].initSegment, child.offset + 20) ===
                44100;
            } else if (['moov', 'trak', 'mdia'].indexOf(child.type) >= 0) {
              walk(child);
            }
          });
      }(moov));
      assert.ok(timescaleFound, 'an mdhd box declares the 44.1kHz timescale');
    });

  it('compares audio range endpoints exactly in the audio sample clock',
    function() {
      const harness = createHarness({inbandMetadata: true});
      // one 1024 sample AAC frame: 90kHz span rounds to 2090
      const inside = id3v24('PRIV', new Uint8Array([1]));
      const atEnd = id3v24('PRIV', new Uint8Array([2]));

      harness.transmuxer.push(buildTransportStream([
        audioPart(90000),
        // 2087 90kHz ticks maps to 1023 audio samples: inside
        metaPart(inside, 92087),
        // 2089 ticks maps to 1024 audio samples: at the right-open end
        metaPart(atEnd, 92089)
      ], {kinds: ['audio', 'meta']}));
      harness.transmuxer.flush();

      const boxes = emsgBoxes(harness.data[0]);

      assert.equal(boxes.length, 1, 'the right-end tag is not emitted');
      const emsg = readEmsgV1(harness.data[0].data, boxes[0]);

      assert.equal(emsg.presentationTime, 2087);
      assert.equal(
        harness.transmuxer.transmuxPipeline_.coalesceStream
          .pendingInbandTags.length,
        1,
        'the endpoint tag is retained for a later segment');
    });

  it('prepends emsg boxes without altering moof, trun offsets or sample bytes',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const audioPayload = new Uint8Array([0xaa, 0xbb, 0xcc, 0xdd]);
      const tag = id3v24('PRIV', new Uint8Array([1]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000, audioPayload),
        metaPart(tag, 90000)
      ]));
      harness.transmuxer.flush();

      const event = harness.data[0];
      const boxes = readTopLevelBoxes(event.data);
      const firstMoofIndex = boxes.findIndex(function(box) {
        return box.type === 'moof';
      });

      assert.ok(firstMoofIndex > 0, 'emsg boxes precede the first moof');
      boxes.slice(0, firstMoofIndex).forEach(function(box) {
        assert.equal(box.type, 'emsg');
      });

      // every trun data_offset still resolves, relative to its moof, to
      // the payload of the following mdat (no emsg length added)
      for (let i = firstMoofIndex; i < boxes.length; i++) {
        if (boxes[i].type !== 'moof') {
          continue;
        }
        const info = readMoofInfo(event.data, boxes[i]);
        const mdat = boxes[i + 1];

        assert.equal(mdat.type, 'mdat');
        assert.equal(
          boxes[i].offset + info.dataOffset,
          mdat.offset + 8,
          'trun data_offset is relative to the moof, not the emsg prefix');

        // sample sizes declared in trun match the actual mdat payload
        const payloadEnd = mdat.offset + 8 +
          info.sampleSizes.reduce(function(total, sample) {
            return total + sample.size;
          }, 0);

        assert.equal(payloadEnd, mdat.offset + mdat.size);
      }

      // the audio sample payload itself is byte for byte intact inside
      // whichever moof/mdat pair carries it (audio boxes are unshifted
      // ahead of video boxes by the coalescer)
      const moofList = boxes.filter(function(box) {
        return box.type === 'moof';
      });
      let foundAudioBytes = false;

      for (let i = 0; i < moofList.length; i++) {
        const info = readMoofInfo(event.data, moofList[i]);
        const sampleStart = moofList[i].offset + info.dataOffset;
        const candidate = event.data.subarray(
          sampleStart, sampleStart + audioPayload.length);

        if (candidate.length === audioPayload.length &&
            Array.from(candidate).every(function(byte, index) {
              return byte === audioPayload[index];
            })) {
          foundAudioBytes = true;
        }
      }
      assert.ok(foundAudioBytes, 'the original AAC sample bytes survive');
    });

  it('adds the aid3 compatible brand to the init segment ftyp', function() {
    const harness = createHarness({inbandMetadata: true});

    harness.transmuxer.push(buildTransportStream([
      videoPart(90000, 93000, true),
      audioPart(90000),
      metaPart(id3v24('PRIV', new Uint8Array([1])), 90000)
    ]));
    harness.transmuxer.flush();

    assert.ok(readFtypBrands(harness.data[0].initSegment).includes('aid3'));
  });
});

// ===========================================================================
// placement: segment ownership, ids, state clearing and compatibility
// ===========================================================================

describe('placement', function() {
  it('assigns future tags to later flushes and excludes the right endpoint',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const firstTag = id3v24('PRIV', new Uint8Array([1]));
      const edgeTag = id3v24('PRIV', new Uint8Array([2]));
      const futureTag = id3v24('PRIV', new Uint8Array([3]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        videoPart(93000, 96000, false),
        audioPart(90000),
        metaPart(firstTag, 93000), // pt 3000, inside flush one
        metaPart(edgeTag, 99000), // pt 9000, at the flush one video end
        metaPart(futureTag, 104000) // pt 14000, beyond flush one
      ]));
      harness.transmuxer.flush();

      let boxes = emsgBoxes(harness.data[0]);

      assert.equal(boxes.length, 1);
      assert.equal(
        readEmsgV1(harness.data[0].data, boxes[0]).presentationTime, 3000);

      harness.transmuxer.push(buildTransportStream([
        videoPart(99000, 102000, true),
        videoPart(102000, 105000, false),
        audioPart(99000)
      ]));
      harness.transmuxer.flush();

      boxes = emsgBoxes(harness.data[1]);
      assert.equal(boxes.length, 2, 'both retained tags surface in flush two');
      const edge = readEmsgV1(harness.data[1].data, boxes[0]);
      const future = readEmsgV1(harness.data[1].data, boxes[1]);

      assert.equal(edge.presentationTime, 9000);
      assert.equal(future.presentationTime, 14000);
      assert.deepEqual(Array.from(future.messageData), Array.from(futureTag));
      // ids are consumed only as boxes are actually written
      assert.equal(edge.id, 1);
      assert.equal(future.id, 2);
    });

  it('drops tags that arrive before the segment range and logs them',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const lateTag = id3v24('PRIV', new Uint8Array([4]));
      const inRangeTag = id3v24('PRIV', new Uint8Array([5]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        videoPart(93000, 96000, false),
        audioPart(90000),
        metaPart(lateTag, 89000), // maps to media time -1000
        metaPart(inRangeTag, 93000)
      ]));
      harness.transmuxer.flush();

      const boxes = emsgBoxes(harness.data[0]);

      assert.equal(boxes.length, 1);
      assert.equal(
        readEmsgV1(harness.data[0].data, boxes[0]).presentationTime, 3000);
      assert.ok(harness.logs.some(function(log) {
        return /before the segment range/.test(log.message);
      }));
    });

  it('keeps duplicate tags with identical content and PTS, and wraps ids at 32 bits',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const duplicate = id3v24('PRIV', new Uint8Array([0x42]));

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(duplicate, 90000),
        metaPart(new Uint8Array(duplicate), 90000)
      ]));
      harness.transmuxer.transmuxPipeline_.coalesceStream.emsgId = 0xffffffff;
      harness.transmuxer.flush();

      const boxes = emsgBoxes(harness.data[0]);

      assert.equal(boxes.length, 2, 'both duplicates are retained');
      assert.equal(readEmsgV1(harness.data[0].data, boxes[0]).id, 0xffffffff);
      assert.equal(readEmsgV1(harness.data[0].data, boxes[1]).id, 0,
        'event id wraps at 32 bits');
    });

  it('does not emit media or consume tags on a metadata-only flush',
    function() {
      const harness = createHarness({inbandMetadata: true});
      const state = continuityState();
      const tag = id3v24('PRIV', new Uint8Array([1]));
      const packets = [
        patPacket(),
        pmtPacket(state, ['video', 'audio', 'meta'])
      ].concat(pesTsPackets(PID_METADATA, 0x06, tag, {
        pts: 90000,
        dts: 90000,
        alignment: true
      }, state));
      const metadataOnly = new Uint8Array(packets.length * TS_PACKET_LENGTH);

      packets.forEach(function(packet, index) {
        metadataOnly.set(packet, index * TS_PACKET_LENGTH);
      });

      harness.transmuxer.push(metadataOnly);
      harness.transmuxer.flush();
      assert.equal(harness.data.length, 0, 'no empty media segment is created');

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000)
      ]));
      harness.transmuxer.flush();

      assert.equal(harness.data.length, 1);
      assert.equal(emsgBoxes(harness.data[0]).length, 1,
        'the retained tag is consumed by the next real media segment');
    });

  it('clears fragments and unsent tags on reset while ids keep increasing',
    function() {
      const harness = createHarness({inbandMetadata: true});

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(id3v24('PRIV', new Uint8Array([1])), 90000),
        metaPart(id3v24('PRIV', new Uint8Array([2])), 90000)
      ]));
      harness.transmuxer.flush();
      assert.equal(emsgBoxes(harness.data[0]).length, 2);

      const metadataStream =
        harness.transmuxer.transmuxPipeline_.metadataStream;
      const partial = id3v24('PRIV', new Uint8Array([1, 1, 1, 1]));

      metadataStream.push({
        type: 'timed-metadata',
        pts: 1,
        dts: 1,
        dataAlignmentIndicator: true,
        data: partial.subarray(0, 5)
      });
      harness.transmuxer.transmuxPipeline_.coalesceStream.push({
        data: id3v24('PRIV', new Uint8Array([8])),
        frames: [],
        pts: 2,
        dts: 2
      });

      harness.transmuxer.reset();

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(id3v24('PRIV', new Uint8Array([3])), 90000),
        metaPart(id3v24('PRIV', new Uint8Array([4])), 90000)
      ]));
      harness.transmuxer.flush();

      const boxes = emsgBoxes(harness.data[1]);

      assert.equal(boxes.length, 2, 'old queued tags were cleared');
      assert.equal(readEmsgV1(harness.data[1].data, boxes[0]).id, 2);
      assert.equal(readEmsgV1(harness.data[1].data, boxes[1]).id, 3);
    });

  it('clears fragments and unsent tags when the base media decode time is set',
    function() {
      const harness = createHarness({inbandMetadata: true});

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(id3v24('PRIV', new Uint8Array([1])), 90000)
      ]));
      harness.transmuxer.flush();

      const metadataStream =
        harness.transmuxer.transmuxPipeline_.metadataStream;
      const partial = id3v24('PRIV', new Uint8Array([9, 9, 9, 9]));

      metadataStream.push({
        type: 'timed-metadata',
        pts: 1,
        dts: 1,
        dataAlignmentIndicator: true,
        data: partial.subarray(0, 6)
      });
      harness.transmuxer.transmuxPipeline_.coalesceStream.push({
        data: id3v24('PRIV', new Uint8Array([8])),
        frames: [],
        pts: 2,
        dts: 2
      });

      harness.transmuxer.setBaseMediaDecodeTime(180000);

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(id3v24('PRIV', new Uint8Array([5])), 90000)
      ]));
      harness.transmuxer.flush();

      const latest = harness.data[harness.data.length - 1];

      assert.equal(emsgBoxes(latest).length, 1, 'only the new tag is emitted');
    });

  it('leaves default mode output unchanged: no emsg, no aid3, id3Frame events intact',
    function() {
      const tag = id3v24('PRIV', new Uint8Array([1]));
      const ts = buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(tag, 93000)
      ]);

      const defaultHarness = createHarness();

      defaultHarness.transmuxer.push(ts);
      defaultHarness.transmuxer.flush();

      const inbandHarness = createHarness({inbandMetadata: true});

      inbandHarness.transmuxer.push(ts);
      inbandHarness.transmuxer.flush();

      assert.equal(emsgBoxes(defaultHarness.data[0]).length, 0);
      assert.ok(!readFtypBrands(
        defaultHarness.data[0].initSegment).includes('aid3'));
      assert.equal(defaultHarness.id3Frames.length, 1,
        'the legacy id3Frame event still fires');
      assert.equal(defaultHarness.data[0].metadata.length, 1);
      // legacy cueTime is relative to the presentation start PTS, not
      // the DTS origin that emsg uses
      assert.equal(defaultHarness.data[0].metadata[0].cueTime, 0);
      assert.equal(defaultHarness.data[0].metadata[0].pts, 93000);

      // moof/mdat bytes are identical once the emsg prefix is removed
      const inbandBoxes = readTopLevelBoxes(inbandHarness.data[0].data);
      const firstMoof = inbandBoxes.find(function(box) {
        return box.type === 'moof';
      });
      const defaultHex = Buffer.from(
        defaultHarness.data[0].data.buffer,
        defaultHarness.data[0].data.byteOffset,
        defaultHarness.data[0].data.byteLength).toString('hex');
      const inbandMediaHex = Buffer.from(
        inbandHarness.data[0].data.buffer,
        inbandHarness.data[0].data.byteOffset + firstMoof.offset,
        inbandHarness.data[0].data.byteLength - firstMoof.offset
      ).toString('hex');

      assert.equal(inbandMediaHex, defaultHex);
    });

  it('rejects raw AAC input through error and emits no media data', function() {
    const aacBytes = new Uint8Array(fs.readFileSync(
      require('path').join(__dirname, 'segments', 'test-aac-segment.aac')));
    const harness = createHarness({inbandMetadata: true});

    harness.transmuxer.push(aacBytes);
    harness.transmuxer.flush();

    assert.equal(harness.errors.length, 1);
    assert.equal(harness.data.length, 0);

    // a reset allows legal transport stream input again
    harness.transmuxer.reset();
    harness.transmuxer.push(buildTransportStream([
      videoPart(90000, 93000, true),
      audioPart(90000)
    ]));
    harness.transmuxer.flush();
    assert.equal(harness.errors.length, 1, 'no new errors after recovery');
    assert.equal(harness.data.length, 1);
  });

  it('rejects remux false through error and emits no media data', function() {
    const harness = createHarness({inbandMetadata: true, remux: false});

    harness.transmuxer.push(buildTransportStream([
      videoPart(90000, 93000, true),
      audioPart(90000)
    ]));
    harness.transmuxer.flush();

    assert.equal(harness.errors.length, 1);
    assert.equal(harness.data.length, 0);
  });

  it('ends a flush with an error on an illegal mapping and recovers after reset',
    function() {
      const harness = createHarness({inbandMetadata: true});

      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000)
      ]));
      harness.transmuxer.transmuxPipeline_.coalesceStream
        .pendingInbandTags.push({
          data: id3v24('PRIV', new Uint8Array([1])),
          frames: [],
          pts: undefined,
          dts: undefined
        });
      harness.transmuxer.flush();

      assert.equal(harness.errors.length, 1);
      assert.equal(harness.data.length, 0, 'no partial segment is emitted');

      harness.transmuxer.reset();
      harness.transmuxer.push(buildTransportStream([
        videoPart(90000, 93000, true),
        audioPart(90000),
        metaPart(id3v24('PRIV', new Uint8Array([2])), 90000)
      ]));
      harness.transmuxer.flush();

      assert.equal(harness.data.length, 1);
      assert.equal(emsgBoxes(harness.data[0]).length, 1);
    });
});
