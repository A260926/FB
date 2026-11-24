/*!
 * \file frame_budget_demo.cpp
 *
 * \brief ENCODER_OPTION_FRAME_BUDGET walk-through.
 *
 * The demo encodes a deterministic moving I420 texture into a fixed-capacity
 * Annex-B sink. The first frame is given an impossibly small byte budget and
 * every fixed-QP candidate is rejected (cmFrameBudgetExceeded, zero output,
 * encoder state untouched). The budget is then raised and the very same
 * picture is encoded successfully, decoded and checked. A few more frames are
 * encoded and decoded, after which the budget is cleared and the encoder goes
 * back to its ordinary rate control.
 */

#include "codec/api/wels/codec_api.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define DEMO_CHECK(x)                                                                                                  \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      std::fprintf (stderr, "frame_budget_demo FAILED at line %d: %s\n", __LINE__, #x);                                \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)

static const int W = 128;
static const int H = 128;

static void MakeTexture (std::vector<unsigned char>& img, int n) {
  img.resize (W * H * 3 / 2);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      img[y * W + x] = static_cast<unsigned char> ((x * 3 + y * 7 + n * 13 + ((x + y) % 17) * n) & 255);
  for (size_t k = static_cast<size_t> (W) * H; k < img.size(); ++k)
    img[k] = static_cast<unsigned char> ((k * 11 + n * 19 + (k / W) * (n + 5)) & 255);
}

static int EncodeAu (ISVCEncoder* e, const std::vector<unsigned char>& img, long long ts, SFrameBSInfo& info) {
  SSourcePicture src;
  std::memset (&src, 0, sizeof (src));
  src.iColorFormat = videoFormatI420;
  src.iPicWidth = W;
  src.iPicHeight = H;
  src.uiTimeStamp = ts;
  src.iStride[0] = W;
  src.iStride[1] = src.iStride[2] = W / 2;
  src.pData[0] = const_cast<unsigned char*> (img.data());
  src.pData[1] = const_cast<unsigned char*> (img.data() + W * H);
  src.pData[2] = const_cast<unsigned char*> (img.data() + W * H * 5 / 4);
  std::memset (&info, 0, sizeof (info));
  return e->EncodeFrame (&src, &info);
}

static std::vector<unsigned char> BuildAnnexB (SFrameBSInfo& info) {
  std::vector<unsigned char> au;
  for (int l = 0; l < info.iLayerNum; ++l) {
    int len = 0;
    for (int n = 0; n < info.sLayerInfo[l].iNalCount; ++n)
      len += info.sLayerInfo[l].pNalLengthInByte[n];
    au.insert (au.end(), info.sLayerInfo[l].pBsBuf, info.sLayerInfo[l].pBsBuf + len);
  }
  return au;
}

