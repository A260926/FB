//----------------------------------------------------------------------------//
// libMachO file memory map - brief usage example.
//
// Maps a thin 64-bit Mach-O file from disk and parses it with the ordinary
// libMachO entry points.  Nothing is modified or executed.
//
// Build (from the repository root):
//   clang -std=gnu11 -IlibMachO tests/file_map_example.c .task-build/libMachO.a -o file_map_example
//
// Run:
//   ./file_map_example /path/to/some/macho64
//----------------------------------------------------------------------------//

#include "macho.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <thin-64-bit-macho>\n", argv[0]);
        return 2;
    }

    // Read the whole file into memory.
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("fopen");
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    long length = ftell(file);
    if (length <= 0) {
        fclose(file);
        return 1;
    }
    rewind(file);

    void *bytes = malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return 1;
    }
    fclose(file);

    // Build a read-only file image.  No slide for a file on disk.
    mk_memory_map_file_t map;
    mk_error_t err = mk_memory_map_file_init(NULL, bytes, (size_t)length, 0, &map);
    free(bytes); // The map owns a snapshot; the input is no longer needed.
    if (err != MK_ESUCCESS) {
        fprintf(stderr, "mk_memory_map_file_init: %s\n", mk_error_string(err));
        return 1;
    }

    // The header address is ready for mk_macho_init_with_slide.
    mk_vm_address_t address = mk_memory_map_file_get_header_address(&map);

    mk_macho_t image;
    err = mk_macho_init_with_slide(NULL, argv[1], 0, address, &map, &image);
    if (err != MK_ESUCCESS) {
        fprintf(stderr, "mk_macho_init_with_slide: %s\n", mk_error_string(err));
        mk_memory_map_file_free(&map);
        return 1;
    }

    printf("header address: 0x%" MK_VM_PRIxADDR "\n", address);

    struct load_command *lc = NULL;
    while ((lc = mk_macho_next_command_type(&image, lc, LC_SEGMENT_64, NULL)) != NULL) {
        mk_segment_t segment;
        if (mk_segment_init_with_mach_load_command(&image, (void*)lc, &segment) != MK_ESUCCESS)
            continue;

        char name[17] = { 0 };
        mk_segment_copy_name(&segment, name);
        printf("segment %-16s vmaddr=0x%" MK_VM_PRIxADDR " vmsize=0x%" MK_VM_PRIxSIZE "\n",
               name, mk_segment_get_target_range(&segment).location, mk_segment_get_vmsize(&segment));

        mk_segment_free(&segment);
    }

    // Free the image first, then the map.
    mk_macho_free(&image);
    mk_memory_map_file_free(&map);
    return 0;
}
