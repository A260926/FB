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

template<int para_dim>
ParameterSpace<para_dim>::ParameterSpace(ParameterSpace const& other)
    : degrees_{other.degrees_},
      periodic_modes_{other.periodic_modes_} {
  CopyKnotVectors(other.knot_vectors_);
  CopyPeriodicState(other);
}

template<int para_dim>
ParameterSpace<para_dim>::ParameterSpace(KnotVectors_ knot_vectors,
                                         Degrees_ degrees,
                                         PeriodicModes<para_dim> periodic_modes,
                                         Tolerance const& tolerance)
    : knot_vectors_(std::move(knot_vectors)),
      degrees_(std::move(degrees)),
      periodic_modes_(periodic_modes) {
  for (int i{}; i < para_dim; ++i) {
    if (periodic_modes_[i] == Periodicity::Periodic) {
      periodic_axes_[i] = std::make_shared<PeriodicAxis>(
          PeriodicAxis::FromFiniteKnotVector(*knot_vectors_[i],
                                             degrees_[i],
                                             tolerance));
    }
  }
}

template<int para_dim>
ParameterSpace<para_dim>&
ParameterSpace<para_dim>::operator=(ParameterSpace const& rhs) {
  CopyKnotVectors(rhs.knot_vectors_);
  degrees_ = rhs.degrees_;
  periodic_modes_ = rhs.periodic_modes_;
  periodic_axes_ = {};
  CopyPeriodicState(rhs);
  return *this;
}

template<int para_dim>
typename ParameterSpace<para_dim>::Index_
ParameterSpace<para_dim>::First() const {
  return Index_::First(GetNumberOfNonZeroBasisFunctions());
}

template<int para_dim>
typename ParameterSpace<para_dim>::Index_
ParameterSpace<para_dim>::Behind() const {
  return Index_::Behind(GetNumberOfNonZeroBasisFunctions());
}

template<int para_dim>
typename ParameterSpace<para_dim>::NumberOfBasisFunctions_
ParameterSpace<para_dim>::GetNumberOfBasisFunctions() const {
  NumberOfBasisFunctions_ number_of_basis_functions;
  int i{};
  for (int& nobf : number_of_basis_functions) {
    nobf = GetNumberOfBasisFunctions(i++);
  }
  assert(i == para_dim);

  return number_of_basis_functions;
}

template<int para_dim>
int ParameterSpace<para_dim>::GetTotalNumberOfBasisFunctions() const {
  NumberOfBasisFunctions_ const& number_of_basis_functions =
      GetNumberOfBasisFunctions();
  return std::reduce(number_of_basis_functions.begin(),
                     number_of_basis_functions.end(),
                     Length{1},
                     std::multiplies{});
}

template<int para_dim>
SharedPointer<ParameterSpace<para_dim - 1>>
ParameterSpace<para_dim>::RemoveOneParametricDimension(
    const int parametric_dimension) const {
  if constexpr (para_dim < 2) {
    return nullptr;
  } else {
    // prepare output
    auto out_pspace = std::make_shared<ParameterSpace<para_dim - 1>>();

    auto& out_degrees = out_pspace->GetDegrees();
    auto& out_knot_vectors = out_pspace->GetKnotVectors();

    int out_dim_id{};
    for (int i{}; i < para_dim; ++i) {
      if (i == parametric_dimension) {
        continue;
      }
      out_degrees[out_dim_id] = degrees_[i];
      out_knot_vectors[out_dim_id] = knot_vectors_[i];
      ++out_dim_id;
    }

    return out_pspace;
  }
}

template<int para_dim>
typename ParameterSpace<para_dim>::Index_
ParameterSpace<para_dim>::FindFirstNonZeroBasisFunction(
    const Type_* parametric_coordinate,
    Tolerance const& tolerance) const {

  assert(tolerance > 0.0);

  Index_ first_support;
  first_support.GetLength() = std::move(GetNumberOfBasisFunctions());
  first_support.GetInvalid() = false;
  IndexValue_& first_index = first_support.GetValue();

  for (int i{}; i < para_dim; ++i) {
    Type_ coordinate = parametric_coordinate[i];
    if (periodic_modes_[i] == Periodicity::Periodic)
      coordinate = periodic_axes_[i]->MapToDomain(coordinate);
    first_index[i] =
        knot_vectors_[i]->FindEffectiveSpan(coordinate,
                                            degrees_[i],
                                            tolerance)
        - degrees_[i];
  }
  return first_support;
}

