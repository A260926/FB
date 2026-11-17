/*
 * Minimal example: walk the DBI module registry, list each module's symbol
 * records, and resolve every global procedure reference.
 *
 * Build (after building libpdb):
 *   cc -std=c11 -Iinclude examples/module_procedures.c -Lbuild -lpdb \
 *      -o module_procedures
 */
#include <pdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, unsigned char **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    *data = malloc((size_t)length);
    if (*data == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data);
        fclose(file);
        return -1;
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s pdb-file\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned char *data = NULL;
    size_t size = 0;
    if (read_file(argv[1], &data, &size) != 0) {
        fprintf(stderr, "could not read %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    void *ctx = pdb_create_context(NULL, NULL);
    if (ctx == NULL) {
        free(data);
        return EXIT_FAILURE;
    }

    if (pdb_load(ctx, data, size) < 0) {
        fprintf(stderr, "load failed: %s\n", pdb_strerror(ctx));
        pdb_destroy_context(ctx);
        free(data);
        return EXIT_FAILURE;
    }

    /* The input buffer is no longer needed after a successful load. */
    free(data);

    const struct pdb_module *modules = NULL;
    uint32_t nr_modules = 0;
    if (pdb_get_modules(ctx, &modules, &nr_modules) < 0) {
        fprintf(stderr, "modules failed: %s\n", pdb_strerror(ctx));
        pdb_destroy_context(ctx);
        return EXIT_FAILURE;
    }

    for (uint32_t i = 0; i < nr_modules; i++) {
        printf(
            "module %u: %s (%s) stream=%u\n", modules[i].index, modules[i].name,
            modules[i].object_name, modules[i].stream_index);

        const struct pdb_module_symbol *symbols = NULL;
        uint32_t nr_symbols = 0;
        if (pdb_get_module_symbols(ctx, i, &symbols, &nr_symbols) < 0) {
            fprintf(stderr, "module symbols failed: %s\n", pdb_strerror(ctx));
            pdb_destroy_context(ctx);
            return EXIT_FAILURE;
        }

        for (uint32_t j = 0; j < nr_symbols; j++) {
            printf(
                "    symbol offset=0x%x size=%u kind=0x%x\n", symbols[j].offset,
                symbols[j].size, symbols[j].kind);
        }
    }

    /*
     * Resolve every global procedure reference. pdb_get_symbols returns
     * pointers into the global record stream; subtracting the stream base
     * recovers the byte offsets that pdb_resolve_procedure_ref expects.
     */
    uint32_t nr_records = 0;
    if (pdb_get_nr_symbols(ctx, &nr_records) < 0) {
        fprintf(stderr, "symbols failed: %s\n", pdb_strerror(ctx));
        pdb_destroy_context(ctx);
        return EXIT_FAILURE;
    }

    const SYMTYPE **records = calloc(nr_records > 0 ? nr_records : 1, sizeof(SYMTYPE *));
    if (records == NULL || (nr_records > 0 && pdb_get_symbols(ctx, records) < 0)) {
        free(records);
        pdb_destroy_context(ctx);
        return EXIT_FAILURE;
    }

    for (uint32_t i = 0; i < nr_records; i++) {
        if (records[i]->rectyp != S_PROCREF && records[i]->rectyp != S_LPROCREF) {
            continue;
        }

        /* Find which stream this pointer belongs to. */
        uint32_t offset = 0;
        uint32_t nr_streams = pdb_get_nr_streams(ctx);
        for (uint32_t s = 0; s < nr_streams; s++) {
            uint32_t stream_size = 0;
            const unsigned char *base = pdb_get_stream(ctx, s, &stream_size);
            if (base != NULL && (const unsigned char *)records[i] >= base &&
                (const unsigned char *)records[i] < base + stream_size) {
                offset = (uint32_t)((const unsigned char *)records[i] - base);
                break;
            }
        }

        struct pdb_procedure proc;
        if (pdb_resolve_procedure_ref(ctx, offset, &proc) < 0) {
            fprintf(
                stderr, "reference at offset 0x%x unresolved: %s\n", offset,
                pdb_strerror(ctx));
            free(records);
            pdb_destroy_context(ctx);
            return EXIT_FAILURE;
        }

        printf(
            "ref 0x%x -> module %u symbol 0x%x %s (%s) %u:0x%x\n", offset, proc.module_index,
            proc.symbol_offset, proc.name, proc.is_local ? "local" : "global", proc.section,
            proc.address_offset);
    }

    free(records);
    pdb_destroy_context(ctx);
    return EXIT_SUCCESS;
}
