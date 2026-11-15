#include "libnurbs/Core/KnotVector.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "libnurbs/Algorithm/MathUtils.hpp"

using namespace std;
using namespace libnurbs;

namespace
{
    // Supported numeric envelope for knot values.
    constexpr Numeric MAX_ABS_KNOT = 1e4;
    constexpr Numeric MIN_KNOT_GAP = 1e-6;

    bool ValidateKnots(const vector<Numeric>& knots)
    {
        if (knots.empty()) return false;
        Numeric last_value = knots.front();
        if (!std::isfinite(last_value) || std::abs(last_value) > MAX_ABS_KNOT) return false;
        for (size_t i = 1; i < knots.size(); ++i)
        {
            Numeric val = knots[i];
            if (!std::isfinite(val) || std::abs(val) > MAX_ABS_KNOT) return false;
            if (val < last_value) return false;
            if (val != last_value && val - last_value < MIN_KNOT_GAP) return false;
            last_value = val;
        }
        // The domain must have a positive width.
        if (!(knots.back() > knots.front())) return false;
        return true;
    }
}


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
    const int additional_knots_count = knots_count - (degree + 1) * 2;;
    assert(additional_knots_count >= 0);
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
    return ValidateKnots(m_Values);
}

bool KnotVector::IsClamped() const
{
    if (m_Values.empty()) return false;
    int count0, count1;
    for (count0 = 0; count0 < (int)m_Values.size(); ++count0)
    {
        if (m_Values[count0] != m_Values.front()) break;
    }
    for (count1 = (int)m_Values.size() - 1; count1 >= 0; --count1)
    {
        if (m_Values[count1] != m_Values.back()) break;
    }
    int trailing = (int)m_Values.size() - 1 - count1;
    return count0 == trailing && count0 >= 2;
}

bool KnotVector::IsUniform() const
{
    Numeric interval = 0;
    for (int i = 1; i < m_Values.size(); ++i)
    {
        Numeric val = m_Values[i];
        Numeric curr = val - m_Values[i - 1];
        if (interval == 0) interval = curr;
        else if (!Approx(curr, interval) && curr != 0) return false;
    }
    return true;
}

Numeric KnotVector::DomainStart(int degree) const
{
    if (degree < 0 || degree >= Count()) throw invalid_argument("degree");
    return m_Values[degree];
}

Numeric KnotVector::DomainEnd(int degree) const
{
    if (degree < 0 || degree >= Count()) throw invalid_argument("degree");
    return m_Values[Count() - degree - 1];
}

void KnotVector::Validate(int degree) const
{
    if (degree < 1 || degree > 3)
    {
        throw invalid_argument("degree out of supported range [1, 3]");
    }
    if (!ValidateKnots(m_Values))
    {
        throw invalid_argument("invalid knot values");
    }
    const int count = Count();
    if (count < 2 * (degree + 1))
    {
        throw invalid_argument("knot count too small for degree");
    }
    // Multiplicity runs: ends must be exactly degree + 1 (clamped),
    // internal knots must not exceed the degree.
    int run = 1;
    auto check_run = [&](Numeric value, int multiplicity)
    {
        bool is_end = (value == m_Values.front() || value == m_Values.back());
        if (is_end)
        {
            if (multiplicity != degree + 1)
                throw invalid_argument("clamped end multiplicity must equal degree + 1");
        }
        else
        {
            if (multiplicity > degree)
                throw invalid_argument("internal knot multiplicity exceeds degree");
        }
    };
    for (int i = 1; i < count; ++i)
    {
        if (m_Values[i] == m_Values[i - 1])
        {
            ++run;
        }
        else
        {
            check_run(m_Values[i - 1], run);
            run = 1;
        }
    }
    check_run(m_Values.back(), run);
}

int KnotVector::FindSpanIndex(int degree, Numeric u) const
{
    int index_first_span = degree;
    int index_last_span = static_cast<int>(m_Values.size()) - degree - 2;
    if (index_last_span < 0 || index_first_span > index_last_span)
    {
        throw invalid_argument("knot vector too small for degree");
    }
    const Numeric domain_start = m_Values[index_first_span];
    const Numeric domain_end   = m_Values[index_last_span + 1];
    if (!(u >= domain_start && u <= domain_end))
    {
        throw invalid_argument("parameter outside of the knot domain");
    }
    if (u == domain_end) return index_last_span;
    // binary search
    auto it = std::upper_bound(m_Values.begin(), m_Values.end(), u);
    int index_result = (int)std::distance(m_Values.begin(), it);
    return index_result - 1;
}

KnotVector::KnotSpan KnotVector::FindSpan(Numeric u) const
{
    auto pairs = GetKnotPairs();
    if (pairs.size() < 2)
    {
        throw invalid_argument("knot vector has no span");
    }
    if (u < pairs.front().Value || u > pairs.back().Value)
    {
        throw invalid_argument("parameter outside of the knot domain");
    }
    for (int i = (int)pairs.size() - 2; i >= 0; --i)
    {
        const auto& pair = pairs[i];
        if (pair.Value <= u)
        {
            return {i, pair, pairs[i + 1]};
        }
    }
    return {};
}

int KnotVector::DetectDegree() const
{
    for (int i = 0; i < (int)m_Values.size(); ++i)
    {
        if (m_Values[i] != m_Values.front()) return i - 1;
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
    if (m_Values.empty() || !(value > m_Values.front() && value < m_Values.back()))
    {
        throw invalid_argument("knot to insert must lie inside the open knot domain");
    }
    if (times < 1)
    {
        throw invalid_argument("insertion times must be positive");
    }
    auto it = std::upper_bound(m_Values.begin(), m_Values.end(), value);
    // Distinct knots must stay separated by at least MIN_KNOT_GAP.
    if (it != m_Values.end() && *it != value && *it - value < MIN_KNOT_GAP)
    {
        throw invalid_argument("knot to insert too close to an existing knot");
    }
    if (it != m_Values.begin())
    {
        Numeric prev = *(it - 1);
        if (prev != value && value - prev < MIN_KNOT_GAP)
        {
            throw invalid_argument("knot to insert too close to an existing knot");
        }
    }
    int i = (int)std::distance(m_Values.begin(), it);
    m_Values.insert(it, times, value);
    return i - 1;
}

void KnotVector::Reverse()
{
    // Map u -> a + b - u so the reversed axis covers the same original domain.
    const Numeric a = m_Values.front();
    const Numeric b = m_Values.back();
    ranges::reverse(m_Values);
    for (auto& t : m_Values)
    {
        t = a + b - t;
    }
}
