/* exif-data-pinned-priv.h
 *
 * Private interface between the pinned MakerNote writer, the EXIF loader and
 * the IFD/entry mutation code. Not installed; not part of the public ABI.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef LIBEXIF_EXIF_DATA_PINNED_PRIV_H
#define LIBEXIF_EXIF_DATA_PINNED_PRIV_H

#include <libexif/exif-data.h>
#include <libexif/exif-byte-order.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Verbatim description of the single non-inline MakerNote block seen while
 * parsing valid EXIF input. Offsets are relative to the TIFF header, i.e.
 * the byte stream starting with the "II"/"MM" marker. */
typedef struct _PinnedNote PinnedNote;
struct _PinnedNote {
	unsigned int offset;
	const unsigned char *data;
	unsigned int size;
	ExifByteOrder order;
};

/* Record a MakerNote block encountered by the loader. Calling this more than
 * once marks the pin as unprovable. The buffer contents are copied. */
void exif_data_pinned_record (ExifData *data, ExifByteOrder order,
			      unsigned int offset,
			      const unsigned char *buf, unsigned int size);

/* Forget any pin state. Called when a new document is loaded into the same
 * ExifData. */
void exif_data_pinned_reset (ExifData *data);

/* Mark any recorded pin unusable (entry removed/rebuilt, entry fixed, byte
 * order switched, ...). */
void exif_data_pinned_invalidate (ExifData *data);

/* Return 1 and fill *out if a usable pin exists, 0 if no MakerNote was ever
 * recorded, and -1 if a MakerNote existed but its position cannot be proven. */
int  exif_data_pinned_get (ExifData *data, PinnedNote *out);

/* Release the pin state together with the ExifData. */
void exif_data_pinned_dispose (ExifData *data);

/* Internal accessor for the ExifMem backing an ExifData. Used so the pinned
 * writer allocates its output with the same allocator the caller frees it
 * with. */
ExifMem *exif_data_get_mem (ExifData *data);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !defined(LIBEXIF_EXIF_DATA_PINNED_PRIV_H) */
