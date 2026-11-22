#ifndef REWIND_FIXTURE_H
#define REWIND_FIXTURE_H
#include <stddef.h>
#include <vorbis/vorbisfile.h>
typedef struct {unsigned char *data;size_t size,capacity,pos;long reads;size_t short_read;} fixture;
int fixture_encode(fixture *f,int channels,int rate,int frames);
int fixture_open(fixture *f,OggVorbis_File *vf,int seekable);
void fixture_free(fixture *f);
#endif
