#ifndef SAMPLE_FIXTURE_H
#define SAMPLE_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
size_t fixture_elf(unsigned char *out, size_t capacity, int bits, int big_endian, uint64_t value);
size_t fixture_fat(unsigned char *out, size_t capacity);
size_t fixture_archive(unsigned char *out, size_t capacity, int nested_fat);
#endif
