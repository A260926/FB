#ifndef PDB_H
#define PDB_H

#include "pdb/cvinfo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 1)

#define PDB_EXPORT __attribute__((visibility("default")))

#define PDB_SIGNATURE_SZ (32)

typedef enum pdb_errno_t {
    EPDB_SUCCESS = 0,
    EPDB_SYSTEM_ERROR,
    EPDB_ALLOCATION_FAILURE,
    EPDB_NO_PDB_LOADED,
    EPDB_INVALID_PARAMETER,
    EPDB_UNSUPPORTED_VERSION,
    EPDB_PARSE_ERROR,
    EPDB_INVALID_SECTION_IDX,
    EPDB_INVALID_SECTION_OFFSET,
    EPDB_NOT_FOUND,
} pdb_errno_t;

struct guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    unsigned char data4[8];
};

#define IMAGE_SIZEOF_SHORT_NAME 8

struct image_section_header {
    char name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        uint32_t physical_address;
        uint32_t virtual_size;
    } misc;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_line_numbers;
    uint16_t number_of_relocations;
    uint16_t number_of_line_numbers;
    uint32_t characteristics;
};

typedef void *(*malloc_fn)(size_t size);
typedef void (*free_fn)(void *ptr);

/* Verify a PDB file's signature */
PDB_EXPORT bool pdb_sig_match(void *data, size_t len);

/* Initialize and destroy a libpdb parser */
PDB_EXPORT void *pdb_create_context(malloc_fn user_malloc_fn, free_fn user_free_fn);
PDB_EXPORT void pdb_destroy_context(void *context);
PDB_EXPORT void pdb_reset_context(void *context);

/* Parse and in-memory PDB file */
PDB_EXPORT int pdb_load(void *context, const void *pdbdata, size_t length);

/* Get PDB header information */
PDB_EXPORT void pdb_get_header(
    void *context,
    uint32_t *block_size,
    uint32_t *nr_blocks,
    const struct guid **guid,
    uint32_t *age,
    uint32_t *nr_streams);
PDB_EXPORT uint32_t pdb_get_block_size(void *context);
PDB_EXPORT uint32_t pdb_get_nr_blocks(void *context);
PDB_EXPORT const struct guid *pdb_get_guid(void *context);
PDB_EXPORT uint32_t pdb_get_age(void *context);

/* Get a raw PDB stream */
PDB_EXPORT const unsigned char *pdb_get_stream(
    void *context, uint32_t stream_idx, uint32_t *stream_size);
PDB_EXPORT uint32_t pdb_get_nr_streams(void *context);

/* Get an image's section headers */
PDB_EXPORT const struct image_section_header *pdb_get_sections(void *context);
PDB_EXPORT uint32_t pdb_get_nr_sections(void *context);

/* Enumerate PDB symbols - public and global */
PDB_EXPORT int pdb_get_symbols(void *context, const struct SYMTYPE **symbols);
PDB_EXPORT int pdb_get_nr_symbols(void *context, uint32_t *nr_symbols);

/* Enumerate PDB symbols - public only */
PDB_EXPORT int pdb_get_public_symbols(void *context, const PUBSYM32 **symbols);
PDB_EXPORT int pdb_get_nr_public_symbols(void *context, uint32_t *nr_public_symbols);

/*
 * Normalized module information from the DBI ModInfo substream. The module
 * index namespace follows the physical order of the substream, starting at 0.
 * A stream_index of 0xffff means the module has no associated module stream.
 * The name, object_name, and symbol/procedure strings are borrowed from the
 * context and remain valid until the context is reset, reloaded, or destroyed.
 */
struct pdb_module {
    uint32_t index;
    const char *name;
    const char *object_name;
    uint16_t stream_index;
    uint32_t symbol_bytes;
    uint32_t c11_bytes;
    uint32_t c13_bytes;
};

/*
 * A record from a module stream's symbol region. offset and size are measured
 * from the first byte of the module stream (the four-byte CodeView signature
 * is included in the offsets but is not itself enumerated). size includes the
 * record length and kind fields.
 */
struct pdb_module_symbol {
    uint32_t offset;
    uint32_t size;
    uint16_t kind;
    const unsigned char *data;
};

/*
 * The target of an S_PROCREF or S_LPROCREF record resolved into a module's
 * procedure definition. symbol_offset is the offset of the S_GPROC32 or
 * S_LPROC32 record in the target module stream.
 */
struct pdb_procedure {
    uint32_t module_index;
    uint32_t symbol_offset;
    const char *name;
    uint16_t section;
    uint32_t address_offset;
    uint32_t code_size;
    uint32_t type_index;
    bool is_local;
};

/* Enumerate the DBI module registry (physical order) */
PDB_EXPORT int pdb_get_modules(
    void *context, const struct pdb_module **modules, uint32_t *count);

/* Enumerate the symbols belonging to a module stream's symbol region */
PDB_EXPORT int pdb_get_module_symbols(
    void *context,
    uint32_t module_index,
    const struct pdb_module_symbol **symbols,
    uint32_t *count);

/* Resolve an S_PROCREF/S_LPROCREF record to its procedure definition */
PDB_EXPORT int pdb_resolve_procedure_ref(
    void *context, uint32_t global_record_offset, struct pdb_procedure *out);

/* Find a public symbol by hash */
PDB_EXPORT const PUBSYM32 *pdb_lookup_public_symbol(
    void *context, const char *name, bool case_sensitive);

/* Convert a section index/offset pair to an RVA */
PDB_EXPORT int pdb_convert_section_offset_to_rva(
    void *context, uint16_t section_idx, uint32_t section_offset, uint32_t *rva);

/* Get libpdb error information */
PDB_EXPORT pdb_errno_t pdb_errno(void *context);
PDB_EXPORT const char *pdb_strerror(void *context);

#pragma pack(pop)

#endif  // PDB_H
