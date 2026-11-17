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

#ifndef SOURCES_SPLINES_B_SPLINE_HPP_
#define SOURCES_SPLINES_B_SPLINE_HPP_

#include <algorithm>
#include <iostream>
#include <iterator>
#include <utility>

#include "BSplineLib/Splines/spline.hpp"
#include "BSplineLib/Splines/shared_refinement.hpp"
#include "BSplineLib/Utilities/containers.hpp"
#include "BSplineLib/Utilities/error_handling.hpp"
#include "BSplineLib/Utilities/index.hpp"
#include "BSplineLib/Utilities/named_type.hpp"
#include "BSplineLib/Utilities/numeric_operations.hpp"
#include "BSplineLib/VectorSpaces/vector_space.hpp"

namespace bsplinelib::splines {

template<int para_dim>
class BSpline;

// B-splines are non-rational splines.  Currently only single-patch B-splines
// are supported.
//
// Example (see, e.g., NURBS book Exe. 3.8):
//   using Surface = BSpline<2, 3>;
//   Surface::Coordinate_ const &coordinate0 = surface({});  // Evaluating the
//   B-spline S(0.0, 0.0) results in P_{0,0}. Multiplicity const
//   &successful_removals = surface.RemoveKnot(Dimension{},
//   Surface::Knot_{0.5}); bool const &successful =
//   surface.ReduceDegree(Dimension{1}, kEpsilon);  // True if spline's degree
//   p_0 be reduced.
template<int para_dim>
class BSpline : public Spline<para_dim> {
public:
  using Base_ = Spline<para_dim>;
  using Coordinate_ = typename Base_::Coordinate_;
  using Derivative_ = typename Base_::Derivative_;
  using Knot_ = typename Base_::Knot_;
  using ParameterSpace_ = typename Base_::ParameterSpace_;
  using ParametricCoordinate_ = typename Base_::ParametricCoordinate_;
  using VectorSpace_ = typename Base_::VectorSpace_;
  using OutputInformation_ = Tuple<typename ParameterSpace_::OutputInformation_,
                                   typename VectorSpace_::OutputInformation_>;

  using Type_ = typename ParameterSpace_::Type_;
  using IntType_ = typename ParameterSpace_::IntType_;

  BSpline();
  BSpline(SharedPointer<ParameterSpace_> parameter_space,
          SharedPointer<VectorSpace_> vector_space);
  BSpline(BSpline const& other);
  BSpline(BSpline&& other) noexcept;
  BSpline& operator=(BSpline const& rhs);
  BSpline& operator=(BSpline&& rhs) noexcept;
  ~BSpline() override = default;

  virtual int Dim() const override { return vector_space_->Dim(); };

  void Evaluate(const Type_* parametric_coordinate, Type_* evaluated) const;
  void EvaluateDerivative(const Type_* parametric_coordinate,
                          const IntType_* derivative,
                          Type_* evaluated) const;

  /// @brief returning evaluate. kept for backward compatibility
  /// @param parametric_coordinate
  /// @param tolerance
  /// @return
  Coordinate_ operator()(const Type_* parametric_coordinate) const override;

  /// @brief returning evaluate. kept for backward compatibility
  /// @param parametric_coordinate
  /// @param tolerance
  /// @return
  Coordinate_ operator()(const Type_* parametric_coordinate,
                         const IntType_* derivative) const override;

  void InsertKnot(Dimension const& dimension,
                  Knot_ knot,
                  Multiplicity const& multiplicity = kMultiplicity,
                  Tolerance const& tolerance = kEpsilon) const override;
  // Tries to interpret knot removal as the inverse process of knot insertion.
  Multiplicity RemoveKnot(Dimension const& dimension,
                          Knot_ const& knot,
                          Tolerance const& tolerance_removal,
                          Multiplicity const& multiplicity = kMultiplicity,
                          Tolerance const& tolerance = kEpsilon) const override;
  void ElevateDegree(Dimension const& dimension,
                     Multiplicity const& multiplicity = kMultiplicity,
                     Tolerance const& tolerance = kEpsilon) const override;
  // Tries to interpret degree reduction as the inverse process of degree
  // elevation.
  bool ReduceDegree(Dimension const& dimension,
                    Tolerance const& tolerance_reduction,
                    Multiplicity const& multiplicity = kMultiplicity,
                    Tolerance const& tolerance = kEpsilon) const override;

