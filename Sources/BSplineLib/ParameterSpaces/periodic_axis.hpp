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

#ifndef SOURCES_PARAMETERSPACES_PERIODIC_AXIS_HPP_
#define SOURCES_PARAMETERSPACES_PERIODIC_AXIS_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "BSplineLib/ParameterSpaces/knot_vector.hpp"
#include "BSplineLib/Utilities/containers.hpp"
#include "BSplineLib/Utilities/error_handling.hpp"
#include "BSplineLib/Utilities/named_type.hpp"

namespace bsplinelib::parameter_spaces {

/// Per-axis periodicity mode.  Old constructors default to
/// Periodicity::NonPeriodic so that existing (clamped) splines keep their
/// representation.
enum class Periodicity { NonPeriodic = 0, Periodic = 1 };

template<int para_dim>
using PeriodicModes = Array<Periodicity, para_dim>;

/// Minimum tolerances / numeric limits enforced on periodic axes.  The
/// contracted numeric ranges are |knot| <= 1e6, period length L >= 1e-4 and
/// nonzero knot spacings >= 1e-6, so a uniform tolerance of 1e-9 is always
/// smaller than any knot spacing while it absorbs floating point roundoff.
constexpr double kPeriodicKnotTolerance{1e-9};
constexpr double kPeriodicMinimumPeriodLength{1e-4};
constexpr double kPeriodicMinimumKnotSpacing{1e-6};
constexpr double kPeriodicMaximumAbsoluteKnot{1e6};

namespace detail {

inline bool PeriodicNearlyEqual(double lhs, double rhs, double tolerance) {
  return std::abs(lhs - rhs) <= tolerance;
}

} // namespace detail

/// A PeriodicAxis describes one periodic parameter axis through its
/// fundamental knot multiset a = t(0) <= ... < b = a + L (b is not part of
/// the fundamental multiset).  The infinite knot column satisfies
/// t(i + m) = t(i) + L where m is the number of fundamental knots.
///
/// Finite storage keeps the knots t(-p), ..., t(m + p) (m + 2p + 1 values)
/// and m + p control points, the last p of which are periodic copies of the
/// first p (homogeneous copies for rational splines).
///
/// All members are inline: the templated parameter space and spline code
/// instantiates them in other translation units that are not linked against
/// periodic_axis.o, while periodic_axis.cpp remains the compiled anchor
/// translation unit of this module.
class PeriodicAxis {
public:
  using Knot_ = double;
  using Knots_ = Vector<Knot_>;

  PeriodicAxis() = default;

  /// @brief Constructs a periodic axis of degree p from a fundamental knot
  /// multiset (non-decreasing, b = a + L not contained) and its period
  /// length L.
  PeriodicAxis(Degree degree,
               Knots_ fundamental_knots,
               Knot_ period_length,
               Tolerance tolerance = kPeriodicKnotTolerance)
      : active_{true},
        degree_{degree},
        fundamental_(std::move(fundamental_knots)),
        period_length_(period_length) {
    ThrowIfInvalid(tolerance);
  }

  /// @brief Infers a periodic axis from a finite knot vector storing
  /// t(-p) .. t(m + p) and validates the periodic extension.
  static PeriodicAxis FromFiniteKnotVector(
      KnotVector const& finite_knot_vector,
      Degree degree,
      Tolerance tolerance = kPeriodicKnotTolerance) {
    Knots_ const& finite = finite_knot_vector.GetKnots();
    int const p = degree;
    int const finite_size = static_cast<int>(finite.size());
    if (p < 2 || p > 3) {
      throw DomainError(
          "PeriodicAxis: only degrees 2 and 3 are supported.");
    }
    // Finite storage holds t(-p) .. t(m + p): m + 2p + 1 entries.
    int const m = finite_size - 2 * p - 1;
    if (m <= p) {
      throw DomainError(
          "PeriodicAxis: number of fundamental knots must exceed the "
          "degree.");
    }
    Knots_ fundamental(m);
    for (int i{}; i < m; ++i)
      fundamental[i] = finite[p + i];
    // t(m) = t(0) + L; t(0) and t(m) sit at finite positions p and p + m.
    Knot_ const period_length = finite[p + m] - finite[p];

    PeriodicAxis axis(p, std::move(fundamental), period_length, tolerance);
    axis.ValidateFiniteKnots(finite_knot_vector, tolerance);
    return axis;
  }

