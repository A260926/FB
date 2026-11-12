/* exFAT deleted-entry recovery tests.
/
/  Groups: sets, allocation, reads, isolation.
/  Every group formats a real exFAT volume with f_mkfs() on the in-repo memory
/  disk; the `sets` group also goes through f_write()/f_unlink() for genuine
/  deletions. Controlled corruptions are then applied directly to the raw
/  image (never through the public API) so that the scanner's diagnostics can
/  be asserted.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

extern void checks_disk_init(void);
extern void checks_disk_set_fail_reads(int on);
extern void checks_disk_fail_range(LBA_t lo, LBA_t hi);
extern void checks_disk_clear_fail_range(void);
extern unsigned char *checks_disk_data(void);
extern LBA_t checks_disk_sectors(void);
extern unsigned long checks_disk_writes;
extern unsigned long checks_disk_reads;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return -1; } } while (0)
#define AS(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return -1; } n++; } while (0)

#define MAXC 16
#define CSIZE 4096		/* Cluster size used by all tests */

/* ------------------------------------------------------------------ */
/* Raw image access                                                    */
/* ------------------------------------------------------------------ */

#define SSN 512

static DWORD ld32(const unsigned char *p) { return (DWORD)(p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24)); }
static void st16(unsigned char *p, WORD v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void st32(unsigned char *p, DWORD v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void st64(unsigned char *p, QWORD v) { st32(p, (DWORD)v); st32(p + 4, (DWORD)(v >> 32)); }

typedef struct {
	unsigned char *base;
	LBA_t fatbase, database, bitbase;
	LBA_t root_sect;
	DWORD csize, n_fatent, root_clst;
	DWORD bitclst, bitsz, upclst;
	QWORD uptlen;
} IMG;

static IMG img;

static unsigned char *secp(LBA_t s) { return img.base + (size_t)s * SSN; }
static unsigned char *clp(DWORD c) { return secp(img.database + (LBA_t)img.csize * (c - 2)); }

static int img_load(void)
{
	unsigned char *b;
	DWORD rofs;
	img.base = checks_disk_data();
	img.fatbase = ld32(img.base + 80);
	img.database = ld32(img.base + 88);
	img.root_clst = ld32(img.base + 96);
	img.csize = 1u << img.base[109];
	img.n_fatent = ld32(img.base + 92) + 2;
	img.root_sect = img.database + (LBA_t)img.csize * (img.root_clst - 2);
	/* Root directory entries 0..2: volume label, bitmap, up-case table. */
	b = secp(img.root_sect);
	if (b[1 * 32] != 0x81 || b[2 * 32] != 0x82) return -1;
	img.bitclst = ld32(b + 1 * 32 + 20);
	img.bitsz = ld32(b + 1 * 32 + 24);
	img.upclst = ld32(b + 2 * 32 + 20);
	img.uptlen = ld32(b + 2 * 32 + 24);
	img.bitbase = img.database + (LBA_t)img.csize * (img.bitclst - 2);
	(void)rofs;
	return 0;
}

static DWORD fat_get(DWORD c) { return ld32(secp(img.fatbase + c * 4 / SSN) + c * 4 % SSN); }
static void fat_set(DWORD c, DWORD v) { st32(secp(img.fatbase + c * 4 / SSN) + c * 4 % SSN, v); }

static int bit_get(DWORD c)
{
	DWORD bit = c - 2;
	return (secp(img.bitbase)[bit / 8] >> (bit % 8)) & 1;
}
static void bit_set(DWORD c, int on)
{
	DWORD bit = c - 2;
	unsigned char *p = secp(img.bitbase) + bit / 8;
	if (on) p[0] |= (unsigned char)(1 << (bit % 8));
	else p[0] &= (unsigned char)~(1 << (bit % 8));
}

static DWORD find_free(DWORD from, DWORD skip)
{
	DWORD c;
	for (c = from; c < img.n_fatent; c++) {
		if (!bit_get(c) && !(skip && c == skip)) return c;
	}
	return 0;
}

/* Restart FatFs so that raw image edits are visible after a cached window. */
static FRESULT remount(FATFS *fs)
{
	FRESULT res = f_mount(0, "0:", 0);
	if (res != FR_OK) return res;
	return f_mount(fs, "0:", 1);
}

/* ------------------------------------------------------------------ */
/* Synthetic deleted entry sets                                        */
/* ------------------------------------------------------------------ */

static WORD name_hash_ascii(const WCHAR *name, UINT nlen)
{
	UINT i;
	WORD sum = 0;
	for (i = 0; i < nlen; i++) {
		WORD ch = name[i];
		if (ch >= 'a' && ch <= 'z') ch = (WORD)(ch - 32);
		sum = (WORD)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (ch & 0xFF));
		sum = (WORD)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (ch >> 8));
	}
	return sum;
}

/* Compute the set checksum over the entry set with InUse restored, which is
/  what the on-disk SetChecksum holds for a set that was later deleted. */

static void set_checksum(unsigned char *set, UINT nsec)
{
	UINT i, sz = (nsec + 1) * 32;
	WORD sum = 0;
	for (i = 0; i < sz; i++) {
		BYTE b = set[i];
		if (i == 2 || i == 3) continue;
		if (i % 32 == 0) b |= 0x80;			/* Restore the InUse bit */
		sum = (WORD)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + b);
	}
	st16(set + 2, sum);
}

