/* ARM ELF section disassembler
 *
 * Reads the symbols and mapping symbols of one executable section from a
 * little-endian ARM ELF32 object or executable, and disassembles that section
 * through disasm_buffer().
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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "armdisasm.h"
#include "armelf.h"

#define ELFCLASS32       1
#define ELFDATA2LSB      1
#define EV_CURRENT       1

#define ET_REL           1
#define ET_EXEC          2

#define EM_ARM           40

#define SHT_PROGBITS     1
#define SHT_SYMTAB       2

#define SHF_EXECINSTR    0x00000004UL

#define STB_LOCAL        0
#define STT_NOTYPE       0
#define STT_FUNC         2

#define SHN_UNDEF        0

#define ELF32_EHDR_SIZE  52
#define ELF32_SHDR_SIZE  40
#define ELF32_SYM_SIZE   16

#define ELF32_ST_BIND(info)  ((info) >> 4)
#define ELF32_ST_TYPE(info)  ((info) & 0x0f)

typedef struct {
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_shoff;
  uint16_t e_shentsize;
  uint16_t e_shnum;
} ELF32_HEADER;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint32_t sh_flags;
  uint32_t sh_addr;
  uint32_t sh_offset;
  uint32_t sh_size;
  uint32_t sh_link;
  uint32_t sh_entsize;
} ELF32_SHDR;

typedef struct {
  uint32_t st_name;
  uint32_t st_value;
  uint8_t st_info;
  uint16_t st_shndx;
} ELF32_SYM;

static bool range_ok(size_t offset, size_t length, size_t size)
{
  return offset <= size && length <= size - offset;
}

static uint16_t read_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_mapping_name(const char *name, int *mode)
{
  assert(name);
  assert(mode);
  if (name[0] != '$' || name[1] == '\0' || (name[2] != '\0' && name[2] != '.'))
    return false;
  switch (name[1]) {
  case 'a':
    *mode = ARMMODE_ARM;
    return true;
  case 't':
    *mode = ARMMODE_THUMB;
    return true;
  case 'd':
    *mode = ARMMODE_DATA;
    return true;
  default:
    return false;
  }
}

static bool read_ehdr(const uint8_t *image, size_t size, ELF32_HEADER *hdr)
{
  assert(image);
  assert(hdr);
  if (size < ELF32_EHDR_SIZE)
    return false;
  if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F')
    return false;
  if (image[4] != ELFCLASS32 || image[5] != ELFDATA2LSB || image[6] != EV_CURRENT)
    return false;
  hdr->e_type = read_u16(image + 16);
  hdr->e_machine = read_u16(image + 18);
  hdr->e_version = read_u32(image + 20);
  hdr->e_shoff = read_u32(image + 32);
  hdr->e_shentsize = read_u16(image + 46);
  hdr->e_shnum = read_u16(image + 48);
  if (hdr->e_version != EV_CURRENT || hdr->e_machine != EM_ARM)
    return false;
  if (hdr->e_type != ET_REL && hdr->e_type != ET_EXEC)
    return false;
  if (hdr->e_shnum == 0 || hdr->e_shentsize < ELF32_SHDR_SIZE || hdr->e_shoff == 0)
    return false;
  size_t table_size = (size_t)hdr->e_shnum * hdr->e_shentsize;
  if (!range_ok(hdr->e_shoff, table_size, size))
    return false;
  return true;
}

static bool read_shdr(const uint8_t *image, size_t size, const ELF32_HEADER *hdr,
                      uint32_t index, ELF32_SHDR *shdr)
{
  assert(image);
  assert(hdr);
  assert(shdr);
  if (index >= hdr->e_shnum)
    return false;
  size_t offset = hdr->e_shoff + (size_t)index * hdr->e_shentsize;
  if (!range_ok(offset, ELF32_SHDR_SIZE, size))
    return false;
  const uint8_t *p = image + offset;
  shdr->sh_name = read_u32(p + 0);
  shdr->sh_type = read_u32(p + 4);
  shdr->sh_flags = read_u32(p + 8);
  shdr->sh_addr = read_u32(p + 12);
  shdr->sh_offset = read_u32(p + 16);
  shdr->sh_size = read_u32(p + 20);
  shdr->sh_link = read_u32(p + 24);
  shdr->sh_entsize = read_u32(p + 36);
  return true;
}

static bool read_sym(const uint8_t *p, ELF32_SYM *sym)
{
  assert(p);
  assert(sym);
  sym->st_name = read_u32(p + 0);
  sym->st_value = read_u32(p + 4);
  sym->st_info = p[12];
  sym->st_shndx = read_u16(p + 14);
  return true;
}

/** clear_symbols() frees the ordinary symbol list without touching the display
 *  options or the literal blocks. It is used to give each section its own set
 *  of names.
 */
static void clear_symbols(ARMSTATE *state)
{
  assert(state);
  if (state->symbols) {
    for (int i = 0; i < state->symbolcount; i++)
      if (state->symbols[i].name)
        free((void *)state->symbols[i].name);
    free(state->symbols);
  }
  state->symbols = NULL;
  state->symbolcount = 0;
  state->symbolsize = 0;
}

