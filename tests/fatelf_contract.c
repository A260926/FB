/*
 * fatelf_contract.c - regression tests for read-only FatELF v1 support.
 *
 * The fixture generators only produce inert bytes; no bundled image is
 * executed.  Assertions go through task_support/check.h.
 */

#include "check.h"
#include "fixture.h"

#include <libelf/libelf.h>
#include <libelf/gelf.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUNDLE ".task-build/samples/bundle.fat"
#define NESTED ".task-build/samples/nested.a"
#define NORMAL ".task-build/samples/normal.a"

#define FAT_CAP 8960
#define ELF_SIZE 768

static int open_ro(const char *path) {
    return open(path, O_RDONLY);
}

/* Read a symbol named "same" from section 3 of an image. */
static int read_same(Elf *elf, unsigned *machine, unsigned *value,
                     const char **name) {
    GElf_Ehdr ehdr;
    GElf_Shdr shdr;
    GElf_Sym sym;
    Elf_Scn *scn;
    Elf_Data *data;
    const char *str;

    if (!gelf_getehdr(elf, &ehdr)) return 0;
    scn = elf_getscn(elf, 3);
    if (!scn || !gelf_getshdr(scn, &shdr)) return 0;
    if (shdr.sh_type != SHT_SYMTAB || shdr.sh_link != 2) return 0;
    data = elf_getdata(scn, NULL);
    if (!data || !gelf_getsym(data, 1, &sym)) return 0;
    str = elf_strptr(elf, shdr.sh_link, sym.st_name);
    if (!str) return 0;
    if (machine) *machine = ehdr.e_machine;
    if (value) *value = (unsigned)sym.st_value;
    if (name) *name = str;
    return 1;
}

/* Build a valid fixture container into caller storage. */
static size_t make_fat(unsigned char *buf) {
    return fixture_fat(buf, FAT_CAP);
}

static void put16le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put64le(unsigned char *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (i * 8));
}

/* ------------------------------------------------------------------ */
/* records: metadata access and container validation                   */
/* ------------------------------------------------------------------ */

static int fat_metadata(void) {
    unsigned char bytes[FAT_CAP];
    Elf_FatRecord rec;
    Elf *fat;
    size_t n = make_fat(bytes);
    int fd;

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);

    /* file entry */
    fd = open_ro(BUNDLE);
    TASK_CHECK(fd >= 0);
    fat = elf_begin(fd, ELF_C_READ, NULL);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_kind(fat) == ELF_K_FAT);
    n = 0;
    TASK_CHECK(elf_getfatnum(fat, &n) == 0);
    TASK_CHECK(n == 2);

    memset(&rec, 0, sizeof(rec));
    TASK_CHECK(elf_getfatrecord(fat, 0, &rec) == &rec);
    TASK_CHECK(rec.machine == 62);
    TASK_CHECK(rec.word_size == 2);
    TASK_CHECK(rec.byte_order == 1);
    TASK_CHECK(rec.osabi == 0);
    TASK_CHECK(rec.osabi_version == 0);
    TASK_CHECK(rec.offset == 4096);
    TASK_CHECK(rec.size == 768);

    TASK_CHECK(elf_getfatrecord(fat, 1, &rec) == &rec);
    TASK_CHECK(rec.machine == 20);
    TASK_CHECK(rec.word_size == 1);
    TASK_CHECK(rec.byte_order == 2);
    TASK_CHECK(rec.offset == 8192);
    TASK_CHECK(rec.size == 768);

    /* out-of-range records */
    TASK_CHECK(elf_getfatrecord(fat, 2, &rec) == NULL);
    TASK_CHECK(elf_errmsg(-1) != NULL);

    /* selection API is only valid on FatELF containers */
    TASK_CHECK(elf_fatselect(fat, 0) == 0);
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    TASK_CHECK(elf_fatselect(fat, 2) == -1);
    TASK_CHECK(elf_errmsg(-1) != NULL);

    TASK_CHECK(elf_end(fat) == 0);
    TASK_CHECK(close(fd) == 0);

    /* memory entry behaves the same */
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_kind(fat) == ELF_K_FAT);
    TASK_CHECK(elf_getfatnum(fat, &n) == 0 && n == 2);
    TASK_CHECK(elf_getfatrecord(fat, 0, &rec) == &rec && rec.machine == 62);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