static unsigned char *dir_map(DWORD first, DWORD ofs);
static void dir_raw_write(DWORD first, DWORD ofs, const void *data, DWORD len);

/* Append (not insert) a deleted entry set at a byte offset of a directory
/  whose first cluster is `dclst`. Caller ensures the space is free/zero. */

static int put_deleted_set(DWORD dclst, DWORD ofs, const WCHAR *name, UINT nlen,
	DWORD first, QWORD dlen, QWORD vlen, BYTE attrib, BYTE genflags)
{
	UINT n1 = (nlen + 14) / 15, sec = 1 + n1, i, j, k;
	unsigned char *set = calloc(1, (sec + 1) * 32);
	if (!set) return -1;
	set[0] = 0x05;					/* InUse cleared File entry */
	set[1] = (BYTE)sec;
	st16(set + 4, (WORD)(attrib | 0x20));
	st64(set + 56, dlen);
	set[32] = 0x40;					/* InUse cleared stream extension */
	set[33] = genflags;				/* GeneralSecondaryFlags (block offset 33) */
	set[35] = (BYTE)nlen;			/* NameLength */
	st64(set + 40, vlen);			/* ValidDataLength */
	st32(set + 52, first);			/* FirstCluster */
	st64(set + 56, dlen);			/* DataLength */
	st16(set + 36, name_hash_ascii(name, nlen));
	j = 0;
	for (i = 0; i < n1; i++) {
		unsigned char *e = set + (2 + i) * 32;
		e[0] = 0x41;
		for (k = 2; k < 32; k += 2) st16(e + k, (j < nlen) ? name[j++] : 0);
	}
	set_checksum(set, sec);
	for (i = 0; i <= sec; i++) set[i * 32] &= 0x7F;

	dir_raw_write(dclst, ofs, set, (sec + 1) * 32);
	free(set);
	return (int)((sec + 1) * 32);
}

/* Map a byte offset in a (possibly multi-cluster) directory chain to a byte
/  pointer. Returns NULL when the chain ends before the offset. */

static unsigned char *dir_map(DWORD first, DWORD ofs)
{
	DWORD c = first, csb = img.csize * SSN;
	while (ofs >= csb) {
		DWORD nxt = fat_get(c);
		if (nxt >= 0x7FFFFFF8 || nxt < 2 || nxt >= img.n_fatent) return 0;
		c = nxt; ofs -= csb;
	}
	return clp(c) + ofs;
}

static void dir_raw_write(DWORD first, DWORD ofs, const void *data, DWORD len)
{
	const unsigned char *s = (const unsigned char *)data;
	DWORD csb = img.csize * SSN;
	while (len) {
		unsigned char *p = dir_map(first, ofs);
		DWORD n = csb - (ofs % csb);
		if (!p) return;
		if (n > len) n = len;
		memcpy(p, s, n);
		s += n; ofs += n; len -= n;
	}
}

/* Fill directory slots so that no end-of-directory marker precedes `to`.
/  Entry type 0x03 is ignored by the scanner (not a deleted file entry). */

static void fill_gap(DWORD dclst, DWORD from, DWORD to)
{
	DWORD csb = img.csize * SSN;
	unsigned char *p = dir_map(dclst, from);
	while (p && from < to) {
		if (p[0] == 0) p[0] = 0x03;
		from += 32; p += 32;
		if ((from % csb) == 0) p = dir_map(dclst, from);
	}
}

/* ------------------------------------------------------------------ */
/* Candidate collection                                                */
/* ------------------------------------------------------------------ */

typedef struct { FF_RECOVERY_INFO rec; } CAP;
typedef struct { CAP c[MAXC]; int n; } CAPS;

static void collect(void *user, const FF_RECOVERY_INFO *rec)
{
	CAPS *cs = (CAPS *)user;
	if (cs->n < MAXC) memcpy(&cs->c[cs->n++].rec, rec, sizeof *rec);
}

static int name_eq(const WCHAR *a, const char *b)
{
	while (*b) { if (*a != (WCHAR)(unsigned char)*b) return 0; a++; b++; }
	return *a == 0;
}

static CAP *find_name(CAPS *cs, const char *name)
{
	int i;
	for (i = 0; i < cs->n; i++) if (name_eq(cs->c[i].rec.name, name)) return &cs->c[i];
	return 0;
}

static CAP *find_ofs(CAPS *cs, DWORD ofs)
{
	int i;
	for (i = 0; i < cs->n; i++) if (cs->c[i].rec.ofs == ofs) return &cs->c[i];
	return 0;
}

static int scan_root(CAPS *cs, UINT *nc)
{
	DIR dir;
	int res;
	memset(cs, 0, sizeof *cs);
	if (f_opendir(&dir, "0:") != FR_OK) return -100;
	res = (int)f_exfat_recovery_scan(&dir, collect, cs, nc);
	f_closedir(&dir);
	return res;
}

/* ------------------------------------------------------------------ */
/* Common setup                                                        */
/* ------------------------------------------------------------------ */

