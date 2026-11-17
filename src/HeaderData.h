#ifndef HEADER_PARSER_HEADER_DATA_H
#define HEADER_PARSER_HEADER_DATA_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum HEADER_TYPE_IDS {
    HEADER_TYPE_NONE,
    HEADER_TYPE_APK,
    HEADER_TYPE_ART,
    HEADER_TYPE_CFBFF,
    HEADER_TYPE_MS_DOS,
    HEADER_TYPE_PE,
    HEADER_TYPE_NE,
    HEADER_TYPE_LE,
    HEADER_TYPE_LX,
    HEADER_TYPE_DEX,
    HEADER_TYPE_ELF,
    HEADER_TYPE_MACH_O,
    HEADER_TYPE_MSI,
    HEADER_TYPE_JAR,
    HEADER_TYPE_JAVA_CLASS,
    HEADER_TYPE_WORD_DOC_X,
    HEADER_TYPE_ZIP,
};

enum ARCHITECTURE_IDS {
    ARCH_UNSUPPORTED,
    ARCH_ALPHA,
    ARCH_ALPHAMOSAIC,
    ARCH_ALC,
    ARCH_ALTERA,
    ARCH_ALTIUM,
    ARCH_AMD,
    ARCH_ANDES,
    ARCH_ANDROID,
    ARCH_ARC,
    ARCH_ARCA,
    ARCH_ARM,
    ARCH_ATMEL,
    ARCH_ATT,
    ARCH_AXIS,
    ARCH_BEYOND,
    ARCH_BLACKFIN,
    ARCH_BLUECHIP,
    ARCH_BROADCOM,
    ARCH_CLOUD_SHIELD,
    ARCH_CRACY,
    ARCH_CSMP,
    ARCH_CSR,
    ARCH_CYAN,
    ARCH_CYPRESS,
    ARCH_CEE,
    ARCH_CEF,
    ARCH_CUDA,
    ARCH_DALLAS,
    ARCH_DEC,
    ARCH_DENSO,
    ARCH_DVAX,
    ARCH_DOT_NET,
    ARCH_EFI_BYTE_CODE,
    ARCH_ELEMENT,
    ARCH_EPSON,
    ARCH_EXCESS,
    ARCH_FREESCALE,
    ARCH_FTDI,
    ARCH_FUJITSU,
    ARCH_HITACHI,
    ARCH_HUMIOF,
    ARCH_IBM,
    ARCH_ICERA,
    ARCH_INFINEON,
    ARCH_INTEL,
    ARCH_IMAGINATION,
    ARCH_JAVA,
    ARCH_KIPO,
    ARCH_KNUTH,
    ARCH_KM,
    ARCH_LATTICE,
    ARCH_LSI,
    ARCH_NANO_RADIO,
    ARCH_NJR,
    ARCH_NS,
    ARCH_NXP,
    ARCH_MAX,
    ARCH_MATSUSHITA,
    ARCH_MCST,
    ARCH_MIPS,
    ARCH_MITSUBISHI,
    ARCH_MOTOROLA,
    ARCH_MOTOROLA_68K,
    ARCH_MOTOROLA_88K,
    ARCH_MOXIE,
    ARCH_MT,
    ARCH_NEC,
    ARCH_OS_X,
    ARCH_PA_RISC,
    ARCH_PANEVE,
    ARCH_PICO_JAVA,
    ARCH_PKU,
    ARCH_PPC,
    ARCH_QUALCOMM,
    ARCH_RENESAS,
    ARCH_RISC,
    ARCH_RISC_OPEN,
    ARCH_RISC_V,
    ARCH_SG,
    ARCH_SHARP,
    ARCH_SIEMENS,
    ARCH_SITERA,
    ARCH_SONY,
    ARCH_SPARC,
    ARCH_STM,
    ARCH_SUN_PLUS,
    ARCH_TENOR,
    ARCH_TENSILICA,
    ARCH_THOMPSON,
    ARCH_TI,
    ARCH_TILERA,
    ARCH_TOYOTA,
    ARCH_TREBIA,
    ARCH_TRW,
    ARCH_UBICOM,
    ARCH_VISUM,
    ARCH_XILINX,
    ARCH_XMOS,
    ARCH_ZILOG,
};

enum endian_type_ids { ENDIAN_NONE, ENDIAN_LITTLE, ENDIAN_BIG };

#define MACHINE_NONE "unsupported"

#define DEFAULT_CODE_REGION_CAPACITY (2)

#ifndef CODE_REGION_DATA
#define CODE_REGION_DATA
typedef struct CodeRegionData
{
    uint64_t start;
    uint64_t end;
    char* name;
} CodeRegionData;
#endif

#ifndef MACHO_REFERENCE_DATA
#define MACHO_REFERENCE_DATA

/**
 * Status of the Mach-O indirect reference result of a parsed image.
 * Stored in HeaderData.macho_references_status.
 */