  bool IsActive() const { return active_; }
  Degree GetDegree() const { return degree_; }

  /// @brief Number m of knots in the fundamental multiset.
  int GetNumberOfFundamentalKnots() const {
    return static_cast<int>(fundamental_.size());
  }
  Knots_ const& GetFundamentalKnots() const { return fundamental_; }
  Knot_ GetDomainStart() const { return fundamental_.front(); }
  Knot_ GetPeriodLength() const { return period_length_; }
  Knot_ GetDomainEnd() const {
    return fundamental_.front() + period_length_;
  }

  /// @brief Knot t(i) of the infinite knot column for any integer index.
  Knot_ KnotAt(int index) const {
    int const m = static_cast<int>(fundamental_.size());
    int local = index % m;
    if (local < 0)
      local += m;
    int const block = (index - local) / m; // floor division
    return fundamental_[local]
           + static_cast<double>(block) * period_length_;
  }

  /// @brief Builds the finite storage t(-p) .. t(m + p).
  Knots_ FiniteKnots() const {
    int const p = degree_;
    int const m = static_cast<int>(fundamental_.size());
    Knots_ finite(m + 2 * p + 1);
    for (int i{-p}; i <= m + p; ++i)
      finite[i + p] = KnotAt(i);
    return finite;
  }

  /// @brief Maps any finite real parameter to the canonical half-open
  /// interval [a, b).  A parameter exactly at b is mapped to a.
  Knot_ MapToDomain(Knot_ parametric_coordinate) const {
    double const a = fundamental_.front();
    double const b = a + period_length_;
    double mapped = parametric_coordinate;
    if (mapped < a || mapped >= b) {
      double const shifts = std::floor((mapped - a) / period_length_);
      mapped -= shifts * period_length_;
      // Guard against roundoff landing exactly on b or below a.
      if (mapped >= b)
        mapped -= period_length_;
      if (mapped < a)
        mapped = a;
    }
    return mapped;
  }

  /// @brief Local span index j in [0, m - 1] such that
  /// t(j) <= u_mapped < t(j + 1); repeated knots take the right side.
  int FindLocalSpan(Knot_ parametric_coordinate) const {
    double const mapped = MapToDomain(parametric_coordinate);
    int const m = static_cast<int>(fundamental_.size());
    // First index i with fundamental_[i] > mapped, so the span is i - 1.
    // upper_bound semantics make repeated knots take the right-hand span.
    int i = static_cast<int>(
        std::upper_bound(fundamental_.begin(),
                         fundamental_.end(),
                         mapped + kPeriodicKnotTolerance)
        - fundamental_.begin());
    int span = i - 1;
    if (span < 0)
      span = 0;
    if (span >= m)
      span = m - 1;
    return span;
  }

  /// @brief Multiplicity of the orbit equivalent to the given parameter in
  /// the fundamental multiset (0 if it is not a knot orbit).
  int DetermineOrbitMultiplicity(Knot_ parametric_coordinate,
                                 Tolerance tolerance =
                                     kPeriodicKnotTolerance) const {
    int const index = FindOrbitIndex(parametric_coordinate, tolerance);
    if (index < 0)
      return 0;
    int multiplicity{1};
    for (int i = index + 1;
         i < static_cast<int>(fundamental_.size())
         && detail::PeriodicNearlyEqual(fundamental_[i],
                                        fundamental_[index],
                                        tolerance);
         ++i) {
      ++multiplicity;
    }
    return multiplicity;
  }

