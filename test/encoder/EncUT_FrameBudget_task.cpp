// Deterministic task tests for ENCODER_OPTION_FRAME_BUDGET.
// Registers the budget / rollback / reference groups consumed by
// task_support/driver.c; this file intentionally has no main().

#include "task_support/check.h"

#include "codec/api/wels/codec_api.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <map>
#include <string>

namespace {

const int BIG_CAP = 0x3fffffff;

struct Kit {
  int W, H;
  ISVCEncoder* e;
  ISVCDecoder* d;
};

static void MakeTexture (std::vector<unsigned char>& img, int W, int H, int n) {
  img.resize (static_cast<size_t> (W) * H * 3 / 2);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      img[y * W + x] = static_cast<unsigned char> ((x * 3 + y * 7 + n * 13 + ((x + y) % 17) * n) & 255);
  for (size_t k = static_cast<size_t> (W) * H; k < img.size(); ++k)
    img[k] = static_cast<unsigned char> ((k * 11 + n * 19 + (k / W) * (n + 5)) & 255);
}

static int KitInit (Kit* k, int W, int H, int cabac, int tl, int slices) {
  std::memset (k, 0, sizeof (*k));
  k->W = W;
  k->H = H;
  if (WelsCreateSVCEncoder (&k->e) != 0 || k->e == NULL)
    return 1;
  int trace = WELS_LOG_ERROR;
  k->e->SetOption (ENCODER_OPTION_TRACE_LEVEL, &trace);

  SEncParamExt p;
  if (k->e->GetDefaultParams (&p) != 0)
    return 1;
  p.iUsageType = CAMERA_VIDEO_REAL_TIME;
  p.iPicWidth = W;
  p.iPicHeight = H;
  p.iTargetBitrate = 400000;
  p.iRCMode = RC_BITRATE_MODE;
  p.fMaxFrameRate = 30.0f;
  p.iSpatialLayerNum = 1;
  p.iTemporalLayerNum = tl;
  p.iMultipleThreadIdc = 1;
  p.bEnableFrameSkip = false;
  p.iPaddingFlag = 0;
  p.bEnableLongTermReference = false;
  p.bEnableDenoise = false;
  p.bEnableBackgroundDetection = false;
  p.bEnableSceneChangeDetect = false;
  p.bEnableAdaptiveQuant = false;
  p.eSpsPpsIdStrategy = CONSTANT_ID;
  p.iEntropyCodingModeFlag = cabac;
  p.uiIntraPeriod = 0;
  p.sSpatialLayers[0].iVideoWidth = W;
  p.sSpatialLayers[0].iVideoHeight = H;
  p.sSpatialLayers[0].fFrameRate = 30.0f;
  p.sSpatialLayers[0].iSpatialBitrate = 400000;
  p.sSpatialLayers[0].uiProfileIdc = cabac ? PRO_HIGH : PRO_BASELINE;
  if (slices <= 1) {
    p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
  } else {
    p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    p.sSpatialLayers[0].sSliceArgument.uiSliceNum = slices;
  }
  if (k->e->InitializeExt (&p) != 0)
    return 1;

  if (WelsCreateDecoder (&k->d) != 0 || k->d == NULL)
    return 1;
  k->d->SetOption (DECODER_OPTION_TRACE_LEVEL, &trace);
  SDecodingParam dp;
  std::memset (&dp, 0, sizeof (dp));
  dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  dp.eEcActiveIdc = ERROR_CON_DISABLE;
  if (k->d->Initialize (&dp) != 0)
    return 1;
  return 0;
}

static void KitFree (Kit* k) {
  if (k->d != NULL) {
    k->d->Uninitialize();
    WelsDestroyDecoder (k->d);
  }
  if (k->e != NULL) {
    k->e->Uninitialize();
    WelsDestroySVCEncoder (k->e);
  }
  k->d = NULL;
  k->e = NULL;
}

static int SetBudget (ISVCEncoder* e, int mx, int fq, int lq) {
  SFrameBudget b;
  b.iMaxBytes = mx;
  b.iFirstQp = fq;
  b.iLastQp = lq;
  return e->SetOption (ENCODER_OPTION_FRAME_BUDGET, &b);
}

static int Encode (Kit* k, const std::vector<unsigned char>& img, long long ts, SFrameBSInfo& info) {
  SSourcePicture s;
  std::memset (&s, 0, sizeof (s));
  s.iColorFormat = videoFormatI420;
  s.iPicWidth = k->W;
  s.iPicHeight = k->H;
  s.uiTimeStamp = ts;
  s.iStride[0] = k->W;
  s.iStride[1] = s.iStride[2] = k->W / 2;
  s.pData[0] = const_cast<unsigned char*> (img.data());
  s.pData[1] = const_cast<unsigned char*> (img.data() + static_cast<size_t> (k->W) * k->H);
  s.pData[2] = const_cast<unsigned char*> (img.data() + static_cast<size_t> (k->W) * k->H * 5 / 4);
  std::memset (&info, 0, sizeof (info));
  return k->e->EncodeFrame (&s, &info);
}

static std::vector<unsigned char> ToAu (SFrameBSInfo& info) {
  std::vector<unsigned char> au;
  for (int l = 0; l < info.iLayerNum; ++l) {
    int len = 0;
    for (int j = 0; j < info.sLayerInfo[l].iNalCount; ++j)
      len += info.sLayerInfo[l].pNalLengthInByte[j];
    au.insert (au.end(), info.sLayerInfo[l].pBsBuf, info.sLayerInfo[l].pBsBuf + len);
  }
  return au;
}

static uint64_t Fnv1a (const unsigned char* p, size_t n, uint64_t h) {
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// Decode one AU and return an exact pixel hash. Returns 0 on success.
static int DecodeHash (Kit* k, const std::vector<unsigned char>& au, uint64_t& hash) {
  unsigned char* planes[3] = {NULL, NULL, NULL};
  SBufferInfo bi;
  std::memset (&bi, 0, sizeof (bi));
  int rc = k->d->DecodeFrameNoDelay (const_cast<unsigned char*> (au.data()), static_cast<int> (au.size()),
                                      planes, &bi);
  if (bi.iBufferStatus == 0)
    rc = k->d->FlushFrame (planes, &bi);
  if (rc != dsErrorFree || bi.iBufferStatus != 1)
    return 1;
  const int w = bi.UsrData.sSystemBuffer.iWidth;
  const int h = bi.UsrData.sSystemBuffer.iHeight;
  hash = 14695981039346656037ull;
  for (int c = 0; c < 3; ++c) {
    const int cw = c ? w / 2 : w;
    const int ch = c ? h / 2 : h;
    const int stride = bi.UsrData.sSystemBuffer.iStride[c ? 1 : 0];
    for (int y = 0; y < ch; ++y)
      hash = Fnv1a (planes[c] + static_cast<size_t> (y) * stride, cw, hash);
  }
  return 0;
}

// Fresh encoder IDR AU size at one fixed QP (production quantization path).
static int OracleSize (int W, int H, int cabac, int slices, int q, std::vector<unsigned char>* auOut) {
  Kit k;
  if (KitInit (&k, W, H, cabac, 1, slices) != 0)
    return -1;
  if (SetBudget (k.e, BIG_CAP, q, q) != 0) {
    KitFree (&k);
    return -1;
  }
  std::vector<unsigned char> img;
  MakeTexture (img, W, H, 0);
  SFrameBSInfo info;
  int rv = Encode (&k, img, 0, info);
  int size = -1;
  if (rv == cmResultSuccess && info.iFrameSizeInBytes > 0) {
    size = info.iFrameSizeInBytes;
    if (auOut != NULL)
      *auOut = ToAu (info);
  }
  KitFree (&k);
  return size;
}

/* ----------------------------- budget group ----------------------------- */

static int case_option_validation (void) {
  Kit k;
  TASK_CHECK (KitInit (&k, 64, 64, 0, 1, 1) == 0);

  TASK_CHECK (SetBudget (k.e, -5, 10, 20) != 0);          // negative cap
  TASK_CHECK (SetBudget (k.e, 100, 40, 10) != 0);         // first > last
  TASK_CHECK (SetBudget (k.e, 100, -1, 10) != 0);         // qp below range
  TASK_CHECK (SetBudget (k.e, 100, 10, 52) != 0);         // qp above range
  TASK_CHECK (SetBudget (k.e, 200000, 20, 44) == 0);      // valid
  SFrameBudget rb;
  std::memset (&rb, 0, sizeof (rb));
  TASK_CHECK (k.e->GetOption (ENCODER_OPTION_FRAME_BUDGET, &rb) == cmResultSuccess);
  TASK_CHECK (rb.iMaxBytes == 200000 && rb.iFirstQp == 20 && rb.iLastQp == 44);
  TASK_CHECK (SetBudget (k.e, 100, 40, 10) != 0);         // invalid keeps old

  // Old valid setting still encodes after the rejected update.
  std::vector<unsigned char> img;
  MakeTexture (img, 64, 64, 2);
  SFrameBSInfo info;
  TASK_CHECK (Encode (&k, img, 66, info) == cmResultSuccess);
  TASK_CHECK (info.iFrameSizeInBytes > 0);
  TASK_CHECK (info.iFrameSizeInBytes <= 200000);

  TASK_CHECK (SetBudget (k.e, 0, 0, 0) == 0);             // zero cap disables
  TASK_CHECK (Encode (&k, img, 99, info) == cmResultSuccess);
  TASK_CHECK (info.iFrameSizeInBytes > 0);
  KitFree (&k);
  return 0;
}

static int BoundaryConfig (int W, int H, int cabac, int slices) {
  const int qs[3] = {24, 32, 40};
  for (size_t qi = 0; qi < sizeof (qs) / sizeof (qs[0]); ++qi) {
    const int q = qs[qi];
    std::vector<unsigned char> oracle;
    const int sz = OracleSize (W, H, cabac, slices, q, &oracle);
    TASK_CHECK (sz > 0);
    TASK_CHECK (static_cast<int> (oracle.size()) == sz);

    std::vector<unsigned char> img;
    MakeTexture (img, W, H, 0);

    { // exact byte boundary fits and is byte identical to the oracle.
      Kit k;
      TASK_CHECK (KitInit (&k, W, H, cabac, 1, slices) == 0);
      TASK_CHECK (SetBudget (k.e, sz, q, q) == 0);
      SFrameBSInfo info;
      TASK_CHECK (Encode (&k, img, 0, info) == cmResultSuccess);
      TASK_CHECK (info.iFrameSizeInBytes == sz);
      TASK_CHECK (ToAu (info) == oracle);
      KitFree (&k);
    }
    { // one byte less with a single QP cannot fit: budget exceeded + zeroed out.
      Kit k;
      TASK_CHECK (KitInit (&k, W, H, cabac, 1, slices) == 0);
      TASK_CHECK (SetBudget (k.e, sz - 1, q, q) == 0);
      SFrameBSInfo info;
      TASK_CHECK (Encode (&k, img, 0, info) == cmFrameBudgetExceeded);
      TASK_CHECK (info.iLayerNum == 0);
      TASK_CHECK (info.iFrameSizeInBytes == 0);
      int nal = 0;
      for (int l = 0; l < info.iLayerNum; ++l)
        nal += info.sLayerInfo[l].iNalCount;
      TASK_CHECK (nal == 0);

      // raising the budget retries the same picture successfully
      TASK_CHECK (SetBudget (k.e, sz, q, q) == 0);
      SFrameBSInfo info2;
      TASK_CHECK (Encode (&k, img, 0, info2) == cmResultSuccess);
      TASK_CHECK (ToAu (info2) == oracle);
      KitFree (&k);
    }
  }
  return 0;
}

static int case_budget_boundary_cavlc (void) {
  return BoundaryConfig (64, 64, 0, 1);
}
static int case_budget_boundary_cabac_slice (void) {
  return BoundaryConfig (128, 128, 1, 2);
}

static int case_qp_retry_selection (void) {
  const int W = 128, H = 128;
  std::map<int, int> sz;
  for (int q = 20; q <= 44; q += 2)
    sz[q] = OracleSize (W, H, 1, 2, q, NULL);

  // Pick a cap from a middle QP and require the first fitting QP to be chosen.
  const int capQ = 32;
  const int cap = sz[capQ];
  int expectQ = -1;
  int expectSize = 0;
  for (int q = 20; q <= 44; q += 2) {
    if (sz[q] > 0 && sz[q] <= cap) {
      expectQ = q;
      expectSize = sz[q];
      break;
    }
  }
  TASK_CHECK (expectQ >= 0);

  Kit k;
  TASK_CHECK (KitInit (&k, W, H, 1, 1, 2) == 0);
  TASK_CHECK (SetBudget (k.e, cap, 20, 44) == 0);
  std::vector<unsigned char> img;
  MakeTexture (img, W, H, 0);
  SFrameBSInfo info;
  TASK_CHECK (Encode (&k, img, 0, info) == cmResultSuccess);
  TASK_CHECK (info.iFrameSizeInBytes <= cap);
  TASK_CHECK (info.iFrameSizeInBytes == expectSize);
  TASK_CHECK (expectQ >= 20 && expectQ <= capQ); // tried lower QPs first
  KitFree (&k);
  return 0;
}

static int case_unsupported_change_rejected (void) {
  Kit k;
  TASK_CHECK (KitInit (&k, 128, 128, 1, 3, 2) == 0);
  TASK_CHECK (SetBudget (k.e, BIG_CAP, 28, 40) == 0);

  // Replacing params with two spatial layers must be refused while budget is on.
  SEncParamExt bad;
  TASK_CHECK (k.e->GetDefaultParams (&bad) == 0);
  bad.iUsageType = CAMERA_VIDEO_REAL_TIME;
  bad.iPicWidth = 128;
  bad.iPicHeight = 128;
  bad.iSpatialLayerNum = 2;
  bad.iMultipleThreadIdc = 1;
  TASK_CHECK (k.e->SetOption (ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &bad) != cmResultSuccess);

  // RC mode / frame skip changes that break the envelope are refused.
  int rcOff = RC_OFF_MODE;
  TASK_CHECK (k.e->SetOption (ENCODER_OPTION_RC_MODE, &rcOff) != cmResultSuccess);
  bool skipOn = true;
  TASK_CHECK (k.e->SetOption (ENCODER_OPTION_RC_FRAME_SKIP, &skipOn) != cmResultSuccess);
  int pad = 1;
  TASK_CHECK (k.e->SetOption (ENCODER_PADDING_PADDING, &pad) != cmResultSuccess);

  // Old budget still codes normally after the refused changes.
  std::vector<unsigned char> img;
  MakeTexture (img, 128, 128, 5);
  SFrameBSInfo info;
  TASK_CHECK (Encode (&k, img, 165, info) == cmResultSuccess);
  TASK_CHECK (info.iFrameSizeInBytes > 0);
  TASK_CHECK (info.iFrameSizeInBytes <= BIG_CAP);
  KitFree (&k);
  return 0;
}

static int case_entropy_multislice_decode (void) {
  const int cfgs[4][3] = {{64, 0, 1}, {64, 0, 2}, {128, 1, 1}, {128, 1, 4}};
  for (int c = 0; c < 4; ++c) {
    const int W = cfgs[c][0], cabac = cfgs[c][1], slices = cfgs[c][2];
    Kit k;
    TASK_CHECK (KitInit (&k, W, W, cabac, 1, slices) == 0);
    TASK_CHECK (SetBudget (k.e, 200000, 24, 44) == 0);
    for (int n = 0; n < 6; ++n) {
      std::vector<unsigned char> img;
      MakeTexture (img, W, W, n);
      SFrameBSInfo info;
      TASK_CHECK (Encode (&k, img, n * 33, info) == cmResultSuccess);
      TASK_CHECK (info.iFrameSizeInBytes <= 200000);
      std::vector<unsigned char> au = ToAu (info);
      uint64_t h = 0;
      TASK_CHECK (DecodeHash (&k, au, h) == 0);
      TASK_CHECK (h != 0);
    }
    KitFree (&k);
  }
  return 0;
}

/* ----------------------------- rollback group ---------------------------- */

// A burns an impossible attempt then retries the same image; B encodes clean.
static int case_fail_retry_same (void) {
  const int W = 128, H = 128, Q = 30;
  Kit A, B;
  TASK_CHECK (KitInit (&A, W, H, 1, 3, 2) == 0);
  TASK_CHECK (KitInit (&B, W, H, 1, 3, 2) == 0);
  TASK_CHECK (SetBudget (B.e, BIG_CAP, Q, Q) == 0);

  for (int n = 0; n < 10; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, W, H, n);
    long long ts = n * 33;

    if (n % 3 == 1) {
      TASK_CHECK (SetBudget (A.e, 1, 40, 48) == 0);
      SFrameBSInfo bad;
      TASK_CHECK (Encode (&A, img, ts, bad) == cmFrameBudgetExceeded);
      TASK_CHECK (bad.iLayerNum == 0 && bad.iFrameSizeInBytes == 0);
      TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
    } else {
      TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
    }

    SFrameBSInfo ia, ib;
    TASK_CHECK (Encode (&A, img, ts, ia) == cmResultSuccess);
    TASK_CHECK (Encode (&B, img, ts, ib) == cmResultSuccess);
    std::vector<unsigned char> aa = ToAu (ia), ab = ToAu (ib);
    TASK_CHECK (aa == ab); // rolled-back state is byte identical to clean encoder

    uint64_t ha = 0, hb = 0;
    TASK_CHECK (DecodeHash (&A, aa, ha) == 0);
    TASK_CHECK (DecodeHash (&B, ab, hb) == 0);
    TASK_CHECK (ha == hb);
  }
  KitFree (&A);
  KitFree (&B);
  return 0;
}

// Failure on one image must not consume it; the next different image aligns.
static int case_fail_change_image (void) {
  const int W = 128, H = 128, Q = 32;
  Kit A, B;
  TASK_CHECK (KitInit (&A, W, H, 0, 2, 1) == 0);
  TASK_CHECK (KitInit (&B, W, H, 0, 2, 1) == 0);
  TASK_CHECK (SetBudget (B.e, BIG_CAP, Q, Q) == 0);

  // common committed prefix
  for (int n = 0; n < 3; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, W, H, n);
    TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
    SFrameBSInfo ia, ib;
    TASK_CHECK (Encode (&A, img, n * 33, ia) == 0);
    TASK_CHECK (Encode (&B, img, n * 33, ib) == 0);
    TASK_CHECK (ToAu (ia) == ToAu (ib));
  }

