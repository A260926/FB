#include "macho.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Benign data only. No code is stored or executed. */
int main(void) {
  size_t len=4096; unsigned char *bytes=calloc(1,len);
  if(!bytes)return 2;
  struct mach_header_64 *h=(void*)bytes;
  h->magic=MH_MAGIC_64;h->cputype=CPU_TYPE_X86_64;h->cpusubtype=CPU_SUBTYPE_X86_64_ALL;
  h->filetype=MH_DYLIB;h->ncmds=1;h->sizeofcmds=sizeof(struct segment_command_64);
  struct segment_command_64 *s=(void*)(h+1);
  s->cmd=LC_SEGMENT_64;s->cmdsize=sizeof(*s);strcpy(s->segname,"__TEXT");
  s->vmaddr=(uintptr_t)bytes;s->vmsize=len;s->filesize=len;s->initprot=VM_PROT_READ;s->maxprot=VM_PROT_READ;
  mk_memory_map_self_t map;mk_macho_t image;
  mk_error_t e=mk_memory_map_self_init(NULL,&map);
  if(!e)e=mk_macho_init_with_slide(NULL,"synthetic-data",0,(uintptr_t)bytes,&map,&image);
  if(e){fprintf(stderr,"macho init error=%d\n",e);return 3;}
  struct load_command *lc=mk_macho_next_command_type(&image,NULL,LC_SEGMENT_64,NULL);
  mk_segment_t seg;e=mk_segment_init_with_mach_load_command(&image,(void*)lc,&seg);
  if(e)return 4;
  if(mk_memory_object_length(mk_segment_get_mapping(&seg))<len)return 5;
  printf("BASELINE_OK header64=%d segment=%s requested=4096\n",mk_macho_is_64_bit(&image),s->segname);
  mk_segment_free(&seg);mk_macho_free(&image);free(bytes);return 0;
}
