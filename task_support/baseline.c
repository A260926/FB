#define PCRE2_CODE_UNIT_WIDTH 0
#include "pcre2.h"
#include <stdio.h>
int main(void){int e;PCRE2_SIZE o;uint8_t p8[]={97,43,0},t8[]={97,97,0};uint16_t p16[]={97,43,0},t16[]={97,97,0};uint32_t p32[]={97,43,0},t32[]={97,97,0};
pcre2_code_8* c8=pcre2_compile_8(p8,2,PCRE2_UTF,&e,&o,0);if(!c8)return 1;pcre2_match_data_8* d8=pcre2_match_data_create_from_pattern_8(c8,0);if(pcre2_match_8(c8,t8,2,0,0,d8,0)<1)return 2;pcre2_match_data_free_8(d8);pcre2_code_free_8(c8);
pcre2_code_16* c16=pcre2_compile_16(p16,2,PCRE2_UTF,&e,&o,0);if(!c16)return 1;pcre2_match_data_16* d16=pcre2_match_data_create_from_pattern_16(c16,0);if(pcre2_match_16(c16,t16,2,0,0,d16,0)<1)return 2;pcre2_match_data_free_16(d16);pcre2_code_free_16(c16);
pcre2_code_32* c32=pcre2_compile_32(p32,2,PCRE2_UTF,&e,&o,0);if(!c32)return 1;pcre2_match_data_32* d32=pcre2_match_data_create_from_pattern_32(c32,0);if(pcre2_match_32(c32,t32,2,0,0,d32,0)<1)return 2;pcre2_match_data_free_32(d32);pcre2_code_free_32(c32);
puts("BASELINE_OK widths=8,16,32");return 0;}
