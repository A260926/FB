/* Regression tests for ELF mapping symbols and mixed ARM/Thumb/data ranges.
 *
 * The cases are grouped (via task_support/check.h) into:
 *   elf_scope     - synthetic ELF parsing, symbol/section scoping, coordinates
 *   mixed_ranges  - disasm_buffer() mapping ranges, fragments, IT and abort
 *   state         - mapping list state, ELF rebuild isolation, cleanup
 *
 * Everything here is plain C11 plus the standard library; the ELF images are
 * built in memory (little-endian ELF32 / EM_ARM) and never read from disk.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "armdisasm.h"
#include "armelf.h"

/* ------------------------------------------------------------------ */
/* capture helper                                                     */
/* ------------------------------------------------------------------ */

#define CAP_MAX 160
struct rec {
  uint32_t addr;
  int size;
  char text[160];
};
struct cap {
  ARMSTATE *st;
  int n;
  int fail_at;      /* when >0, the fail_at-th callback returns false */
  struct rec lines[CAP_MAX];
};

static bool cap_cb(uint32_t address, const char *text, void *user)
{
  struct cap *c = (struct cap *)user;
  if (c->n < CAP_MAX) {
    struct rec *r = &c->lines[c->n];
    r->addr = address;
    r->size = c->st ? c->st->size : -1;
    strncpy(r->text, text, sizeof r->text - 1);
    r->text[sizeof r->text - 1] = '\0';
    c->n += 1;
  } else {
    c->n += 1;
  }
  if (c->fail_at > 0 && c->n == c->fail_at)
    return false;
  return true;
}

static int match_line(const struct cap *c, int i, uint32_t addr, int size,
                      const char *text)
{
  if (i < 0 || i >= c->n || i >= CAP_MAX) {
    printf("    line %d missing (callbacks=%d)\n", i, c->n);
    return 0;
  }
  const struct rec *r = &c->lines[i];
  if (r->addr != addr || r->size != size || strcmp(r->text, text) != 0) {
    printf("    line %d mismatch\n      expected a=%08x s=%d [%s]\n"
           "      actual   a=%08x s=%d [%s]\n",
           i, addr, size, text, r->addr, r->size, r->text);
    return 0;
  }
  return 1;
}

static int has_substr(const struct cap *c, int i, const char *needle)
{
  if (i < 0 || i >= c->n || i >= CAP_MAX)
    return 0;
  return strstr(c->lines[i].text, needle) != NULL;
}

static int symbol_index(const ARMSTATE *s, const char *name)
{
  for (int i = 0; i < s->symbolcount; i++) {
    if (s->symbols[i].name && strcmp(s->symbols[i].name, name) == 0)
      return i;
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* tiny in-memory ELF32 little-endian builder                         */
/* ------------------------------------------------------------------ */

#define EB_CAP 32768

struct eb_sym {
  const char *name;
  uint32_t value;
  uint8_t bind;
  uint8_t type;
  uint16_t shndx;
};

struct eb {
  uint8_t img[EB_CAP];
  size_t len;
  int ok;
  uint32_t shoff;
  uint32_t text_sh_off;   /* file offset of the selected (index 1) shdr */
};

static void eb_w16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)(v >> 8);
}

static void eb_w32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t eb_align(struct eb *b, uint32_t a)
{
  while (b->len % a != 0) {
    if (b->len < sizeof b->img)
      b->img[b->len] = 0;
    b->len++;
  }
  return (uint32_t)b->len;
}

static uint32_t eb_bytes(struct eb *b, const void *data, size_t n, uint32_t a)
{
  uint32_t off = eb_align(b, a);
  if (b->len + n > sizeof b->img) {
    b->ok = 0;
    return off;
  }
  memcpy(b->img + b->len, data, n);
  b->len += n;
  return off;
}

struct eb_sh {
  uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
};

static void eb_put_sh(uint8_t *p, const struct eb_sh *s)
{
  eb_w32(p + 0, s->name);
  eb_w32(p + 4, s->type);
  eb_w32(p + 8, s->flags);
  eb_w32(p + 12, s->addr);
  eb_w32(p + 16, s->offset);
  eb_w32(p + 20, s->size);
  eb_w32(p + 24, s->link);
  eb_w32(p + 28, s->info);
  eb_w32(p + 32, s->addralign);
  eb_w32(p + 36, s->entsize);
}

struct eb_cfg {
  uint16_t etype;             /* ET_REL=1 or ET_EXEC=2 */
  uint32_t text_addr;         /* sh_addr of the selected section */
  uint32_t text_type;         /* 0 -> SHT_PROGBITS */
  uint32_t text_flags;        /* 0 -> SHF_EXECINSTR */
  const uint8_t *text;
  size_t text_size;
  const struct eb_sym *syms;
  int nsym;
};

