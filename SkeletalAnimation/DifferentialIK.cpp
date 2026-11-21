#include "DifferentialIK.h"

#include "SkeletalModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace SA
{
	namespace
	{
		// ---------------------------------------------------------------------
		// Small dense double-precision linear algebra. The problem size is tiny
		// (at most 16 hinges), so straightforward O(n^3) routines are enough.
		// ---------------------------------------------------------------------

		typedef std::vector<double>              Vec;
		typedef std::vector<Vec>                 Mat; // Mat[row][column]

		Vec ZeroVec(std::size_t n) { return Vec(n, 0.0); }

		Mat ZeroMat(std::size_t r, std::size_t c)
		{
			return Mat(r, Vec(c, 0.0));
		}

		Mat Identity(std::size_t n)
		{
			Mat m = ZeroMat(n, n);
			for (std::size_t i = 0; i < n; ++i) m[i][i] = 1.0;
			return m;
		}

		double Dot(const Vec& a, const Vec& b)
		{
			double s = 0.0;
			for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
			return s;
		}

		double Norm(const Vec& a) { return std::sqrt(Dot(a, a)); }

		Vec MatVec(const Mat& a, const Vec& x)
		{
			Vec y(a.size(), 0.0);
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				double s = 0.0;
				for (std::size_t j = 0; j < x.size(); ++j) s += a[i][j] * x[j];
				y[i] = s;
			}
			return y;
		}

		// One-sided Jacobi SVD of a (rows x cols): a = U diag(sigma) V^T.  Only V
		// and the singular values are needed.  Singular values whose ratio to the
		// largest is at most 1e-9 are reported as exactly zero.
		void Svd(const Mat& a, Mat& vOut, Vec& sigmaOut)
		{
			const std::size_t m = a.size();
			const std::size_t n = m ? a[0].size() : 0;
			Mat b = a;
			Mat v = Identity(n);

			for (int sweep = 0; sweep < 64; ++sweep)
			{
				bool changed = false;
				for (std::size_t p = 0; p < n; ++p)
				{
					for (std::size_t q = p + 1; q < n; ++q)
					{
						double alpha = 0.0, beta = 0.0, gamma = 0.0;
						for (std::size_t i = 0; i < m; ++i)
						{
							alpha += b[i][p] * b[i][p];
							beta  += b[i][q] * b[i][q];
							gamma += b[i][p] * b[i][q];
						}
						if (alpha == 0.0 || beta == 0.0) continue;
						double scale = std::sqrt(alpha) * std::sqrt(beta);
						if (scale == 0.0 || std::fabs(gamma) <= 1e-15 * scale) continue;

						changed = true;
						double zeta = (beta - alpha) / (2.0 * gamma);
						double t = (zeta >= 0.0)
							? 1.0 / (zeta + std::sqrt(1.0 + zeta * zeta))
							: -1.0 / (-zeta + std::sqrt(1.0 + zeta * zeta));
						double c = 1.0 / std::sqrt(1.0 + t * t);
						double s = c * t;

						for (std::size_t i = 0; i < m; ++i)
						{
							double bp = b[i][p], bq = b[i][q];
							b[i][p] = c * bp - s * bq;
							b[i][q] = s * bp + c * bq;
						}
						for (std::size_t i = 0; i < n; ++i)
						{
							double vp = v[i][p], vq = v[i][q];
							v[i][p] = c * vp - s * vq;
							v[i][q] = s * vp + c * vq;
						}
					}
				}
				if (!changed) break;
			}

			sigmaOut.assign(n, 0.0);
			for (std::size_t j = 0; j < n; ++j)
			{
				double s = 0.0;
				for (std::size_t i = 0; i < m; ++i) s += b[i][j] * b[i][j];
				sigmaOut[j] = std::sqrt(s);
			}
			vOut = v;
		}

		// Minimum-norm least squares solution of min ||A x - b||.  Directions
		// whose relative singular value is at most 1e-9 are treated as zero rank.
		Vec LeastSquares(const Mat& a, const Vec& b)
		{
			const std::size_t n = a.empty() ? b.size() : a[0].size();
			if (n == 0) return Vec();
			if (a.empty()) return ZeroVec(n);

			Mat v;
			Vec sigma;
			Svd(a, v, sigma);
			double smax = 0.0;
			for (std::size_t j = 0; j < sigma.size(); ++j) smax = std::max(smax, sigma[j]);
			double cutoff = smax * 1e-9;

			Vec y(n, 0.0);
			for (std::size_t j = 0; j < n; ++j)
			{
				if (sigma[j] <= 0.0 || sigma[j] <= cutoff) continue;
				double beta = 0.0;
				for (std::size_t i = 0; i < a.size(); ++i)
				{
					double av = 0.0;
					for (std::size_t k = 0; k < n; ++k) av += a[i][k] * v[k][j];
					beta += av * b[i];
				}
				y[j] = beta / (sigma[j] * sigma[j]);
			}
			return MatVec(v, y);
		}

		// Orthonormal basis (n x k) of the null space of a (rows x n).
		Mat NullSpace(const Mat& a, std::size_t n)
		{
			if (n == 0) return Mat();
			if (a.empty()) return Identity(n);

			Mat v;
			Vec sigma;
			Svd(a, v, sigma);
			double smax = 0.0;
			for (std::size_t j = 0; j < sigma.size(); ++j) smax = std::max(smax, sigma[j]);
			double cutoff = smax * 1e-9;

			std::size_t k = 0;
			for (std::size_t j = 0; j < n; ++j)
				if (sigma[j] <= cutoff || sigma[j] <= 0.0) ++k;

			Mat out = ZeroMat(n, k);
			std::size_t written = 0;
			for (std::size_t j = 0; j < n; ++j)
			{
				if (sigma[j] > cutoff && sigma[j] > 0.0) continue;
				for (std::size_t i = 0; i < n; ++i) out[i][written] = v[i][j];
				++written;
			}
			return out;
		}

		// ---------------------------------------------------------------------
		// Validation helpers.
		// ---------------------------------------------------------------------

		bool Finite(float x) { return std::isfinite(x); }

		bool FiniteVec(const glm::vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

		bool FiniteMat(const glm::mat4x4& m)
		{
			for (int c = 0; c < 4; ++c)
				for (int row = 0; row < 4; ++row)
					if (!Finite(m[c][row])) return false;
			return true;
		}

		bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

		// Finite and affine. The caller promises rigid transforms; a non-finite
		// value is the failure we actually have to report.
		bool IsFiniteAffine(const glm::mat4x4& m)
		{
			if (!FiniteMat(m)) return false;
			return Near(m[0][3], 0.0, 1e-3) && Near(m[1][3], 0.0, 1e-3) &&
			       Near(m[2][3], 0.0, 1e-3) && Near(m[3][3], 1.0, 1e-3);
		}

		// Bone 0 must be the unique root of a spanning, acyclic tree.
		bool ValidateTopology(const sSkeleton& skeleton)
		{
			const std::size_t n = skeleton.Bones.size();
			if (n == 0) return false;

			std::vector<int> parent(n, -1);
			for (std::size_t i = 0; i < n; ++i)
			{
				const sBone& bone = skeleton.Bones[i];
				if (bone.NumChildren > 0 && bone.pChildren == NULL) return false;
				for (unsigned int c = 0; c < bone.NumChildren; ++c)
				{
					unsigned int child = bone.pChildren[c];
					if (child >= n) return false;
					if (child == i) return false;
					if (parent[child] != -1) return false;
					parent[child] = static_cast<int>(i);
				}
			}
			if (parent[0] != -1) return false;
			for (std::size_t i = 1; i < n; ++i)
				if (parent[i] == -1) return false;

			std::vector<int> state(n, 0);
			std::vector<std::size_t> stack;
			stack.push_back(0);
			state[0] = 1;
			std::size_t reached = 0;
			while (!stack.empty())
			{
				std::size_t u = stack.back();
				stack.pop_back();
				++reached;
				const sBone& bone = skeleton.Bones[u];
				for (unsigned int c = 0; c < bone.NumChildren; ++c)
				{
					std::size_t child = bone.pChildren[c];
					if (state[child] != 0) return false;
					state[child] = 1;
					stack.push_back(child);
				}
			}
			return reached == n;
		}

		bool ValidateConfiguration(const SkeletalModel& model)
		{
			const sSkeleton& skeleton = model.GetSkeleton();
			if (!ValidateTopology(skeleton)) return false;
			if (!IsFiniteAffine(model.GetGlobalInverseTransform())) return false;
			for (std::size_t i = 0; i < skeleton.Bones.size(); ++i)
			{
				if (!IsFiniteAffine(skeleton.Bones[i].NodeTransform)) return false;
				if (!IsFiniteAffine(skeleton.Bones[i].OffsetMatrix)) return false;
			}
			return true;
		}

		// ---------------------------------------------------------------------
		// Animation sampling in double precision (never reads FinalTransformation,
		// so previously corrected matrices can never feed back in).
		// ---------------------------------------------------------------------

		glm::dquat NormalizeQuat(const glm::dquat& q)
		{
			double len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (len <= 0.0) return glm::dquat(1.0, 0.0, 0.0, 0.0);
			return glm::dquat(q.w / len, q.x / len, q.y / len, q.z / len);
		}

		glm::dquat Slerp(const glm::dquat& aIn, const glm::dquat& bIn, double f)
		{
			glm::dquat a = NormalizeQuat(aIn);
			glm::dquat b = NormalizeQuat(bIn);
			double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
			if (d < 0.0) { b = glm::dquat(-b.w, -b.x, -b.y, -b.z); d = -d; }
			if (d > 0.9995)
			{
				glm::dquat r(a.w + (b.w - a.w) * f, a.x + (b.x - a.x) * f,
				             a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f);
				return NormalizeQuat(r);
			}
			double theta = std::acos(std::max(-1.0, std::min(1.0, d)));
			double s = std::sin(theta);
			double wa = std::sin((1.0 - f) * theta) / s;
			double wb = std::sin(f * theta) / s;
			return glm::dquat(a.w * wa + b.w * wb, a.x * wa + b.x * wb,
			                  a.y * wa + b.y * wb, a.z * wa + b.z * wb);
		}

		glm::dvec3 SamplePosition(const std::vector<sNodeAnimationKey<glm::vec3> >& keys, double t)
		{
			if (keys.empty()) return glm::dvec3(0.0);
			if (keys.size() == 1 || t <= double(keys.front().Time)) return glm::dvec3(keys.front().Value);
			if (t >= double(keys.back().Time)) return glm::dvec3(keys.back().Value);
			for (std::size_t i = 0; i + 1 < keys.size(); ++i)
			{
				double t0 = keys[i].Time;
				double t1 = keys[i + 1].Time;
				if (t <= t1)
				{
					double denom = t1 - t0;
					double f = denom > 0.0 ? (t - t0) / denom : 0.0;
					glm::dvec3 a(keys[i].Value);
					glm::dvec3 b(keys[i + 1].Value);
					return a + (b - a) * f;
				}
			}
			return glm::dvec3(keys.back().Value);
		}

		glm::dquat SampleRotation(const std::vector<sNodeAnimationKey<glm::quat> >& keys, double t)
		{
			if (keys.empty()) return glm::dquat(1.0, 0.0, 0.0, 0.0);
			if (keys.size() == 1 || t <= double(keys.front().Time))
			{
				glm::quat q = keys.front().Value;
				return glm::dquat(double(q.w), double(q.x), double(q.y), double(q.z));
			}
			if (t >= double(keys.back().Time))
			{
				glm::quat q = keys.back().Value;
				return glm::dquat(double(q.w), double(q.x), double(q.y), double(q.z));
			}
			for (std::size_t i = 0; i + 1 < keys.size(); ++i)
			{
				double t0 = keys[i].Time;
				double t1 = keys[i + 1].Time;
				if (t <= t1)
				{
					double denom = t1 - t0;
					double f = denom > 0.0 ? (t - t0) / denom : 0.0;
					glm::quat qa = keys[i].Value;
					glm::quat qb = keys[i + 1].Value;
					return Slerp(glm::dquat(double(qa.w), double(qa.x), double(qa.y), double(qa.z)),
					             glm::dquat(double(qb.w), double(qb.x), double(qb.y), double(qb.z)), f);
				}
			}
			glm::quat q = keys.back().Value;
			return glm::dquat(double(q.w), double(q.x), double(q.y), double(q.z));
		}

		// The rest of the project uses GLM in degree mode; theta and the current
		// action's keyframes are in radians here, so build the correction quat
		// directly rather than going through glm::angleAxis().
		glm::dquat RotationFromRadians(double angle, const glm::dvec3& axis)
		{
			double half = 0.5 * angle;
			double s = std::sin(half);
			return glm::dquat(std::cos(half), axis.x * s, axis.y * s, axis.z * s);
		}

		const sNodeAnimation* FindNodeAnimByName(const sAnimation& anim, const std::string& name)
		{
			for (std::size_t i = 0; i < anim.NodeAnimations.size(); ++i)
				if (anim.NodeAnimations[i].Name == name) return &anim.NodeAnimations[i];
			return NULL;
		}

		// ---------------------------------------------------------------------
		// Box-constrained least-squares QP with linear equality constraints:
		//
		//   minimize ||A x - b||^2   subject to   E x = d,  l <= x <= u
		//
		// A primal active-set method.  The free step is computed in the null
		// space of the working set (accumulated equalities + pins), so the PSD
		// normal equations are never formed and rank-deficient Jacobians are
		// handled by the SVD singular-value cutoff.
		// ---------------------------------------------------------------------

		struct QpResult
		{
			Vec x;
			std::vector<int> active;
		};

		Mat BuildWorkingSet(const Mat& equality, std::size_t n, const std::vector<int>& active)
		{
			Mat c = equality;
			for (std::size_t k = 0; k < active.size(); ++k)
			{
				Vec row(n, 0.0);
				row[active[k]] = 1.0;
				c.push_back(row);
			}
			return c;
		}

		// A step p satisfying the homogeneous working set, chosen to decrease the
		// objective within that subspace.  Also corrects small equality drift.
		Vec ConstrainedStep(const Mat& a, const Vec& r, const Mat& equality,
		                    const Vec& eqRhs, const Vec& x, const std::vector<int>& active,
		                    std::size_t n)
		{
			Mat c = BuildWorkingSet(equality, n, active);
			Mat z = NullSpace(c, n);
			std::size_t k = z.empty() ? 0 : z[0].size();

			// Particular step satisfying the whole working set: restore equality
			// drift and hold pinned coordinates at their current values.
			Vec target(c.size(), 0.0);
			for (std::size_t e = 0; e < equality.size(); ++e)
				target[e] = eqRhs[e] - Dot(equality[e], x);
			for (std::size_t q = 0; q < active.size(); ++q)
				target[equality.size() + q] = 0.0;

			bool drift = false;
			for (std::size_t e = 0; e < target.size(); ++e)
				if (std::fabs(target[e]) > 1e-14) drift = true;

			Vec p0 = ZeroVec(n);
			if (drift && !c.empty()) p0 = LeastSquares(c, target);

			Vec r2 = r;
			Vec ap0 = MatVec(a, p0);
			for (std::size_t i = 0; i < r2.size(); ++i) r2[i] -= ap0[i];

			Vec p = p0;
			if (k > 0 && !a.empty())
			{
				Mat az = ZeroMat(a.size(), k);
				for (std::size_t i = 0; i < a.size(); ++i)
					for (std::size_t j = 0; j < k; ++j)
					{
						double s = 0.0;
						for (std::size_t t = 0; t < n; ++t) s += a[i][t] * z[t][j];
						az[i][j] = s;
					}
				Vec y = LeastSquares(az, r2);
				Vec zp = MatVec(z, y);
				for (std::size_t i = 0; i < n; ++i) p[i] += zp[i];
			}
			return p;
		}

		QpResult SolveBoxQp(const Mat& a, const Vec& b, const Mat& equality,
		                   const Vec& eqRhs, const Vec& l, const Vec& u, const Vec& x0)
		{
			const std::size_t n = l.size();
			QpResult result;
			result.x = x0;
			if (n == 0) return result;

			std::vector<int> active;
			const int maxIterations = 8192;

			for (int iteration = 0; iteration < maxIterations; ++iteration)
			{
				Vec ax = MatVec(a, result.x);
				Vec r(b.size(), 0.0);
				for (std::size_t i = 0; i < b.size(); ++i) r[i] = b[i] - ax[i];

				Vec p = ConstrainedStep(a, r, equality, eqRhs, result.x, active, n);
				double pnorm = Norm(p);
				double xscale = 1.0;
				for (std::size_t i = 0; i < n; ++i) xscale = std::max(xscale, std::fabs(result.x[i]));

				if (pnorm <= 1e-12 * xscale)
				{
					// KKT point: try releasing each pin.  If the relaxed step moves
					// the variable away from its bound it was not really binding.
					bool released = false;
					for (std::size_t k = 0; k < active.size(); ++k)
					{
						int idx = active[k];
						if (l[idx] >= u[idx]) continue; // effectively fixed
						std::vector<int> trial;
						for (std::size_t q = 0; q < active.size(); ++q)
							if (q != k) trial.push_back(active[q]);
						Vec p2 = ConstrainedStep(a, r, equality, eqRhs, result.x, trial, n);
						bool atLower = std::fabs(result.x[idx] - l[idx]) <= 1e-9 * xscale;
						bool atUpper = std::fabs(result.x[idx] - u[idx]) <= 1e-9 * xscale;
						// At a lower bound a descent step moves up; at an upper
						// bound it moves down.
						bool wantsInterior = (atLower && p2[idx] > 1e-12 * xscale) ||
						                     (atUpper && p2[idx] < -1e-12 * xscale);
						if (wantsInterior && Norm(p2) > 1e-12 * xscale)
						{
							active = trial;
							released = true;
							break;
						}
					}
					if (!released)
					{
						result.active = active;
						return result;
					}
					continue;
				}

				double alpha = 1.0;
				int blocking = -1;
				for (std::size_t i = 0; i < n; ++i)
				{
					bool isActive = false;
					for (std::size_t k = 0; k < active.size(); ++k)
						if (active[k] == static_cast<int>(i)) { isActive = true; break; }
					if (isActive) continue;
					if (p[i] < 0.0)
					{
						double step = (l[i] - result.x[i]) / p[i];
						if (step < alpha) { alpha = step; blocking = static_cast<int>(i); }
					}
					else if (p[i] > 0.0)
					{
						double step = (u[i] - result.x[i]) / p[i];
						if (step < alpha) { alpha = step; blocking = static_cast<int>(i); }
					}
				}
				if (alpha < 0.0) alpha = 0.0;
				if (alpha > 1.0) alpha = 1.0;
				for (std::size_t i = 0; i < n; ++i) result.x[i] += alpha * p[i];

				if (alpha < 1.0 && blocking >= 0)
				{
					result.x[blocking] = (p[blocking] < 0.0) ? l[blocking] : u[blocking];
					bool exists = false;
					for (std::size_t k = 0; k < active.size(); ++k)
						if (active[k] == blocking) exists = true;
					if (!exists) active.push_back(blocking);
				}
				else
				{
					for (std::size_t i = 0; i < n; ++i)
					{
						if (result.x[i] < l[i]) result.x[i] = l[i];
						if (result.x[i] > u[i]) result.x[i] = u[i];
					}
				}
			}

			result.active = active;
			return result;
		}

		// Append equalities that pin the complete optimal set of one priority
		// layer, given the equalities already accumulated for higher layers.
		//
		// For f(x) = ||A x - b||^2 the set of minimizers over the current affine
		// space intersected with the box is exactly
		//     { x feasible : A x = A x* }
		// (convexity: f(x) - f(x*) = grad(x*) . (x - x*) + ||A (x - x*)||^2 with
		// both terms non-negative at the optimum).  Expressed in the tangent
		// space of the previously accumulated equalities only the row space of
		// A Z contributes, so the added rows are well conditioned and stay
		// compatible with the box (x* itself satisfies them).
		void AddLayerEqualities(const Mat& a, const Vec& x, Mat& equality, Vec& eqRhs)
		{
			const std::size_t n = x.size();
			Mat z = NullSpace(equality, n);
			std::size_t k = z.empty() ? 0 : z[0].size();
			if (k == 0 || a.empty()) return;

			// M = A Z  (rows x k)
			Mat m = ZeroMat(a.size(), k);
			for (std::size_t i = 0; i < a.size(); ++i)
				for (std::size_t j = 0; j < k; ++j)
				{
					double s = 0.0;
					for (std::size_t t = 0; t < n; ++t) s += a[i][t] * z[t][j];
					m[i][j] = s;
				}

			Mat v;
			Vec sigma;
			Svd(m, v, sigma);
			double smax = 0.0;
			for (std::size_t j = 0; j < sigma.size(); ++j) smax = std::max(smax, sigma[j]);
			if (smax <= 0.0) return;
			double cutoff = smax * 1e-9;

			for (std::size_t j = 0; j < k; ++j)
			{
				if (sigma[j] <= cutoff) continue;
				Vec row(n, 0.0);
				for (std::size_t i = 0; i < n; ++i)
				{
					double s = 0.0;
					for (std::size_t t = 0; t < k; ++t) s += z[i][t] * v[t][j];
					row[i] = s;
				}
				equality.push_back(row);
				eqRhs.push_back(Dot(row, x));
			}
		}
	} // anonymous namespace

	// -------------------------------------------------------------------------
	// SkeletalModel methods.
	// -------------------------------------------------------------------------

	bool SkeletalModel::ConfigureIK(const std::vector<sIKHinge>& a_Hinges)
	{
		if (a_Hinges.size() > 16) return false;
		if (!ValidateConfiguration(*this)) return false;

		std::vector<bool> used(m_Skeleton.Bones.size(), false);
		for (std::size_t i = 0; i < a_Hinges.size(); ++i)
		{
			const sIKHinge& h = a_Hinges[i];
			if (h.BoneIndex >= m_Skeleton.Bones.size()) return false;
			if (used[h.BoneIndex]) return false;
			used[h.BoneIndex] = true;
			if (!FiniteVec(h.Axis)) return false;
			double len = std::sqrt(double(h.Axis.x) * h.Axis.x + double(h.Axis.y) * h.Axis.y +
			                       double(h.Axis.z) * h.Axis.z);
			if (std::fabs(len - 1.0) > 1e-4) return false;
			if (!Finite(h.MinAngle) || !Finite(h.MaxAngle) || !Finite(h.MaxSpeed)) return false;
			if (h.MinAngle > 0.0f || h.MaxAngle < 0.0f) return false;
			if (h.MaxSpeed <= 0.0f) return false;
		}

		m_IKHinges = a_Hinges;
		m_IKTheta.assign(a_Hinges.size(), 0.0);
		m_IKConfigured = true;
		m_IKReport = sIKReport();
		m_IKReport.Valid = false;
		return true;
	}

	bool SkeletalModel::SetIKTasks(const std::vector<sIKTask>& a_Tasks)
	{
		for (std::size_t i = 0; i < a_Tasks.size(); ++i)
		{
			const sIKTask& t = a_Tasks[i];
			if (t.BoneIndex >= m_Skeleton.Bones.size()) return false;
			if (!FiniteVec(t.LocalPoint) || !FiniteVec(t.DesiredVelocity)) return false;
			if (!Finite(t.Weight) || t.Weight <= 0.0f) return false;
		}
		m_IKTasks = a_Tasks;
		return true;
	}

	void SkeletalModel::DisableIK()
	{
		m_IKConfigured = false;
		m_IKHinges.clear();
		m_IKTasks.clear();
		m_IKTheta.clear();
		m_IKReport = sIKReport();
		m_IKReport.Valid = false;
	}

	// -------------------------------------------------------------------------
	// FK with per-hinge theta, in double precision.
	// -------------------------------------------------------------------------

	void SkeletalModel::ComputeIKGlobals(float a_AnimationTime, const std::vector<double>& a_Theta,
	                                     std::vector<glm::dmat4x4>& a_Globals) const
	{
		const std::size_t n = m_Skeleton.Bones.size();
		a_Globals.assign(n, glm::dmat4x4(1.0));
		if (n == 0) return;

		std::vector<int> parent(n, -1);
		for (std::size_t i = 0; i < n; ++i)
		{
			const sBone& bone = m_Skeleton.Bones[i];
			for (unsigned int c = 0; c < bone.NumChildren; ++c)
			{
				std::size_t child = bone.pChildren[c];
				if (child < n) parent[child] = static_cast<int>(i);
			}
		}

		std::vector<std::size_t> order;
		order.reserve(n);
		std::vector<std::size_t> stack;
		stack.push_back(0);
		while (!stack.empty())
		{
			std::size_t u = stack.back();
			stack.pop_back();
			order.push_back(u);
			const sBone& bone = m_Skeleton.Bones[u];
			for (unsigned int c = 0; c < bone.NumChildren; ++c)
			{
				std::size_t child = bone.pChildren[c];
				if (child < n) stack.push_back(child);
			}
		}

		std::vector<int> hingeOf(n, -1);
		for (std::size_t h = 0; h < m_IKHinges.size() && h < a_Theta.size(); ++h)
			if (m_IKHinges[h].BoneIndex < n) hingeOf[m_IKHinges[h].BoneIndex] = static_cast<int>(h);

		const double t = double(a_AnimationTime);
		for (std::size_t oi = 0; oi < order.size(); ++oi)
		{
			std::size_t i = order[oi];
			const sBone& bone = m_Skeleton.Bones[i];
			glm::dmat4 local(1.0);

			const sNodeAnimation* channel = FindNodeAnimByName(m_Animation, bone.Name);
			if (channel)
			{
				glm::dvec3 pos = SamplePosition(channel->PositionKeys, t);
				glm::dquat rot = SampleRotation(channel->RotationKeys, t);
				local = glm::mat4_cast(rot);
				local[3] = glm::dvec4(pos, 1.0);
			}
			else
			{
				local = glm::dmat4(bone.NodeTransform);
			}

			int h = hingeOf[i];
			if (h >= 0)
			{
				glm::dvec3 axis(double(m_IKHinges[h].Axis.x), double(m_IKHinges[h].Axis.y),
				                double(m_IKHinges[h].Axis.z));
				// Right multiplication rotates the bone locally and leaves the
				// local translation untouched.
				local = local * glm::mat4_cast(RotationFromRadians(a_Theta[h], axis));
			}

			int p = parent[i];
			a_Globals[i] = (p >= 0) ? a_Globals[p] * local : local;
		}
	}

	void SkeletalModel::ApplyIKGlobals(const std::vector<glm::dmat4x4>& a_Globals)
	{
		glm::dmat4 gi = glm::dmat4(m_GlobalInverseTransform);
		for (std::size_t i = 0; i < m_Skeleton.Bones.size() && i < a_Globals.size(); ++i)
		{
			glm::dmat4 final = gi * a_Globals[i] * glm::dmat4(m_Skeleton.Bones[i].OffsetMatrix);
			glm::mat4x4 out;
			for (int c = 0; c < 4; ++c)
				for (int row = 0; row < 4; ++row) out[c][row] = float(final[c][row]);
			m_Skeleton.Bones[i].FinalTransformation = out;
		}
		TransformVertices(m_Skeleton);
	}

	// -------------------------------------------------------------------------
	// Controller step.
	// -------------------------------------------------------------------------

	bool SkeletalModel::UpdateIK(float a_Dt)
	{
		if (!m_IKConfigured) return false;
		if (!std::isfinite(a_Dt) || a_Dt < 0.0f || a_Dt > 0.1f) return false;

		const std::size_t n = m_Skeleton.Bones.size();
		if (n == 0) return false;
		const std::size_t h = m_IKHinges.size();
		const std::size_t numTasks = m_IKTasks.size();

		const double dt = double(a_Dt);
		const float oldTime = m_AnimationTime;
		float newTime = oldTime;
		if (a_Dt > 0.0f && std::isfinite(m_Animation.Duration) && m_Animation.Duration > 0.0f)
			newTime = fmodf(oldTime + a_Dt * m_Animation.TicksPerSecond, m_Animation.Duration);

		const std::vector<double> thetaOld = m_IKTheta;

		// Ascending priority order shared by the solve and the report.
		std::vector<unsigned int> priorities;
		for (std::size_t i = 0; i < numTasks; ++i)
		{
			unsigned int p = m_IKTasks[i].Priority;
			if (std::find(priorities.begin(), priorities.end(), p) == priorities.end())
				priorities.push_back(p);
		}
		std::sort(priorities.begin(), priorities.end());

		if (a_Dt == 0.0f)
		{
			std::vector<glm::dmat4x4> globals;
			ComputeIKGlobals(oldTime, thetaOld, globals);
			ApplyIKGlobals(globals);

			m_IKReport.Valid = true;
			m_IKReport.Dt = 0.0f;
			m_IKReport.AnimationTime = oldTime;
			m_IKReport.AngularVelocity.assign(h, 0.0f);
			m_IKReport.Angles.assign(h, 0.0f);
			for (std::size_t j = 0; j < h; ++j) m_IKReport.Angles[j] = float(thetaOld[j]);
			m_IKReport.LayerResidual.assign(priorities.size(), 0.0f);

			m_IKReport.EndEffectorPosition.assign(numTasks, glm::vec3(0.0f));
			glm::dmat4 gi = glm::dmat4(m_GlobalInverseTransform);
			for (std::size_t ti = 0; ti < numTasks; ++ti)
			{
				const sIKTask& task = m_IKTasks[ti];
				glm::dvec3 x(double(task.LocalPoint.x), double(task.LocalPoint.y), double(task.LocalPoint.z));
				glm::dvec4 p = gi * (globals[task.BoneIndex] * glm::dvec4(x, 1.0));
				m_IKReport.EndEffectorPosition[ti] = glm::vec3(float(p.x), float(p.y), float(p.z));
			}
			return true;
		}

		std::vector<glm::dmat4x4> oldGlobals, newGlobals;
		ComputeIKGlobals(oldTime, thetaOld, oldGlobals);
		ComputeIKGlobals(newTime, thetaOld, newGlobals);

		const glm::dmat4 gi = glm::dmat4(m_GlobalInverseTransform);

		std::vector<int> parent(n, -1);
		for (std::size_t i = 0; i < n; ++i)
		{
			const sBone& bone = m_Skeleton.Bones[i];
			for (unsigned int c = 0; c < bone.NumChildren; ++c)
			{
				std::size_t child = bone.pChildren[c];
				if (child < n) parent[child] = static_cast<int>(i);
			}
		}

		std::vector<std::size_t> hingeBone(h);
		for (std::size_t j = 0; j < h; ++j) hingeBone[j] = m_IKHinges[j].BoneIndex;

		// v_anim and the analytic Jacobian of every task point w.r.t. theta.
		std::vector<glm::dvec3> vAnim(numTasks, glm::dvec3(0.0));
		std::vector<Mat> jac(numTasks, ZeroMat(3, h));
		for (std::size_t ti = 0; ti < numTasks; ++ti)
		{
			const sIKTask& task = m_IKTasks[ti];
			glm::dvec3 x(double(task.LocalPoint.x), double(task.LocalPoint.y), double(task.LocalPoint.z));
			glm::dvec3 pOld(gi * (oldGlobals[task.BoneIndex] * glm::dvec4(x, 1.0)));
			glm::dvec3 pNew(gi * (newGlobals[task.BoneIndex] * glm::dvec4(x, 1.0)));
			vAnim[ti] = (pNew - pOld) / dt;

			std::vector<bool> isAncestor(n, false);
			int cur = static_cast<int>(task.BoneIndex);
			int guard = 0;
			while (cur >= 0 && static_cast<std::size_t>(cur) < n && guard++ <= static_cast<int>(n))
			{
				isAncestor[cur] = true;
				cur = parent[cur];
			}

			glm::dvec3 pointInWorld = glm::dvec3(newGlobals[task.BoneIndex] * glm::dvec4(x, 1.0));
			for (std::size_t j = 0; j < h; ++j)
			{
				if (!isAncestor[hingeBone[j]]) continue;
				glm::dmat4 gj = newGlobals[hingeBone[j]];
				glm::dvec3 w = glm::dvec3(glm::inverse(gj) * glm::dvec4(pointInWorld, 1.0));
				glm::dvec3 axis(double(m_IKHinges[j].Axis.x), double(m_IKHinges[j].Axis.y),
				                double(m_IKHinges[j].Axis.z));
				glm::dvec3 localVel = glm::cross(axis, w);
				glm::dmat3 rot(gi * gj); // rigid rotation part only
				glm::dvec3 col = rot * localVel;
				jac[ti][0][j] = col.x;
				jac[ti][1][j] = col.y;
				jac[ti][2][j] = col.z;
			}
		}

		// Bounds on nu for the integration step.
		Vec lo(h, 0.0), hi(h, 0.0);
		for (std::size_t j = 0; j < h; ++j)
		{
			double maxSpeed = double(m_IKHinges[j].MaxSpeed);
			double theta = thetaOld[j];
			double l = std::max(-maxSpeed, (double(m_IKHinges[j].MinAngle) - theta) / dt);
			double u = std::min(maxSpeed, (double(m_IKHinges[j].MaxAngle) - theta) / dt);
			if (l > u) { l = 0.0; u = 0.0; }
			lo[j] = l;
			hi[j] = u;
		}

		// Strictly prioritized hierarchy.
		Vec nu = ZeroVec(h);
		Mat equality;
		Vec eqRhs;
		if (h > 0)
		{
			for (std::size_t li = 0; li < priorities.size(); ++li)
			{
				std::size_t rows = 0;
				for (std::size_t ti = 0; ti < numTasks; ++ti)
					if (m_IKTasks[ti].Priority == priorities[li]) rows += 3;

				Mat a = ZeroMat(rows, h);
				Vec b = ZeroVec(rows);
				std::size_t r = 0;
				for (std::size_t ti = 0; ti < numTasks; ++ti)
				{
					if (m_IKTasks[ti].Priority != priorities[li]) continue;
					double sw = std::sqrt(double(m_IKTasks[ti].Weight));
					glm::dvec3 target = glm::dvec3(double(m_IKTasks[ti].DesiredVelocity.x),
					                               double(m_IKTasks[ti].DesiredVelocity.y),
					                               double(m_IKTasks[ti].DesiredVelocity.z)) - vAnim[ti];
					for (int rr = 0; rr < 3; ++rr)
					{
						for (std::size_t j = 0; j < h; ++j) a[r][j] = sw * jac[ti][rr][j];
						b[r] = sw * target[rr];
						++r;
					}
				}

				QpResult stage = SolveBoxQp(a, b, equality, eqRhs, lo, hi, nu);
				nu = stage.x;
				AddLayerEqualities(a, nu, equality, eqRhs);
			}

			// Minimum Euclidean norm inside the common optimal set.
			QpResult minNorm = SolveBoxQp(Identity(h), ZeroVec(h), equality, eqRhs, lo, hi, nu);
			nu = minNorm.x;
		}

		// One integration, then one full FK pass and the original skinning.
		std::vector<double> thetaNew(h, 0.0);
		for (std::size_t j = 0; j < h; ++j)
		{
			thetaNew[j] = thetaOld[j] + dt * nu[j];
			if (thetaNew[j] < double(m_IKHinges[j].MinAngle)) thetaNew[j] = double(m_IKHinges[j].MinAngle);
			if (thetaNew[j] > double(m_IKHinges[j].MaxAngle)) thetaNew[j] = double(m_IKHinges[j].MaxAngle);
		}
		std::vector<glm::dmat4x4> finalGlobals;
		ComputeIKGlobals(newTime, thetaNew, finalGlobals);
		ApplyIKGlobals(finalGlobals);

		m_AnimationTime = newTime;
		m_IKTheta = thetaNew;

		m_IKReport.Valid = true;
		m_IKReport.Dt = a_Dt;
		m_IKReport.AnimationTime = newTime;
		m_IKReport.AngularVelocity.assign(h, 0.0f);
		m_IKReport.Angles.assign(h, 0.0f);
		for (std::size_t j = 0; j < h; ++j)
		{
			m_IKReport.AngularVelocity[j] = float(nu[j]);
			m_IKReport.Angles[j] = float(thetaNew[j]);
		}

		// Linear residuals use the pre-integration J, v_anim and nu.
		m_IKReport.LayerResidual.assign(priorities.size(), 0.0f);
		for (std::size_t li = 0; li < priorities.size(); ++li)
		{
			double sum = 0.0;
			for (std::size_t ti = 0; ti < numTasks; ++ti)
			{
				if (m_IKTasks[ti].Priority != priorities[li]) continue;
				double sq = 0.0;
				for (std::size_t rr = 0; rr < 3; ++rr)
				{
					double vel = vAnim[ti][rr];
					for (std::size_t j = 0; j < h; ++j) vel += jac[ti][rr][j] * nu[j];
					double want = double(m_IKTasks[ti].DesiredVelocity[rr]);
					double d = vel - want;
					sq += d * d;
				}
				sum += double(m_IKTasks[ti].Weight) * sq;
			}
			m_IKReport.LayerResidual[li] = float(sum);
		}

		m_IKReport.EndEffectorPosition.assign(numTasks, glm::vec3(0.0f));
		for (std::size_t ti = 0; ti < numTasks; ++ti)
		{
			const sIKTask& task = m_IKTasks[ti];
			glm::dvec3 x(double(task.LocalPoint.x), double(task.LocalPoint.y), double(task.LocalPoint.z));
			glm::dvec4 p = gi * (finalGlobals[task.BoneIndex] * glm::dvec4(x, 1.0));
			m_IKReport.EndEffectorPosition[ti] = glm::vec3(float(p.x), float(p.y), float(p.z));
		}

		return true;
	}

	// -------------------------------------------------------------------------
	// Free-function facade.
	// -------------------------------------------------------------------------

	bool ConfigureIK(SkeletalModel& a_Model, const std::vector<sIKHinge>& a_Hinges)
	{
		return a_Model.ConfigureIK(a_Hinges);
	}

	bool SetIKTasks(SkeletalModel& a_Model, const std::vector<sIKTask>& a_Tasks)
	{
		return a_Model.SetIKTasks(a_Tasks);
	}

	bool UpdateIK(SkeletalModel& a_Model, float a_Dt)
	{
		return a_Model.UpdateIK(a_Dt);
	}

	void DisableIK(SkeletalModel& a_Model)
	{
		a_Model.DisableIK();
	}

	const sIKReport& GetIKReport(const SkeletalModel& a_Model)
	{
		return a_Model.GetIKReport();
	}
}
