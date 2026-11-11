const assert = require('node:assert/strict');
const WaveformData = require('../dist/waveform-data.cjs.js');

// Creates waveform data in one shot through the existing whole-buffer entry.
function fromAudio(channels, options) {
  const audio = {
    numberOfChannels: channels.length,
    length: channels[0].length,
    sampleRate: 48000,
    getChannelData: i => channels[i]
  };
  let result;

  WaveformData.createFromAudio(
    Object.assign({ audio_buffer: audio, disable_worker: true }, options),
    (e, w) => {
      if (e) throw e;
      result = w;
    }
  );

  return result;
}

// Creates waveform data incrementally through the builder, appending
// chunks of the given sizes (in frames). Sizes of 0 produce empty blocks.
function fromChunks(channels, options, chunkSizes) {
  const builder = WaveformData.createBuilder(
    Object.assign({ sample_rate: 48000, channels: channels.length }, options)
  );
  let frame = 0;

  for (const size of chunkSizes) {
    builder.append(channels.map(c => c.subarray(frame, frame + size)));
    frame += size;
  }

  assert.equal(frame, channels[0].length, 'chunk sizes must add up to the frame count');

  return builder.finish();
}

function assertWaveformEqual(actual, expected) {
  assert.equal(actual.channels, expected.channels);
  assert.equal(actual.length, expected.length);
  assert.equal(actual.bits, expected.bits);
  assert.equal(actual.scale, expected.scale);
  assert.equal(actual.sample_rate, expected.sample_rate);

  for (let channel = 0; channel < actual.channels; channel++) {
    assert.deepEqual(actual.channel(channel).min_array(), expected.channel(channel).min_array());
    assert.deepEqual(actual.channel(channel).max_array(), expected.channel(channel).max_array());
  }
}

// 48 kHz stereo example: chunking must not change the waveform, and the
// 16-bit multi-channel tail bucket must be correct in both entries.
{
  const left = new Float32Array([0.5, -0.5, 1]);
  const right = new Float32Array([0.25, -0.25, -1]);
  const options = { scale: 2, bits: 16, split_channels: true };

  for (const chunkSizes of [[3], [1, 2], [1, 1, 1], [0, 3, 0]]) {
    const w = fromChunks([left, right], options, chunkSizes);

    assert.equal(w.channels, 2);
    assert.equal(w.length, 2);
    assert.deepEqual(w.channel(0).min_array(), [-16384, 32767]);
    assert.deepEqual(w.channel(0).max_array(), [16383, 32767]);
    assert.deepEqual(w.channel(1).min_array(), [-8192, -32767]);
    assert.deepEqual(w.channel(1).max_array(), [8191, -32767]);
  }

  const whole = fromAudio([left, right], options);

  assert.deepEqual(whole.channel(0).min_array(), [-16384, 32767]);
  assert.deepEqual(whole.channel(0).max_array(), [16383, 32767]);
  assert.deepEqual(whole.channel(1).min_array(), [-8192, -32767]);
  assert.deepEqual(whole.channel(1).max_array(), [8191, -32767]);

  console.log('PASS: 16-bit stereo, chunking invariant, tail bucket correct in both entries');
}

// 8-bit split-channel results match the established whole-buffer values.
{
  const left = new Float32Array([0.5, -0.5, 1]);
  const right = new Float32Array([0.25, -0.25, -1]);
  const w = fromChunks([left, right], { scale: 2, bits: 8, split_channels: true }, [3]);

  assert.deepEqual(w.channel(0).min_array(), [-64, 127]);
  assert.deepEqual(w.channel(0).max_array(), [63, 127]);
  assert.deepEqual(w.channel(1).min_array(), [-32, -127]);
  assert.deepEqual(w.channel(1).max_array(), [31, -127]);

  console.log('PASS: 8-bit split-channel builder matches whole-buffer values');
}

// Mixed (non-split) mode averages channels within each sample frame, so
// opposite signals cancel; averaging per-channel peaks would not give 0.
{
  const left = new Float32Array([0.5, -0.5]);
  const right = new Float32Array([-0.5, 0.5]);

  for (const bits of [8, 16]) {
    const w = fromChunks([left, right], { scale: 2, bits }, [1, 1]);

    assert.equal(w.channels, 1);
    assert.deepEqual(w.channel(0).min_array(), [0]);
    assert.deepEqual(w.channel(0).max_array(), [0]);
  }

  // Asymmetric mix: floor(127 * 1 / 2) = 63
  const w = fromChunks(
    [new Float32Array([1]), new Float32Array([0])],
    { scale: 1, bits: 8 },
    [1]
  );

  assert.deepEqual(w.channel(0).min_array(), [63]);
  assert.deepEqual(w.channel(0).max_array(), [63]);

  console.log('PASS: mixed mode averages channels per frame (cancellation, not peak average)');
}