static FRESULT fresh_volume(FATFS *fs)
{
	BYTE work[4096];
	MKFS_PARM par;
	checks_disk_init();
	memset(&par, 0, sizeof par);
	par.fmt = FM_EXFAT | FM_SFD;
	par.n_fat = 1;
	par.au_size = CSIZE;
	if (f_mkfs("0:", &par, work, sizeof work) != FR_OK) return FR_MKFS_ABORTED;
	if (f_mount(fs, "0:", 1) != FR_OK) return FR_NOT_READY;
	if (img_load() != 0) return FR_INT_ERR;
	return FR_OK;
}

static int write_file(const char *path, const void *data, UINT len)
{
	FIL f; UINT bw;
	if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
	if (len && (f_write(&f, data, len, &bw) != FR_OK || bw != len)) { f_close(&f); return -1; }
	return f_close(&f);
}

/* ================================================================== */
/* Group: sets                                                         */
/* ================================================================== */

static int test_sets(void)
{
	FATFS fs;
	CAPS cs;
	UINT nc, n = 0;
	const char *longname = "abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789-long.txt";
	char path[128];
	CAP *c;

	CHECK(fresh_volume(&fs) == FR_OK && fs.fs_type == FS_EXFAT);

	/* --- Real f_mkfs / f_write / f_unlink --- */
	strcpy(path, "0:/");
	strcat(path, longname);
	CHECK(write_file(path, "SETS-CONTENT", 12) == 0);
	CHECK(f_unlink(path) == FR_OK);
	AS(scan_root(&cs, &nc) == 0);
	AS(nc == 1);
	c = &cs.c[0];
	AS(c->rec.state == FF_RECOVERY_STATE_VALID);
	AS(c->rec.diag == FF_RECOVERY_DIAG_NONE);
	AS(c->rec.name_len == (WORD)strlen(longname));
	AS(name_eq(c->rec.name, longname));
	AS(c->rec.data_len == 12 && c->rec.valid_len == 12);
	AS(c->rec.first_clst >= 2 && c->rec.first_clst < img.n_fatent);
	AS(c->rec.ofs > 2 * 32);		/* After the three system entries */
	{
		BYTE b[16]; QWORD got;
		AS(f_exfat_recovery_read(&c->rec, b, 0, 12, &got) == FR_OK && got == 12);
		AS(memcmp(b, "SETS-CONTENT", 12) == 0);
	}
	/* Repeat scan: same identity, state and diagnostics. */
	{
		CAPS cs2; UINT nc2;
		AS(scan_root(&cs2, &nc2) == 0 && nc2 == 1);
		AS(cs2.c[0].rec.ofs == c->rec.ofs);
		AS(cs2.c[0].rec.state == c->rec.state && cs2.c[0].rec.diag == c->rec.diag);
		AS(cs2.c[0].rec.first_clst == c->rec.first_clst);
	}

	/* --- Zero-length file --- */
	CHECK(write_file("0:/empty.bin", 0, 0) == 0);
	CHECK(f_unlink("0:/empty.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0);
	c = find_name(&cs, "empty.bin");
	AS(c != 0);
	AS(c->rec.state == FF_RECOVERY_STATE_VALID && c->rec.diag == FF_RECOVERY_DIAG_NONE);
	AS(c->rec.data_len == 0 && c->rec.n_clust == 0);
	{
		BYTE b[4]; QWORD got;
		AS(f_exfat_recovery_read(&c->rec, b, 0, 4, &got) == FR_OK && got == 0);
	}

	/* --- Live entry is invisible (created before the deletions so that its
	   entry set does not reuse a freed slot) --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/live.bin", "LIVE", 4) == 0);
	CHECK(write_file("0:/gone1.bin", "G1", 2) == 0);
	CHECK(write_file("0:/gone2.bin", "G2", 2) == 0);
	CHECK(f_unlink("0:/gone1.bin") == FR_OK);
	CHECK(f_unlink("0:/gone2.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0);
	AS(cs.n == 2 && find_name(&cs, "live.bin") == 0);
	AS(find_name(&cs, "gone1.bin") != 0 && find_name(&cs, "gone2.bin") != 0);

	/* --- Whole deleted directory is classified only --- */
	CHECK(f_mkdir("0:/dead-dir") == FR_OK);
	CHECK(f_unlink("0:/dead-dir") == FR_OK);
	AS(scan_root(&cs, &nc) == 0);
	c = find_name(&cs, "dead-dir");
	AS(c != 0);
	AS(c->rec.state == FF_RECOVERY_STATE_DIR && (c->rec.attr & AM_DIR));
	AS(c->rec.readable_len == 0);

	/* --- Cross-sector set: entry set starts at offset 480, 5 entries --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR nm[120];
		const char *want = "sector-span-abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrst";
		UINT i;
		for (i = 0; want[i]; i++) nm[i] = (WCHAR)want[i];
		nm[i] = 0;
		AS(i == 106);					/* 8 C1 entries, crossing sectors */
		{
			DWORD dcl = find_free(2, 0);
			AS(dcl != 0);
			fill_gap(img.root_clst, 3 * 32, 480);
			AS(put_deleted_set(img.root_clst, 480, nm, i, dcl, 10, 10, 0x20, 2) > 0);
			AS(remount(&fs) == FR_OK);
			AS(scan_root(&cs, &nc) == 0 && nc == 1);
			AS(cs.c[0].rec.ofs == 480);
			AS(cs.c[0].rec.state == FF_RECOVERY_STATE_VALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_NONE);
			AS(cs.c[0].rec.name_len == 106);
			AS(name_eq(cs.c[0].rec.name, want));
		}
	}

	/* --- Cross directory-cluster set: 4 entries start at 4064 --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR nm[20] = {'c','l','u','s','t','e','r','-','s','p','a','n','-','a','b','c','d','e',0};
		DWORD dcl = find_free(2, 0);
		DWORD tail = find_free(dcl + 1, 0);
		AS(dcl != 0 && tail != 0);
		bit_set(tail, 1);					/* Attach second root cluster */
		fat_set(img.root_clst, tail);
		fat_set(tail, 0xFFFFFFFF);
		fill_gap(img.root_clst, 3 * 32, 4064);
		AS(put_deleted_set(img.root_clst, 4064, nm, 18, dcl, 10, 10, 0x20, 2) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.ofs == 4064);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_VALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_NONE);
		AS(name_eq(cs.c[0].rec.name, "cluster-span-abcde"));
	}

	/* --- Name hash checked against the volume up-case table (non-ASCII) --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		/* "é" is U+00E9, up-cased to U+00C9 by the volume table. */
		WCHAR nm[6];
		CHECK(write_file("0:/\xC3\xA9.bin", "UNI", 3) == 0);	/* UTF-8 é */
		CHECK(f_unlink("0:/\xC3\xA9.bin") == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		c = &cs.c[0];
		AS(c->rec.state == FF_RECOVERY_STATE_VALID && c->rec.diag == FF_RECOVERY_DIAG_NONE);
		AS(c->rec.name_len == 5);
		nm[0] = 0xE9; nm[1] = '.'; nm[2] = 'b'; nm[3] = 'i'; nm[4] = 'n'; nm[5] = 0;
		AS(memcmp(c->rec.name, nm, 6 * sizeof(WCHAR)) == 0);
	}

	/* --- Damage: a live secondary inside a deleted set --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/mix.bin", "MIX", 3) == 0);
	CHECK(f_unlink("0:/mix.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	{
		DWORD ofs = cs.c[0].rec.ofs;
		clp(img.root_clst)[ofs + 32] |= 0x80;	/* Stream extension InUse set */
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, ofs);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID);
		AS(c->rec.diag == FF_RECOVERY_DIAG_LIVE);
	}

	/* --- Damage: SetChecksum mismatch after restoring InUse --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/sum.bin", "SUM", 3) == 0);
	CHECK(f_unlink("0:/sum.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	{
		DWORD ofs = cs.c[0].rec.ofs;
		clp(img.root_clst)[ofs + 2] ^= 0x55;
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, ofs);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID);
		AS(c->rec.diag == FF_RECOVERY_DIAG_CHECKSUM);
		AS(c->rec.readable_len == 0);
	}

	/* --- Damage: NameHash mismatch (checksum recomputed to stay valid) --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/hash.bin", "HASH", 4) == 0);
	CHECK(f_unlink("0:/hash.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	{
		DWORD ofs = cs.c[0].rec.ofs;
		unsigned char *d = clp(img.root_clst) + ofs;
		d[36] ^= 0xFF;				/* Break NameHash */
		d[0] &= 0x7F; d[32] &= 0x7F; d[64] &= 0x7F;	/* Keep the set deleted */
		set_checksum(d, 2);			/* Recompute while InUse is clear */
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, ofs);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID);
		AS(c->rec.diag == FF_RECOVERY_DIAG_NAME_HASH);
	}

	/* --- Damage: NameLength/entry-count mismatch --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/nlen.bin", "NLEN", 4) == 0);
	CHECK(f_unlink("0:/nlen.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	{
		DWORD ofs = cs.c[0].rec.ofs;
		unsigned char *d = clp(img.root_clst) + ofs;
		d[35] = 40;					/* Claims 40 chars, only 8 stored */
		d[1] = 2;					/* SecondaryCount still 2 */
		d[0] &= 0x7F; d[32] &= 0x7F; d[64] &= 0x7F;
		set_checksum(d, 2);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, ofs);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID);
		AS(c->rec.diag == FF_RECOVERY_DIAG_NAME_LEN);
	}

	/* --- Damage: broken SecondaryCount must not swallow a later good set --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR badnm[8] = {'b','a','d',0};
		WCHAR goodnm[8] = {'g','o','o','d',0};
		DWORD dcl = find_free(2, 0);
		unsigned char *d = clp(img.root_clst);
		fill_gap(img.root_clst, 3 * 32, 160);
		/* Slot 128: corrupted File entry claiming 19 secondaries (max is 18). */
		d[128] = 0x05; d[129] = 19; st16(d + 132, 0x20);
		/* Slot 160: a fully valid deleted set (4 entries) - inside the bogus span. */
		AS(find_free(dcl + 1, 0) != 0);
		AS(put_deleted_set(img.root_clst, 160, goodnm, 4, dcl, 10, 10, 0x20, 2) > 0);
		(void)badnm;
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, 128);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID && c->rec.diag == FF_RECOVERY_DIAG_SECCOUNT);
		c = find_ofs(&cs, 160);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_VALID && c->rec.diag == FF_RECOVERY_DIAG_NONE);
		AS(name_eq(c->rec.name, "good"));
	}

	/* --- End-of-directory marker stops the scan --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR nm[8] = {'b','e','y','o','n','d',0};
		DWORD dcl = find_free(2, 0);
		unsigned char *d = clp(img.root_clst);
		DWORD ofs;
		for (ofs = 0; ofs < img.csize * SSN && d[ofs] != 0; ofs += 32) ;
		AS(d[ofs] == 0);
		fill_gap(img.root_clst, 3 * 32, ofs);		/* Keep the marker at `ofs` */
		AS(put_deleted_set(img.root_clst, ofs + 64, nm, 6, dcl, 10, 10, 0x20, 2) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 0);
	}

	/* --- Truncated set: the directory chain ends in the middle of a set --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR nm[8] = {'t','r','u','n','c',0};
		DWORD dcl = find_free(2, 0);
		AS(dcl != 0);
		fill_gap(img.root_clst, 3 * 32, 4064);
		/* 3-entry set at 4064; entries 1-2 would be in the next cluster,
		   but the root directory chain stops after the first cluster. */
		AS(put_deleted_set(img.root_clst, 4064, nm, 5, dcl, 10, 10, 0x20, 2) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.ofs == 4064);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_INCOMPLETE);
	}

	/* --- Missing volume up-case table: the name hash cannot be verified --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(write_file("0:/noupc.bin", "ABC", 3) == 0);
	CHECK(f_unlink("0:/noupc.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0);
	{
		DWORD ofs = cs.c[0].rec.ofs;
		clp(img.root_clst)[2 * 32 + 0] = 0x02;	/* Hide the up-case entry type */
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0);
		c = find_ofs(&cs, ofs);
		AS(c != 0 && c->rec.state == FF_RECOVERY_STATE_INVALID);
		AS(c->rec.diag == FF_RECOVERY_DIAG_UPCASE);
	}

	/* --- Scan a live sub-directory (not the root) --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	CHECK(f_mkdir("0:/sub") == FR_OK);
	CHECK(write_file("0:/sub/inner.bin", "INNER", 5) == 0);
	CHECK(f_unlink("0:/sub/inner.bin") == FR_OK);
	{
		DIR sub;
		CAPS sc; UINT snc;
		AS(f_opendir(&sub, "0:/sub") == FR_OK);
		memset(&sc, 0, sizeof sc);
		AS(f_exfat_recovery_scan(&sub, collect, &sc, &snc) == FR_OK && snc == 1);
		AS(sc.c[0].rec.state == FF_RECOVERY_STATE_VALID && sc.c[0].rec.diag == FF_RECOVERY_DIAG_NONE);
		AS(name_eq(sc.c[0].rec.name, "inner.bin"));
		{
			BYTE b[8]; QWORD got;
			AS(f_exfat_recovery_read(&sc.c[0].rec, b, 0, 5, &got) == FR_OK && got == 5);
			AS(memcmp(b, "INNER", 5) == 0);
		}
		f_closedir(&sub);
	}

	return n;
}
/* ================================================================== */

