/* Copyright (c) 2024 BSplineLib contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

// Compiled anchor translation unit of the periodic axis module.  All
// production logic is header-inline so that the existing template-heavy
// parameter space and spline translation units can instantiate it without
// requiring an additional link entry; this file is still compiled and
// linked by the task harness (see task_support/run.sh).

#include "BSplineLib/ParameterSpaces/periodic_axis.hpp"

// All periodic axis logic is header-inline (see periodic_axis.hpp) so that
// the templated parameter space and spline translation units can instantiate
// it without an extra link entry.  This file remains the compiled anchor
// translation unit of the module, as required by the task harness; including
// the header here also catches header-only compile regressions.

namespace bsplinelib::parameter_spaces {
namespace {

// Out-of-line, externally invisible anchor so this translation unit emits an
// object of its own without carrying production symbols elsewhere.
struct PeriodicAxisModuleAnchor {
  virtual ~PeriodicAxisModuleAnchor() = default;
};

} // namespace
} // namespace bsplinelib::parameter_spaces
