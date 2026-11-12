// Generates harness/workspace_streams.json: small, hand-auditable Vorbis
// streams that exercise the decoder's reusable workspaces. All fields are
// written LSB-first, as required by the Vorbis setup header.
//
// The streams share two codebooks, both with one-bit codes:
//   book 0: scalar, 2 entries, no values (entry 0 = bit 0, entry 1 = bit 1)
//   book 1: dimension 2, 2 entries, lookup type 1, entry values [0 0]/[1 1]
// and use 64-sample short and 256-sample long blocks.
//
// Streams:
//   sharedFloor - two channels, one submap, one floor1 shared by both
//                 channels, residue type 0, short and long blocks
//   twoSubmaps  - two channels on separate submaps: floor0 + residue type 1
//                 and floor1 + residue type 2
//   coupling    - two channels, one coupling step, residue type 1, short and
//                 long blocks
//
// Every stream contains eight packets. The two-mode streams alternate short
// and long blocks, and floor presence cycles through
// both/left-only/right-only/neither. Packets are written with every bit the
// decoder reads, so decoding never reaches end of packet. Rebuild with:
//   node harness/make_workspace_fixtures.cjs
const fs = require('node:fs');

class Bits {
  constructor() { this.bits = []; }
  put(value, count) { for (let i = 0; i < count; ++i) this.bits.push((value >>> i) & 1); }
  bytes() {
    const out = Buffer.alloc(Math.ceil(this.bits.length / 8));
    this.bits.forEach((b, i) => { out[i >>> 3] |= b << (i & 7); });
    return out;
  }
}

const head = (type, data) => Buffer.concat([Buffer.from([type]), Buffer.from('vorbis'), data]);

function writeCodebooks(b) {
  const w = b.put.bind(b);
  w(1, 8); // two codebooks (count is stored minus one)
  // book 0: scalar, two entries, one-bit codes, no lookup table
  w(0x564342, 24); w(1, 16); w(2, 24); w(0, 1); w(0, 1);
  w(0, 5); w(0, 5); w(0, 4);
  // book 1: dimension 2, two entries, one-bit codes, lookup type 1,
  // values [0 0] and [1 1]
  w(0x564342, 24); w(2, 16); w(2, 24); w(0, 1); w(0, 1);
  w(0, 5); w(0, 5); w(1, 4);
  w(0, 32); w(0x62800001, 32); w(0, 4); w(0, 1); w(1, 1);
}

function writeFloor1(b) {
  const w = b.put.bind(b);
  w(1, 16); // floor type 1
  w(1, 5);  // one partition class
  w(0, 4);  // class 0
  w(0, 3);  // dimension 1
  w(0, 2);  // no subclasses
  w(1, 8);  // value codebook 0 (stored plus one)
  w(0, 2);  // multiplier 1
  w(5, 4);  // range bits -> x coordinates in [0,32]
  w(16, 5); // single extra x coordinate
}

function writeFloor0(b) {
  const w = b.put.bind(b);
  w(0, 16);   // floor type 0
  w(2, 8);    // order
  w(44100 & 0xffff, 16);
  w(16, 16);  // bark map size
  w(6, 6);    // amplitude bits
  w(0, 8);    // amplitude offset
  w(0, 4);    // one book
  w(1, 8);    // codebook 1
}

function writeResidue(b, type, end) {
  const w = b.put.bind(b);
  w(type, 16);
  w(0, 24);    // begin
  // end is the largest residue vector this stream can produce (the long
  // block's spectrum, times the channel count for an interleaved type 2).
  // Per-packet decoding clamps it to the current block's actual size.
  w(end, 24);
  w(31, 24);   // partition size 32
  w(0, 6);     // one classification
  w(0, 8);     // classbook 0
  w(1, 3);     // cascade: pass 0 present
  w(0, 1);     // no high bits
  w(1, 8);     // vector codebook 1
}

function identification(channels) {
  const identBody = Buffer.alloc(23);
  identBody[4] = channels;
  identBody.writeUInt32LE(44100, 5);
  identBody[21] = 0x86; // 64-sample short, 256-sample long blocks
  identBody[22] = 1;
  return head(1, identBody);
}
const comment = head(3, Buffer.from([0, 0, 0, 0, 0, 0, 0, 0, 1]));

function setup(writeBody) {
  const b = new Bits();
  writeCodebooks(b);
  b.put(0, 6); b.put(0, 16); // one time domain transform, type 0
  writeBody(b);
  return head(5, b.bytes());
}

// floor presence pattern shared by every stream
const presence = [[1, 1], [1, 0], [0, 1], [1, 1], [0, 0], [1, 1], [1, 0], [0, 1]];
// block sizes: short, long alternating; streams with one mode use short only
const longBlock = [false, true, false, true, false, true, false, true];
const SHORT_N = 32;
const LONG_N = 128;