static int test_allocation(void)
{
	FATFS fs;
	CAPS cs;
	UINT nc, n = 0;
	CAP *c;
	BYTE b[4096];

	/* --- Contiguous NoFatChain file; FAT is stale and must not be trusted --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	memset(b, 'C', sizeof b);
	CHECK(write_file("0:/contig.bin", b, sizeof b) == 0);
	CHECK(f_unlink("0:/contig.bin") == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	c = &cs.c[0];
	AS(c->rec.state == FF_RECOVERY_STATE_VALID);
	AS((c->rec.flags & FF_RECOVERY_FLAG_NOFATCHAIN) != 0);
	AS(c->rec.n_clust == 1);
	fat_set(c->rec.first_clst, c->rec.first_clst);	/* Nonsense cycle in the stale FAT */
	AS(remount(&fs) == FR_OK);
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	AS(cs.c[0].rec.state == FF_RECOVERY_STATE_VALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_NONE);
	{
		QWORD got;
		AS(f_exfat_recovery_read(&cs.c[0].rec, b, 0, 4096, &got) == FR_OK && got == 4096);
		AS(b[0] == 'C' && b[4095] == 'C');
	}

	/* --- Fragmented FAT chain across three free, non-adjacent clusters --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		DWORD c2 = find_free(c1 + 2, 0);		/* Skip one to force fragmentation */
		DWORD c3 = find_free(c2 + 2, 0);
		WCHAR nm[8] = {'f','r','a','g',0};
		unsigned char *p;
		int i;
		AS(c1 && c2 && c3);
		fat_set(c1, c2); fat_set(c2, c3); fat_set(c3, 0xFFFFFFFF);
		for (i = 0; i < 3; i++) {
			p = clp(i == 0 ? c1 : i == 1 ? c2 : c3);
			memset(p, (int)('A' + i), CSIZE);
		}
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 3 * CSIZE, 3 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		c = &cs.c[0];
		AS(c->rec.state == FF_RECOVERY_STATE_VALID && c->rec.diag == FF_RECOVERY_DIAG_NONE);
		AS((c->rec.flags & FF_RECOVERY_FLAG_NOFATCHAIN) == 0);
		AS(c->rec.n_clust == 3 && c->rec.first_clst == c1);
		{
			static BYTE out[3 * CSIZE]; QWORD got;
			AS(f_exfat_recovery_read(&c->rec, out, 0, 3 * CSIZE, &got) == FR_OK && got == 3 * CSIZE);
			AS(out[0] == 'A' && out[CSIZE] == 'B' && out[2 * CSIZE] == 'C');
			AS(out[CSIZE - 1] == 'A' && out[3 * CSIZE - 1] == 'C');
		}
	}

	/* --- Partial reuse: any allocated cluster marks the whole candidate reused --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		DWORD c2 = find_free(c1 + 1, 0);
		WCHAR nm[8] = {'r','e','u','s','e',0};
		AS(c1 && c2);
		fat_set(c1, c2); fat_set(c2, 0xFFFFFFFF);
		memset(clp(c1), 'O', CSIZE);
		memset(clp(c2), 'O', CSIZE);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 5, c1, 2 * CSIZE, 2 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_VALID);
		bit_set(c2, 1);				/* Second cluster now allocated to something else */
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		c = &cs.c[0];
		AS(c->rec.state == FF_RECOVERY_STATE_REUSED);
		AS(c->rec.readable_len == 0);
		{
			QWORD got;
			AS(f_exfat_recovery_read(&c->rec, b, 0, 16, &got) == FR_DENIED && got == 0);
		}
	}

	/* --- Chain diagnostics: early end, invalid cluster, loop, overlong --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0), c2 = find_free(c1 + 1, 0), c3 = find_free(c2 + 1, 0), c4 = find_free(c3 + 1, 0);
		WCHAR nm[8] = {'e','a','r','l','y',0};
		AS(c1 && c2 && c3 && c4);
		/* Early end: needs 3 clusters, chain ends after 1. */
		fat_set(c1, 0xFFFFFFFF);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 5, c1, 3 * CSIZE, 3 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_CHAIN_EARLY);
	}
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0), c2 = find_free(c1 + 1, 0);
		WCHAR nm[8] = {'i','l','c','l',0};
		AS(c1 && c2);
		/* Second link points below the valid cluster range. */
		fat_set(c1, 1);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 2 * CSIZE, 2 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_CHAIN_CLUSTER);
	}
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0), c2 = find_free(c1 + 1, 0);
		WCHAR nm[8] = {'l','o','o','p',0};
		AS(c1 && c2);
		fat_set(c1, c2); fat_set(c2, c1);		/* Cycle of length 2 */
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 2 * CSIZE, 2 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_CHAIN_LOOP);
	}
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0), c2 = find_free(c1 + 1, 0), c3 = find_free(c2 + 1, 0);
		WCHAR nm[8] = {'l','o','n','g',0};
		AS(c1 && c2 && c3);
		fat_set(c1, c2); fat_set(c2, c3); fat_set(c3, 0xFFFFFFFF);
		/* DataLength covers 2 clusters; the chain has a third. */
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 2 * CSIZE, 2 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_CHAIN_LONG);
	}

	/* --- FirstCluster outside the volume --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		WCHAR nm[8] = {'b','a','d','c',0};
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, img.n_fatent + 16, CSIZE, CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_CHAIN_CLUSTER);
	}

	/* --- ValidDataLength > DataLength is rejected, not read --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		WCHAR nm[8] = {'v','l','e','n',0};
		AS(c1 != 0);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 100, 200, 0x20, 2) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID && cs.c[0].rec.diag == FF_RECOVERY_DIAG_VALID_LEN);
	}

	/* --- Deleted directory entry: DIR state, no data follow --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD dcl = find_free(2, 0);
		WCHAR nm[8] = {'d','i','r','d','e','l',0};
		AS(dcl != 0);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 6, dcl, CSIZE, CSIZE, AM_DIR, 3) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_DIR);
		AS(cs.c[0].rec.attr & AM_DIR);
		{
			QWORD got;
			AS(f_exfat_recovery_read(&cs.c[0].rec, b, 0, 4, &got) == FR_DENIED && got == 0);
		}
	}

	return n;
}

/* ================================================================== */
/* Group: reads                                                        */
/* ================================================================== */

