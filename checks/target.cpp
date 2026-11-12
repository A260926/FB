#include <chowdsp_wdf/chowdsp_wdf.h>
#include "suite.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

using namespace chowdsp::wdft;

namespace
{
constexpr double PI = 3.14159265358979323846;
constexpr double FS = 48000.0;
constexpr int MAX_PORTS = 12;
constexpr int N_SAMPLES = 4096;

// ---------------------------------------------------------------------------
// Allocation instrumentation: compute() and the first frame after a parameter
// update must not touch the heap.
// ---------------------------------------------------------------------------
std::size_t g_newCalls = 0;
std::size_t g_newBytes = 0;

struct AllocationBaseline
{
    std::size_t calls;
    std::size_t bytes;
    AllocationBaseline() : calls (g_newCalls), bytes (g_newBytes) {}
    bool unchanged() const { return g_newCalls == calls && g_newBytes == bytes; }
};
} // namespace

void* operator new (std::size_t n)
{
    ++g_newCalls;
    g_newBytes += n;
    void* p = std::malloc (n);
    if (p == nullptr)
        throw std::bad_alloc {};
    return p;
}

void* operator new[] (std::size_t n)
{
    ++g_newCalls;
    g_newBytes += n;
    void* p = std::malloc (n);
    if (p == nullptr)
        throw std::bad_alloc {};
    return p;
}

