#include <pdb.h>
#include "image.h"
#include <stdio.h>
#include <string.h>
int main(void){
 struct task_image im;if(task_make_image(&im,0))return 1;
 FILE *file=fopen(".task-build/ordinary.pdb","wb");if(!file)return 10;
 if(fwrite(im.data,1,im.size,file)!=im.size||fclose(file))return 11;
 void *ctx=pdb_create_context(NULL,NULL);if(!ctx)return 2;
 if(pdb_load(ctx,im.data,im.size)){fprintf(stderr,"load: %s\n",pdb_strerror(ctx));return 3;}
 task_free_image(&im);
 uint32_t n=0,pubs=99,bytes=0;
 if(pdb_get_nr_symbols(ctx,&n)||pdb_get_nr_public_symbols(ctx,&pubs)||n!=3||pubs!=1)return 4;
 const PUBSYM32 *pub=pdb_lookup_public_symbol(ctx,"public_marker",true);
 if(!pub||pub->off!=0x20||pub->seg!=1||strcmp((const char *)pub->name,"public_marker"))return 12;
 const unsigned char *module=pdb_get_stream(ctx,8,&bytes);
 if(!module||bytes<64||module[0]!=4||module[6]!=0x10||module[7]!=0x11)return 5;
 uint32_t rva=0;if(pdb_convert_section_offset_to_rva(ctx,1,0x20,&rva)||rva!=0x1020)return 6;
 printf("NORMAL_OK global_records=%u publics=%u module8_bytes=%u rva=0x%x input_freed=1 lookup=public_marker\n",n,pubs,bytes,rva);
 pdb_reset_context(ctx);
 if(task_make_image(&im,1))return 7;
 int result=pdb_load(ctx,im.data,im.size);int error=pdb_errno(ctx);
 printf("NO_PSI_CURRENT load_rc=%d error=%d\n",result,error);
 if(result!=0 && (result!=-1||error!=EPDB_PARSE_ERROR))return 8;
 if(result==0){uint32_t count=99;if(pdb_get_nr_public_symbols(ctx,&count)||count!=1)return 9;}
 task_free_image(&im);pdb_destroy_context(ctx);
 printf("BASELINE_OK ordinary_load_and_refs=1 no_psi_rejected=%d\n",result==-1);return 0;
}
