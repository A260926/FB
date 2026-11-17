#pragma once

// MSF / CodeView / PDB on-disk layouts used by the parser.
// Field layouts follow the publicly documented Microsoft PDB/MSF formats and
// were cross-checked against LLVM DebugInfo PDB/CodeView headers
// (Apache-2.0 WITH LLVM-exception). See THIRD_PARTY_NOTICES.md.

#include <cstdint>

#pragma pack(push, 1)

namespace MemPDB::detail
{
    inline constexpr char kMSFMagic[32] = {
        'M','i','c','r','o','s','o','f','t',' ','C','/','C','+','+',' ',
        'M','S','F',' ','7','.','0','0','\r','\n','\x1a','D','S',
        '\0','\0','\0'
    };

    struct MSFSuperBlock
    {
        char     Magic[32];
        uint32_t BlockSize;
        uint32_t FreeBlockMapBlock;
        uint32_t NumBlocks;
        uint32_t NumDirectoryBytes;
        uint32_t Unknown;
        uint32_t BlockMapAddr;
    };

    struct DBIStreamHeader
    {
        int32_t  VersionSignature;
        uint32_t VersionHeader;
        uint32_t Age;
        uint16_t GlobalStreamIndex;
        uint16_t BuildNumber;
        uint16_t PublicStreamIndex;
        uint16_t PdbDllVersion;
        uint16_t SymRecordStream;
        uint16_t PdbDllRbld;
        int32_t  ModInfoSize;
        int32_t  SectionContributionSize;
        int32_t  SectionMapSize;
        int32_t  SourceInfoSize;
        int32_t  TypeServerMapSize;
        uint32_t MFCTypeServerIndex;
        int32_t  OptionalDbgHeaderSize;
        int32_t  ECSubstreamSize;
        uint16_t Flags;
        uint16_t Machine;
        uint32_t Padding;
    };

    struct SectionHeader
    {
        char     Name[8];
        uint32_t VirtualSize;
        uint32_t VirtualAddress;
        uint32_t SizeOfRawData;
        uint32_t PointerToRawData;
        uint32_t PointerToRelocations;
        uint32_t PointerToLinenumbers;
        uint16_t NumberOfRelocations;
        uint16_t NumberOfLinenumbers;
        uint32_t Characteristics;
    };

    struct PublicSym32Payload
    {
        uint32_t PublicSymFlags;
        uint32_t Offset;
        uint16_t Segment;
    };

    // PublicSymFlags bits (CV_PUBSYMFLAGS).
    inline constexpr uint32_t kPubSymFlag_Code     = 0x1; // lives in a code section
    inline constexpr uint32_t kPubSymFlag_Function = 0x2; // callable function
    inline constexpr uint32_t kPubSymFlag_Managed  = 0x4; // managed code
    inline constexpr uint32_t kPubSymFlag_MSIL     = 0x8; // MSIL

    struct ProcSym32Payload
    {
        uint32_t Parent;
        uint32_t End;
        uint32_t Next;
        uint32_t ProcLen;
        uint32_t DbgStart;
        uint32_t DbgEnd;
        uint32_t TypeIndex;
        uint32_t Offset;
        uint16_t Segment;
        uint8_t  Flags;
    };

    struct RegRelSym32Payload
    {
        uint32_t Offset;
        uint32_t TypeIndex;
        uint16_t Register;
    };

    struct FrameRelSymPayload
    {
        uint32_t Offset;
        uint32_t TypeIndex;
    };

    struct TPIStreamHeader
    {
        uint32_t Version;
        uint32_t HeaderSize;
        uint32_t TypeIndexBegin;
        uint32_t TypeIndexEnd;
        uint32_t TypeRecordBytes;
        int16_t  HashStreamIndex;
        int16_t  HashAuxStreamIndex;
        uint32_t HashKeySize;
        uint32_t NumHashBuckets;
        int32_t  HashValueBufferOffset;
        int32_t  HashValueBufferLength;
        int32_t  IndexOffsetBufferOffset;
        int32_t  IndexOffsetBufferLength;
        int32_t  HashAdjBufferOffset;
        int32_t  HashAdjBufferLength;
    };

    // The leading section-contribution sub-entry is exactly 28 bytes and starts
    // with int16 Section + 2 pad bytes (NOT a uint32). Getting this wrong shifts
    // ModuleSymStream / SymByteSize and desyncs the whole module-info walk.
    struct ModuleInfoEntry
    {
        uint32_t Unused1;
        // --- SectionContribEntry (28 bytes) ---
        int16_t  ScSection;
        uint16_t ScPadding1;
        int32_t  ScOffset;
        int32_t  ScSize;
        uint32_t ScCharacteristics;
        int16_t  ScModuleIndex;
        uint16_t ScPadding2;
        uint32_t ScDataCrc;
        uint32_t ScRelocCrc;
        // --- module fields ---
        uint16_t Flags;
        uint16_t ModuleSymStream;
        uint32_t SymByteSize;
        uint32_t C11ByteSize;
        uint32_t C13ByteSize;
        uint16_t SourceFileCount;
        uint16_t Padding1;
        uint32_t Unused2;
        uint32_t SourceFileNameIndex;
        uint32_t PdbFilePathNameIndex;
    };
    static_assert(sizeof(ModuleInfoEntry) == 64, "DBI module-info entry must be 64 bytes");

