#include "libnurbs/Curve/Curve.hpp"

#include <cassert>
#include <fstream>
#include <iomanip>
#include <stack>
#include <mdspan>

#include "libnurbs/Algorithm/DegreeAlgo.hpp"
#include "libnurbs/Algorithm/KnotInsertion.hpp"
#include "libnurbs/Algorithm/KnotRemoval.hpp"
#include "libnurbs/Algorithm/MathUtils.hpp"
#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Utils/Serialization.hpp"


using namespace std;
using namespace libnurbs;

Curve& Curve::LoadFromFile(const string& filename)
{
    ifstream file(filename);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    return LoadFromFile(file);
}

Curve& Curve::LoadFromFile(std::istream& is)
{
    // Read header line
    std::string header_line;
    std::getline(is, header_line);
    if (is.fail())
    {
        throw std::runtime_error("Failed to read header line.");
    }
    std::stringstream ss_header(header_line);
    std::string magic, mode, type;
    ss_header >> magic >> mode >> type;

    if (magic != LIBNURBS_MAGIC)
    {
        throw std::runtime_error("Invalid file magic: " + magic);
    }

    bool mode_binary;
    if (mode == "TXT")
    {
        mode_binary = false;
    }
    else if (mode == "BIN")
    {
        mode_binary = true;
    }
    else
    {
        throw std::runtime_error("Unknown serialization mode: " + mode);
    }

    if (type != "CURVE")
    {
        throw std::runtime_error("Object type mismatch. Expected CURVE, got: " + type);
    }

    if (mode_binary)
    {
        is.read(reinterpret_cast<char*>(&Degree), sizeof(int));
        if (is.fail())
        {
            throw std::runtime_error("Failed to read Degree.");
        }

        Utils::ReadKnotVectorFromStream(Knots, is);

        int cp_count;
        is.read(reinterpret_cast<char*>(&cp_count), sizeof(int));
        if (is.fail() || cp_count < 0)
        {
            throw std::runtime_error("Failed to read ControlPoints count.");
        }
        ControlPoints.resize(cp_count);
        for (int i = 0; i < cp_count; ++i)
        {
            Utils::ReadVec4FromStream(ControlPoints[i], is);
        }
        Knots.ValidateValues();
        return *this;
    }

    // TXT mode
    std::string line;
    std::string current_key;
    vector<std::string> content_lines;

    auto process_key = [&](const std::string& key, const vector<std::string>& lines)
    {
        if (key == "Degree")
        {
            if (lines.size() != 1)
            {
                throw std::runtime_error("Degree should have exactly one value line.");
            }
            std::stringstream ss(lines[0]);
            ss >> Degree;
            if (ss.fail())
            {
                throw std::runtime_error("Failed to parse Degree value.");
            }
        }
        else if (key == "Knots")
        {
            for (const auto& l : lines)
            {
                std::stringstream ss(l);
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    Utils::TrimString(token);
                    if (!token.empty())
                    {
                        Numeric knot;
                        std::stringstream num_stream(token);
                        num_stream >> knot;
                        if (num_stream.fail())
                        {
                            throw std::runtime_error("Failed to parse knot value: " + token);
                        }
                        Knots.Values().push_back(knot);
                    }
                }
            }
        }
        else if (key == "ControlPoints")
        {
            for (const auto& l : lines)
            {
                std::stringstream ss(l);
                std::string cp_str;
                while (std::getline(ss, cp_str, ','))
                {
                    Utils::TrimString(cp_str);
                    if (!cp_str.empty())
                    {
                        Vec4 vec;
                        Utils::ReadVec4FromText(vec, cp_str);
                        ControlPoints.push_back(vec);
                    }
                }
            }
        }
        else
        {
            throw std::runtime_error("Unknown key encountered: " + key);
        }
    };

    while (std::getline(is, line))
    {
        Utils::TrimString(line);
        if (line.empty())
            continue;

        if (line.front() == '*' && line.find(':') != std::string::npos)
        {
            // Process previous key
            if (!current_key.empty())
            {
                process_key(current_key, content_lines);
                content_lines.clear();
            }

            // Extract key and possible inline value
            size_t colon_pos = line.find(':');
            current_key      = line.substr(1, colon_pos - 1);
            Utils::TrimString(current_key);

            std::string value_part = line.substr(colon_pos + 1);
            Utils::TrimString(value_part);

            if (!value_part.empty())
            {
                content_lines.push_back(value_part);
            }
        }
        else
        {
            if (current_key.empty())
            {
                throw std::runtime_error("Content found before any key.");
            }
            content_lines.push_back(line);
        }
    }

    // Process the last key
    if (!current_key.empty())
    {
        process_key(current_key, content_lines);
    }
    Knots.ValidateValues();
    return *this;
}

