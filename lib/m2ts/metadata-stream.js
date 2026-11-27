/**
 * mux.js
 *
 * Copyright (c) Brightcove
 * Licensed Apache-2.0 https://github.com/videojs/mux.js/blob/master/LICENSE
 *
 * Accepts program elementary stream (PES) data events and parses out
 * ID3 metadata from them, if present.
 * @see http://id3.org/id3v2.3.0
 */
'use strict';
var
  Stream = require('../utils/stream'),
  StreamTypes = require('./stream-types'),
  id3 = require('../tools/parse-id3'),
  MetadataStream;

MetadataStream = function(options) {
  var
    settings = {
      // the bytes of the program-level descriptor field in MP2T
      // see ISO/IEC 13818-1:2013 (E), section 2.6 "Program and
      // program element descriptors"
      descriptor: options && options.descriptor,
      // collect complete, byte-for-byte ID3 v2.4 tags so they can be
      // emitted as in-band emsg events
      inbandMetadata: !!(options && options.inbandMetadata)
    },
    // the total size in bytes of the ID3 tag being parsed
    tagSize = 0,
    // tag data that is not complete enough to be parsed
    buffer = [],
    // the total number of bytes currently in the buffer
    bufferSize = 0,
    // in-band framing state: a queue of byte ranges with the PES
    // timestamps they arrived on
    inbandChunks = [],
    inbandBufferSize = 0,
    // the size of the tag currently being collected
    inbandTagSize = 0,
    // the presentation and decode times of the PES that contained
    // the first byte of the tag currently being collected
    inbandTagPts,
    inbandTagDts,
    inbandCollecting = false,
    // true while skipping the body of an unsupported tag
    inbandSkipTag = false,
    i;

  var
    // return the first `count` bytes of the in-band buffer without
    // removing them
    peekInbandBytes = function(count) {
      var
        result = new Uint8Array(Math.min(count, inbandBufferSize)),
        offset = 0,
        chunk,
        take,
        j;

      for (j = 0; j < inbandChunks.length && offset < result.length; j++) {
        chunk = inbandChunks[j];
        take = Math.min(chunk.data.byteLength, result.length - offset);
        result.set(chunk.data.subarray(0, take), offset);
        offset += take;
      }
      return result;
    },
    // remove and return the first `count` bytes of the in-band buffer
    consumeInbandBytes = function(count) {
      var
        result = new Uint8Array(count),
        offset = 0,
        chunk,
        take;

      while (offset < count) {
        chunk = inbandChunks[0];
        take = Math.min(chunk.data.byteLength, count - offset);
        result.set(chunk.data.subarray(0, take), offset);
        offset += take;
        inbandBufferSize -= take;
        if (take === chunk.data.byteLength) {
          inbandChunks.shift();
        } else {
          chunk.data = chunk.data.subarray(take);
        }
      }
      return result;
    },
    // read the ID3 tag size field. Returns -1 when any of the four
    // bytes has its high bit set, which means the value is not a valid
    // syncsafe integer.
    readSyncsafeSize = function(bytes, offset) {
      var
        b0 = bytes[offset],
        b1 = bytes[offset + 1],
        b2 = bytes[offset + 2],
        b3 = bytes[offset + 3];

      if ((b0 | b1 | b2 | b3) & 0x80) {
        return -1;
      }
      return (b0 * 2097152) + (b1 * 16384) + (b2 * 128) + b3;
    },
    clearInbandState = function() {
      inbandChunks.length = 0;
      inbandBufferSize = 0;
      inbandTagSize = 0;
      inbandTagPts = undefined;
      inbandTagDts = undefined;
      inbandCollecting = false;
      inbandSkipTag = false;
    },
    inbandLog = function(stream, message) {
      stream.trigger('log', {
        level: 'warn',
        message: message
      });
    },
    pushInband = function(chunk, stream) {
      var
        header,
        size,
        frames,
        raw;

      // A data_alignment_indicator means the payload must start a new
      // ID3 tag. Any bytes left over from a tag that was never
      // completed are malformed; drop them rather than gluing them onto
      // the new tag.
      if (chunk.dataAlignmentIndicator && (inbandBufferSize > 0 || inbandCollecting)) {
        inbandLog(stream, 'Discarding incomplete ID3 tag at data alignment boundary');
        clearInbandState();
      }

      if (!chunk.data || !chunk.data.byteLength) {
        return;
      }

      // Copy the incoming bytes. The caller may reuse the input buffer
      // for a later push, and any bytes that are still buffered here
      // must not change underneath us.
      inbandChunks.push({
        data: new Uint8Array(chunk.data),
        pts: chunk.pts,
        dts: chunk.dts
      });
      inbandBufferSize += chunk.data.byteLength;

      while (inbandBufferSize > 0) {
        if (!inbandCollecting) {
          // an ID3 header is ten bytes long
          if (inbandBufferSize < 10) {
            break;
          }

          header = peekInbandBytes(10);

          if (header[0] !== 'I'.charCodeAt(0) ||
              header[1] !== 'D'.charCodeAt(0) ||
              header[2] !== '3'.charCodeAt(0)) {
            inbandLog(stream, 'Skipping unrecognized metadata packet');
            consumeInbandBytes(1);
            continue;
          }

          size = readSyncsafeSize(header, 6);

          // Only version 2.4 with no flags is supported. Tags that use
          // unsynchronization, an extended header or a footer are not
          // expanded. An unsupported tag is still delimited by its own
          // size so any complete tags that follow it are not lost.
          if (header[3] !== 4 || header[5] !== 0 || size < 0) {
            inbandLog(stream, 'Skipping unsupported ID3 tag');
            if (size < 0) {
              consumeInbandBytes(1);
            } else if (inbandBufferSize >= size + 10) {
              consumeInbandBytes(size + 10);
            } else {
              // wait for the rest of the unsupported tag so its bytes
              // are not mistaken for the start of a new tag
              inbandTagSize = size + 10;
              inbandCollecting = true;
              inbandSkipTag = true;
              inbandTagPts = inbandChunks[0].pts;
              inbandTagDts = inbandChunks[0].dts;
            }
            continue;
          }

          inbandTagSize = size + 10;
          inbandCollecting = true;
          inbandSkipTag = false;
          // the tag belongs to the PES that contained its first byte
          inbandTagPts = inbandChunks[0].pts;
          inbandTagDts = inbandChunks[0].dts;
        }

        if (inbandBufferSize < inbandTagSize) {
          break;
        }

        raw = consumeInbandBytes(inbandTagSize);

        if (inbandSkipTag) {
          inbandCollecting = false;
          inbandSkipTag = false;
          inbandTagSize = 0;
          inbandTagPts = undefined;
          inbandTagDts = undefined;
          continue;
        }

        frames = id3.parseId3Frames(raw);

        stream.trigger('data', {
          // keep the complete original tag bytes: header, unknown
          // frames, padding and all
          data: raw,
          frames: frames || [],
          pts: inbandTagPts,
          dts: inbandTagDts
        });

        inbandCollecting = false;
        inbandTagSize = 0;
        inbandTagPts = undefined;
        inbandTagDts = undefined;
      }
    };

  MetadataStream.prototype.init.call(this);

  // calculate the text track in-band metadata track dispatch type
  // https://html.spec.whatwg.org/multipage/embedded-content.html#steps-to-expose-a-media-resource-specific-text-track
  this.dispatchType = StreamTypes.METADATA_STREAM_TYPE.toString(16);
  if (settings.descriptor) {
    for (i = 0; i < settings.descriptor.length; i++) {
      this.dispatchType += ('00' + settings.descriptor[i].toString(16)).slice(-2);
    }
  }

  this.push = function(chunk) {
    var tag, frameStart, frameSize, frame, i, frameHeader;
    if (chunk.type !== 'timed-metadata') {
      return;
    }

    if (settings.inbandMetadata) {
      pushInband(chunk, this);
      return;
    }

    // if data_alignment_indicator is set in the PES header,
    // we must have the start of a new ID3 tag. Assume anything
    // remaining in the buffer was malformed and throw it out
    if (chunk.dataAlignmentIndicator) {
      bufferSize = 0;
      buffer.length = 0;
    }

    // ignore events that don't look like ID3 data
    if (buffer.length === 0 &&
        (chunk.data.length < 10 ||
          chunk.data[0] !== 'I'.charCodeAt(0) ||
          chunk.data[1] !== 'D'.charCodeAt(0) ||
          chunk.data[2] !== '3'.charCodeAt(0))) {
      this.trigger('log', {
        level: 'warn',
        message: 'Skipping unrecognized metadata packet'
      });
      return;
    }

    // add this chunk to the data we've collected so far

    buffer.push(chunk);
    bufferSize += chunk.data.byteLength;

    // grab the size of the entire frame from the ID3 header
    if (buffer.length === 1) {
      // the frame size is transmitted as a 28-bit integer in the
      // last four bytes of the ID3 header.
      // The most significant bit of each byte is dropped and the
      // results concatenated to recover the actual value.
      tagSize = id3.parseSyncSafeInteger(chunk.data.subarray(6, 10));

      // ID3 reports the tag size excluding the header but it's more
      // convenient for our comparisons to include it
      tagSize += 10;
    }

    // if the entire frame has not arrived, wait for more data
    if (bufferSize < tagSize) {
      return;
    }

    // collect the entire frame so it can be parsed
    tag = {
      data: new Uint8Array(tagSize),
      frames: [],
      pts: buffer[0].pts,
      dts: buffer[0].dts
    };
    for (i = 0; i < tagSize;) {
      tag.data.set(buffer[0].data.subarray(0, tagSize - i), i);
      i += buffer[0].data.byteLength;
      bufferSize -= buffer[0].data.byteLength;
      buffer.shift();
    }

    // find the start of the first frame and the end of the tag
    frameStart = 10;
    if (tag.data[5] & 0x40) {
      // advance the frame start past the extended header
      frameStart += 4; // header size field
      frameStart += id3.parseSyncSafeInteger(tag.data.subarray(10, 14));

      // clip any padding off the end
      tagSize -= id3.parseSyncSafeInteger(tag.data.subarray(16, 20));
    }

    // parse one or more ID3 frames
    // http://id3.org/id3v2.3.0#ID3v2_frame_overview
    do {
      // determine the number of bytes in this frame
      frameSize = id3.parseSyncSafeInteger(tag.data.subarray(frameStart + 4, frameStart + 8));
      if (frameSize < 1) {
        this.trigger('log', {
          level: 'warn',
          message: 'Malformed ID3 frame encountered. Skipping remaining metadata parsing.'
        });
        // If the frame is malformed, don't parse any further frames but allow previous valid parsed frames
        // to be sent along.
        break;
      }
      frameHeader = String.fromCharCode(tag.data[frameStart],
                                        tag.data[frameStart + 1],
                                        tag.data[frameStart + 2],
                                        tag.data[frameStart + 3]);


      frame = {
        id: frameHeader,
        data: tag.data.subarray(frameStart + 10, frameStart + frameSize + 10)
      };
      frame.key = frame.id;

      // parse frame values
      if (id3.frameParsers[frame.id]) {
        // use frame specific parser
        id3.frameParsers[frame.id](frame);
      } else if (frame.id[0] === 'T') {
        // use text frame generic parser
        id3.frameParsers['T*'](frame);
      } else if (frame.id[0] === 'W') {
        // use URL link frame generic parser
        id3.frameParsers['W*'](frame);
      }

      // handle the special PRIV frame used to indicate the start
      // time for raw AAC data
      if (frame.owner === 'com.apple.streaming.transportStreamTimestamp') {
        var
          d = frame.data,
          size = ((d[3] & 0x01)  << 30) |
                  (d[4]  << 22) |
                  (d[5] << 14) |
                  (d[6] << 6) |
                  (d[7] >>> 2);

        size *= 4;
        size += d[7] & 0x03;
        frame.timeStamp = size;
        // in raw AAC, all subsequent data will be timestamped based
        // on the value of this frame
        // we couldn't have known the appropriate pts and dts before
        // parsing this ID3 tag so set those values now
        if (tag.pts === undefined && tag.dts === undefined) {
          tag.pts = frame.timeStamp;
          tag.dts = frame.timeStamp;
        }
        this.trigger('timestamp', frame);
      }

      tag.frames.push(frame);

      frameStart += 10; // advance past the frame header
      frameStart += frameSize; // advance past the frame body
    } while (frameStart < tagSize);
    this.trigger('data', tag);
  };

  this.reset = function(flushSource) {
    // Always drop in-band framing fragments; in the default mode the
    // legacy buffer is left intact to preserve existing behaviour.
    clearInbandState();
    if (settings.inbandMetadata) {
      bufferSize = 0;
      buffer.length = 0;
      tagSize = 0;
    }
    this.trigger('reset', flushSource);
  };

  // Discard any partially collected in-band data without emitting a
  // reset event. Used when the caller adjusts the media timeline and
  // wants to make sure stale fragments cannot be glued onto new data.
  this.resetFraming = function() {
    clearInbandState();
  };
};
MetadataStream.prototype = new Stream();

module.exports = MetadataStream;
