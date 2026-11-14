#include "sample_io.h"
#include <inttypes.h>
int main(int argc,char **argv){
 if(argc!=2)return 1;pe_ctx_t c;if(sample_open(&c,argv[1]))return 2;
 IMAGE_DATA_DIRECTORY *d=pe_directory_by_entry(&c,IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR);
 if(!d||d->VirtualAddress!=0x2000||d->Size!=72)return 3;
 uint64_t clr=pe_rva2ofs(&c,d->VirtualAddress),md=pe_rva2ofs(&c,0x2100),body=pe_rva2ofs(&c,0x4ff8),tail=pe_rva2ofs(&c,0x5004);
 if(clr!=0x400||md!=0x500||body!=0x1ff8||tail!=0x3004)return 4;
 if(memcmp((unsigned char*)c.map_addr+md,"BSJB",4))return 5;
 printf("LIBPE_CLR_RAW_OK sections=%u clr_ofs=%"PRIu64" metadata_ofs=%"PRIu64" fat_header_ofs=%"PRIu64" il_ofs=%"PRIu64"\n",c.pe.num_sections,clr,md,body,tail);
 sample_close(&c);return 0;
}