void Curve::SaveToFile(const std::string& filename, bool binary_mode) const
{
    std::ofstream file;
    if (binary_mode)
    {
        file.open(filename, std::ios::binary);
    }
    else
    {
        file.open(filename);
    }

    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }

    SaveToFile(file, binary_mode);
}

void Curve::SaveToFile(std::ostream& os, bool binary_mode) const
{
    // Write header
    os << LIBNURBS_MAGIC << " " << (binary_mode ? "BIN" : "TXT") << " CURVE\n";

    if (binary_mode)
    {
        // Write binary data
        os.write(reinterpret_cast<const char*>(&Degree), sizeof(int));
        if (os.fail())
        {
            throw std::runtime_error("Failed to write Degree to binary stream.");
        }

        Utils::WriteKnotVectorToStream(Knots, os);

        int cp_count = static_cast<int>(ControlPoints.size());
        os.write(reinterpret_cast<const char*>(&cp_count), sizeof(int));
        if (os.fail())
        {
            throw std::runtime_error("Failed to write ControlPoints count to binary stream.");
        }

        for (const auto& cp : ControlPoints)
        {
            Utils::WriteVec4ToStream(cp, os);
        }
    }
    else
    {
        // Write textual data
        os << "*Degree: " << Degree << "\n";

        os << std::fixed << std::setprecision(std::numeric_limits<double>::digits10 + 2);

        os << "*Knots:\n";
        for (size_t i = 0; i < Knots.Values().size(); ++i)
        {
            os << Knots.Values()[i];
            if (i != Knots.Values().size() - 1)
            {
                os << ", ";
            }
        }
        os << "\n";

        os << "*ControlPoints:\n";
        for (const auto& cp : ControlPoints)
        {
            os << cp.x() << " " << cp.y() << " " << cp.z() << " " << cp.w() << ",\n";
        }
    }

    if (os.fail())
    {
        throw std::runtime_error("Failed to write data to stream.");
    }
}


Vec3 Curve::Evaluate(Numeric x) const
{
    int index_span = Knots.FindSpanIndex(Degree, x);
    VecX basis     = BSplineBasis::Evaluate(Degree, Knots.Values(), index_span, x);
    assert(basis.size() == Degree + 1);
    Vec4 result = Vec4::Zero();
    for (int i = 0; i <= Degree; i++)
    {
        auto point = ToHomo(ControlPoints[index_span - Degree + i]);
        result += basis(i) * point;
    }
    return result.head<3>() / result.w();
}


vector<Vec4> Curve::HomogeneousDerivative(Numeric x, int order) const
{
    int index_span = Knots.FindSpanIndex(Degree, x);
    MatX basis     = BSplineBasis::EvaluateAll(Degree, Knots.Values(), index_span, x, order);
    assert(basis.rows() == order + 1);
    assert(basis.cols() == Degree + 1);
    vector<Vec4> result(order + 1);
    for (int k = 0; k <= order; ++k)
    {
        Vec4 tmp = Vec4::Zero();
        for (int i = 0; i <= Degree; i++)
        {
            auto point = ToHomo(ControlPoints[index_span - Degree + i]);
            tmp.noalias() += point * basis(k, i);
        }
        result[k] = tmp;
    }
    return result;
}

