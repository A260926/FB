/*
 * Module registry, module symbol view, and procedure reference resolution.
 *
 * These cases consume the shared counted harness in task_support/check.h and
 * are grouped by the driver into modules, references, and optional_streams.
 * Most cases build a PDB with task_support/image.c; the reference cases then
 * probe malformed identities that image.c does not emit.
 */
#include <pdb.h>

#include "task_support/check.h"
#include "task_support/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PDBPARSE_PATH
#define PDBPARSE_PATH "./.task-build/pdbparse"
#endif

/* Record kinds used to validate the material; the test never re-encodes it. */
#define KIND_PROC32 0x1110u
#define KIND_LPROC32 0x110fu
#define KIND_END 0x0006u

static int load_image(void **ctx, struct task_image *image, int mode)
{
    if (task_make_image(image, mode) != 0) {
        return -1;
    }
    *ctx = pdb_create_context(NULL, NULL);
    if (*ctx == NULL) {
        task_free_image(image);
        return -1;
    }
    if (pdb_load(*ctx, image->data, image->size) < 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* modules group                                                              */
/* ------------------------------------------------------------------------- */

static int case_module_registry(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module *modules = NULL;
    uint32_t count = 0;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
    TASK_CHECK(count == 2);
    TASK_CHECK(modules != NULL);

    /* Physical order is preserved, and identical names are not merged. */
    TASK_CHECK(modules[0].index == 0);
    TASK_CHECK(modules[1].index == 1);
    TASK_CHECK(strcmp(modules[0].name, "same.obj") == 0);
    TASK_CHECK(strcmp(modules[1].name, "same.obj") == 0);
    TASK_CHECK(strcmp(modules[0].object_name, "dirA/same.obj") == 0);
    TASK_CHECK(strcmp(modules[1].object_name, "dirB/same.obj") == 0);
    TASK_CHECK(modules[0].stream_index == 8);
    TASK_CHECK(modules[1].stream_index == 9);

    /*
     * SymByteSize includes the four-byte signature; C13 line info follows it
     * and is not part of the symbol region.
     */
    TASK_CHECK(modules[0].symbol_bytes > 4);
    TASK_CHECK(modules[0].c13_bytes == 12);
    TASK_CHECK(modules[1].c13_bytes == 12);

    /* A second enumerations yields the same borrowed array. */
    {
        const struct pdb_module *again = NULL;
        uint32_t again_count = 0;
        TASK_CHECK(pdb_get_modules(ctx, &again, &again_count) == 0);
        TASK_CHECK(again == modules);
        TASK_CHECK(again_count == count);
    }

    task_free_image(&image);
    pdb_destroy_context(ctx);

    /* An empty module registry succeeds with count 0 and a NULL array. */
    TASK_CHECK(load_image(&ctx, &image, 4) == 0);
    {
        uint32_t nr_modules = 99;
        const struct pdb_module *none = (const struct pdb_module *)1;
        TASK_CHECK(pdb_get_modules(ctx, &none, &nr_modules) == 0);
        TASK_CHECK(nr_modules == 0);
        TASK_CHECK(none == NULL);

        /* No module means no valid symbol view query. */
        const struct pdb_module_symbol *symbols = (const struct pdb_module_symbol *)1;
        uint32_t nr_symbols = 99;
        TASK_CHECK(pdb_get_module_symbols(ctx, 0, &symbols, &nr_symbols) < 0);
        TASK_CHECK(pdb_errno(ctx) == EPDB_INVALID_PARAMETER);
    }
    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_module_symbol_view(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module_symbol *symbols = NULL;
    uint32_t count = 0;

    uint32_t stream_size = 0;
    const unsigned char *stream = NULL;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_get_module_symbols(ctx, 0, &symbols, &count) == 0);
    stream = pdb_get_stream(ctx, 8, &stream_size);
    TASK_CHECK(count == 2);
    TASK_CHECK(symbols != NULL);

    /* The signature at offset 0 is not enumerated; the procedure starts at 4. */
    TASK_CHECK(symbols[0].offset == 4);
    TASK_CHECK(symbols[0].kind == KIND_PROC32);
    TASK_CHECK(symbols[0].size == 52);
    TASK_CHECK(symbols[0].data != NULL);
    TASK_CHECK(((const unsigned char *)symbols[0].data)[0] == (unsigned char)(52 - 2));
    TASK_CHECK(symbols[1].kind == KIND_END);
    TASK_CHECK(symbols[1].offset == 4 + 52);

    /* Every record lies entirely inside the declared symbol region. */
    TASK_CHECK(stream != NULL);
    TASK_CHECK(stream_size == 76);
    for (uint32_t i = 0; i < count; i++) {
        TASK_CHECK(symbols[i].offset >= 4);
        TASK_CHECK(symbols[i].offset + symbols[i].size <= 60);
    }

    /* The second module carries an unrecognized record that must still occupy a slot. */
    {
        const struct pdb_module_symbol *second = NULL;
        uint32_t second_count = 0;
        TASK_CHECK(pdb_get_module_symbols(ctx, 1, &second, &second_count) == 0);
        TASK_CHECK(second_count == 3);
        TASK_CHECK(second[0].kind == KIND_LPROC32);
        TASK_CHECK(second[1].kind == 0x7777);
        TASK_CHECK(second[1].offset == 4 + 52);
        TASK_CHECK(second[1].size == 8);
        TASK_CHECK(second[2].kind == KIND_END);
    }

    /* Borrowed arrays stay stable across repeated queries. */
    {
        const struct pdb_module_symbol *again = NULL;
        uint32_t again_count = 0;
        TASK_CHECK(pdb_get_module_symbols(ctx, 0, &again, &again_count) == 0);
        TASK_CHECK(again == symbols);
        TASK_CHECK(again_count == count);
    }

    /* An invalid module index is rejected. */
    TASK_CHECK(pdb_get_module_symbols(ctx, 2, &symbols, &count) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_INVALID_PARAMETER);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_module_symbol_boundary(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module_symbol *symbols = NULL;
    uint32_t count = 0;
    uint32_t stream_size = 0;
    const unsigned char *stream = NULL;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_get_module_symbols(ctx, 0, &symbols, &count) == 0);
    stream = pdb_get_stream(ctx, 8, &stream_size);

    /*
     * The C13 line data begins at SymByteSize. Assertions below show that the
     * symbol view stops before it: no record may start at or beyond it, and the
     * last record ends exactly there.
     */
    TASK_CHECK(stream != NULL);
    TASK_CHECK(stream_size == 76);
    TASK_CHECK(symbols[count - 1].offset + symbols[count - 1].size == 60);
    TASK_CHECK(60 + 12 + 4 == stream_size);

    /* The bytes at SymByteSize are the C13 line information, not a symbol. */
    TASK_CHECK(stream[60] == 0xf3);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_modules_after_input_free(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module *modules = NULL;
    uint32_t count = 0;
    const struct pdb_module_symbol *symbols = NULL;
    uint32_t symbol_count = 0;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
    TASK_CHECK(pdb_get_module_symbols(ctx, 1, &symbols, &symbol_count) == 0);

    /* The backing input is no longer needed once loaded. */
    task_free_image(&image);

    TASK_CHECK(count == 2);
    TASK_CHECK(strcmp(modules[0].object_name, "dirA/same.obj") == 0);
    TASK_CHECK(strcmp(modules[1].object_name, "dirB/same.obj") == 0);
    TASK_CHECK(symbol_count == 3);
    TASK_CHECK(symbols[1].kind == 0x7777);
    TASK_CHECK(symbols[1].data != NULL);

    /* Symbols still resolve without the original input buffer. */
    {
        struct pdb_procedure proc;
        TASK_CHECK(pdb_resolve_procedure_ref(ctx, 0, &proc) == 0);
        TASK_CHECK(strcmp(proc.name, "shared_name") == 0);
    }

    pdb_destroy_context(ctx);
    return 0;
}

static int case_reset_and_reload(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module *modules = NULL;
    uint32_t count = 0;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
    task_free_image(&image);

    pdb_reset_context(ctx);
    /* After a reset the context reports no PDB without touching stale state. */
    TASK_CHECK(pdb_errno(ctx) == EPDB_SUCCESS);

    /* A different material can be loaded into the same context afterwards. */
    TASK_CHECK(task_make_image(&image, 1) == 0);
    TASK_CHECK(pdb_load(ctx, image.data, image.size) == 0);
    TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
    TASK_CHECK(count == 2);
    TASK_CHECK(modules[0].stream_index == 8);
    TASK_CHECK(modules[1].stream_index == 9);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* references group                                                           */
/* ------------------------------------------------------------------------- */

static int case_reference_to_global_and_local(void)
{
    struct task_image image;
    void *ctx = NULL;
    struct pdb_procedure proc;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);

    /* S_PROCREF resolves to the S_GPROC32 in module 0. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[0], &proc) == 0);
    TASK_CHECK(proc.module_index == 0);
    TASK_CHECK(proc.symbol_offset == 4);
    TASK_CHECK(strcmp(proc.name, "shared_name") == 0);
    TASK_CHECK(proc.section == 1);
    TASK_CHECK(proc.address_offset == 0x20);
    TASK_CHECK(proc.code_size == 0x20);
    TASK_CHECK(proc.type_index == 0x74);
    TASK_CHECK(proc.is_local == false);

    /* S_LPROCREF resolves to the S_LPROC32 in module 1. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[1], &proc) == 0);
    TASK_CHECK(proc.module_index == 1);
    TASK_CHECK(proc.symbol_offset == 4);
    TASK_CHECK(strcmp(proc.name, "shared_name") == 0);
    TASK_CHECK(proc.address_offset == 0x80);
    TASK_CHECK(proc.is_local == true);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_same_offsets_distinct_definitions(void)
{
    struct task_image image;
    void *ctx = NULL;
    struct pdb_procedure first;
    struct pdb_procedure second;

    TASK_CHECK(load_image(&ctx, &image, 0) == 0);
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[0], &first) == 0);
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[1], &second) == 0);

    /*
     * Both references point at ibSym == 4 and both names are identical, yet
     * the imod selects distinct modules and therefore distinct procedures.
     */
    TASK_CHECK(first.symbol_offset == 4);
    TASK_CHECK(second.symbol_offset == 4);
    TASK_CHECK(strcmp(first.name, second.name) == 0);
    TASK_CHECK(first.module_index != second.module_index);
    TASK_CHECK(first.address_offset != second.address_offset);
    TASK_CHECK(first.is_local != second.is_local);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_invalid_references(void)
{
    struct task_image image;
    void *ctx = NULL;
    struct pdb_procedure proc;

    TASK_CHECK(task_make_image(&image, 3) == 0);
    ctx = pdb_create_context(NULL, NULL);
    TASK_CHECK(ctx != NULL);
    TASK_CHECK(pdb_load(ctx, image.data, image.size) == 0);

    /* imod 3 is one past the last module. */
    memset(&proc, 0xaa, sizeof(proc));
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.bad_reference_offsets[0], &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_PARSE_ERROR);
    /* Outputs are cleared on failure. */
    TASK_CHECK(proc.name == NULL);
    TASK_CHECK(proc.module_index == 0);

    /* ibSym points into the middle of a record. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.bad_reference_offsets[1], &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_PARSE_ERROR);

    /* ibSym lands in the C13 line information, past SymByteSize. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.bad_reference_offsets[2], &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_PARSE_ERROR);

    /* A record-start offset that is not a supported procedure kind. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.bad_reference_offsets[3], &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_UNSUPPORTED_VERSION);

    /* A valid record offset that is not the start of any record. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, 1, &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_PARSE_ERROR);

    /* A subsequent, well-formed reference must still succeed. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.bad_reference_offsets[4], &proc) == 0);
    TASK_CHECK(proc.module_index == 0);
    TASK_CHECK(proc.symbol_offset == 4);
    TASK_CHECK(strcmp(proc.name, "shared_name") == 0);

    /* And the original valid references still resolve. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[1], &proc) == 0);
    TASK_CHECK(proc.module_index == 1);
    TASK_CHECK(proc.is_local == true);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* optional_streams group                                                     */
/* ------------------------------------------------------------------------- */

static int case_optional_psi(void)
{
    struct task_image image;
    void *ctx = NULL;
    const PUBSYM32 *pub = NULL;
    uint32_t nr_public = 0;
    uint32_t nr_symbols = 0;

    TASK_CHECK(load_image(&ctx, &image, 1) == 0);

    /* Global enumeration and S_PUB32 enumeration inspect the record stream. */
    TASK_CHECK(pdb_get_nr_symbols(ctx, &nr_symbols) == 0);
    TASK_CHECK(nr_symbols == 3);
    TASK_CHECK(pdb_get_nr_public_symbols(ctx, &nr_public) == 0);
    TASK_CHECK(nr_public == 1);

    /* Lookup falls back to a scan and honours case sensitivity. */
    pub = pdb_lookup_public_symbol(ctx, "public_marker", true);
    TASK_CHECK(pub != NULL);
    TASK_CHECK(strcmp((const char *)pub->name, "public_marker") == 0);
    TASK_CHECK(pub->off == 0x20);
    TASK_CHECK(pub->seg == 1);

    TASK_CHECK(pdb_lookup_public_symbol(ctx, "PUBLIC_MARKER", false) != NULL);
    TASK_CHECK(pdb_lookup_public_symbol(ctx, "PUBLIC_MARKER", true) == NULL);
    TASK_CHECK(pdb_errno(ctx) == EPDB_NOT_FOUND);

    /* Module data remains available without the public table. */
    {
        const struct pdb_module *modules = NULL;
        uint32_t count = 0;
        TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
        TASK_CHECK(count == 2);
    }

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_optional_global_stream(void)
{
    struct task_image image;
    void *ctx = NULL;
    uint32_t nr_symbols = 99;
    uint32_t nr_public = 99;
    const PUBSYM32 *symbols[1] = {NULL};
    struct pdb_procedure proc;

    TASK_CHECK(load_image(&ctx, &image, 2) == 0);

    /* Without GSS both enumerations are empty. */
    TASK_CHECK(pdb_get_nr_symbols(ctx, &nr_symbols) == 0);
    TASK_CHECK(nr_symbols == 0);
    TASK_CHECK(pdb_get_nr_public_symbols(ctx, &nr_public) == 0);
    TASK_CHECK(nr_public == 0);
    TASK_CHECK(pdb_get_symbols(ctx, (const SYMTYPE **)symbols) == 0);

    /* A procedure reference cannot exist without a record stream. */
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, 0, &proc) < 0);
    TASK_CHECK(pdb_errno(ctx) == EPDB_NOT_FOUND);

    /* Name lookup returns EPDB_NOT_FOUND rather than an empty success. */
    TASK_CHECK(pdb_lookup_public_symbol(ctx, "public_marker", true) == NULL);
    TASK_CHECK(pdb_errno(ctx) == EPDB_NOT_FOUND);

    /* Modules remain readable through the same load. */
    {
        const struct pdb_module *modules = NULL;
        uint32_t count = 0;
        TASK_CHECK(pdb_get_modules(ctx, &modules, &count) == 0);
        TASK_CHECK(count == 2);
    }

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

static int case_modules_without_public_parser(void)
{
    struct task_image image;
    void *ctx = NULL;
    const struct pdb_module_symbol *symbols = NULL;
    uint32_t count = 0;
    struct pdb_procedure proc;

    /* Mode 1 has no PSI/GSI: module access must not depend on them. */
    TASK_CHECK(load_image(&ctx, &image, 1) == 0);

    TASK_CHECK(pdb_get_module_symbols(ctx, 0, &symbols, &count) == 0);
    TASK_CHECK(count == 2);
    TASK_CHECK(symbols[0].kind == KIND_PROC32);
    TASK_CHECK(pdb_resolve_procedure_ref(ctx, image.reference_offsets[0], &proc) == 0);
    TASK_CHECK(proc.module_index == 0);

    task_free_image(&image);
    pdb_destroy_context(ctx);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* optional_streams: real pdbparse process                                    */
/* ------------------------------------------------------------------------- */

static int run_pdbparse(const char *pdbpath, const char *const *args, int nr_args, char *out,
                        size_t out_size, int *exit_code)
{
    char command[1024];
    size_t used = 0;
    int written = snprintf(command, sizeof(command), "%s", PDBPARSE_PATH);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return -1;
    }
    used = (size_t)written;

    for (int i = 0; i < nr_args; i++) {
        written = snprintf(command + used, sizeof(command) - used, " %s", args[i]);
        if (written < 0 || (size_t)written >= sizeof(command) - used) {
            return -1;
        }
        used += (size_t)written;
    }
    written = snprintf(command + used, sizeof(command) - used, " %s", pdbpath);
    if (written < 0 || (size_t)written >= sizeof(command) - used) {
        return -1;
    }

    char output_path[512];
    written = snprintf(
        output_path, sizeof(output_path), "%s.out", pdbpath);
    if (written < 0 || (size_t)written >= sizeof(output_path)) {
        return -1;
    }

    char full[1400];
    written = snprintf(
        full, sizeof(full), "%s >%s 2>&1", command, output_path);
    if (written < 0 || (size_t)written >= sizeof(full)) {
        return -1;
    }

    int status = system(full);
    if (status < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    }
    else {
        *exit_code = -1;
    }

    FILE *file = fopen(output_path, "rb");
    if (file == NULL) {
        return -1;
    }
    size_t n = fread(out, 1, out_size - 1, file);
    out[n] = '\0';
    fclose(file);
    remove(output_path);
    return 0;
}

static int write_image_file(struct task_image *image, int mode, const char *path)
{
    if (task_make_image(image, mode) != 0) {
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    size_t written = fwrite(image->data, 1, image->size, file);
    if (written != image->size || fclose(file) != 0) {
        return -1;
    }
    return 0;
}

/*
 * The harness builds into .task-build before running, so that directory is
 * writable and independent of the host's temp-file configuration. The process
 * id keeps parallel runs from colliding.
 */
static int temp_pdb_path(char *path, size_t size, const char *tag)
{
    int written =
        snprintf(path, size, ".task-build/%s-%ld.pdb", tag, (long)getpid());
    return written > 0 && (size_t)written < size ? 0 : -1;
}

static int contains(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static int case_pdbparse_modules_output(void)
{
    struct task_image image;
    static char output[65536];
    int exit_code = -1;
    char path[512];
    TASK_CHECK(temp_pdb_path(path, sizeof(path), "libpdb-modules") == 0);
    TASK_CHECK(write_image_file(&image, 0, path) == 0);

    {
        const char *args[] = {"--modules"};
        TASK_CHECK(run_pdbparse(path, args, 1, output, sizeof(output), &exit_code) == 0);
    }
    TASK_CHECK(exit_code == 0);

    /* Both physical modules, each identified by index and object path. */
    TASK_CHECK(contains(output, "[0]"));
    TASK_CHECK(contains(output, "[1]"));
    TASK_CHECK(contains(output, "same.obj"));
    TASK_CHECK(contains(output, "dirA/same.obj"));
    TASK_CHECK(contains(output, "dirB/same.obj"));

    /* Module symbol entries are shown. */
    TASK_CHECK(contains(output, "0x1110"));
    TASK_CHECK(contains(output, "0x110f"));
    TASK_CHECK(contains(output, "0x7777"));

    /* Global references identify target module and procedure identity. */
    TASK_CHECK(contains(output, "ref offset=0x0 module=0 symbol_offset=0x4"));
    TASK_CHECK(contains(output, "ref offset=0x1c module=1 symbol_offset=0x4"));
    TASK_CHECK(contains(output, "section:offset=1:0x20"));
    TASK_CHECK(contains(output, "section:offset=1:0x80"));
    TASK_CHECK(contains(output, "shared_name"));

    remove(path);
    task_free_image(&image);
    return 0;
}

static int case_pdbparse_optional_and_composable(void)
{
    struct task_image image;
    static char output[65536];
    int exit_code = -1;
    char path[512];

    /*
     * Mode 1 removes PSI/GSI. --modules still works, and composes with the
     * pre-existing -f, -S, and -s options.
     */
    TASK_CHECK(temp_pdb_path(path, sizeof(path), "libpdb-optional") == 0);
    TASK_CHECK(write_image_file(&image, 1, path) == 0);

    {
        const char *args[] = {"--modules", "-f", "-S", "-s"};
        TASK_CHECK(run_pdbparse(path, args, 4, output, sizeof(output), &exit_code) == 0);
    }
    TASK_CHECK(exit_code == 0);
    TASK_CHECK(contains(output, "PDB Header:"));
    TASK_CHECK(contains(output, "Section Headers:"));
    TASK_CHECK(contains(output, "Public stream contains"));
    TASK_CHECK(contains(output, "public_marker"));
    TASK_CHECK(contains(output, "Modules (2):"));
    TASK_CHECK(contains(output, "ref offset=0x0 module=0"));
    TASK_CHECK(contains(output, "ref offset=0x1c module=1"));

    remove(path);
    task_free_image(&image);
    return 0;
}

static int case_pdbparse_reference_failure(void)
{
    struct task_image image;
    static char output[65536];
    int exit_code = 0;
    char path[512];
    char needle[128];
    TASK_CHECK(temp_pdb_path(path, sizeof(path), "libpdb-badref") == 0);

    /* Mode 3 appends a reference with an out-of-range module number. */
    TASK_CHECK(write_image_file(&image, 3, path) == 0);

    {
        const char *args[] = {"--modules"};
        TASK_CHECK(run_pdbparse(path, args, 1, output, sizeof(output), &exit_code) == 0);
    }
    TASK_CHECK(exit_code != 0);

    /* The failing global record offset is named, as a hex byte offset. */
    int written = snprintf(
        needle, sizeof(needle), "record offset 0x%x", image.bad_reference_offsets[0]);
    TASK_CHECK(written > 0);
    TASK_CHECK(contains(output, needle));

    remove(path);
    task_free_image(&image);
    return 0;
}

const struct task_case task_cases[] = {
    {"modules", "registry", case_module_registry},
    {"modules", "symbol_view", case_module_symbol_view},
    {"modules", "symbol_boundary", case_module_symbol_boundary},
    {"modules", "input_freed", case_modules_after_input_free},
    {"modules", "reset_reload", case_reset_and_reload},
    {"references", "global_and_local", case_reference_to_global_and_local},
    {"references", "same_offsets_distinct", case_same_offsets_distinct_definitions},
    {"references", "invalid", case_invalid_references},
    {"optional_streams", "no_psi", case_optional_psi},
    {"optional_streams", "no_global_stream", case_optional_global_stream},
    {"optional_streams", "modules_without_public", case_modules_without_public_parser},
    {"optional_streams", "pdbparse_modules", case_pdbparse_modules_output},
    {"optional_streams", "pdbparse_composable", case_pdbparse_optional_and_composable},
    {"optional_streams", "pdbparse_reference_failure", case_pdbparse_reference_failure},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
