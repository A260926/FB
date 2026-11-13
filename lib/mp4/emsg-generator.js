/**
 * mux.js
 *
 * Copyright (c) Brightcove
 * Licensed Apache-2.0 https://github.com/videojs/mux.js/blob/master/LICENSE
 *
 * Builds version 1 Event Message (emsg) boxes, ISO/IEC 23009-1 5.10.3.3.
 * See https://aomediacodec.github.io/id3-emsg/ for the ID3 scheme used
 * when carrying complete ID3 tags in-band.
 */
'use strict';

var numbers = require('../utils/numbers');

var BOX_HEADER_SIZE = 8;
var FULL_BOX_HEADER_SIZE = 4;
var UINT32_SIZE = 4;
var UINT64_SIZE = 8;

/**
 * Encode a string as UTF-8 bytes followed by a NUL terminator.
 * @param {string} value the string to encode
 * @returns {Uint8Array} NUL terminated UTF-8 bytes
 */
var cString = function(value) {
  var percentEncoded = encodeURIComponent(value);
  var bytes = [];
  var i;

  for (i = 0; i < percentEncoded.length; i++) {
    if (percentEncoded[i] === '%') {
      bytes.push(parseInt(percentEncoded.slice(i + 1, i + 3), 16));
      i += 2;
    } else {
      bytes.push(percentEncoded.charCodeAt(i));
    }
  }

  bytes.push(0);
  return new Uint8Array(bytes);
};

/**
 * Generate a version 1 emsg box.
 * @param {object} options
 * @param {string} options.schemeIdUri scheme_id_uri field
 * @param {string} options.value value field
 * @param {number} options.timescale timescale field
 * @param {number} options.presentationTime presentation_time field, a
 *        non-negative integer up to Number.MAX_SAFE_INTEGER
 * @param {number} options.eventDuration event_duration field
 * @param {number} options.id event id field (32 bit)
 * @param {Uint8Array} options.messageData complete message payload
 * @returns {Uint8Array} the assembled emsg box
 */
var emsg = function(options) {
  var scheme = cString(options.schemeIdUri);
  var value = cString(options.value);
  var payloadSize = FULL_BOX_HEADER_SIZE +
    UINT32_SIZE + // timescale
    UINT64_SIZE + // presentation_time
    UINT32_SIZE + // event_duration
    UINT32_SIZE + // id
    scheme.byteLength +
    value.byteLength +
    options.messageData.byteLength;
  var result = new Uint8Array(BOX_HEADER_SIZE + payloadSize);
  var view = new DataView(result.buffer, result.byteOffset, result.byteLength);
  var offset = BOX_HEADER_SIZE;

  // box size and type
  view.setUint32(0, result.byteLength);
  result.set([0x65, 0x6d, 0x73, 0x67], 4); // 'emsg'

  // full box header: version 1, flags 0
  result[offset] = 0x01;
  result[offset + 1] = 0x00;
  result[offset + 2] = 0x00;
  result[offset + 3] = 0x00;
  offset += FULL_BOX_HEADER_SIZE;

  view.setUint32(offset, options.timescale);
  offset += UINT32_SIZE;

  // arithmetic rather than bit shifts so >32 bit values survive
  numbers.setUint64(view, offset, options.presentationTime);
  offset += UINT64_SIZE;

  numbers.setUint32(result, offset, options.eventDuration);
  offset += UINT32_SIZE;

  numbers.setUint32(result, offset, options.id);
  offset += UINT32_SIZE;

  result.set(scheme, offset);
  offset += scheme.byteLength;

  result.set(value, offset);
  offset += value.byteLength;

  result.set(options.messageData, offset);

  return result;
};

module.exports = emsg;