Vec3 Curve::EvaluateDerivative(Numeric x, int order) const
{
    return EvaluateAll(x, order)[order];
}

vector<Vec3> Curve::EvaluateAll(Numeric x, int order) const
{
    auto homo_ders = HomogeneousDerivative(x, order);
    vector<Vec3> result(order + 1, Vec3::Zero());

    // Compute rational derivatives
    Numeric Wders0 = homo_ders[0].w();
    for (int k = 0; k <= order; k++)
    {
        Vec3 Aders = homo_ders[k].head<3>();
        for (int i = 1; i <= k; i++)
        {
            Numeric Wders = homo_ders[i].w();
            Aders.noalias() -= Binomial(k, i) * Wders * result[k - i];
        }
        result[k] = (Aders / Wders0);
    }
    return result;
}

bool Curve::IsRational() const
{
    const auto& cps = ControlPoints;
    if (cps.empty())
        return false;
    Numeric w = cps.front().w();
    for (size_t i = 1; i < cps.size(); ++i)
    {
        if (cps[i].w() != w)
            return true;
    }
    return false;
}

Numeric Curve::SearchParameter(const Vec3& point, Numeric init, Numeric epsilon, Numeric max_iteration_count) const
{
    const auto [domain_begin, domain_end] = GetDomain();
    // An initial guess outside the domain is projected onto the nearest boundary.
    Numeric init_value = std::clamp(init, domain_begin, domain_end);

    // Run the BFGS iteration in a normalised coordinate s = (u - a) / (b - a)
    // on [0, 1]. This makes the step scaling independent of the original
    // domain width, so an affine reparameterisation does not change the
    // iteration behaviour. All residuals stay spatial.
    const Numeric span = domain_end - domain_begin;
    auto to_parameter = [domain_begin, span](Numeric s) { return domain_begin + s * span; };
    auto to_normalized = [domain_begin, span](Numeric u) { return (u - domain_begin) / span; };

    auto Ri = [&point, this, &to_parameter](Numeric s) -> Vec3
    {
        return Evaluate(to_parameter(s)) - point;
    };

    // d/ds of the squared residual is d/du * (b - a).
    auto fi = [this, &Ri, span, &to_parameter](Numeric s) -> Numeric
    {
        auto ri = Ri(s);
        auto Cu = EvaluateDerivative(to_parameter(s), 1);
        return ri.dot(Cu) * span;
    };

    Numeric s_last           = to_normalized(init_value);
    Numeric current_residual = Ri(s_last).norm();
    Numeric Hk               = 1.0;

    auto line_search = [&](Numeric gk) -> Numeric
    {
        Numeric alpha                  = 1.0;
        Numeric c1                     = 1e-4;
        Numeric beta                   = 0.9;
        int max_line_search_iterations = 20;
        int ls_count                   = 0;

        while (ls_count++ < max_line_search_iterations)
        {
            Numeric s_trial = s_last - alpha * Hk * gk;
            s_trial         = std::clamp(s_trial, Numeric(0), Numeric(1));

            if (Ri(s_trial).norm() <= current_residual - c1 * alpha * gk * Hk * gk)
            {
                return s_trial;
            }
            alpha *= beta;
        }
        Numeric s_final = s_last - alpha * Hk * gk;
        return std::clamp(s_final, Numeric(0), Numeric(1));
    };

    int count = 0;
    while (!((current_residual < epsilon) || (count++ >= max_iteration_count)))
    {
        Numeric gk    = fi(s_last);
        Numeric s_new = line_search(gk);

        Numeric sk     = s_new - s_last;
        Numeric gk_new = fi(s_new);
        Numeric yk     = gk_new - gk;

        Numeric yk_dot_sk = yk * sk;
        if (std::abs(yk_dot_sk) < 1e-16)
        {
            Hk        = 1.0;
            yk_dot_sk = 1e-10;
        }

        Numeric rho = 1.0 / yk_dot_sk;
        Hk          = (1.0 - rho * yk * sk) * Hk * (1.0 - rho * sk * yk) + rho * sk * sk;

        s_last           = s_new;
        current_residual = Ri(s_last).norm();
    }
    return to_parameter(s_last);
}

