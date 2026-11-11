/*
 *  bit_stream.c package
 *  Author:  Jean-Georges Fritsch, C-Cube Microsystems
 *
 * This package provides functions to write information to the bit stream.
 *
 * Removed unused functions. Feb 2001 P.Everett
 */

#include "bitstream.h"
#include "types.h"

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <malloc.h>
#endif

/* open the device to write the bit stream into it */
void shine_open_bit_stream(bitstream_t *bs, int size) {
  bs->data = (unsigned char *)malloc(size * sizeof(unsigned char));
  bs->data_size = size;
  bs->data_position = 0;
  bs->cache = 0;
  bs->cache_bits = 32;
}

/*close the device containing the bit stream */
void shine_close_bit_stream(bitstream_t *bs) {
  if (bs->data)
    free(bs->data);
}

/* Allocate a standalone bit stream with its own data buffer. */
bitstream_t *shine_new_bit_stream(int size) {
  bitstream_t *bs = (bitstream_t *)calloc(1, sizeof(bitstream_t));
  if (bs == NULL)
    return NULL;
  shine_open_bit_stream(bs, size);
  if (bs->data == NULL) {
    free(bs);
    return NULL;
  }
  return bs;
}

/* Free a bit stream created by shine_new_bit_stream. */
void shine_free_bit_stream(bitstream_t *bs) {
  if (bs == NULL)
    return;
  shine_close_bit_stream(bs);
  free(bs);
}

/* Write out any complete bytes that are still held in the cache, leaving
 * only sub-byte residue (Layer III frame boundaries are byte aligned). */
void shine_drain_bit_stream(bitstream_t *bs) {
  int full_bytes = (32 - bs->cache_bits) / 8;
  int i;

  if (full_bytes == 0)
    return;

  if (bs->data_position + full_bytes >= bs->data_size) {
    int new_size = bs->data_size + (bs->data_size / 2);
    while (new_size < bs->data_position + full_bytes + 1)
      new_size += (new_size / 2);
    bs->data = (unsigned char *)realloc(bs->data, new_size);
    bs->data_size = new_size;
  }

  for (i = 0; i < full_bytes; i++) {
    bs->data[bs->data_position++] = (unsigned char)(bs->cache >> 24);
    bs->cache <<= 8;
    bs->cache_bits += 8;
  }
}

/*
 * Layer III error protection checksum.
 *
 * 16-bit shift register, initial value 0xffff, MSB first, generator
 * polynomial 0x8005, no final exclusive-or. Each fed bit is XORed into
 * the most significant position before the feedback shift.
 */
unsigned int shine_crc16(unsigned int crc, unsigned int val, unsigned int N) {
  while (N--) {
    unsigned int bit = (val >> N) & 1;
    unsigned int msb = (crc >> 15) & 1;

    crc = (crc << 1) & 0xffff;
    if (msb ^ bit)
      crc ^= 0x8005;
  }
  return crc;
}

/*
 * shine_putbits:
 * --------
 * write N bits into the bit stream.
 * bs = bit stream structure
 * val = value to write into the buffer
 * N = number of bits of val
 */
void shine_putbits(bitstream_t *bs, unsigned int val, unsigned int N) {
#ifdef DEBUG
  if (N > 32)
    printf("Cannot write more than 32 bits at a time.\n");
  if (N < 32 && (val >> N) != 0)
    printf("Upper bits (higher than %d) are not all zeros.\n", N);
#endif

  if (bs->cache_bits > N) {
    bs->cache_bits -= N;
    bs->cache |= val << bs->cache_bits;
  } else {
    if (bs->data_position + sizeof(unsigned int) >= bs->data_size) {
      bs->data = (unsigned char *)realloc(bs->data,
                                          bs->data_size + (bs->data_size / 2));
      bs->data_size += (bs->data_size / 2);
    }

    N -= bs->cache_bits;
    bs->cache |= val >> N;
#ifdef SHINE_BIG_ENDIAN
    *(unsigned int *)(bs->data + bs->data_position) = bs->cache;
#else
    *(unsigned int *)(bs->data + bs->data_position) = SWAB32(bs->cache);
#endif
    bs->data_position += sizeof(unsigned int);
    bs->cache_bits = 32 - N;
    if (N != 0)
      bs->cache = val << bs->cache_bits;
    else
      bs->cache = 0;
  }
}

int shine_get_bits_count(bitstream_t *bs) {
  return bs->data_position * 8 + 32 - bs->cache_bits;
}