int main() {
  ISVCEncoder* enc = NULL;
  ISVCDecoder* dec = NULL;
  DEMO_CHECK (WelsCreateSVCEncoder (&enc) == 0 && enc != NULL);
  DEMO_CHECK (WelsCreateDecoder (&dec) == 0 && dec != NULL);

  int trace = WELS_LOG_ERROR;
  enc->SetOption (ENCODER_OPTION_TRACE_LEVEL, &trace);
  dec->SetOption (DECODER_OPTION_TRACE_LEVEL, &trace);

  SEncParamExt p;
  DEMO_CHECK (enc->GetDefaultParams (&p) == 0);
  p.iUsageType = CAMERA_VIDEO_REAL_TIME;
  p.iPicWidth = W;
  p.iPicHeight = H;
  p.iTargetBitrate = 300000;
  p.iRCMode = RC_BITRATE_MODE;
  p.fMaxFrameRate = 30.0f;
  p.iSpatialLayerNum = 1;
  p.iTemporalLayerNum = 3;
  p.iMultipleThreadIdc = 1;
  p.bEnableFrameSkip = false;
  p.iPaddingFlag = 0;
  p.bEnableLongTermReference = false;
  p.bEnableDenoise = false;
  p.bEnableBackgroundDetection = false;
  p.bEnableSceneChangeDetect = false;
  p.bEnableAdaptiveQuant = false;
  p.eSpsPpsIdStrategy = CONSTANT_ID;
  p.iEntropyCodingModeFlag = 1;
  p.uiIntraPeriod = 0;
  p.sSpatialLayers[0].iVideoWidth = W;
  p.sSpatialLayers[0].iVideoHeight = H;
  p.sSpatialLayers[0].fFrameRate = 30.0f;
  p.sSpatialLayers[0].iSpatialBitrate = 300000;
  p.sSpatialLayers[0].uiProfileIdc = PRO_HIGH;
  p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
  p.sSpatialLayers[0].sSliceArgument.uiSliceNum = 2;
  DEMO_CHECK (enc->InitializeExt (&p) == 0);

  SDecodingParam dp;
  std::memset (&dp, 0, sizeof (dp));
  dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  dp.eEcActiveIdc = ERROR_CON_DISABLE;
  DEMO_CHECK (dec->Initialize (&dp) == 0);

  auto decode = [&] (const std::vector<unsigned char>& au) -> int {
    unsigned char* planes[3] = {NULL, NULL, NULL};
    SBufferInfo bi;
    std::memset (&bi, 0, sizeof (bi));
    int rc = dec->DecodeFrameNoDelay (au.data(), static_cast<int> (au.size()), planes, &bi);
    if (bi.iBufferStatus == 0)
      dec->FlushFrame (planes, &bi);
    return (rc == dsErrorFree && bi.iBufferStatus == 1) ? 0 : 1;
  };

  std::vector<unsigned char> img;
  MakeTexture (img, 0);

  // 1) Impossible budget: all high-QP candidates are real encodes but none fit.
  SFrameBudget tight;
  tight.iMaxBytes = 1;
  tight.iFirstQp = 45;
  tight.iLastQp = 51;
  DEMO_CHECK (enc->SetOption (ENCODER_OPTION_FRAME_BUDGET, &tight) == 0);

  SFrameBSInfo bad;
  int rv = EncodeAu (enc, img, 0, bad);
  std::printf ("tight budget -> EncodeFrame returned %d (cmFrameBudgetExceeded=%d)\n", rv,
               static_cast<int> (cmFrameBudgetExceeded));
  DEMO_CHECK (rv == cmFrameBudgetExceeded);
  DEMO_CHECK (bad.iLayerNum == 0 && bad.iFrameSizeInBytes == 0);

  // 2) Raise the budget and retry the very same picture: it must now succeed.
  SFrameBudget fit;
  fit.iMaxBytes = 60000;
  fit.iFirstQp = 20;
  fit.iLastQp = 44;
  DEMO_CHECK (enc->SetOption (ENCODER_OPTION_FRAME_BUDGET, &fit) == 0);

  SFrameBSInfo ok;
  DEMO_CHECK (EncodeAu (enc, img, 0, ok) == cmResultSuccess);
  DEMO_CHECK (ok.iFrameSizeInBytes > 0 && ok.iFrameSizeInBytes <= fit.iMaxBytes);
  std::vector<unsigned char> au0 = BuildAnnexB (ok);
  DEMO_CHECK (static_cast<int> (au0.size()) == ok.iFrameSizeInBytes);
  DEMO_CHECK (decode (au0) == 0);
  std::printf ("retried same picture fits: %d Annex-B bytes (cap %d), decoded OK\n", ok.iFrameSizeInBytes,
               fit.iMaxBytes);

  // 3) Continue with a short moving sequence under the same budget.
  int decoded = 1;
  for (int n = 1; n < 12; ++n) {
    MakeTexture (img, n);
    SFrameBSInfo fi;
    DEMO_CHECK (EncodeAu (enc, img, static_cast<long long> (n) * 33, fi) == cmResultSuccess);
    DEMO_CHECK (fi.iFrameSizeInBytes <= fit.iMaxBytes);
    std::vector<unsigned char> au = BuildAnnexB (fi);
    DEMO_CHECK (decode (au) == 0);
    ++decoded;
  }

  // 4) Clear the budget: ordinary rate control takes over again.
  SFrameBudget off;
  off.iMaxBytes = 0;
  off.iFirstQp = 0;
  off.iLastQp = 0;
  DEMO_CHECK (enc->SetOption (ENCODER_OPTION_FRAME_BUDGET, &off) == 0);
  MakeTexture (img, 12);
  SFrameBSInfo normal;
  DEMO_CHECK (EncodeAu (enc, img, 12 * 33, normal) == cmResultSuccess);
  DEMO_CHECK (normal.iFrameSizeInBytes > 0);
  DEMO_CHECK (decode (BuildAnnexB (normal)) == 0);
  ++decoded;

  std::printf ("FRAME_BUDGET_DEMO_COMPLETE decoded=%d final_normal_au=%d bytes\n", decoded,
               normal.iFrameSizeInBytes);

  dec->Uninitialize();
  WelsDestroyDecoder (dec);
  enc->Uninitialize();
  WelsDestroySVCEncoder (enc);
  return 0;
}
