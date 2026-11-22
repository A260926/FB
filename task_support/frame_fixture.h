#ifndef FRAME_FIXTURE_H
#define FRAME_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Synthetic MPEG-1 Layer III, table-1 pair (1,0), long blocks.
 * No downloaded audio or frozen decoder output. Input construction only. */
static void fixture_bits(unsigned char *out, unsigned *pos, unsigned value, unsigned count) {
    while(count) { --count; if((value>>count)&1u) out[*pos/8]|=(unsigned char)(1u<<(7-*pos%8)); ++*pos; }
}
static unsigned fixture_crc_bit(unsigned crc, unsigned bit) {
    unsigned msb=(crc>>15)^bit;
    return ((crc<<1)^((msb&1)?0x8005u:0u))&65535u;
}
static size_t fixture_frame(unsigned char *out, unsigned rate_index, unsigned channels,
                            unsigned padding, unsigned crc_on, unsigned phase) {
    static const unsigned rates[]={44100,48000,32000};
    if(rate_index>2 || (channels!=1 && channels!=2))return 0;
    size_t size=144u*128000u/rates[rate_index]+padding;
    memset(out,0,size);
    uint32_t head=0xfffb9000u|(rate_index<<10)|(padding<<9)|((channels==1?3u:0u)<<6);
    if(crc_on)head&=~(1u<<16);
    for(int i=0;i<4;i++)out[i]=(unsigned char)(head>>(24-8*i));
    unsigned side=4+(crc_on?2:0),pos=side*8;
    fixture_bits(out,&pos,0,9); /* main_data_begin */
    fixture_bits(out,&pos,0,channels==1?5:3);
    for(unsigned ch=0;ch<channels;ch++)fixture_bits(out,&pos,0,4); /* scfsi */
    for(unsigned gr=0;gr<2;gr++)for(unsigned ch=0;ch<channels;ch++) {
        fixture_bits(out,&pos,3,12); /* part2_3_length: one pair plus sign */
        fixture_bits(out,&pos,1,9); fixture_bits(out,&pos,230,8);
        fixture_bits(out,&pos,0,4); fixture_bits(out,&pos,0,1);
        fixture_bits(out,&pos,1,5); fixture_bits(out,&pos,0,5); fixture_bits(out,&pos,0,5);
        fixture_bits(out,&pos,0,4); fixture_bits(out,&pos,0,3);
        fixture_bits(out,&pos,0,1); fixture_bits(out,&pos,0,1); fixture_bits(out,&pos,0,1);
    }
    unsigned end=side+(channels==1?17:32);
    if(pos!=end*8)return 0;
    for(unsigned gr=0;gr<2;gr++)for(unsigned ch=0;ch<channels;ch++) {
        fixture_bits(out,&pos,1,2); /* Huffman table 1: 01 => (1,0) */
        fixture_bits(out,&pos,(phase+gr+ch)&1u,1);
    }
    if(crc_on) {
        unsigned crc=65535;
        for(unsigned i=16;i<32;i++)crc=fixture_crc_bit(crc,(head>>(31-i))&1u);
        for(unsigned i=side*8;i<end*8;i++)crc=fixture_crc_bit(crc,(out[i/8]>>(7-i%8))&1u);
        out[4]=(unsigned char)(crc>>8);out[5]=(unsigned char)crc;
    }
    return size;
}
#endif
