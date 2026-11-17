/* Regression tests for ELF mapping symbols and disasm_buffer().
 *
 * Constructs synthetic little-endian ARM ELF32 images in memory (ET_REL and
 * ET_EXEC) and checks the text, addresses and state produced by
 * disasm_elf_section() and disasm_buffer().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "task_support/check.h"
#include "armdisasm.h"
#include "armelf.h"

/* ------------------------------------------------------------------ helpers */

typedef struct {
  uint32_t addresses[64];
  char texts[64][128];
  int count;
  int stop_at;              /* abort after this many callbacks, or -1 */
} CAPTURE;

static bool capture_cb(uint32_t address, const char *text, void *user)
{
  CAPTURE *cap = user;
  if (cap->count < 64) {
    cap->addresses[cap->count] = address;
    snprintf(cap->texts[cap->count], sizeof(cap->texts[0]), "%s", text);
  }
  cap->count += 1;
  if (cap->stop_at >= 0 && cap->count >= cap->stop_at)
    return false;
  return true;
}

typedef struct {
  uint32_t address;
  char text[128];
} LINE;

typedef struct {
  LINE lines[64];
  int count;
} LOG;

static bool log_cb(uint32_t address, const char *text, void *user)
{
  LOG *log = user;
  if (log->count < 64) {
    log->lines[log->count].address = address;
    snprintf(log->lines[log->count].text, sizeof(log->lines[0].text), "%s", text);
  }
  log->count += 1;
  return true;
}

/* ------------------------------------------------------------------- ELF builder */

#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHF_EXECINSTR 4u
#define ET_REL        1
#define ET_EXEC       2

#define STT_NOTYPE    0
#define STT_FUNC      2
#define STB_LOCAL     0
#define STB_GLOBAL    1

typedef struct {
  const char *name;
  uint16_t shndx;
  uint32_t value;
  uint8_t info;
} TEST_SYM;

typedef struct {
  uint32_t section_address;
  uint32_t section_size;
  uint32_t section_flags;
  uint32_t section_type;
  const uint8_t *content;
  size_t content_size;
  uint16_t type;               /* ET_REL or ET_EXEC */
} SECTION_SPEC;

typedef struct {
  const TEST_SYM *symbols;
  size_t symbolcount;
  const char *strtab;
  size_t strtabsize;
} SYMTAB_SPEC;

/* Extendable byte image used during construction. */
typedef struct {
  uint8_t data[4096];
  size_t size;
} IMAGE;

static uint32_t append(IMAGE *img, const void *data, size_t len)
{
  uint32_t offset = (uint32_t)img->size;
  if (img->size + len > sizeof(img->data))
    abort();
  if (len > 0)
    memcpy(img->data + img->size, data, len);
  img->size += len;
  return offset;
}

static void put_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* find a NUL-terminated string inside a string table (which itself starts with
 * an empty entry), returning its table offset or 0 */
static uint32_t strtab_offset(const char *strtab, size_t strtabsize, const char *name)
{
  size_t pos = 1;              /* offset 0 is the mandatory empty string */
  while (pos < strtabsize) {
    const char *entry = strtab + pos;
    size_t len = strnlen(entry, strtabsize - pos);
    if (len < strtabsize - pos && strcmp(entry, name) == 0)
      return (uint32_t)pos;
    pos += len + 1;
  }
  return 0;
}

/* Build an image with exactly one selected PROGBITS section, one SYMTAB and
 * one STRTAB. Returns the image plus the index of the selected section. */