template<int para_dim>
typename ParameterSpace<para_dim>::KnotSpans_
ParameterSpace<para_dim>::FindKnotSpans(const Type_* parametric_coordinate,
                                        Tolerance const& tolerance) const {
  KnotSpans_ out;
  for (int i{}; i < para_dim; ++i) {
    Type_ coordinate = parametric_coordinate[i];
    if (periodic_modes_[i] == Periodicity::Periodic)
      coordinate = periodic_axes_[i]->MapToDomain(coordinate);
    out[i] = knot_vectors_[i]->FindSpan(coordinate, tolerance);
  }
  return out;
}

template<int para_dim>
typename ParameterSpace<para_dim>::BezierInformation_
ParameterSpace<para_dim>::DetermineBezierExtractionKnots(
    Dimension const& dimension,
    Tolerance const& tolerance) const {

  assert(tolerance > 0.0);
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);

  KnotVector const& knot_vector = *knot_vectors_[dimension];
  Knots_ const& unique_knots = knot_vector.GetUniqueKnots(tolerance);
  typename Knots_::const_iterator const &first_interior_knot =
                                            (unique_knots.begin() + 1),
                                        &last_knot =
                                            std::prev(unique_knots.end());
  int const& number_of_interior_knots = (last_knot - first_interior_knot);
  Knots_ bezier_extraction_knots;
  bezier_extraction_knots.reserve(number_of_interior_knots);
  std::for_each(first_interior_knot,
                last_knot,
                [&](ParametricCoordinate const& interior_knot) {
                  std::fill_n(
                      std::back_inserter(bezier_extraction_knots),
                      degrees_[dimension]
                          - knot_vector.DetermineMultiplicity(interior_knot),
                      interior_knot);
                });
  return BezierInformation_{number_of_interior_knots + 1,
                            bezier_extraction_knots};
}

template<int para_dim>
Vector<Vector<int>>
ParameterSpace<para_dim>::KnotMultiplicities(Tolerance const& tolerance) const {

  Vector<Vector<int>> multiplicities;
  multiplicities.reserve(para_dim);
  for (int i{}; i < para_dim; ++i) {
    multiplicities.emplace_back(
        knot_vectors_[i]->DetermineMultiplicities(tolerance));
  }

  return multiplicities;
}

template<int para_dim>
typename ParameterSpace<para_dim>::BasisValuesPerDimension_
ParameterSpace<para_dim>::EvaluateBasisValuesPerDimension(
    const Type_* parametric_coordinate,
    Tolerance const& tolerance) const {

  // prepare output
  BasisValuesPerDimension_ output;
  for (int i{}; i < para_dim; ++i) {

    // get this dim's info
    const auto& this_dim_degree = degrees_[i];
    const auto this_dim_n_basis = this_dim_degree + 1;
    Type_ this_dim_parametric_coordinate = parametric_coordinate[i];
    if (periodic_modes_[i] == Periodicity::Periodic)
      this_dim_parametric_coordinate =
          periodic_axes_[i]->MapToDomain(this_dim_parametric_coordinate);
    const auto& this_knot_vector = *knot_vectors_[i];
    const auto& this_knots = this_knot_vector.GetKnots();
    const int this_zero_degree_support =
        this_knot_vector
            .FindEffectiveSpan(this_dim_parametric_coordinate,
                               this_dim_degree,
                               tolerance)
            .Get();

    // this dim's output
    auto& this_dim_output = output[i];
    this_dim_output.Reallocate(this_dim_n_basis);
    this_dim_output[0] = 1.;

    TemporaryData_<double> left(this_dim_n_basis), right(this_dim_n_basis);

    double saved, temp;

    for (int k{1}; k < this_dim_n_basis; ++k) {
      this_dim_output[k] = 1.;

      left[k] = this_dim_parametric_coordinate
                - this_knots[this_zero_degree_support + 1 - k];
      right[k] = this_knots[this_zero_degree_support + k]
                 - this_dim_parametric_coordinate;

      saved = 0.0;
      for (int j{}; j < k; j++) {
        temp = this_dim_output[j] / (right[j + 1] + left[k - j]);
        this_dim_output[j] = saved + right[j + 1] * temp;
        saved = left[k - j] * temp;
      }
      this_dim_output[k] = saved;
    }
  }

  return output;
}

