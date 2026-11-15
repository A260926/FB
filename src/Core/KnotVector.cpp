#include "libnurbs/Core/KnotVector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "libnurbs/Algorithm/MathUtils.hpp"

using namespace std;
using namespace libnurbs;

// Numeric limits for knot values. The public contract bounds knot values and
// coordinates by 1e4 and requires distinct knots to be separated by at least
// 1e-6. A tiny slack absorbs representation noise from affine remapping.
constexpr Numeric KNOT_ABS_LIMIT = 1e4;
constexpr Numeric KNOT_MIN_SPACING = 1e-6;
constexpr Numeric KNOT_RANGE_SLACK = 1e-12;

namespace
{
    // Validate a raw knot sequence: non-decreasing, bounded, with distinct
    // knots separated by at least the minimum spacing, and with matching end
    // multiplicities (the structural property of a clamped knot vector).
    bool ValidateKnots(const vector<Numeric>& knots)
    {
        if (knots.empty()) return false;
        for (Numeric val : knots)
        {
            if (!std::isfinite(val)) return false;
            if (std::abs(val) > KNOT_ABS_LIMIT * (1 + KNOT_RANGE_SLACK)) return false;
        }
        for (size_t i = 1; i < knots.size(); ++i)
        {
            Numeric delta = knots[i] - knots[i - 1];
            if (delta < 0) return false;
            if (delta > 0 && delta < KNOT_MIN_SPACING * (1 - 1e-9)) return false;
        }

        // Ends must be clamped with the same multiplicity. A two-value vector
        // is accepted as a degenerate but usable sequence.
        if (knots.size() >= 3)
        {
            const Numeric first = knots.front();
            const Numeric last = knots.back();
            if (!(last > first)) return false;
            int first_multiplicity = 0;
            while (first_multiplicity < (int)knots.size() &&
                   knots[first_multiplicity] == first)
            {
                ++first_multiplicity;
            }
            int last_multiplicity = 0;
            while (last_multiplicity < (int)knots.size() &&
                   knots[knots.size() - 1 - last_multiplicity] == last)
            {
                ++last_multiplicity;
            }
            if (first_multiplicity != last_multiplicity) return false;
        }
        else if (knots.size() == 2 && !(knots[1] > knots[0]))
        {
            return false;
        }
        return true;
    }

    bool ValidateKnotsForDegree(const vector<Numeric>& knots, int degree)
    {
        if (degree < 1) return false;
        if (static_cast<int>(knots.size()) < 2 * degree + 2) return false;
        if (!ValidateKnots(knots)) return false;

        const int count = static_cast<int>(knots.size());
        const Numeric first = knots[degree];
        const Numeric last = knots[count - degree - 1];
        if (!(last > first)) return false;

        // Clamped: the first degree+1 knots share the domain start, the last
        // degree+1 knots share the domain end.
        for (int i = 0; i <= degree; ++i)
        {
            if (knots[i] != first) return false;
            if (knots[count - 1 - i] != last) return false;
        }

        // Interior multiplicities must not exceed the degree. The clamped end
        // groups are allowed to have degree+1 repeats.
        int multiplicity = 1;
        for (size_t i = 1; i < knots.size(); ++i)
        {
            if (knots[i] == knots[i - 1])
            {
                ++multiplicity;
            }
            else
            {
                multiplicity = 1;
            }
            const bool interior = knots[i] > first && knots[i] < last;
            if (interior && multiplicity > degree) return false;
        }
        return true;
    }
} // namespace


KnotVector::KnotVector(const vector<KnotPair>& knots)
{
    for (const auto& knot : knots)
    {
        for (int i = 0; i < knot.Multiplicity; ++i)
        {
            m_Values.emplace_back(knot.Value);
        }
    }
    if (!ValidateKnots(m_Values)) throw invalid_argument("knots");
}

KnotVector KnotVector::Uniform(int degree, int knots_count)
{
    const int additional_knots_count = knots_count - (degree + 1) * 2;
    if (additional_knots_count < 0) throw invalid_argument("knots_count");
    vector<Numeric> knots;
    knots.reserve(knots_count);
    for (int i = 0; i < degree + 1; ++i)
    {
        knots.push_back(0.0);
    }
    const Numeric t = 1.0 / (additional_knots_count + 1);
    for (int i = 1; i <= additional_knots_count; ++i)
    {
        knots.push_back(i * t);
    }
    for (int i = 0; i < degree + 1; ++i)
    {
        knots.push_back(1.0);
    }
    return KnotVector{knots};
}

KnotVector::KnotVector(const vector<Numeric>& values)
{
    if (!ValidateKnots(values)) throw invalid_argument("values");
    m_Values = values;
}

vector<KnotVector::KnotPair> KnotVector::GetKnotPairs() const
{
    if (m_Values.empty()) return {};
    vector<KnotPair> result;
    int len = (int)m_Values.size();
    Numeric last_value = m_Values[0];
    int multiplicity{1};
    for (int i = 1; i < len; ++i)
    {
        Numeric val = m_Values[i];
        if (val != last_value)
        {
            result.emplace_back(i, last_value, multiplicity);
            last_value = val;
            multiplicity = 1;
        }
        else ++multiplicity;
    }
    result.emplace_back(len - multiplicity, last_value, multiplicity);
    return result;
}