static bool build_elf(IMAGE *img, const SECTION_SPEC *spec, const SYMTAB_SPEC *symtab,
                      uint32_t *section_index_out)
{
  memset(img, 0, sizeof(*img));
  img->size = 0;

  /* content placed after the header; header patched later */
  uint8_t hdr[52];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 0x7f;
  hdr[1] = 'E';
  hdr[2] = 'L';
  hdr[3] = 'F';
  hdr[4] = 1;                  /* ELFCLASS32 */
  hdr[5] = 1;                  /* ELFDATA2LSB */
  hdr[6] = 1;                  /* EV_CURRENT */
  put_u16(hdr + 16, spec->type);
  put_u16(hdr + 18, 40);       /* EM_ARM */
  put_u32(hdr + 20, 1);
  append(img, hdr, sizeof(hdr));

  /* section contents, kept 4-byte aligned */
  while (img->size % 4 != 0)
    append(img, "", 1);
  uint32_t content_offset = append(img, spec->content, spec->content_size);

  /* null string table plus symbol names */
  while (img->size % 4 != 0)
    append(img, "", 1);
  uint32_t strtab_off = append(img, symtab->strtab, symtab->strtabsize);

  /* Some tests deliberately add a decoy function symbol located in another
     section; a small extra PROGBITS section holds it. */
  static const uint8_t decoy_data[4] = { 0x00, 0xbf, 0x00, 0xbf };
  while (img->size % 4 != 0)
    append(img, "", 1);
  uint32_t decoy_offset = append(img, decoy_data, sizeof(decoy_data));

  /* symbol table */
  while (img->size % 4 != 0)
    append(img, "", 1);
  uint32_t symtab_offset = img->size;
  uint8_t nullsym[16];
  memset(nullsym, 0, sizeof(nullsym));
  append(img, nullsym, sizeof(nullsym));
  for (size_t i = 0; i < symtab->symbolcount; i++) {
    const TEST_SYM *sym = &symtab->symbols[i];
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    uint32_t nameoff = 0;
    if (sym->name != NULL && sym->name[0] != '\0')
      nameoff = strtab_offset(symtab->strtab, symtab->strtabsize, sym->name);
    put_u32(raw + 0, nameoff);
    put_u32(raw + 4, sym->value);
    raw[12] = sym->info;
    put_u16(raw + 14, sym->shndx);
    append(img, raw, sizeof(raw));
  }

  /* section header table; section 1 = selected, 2 = decoy PROGBITS,
     3 = SYMTAB, 4 = STRTAB */
  while (img->size % 4 != 0)
    append(img, "", 1);
  uint32_t shoff = img->size;

  typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
  } SH;
  SH sh[5];
  memset(sh, 0, sizeof(sh));
  sh[1].type = spec->section_type;
  sh[1].flags = spec->section_flags;
  sh[1].addr = spec->section_address;
  sh[1].offset = content_offset;
  sh[1].size = spec->section_size;
  sh[1].addralign = 4;
  sh[2].type = SHT_PROGBITS;
  sh[2].flags = SHF_EXECINSTR;
  /* deliberately the same address as the selected section: switching sections in
     one state must still give independent results */
  sh[2].addr = spec->section_address;
  sh[2].offset = decoy_offset;
  sh[2].size = sizeof(decoy_data);
  sh[2].addralign = 4;
  sh[3].type = SHT_SYMTAB;
  sh[3].offset = symtab_offset;
  sh[3].size = (uint32_t)((symtab->symbolcount + 1) * 16);
  sh[3].link = 4;
  sh[3].entsize = 16;
  sh[4].type = SHT_STRTAB;
  sh[4].offset = strtab_off;
  sh[4].size = (uint32_t)symtab->strtabsize;

  for (int i = 0; i < 5; i++) {
    uint8_t raw[40];
    memset(raw, 0, sizeof(raw));
    put_u32(raw + 0, sh[i].name);
    put_u32(raw + 4, sh[i].type);
    put_u32(raw + 8, sh[i].flags);
    put_u32(raw + 12, sh[i].addr);
    put_u32(raw + 16, sh[i].offset);
    put_u32(raw + 20, sh[i].size);
    put_u32(raw + 24, sh[i].link);
    put_u32(raw + 28, sh[i].info);
    put_u32(raw + 32, sh[i].addralign);
    put_u32(raw + 36, sh[i].entsize);
    append(img, raw, sizeof(raw));
  }

  /* patch the ELF header now that offsets are known */
  put_u32(img->data + 32, shoff);
  put_u16(img->data + 46, 40);
  put_u16(img->data + 48, 5);
  put_u16(img->data + 50, 0);

  *section_index_out = 1;
  return true;
}

#define SYM(name, shndx, value, bind, type) \
  { name, shndx, value, (uint8_t)(((bind) << 4) | (type)) }

/* ---------------------------------------------------------- test: elf_scope */