static int fat_reject_nonfat(void) {
    unsigned char bytes[ELF_SIZE];
    Elf *elf;
    Elf_FatRecord rec;
    size_t n = 0;

    TASK_CHECK(fixture_elf(bytes, sizeof(bytes), 64, 0, 1) == ELF_SIZE);
    elf = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(elf != NULL);
    TASK_CHECK(elf_kind(elf) == ELF_K_ELF);
    TASK_CHECK(elf_getfatnum(elf, &n) == -1);
    TASK_CHECK(elf_errmsg(-1) != NULL);
    TASK_CHECK(elf_getfatrecord(elf, 0, &rec) == NULL);
    TASK_CHECK(elf_fatselect(elf, 0) == -1);
    TASK_CHECK(elf_end(elf) == 0);
    return 0;
}

/* Every malformed container must fail the opening call itself. */
static int expect_bad(const unsigned char *bytes, size_t size, const char *what) {
    Elf *elf = elf_memory((char*)bytes, size);

    if (elf != NULL) {
        fprintf(stderr, "records: accepted malformed container: %s\n", what);
        TASK_CHECK(elf_end(elf) == 0);
        return 1;
    }
    TASK_CHECK(elf_errmsg(-1) != NULL);
    TASK_CHECK(elf_errmsg(-1)[0] != '\0');
    return 0;
}

static int fat_validation(void) {
    unsigned char good[FAT_CAP];
    unsigned char bad[FAT_CAP];
    size_t n;

    TASK_CHECK((n = make_fat(good)) == FAT_CAP);

    /* unsupported version */
    memcpy(bad, good, n);
    put16le(bad + 4, 2);
    if (expect_bad(bad, n, "version")) return 1;

    /* reserved header byte set */
    memcpy(bad, good, n);
    bad[7] = 1;
    if (expect_bad(bad, n, "header reserved")) return 1;

    /* record table larger than the container */
    memcpy(bad, good, n);
    bad[6] = 200;
    if (expect_bad(bad, n, "record table truncated")) return 1;

    /* record reserved bytes set */
    memcpy(bad, good, n);
    bad[8 + 6] = 1;
    if (expect_bad(bad, n, "record reserved")) return 1;

    /* invalid word size */
    memcpy(bad, good, n);
    bad[8 + 4] = 3;
    if (expect_bad(bad, n, "word size")) return 1;

    /* invalid byte order */
    memcpy(bad, good, n);
    bad[8 + 5] = 0;
    if (expect_bad(bad, n, "byte order")) return 1;

    /* duplicate target: make record 1 claim record 0's target */
    memcpy(bad, good, n);
    put16le(bad + 8 + 24, 62);
    bad[8 + 24 + 4] = 2;
    bad[8 + 24 + 5] = 1;
    if (expect_bad(bad, n, "duplicate target")) return 1;

    /* record range outside the container */
    memcpy(bad, good, n);
    put64le(bad + 8 + 8, 4096);
    put64le(bad + 8 + 16, FAT_CAP + 1);
    if (expect_bad(bad, n, "range past container")) return 1;

    /* record range inside the metadata area */
    memcpy(bad, good, n);
    put64le(bad + 8 + 8, 0);
    put64le(bad + 8 + 16, 768);
    if (expect_bad(bad, n, "range into metadata")) return 1;

    /* overlapping records */
    memcpy(bad, good, n);
    put64le(bad + 8 + 24 + 8, 4096 + 100);
    put64le(bad + 8 + 24 + 16, 768);
    if (expect_bad(bad, n, "overlap")) return 1;

    /* a valid container with no records is well formed */
    memcpy(bad, good, n);
    bad[6] = 0;
    {
        Elf *elf = elf_memory((char*)bad, n);
        size_t num = 99;
        TASK_CHECK(elf != NULL);
        TASK_CHECK(elf_kind(elf) == ELF_K_FAT);
        TASK_CHECK(elf_getfatnum(elf, &num) == 0 && num == 0);
        TASK_CHECK(elf_fatselect(elf, 0) == -1);
        TASK_CHECK(elf_begin(-1, ELF_C_READ, elf) == NULL);
        TASK_CHECK(elf_end(elf) == 0);
    }
    return 0;
}