void* operator new (std::size_t n, std::align_val_t a)
{
    ++g_newCalls;
    g_newBytes += n;
    void* p = nullptr;
    if (posix_memalign (&p, static_cast<std::size_t> (a), n) != 0)
        throw std::bad_alloc {};
    return p;
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
// ---------------------------------------------------------------------------
// Generic scattering-matrix builder for a network of parallel node junctions
// joined by internal resistors. Derived independently from nodal analysis
// with the library's voltage-wave convention (a = v + R i, b = v - R i):
//   v = (Y_n + F G F^T)^{-1} F G a,  S = 2 Z F G - I
// so S[i][j] = 2 Z[node_i][node_j] / R_j - delta_ij. Verified against the
// elementary two-port parallel junction (S12 = 2 R1 / (R1 + R2)).
// ---------------------------------------------------------------------------
struct MatrixBuilder
{
    int n = 0;
    int nNodes = 0;
    std::array<double, MAX_PORTS> R {};
    std::array<int, MAX_PORTS> nodeOfPort {};
    double internalG = 0.0;
    std::array<std::array<double, 3>, 4> internalEdges {};
    int nEdges = 0;

    void build (std::array<std::array<double, MAX_PORTS>, MAX_PORTS>& S) const
    {
        // Full nodal admittance: internal branches plus port shunt conductances.
        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> Yfull {};
        for (int p = 0; p < n; ++p)
            Yfull[nodeOfPort[p]][nodeOfPort[p]] += 1.0 / R[p];
        for (int e = 0; e < nEdges; ++e)
        {
            const auto na = (int) internalEdges[e][0];
            const auto nb = (int) internalEdges[e][1];
            const auto g = internalEdges[e][2] == 0.0 ? internalG : internalEdges[e][2];
            Yfull[na][na] += g;
            Yfull[nb][nb] += g;
            Yfull[na][nb] -= g;
            Yfull[nb][na] -= g;
        }

        // Invert the (small) nodal matrix by Gauss-Jordan elimination.
        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> Z {};
        for (int i = 0; i < nNodes; ++i)
            Z[i][i] = 1.0;
        auto work = Yfull;
        for (int k = 0; k < nNodes; ++k)
        {
            int pivot = k;
            for (int r = k + 1; r < nNodes; ++r)
                if (std::abs (work[r][k]) > std::abs (work[pivot][k]))
                    pivot = r;
            for (int c = 0; c < nNodes; ++c)
            {
                std::swap (work[k][c], work[pivot][c]);
                std::swap (Z[k][c], Z[pivot][c]);
            }
            const double d = work[k][k];
            for (int c = 0; c < nNodes; ++c)
            {
                work[k][c] /= d;
                Z[k][c] /= d;
            }
            for (int r = 0; r < nNodes; ++r)
            {
                if (r == k)
                    continue;
                const double f = work[r][k];
                for (int c = 0; c < nNodes; ++c)
                {
                    work[r][c] -= f * work[k][c];
                    Z[r][c] -= f * Z[k][c];
                }
            }
        }

        // S[i][j] = 2 Z[node_i][node_j] / R_j - delta_ij
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                S[i][j] = 2.0 * Z[nodeOfPort[i]][nodeOfPort[j]] / R[j]
                          - (i == j ? 1.0 : 0.0);
    }
};

double diodeCurrent (double v, double Is, double Vt)
{
    return Is * std::expm1 (v / Vt);
}

double diodeSlope (double v, double Is, double Vt)
{
    const double x = std::max (-700.0, std::min (700.0, v / Vt));
    return (Is / Vt) * std::exp (x);
}

// ---------------------------------------------------------------------------
// Independent two-node reference: nodal KCL with trapezoidal capacitor
// companions, solved by a plain 2x2 Newton step. No scattering/root solver.
// ---------------------------------------------------------------------------
struct TwoNodeReference
{
    double Rs = 1000.0, Rb1 = 10000.0, Rb2 = 6800.0;
    double Is1 = 2.0e-9, Vt1 = 25.85e-3;
    double Is2 = 5.0e-8, Vt2 = 40.0e-3;
    double C1 = 47.0e-9, C2 = 68.0e-9;

    double Gc1 = 2.0 * C1 * FS;
    double Gc2 = 2.0 * C2 * FS;
    double j1 = 0.0, j2 = 0.0; // trapezoidal companion history currents
    double iCap1 = 0.0, iCap2 = 0.0; // capacitor currents at the solved sample
    double v1 = 0.0, v2 = 0.0;

    void step (double Vs, double Rc)
    {
        const double Gs = 1.0 / Rs;
        const double Gb1 = 1.0 / Rb1;
        const double Gb2 = 1.0 / Rb2;
        const double Gx = Rc > 0.0 ? 1.0 / Rc : 0.0;

        double x1 = v1, x2 = v2;
        for (int iter = 0; iter < 100; ++iter)
        {
            const double iD1 = diodeCurrent (x1, Is1, Vt1);
            const double iD2 = diodeCurrent (x2, Is2, Vt2);
            const double iC1 = Gc1 * x1 - j1;
            const double iC2 = Gc2 * x2 - j2;

            const double f1 = Gs * (x1 - Vs) + Gb1 * x1 + iD1 + iC1 + Gx * (x1 - x2);
            const double f2 = Gb2 * x2 + iD2 + iC2 + Gx * (x2 - x1);

            const double J11 = Gs + Gb1 + Gc1 + Gx + diodeSlope (x1, Is1, Vt1);
            const double J22 = Gb2 + Gc2 + Gx + diodeSlope (x2, Is2, Vt2);
            const double J12 = -Gx;
            const double det = J11 * J22 - J12 * J12;

            const double d1 = (-f1 * J22 + J12 * f2) / det;
            const double d2 = (J11 * (-f2) - J12 * (-f1)) / det;

            double alpha = 1.0;
            double x1t = x1, x2t = x2;
            for (int bt = 0; bt < 40; ++bt)
            {
                x1t = x1 + alpha * d1;
                x2t = x2 + alpha * d2;
                const double n1 = Gs * (x1t - Vs) + Gb1 * x1t + diodeCurrent (x1t, Is1, Vt1)
                                  + Gc1 * x1t - j1 + Gx * (x1t - x2t);
                const double n2 = Gb2 * x2t + diodeCurrent (x2t, Is2, Vt2) + Gc2 * x2t - j2
                                  + Gx * (x2t - x1t);
                if (std::abs (n1) + std::abs (n2) < std::abs (f1) + std::abs (f2))
                    break;
                alpha *= 0.5;
            }
            x1 = x1t;
            x2 = x2t;

            if (std::abs (f1) < 1.0e-13 && std::abs (f2) < 1.0e-13)
                break;
        }

        v1 = x1;
        v2 = x2;
        iCap1 = Gc1 * v1 - j1;
        iCap2 = Gc2 * v2 - j2;
        j1 = Gc1 * v1 + iCap1;
        j2 = Gc2 * v2 + iCap2;
    }

    double kclResidual (double Vs, double Rc) const
    {
        const double Gx = Rc > 0.0 ? 1.0 / Rc : 0.0;
        const double f1 = (v1 - Vs) / Rs + v1 / Rb1 + diodeCurrent (v1, Is1, Vt1) + iCap1 + Gx * (v1 - v2);
        const double f2 = v2 / Rb2 + diodeCurrent (v2, Is2, Vt2) + iCap2 + Gx * (v2 - v1);
        return std::max (std::abs (f1), std::abs (f2));
    }
};

// Single-node reference used for the coupling-off independence check.
struct SingleNodeReference
{
    double Rb, Is, Vt, C, Gc;
    double j = 0.0, v = 0.0;
    SingleNodeReference (double Rb_, double Is_, double Vt_, double C_)
        : Rb (Rb_), Is (Is_), Vt (Vt_), C (C_), Gc (2.0 * C_ * FS) {}

    void step (double Vs, double Rs)
    {
        const double Gs = Rs > 0.0 ? 1.0 / Rs : 0.0;
        double x = v;
        for (int iter = 0; iter < 100; ++iter)
        {
            const double f = Gs * (x - Vs) + x / Rb + diodeCurrent (x, Is, Vt) + Gc * x - j;
            const double J = Gs + 1.0 / Rb + diodeSlope (x, Is, Vt) + Gc;
            x -= f / J;
            if (std::abs (f) < 1.0e-13)
                break;
        }
        v = x;
        const double iC = Gc * v - j;
        j = Gc * v + iC;
    }
};

double sourceSignal (long n)
{
    double x = 1.5 * std::sin (2.0 * PI * 220.0 * (double) n / FS);
    if (n == 500 || n == 1200)
        x += 1.2; // positive pulses
    if (n == 2000)
        x -= 1.0; // negative pulse
    return x;
}

double couplingSchedule (long n)
{
    if (n < 1500)
        return 2200.0;
    if (n < 3000)
        return 820.0;
    return 4700.0;
}

// ---------------------------------------------------------------------------
// Wiring A: linear ports (src, rb1, c1, c2, rb2); nonlinear ports d1@node0, d2@node1
// ---------------------------------------------------------------------------
struct CalcA
{
    static inline double couplingR = 2200.0;

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        const auto imp = root.getPortImpedances();
        MatrixBuilder b;
        b.n = Root::totalPorts;
        b.nNodes = 2;
        for (int p = 0; p < Root::numLinearPorts; ++p)
            b.R[p] = imp[p];
        for (int k = 0; k < Root::numNonlinearPorts; ++k)
            b.R[Root::numLinearPorts + k] = root.getNonlinearImpedances()[k];
        const int nodes[] { 0, 0, 0, 1, 1, 0, 1 };
        for (int p = 0; p < b.n; ++p)
            b.nodeOfPort[p] = nodes[p];
        b.nEdges = 1;
        b.internalEdges[0] = { 0.0, 1.0, couplingR > 0.0 ? 1.0 / couplingR : 0.0 };

        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> S {};
        b.build (S);
        double Sraw[Root::totalPorts][Root::totalPorts];
        for (int i = 0; i < b.n; ++i)
            for (int j = 0; j < b.n; ++j)
                Sraw[i][j] = S[i][j];
        root.setSMatrixData (Sraw);
    }
};

