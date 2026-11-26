/* Minimal ELF32 (little-endian, EM_ARM) section loader for the disassembler.
 * See armelf.h for the documented entry point.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "armelf.h"

/* Minimal ELF32 constants (from the ELF ABI). */
#define ELFMAG0        0x7f
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'
#define ELFCLASS32     1
#define ELFDATA2LSB    1

#define ET_REL         1
#define ET_EXEC        2
#define EM_ARM         40

#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3

#define SHF_EXECINSTR  0x4u

#define STB_LOCAL      0
#define STT_NOTYPE     0
#define STT_FUNC       2

#define ELF32_ST_TYPE(info) ((info) & 0x0f)
#define ELF32_ST_BIND(info) ((info) >> 4)

#define ELF32_EHDR_SIZE 52
#define ELF32_SHDR_SIZE 40
#define ELF32_SYM_SIZE  16

/* Section header field offsets. */
#define SH_NAME        0
#define SH_TYPE        4
#define SH_FLAGS       8
#define SH_ADDR        12
#define SH_OFFSET      16
#define SH_SIZE        20
#define SH_LINK        24
#define SH_ENTSIZE     36

/* Symbol table entry field offsets. */
#define SYM_NAME       0
#define SYM_VALUE      4
#define SYM_INFO       12
#define SYM_SHNDX      14

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Return a checked pointer to "len" bytes at "off", or NULL if out of bounds. */
static const uint8_t *elf_ptr(const uint8_t *image, size_t size,
                              uint32_t off, size_t len)
{
  if ((size_t)off > size || len > size - (size_t)off)
    return NULL;
  return image + off;
}

/* Resolve a NUL terminated symbol name in the string table described by
   [str_off, str_off + str_size). Returns NULL for an invalid offset or an
   unterminated name. The returned pointer points into the read-only image. */
static const char *elf_string(const uint8_t *image, size_t size,
                              uint32_t str_off, uint32_t str_size,
                              uint32_t name_off)
{
  if (name_off >= str_size)
    return NULL;
  uint64_t start64 = (uint64_t)str_off + name_off;
  if (start64 >= size)
    return NULL;
  size_t start = (size_t)start64;
  size_t maxlen = (size_t)(str_size - name_off);
  if (maxlen > size - start)
    maxlen = size - start;
  const void *nul = memchr(image + start, '\0', maxlen);
  if (!nul)
    return NULL;
  return (const char *)(image + start);
}

/* Classify an ARM mapping symbol name. Returns -1 for non-mapping names. */
static int mapping_name_mode(const char *name)
{
  if (!name || name[0] != '$' || name[1] == '\0')
    return -1;
  /* Plain "$x", or "$x.<suffix>" (e.g. "$t.1", "$d.const"). */
  if (name[2] != '\0' && name[2] != '.')
    return -1;
  switch (name[1]) {
  case 'a': return ARMMODE_ARM;
  case 't': return ARMMODE_THUMB;
  case 'd': return ARMMODE_DATA;
  default:  return -1;
  }
}

/* Drop the section-derived tables of a previous ELF section while keeping
   the caller's display options and any disasm_literals() blocks. */
static void reset_section_tables(ARMSTATE *state)
{
  if (state->symbols) {
    for (int i = 0; i < state->symbolcount; i++) {
      if (state->symbols[i].name)
        free((void *)state->symbols[i].name);
    }
    free(state->symbols);
    state->symbols = NULL;
    state->symbolcount = 0;
    state->symbolsize = 0;
  }
  disasm_clear_mappings(state);
  disasm_clear_codepool(state);

  state->address = 0;
  state->size = 0;
  state->arm_mode = 0;
  state->it_mask = 0;
  state->it_cond = 0;
  state->ldr_addr = ~0u;
  state->text[0] = '\0';
}