template<int para_dim>
typename ParameterSpace<para_dim>::BasisValues_
ParameterSpace<para_dim>::EvaluateBasisValues(
    const Type_* parametric_coordinate,
    Tolerance const& tolerance) const {
  return RecursiveCombine(
      EvaluateBasisValuesPerDimension(parametric_coordinate, tolerance));
}

template<int para_dim>
typename ParameterSpace<para_dim>::BasisValuesPerDimension_
ParameterSpace<para_dim>::EvaluateBasisDerivativeValuesPerDimension(
    const Type_* parametric_coordinate,
    const IntType_* derivative,
    Tolerance const& tolerance) const {

  // prepare output
  BasisValuesPerDimension_ output;

  for (int i{}; i < para_dim; ++i) {

    // get this dim's info
    const auto& this_dim_degree = degrees_[i];
    const auto& this_dim_derivative = derivative[i];
    const auto this_dim_n_basis = this_dim_degree + 1;
    // this dim's output and allocate
    auto& this_dim_output = output[i];
    this_dim_output.Reallocate(this_dim_n_basis);

    // special case for early exit - derivetive query is bigger than degree
    // all zeros.
    if (this_dim_derivative > this_dim_degree) {
      this_dim_output.Fill(0.);
      continue;
    }

    const auto& this_dim_parametric_coordinate_raw = parametric_coordinate[i];
    Type_ this_dim_parametric_coordinate = this_dim_parametric_coordinate_raw;
    if (periodic_modes_[i] == Periodicity::Periodic)
      this_dim_parametric_coordinate =
          periodic_axes_[i]->MapToDomain(this_dim_parametric_coordinate);
    const auto& this_knot_vector = *knot_vectors_[i];
    const auto& this_knots = this_knot_vector.GetKnots();
    const int this_zero_degree_support =
        this_knot_vector
            .FindEffectiveSpan(this_dim_parametric_coordinate,
                               this_dim_degree,
                               tolerance)
            .Get();

    // temporary ones that we need for second special case
    TemporaryData_<double> left(this_dim_n_basis), right(this_dim_n_basis);
    double saved, temp, d;

    // special case 2 - derivative 0 query is evaluation query
    if (this_dim_derivative == 0) {
      // just raw copy of evaluation
      this_dim_output[0] = 1.;

      for (int k{1}; k < this_dim_n_basis; ++k) {
        this_dim_output[k] = 1.;

        left[k] = this_dim_parametric_coordinate
                  - this_knots[this_zero_degree_support + 1 - k];
        right[k] = this_knots[this_zero_degree_support + k]
                   - this_dim_parametric_coordinate;

        saved = 0.0;
        for (int j{}; j < k; j++) {
          temp = this_dim_output[j] / (right[j + 1] + left[k - j]);
          this_dim_output[j] = saved + right[j + 1] * temp;
          saved = left[k - j] * temp;
        }
        this_dim_output[k] = saved;
      }
      continue;
    }

    // here, proper derivative query.
    // more temporary variables
    TemporaryData2D_<double> a(2, this_dim_n_basis),
        ndu(this_dim_n_basis, this_dim_n_basis);
    int j1, j2;

    ndu(0, 0) = 1.;
    for (int j{1}; j < this_dim_n_basis; ++j) {
      left[j] = this_dim_parametric_coordinate
                - this_knots[this_zero_degree_support + 1 - j];
      right[j] = this_knots[this_zero_degree_support + j]
                 - this_dim_parametric_coordinate;

      saved = 0.0;
      for (int r{}; r < j; ++r) {
        ndu(j, r) = right[r + 1] + left[j - r];
        temp = ndu(r, j - 1) / ndu(j, r);
        ndu(r, j) = saved + right[r + 1] * temp;
        saved = left[j - r] * temp;
      }
      ndu(j, j) = saved;
    }

    if (this_dim_derivative == 0) {
      for (int j{}; j < this_dim_n_basis; ++j) {
        this_dim_output[j] = ndu(j, this_dim_degree);
      }
      continue;
    }

    for (int r{}; r < this_dim_n_basis; ++r) {
      int s1{}, s2{1}, j;
      a(0, 0) = 1.0;
      for (int k{1}; k < this_dim_derivative + 1; ++k) {
        d = 0.0;
        const int rk = r - k;
        const int pk = this_dim_degree - k;
        if (r >= k) {
          a(s2, 0) = a(s1, 0) / ndu(pk + 1, rk);
          d = a(s2, 0) * ndu(rk, pk);
        }

        if (rk >= -1) {
          j1 = 1;
        } else {
          j1 = -rk;
        }

        if (r - 1 <= pk) {
          j2 = k - 1;
        } else {
          j2 = this_dim_degree - r;
        }

        for (j = j1; j < j2 + 1; ++j) {
          a(s2, j) = (a(s1, j) - a(s1, j - 1)) / ndu(pk + 1, rk + j);
          d += a(s2, j) * ndu(rk + j, pk);
        }

        if (r <= pk) {
          a(s2, k) = -a(s1, k - 1) / ndu(pk + 1, r);
          d += a(s2, j) * ndu(rk + j, pk);
        }
        // here, we could also gather lower derivative queries
        // for now, just rewrite at the same place
        this_dim_output[r] = d;

        j = s1;
        s1 = s2;
        s2 = j;
      }
    }

    temp = this_dim_degree;
    for (int k{1}; k < this_dim_derivative; ++k) {
      temp *= (this_dim_degree - k);
    }
    for (int j{}; j < this_dim_n_basis; ++j) {
      this_dim_output[j] *= temp;
    }
  }

  return output;
}

