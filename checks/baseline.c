#include <libexif/exif-data.h>
#include "opaque_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
int main(void){unsigned char input[150];make_opaque_fixture(input,EXIF_BYTE_ORDER_INTEL);
 ExifData *d=exif_data_new();assert(d);exif_data_unset_option(d,EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);exif_data_set_option(d,EXIF_DATA_OPTION_DONT_CHANGE_MAKER_NOTE);exif_data_load_data(d,input,sizeof input);
 ExifEntry *e=exif_content_get_entry(d->ifd[EXIF_IFD_EXIF],EXIF_TAG_MAKER_NOTE);assert(e&&e->size==16&&!memcmp(e->data,"UFN0",4));
 unsigned char *out=0;unsigned int size=0;exif_data_save_data(d,&out,&size);assert(out&&size>40);
 unsigned int ifd=exif_get_long(out+24,EXIF_BYTE_ORDER_INTEL);unsigned int saved=exif_get_long(out+6+ifd+10,EXIF_BYTE_ORDER_INTEL);
 assert(saved!=128);assert(!memcmp(out+6+saved,e->data,16));
 printf("OPAQUE_NOTE_BASELINE_OK original=128 saved=%u bytes=%u\n",saved,size);
 ExifData *round=exif_data_new();exif_data_unset_option(round,EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);exif_data_load_data(round,out,size);assert(exif_content_get_entry(round->ifd[EXIF_IFD_EXIF],EXIF_TAG_MAKER_NOTE));
 exif_data_unref(round);free(out);exif_data_unref(d);return 0;}
