/*
 * Mach-O indirect symbol references - C call example.
 *
 * Build (from the repository root), linking the library implementation and
 * the PE helper translation units the same way task_support/run.sh does:
 *
 *   cc -std=gnu11 -include stdbool.h -I. -Isrc -D_FILE_OFFSET_BITS=64 \
 *      examples/macho_references_example.c \
 *      src/headerParserLib.c src/pe/PEHeader.c src/pe/PEHeaderOffsets.c \
 *      src/utils/fifo/Fifo.c -o /tmp/macho_refs_example
 *
 * Run:
 *   /tmp/macho_refs_example path/to/image [start_offset]
 *
 * The example only reads static on-disk structure.  It never opens the
 * referenced dylibs and never loads or executes the image.
 */

#include <stdio.h>
#include <stdlib.h>

#include "src/headerParserLib.h"

static const char *status_name(uint8_t status)
{
    switch (status) {
        case MACHO_REFERENCE_RESOLVED:   return "resolved";
        case MACHO_REFERENCE_UNRESOLVED: return "unresolved";
        case MACHO_REFERENCE_LOCAL:      return "local";
        case MACHO_REFERENCE_ABS:        return "abs";
        case MACHO_REFERENCE_LOCAL_ABS:  return "local+abs";
        default:                         return "unknown";
    }
}

int main(int argc, char **argv)
{
    size_t start = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mach-o-file> [start_offset]\n", argv[0]);
        return 2;
    }
    if (argc >= 3)
        start = (size_t)strtoull(argv[2], NULL, 0);

    HeaderData *hd = getBasicHeaderParserInfo(argv[1], start, FORCE_NONE);
    if (hd == NULL) {
        fprintf(stderr, "could not read header of \"%s\"\n", argv[1]);
        return 1;
    }

    if (hd->headertype != HEADER_TYPE_MACH_O) {
        fprintf(stderr, "%s is not a Mach-O image\n", argv[1]);
        freeHeaderData(hd);
        return 1;
    }

    if (hd->macho_references_error != MACHO_REFERENCES_OK) {
        /* Basic header fields are still usable; the reference table itself
         * is incomplete and therefore reported empty. */
        printf("reference table error code: %u\n",
               (unsigned)hd->macho_references_error);
    }

    printf("macho_references_size = %zu\n", hd->macho_references_size);
    for (size_t i = 0; i < hd->macho_references_size; ++i) {
        const MachOReferenceData *r = &hd->macho_references[i];

        /* All offsets use bytes; address is a virtual memory address. */
        printf("[%zu] %s,%s segment#%u section#%u slot#%u\n",
               i,
               r->segment_name ? r->segment_name : "",
               r->section_name ? r->section_name : "",
               r->segment_index, r->section_index, r->slot_index);
        printf("     address=0x%llx image_offset=%llu file_offset=%llu\n",
               (unsigned long long)r->address,
               (unsigned long long)r->image_offset,
               (unsigned long long)r->file_offset);
        printf("     indirect[%u]=0x%08x %s\n",
               r->indirect_index, r->indirect_value, status_name(r->status));

        if (r->status == MACHO_REFERENCE_RESOLVED ||
            r->status == MACHO_REFERENCE_UNRESOLVED) {
            printf("     nlist#%u name=\"%s\" type=0x%02x sect=%u "
                   "desc=0x%04x value=0x%llx\n",
                   r->nlist_index,
                   r->symbol_name ? r->symbol_name : "",
                   (unsigned)r->n_type, (unsigned)r->n_sect,
                   (unsigned)r->n_desc,
                   (unsigned long long)r->n_value);
            if (r->library_ordinal >= 0)
                printf("     library ordinal=%d path=\"%s\"\n",
                       r->library_ordinal,
                       r->library_path ? r->library_path : "");
        }
    }

    /* Frees every row and all owned segment/section/symbol/library strings. */
    freeHeaderData(hd);
    return 0;
}