static int test_elf_rel_mapping_and_scope(void)
{
  /* Thumb NOP; ARM "mov r0, r0"; two data bytes. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0x00, 0xa0, 0xe1, 0x01, 0x02 };
  static const char strtab[] = "\0$t\0$a.1\0$d\0thumb_fn\0arm_fn\0other_fn\0";
  static const TEST_SYM syms[] = {
    SYM("$t", 1, 0, STB_LOCAL, STT_NOTYPE),
    SYM("$a.1", 1, 2, STB_LOCAL, STT_NOTYPE),
    SYM("$d", 1, 6, STB_LOCAL, STT_NOTYPE),
    SYM("thumb_fn", 1, 1, STB_GLOBAL, STT_FUNC),    /* odd value -> Thumb at 0 */
    SYM("arm_fn", 1, 2, STB_GLOBAL, STT_FUNC),
    SYM("other_fn", 2, 0, STB_GLOBAL, STT_FUNC),    /* decoy: other section */
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_address = 0x1000;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));

  disasm_init(&state, DISASM_ADDRESS | DISASM_COMMENT);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(log.count == 4);

  /* first instruction is Thumb NOP at 0x1000 */
  TASK_CHECK(log.lines[0].address == 0x1000);
  TASK_CHECK(strcmp(log.lines[0].text, "00001000    nop") == 0);
  /* second is ARM mov r0, r0 at 0x1002 */
  TASK_CHECK(log.lines[1].address == 0x1002);
  TASK_CHECK(strncmp(log.lines[1].text + 12, "mov", 3) == 0);
  /* two data bytes at 0x1006..0x1007 */
  TASK_CHECK(log.lines[2].address == 0x1006);
  TASK_CHECK(strncmp(log.lines[2].text + 12, ".byte   0x01", 12) == 0);
  TASK_CHECK(log.lines[3].address == 0x1007);
  TASK_CHECK(strncmp(log.lines[3].text + 12, ".byte   0x02", 12) == 0);

  /* ordinary function names from the selected section only, and with the Thumb
     address bit cleared */
  TASK_CHECK(state.symbolcount == 2);
  TASK_CHECK(state.symbols[0].address == 0x1000);
  TASK_CHECK(state.symbols[0].mode == ARMMODE_THUMB);
  TASK_CHECK(state.symbols[0].name != NULL && strcmp(state.symbols[0].name, "thumb_fn") == 0);
  TASK_CHECK(state.symbols[1].address == 0x1002);
  TASK_CHECK(state.symbols[1].name != NULL && strcmp(state.symbols[1].name, "arm_fn") == 0);
  for (int i = 0; i < state.symbolcount; i++)
    TASK_CHECK(state.symbols[i].name == NULL || strcmp(state.symbols[i].name, "other_fn") != 0);
  /* mapping symbols are kept separately from ordinary symbols */
  TASK_CHECK(state.mappingcount == 3);
  TASK_CHECK(state.mappings[0].address == 0x1000 && state.mappings[0].mode == ARMMODE_THUMB);
  TASK_CHECK(state.mappings[1].address == 0x1002 && state.mappings[1].mode == ARMMODE_ARM);
  TASK_CHECK(state.mappings[2].address == 0x1006 && state.mappings[2].mode == ARMMODE_DATA);

  disasm_cleanup(&state);
  TASK_CHECK(state.symbols == NULL);
  TASK_CHECK(state.mappings == NULL);
  return 0;
}

static int test_elf_exec_address_coordinates(void)
{
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf };
  static const char strtab[] = "\0$t\0start\0";
  static const TEST_SYM syms[] = {
    SYM("$t", 1, 0x4000, STB_LOCAL, STT_NOTYPE),
    SYM("start", 1, 0x4001, STB_GLOBAL, STT_FUNC),
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_EXEC;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_address = 0x4000;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));

  disasm_init(&state, DISASM_ADDRESS);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(log.lines[0].address == 0x4000);
  TASK_CHECK(log.lines[3].address == 0x4006);
  TASK_CHECK(strncmp(log.lines[0].text, "00004000", 8) == 0);

  disasm_cleanup(&state);
  return 0;
}

