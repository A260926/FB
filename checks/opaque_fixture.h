#ifndef OPAQUE_FIXTURE_H
#define OPAQUE_FIXTURE_H
#include <string.h>
#include <libexif/exif-utils.h>
/* Independently laid out TIFF: note at128 and its internal pointer to136. */
static void make_opaque_fixture(unsigned char out[150],ExifByteOrder o){
 memset(out,0,150);memcpy(out,"Exif\0\0",6);memcpy(out+6,o==EXIF_BYTE_ORDER_INTEL?"II":"MM",2);
 exif_set_short(out+8,o,42);exif_set_long(out+10,o,8);
 exif_set_short(out+14,o,1);exif_set_short(out+16,o,0x8769);exif_set_short(out+18,o,4);exif_set_long(out+20,o,1);exif_set_long(out+24,o,26);
 exif_set_short(out+32,o,1);exif_set_short(out+34,o,0x927c);exif_set_short(out+36,o,7);exif_set_long(out+38,o,16);exif_set_long(out+42,o,128);
 memcpy(out+134,"UFN0",4);exif_set_long(out+138,o,136);memcpy(out+142,"PIXEL123",8);
}
#endif
