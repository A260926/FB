#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
static unsigned char *disk;
static const LBA_t sectors=131072;

/* Test instrumentation. Defaults keep the plain in-memory disk behaviour. */
static int fail_reads;			/* Fail every disk_read() while non-zero */
static LBA_t fail_lo, fail_hi;	/* Fail reads of sectors in [fail_lo, fail_hi) when fail_hi > fail_lo */
unsigned long checks_disk_writes;	/* Number of disk_write() calls */
unsigned long checks_disk_reads;	/* Number of disk_read() calls */

void checks_disk_init(void){free(disk);disk=calloc((size_t)sectors,512);if(!disk)abort();fail_reads=0;fail_lo=fail_hi=0;checks_disk_writes=0;checks_disk_reads=0;}
void checks_disk_set_fail_reads(int on){fail_reads=on;}
void checks_disk_fail_range(LBA_t lo,LBA_t hi){fail_lo=lo;fail_hi=(hi>lo)?hi:lo;}
void checks_disk_clear_fail_range(void){fail_lo=fail_hi=0;}
unsigned char *checks_disk_data(void){return disk;}
LBA_t checks_disk_sectors(void){return sectors;}

DSTATUS disk_initialize(BYTE p){return p||!disk?STA_NOINIT:0;}
DSTATUS disk_status(BYTE p){return disk_initialize(p);}
DRESULT disk_read(BYTE p,BYTE *b,LBA_t s,UINT n){
	if(p||!disk||s>sectors||n>sectors-s)return RES_PARERR;
	checks_disk_reads++;
	if(fail_reads)return RES_ERROR;
	if(fail_hi>fail_lo&&s>=fail_lo&&s<fail_hi)return RES_ERROR;
	memcpy(b,disk+(size_t)s*512,(size_t)n*512);return RES_OK;
}
DRESULT disk_write(BYTE p,const BYTE *b,LBA_t s,UINT n){
	checks_disk_writes++;
	if(p||!disk||s>sectors||n>sectors-s)return RES_PARERR;
	memcpy(disk+(size_t)s*512,b,(size_t)n*512);return RES_OK;
}
DRESULT disk_ioctl(BYTE p,BYTE c,void *b){if(p||!disk)return RES_PARERR;switch(c){case CTRL_SYNC:return RES_OK;case GET_SECTOR_COUNT:*(LBA_t*)b=sectors;return RES_OK;case GET_BLOCK_SIZE:*(DWORD*)b=1;return RES_OK;case GET_SECTOR_SIZE:*(WORD*)b=512;return RES_OK;default:return RES_PARERR;}}