static int test_reads(void)
{
	FATFS fs;
	CAPS cs;
	UINT nc, n = 0;
	CAP *c;
	static BYTE data[20000];

	/* --- Real deletion, cross-fragment reads --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		UINT i;
		for (i = 0; i < sizeof data; i++) data[i] = (BYTE)(i * 37 + 11);
		CHECK(write_file("0:/big.bin", data, sizeof data) == 0);
		CHECK(f_unlink("0:/big.bin") == FR_OK);
	}
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	c = &cs.c[0];
	AS(c->rec.state == FF_RECOVERY_STATE_VALID);
	AS(c->rec.data_len == sizeof data && c->rec.valid_len == sizeof data);
	{
		static BYTE out[20000];
		QWORD got;
		UINT i;
		AS(f_exfat_recovery_read(&c->rec, out, 0, sizeof data, &got) == FR_OK && got == sizeof data);
		for (i = 0; i < sizeof data; i++) if (out[i] != data[i]) break;
		AS(i == sizeof data);
		/* Unaligned read crossing a cluster boundary. */
		AS(f_exfat_recovery_read(&c->rec, out, 4000, 300, &got) == FR_OK && got == 300);
		for (i = 0; i < 300; i++) if (out[i] != data[4000 + i]) break;
		AS(i == 300);
		/* Read beyond the logical end is clipped. */
		AS(f_exfat_recovery_read(&c->rec, out, sizeof data - 8, 64, &got) == FR_OK && got == 8);
		for (i = 0; i < 8; i++) if (out[i] != data[sizeof data - 8 + i]) break;
		AS(i == 8);
	}

	/* --- ValidDataLength tail must be zero, never cluster garbage --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		DWORD c2;
		WCHAR nm[8] = {'t','a','i','l',0};
		AS(c1 != 0);
		c2 = find_free(c1 + 1, 0);
		AS(c2 != 0);
		memset(clp(c1), 0xA5, CSIZE);			/* Uninitialised cluster tail */
		memset(clp(c2), 0xA5, CSIZE);
		fat_set(c1, c2); fat_set(c2, 0xFFFFFFFF);	/* FAT-chained */
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 2 * CSIZE, 100, 0x20, 0) > 0);		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		c = &cs.c[0];
		AS(c->rec.state == FF_RECOVERY_STATE_VALID);
		AS(c->rec.data_len == 2 * CSIZE && c->rec.valid_len == 100);
		{
			static BYTE out[2 * CSIZE];
			QWORD got;
			UINT i;
			memset(out, 0xEE, sizeof out);
			AS(f_exfat_recovery_read(&c->rec, out, 0, 2 * CSIZE, &got) == FR_OK && got == 2 * CSIZE);
			for (i = 0; i < 100; i++) if (out[i] != 0xA5) break;
			AS(i == 100);
			for (i = 100; i < 2 * CSIZE; i++) if (out[i] != 0) break;
			AS(i == 2 * CSIZE);
			/* A read starting inside the zeroed tail also returns zeros. */
			AS(f_exfat_recovery_read(&c->rec, out, CSIZE, 64, &got) == FR_OK && got == 64);
			for (i = 0; i < 64; i++) if (out[i] != 0) break;
			AS(i == 64);
		}
	}

	/* --- Read failure: exact partial count, no zero fill --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		DWORD c2;
		WCHAR nm[8] = {'f','a','i','l',0};
		LBA_t dbase;
		AS(c1 != 0);
		c2 = find_free(c1 + 1, 0);
		AS(c2 != 0);
		memset(clp(c1), 'F', CSIZE);
		memset(clp(c2), 'F', CSIZE);
		fat_set(c1, c2); fat_set(c2, 0xFFFFFFFF);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, 2 * CSIZE, 2 * CSIZE, 0x20, 0) > 0);
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_VALID);
		{
			static BYTE out[2 * CSIZE];
			QWORD got;
			memset(out, 0xEE, sizeof out);
			dbase = img.database + (LBA_t)img.csize * (c1 - 2);
			checks_disk_fail_range(dbase + 1, dbase + 2);	/* Fail the 2nd sector */
			AS(f_exfat_recovery_read(&cs.c[0].rec, out, 0, 2 * CSIZE, &got) == FR_DISK_ERR);
			AS(got == SSN);					/* Exactly one sector produced */
			AS(out[0] == 'F' && out[SSN - 1] == 'F');
			AS(out[SSN] == 0xEE && out[2 * CSIZE - 1] == 0xEE);	/* No zero fill */
			checks_disk_clear_fail_range();
		}
		/* Whole-device read failure during a content read. */
		{
			QWORD got;
			checks_disk_set_fail_reads(1);
			AS(f_exfat_recovery_read(&cs.c[0].rec, (void *)data, 0, 16, &got) == FR_DISK_ERR && got == 0);
			checks_disk_set_fail_reads(0);
		}
		/* A short image: all sectors beyond the volume fail. Keep the exact
		   count of bytes produced before the shortfall, no zero fill. */
		{
			static BYTE out[2 * CSIZE];
			QWORD got;
			memset(out, 0xEE, sizeof out);
			checks_disk_fail_range((LBA_t)(img.database + img.csize * (c1 - 2) + 1), (LBA_t)-1);
			AS(f_exfat_recovery_read(&cs.c[0].rec, out, 0, 2 * CSIZE, &got) == FR_DISK_ERR);
			AS(got == SSN);
			checks_disk_clear_fail_range();
		}
	}

	/* --- INVALID candidate cannot be read at all --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		DWORD c1 = find_free(2, 0);
		WCHAR nm[8] = {'n','o','p','e',0};
		AS(c1 != 0);
		AS(put_deleted_set(img.root_clst, 3 * 32, nm, 4, c1, CSIZE, CSIZE, 0x20, 0) > 0);
		/* Break the checksum after the set is written. */
		clp(img.root_clst)[3 * 32 + 2] ^= 0xFF;
		AS(remount(&fs) == FR_OK);
		AS(scan_root(&cs, &nc) == 0 && nc == 1);
		AS(cs.c[0].rec.state == FF_RECOVERY_STATE_INVALID);
		{
			QWORD got;
			AS(f_exfat_recovery_read(&cs.c[0].rec, data, 0, 16, &got) == FR_DENIED && got == 0);
		}
	}

	return n;
}

