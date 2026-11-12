#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
extern void checks_disk_init(void);
#define CHECK(x) do{if(!(x)){fprintf(stderr,"baseline failed line %d\n",__LINE__);return 1;}}while(0)
int main(void){
 FATFS fs;FIL file;DIR dir;FILINFO info;UINT n;BYTE work[4096],out[16];
 const MKFS_PARM params={FM_EXFAT|FM_SFD,1,0,0,4096};
 const char *name="0:/evidence-long-name.bin";
 checks_disk_init();
 CHECK(f_mkfs("0:",&params,work,sizeof(work))==FR_OK);
 CHECK(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_EXFAT);
 CHECK(f_open(&file,name,FA_CREATE_ALWAYS|FA_WRITE)==FR_OK);
 CHECK(f_write(&file,"EVIDENCE-CONTENT",16,&n)==FR_OK && n==16);
 CHECK(f_close(&file)==FR_OK);
 CHECK(f_open(&file,name,FA_READ)==FR_OK);
 CHECK(f_read(&file,out,16,&n)==FR_OK && n==16 && memcmp(out,"EVIDENCE-CONTENT",16)==0);
 CHECK(f_close(&file)==FR_OK);
 CHECK(f_unlink(name)==FR_OK);
 CHECK(f_opendir(&dir,"0:")==FR_OK);
 CHECK(f_readdir(&dir,&info)==FR_OK && info.fname[0]==0);
 CHECK(f_closedir(&dir)==FR_OK);
 CHECK(f_mount(NULL,"0:",0)==FR_OK);
 puts("EXFAT_BASELINE_CREATE_READ_DELETE_OK");return 0;
}