enum { EB_ET_REL = 1, EB_ET_EXEC = 2, EB_SHT_PROGBITS = 1, EB_SHT_SYMTAB = 2,
       EB_SHT_STRTAB = 3 };
#define EB_SHF_EXECINSTR 0x4u
#define EB_STB_LOCAL 0
#define EB_STB_GLOBAL 1
#define EB_STT_NOTYPE 0
#define EB_STT_FUNC 2

/* Build a 5 section ELF: [0] null, [1] selected text, [2] other progbits,
   [3] strtab, [4] symtab. */
static void eb_build(struct eb *b, const struct eb_cfg *cfg)
{
  memset(b, 0, sizeof *b);
  b->ok = 1;

  /* ELF header */
  b->len = 52;
  memset(b->img, 0, 52);
  b->img[0] = 0x7f; b->img[1] = 'E'; b->img[2] = 'L'; b->img[3] = 'F';
  b->img[4] = 1;    /* ELFCLASS32 */
  b->img[5] = 1;    /* ELFDATA2LSB */
  b->img[6] = 1;    /* EV_CURRENT */
  eb_w16(b->img + 16, cfg->etype);
  eb_w16(b->img + 18, 40); /* EM_ARM */
  eb_w32(b->img + 20, 1);  /* e_version */

  /* section contents */
  uint32_t text_off = eb_bytes(b, cfg->text, cfg->text_size, 4);

  static const uint8_t other_bytes[4] = { 0x00, 0xbf, 0x00, 0xbf };
  uint32_t other_off = eb_bytes(b, other_bytes, sizeof other_bytes, 4);

  /* symbol string table: leading NUL then each name */
  uint32_t str_off = (uint32_t)b->len;
  {
    uint8_t nul = 0;
    eb_bytes(b, &nul, 1, 1);
    for (int i = 0; i < cfg->nsym; i++) {
      if (cfg->syms[i].name && cfg->syms[i].name[0])
        eb_bytes(b, cfg->syms[i].name, strlen(cfg->syms[i].name) + 1, 1);
    }
  }
  uint32_t str_size = (uint32_t)b->len - str_off;

  /* symbol table */
  uint32_t sym_off = eb_align(b, 4);
  for (int i = 0; i < cfg->nsym; i++) {
    const struct eb_sym *sy = &cfg->syms[i];
    uint32_t name_off = 0;
    if (sy->name && sy->name[0]) {
      /* recompute this name's offset (sequential layout above) */
      name_off = 1;
      for (int j = 0; j < i; j++)
        if (cfg->syms[j].name && cfg->syms[j].name[0])
          name_off += (uint32_t)strlen(cfg->syms[j].name) + 1;
    }
    uint8_t ent[16];
    memset(ent, 0, sizeof ent);
    eb_w32(ent + 0, name_off);
    eb_w32(ent + 4, sy->value);
    eb_w32(ent + 8, 0);                       /* st_size */
    ent[12] = (uint8_t)((sy->bind << 4) | sy->type);
    ent[13] = 0;                              /* st_other */
    eb_w16(ent + 14, sy->shndx);
    eb_bytes(b, ent, sizeof ent, 1);
  }
  uint32_t sym_size = (uint32_t)b->len - sym_off;

  /* section header table */
  uint32_t shoff = eb_align(b, 4);
  b->shoff = shoff;
  static const int SHNUM = 5;
  if (b->len + (size_t)SHNUM * 40 > sizeof b->img) {
    b->ok = 0;
    return;
  }
  memset(b->img + shoff, 0, (size_t)SHNUM * 40);

  struct eb_sh sh[5];
  memset(sh, 0, sizeof sh);
  sh[1].type = cfg->text_type ? cfg->text_type : EB_SHT_PROGBITS;
  sh[1].flags = cfg->text_flags ? cfg->text_flags : EB_SHF_EXECINSTR;
  sh[1].addr = cfg->text_addr;
  sh[1].offset = text_off;
  sh[1].size = (uint32_t)cfg->text_size;
  sh[1].addralign = 4;

  sh[2].type = EB_SHT_PROGBITS;
  sh[2].flags = 0x2; /* SHF_ALLOC */
  sh[2].addr = cfg->text_addr; /* deliberately same address as text */
  sh[2].offset = other_off;
  sh[2].size = sizeof other_bytes;
  sh[2].addralign = 2;

  sh[3].type = EB_SHT_STRTAB;
  sh[3].offset = str_off;
  sh[3].size = str_size;
  sh[3].addralign = 1;

  sh[4].type = EB_SHT_SYMTAB;
  sh[4].offset = sym_off;
  sh[4].size = sym_size;
  sh[4].link = 3;
  sh[4].addralign = 4;
  sh[4].entsize = 16;

  for (int i = 0; i < SHNUM; i++)
    eb_put_sh(b->img + shoff + (size_t)i * 40, &sh[i]);
  b->text_sh_off = shoff + 40;
  b->len = shoff + (size_t)SHNUM * 40;

  eb_w32(b->img + 32, shoff);    /* e_shoff */
  eb_w16(b->img + 46, 40);       /* e_shentsize */
  eb_w16(b->img + 48, SHNUM);    /* e_shnum */
  eb_w16(b->img + 50, 0);        /* e_shstrndx */
}

