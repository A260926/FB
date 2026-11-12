#include "caption/cea708.h"
#include "caption/eia608.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    caption_frame_t f;
    cea708_t c, parsed;
    unsigned char wire[256];
    caption_frame_init(&f);
    cea708_init(&c, 1.0);
    assert(cea708_add_cc_data(&c,1,cc_type_ntsc_cc_field_1,
      eia608_control_command(eia608_control_resume_direct_captioning,0)));
    assert(cea708_add_cc_data(&c,1,cc_type_ntsc_cc_field_1,eia608_parity(0x4142)));
    int n=cea708_render(&c,wire,sizeof wire);
    cea708_init(&parsed,1.0);
    assert(cea708_parse_h264(wire,n,&parsed)==LIBCAPTION_OK);
    assert(cea708_to_caption_frame(&f,&parsed)!=LIBCAPTION_ERROR);
    assert(!strcmp(caption_frame_read_char(&f,14,0,0,0),"A"));
    assert(!strcmp(caption_frame_read_char(&f,14,1,0,0),"B"));
    puts("BASELINE_608_GA94_AB_PASS");
    return 0;
}
