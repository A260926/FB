#include "libnurbs/Surface/Surface.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "libnurbs/Algorithm/DegreeAlgo.hpp"
#include "libnurbs/Algorithm/KnotInsertion.hpp"
#include "libnurbs/Algorithm/KnotRemoval.hpp"
#include "libnurbs/Algorithm/MathUtils.hpp"
#include "libnurbs/Basis/BSplineBasis.hpp"
#include "libnurbs/Utils/Serialization.hpp"

using namespace std;
using namespace libnurbs;

Surface& Surface::LoadFromFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    return LoadFromFile(file);
}

Surface& Surface::LoadFromFile(std::istream& is)
{
    // Read header line
    std::string header_line;
    std::getline(is, header_line);
    if (is.fail())
    {
        throw std::runtime_error("Failed to read header line from stream.");
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

    if (type != "SURFACE")
    {
        throw std::runtime_error("Object type mismatch. Expected SURFACE, got: " + type);
    }

    if (mode_binary)
    {
        // Binary mode
        // Read degrees
        is.read(reinterpret_cast<char*>(&DegreeU), sizeof(int));
        is.read(reinterpret_cast<char*>(&DegreeV), sizeof(int));
        if (is.fail())
        {
            throw std::runtime_error("Failed to read Degrees from binary stream.");
        }

        // Read knot vectors
        Utils::ReadKnotVectorFromStream(KnotsU, is);
        Utils::ReadKnotVectorFromStream(KnotsV, is);

        // Read control point grid dimensions
        is.read(reinterpret_cast<char*>(&ControlPoints.UCount), sizeof(int));
        is.read(reinterpret_cast<char*>(&ControlPoints.VCount), sizeof(int));
        if (is.fail() || ControlPoints.UCount <= 0 || ControlPoints.VCount <= 0)
        {
            throw std::runtime_error("Failed to read control point grid dimensions.");
        }

        // Read control points
        int cp_count = ControlPoints.UCount * ControlPoints.VCount;
        ControlPoints.Values.resize(cp_count);
        for (int i = 0; i < cp_count; ++i)
        {
            Utils::ReadVec4FromStream(ControlPoints.Values[i], is);
        }
        KnotsU.ValidateValues();
        KnotsV.ValidateValues();
    }
    else
    {
        // Text mode
        std::string line;
        std::string current_key;
        std::vector<std::string> content_lines;

        auto process_key = [&](const std::string& key, const std::vector<std::string>& lines)
        {
            if (key == "DegreeU")
            {
                if (lines.size() != 1)
                {
                    throw std::runtime_error("DegreeU should have exactly one value line.");
                }
                std::stringstream ss(lines[0]);
                ss >> DegreeU;
                if (ss.fail())
                {
                    throw std::runtime_error("Failed to parse DegreeU value.");
                }
            }
            else if (key == "DegreeV")
            {
                if (lines.size() != 1)
                {
                    throw std::runtime_error("DegreeV should have exactly one value line.");
                }
                std::stringstream ss(lines[0]);
                ss >> DegreeV;
                if (ss.fail())
                {
                    throw std::runtime_error("Failed to parse DegreeV value.");
                }
            }
            else if (key == "KnotsU")
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
                                throw std::runtime_error("Failed to parse KnotsU value: " + token);
                            }
                            KnotsU.Values().push_back(knot);
                        }
                    }
                }
            }
            else if (key == "KnotsV")
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
                                throw std::runtime_error("Failed to parse KnotsV value: " + token);
                            }
                            KnotsV.Values().push_back(knot);
                        }
                    }
                }
            }
            else if (key == "ControlPoints")
            {
                // Expecting first line to contain UCount and VCount
                if (lines.empty())
                {
                    throw std::runtime_error("ControlPoints section missing grid dimensions.");
                }
                std::stringstream ss_dims(lines[0]);
                ss_dims >> ControlPoints.UCount >> ControlPoints.VCount;
                if (ss_dims.fail() || ControlPoints.UCount <= 0 || ControlPoints.VCount <= 0)
                {
                    throw std::runtime_error("Invalid control point grid dimensions.");
                }

                // Read control points
                int expected_cp_count = ControlPoints.UCount * ControlPoints.VCount;
                if (static_cast<int>(lines.size() - 1) != expected_cp_count)
                {
                    throw std::runtime_error("ControlPoints count does not match grid dimensions.");
                }

                ControlPoints.Values.resize(expected_cp_count);
                for (int i = 1; i < static_cast<int>(lines.size()); ++i)
                {
                    std::string cp_line = lines[i];
                    Utils::TrimString(cp_line);
                    if (!cp_line.empty())
                    {
                        Vec4 vec;
                        Utils::ReadVec4FromText(vec, cp_line);
                        ControlPoints.Values[i - 1] = vec;
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
            if (line.empty()) continue;

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
                current_key = line.substr(1, colon_pos - 1);
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
    }
    KnotsU.ValidateValues();
    KnotsV.ValidateValues();
    return *this;
}

void Surface::SaveToFile(const std::string& filename, bool binary_mode) const
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


void Surface::SaveToFile(std::ostream& os, bool binary_mode) const
{
    // Write header
    os << LIBNURBS_MAGIC << " " << (binary_mode ? "BIN" : "TXT") << " SURFACE\n";

    if (binary_mode)
    {
        // Write binary data

        // Write degrees
        os.write(reinterpret_cast<const char*>(&DegreeU), sizeof(int));
        os.write(reinterpret_cast<const char*>(&DegreeV), sizeof(int));
        if (os.fail())
        {
            throw std::runtime_error("Failed to write Degrees to binary stream.");
        }

        // Write knot vectors
        Utils::WriteKnotVectorToStream(KnotsU, os);
        Utils::WriteKnotVectorToStream(KnotsV, os);

        // Write control point grid dimensions
        os.write(reinterpret_cast<const char*>(&ControlPoints.UCount), sizeof(int));
        os.write(reinterpret_cast<const char*>(&ControlPoints.VCount), sizeof(int));
        if (os.fail())
        {
            throw std::runtime_error("Failed to write control point grid dimensions.");
        }

        // Write control points
        int cp_count = ControlPoints.UCount * ControlPoints.VCount;
        for (int i = 0; i < cp_count; ++i)
        {
            Utils::WriteVec4ToStream(ControlPoints.Values[i], os);
        }
    }
    else
    {
        // Text mode

        // Write degrees
        os << "*DegreeU: " << DegreeU << "\n";
        os << "*DegreeV: " << DegreeV << "\n";

        os << std::fixed << std::setprecision(std::numeric_limits<double>::digits10 + 2);

        // Write knot vectors
        os << "*KnotsU:\n";
        for (size_t i = 0; i < KnotsU.Values().size(); ++i)
        {
            os << KnotsU.Values()[i];
            if (i != KnotsU.Values().size() - 1)
            {
                os << ", ";
            }
        }
        os << "\n";

        os << "*KnotsV:\n";
        for (size_t i = 0; i < KnotsV.Values().size(); ++i)
        {
            os << KnotsV.Values()[i];
            if (i != KnotsV.Values().size() - 1)
            {
                os << ", ";
            }
        }
        os << "\n";

        // Write control points
        os << "*ControlPoints:\n";
        os << ControlPoints.UCount << " " << ControlPoints.VCount << "\n";

        for (int v = 0; v < ControlPoints.VCount; ++v)
        {
            for (int u = 0; u < ControlPoints.UCount; ++u)
            {
                const auto& cp = ControlPoints.Get(u, v);
                os << cp.x() << " " << cp.y() << " " << cp.z() << " " << cp.w() << "\n";
            }
        }
    }

    if (os.fail())
    {
        throw std::runtime_error("Failed to write data to stream.");
    }
}


Vec3 Surface::Evaluate(Numeric u, Numeric v) const
{
    int index_span_u = KnotsU.FindSpanIndex(DegreeU, u);
    int index_span_v = KnotsV.FindSpanIndex(DegreeV, v);
    VecX basis_u = BSplineBasis::Evaluate(DegreeU, KnotsU.Values(), index_span_u, u);
    VecX basis_v = BSplineBasis::Evaluate(DegreeV, KnotsV.Values(), index_span_v, v);
    assert(basis_u.size() == DegreeU + 1);
    assert(basis_v.size() == DegreeV + 1);

    Vec4 result = Vec4::Zero();
    int index_pre_u = index_span_u - DegreeU;
    int index_pre_v = index_span_v - DegreeV;

    for (int j = 0; j <= DegreeV; j++)
    {
        int index_v = index_pre_v + j;
        Vec4 tmp = Vec4::Zero();
        for (int i = 0; i <= DegreeU; i++)
        {
            int index_u = index_pre_u + i;
            auto point = ToHomo(ControlPoints.Get(index_u, index_v));
            tmp.noalias() += basis_u(i) * point;
        }
        result.noalias() += basis_v(j) * tmp;
    }
    return result.head<3>() / result.w();
}

Vec3 Surface::EvaluateDerivative(Numeric u, Numeric v, int order_u, int order_v) const
{
    return EvaluateAll(u, v, order_u, order_v).Get(order_u, order_v);
}

Grid<Vec3> Surface::EvaluateAll(Numeric u, Numeric v, int order_u, int order_v) const
{
    auto homo_ders = HomogeneousDerivative(u, v, order_u, order_v);
    Grid<Vec3> result(order_u + 1, order_v + 1, Vec3::Zero());

    Numeric Wders00 = homo_ders.Get(0, 0).w();

    for (int ou = 0; ou <= order_u; ++ou)
    {
        for (int ov = 0; ov <= order_v; ++ov)
        {
            Vec3 Aders = homo_ders.Get(ou, ov).head<3>();
            for (int i = 1; i <= ou; ++i)
            {
                Numeric Wders = homo_ders.Get(i, 0).w();
                Aders.noalias() -= Binomial(ou, i) * Wders * result.Get(ou - i, ov);
            }

            for (int j = 1; j <= ov; ++j)
            {
                Numeric Wders = homo_ders.Get(0, j).w();
                Aders.noalias() -= Binomial(ov, j) * Wders * result.Get(ou, ov - j);
            }

            for (int i = 1; i <= ou; ++i)
            {
                const int bi = Binomial(ou, i);
                for (int j = 1; j <= ov; ++j)
                {
                    Numeric Wders = homo_ders.Get(i, j).w();
                    Aders.noalias() -= bi * Binomial(ov, j) * Wders * result.Get(ou - i, ov - j);
                }
            }
            result.Get(ou, ov) = Aders / Wders00;
        }
    }

    return result;
}

bool Surface::IsRational() const
{
    const auto& cps = ControlPoints.Values;
    if (cps.empty()) return false;
    Numeric w = cps.front().w();
    for (size_t i = 1; i < cps.size(); ++i)
    {
        if (cps[i].w() != w) return true;
    }
    return false;
}

auto Surface::SearchParameter(const Vec3& point, Numeric init_u, Numeric init_v,
                              Numeric epsilon, Numeric max_iteration_count) const
    -> std::pair<Numeric, Numeric>
{
    const auto [u_begin, u_end] = GetDomainU();
    const auto [v_begin, v_end] = GetDomainV();
    // Initial guesses outside the domain are projected onto the nearest boundary.
    const Numeric init_u_clamped = std::clamp(init_u, u_begin, u_end);
    const Numeric init_v_clamped = std::clamp(init_v, v_begin, v_end);

    // Search in normalised coordinates s_u, s_v in [0, 1] so that the BFGS
    // step scaling is independent of the original domain widths. All
    // residuals remain spatial and the public signature is unchanged.
    const Numeric u_span = u_end - u_begin;
    const Numeric v_span = v_end - v_begin;

    auto to_uv = [&](const Vec2& s) -> Vec2
    {
        return Vec2{u_begin + s.x() * u_span, v_begin + s.y() * v_span};
    };

    using Mat2x2 = Eigen::Matrix<Numeric, 2, 2>;

    auto Ri = [&point, this, &to_uv](const Vec2& s) -> Vec3
    {
        const Vec2 uv = to_uv(s);
        return Evaluate(uv.x(), uv.y()) - point;
    };

    // Chain rule: d/ds = d/du * span_u, d/dv * span_v.
    auto Ki = [this, &Ri, &to_uv, u_span, v_span](const Vec2& s) -> Vec2
    {
        const Vec2 uv = to_uv(s);
        auto ri = Ri(s);
        auto Su = EvaluateDerivative(uv.x(), uv.y(), 1, 0);
        auto Sv = EvaluateDerivative(uv.x(), uv.y(), 0, 1);
        return Vec2{ri.dot(Su) * u_span, ri.dot(Sv) * v_span};
    };

    Vec2 s_last{ (init_u_clamped - u_begin) / u_span, (init_v_clamped - v_begin) / v_span };
    Numeric current_residual = Ri(s_last).norm();
    Mat2x2 Hk = Mat2x2::Identity();

    auto line_search = [&](const Vec2& gk) -> Vec2
    {
        Numeric alpha = 1.0;
        Numeric c1 = 1e-4;
        Numeric beta = 0.9;
        int max_line_search_iterations = 20;
        int ls_count = 0;

        while (ls_count++ < max_line_search_iterations)
        {
            Vec2 s_trial = s_last - alpha * Hk * gk;
            s_trial.x() = std::clamp(s_trial.x(), Numeric(0), Numeric(1));
            s_trial.y() = std::clamp(s_trial.y(), Numeric(0), Numeric(1));

            if (Ri(s_trial).norm() <= current_residual - c1 * alpha * gk.dot(Hk * gk))
            {
                return s_trial;
            }
            alpha *= beta;
        }
        Vec2 s_final = s_last - alpha * Hk * gk;
        s_final.x() = std::clamp(s_final.x(), Numeric(0), Numeric(1));
        s_final.y() = std::clamp(s_final.y(), Numeric(0), Numeric(1));
        return s_final;
    };

    int count = 0;
    while (!((current_residual < epsilon) || (count++ >= max_iteration_count)))
    {
        auto gk = Ki(s_last);
        Vec2 s_new = line_search(gk);

        Vec2 sk = s_new - s_last;
        Vec2 gk_new = Ki(s_new);
        Vec2 yk = gk_new - gk;

        Numeric yk_dot_sk = yk.dot(sk);
        if (std::abs(yk_dot_sk) < 1e-16)
        {
            Hk = Mat2x2::Identity();
            yk_dot_sk = 1e-10;
        }

        Numeric rho = 1.0 / yk_dot_sk;
        Mat2x2 I = Mat2x2::Identity();
        Hk = (I - rho * sk * yk.transpose()) * Hk * (I - rho * yk * sk.transpose()) + rho * sk * sk.transpose();

        s_last = s_new;
        current_residual = Ri(s_last).norm();
    }
    const Vec2 result = to_uv(s_last);
    return {result.x(), result.y()};
}

auto Surface::SearchParameterOn(const Vec3& point, int direction, Numeric constant,
                                Numeric init_value,
                                Numeric epsilon, Numeric max_iteration_count) const
    -> std::pair<Numeric, Numeric>
{
    const auto [u_begin, u_end] = GetDomainU();
    const auto [v_begin, v_end] = GetDomainV();
    const Numeric begin = direction == 0 ? v_begin : u_begin;
    const Numeric end   = direction == 0 ? v_end : u_end;

    if (!(constant >= (direction == 0 ? u_begin : v_begin) &&
          constant <= (direction == 0 ? u_end : v_end)))
    {
        throw std::invalid_argument("constant parameter outside of the surface domain");
    }

    // Search in a normalised coordinate s in [0, 1] for the same reason as
    // the 2D search: the step scaling must not depend on the original domain
    // width, and an affine reparameterisation must not change the iteration.
    const Numeric span = end - begin;
    auto to_value = [begin, span](Numeric s) { return begin + s * span; };
    auto to_normalized = [begin, span](Numeric val) { return (val - begin) / span; };

    auto Ri = [&point, this, direction, constant, &to_value](Numeric s) -> Vec3
    {
        const Numeric val = to_value(s);
        Numeric u = direction == 0 ? constant : val;
        Numeric v = direction == 1 ? constant : val;
        return Evaluate(u, v) - point;
    };

    auto fi = [this, &Ri, direction, constant, span, &to_value](Numeric s) -> Numeric
    {
        const Numeric val = to_value(s);
        Numeric u = direction == 0 ? constant : val;
        Numeric v = direction == 1 ? constant : val;
        auto ri = Ri(s);
        auto Cu = direction == 1
                      ? EvaluateDerivative(u, v, 1, 0)
                      : EvaluateDerivative(u, v, 0, 1);
        return ri.dot(Cu) * span;
    };

    Numeric s_last = to_normalized(std::clamp(init_value, begin, end));
    Numeric current_residual = Ri(s_last).norm();
    Numeric Hk = 1.0;

    auto line_search = [&](Numeric gk) -> Numeric
    {
        Numeric alpha = 1.0;
        Numeric c1 = 1e-4;
        Numeric beta = 0.9;
        int max_line_search_iterations = 20;
        int ls_count = 0;

        while (ls_count++ < max_line_search_iterations)
        {
            Numeric s_trial = s_last - alpha * Hk * gk;
            s_trial = std::clamp(s_trial, Numeric(0), Numeric(1));

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
        Numeric gk = fi(s_last);
        Numeric s_new = line_search(gk);

        Numeric sk = s_new - s_last;
        Numeric gk_new = fi(s_new);
        Numeric yk = gk_new - gk;

        Numeric yk_dot_sk = yk * sk;
        if (std::abs(yk_dot_sk) < 1e-16)
        {
            Hk = 1.0;
            yk_dot_sk = 1e-10;
        }

        Numeric rho = 1.0 / yk_dot_sk;
        Hk = (1.0 - rho * yk * sk) * Hk * (1.0 - rho * sk * yk) + rho * sk * sk;

        s_last = s_new;
        current_residual = Ri(s_last).norm();
    }

    // If the gradient step did not reach a spatial residual within epsilon,
    // fall back to a residual-driven interval reduction and keep whichever
    // candidate is spatially closer. Both are spatial measures, never raw UV
    // step sizes.
    if (!(current_residual < epsilon))
    {
        Numeric low  = Numeric(0);
        Numeric high = Numeric(1);
        int fallback_count = 0;
        while (fallback_count++ < max_iteration_count)
        {
            Numeric width = high - low;
            if (width <= 0) break;
            Numeric third = width / 3.0;
            Numeric m1    = low + third;
            Numeric m2    = high - third;
            if (!(m1 > low && m2 < high)) break;

            Numeric left_residual  = Ri(m1).norm();
            Numeric right_residual = Ri(m2).norm();
            if (left_residual <= right_residual)
            {
                high = m2;
            }
            else
            {
                low = m1;
            }
            if (width <= 1e-13) break;
        }
        Numeric candidates[3] = {low, (low + high) / 2.0, high};
        for (Numeric candidate : candidates)
        {
            Numeric residual = Ri(candidate).norm();
            if (residual < current_residual)
            {
                current_residual = residual;
                s_last           = candidate;
            }
        }
    }
    const Numeric val_final = to_value(s_last);
    return direction == 0
               ? std::make_pair(constant, val_final)
               : std::make_pair(val_final, constant);
}

auto Surface::BinarySearchParameterOn(const Vec3& point, int direction, Numeric constant,
                                      Numeric epsilon, Numeric max_iteration_count) const
    -> std::pair<Numeric, Numeric>
{
    const auto [u_begin, u_end] = GetDomainU();
    const auto [v_begin, v_end] = GetDomainV();
    const Numeric begin = direction == 0 ? v_begin : u_begin;
    const Numeric end   = direction == 0 ? v_end : u_end;

    if (!(constant >= (direction == 0 ? u_begin : v_begin) &&
          constant <= (direction == 0 ? u_end : v_end)))
    {
        throw std::invalid_argument("constant parameter outside of the surface domain");
    }

    auto Ri = [&point, this, direction, constant](Numeric val) -> Numeric
    {
        Numeric u = direction == 0 ? constant : val;
        Numeric v = direction == 1 ? constant : val;
        return (Evaluate(u, v) - point).norm();
    };

    // Minimise the spatial residual over the original-domain interval. The
    // bracket is compared by residual value only; epsilon is only ever
    // compared against a spatial residual, never used as a parameter step.
    const Numeric domain_width = end - begin;
    Numeric low  = begin;
    Numeric high = end;

    Numeric best          = begin;
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
    consider(end);
    auto make_result = [&](Numeric val)
    {
        return direction == 0 ? std::make_pair(constant, val) : std::make_pair(val, constant);
    };
    if (best_residual < epsilon) return make_result(best);

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
        if (best_residual < epsilon) return make_result(best);
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

    return make_result(best);
}


Surface Surface::InsertKnotU(Numeric knot_value) const
{
    return InsertKnotU(knot_value, 1);
}

Surface Surface::InsertKnotU(Numeric knot_value, int times) const
{
    KnotsU.Validate(DegreeU);
    if (times < 1) throw std::invalid_argument("times");
    if (ControlPoints.UCount != KnotsU.Count() - DegreeU - 1)
    {
        throw std::invalid_argument("U control point count does not match the knot vector");
    }
    Surface result{*this};
    KnotVector knots;
    std::vector<std::vector<Vec4>> rows;
    rows.reserve(ControlPoints.VCount);
    for (int j = 0; j < ControlPoints.VCount; ++j)
    {
        std::vector<Vec4> row = ControlPoints.GetV(j);
        KnotVector row_knots = KnotsU;
        InsertKnotCurve(row_knots, row, DegreeU, knot_value, times);
        if (j == 0) knots = row_knots;
        rows.push_back(std::move(row));
    }

    const int new_u_count = rows.empty() ? 0 : (int)rows.front().size();
    Grid<Vec4> new_grid(new_u_count, ControlPoints.VCount);
    for (int j = 0; j < ControlPoints.VCount; ++j)
    {
        for (int i = 0; i < new_u_count; ++i)
        {
            new_grid.Get(i, j) = rows[j][i];
        }
    }
    result.KnotsU = std::move(knots);
    result.ControlPoints = std::move(new_grid);
    return result;
}

Surface Surface::InsertKnotU(std::span<Numeric> knots_to_insert) const
{
    Surface result{*this};
    for (auto knot : knots_to_insert)
    {
        result = result.InsertKnotU(knot);
    }
    return result;
}

Surface Surface::InsertKnotV(Numeric knot_value) const
{
    return InsertKnotV(knot_value, 1);
}

Surface Surface::InsertKnotV(Numeric knot_value, int times) const
{
    KnotsV.Validate(DegreeV);
    if (times < 1) throw std::invalid_argument("times");
    if (ControlPoints.VCount != KnotsV.Count() - DegreeV - 1)
    {
        throw std::invalid_argument("V control point count does not match the knot vector");
    }
    Surface result{*this};
    KnotVector knots;

    std::vector<std::vector<Vec4>> columns;
    columns.reserve(ControlPoints.UCount);
    for (int i = 0; i < ControlPoints.UCount; ++i)
    {
        std::vector<Vec4> column = ControlPoints.GetU(i);
        KnotVector column_knots = KnotsV;
        InsertKnotCurve(column_knots, column, DegreeV, knot_value, times);
        if (i == 0) knots = column_knots;
        columns.push_back(std::move(column));
    }

    const int new_v_count = (int)columns.front().size();
    Grid<Vec4> new_grid(ControlPoints.UCount, new_v_count);
    for (int i = 0; i < ControlPoints.UCount; ++i)
    {
        for (int j = 0; j < new_v_count; ++j)
        {
            new_grid.Get(i, j) = columns[i][j];
        }
    }
    result.KnotsV = std::move(knots);
    result.ControlPoints = std::move(new_grid);
    return result;
}

Surface Surface::InsertKnotV(std::span<Numeric> knots_to_insert) const
{
    Surface result{*this};
    for (auto knot : knots_to_insert)
    {
        result = result.InsertKnotV(knot);
    }
    return result;
}

std::tuple<Surface, int> Surface::RemoveKnotU(Numeric knot_remove, int times, Numeric tolerance) const
{
    if (times < 1) throw std::invalid_argument("times");
    const auto [domain_begin, domain_end] = GetDomainU();
    if (!(knot_remove > domain_begin && knot_remove < domain_end))
    {
        throw std::invalid_argument("knot to remove must lie strictly inside the parameter domain");
    }
    Surface result{*this};
    auto& points_ref = result.ControlPoints;
    points_ref = {points_ref.UCount - times, points_ref.VCount};
    int t = 0;
    for (int it_v = 0; it_v < ControlPoints.VCount; ++it_v)
    {
        auto knots = result.KnotsU;
        auto points = this->ControlPoints.GetV(it_v);
        int t_tmp = KnotRemoval(knots, points, result.DegreeU, knot_remove, times, tolerance);
        if (t_tmp == 0 || it_v != 0 && t != t_tmp) return {*this, 0};
        if (it_v == ControlPoints.VCount - 1) result.KnotsU = knots;
        points_ref.SetV(it_v, points);
        t = t_tmp;
    }
    return {result, t};
}

std::tuple<Surface, int> Surface::RemoveKnotV(Numeric knot_remove, int times, Numeric tolerance) const
{
    if (times < 1) throw std::invalid_argument("times");
    const auto [domain_begin, domain_end] = GetDomainV();
    if (!(knot_remove > domain_begin && knot_remove < domain_end))
    {
        throw std::invalid_argument("knot to remove must lie strictly inside the parameter domain");
    }
    Surface result{*this};
    auto& points_ref = result.ControlPoints;
    points_ref = {points_ref.UCount, points_ref.VCount - times};
    int t = 0;
    for (int it_u = 0; it_u < ControlPoints.UCount; ++it_u)
    {
        auto knots = result.KnotsV;
        auto points = this->ControlPoints.GetU(it_u);
        int t_tmp = KnotRemoval(knots, points, result.DegreeV, knot_remove, times, tolerance);
        if (t_tmp == 0 || it_u != 0 && t != t_tmp) return {*this, 0};
        if (it_u == ControlPoints.UCount - 1) result.KnotsV = knots;
        points_ref.SetU(it_u, points);
        t = t_tmp;
    }
    return {result, t};
}

Surface Surface::ElevateDegreeU(int times) const
{
    if (times < 1) throw std::invalid_argument("times");
    Surface result{*this};
    KnotsU.Validate(DegreeU);

    std::vector<std::vector<Vec4>> rows;
    rows.reserve(ControlPoints.VCount);
    KnotVector knots;
    for (int it_v = 0; it_v < ControlPoints.VCount; ++it_v)
    {
        std::vector<Vec4> row = ControlPoints.GetV(it_v);
        KnotVector row_knots = KnotsU;
        int degree = DegreeU;
        NurbsDegreeElevation(row_knots, row, degree, times);
        if (it_v == 0) knots = row_knots;
        rows.push_back(std::move(row));
    }
    const int new_degree_u = DegreeU + times;

    const int new_u_count = rows.empty() ? 0 : (int)rows.front().size();
    Grid<Vec4> new_grid(new_u_count, ControlPoints.VCount);
    for (int j = 0; j < ControlPoints.VCount; ++j)
    {
        for (int i = 0; i < new_u_count; ++i)
        {
            new_grid.Get(i, j) = rows[j][i];
        }
    }
    result.KnotsU = std::move(knots);
    result.DegreeU = new_degree_u;
    result.ControlPoints = std::move(new_grid);
    return result;
}

Surface Surface::ElevateDegreeV(int times) const
{
    if (times < 1) throw std::invalid_argument("times");
    Surface result{*this};
    KnotsV.Validate(DegreeV);

    std::vector<std::vector<Vec4>> columns;
    columns.reserve(ControlPoints.UCount);
    KnotVector knots;
    for (int it_u = 0; it_u < ControlPoints.UCount; ++it_u)
    {
        std::vector<Vec4> column = ControlPoints.GetU(it_u);
        KnotVector column_knots = KnotsV;
        int degree = DegreeV;
        NurbsDegreeElevation(column_knots, column, degree, times);
        if (it_u == 0) knots = column_knots;
        columns.push_back(std::move(column));
    }
    const int new_degree_v = DegreeV + times;

    const int new_v_count = columns.empty() ? 0 : (int)columns.front().size();
    Grid<Vec4> new_grid(ControlPoints.UCount, new_v_count);
    for (int i = 0; i < ControlPoints.UCount; ++i)
    {
        for (int j = 0; j < new_v_count; ++j)
        {
            new_grid.Get(i, j) = columns[i][j];
        }
    }
    result.KnotsV = std::move(knots);
    result.DegreeV = new_degree_v;
    result.ControlPoints = std::move(new_grid);
    return result;
}

Surface Surface::Transform(const Mat3x3& R) const
{
    Surface transformed_surface = *this;
    for (auto& point : transformed_surface.ControlPoints.Values)
    {
        point.head<3>() = R * point.head<3>();
    }
    return transformed_surface;
}

Surface Surface::AlignParameterDomain(AlignAxis u_axis, AlignAxis v_axis)
{
    // 创建当前 Surface 的副本，以便进行操作
    Surface result = *this;

    // 检查控制点网格是否至少有 2 个控制点，以计算参数方向
    if (ControlPoints.UCount < 2 || ControlPoints.VCount < 2)
    {
        // 控制点太少，无法计算参数方向，直接返回原始 Surface
        return result;
    }

    // 1. 获取四个角点的控制点
    int u_max = ControlPoints.UCount - 1;
    int v_max = ControlPoints.VCount - 1;

    Vec3 S00 = ControlPoints.Get(0, 0).head<3>();
    Vec3 S10 = ControlPoints.Get(u_max, 0).head<3>();
    Vec3 S01 = ControlPoints.Get(0, v_max).head<3>();
    Vec3 S11 = ControlPoints.Get(u_max, v_max).head<3>();

    // 2. 计算包围盒
    Numeric min_x = std::min({S00.x(), S10.x(), S01.x(), S11.x()});
    Numeric max_x = std::max({S00.x(), S10.x(), S01.x(), S11.x()});
    Numeric min_y = std::min({S00.y(), S10.y(), S01.y(), S11.y()});
    Numeric max_y = std::max({S00.y(), S10.y(), S01.y(), S11.y()});
    Numeric min_z = std::min({S00.z(), S10.z(), S01.z(), S11.z()});
    Numeric max_z = std::max({S00.z(), S10.z(), S01.z(), S11.z()});

    Numeric dx = max_x - min_x;
    Numeric dy = max_y - min_y;
    Numeric dz = max_z - min_z;

    // 3. 确定主要的两个坐标轴和法向量
    std::vector<std::pair<Numeric, AlignAxis>> extents = {
        {dx, AlignAxis::XAxis},
        {dy, AlignAxis::YAxis},
        {dz, AlignAxis::ZAxis}
    };

    // 按照尺寸从大到小排序
    std::sort(extents.begin(),
              extents.end(),
              [](const auto& a, const auto& b)
              {
                  return a.first > b.first;
              });

    // 主要的两个方向（参数方向）
    AlignAxis major_axis1 = extents[0].second;
    AlignAxis major_axis2 = extents[1].second;
    // 法向量方向
    AlignAxis normal_axis = extents[2].second;

    // 4. 计算控制点之间的矢量
    struct VectorInfo
    {
        Vec3 vector;
        // 角度
        Numeric angle1; // 与 major_axis1 的夹角
        Numeric angle2; // 与 major_axis2 的夹角
    };
    std::vector<VectorInfo> vectors;

    auto compute_angle = [](const Vec3& vec, AlignAxis axis) -> Numeric
    {
        Vec3 axis_vector;
        switch (axis)
        {
        case AlignAxis::XAxis:
            axis_vector = Vec3::UnitX();
            break;
        case AlignAxis::YAxis:
            axis_vector = Vec3::UnitY();
            break;
        case AlignAxis::ZAxis:
            axis_vector = Vec3::UnitZ();
            break;
        }
        // 计算夹角的余弦值
        Numeric cos_angle = vec.normalized().dot(axis_vector);
        // 为了比较方便，使用夹角的绝对值
        return std::acos(std::abs(cos_angle));
    };

    // 计算非零矢量并计算其与主要方向的夹角
    auto add_vector_info = [&](const Vec3& vec)
    {
        if (vec.norm() > 1e-6)
        {
            VectorInfo info;
            info.vector = vec;
            info.angle1 = compute_angle(vec, major_axis1);
            info.angle2 = compute_angle(vec, major_axis2);
            vectors.push_back(info);
        }
    };

    add_vector_info(S10 - S00);
    add_vector_info(S01 - S00);
    add_vector_info(S11 - S10);
    add_vector_info(S11 - S01);

    if (vectors.empty())
    {
        // 无法确定参数方向，返回原始 Surface
        return result;
    }

    // 5. 为每个矢量分配最接近的主要方向
    AlignAxis current_u_axis, current_v_axis;
    int u_sign = 0, v_sign = 0;

    if (vectors.size() < 4)
    {
        int count_axis1 = 0;
        int count_axis2 = 0;
        Numeric du = 0;
        Numeric dv = 0;

        auto GetAxisValue = [](const Vec3& point, AlignAxis axis) -> Numeric
        {
            switch (axis)
            {
            case AlignAxis::XAxis:
                return point.x();
            case AlignAxis::YAxis:
                return point.y();
            case AlignAxis::ZAxis:
                return point.z();
            }
            return 0.0;
        };

        for (const auto& info : vectors)
        {
            if (info.angle1 < info.angle2)
            {
                // 分配给 major_axis1
                ++count_axis1;
                // 累加在该轴上的增量
                du += GetAxisValue(info.vector, major_axis1);
            }
            else
            {
                // 分配给 major_axis2
                ++count_axis2;
                dv += GetAxisValue(info.vector, major_axis2);
            }
        }

        if (count_axis1 >= count_axis2)
        {
            current_u_axis = major_axis1;
            current_v_axis = major_axis2;
            u_sign = (du >= 0) ? 1 : -1;
            v_sign = (dv >= 0) ? 1 : -1;
        }
        else
        {
            current_u_axis = major_axis2;
            current_v_axis = major_axis1;
            u_sign = (dv >= 0) ? 1 : -1;
            v_sign = (du >= 0) ? 1 : -1;
        }
    }
    else
    {
        auto GetDominantAxis = [](const Vec3& vec) -> std::pair<AlignAxis, int>
        {
            Numeric abs_x = std::abs(vec.x());
            Numeric abs_y = std::abs(vec.y());
            Numeric abs_z = std::abs(vec.z());
            Numeric max_abs = std::max({abs_x, abs_y, abs_z});

            AlignAxis axis;
            if (max_abs == abs_x)
                axis = AlignAxis::XAxis;
            else if (max_abs == abs_y)
                axis = AlignAxis::YAxis;
            else
                axis = AlignAxis::ZAxis;

            int sign = 0;
            switch (axis)
            {
            case AlignAxis::XAxis:
                sign = (vec.x() >= 0) ? 1 : -1;
                break;
            case AlignAxis::YAxis:
                sign = (vec.y() >= 0) ? 1 : -1;
                break;
            case AlignAxis::ZAxis:
                sign = (vec.z() >= 0) ? 1 : -1;
                break;
            }

            return {axis, sign};
        };

        std::tie(current_u_axis, u_sign) = GetDominantAxis(vectors[0].vector);
        std::tie(current_v_axis, v_sign) = GetDominantAxis(vectors[1].vector);
    }


    // 7. 判断是否需要交换 u 和 v
    bool need_swap = false;
    if (current_u_axis != u_axis)
    {
        if (current_v_axis == u_axis && current_u_axis == v_axis)
        {
            // 需要交换 u 和 v
            need_swap = true;
            std::swap(current_u_axis, current_v_axis);
            std::swap(u_sign, v_sign);
        }
        else
        {
            // 无法通过交换实现对齐，返回原始 Surface
            return result;
        }
    }

    // 8. 判断是否需要反转 u 和 v 的方向
    bool reverse_u = false;
    if (current_u_axis == u_axis)
    {
        if (u_sign < 0)
            reverse_u = true;
    }

    bool reverse_v = false;
    if (current_v_axis == v_axis)
    {
        if (v_sign < 0)
            reverse_v = true;
    }

    // 9. 执行必要的操作：交换和反转
    if (need_swap)
    {
        // 交换次数
        std::swap(result.DegreeU, result.DegreeV);
        // 交换节点矢量
        std::swap(result.KnotsU, result.KnotsV);

        // 交换控制点网格
        auto& grid = result.ControlPoints;
        Grid<Vec4> transposed_grid(grid.VCount, grid.UCount);
        for (int u = 0; u < grid.UCount; ++u)
        {
            for (int v = 0; v < grid.VCount; ++v)
            {
                transposed_grid.Get(v, u) = grid.Get(u, v);
            }
        }
        result.ControlPoints = std::move(transposed_grid);
    }

    if (reverse_u)
    {
        // 反转 u 方向的节点矢量
        result.KnotsU.Reverse();

        // 反转 u 方向的控制点顺序
        auto& grid = result.ControlPoints;
        for (int v = 0; v < grid.VCount; ++v)
        {
            for (int u = 0; u < grid.UCount / 2; ++u)
            {
                std::swap(grid.Get(u, v), grid.Get(grid.UCount - 1 - u, v));
            }
        }
    }

    if (reverse_v)
    {
        // 反转 v 方向的节点矢量
        result.KnotsV.Reverse();

        // 反转 v 方向的控制点顺序
        auto& grid = result.ControlPoints;
        for (int u = 0; u < grid.UCount; ++u)
        {
            for (int v = 0; v < grid.VCount / 2; ++v)
            {
                std::swap(grid.Get(u, v), grid.Get(u, grid.VCount - 1 - v));
            }
        }
    }

    // 10. 返回调整后的 Surface
    return result;
}


Grid<Vec4> Surface::HomogeneousDerivative(Numeric u, Numeric v, int order_u, int order_v) const
{
    int index_span_u = KnotsU.FindSpanIndex(DegreeU, u);
    int index_span_v = KnotsV.FindSpanIndex(DegreeV, v);
    MatX basis_u = BSplineBasis::EvaluateAll(DegreeU, KnotsU.Values(), index_span_u, u, order_u);
    MatX basis_v = BSplineBasis::EvaluateAll(DegreeV, KnotsV.Values(), index_span_v, v, order_v);
    assert(basis_u.cols() == DegreeU + 1);
    assert(basis_v.cols() == DegreeV + 1);

    int index_pre_u = index_span_u - DegreeU;
    int index_pre_v = index_span_v - DegreeV;

    Grid<Vec4> result(order_u + 1, order_v + 1, Vec4::Zero());

    for (int l = 0; l <= order_v; ++l)
    {
        for (int k = 0; k <= order_u; ++k)
        {
            for (int j = 0; j <= DegreeV; j++)
            {
                int index_v = index_pre_v + j;
                Vec4 tmp = Vec4::Zero();
                for (int i = 0; i <= DegreeU; i++)
                {
                    int index_u = index_pre_u + i;
                    auto point = ToHomo(ControlPoints.Get(index_u, index_v));
                    tmp.noalias() += basis_u(k, i) * point;
                }
                result.Get(k, l).noalias() += basis_v(l, j) * tmp;
            }
        }
    }
    return result;
}