  // poison image 3 fully fails on A
  std::vector<unsigned char> poison;
  MakeTexture (poison, W, H, 3);
  TASK_CHECK (SetBudget (A.e, 1, 42, 50) == 0);
  SFrameBSInfo bad;
  TASK_CHECK (Encode (&A, poison, 99, bad) == cmFrameBudgetExceeded);

  // then a different image 4: A must match B's next committed frame
  std::vector<unsigned char> next;
  MakeTexture (next, W, H, 4);
  TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
  SFrameBSInfo ia, ib;
  TASK_CHECK (Encode (&A, next, 132, ia) == 0);
  TASK_CHECK (Encode (&B, next, 132, ib) == 0);
  TASK_CHECK (ToAu (ia) == ToAu (ib));
  KitFree (&A);
  KitFree (&B);
  return 0;
}

static int case_force_idr_pending (void) {
  const int W = 128, H = 128, Q = 31;
  Kit A, B;
  TASK_CHECK (KitInit (&A, W, H, 1, 1, 1) == 0);
  TASK_CHECK (KitInit (&B, W, H, 1, 1, 1) == 0);
  TASK_CHECK (SetBudget (B.e, BIG_CAP, Q, Q) == 0);

  for (int n = 0; n < 4; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, W, H, n);
    TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
    SFrameBSInfo ia, ib;
    TASK_CHECK (Encode (&A, img, n * 33, ia) == 0);
    TASK_CHECK (Encode (&B, img, n * 33, ib) == 0);
    TASK_CHECK (ToAu (ia) == ToAu (ib));
  }

  // Pending IDR must survive a fully failed budget attempt.
  TASK_CHECK (A.e->ForceIntraFrame (true) == cmResultSuccess);
  TASK_CHECK (B.e->ForceIntraFrame (true) == cmResultSuccess);
  std::vector<unsigned char> img;
  MakeTexture (img, W, H, 4);
  TASK_CHECK (SetBudget (A.e, 1, 42, 50) == 0);
  SFrameBSInfo bad;
  TASK_CHECK (Encode (&A, img, 132, bad) == cmFrameBudgetExceeded);

  TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);
  SFrameBSInfo ia, ib;
  TASK_CHECK (Encode (&A, img, 132, ia) == 0);
  TASK_CHECK (Encode (&B, img, 132, ib) == 0);
  TASK_CHECK (ia.eFrameType == videoFrameTypeIDR);
  TASK_CHECK (ib.eFrameType == videoFrameTypeIDR);
  TASK_CHECK (ToAu (ia) == ToAu (ib)); // IDR numbering not consumed twice
  KitFree (&A);
  KitFree (&B);
  return 0;
}