/* Canonical mixed section: Thumb nop, Thumb bx lr, ARM mov r0,r0, two bytes
   of data. Coordinates are base+offset. */
static const uint8_t MIXED_TEXT[10] = {
  0x00, 0xbf,             /* nop */
  0x70, 0x47,             /* bx lr */
  0x00, 0x00, 0xa0, 0xe1, /* mov r0, r0 */
  0xab, 0xcd
};

static void mixed_symbols(struct eb_sym *s, int exec, uint32_t base)
{
  uint32_t c0 = exec ? base : 0;
  s[0] = (struct eb_sym){ "$t",    c0 + 0, EB_STB_LOCAL,  EB_STT_NOTYPE, 1 };
  s[1] = (struct eb_sym){ "$a.0",  c0 + 4, EB_STB_LOCAL,  EB_STT_NOTYPE, 1 };
  s[2] = (struct eb_sym){ "$d",    c0 + 8, EB_STB_LOCAL,  EB_STT_NOTYPE, 1 };
  s[3] = (struct eb_sym){ "thumbfn", c0 + 1, EB_STB_GLOBAL, EB_STT_FUNC, 1 };
  s[4] = (struct eb_sym){ "armfn",   c0 + 4, EB_STB_GLOBAL, EB_STT_FUNC, 1 };
  /* decoys living in section 2, with the same values */
  s[5] = (struct eb_sym){ "$t",      0,        EB_STB_LOCAL,  EB_STT_NOTYPE, 2 };
  s[6] = (struct eb_sym){ "other_fn", c0 + 0,  EB_STB_GLOBAL, EB_STT_FUNC, 2 };
}

/* ------------------------------------------------------------------ */
/* elf_scope group                                                    */
/* ------------------------------------------------------------------ */

static int case_elf_scope_rel(void)
{
  struct eb_sym syms[7];
  mixed_symbols(syms, 0, 0x1000);
  struct eb b;
  struct eb_cfg cfg = { EB_ET_REL, 0x1000, 0, 0, MIXED_TEXT, sizeof MIXED_TEXT,
                        syms, 7 };
  eb_build(&b, &cfg);
  TASK_CHECK(b.ok);

  ARMSTATE s;
  disasm_init(&s, 0);
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  TASK_CHECK(c.n == 5);
  TASK_CHECK(match_line(&c, 0, 0x1000, 2, "nop"));
  TASK_CHECK(match_line(&c, 1, 0x1002, 2, "bx      lr"));
  TASK_CHECK(match_line(&c, 2, 0x1004, 4, "mov     r0, r0"));
  TASK_CHECK(match_line(&c, 3, 0x1008, 1, ".byte 0xab"));
  TASK_CHECK(match_line(&c, 4, 0x1009, 1, ".byte 0xcd"));

  /* only the two STT_FUNC names of section 1 are ordinary symbols */
  TASK_CHECK(s.symbolcount == 2);
  int ti = symbol_index(&s, "thumbfn");
  int ai = symbol_index(&s, "armfn");
  TASK_CHECK(ti >= 0 && ai >= 0);
  TASK_CHECK(s.symbols[ti].address == 0x1000);
  TASK_CHECK(s.symbols[ti].mode == ARMMODE_THUMB);
  TASK_CHECK(s.symbols[ai].address == 0x1004);
  TASK_CHECK(s.symbols[ai].mode == ARMMODE_ARM);

  /* mapping names stay out of the ordinary symbol table, including the
     same-address $a marker next to armfn */
  TASK_CHECK(symbol_index(&s, "$a.0") < 0);
  TASK_CHECK(symbol_index(&s, "$t") < 0);
  TASK_CHECK(symbol_index(&s, "other_fn") < 0);

  /* three section-1 mappings, sorted */
  TASK_CHECK(s.mappingcount == 3);
  TASK_CHECK(s.mappings[0].address == 0x1000 && s.mappings[0].mode == ARMMODE_THUMB);
  TASK_CHECK(s.mappings[1].address == 0x1004 && s.mappings[1].mode == ARMMODE_ARM);
  TASK_CHECK(s.mappings[2].address == 0x1008 && s.mappings[2].mode == ARMMODE_DATA);

  disasm_cleanup(&s);
  return 0;
}

