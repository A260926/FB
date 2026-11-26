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

/*
 * Status of one MachOReferenceData relation.
 *
 * MACHO_REFERENCE_RESOLVED:
 *      an ordinary indirect table entry pointed at an nlist entry whose
 *      name and library ordinal could all be linked.
 * MACHO_REFERENCE_UNRESOLVED:
 *      the indirect entry is an ordinary (non special) index, but the
 *      nlist index, the string table offset or the library ordinal could
 *      not be linked.  The slot and all raw numbers are still preserved.
 * MACHO_REFERENCE_LOCAL / MACHO_REFERENCE_ABS / MACHO_REFERENCE_LOCAL_ABS:
 *      the indirect entry carried INDIRECT_SYMBOL_LOCAL (0x80000000),
 *      INDIRECT_SYMBOL_ABS (0x40000000) or both bits.  Such values are
 *      state markers, never nlist indexes.
 */
enum MachOReferenceStatus
{
    MACHO_REFERENCE_RESOLVED = 0,
    MACHO_REFERENCE_UNRESOLVED = 1,
    MACHO_REFERENCE_LOCAL = 2,
    MACHO_REFERENCE_ABS = 3,
    MACHO_REFERENCE_LOCAL_ABS = 4
};

/*
 * Meaning of MachOReferenceData.library_ordinal / library_kind.
 *
 * Positive ordinals (1 .. 0xfd) address the loading class dylib commands
 * (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB and
 * LC_LOAD_UPWARD_DYLIB) in their in-file load order.  LC_ID_DYLIB does not
 * take part in the numbering.
 * 0x00  self, 0xfe dynamic lookup, 0xff executable are the Mach-O special
 * ordinals.
 */
enum MachOLibraryOrdinalKind
{
    MACHO_LIBRARY_ORDINAL_NONE = 0,
    MACHO_LIBRARY_ORDINAL_NORMAL = 1,
    MACHO_LIBRARY_ORDINAL_SELF = 2,
    MACHO_LIBRARY_ORDINAL_DYNAMIC_LOOKUP = 3,
    MACHO_LIBRARY_ORDINAL_EXECUTABLE = 4,
    MACHO_LIBRARY_ORDINAL_INVALID = 5
};

#define MACHO_LIBRARY_ORDINAL_SELF_VALUE             0x00
#define MACHO_LIBRARY_ORDINAL_DYNAMIC_LOOKUP_VALUE   0xfe
#define MACHO_LIBRARY_ORDINAL_EXECUTABLE_VALUE       0xff

/*
 * Image wide state of HeaderData.macho_references.
 *
 * MACHO_REFERENCES_OK means the macho_references array is complete.
 * Any other value marks a structural defect (zero stub width, a section
 * whose size is not a whole number of slots, or tables that do not cover
 * the slots).  In that case macho_references_size is 0 and
 * macho_references holds no half complete table; the basic header fields
 * are still valid.
 */
enum MachOReferencesError
{
    MACHO_REFERENCES_OK = 0,
    MACHO_REFERENCES_ERR_STUB_WIDTH = 1,
    MACHO_REFERENCES_ERR_SLOT_ALIGNMENT = 2,
    MACHO_REFERENCES_ERR_INDIRECT_TABLE = 3,
    MACHO_REFERENCES_ERR_MISSING_DYSYMTAB = 4,
    MACHO_REFERENCES_ERR_READ = 5
};

/*
 * One indirect symbol relation:
 * an S_NON_LAZY_SYMBOL_POINTERS / S_LAZY_SYMBOL_POINTERS pointer slot or an
 * S_SYMBOL_STUBS stub, its indirect symbol table entry and, for ordinary
 * entries, the resolved nlist symbol and two-level namespace library.
 *
 * All indexes that name a physical Mach-O object (segment_index,
 * section_index) are 1 based physical ordinals; slot_index is 0 based.
 *
 * Units:
 *  - address       : virtual memory address in bytes (section addr + slot)
 *  - image_offset  : byte offset from the start of the Mach-O image.  When
 *                    getBasicHeaderParserInfo() is called with a non zero
 *                    start, this still is relative to that image start.
 *  - file_offset   : byte offset from the beginning of the whole file,
 *                    i.e. start + image_offset.
 *  - n_value       : raw nlist n_value (64 bit widened for 32 bit images)
 *
 * Every char* member is either NULL or a NUL terminated string owned by the
 * enclosing HeaderData object; free it only through freeHeaderData().
 */
#ifndef MACHO_REFERENCE_DATA
#define MACHO_REFERENCE_DATA
typedef struct MachOReferenceData
{
    /* physical coordinates */
    uint32_t segment_index;             /* 1 based segment command ordinal */
    uint32_t section_index;             /* 1 based ordinal over all sections */
    uint32_t section_index_in_segment;  /* 1 based ordinal inside its segment */
    uint32_t slot_index;                /* 0 based slot inside the section */
    char* segment_name;                 /* owned, e.g. "__DATA" */
    char* section_name;                 /* owned, e.g. "__nl_symbol_ptr" */
    uint8_t section_type;               /* low section type: 6, 7 or 8 */

    /* positions */
    uint64_t address;                   /* virtual address of the slot/stub */
    uint64_t image_offset;              /* bytes relative to image start */
    uint64_t file_offset;               /* bytes relative to whole file */

    /* indirect symbol table */
    uint32_t indirect_index;            /* section reserved1 + slot_index */
    uint32_t indirect_value;            /* raw, endian corrected table word */

    /* one of MachOReferenceStatus */
    uint8_t status;

    /* ordinary nlist identity (only meaningful for RESOLVED/UNRESOLVED) */
    uint32_t nlist_index;               /* raw ordinary indirect value */
    char* symbol_name;                  /* owned, NULL if not linkable */
    uint8_t n_type;                     /* raw n_type byte */
    uint8_t n_sect;                     /* raw n_sect byte */
    uint16_t n_desc;                    /* raw, endian corrected n_desc */
    uint64_t n_value;                   /* raw n_value, widened to 64 bit */

    /* two-level namespace library ordinal */
    int32_t library_ordinal;            /* -1 when not applicable, else 0..255 */
    uint8_t library_kind;               /* one of MachOLibraryOrdinalKind */
    char* library_path;                 /* owned dylib path, or NULL */
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

    /* Mach-O indirect symbol (pointer/stub -> symbol/library) relations.
     * NULL / 0 when the image has no supported section type.
     * macho_references_error is MACHO_REFERENCES_OK or a MachOReferencesError. */
    MachOReferenceData* macho_references;
    size_t macho_references_size;
    uint8_t macho_references_error;
} HeaderData, *PHeaderData;
#endif

#endif
