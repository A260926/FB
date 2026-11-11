/**
 * Incremental WaveformData builder.
 *
 * Accepts PCM audio samples chunk by chunk and computes waveform min/max
 * data using the same rounding and saturation rules as generateWaveformData.
 * Only the computed min/max values and the current incomplete pixel are
 * retained; the incoming PCM samples are not stored.
 */

const INT8_MAX = 127;
const INT8_MIN = -128;

const INT16_MAX = 32767;
const INT16_MIN = -32768;

const defaultOptions = {
  scale: 512,
  bits: 8,
  amplitude_scale: 1.0,
  split_channels: false
};

function isPositiveInteger(value) {
  return typeof value === 'number' && Number.isInteger(value) && value > 0;
}

function getOption(options, name) {
  return options[name] === undefined ? defaultOptions[name] : options[name];
}

function WaveformDataBuilder(options) {
  options = options || {};

  const sample_rate = options.sample_rate;
  const channels = options.channels;
  const scale = getOption(options, 'scale');
  const bits = getOption(options, 'bits');
  const amplitude_scale = getOption(options, 'amplitude_scale');

  if (!isPositiveInteger(sample_rate)) {
    throw new RangeError('WaveformData.createBuilder(): sample_rate must be a positive integer');
  }

  if (!isPositiveInteger(channels)) {
    throw new RangeError('WaveformData.createBuilder(): channels must be a positive integer');
  }

  if (!isPositiveInteger(scale)) {
    throw new RangeError('WaveformData.createBuilder(): scale must be a positive integer');
  }

  if (bits !== 8 && bits !== 16) {
    throw new RangeError('WaveformData.createBuilder(): bits must be either 8 or 16');
  }

  if (typeof amplitude_scale !== 'number' ||
      !Number.isFinite(amplitude_scale) ||
      amplitude_scale < 0) {
    throw new RangeError(
      'WaveformData.createBuilder(): amplitude_scale must be a finite, non-negative number'
    );
  }

  this._sampleRate = sample_rate;
  this._inputChannels = channels;
  this._scale = scale;
  this._bits = bits;
  this._amplitudeScale = amplitude_scale;
  this._outputChannels = getOption(options, 'split_channels') ? channels : 1;

  this._rangeMin = bits === 8 ? INT8_MIN : INT16_MIN;
  this._rangeMax = bits === 8 ? INT8_MAX : INT16_MAX;

  this._min = [];
  this._max = [];
  this._minValue = [];
  this._maxValue = [];

  for (let channel = 0; channel < this._outputChannels; channel++) {
    this._min[channel] = [];
    this._max[channel] = [];
    this._minValue[channel] = Infinity;
    this._maxValue[channel] = -Infinity;
  }

  this._scaleCounter = 0;
  this._finished = false;
  this._buffer = null;
}

WaveformDataBuilder.prototype._update = function(channel, sample) {
  if (sample < this._minValue[channel]) {
    this._minValue[channel] = sample;

    if (this._minValue[channel] < this._rangeMin) {
      this._minValue[channel] = this._rangeMin;
    }
  }

  if (sample > this._maxValue[channel]) {
    this._maxValue[channel] = sample;

    if (this._maxValue[channel] > this._rangeMax) {
      this._maxValue[channel] = this._rangeMax;
    }
  }
};

WaveformDataBuilder.prototype._flushPixel = function() {
  for (let channel = 0; channel < this._outputChannels; channel++) {
    this._min[channel].push(this._minValue[channel]);
    this._max[channel].push(this._maxValue[channel]);

    this._minValue[channel] = Infinity;
    this._maxValue[channel] = -Infinity;
  }

  this._scaleCounter = 0;
};

/**
 * Appends a chunk of PCM audio samples.
 *
 * The channelData argument is an array of Float32Array objects, one per
 * channel, each of the same length. The arrays may be views onto a larger
 * buffer. The data is consumed synchronously, so the caller is free to
 * reuse the arrays once this function returns.
 *
 * Throws (and ignores the whole chunk) if the number of channels is wrong,
 * the channel lengths differ, or any sample is not a finite number.
 */