template<int para_dim>
typename ParameterSpace<para_dim>::BasisValues_
ParameterSpace<para_dim>::EvaluateBasisDerivativeValues(
    const Type_* parametric_coordinate,
    const IntType_* derivative,
    Tolerance const& tolerance) const {
  return RecursiveCombine(
      EvaluateBasisDerivativeValuesPerDimension(parametric_coordinate,
                                                derivative,
                                                tolerance));
}

template<int para_dim>
typename ParameterSpace<para_dim>::InsertionInformation_
ParameterSpace<para_dim>::InsertKnot(Dimension const& dimension,
                                     Knot_ knot,
                                     Multiplicity const& multiplicity,
                                     Tolerance const& tolerance) {
  assert(tolerance > 0.0);

  // runtime checks
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);

  // Periodic axes are handled through PeriodicAxis orbits by the owning
  // spline; see splines::BSpline::InsertKnot.
  if (periodic_modes_[dimension] == Periodicity::Periodic) {
    throw RuntimeError(
        "ParameterSpace::InsertKnot must not be called directly on a "
        "periodic axis; use the spline's InsertKnot instead.");
  }

  KnotWithinBoundCheck(BSPLINELIB_FUNC(), dimension, knot, true);

  // if it is on bound, early exit
  if (!KnotNotOnBoundCheck(BSPLINELIB_FUNC(),
                           dimension,
                           knot,
                           tolerance,
                           false)) {
    return InsertionInformation_{};
  }

  KnotVector& knot_vector = *knot_vectors_[dimension];
  // instead of (degree - current_multiplicity),
  // clip at (degree - current_multiplicity) + 1 is to allow C^-1
  Multiplicity const insertion{
      std::min(multiplicity,
               degrees_[dimension]
                   - knot_vector.DetermineMultiplicity(knot, tolerance) + 1)};
  InsertionInformation_ const& insertion_information =
      DetermineInsertionInformation(dimension, knot, insertion, tolerance);
  knot_vector.Insert(std::move(knot), insertion, tolerance);
  return insertion_information;
}