static int fat_inner_validation(void) {
    unsigned char bad[FAT_CAP];
    unsigned char good[FAT_CAP];
    Elf *fat;
    size_t n;

    TASK_CHECK((n = make_fat(good)) == FAT_CAP);

    /* record 0 claims 32-bit but the image is ELF64 */
    memcpy(bad, good, n);
    bad[8 + 4] = 1;
    fat = elf_memory((char*)bad, n);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_READ, fat) == NULL);
    TASK_CHECK(elf_errmsg(-1) != NULL);
    /* the failure did not consume the selection: select another record */
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    TASK_CHECK(elf_end(fat) == 0);

    /* record 0 claims big endian but the image is little endian */
    memcpy(bad, good, n);
    bad[8 + 5] = 2;
    fat = elf_memory((char*)bad, n);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_READ, fat) == NULL);
    TASK_CHECK(elf_end(fat) == 0);

    /* record 0 claims a different machine */
    memcpy(bad, good, n);
    put16le(bad + 8, 3);
    fat = elf_memory((char*)bad, n);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_READ, fat) == NULL);
    /*
     * The failed child open must not consume the position: record 1 is
     * still reachable and valid.
     */
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    {
        Elf *ok = elf_begin(-1, ELF_C_READ, fat);
        unsigned machine = 0, value = 0;

        TASK_CHECK(ok != NULL);
        TASK_CHECK(read_same(ok, &machine, &value, NULL));
        TASK_CHECK(machine == 20 && value == 2);
        TASK_CHECK(elf_end(ok) == 0);
    }
    TASK_CHECK(elf_end(fat) == 0);

    /* record 0 claims a different ABI version */
    memcpy(bad, good, n);
    bad[8 + 3] = 9;
    fat = elf_memory((char*)bad, n);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_READ, fat) == NULL);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* views: independent ELF views of each record                         */
/* ------------------------------------------------------------------ */

static int fat_mixed_targets(void) {
    unsigned char bytes[FAT_CAP];
    Elf_FatRecord rec;
    Elf *fat, *child;
    size_t n = make_fat(bytes);
    char *raw;
    size_t rawsize;
    unsigned machine, value;
    const char *name;

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_kind(fat) == ELF_K_FAT);

    /* record 0: ELF64 little endian x86_64 */
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(elf_kind(child) == ELF_K_ELF);
    TASK_CHECK(gelf_getclass(child) == ELFCLASS64);
    TASK_CHECK(read_same(child, &machine, &value, &name));
    TASK_CHECK(machine == 62 && value == 1 && !strcmp(name, "same"));
    TASK_CHECK(elf_getbase(child) == 4096);
    raw = elf_rawfile(child, &rawsize);
    TASK_CHECK(raw != NULL && rawsize == 768);
    TASK_CHECK(raw == (char*)bytes + 4096);
    TASK_CHECK(!memcmp(raw, bytes + 4096, 768));

    /* record 1: ELF32 big endian PowerPC */
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    {
        Elf *child2 = elf_begin(-1, ELF_C_READ, fat);
        TASK_CHECK(child2 != NULL);
        TASK_CHECK(child2 != child);
        TASK_CHECK(elf_kind(child2) == ELF_K_ELF);
        TASK_CHECK(gelf_getclass(child2) == ELFCLASS32);
        TASK_CHECK(read_same(child2, &machine, &value, &name));
        TASK_CHECK(machine == 20 && value == 2 && !strcmp(name, "same"));
        TASK_CHECK(elf_getbase(child2) == 8192);
        raw = elf_rawfile(child2, &rawsize);
        TASK_CHECK(raw != NULL && rawsize == 768);
        TASK_CHECK(raw == (char*)bytes + 8192);
        TASK_CHECK(elf_end(child2) == 0);
    }

    /* metadata inspection did not disturb the record table */
    TASK_CHECK(elf_getfatrecord(fat, 1, &rec) == &rec && rec.machine == 20);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

