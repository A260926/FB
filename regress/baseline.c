#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "dvbpsi.h"
#include "descriptor.h"
#include "psi.h"
#include "tables/eit.h"
#include "descriptors/dr_4e.h"
int main(void) {
 uint8_t raw[]={0x00,'e','n','g',0,3,0x15,'h','i'};
 dvbpsi_descriptor_t *d=dvbpsi_NewDescriptor(0x4e,sizeof raw,raw); assert(d);
 dvbpsi_extended_event_dr_t *v=dvbpsi_DecodeExtendedEventDr(d);
 assert(v && v->i_text_length==3 && !memcmp(v->i_text,raw+6,3));
 dvbpsi_t *h=dvbpsi_new(NULL,DVBPSI_MSG_NONE); assert(h);
 dvbpsi_eit_t *e=dvbpsi_eit_new(0x4e,7,1,true,8,9,0,0x4e); assert(e);
 dvbpsi_eit_event_t *ev=dvbpsi_eit_event_add(e,42,0xea60000000ULL,0x010203,4,false,0); assert(ev);
 assert(dvbpsi_eit_event_descriptor_add(ev,0x4e,sizeof raw,raw));
 dvbpsi_psi_section_t *s=dvbpsi_eit_sections_generate(h,e,0x4e); assert(s && !s->p_next);
 assert(dvbpsi_ValidPSISection(s));
 assert(s->p_data[14]==0 && s->p_data[15]==42 && s->p_data[26]==0x4e);
 assert(!memcmp(s->p_data+28,raw,sizeof raw));
 puts("seed DVB EIT extended descriptor: ok");
 dvbpsi_DeletePSISections(s);dvbpsi_eit_delete(e);dvbpsi_delete(h);dvbpsi_DeleteDescriptors(d);
 return 0;
}