  /// @brief True if u is periodic-equivalent to one of the fundamental
  /// knots.
  bool IsKnotOrbit(Knot_ parametric_coordinate,
                   Tolerance tolerance = kPeriodicKnotTolerance) const {
    return FindOrbitIndex(parametric_coordinate, tolerance) >= 0;
  }

  /// @brief Inserts the whole translation orbit of u.  The requested
  /// multiplicity is clipped so that fundamental multiplicity never
  /// exceeds p; the performed number of insertions is returned.
  Multiplicity InsertOrbit(Knot_ parametric_coordinate,
                           Multiplicity multiplicity,
                           Tolerance tolerance = kPeriodicKnotTolerance) {
    if (multiplicity <= 0)
      return Multiplicity{};
    int const p = degree_;
    int const current =
        DetermineOrbitMultiplicity(parametric_coordinate, tolerance);
    int const allowed = std::max(0, p - current);
    int const performed = std::min(multiplicity, allowed);
    if (performed == 0)
      return Multiplicity{};

    double const mapped = MapToDomain(parametric_coordinate);
    // Insertion position: after all knots not greater than mapped.
    int position = static_cast<int>(
        std::upper_bound(fundamental_.begin(),
                         fundamental_.end(),
                         mapped + tolerance)
        - fundamental_.begin());
    fundamental_.insert(fundamental_.begin() + position,
                        performed,
                        mapped);
    // The period length is unchanged; only the multiset grew.
    return Multiplicity{performed};
  }

  /// @brief Moves the parameter seam to s, which must be equivalent to an
  /// existing fundamental knot.  Only the ordering of the knot orbits
  /// changes; the infinite knot column as a set is invariant.
  void RotateSeam(Knot_ parametric_coordinate,
                  Tolerance tolerance = kPeriodicKnotTolerance) {
    int const index = FindOrbitIndex(parametric_coordinate, tolerance);
    if (index < 0) {
      throw InvalidArgument(
          "RotatePeriodicSeam: the parameter must be periodic-equivalent "
          "to an existing fundamental knot.");
    }
    double const new_start = MapToDomain(parametric_coordinate);
    int const m = static_cast<int>(fundamental_.size());
    Knots_ rotated(m);
    for (int i{}; i < m; ++i) {
      double value = fundamental_[(index + i) % m];
      if (value < new_start - tolerance)
        value += period_length_;
      rotated[i] = value;
    }
    fundamental_ = std::move(rotated);
  }

  /// @brief Validates a finite knot vector against this axis.
  void ValidateFiniteKnots(KnotVector const& finite_knot_vector,
                           Tolerance tolerance =
                               kPeriodicKnotTolerance) const {
    Knots_ const& finite = finite_knot_vector.GetKnots();
    int const p = degree_;
    int const m = static_cast<int>(fundamental_.size());
    if (finite.size() != static_cast<std::size_t>(m + 2 * p + 1)) {
      throw DomainError(
          "PeriodicAxis: finite knot vector must store t(-p)..t(m+p).");
    }
    Knots_ const expected = FiniteKnots();
    for (int i{}; i < static_cast<int>(expected.size()); ++i) {
      if (!detail::PeriodicNearlyEqual(finite[i], expected[i], tolerance)) {
        throw DomainError(
            "PeriodicAxis: finite knot vector does not satisfy the "
            "periodic extension t(i+m)=t(i)+L.");
      }
    }
  }