static int case_statistics_counters (void) {
  Kit k;
  TASK_CHECK (KitInit (&k, 128, 128, 1, 1, 1) == 0);

  SEncoderStatistics s0;
  std::memset (&s0, 0, sizeof (s0));
  TASK_CHECK (k.e->GetOption (ENCODER_OPTION_GET_STATISTICS, &s0) == cmResultSuccess);

  TASK_CHECK (SetBudget (k.e, BIG_CAP, 30, 40) == 0);
  for (int n = 0; n < 3; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, 128, 128, n);
    SFrameBSInfo info;
    TASK_CHECK (Encode (&k, img, n * 33, info) == 0);
  }
  for (int n = 3; n < 5; ++n) { // failed attempts do not count
    std::vector<unsigned char> img;
    MakeTexture (img, 128, 128, n);
    TASK_CHECK (SetBudget (k.e, 1, 42, 48) == 0);
    SFrameBSInfo info;
    TASK_CHECK (Encode (&k, img, n * 33, info) == cmFrameBudgetExceeded);
    TASK_CHECK (SetBudget (k.e, BIG_CAP, 30, 40) == 0);
  }
  std::vector<unsigned char> img5;
  MakeTexture (img5, 128, 128, 5);
  SFrameBSInfo i5;
  TASK_CHECK (Encode (&k, img5, 5 * 33, i5) == 0);

  SEncoderStatistics s1;
  std::memset (&s1, 0, sizeof (s1));
  TASK_CHECK (k.e->GetOption (ENCODER_OPTION_GET_STATISTICS, &s1) == cmResultSuccess);
  TASK_CHECK (s1.uiInputFrameCount == s0.uiInputFrameCount + 4u);

  // Clearing the budget returns to normal RC; those frames count as well.
  TASK_CHECK (SetBudget (k.e, 0, 0, 0) == 0);
  for (int n = 6; n < 8; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, 128, 128, n);
    SFrameBSInfo info;
    TASK_CHECK (Encode (&k, img, n * 33, info) == 0);
    TASK_CHECK (info.iFrameSizeInBytes > 0);
  }
  SEncoderStatistics s2;
  std::memset (&s2, 0, sizeof (s2));
  TASK_CHECK (k.e->GetOption (ENCODER_OPTION_GET_STATISTICS, &s2) == cmResultSuccess);
  TASK_CHECK (s2.uiInputFrameCount == s0.uiInputFrameCount + 6u);
  KitFree (&k);
  return 0;
}

