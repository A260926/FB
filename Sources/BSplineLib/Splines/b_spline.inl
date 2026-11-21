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
}

template<int para_dim>
BSpline<para_dim>::BSpline(
    SharedPointer<ParameterSpace_> parameter_space,
    SharedPointer<VectorSpace_> vector_space,
    parameter_spaces::PeriodicModes<para_dim> periodic_modes)
    : Base_(std::move(parameter_space), false) {
  using parameter_spaces::Periodicity;

  auto& parameter_space_ref = *Base_::parameter_space_;
  if (parameter_space_ref.GetPeriodicModes() != periodic_modes) {
    parameter_space_ref =
        ParameterSpace_(parameter_space_ref.GetKnotVectors(),
                        parameter_space_ref.GetDegrees(),
                        periodic_modes);
  }

  auto const number_of_basis_functions =
      parameter_space_ref.GetNumberOfBasisFunctions();
  Array<int, para_dim> sizes{};
  for (int d{}; d < para_dim; ++d)
    sizes[d] = number_of_basis_functions[d];

  parameter_spaces::ValidatePeriodicControlPadding<para_dim>(
      vector_space->GetCoordinates(),
      sizes,
      periodic_modes,
      parameter_space_ref.GetFundamentalSizes(),
      parameter_space_ref.GetDegrees(),
      false);
  vector_space_ = std::move(vector_space);
}

template<int para_dim>
BSpline<para_dim>::BSpline(BSpline const& other)
    : Base_(other),
      vector_space_{std::make_shared<VectorSpace_>(*other.vector_space_)} {}

template<int para_dim>
BSpline<para_dim>& BSpline<para_dim>::operator=(BSpline const& rhs) {
  Base_::operator=(rhs),
  vector_space_ = std::make_shared<VectorSpace_>(*rhs.vector_space_);
  return *this;
}