/* ================================================================== */
/* Group: isolation                                                    */
/* ================================================================== */

static int test_isolation(void)
{
	FATFS fs;
	CAPS cs;
	UINT nc, n = 0;
	CAP *c;
	unsigned long w0;
	unsigned char *snap;
	size_t snap_sz;

	CHECK(fresh_volume(&fs) == FR_OK);
	snap_sz = (size_t)checks_disk_sectors() * 512;
	snap = malloc(snap_sz);
	AS(snap != 0);

	/* --- No disk_write() during scan or read; image bytes unchanged --- */
	CHECK(write_file("0:/iso.bin", "ISOLATION", 9) == 0);
	CHECK(f_unlink("0:/iso.bin") == FR_OK);
	memcpy(snap, checks_disk_data(), snap_sz);
	w0 = checks_disk_writes;
	AS(scan_root(&cs, &nc) == 0 && nc == 1);
	c = &cs.c[0];
	AS(c->rec.state == FF_RECOVERY_STATE_VALID);
	{
		BYTE b[16]; QWORD got;
		AS(f_exfat_recovery_read(&c->rec, b, 0, 9, &got) == FR_OK && got == 9);
		AS(memcmp(b, "ISOLATION", 9) == 0);
	}
	AS(checks_disk_writes == w0);				/* Zero writes happened */
	AS(memcmp(snap, checks_disk_data(), snap_sz) == 0);	/* Image unchanged */
	/* A second scan is identical (deterministic). */
	{
		CAPS cs2; UINT nc2;
		AS(scan_root(&cs2, &nc2) == 0 && nc2 == nc);
		AS(cs2.c[0].rec.ofs == c->rec.ofs && cs2.c[0].rec.state == c->rec.state);
	}

	/* --- f_readdir position survives a scan --- */
	CHECK(fresh_volume(&fs) == FR_OK);
	{
		char names[5][32], path[48];
		int k;
		DIR dir, d2;
		FILINFO fi;
		for (k = 0; k < 5; k++) {
			snprintf(names[k], sizeof names[k], "live%d.bin", k);
			path[0] = 0;
			strcat(path, "0:/");
			strcat(path, names[k]);
			CHECK(write_file(path, "L", 1) == 0);
		}
		/* Record the natural directory order. */
		CHECK(f_opendir(&dir, "0:") == FR_OK);
		for (k = 0; k < 5; k++) {
			CHECK(f_readdir(&dir, &fi) == FR_OK && fi.fname[0]);
			strcpy(names[k], fi.fname);
		}
		CHECK(f_closedir(&dir) == FR_OK);
		/* Now read two items, scan, and expect the third item next. */
		CHECK(f_opendir(&d2, "0:") == FR_OK);
		CHECK(f_readdir(&d2, &fi) == FR_OK && fi.fname[0]);
		CHECK(f_readdir(&d2, &fi) == FR_OK && fi.fname[0]);
		CHECK(f_exfat_recovery_scan(&d2, 0, 0, &nc) == FR_OK && nc == 0);
		CHECK(f_readdir(&d2, &fi) == FR_OK && fi.fname[0]);
		AS(strcmp(fi.fname, names[2]) == 0);
		CHECK(f_closedir(&d2) == FR_OK);
	}

	/* --- Scanner leaves the DIR object byte-for-byte intact --- */
	{
		DIR dir;
		DIR copy;
		CHECK(f_opendir(&dir, "0:") == FR_OK);
		CHECK(f_readdir(&dir, &(FILINFO){0}) == FR_OK);
		memcpy(&copy, &dir, sizeof copy);
		CHECK(f_exfat_recovery_scan(&dir, 0, 0, &nc) == FR_OK);
		AS(memcmp(&dir, &copy, sizeof copy) == 0);
		CHECK(f_closedir(&dir) == FR_OK);
	}

	/* --- Only exFAT volumes are accepted; bad parameters are rejected --- */
	AS(f_exfat_recovery_scan(0, 0, 0, &nc) == FR_INVALID_OBJECT);
	{
		FF_RECOVERY_INFO bad;
		QWORD got;
		memset(&bad, 0, sizeof bad);
		AS(f_exfat_recovery_read(0, (void *)snap, 0, 1, &got) == FR_INVALID_OBJECT);
		AS(f_exfat_recovery_read(&bad, (void *)snap, 0, 1, &got) == FR_INVALID_OBJECT);
	free(snap);
	}

	return n;
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

int exfat_recovery_test(const char *group)
{
	int r;
	if (!strcmp(group, "sets")) r = test_sets();
	else if (!strcmp(group, "allocation")) r = test_allocation();
	else if (!strcmp(group, "reads")) r = test_reads();
	else if (!strcmp(group, "isolation")) r = test_isolation();
	else { fprintf(stderr, "unknown group %s\n", group); return -1; }
	return r;
}