/* ---------------------------- reference group --------------------------- */

static int case_temporal_two_periods (void) {
  const int W = 128, H = 128, Q = 30, N = 20; // tl=3 -> period 8; >2 periods
  Kit A, B;
  TASK_CHECK (KitInit (&A, W, H, 1, 3, 4) == 0);
  TASK_CHECK (KitInit (&B, W, H, 1, 3, 4) == 0);
  TASK_CHECK (SetBudget (B.e, BIG_CAP, Q, Q) == 0);

  int committed = 0;
  for (int n = 0; n < N; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, W, H, n);
    long long ts = n * 33;
    bool disturb = (n % 4 == 2);
    if (disturb) {
      TASK_CHECK (SetBudget (A.e, 1, 42, 50) == 0);
      SFrameBSInfo bad;
      TASK_CHECK (Encode (&A, img, ts, bad) == cmFrameBudgetExceeded);
    }
    TASK_CHECK (SetBudget (A.e, BIG_CAP, Q, Q) == 0);

    SFrameBSInfo ia, ib;
    TASK_CHECK (Encode (&A, img, ts, ia) == 0);
    TASK_CHECK (Encode (&B, img, ts, ib) == 0);
    std::vector<unsigned char> aa = ToAu (ia), ab = ToAu (ib);
    TASK_CHECK (aa == ab);
    uint64_t ha = 0, hb = 0;
    TASK_CHECK (DecodeHash (&A, aa, ha) == 0);
    TASK_CHECK (DecodeHash (&B, ab, hb) == 0);
    TASK_CHECK (ha == hb);
    ++committed;
  }
  TASK_CHECK (committed == N);
  KitFree (&A);
  KitFree (&B);
  return 0;
}