static int case_elf_scope_exec(void)
{
  struct eb_sym syms[7];
  mixed_symbols(syms, 1, 0x2000);
  struct eb b;
  struct eb_cfg cfg = { EB_ET_EXEC, 0x2000, 0, 0, MIXED_TEXT, sizeof MIXED_TEXT,
                        syms, 7 };
  eb_build(&b, &cfg);
  TASK_CHECK(b.ok);

  ARMSTATE s;
  disasm_init(&s, 0);
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  TASK_CHECK(c.n == 5);
  TASK_CHECK(match_line(&c, 0, 0x2000, 2, "nop"));
  TASK_CHECK(match_line(&c, 2, 0x2004, 4, "mov     r0, r0"));
  TASK_CHECK(match_line(&c, 3, 0x2008, 1, ".byte 0xab"));

  int ti = symbol_index(&s, "thumbfn");
  TASK_CHECK(ti >= 0);
  TASK_CHECK(s.symbols[ti].address == 0x2000);
  TASK_CHECK(s.symbols[ti].mode == ARMMODE_THUMB);
  TASK_CHECK(symbol_index(&s, "other_fn") < 0);
  TASK_CHECK(s.mappingcount == 3);

  disasm_cleanup(&s);
  return 0;
}

/* Ordinary STT_FUNC names still feed the original comment lookup. */
static int case_elf_scope_comment(void)
{
  /* ldr r0,[pc,#0]; nop; .word leaf; bx lr */
  static const uint8_t text[10] = {
    0x00, 0x48,
    0x00, 0xbf,
    0x08, 0x10, 0x00, 0x00,  /* 0x00001008 */
    0x70, 0x47
  };
  struct eb_sym syms[2] = {
    { "$t",      0x1000, EB_STB_LOCAL,  EB_STT_NOTYPE, 1 },
    { "leaf_fn", 0x1009, EB_STB_GLOBAL, EB_STT_FUNC,    1 },
  };
  struct eb b;
  struct eb_cfg cfg = { EB_ET_EXEC, 0x1000, 0, 0, text, sizeof text, syms, 2 };
  eb_build(&b, &cfg);
  TASK_CHECK(b.ok);

  ARMSTATE s;
  disasm_init(&s, DISASM_COMMENT);
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));
  TASK_CHECK(c.n >= 1);
  TASK_CHECK(has_substr(&c, 0, "leaf_fn"));
  disasm_cleanup(&s);
  return 0;
}

/* ------------------------------------------------------------------ */
/* mixed_ranges group                                                 */
/* ------------------------------------------------------------------ */

static int case_ranges_basic(void)
{
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_address(&s, 0x1000);
  TASK_CHECK(disasm_mapping(&s, 0x1000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s, 0x1004, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&s, 0x1008, ARMMODE_DATA));

  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, MIXED_TEXT, sizeof MIXED_TEXT,
                           ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(c.n == 5);
  TASK_CHECK(match_line(&c, 0, 0x1000, 2, "nop"));
  TASK_CHECK(match_line(&c, 1, 0x1002, 2, "bx      lr"));
  TASK_CHECK(match_line(&c, 2, 0x1004, 4, "mov     r0, r0"));
  TASK_CHECK(match_line(&c, 3, 0x1008, 1, ".byte 0xab"));
  TASK_CHECK(match_line(&c, 4, 0x1009, 1, ".byte 0xcd"));
  TASK_CHECK(s.size == 1 && s.address == 0x1009);
  disasm_cleanup(&s);
  return 0;
}

/* A future mapping marker must not change the starting mode early. */
static int case_ranges_future_marker(void)
{
  static const uint8_t code[8] = {
    0x00, 0xbf, 0x00, 0xbf, 0x00, 0x00, 0xa0, 0xe1
  };
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_address(&s, 0x1000);
  TASK_CHECK(disasm_mapping(&s, 0x1004, ARMMODE_ARM)); /* future only */

  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, code, sizeof code, ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(c.n == 3);
  TASK_CHECK(match_line(&c, 0, 0x1000, 2, "nop"));
  TASK_CHECK(match_line(&c, 1, 0x1002, 2, "nop"));
  TASK_CHECK(match_line(&c, 2, 0x1004, 4, "mov     r0, r0"));
  disasm_cleanup(&s);
  return 0;
}