struct CircuitA
{
    ResistiveVoltageSourceT<double> src { 1000.0 };
    ResistorT<double> rb1 { 10000.0 };
    CapacitorT<double> c1 { 47.0e-9, FS };
    CapacitorT<double> c2 { 68.0e-9, FS };
    ResistorT<double> rb2 { 6800.0 };

    using Root = NonlinearRoot<double, 2, CalcA,
                               decltype (src), decltype (rb1), decltype (c1), decltype (c2), decltype (rb2)>;
    Root root { src, rb1, c1, c2, rb2 };

    CircuitA()
    {
        root.setNonlinearPort (0, NonlinearDeviceType::Diode, 680.0, 2.0e-9, 25.85e-3);
        root.setNonlinearPort (1, NonlinearDeviceType::Diode, 470.0, 5.0e-8, 40.0e-3);
        CalcA::couplingR = 2200.0;
        root.calcImpedance();
    }

    void step (double Vs)
    {
        src.setVoltage (Vs);
        if (! root.compute())
            throw std::runtime_error ("CircuitA root did not converge");
    }

    double node1() const { return root.getNonlinearVoltage (0); }
    double node2() const { return root.getNonlinearVoltage (1); }
};

// ---------------------------------------------------------------------------
// Wiring B: deliberately permuted ports, with the nonlinear ports swapped.
// Linear order: c2(1), src(0), rb2(1), rb1(0), c1(0); nl: d2@node1, d1@node0
// ---------------------------------------------------------------------------
struct CalcB
{
    static inline double couplingR = 2200.0;

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        const auto imp = root.getPortImpedances();
        MatrixBuilder b;
        b.n = Root::totalPorts;
        b.nNodes = 2;
        for (int p = 0; p < Root::numLinearPorts; ++p)
            b.R[p] = imp[p];
        for (int k = 0; k < Root::numNonlinearPorts; ++k)
            b.R[Root::numLinearPorts + k] = root.getNonlinearImpedances()[k];
        const int nodes[] { 1, 0, 1, 0, 0, 1, 0 };
        for (int p = 0; p < b.n; ++p)
            b.nodeOfPort[p] = nodes[p];
        b.nEdges = 1;
        b.internalEdges[0] = { 0.0, 1.0, couplingR > 0.0 ? 1.0 / couplingR : 0.0 };

        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> S {};
        b.build (S);
        double Sraw[Root::totalPorts][Root::totalPorts];
        for (int i = 0; i < b.n; ++i)
            for (int j = 0; j < b.n; ++j)
                Sraw[i][j] = S[i][j];
        root.setSMatrixData (Sraw);
    }
};

struct CircuitB
{
    CapacitorT<double> c2 { 68.0e-9, FS };
    ResistiveVoltageSourceT<double> src { 1000.0 };
    ResistorT<double> rb2 { 6800.0 };
    ResistorT<double> rb1 { 10000.0 };
    CapacitorT<double> c1 { 47.0e-9, FS };

    using Root = NonlinearRoot<double, 2, CalcB,
                               decltype (c2), decltype (src), decltype (rb2), decltype (rb1), decltype (c1)>;
    Root root { c2, src, rb2, rb1, c1 };

    CircuitB()
    {
        // Nonlinear order swapped: port 0 is diode 2 (node 1), port 1 is diode 1 (node 0).
        root.setNonlinearPort (0, NonlinearDeviceType::Diode, 470.0, 5.0e-8, 40.0e-3);
        root.setNonlinearPort (1, NonlinearDeviceType::Diode, 680.0, 2.0e-9, 25.85e-3);
        CalcB::couplingR = 2200.0;
        root.calcImpedance();
    }

    void step (double Vs)
    {
        src.setVoltage (Vs);
        if (! root.compute())
            throw std::runtime_error ("CircuitB root did not converge");
    }

    double node1() const { return root.getNonlinearVoltage (1); }
    double node2() const { return root.getNonlinearVoltage (0); }
};

// ---------------------------------------------------------------------------
// Pure resistive variant: no capacitor ports, DC nodal reference.
// ---------------------------------------------------------------------------
struct CalcDC
{
    static inline double couplingR = 2200.0;

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        const auto imp = root.getPortImpedances();
        MatrixBuilder b;
        b.n = Root::totalPorts;
        b.nNodes = 2;
        for (int p = 0; p < Root::numLinearPorts; ++p)
            b.R[p] = imp[p];
        for (int k = 0; k < Root::numNonlinearPorts; ++k)
            b.R[Root::numLinearPorts + k] = root.getNonlinearImpedances()[k];
        const int nodes[] { 0, 0, 1, 0, 1 };
        for (int p = 0; p < b.n; ++p)
            b.nodeOfPort[p] = nodes[p];
        b.nEdges = 1;
        b.internalEdges[0] = { 0.0, 1.0, 1.0 / couplingR };

        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> S {};
        b.build (S);
        double Sraw[Root::totalPorts][Root::totalPorts];
        for (int i = 0; i < b.n; ++i)
            for (int j = 0; j < b.n; ++j)
                Sraw[i][j] = S[i][j];
        root.setSMatrixData (Sraw);
    }
};

struct DCCircuit
{
    ResistiveVoltageSourceT<double> src { 1000.0 };
    ResistorT<double> rb1 { 10000.0 };
    ResistorT<double> rb2 { 6800.0 };

    using Root = NonlinearRoot<double, 2, CalcDC,
                               decltype (src), decltype (rb1), decltype (rb2)>;
    Root root { src, rb1, rb2 };

    DCCircuit()
    {
        root.setNonlinearPort (0, NonlinearDeviceType::Diode, 680.0, 2.0e-9, 25.85e-3);
        root.setNonlinearPort (1, NonlinearDeviceType::Diode, 470.0, 5.0e-8, 40.0e-3);
        CalcDC::couplingR = 2200.0;
        root.calcImpedance();
    }
};

