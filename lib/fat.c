/*
 * fat.c - read-only FatELF v1 container support.
 *
 * A FatELF container bundles several ELF images.  The container header and
 * record table use little-endian integers, while each bundled image keeps
 * its own ELF class and byte order.  Children opened from a FatELF
 * descriptor are ordinary ELF descriptors whose file window is the record;
 * the normal section, string and symbol code handles them unchanged.
 */

#include <private.h>

#ifndef lint
static const char rcsid[] = "@(#) $Id: fat.c,v 1.0 2026/11/18 Exp $";
#endif /* lint */

/*
 * FatELF v1 on-disk layout (all metadata integers little endian):
 *
 *   header: magic[4] version:u16 records:u8 reserved:u8
 *   record: machine:u16 osabi:u8 osabi_version:u8
 *           word_size:u8 byte_order:u8 reserved[2]
 *           offset:u64 size:u64
 *
 * The magic byte sequence is FA 70 0E 1F, i.e. the little-endian value
 * 0x1F0E70FA.
 */
#define FAT_MAGIC0	0xfa
#define FAT_MAGIC1	0x70
#define FAT_MAGIC2	0x0e
#define FAT_MAGIC3	0x1f
#define FAT_HDR_SIZE	8
#define FAT_REC_SIZE	24
#define FAT_VERSION	1

static const Elf _elf_fat_template = INIT_ELF;

static unsigned
fat_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint64_t
fat_u64(const unsigned char *p) {
    uint64_t v = 0;
    unsigned i;

    for (i = 8; i-- > 0; ) {
	v = (v << 8) | p[i];
    }
    return v;
}

int
_elf_fat_recognize(const void *image, size_t size) {
    const unsigned char *p = (const unsigned char*)image;

    return p && size >= FAT_HDR_SIZE
	&& p[0] == FAT_MAGIC0 && p[1] == FAT_MAGIC1
	&& p[2] == FAT_MAGIC2 && p[3] == FAT_MAGIC3;
}

/*
 * Validate and parse the record table.  Called once, while the container
 * descriptor is being created, so that malformed metadata makes the open
 * call fail instead of producing a half-usable descriptor.
 */
