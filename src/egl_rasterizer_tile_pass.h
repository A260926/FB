//============================================================================
// EmberGL
//
// Copyright (c) 2022, Jarkko Lempiainen
// All rights reserved.
//============================================================================

#ifndef EGL_RASTERIZER_TILE_PASS_H
#define EGL_RASTERIZER_TILE_PASS_H
//----------------------------------------------------------------------------


//============================================================================
// interface
//============================================================================
// external
#include "egl_rasterizer_target.h"
EGL_NAMESPACE_BEGIN

// new
struct rasterizer_tile_pass_context;
class rasterizer;
//----------------------------------------------------------------------------

// trivially copyable check without pulling in <type_traits> on the MCU builds
#ifndef EGL_IS_TRIVIALLY_COPYABLE
  #if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
    #define EGL_IS_TRIVIALLY_COPYABLE(T) __is_trivially_copyable(T)
  #else
    #include <type_traits>
    #define EGL_IS_TRIVIALLY_COPYABLE(T) ::std::is_trivially_copyable<T>::value
  #endif
#endif
//----------------------------------------------------------------------------


//============================================================================
// rasterizer_tile_pass_context
//============================================================================
// Per-tile context handed to a tile pass callback. All render target and
// depth pointers are already offset to the local origin (pixel 0,0) of the
// tile, so the callback indexes them directly with local coordinates in the
// ranges [0, width) and [0, height). Unconfigured attachments are null. The
// tile pitch is the allocated (full) tile row stride, which may be larger
// than width on the right/bottom edge tiles of an uneven render target.
struct rasterizer_tile_pass_context
{
  // tile geometry in screen coordinates. width/height is the valid (clipped)
  // tile size, pitch the allocated tile row stride in pixels.
  uint16_t x, y;
  uint16_t width, height;
  uint16_t pitch;
  // tile local render targets (null when not configured). pixels are
  // interpreted in the format configured for the attachment.
  void *rts[rasterizer_max_rts];
  uint8_t rt_pixel_sizes[rasterizer_max_rts];
  // tile local depth target. data is null when no depth is configured and
  // only writable from the pass when the pass writes_depth is true.
  rasterizer_depth_target depth;
};
//----------------------------------------------------------------------------


//============================================================================
// rasterizer tile pass dispatch wrapper
//============================================================================
// A tile pass dispatch entry stores a type-erased wrapper in the same
// fixed-size dispatch store used by PSO dispatches. The wrapper refers to the
// copy of the trivially-copyable pass object made in the per-commit frame
// (PSO) store. No callback runs at submission time, only during commit.
struct rasterizer_tile_pass_wrapper_base
{
  virtual void run_tile_pass(rasterizer_tile_pass_context&) const=0;
  virtual bool writes_depth() const=0;
  const void *m_pass;
};
//----

template<class Pass>
struct rasterizer_tile_pass_wrapper: rasterizer_tile_pass_wrapper_base
{
  rasterizer_tile_pass_wrapper(const Pass &pass_)
  {
    m_pass=&pass_;
  }

  virtual void run_tile_pass(rasterizer_tile_pass_context &ctx_) const
  {
    static_cast<const Pass*>(m_pass)->exec(ctx_);
  }

  virtual bool writes_depth() const
  {
    return Pass::writes_depth;
  }
};
//----------------------------------------------------------------------------

//============================================================================
EGL_NAMESPACE_END
#endif