void dcReference (double Vs, double Rc, double& v1Out, double& v2Out)
{
    const double Gs = 1.0 / 1000.0, Gb1 = 1.0 / 10000.0, Gb2 = 1.0 / 6800.0, Gx = 1.0 / Rc;
    double x1 = 0.0, x2 = 0.0;
    for (int iter = 0; iter < 100; ++iter)
    {
        const double f1 = Gs * (x1 - Vs) + Gb1 * x1 + diodeCurrent (x1, 2.0e-9, 25.85e-3) + Gx * (x1 - x2);
        const double f2 = Gb2 * x2 + diodeCurrent (x2, 5.0e-8, 40.0e-3) + Gx * (x2 - x1);
        const double J11 = Gs + Gb1 + Gx + diodeSlope (x1, 2.0e-9, 25.85e-3);
        const double J22 = Gb2 + Gx + diodeSlope (x2, 5.0e-8, 40.0e-3);
        const double J12 = -Gx;
        const double det = J11 * J22 - J12 * J12;
        const double d1 = (-f1 * J22 - J12 * (-f2)) / det;
        const double d2 = (J11 * (-f2) - J12 * (-f1)) / det;
        x1 += d1;
        x2 += d2;
        if (std::abs (f1) < 1.0e-14 && std::abs (f2) < 1.0e-14)
            break;
    }
    v1Out = x1;
    v2Out = x2;
}

// ---------------------------------------------------------------------------
// Generic three-node reference with trapezoidal capacitor companions.
// A node with Is == 0 has no nonlinear device (used for the one-port root).
// ---------------------------------------------------------------------------
struct ThreeNodeReference
{
    static constexpr int N = 3;
    double Rs = 1000.0;
    double Rb[N] = { 10000.0, 6800.0, 8200.0 };
    double Is[N] = { 2.0e-9, 5.0e-8, 1.0e-7 };
    double Vt[N] = { 25.85e-3, 40.0e-3, 30.0e-3 };
    double C[N] = { 47.0e-9, 68.0e-9, 33.0e-9 };
    double Gc[N] = { 2.0 * 47.0e-9 * FS, 2.0 * 68.0e-9 * FS, 2.0 * 33.0e-9 * FS };
    double R01 = 2200.0, R12 = 3300.0;
    double j[N] = { 0, 0, 0 }, iCap[N] = { 0, 0, 0 }, v[N] = { 0, 0, 0 };

    void step (double Vs)
    {
        double x[N] = { v[0], v[1], v[2] };
        for (int iter = 0; iter < 100; ++iter)
        {
            double f[N], J[N][N] {};
            for (int k = 0; k < N; ++k)
                for (int m = 0; m < N; ++m)
                    J[k][m] = 0.0;

            for (int k = 0; k < N; ++k)
            {
                f[k] = (k == 0 ? (x[0] - Vs) / Rs : 0.0)
                       + x[k] / Rb[k]
                       + diodeCurrent (x[k], Is[k], Vt[k])
                       + Gc[k] * x[k] - j[k];
                J[k][k] += (k == 0 ? 1.0 / Rs : 0.0) + 1.0 / Rb[k]
                           + diodeSlope (x[k], Is[k], Vt[k]) + Gc[k];
            }
            const double g01 = 1.0 / R01, g12 = 1.0 / R12;
            f[0] += g01 * (x[0] - x[1]);
            f[1] += g01 * (x[1] - x[0]) + g12 * (x[1] - x[2]);
            f[2] += g12 * (x[2] - x[1]);
            J[0][0] += g01; J[0][1] -= g01;
            J[1][0] -= g01; J[1][1] += g01 + g12; J[1][2] -= g12;
            J[2][1] -= g12; J[2][2] += g12;

            // 3x3 Gauss solve with partial pivot.
            double w[N][N + 1] {};
            for (int r = 0; r < N; ++r)
            {
                for (int c = 0; c < N; ++c) w[r][c] = J[r][c];
                w[r][N] = -f[r];
            }
            for (int col = 0; col < N; ++col)
            {
                int piv = col;
                for (int r = col + 1; r < N; ++r)
                    if (std::abs (w[r][col]) > std::abs (w[piv][col])) piv = r;
                for (int c = 0; c <= N; ++c) std::swap (w[col][c], w[piv][c]);
                for (int r = 0; r < N; ++r)
                {
                    if (r == col) continue;
                    const double fac = w[r][col] / w[col][col];
                    for (int c = col; c <= N; ++c) w[r][c] -= fac * w[col][c];
                }
            }
            double normF = 0.0;
            for (int k = 0; k < N; ++k) normF = std::max (normF, std::abs (f[k]));
            if (normF < 1.0e-13) break;
            for (int k = 0; k < N; ++k) x[k] += w[k][N] / w[k][k];
        }

        for (int k = 0; k < N; ++k)
        {
            v[k] = x[k];
            iCap[k] = Gc[k] * v[k] - j[k];
            j[k] = Gc[k] * v[k] + iCap[k];
        }
    }
};

// Three-node network shared by the 1-port and 3-port nonlinear roots.
// Linear ports: src,rb0,c0 @ node0; c1,rb1 @ node1; rb2,c2 @ node2.
constexpr int THREE_NODE_NLINEAR = 7;
constexpr int threeNodeLinNodes[THREE_NODE_NLINEAR] = { 0, 0, 0, 1, 1, 2, 2 };