static int test_elf_rejects_unsupported(void)
{
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf };
  static const char strtab[] = "\0$t\0";
  static const TEST_SYM syms[] = { SYM("$t", 1, 0, STB_LOCAL, STT_NOTYPE) };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  /* make section 2 a non-executable PROGBITS section for the rejection check */
  {
    size_t shoff = (size_t)img.data[32] | ((size_t)img.data[33] << 8)
                 | ((size_t)img.data[34] << 16) | ((size_t)img.data[35] << 24);
    put_u32(img.data + shoff + 2 * 40 + 8, 0); /* sh_flags of section 2 */
  }
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, 0);

  /* corrupt magic */
  IMAGE bad = img;
  bad.data[0] = 0;
  TASK_CHECK(!disasm_elf_section(&state, bad.data, bad.size, index, log_cb, &log));

  /* wrong machine */
  bad = img;
  put_u16(bad.data + 18, 3);    /* EM_386 */
  TASK_CHECK(!disasm_elf_section(&state, bad.data, bad.size, index, log_cb, &log));

  /* wrong ELF class (64-bit) */
  bad = img;
  bad.data[4] = 2;
  TASK_CHECK(!disasm_elf_section(&state, bad.data, bad.size, index, log_cb, &log));

  /* big endian */
  bad = img;
  bad.data[5] = 2;
  TASK_CHECK(!disasm_elf_section(&state, bad.data, bad.size, index, log_cb, &log));

  /* out-of-range section index */
  TASK_CHECK(!disasm_elf_section(&state, img.data, img.size, 99, log_cb, &log));

  /* truncated image */
  TASK_CHECK(!disasm_elf_section(&state, img.data, 20, index, log_cb, &log));

  /* non-executable PROGBITS section (section 2, flags cleared above) */
  TASK_CHECK(!disasm_elf_section(&state, img.data, img.size, 2, log_cb, &log));

  /* SHT_STRTAB section */
  TASK_CHECK(!disasm_elf_section(&state, img.data, img.size, 4, log_cb, &log));

  /* NULL arguments */
  TASK_CHECK(!disasm_elf_section(NULL, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(!disasm_elf_section(&state, NULL, img.size, index, log_cb, &log));
  TASK_CHECK(!disasm_elf_section(&state, img.data, img.size, index, NULL, &log));

  disasm_cleanup(&state);
  return 0;
}

static int test_elf_scope_between_sections(void)
{
  /* Two different executable sections share the same image; disassembling one
     then the other must give independent results. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf };
  static const char strtab[] = "\0$t\0fn\0";
  static const TEST_SYM syms[] = {
    SYM("$t", 1, 0, STB_LOCAL, STT_NOTYPE),
    SYM("fn", 1, 1, STB_GLOBAL, STT_FUNC),
    SYM("fn", 2, 1, STB_GLOBAL, STT_FUNC),
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  /* the decoy section 2 overlaps the same value range, but its symbols must
     not be visible while section 1 is selected */
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_COMMENT);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(state.symbolcount == 1);
  TASK_CHECK(state.symbols[0].name != NULL && strcmp(state.symbols[0].name, "fn") == 0);
  TASK_CHECK(state.mappingcount == 1);
  TASK_CHECK(state.mappings[0].address == 0 && state.mappings[0].mode == ARMMODE_THUMB);

  /* without an intervening cleanup, switch to the other section: its symbols,
     mappings and code pool replace the previous ones */
  memset(&log, 0, sizeof(log));
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, 2, log_cb, &log));
  TASK_CHECK(log.count == 2);   /* decoy section is 4 bytes: two Thumb NOPs */
  TASK_CHECK(state.symbolcount == 1);
  TASK_CHECK(state.symbols[0].name != NULL && strcmp(state.symbols[0].name, "fn") == 0);
  TASK_CHECK(state.symbols[0].mode == ARMMODE_THUMB);
  TASK_CHECK(state.mappingcount == 0); /* no mapping symbols in that section */
  disasm_cleanup(&state);
  return 0;
}

static int test_elf_section_starting_with_data(void)
{
  /* A section whose first mapping symbol marks data must not be decoded, even
     though it is an executable section. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf };
  static const char strtab[] = "\0$d\0$t\0";
  static const TEST_SYM syms[] = {
    SYM("$d", 1, 0, STB_LOCAL, STT_NOTYPE),
    SYM("$t", 1, 4, STB_LOCAL, STT_NOTYPE),
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_address = 0xe000;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(log.count == 6);
  TASK_CHECK(strcmp(log.lines[0].text, "0000e000    .byte   0x00") == 0);
  TASK_CHECK(strcmp(log.lines[1].text, "0000e001    .byte   0xbf") == 0);
  TASK_CHECK(strcmp(log.lines[2].text, "0000e002    .byte   0x00") == 0);
  TASK_CHECK(strcmp(log.lines[3].text, "0000e003    .byte   0xbf") == 0);
  TASK_CHECK(strncmp(log.lines[4].text + 12, "nop", 3) == 0);
  TASK_CHECK(strncmp(log.lines[5].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[4].address == 0xe004);
  disasm_cleanup(&state);
  return 0;
}

/* ------------------------------------------------------- test: mixed_ranges */

static int test_mixed_ranges(void)
{
  /* 0x1000: $t  Thumb nop, Thumb 16-bit
     0x1002: $a  ARM mov r0, r0
     0x1006: $d  data bytes
     0x1008: $t  Thumb nop */
  static const uint8_t code[] = {
    0x00, 0xbf,                         /* thumb nop */
    0x00, 0x00, 0xa0, 0xe1,             /* arm mov r0, r0 */
    0x11, 0x22,                         /* data */
    0x00, 0xbf,                         /* thumb nop */
  };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x1000);
  TASK_CHECK(disasm_mapping(&state, 0x1000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&state, 0x1002, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&state, 0x1006, ARMMODE_DATA));
  TASK_CHECK(disasm_mapping(&state, 0x1008, ARMMODE_THUMB));

  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 5);
  TASK_CHECK(log.lines[0].address == 0x1000);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[1].address == 0x1002);
  TASK_CHECK(strncmp(log.lines[1].text + 12, "mov", 3) == 0);
  TASK_CHECK(log.lines[2].address == 0x1006);
  TASK_CHECK(strncmp(log.lines[2].text + 12, ".byte   0x11", 12) == 0);
  TASK_CHECK(log.lines[3].address == 0x1007);
  TASK_CHECK(strncmp(log.lines[3].text + 12, ".byte   0x22", 12) == 0);
  TASK_CHECK(log.lines[4].address == 0x1008);
  TASK_CHECK(strncmp(log.lines[4].text + 12, "nop", 3) == 0);
  TASK_CHECK(state.size == 2);
  disasm_cleanup(&state);
  return 0;
}

