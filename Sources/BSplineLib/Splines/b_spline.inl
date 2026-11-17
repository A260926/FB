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

// TODO(all): use NamedInteger::ForEach once clang supports capturing of
// variables from structured bindings.

template<int para_dim>
BSpline<para_dim>::BSpline() : Base_(false) {}

template<int para_dim>
BSpline<para_dim>::BSpline(SharedPointer<ParameterSpace_> parameter_space,
                           SharedPointer<VectorSpace_> vector_space)
    : Base_(std::move(parameter_space), false) {
  using std::to_string;

#ifndef NDEBUG
  int const &total_number_of_basis_functions =
                Base_::parameter_space_->GetTotalNumberOfBasisFunctions(),
            &number_of_coordinates = vector_space->GetNumberOfCoordinates();
  if (number_of_coordinates != total_number_of_basis_functions)
    Throw(DomainError(to_string(number_of_coordinates)
                      + " coordinates were provided but "
                      + to_string(total_number_of_basis_functions)
                      + " are needed to associate each basis function "
                        "with a coordinate."),
          "bsplinelib::splines::BSpline::BSpline");
#endif
  vector_space_ = std::move(vector_space);
  JoinSharedRefinementGroup();
}

template<int para_dim>
BSpline<para_dim>::BSpline(BSpline const& other)
    : Base_(other),
      vector_space_{std::make_shared<VectorSpace_>(*other.vector_space_)} {
  // The copy owns a fresh parameter space with shared refinement disabled, so
  // it must not register with the source's refinement group.
}

template<int para_dim>
BSpline<para_dim>& BSpline<para_dim>::operator=(BSpline const& rhs) {
  LeaveSharedRefinementGroup();
  Base_::operator=(rhs),
  vector_space_ = std::make_shared<VectorSpace_>(*rhs.vector_space_);
  return *this;
}

template<int para_dim>
BSpline<para_dim>::BSpline(BSpline&& other) noexcept
    : Base_(std::move(other)),
      vector_space_(std::move(other.vector_space_)) {
  // The moved-from wrapper's observer still points at its own (now stale)
  // callback: detach it and let the new object take over the participation.
  other.LeaveSharedRefinementGroup();
  JoinSharedRefinementGroup();
}

template<int para_dim>
BSpline<para_dim>&
BSpline<para_dim>::operator=(BSpline&& rhs) noexcept {
  if (this == &rhs) {
    return *this;
  }
  LeaveSharedRefinementGroup();
  Base_::operator=(std::move(rhs));
  vector_space_ = std::move(rhs.vector_space_);
  rhs.LeaveSharedRefinementGroup();
  JoinSharedRefinementGroup();
  return *this;
}

template<int para_dim>
void BSpline<para_dim>::Evaluate(const Type_* parametric_coordinate,
                                 Type_* evaluated) const {

  ParameterSpace_ const& parameter_space = *Base_::parameter_space_;

  Coordinate_ evaluated_b_spline;
  evaluated_b_spline.SetData(evaluated);
  evaluated_b_spline.SetShape(vector_space_->Dim());

  // zero initialization is necessary
  evaluated_b_spline.Fill(0.);

  const auto basis_per_dim =
      parameter_space.EvaluateBasisValuesPerDimension(parametric_coordinate);
  auto beginning =
      parameter_space.FindFirstNonZeroBasisFunction(parametric_coordinate);
  auto offset = parameter_space.First();

  bsplinelib::parameter_spaces::RecursiveCombine(
      basis_per_dim,
      beginning,
      offset,
      vector_space_->GetCoordinates(),
      evaluated_b_spline);
}