template<int para_dim>
void BSpline<para_dim>::Evaluate(const Type_* parametric_coordinate,
                                 Type_* evaluated) const {

  ParameterSpace_ const& parameter_space = *Base_::parameter_space_;

  if (parameter_space.HasAnyPeriodicAxis()) {
    // Periodic evaluation: parameters are mapped to [a, b) per axis and
    // control point indices are wrapped modulo the periodic control count.
    // No clamped surrogate spline is constructed.
    auto const basis_per_dim =
        parameter_space.EvaluateBasisValuesPerDimension(parametric_coordinate);
    auto const first_index =
        parameter_space.FindFirstNonZeroBasisFunction(parametric_coordinate)
            .MultiIndex();
    auto const control_counts =
        parameter_space.GetNumberOfBasisFunctions();
    Array<int, para_dim> counts{};
    for (int d{}; d < para_dim; ++d)
      counts[d] = control_counts[d];
    parameter_spaces::PeriodicTensorCombine<para_dim>(
        basis_per_dim,
        first_index,
        counts,
        parameter_space.GetFundamentalSizes(),
        parameter_space.GetDegrees(),
        parameter_space.GetPeriodicModes(),
        vector_space_->GetCoordinates().data(),
        vector_space_->Dim(),
        evaluated);
    return;
  }

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

  if (parameter_space.HasAnyPeriodicAxis()) {
    // Basis derivatives are taken with respect to the original real
    // parameter: periodic wrapping is a pure translation, so the local
    // Cox-de Boor derivatives on the mapped parameter equal the derivatives
    // on the original parameter.
    int total_order{};
    for (int d{}; d < para_dim; ++d)
      total_order += derivative[d];
    if (total_order > 2) {
      throw InvalidArgument(
          "Periodic splines support derivative orders up to 2 only.");
    }
    auto const basis_derivative_per_dim =
        parameter_space.EvaluateBasisDerivativeValuesPerDimension(
            parametric_coordinate,
            derivative);
    auto const first_index =
        parameter_space.FindFirstNonZeroBasisFunction(parametric_coordinate)
            .MultiIndex();
    auto const control_counts =
        parameter_space.GetNumberOfBasisFunctions();
    Array<int, para_dim> counts{};
    for (int d{}; d < para_dim; ++d)
      counts[d] = control_counts[d];
    parameter_spaces::PeriodicTensorCombine<para_dim>(
        basis_derivative_per_dim,
        first_index,
        counts,
        parameter_space.GetFundamentalSizes(),
        parameter_space.GetDegrees(),
        parameter_space.GetPeriodicModes(),
        vector_space_->GetCoordinates().data(),
        vector_space_->Dim(),
        evaluated);
    return;
  }

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
void BSpline<para_dim>::InsertKnot(Dimension const& dimension,
                                   Knot_ knot,
                                   Multiplicity const& multiplicity,
                                   Tolerance const& tolerance) const {
  using utilities::containers::Add, utilities::containers::Multiply;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;

  if (parameter_space.IsPeriodic(dimension)) {
    InsertKnotPeriodic(dimension, knot, multiplicity, tolerance);
    return;
  }

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
Multiplicity BSpline<para_dim>::RemoveKnot(Dimension const& dimension,
                                           Knot_ const& knot,
                                           Tolerance const& tolerance_removal,
                                           Multiplicity const& multiplicity,
                                           Tolerance const& tolerance) const {
  using utilities::containers::Divide, utilities::containers::Multiply,
      utilities::containers::Subtract;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;

  if (parameter_space.IsPeriodic(dimension)) {
    throw std::logic_error(
        "RemoveKnot is not supported on a periodic axis.");
  }

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
void BSpline<para_dim>::ElevateDegree(Dimension const& dimension,
                                      Multiplicity const& multiplicity,
                                      Tolerance const& tolerance) const {
  using utilities::containers::AddAndAssignToFirst,
      utilities::containers::GetBack, utilities::containers::Multiply;

  // bound checks are all done in parametric space
  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;

  if (parameter_space.IsPeriodic(dimension)) {
    throw std::logic_error(
        "ElevateDegree is not supported on a periodic axis.");
  }

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
bool BSpline<para_dim>::ReduceDegree(Dimension const& dimension,
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

  if (parameter_space.IsPeriodic(dimension)) {
    throw std::logic_error(
        "ReduceDegree is not supported on a periodic axis.");
  }

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

template<int para_dim>
parameter_spaces::PeriodicModes<para_dim>
BSpline<para_dim>::GetPeriodicModes() const {
  return Base_::parameter_space_->GetPeriodicModes();
}

template<int para_dim>
bool BSpline<para_dim>::IsPeriodic(Dimension const& dimension) const {
  return Base_::parameter_space_->IsPeriodic(dimension);
}

namespace {

// One periodic orbit insertion on one control row.  The whole translation
// orbit of u is inserted at once, so the new control polygon stays periodic
// with period m + 1.  Takes the m fundamental controls (natural convention
// Q_r = P_(r mod m)) and returns m + 1 fundamental controls.  Modified
// controls near the seam wrap into the upper fundamental slots, which is
// what makes seam-crossing insertion exact.
inline std::vector<double> PeriodicInfiniteInsertion(
    parameter_spaces::PeriodicAxis const& axis,
    std::vector<double> const& fundamental_controls,
    int coordinate_dimension,
    double mapped_knot,
    int span_index) {
  int const p = axis.GetDegree();
  int const m = axis.GetNumberOfFundamentalKnots();
  int const m_new = m + 1;
  std::vector<double> refined(m_new * coordinate_dimension, 0.0);

  auto old_slot = [m](int infinite_index) {
    int slot = infinite_index % m;
    if (slot < 0)
      slot += m;
    return slot;
  };
  auto new_slot = [m_new](int infinite_index) {
    int slot = infinite_index % m_new;
    if (slot < 0)
      slot += m_new;
    return slot;
  };

  // Default identity mapping of the infinite insertion formula; every new
  // slot first inherits an unchanged (periodically wrapped) old control.
  for (int j{}; j < m_new; ++j) {
    int source_infinite = (j <= span_index - p) ? j : (j - 1);
    int const source = old_slot(source_infinite);
    for (int c{}; c < coordinate_dimension; ++c) {
      refined[j * coordinate_dimension + c] =
          fundamental_controls[source * coordinate_dimension + c];
    }
  }

  // Overwrite the p modified controls of the affected window, wrapping
  // seam-adjacent infinite indices into fundamental slots.
  for (int r{span_index - p + 1}; r <= span_index; ++r) {
    double const lower_knot = axis.KnotAt(r);
    double const upper_knot = axis.KnotAt(r + p);
    double const alpha =
        (mapped_knot - lower_knot) / (upper_knot - lower_knot);
    int const target = new_slot(r);
    int const source_j = old_slot(r);
    int const source_j_minus_one = old_slot(r - 1);
    for (int c{}; c < coordinate_dimension; ++c) {
      refined[target * coordinate_dimension + c] =
          alpha
              * fundamental_controls[source_j * coordinate_dimension + c]
          + (1.0 - alpha)
                * fundamental_controls[source_j_minus_one
                                       * coordinate_dimension + c];
    }
  }
  return refined;
}

} // namespace

template<int para_dim>
void BSpline<para_dim>::InsertKnotPeriodic(Dimension const& dimension,
                                           Knot_ knot,
                                           Multiplicity const& multiplicity,
                                           Tolerance const& tolerance) const {
  using parameter_spaces::PeriodicAxis;
  using parameter_spaces::Periodicity;

  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;
  PeriodicAxis& axis = parameter_space.GetPeriodicAxis(dimension);
  int const p = parameter_space.GetDegree(dimension);
  int const m = axis.GetNumberOfFundamentalKnots();

  int const current_multiplicity =
      axis.DetermineOrbitMultiplicity(knot, tolerance);
  int const performed =
      std::max(0, std::min(multiplicity, p - current_multiplicity));
  if (performed == 0)
    return;

  double const mapped_knot = axis.MapToDomain(knot);
  int const m_new = m + performed;
  int const n_new = m_new + p;
  int const d = vector_space.Dim();

  auto const old_shape = parameter_space.GetNumberOfBasisFunctions();
  Array<int, para_dim> lengths{};
  for (int k{}; k < para_dim; ++k)
    lengths[k] = old_shape[k];

  int stride{1};
  for (int k{}; k < dimension; ++k)
    stride *= lengths[k];
  int slice_count{1};
  for (int k{}; k < para_dim; ++k)
    if (k != dimension)
      slice_count *= lengths[k];

  int total_new{1};
  Array<int, para_dim> new_lengths = lengths;
  new_lengths[dimension] = n_new;
  for (int k{}; k < para_dim; ++k)
    total_new *= new_lengths[k];
  typename VectorSpace_::Coordinates_ new_coordinates(total_new, d);

  // All rows are refined on locally allocated fundamental storage first so
  // that no half-edited state is visible if an allocation fails.
  for (int slice{}; slice < slice_count; ++slice) {
    int const low = slice % stride;
    int const high = slice / stride;
    int const old_base = low + high * (lengths[dimension] * stride);
    int const new_base = low + high * (n_new * stride);

    std::vector<double> row(m * d);
    for (int j{}; j < m; ++j) {
      std::copy_n(vector_space.CoordinateBegin(old_base + j * stride),
                  d,
                  row.data() + j * d);
    }

    // A working copy of the axis supplies updated knots after each single
    // insertion, leaving the real parameter space untouched until commit.
    PeriodicAxis work_axis = axis;
    for (int r{}; r < performed; ++r) {
      int const span = work_axis.FindLocalSpan(mapped_knot);
      row = PeriodicInfiniteInsertion(
          work_axis, row, d, mapped_knot, span);
      work_axis.InsertOrbit(mapped_knot, 1, tolerance);
    }

    // Fundamental controls followed by exact periodic padding copies.
    for (int j{}; j < m_new; ++j) {
      std::copy_n(row.data() + j * d,
                  d,
                  &new_coordinates(new_base + j * stride, 0));
    }
    for (int j{}; j < p; ++j) {
      std::copy_n(row.data() + j * d,
                  d,
                  &new_coordinates(new_base + (m_new + j) * stride, 0));
    }
  }

  // All local work succeeded; commit in an order whose failing steps cannot
  // leave a half-edited spline.  The fundamental multiset is updated first;
  // the only subsequent allocations (the rebuilt knot vectors) are mirrored
  // into local pointers before they replace the stored ones, and the final
  // control net update is a non-throwing move.
  axis.InsertOrbit(knot, performed, tolerance);
  typename ParameterSpace_::KnotVectors_ rebuilt =
      parameter_space.GetKnotVectors();
  for (int k{}; k < para_dim; ++k) {
    if (parameter_space.GetPeriodicModes()[k]
        == parameter_spaces::Periodicity::Periodic) {
      rebuilt[k] = std::make_shared<parameter_spaces::KnotVector>(
          parameter_space.GetPeriodicAxis(k).FiniteKnots());
    }
  }
  for (int k{}; k < para_dim; ++k)
    parameter_space.GetKnotVectors()[k] = rebuilt[k];
  vector_space.GetCoordinates() = std::move(new_coordinates);
}

template<int para_dim>
void BSpline<para_dim>::RotatePeriodicSeam(Dimension const& dimension,
                                           Knot_ seam,
                                           Tolerance const& tolerance) const {
  using parameter_spaces::PeriodicAxis;

  ParameterSpace_& parameter_space = *Base_::parameter_space_;
  VectorSpace_& vector_space = *vector_space_;
  PeriodicAxis& axis = parameter_space.GetPeriodicAxis(dimension);

  int const orbit = axis.FindOrbitIndex(seam, tolerance);
  if (orbit < 0) {
    throw InvalidArgument(
        "RotatePeriodicSeam: the seam must be periodic-equivalent to an "
        "existing fundamental knot.");
  }

  int const p = parameter_space.GetDegree(dimension);
  int const m = axis.GetNumberOfFundamentalKnots();
  int const n = m + p;
  int const d = vector_space.Dim();

  auto const old_shape = parameter_space.GetNumberOfBasisFunctions();
  Array<int, para_dim> lengths{};
  for (int k{}; k < para_dim; ++k)
    lengths[k] = old_shape[k];

  int stride{1};
  for (int k{}; k < dimension; ++k)
    stride *= lengths[k];
  int slice_count{1};
  for (int k{}; k < para_dim; ++k)
    if (k != dimension)
      slice_count *= lengths[k];

  int total{1};
  for (int k{}; k < para_dim; ++k)
    total *= lengths[k];
  typename VectorSpace_::Coordinates_ rotated_coordinates(total, d);

  for (int slice{}; slice < slice_count; ++slice) {
    int const low = slice % stride;
    int const high = slice / stride;
    int const base = low + high * (lengths[dimension] * stride);

    // Rotated fundamental row j holds old fundamental row (orbit + j) mod m.
    std::vector<int> source(m);
    for (int j{}; j < m; ++j)
      source[j] = (orbit + j) % m;
    for (int j{}; j < n; ++j) {
      int fundamental_slot = (j < m) ? j : (j - m);
      int const old_slot = source[fundamental_slot];
      std::copy_n(vector_space.CoordinateBegin(base + old_slot * stride),
                  d,
                  &rotated_coordinates(base + j * stride, 0));
    }
  }

  // Commit: rotate the local multiset first; allocate the rebuilt knot
  // vectors before replacing the stored pointers so an allocation failure
  // cannot leave the spline half-edited, then move the control net.
  axis.RotateSeam(seam, tolerance);
  typename ParameterSpace_::KnotVectors_ rebuilt =
      parameter_space.GetKnotVectors();
  for (int k{}; k < para_dim; ++k) {
    if (parameter_space.GetPeriodicModes()[k]
        == parameter_spaces::Periodicity::Periodic) {
      rebuilt[k] = std::make_shared<parameter_spaces::KnotVector>(
          parameter_space.GetPeriodicAxis(k).FiniteKnots());
    }
  }
  for (int k{}; k < para_dim; ++k)
    parameter_space.GetKnotVectors()[k] = rebuilt[k];
  vector_space.GetCoordinates() = std::move(rotated_coordinates);
}
