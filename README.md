# UTIF.js
A small, fast and advanced TIFF / EXIF (+ DNG, CR2, NEF and other TIFF-ish files) decoder and encoder. It is the main TIFF library for [Photopea image editor](https://www.photopea.com). Try to open your TIFF file with Photopea to see, if UTIF.js can parse it.

* Supports Black & White, Grayscale, RGB and Paletted images
* Supports Fax 3 and Fax 4 (CCITT), JPEG, LZW, PackBits and other compressions (1,3,4,5,6,7,8,32773,32809)
* E.g. [this 8 MPix image](//www.photopea.com/api/img2/G4.TIF) with Fax 4 compression is just 56 kB ( [Open in Photopea](https://www.photopea.com?p=%7B%22files%22:%5B%22//www.photopea.com/api/img2/G4.TIF%22%5D%7D) )

For RAW files, UTIF.js only decodes raw sensor data (and JPG previews, if there are any). It does not convert the raw data into a displayable image (RGBA). Such conversion is complex and out of scope of this library.

## Installation

Download and include the `UTIF.js` file in your code. 

#### `UTIF.decode(buffer)`
* `buffer`: ArrayBuffer containing TIFF or EXIF data
* returns an array of "IFDs" (image file directories). Each IFD is an object, keys are "tXYZ" (XYZ is a TIFF tag number), values are values of these tags. You can get the the dimension (and other properties, "metadata") of the image without decompressing pixel data.

#### `UTIF.decodeImage(buffer, ifd)`
* `buffer`: ArrayBuffer containing TIFF or EXIF data
* `ifd`: the element of the output of UTIF.decode()
* If there is an image inside the IFD, it is decoded and three new properties are added to the IFD:
* * `width`: the width of the image
* * `height`: the height of the image
* * `data`: decompressed pixel data of the image

TIFF files may have various number of channels and various color depth. The interpretation of `data` depends on many tags (see the [TIFF 6 specification](http://www.npes.org/pdf/TIFF-v6.pdf)). The following function converts any TIFF image into a 8-bit RGBA image.

#### `UTIF.toRGBA8(ifd)`
* `ifd`: image file directory (element of "ifds" returned by UTIF.decode(), processed by UTIF.decodeImage())
* returns Uint8Array of the image in RGBA format, 8 bits per channel (ready to use in context2d.putImageData() etc.)

### Example

```javascript
function imgLoaded(e) {
  var ifds = UTIF.decode(e.target.response);
  UTIF.decodeImage(e.target.response, ifds[0])
  var rgba  = UTIF.toRGBA8(ifds[0]);  // Uint8Array with RGBA pixels
  console.log(ifds[0].width, ifds[0].height, ifds[0]);
}

var xhr = new XMLHttpRequest();
xhr.open("GET", "my_image.tif");
xhr.responseType = "arraybuffer";
xhr.onload = imgLoaded;   xhr.send();
```
## Use TIFF images in HTML

If you are not a programmer, you can use TIFF images directly inside the `<img>` element of HTML. Then, it is enough to call `UTIF.replaceIMG()` once at some point.

#### `UTIF.replaceIMG()`
```html
<body onload="UTIF.replaceIMG()">
...
<img src="image.tif" />  <img src="dog.tif" /> ...
```
And UTIF.js will do the rest. Internally, the "src" attribute of the image will be replaced with a new URI of the image (base64-encoded PNG). Note, that you can also insert DNG, CR2, NEF and other raw images into HTML this way.

## Encoding TIFF images

You should not save images into TIFF format in the 21st century. Save them as PNG instead (e.g. using [UPNG.js](https://github.com/photopea/UPNG.js)). If you still want to use TIFF format for some reason, here it is.

#### `UTIF.encodeImage(rgba, w, h, metadata)`
* `rgba`: ArrayBuffer containing RGBA pixel data
* `w`: image width
* `h`: image height
* `metadata` [optional]: IFD object (see below)
* returns ArrayBuffer of the binary TIFF file. No compression right now.

#### `UTIF.encode(ifds)`
* `ifds`: array of IFDs (image file directories). An IFD is a JS object with properties "tXYZ" (where XYZ are TIFF tags)
* returns ArrayBuffer of binary data. You can use it to encode EXIF data.

## Floating-point images (SampleFormat = 3)

For 32-bit RGB / RGBA / grayscale images and 64-bit grayscale images whose samples are IEEE floats, `decodeImage` produces an `ifd.data` byte array that is interpreted read-only by `toRGBA8`. Calling `toRGBA8` repeatedly never changes those bytes, and every call returns a fresh, independent RGBA `Uint8Array`.

### Byte order

The decompressed float bytes are stored in the TIFF file's own byte order: `II` means little-endian and `MM` means big-endian. The endianness comes only from the file header (exposed as `ifd.isLE`); it is never inferred from the sample values. A negative linear color is valid data and does not imply a wrong byte order, so bytes are never swapped in place. `toRGBA8` reads the samples through a `DataView`, so a `data` view with a non-zero `byteOffset` is interpreted strictly within its own range.

### Data layout

* Chunky data (`PlanarConfiguration = 1`) holds the channels of each pixel consecutively, one whole multi-byte sample per channel.
* Planar data (`PlanarConfiguration = 2`, RGB) is stored as separate R, G and B planes. `decodeImage` interleaves the planes into per-pixel samples while decoding; a sample byte is never treated as a channel by itself.
* With strips, the last strip may contain fewer rows than `RowsPerStrip`; with tiles, the physical tile width/height may extend beyond the image's right and bottom edges. Sample strides, plane offsets and copy ranges are all computed from the storage block (strip or physical tile), and only the pixels inside the image boundary are copied.
* Only the uncompressed, Deflate and PackBits paths are handled for floats; the Predictor stays at 1.

### Color and alpha

RGB values are treated as linear sRGB: alpha association is resolved first, colors are clamped to `[0, 1]`, the standard sRGB transfer function is applied, and the result is scaled to 0–255 and rounded. Grayscale keeps the existing linear-intensity mapping (clamp, then round) and is never gamma-corrected.

* `ExtraSamples = 2` (non-associated alpha): color is converted independently; alpha is only clamped and rounded linearly, with no gamma applied.
* `ExtraSamples = 1` (associated / premultiplied alpha): the color is divided by the effective alpha *before* the sRGB transfer function. Alpha itself is never gamma-corrected.
* Associated alpha of `0` outputs transparent black; a non-associated transparent pixel keeps its underlying color.
* RGB and grayscale images without an alpha channel output `255`.
* `NaN` and negative infinity become `0`, positive infinity becomes `1` (including for alpha). Ordinary negative values clamp to `0` and values above `1` clamp to `1`; there is no exposure adjustment or automatic range normalization.