  /// @brief Index q of the fundamental knot orbit equivalent to u, or -1.
  int FindOrbitIndex(Knot_ parametric_coordinate,
                     Tolerance tolerance = kPeriodicKnotTolerance) const {
    double const mapped = MapToDomain(parametric_coordinate);
    for (int i{}; i < static_cast<int>(fundamental_.size()); ++i) {
      if (detail::PeriodicNearlyEqual(fundamental_[i], mapped, tolerance))
        return i;
    }
    return -1;
  }

private:
  void ThrowIfInvalid(Tolerance tolerance) const {
    int const m = static_cast<int>(fundamental_.size());
    int const p = degree_;

    if (p < 2 || p > 3) {
      throw DomainError(
          "PeriodicAxis: only degrees 2 and 3 are supported.");
    }
    if (m <= p) {
      throw DomainError(
          "PeriodicAxis: the number of fundamental knots must exceed the "
          "degree.");
    }
    for (double const knot : fundamental_) {
      if (!std::isfinite(knot)
          || std::abs(knot) > kPeriodicMaximumAbsoluteKnot) {
        throw DomainError(
            "PeriodicAxis: knots must be finite with absolute value <= "
            "1e6.");
      }
    }
    for (int i{1}; i < m; ++i) {
      if (fundamental_[i] + tolerance < fundamental_[i - 1]) {
        throw DomainError(
            "PeriodicAxis: the fundamental knot multiset must be "
            "non-decreasing.");
      }
    }
    double const a = fundamental_.front();
    double const b = a + period_length_;
    if (!(period_length_ >= kPeriodicMinimumPeriodLength)) {
      throw DomainError(
          "PeriodicAxis: the period length must be at least 1e-4.");
    }
    if (!(b > fundamental_.back() + 0.0)) {
      throw DomainError(
          "PeriodicAxis: the domain end b = a + L must not be part of the "
          "fundamental knot multiset.");
    }
    for (int i{1}; i < m; ++i) {
      double const spacing = fundamental_[i] - fundamental_[i - 1];
      if (spacing > 0.0 && spacing < kPeriodicMinimumKnotSpacing) {
        throw DomainError(
            "PeriodicAxis: nonzero knot spacings must be at least 1e-6.");
      }
    }
    // The closing span from the last fundamental knot to b = a + L is also
    // a nonzero knot spacing.
    if (b - fundamental_.back() < kPeriodicMinimumKnotSpacing) {
      throw DomainError(
          "PeriodicAxis: the closing knot spacing to b must be at least "
          "1e-6.");
    }
    // Fundamental multiplicity must never exceed the degree.
    int run{1};
    for (int i{1}; i <= m; ++i) {
      if (i < m
          && detail::PeriodicNearlyEqual(fundamental_[i],
                                         fundamental_[i - 1],
                                         tolerance))
        ++run;
      else {
        if (run > p) {
          throw DomainError(
              "PeriodicAxis: fundamental knot multiplicity exceeds the "
              "degree.");
        }
        run = 1;
      }
    }
  }

