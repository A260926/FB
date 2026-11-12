#ifndef CHOWDSP_WDF_NONLINEAR_ROOT_H
#define CHOWDSP_WDF_NONLINEAR_ROOT_H

#include "../wdft/wdft_base.h"
#include "rtype_detail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chowdsp
{
namespace wdft
{
    /** Type of nonlinear device attached to a NonlinearRoot port. */
    enum class NonlinearDeviceType
    {
        Diode, /**< Single diode: I(v) = Is * expm1(v / Vt). */
        DiodePair, /**< Anti-parallel diode pair: I(v) = 2 * Is * sinh(v / Vt). */
    };

    /**
     * Root WDF node that couples one to three nonlinear ports with any fixed
     * number of linear downstream WDF ports, solving all nonlinear ports
     * simultaneously as one coupled equation system.
     *
     * @tparam T                   scalar sample type (only double is supported)
     * @tparam NumNonlinearPorts   number of nonlinear ports (1 through 3)
     * @tparam ImpedanceCalculator same style of calculator as RootRtypeAdaptor,
     *                             with a static `calcImpedance (RootType&)` that
     *                             regenerates the scattering matrix through
     *                             setSMatrixData()
     * @tparam PortTypes           linear downstream port types
     *
     * Port ordering convention: linear ports come first (in tuple order),
     * nonlinear ports follow.
     *
     * Wave conventions (identical to the rest of the library):
     *  - linear port: its reflected() wave is taken as a, and the solved wave b
     *    is delivered through incident();
     *  - nonlinear port: a = v - R * I(v), b = v + R * I(v), where I(v) is the
     *    current flowing *into* the device and v is the device voltage, so
     *    v = (a + b) / 2 and I = (b - a) / (2 R);
     *  - every port satisfies b = S * a together.
     *
     * The scattering matrix is passed in ordinary external row/column order;
     * callers never see the internally transposed storage.
     *
     * Only passive resistive networks are supported: the power-wave normalized
     * nonlinear sub-block must be symmetric with a norm no greater than 0.8.
     */
    template <typename T, int NumNonlinearPorts, typename ImpedanceCalculator, typename... PortTypes>
    class NonlinearRoot : public RootWDF
    {
        static_assert (std::is_same<T, double>::value, "NonlinearRoot currently only supports scalar double");
        static_assert (NumNonlinearPorts >= 1 && NumNonlinearPorts <= 3, "NonlinearRoot supports 1 to 3 nonlinear ports");

    public:
        static constexpr auto numLinearPorts = int (sizeof...(PortTypes));
        static constexpr auto numNonlinearPorts = NumNonlinearPorts;
        static constexpr auto totalPorts = numLinearPorts + numNonlinearPorts;

        explicit NonlinearRoot (PortTypes&... dps) : downPorts (std::tie (dps...))
        {
            aVec.clear();
            bVec.clear();

            rtype_detail::forEachInTuple ([&] (auto& port, size_t) { port.connectToParent (this); },
                                          downPorts);

            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                deviceType[k] = NonlinearDeviceType::Diode;
                nlR[k] = (T) 1000.0;
                nlG[k] = (T) 1.0 / nlR[k];
                nlIs[k] = (T) 1.0e-9;
                nlVt[k] = (T) 25.85e-3;
                nlInvVt[k] = (T) 1.0 / nlVt[k];
            }

            paramsValid = true;
            calcImpedance();
        }

        /** Recomputes the scattering matrix from the current downstream impedances. */
        void calcImpedance() override
        {
            ImpedanceCalculator::calcImpedance (*this);
        }

        /** Impedances of the linear ports, in tuple order. */
        std::array<T, numLinearPorts> getPortImpedances()
        {
            std::array<T, numLinearPorts> portImpedances {};
            rtype_detail::forEachInTuple ([&] (auto& port, size_t i) { portImpedances[i] = port.wdf.R; },
                                          downPorts);
            return portImpedances;
        }

        /** Reference resistances of the nonlinear ports. */
        std::array<T, numNonlinearPorts> getNonlinearImpedances() const
        {
            return nlR;
        }

        /**
         * Configures one nonlinear port.
         * @param portIndex nonlinear port index, starting at zero
         * @param type      single diode or anti-parallel pair
         * @param R         reference resistance, 100 to 100000 Ohm
         * @param Is        saturation current, 1e-12 to 1e-6 Ampere
         * @param Vt        thermal voltage, 0.02 to 0.06 Volt
         *
         * After changing parameters, call calcImpedance() (or let an impedance
         * propagation do it) so the scattering matrix is regenerated.
         */
        void setNonlinearPort (int portIndex, NonlinearDeviceType type, T R, T Is, T Vt)
        {
            if (portIndex < 0 || portIndex >= numNonlinearPorts
                || ! std::isfinite (R) || ! std::isfinite (Is) || ! std::isfinite (Vt)
                || R < (T) 100.0 || R > (T) 100000.0
                || Is < (T) 1.0e-12 || Is > (T) 1.0e-6
                || Vt < (T) 0.02 || Vt > (T) 0.06)
            {
                paramsValid = false;
                return;
            }

            deviceType[portIndex] = type;
            nlR[portIndex] = R;
            nlG[portIndex] = (T) 1.0 / R;
            nlIs[portIndex] = Is;
            nlVt[portIndex] = Vt;
            nlInvVt[portIndex] = (T) 1.0 / Vt;
            paramsValid = true;
            // The reference resistance enters both the power-wave normalization
            // and the scattering matrix, so any previously supplied matrix is
            // now stale until calcImpedance() regenerates it.
            matrixValid = false;
        }

        /**
         * Sets the full scattering matrix. The matrix is supplied in ordinary
         * external row/column order (b = S * a), linear ports first and nonlinear
         * ports last; the transposed internal storage is handled here.
         *
         * The matrix is checked for finiteness, symmetry of the power-wave
         * normalized nonlinear sub-block, and a contraction norm of at most 0.8.
         * Invalid matrices leave the root in a not-ready state, reported through
         * isReady() and a false return from compute().
         */
        void setSMatrixData (const T (&mat)[totalPorts][totalPorts])
        {
            for (int i = 0; i < totalPorts; ++i)
                for (int j = 0; j < totalPorts; ++j)
                    SMatrix[j][i] = mat[i][j]; // internal storage is transposed

            // Cache the blocks touching the nonlinear ports, in external order.
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                for (int j = 0; j < numLinearPorts; ++j)
                    sLinear[k][j] = mat[numLinearPorts + k][j];

                for (int m = 0; m < numNonlinearPorts; ++m)
                    sNonlinear[k][m] = mat[numLinearPorts + k][numLinearPorts + m];
            }

            matrixValid = validateMatrix (mat);
        }

        /** Returns false until a valid scattering matrix has been supplied. */
        bool isReady() const noexcept
        {
            return matrixValid && paramsValid;
        }

        /**
         * Clears the warm-start state (the previous sample's nonlinear port
         * voltages), so the next compute() iterates from zero. Capacitor and
         * other downstream wave histories are not touched.
         */
        void resetWarmStart() noexcept
        {
            vWarm.fill ((T) 0);
        }

        /**
         * Solves the coupled nonlinear wave equations for one sample.
         *
         * Each linear downstream port is queried for its reflected wave exactly
         * once. No downstream incident() calls happen during the iteration; the
         * final b waves are handed to the ports exactly once, and only after the
         * solve has converged. On failure the ports are not touched and no NaN
         * is propagated.
         *
         * Returns true when the maximum nonlinear wave residual is at most
         * 1e-9 * (1 + max wave magnitude of the sample).
         */
        bool compute() noexcept
        {
            if (! isReady())
                return false;

            // 1. Collect the linear incident waves, exactly once per port.
            rtype_detail::forEachInTuple ([&] (auto& port, size_t i) { aVec[(int) i] = port.reflected(); },
                                          downPorts);

            // 2. Constant drive of each nonlinear residual from the linear ports.
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                T drive = (T) 0;
                for (int j = 0; j < numLinearPorts; ++j)
                    drive += sLinear[k][j] * aVec[j];
                cDrive[k] = drive;
            }

            // 3. Coupled Newton solve, first from the warm state, then retrying
            //    from zero so cold and hot starts reach the same physical solution.
            bool converged = solveNewton (vWarm);
            if (! converged)
            {
                std::array<T, numNonlinearPorts> zeroGuess {};
                converged = solveNewton (zeroGuess);
            }

            if (! converged)
            {
                lastIterations = maxIterations;
                lastResidual = (T) 0; // no unconverged solution is reported as sound
                return false;
            }

            // 4. Final waves of the nonlinear ports.
            T maxWave = (T) 0;
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                const T v = vState[k];
                const T i = deviceCurrent (k, v);
                iState[k] = i;

                const T a = v - nlR[k] * i;
                const T b = v + nlR[k] * i;
                aVec[numLinearPorts + k] = a;
                bVec[numLinearPorts + k] = b;

                maxWave = std::max (maxWave, std::abs (a));
                maxWave = std::max (maxWave, std::abs (b));
            }

            for (int j = 0; j < numLinearPorts; ++j)
                maxWave = std::max (maxWave, std::abs (aVec[j]));

            // 5. Compute the linear outgoing waves numerically (nothing is
            //    submitted yet) so they count toward this sample's max wave
            //    amplitude in the acceptance tolerance.
            for (int j = 0; j < numLinearPorts; ++j)
            {
                T bOut = (T) 0;
                for (int q = 0; q < totalPorts; ++q)
                    bOut += SMatrix[q][j] * aVec[q]; // external S[j][q]
                bVec[j] = bOut;
                maxWave = std::max (maxWave, std::abs (bOut));
            }

            // 6. Independent verification of the nonlinear wave equations.
            T worstResidual = (T) 0;
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                T scattered = cDrive[k];
                for (int m = 0; m < numNonlinearPorts; ++m)
                    scattered += sNonlinear[k][m] * aVec[numLinearPorts + m];

                const T residual = bVec[numLinearPorts + k] - scattered;
                worstResidual = std::max (worstResidual, std::abs (residual));
            }

            const T tolerance = (T) 1.0e-9 * ((T) 1.0 + maxWave);
            if (! std::isfinite (worstResidual) || worstResidual > tolerance)
                return false;

            for (int j = 0; j < numLinearPorts; ++j)
            {
                if (! std::isfinite (bVec[j]))
                    return false;
            }

            // 7. Only now are the converged b waves handed downstream, once each.
            rtype_detail::forEachInTuple ([&] (auto& port, size_t i) { port.incident (bVec[(int) i]); },
                                          downPorts);

            for (int k = 0; k < numNonlinearPorts; ++k)
                vWarm[k] = vState[k];

            lastResidual = worstResidual;
            return true;
        }

        /** Device voltage v at a nonlinear port after a successful compute(). */
        T getNonlinearVoltage (int portIndex) const noexcept { return vState[portIndex]; }

        /** Device current I(v) (flowing into the device) after a successful compute(). */
        T getNonlinearCurrent (int portIndex) const noexcept { return iState[portIndex]; }

        /** Maximum nonlinear wave residual of the last successful compute(). */
        T getLastResidual() const noexcept { return lastResidual; }

        /** Number of Newton iterations used by the last successful compute(). */
        int getLastIterations() const noexcept { return lastIterations; }

        /** 2-norm of the power-wave normalized nonlinear sub-block. */
        T getNonlinearBlockNorm() const noexcept { return nonlinearBlockNorm; }

    private:
        static constexpr int maxIterations = 60;

        /** Keeps exp() inside the finite double range without touching the audio. */
        static inline T safeExp (T x) noexcept
        {
            const T limit = (T) 700.0;
            if (x > limit)
                x = limit;
            else if (x < -limit)
                x = -limit;
            return std::exp (x);
        }

        /** I(v), current into the device. */
        inline T deviceCurrent (int k, T v) const noexcept
        {
            const T x = v * nlInvVt[k];
            if (deviceType[k] == NonlinearDeviceType::Diode)
                return nlIs[k] * std::expm1 (x);

            // 2 * sinh(x) = exp(x) - exp(-x)
            return nlIs[k] * (safeExp (x) - safeExp (-x));
        }

        /** dI/dv of the device. */
        inline T deviceSlope (int k, T v) const noexcept
        {
            const T x = v * nlInvVt[k];
            if (deviceType[k] == NonlinearDeviceType::Diode)
                return nlIs[k] * nlInvVt[k] * safeExp (x);

            // d/dv [2 Is sinh(v/Vt)] = (Is/Vt) (exp(x) + exp(-x))
            return nlIs[k] * nlInvVt[k] * (safeExp (x) + safeExp (-x));
        }

        /**
         * Evaluates the nonlinear wave residuals
         * f_k = b_k(v_k) - c_k - sum_m S_nl[k][m] * a_m(v_m)
         * and the analytic Jacobian df/dv.
         */
        void assemble (const std::array<T, numNonlinearPorts>& v,
                       std::array<T, numNonlinearPorts>& f,
                       T jacobian[numNonlinearPorts][numNonlinearPorts]) noexcept
        {
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                const T ik = deviceCurrent (k, v[k]);
                portA[k] = v[k] - nlR[k] * ik;
                portB[k] = v[k] + nlR[k] * ik;
                waveFactor[k] = (T) 1.0 - nlR[k] * deviceSlope (k, v[k]); // da/dv
                onePlusRSlope[k] = (T) 1.0 + nlR[k] * deviceSlope (k, v[k]); // db/dv
            }

            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                T residual = portB[k] - cDrive[k];
                for (int m = 0; m < numNonlinearPorts; ++m)
                {
                    residual -= sNonlinear[k][m] * portA[m];
                    jacobian[k][m] = -sNonlinear[k][m] * waveFactor[m];
                }
                jacobian[k][k] += onePlusRSlope[k];
                f[k] = residual;
            }
        }

        /** Gaussian elimination with partial pivoting for n <= 3; returns false on a singular matrix. */
        static bool solveLinear (T matrix[numNonlinearPorts][numNonlinearPorts],
                                 const std::array<T, numNonlinearPorts>& rhs,
                                 std::array<T, numNonlinearPorts>& delta) noexcept
        {
            T work[numNonlinearPorts][numNonlinearPorts + 1] {};
            for (int r = 0; r < numNonlinearPorts; ++r)
            {
                for (int c = 0; c < numNonlinearPorts; ++c)
                    work[r][c] = matrix[r][c];
                work[r][numNonlinearPorts] = rhs[r];
            }

            for (int col = 0; col < numNonlinearPorts; ++col)
            {
                int pivot = col;
                T pivotMag = std::abs (work[col][col]);
                for (int r = col + 1; r < numNonlinearPorts; ++r)
                {
                    const T mag = std::abs (work[r][col]);
                    if (mag > pivotMag)
                    {
                        pivotMag = mag;
                        pivot = r;
                    }
                }

                if (pivotMag < (T) 1.0e-30)
                    return false;

                if (pivot != col)
                {
                    for (int c = 0; c <= numNonlinearPorts; ++c)
                        std::swap (work[col][c], work[pivot][c]);
                }

                for (int r = 0; r < numNonlinearPorts; ++r)
                {
                    if (r == col)
                        continue;

                    const T factor = work[r][col] / work[col][col];
                    for (int c = col; c <= numNonlinearPorts; ++c)
                        work[r][c] -= factor * work[col][c];
                }
            }

            for (int r = 0; r < numNonlinearPorts; ++r)
                delta[r] = work[r][numNonlinearPorts] / work[r][r];

            for (T x : delta)
            {
                if (! std::isfinite (x))
                    return false;
            }
            return true;
        }

        /** Coupled Newton iteration with diode voltage limiting and backtracking. */
        bool solveNewton (const std::array<T, numNonlinearPorts>& initialGuess) noexcept
        {
            auto v = initialGuess;
            std::array<T, numNonlinearPorts> f {};
            std::array<T, numNonlinearPorts> fTrial {};
            std::array<T, numNonlinearPorts> delta {};
            std::array<T, numNonlinearPorts> vTrial {};
            T jacobian[numNonlinearPorts][numNonlinearPorts] {};

            assemble (v, f, jacobian);
            T normF = infNorm (f);
            if (! std::isfinite (normF))
                return false;

            int iterations = 0;
            for (; iterations < maxIterations; ++iterations)
            {
                // Tighter than the public acceptance threshold so the final
                // independent check always clears the 1e-9 specification.
                T waveScale = (T) 0;
                for (int q = 0; q < numLinearPorts; ++q)
                    waveScale = std::max (waveScale, std::abs (aVec[q]));
                for (int k = 0; k < numNonlinearPorts; ++k)
                {
                    waveScale = std::max (waveScale, std::abs (portA[k]));
                    waveScale = std::max (waveScale, std::abs (portB[k]));
                }

                if (normF <= (T) 1.0e-12 * ((T) 1.0 + waveScale))
                {
                    vState = v;
                    lastIterations = iterations;
                    return true;
                }

                if (! solveLinear (jacobian, f, delta))
                    return false;

                // Diode voltage limiting: a raw Newton step from a cold start
                // lands far up the exponential knee and makes exp() overflow.
                // Cap each voltage change at a few thermal voltages so the
                // iteration never leaves the well-conditioned region; the cap
                // stops binding (alpha reaches 1) as soon as the step is small,
                // preserving quadratic convergence near the solution.
                T alpha = (T) 1.0;
                for (int k = 0; k < numNonlinearPorts; ++k)
                {
                    const T stepLimit = (T) 8.0 * nlVt[k];
                    if (std::abs (delta[k]) > stepLimit)
                        alpha = std::min (alpha, stepLimit / std::abs (delta[k]));
                }

                // Backtracking from the limited step: guarantee monotone
                // decrease of the residual norm without clipping any audio.
                bool improved = false;
                for (int backtrack = 0; backtrack < 50; ++backtrack)
                {
                    for (int k = 0; k < numNonlinearPorts; ++k)
                        vTrial[k] = v[k] - alpha * delta[k];

                    T trialJacobian[numNonlinearPorts][numNonlinearPorts] {};
                    assemble (vTrial, fTrial, trialJacobian);
                    const T normTrial = infNorm (fTrial);

                    if (std::isfinite (normTrial) && normTrial < normF)
                    {
                        v = vTrial;
                        f = fTrial;
                        normF = normTrial;
                        for (int r = 0; r < numNonlinearPorts; ++r)
                            for (int c = 0; c < numNonlinearPorts; ++c)
                                jacobian[r][c] = trialJacobian[r][c];
                        improved = true;
                        break;
                    }
                    alpha *= (T) 0.5;
                    if (alpha < (T) 1.0e-12)
                        break;
                }

                if (! improved)
                    return false;
            }

            return false;
        }

        static T infNorm (const std::array<T, numNonlinearPorts>& x) noexcept
        {
            T norm = (T) 0;
            for (T value : x)
                norm = std::max (norm, std::abs (value));
            return norm;
        }

        /**
         * Validates finiteness and that the power-wave normalized nonlinear
         * sub-block is symmetric and contractive (2-norm at most 0.8).
         */
        bool validateMatrix (const T (&mat)[totalPorts][totalPorts])
        {
            for (int i = 0; i < totalPorts; ++i)
                for (int j = 0; j < totalPorts; ++j)
                    if (! std::isfinite (mat[i][j]))
                        return false;

            T normalized[numNonlinearPorts][numNonlinearPorts] {};
            T maxEntry = (T) 0;
            for (int k = 0; k < numNonlinearPorts; ++k)
            {
                for (int m = 0; m < numNonlinearPorts; ++m)
                {
                    // Power waves: a_pw = a / sqrt(R), so S_pw = sqrt(G) S sqrt(R).
                    normalized[k][m] = std::sqrt (nlG[k]) * sNonlinear[k][m] * std::sqrt (nlR[m]);
                    maxEntry = std::max (maxEntry, std::abs (normalized[k][m]));
                }
            }

            for (int k = 0; k < numNonlinearPorts; ++k)
                for (int m = k + 1; m < numNonlinearPorts; ++m)
                    if (std::abs (normalized[k][m] - normalized[m][k]) > (T) 1.0e-12 * ((T) 1.0 + maxEntry))
                        return false;

            // Power iteration for the 2-norm of the small symmetric block.
            std::array<T, numNonlinearPorts> x {};
            for (T& value : x)
                value = (T) 1.0;
            T xNorm = std::sqrt (dot (x, x));
            for (T& value : x)
                value /= xNorm;

            T norm = (T) 0;
            for (int iter = 0; iter < 30; ++iter)
            {
                std::array<T, numNonlinearPorts> y {};
                for (int k = 0; k < numNonlinearPorts; ++k)
                {
                    T sum = (T) 0;
                    for (int m = 0; m < numNonlinearPorts; ++m)
                        sum += normalized[k][m] * x[m];
                    y[k] = sum;
                }
                norm = std::sqrt (dot (y, y));
                if (norm == (T) 0)
                    break;
                for (int k = 0; k < numNonlinearPorts; ++k)
                    x[k] = y[k] / norm;
            }

            nonlinearBlockNorm = norm;
            return std::isfinite (norm) && norm <= (T) 0.8 + (T) 1.0e-12;
        }

        static T dot (const std::array<T, numNonlinearPorts>& a, const std::array<T, numNonlinearPorts>& b)
        {
            T sum = (T) 0;
            for (int k = 0; k < numNonlinearPorts; ++k)
                sum += a[k] * b[k];
            return sum;
        }

        std::tuple<PortTypes&...> downPorts;

        rtype_detail::Matrix<T, totalPorts> SMatrix; // transposed storage: SMatrix[j][i] == external S[i][j]
        rtype_detail::AlignedArray<T, totalPorts> aVec;
        rtype_detail::AlignedArray<T, totalPorts> bVec;

        T sLinear[numNonlinearPorts][numLinearPorts == 0 ? 1 : numLinearPorts] {};
        T sNonlinear[numNonlinearPorts][numNonlinearPorts] {};
        T cDrive[numNonlinearPorts] {};

        std::array<NonlinearDeviceType, numNonlinearPorts> deviceType {};
        std::array<T, numNonlinearPorts> nlR {};
        std::array<T, numNonlinearPorts> nlG {};
        std::array<T, numNonlinearPorts> nlIs {};
        std::array<T, numNonlinearPorts> nlVt {};
        std::array<T, numNonlinearPorts> nlInvVt {};

        // Newton scratch space (kept as members so compute() never allocates).
        std::array<T, numNonlinearPorts> portA {};
        std::array<T, numNonlinearPorts> portB {};
        std::array<T, numNonlinearPorts> waveFactor {};
        std::array<T, numNonlinearPorts> onePlusRSlope {};

        std::array<T, numNonlinearPorts> vWarm {}; // previous sample's voltages, used as warm start
        std::array<T, numNonlinearPorts> vState {};
        std::array<T, numNonlinearPorts> iState {};

        T lastResidual = (T) 0;
        int lastIterations = 0;
        T nonlinearBlockNorm = (T) 0;

        bool matrixValid = false;
        bool paramsValid = false;
    };
} // namespace wdft
} // namespace chowdsp

#endif //CHOWDSP_WDF_NONLINEAR_ROOT_H
