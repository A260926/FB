#include <blend2d/blend2d.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
static void check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
int main(int argc, char** argv) {
 try {
  const bool legacy = argc == 2 && std::strcmp(argv[1], "--legacy") == 0;
  check(argc == 1 || legacy, "usage: baseline [--legacy]");
  BLImage image(16,16,BL_FORMAT_PRGB32),mask(3,2,BL_FORMAT_A8);
  BLImageData md{};check(mask.make_mutable(&md)==BL_SUCCESS,"mask storage");
  for(int y=0;y<2;++y)for(int x=0;x<3;++x)
    static_cast<uint8_t*>(md.pixel_data)[y*md.stride+x]=uint8_t(50+x*50);
  BLContext c(image);
  check(c.set_comp_op(BL_COMP_OP_SRC_COPY)==BL_SUCCESS,"comp op");
  check(c.fill_all(BLRgba32(0))==BL_SUCCESS,"clear");
  check(c.set_fill_style(BLRgba32(0xffff0000))==BL_SUCCESS,"style");
  check(c.fill_mask(BLPointI(2,2),mask)==BL_SUCCESS,"aligned mask");
  check(c.flush(BL_CONTEXT_FLUSH_SYNC)==BL_SUCCESS,"flush");
  BLImageData d{};check(image.get_data(&d)==BL_SUCCESS,"pixels");
  auto* row=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d.pixel_data)+2*d.stride);
  check(row[2]==0x32320000 && row[4]==0x96960000,"legacy aligned pixels");
  if (legacy) {
    check(c.end()==BL_SUCCESS,"end");
    std::puts("BLEND2D_LEGACY_ALIGNED_MASK_OK");
  } else {
    BLResult fractional=c.fill_mask(BLPoint(6.25,2.5),mask);
    check(fractional==BL_ERROR_NOT_IMPLEMENTED,"starting-point fractional witness");
    check(c.end()==BL_SUCCESS,"end");
    std::printf("BLEND2D_ALIGNED_MASK_OK fractional_unimplemented=%u\n",unsigned(fractional));
  }
  return 0;
 } catch(const std::exception& e) {
   std::fprintf(stderr,"FAIL %s\n",e.what()); return 1;
 }
}