Numeric Curve::BinarySearchParameter(const Vec3& point, Numeric epsilon, int max_iteration_count) const
{
    const auto [domain_begin, domain_end] = GetDomain();

    auto Ri = [&point, this](Numeric u) -> Numeric
    {
        return (Evaluate(u) - point).norm();
    };

    // Golden-section style search for the minimum of the spatial residual.
    // The bracket is driven by residual values, never by an original-domain
    // step size, so it behaves the same for any finite parameter domain.
    // epsilon is only ever compared against a spatial residual.
    const Numeric domain_width = domain_end - domain_begin;
    Numeric low  = domain_begin;
    Numeric high = domain_end;

    Numeric best        = domain_begin;
    Numeric best_residual = Ri(best);
    auto consider = [&](Numeric candidate)
    {
        Numeric residual = Ri(candidate);
        if (residual < best_residual)
        {
            best_residual = residual;
            best          = candidate;
        }
    };
    consider(domain_end);
    if (best_residual < epsilon) return best;

    int count = 0;
    while (count++ < max_iteration_count)
    {
        Numeric width = high - low;
        if (width <= 0) break;
        Numeric third = width / 3.0;
        Numeric m1    = low + third;
        Numeric m2    = high - third;
        if (!(m1 > low && m2 < high)) break;

        Numeric left_residual  = Ri(m1);
        Numeric right_residual = Ri(m2);
        if (left_residual < best_residual)
        {
            best_residual = left_residual;
            best          = m1;
        }
        if (right_residual < best_residual)
        {
            best_residual = right_residual;
            best          = m2;
        }
        if (best_residual < epsilon) return best;
        if (left_residual <= right_residual)
        {
            high = m2;
        }
        else
        {
            low = m1;
        }
        if (width <= 1e-13 * std::max(Numeric(1), domain_width)) break;
    }
    return best;
}

Curve Curve::InsertKnot(Numeric knot_value) const
{
    return InsertKnot(knot_value, 1);
}

Curve Curve::InsertKnot(Numeric knot_value, int times) const
{
    Curve result{*this};
    InsertKnotCurve(result.Knots, result.ControlPoints, result.Degree, knot_value, times);
    return result;
}

Curve Curve::InsertKnot(std::span<Numeric> knots_to_insert) const
{
    Curve result = *this;
    for (auto knot_value : knots_to_insert)
    {
        InsertKnotCurve(result.Knots, result.ControlPoints, result.Degree, knot_value, 1);
    }
    return result;
}

std::tuple<Curve, int> Curve::RemoveKnot(Numeric knot_remove, int times, Numeric tolerance) const
{
    if (times < 1) throw std::invalid_argument("times");
    const auto [domain_begin, domain_end] = GetDomain();
    if (!(knot_remove > domain_begin && knot_remove < domain_end))
    {
        throw std::invalid_argument("knot to remove must lie strictly inside the parameter domain");
    }

    Curve result{*this};
    int degree   = result.Degree;
    auto& points = result.ControlPoints;
    int t        = KnotRemoval(result.Knots, points, degree, knot_remove, times, tolerance);
    return {result, t};
}

Curve Curve::ElevateDegree(int times) const
{
    if (times < 1) throw std::invalid_argument("times");

    Curve result{*this};
    NurbsDegreeElevation(result.Knots, result.ControlPoints, result.Degree, times);
    return result;
}

