/* ARM ELF section disassembler
 *
 * Loads the mapping symbols and ordinary function symbols of one executable
 * section of a little-endian ARM ELF32 file into an ARMSTATE, and disassembles
 * that section with the regular disasm_buffer() entry point.
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
#ifndef _ARMELF_H
#define _ARMELF_H

#include <stddef.h>
#include <stdint.h>
#include "armdisasm.h"

/** disasm_elf_section() disassembles one executable section of an ARM ELF32
 *  image.
 *
 *  Supported input is a little-endian ELF32 file with e_machine == EM_ARM, of
 *  type ET_REL or ET_EXEC. The section indicated by `section_index` must be an
 *  executable SHT_PROGBITS section (SHF_EXECINSTR). Any other input makes the
 *  function return `false`.
 *
 *  For every SHT_SYMTAB section, its sh_link is used to locate the associated
 *  string table. Only symbols whose st_shndx refers to the selected section are
 *  imported, so same-valued symbols from other sections never leak in.
 *
 *  - Local STT_NOTYPE symbols named `$a`, `$t` or `$d` (optionally followed by a
 *    dot and a suffix, e.g. `$a.1` or `$t.0`) are installed as mapping symbols
 *    with disasm_mapping(); they delimit ARM, Thumb and data ranges.
 *  - STT_FUNC symbols are installed as ordinary named symbols for the comment
 *    lookup. The Thumb address bit (bit 0) is cleared first, and the low bit
 *    selects the mode recorded for the symbol. Mapping symbols live in their own
 *    list, so a mapping symbol can never displace a function name at the same
 *    address.
 *
 *  For ET_REL, st_value is a section offset and is converted to
 *  `sh_addr + st_value`; for ET_EXEC, st_value already is the address in the
 *  coordinate space of sh_addr.
 *
 *  Each call rebuilds the symbol list, the mapping list and the code pool for
 *  the selected section, while the display options set by disasm_init() (and
 *  any literal blocks added with disasm_literals()) are preserved. The image is
 *  treated as read-only and is never modified. The section itself is decoded
 *  through the regular disasm_buffer() call, so mapping symbols installed by
 *  this function also apply when disasm_buffer() is called directly afterwards.
 *
 *  \param state          The decoder state (initialized with disasm_init()).
 *  \param image          Pointer to the complete ELF image.
 *  \param size           Size of the image in bytes.
 *  \param section_index  Index of the executable section to disassemble.
 *  \param callback       Callback invoked for each line of output.
 *  \param user           Passed through to the callback.
 *
 *  \return `true` on success, `false` for an unsupported file or invalid
 *          section index.
 *
 *  Example:
 *
 *      ARMSTATE state;
 *      disasm_init(&state, DISASM_ADDRESS | DISASM_COMMENT);
 *      if (disasm_elf_section(&state, image, imagesize, 1, on_line, NULL))
 *        puts("section disassembled");
 *      disasm_cleanup(&state);
 */
bool disasm_elf_section(ARMSTATE *state, const uint8_t *image, size_t size,
                        uint32_t section_index, DISASM_CALLBACK callback, void *user);

#endif /* _ARMELF_H */
