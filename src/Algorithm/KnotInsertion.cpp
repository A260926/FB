#include "libnurbs/Algorithm/KnotInsertion.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "libnurbs/Core/KnotVector.hpp"

namespace libnurbs
{
    namespace
    {
        // Count knots exactly equal to value (matching how the vector stores
        // repeated values).
        int ExactMultiplicity(const KnotVector& knot_vector, Numeric value)
        {
            int count = 0;
            for (Numeric knot : knot_vector.Values())
            {
                if (knot == value) ++count;
            }
            return count;
        }

        // One Boehm knot insertion. Returns the extra control point in
        // homogeneous coordinates appended into `result`.
        std::vector<Vec4> InsertSingleKnot(const KnotVector& knot_vector,
                                           const std::vector<Vec4>& homogeneous_points,
                                           int degree,
                                           Numeric value)
        {
            const int p = degree;
            const int n = static_cast<int>(homogeneous_points.size()) - 1;
            const int k = knot_vector.FindSpanIndex(p, value);
            const auto& knots = knot_vector.Values();

            std::vector<Vec4> result(homogeneous_points.size() + 1, Vec4::Zero());
            for (int i = 0; i <= k - p; ++i) result[i] = homogeneous_points[i];
            for (int i = k + 1; i <= n + 1; ++i) result[i] = homogeneous_points[i - 1];
            for (int i = k - p + 1; i <= k; ++i)
            {
                const Numeric denom = knots[i + p] - knots[i];
                const Numeric alpha = denom == 0.0 ? 0.0 : (value - knots[i]) / denom;
                result[i] = alpha * homogeneous_points[i] + (1.0 - alpha) * homogeneous_points[i - 1];
            }
            return result;
        }
    } // namespace

    void InsertKnotCurve(KnotVector& knot_vector,
                         std::vector<Vec4>& points,
                         int degree,
                         Numeric value,
                         int times)
    {
        if (times < 1) throw std::invalid_argument("times");
        knot_vector.Validate(degree);
        if (points.size() != static_cast<size_t>(knot_vector.Count() - degree - 1))
        {
            throw std::invalid_argument("control point count does not match the knot vector");
        }

        const auto [domain_begin, domain_end] = knot_vector.GetDomain(degree);
        const Numeric tolerance =
            1e-12 * std::max(Numeric(1), std::max(std::abs(domain_begin), std::abs(domain_end)));
        if (!(value > domain_begin - tolerance && value < domain_end + tolerance))
        {
            throw std::invalid_argument("inserted knot must lie inside the parameter domain");
        }
        Numeric knot_value = value;
        if (knot_value <= domain_begin) knot_value = std::nextafter(domain_begin, domain_end);
        if (knot_value >= domain_end) knot_value = std::nextafter(domain_end, domain_begin);

        const int existing = ExactMultiplicity(knot_vector, knot_value);
        if (existing + times > degree)
        {
            throw std::invalid_argument("inserted knot multiplicity exceeds the degree");
        }

        std::vector<Vec4> homogeneous(points.size());
        for (size_t i = 0; i < points.size(); ++i)
        {
            homogeneous[i] = ToHomo(points[i]);
        }

        auto& knots = knot_vector.Values();
        for (int insertion = 0; insertion < times; ++insertion)
        {
            const int k = knot_vector.FindSpanIndex(degree, knot_value);
            homogeneous = InsertSingleKnot(knot_vector, homogeneous, degree, knot_value);
            knots.insert(knots.begin() + k + 1, knot_value);
        }

        points.resize(homogeneous.size());
        for (size_t i = 0; i < homogeneous.size(); ++i)
        {
            points[i] = FromHomo(homogeneous[i]);
        }
    }
}