template<int para_dim>
void BSpline<para_dim>::EvaluateDerivative(const Type_* parametric_coordinate,
                                           const IntType_* derivative,
                                           Type_* evaluated) const {
  ParameterSpace_ const& parameter_space = *Base_::parameter_space_;
  Coordinate_ evaluated_b_spline_derivative;
  evaluated_b_spline_derivative.SetData(evaluated);
  evaluated_b_spline_derivative.SetShape(vector_space_->Dim());

  // zero initialization is necessary
  evaluated_b_spline_derivative.Fill(0.);

  const auto basis_derivative_per_dim =
      parameter_space.EvaluateBasisDerivativeValuesPerDimension(
          parametric_coordinate,
          derivative);
  auto beginning =
      parameter_space.FindFirstNonZeroBasisFunction(parametric_coordinate);
  auto offset = parameter_space.First();

  bsplinelib::parameter_spaces::RecursiveCombine(
      basis_derivative_per_dim,
      beginning,
      offset,
      vector_space_->GetCoordinates(),
      evaluated_b_spline_derivative);
}

template<int para_dim>
typename Spline<para_dim>::Coordinate_
BSpline<para_dim>::operator()(const Type_* parametric_coordinate) const {
  Coordinate_ evaluated_b_spline(vector_space_->Dim());

  Evaluate(parametric_coordinate, evaluated_b_spline.data());

  return evaluated_b_spline;
}

template<int para_dim>
typename Spline<para_dim>::Coordinate_
BSpline<para_dim>::operator()(const Type_* parametric_coordinate,
                              const IntType_* derivative) const {
  Coordinate_ evaluated_b_spline_derivative(vector_space_->Dim());

  EvaluateDerivative(parametric_coordinate,
                     derivative,
                     evaluated_b_spline_derivative.data());

  return evaluated_b_spline_derivative;
}

// Cf. NURBS book Eq. (5.15).
template<int para_dim>
void BSpline<para_dim>::InsertKnotImpl(Dimension const& dimension,
                                       Knot_ knot,
                                       Multiplicity const& multiplicity,
                                       Tolerance const& tolerance) const {
  using utilities::containers::Add, utilities::containers::Multiply;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;

  // collect values before updating parameter_space
  const int n_original_coord = vector_space.GetNumberOfCoordinates();
  IndexLength_ number_of_coordinates{
      parameter_space.GetNumberOfBasisFunctions()};
  const int n_total_original_coords =
      parameter_space.GetTotalNumberOfBasisFunctions();
  const int n_coords_per_slice =
      n_total_original_coords / number_of_coordinates[dimension];

  auto const& [start_value, coefficients] =
      parameter_space.InsertKnot(dimension, knot, multiplicity, tolerance);
  // TODO(all): use std::for_each once clang supports capturing of variables
  // from structured bindings
  const int n_new_points = coefficients.size() * n_coords_per_slice;
  vector_space.AppendEmptyCoordinates(n_new_points);
  int ignore_from{};
  for (KnotRatios_ const& current_coefficients : coefficients) {
    IndexLength_ number_of_coordinates_in_slice{number_of_coordinates};
    number_of_coordinates_in_slice[dimension] = Length{};
    IndexLength_ const previous_number_of_coordinates{number_of_coordinates};
    ++number_of_coordinates[dimension];
    for (Index_ slice_coordinate{Index_::First(number_of_coordinates_in_slice)};
         slice_coordinate != Index_::Behind(number_of_coordinates_in_slice);
         ++slice_coordinate) {
      constexpr KnotRatio_ const k1_0{1.0};

      IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
      coordinate_value[dimension] = start_value;
      Index_ coordinate{number_of_coordinates, coordinate_value};
      Index const insertion_position = coordinate.GetIndex1d();
      // C^0 to C^-1 insertion, second case does not apply (insert repetition)
      if (current_coefficients.empty()) {
        // we need to pass copy to this
        vector_space.StaticInsert(
            insertion_position,
            vector_space[Index_::GetIndex1d(previous_number_of_coordinates,
                                            coordinate_value)
                         + slice_coordinate.GetIndex1d()]
                .Copy(),
            n_total_original_coords + ignore_from++);
        continue;
      }

      typename KnotRatios_::const_reverse_iterator coefficient{
          current_coefficients.rbegin()};
      vector_space.StaticInsert(
          insertion_position,
          Add(Multiply(vector_space[Index_::GetIndex1d(
                                        previous_number_of_coordinates,
                                        coordinate_value)
                                    + slice_coordinate.GetIndex1d()],
                       *coefficient),
              Multiply(
                  vector_space[coordinate.Decrement(dimension).GetIndex1d()],
                  k1_0 - *coefficient)),
          n_total_original_coords + ignore_from++);
      ++coefficient;
      for (; coefficient < current_coefficients.rend(); ++coefficient) {
        Index const& replacement_position = coordinate.GetIndex1d();
        vector_space.Replace(
            replacement_position,
            Add(Multiply(vector_space[replacement_position], *coefficient),
                Multiply(
                    vector_space[coordinate.Decrement(dimension).GetIndex1d()],
                    k1_0 - *coefficient)));
      }
    }
  }
}