bool KnotVector::IsValid() const
{
    if (m_Values.size() < 4) return false;
    if (!ValidateKnots(m_Values)) return false;
    return m_Values.front() != m_Values.back();
}

void KnotVector::ValidateValues() const
{
    if (!ValidateKnots(m_Values)) throw std::invalid_argument("knots");
}

void KnotVector::Validate(int degree) const
{
    if (!ValidateKnotsForDegree(m_Values, degree))
    {
        throw std::invalid_argument("knots");
    }
}

std::pair<Numeric, Numeric> KnotVector::GetDomain(int degree) const
{
    Validate(degree);
    return {m_Values[degree], m_Values[(int)m_Values.size() - degree - 1]};
}

bool KnotVector::IsClamped() const
{
    if (m_Values.size() < 2) return false;
    int count0, count1;
    for (count0 = 0; count0 < (int)m_Values.size(); ++count0)
    {
        if (m_Values[count0] != m_Values.front()) break;
    }
    for (count1 = (int)m_Values.size() - 1; count1 >= 0; --count1)
    {
        if (m_Values[count1] != m_Values.back()) break;
    }
    return count0 == (int)m_Values.size() - 1 - count1;
}

bool KnotVector::IsClamped(int degree) const
{
    if (degree < 1 || (int)m_Values.size() < 2 * degree + 2) return false;
    const int count = (int)m_Values.size();
    const Numeric first = m_Values[degree];
    const Numeric last = m_Values[count - degree - 1];
    if (!(last > first)) return false;
    for (int i = 0; i <= degree; ++i)
    {
        if (m_Values[i] != first) return false;
        if (m_Values[count - 1 - i] != last) return false;
    }
    return true;
}

bool KnotVector::IsUniform() const
{
    Numeric interval = 0;
    for (int i = 1; i < (int)m_Values.size(); ++i)
    {
        Numeric val = m_Values[i];
        Numeric curr = val - m_Values[i - 1];
        if (interval == 0) interval = curr;
        else if (!Approx(curr, interval) && curr != 0) return false;
    }
    return true;
}

int KnotVector::FindSpanIndex(int degree, Numeric u) const
{
    Validate(degree);
    const int count = (int)m_Values.size();
    const Numeric first = m_Values[degree];
    const Numeric last = m_Values[count - degree - 1];
    // Accept values that are within rounding noise of the domain boundary.
    const Numeric tolerance = 1e-12 * std::max(Numeric(1), std::max(std::abs(first), std::abs(last)));
    if (!(u >= first - tolerance && u <= last + tolerance))
    {
        throw std::invalid_argument("parameter outside of the knot domain");
    }
    Numeric value = std::clamp(u, first, last);

    int index_first_span = degree;
    int index_last_span = count - degree - 2;
    if (value == last) return index_last_span;
    if (value == first) return index_first_span;
    auto it = std::upper_bound(m_Values.begin(), m_Values.end(), value);
    int index_result = (int)std::distance(m_Values.begin(), it);
    return index_result - 1;
}

KnotVector::KnotSpan KnotVector::FindSpan(Numeric u) const
{
    int degree = DetectDegree();
    Validate(degree);
    const int count = (int)m_Values.size();
    const Numeric first = m_Values[degree];
    const Numeric last = m_Values[count - degree - 1];
    const Numeric tolerance = 1e-12 * std::max(Numeric(1), std::max(std::abs(first), std::abs(last)));
    if (!(u >= first - tolerance && u <= last + tolerance))
    {
        throw std::invalid_argument("parameter outside of the knot domain");
    }
    Numeric value = std::clamp(u, first, last);

    auto pairs = GetKnotPairs();
    if (pairs.size() < 2) return {};
    for (int i = (int)pairs.size() - 2; i >= 0; --i)
    {
        const auto& pair = pairs[i];
        if (pair.Value <= value)
        {
            return {i, pair, pairs[i + 1]};
        }
    }
    return {};
}

int KnotVector::DetectDegree() const
{
    if (m_Values.empty()) throw std::out_of_range("The knot vector is invalid.");
    const Numeric first = m_Values.front();
    for (int i = 0; i < (int)m_Values.size(); ++i)
    {
        if (m_Values[i] != first) return i - 1;
    }
    throw std::out_of_range("The knot vector is invalid.");
}

int KnotVector::GetMultiplicity(Numeric u) const
{
    int result = 0;
    for (Numeric val : m_Values)
    {
        if (Approx(val, u)) ++result;
    }
    return result;
}

int KnotVector::InsertKnot(Numeric value, int times)
{
    if (times < 1) throw std::invalid_argument("times");
    if (m_Values.empty()) throw std::invalid_argument("knots");
    int degree = DetectDegree();
    Validate(degree);
    const int count = (int)m_Values.size();
    const Numeric first = m_Values[degree];
    const Numeric last = m_Values[count - degree - 1];
    if (!(value > first && value < last))
    {
        throw std::invalid_argument("inserted knot must be strictly inside the domain");
    }
    auto it = std::upper_bound(m_Values.begin(), m_Values.end(), value);
    int i = (int)std::distance(m_Values.begin(), it);
    m_Values.insert(it, times, value);
    return i - 1;
}

void KnotVector::Reverse()
{
    if (m_Values.empty()) return;
    const Numeric a = m_Values.front();
    const Numeric b = m_Values.back();
    ranges::reverse(m_Values);
    for (auto& t : m_Values)
    {
        t = a + b - t;
    }
}