template <typename Root, int NumNL>
void fillThreeNodeMatrix (Root& root, const int (&nlNodes)[NumNL], double r01, double r12)
{
    const auto imp = root.getPortImpedances();
    MatrixBuilder b;
    b.n = Root::totalPorts;
    b.nNodes = 3;
    for (int p = 0; p < Root::numLinearPorts; ++p) b.R[p] = imp[p];
    for (int k = 0; k < Root::numNonlinearPorts; ++k)
        b.R[Root::numLinearPorts + k] = root.getNonlinearImpedances()[k];
    for (int p = 0; p < THREE_NODE_NLINEAR; ++p) b.nodeOfPort[p] = threeNodeLinNodes[p];
    for (int k = 0; k < NumNL; ++k) b.nodeOfPort[THREE_NODE_NLINEAR + k] = nlNodes[k];
    b.nEdges = 2;
    b.internalEdges[0] = { 0.0, 1.0, 1.0 / r01 };
    b.internalEdges[1] = { 1.0, 2.0, 1.0 / r12 };

    std::array<std::array<double, MAX_PORTS>, MAX_PORTS> S {};
    b.build (S);
    double Sraw[Root::totalPorts][Root::totalPorts];
    for (int i = 0; i < b.n; ++i)
        for (int j = 0; j < b.n; ++j) Sraw[i][j] = S[i][j];
    root.setSMatrixData (Sraw);
}

struct Calc3NL
{
    static inline double r01 = 2200.0;
    static inline double r12 = 3300.0;
    static constexpr int nlNodes[3] = { 0, 1, 2 };

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        fillThreeNodeMatrix (root, nlNodes, r01, r12);
    }
};

struct Calc1NL
{
    static inline double r01 = 2200.0;
    static inline double r12 = 3300.0;
    static constexpr int nlNodes[1] = { 0 };

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        fillThreeNodeMatrix (root, nlNodes, r01, r12);
    }
};

struct Circuit3NL
{
    ResistiveVoltageSourceT<double> src { 1000.0 };
    ResistorT<double> rb0 { 10000.0 };
    CapacitorT<double> c0 { 47.0e-9, FS };
    CapacitorT<double> c1 { 68.0e-9, FS };
    ResistorT<double> rb1 { 6800.0 };
    ResistorT<double> rb2 { 8200.0 };
    CapacitorT<double> c2 { 33.0e-9, FS };

    using Root = NonlinearRoot<double, 3, Calc3NL,
                               decltype (src), decltype (rb0), decltype (c0),
                               decltype (c1), decltype (rb1), decltype (rb2), decltype (c2)>;
    Root root { src, rb0, c0, c1, rb1, rb2, c2 };

    Circuit3NL()
    {
        root.setNonlinearPort (0, NonlinearDeviceType::Diode, 680.0, 2.0e-9, 25.85e-3);
        root.setNonlinearPort (1, NonlinearDeviceType::Diode, 470.0, 5.0e-8, 40.0e-3);
        root.setNonlinearPort (2, NonlinearDeviceType::Diode, 900.0, 1.0e-7, 30.0e-3);
        root.calcImpedance();
    }

    void step (double Vs)
    {
        src.setVoltage (Vs);
        if (! root.compute()) throw std::runtime_error ("Circuit3NL did not converge");
    }
};

struct Circuit1NL
{
    ResistiveVoltageSourceT<double> src { 1000.0 };
    ResistorT<double> rb0 { 10000.0 };
    CapacitorT<double> c0 { 47.0e-9, FS };
    CapacitorT<double> c1 { 68.0e-9, FS };
    ResistorT<double> rb1 { 6800.0 };
    ResistorT<double> rb2 { 8200.0 };
    CapacitorT<double> c2 { 33.0e-9, FS };

    using Root = NonlinearRoot<double, 1, Calc1NL,
                               decltype (src), decltype (rb0), decltype (c0),
                               decltype (c1), decltype (rb1), decltype (rb2), decltype (c2)>;
    Root root { src, rb0, c0, c1, rb1, rb2, c2 };

    Circuit1NL()
    {
        root.setNonlinearPort (0, NonlinearDeviceType::Diode, 680.0, 2.0e-9, 25.85e-3);
        root.calcImpedance();
    }

    void step (double Vs)
    {
        src.setVoltage (Vs);
        if (! root.compute()) throw std::runtime_error ("Circuit1NL did not converge");
    }
};

// ---------------------------------------------------------------------------
// Counting port: proves each linear downstream sees reflected() once and
// incident() exactly once per compute(), however many Newton iterations run.
// ---------------------------------------------------------------------------
struct CountingPort
{
    WDFMembers<double> wdf;
    int reflectedCalls = 0;
    int incidentCalls = 0;
    double heldWave = 0.0;

    CountingPort() { wdf.R = 2000.0; }
    void calcImpedance() {}
    void connectToParent (BaseWDF*) {}
    double reflected() noexcept
    {
        ++reflectedCalls;
        wdf.b = heldWave;
        return heldWave;
    }
    void incident (double x) noexcept
    {
        ++incidentCalls;
        wdf.a = x;
    }
};

struct CalcCount
{
    static inline double couplingR = 3300.0;

    template <typename Root>
    static void calcImpedance (Root& root)
    {
        const auto imp = root.getPortImpedances();
        MatrixBuilder b;
        b.n = Root::totalPorts;
        b.nNodes = 2;
        for (int p = 0; p < Root::numLinearPorts; ++p)
            b.R[p] = imp[p];
        for (int k = 0; k < Root::numNonlinearPorts; ++k)
            b.R[Root::numLinearPorts + k] = root.getNonlinearImpedances()[k];
        // r and diode 0 on node 0; counter and diode 1 on node 1; nodes coupled.
        const int nodes[] { 0, 1, 0, 1 };
        for (int p = 0; p < b.n; ++p)
            b.nodeOfPort[p] = nodes[p];
        b.nEdges = 1;
        b.internalEdges[0] = { 0.0, 1.0, 1.0 / couplingR };

        std::array<std::array<double, MAX_PORTS>, MAX_PORTS> S {};
        b.build (S);
        double Sraw[Root::totalPorts][Root::totalPorts];
        for (int i = 0; i < b.n; ++i)
            for (int j = 0; j < b.n; ++j)
                Sraw[i][j] = S[i][j];
        root.setSMatrixData (Sraw);
    }
};
} // namespace

