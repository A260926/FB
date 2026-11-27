var MAX_UINT32 = Math.pow(2, 32);

var getUint64 = function(uint8) {
  var dv = new DataView(uint8.buffer, uint8.byteOffset, uint8.byteLength);
  var value;

  if (dv.getBigUint64) {
    value = dv.getBigUint64(0);

    if (value < Number.MAX_SAFE_INTEGER) {
      return Number(value);
    }

    return value;
  }

  return (dv.getUint32(0) * MAX_UINT32) + dv.getUint32(4);
};

/**
 * Write a non-negative integer no larger than Number.MAX_SAFE_INTEGER as
 * an unsigned 64-bit big-endian value starting at `offset`.
 * Bitwise operators are avoided because they truncate to 32 bits.
 */
var setUint64 = function(uint8, offset, value) {
  var high = Math.floor(value / MAX_UINT32);
  var low = value - (high * MAX_UINT32);

  uint8[offset] = Math.floor(high / 16777216) & 0xFF;
  uint8[offset + 1] = Math.floor(high / 65536) & 0xFF;
  uint8[offset + 2] = Math.floor(high / 256) & 0xFF;
  uint8[offset + 3] = high & 0xFF;
  uint8[offset + 4] = Math.floor(low / 16777216) & 0xFF;
  uint8[offset + 5] = Math.floor(low / 65536) & 0xFF;
  uint8[offset + 6] = Math.floor(low / 256) & 0xFF;
  uint8[offset + 7] = low & 0xFF;
};

module.exports = {
  getUint64: getUint64,
  setUint64: setUint64,
  MAX_UINT32: MAX_UINT32
};
