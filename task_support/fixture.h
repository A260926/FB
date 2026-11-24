#ifndef CAPTION_FIXTURE_H
#define CAPTION_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
/* Metadata-only AVC/FLV source: no compressed picture or decoder is required. */
int caption_fixture_nal(uint8_t **nal,size_t *size);
int caption_fixture_write(const char *path);
#endif
