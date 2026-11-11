#ifndef BITSTREAM_H
#define BITSTREAM_H

typedef struct bit_stream_struc {
  unsigned char *data; /* Processed data */
  int data_size;       /* Total data size */
  int data_position;   /* Data position */
  unsigned int cache;  /* bit stream cache */
  int cache_bits;      /* free bits in cache */
} bitstream_t;

/* "bit_stream.h" Definitions */

#define MINIMUM 4 /* Minimum size of the buffer in bytes */
#define MAX_LENGTH                                                             \
  32 /* Maximum length of word written or                                      \
        read from bit stream */

#define BUFFER_SIZE 4096

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

void shine_open_bit_stream(bitstream_t *bs, const int size);
void shine_close_bit_stream(bitstream_t *bs);
void shine_putbits(bitstream_t *bs, unsigned int val, unsigned int N);
int shine_get_bits_count(bitstream_t *bs);

/* Allocate a separate, initially empty bit stream. Used as scratch space
 * when Layer III error protection is enabled. Returns NULL on failure. */
bitstream_t *shine_new_bit_stream(int size);

/* Free a bit stream obtained from shine_new_bit_stream. */
void shine_free_bit_stream(bitstream_t *bs);

/* Flush every fully assembled byte, including bytes still held in the
 * 32-bit cache at a frame or stream boundary. Sub-byte bits (there are
 * none at Layer III frame boundaries) remain in the cache. */
void shine_drain_bit_stream(bitstream_t *bs);

/* Layer III CRC-16 (ISO 11172-3): 16-bit register initialised to 0xffff,
 * MSB-first feedback, polynomial 0x8005, no final exclusive-or. Feeds the
 * N lowest bits of val, most significant bit first. */
unsigned int shine_crc16(unsigned int crc, unsigned int val, unsigned int N);

#endif
