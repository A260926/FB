/* ELF loader bridge for the ARM disassembler.
 *
 * Parses a little-endian, 32-bit ARM ELF file (ET_REL or ET_EXEC) and feeds a
 * selected executable section to disasm_buffer(), after installing the ELF
 * mapping symbols ($a, $t, $d) and the ordinary STT_FUNC symbol names.
 *
 * Copyright 2022-2024, CompuPhase
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "armdisasm.h"

/** Disassemble one executable section of an in-memory ELF image.
 *
 *  The image is only read; it is never modified or executed. The function
 *  supports little-endian ELF32 files for EM_ARM, of type ET_REL or ET_EXEC.
 *  \p section_index must identify an executable, allocated `SHT_PROGBITS`
 *  section (`SHF_EXECINSTR`).
 *
 *  Every `SHT_SYMTAB` section is inspected and its `sh_link` section is used
 *  as the associated string table. Only symbols whose `st_shndx` equals
 *  \p section_index are imported, so equally-valued symbols from other
 *  sections never leak in.
 *
 *  Local `STT_NOTYPE` mapping symbols `$a`, `$t` and `$d` (and their dot
 *  suffixed variants such as `$t.1`) become disasm_mapping() ranges. Named
 *  `STT_FUNC` symbols become ordinary comment symbols after clearing the
 *  Thumb low address bit. Mapping symbols never replace a same-address
 *  function name, because mappings and ordinary symbols are stored
 *  independently.
 *
 *  For `ET_REL`, `st_value` is a section offset; for `ET_EXEC` it is an
 *  absolute address. Both are converted to the `sh_addr` coordinate of the
 *  selected section before installation.
 *
 *  Each call rebuilds the ordinary symbols, mappings and codepool for the
 *  selected section (while preserving the display option flags), so the same
 *  `state` can be reused for another section, including a different section
 *  at the same `sh_addr`.
 *
 *  The section is ultimately decoded with disasm_buffer(); the callback,
 *  address and text semantics are those of that function. The installed
 *  mappings also stay effective for a later direct disasm_buffer() call.
 *
 *  \return `true` when the ELF file and section are supported and the buffer
 *          was processed; `false` for an unsupported or malformed image, an
 *          invalid/non-executable section index, or an aborted callback.
 *
 *  ## C example
 *  \code
 *  FILE *fp = fopen("firmware.elf", "rb");
 *  fseek(fp, 0, SEEK_END);
 *  long n = ftell(fp);
 *  rewind(fp);
 *  uint8_t *image = malloc((size_t)n);
 *  fread(image, 1, (size_t)n, fp);
 *  fclose(fp);
 *
 *  ARMSTATE state;
 *  disasm_init(&state, DISASM_ADDRESS | DISASM_COMMENT);
 *  // section 0 is always the null header; pick the .text index from shstrtab.
 *  bool ok = disasm_elf_section(&state, image, (size_t)n, text_index,
 *                               my_callback, NULL);
 *  disasm_cleanup(&state);
 *  free(image);
 *  \endcode
 */
bool disasm_elf_section(ARMSTATE *state, const uint8_t *image, size_t size,
                        uint32_t section_index, DISASM_CALLBACK callback, void *user);

#endif /* _ARMELF_H */