template<int para_dim>
typename ParameterSpace<para_dim>::InsertionInformation_
ParameterSpace<para_dim>::RemoveKnot(Dimension const& dimension,
                                     Knot_ const& knot,
                                     Multiplicity const& multiplicity,
                                     Tolerance const& tolerance) {
  assert(tolerance > 0.0);

  assert(tolerance > 0.0);

  // runtime checks
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);

  if (periodic_modes_[dimension] == Periodicity::Periodic) {
    throw std::logic_error(
        "RemoveKnot is not supported on a periodic axis.");
  }

  KnotWithinBoundCheck(BSPLINELIB_FUNC(), dimension, knot, true);

  // if it is on bound, early exit
  if (!KnotNotOnBoundCheck(BSPLINELIB_FUNC(),
                           dimension,
                           knot,
                           tolerance,
                           false)) {
    return InsertionInformation_{};
  }

  if (Multiplicity const& removals =
          knot_vectors_[dimension]->Remove(knot, multiplicity, tolerance);
      removals != Multiplicity{}) {
    return DetermineInsertionInformation(dimension, knot, removals, tolerance);
  } else {
    return InsertionInformation_{};
  }
}

// Cf. NURBS book Eq. (5.36).
template<int para_dim>
typename ParameterSpace<para_dim>::ElevationInformation_
ParameterSpace<para_dim>::ElevateDegree(Dimension const& dimension,
                                        Multiplicity const& multiplicity,
                                        Tolerance const& tolerance) {
  assert(tolerance > 0.0);
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);

  if (periodic_modes_[dimension] == Periodicity::Periodic) {
    throw std::logic_error(
        "ElevateDegree is not supported on a periodic axis.");
  }

  ElevationInformation_ const& bezier_information =
      DetermineElevationInformation(dimension, multiplicity);
  knot_vectors_[dimension]->IncreaseMultiplicities(multiplicity);
  degrees_[dimension] += multiplicity;
  return bezier_information;
}

template<int para_dim>
typename ParameterSpace<para_dim>::ElevationInformation_
ParameterSpace<para_dim>::ReduceDegree(Dimension const& dimension,
                                       Multiplicity const& multiplicity,
                                       Tolerance const& tolerance) {
  assert(tolerance > 0.0);
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);

  if (periodic_modes_[dimension] == Periodicity::Periodic) {
    throw std::logic_error(
        "ReduceDegree is not supported on a periodic axis.");
  }

  Degree& degree = degrees_[dimension];
  Multiplicity const reduction{std::min(multiplicity, degree - 1)};
  knot_vectors_[dimension]->DecreaseMultiplicities(reduction);
  degree -= reduction;
  return DetermineElevationInformation(dimension, reduction);
}

template<int para_dim>
typename ParameterSpace<para_dim>::OutputInformation_
ParameterSpace<para_dim>::Write(Precision const& precision) const {
  using std::get, std::tuple_element_t, utilities::string_operations::Write;

  OutputInformation_ knot_vectors_degrees_and_maximum_basis_function_indices;
  for (int i{}; i < para_dim; ++i) {
    get<0>(knot_vectors_degrees_and_maximum_basis_function_indices)[i] =
        knot_vectors_[i]->Write(precision);
  }
  get<1>(knot_vectors_degrees_and_maximum_basis_function_indices) =
      Write<tuple_element_t<1, OutputInformation_>>(degrees_, precision);
  get<2>(knot_vectors_degrees_and_maximum_basis_function_indices) =
      Write<tuple_element_t<2, OutputInformation_>>(GetNumberOfBasisFunctions(),
                                                    precision);
  return knot_vectors_degrees_and_maximum_basis_function_indices;
}

#ifndef NDEBUG
template<int para_dim>
void ParameterSpace<para_dim>::ThrowIfParametricCoordinateIsOutsideScope(
    Dimension const& dimension,
    ParametricCoordinate const& parametric_coordinate,
    Tolerance const& tolerance) const {
  knot_vectors_[dimension]->ThrowIfParametricCoordinateIsOutsideScope(
      parametric_coordinate,
      tolerance);
}
#endif

template<int para_dim>
void ParameterSpace<para_dim>::CopyKnotVectors(
    KnotVectors_ const& knot_vectors) {
  for (int i{}; i < para_dim; ++i) {
    knot_vectors_[i] = std::make_shared<KnotVector>(*knot_vectors[i]);
  }
}