static int case_ranges_data_hex(void)
{
  static const uint8_t data[7] = {
    0x00, 0x01, 0x0a, 0x0f, 0x10, 0xab, 0xff
  };
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_address(&s, 0x5000);
  TASK_CHECK(disasm_mapping(&s, 0x5000, ARMMODE_DATA));

  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, data, sizeof data, ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(c.n == 7);
  TASK_CHECK(match_line(&c, 0, 0x5000, 1, ".byte 0x00"));
  TASK_CHECK(match_line(&c, 1, 0x5001, 1, ".byte 0x01"));
  TASK_CHECK(match_line(&c, 2, 0x5002, 1, ".byte 0x0a"));
  TASK_CHECK(match_line(&c, 3, 0x5003, 1, ".byte 0x0f"));
  TASK_CHECK(match_line(&c, 4, 0x5004, 1, ".byte 0x10"));
  TASK_CHECK(match_line(&c, 5, 0x5005, 1, ".byte 0xab"));
  TASK_CHECK(match_line(&c, 6, 0x5006, 1, ".byte 0xff"));
  disasm_cleanup(&s);
  return 0;
}

static int case_ranges_boundaries(void)
{
  /* 3 bytes of a Thumb range holding a 32-bit prefix, then DATA. */
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_address(&s, 0x2000);
  static const uint8_t b1[4] = { 0xff, 0xf7, 0xab, 0x00 };
  TASK_CHECK(disasm_mapping(&s, 0x2000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s, 0x2003, ARMMODE_DATA));
  struct cap c1 = { 0 };
  c1.st = &s;
  TASK_CHECK(disasm_buffer(&s, b1, sizeof b1, ARMMODE_THUMB, cap_cb, &c1));
  TASK_CHECK(c1.n == 4);
  TASK_CHECK(match_line(&c1, 0, 0x2000, 1, ".byte 0xff"));
  TASK_CHECK(match_line(&c1, 1, 0x2001, 1, ".byte 0xf7"));
  TASK_CHECK(match_line(&c1, 2, 0x2002, 1, ".byte 0xab"));
  TASK_CHECK(match_line(&c1, 3, 0x2003, 1, ".byte 0x00"));
  disasm_cleanup(&s);

  /* Full ARM instruction plus a 2 byte ARM tail before the DATA marker. */
  ARMSTATE s2;
  disasm_init(&s2, 0);
  disasm_address(&s2, 0x3000);
  static const uint8_t b2[8] = {
    0x00, 0x00, 0xa0, 0xe1, 0x11, 0x22, 0xcc, 0xdd
  };
  TASK_CHECK(disasm_mapping(&s2, 0x3000, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&s2, 0x3006, ARMMODE_DATA));
  struct cap c2 = { 0 };
  c2.st = &s2;
  TASK_CHECK(disasm_buffer(&s2, b2, sizeof b2, ARMMODE_ARM, cap_cb, &c2));
  TASK_CHECK(c2.n == 5);
  TASK_CHECK(match_line(&c2, 0, 0x3000, 4, "mov     r0, r0"));
  TASK_CHECK(match_line(&c2, 1, 0x3004, 1, ".byte 0x11"));
  TASK_CHECK(match_line(&c2, 2, 0x3005, 1, ".byte 0x22"));
  TASK_CHECK(match_line(&c2, 3, 0x3006, 1, ".byte 0xcc"));
  TASK_CHECK(match_line(&c2, 4, 0x3007, 1, ".byte 0xdd"));
  disasm_cleanup(&s2);
  return 0;
}

static int case_ranges_it_abort(void)
{
  /* IT state must not survive a DATA gap. */
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_address(&s, 0x1000);
  static const uint8_t b[6] = {
    0x1c, 0xbf,             /* itt ne */
    0xee, 0xff,             /* data gap */
    0x13, 0x40              /* ands r3, r2 (must have no "ne") */
  };
  TASK_CHECK(disasm_mapping(&s, 0x1000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s, 0x1002, ARMMODE_DATA));
  TASK_CHECK(disasm_mapping(&s, 0x1004, ARMMODE_THUMB));
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, b, sizeof b, ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(c.n == 4);
  TASK_CHECK(match_line(&c, 0, 0x1000, 2, "itt     ne"));
  TASK_CHECK(match_line(&c, 3, 0x1004, 2, "ands    r3, r2"));
  TASK_CHECK(s.it_mask == 0);
  disasm_cleanup(&s);

  /* A false callback aborts immediately and reports the current byte. */
  ARMSTATE s2;
  disasm_init(&s2, 0);
  disasm_address(&s2, 0x4000);
  static const uint8_t db[4] = { 1, 2, 3, 4 };
  TASK_CHECK(disasm_mapping(&s2, 0x4000, ARMMODE_DATA));
  struct cap c2 = { 0 };
  c2.st = &s2;
  c2.fail_at = 2;
  TASK_CHECK(!disasm_buffer(&s2, db, sizeof db, ARMMODE_THUMB, cap_cb, &c2));
  TASK_CHECK(c2.n == 2);
  TASK_CHECK(s2.size == 1);
  TASK_CHECK(s2.address == 0x4001);
  TASK_CHECK(strcmp(s2.text, ".byte 0x02") == 0);
  disasm_cleanup(&s2);
  return 0;
}

