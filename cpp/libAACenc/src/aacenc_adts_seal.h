#ifndef AACENC_ADTS_SEAL_H
#define AACENC_ADTS_SEAL_H

#include "aacenc_lib.h"

#include "machine_type.h"

#define AACENC_ADTS_SEAL_MAX_AUS 4u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  UINT nominalBlocks;   /* Transport aggregation size N. */
  UINT numAUs;          /* Actually staged access units k. */
  UINT protection;      /* AACENC_PROTECTION / CC_PROTECTION state. */
  UINT sampleFreqIndex; /* ADTS sampling_frequency_index. */
  UINT channelConfig;   /* ADTS channel_configuration. */

  const UCHAR *stagedBuffer; /* Production transport bitstream buffer. */
  UINT stagedEndBits[AACENC_ADTS_SEAL_MAX_AUS]; /* Absolute end after CRC. */

  UINT rawStartBits;
  UINT audioStartBits[AACENC_ADTS_SEAL_MAX_AUS];
  UINT audioBits[AACENC_ADTS_SEAL_MAX_AUS];
  UINT audioBytes[AACENC_ADTS_SEAL_MAX_AUS];

  UINT originalBits;
  UINT finalBytes;
  UINT removedBits;
} AACENC_ADTS_SEAL_PLAN;

/* Calculate all byte/boundary values without modifying encoder state. */
INT aacEncAdtsSeal_MakePlan(AACENC_ADTS_SEAL_PLAN *plan);

/* Write a complete short ADTS group. Capacity is checked by the caller. */
INT aacEncAdtsSeal_Build(const AACENC_ADTS_SEAL_PLAN *plan, UCHAR *out);

/* Scalar ADTS CRC helper used by the production single-block conversion. */
USHORT aacEncAdtsSeal_CrcBits(USHORT crc, const UCHAR *data, UINT startBit,
                              UINT numBits);
USHORT aacEncAdtsSeal_CrcZeroBits(USHORT crc, UINT numBits);

typedef struct {
  UINT startBit;   /* Absolute bit offset in the staged writer buffer. */
  UINT fixedBits;  /* Configured region width (192 or 128 for AAC-LC). */
  UINT actualBits; /* Syntax bits actually covered before zero padding. */
} AACENC_AdtsSealCrcDomain;

/* Test/introspection hook linked into the production static library. */
INT aacEncAdtsSeal_DebugState(
    HANDLE_AACENCODER h, UINT *numAUs, UINT *nominalBlocks,
    UCHAR **stagedBuffer, UINT *endBits, INT endBitCapacity, INT *bitRes,
    INT *bitResMax, AACENC_AdtsSealCrcDomain *domains, INT domainCapacity,
    INT *numDomains);
INT aacEncAdtsSeal_DebugBudget(HANDLE_AACENCODER h, INT *grantedDynBits,
                               INT *usedDynBits);
INT aacEncAdtsSeal_DebugResetAssembly(HANDLE_AACENCODER h);

#ifdef __cplusplus
}
#endif

#endif /* AACENC_ADTS_SEAL_H */
