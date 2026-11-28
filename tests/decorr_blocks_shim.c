////////////////////////////////////////////////////////////////////////////
//                       **** WAVPACK ****                                 //
//   Test shim: reuse the library entropy writer for planned blocks        //
//                                                                         //
//  tests/decorr_blocks_shim.c                                             //
//                                                                         //
//  The planned-block regression must produce a legal ID_WV_BITSTREAM for  //
//  a residual plan it chose itself. Rather than reimplement the entropy   //
//  coder, this shim calls the library's own lossless entropy writer        //
//  (init_words / send_words_lossless / flush_word) on a locally built      //
//  WavpackStream, starting from the same zeroed entropy state that        //
//  unpack_init leaves in the decoder when no ID_ENTROPY_VARS item is       //
//  present. It does not touch decorrelation.                              //
////////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <string.h>

#include "../src/wavpack_local.h"

// Locally replicate the bitstream open/close that pack.c performs, so the
// shim does not need the static functions from pack.c.

static void shim_bs_write (Bitstream *bs)
{
    bs->ptr = bs->buf;
    bs->error = 1;
}

static void shim_bs_open (Bitstream *bs, void *start, void *end)
{
    bs->error = bs->sr = bs->bc = 0;
    bs->ptr = bs->buf = start;
    bs->end = end;
    bs->wrap = shim_bs_write;
}

static uint32_t shim_bs_close (Bitstream *bs)
{
    uint32_t bytes_written;

    if (bs->error)
        return (uint32_t) -1;

    while (1) {
        while (bs->bc)
            putbit_1 (bs);

        bytes_written = (uint32_t) (bs->ptr - bs->buf) * sizeof (*(bs->ptr));

        if (bytes_written & 1)
            putbit_1 (bs);
        else
            break;
    }

    CLEAR (*bs);
    return bytes_written;
}

// Encode "frames" interleaved residuals (mono or stereo, lossless) and return
// the raw descriptor bytes. Returns 0 on success.
int shim_encode_residuals (int mono, int bits, const int32_t *residual, int frames,
    unsigned char *out, int out_cap, int *out_len)
{
    WavpackStream wps;
    uint32_t n;

    (void) bits;

    if ((size_t) out_cap < 16)
        return -1;

    memset (&wps, 0, sizeof (wps));

    if (mono)
        wps.wphdr.flags |= MONO_FLAG;

    // zero entropy state, matching a decoder that saw no ID_ENTROPY_VARS item
    init_words (&wps);

    shim_bs_open (&wps.wvbits, out, out + out_cap);

    send_words_lossless (&wps, (int32_t *) residual, frames);
    flush_word (&wps);

    n = shim_bs_close (&wps.wvbits);

    if (n == (uint32_t) -1)
        return -1;

    *out_len = (int) n;
    return 0;
}