static int case_ranges_prefixes_literal(void)
{
  /* address prefix stays aligned on byte lines */
  ARMSTATE s;
  disasm_init(&s, DISASM_ADDRESS);
  disasm_address(&s, 0x1000);
  TASK_CHECK(disasm_mapping(&s, 0x1000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s, 0x1004, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&s, 0x1008, ARMMODE_DATA));
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, MIXED_TEXT, sizeof MIXED_TEXT,
                           ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(match_line(&c, 0, 0x1000, 2, "00001000    nop"));
  TASK_CHECK(match_line(&c, 3, 0x1008, 1, "00001008    .byte 0xab"));
  TASK_CHECK(s.add_addr == 1 && s.add_bin == 0);
  disasm_cleanup(&s);

  /* address + binary prefix: the byte field is two hex digits padded to the
     same 12 column binary slot. */
  ARMSTATE s2;
  disasm_init(&s2, DISASM_ADDRESS | DISASM_INSTR);
  disasm_address(&s2, 0x1000);
  TASK_CHECK(disasm_mapping(&s2, 0x1000, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s2, 0x1004, ARMMODE_ARM));
  TASK_CHECK(disasm_mapping(&s2, 0x1008, ARMMODE_DATA));
  struct cap c2 = { 0 };
  c2.st = &s2;
  TASK_CHECK(disasm_buffer(&s2, MIXED_TEXT, sizeof MIXED_TEXT,
                           ARMMODE_THUMB, cap_cb, &c2));
  char expected[128];
  strcpy(expected, "00001008    ab");
  while (strlen(expected) < 24)
    strcat(expected, " ");
  strcat(expected, ".byte 0xab");
  TASK_CHECK(c2.n == 5);
  TASK_CHECK(strcmp(c2.lines[3].text, expected) == 0);
  TASK_CHECK(c2.lines[3].size == 1);
  disasm_cleanup(&s2);

  /* An explicit ARM code mapping overrides a speculative POOL_LITERAL mark. */
  ARMSTATE s3;
  disasm_init(&s3, 0);
  disasm_address(&s3, 0x5000);
  static const uint8_t w[4] = { 0x00, 0x00, 0xa0, 0xe1 };
  TASK_CHECK(disasm_mapping(&s3, 0x5000, ARMMODE_ARM));
  ARMPOOL *pool = (ARMPOOL *)calloc(1, sizeof(ARMPOOL));
  TASK_CHECK(pool != NULL);
  pool->address = 0x5000;
  pool->type = 1; /* POOL_LITERAL in armdisasm.c */
  s3.codepool = pool;
  s3.poolcount = 1;
  s3.poolsize = 1;
  struct cap c3 = { 0 };
  c3.st = &s3;
  TASK_CHECK(disasm_buffer(&s3, w, sizeof w, ARMMODE_ARM, cap_cb, &c3));
  TASK_CHECK(c3.n == 1);
  TASK_CHECK(match_line(&c3, 0, 0x5000, 4, "mov     r0, r0"));
  disasm_cleanup(&s3); /* frees the installed codepool */
  return 0;
}

/* ------------------------------------------------------------------ */
/* state group                                                        */
/* ------------------------------------------------------------------ */

static int case_state_mapping_list(void)
{
  ARMSTATE s;
  disasm_init(&s, 0);
  TASK_CHECK(disasm_mapping(&s, 0x30, ARMMODE_DATA));
  TASK_CHECK(disasm_mapping(&s, 0x10, ARMMODE_THUMB));
  TASK_CHECK(disasm_mapping(&s, 0x20, ARMMODE_ARM));
  TASK_CHECK(s.mappingcount == 3);
  TASK_CHECK(s.mappings[0].address == 0x10 && s.mappings[0].mode == ARMMODE_THUMB);
  TASK_CHECK(s.mappings[1].address == 0x20 && s.mappings[1].mode == ARMMODE_ARM);
  TASK_CHECK(s.mappings[2].address == 0x30 && s.mappings[2].mode == ARMMODE_DATA);

  /* same address + same mode merges (no new entry) */
  TASK_CHECK(disasm_mapping(&s, 0x10, ARMMODE_THUMB));
  TASK_CHECK(s.mappingcount == 3);

  /* same address + different mode is rejected and leaves state intact */
  TASK_CHECK(!disasm_mapping(&s, 0x20, ARMMODE_DATA));
  TASK_CHECK(s.mappingcount == 3);
  TASK_CHECK(s.mappings[1].mode == ARMMODE_ARM);

  /* invalid modes are rejected */
  TASK_CHECK(!disasm_mapping(&s, 0x40, ARMMODE_UNKNOWN));
  TASK_CHECK(s.mappingcount == 3);

  disasm_clear_mappings(&s);
  TASK_CHECK(s.mappingcount == 0 && s.mappings == NULL);

  disasm_cleanup(&s);
  TASK_CHECK(s.mappingcount == 0 && s.mappings == NULL);
  TASK_CHECK(s.symbolcount == 0 && s.symbols == NULL);
  return 0;
}

