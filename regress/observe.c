#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include "dvbpsi.h"
#include "descriptor.h"
#include "descriptors/dr_4e.h"
int main(void){
 uint8_t text[]={0x15,'h','i'};
 dvbpsi_extended_event_dr_t in={0};
 in.i_iso_639_code[0]='e';in.i_iso_639_code[1]='n';in.i_iso_639_code[2]='g';
 in.i_text_length=sizeof text;in.i_text=text;
 dvbpsi_descriptor_t *d=dvbpsi_GenExtendedEventDr(&in,false);assert(d);
 printf("current extended text wire: items_length=%u text_length_slot=%u (expected 0,3)\n",d->p_data[4],d->p_data[5]);
 dvbpsi_DeleteDescriptors(d);return 0;
}