  bool active_{false};
  Degree degree_{};
  Knots_ fundamental_;
  Knot_ period_length_{};
};

/// @brief Reduces an integer index (possibly negative) modulo modulus.
inline int WrapPeriodicIndex(int index, int modulus) {
  int const wrapped = index % modulus;
  return wrapped < 0 ? wrapped + modulus : wrapped;
}

/// @brief Validates the periodic control net padding of a spline against its
/// periodic modes.  For every periodic axis d, control points at local
/// positions 0..p-1 must equal those at m..m+p-1 (all homogeneous components
/// are compared, so rational splines validate their complete homogeneous
/// control points).  Rational splines additionally require strictly positive
/// finite weights in the last component.
template<int para_dim>
inline void ValidatePeriodicControlPadding(
    const bsplinelib::utilities::containers::Data<double, 2>& coordinates,
    const Array<int, para_dim>& number_of_control_points,
    const PeriodicModes<para_dim>& modes,
    const Array<int, para_dim>& fundamental_sizes,
    const Array<int, para_dim>& degrees,
    bool is_rational) {
  int const geometric_dimension = coordinates.Shape()[1];
  int total{1};
  for (int d{}; d < para_dim; ++d)
    total *= number_of_control_points[d];
  if (coordinates.Shape()[0] != total) {
    throw DomainError(
        "Periodic construction: control net size does not match the "
        "parameter space.");
  }

  auto flat_index = [&](const Array<int, para_dim>& multi) {
    int index{multi[0]};
    int stride{number_of_control_points[0]};
    for (int d{1}; d < para_dim; ++d) {
      index += multi[d] * stride;
      stride *= number_of_control_points[d];
    }
    return index;
  };

  for (int d{}; d < para_dim; ++d) {
    if (modes[d] != Periodicity::Periodic)
      continue;
    int const m = fundamental_sizes[d];
    int const p = degrees[d];
    Array<int, para_dim> multi{};
    int other_product{1};
    for (int k{}; k < para_dim; ++k)
      if (k != d)
        other_product *= number_of_control_points[k];
    for (int linear{}; linear < other_product; ++linear) {
      int residual{linear};
      for (int k{para_dim - 1}; k >= 0; --k) {
        if (k == d)
          continue;
        multi[k] = residual % number_of_control_points[k];
        residual /= number_of_control_points[k];
      }
      for (int j{}; j < p; ++j) {
        multi[d] = j;
        int const first = flat_index(multi);
        multi[d] = m + j;
        int const padded = flat_index(multi);
        for (int c{}; c < geometric_dimension; ++c) {
          if (std::abs(coordinates(first, c) - coordinates(padded, c))
              > kPeriodicKnotTolerance) {
            throw DomainError(
                "Periodic construction: control point padding is not "
                "periodic on axis "
                + std::to_string(d) + ".");
          }
        }
      }
    }
  }

  if (is_rational) {
    int const weight_column = geometric_dimension - 1;
    for (int i{}; i < coordinates.Shape()[0]; ++i) {
      double const weight = coordinates(i, weight_column);
      if (!std::isfinite(weight) || weight <= 0.0) {
        throw DomainError(
            "Periodic construction: all weights must be finite and "
            "strictly positive.");
      }
    }
  }
}

/// @brief Tensor-product combination used by periodic evaluation.  Basis
/// values are combined per axis.  On a periodic axis the finite slot array
/// stores Q_i = P_(i mod m) with the last p slots copying the first p, so
/// local basis position k addresses fundamental control
/// (first_non_zero + k - p) mod m; non-periodic axes use first_non_zero + k
/// directly.  A single routine handles mixed periodic / clamped surfaces.
template<int para_dim, typename BasisPerDimension>
inline void PeriodicTensorCombine(
    const BasisPerDimension& basis_per_dimension,
    const Array<int, para_dim>& first_non_zero,
    const Array<int, para_dim>& number_of_control_points,
    const Array<int, para_dim>& fundamental_sizes,
    const Array<int, para_dim>& degrees,
    const PeriodicModes<para_dim>& modes,
    const double* coordinates,
    int geometric_dimension,
    double* evaluated) {
  for (int c{}; c < geometric_dimension; ++c)
    evaluated[c] = 0.0;

  Array<int, para_dim> extent{};
  int total{1};
  for (int d{}; d < para_dim; ++d) {
    extent[d] = static_cast<int>(basis_per_dimension[d].size());
    total *= extent[d];
  }

  for (int linear{}; linear < total; ++linear) {
    int residual{linear};
    double basis_value{1.0};
    int flat{0};
    int stride{1};
    for (int d{}; d < para_dim; ++d) {
      int const k = residual % extent[d];
      residual /= extent[d];
      basis_value *= basis_per_dimension[d][k];
      int index = first_non_zero[d] + k;
      if (modes[d] == Periodicity::Periodic) {
        // Shift by the degree onto the infinite-column basis index and
        // wrap into the fundamental control range.
        index = WrapPeriodicIndex(index - degrees[d],
                                  fundamental_sizes[d]);
      }
      flat += index * stride;
      stride *= number_of_control_points[d];
    }
    for (int c{}; c < geometric_dimension; ++c)
      evaluated[c] += basis_value * coordinates[flat * geometric_dimension + c];
  }
}

} // namespace bsplinelib::parameter_spaces

#endif // SOURCES_PARAMETERSPACES_PERIODIC_AXIS_HPP_