static int case_state_elf_rebuild(void)
{
  /* Section A at 0x4000: canonical mixed content. */
  struct eb_sym symsA[7];
  mixed_symbols(symsA, 0, 0x4000);
  struct eb a;
  struct eb_cfg cfgA = { EB_ET_REL, 0x4000, 0, 0, MIXED_TEXT, sizeof MIXED_TEXT,
                         symsA, 7 };
  eb_build(&a, &cfgA);
  TASK_CHECK(a.ok);

  /* Section B at the SAME address 0x4000: four data bytes, one function. */
  static const uint8_t textB[4] = { 0xde, 0xad, 0xbe, 0xef };
  struct eb_sym symsB[2] = {
    { "$d",    0, EB_STB_LOCAL,  EB_STT_NOTYPE, 1 },
    { "bfunc", 0, EB_STB_GLOBAL, EB_STT_FUNC,    1 },
  };
  struct eb b;
  struct eb_cfg cfgB = { EB_ET_REL, 0x4000, 0, 0, textB, sizeof textB,
                         symsB, 2 };
  eb_build(&b, &cfgB);
  TASK_CHECK(b.ok);

  ARMSTATE s;
  disasm_init(&s, DISASM_ADDRESS);
  struct cap ca = { 0 }, cbcap = { 0 };
  ca.st = &s;
  TASK_CHECK(disasm_elf_section(&s, a.img, a.len, 1, cap_cb, &ca));
  TASK_CHECK(ca.n == 5);
  TASK_CHECK(strncmp(ca.lines[0].text, "00004000    ", 12) == 0);
  TASK_CHECK(s.add_addr == 1);

  cbcap.st = &s;
  TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &cbcap));
  TASK_CHECK(cbcap.n == 4);
  TASK_CHECK(match_line(&cbcap, 0, 0x4000, 1, "00004000    .byte 0xde"));
  TASK_CHECK(match_line(&cbcap, 3, 0x4003, 1, "00004003    .byte 0xef"));

  /* display option preserved, section A tables fully replaced by B's */
  TASK_CHECK(s.add_addr == 1);
  TASK_CHECK(s.symbolcount == 1);
  TASK_CHECK(symbol_index(&s, "bfunc") >= 0);
  TASK_CHECK(symbol_index(&s, "armfn") < 0);
  TASK_CHECK(symbol_index(&s, "thumbfn") < 0);
  TASK_CHECK(s.mappingcount == 1);
  TASK_CHECK(s.mappings[0].address == 0x4000);
  TASK_CHECK(s.mappings[0].mode == ARMMODE_DATA);

  disasm_cleanup(&s);
  return 0;
}

static int case_state_direct_buffer(void)
{
  struct eb_sym syms[7];
  mixed_symbols(syms, 0, 0x4000);
  struct eb b;
  struct eb_cfg cfg = { EB_ET_REL, 0x4000, 0, 0, MIXED_TEXT, sizeof MIXED_TEXT,
                        syms, 7 };
  eb_build(&b, &cfg);
  TASK_CHECK(b.ok);

  ARMSTATE s;
  disasm_init(&s, 0);
  TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb,
                                 &(struct cap){ .st = &s }));

  /* Reuse the installed mappings through the plain buffer entry. The mapping
     at 0x4000 is Thumb and must override the deliberately wrong passed mode. */
  static const uint8_t code[8] = {
    0x00, 0xbf, 0x00, 0xbf, 0x00, 0x00, 0xa0, 0xe1
  };
  disasm_address(&s, 0x4000);
  struct cap c = { 0 };
  c.st = &s;
  TASK_CHECK(disasm_buffer(&s, code, sizeof code, ARMMODE_ARM, cap_cb, &c));
  TASK_CHECK(c.n == 3);
  TASK_CHECK(match_line(&c, 0, 0x4000, 2, "nop"));
  TASK_CHECK(match_line(&c, 1, 0x4002, 2, "nop"));
  TASK_CHECK(match_line(&c, 2, 0x4004, 4, "mov     r0, r0"));
  disasm_cleanup(&s);
  return 0;
}