  Coordinate ComputeUpperBoundForMaximumDistanceFromOrigin() const override;
  OutputInformation_ Write(Precision const& precision = kPrecision) const;

  /// @brief
  /// @param from
  /// @param to
  virtual void ShareMembers(std::shared_ptr<BSpline>& from) {
    LeaveSharedRefinementGroup();
    vector_space_ = from->vector_space_;
    Base_::parameter_space_ = from->parameter_space_;
    JoinSharedRefinementGroup();
  };

protected:
  SharedPointer<VectorSpace_> vector_space_;

private:
  using BezierInformation_ = typename ParameterSpace_::BezierInformation_;
  using BinomialRatios_ = typename ParameterSpace_::BinomialRatios_;
  using Index_ = typename Base_::Index_;
  using IndexLength_ = typename Index_::Length_;
  using IndexValue_ = typename Index_::Value_;
  using KnotRatios_ = typename ParameterSpace_::KnotRatios_;
  using Knots_ = typename Base_::Knots_;
  using BinomialRatio_ = typename BinomialRatios_::value_type;
  using KnotRatio_ = typename KnotRatios_::value_type;
  using Registry_ = SharedRefinementRegistry;

  BezierInformation_ MakeBezier(Dimension const& dimension,
                                Tolerance const& tolerance = kEpsilon) const;

  // Shape-preserving edits operating on this spline only.  The public
  // InsertKnot/ElevateDegree wrappers dispatch these to the whole refinement
  // group when shared refinement is enabled; the knot re-insertion and knot
  // removal performed inside degree elevation (see MakeBezier) call the
  // public wrappers while the reentrancy guard is active.
  void InsertKnotImpl(Dimension const& dimension,
                      Knot_ knot,
                      Multiplicity const& multiplicity,
                      Tolerance const& tolerance) const;
  Multiplicity RemoveKnotImpl(Dimension const& dimension,
                              Knot_ const& knot,
                              Tolerance const& tolerance_removal,
                              Multiplicity const& multiplicity,
                              Tolerance const& tolerance) const;
  void ElevateDegreeImpl(Dimension const& dimension,
                         Multiplicity const& multiplicity,
                         Tolerance const& tolerance) const;
  bool ReduceDegreeImpl(Dimension const& dimension,
                        Tolerance const& tolerance_reduction,
                        Multiplicity const& multiplicity,
                        Tolerance const& tolerance) const;

  // Refinement group membership.  The observer token unregisters this wrapper
  // on destruction; it is not copied by the copy constructor, since copies get
  // an independent parameter space with shared refinement disabled.
  void JoinSharedRefinementGroup();
  void LeaveSharedRefinementGroup();
  void ApplySharedEdit(Registry_::Edit const& edit) const;
  void ApplyEditGuarded(Registry_::Edit const& edit) const;
  void DispatchEditLocally(Registry_::EditKind kind,
                           Dimension const& dimension,
                           Knot_ knot,
                           Multiplicity const& multiplicity,
                           Tolerance const& tolerance) const;

  std::shared_ptr<void> shared_refinement_token_{};
  // True while this wrapper is executing an edit of its own group, so the
  // auxiliary InsertKnot/RemoveKnot calls of ElevateDegree stay local instead
  // of re-entering the dispatch.
  mutable bool shared_refinement_active_{false};
};

#include "BSplineLib/Splines/b_spline.inl"

} // namespace bsplinelib::splines

#endif // SOURCES_SPLINES_B_SPLINE_HPP_