static int test_mapping_merge_and_conflict(void)
{
  ARMSTATE state;
  disasm_init(&state, 0);
  TASK_CHECK(disasm_mapping(&state, 0x2000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&state, 0x2000, ARMMODE_THUMB)); /* same mode: merge */
  TASK_CHECK(state.mappingcount == 1);
  TASK_CHECK(!disasm_mapping(&state, 0x2000, ARMMODE_ARM));   /* conflict */
  TASK_CHECK(!disasm_mapping(&state, 0x2000, ARMMODE_DATA));
  TASK_CHECK(state.mappingcount == 1);
  /* unsupported mode */
  TASK_CHECK(!disasm_mapping(&state, 0x2004, ARMMODE_UNKNOWN));
  TASK_CHECK(state.mappingcount == 1);
  /* address ordering */
  TASK_CHECK(disasm_mapping(&state, 0x2008, ARMMODE_DATA));
  TASK_CHECK(disasm_mapping(&state, 0x2004, ARMMODE_ARM));
  TASK_CHECK(state.mappingcount == 3);
  TASK_CHECK(state.mappings[0].address == 0x2000);
  TASK_CHECK(state.mappings[1].address == 0x2004);
  TASK_CHECK(state.mappings[2].address == 0x2008);
  disasm_clear_mappings(&state);
  TASK_CHECK(state.mappingcount == 0);
  TASK_CHECK(state.mappings == NULL);
  disasm_cleanup(&state);
  return 0;
}

static int test_mapping_does_not_override_literal_pool(void)
{
  /* A previous LDR marks 0x3004 as a literal pool. A code mapping must still
     decode an instruction there. */
  static const uint8_t code[] = {
    0x00, 0x48,             /* ldr r0, [pc, #0] -> 0x3004 */
    0x00, 0xbf,             /* nop at 0x3002 */
    0x00, 0xbf,             /* nop at 0x3004: would be .word if the pool won */
    0x00, 0xbf,             /* nop at 0x3006 */
  };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x3000);
  disasm_mapping(&state, 0x3000, ARMMODE_THUMB);
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "ldr", 3) == 0);
  for (int i = 1; i < 4; i++)
    TASK_CHECK(strncmp(log.lines[i].text + 12, "nop", 3) == 0);
  /* the literal-pool speculation really is present in the code pool */
  TASK_CHECK(state.poolcount >= 2);
  {
    bool found_literal = false;
    for (int i = 0; i < state.poolcount; i++)
      if (state.codepool[i].address == 0x3004)
        found_literal = true;
    TASK_CHECK(found_literal);
  }
  disasm_cleanup(&state);
  return 0;
}

