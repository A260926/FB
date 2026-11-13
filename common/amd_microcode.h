#ifndef AMD_MICROCODE_H
#define AMD_MICROCODE_H

#include "basetypes.h"
#include "ubytearray.h"

// Make sure we use right packing rules
#pragma pack(push, 1)

typedef struct AMD_MICROCODE_HEADER_ {
    UINT16 DateYear;
    UINT8  DateDay;
    UINT8  DateMonth;
    UINT32 UpdateRevision;
    UINT16 LoaderID;
    UINT8  DataSize;
    UINT8  InitializationFlag;
    UINT32 DataChecksum;
    UINT16 NorthBridgeVEN_ID;
    UINT16 NorthBridgeDEV_ID;
    UINT16 SouthBridgeVEN_ID;
    UINT16 SouthBridgeDEV_ID;
    UINT16 ProcessorSignature;
    UINT8  NorthBridgeREV_ID;
    UINT8  SouthBridgeREV_ID;
    UINT8  BiosApiRevision;
    UINT8  LoadControl;
    UINT8  Reserved_1E;
    UINT8  Reserved_1F;
} AMD_MICROCODE_HEADER;

UINT32 amdMicrocodeGetDataSize(const AMD_MICROCODE_HEADER *ucodeHeader);
UINT32 amdMicrocodeGetSize(const AMD_MICROCODE_HEADER *ucodeHeader);
UINT32 amdMicrocodeGetCpuId(const AMD_MICROCODE_HEADER *ucodeHeader);
UINT16 amdMicrocodeGetYear(const AMD_MICROCODE_HEADER *ucodeHeader);
UINT8  amdMicrocodeGetMonth(const AMD_MICROCODE_HEADER *ucodeHeader);
UINT8  amdMicrocodeGetDay(const AMD_MICROCODE_HEADER *ucodeHeader);

#pragma pack(pop)

#endif // AMD_MICROCODE_H