function encodePacket(cfg, packetIndex) {
  const b = new Bits();
  const w = b.put.bind(b);
  const present = presence[packetIndex];
  const isLong = cfg.twoModes && longBlock[packetIndex];
  const n = isLong ? LONG_N : SHORT_N;

  w(0, 1); // audio packet
  if (cfg.twoModes) w(isLong ? 1 : 0, 1);
  if (isLong) {
    // window flags describe the actual neighbors in the packet sequence
    const prevLong = packetIndex > 0 && longBlock[packetIndex - 1];
    const nextLong = packetIndex + 1 < presence.length && longBlock[packetIndex + 1];
    w(prevLong ? 1 : 0, 1);
    w(nextLong ? 1 : 0, 1);
  }

  // floors, one per channel
  cfg.floors.forEach((floorType, ch) => {
    if (floorType === 1) {
      w(present[ch], 1);
      if (present[ch]) {
        w(200, 8); w(200, 8); // y0, y1
        w(0, 1);              // floor1 class value, book 0 entry 0
      }
    } else if (present[ch]) {
      w(40, 6); // nonzero amplitude
      w(0, 1);  // floor book list index 0
      w(0, 1);  // coefficient vector, book 1 entry 0
    } else {
      w(0, 6);  // amplitude zero -> unused floor
    }
  });

  // residues, one per submap
  let counter = packetIndex * 7 + 1;
  const vectorBit = () => { const v = counter & 1; counter++; return v; };
  cfg.submaps.forEach((sub, subIndex) => {
    const members = [];
    cfg.mux.forEach((m, ch) => { if (m === subIndex) members.push(ch); });
    let active = members.map((ch) => present[ch] === 1);
    if (cfg.coupling) {
      // coupling makes both channels participate whenever either floor is on
      const any = active.some(Boolean);
      active = active.map(() => any);
    }
    if (!active.some(Boolean)) return;
    const combinedN = sub.type === 2 ? n * members.length : n;
    const partitions = combinedN / 32;
    const vectorsPerPartition = 32 / 2; // book 1 has dimension 2
    for (let p = 0; p < partitions; ++p) {
      if (sub.type === 2) {
        // one classification for the whole combined vector
        w(0, 1);
        for (let k = 0; k < vectorsPerPartition; ++k) w(vectorBit(), 1);
      } else {
        active.forEach((on) => { if (on) w(0, 1); });
        active.forEach((on) => {
          if (!on) return;
          for (let k = 0; k < vectorsPerPartition; ++k) w(vectorBit(), 1);
        });
      }
    }
  });
  return b.bytes();
}

function buildStream(name, cfg) {
  const headers = [identification(2), comment, cfg.setup];
  const packets = presence.map((_, i) => encodePacket(cfg, i));
  return {
    name,
    channels: 2,
    blocksize: [64, 256],
    headers: headers.map((x) => x.toString('base64')),
    packets: packets.map((x) => x.toString('base64')),
  };
}

const streams = [
  buildStream('sharedFloor', {
    twoModes: true,
    floors: [1, 1],
    mux: [0, 0],
    coupling: false,
    submaps: [{ floor: 0, type: 0 }],
    setup: setup((b) => {
      const w = b.put.bind(b);
      w(0, 6); writeFloor1(b);          // one floor, shared by both channels
      w(0, 6); writeResidue(b, 0, 128);  // one residue, type 0; long-block spectrum is 128
      w(0, 6);                          // one mapping
      w(0, 16); w(0, 1);                // type 0, one submap
      w(0, 1);                          // no coupling
      w(0, 2);                          // reserved
      w(0, 8); w(0, 8); w(0, 8);        // submap: floor 0, residue 0
      w(1, 6);                          // two modes
      w(0, 1); w(0, 16); w(0, 16); w(0, 8); // short block, mapping 0
      w(1, 1); w(0, 16); w(0, 16); w(0, 8); // long block, mapping 0
      w(1, 1);                          // framing
    }),
  }),
  buildStream('twoSubmaps', {
    twoModes: false,
    floors: [0, 1],
    mux: [0, 1],
    coupling: false,
    submaps: [{ floor: 0, type: 1 }, { floor: 1, type: 2 }],
    setup: setup((b) => {
      const w = b.put.bind(b);
      w(1, 6); writeFloor0(b); writeFloor1(b);
      w(1, 6); writeResidue(b, 1, 32); writeResidue(b, 2, 32); // short-only: per-channel (r1) and one-channel combined (r2) both span 32
      w(0, 6);                          // one mapping
      w(0, 16); w(1, 1); w(1, 4);       // type 0, two submaps
      w(0, 1);                          // no coupling
      w(0, 2);                          // reserved
      w(0, 4); w(1, 4);                 // channel mux
      w(0, 8); w(0, 8); w(0, 8);        // submap 0: floor 0, residue 0
      w(0, 8); w(1, 8); w(1, 8);        // submap 1: floor 1, residue 1
      w(0, 6);                          // one mode
      w(0, 1); w(0, 16); w(0, 16); w(0, 8);
      w(1, 1);                          // framing
    }),
  }),
  buildStream('coupling', {
    twoModes: true,
    floors: [1, 1],
    mux: [0, 0],
    coupling: true,
    submaps: [{ floor: 0, type: 1 }],
    setup: setup((b) => {
      const w = b.put.bind(b);
      w(1, 6); writeFloor1(b); writeFloor1(b);
      w(0, 6); writeResidue(b, 1, 128);  // one residue, type 1; long-block spectrum is 128
      w(0, 6);                          // one mapping
      w(0, 16); w(0, 1);                // type 0, one submap
      w(1, 1); w(0, 8);                 // one coupling step (stored minus one)
      w(0, 1); w(1, 1);                 // magnitude channel 0, angle channel 1
      w(0, 2);                          // reserved
      w(0, 8); w(0, 8); w(0, 8);        // submap: floor 0, residue 0
      w(1, 6);                          // two modes
      w(0, 1); w(0, 16); w(0, 16); w(0, 8);
      w(1, 1); w(0, 16); w(0, 16); w(0, 8);
      w(1, 1);                          // framing
    }),
  }),
];

fs.writeFileSync('harness/workspace_streams.json', JSON.stringify({ streams }, null, 2) + '\n');