// Explicit amplitude_scale of 0 produces silence, in both modes and bit depths.
{
  const left = new Float32Array([0.5, -0.5, 1]);
  const right = new Float32Array([0.25, -0.25, -1]);

  for (const bits of [8, 16]) {
    for (const split of [false, true]) {
      const w = fromChunks(
        [left, right],
        { scale: 2, bits, split_channels: split, amplitude_scale: 0 },
        [3]
      );
      const channels = split ? 2 : 1;

      assert.equal(w.channels, channels);
      assert.equal(w.length, 2);

      for (let channel = 0; channel < channels; channel++) {
        assert.deepEqual(w.channel(channel).min_array(), [0, 0]);
        assert.deepEqual(w.channel(channel).max_array(), [0, 0]);
      }
    }
  }

  console.log('PASS: amplitude_scale 0 outputs silence');
}

// Three channels, split and mixed, equal to the whole-buffer entry.
{
  const c0 = new Float32Array([1, -1, 0.5]);
  const c1 = new Float32Array([0.5, -0.5, 0.25]);
  const c2 = new Float32Array([-1, 1, -0.5]);
  const options = { scale: 2, bits: 16, split_channels: true };
  const w = fromChunks([c0, c1, c2], options, [2, 1]);

  assert.equal(w.channels, 3);
  assert.deepEqual(w.channel(0).min_array(), [-32767, 16383]);
  assert.deepEqual(w.channel(0).max_array(), [32767, 16383]);
  assert.deepEqual(w.channel(1).min_array(), [-16384, 8191]);
  assert.deepEqual(w.channel(1).max_array(), [16383, 8191]);
  assert.deepEqual(w.channel(2).min_array(), [-32767, -16384]);
  assert.deepEqual(w.channel(2).max_array(), [32767, -16384]);

  assertWaveformEqual(w, fromAudio([c0, c1, c2], options));
  assertWaveformEqual(
    fromChunks([c0, c1, c2], { scale: 2, bits: 16 }, [1, 1, 1]),
    fromAudio([c0, c1, c2], { scale: 2, bits: 16 })
  );

  console.log('PASS: 3-channel split and mixed waveforms');
}

// Channel data may be views with a byteOffset onto a larger buffer.
{
  const backingLeft = new Float32Array([9, 9, 0.5, -0.5, 1, 9]);
  const backingRight = new Float32Array([9, 0.25, -0.25, -1, 9, 9]);
  const viewLeft = new Float32Array(backingLeft.buffer, 2 * 4, 3);
  const viewRight = new Float32Array(backingRight.buffer, 1 * 4, 3);

  const builder = WaveformData.createBuilder({
    sample_rate: 48000, channels: 2, scale: 2, bits: 16, split_channels: true
  });

  builder.append([viewLeft, viewRight]);

  const w = builder.finish();

  assert.deepEqual(w.channel(0).min_array(), [-16384, 32767]);
  assert.deepEqual(w.channel(0).max_array(), [16383, 32767]);
  assert.deepEqual(w.channel(1).min_array(), [-8192, -32767]);
  assert.deepEqual(w.channel(1).max_array(), [8191, -32767]);

  console.log('PASS: channel data accepted as views with byteOffset');
}

// Invalid chunks throw synchronously and are ignored entirely; the stream
// can continue as if the bad chunk had never been appended.
{
  const options = { sample_rate: 48000, channels: 2, scale: 2, bits: 16, split_channels: true };
  const good1 = [new Float32Array([0.5, -0.5]), new Float32Array([0.25, -0.25])];
  const good2 = [new Float32Array([1]), new Float32Array([-1])];

  const reference = WaveformData.createBuilder(options);

  reference.append(good1);
  reference.append(good2);

  const expected = reference.finish();
  const builder = WaveformData.createBuilder(options);

  builder.append(good1);

  // Wrong number of channels
  assert.throws(() => builder.append([new Float32Array([0.1])]));
  // Channel lengths differ
  assert.throws(() => builder.append([new Float32Array([0.1, 0.2]), new Float32Array([0.1])]));
  // Non-finite samples
  assert.throws(() => builder.append([new Float32Array([NaN]), new Float32Array([0.1])]));
  assert.throws(() => builder.append([new Float32Array([Infinity]), new Float32Array([0.1])]));
  assert.throws(() => builder.append([new Float32Array([-Infinity]), new Float32Array([0.1])]));
  // Not an array of channel data
  assert.throws(() => builder.append(null));
  assert.throws(() => builder.append([null, null]));

  builder.append(good2);

  assertWaveformEqual(builder.finish(), expected);

  console.log('PASS: invalid chunks rejected, stream continues unaffected');
}

