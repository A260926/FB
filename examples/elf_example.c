/* Example: disassemble the first executable section of an ARM ELF32 file.
 *
 * Build (from the repository root):
 *   cc -std=c11 -I. examples/elf_example.c armelf.c armdisasm.c -o elf_example
 *
 * Usage:
 *   ./elf_example firmware.elf [section-index]
 *
 * The example uses only the C standard library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "armdisasm.h"
#include "armelf.h"

static bool print_line(uint32_t address, const char *text, void *user)
{
    (void)user;
    printf("%08x  %s\n", address, text);
    return true; /* return false here to abort disassembly early */
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s firmware.elf [section-index]\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 1;
    }
    long length = ftell(fp);
    if (length < 0) {
        fclose(fp);
        return 1;
    }
    rewind(fp);

    uint8_t *image = malloc((size_t)length);
    if (!image) {
        fclose(fp);
        return 1;
    }
    if (fread(image, 1, (size_t)length, fp) != (size_t)length) {
        fprintf(stderr, "%s: short read\n", argv[1]);
        free(image);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    ARMSTATE state;
    disasm_init(&state, DISASM_ADDRESS | DISASM_COMMENT);

    int section = (argc > 2) ? atoi(argv[2]) : -1;
    if (section < 0 && length >= 52) {
        /* No index given: pick the first executable SHT_PROGBITS section. */
        uint32_t shoff = le32(image + 32);
        uint16_t entsize = le16(image + 46);
        uint16_t shnum = le16(image + 48);
        for (uint16_t i = 1; i < shnum && section < 0; i++) {
            const uint8_t *sh = image + shoff + (size_t)i * entsize;
            if (le32(sh + 4) == 1 /* SHT_PROGBITS */ &&
                (le32(sh + 8) & 0x4u /* SHF_EXECINSTR */))
                section = (int)i;
        }
    }

    bool ok = false;
    if (section >= 0) {
        ok = disasm_elf_section(&state, image, (size_t)length,
                                (uint32_t)section, print_line, NULL);
    }
    if (!ok)
        fprintf(stderr, "unsupported ELF or no executable section (index %d)\n",
                section);

    disasm_cleanup(&state);
    free(image);
    return ok ? 0 : 2;
}