template<int para_dim>
Multiplicity BSpline<para_dim>::RemoveKnotImpl(
    Dimension const& dimension,
    Knot_ const& knot,
    Tolerance const& tolerance_removal,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  using utilities::containers::Divide, utilities::containers::Multiply,
      utilities::containers::Subtract;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;

  ParameterSpace_ parameter_space_backup{parameter_space};
  IndexLength_ number_of_coordinates{
      parameter_space.GetNumberOfBasisFunctions()};
  auto const& [start_value, coefficients] =
      parameter_space.RemoveKnot(dimension, knot, multiplicity, tolerance);
  Multiplicity const& removals = coefficients.size();
  for (Multiplicity removal{removals}; removal > Multiplicity{}; --removal) {
    VectorSpace_& vector_space = *vector_space_;
    VectorSpace_ const vector_space_backup{vector_space};
    KnotRatios_ const& current_coefficients = coefficients[removal - 1];
    IndexLength_ number_of_coordinates_in_slice{number_of_coordinates};
    number_of_coordinates_in_slice[dimension] = Length{};
    IndexLength_ const previous_number_of_coordinates{number_of_coordinates};
    --number_of_coordinates[dimension];
    for (Index_ slice_coordinate{Index_::Last(number_of_coordinates_in_slice)};
         slice_coordinate != Index_::Before(number_of_coordinates_in_slice);
         --slice_coordinate) {
      constexpr KnotRatio_ const k1_0{1.0};

      IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
      coordinate_value[dimension] =
          (start_value
           - Index{static_cast<Index>(current_coefficients.size())});
      Index_ coordinate{previous_number_of_coordinates, coordinate_value};
      Index coordinate_index{coordinate.GetIndex1d()}, lower_coordinate_index;
      typename KnotRatios_::const_iterator coefficient{
          current_coefficients.begin()};
      for (; coefficient != std::prev(current_coefficients.end());
           ++coefficient) {
        KnotRatio_ const& current_coefficient = *coefficient;
        lower_coordinate_index = coordinate_index;
        coordinate_index = coordinate.Increment(dimension).GetIndex1d();
        vector_space.Replace(
            coordinate_index,
            Divide(Subtract(vector_space[coordinate_index],
                            Multiply(vector_space[lower_coordinate_index],
                                     k1_0 - current_coefficient)),
                   current_coefficient));
      }
      KnotRatio_ const& current_coefficient = *coefficient;
      lower_coordinate_index = coordinate_index;
      coordinate_index = coordinate.Increment(dimension).GetIndex1d();
      if ( // IsLessOrEqual(
          utilities::containers::EuclidianDistance(
              Divide(Subtract(vector_space[coordinate_index],
                              Multiply(vector_space[lower_coordinate_index],
                                       k1_0 - current_coefficient)),
                     current_coefficient),
              vector_space[Index_{number_of_coordinates, coordinate.GetIndex()}
                               .GetIndex1d()
                           + slice_coordinate.GetIndex1d() + Index{1}])
          <= tolerance_removal) {
        vector_space.Erase(coordinate_index);
      } else {
        Multiplicity const& successful_removals = (multiplicity - removal);
        parameter_space_backup.RemoveKnot(dimension,
                                          knot,
                                          successful_removals,
                                          tolerance);
        parameter_space = parameter_space_backup;
        vector_space = vector_space_backup;
        return successful_removals;
      }
    }
  }
  return Multiplicity{removals};
}

