const fs = require('node:fs');
class Bits {
  constructor() { this.bits = []; }
  put(value, count) { for (let i = 0; i < count; ++i) this.bits.push((value >>> i) & 1); }
  bytes() { const out = Buffer.alloc(Math.ceil(this.bits.length / 8)); this.bits.forEach((b, i) => { out[i >>> 3] |= b << (i & 7); }); return out; }
}
const head = (type, data) => Buffer.concat([Buffer.from([type]), Buffer.from('vorbis'), data]);
const identBody = Buffer.alloc(23); identBody[4] = 2; identBody.writeUInt32LE(44100, 5); identBody[21] = 0x66; identBody[22] = 1;
const ident = head(1, identBody);
const comment = head(3, Buffer.from([0, 0, 0, 0, 0, 0, 0, 0, 1]));
const s = new Bits(); const w = s.put.bind(s); w(1, 8);
for (const lookup of [0, 1]) {
  w(0x564342,24);w(1,16);w(2,24);w(0,1);w(0,1);w(0,5);w(0,5);w(lookup,4);
  if (lookup) { w(0,32);w(0x62800001,32);w(0,4);w(0,1);w(1,1);w(1,1); }
}
w(0,6);w(0,16);
w(0,6);w(1,16);w(1,5);w(0,4);w(0,3);w(0,2);w(0,8);w(0,2);w(5,4);w(16,5);
w(0,6);w(2,16);w(0,24);w(64,24);w(63,24);w(0,6);w(0,8);w(1,3);w(0,1);w(1,8);
w(0,6);w(0,16);w(0,1);w(0,1);w(0,2);w(0,8);w(0,8);w(0,8);
w(0,6);w(0,1);w(0,16);w(0,16);w(0,8);w(1,1);
const setup = head(5, s.bytes());
function audio(left, right) {
  const b = new Bits(); b.put(0, 1);
  for (const present of [left, right]) { b.put(present, 1); if (present) { b.put(255, 8); b.put(255, 8); } }
  b.put(0, 1); for (let i = 0; i < 64; ++i) b.put(0, 1); return b.bytes();
}
const fixtures = {headers:[ident,comment,setup].map(b=>b.toString('base64')),left:audio(1,0).toString('base64'),right:audio(0,1).toString('base64')};
fs.writeFileSync('harness/residue2.json', JSON.stringify(fixtures, null, 2) + '\n');
function page(packet, seq, flags, granule) {
  const laces = Buffer.alloc(Math.floor(packet.length / 255) + 1, 255); laces[laces.length - 1] = packet.length % 255;
  const header = Buffer.alloc(27); header.write('OggS'); header[5] = flags; header.writeBigUInt64LE(BigInt(granule), 6); header.writeUInt32LE(7, 14); header.writeUInt32LE(seq, 18); header[26] = laces.length;
  const out = Buffer.concat([header, laces, packet]); let crc = 0;
  for (const byte of out) { crc = (crc ^ (byte << 24)) >>> 0; for (let i = 0; i < 8; ++i) crc = ((crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0)) >>> 0; }
  out.writeUInt32LE(crc, 22); return out;
}
for (const side of ['left', 'right']) {
  const packet = Buffer.from(fixtures[side], 'base64'); const packets = [ident, comment, setup, packet, packet];
  fs.writeFileSync('harness/' + side + '.ogg', Buffer.concat(packets.map((p, i) => page(p, i, i === 0 ? 2 : i === 4 ? 4 : 0, i === 4 ? 32 : 0))));
}