enum MACHO_REFERENCES_STATUS_IDS {
    MACHO_REFERENCES_NONE = 0, /* no indirect reference sections found (or not a Mach-O); the array is empty */
    MACHO_REFERENCES_OK = 1,   /* the reference table is complete; single slots may still be unresolved */
    MACHO_REFERENCES_ERROR = 2 /* structural error (zero stub width, section size not a multiple of the
                                  slot width, or the indirect table range does not cover the sections);
                                  the incomplete table is dropped: the array is empty */
};

/**
 * Resolution kind of one indirect slot. Stored in MachOReferenceData.kind.
 */
enum MACHO_REFERENCE_KIND_IDS {
    MACHO_REFERENCE_UNRESOLVED = 0, /* raw values are kept, but the symbol index, string offset or library
                                       ordinal could not be resolved; no name is made up */
    MACHO_REFERENCE_SYMBOL = 1,     /* the slot references an nlist symbol */
    MACHO_REFERENCE_LOCAL = 2,      /* INDIRECT_SYMBOL_LOCAL: not an nlist index */
    MACHO_REFERENCE_ABS = 3,        /* INDIRECT_SYMBOL_ABS: not an nlist index */
    MACHO_REFERENCE_LOCAL_ABS = 4   /* INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS: not an nlist index */
};

/**
 * One resolved indirect pointer slot or symbol stub of a thin Mach-O image.
 *
 * There is one entry per section slot, ordered by physical section order first and slot order
 * second. Entries are not merged by name: slots referencing equal symbol names keep their own
 * identity.
 *
 * The array and all strings are owned by the enclosing HeaderData and released by freeHeaderData.
 * They stay valid after the parsed file has been closed or deleted and after other images have
 * been parsed.
 *
 * Units: indexes are plain 0 based counts, addresses and offsets are bytes.
 */
typedef struct MachOReferenceData
{
    uint32_t segment_index;     /* physical index (0 based, load command order) of the owning
                                   LC_SEGMENT/LC_SEGMENT_64 command */
    uint32_t section_index;     /* physical index (0 based, load command order) of the owning section
                                   among all sections of the image */
    char* segment_name;         /* owned segment name, NUL terminated, at most 16 chars */
    char* section_name;         /* owned section name, NUL terminated, at most 16 chars */
    uint32_t section_type;      /* section type: S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS
                                   or S_SYMBOL_STUBS */

    uint64_t slot_index;        /* index (0 based count) of the slot within the section */
    uint64_t slot_address;      /* virtual address of the slot in bytes */
    uint64_t slot_image_offset; /* byte offset of the slot relative to the image start */
    uint64_t slot_file_offset;  /* byte offset of the slot relative to the whole file
                                   (slot_image_offset + the start argument the image was parsed with) */

    uint64_t indirect_index;    /* index (0 based count) into the indirect symbol table */
    uint32_t indirect_value;    /* raw indirect symbol table entry */

    uint8_t kind;               /* one of MACHO_REFERENCE_KIND_IDS */
    uint32_t symbol_index;      /* raw nlist index when the indirect entry is a plain symbol index,
                                   0 for LOCAL/ABS entries */
    char* symbol_name;          /* owned symbol name; NULL when kind is not MACHO_REFERENCE_SYMBOL */
    uint8_t symbol_type;        /* raw n_type (0 when no nlist entry could be read) */
    uint8_t symbol_sect;        /* raw n_sect (0 when no nlist entry could be read) */
    uint16_t symbol_desc;       /* raw n_desc (0 when no nlist entry could be read) */
    uint64_t symbol_value;      /* raw n_value (0 when no nlist entry could be read) */
    uint8_t library_ordinal;    /* library ordinal taken from n_desc: 0 = self, 0xfe = dynamic lookup,
                                   0xff = executable, 1..0xfd = 1 based index into the load class dylib
                                   commands (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB,
                                   LC_LOAD_UPWARD_DYLIB, LC_LAZY_LOAD_DYLIB in load command order;
                                   LC_ID_DYLIB is not counted) */
    char* library_path;         /* owned path of the dylib the ordinal maps to; NULL for
                                   self/dynamic_lookup/executable and for unresolved entries */
} MachOReferenceData;
#endif

#ifndef HEADER_DATA
#define HEADER_DATA
typedef struct HeaderData
{
    uint8_t headertype;
    uint8_t h_bitness;
    uint8_t i_bitness;
    uint8_t endian;
    uint16_t CPU_arch;
    const char* Machine;

    CodeRegionData* code_regions;
    size_t code_regions_size;
    size_t code_regions_capacity;

    /* Mach-O only: resolved indirect symbol references (pointer slots and symbol stubs). */
    MachOReferenceData* macho_references;
    size_t macho_references_size;
    uint8_t macho_references_status; /* one of MACHO_REFERENCES_STATUS_IDS */
} HeaderData, *PHeaderData;
#endif

#endif