static int fat_interleaved_views(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *c0, *c1;
    GElf_Sym sym;
    Elf_Scn *scn;
    Elf_Data *d;
    unsigned machine, value;
    size_t n = make_fat(bytes);

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);

    /* open record 0, then record 1 through the cursor */
    c0 = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(c0 != NULL);
    TASK_CHECK(read_same(c0, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);

    c1 = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(c1 != NULL);
    TASK_CHECK(read_same(c1, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);

    /*
     * Both images use section 3, the same string offsets and the same
     * symbol name.  Re-read record 0 while record 1 is live: the views
     * must not share buffers.
     */
    scn = elf_getscn(c0, 3);
    TASK_CHECK(scn != NULL);
    d = elf_getdata(scn, NULL);
    TASK_CHECK(d != NULL);
    TASK_CHECK(gelf_getsym(d, 1, &sym) != NULL);
    TASK_CHECK(sym.st_value == 1);
    TASK_CHECK(!strcmp(elf_strptr(c0, 2, sym.st_name), "same"));

    scn = elf_getscn(c1, 3);
    TASK_CHECK(scn != NULL);
    d = elf_getdata(scn, NULL);
    TASK_CHECK(d != NULL);
    TASK_CHECK(gelf_getsym(d, 1, &sym) != NULL);
    TASK_CHECK(sym.st_value == 2);
    TASK_CHECK(!strcmp(elf_strptr(c1, 2, sym.st_name), "same"));

    /* and back again */
    TASK_CHECK(read_same(c0, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(read_same(c1, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);

    TASK_CHECK(elf_end(c1) == 0);
    TASK_CHECK(elf_end(c0) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

static int fat_nested_archive(void) {
    unsigned char bytes[10000];
    Elf *ar, *fat, *child, *child0, *plain;
    Elf_FatRecord rec;
    size_t n, num;
    unsigned machine, value;
    const char *name;
    int fd;

    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);
    fd = open_ro(NESTED);
    TASK_CHECK(fd >= 0);
    ar = elf_begin(fd, ELF_C_READ, NULL);
    TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);

    fat = elf_begin(fd, ELF_C_READ, ar);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_kind(fat) == ELF_K_FAT);
    TASK_CHECK(elf_getfatnum(fat, &num) == 0 && num == 2);
    /* the container starts at the first archive member: 8 + 60 */
    TASK_CHECK(elf_getbase(fat) == 68);

    child = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, &name));
    TASK_CHECK(machine == 62 && value == 1 && !strcmp(name, "same"));
    /* cumulative offset: member header + record offset */
    TASK_CHECK(elf_getbase(child) == 68 + 4096);
    {
        char *raw;
        size_t rawsize;
        raw = elf_rawfile(child, &rawsize);
        TASK_CHECK(raw != NULL && rawsize == 768);
    }
    TASK_CHECK(elf_getfatrecord(fat, 0, &rec) == &rec && rec.offset == 4096);

    /* second record through the FatELF cursor */
    child0 = child;
    TASK_CHECK(elf_next(child0) == ELF_C_READ);
    child = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_getbase(child) == 68 + 8192);
    TASK_CHECK(elf_next(child) == ELF_C_NULL);

    /* opening past the last record fails */
    TASK_CHECK(elf_begin(fd, ELF_C_READ, fat) == NULL);
    TASK_CHECK(elf_errmsg(-1) != NULL);

    /* the next archive member is the ordinary 32-bit ELF */
    TASK_CHECK(elf_next(fat) == ELF_C_READ);
    plain = elf_begin(fd, ELF_C_READ, ar);
    TASK_CHECK(plain != NULL);
    TASK_CHECK(elf_kind(plain) == ELF_K_ELF);
    TASK_CHECK(read_same(plain, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_getbase(plain) > 68 + 8192);

    TASK_CHECK(elf_end(plain) == 0);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(child0) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    TASK_CHECK(elf_end(ar) == 0);
    TASK_CHECK(close(fd) == 0);

    /* ordinary archives keep their previous behaviour */
    fd = open_ro(NORMAL);
    TASK_CHECK(fd >= 0);
    ar = elf_begin(fd, ELF_C_READ, NULL);
    TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
    n = 0;
    child = elf_begin(fd, ELF_C_READ, ar);
    TASK_CHECK(child != NULL && elf_kind(child) == ELF_K_ELF);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(elf_getbase(child) == 68);
    TASK_CHECK(elf_next(child) == ELF_C_READ);
    plain = elf_begin(fd, ELF_C_READ, ar);
    TASK_CHECK(plain != NULL);
    TASK_CHECK(read_same(plain, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_next(plain) == ELF_C_NULL);
    TASK_CHECK(elf_end(plain) == 0);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(ar) == 0);
    TASK_CHECK(close(fd) == 0);
    (void)n;
    return 0;
}

static int fat_selection_contract(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *child;
    unsigned machine, value;
    size_t n = make_fat(bytes);

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);

    /* selection is metadata only: it opens nothing */
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    TASK_CHECK(elf_fatselect(fat, 0) == 0);
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(elf_end(child) == 0);

    /*
     * A failed selection does not change the position.  Open record 1 as
     * well so the cursor rests at the end, then select out of range; the
     * open that follows must still fail, and a valid selection recovers.
     */
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_end(child) == 0);

    TASK_CHECK(elf_fatselect(fat, 99) == -1);
    TASK_CHECK(elf_begin(-1, ELF_C_READ, fat) == NULL);
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_end(child) == 0);

    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

static int fat_rejects_write(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *child;
    size_t n = make_fat(bytes);
    int fd;

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);

    fd = open_ro(BUNDLE);
    TASK_CHECK(fd >= 0);
    TASK_CHECK(elf_begin(fd, ELF_C_RDWR, NULL) == NULL);
    TASK_CHECK(elf_errmsg(-1) != NULL);
    TASK_CHECK(close(fd) == 0);

    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_RDWR, fat) == NULL);
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(elf_begin(-1, ELF_C_RDWR, child) == NULL);
    TASK_CHECK(elf_update(child, ELF_C_NULL) == -1);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifetime: parent and descriptor-independent children                */