static int test_data_range_no_instruction_decode(void)
{
  /* A byte sequence that would decode as a Thumb instruction, plus an ARM word,
     but is mapped as data. */
  static const uint8_t data[] = { 0x00, 0xbf, 0x00, 0x00, 0xa0, 0xe1, 0xff };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x5000);
  disasm_mapping(&state, 0x5000, ARMMODE_DATA);
  TASK_CHECK(disasm_buffer(&state, data, sizeof(data), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == (int)sizeof(data));
  for (int i = 0; i < (int)sizeof(data); i++) {
    TASK_CHECK(log.lines[i].address == 0x5000u + (uint32_t)i);
    TASK_CHECK(strncmp(log.lines[i].text + 12, ".byte   0x", 10) == 0);
  }
  TASK_CHECK(strncmp(log.lines[0].text + 12, ".byte   0x00", 12) == 0);
  TASK_CHECK(strncmp(log.lines[6].text + 12, ".byte   0xff", 12) == 0);
  TASK_CHECK(state.size == 1);
  disasm_cleanup(&state);
  return 0;
}

static int test_range_end_fragments(void)
{
  /* ARM range of 6 bytes: one complete instruction then two fragment bytes.
     Followed by a Thumb range of 3 bytes: one 16-bit instruction then one
     remaining byte. */
  static const uint8_t code[] = {
    0x00, 0x00, 0xa0, 0xe1, 0xaa, 0xbb,        /* ARM mov + 2 bytes */
    0x00, 0xbf, 0xcc,                          /* thumb nop + 1 byte */
  };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x6000);
  TASK_CHECK(disasm_mapping(&state, 0x6000, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&state, 0x6006, ARMMODE_THUMB));
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 5);
  TASK_CHECK(log.lines[0].address == 0x6000);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "mov", 3) == 0);
  TASK_CHECK(log.lines[1].address == 0x6004);
  TASK_CHECK(strncmp(log.lines[1].text + 12, ".byte   0xaa", 12) == 0);
  TASK_CHECK(log.lines[2].address == 0x6005);
  TASK_CHECK(strncmp(log.lines[2].text + 12, ".byte   0xbb", 12) == 0);
  TASK_CHECK(log.lines[3].address == 0x6006);
  TASK_CHECK(strncmp(log.lines[3].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[4].address == 0x6008);
  TASK_CHECK(strncmp(log.lines[4].text + 12, ".byte   0xcc", 12) == 0);
  disasm_cleanup(&state);
  return 0;
}

static int test_thumb32_does_not_cross_range(void)
{
  /* A Thumb 32-bit encoding at the end of a 2-byte range must be emitted as
     bytes rather than consuming the next range's bytes. */
  static const uint8_t code[] = {
    0x00, 0xf0,             /* first half of a 32-bit Thumb instruction */
    0x00, 0xbf,             /* next range: thumb nop */
    0x00, 0xbf,
  };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x7000);
  TASK_CHECK(disasm_mapping(&state, 0x7000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&state, 0x7002, ARMMODE_THUMB));
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(log.lines[0].address == 0x7000);
  TASK_CHECK(strncmp(log.lines[0].text + 12, ".byte   0x00", 12) == 0);
  TASK_CHECK(log.lines[1].address == 0x7001);
  TASK_CHECK(strncmp(log.lines[1].text + 12, ".byte   0xf0", 12) == 0);
  TASK_CHECK(log.lines[2].address == 0x7002);
  TASK_CHECK(strncmp(log.lines[2].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[3].address == 0x7004);
  TASK_CHECK(strncmp(log.lines[3].text + 12, "nop", 3) == 0);
  disasm_cleanup(&state);
  return 0;
}

static int test_mapping_prefix_lookup_only_current(void)
{
  /* A mapping symbol at a later address must not change the initial mode. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x8000);
  TASK_CHECK(disasm_mapping(&state, 0x8002, ARMMODE_ARM));
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 3);
  TASK_CHECK(log.lines[0].address == 0x8000);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "nop", 3) == 0);
  /* the ARM range starts at 0x8002, where only two bytes remain: fragment */
  TASK_CHECK(log.lines[1].address == 0x8002);
  TASK_CHECK(strncmp(log.lines[1].text + 12, ".byte   0x00", 12) == 0);
  TASK_CHECK(log.lines[2].address == 0x8003);
  TASK_CHECK(strncmp(log.lines[2].text + 12, ".byte   0xbf", 12) == 0);
  disasm_cleanup(&state);
  return 0;
}

/* ------------------------------------------------------------- test: state */

