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
 * Write a non-negative integer up to Number.MAX_SAFE_INTEGER into a
 * DataView as a big-endian uint64. Division is used instead of bitwise
 * operators so values above 2^32 keep their high bits (bitwise ops are
 * limited to 32 bits).
 * @param {DataView} view the view to write into
 * @param {number} offset byte offset of the uint64 field
 * @param {number} value non-negative integer, at most MAX_SAFE_INTEGER
 */
var setUint64 = function(view, offset, value) {
  var high = Math.floor(value / MAX_UINT32);
  var low = value % MAX_UINT32;

  view.setUint32(offset, high);
  view.setUint32(offset + 4, low);
};

/**
 * Write a non-negative integer into a Uint8Array as a big-endian
 * uint32. Uses division instead of bitwise shifts so callers handling
 * >32 bit values cannot truncate them by accident.
 * @param {Uint8Array} bytes the array to write into
 * @param {number} offset byte offset of the uint32 field
 * @param {number} value non-negative integer
 */
var setUint32 = function(bytes, offset, value) {
  bytes[offset] = Math.floor(value / 0x1000000) & 0xFF;
  bytes[offset + 1] = Math.floor(value / 0x10000) & 0xFF;
  bytes[offset + 2] = Math.floor(value / 0x100) & 0xFF;
  bytes[offset + 3] = value & 0xFF;
};

module.exports = {
  getUint64: getUint64,
  setUint64: setUint64,
  setUint32: setUint32,
  MAX_UINT32: MAX_UINT32
};