    inline constexpr uint16_t kS_PUB32      = 0x110E;
    inline constexpr uint16_t kS_GPROC32    = 0x1110;
    inline constexpr uint16_t kS_LPROC32    = 0x110F;
    inline constexpr uint16_t kS_GPROC32_ID = 0x1147; // modern MSVC: type idx -> IPI
    inline constexpr uint16_t kS_LPROC32_ID = 0x1146;
    inline constexpr uint16_t kS_REGREL32   = 0x1111;
    inline constexpr uint16_t kS_FRAMEREL   = 0x1101;
    inline constexpr uint16_t kS_END        = 0x0006;
    inline constexpr uint16_t kS_ENDPROC    = 0x114F;

    inline constexpr uint16_t kLF_POINTER   = 0x1002;
    inline constexpr uint16_t kLF_PROCEDURE = 0x1008;
    inline constexpr uint16_t kLF_MFUNCTION = 0x1009;
    inline constexpr uint16_t kLF_ARGLIST   = 0x1201;
    inline constexpr uint16_t kLF_MODIFIER  = 0x1001;
    inline constexpr uint16_t kLF_FUNC_ID   = 0x1601; // IPI: { u32 scope; u32 type; name }
    inline constexpr uint16_t kLF_MFUNC_ID  = 0x1602; // IPI: { u32 parent; u32 type; name }

    // Aggregate / member type leaves (TPI).
    inline constexpr uint16_t kLF_ARRAY     = 0x1503;
    inline constexpr uint16_t kLF_CLASS     = 0x1504;
    inline constexpr uint16_t kLF_STRUCTURE = 0x1505;
    inline constexpr uint16_t kLF_UNION     = 0x1506;
    inline constexpr uint16_t kLF_ENUM      = 0x1507;
    inline constexpr uint16_t kLF_BITFIELD  = 0x1205;
    inline constexpr uint16_t kLF_FIELDLIST = 0x1203;

    // Field-list sub-leaves (the members inside a struct/class/union/enum).
    inline constexpr uint16_t kLF_BCLASS    = 0x1400; // base class
    inline constexpr uint16_t kLF_VBCLASS   = 0x1401; // virtual base
    inline constexpr uint16_t kLF_IVBCLASS  = 0x1402; // indirect virtual base
    inline constexpr uint16_t kLF_INDEX     = 0x1404; // continuation -> next fieldlist
    inline constexpr uint16_t kLF_VFUNCTAB  = 0x1409; // vtable pointer slot
    inline constexpr uint16_t kLF_FRIENDCLS = 0x150A;
    inline constexpr uint16_t kLF_ENUMERATE = 0x1502; // enum value
    inline constexpr uint16_t kLF_MEMBER    = 0x150D; // data member
    inline constexpr uint16_t kLF_STMEMBER  = 0x150E; // static data member
    inline constexpr uint16_t kLF_METHOD    = 0x150F; // overloaded method group
    inline constexpr uint16_t kLF_NESTTYPE  = 0x1510; // nested type
    inline constexpr uint16_t kLF_ONEMETHOD = 0x1511; // single method

    // Property flag in struct/class/union records: forward-reference (no body).
    inline constexpr uint16_t kCV_PROP_FWDREF = 0x0080;

    inline constexpr uint32_t kT_VOID      = 0x0003;
    inline constexpr uint32_t kT_BOOL08    = 0x0030;
    inline constexpr uint32_t kT_CHAR      = 0x0010;
    inline constexpr uint32_t kT_UCHAR     = 0x0020;
    inline constexpr uint32_t kT_SHORT     = 0x0011;
    inline constexpr uint32_t kT_USHORT    = 0x0021;
    inline constexpr uint32_t kT_LONG      = 0x0012;
    inline constexpr uint32_t kT_ULONG     = 0x0022;
    inline constexpr uint32_t kT_INT4      = 0x0074;
    inline constexpr uint32_t kT_UINT4     = 0x0075;
    inline constexpr uint32_t kT_INT8      = 0x0076;
    inline constexpr uint32_t kT_UINT8     = 0x0077;
    inline constexpr uint32_t kT_REAL32    = 0x0040;
    inline constexpr uint32_t kT_REAL64    = 0x0041;
    inline constexpr uint32_t kT_WCHAR     = 0x0071;
    inline constexpr uint32_t kT_RCHAR     = 0x0070;
    inline constexpr uint32_t kT_INT2      = 0x0072;
    inline constexpr uint32_t kT_UINT2     = 0x0073;

    inline constexpr uint32_t kStreamDeleted = 0xFFFFFFFFu;

    inline constexpr uint16_t kLF_NUMERIC_START = 0x8000;
}

#pragma pack(pop)
