# libfoxenflac ― Tiny, heap-allocation free FLAC decoder written in C 

![Build Status](https://github.com/astoeckel/libfoxenflac/actions/workflows/ci.yml/badge.svg)
[![Coverage Status](https://coveralls.io/repos/github/astoeckel/libfoxenflac/badge.svg)](https://coveralls.io/github/astoeckel/libfoxenflac)

`libfoxenflac` is a tiny FLAC ([Free Lossless Audio Codec](https://xiph.org/flac/))
decoder written in C99. It does not depend on any C library function,
including memory allocations. It provides a simple, state-machine based
interface that allows you to decode a FLAC stream from a sequence of arbitrarily
sized byte buffers. Depending on maximum number of channels and the maximum
block size, `libfoxenflac` requires between 73 kiB (FLAC Subset format,
stereo, up to 48000 kHz) and 4 MiB of memory (all standard-conformant FLAC
files, including 32-bit streams). Query the exact size at runtime with
`fx_flac_size()`; it is not a compile-time constant.

This library is perfect for environments without runtime library, such as
embedded devices or Web Assembly (WASM).

## Features

* **Small footprint**. Zero dependencies on the C runtime (runs on bare metal)
* Supports reading/writing to/from **arbitrarily sized memory buffers**. If you
  want to, you can decode from single bytes trickeling in and write to
  individual samples.
* Compiles to 8.8 kiB of **WebAssembly** (4.5 kiB compressed). However, no
  JavaScript binding is provided at the moment.
* Supports **all FLAC features**, including **32-bit audio** (RFC 9639).
* Quite thoroughly tested, considerable **test coverage**.
* Roboust **resynchronisation** on corrupted files.
* Implements all **CRC checks**.
* **Fast**. Although the code is not optimized, `libfoxenflac` is reasonably
  fast, being about 25% faster than the `flac` reference decoder
  application on `x86_64` systems. Performance on `ARM6` systems is significantly
  worse.

The following list of items is *not* implemented at the moment:
* Reading headers other than `STREAMINFO`, such as the metadata header or the
  seek table. Support for metadata could be implemented in the future.
* Access to information stored in the frame header, such as synchronisation
  information.
* **Seeking**. There is no intention to implement seeking directly in the
  library. The library solely operates on a stream of data; there is no notion
  of position within a file. However, once access to the frame header
  information is provided, it should be quite simple to implement seeking on top of
  this library.


## Usage

```C
#include <foxen-flac.h>

#include <stdlib.h> /* The FX_FLAC_ALLOC_DEFAULT macro uses malloc(). Have
                       a look at flac.h for more info on how to instantiate
                       libfoxenflac without heap allocations. */

int main() {
	fx_flac_t *flac = FX_FLAC_ALLOC_DEFAULT();
	if (!flac) {
		/* Out of memory! */
		return 1;
	}


	uint8_t buf[128];
	int32_t out_buf[512];
	while (true) {
		/* TODO: Append data to buf, adjust buf_len */
		uint32_t buf_len = /* Available input data in bytes */

		/* Run fx_flac_process, buf_len will be set to the number of bytes
		   that have been processed, out_buf_len will be set to the number
		   of samples (individual int32_t integers) that have been written. */
		uint32_t out_buf_len = 512;
		if (fx_flac_process(flac, buf, &buf_len, out_buf, &out_buf_len) == FLAC_ERR) {
			/* TODO: Handle error */
			break;
		}

		if (out_buf_len > 0) {
			/* TODO: Do something with the channel-interlaced data in out_buf */
			/* Note that this data is always shifted such that it uses the
			   entire 32-bit signed integer; shift to the right to the desired
			   output bit depth. You can obtain the bit-depth used in the file
			   using fx_flac_get_streaminfo(). */
		}

		/* TODO: Discard the first buf_len bytes in buf, pass the remaining
		   bytes to fx_flac_process() in the next iteration. */
	}

	free(flac); /* Need to free the flac instance because we implicitly used
	               malloc() in FX_FLAC_ALLOC_DEFAULT */
}
```

See `examples/flac_decoder.c` for a complete example.

## Bit depths and output alignment

`libfoxenflac` decodes all FLAC bit depths from 4 to 32 bits per sample,
including the 32-bit format introduced by
[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html). A 32-bit stream may
declare its bit depth either in the `STREAMINFO` block (with the frame
header inheriting it) or directly in the frame header using the formerly
reserved sample-size code 7. All channel assignments (independent, left-side,
right-side and mid-side stereo) are supported; side channels of 32-bit
streams are decoded at their full 33-bit width, and the prediction
accumulator and channel decorrelation run in 64 bits so that no intermediate
result is truncated to 32 bits.

Decoded samples are always delivered as `int32_t`:

* **32-bit streams** are passed through exactly, bit for bit.
* **Streams with a lower bit depth** are shifted to the left so that they
  occupy the full 32-bit signed integer range (e.g. a 16-bit sample `v`
  becomes `v << 16`). Use `fx_flac_get_streaminfo(inst, FLAC_KEY_SAMPLE_SIZE)`
  to query the bit depth and shift right to the desired output depth.

Note that the per-channel sample buffers are 64 bits wide to accommodate
33-bit side channels, so `fx_flac_size()` returns larger values than it did
before 32-bit support was added. Always allocate the decoder instance using
the size reported by `fx_flac_size()` (the `FX_FLAC_ALLOC*` macros do this
for you); the decoder never allocates memory on its own and never writes
outside the provided region.

## Using libfoxenflac in your own project

To use libfoxenflac, simply add `flac.c` and `flac.h` to your project.
These files have no further dependencies (apart from the standard library
headers `assert.h`, `stddef.h`, `stdint.h`, `stdbool.h`).

However, if you'd like to, you can also use the `meson` build system to run
unit tests and to add `libfoxenflac` into your own project.

## Building and running the test application

`libfoxenflac` uses the `meson` build system for building and dependency
management. You can install `meson` by running `pip3 install meson`.

```sh
git clone https://github.com/astoeckel/libfoxenflac
cd libfoxenflac; mkdir build; cd build
meson -Dbuildtype=release -Db_lto=True ..
ninja
```

Run the unit tests by executing
```sh
ninja test
```

> [!NOTE]  
> This requires the reference `flac` decoder to be installed. Some test files
> need to be downloaded from the internet. Manually run
> `test/test_flac_integration_runner.py` to download these files.

To test the library in action, you can for example run the following (assumes
you have `curl` and `aplay` installed, which is the default on most Linux
distributions)
```
curl http://www.hyperion-records.co.uk/audiotest/1%20Vaet%20Videns%20Dominus.FLAC | ./flac_decoder - | aplay -f S32_LE -r 44100 -c 2
```

### Web Assembly Build

`libfoxenflac` should work out of the box with WASM. For example, to compile and
run the Unit tests just run the the following from the `build` directory
```
emcc -Oz \
    ../test/test_flac.c \
    ../src/foxen-flac.c \
    ../subprojects/libfoxenbitstream/foxen/bitstream.c \
    -I ../subprojects/libfoxenunit \
    -I ../ \
    -o test_flac.js
node test_flac.js
```


## FAQ

**Q: What's with the name?**

**A:** [*Foxen*](http://kingkiller.wikia.com/wiki/Foxen) is a mysterious glowing object guiding Auri through the catacumbal “Underthing”. The *Foxen* software libraries are similar in key aspects: mysterious and catacumbal. Probably less useful than an eternal sympathy lamp though.

**Q: What is the purpose and goal of these libraries?**

**A:** The *Foxen* libraries are extremely small C libraries that rely on the [Meson](https://mesonbuild.com/) build system for dependency management. One common element is that the libraries do not use [heap memory allocations](https://github.com/astoeckel/libfoxenmem). They can thus be easily compiled to tiny, standalone [WASM](https://webassembly.org/) code.

**Q: Why?**

**A:** Excellent question! The author mainly created these libraries because he grew tired of copying his own source code files between projects all the time.

**Q: Would you recommend to use libfoxenflac in my project?**

**A:** That depends. Some of the code is fairly specialized according to my own needs and might not be intended to be general. If what you are going to use these libraries for something that aligns with their original purpose, then sure, go ahead. Otherwise, I'd probably advise against using these libraries, and as explained below, I'm not super open to expanding their scope.

**Q: Can I contribute?**

**A:** Sure! Feel free to open an issue or a PR. However, be warned that since I've mainly developed these libraries for use in my own stuff, I might be a little picky about what I'm going to include and what not.

## Licence

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.