template<int para_dim>
void ParameterSpace<para_dim>::CopyPeriodicState(
    ParameterSpace const& other) {
  for (int i{}; i < para_dim; ++i) {
    if (other.periodic_axes_[i]) {
      periodic_axes_[i] =
          std::make_shared<PeriodicAxis>(*other.periodic_axes_[i]);
    }
  }
}

template<int para_dim>
bool ParameterSpace<para_dim>::HasAnyPeriodicAxis() const {
  return std::any_of(periodic_modes_.begin(),
                     periodic_modes_.end(),
                     [](Periodicity const& mode) {
                       return mode == Periodicity::Periodic;
                     });
}

template<int para_dim>
PeriodicAxis& ParameterSpace<para_dim>::GetPeriodicAxis(
    Dimension const& dimension) {
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);
  if (periodic_modes_[dimension] != Periodicity::Periodic) {
    throw InvalidArgument(
        "ParameterSpace::GetPeriodicAxis - axis is not periodic.");
  }
  return *periodic_axes_[dimension];
}

template<int para_dim>
PeriodicAxis const& ParameterSpace<para_dim>::GetPeriodicAxis(
    Dimension const& dimension) const {
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);
  if (periodic_modes_[dimension] != Periodicity::Periodic) {
    throw InvalidArgument(
        "ParameterSpace::GetPeriodicAxis - axis is not periodic.");
  }
  return *periodic_axes_[dimension];
}

template<int para_dim>
typename ParameterSpace<para_dim>::ParametricCoordinate_
ParameterSpace<para_dim>::MapToPeriodicDomain(
    const Type_* parametric_coordinate) const {
  ParametricCoordinate_ mapped{};
  for (int i{}; i < para_dim; ++i) {
    mapped[i] = (periodic_modes_[i] == Periodicity::Periodic)
                    ? periodic_axes_[i]->MapToDomain(parametric_coordinate[i])
                    : parametric_coordinate[i];
  }
  return mapped;
}

template<int para_dim>
void ParameterSpace<para_dim>::RotatePeriodicSeam(
    Dimension const& dimension,
    Knot_ const& seam,
    Tolerance const& tolerance) {
  DimensionBoundCheck(BSPLINELIB_FUNC(), dimension, true);
  if (periodic_modes_[dimension] != Periodicity::Periodic) {
    throw InvalidArgument(
        "ParameterSpace::RotatePeriodicSeam - axis is not periodic.");
  }
  PeriodicAxis& axis = *periodic_axes_[dimension];
  axis.RotateSeam(seam, tolerance); // validates the orbit before changing
  knot_vectors_[dimension] =
      std::make_shared<KnotVector>(axis.FiniteKnots());
}

template<int para_dim>
void ParameterSpace<para_dim>::RebuildPeriodicKnotVectors() {
  for (int i{}; i < para_dim; ++i) {
    if (periodic_modes_[i] == Periodicity::Periodic) {
      knot_vectors_[i] =
          std::make_shared<KnotVector>(periodic_axes_[i]->FiniteKnots());
    }
  }
}

template<int para_dim>
Array<int, para_dim> ParameterSpace<para_dim>::GetFundamentalSizes() const {
  Array<int, para_dim> sizes{};
  for (int i{}; i < para_dim; ++i) {
    sizes[i] = (periodic_modes_[i] == Periodicity::Periodic)
                   ? periodic_axes_[i]->GetNumberOfFundamentalKnots()
                   : GetNumberOfBasisFunctions(i);
  }
  return sizes;
}

template<int para_dim>
Array<Array<double, 2>, para_dim>
ParameterSpace<para_dim>::GetDomainRanges() const {
  Array<Array<double, 2>, para_dim> ranges{};
  for (int i{}; i < para_dim; ++i) {
    if (periodic_modes_[i] == Periodicity::Periodic) {
      ranges[i] = {periodic_axes_[i]->GetDomainStart(),
                   periodic_axes_[i]->GetDomainEnd()};
    } else {
      ranges[i] = {knot_vectors_[i]->GetFront(),
                   knot_vectors_[i]->GetBack()};
    }
  }
  return ranges;
}

