#pragma once
#include <cstddef>
#include <utility>
#ifndef SOFT_RASTERIZER_USE_TBB
#define SOFT_RASTERIZER_USE_TBB 0
#endif
#if SOFT_RASTERIZER_USE_TBB
#include <tbb/parallel_for.h>
#endif
namespace SoftRasterizer::Parallel {
template <class Index, class Body>
void parallelFor(Index begin, Index end, Body &&body) {
#if SOFT_RASTERIZER_USE_TBB
  tbb::parallel_for(begin, end, std::forward<Body>(body));
#else
  for (Index i = begin; i < end; ++i) {
    body(i);
  }
#endif
}
} // namespace SoftRasterizer::Parallel