int
_elf_fat_init(Elf *elf) {
    const unsigned char *image;
    Elf_FatRecord *records;
    size_t count;
    size_t header_end;
    size_t i;
    size_t j;

    elf_assert(elf);
    elf_assert(elf->e_magic == ELF_MAGIC);
    elf_assert(_elf_fat_recognize(elf->e_data, elf->e_size));

    image = (const unsigned char*)elf->e_data;
    if (elf->e_size < FAT_HDR_SIZE) {
	seterr(ERROR_FAT_FORMAT);
	return 0;
    }
    if (fat_u16(image + 4) != FAT_VERSION) {
	seterr(ERROR_FAT_VERSION);
	return 0;
    }
    if (image[7] != 0) {
	/* reserved header byte must be zero */
	seterr(ERROR_FAT_FORMAT);
	return 0;
    }

    count = image[6];
    if (count > (elf->e_size - FAT_HDR_SIZE) / FAT_REC_SIZE) {
	/* record table does not fit in the container */
	seterr(ERROR_FAT_FORMAT);
	return 0;
    }
    header_end = FAT_HDR_SIZE + count * FAT_REC_SIZE;

    records = NULL;
    if (count) {
	records = (Elf_FatRecord*)malloc(count * sizeof(*records));
	if (!records) {
	    seterr(ERROR_MEM_ELF);
	    return 0;
	}
    }

    for (i = 0; i < count; i++) {
	const unsigned char *r = image + FAT_HDR_SIZE + i * FAT_REC_SIZE;
	Elf_FatRecord *rec = &records[i];
	uint64_t offset;
	uint64_t size;

	rec->machine = fat_u16(r);
	rec->osabi = r[2];
	rec->osabi_version = r[3];
	rec->word_size = r[4];
	rec->byte_order = r[5];

	/* reserved bits must be zero */
	if (r[6] != 0 || r[7] != 0) {
	    free(records);
	    seterr(ERROR_FAT_FORMAT);
	    return 0;
	}
	/* the target description must be a valid ELF class/encoding */
	if ((rec->word_size != ELFCLASS32 && rec->word_size != ELFCLASS64)
	 || (rec->byte_order != ELFDATA2LSB && rec->byte_order != ELFDATA2MSB)) {
	    free(records);
	    seterr(ERROR_FAT_FORMAT);
	    return 0;
	}

	offset = fat_u64(r + 8);
	size = fat_u64(r + 16);
	rec->offset = offset;
	rec->size = size;

	/* the image must lie inside the container and outside the metadata */
	if (offset < (uint64_t)header_end
	 || offset > (uint64_t)elf->e_size
	 || size > (uint64_t)elf->e_size - offset) {
	    free(records);
	    seterr(ERROR_FAT_RANGE);
	    return 0;
	}

	/* images must not overlap each other */
	for (j = 0; j < i; j++) {
	    uint64_t o = records[j].offset;
	    uint64_t s = records[j].size;

	    if (offset < o + s && o < offset + size) {
		free(records);
		seterr(ERROR_FAT_RANGE);
		return 0;
	    }
	}

	/*
	 * There must be at most one image per target: a duplicate
	 * machine/word size/byte order combination is invalid.
	 */
	for (j = 0; j < i; j++) {
	    if (records[j].machine == rec->machine
	     && records[j].word_size == rec->word_size
	     && records[j].byte_order == rec->byte_order) {
		free(records);
		seterr(ERROR_FAT_FORMAT);
		return 0;
	    }
	}
    }

    elf->e_kind = ELF_K_FAT;
    elf->e_idlen = FAT_HDR_SIZE;
    elf->e_fat_records = records;
    elf->e_fat_count = count;
    elf->e_fat_next = 0;
    return 1;
}

/*
 * Verify that the bytes described by a record really hold the requested
 * ELF target.  The record metadata is untrusted input, like the image.
 */
static int
fat_check_image(const unsigned char *image, const Elf_FatRecord *rec) {
    unsigned machine;

    if (rec->size < 20) {
	seterr(ERROR_FAT_MISMATCH);
	return 0;
    }
    if (memcmp(image, ELFMAG, SELFMAG)) {
	seterr(ERROR_FAT_MISMATCH);
	return 0;
    }
    if (image[EI_CLASS] != rec->word_size
     || image[EI_DATA] != rec->byte_order
     || image[EI_OSABI] != rec->osabi
     || image[EI_ABIVERSION] != rec->osabi_version) {
	seterr(ERROR_FAT_MISMATCH);
	return 0;
    }
    if (rec->byte_order == ELFDATA2MSB) {
	machine = ((unsigned)image[18] << 8) | image[19];
    }
    else {
	machine = ((unsigned)image[19] << 8) | image[18];
    }
    if (machine != rec->machine) {
	seterr(ERROR_FAT_MISMATCH);
	return 0;
    }
    return 1;
}

/*
 * Open the record currently selected by the container cursor.  The cursor
 * only advances after the child has been validated and linked, so a failed
 * open leaves the selection untouched.
 */
