#pragma once

// #define USE_CAL3D_WITH_CPP_11 1

#if defined(USE_CAL3D_WITH_CPP_11)

#include <memory>
#include <optional>
#include <algorithm>

#define ISTD std
#define IMVU_STATIC_ASSERT static_assert

#else

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/optional.hpp>
#include <boost/static_assert.hpp>

#define ISTD boost
#define IMVU_STATIC_ASSERT BOOST_STATIC_ASSERT

#endif