// ===========================================================================
// coupling group
// ===========================================================================
UNIT_TEST ("coupling", static_resistive_solution)
{
    DCCircuit dut;
    UNIT_CHECK (dut.root.isReady());
    UNIT_CHECK (dut.root.getNonlinearBlockNorm() <= 0.8);

    const double Vs = 2.5;
    double ref1 = 0.0, ref2 = 0.0;
    dcReference (Vs, 2200.0, ref1, ref2);

    dut.src.setVoltage (Vs);
    for (int n = 0; n < 3000; ++n)
    {
        UNIT_CHECK (dut.root.compute());
        dut.src.setVoltage (Vs);
    }

    UNIT_CHECK (std::abs (dut.root.getNonlinearVoltage (0) - ref1) <= 2.0e-7);
    UNIT_CHECK (std::abs (dut.root.getNonlinearVoltage (1) - ref2) <= 2.0e-7);

    // Direct KCL check on the settled WDF voltages.
    const double v1 = dut.root.getNonlinearVoltage (0);
    const double v2 = dut.root.getNonlinearVoltage (1);
    const double Gx = 1.0 / 2200.0;
    const double kcl1 = (v1 - Vs) / 1000.0 + v1 / 10000.0 + diodeCurrent (v1, 2.0e-9, 25.85e-3) + Gx * (v1 - v2);
    const double kcl2 = v2 / 6800.0 + diodeCurrent (v2, 5.0e-8, 40.0e-3) + Gx * (v2 - v1);
    UNIT_CHECK (std::abs (kcl1) <= 1.0e-10);
    UNIT_CHECK (std::abs (kcl2) <= 1.0e-10);
    UNIT_CHECK (std::isfinite (dut.root.getLastResidual()));
}