// Cf. NURBS book Eq. (5.36).
template<int para_dim>
void BSpline<para_dim>::ElevateDegreeImpl(Dimension const& dimension,
                                          Multiplicity const& multiplicity,
                                          Tolerance const& tolerance) const {
  using utilities::containers::AddAndAssignToFirst,
      utilities::containers::GetBack, utilities::containers::Multiply;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;

  // collect values before updating parameter_space
  auto const& [number_of_segments, knots_inserted] =
      MakeBezier(dimension, tolerance);
  IndexLength_ number_of_coordinates{
      parameter_space.GetNumberOfBasisFunctions()};
  const int n_total_original_coords =
      parameter_space.GetTotalNumberOfBasisFunctions();
  const int n_coords_per_slice =
      n_total_original_coords / number_of_coordinates[dimension];

  // update parameter space and prepare loops
  auto const& [last_segment_coordinate, coefficients] =
      parameter_space.ElevateDegree(dimension, multiplicity);
  IndexLength_ number_of_coordinates_in_slice{number_of_coordinates};
  number_of_coordinates_in_slice[dimension] = Length{};
  Index const maximum_interior_coordinate{
      static_cast<int>(coefficients.size() - 1)};
  int ignore_from{};
  vector_space.AppendEmptyCoordinates(
      n_coords_per_slice * number_of_segments
      * (maximum_interior_coordinate - (last_segment_coordinate - 1) + 1));
  for (int segment{}; segment < number_of_segments; ++segment) {
    int interior_coordinate{maximum_interior_coordinate},
        last_coordinate{((segment + 1) * last_segment_coordinate)
                        + (segment * multiplicity)};
    for (; interior_coordinate >= (last_segment_coordinate - 1);
         --interior_coordinate) {
      BinomialRatios_ const& current_coefficients =
          coefficients[interior_coordinate];
      IndexLength_ const previous_number_of_coordinates{number_of_coordinates};
      ++number_of_coordinates[dimension];
      for (Index_ slice_coordinate{
               Index_::First(number_of_coordinates_in_slice)};
           slice_coordinate != Index_::Behind(number_of_coordinates_in_slice);
           ++slice_coordinate) {
        IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
        coordinate_value[dimension] = last_coordinate;
        Index_ current_coordinate{number_of_coordinates, coordinate_value};
        Index const& insertion_position = current_coordinate.GetIndex1d();
        IndexValue_ current_last_coordinate_value{coordinate_value};
        current_last_coordinate_value[dimension] +=
            (maximum_interior_coordinate - interior_coordinate);
        Coordinate_ coordinate{
            Multiply(vector_space[Index_{previous_number_of_coordinates,
                                         current_last_coordinate_value}
                                      .GetIndex1d()
                                  + slice_coordinate.GetIndex1d()],
                     GetBack(current_coefficients))};
        std::for_each(
            current_coefficients.rbegin() + 1,
            current_coefficients.rend(),
            [&](BinomialRatio_ const& coefficient) {
              AddAndAssignToFirst(
                  coordinate,
                  Multiply(vector_space[current_coordinate.Decrement(dimension)
                                            .GetIndex1d()],
                           coefficient));
            });
        vector_space.StaticInsert(insertion_position,
                                  coordinate,
                                  n_total_original_coords + ignore_from++);
      }
    }

    for (; interior_coordinate >= Index{}; --interior_coordinate) {
      BinomialRatios_ const& current_coefficients =
          coefficients[interior_coordinate];
      --last_coordinate;
      for (Index_ slice_coordinate{
               Index_::First(number_of_coordinates_in_slice)};
           slice_coordinate != Index_::Behind(number_of_coordinates_in_slice);
           ++slice_coordinate) {
        IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
        coordinate_value[dimension] = last_coordinate;
        Index_ current_coordinate{number_of_coordinates, coordinate_value};
        Index const& replacement_position = current_coordinate.GetIndex1d();
        Coordinate_ coordinate{Multiply(vector_space[replacement_position],
                                        GetBack(current_coefficients))};
        std::for_each(
            current_coefficients.rbegin() + 1,
            current_coefficients.rend(),
            [&](BinomialRatio_ const& coefficient) {
              AddAndAssignToFirst(
                  coordinate,
                  Multiply(vector_space[current_coordinate.Decrement(dimension)
                                            .GetIndex1d()],
                           coefficient));
            });
        vector_space.Replace(replacement_position, coordinate);
      }
    }
  }
  Base_::CoarsenKnots(dimension, knots_inserted, tolerance);
}