static int case_clear_budget_normal_rc (void) {
  Kit k;
  TASK_CHECK (KitInit (&k, 128, 128, 0, 2, 2) == 0);
  TASK_CHECK (SetBudget (k.e, 200000, 30, 42) == 0);
  for (int n = 0; n < 5; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, 128, 128, n);
    SFrameBSInfo info;
    TASK_CHECK (Encode (&k, img, n * 33, info) == cmResultSuccess);
    TASK_CHECK (info.iFrameSizeInBytes <= 200000);
    uint64_t h = 0;
    TASK_CHECK (DecodeHash (&k, ToAu (info), h) == 0); // feeds IDR/parameter sets
  }

  TASK_CHECK (SetBudget (k.e, 0, 0, 0) == 0);
  for (int n = 5; n < 9; ++n) {
    std::vector<unsigned char> img;
    MakeTexture (img, 128, 128, n);
    SFrameBSInfo info;
    TASK_CHECK (Encode (&k, img, n * 33, info) == cmResultSuccess);
    TASK_CHECK (info.iFrameSizeInBytes > 0);
    uint64_t h = 0;
    TASK_CHECK (DecodeHash (&k, ToAu (info), h) == 0);
  }
  KitFree (&k);
  return 0;
}

} // namespace

// task_support/driver.c is built as C++ by run.sh (clang++ treats the .c input
// as C++), so these definitions keep the ordinary C++ linkage matching check.h.
const struct task_case task_cases[] = {
    {"budget", "option_validation", case_option_validation},
    {"budget", "boundary_cavlc_single", case_budget_boundary_cavlc},
    {"budget", "boundary_cabac_fixedslice", case_budget_boundary_cabac_slice},
    {"budget", "qp_retry_selection", case_qp_retry_selection},
    {"budget", "unsupported_change_rejected", case_unsupported_change_rejected},
    {"budget", "entropy_multislice_decode", case_entropy_multislice_decode},

    {"rollback", "fail_retry_same_compare", case_fail_retry_same},
    {"rollback", "fail_change_image_compare", case_fail_change_image},
    {"rollback", "force_idr_pending_survives", case_force_idr_pending},
    {"rollback", "statistics_counters", case_statistics_counters},

    {"reference", "temporal_two_periods_compare", case_temporal_two_periods},
    {"reference", "clear_budget_normal_rc", case_clear_budget_normal_rc},
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
