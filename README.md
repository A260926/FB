alac.js: An Apple Lossless decoder in the browser
================================================================================

The Apple Lossless Audio Codec (ALAC) is an audio codec developed by Apple and included in the original iPod.
ALAC is a data compression method which reduces the size of audio files with no loss of information.
A decoded ALAC stream is bit-for-bit identical to the original uncompressed audio file.

The original encoder and decoder were [open sourced](http://alac.macosforge.org/) by Apple, 
and this is a port of the decoder to CoffeeScript so that ALAC files can be played in the browser.

## Demo

You can check out a [demo](http://audiocogs.org/codecs/alac/) alongside our other decoders [flac.js](http://github.com/audiocogs/flac.js), [MP3.js](http://github.com/devongovett/mp3.js), and [AAC.js](http://github.com/audiocogs/aac.js).  Currently, alac.js works properly in the latest versions of Firefox, Chrome, and Safari.

## Authors

alac.js was written by [@jensnockert](http://github.com/jensnockert) and [@devongovett](http://github.com/devongovett) 
of [Audiocogs](http://audiocogs.org/).

## Building

We use [browserify](https://github.com/substack/node-browserify) to build alac.js.  You can download a
prebuilt version from the Github [releases](https://github.com/audiocogs/alac.js/releases) page. 
To build alac.js for the browser yourself, use the following commands:

    npm install
    make browser
    
This will place a built `alac.js` file, as well as a source map in the `build/` directory.

alac.js depends on [Aurora.js](https://github.com/audiocogs/aurora.js), our audio codec framework.
For detailed information on how to use Aurora.js, check out the [documentation](https://github.com/audiocogs/aurora.js/wiki).

## Decoding notes

Each call to `readChunk` consumes exactly one ALAC packet and returns an
`Int16Array` whose length is the packet's *actual* frame count times
`config.numChannels`. The `frameLength` field in the magic cookie is only the
maximum number of frames a packet may contain: a packet with the partial-frame
flag set carries its own (smaller) frame count, and the returned array is
sized to that count, never padded out to `frameLength` with zeros.

Samples are returned interleaved per packet: the SCE/LFE (one channel) and
CPE (two channels) elements in a packet occupy consecutive output channels in
the order they appear, and frame *i* of every element is written to frame *i*
of the interleaved result. For example, an SCE with frames `10, 20` followed
by a CPE with frames `(30, 40), (50, 60)` yields `[10, 30, 40, 20, 50, 60]`.
No speaker-role reordering is applied between elements.

All audio elements in a packet must agree on the frame count, and the
packet's elements must fill exactly `config.numChannels` channels by the
`ID_END` tag; otherwise the packet is rejected with an error rather than
returning partial or zero-padded data. Truncated packets keep Aurora's
`UnderflowError` behavior, so decoding resumes once more data arrives.

    
## License

alac.js is released under the same terms as the original ALAC decoder from Apple, which is the 
[Apache 2](http://www.apache.org/licenses/LICENSE-2.0) license.