static int case_state_unsupported(void)
{
  ARMSTATE s;
  disasm_init(&s, 0);
  struct cap c = { 0 };
  c.st = &s;

  struct eb_sym syms[1] = {
    { "$t", 0, EB_STB_LOCAL, EB_STT_NOTYPE, 1 }
  };
  struct eb b;
  struct eb_cfg cfg = { EB_ET_REL, 0x1000, 0, 0, MIXED_TEXT, 4, syms, 1 };

  /* truncated below Ehdr size */
  eb_build(&b, &cfg);
  TASK_CHECK(!disasm_elf_section(&s, b.img, 16, 1, cap_cb, &c));

  /* garbage / bad magic */
  uint8_t junk[64];
  memset(junk, 0x55, sizeof junk);
  TASK_CHECK(!disasm_elf_section(&s, junk, sizeof junk, 1, cap_cb, &c));

  /* ELF64 */
  eb_build(&b, &cfg);
  b.img[4] = 2;
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* big endian */
  eb_build(&b, &cfg);
  b.img[5] = 2;
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* wrong machine */
  eb_build(&b, &cfg);
  eb_w16(b.img + 18, 8); /* EM_MIPS */
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* unsupported type ET_DYN */
  eb_build(&b, &cfg);
  eb_w16(b.img + 16, 3);
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* bad section indices */
  eb_build(&b, &cfg);
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 99, cap_cb, &c));
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 0, cap_cb, &c));

  /* selected section is not executable */
  eb_build(&b, &cfg);
  eb_w32(b.img + b.text_sh_off + 8 /*sh_flags*/, 0);
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* selected section is not SHT_PROGBITS */
  eb_build(&b, &cfg);
  eb_w32(b.img + b.text_sh_off + 4 /*sh_type*/, EB_SHT_STRTAB);
  TASK_CHECK(!disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));

  /* section header table entirely outside the image */
  eb_build(&b, &cfg);
  TASK_CHECK(!disasm_elf_section(&s, b.img, (size_t)b.shoff, 1, cap_cb, &c));

  disasm_cleanup(&s);
  return 0;
}

static int case_state_cleanup_reuse(void)
{
  struct eb_sym syms[1] = {
    { "$t", 0, EB_STB_LOCAL, EB_STT_NOTYPE, 1 }
  };
  struct eb b;
  struct eb_cfg cfg = { EB_ET_REL, 0x1000, 0, 0, MIXED_TEXT, 4, syms, 1 };
  eb_build(&b, &cfg);
  TASK_CHECK(b.ok);

  for (int round = 0; round < 3; round++) {
    ARMSTATE s;
    disasm_init(&s, 0);
    struct cap c = { 0 };
    c.st = &s;
    TASK_CHECK(disasm_elf_section(&s, b.img, b.len, 1, cap_cb, &c));
    TASK_CHECK(c.n >= 1);
    disasm_cleanup(&s);
    TASK_CHECK(s.mappings == NULL && s.symbols == NULL && s.codepool == NULL);
  }

  /* after cleanup + init the state is usable again */
  ARMSTATE s;
  disasm_init(&s, 0);
  disasm_mapping(&s, 0, ARMMODE_DATA);
  static const uint8_t one[1] = { 0x7f };
  struct cap c = { 0 };
  c.st = &s;
  disasm_address(&s, 0);
  TASK_CHECK(disasm_buffer(&s, one, 1, ARMMODE_THUMB, cap_cb, &c));
  TASK_CHECK(c.n == 1);
  TASK_CHECK(match_line(&c, 0, 0, 1, ".byte 0x7f"));
  disasm_cleanup(&s);
  return 0;
}

/* ------------------------------------------------------------------ */
/* case registry (consumed by task_support/driver.c)                  */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "elf_scope", "rel_symbols",    case_elf_scope_rel },
  { "elf_scope", "exec_symbols",   case_elf_scope_exec },
  { "elf_scope", "comment_lookup", case_elf_scope_comment },

  { "mixed_ranges", "basic",          case_ranges_basic },
  { "mixed_ranges", "future_marker",  case_ranges_future_marker },
  { "mixed_ranges", "data_hex",       case_ranges_data_hex },
  { "mixed_ranges", "boundaries",     case_ranges_boundaries },
  { "mixed_ranges", "it_abort",       case_ranges_it_abort },
  { "mixed_ranges", "prefixes_literal", case_ranges_prefixes_literal },

  { "state", "mapping_list",  case_state_mapping_list },
  { "state", "elf_rebuild",   case_state_elf_rebuild },
  { "state", "direct_buffer", case_state_direct_buffer },
  { "state", "unsupported",   case_state_unsupported },
  { "state", "cleanup_reuse", case_state_cleanup_reuse },
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