// The caller may reuse and overwrite the arrays once append() returns.
{
  const left = new Float32Array([0.5, -0.5]);
  const right = new Float32Array([0.25, -0.25]);
  const builder = WaveformData.createBuilder({
    sample_rate: 48000, channels: 2, scale: 2, bits: 16, split_channels: true
  });

  builder.append([left, right]);

  left.fill(0);
  right.fill(0);

  builder.append([new Float32Array([1]), new Float32Array([-1])]);

  const w = builder.finish();

  assert.deepEqual(w.channel(0).min_array(), [-16384, 32767]);
  assert.deepEqual(w.channel(0).max_array(), [16383, 32767]);
  assert.deepEqual(w.channel(1).min_array(), [-8192, -32767]);
  assert.deepEqual(w.channel(1).max_array(), [8191, -32767]);

  console.log('PASS: appended PCM is not retained; caller may reuse buffers');
}

// finish() is idempotent; append() after finish() throws; no input gives
// a zero-length waveform.
{
  const builder = WaveformData.createBuilder({ sample_rate: 48000, channels: 1, scale: 2, bits: 8 });

  builder.append([new Float32Array([0.5])]);

  const w1 = builder.finish();
  const w2 = builder.finish();

  assert.equal(w1.length, 1);
  assert.deepEqual(w1.channel(0).min_array(), [63]);
  assert.deepEqual(w1.channel(0).min_array(), w2.channel(0).min_array());
  assert.deepEqual(w1.channel(0).max_array(), w2.channel(0).max_array());
  assert.throws(() => builder.append([new Float32Array([0.5])]));

  const empty = WaveformData.createBuilder({ sample_rate: 48000, channels: 2, scale: 512, bits: 8 });

  empty.append([new Float32Array(0), new Float32Array(0)]);

  const w = empty.finish();

  assert.equal(w.length, 0);
  assert.equal(w.channels, 1);
  assert.deepEqual(w.channel(0).min_array(), []);
  assert.deepEqual(w.channel(0).max_array(), []);

  console.log('PASS: finish idempotent, append after finish throws, empty input');
}

// Builder option validation.
{
  assert.throws(() => WaveformData.createBuilder({ channels: 2 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 0, channels: 2 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000.5, channels: 2 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 0 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, scale: 0 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, scale: 1.5 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, bits: 12 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, amplitude_scale: -1 }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, amplitude_scale: NaN }));
  assert.throws(() => WaveformData.createBuilder({ sample_rate: 48000, channels: 2, amplitude_scale: Infinity }));

  console.log('PASS: builder option validation');
}

// Chunked and whole-buffer entries agree on a longer signal, for 8 and 16
// bits, split and mixed, including saturation from amplitude_scale > 1.
{
  let seed = 12345;
  const rand = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;

    return seed / 0x40000000 - 1;
  };
  const frames = 1000;
  const c0 = new Float32Array(frames);
  const c1 = new Float32Array(frames);

  for (let i = 0; i < frames; i++) {
    c0[i] = rand();
    c1[i] = rand();
  }

  const chunkings = [[1000], Array(frames).fill(1), [0, 1, 998, 1], [255, 256, 257, 232]];
  const variants = [
    { scale: 256, bits: 8, split_channels: false },
    { scale: 256, bits: 8, split_channels: true },
    { scale: 256, bits: 16, split_channels: false },
    { scale: 256, bits: 16, split_channels: true },
    { scale: 256, bits: 16, split_channels: true, amplitude_scale: 2.5 },
    { scale: 300, bits: 16, split_channels: false, amplitude_scale: 0.5 }
  ];

  for (const options of variants) {
    const whole = fromAudio([c0, c1], options);

    for (const chunkSizes of chunkings) {
      assertWaveformEqual(fromChunks([c0, c1], options, chunkSizes), whole);
    }
  }

  console.log('PASS: chunked and whole-buffer entries agree (8/16-bit, split/mixed, gain)');
}

// Builder output works with channel(), slice() and resample().
{
  const left = new Float32Array([0.5, -0.5, 1]);
  const right = new Float32Array([0.25, -0.25, -1]);
  const w = fromChunks([left, right], { scale: 2, bits: 16, split_channels: true }, [1, 2]);

  const sliced = w.slice({ startIndex: 1, endIndex: 2 });

  assert.deepEqual(sliced.channel(0).min_array(), [32767]);
  assert.deepEqual(sliced.channel(1).max_array(), [-32767]);

  const resampled = w.resample({ scale: 4 });

  assert.equal(resampled.length, 1);
  assert.deepEqual(resampled.channel(0).min_array(), [-16384]);
  assert.deepEqual(resampled.channel(0).max_array(), [32767]);
  assert.deepEqual(resampled.channel(1).min_array(), [-32767]);
  assert.deepEqual(resampled.channel(1).max_array(), [8191]);

  console.log('PASS: builder output works with channel, slice and resample');
}