Elf*
_elf_fat_begin(Elf *fat, Elf_Cmd cmd) {
    Elf_FatRecord rec;
    Elf *child;
    const char *image;
    size_t index;

    elf_assert(fat);
    elf_assert(fat->e_magic == ELF_MAGIC);
    elf_assert(fat->e_kind == ELF_K_FAT);

    if (cmd != ELF_C_READ) {
	seterr(ERROR_MEMBERWRITE);
	return NULL;
    }
    index = fat->e_fat_next;
    if (index >= fat->e_fat_count) {
	seterr(ERROR_FAT_INDEX);
	return NULL;
    }
    rec = fat->e_fat_records[index];

    /*
     * Reopening the same record must reuse the live descriptor, following
     * the activation-count convention used for archive members.
     */
    for (child = fat->e_members; child; child = child->e_link) {
	elf_assert(child->e_parent == fat);
	if (child->e_fat_member && child->e_fat_index == index) {
	    child->e_count++;
	    fat->e_fat_next = index + 1;
	    return child;
	}
    }

    if (!fat->e_data) {
	seterr(ERROR_FAT_MISMATCH);
	return NULL;
    }
    image = fat->e_data + rec.offset;
    if (!fat_check_image((const unsigned char*)image, &rec)) {
	return NULL;
    }

    if (!(child = (Elf*)malloc(sizeof(Elf)))) {
	seterr(ERROR_MEM_ELF);
	return NULL;
    }
    *child = _elf_fat_template;
    child->e_fd = fat->e_fd;
    child->e_parent = fat;
    child->e_size = child->e_dsize = (size_t)rec.size;
    child->e_data = child->e_rawdata = (char*)image;
    child->e_readable = 1;
    child->e_disabled = fat->e_disabled;
    child->e_memory = fat->e_memory;
    child->e_fat_member = 1;
    child->e_fat_index = index;
    child->e_base = fat->e_base + (size_t)rec.offset;
    child->e_next = index + 1;
    /* the child is a normal ELF file inside its record window */
    child->e_kind = ELF_K_ELF;
    child->e_idlen = EI_NIDENT;
    child->e_class = (unsigned char)image[EI_CLASS];
    child->e_encoding = (unsigned char)image[EI_DATA];
    child->e_version = (unsigned char)image[EI_VERSION];

    /* parent/child linking */
    child->e_link = fat->e_members;
    fat->e_members = child;
    fat->e_count++;
    fat->e_fat_next = index + 1;
    return child;
}

/*
 * elf_next() for a FatELF child: move the container cursor just past the
 * child's record.
 */
Elf_Cmd
_elf_fat_next(Elf *elf) {
    Elf *fat;

    elf_assert(elf);
    elf_assert(elf->e_magic == ELF_MAGIC);
    elf_assert(elf->e_fat_member);
    fat = elf->e_parent;
    elf_assert(fat);
    elf_assert(fat->e_magic == ELF_MAGIC);
    elf_assert(fat->e_kind == ELF_K_FAT);

    fat->e_fat_next = elf->e_fat_index + 1;
    if (fat->e_fat_next >= fat->e_fat_count) {
	return ELF_C_NULL;
    }
    return ELF_C_READ;
}

int
elf_getfatnum(Elf *elf, size_t *resultp) {
    if (!elf) {
	return -1;
    }
    elf_assert(elf->e_magic == ELF_MAGIC);
    if (elf->e_kind != ELF_K_FAT) {
	seterr(ERROR_NOTFAT);
	return -1;
    }
    if (resultp) {
	*resultp = elf->e_fat_count;
    }
    return 0;
}

Elf_FatRecord*
elf_getfatrecord(Elf *elf, size_t index, Elf_FatRecord *resultp) {
    if (!elf || !resultp) {
	return NULL;
    }
    elf_assert(elf->e_magic == ELF_MAGIC);
    if (elf->e_kind != ELF_K_FAT) {
	seterr(ERROR_NOTFAT);
	return NULL;
    }
    if (index >= elf->e_fat_count) {
	seterr(ERROR_FAT_INDEX);
	return NULL;
    }
    *resultp = elf->e_fat_records[index];
    return resultp;
}

int
elf_fatselect(Elf *elf, size_t index) {
    if (!elf) {
	return -1;
    }
    elf_assert(elf->e_magic == ELF_MAGIC);
    if (elf->e_kind != ELF_K_FAT) {
	seterr(ERROR_NOTFAT);
	return -1;
    }
    if (index >= elf->e_fat_count) {
	seterr(ERROR_FAT_INDEX);
	return -1;
    }
    /*
     * Selection only changes the position used by the next elf_begin().
     * It does not open anything, so a bad index leaves it unchanged.
     */
    elf->e_fat_next = index;
    return 0;
}