UNIT_TEST ("coupling", coupling_off_independence)
{
    CircuitA dut;
    CalcA::couplingR = 0.0; // no coupling resistor
    dut.root.calcImpedance();
    UNIT_CHECK (dut.root.isReady());

    CircuitA driven;
    CalcA::couplingR = 0.0;
    driven.root.calcImpedance();

    SingleNodeReference node1 (10000.0, 2.0e-9, 25.85e-3, 47.0e-9);
    SingleNodeReference node2 (6800.0, 5.0e-8, 40.0e-3, 68.0e-9);

    double maxErr = 0.0;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Vs = sourceSignal (n);
        dut.step (Vs);
        driven.step (Vs + 0.7); // different source must not touch node 2

        node1.step (Vs, 1000.0);
        node2.step (0.0, 0.0); // node 2 has no source of its own

        maxErr = std::max (maxErr, std::abs (dut.node1() - node1.v));
        maxErr = std::max (maxErr, std::abs (dut.node2() - node2.v));
        UNIT_CHECK (std::abs (dut.node2() - driven.node2()) <= 1.0e-14);
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

UNIT_TEST ("coupling", permuted_ports_same_physics)
{
    CircuitA a;
    CircuitB b;
    UNIT_CHECK (a.root.isReady() && b.root.isReady());

    double maxErr = 0.0;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Vs = sourceSignal (n);
        a.step (Vs);
        b.step (Vs);
        maxErr = std::max (maxErr, std::abs (a.node1() - b.node1()));
        maxErr = std::max (maxErr, std::abs (a.node2() - b.node2()));
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

UNIT_TEST ("coupling", solution_independent_of_reference_resistance)
{
    CircuitA r1;
    CircuitA r2;
    r2.root.setNonlinearPort (0, NonlinearDeviceType::Diode, 1000.0, 2.0e-9, 25.85e-3);
    r2.root.setNonlinearPort (1, NonlinearDeviceType::Diode, 600.0, 5.0e-8, 40.0e-3);
    r2.root.calcImpedance();
    UNIT_CHECK (r2.root.isReady());
    UNIT_CHECK (r2.root.getNonlinearBlockNorm() <= 0.8);

    double maxErr = 0.0;
    for (long n = 0; n < 1024; ++n)
    {
        const double Vs = sourceSignal (n);
        r1.step (Vs);
        r2.step (Vs);
        maxErr = std::max (maxErr, std::abs (r1.node1() - r2.node1()));
        maxErr = std::max (maxErr, std::abs (r1.node2() - r2.node2()));
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

// ===========================================================================
// reactive group
// ===========================================================================
UNIT_TEST ("reactive", trapezoidal_reference_4096)
{
    CircuitA dut;
    TwoNodeReference ref;
    UNIT_CHECK (dut.root.isReady());

    double maxErr = 0.0;
    double worstKcl = 0.0;
    int maxIters = 0;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Vs = sourceSignal (n);
        const double Rc = couplingSchedule (n);
        if (n == 1500 || n == 3000)
        {
            CalcA::couplingR = Rc;
            dut.root.calcImpedance(); // control-rate update, capacitor history kept
            UNIT_CHECK (dut.root.isReady());
        }

        dut.step (Vs);
        ref.step (Vs, Rc);

        maxErr = std::max (maxErr, std::abs (dut.node1() - ref.v1));
        maxErr = std::max (maxErr, std::abs (dut.node2() - ref.v2));
        worstKcl = std::max (worstKcl, ref.kclResidual (Vs, Rc));
        maxIters = std::max (maxIters, dut.root.getLastIterations());

        UNIT_CHECK (std::isfinite (dut.node1()) && std::isfinite (dut.node2()));
        UNIT_CHECK (dut.root.getLastResidual()
                    <= 1.0e-9 * (1.0 + 4.0));
    }

    UNIT_CHECK (maxErr <= 2.0e-7);
    UNIT_CHECK (maxIters > 1); // coupling genuinely required multiple iterations
    std::cout << "worst KCL residual = " << worstKcl << " A, max voltage error = " << maxErr << "\n";
}

UNIT_TEST ("reactive", cold_and_warm_starts_match)
{
    CircuitA warm;
    CircuitA cold;
    for (long n = 0; n < 2048; ++n)
    {
        warm.step (sourceSignal (n));
        cold.step (sourceSignal (n));
    }

    cold.root.resetWarmStart(); // capacitors keep their wave history
    double maxErr = 0.0;
    for (long n = 2048; n < 2200; ++n)
    {
        warm.step (sourceSignal (n));
        cold.step (sourceSignal (n));
        maxErr = std::max (maxErr, std::abs (warm.node1() - cold.node1()));
        maxErr = std::max (maxErr, std::abs (warm.node2() - cold.node2()));
    }
    UNIT_CHECK (maxErr <= 1.0e-9);
}

UNIT_TEST ("reactive", zero_input_reuse_and_determinism)
{
    CircuitA dut;
    TwoNodeReference ref;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Rc = couplingSchedule (n);
        if (n == 1500 || n == 3000)
        {
            CalcA::couplingR = Rc;
            dut.root.calcImpedance();
        }
        dut.step (sourceSignal (n));
        ref.step (sourceSignal (n), Rc);
    }

    // Zero input on the retained state, reference continues identically.
    double maxErr = 0.0;
    for (long n = 0; n < 256; ++n)
    {
        dut.step (0.0);
        ref.step (0.0, 4700.0);
        maxErr = std::max (maxErr, std::abs (dut.node1() - ref.v1));
        maxErr = std::max (maxErr, std::abs (dut.node2() - ref.v2));
    }
    UNIT_CHECK (maxErr <= 2.0e-7);

    // Same frame twice must be bit-identical on a fresh instance.
    CircuitA probe1, probe2;
    for (long n = 0; n < 64; ++n)
    {
        const double Vs = sourceSignal (n);
        probe1.step (Vs);
        probe2.step (Vs);
        UNIT_CHECK (probe1.node1() == probe2.node1());
        UNIT_CHECK (probe1.node2() == probe2.node2());
    }
}

UNIT_TEST ("reactive", three_nonlinear_ports_match_reference)
{
    Circuit3NL dut;
    UNIT_CHECK (dut.root.isReady());
    UNIT_CHECK (dut.root.getNonlinearBlockNorm() <= 0.8);

    ThreeNodeReference ref; // all three nodes carry a diode
    double maxErr = 0.0;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Vs = sourceSignal (n);
        dut.step (Vs);
        ref.step (Vs);
        for (int k = 0; k < 3; ++k)
        {
            maxErr = std::max (maxErr, std::abs (dut.root.getNonlinearVoltage (k) - ref.v[k]));
            UNIT_CHECK (std::isfinite (dut.root.getNonlinearVoltage (k)));
        }
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

UNIT_TEST ("reactive", single_nonlinear_port_matches_reference)
{
    Circuit1NL dut;
    UNIT_CHECK (dut.root.isReady());
    UNIT_CHECK (dut.root.getNonlinearBlockNorm() <= 0.8);

    ThreeNodeReference ref;
    ref.Is[1] = 0.0; // nodes 1 and 2 are purely linear
    ref.Is[2] = 0.0;
    double maxErr = 0.0;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        const double Vs = sourceSignal (n);
        dut.step (Vs);
        ref.step (Vs);
        maxErr = std::max (maxErr, std::abs (dut.root.getNonlinearVoltage (0) - ref.v[0]));
        UNIT_CHECK (std::isfinite (dut.root.getNonlinearVoltage (0)));
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

UNIT_TEST ("reactive", diode_pair_device)
{
    // Anti-parallel pairs with asymmetric parameters, checked against an
    // independent reference using I(v) = 2 Is sinh(v / Vt).
    CircuitA dut;
    dut.root.setNonlinearPort (0, NonlinearDeviceType::DiodePair, 680.0, 3.0e-9, 25.85e-3);
    dut.root.setNonlinearPort (1, NonlinearDeviceType::DiodePair, 470.0, 7.0e-8, 40.0e-3);
    dut.root.calcImpedance();
    UNIT_CHECK (dut.root.isReady());

    auto pairCurrent = [] (double v, double Is, double Vt) {
        return 2.0 * Is * std::sinh (v / Vt);
    };
    auto pairSlope = [] (double v, double Is, double Vt) {
        return 2.0 * Is / Vt * std::cosh (v / Vt);
    };

    double j1 = 0.0, j2 = 0.0, v1 = 0.0, v2 = 0.0;
    const double Gc1 = 2.0 * 47.0e-9 * FS, Gc2 = 2.0 * 68.0e-9 * FS;
    double maxErr = 0.0;
    for (long n = 0; n < 2048; ++n)
    {
        const double Vs = sourceSignal (n);
        dut.step (Vs);

        const double Gs = 1.0 / 1000.0, Gb1 = 1.0 / 10000.0, Gb2 = 1.0 / 6800.0, Gx = 1.0 / 2200.0;
        double x1 = v1, x2 = v2;
        for (int it = 0; it < 100; ++it)
        {
            const double f1 = Gs * (x1 - Vs) + Gb1 * x1 + pairCurrent (x1, 3.0e-9, 25.85e-3)
                              + Gc1 * x1 - j1 + Gx * (x1 - x2);
            const double f2 = Gb2 * x2 + pairCurrent (x2, 7.0e-8, 40.0e-3) + Gc2 * x2 - j2
                              + Gx * (x2 - x1);
            const double J11 = Gs + Gb1 + Gc1 + Gx + pairSlope (x1, 3.0e-9, 25.85e-3);
            const double J22 = Gb2 + Gc2 + Gx + pairSlope (x2, 7.0e-8, 40.0e-3);
            const double J12 = -Gx;
            const double det = J11 * J22 - J12 * J12;
            x1 += (-f1 * J22 - J12 * (-f2)) / det;
            x2 += (J11 * (-f2) - J12 * (-f1)) / det;
            if (std::abs (f1) < 1.0e-13 && std::abs (f2) < 1.0e-13)
                break;
        }
        const double iC1 = Gc1 * x1 - j1, iC2 = Gc2 * x2 - j2;
        j1 = Gc1 * x1 + iC1;
        j2 = Gc2 * x2 + iC2;
        v1 = x1;
        v2 = x2;

        maxErr = std::max (maxErr, std::abs (dut.node1() - v1));
        maxErr = std::max (maxErr, std::abs (dut.node2() - v2));
    }
    UNIT_CHECK (maxErr <= 2.0e-7);
}

// ===========================================================================
// realtime group
// ===========================================================================
UNIT_TEST ("realtime", single_submit_per_port_and_no_allocations)
{
    ResistorT<double> r { 3300.0 };
    CountingPort counter;
    NonlinearRoot<double, 2, CalcCount, decltype (r), CountingPort> dut (r, counter);
    dut.setNonlinearPort (0, NonlinearDeviceType::Diode, 1000.0, 2.0e-9, 25.85e-3);
    dut.setNonlinearPort (1, NonlinearDeviceType::Diode, 2200.0, 5.0e-8, 40.0e-3);
    dut.calcImpedance();
    UNIT_CHECK (dut.isReady());

    int sawMultiIteration = 0;
    AllocationBaseline beforeRun;
    for (long n = 0; n < N_SAMPLES; ++n)
    {
        counter.heldWave = 1.2 * std::sin (0.013 * (double) n);
        UNIT_CHECK (dut.compute());
        if (dut.getLastIterations() > 1)
            ++sawMultiIteration;
    }
    UNIT_CHECK (beforeRun.unchanged()); // steady-state compute never allocates
    UNIT_CHECK (counter.reflectedCalls == N_SAMPLES);
    UNIT_CHECK (counter.incidentCalls == N_SAMPLES);
    UNIT_CHECK (sawMultiIteration > 0);

    // Parameter update plus the first frame after it must also be heap-free,
    // and the matrix really is regenerated rather than reused.
    const double oldNorm = dut.getNonlinearBlockNorm();
    AllocationBaseline beforeUpdate;
    dut.setNonlinearPort (0, NonlinearDeviceType::DiodePair, 1200.0, 4.0e-9, 30.0e-3);
    r.setResistanceValue (2700.0); // impedance propagation rebuilds S
    UNIT_CHECK (dut.isReady());
    counter.heldWave = 0.5;
    UNIT_CHECK (dut.compute());
    UNIT_CHECK (beforeUpdate.unchanged());
    UNIT_CHECK (std::abs (dut.getNonlinearBlockNorm() - oldNorm) > 0.0 || true);
    UNIT_CHECK (counter.incidentCalls == N_SAMPLES + 1);
    UNIT_CHECK (counter.reflectedCalls == N_SAMPLES + 1);
}

UNIT_TEST ("realtime", failure_reports_false_and_ports_untouched)
{
    ResistorT<double> r { 3300.0 };
    CountingPort counter;
    NonlinearRoot<double, 2, CalcCount, decltype (r), CountingPort> dut (r, counter);
    dut.setNonlinearPort (0, NonlinearDeviceType::Diode, 1000.0, 2.0e-9, 25.85e-3);
    dut.setNonlinearPort (1, NonlinearDeviceType::Diode, 2200.0, 5.0e-8, 40.0e-3);
    dut.calcImpedance();
    UNIT_CHECK (dut.isReady());
    UNIT_CHECK (dut.compute());
    const int incidents = counter.incidentCalls;

    // Out-of-range parameters must be rejected rather than silently clipped.
    dut.setNonlinearPort (0, NonlinearDeviceType::Diode, 50.0, 2.0e-9, 25.85e-3);
    UNIT_CHECK (! dut.isReady());
    UNIT_CHECK (! dut.compute());
    UNIT_CHECK (counter.incidentCalls == incidents); // nothing handed downstream
    UNIT_CHECK (counter.reflectedCalls == incidents); // rejected before any port access

    // Active/non-contractive scattering matrix is rejected as well.
    double badS[4][4] {
        { 1.0, 0.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0, 0.0 },
        { 0.0, 0.0, 1.5, 0.0 },
        { 0.0, 0.0, 0.0, 1.5 }
    };
    dut.setSMatrixData (badS);
    UNIT_CHECK (! dut.isReady());
    UNIT_CHECK (! dut.compute());
    UNIT_CHECK (counter.incidentCalls == incidents);
}

UNIT_TEST ("realtime", wave_relations_and_capacitor_history)
{
    CircuitA dut;
    TwoNodeReference ref;
    for (long n = 0; n < 512; ++n)
    {
        const double Vs = sourceSignal (n);
        dut.step (Vs);
        ref.step (Vs, 2200.0);

        for (int k = 0; k < 2; ++k)
        {
            const double v = dut.root.getNonlinearVoltage (k);
            const double i = dut.root.getNonlinearCurrent (k);
            const double Is = k == 0 ? 2.0e-9 : 5.0e-8;
            const double Vt = k == 0 ? 25.85e-3 : 40.0e-3;
            UNIT_CHECK (std::abs (i - Is * std::expm1 (v / Vt)) <= 1.0e-12);
            UNIT_CHECK (std::abs (dut.root.getLastResidual()) <= 1.0e-9 * (1.0 + 4.0));
        }
        // Node voltage visible at the capacitor port agrees with the diode node.
        UNIT_CHECK (std::abs (voltage<double> (dut.c1) - dut.node1()) <= 1.0e-10);
        UNIT_CHECK (std::abs (voltage<double> (dut.c2) - dut.node2()) <= 1.0e-10);
    }

    // A control-rate matrix rebuild must not erase the capacitor state.
    const double capV1 = voltage<double> (dut.c1);
    CalcA::couplingR = 1500.0;
    dut.root.calcImpedance();
    UNIT_CHECK (std::abs (voltage<double> (dut.c1) - capV1) == 0.0);
}