static int test_state_callback_addresses(void)
{
  static const uint8_t code[] = { 0x00, 0xbf, 0xaa, 0xbb, 0x00, 0xbf };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0x9000);
  disasm_mapping(&state, 0x9000, ARMMODE_THUMB);
  disasm_mapping(&state, 0x9002, ARMMODE_DATA);
  disasm_mapping(&state, 0x9004, ARMMODE_THUMB);
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(log.lines[0].address == 0x9000);
  TASK_CHECK(log.lines[1].address == 0x9002);
  TASK_CHECK(log.lines[2].address == 0x9003);
  TASK_CHECK(log.lines[3].address == 0x9004);
  /* state reflects the last consumed item */
  TASK_CHECK(state.address == 0x9004);
  TASK_CHECK(state.size == 2);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_callback_abort(void)
{
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf };
  ARMSTATE state;
  CAPTURE cap;
  memset(&cap, 0, sizeof(cap));
  cap.stop_at = 2;
  disasm_init(&state, 0);
  disasm_address(&state, 0xa000);
  disasm_mapping(&state, 0xa000, ARMMODE_THUMB);
  TASK_CHECK(!disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, capture_cb, &cap));
  TASK_CHECK(cap.count == 2);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_it_cleared_across_data(void)
{
  /* Start an IT block, then switch to data and back to code; the predicate must
     not carry over the data range. */
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0xb000);
  disasm_mapping(&state, 0xb000, ARMMODE_THUMB);
  disasm_mapping(&state, 0xb002, ARMMODE_DATA);
  disasm_mapping(&state, 0xb004, ARMMODE_THUMB);
  /* itt eq ; then data ; then a conditional-less movs r0, #1 */
  static const uint8_t code[] = {
    0x04, 0xbf,             /* itt eq */
    0x01, 0x02,             /* data */
    0x01, 0x20,             /* movs r0, #1 */
  };
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_THUMB, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "itt", 3) == 0);
  TASK_CHECK(strncmp(log.lines[1].text + 12, ".byte", 5) == 0);
  TASK_CHECK(log.lines[2].address == 0xb003);
  TASK_CHECK(strncmp(log.lines[3].text + 12, "movs", 4) == 0);
  TASK_CHECK(strstr(log.lines[3].text, "eq") == NULL);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_cleanup_frees_mappings(void)
{
  ARMSTATE state;
  disasm_init(&state, 0);
  disasm_mapping(&state, 0x100, ARMMODE_ARM);
  disasm_mapping(&state, 0x200, ARMMODE_DATA);
  TASK_CHECK(state.mappingcount == 2);
  disasm_cleanup(&state);
  TASK_CHECK(state.mappings == NULL);
  TASK_CHECK(state.mappingcount == 0);
  TASK_CHECK(state.mappingsize == 0);
  TASK_CHECK(state.symbols == NULL);
  return 0;
}