WaveformDataBuilder.prototype.append = function(channelData) {
  if (this._finished) {
    throw new Error('WaveformDataBuilder.append(): Cannot append after finish() has been called');
  }

  if (channelData === null ||
      typeof channelData !== 'object' ||
      typeof channelData.length !== 'number') {
    throw new TypeError(
      'WaveformDataBuilder.append(): Expected an array of Float32Array channel data'
    );
  }

  if (channelData.length !== this._inputChannels) {
    throw new RangeError(
      'WaveformDataBuilder.append(): Expected ' + this._inputChannels +
      ' channels, got ' + channelData.length
    );
  }

  // Validate the whole chunk before consuming any of it, so that a
  // rejected chunk leaves the builder state untouched.

  let length = 0;

  for (let channel = 0; channel < this._inputChannels; channel++) {
    const data = channelData[channel];

    if (data === null || typeof data !== 'object' || typeof data.length !== 'number') {
      throw new TypeError('WaveformDataBuilder.append(): Channel data must be a Float32Array');
    }

    if (channel === 0) {
      length = data.length;
    }
    else if (data.length !== length) {
      throw new RangeError('WaveformDataBuilder.append(): All channels must have the same length');
    }

    for (let i = 0; i < data.length; i++) {
      if (!Number.isFinite(data[i])) {
        throw new TypeError('WaveformDataBuilder.append(): Samples must be finite numbers');
      }
    }
  }

  for (let i = 0; i < length; i++) {
    if (this._outputChannels === 1) {
      let sample = 0;

      for (let channel = 0; channel < this._inputChannels; channel++) {
        sample += channelData[channel][i];
      }

      sample = Math.floor(this._rangeMax * sample * this._amplitudeScale / this._inputChannels);

      this._update(0, sample);
    }
    else {
      for (let channel = 0; channel < this._outputChannels; channel++) {
        const sample = Math.floor(this._rangeMax * channelData[channel][i] * this._amplitudeScale);

        this._update(channel, sample);
      }
    }

    if (++this._scaleCounter === this._scale) {
      this._flushPixel();
    }
  }
};

/**
 * Completes the waveform, flushing any incomplete final pixel, and returns
 * the waveform data in binary format as an ArrayBuffer. May be called
 * multiple times; subsequent calls return the same data.
 */

WaveformDataBuilder.prototype.finish = function() {
  if (this._finished) {
    return this._buffer;
  }

  if (this._scaleCounter > 0) {
    this._flushPixel();
  }

  const data_length = this._min[0].length;
  const header_size = 24; // Version 2
  const bytes_per_sample = this._bits === 8 ? 1 : 2;
  const total_size = header_size + data_length * 2 * bytes_per_sample * this._outputChannels;
  const buffer = new ArrayBuffer(total_size);
  const data_view = new DataView(buffer);

  data_view.setInt32(0, 2, true); // Version
  data_view.setUint32(4, this._bits === 8, true); // Is 8 bit?
  data_view.setInt32(8, this._sampleRate, true); // Sample rate
  data_view.setInt32(12, this._scale, true); // Scale
  data_view.setInt32(16, data_length, true); // Length
  data_view.setInt32(20, this._outputChannels, true); // Channels

  let offset = header_size;

  for (let i = 0; i < data_length; i++) {
    for (let channel = 0; channel < this._outputChannels; channel++) {
      if (this._bits === 8) {
        data_view.setInt8(offset++, this._min[channel][i]);
        data_view.setInt8(offset++, this._max[channel][i]);
      }
      else {
        data_view.setInt16(offset, this._min[channel][i], true);
        data_view.setInt16(offset + 2, this._max[channel][i], true);
        offset += 4;
      }
    }
  }

  this._finished = true;
  this._buffer = buffer;

  return buffer;
};

export default WaveformDataBuilder;