template<int para_dim>
bool BSpline<para_dim>::ReduceDegreeImpl(
    Dimension const& dimension,
    Tolerance const& tolerance_reduction,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  using std::for_each, utilities::containers::GetBack,
      utilities::containers::Multiply,
      utilities::containers::SubtractAndAssignToFirst;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  ParameterSpace_ parameter_space_backup{parameter_space};
  VectorSpace_& vector_space = *vector_space_;
  VectorSpace_ vector_space_backup{vector_space};
  auto const& [number_of_segments, knots_inserted] =
      MakeBezier(dimension, tolerance);
  IndexLength_ number_of_coordinates{
      parameter_space.GetNumberOfBasisFunctions()};
  auto const& [last_segment_coordinate, coefficients] =
      parameter_space.ReduceDegree(dimension, multiplicity);
  Degree const& elevatetd_degree = (coefficients.size() + 1);
  IndexLength_ number_of_coordinates_in_slice{number_of_coordinates};
  number_of_coordinates_in_slice[dimension] = Length{};
  for (int segment{number_of_segments - 1}; segment >= 0; --segment) {
    Index interior_coordinate{},
        coordinate_index{1 + (segment * elevatetd_degree)};
    for (; interior_coordinate < (last_segment_coordinate - Index{1});
         ++interior_coordinate) {
      BinomialRatios_ const& current_coefficients =
          coefficients[interior_coordinate];
      for (Index_ slice_coordinate{
               Index_::Last(number_of_coordinates_in_slice)};
           slice_coordinate != Index_::Before(number_of_coordinates_in_slice);
           --slice_coordinate) {
        IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
        coordinate_value[dimension] = coordinate_index;
        Index_ current_coordinate{number_of_coordinates, coordinate_value};
        Index const& replacement_position = current_coordinate.GetIndex1d();
        Coordinate_ coordinate{vector_space[replacement_position]};
        for_each(
            current_coefficients.begin(),
            std::prev(current_coefficients.end()),
            [&](BinomialRatio_ const& coefficient) {
              SubtractAndAssignToFirst(
                  coordinate,
                  Multiply(vector_space[current_coordinate.Decrement(dimension)
                                            .GetIndex1d()],
                           coefficient));
            });
        vector_space.Replace(
            replacement_position,
            utilities::containers::Divide(coordinate,
                                          GetBack(current_coefficients)));
      }
      ++coordinate_index;
    }
    Index const maximum_interior_coordinate{elevatetd_degree - 2};
    for (; interior_coordinate <= maximum_interior_coordinate;
         ++interior_coordinate) {
      BinomialRatios_ const& current_coefficients =
          coefficients[interior_coordinate];
      IndexLength_ const previous_number_of_coordinates{number_of_coordinates};
      --number_of_coordinates[dimension];
      for (Index_ slice_coordinate{
               Index_::Last(number_of_coordinates_in_slice)};
           slice_coordinate != Index_::Before(number_of_coordinates_in_slice);
           --slice_coordinate) {
        IndexValue_ coordinate_value{slice_coordinate.GetIndex()};
        coordinate_value[dimension] = coordinate_index;
        Index_ current_coordinate{previous_number_of_coordinates,
                                  coordinate_value};
        Index const& erasure_position = current_coordinate.GetIndex1d();
        IndexValue_ current_last_coordinate_value{coordinate_value};
        current_last_coordinate_value[dimension] +=
            (maximum_interior_coordinate - interior_coordinate);
        Coordinate_ coordinate{vector_space[erasure_position]};
        for_each(
            current_coefficients.rbegin() + 1,
            current_coefficients.rend(),
            [&](BinomialRatio_ const& coefficient) {
              SubtractAndAssignToFirst(
                  coordinate,
                  Multiply(vector_space[current_coordinate.Decrement(dimension)
                                            .GetIndex1d()],
                           coefficient));
            });
        if ( // IsLessOrEqual(
            utilities::containers::EuclidianDistance(
                utilities::containers::DivideAndAssignToFirst(
                    coordinate,
                    GetBack(current_coefficients)),
                vector_space[Index_{number_of_coordinates,
                                    current_last_coordinate_value}
                                 .GetIndex1d()
                             + slice_coordinate.GetIndex1d() + Index{1}])
            <= tolerance_reduction) {
          vector_space.Erase(erasure_position);
        } else {
          parameter_space = parameter_space_backup;
          vector_space = vector_space_backup;
          return false;
        }
      }
    }
  }
  Base_::CoarsenKnots(dimension, knots_inserted, tolerance);
  return true;
}