/* ------------------------------------------------------------------ */

static int fat_parent_closed_first(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *c0, *c1;
    unsigned machine, value;
    size_t n = make_fat(bytes);
    int fd;

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);

    /* file entry: close the FatELF handle while children live */
    fd = open_ro(BUNDLE);
    TASK_CHECK(fd >= 0);
    fat = elf_begin(fd, ELF_C_READ, NULL);
    TASK_CHECK(fat != NULL);
    c0 = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(c0 != NULL);
    c1 = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(c1 != NULL);
    /*
     * Releasing the FatELF handle drops one reference; the live children
     * still hold the shared image (activation-count convention).
     */
    TASK_CHECK(elf_end(fat) == 2);
    TASK_CHECK(close(fd) == 0);

    TASK_CHECK(read_same(c0, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(read_same(c1, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_getbase(c0) == 4096);
    TASK_CHECK(elf_getbase(c1) == 8192);

    TASK_CHECK(elf_end(c1) == 0);
    TASK_CHECK(elf_end(c0) == 0);

    /* memory entry: caller still owns the bytes */
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);
    c0 = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(c0 != NULL);
    TASK_CHECK(elf_end(fat) == 1);	/* child keeps the image alive */
    TASK_CHECK(read_same(c0, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(elf_rawfile(c0, NULL) == (char*)bytes + 4096);
    /* the caller's bytes remain intact */
    TASK_CHECK(bytes[4096] == 0x7f && bytes[4096 + 1] == 'E');
    TASK_CHECK(elf_end(c0) == 0);
    return 0;
}

static int fat_deferred_after_fdclose(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *child;
    unsigned machine, value;
    size_t n = make_fat(bytes);
    int fd;

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);

    /* touch nothing before disabling the descriptor */
    fd = open_ro(BUNDLE);
    TASK_CHECK(fd >= 0);
    fat = elf_begin(fd, ELF_C_READ, NULL);
    TASK_CHECK(fat != NULL);
    TASK_CHECK(elf_cntl(fat, ELF_C_FDREAD) == 0);
    TASK_CHECK(close(fd) == 0);

    /* open a record that was never accessed while the fd existed */
    TASK_CHECK(elf_fatselect(fat, 1) == 0);
    child = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 20 && value == 2);
    TASK_CHECK(elf_getbase(child) == 8192);
    TASK_CHECK(elf_end(child) == 0);

    /* the still-unvisited record is also readable */
    TASK_CHECK(elf_fatselect(fat, 0) == 0);
    child = elf_begin(fd, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(fat) == 0);

    /* memory containers need no descriptor at all */
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);
    child = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(child != NULL);
    TASK_CHECK(read_same(child, &machine, &value, NULL));
    TASK_CHECK(machine == 62 && value == 1);
    TASK_CHECK(elf_end(child) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

static int fat_reopen_reuses_descriptor(void) {
    unsigned char bytes[FAT_CAP];
    Elf *fat, *a, *b;
    size_t n = make_fat(bytes);

    TASK_CHECK(n == FAT_CAP);
    TASK_CHECK(elf_version(EV_CURRENT) == EV_CURRENT);
    fat = elf_memory((char*)bytes, sizeof(bytes));
    TASK_CHECK(fat != NULL);

    a = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(a != NULL);
    /* go back to record 0: the live descriptor is reused */
    TASK_CHECK(elf_fatselect(fat, 0) == 0);
    b = elf_begin(-1, ELF_C_READ, fat);
    TASK_CHECK(b == a);
    /* activation count: two end calls are needed to free the child */
    TASK_CHECK(elf_end(b) != 0);
    TASK_CHECK(elf_end(a) == 0);
    TASK_CHECK(elf_end(fat) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "records",  "metadata",          fat_metadata },
    { "records",  "reject_nonfat",     fat_reject_nonfat },
    { "records",  "validation",        fat_validation },
    { "records",  "inner_validation",  fat_inner_validation },
    { "views",    "mixed_targets",     fat_mixed_targets },
    { "views",    "interleaved",       fat_interleaved_views },
    { "views",    "nested_archive",    fat_nested_archive },
    { "views",    "selection",         fat_selection_contract },
    { "views",    "reject_write",      fat_rejects_write },
    { "lifetime", "parent_closed",     fat_parent_closed_first },
    { "lifetime", "deferred_fdclose",  fat_deferred_after_fdclose },
    { "lifetime", "reopen_reuse",      fat_reopen_reuses_descriptor },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
