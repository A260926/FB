/* Read-only example: scan an exFAT directory of an offline image for deleted
/  file candidates and print their recovery status plus a little of their
/  content.
/
/  The recovery calls shown here (f_exfat_recovery_scan / f_exfat_recovery_read)
/  only read the image: they never call disk_write(), never touch the allocation
/  bitmap and never modify a directory entry. Normal FatFs calls (f_readdir,
/  f_open, ...) keep their usual meaning.
/
/  The first half of main() merely builds a throw-away demo image (f_mkfs,
/  f_write, f_unlink) so the example has a deleted file to report; that setup is
/  not part of the read-only analysis.
/
/  Build (with the in-repo memory disk used by the checks):
/
/      cc -std=c99 -I include -I checks -DZEPHYR_CONFIG_OVERRIDE=ffconf_override.h \
/         ff.c option/ffunicode.c checks/memdisk.c examples/exfat_recovery_example.c
/
/  See the contract documented in ff.h before relying on recovered bytes. A
/  candidate being "valid" means its clusters are currently free, not that the
/  bytes were never overwritten after the deletion.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ff.h"
#include "diskio.h"

extern void checks_disk_init(void);

/* Callback: print one candidate and, when readable, fetch the first 64 bytes. */

static void report(void *user, const FF_RECOVERY_INFO *rec)
{
	unsigned char head[64];
	QWORD got = 0;
	UINT i;

	(void)user;
	printf("offset=%lu state=%d diag=%d size=%llu valid=%llu name=",
		(unsigned long)rec->ofs, rec->state, rec->diag,
		(unsigned long long)rec->data_len, (unsigned long long)rec->valid_len);

	/* The name is UTF-16; this example prints the ASCII subset. */
	for (i = 0; i < rec->name_len; i++) {
		WCHAR ch = rec->name[i];
		putchar((ch >= 0x20 && ch < 0x7F) ? (char)ch : '?');
	}
	putchar('\n');

	if (rec->state != FF_RECOVERY_STATE_VALID || (rec->attr & AM_DIR)) {
		printf("  (content not readable in this state)\n");
		return;
	}
	if (f_exfat_recovery_read(rec, head, 0, sizeof head, &got) != FR_OK) {
		printf("  read error after %llu bytes\n", (unsigned long long)got);
		return;
	}
	printf("  first %llu bytes:", (unsigned long long)got);
	for (i = 0; i < got; i++) printf(" %02X", head[i]);
	putchar('\n');
}

int main(void)
{
	FATFS fs;
	DIR dir;
	UINT ncand = 0;
	FRESULT res;
	MKFS_PARM par = {FM_EXFAT | FM_SFD, 1, 0, 0, 4096};
	BYTE work[4096];
	FIL f;
	UINT bw;

	/* --- Prepare a throw-away image with one deleted file. --- */
	checks_disk_init();
	res = f_mkfs("0:", &par, work, sizeof work);
	if (res != FR_OK) { printf("mkfs: %d\n", res); return 1; }
	res = f_mount(&fs, "0:", 1);
	if (res != FR_OK) { printf("mount: %d\n", res); return 1; }
	if (f_open(&f, "0:/evidence.bin", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
		f_write(&f, "evidence bytes", 14, &bw);
		f_close(&f);
		f_unlink("0:/evidence.bin");
	}

	/* --- Scan the directory. The DIR read position is left untouched. --- */
	res = f_opendir(&dir, "0:");
	if (res != FR_OK) { printf("opendir: %d\n", res); return 1; }
	res = f_exfat_recovery_scan(&dir, report, 0, &ncand);
	f_closedir(&dir);
	if (res != FR_OK) { printf("scan: %d\n", res); return 1; }
	printf("candidates: %u\n", ncand);

	f_mount(0, "0:", 0);
	return 0;
}