static void import_symtab(ARMSTATE *state, const uint8_t *image, size_t size,
                          const ELF32_HEADER *hdr, const ELF32_SHDR *symtab,
                          const ELF32_SHDR *target, uint32_t target_index)
{
  assert(state);
  assert(image);
  assert(hdr);
  assert(symtab);
  assert(target);

  if (symtab->sh_link == 0 || symtab->sh_link >= hdr->e_shnum)
    return;                     /* no usable string table */
  ELF32_SHDR strtab;
  if (!read_shdr(image, size, hdr, symtab->sh_link, &strtab))
    return;
  if (!range_ok(strtab.sh_offset, strtab.sh_size, size))
    return;
  if (!range_ok(symtab->sh_offset, symtab->sh_size, size))
    return;
  size_t entsize = (symtab->sh_entsize >= ELF32_SYM_SIZE) ? symtab->sh_entsize : ELF32_SYM_SIZE;
  size_t count = symtab->sh_size / entsize;
  const uint8_t *strdata = image + strtab.sh_offset;
  size_t strsize = strtab.sh_size;
  const uint8_t *symdata = image + symtab->sh_offset;

  for (size_t i = 0; i < count; i++) {
    const uint8_t *p = symdata + i * entsize;
    ELF32_SYM sym;
    read_sym(p, &sym);
    if (sym.st_shndx != target_index)
      continue;                 /* only symbols of the selected section */
    const char *name = NULL;
    if (sym.st_name != 0 && sym.st_name < strsize) {
      const char *candidate = (const char *)strdata + sym.st_name;
      size_t maxlen = strsize - sym.st_name;
      if (memchr(candidate, '\0', maxlen) != NULL)
        name = candidate;
    }

    /* addresses are expressed in the coordinate space of the section's sh_addr */
    uint32_t address = (hdr->e_type == ET_REL) ? (uint32_t)(target->sh_addr + sym.st_value)
                                               : sym.st_value;

    int bind = ELF32_ST_BIND(sym.st_info);
    int type = ELF32_ST_TYPE(sym.st_info);

    if (bind == STB_LOCAL && type == STT_NOTYPE && name != NULL) {
      int mode;
      if (is_mapping_name(name, &mode)) {
        /* mapping symbols live in their own list; conflicts are ignored */
        disasm_mapping(state, address, mode);
        continue;
      }
    }

    if (type == STT_FUNC) {
      /* the low bit of the value is the ARM/Thumb selector, not an address bit */
      int mode = (sym.st_value & 1) ? ARMMODE_THUMB : ARMMODE_ARM;
      uint32_t func_address = address & ~(uint32_t)1;
      disasm_symbol(state, name, func_address, mode);
    }
  }
}

/** disasm_elf_section() disassembles one executable section of an ARM ELF32
 *  image. See armelf.h for the full interface description.
 */
bool disasm_elf_section(ARMSTATE *state, const uint8_t *image, size_t size,
                        uint32_t section_index, DISASM_CALLBACK callback, void *user)
{
  if (state == NULL || image == NULL || callback == NULL)
    return false;

  ELF32_HEADER hdr;
  if (!read_ehdr(image, size, &hdr))
    return false;

  ELF32_SHDR target;
  if (!read_shdr(image, size, &hdr, section_index, &target))
    return false;
  if (target.sh_type != SHT_PROGBITS || (target.sh_flags & SHF_EXECINSTR) == 0)
    return false;
  if (!range_ok(target.sh_offset, target.sh_size, size))
    return false;

  /* each call rebuilds the symbols, mappings and code pool of this section,
     while the display options of the state are preserved */
  clear_symbols(state);
  disasm_clear_mappings(state);
  disasm_clear_codepool(state);
  state->it_mask = 0;
  state->it_cond = 0;
  state->ldr_addr = ~0;
  state->size = 0;

  for (uint32_t idx = 0; idx < hdr.e_shnum; idx++) {
    ELF32_SHDR shdr;
    if (!read_shdr(image, size, &hdr, idx, &shdr))
      continue;
    if (shdr.sh_type == SHT_SYMTAB)
      import_symtab(state, image, size, &hdr, &shdr, &target, section_index);
  }

  disasm_address(state, target.sh_addr);

  /* the mapping symbol covering the section start, if any, supplies the initial
     mode; otherwise disasm_buffer() uses its own fallback. A data mapping at
     the start is handled by disasm_buffer()'s per-byte path, so only code modes
     are passed on as the initial mode. */
  int mode = ARMMODE_UNKNOWN;
  if (state->mappingcount > 0 && state->mappings[0].address <= target.sh_addr) {
    int start_mode = state->mappings[0].mode;
    if (start_mode == ARMMODE_ARM || start_mode == ARMMODE_THUMB)
      mode = start_mode;
  }

  return disasm_buffer(state, image + target.sh_offset, target.sh_size,
                       mode, callback, user);
}