template<int para_dim>
int ParameterSpace<para_dim>::GetNumberOfNonZeroBasisFunctions(
    Dimension const& dimension) const {
  return degrees_[dimension] + 1;
}

template<int para_dim>
typename ParameterSpace<para_dim>::NumberOfBasisFunctions_
ParameterSpace<para_dim>::GetNumberOfNonZeroBasisFunctions() const {
  NumberOfBasisFunctions_ number_of_non_zero_basis_functions;
  for (int i{}; i < para_dim; ++i) {
    number_of_non_zero_basis_functions[i] = GetNumberOfNonZeroBasisFunctions(i);
  }

  return number_of_non_zero_basis_functions;
}

template<int para_dim>
Length ParameterSpace<para_dim>::GetNumberOfBasisFunctions(
    Dimension const& dimension) const {
  return Length{knot_vectors_[dimension]->GetSize()
                - GetNumberOfNonZeroBasisFunctions(dimension)};
}

// Cf. NURBS book below Eq. (5.15).
template<int para_dim>
typename ParameterSpace<para_dim>::InsertionInformation_
ParameterSpace<para_dim>::DetermineInsertionInformation(
    Dimension const& dimension,
    ParametricCoordinate const& knot,
    Multiplicity const& multiplicity,
    Tolerance const& tolerance) const {
  KnotVector& knot_vector = *knot_vectors_[dimension];
  Multiplicity const previous_multiplicity{
      knot_vector.DetermineMultiplicity(knot, tolerance)};
  KnotSpan::Type_ const knot_span{knot_vector.FindSpan(knot, tolerance).Get()};
  InsertionCoefficients_ knot_ratios;
  knot_ratios.reserve(multiplicity);
  Index const end_knot{knot_span - previous_multiplicity};

  // multiplicity loop
  for (int i{}; i < multiplicity; ++i) {
    KnotRatios_ current_knot_ratios;
    Multiplicity const remaining_mulitplicity{degrees_[dimension] - i};
    current_knot_ratios.reserve(remaining_mulitplicity - previous_multiplicity);

    for (int j{knot_span - remaining_mulitplicity + 1}; j < end_knot + 1; ++j) {
      ParametricCoordinate const& lower_knot = knot_vector[j];
      current_knot_ratios.push_back(
          (knot - lower_knot)
          / (knot_vector[j + remaining_mulitplicity] - lower_knot));
    }
    knot_ratios.push_back(current_knot_ratios);
  }

  return InsertionInformation_{end_knot, knot_ratios};
}

template<int para_dim>
typename ParameterSpace<para_dim>::ElevationInformation_
ParameterSpace<para_dim>::DetermineElevationInformation(
    Dimension const& dimension,
    Multiplicity const& multiplicity) const {
  using utilities::math_operations::ComputeBinomialCoefficient;

  Degree const& degree = degrees_[dimension];
  const int maximum_bezier_coordinate = (degree + multiplicity);
  ElevationCoefficients_ bezier_coefficients;
  bezier_coefficients.reserve(maximum_bezier_coordinate - 1);

  for (int i{1}; i < maximum_bezier_coordinate; ++i) {
    const int bezier_coordinate_begin{std::max(0, i - multiplicity)};
    const int bezier_coordinate_end{std::min(degree, i) + 1};
    // Capturing result by reference produces dangling references if maxima
    // of the parameters are temporaries.

    BinomialRatios_ current_bezier_coefficients(
        bezier_coordinate_end - bezier_coordinate_begin,
        1.0 / ComputeBinomialCoefficient(maximum_bezier_coordinate, i));

    for (int j{bezier_coordinate_begin}; j < bezier_coordinate_end; ++j) {
      current_bezier_coefficients[j - bezier_coordinate_begin] *=
          (ComputeBinomialCoefficient(degree, j)
           * ComputeBinomialCoefficient(multiplicity, i - j));
    }
    bezier_coefficients.push_back(current_bezier_coefficients);
  }

  return ElevationInformation_{Index{degree}, bezier_coefficients};
}
