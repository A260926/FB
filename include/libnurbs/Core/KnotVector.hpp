#pragma once

#include <iostream>
#include <libnurbs/Core/Typedefs.hpp>
#include <utility>
#include <vector>

using std::vector;

namespace libnurbs
{
    class KnotVector
    {
    private:
        vector<Numeric> m_Values{};

    public:
        struct KnotPair;
        struct KnotSpan;

    public:
        KnotVector() = default;

        explicit KnotVector(const vector<Numeric>& values);

        explicit KnotVector(const vector<KnotPair>& knots);

        static KnotVector Uniform(int degree, int knots_count);

        [[nodiscard]] vector<KnotPair> GetKnotPairs() const;

        [[nodiscard]] auto& Values() const
        {
            return m_Values;
        }

        [[nodiscard]] auto& Values()
        {
            return m_Values;
        }

        [[nodiscard]] int Count() const
        {
            return (int)m_Values.size();
        }

        Numeric& operator[](int index)
        {
            return m_Values[index];
        }

        const Numeric& operator()(int index) const
        {
            return m_Values[index];
        }

        [[nodiscard]] bool IsValid() const;

        /**
         * @brief Validate the raw knot values only (no degree information).
         *
         * Checks finiteness, numeric bounds, non-decreasing order, minimum
         * spacing between distinct knots and matching end multiplicities.
         *
         * @throws std::invalid_argument when the values are not usable.
         */
        void ValidateValues() const;

        /**
         * @brief Validate this knot vector for evaluation with the given degree.
         *
         * A valid knot vector is finite, non-decreasing, stays within the
         * supported numeric range, has distinct knots separated by at least
         * 1e-6 and is clamped at both ends. Interior knot multiplicities must
         * not exceed the degree.
         *
         * @throws std::invalid_argument when the vector is not usable.
         */
        void Validate(int degree) const;

        /**
         * @brief Return the effective parameter domain [knots[degree], knots[count-degree-1]].
         * @throws std::invalid_argument when the knot vector is invalid for the degree.
         */
        [[nodiscard]] std::pair<Numeric, Numeric> GetDomain(int degree) const;

        [[nodiscard]] bool IsClamped() const;

        [[nodiscard]] bool IsClamped(int degree) const;

        [[nodiscard]] bool IsUniform() const;

        /**
         * @brief Find the index of the span contains u into the knot vector.
         * @param degree
         * @param u Parameter in the effective domain of the knot vector.
         * @return Span1, [Span1,Span2).
         * @throws std::invalid_argument when u lies outside the effective domain.
         */
        [[nodiscard]] int FindSpanIndex(int degree, Numeric u) const;

        [[nodiscard]] KnotSpan FindSpan(Numeric u) const;

        [[nodiscard]] int DetectDegree() const;

        [[nodiscard]] int GetMultiplicity(Numeric u) const;

        /**
         * \brief Insert a knot value into the knot vector.
         * \param value Parameter strictly inside the effective domain.
         * \param times
         * \return Span index in original KnotVector that contains the new knot value.
         * \throws std::invalid_argument when the value is not strictly interior.
         */
        int InsertKnot(Numeric value, int times = 1);

        /**
         * \brief Reverse the parameter direction using u -> a + b - u,
         *        where [a, b] is the knot vector's domain.
         */
        void Reverse();
    };

    struct KnotVector::KnotPair
    {
        int Index{INVALID_INDEX};
        Numeric Value{INVALID_VALUE};
        int Multiplicity{INVALID_VALUE};

        KnotPair() = default;

        KnotPair(int index, Numeric value, int multiplicity)
                : Index(index), Value(value), Multiplicity(multiplicity) {}
    };

    struct KnotVector::KnotSpan
    {
        int Index{INVALID_INDEX};
        KnotPair Left{};
        KnotPair Right{};

        KnotSpan() = default;

        KnotSpan(int index, const KnotPair& left, const KnotPair& right)
                : Index(index), Left(left), Right(right) {}
    };

}