static int test_state_symbol_rules_without_mapping(void)
{
  /* Without mapping symbols, the original ordinary-symbol mode switching must
     still apply: the symbol at the buffer start sets the mode, and crossing a
     later symbol switches the mode. */
  static const uint8_t code[] = {
    0x00, 0x00, 0xa0, 0xe1,             /* arm mov r0, r0 at 0xc000 */
    0x00, 0xbf, 0x00, 0xbf,             /* thumb nop, nop at 0xc004 */
  };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0xc000);
  disasm_symbol(&state, "fn_arm", 0xc000, ARMMODE_ARM);
  disasm_symbol(&state, "fn_thumb", 0xc004, ARMMODE_THUMB);
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_UNKNOWN, log_cb, &log));
  TASK_CHECK(log.count == 3);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "mov", 3) == 0);
  TASK_CHECK(log.lines[0].address == 0xc000);
  TASK_CHECK(strncmp(log.lines[1].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[1].address == 0xc004);
  TASK_CHECK(strncmp(log.lines[2].text + 12, "nop", 3) == 0);
  TASK_CHECK(log.lines[2].address == 0xc006);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_mapping_overrides_ordinary_mode(void)
{
  /* When a mapping symbol is in effect, it decides the mode even if an ordinary
     symbol at the same address claims otherwise. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf };
  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  disasm_address(&state, 0xc100);
  disasm_symbol(&state, "fn", 0xc100, ARMMODE_ARM);
  disasm_mapping(&state, 0xc100, ARMMODE_THUMB);
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_ARM, log_cb, &log));
  TASK_CHECK(log.count == 2);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "nop", 3) == 0);
  TASK_CHECK(strncmp(log.lines[1].text + 12, "nop", 3) == 0);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_mapping_survives_repeated_call(void)
{
  /* The mappings installed by disasm_elf_section() must also apply when
     disasm_buffer() is invoked directly afterwards. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x11, 0x22, 0x00, 0xbf };
  static const char strtab[] = "\0$t\0$d\0$t\0";
  static const TEST_SYM syms[] = {
    SYM("$t", 1, 0, STB_LOCAL, STT_NOTYPE),
    SYM("$d", 1, 2, STB_LOCAL, STT_NOTYPE),
    SYM("$t", 1, 4, STB_LOCAL, STT_NOTYPE),
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_address = 0xd000;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(log.count == 4);

  /* direct buffer call at the same address; mapping symbols are still installed */
  memset(&log, 0, sizeof(log));
  disasm_address(&state, 0xd000);
  TASK_CHECK(state.mappingcount == 3);
  TASK_CHECK(disasm_buffer(&state, code, sizeof(code), ARMMODE_DATA, log_cb, &log));
  TASK_CHECK(log.count == 4);
  TASK_CHECK(log.lines[0].address == 0xd000);
  TASK_CHECK(strncmp(log.lines[0].text + 12, "nop", 3) == 0);
  TASK_CHECK(strncmp(log.lines[1].text + 12, ".byte   0x11", 12) == 0);
  disasm_cleanup(&state);
  return 0;
}

static int test_state_rebuild_resets_previous_section(void)
{
  /* Installing symbols/mappings for one section twice must not accumulate. */
  static const uint8_t code[] = { 0x00, 0xbf, 0x00, 0xbf };
  static const char strtab[] = "\0$t\0fn\0";
  static const TEST_SYM syms[] = {
    SYM("$t", 1, 0, STB_LOCAL, STT_NOTYPE),
    SYM("fn", 1, 1, STB_GLOBAL, STT_FUNC),
  };
  SYMTAB_SPEC symtab = { syms, sizeof(syms) / sizeof(syms[0]), strtab, sizeof(strtab) };
  SECTION_SPEC spec;
  memset(&spec, 0, sizeof(spec));
  spec.type = ET_REL;
  spec.section_type = SHT_PROGBITS;
  spec.section_flags = SHF_EXECINSTR;
  spec.section_size = sizeof(code);
  spec.content = code;
  spec.content_size = sizeof(code);

  IMAGE img;
  uint32_t index;
  build_elf(&img, &spec, &symtab, &index);

  ARMSTATE state;
  LOG log;
  memset(&log, 0, sizeof(log));
  disasm_init(&state, DISASM_ADDRESS);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  int syms1 = state.symbolcount, maps1 = state.mappingcount;
  TASK_CHECK(syms1 == 1);
  TASK_CHECK(maps1 == 1);
  TASK_CHECK(disasm_elf_section(&state, img.data, img.size, index, log_cb, &log));
  TASK_CHECK(state.symbolcount == syms1);
  TASK_CHECK(state.mappingcount == maps1);
  disasm_cleanup(&state);
  return 0;
}

/* ------------------------------------------------------------------ registry */

const struct task_case task_cases[] = {
  { "elf_scope",   "rel_mapping_and_scope",     test_elf_rel_mapping_and_scope },
  { "elf_scope",   "exec_address_coordinates",  test_elf_exec_address_coordinates },
  { "elf_scope",   "rejects_unsupported",       test_elf_rejects_unsupported },
  { "elf_scope",   "section_isolation",         test_elf_scope_between_sections },
  { "elf_scope",   "section_starts_with_data",  test_elf_section_starting_with_data },
  { "mixed_ranges", "even_mixed",               test_mixed_ranges },
  { "mixed_ranges", "merge_and_conflict",       test_mapping_merge_and_conflict },
  { "mixed_ranges", "literal_pool_override",    test_mapping_does_not_override_literal_pool },
  { "mixed_ranges", "data_no_decode",           test_data_range_no_instruction_decode },
  { "mixed_ranges", "range_end_fragments",      test_range_end_fragments },
  { "mixed_ranges", "thumb32_range_end",        test_thumb32_does_not_cross_range },
  { "mixed_ranges", "no_future_mapping",        test_mapping_prefix_lookup_only_current },
  { "state",       "callback_addresses",        test_state_callback_addresses },
  { "state",       "callback_abort",            test_state_callback_abort },
  { "state",       "it_cleared_across_data",    test_state_it_cleared_across_data },
  { "state",       "cleanup_frees_mappings",    test_state_cleanup_frees_mappings },
  { "state",       "symbol_rules_without_map",  test_state_symbol_rules_without_mapping },
  { "state",       "mapping_beats_symbol",      test_state_mapping_overrides_ordinary_mode },
  { "state",       "mapping_survives_direct",   test_state_mapping_survives_repeated_call },
  { "state",       "rebuild_resets",            test_state_rebuild_resets_previous_section },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
