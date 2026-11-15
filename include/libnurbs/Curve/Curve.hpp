#pragma once

#include <span>
#include <vector>
#include <tuple>
#include <string>
#include <libnurbs/Core/KnotVector.hpp>
#include <libnurbs/Core/Typedefs.hpp>
#include <libnurbs/Core/BoundingBox.hpp>

using std::vector;
using std::string;

namespace libnurbs
{
    class Curve
    {
    public:
        int Degree{INVALID_DEGREE};
        KnotVector Knots{};
        vector<Vec4> ControlPoints{};

    public:
        Curve() = default;

        Curve& LoadFromFile(const string& filename);

        Curve& LoadFromFile(std::istream& is);

        void SaveToFile(const string& filename, bool binary_mode = false) const;

        void SaveToFile(std::ostream& os, bool binary_mode = false) const;


        [[nodiscard]] Vec3 Evaluate(Numeric x) const;

        [[nodiscard]] Vec3 EvaluateDerivative(Numeric x, int order) const;

        [[nodiscard]] vector<Vec3> EvaluateAll(Numeric x, int order) const;

        [[nodiscard]] bool IsRational() const;

        /**
         * @brief Validate degree, knot domain and control points (weights in [0.1, 10],
         *        finite coordinates with |coordinate| <= 1e4, consistent counts).
         *        Throws std::invalid_argument on any violation.
         */
        void Validate() const;

        [[nodiscard]] Numeric DomainStart() const { return Knots.DomainStart(Degree); }

        [[nodiscard]] Numeric DomainEnd() const { return Knots.DomainEnd(Degree); }

        [[nodiscard]] Numeric SearchParameter(const Vec3& point,
                                              Numeric init                = 0.5,
                                              Numeric epsilon             = 1e-8,
                                              Numeric max_iteration_count = 512) const;

        [[nodiscard]] Numeric BinarySearchParameter(const Vec3& point,
                                                    Numeric epsilon         = 1e-8,
                                                    int max_iteration_count = 512) const;


        [[nodiscard]] Curve InsertKnot(Numeric knot_value) const;

        [[nodiscard]] Curve InsertKnot(Numeric knot_value, int times) const;

        [[nodiscard]] Curve InsertKnot(std::span<Numeric> knots_to_insert) const;

        [[nodiscard]] std::tuple<Curve, int> RemoveKnot(Numeric knot_remove,
                                                        int times         = 1,
                                                        Numeric tolerance = 1e-12) const;

        [[nodiscard]] Curve ElevateDegree(int times = 1) const;

        [[nodiscard]] Curve Transform(const Mat3x3& R) const;

        vector<Curve> ExtractBezier() const;

        BoundingBox GetBoundingBox(Numeric epsilon = 1e-3) const;

    private:
        [[nodiscard]] vector<Vec4> HomogeneousDerivative(Numeric x, int order) const;
    };
}
