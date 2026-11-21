// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_surface_guides.h"

namespace fj {

SurfaceGuideBuffer::SurfaceGuideBuffer() :
    pixels_(),
    width_(0),
    height_(0)
{
}

SurfaceGuideBuffer::~SurfaceGuideBuffer()
{
}

int SurfaceGuideBuffer::GetWidth() const
{
  return width_;
}

int SurfaceGuideBuffer::GetHeight() const
{
  return height_;
}

bool SurfaceGuideBuffer::IsEmpty() const
{
  return pixels_.empty();
}

SurfaceGuide SurfaceGuideBuffer::GetPixel(int x, int y) const
{
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    return SurfaceGuide();
  }

  return pixels_[static_cast<std::size_t>(y) * width_ + x];
}

void SurfaceGuideBuffer::Reset(int width, int height)
{
  if (width < 0 || height < 0) {
    width = 0;
    height = 0;
  }

  width_ = width;
  height_ = height;
  pixels_.assign(static_cast<std::size_t>(width_) * height_, SurfaceGuide());
}

void SurfaceGuideBuffer::SetPixel(int x, int y, const SurfaceGuide &guide)
{
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    return;
  }

  pixels_[static_cast<std::size_t>(y) * width_ + x] = guide;
}

} // namespace xxx