Curve Curve::Transform(const Mat3x3& R) const
{
    Curve transformed_curve = *this;
    for (auto& point : transformed_curve.ControlPoints)
    {
        point.head<3>() = R * point.head<3>();
    }
    return transformed_curve;
}

vector<Curve> Curve::ExtractBezier() const
{
    const auto [domain_begin, domain_end] = GetDomain();
    Curve bezier = *this;
    auto pairs   = this->Knots.GetKnotPairs();
    for (auto& item : pairs)
    {
        Numeric u = item.Value;
        int mult  = item.Multiplicity;
        if (u <= domain_begin || u >= domain_end)
            continue;
        if (mult >= Degree)
            continue;
        bezier = bezier.InsertKnot(u, Degree - mult);
    }

    vector<Curve> segments;
    segments.reserve(pairs.size());
    const auto& ctrl = bezier.ControlPoints;
    int p            = bezier.Degree;

    for (int i = 0; i + p < (int)ctrl.size(); i += p)
    {
        Curve seg;
        seg.Degree = p;
        seg.Knots  = KnotVector::Uniform(p, 2 * p + 2);
        seg.ControlPoints.resize(p + 1);
        for (int k = 0; k <= p; ++k)
        {
            seg.ControlPoints[k] = ctrl[i + k];
        }
        segments.emplace_back(std::move(seg));
    }
    return segments;
}

BoundingBox Curve::GetBoundingBox(Numeric epsilon) const
{
    auto segments = this->ExtractBezier();
    int p         = this->Degree;

    auto to3 = [&](const Vec4& v) -> Vec3
    {
        return v.head<3>();
    };

    // if p <= 5, use static buffer
    vector<Vec4> b((p + 1) * (p + 1));
    using std_2dspan = mdspan<Vec4, extents<int, dynamic_extent, dynamic_extent>>;

    bool initialized = false;
    BoundingBox globalBox;
    for (auto& seg : segments)
    {
        stack<vector<Vec4>> stack;
        stack.push(std::move(seg.ControlPoints));
        while (!stack.empty())
        {
            auto ctrlPts = std::move(stack.top());
            stack.pop();

            // Bounding-box of control points net
            BoundingBox cpBox(to3(ctrlPts[0]), to3(ctrlPts[0]));
            for (size_t m = 1; m < ctrlPts.size(); ++m)
            {
                Vec3 p3 = to3(ctrlPts[m]);
                cpBox.ExpandToInclude(p3);
            }

            // Bounding-box of end points
            Vec3 A = to3(ctrlPts.front());
            Vec3 B = to3(ctrlPts.back());
            BoundingBox epBox(A, B);

            // is this condition working?
            bool small_enough = cpBox.Length().norm() <= epsilon;

            bool flat_enough = Approx(cpBox.Min, epBox.Min, 1e-6) &&
                               Approx(cpBox.Max, epBox.Max, 1e-6);

            if (small_enough || flat_enough)
            {
                if (!initialized)
                {
                    globalBox   = epBox;
                    initialized = true;
                }
                else
                {
                    globalBox.ExpandToInclude(epBox);
                }
                continue;
            }

            std_2dspan b_view{b.data(), p + 1, p + 1};
            // fill first row
            for (int s = 0; s <= p; ++s)
            {
                b_view[0, s] = ctrlPts[s];
            }

            for (int r = 1; r <= p; ++r)
            {
                for (int s = 0; s <= p - r; ++s)
                {
                    b_view[r, s] = b_view[r - 1, s] * 0.5 + b_view[r - 1, s + 1] * 0.5;
                }
            }
            vector<Vec4> L(p + 1), R(p + 1);
            for (int r = 0; r <= p; ++r)
            {
                L[r] = b_view[r, 0];
                R[r] = b_view[p - r, r];
            }
            stack.push(std::move(R));
            stack.push(std::move(L));
        }
    }
    return globalBox;
}
