/* Copyright (c) 2018–2021 SplineLib

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

#ifndef SOURCES_SPLINES_SHARED_REFINEMENT_HPP_
#define SOURCES_SPLINES_SHARED_REFINEMENT_HPP_

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "BSplineLib/Utilities/containers.hpp"
#include "BSplineLib/VectorSpaces/vector_space.hpp"

namespace bsplinelib::splines {

// SharedRefinementRegistry coordinates shape-preserving joint knot insertion
// and degree elevation of all splines that have been constructed from one and
// the same ParameterSpace after
// ParameterSpace::EnableSharedRefinement() has been called.
//
// The registry never owns the splines themselves: every spline keeps a weak
// reference to the group together with an observer token, so destroying the
// last wrapper (even while the caller still holds the control net) makes it
// leave the group automatically.  Every distinct control net (VectorSpace or
// WeightedVectorSpace, the latter also being the homogeneous net of the
// enclosing Nurbs) is refined exactly once per edit because the registry
// deduplicates by the net's address.
class SharedRefinementRegistry
    : public std::enable_shared_from_this<SharedRefinementRegistry> {
public:
  // Editing entry points exposed by every participating spline.  The kind of
  // edit is identified by an integer so that BSpline and Nurbs can register
  // the same callback signature.
  enum class EditKind {
    kInsertKnot = 0,
    kElevateDegree = 1,
  };

  struct Edit {
    EditKind kind;
    int dimension;
    double knot;
    int multiplicity;
    double tolerance;
    // Copy of the parameter space as it was before the edit.  Every non-
    // initiating member temporarily swaps it in while replaying the edit, so
    // the existing shape-preserving algorithms run unchanged against the
    // original knot vectors and degrees while the one shared parameter space
    // is only updated by the initiating member.
    std::shared_ptr<void> pre_edit_parameter_space;
  };

  using Callback = std::function<void(Edit const&)>;

  SharedRefinementRegistry();
  ~SharedRefinementRegistry();
  SharedRefinementRegistry(SharedRefinementRegistry const&) = delete;
  SharedRefinementRegistry&
  operator=(SharedRefinementRegistry const&) = delete;

  // Registers one control net together with the edit callback of the spline
  // wrapper that owns (or aliases) it.  The returned observer token unregisters
  // the wrapper again when it is destroyed or reset.
  std::shared_ptr<void>
  RegisterNet(SharedPointer<vector_spaces::VectorSpace> net,
              Callback callback);

  // Applies one shape-preserving edit to every currently participating
  // control net.  Splines sharing the same net (aliases created through
  // ShareMembers and the homogeneous BSpline inside a Nurbs) are visited only
  // once.  The initiating spline updates the shared parameter space together
  // with its own net in initiating_callback; every other callback then
  // performs the very same edit against the already updated parameter space.
  void DispatchEdit(Edit const& edit,
                    void* initiating_net,
                    Callback const& initiating_callback);

  // Number of distinct control nets currently taking part in joint edits.
  std::size_t NumberOfNets();

private:
  struct Entry;

  void UnregisterNet(void* key);
  void CleanupExpired();
  void VisitNets(Edit const& edit, void* initiating_net);
  static void* TokenKey(std::weak_ptr<void> const& observer);

  std::mutex mutex_;
  std::vector<Entry> entries_;
};

} // namespace bsplinelib::splines

#endif // SOURCES_SPLINES_SHARED_REFINEMENT_HPP_
