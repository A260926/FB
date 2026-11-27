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

#ifndef SOURCES_PARAMETERSPACES_SHARED_REFINEMENT_HPP_
#define SOURCES_PARAMETERSPACES_SHARED_REFINEMENT_HPP_

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#include "BSplineLib/Utilities/containers.hpp"
#include "BSplineLib/Utilities/named_type.hpp"

namespace bsplinelib::vector_spaces {
class VectorSpace;
}

namespace bsplinelib::parameter_spaces {

// SharedRefinementGroups form behind a ParameterSpace for which shared
// refinement has been enabled (see ParameterSpace<para_dim>::
// EnableSharedRefinement).  The group applies the geometric part of knot
// insertion, degree elevation, and - only internally, while degree elevation
// restores the original continuities - knot removal to every subscribed
// control net exactly once, so that all members of the group always match the
// shared knots and degrees.  Membership handles are owned by the subscribing
// spline wrappers: a control net is refined as long as at least one wrapper
// referencing it is alive, and it is never refined more than once per edit,
// no matter how many wrappers reference it.
class SharedRefinementGroup {
public:
  using VectorSpace_ = vector_spaces::VectorSpace;
  using KnotRatios_ = Vector<Vector<KnotRatio>>;
  // Matches both ParameterSpace<para_dim>::InsertionInformation_ and
  // ParameterSpace<para_dim>::ElevationInformation_ for any para_dim.
  using EditInformation_ = Tuple<Index, KnotRatios_>;

  // Geometric update routines supplied by the subscribing spline.  The
  // Length pointer addresses the para_dim numbers of basis functions as they
  // were before the shared parameter space was updated.
  using InsertionRoutine_ = void (*)(VectorSpace_&,
                                     Dimension const&,
                                     Length const*,
                                     EditInformation_ const&);
  using ElevationRoutine_ = void (*)(VectorSpace_&,
                                     Dimension const&,
                                     Length const*,
                                     int,
                                     Multiplicity const&,
                                     EditInformation_ const&);
  using RemovalRoutine_ = Multiplicity (*)(VectorSpace_&,
                                           Dimension const&,
                                           Length const*,
                                           EditInformation_ const&,
                                           Tolerance const&);

  // A single subscription.  Owned by the subscribing spline wrapper; the
  // group itself only keeps a weak reference.
  struct Member_ {
    std::weak_ptr<VectorSpace_> vector_space_;
    InsertionRoutine_ insert_knot_;
    ElevationRoutine_ elevate_degree_;
    RemovalRoutine_ remove_knot_;
  };

  SharedRefinementGroup() = default;

  // Subscribes a control net.  The net leaves the group as soon as the last
  // wrapper holding the returned membership handle is destroyed.
  SharedPointer<Member_> Subscribe(std::weak_ptr<VectorSpace_> vector_space,
                                   InsertionRoutine_ insert_knot,
                                   ElevationRoutine_ elevate_degree,
                                   RemovalRoutine_ remove_knot);

  // Applies the geometric part of an edit to every subscribed control net
  // exactly once, skipping the (already updated) net behind skip.
  void ApplyInsertion(Dimension const& dimension,
                      Length const* number_of_coordinates,
                      EditInformation_ const& insertion_information,
                      VectorSpace_ const* skip);
  void ApplyElevation(Dimension const& dimension,
                      Length const* number_of_coordinates,
                      int number_of_segments,
                      Multiplicity const& multiplicity,
                      EditInformation_ const& elevation_information,
                      VectorSpace_ const* skip);
  void ApplyRemoval(Dimension const& dimension,
                    Length const* number_of_coordinates,
                    EditInformation_ const& removal_information,
                    Tolerance const& tolerance_removal,
                    VectorSpace_ const* skip);

  bool IsKnotRemovalPermitted() const { return knot_removal_permitted_; }

  // Permits knot removal while degree elevation restores the original
  // continuities; removal is forbidden again as soon as the permit dies.
  class RemovalPermit_ {
  public:
    explicit RemovalPermit_(SharedRefinementGroup& group) : group_{group} {
      group_.knot_removal_permitted_ = true;
    }
    RemovalPermit_(RemovalPermit_ const&) = delete;
    RemovalPermit_(RemovalPermit_&&) = delete;
    RemovalPermit_& operator=(RemovalPermit_ const&) = delete;
    RemovalPermit_& operator=(RemovalPermit_&&) = delete;
    ~RemovalPermit_() { group_.knot_removal_permitted_ = false; }

  private:
    SharedRefinementGroup& group_;
  };

private:
  template<typename Routine>
  void ForEachUniqueMember(VectorSpace_ const* const skip, Routine&& routine) {
    std::erase_if(members_, [](std::weak_ptr<Member_> const& member) {
      return member.expired();
    });
    Vector<VectorSpace_ const*> refined;
    if (skip != nullptr) refined.push_back(skip);
    for (std::weak_ptr<Member_> const& weak_member : members_) {
      SharedPointer<Member_> const member{weak_member.lock()};
      if (!member) continue;
      SharedPointer<VectorSpace_> vector_space{member->vector_space_.lock()};
      if (!vector_space) continue;
      VectorSpace_ const* const address{vector_space.get()};
      if (std::find(refined.begin(), refined.end(), address)
          != refined.end())
        continue;
      refined.push_back(address);
      routine(*member, *vector_space);
    }
  }

  Vector<std::weak_ptr<Member_>> members_;
  bool knot_removal_permitted_{};
};

} // namespace bsplinelib::parameter_spaces

#endif // SOURCES_PARAMETERSPACES_SHARED_REFINEMENT_HPP_
