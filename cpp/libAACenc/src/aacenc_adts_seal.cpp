#include "aacenc_adts_seal.h"

#include "genericStds.h"

static USHORT sealCrcStep(USHORT crc, UINT inputBit) {
  UINT feedback = (inputBit ^ ((crc >> 15) & 1u)) & 1u;
  crc = (USHORT)((crc << 1) & 0xffffu);
  if (feedback) crc ^= 0x8005u;
  return crc;
}

USHORT aacEncAdtsSeal_CrcBits(USHORT crc, const UCHAR *data, UINT startBit,
                              UINT numBits) {
  UINT i;
  if (data == NULL) {
    return aacEncAdtsSeal_CrcZeroBits(crc, numBits);
  }
  for (i = 0; i < numBits; i++) {
    UINT bitPos = startBit + i;
    UINT bit = (data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1u;
    crc = sealCrcStep(crc, bit);
  }
  return crc;
}

USHORT aacEncAdtsSeal_CrcZeroBits(USHORT crc, UINT numBits) {
  while (numBits--) {
    crc = sealCrcStep(crc, 0);
  }
  return crc;
}

static void sealPutBitsAt(UCHAR *buffer, UINT startBit, UINT value,
                          UINT numBits) {
  UINT i;
  for (i = 0; i < numBits; i++) {
    UINT bitPos = startBit + i;
    UINT bit = (value >> (numBits - 1 - i)) & 1u;
    if (bit) {
      buffer[bitPos >> 3] |= (UCHAR)(1u << (7 - (bitPos & 7)));
    } else {
      buffer[bitPos >> 3] &= (UCHAR)~(1u << (7 - (bitPos & 7)));
    }
  }
}

static USHORT sealReadBe16(const UCHAR *p) {
  return (USHORT)(((USHORT)p[0] << 8) | p[1]);
}

static void sealWriteBe16(UCHAR *p, USHORT value) {
  p[0] = (UCHAR)(value >> 8);
  p[1] = (UCHAR)(value & 0xff);
}

INT aacEncAdtsSeal_MakePlan(AACENC_ADTS_SEAL_PLAN *plan) {
  UINT i, previousEnd;

  if (plan == NULL || plan->stagedBuffer == NULL) return -1;
  if (plan->nominalBlocks < 2 ||
      plan->nominalBlocks > AACENC_ADTS_SEAL_MAX_AUS)
    return -1;
  if (plan->numAUs < 1 || plan->numAUs >= plan->nominalBlocks) return -1;
  if (plan->protection > 1) return -1;
  if (plan->channelConfig != 1 && plan->channelConfig != 2) return -1;

  plan->rawStartBits =
      56 + (plan->protection ? 16u * plan->nominalBlocks : 0u);
  previousEnd = plan->rawStartBits;

  for (i = 0; i < plan->numAUs; i++) {
    UINT end = plan->stagedEndBits[i];
    UINT segmentBits, audioBits;

    if (end <= previousEnd || (end & 7u) != 0) return -1;
    segmentBits = end - previousEnd;
    if (plan->protection) {
      if (segmentBits <= 16u) return -1;
      audioBits = segmentBits - 16u;
    } else {
      audioBits = segmentBits;
    }
    if ((audioBits & 7u) != 0 || audioBits == 0) return -1;

    plan->audioStartBits[i] = previousEnd;
    plan->audioBits[i] = audioBits;
    plan->audioBytes[i] = audioBits >> 3;
    previousEnd = end;
  }

  plan->originalBits = plan->stagedEndBits[plan->numAUs - 1];
  if (plan->protection) {
    if (plan->numAUs == 1) {
      plan->finalBytes = 9 + plan->audioBytes[0];
    } else {
      UINT audioBytes = 0;
      for (i = 0; i < plan->numAUs; i++) audioBytes += plan->audioBytes[i];
      plan->finalBytes = 7 + 2 * (plan->numAUs - 1) + 2 +
                         2 * plan->numAUs + audioBytes;
    }
  } else {
    UINT audioBytes = 0;
    for (i = 0; i < plan->numAUs; i++) audioBytes += plan->audioBytes[i];
    plan->finalBytes = 7 + audioBytes;
  }

  if (plan->originalBits < plan->finalBytes * 8u) return -1;
  plan->removedBits = plan->originalBits - plan->finalBytes * 8u;
  return 0;
}

INT aacEncAdtsSeal_Build(const AACENC_ADTS_SEAL_PLAN *plan, UCHAR *out) {
  UINT k, n, p, i;
  UINT rawOutByte, rawInByte, rawBytes;

  if (plan == NULL || out == NULL || plan->stagedBuffer == NULL) return -1;
  if (aacEncAdtsSeal_MakePlan((AACENC_ADTS_SEAL_PLAN *)plan) != 0) return -1;

  k = plan->numAUs;
  n = plan->nominalBlocks;
  p = plan->protection;

  FDKmemclear(out, plan->finalBytes);

  /* The fixed/variable header is already canonical; preserve its static
     fields and repair only frame_length and number_of_raw_data_blocks. */
  FDKmemcpy(out, plan->stagedBuffer, 7);
  sealPutBitsAt(out, 30, plan->finalBytes, 13);
  sealPutBitsAt(out, 54, k - 1, 2);

  rawInByte = plan->rawStartBits >> 3;

  if (!p) {
    rawOutByte = 7;
    rawBytes = (plan->originalBits >> 3) - rawInByte;
    FDKmemcpy(out + rawOutByte, plan->stagedBuffer + rawInByte, rawBytes);
    return 0;
  }

  if (k >= 2) {
    UINT positionBytes = 2 * (k - 1);
    USHORT headerCrc;

    /* The first k-1 relative positions do not depend on the relocated raw
       data start. */
    FDKmemcpy(out + 7, plan->stagedBuffer + 7, positionBytes);
    headerCrc = aacEncAdtsSeal_CrcBits(0xffff, out, 0, 56 + 8 * positionBytes);
    sealWriteBe16(out + 7 + positionBytes, headerCrc);

    rawOutByte = 7 + positionBytes + 2;
    rawBytes = (plan->originalBits >> 3) - rawInByte;
    FDKmemcpy(out + rawOutByte, plan->stagedBuffer + rawInByte, rawBytes);
    return 0;
  }

  /* k == 1: replace the independent multi-block raw CRC with the shared
     ADTS header/protection-domain CRC. CRC is affine in its starting state,
     so chain the old protection-domain result after the new 56 header bits
     without CRCing unprotected payload bits. */
  {
    USHORT headerCrc = aacEncAdtsSeal_CrcBits(0xffff, out, 0, 56);
    UINT crcByte = (plan->audioStartBits[0] + plan->audioBits[0]) >> 3;
    USHORT oldRawCrc = sealReadBe16(plan->stagedBuffer + crcByte);
    UINT protectionBits = (plan->channelConfig == 1) ? 192u : 320u;
    USHORT stateDelta = (USHORT)(headerCrc ^ 0xffffu);
    USHORT sharedCrc =
        (USHORT)(oldRawCrc ^
                 aacEncAdtsSeal_CrcZeroBits(stateDelta, protectionBits));

    sealWriteBe16(out + 7, sharedCrc);
    FDKmemcpy(out + 9,
              plan->stagedBuffer + (plan->audioStartBits[0] >> 3),
              plan->audioBytes[0]);
  }

  /* n is intentionally used only to validate the source header layout. */
  (void)n;
  (void)i;
  return 0;
}
