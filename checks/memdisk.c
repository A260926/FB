#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
static unsigned char *disk;
static const LBA_t sectors=131072;
void checks_disk_init(void){free(disk);disk=calloc((size_t)sectors,512);if(!disk)abort();}
DSTATUS disk_initialize(BYTE p){return p||!disk?STA_NOINIT:0;}
DSTATUS disk_status(BYTE p){return disk_initialize(p);}
DRESULT disk_read(BYTE p,BYTE *b,LBA_t s,UINT n){if(p||s>sectors||n>sectors-s)return RES_PARERR;memcpy(b,disk+(size_t)s*512,(size_t)n*512);return RES_OK;}
DRESULT disk_write(BYTE p,const BYTE *b,LBA_t s,UINT n){if(p||s>sectors||n>sectors-s)return RES_PARERR;memcpy(disk+(size_t)s*512,b,(size_t)n*512);return RES_OK;}
DRESULT disk_ioctl(BYTE p,BYTE c,void *b){if(p)return RES_PARERR;switch(c){case CTRL_SYNC:return RES_OK;case GET_SECTOR_COUNT:*(LBA_t*)b=sectors;return RES_OK;case GET_BLOCK_SIZE:*(DWORD*)b=1;return RES_OK;case GET_SECTOR_SIZE:*(WORD*)b=512;return RES_OK;default:return RES_PARERR;}}