template<int para_dim>
Coordinate
BSpline<para_dim>::ComputeUpperBoundForMaximumDistanceFromOrigin() const {
  return vector_space_->DetermineMaximumDistanceFromOrigin();
}

template<int para_dim>
typename BSpline<para_dim>::OutputInformation_
BSpline<para_dim>::Write(Precision const& precision) const {
  return OutputInformation_{Base_::parameter_space_->Write(precision),
                            vector_space_->Write(precision)};
}

// See NURBS book p. 169.
template<int para_dim>
typename BSpline<para_dim>::BezierInformation_
BSpline<para_dim>::MakeBezier(Dimension const& dimension,
                              Tolerance const& tolerance) const {
  BezierInformation_ const& bezier_information =
      Base_::parameter_space_->DetermineBezierExtractionKnots(dimension,
                                                              tolerance);
  Base_::RefineKnots(dimension, std::get<1>(bezier_information));
  return bezier_information;
}

// ---------------------------------------------------------------------------
// Shared refinement.
//
// A public edit (InsertKnot, RefineKnots via InsertKnot, ElevateDegree) on a
// participating spline is executed locally first - updating the single shared
// ParameterSpace together with this spline's own control net - and the same
// shape-preserving edit is then replayed on every other distinct control net
// of the group.  Knot re-insertion and knot removal inside degree elevation
// go through the public wrappers as well; the reentrancy guard makes sure
// those auxiliary steps stay local to the ongoing elevation.
// ---------------------------------------------------------------------------

template<int para_dim>
void BSpline<para_dim>::JoinSharedRefinementGroup() {
  if (Base_::parameter_space_ == nullptr
      || !Base_::parameter_space_->IsSharedRefinementEnabled()) {
    return;
  }
  auto const& registry =
      Base_::parameter_space_->GetSharedRefinementRegistry();
  shared_refinement_token_ = registry->RegisterNet(
      vector_space_,
      [this](Registry_::Edit const& edit) { this->ApplySharedEdit(edit); });
}

template<int para_dim>
void BSpline<para_dim>::LeaveSharedRefinementGroup() {
  shared_refinement_token_.reset();
  shared_refinement_active_ = false;
}

template<int para_dim>
void BSpline<para_dim>::ApplySharedEdit(Registry_::Edit const& edit) const {
  // Peer replay: run the unchanged shape-preserving algorithm against a
  // private copy of the parameter space as it was before the edit.  Every
  // peer needs its own copy, since the algorithms mutate their parameter space
  // in place; the one shared parameter space is only updated by the
  // initiating member.
  SharedPointer<ParameterSpace_> const current_parameter_space =
      Base_::parameter_space_;
  auto replay_parameter_space = std::make_shared<ParameterSpace_>(
      *std::static_pointer_cast<ParameterSpace_>(
          edit.pre_edit_parameter_space));
  Base_::parameter_space_ = replay_parameter_space;
  bool const was_active = shared_refinement_active_;
  shared_refinement_active_ = true;
  try {
    if (edit.kind == Registry_::EditKind::kInsertKnot) {
      InsertKnotImpl(edit.dimension,
                     edit.knot,
                     edit.multiplicity,
                     edit.tolerance);
    } else {
      ElevateDegreeImpl(edit.dimension, edit.multiplicity, edit.tolerance);
    }
  } catch (...) {
    shared_refinement_active_ = was_active;
    Base_::parameter_space_ = current_parameter_space;
    throw;
  }
  shared_refinement_active_ = was_active;
  Base_::parameter_space_ = current_parameter_space;
}

