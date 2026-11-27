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

#include "BSplineLib/ParameterSpaces/shared_refinement.hpp"

#include "BSplineLib/VectorSpaces/vector_space.hpp"

namespace bsplinelib::parameter_spaces {

SharedPointer<SharedRefinementGroup::Member_>
SharedRefinementGroup::Subscribe(std::weak_ptr<VectorSpace_> vector_space,
                                 InsertionRoutine_ insert_knot,
                                 ElevationRoutine_ elevate_degree,
                                 RemovalRoutine_ remove_knot) {
  std::erase_if(members_, [](std::weak_ptr<Member_> const& member) {
    return member.expired();
  });
  SharedPointer<Member_> member = std::make_shared<Member_>(
      Member_{std::move(vector_space),
              insert_knot,
              elevate_degree,
              remove_knot});
  members_.emplace_back(member);
  return member;
}

void SharedRefinementGroup::ApplyInsertion(
    Dimension const& dimension,
    Length const* number_of_coordinates,
    EditInformation_ const& insertion_information,
    VectorSpace_ const* const skip) {
  ForEachUniqueMember(skip,
                      [&](Member_ const& member, VectorSpace_& vector_space) {
                        member.insert_knot_(vector_space,
                                            dimension,
                                            number_of_coordinates,
                                            insertion_information);
                      });
}

void SharedRefinementGroup::ApplyElevation(
    Dimension const& dimension,
    Length const* number_of_coordinates,
    int const number_of_segments,
    Multiplicity const& multiplicity,
    EditInformation_ const& elevation_information,
    VectorSpace_ const* const skip) {
  ForEachUniqueMember(skip,
                      [&](Member_ const& member, VectorSpace_& vector_space) {
                        member.elevate_degree_(vector_space,
                                               dimension,
                                               number_of_coordinates,
                                               number_of_segments,
                                               multiplicity,
                                               elevation_information);
                      });
}

void SharedRefinementGroup::ApplyRemoval(
    Dimension const& dimension,
    Length const* number_of_coordinates,
    EditInformation_ const& removal_information,
    Tolerance const& tolerance_removal,
    VectorSpace_ const* const skip) {
  Multiplicity const removals{
      static_cast<int>(std::get<1>(removal_information).size())};
  ForEachUniqueMember(
      skip,
      [&](Member_ const& member, VectorSpace_& vector_space) {
        if (member.remove_knot_(vector_space,
                                dimension,
                                number_of_coordinates,
                                removal_information,
                                tolerance_removal)
            != removals) {
          throw std::logic_error(
              "BSplineLib::SharedRefinementGroup::ApplyRemoval - A subscribed "
              "control net could not remove the knot; the shared refinement "
              "group would become inconsistent.");
        }
      });
}

} // namespace bsplinelib::parameter_spaces