bool disasm_elf_section(ARMSTATE *state, const uint8_t *image, size_t size,
                        uint32_t section_index, DISASM_CALLBACK callback, void *user)
{
  if (!state || !image || !callback || size < ELF32_EHDR_SIZE)
    return false;

  /* --- ELF header ------------------------------------------------------- */
  const uint8_t *eh = image;
  if (eh[0] != ELFMAG0 || eh[1] != ELFMAG1 || eh[2] != ELFMAG2 || eh[3] != ELFMAG3)
    return false;
  if (eh[4] != ELFCLASS32 || eh[5] != ELFDATA2LSB)
    return false;                 /* only ELF32, little-endian */

  uint16_t elf_type = rd16(eh + 16);
  uint16_t machine = rd16(eh + 18);
  if (machine != EM_ARM)
    return false;
  if (elf_type != ET_REL && elf_type != ET_EXEC)
    return false;

  uint32_t shoff     = rd32(eh + 32);
  uint16_t shentsize = rd16(eh + 46);
  uint32_t shnum     = rd16(eh + 48);
  if (shentsize < ELF32_SHDR_SIZE)
    return false;

  /* Section zero may hold the real count when e_shnum has the escape value. */
  if (shnum == 0) {
    const uint8_t *sh0 = elf_ptr(image, size, shoff, ELF32_SHDR_SIZE);
    if (!sh0)
      return false;
    shnum = rd32(sh0 + SH_SIZE);
  }
  if (shnum == 0 || section_index >= shnum)
    return false;

  /* --- selected section header ----------------------------------------- */
  uint64_t sh_off64 = (uint64_t)shoff + (uint64_t)section_index * shentsize;
  if (sh_off64 > 0xffffffffu)
    return false;
  const uint8_t *sh = elf_ptr(image, size, (uint32_t)sh_off64, ELF32_SHDR_SIZE);
  if (!sh)
    return false;

  uint32_t sh_type   = rd32(sh + SH_TYPE);
  uint32_t sh_flags  = rd32(sh + SH_FLAGS);
  uint32_t sh_addr   = rd32(sh + SH_ADDR);
  uint32_t sh_offset = rd32(sh + SH_OFFSET);
  uint32_t sh_size   = rd32(sh + SH_SIZE);

  if (sh_type != SHT_PROGBITS || !(sh_flags & SHF_EXECINSTR))
    return false;                 /* selected section must be executable code */
  const uint8_t *section = elf_ptr(image, size, sh_offset, sh_size);
  if (!section && sh_size != 0)
    return false;
  /* A zero-sized section may yield a NULL checked pointer; disasm_buffer only
     requires a non-NULL pointer, so fall back to the image itself. */
  const uint8_t *section_bytes = section ? section : image;

  /* The image and selected section are valid: rebuild section state. A bad
     symbol table below only skips its entries, it does not fail the call. */
  reset_section_tables(state);

  /* --- symbol tables ---------------------------------------------------- */
  for (uint32_t si = 0; si < shnum; si++) {
    uint64_t stab_off64 = (uint64_t)shoff + (uint64_t)si * shentsize;
    const uint8_t *stab_hdr = elf_ptr(image, size, (uint32_t)stab_off64, ELF32_SHDR_SIZE);
    if (!stab_hdr)
      continue;
    if (rd32(stab_hdr + SH_TYPE) != SHT_SYMTAB)
      continue;

    uint32_t sym_offset = rd32(stab_hdr + SH_OFFSET);
    uint32_t sym_size   = rd32(stab_hdr + SH_SIZE);
    uint32_t sym_link   = rd32(stab_hdr + SH_LINK);
    uint32_t sym_entsize = rd32(stab_hdr + SH_ENTSIZE);
    if (sym_entsize < ELF32_SYM_SIZE)
      sym_entsize = ELF32_SYM_SIZE;

    if (sym_link >= shnum)
      continue;
    uint64_t link_off64 = (uint64_t)shoff + (uint64_t)sym_link * shentsize;
    const uint8_t *str_hdr = elf_ptr(image, size, (uint32_t)link_off64, ELF32_SHDR_SIZE);
    if (!str_hdr)
      continue;
    uint32_t str_offset = rd32(str_hdr + SH_OFFSET);
    uint32_t str_size   = rd32(str_hdr + SH_SIZE);
    /* Names are resolved through sh_link; the linked section is normally a
       SHT_STRTAB, but only its bounded byte range is required here. */
    if (!elf_ptr(image, size, str_offset, str_size))
      continue;

    size_t count = sym_size / sym_entsize;
    for (size_t k = 0; k < count; k++) {
      uint64_t ent64 = (uint64_t)sym_offset + (uint64_t)k * sym_entsize;
      const uint8_t *sym = elf_ptr(image, size, (uint32_t)ent64, ELF32_SYM_SIZE);
      if (!sym)
        break;

      uint16_t sym_shndx = rd16(sym + SYM_SHNDX);
      if (sym_shndx != section_index)
        continue;                 /* only symbols of the selected section */

      uint32_t name_off = rd32(sym + SYM_NAME);
      uint32_t value    = rd32(sym + SYM_VALUE);
      uint8_t  info     = sym[SYM_INFO];
      int      sym_type = ELF32_ST_TYPE(info);
      int      sym_bind = ELF32_ST_BIND(info);

      const char *name = elf_string(image, size, str_offset, str_size, name_off);

      /* Convert st_value into the sh_addr coordinate of the section. */
      uint32_t address = (elf_type == ET_REL) ? (sh_addr + value) : value;

      if (sym_bind == STB_LOCAL && sym_type == STT_NOTYPE) {
        int mapmode = mapping_name_mode(name);
        if (mapmode >= 0) {
          disasm_mapping(state, address, mapmode);
          continue;             /* mapping names never become ordinary symbols */
        }
      }

      if (sym_type == STT_FUNC && name && name[0] != '\0') {
        int thumb = address & 1u;
        uint32_t target = address & ~(uint32_t)1u;
        disasm_symbol(state, name, target,
                      thumb ? ARMMODE_THUMB : ARMMODE_ARM);
      }
    }
  }

  /* --- disassemble through the ordinary buffer entry point ------------- */
  disasm_address(state, sh_addr);
  return disasm_buffer(state, section_bytes, sh_size, ARMMODE_UNKNOWN, callback, user);
}