template<int para_dim>
void BSpline<para_dim>::ApplyEditGuarded(
    Registry_::Edit const& edit) const {
  // Initiating member: update the one shared parameter space together with
  // this spline's own control net.
  bool const was_active = shared_refinement_active_;
  shared_refinement_active_ = true;
  try {
    if (edit.kind == Registry_::EditKind::kInsertKnot) {
      InsertKnotImpl(edit.dimension,
                     edit.knot,
                     edit.multiplicity,
                     edit.tolerance);
    } else {
      ElevateDegreeImpl(edit.dimension, edit.multiplicity, edit.tolerance);
    }
  } catch (...) {
    shared_refinement_active_ = was_active;
    throw;
  }
  shared_refinement_active_ = was_active;
}

template<int para_dim>
void BSpline<para_dim>::DispatchEditLocally(
    Registry_::EditKind kind,
    Dimension const& dimension,
    Knot_ knot,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  auto const& registry =
      Base_::parameter_space_->GetSharedRefinementRegistry();
  if (registry == nullptr || shared_refinement_token_ == nullptr
      || shared_refinement_active_) {
    // Shared refinement off, this wrapper not in a group, or an auxiliary
    // step of an ongoing joint edit: behave exactly as the standalone path.
    Registry_::Edit const local_edit{kind,
                                     dimension,
                                     knot,
                                     multiplicity,
                                     tolerance,
                                     nullptr};
    ApplyEditGuarded(local_edit);
    return;
  }
  // Snapshot the parameter space before the initiating member changes it;
  // every peer replays against this snapshot.
  Registry_::Edit const edit{
      kind,
      dimension,
      knot,
      multiplicity,
      tolerance,
      std::make_shared<ParameterSpace_>(*Base_::parameter_space_)};
  registry->DispatchEdit(
      edit,
      static_cast<void*>(vector_space_.get()),
      [this](Registry_::Edit const& dispatched_edit) {
        this->ApplyEditGuarded(dispatched_edit);
      });
}

template<int para_dim>
void BSpline<para_dim>::InsertKnot(Dimension const& dimension,
                                   Knot_ knot,
                                   Multiplicity const& multiplicity,
                                   Tolerance const& tolerance) const {
  DispatchEditLocally(Registry_::EditKind::kInsertKnot,
                      dimension,
                      knot,
                      multiplicity,
                      tolerance);
}

template<int para_dim>
Multiplicity BSpline<para_dim>::RemoveKnot(
    Dimension const& dimension,
    Knot_ const& knot,
    Tolerance const& tolerance_removal,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  if (!shared_refinement_active_ && Base_::parameter_space_ != nullptr
      && Base_::parameter_space_->IsSharedRefinementEnabled()) {
    throw std::logic_error(
        "BSplineLib::splines::BSpline::RemoveKnot - knot removal is not "
        "allowed while shared refinement is enabled.");
  }
  return RemoveKnotImpl(dimension,
                        knot,
                        tolerance_removal,
                        multiplicity,
                        tolerance);
}

template<int para_dim>
void BSpline<para_dim>::ElevateDegree(Dimension const& dimension,
                                      Multiplicity const& multiplicity,
                                      Tolerance const& tolerance) const {
  DispatchEditLocally(Registry_::EditKind::kElevateDegree,
                      dimension,
                      Knot_{},
                      multiplicity,
                      tolerance);
}

template<int para_dim>
bool BSpline<para_dim>::ReduceDegree(
    Dimension const& dimension,
    Tolerance const& tolerance_reduction,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  if (!shared_refinement_active_ && Base_::parameter_space_ != nullptr
      && Base_::parameter_space_->IsSharedRefinementEnabled()) {
    throw std::logic_error(
        "BSplineLib::splines::BSpline::ReduceDegree - degree reduction is "
        "not allowed while shared refinement is enabled.");
  }
  return ReduceDegreeImpl(dimension,
                          tolerance_reduction,
                          multiplicity,
                          tolerance);
}
