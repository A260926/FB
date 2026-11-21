// Shared helpers for tests/differential_ik.cpp.
//
// Everything here is independent of DifferentialIK.cpp's internal solver:
//  * a scalar FK in double precision, sampled straight from the animation data;
//  * a central-difference Jacobian built from that FK;
//  * a brute-force box-QP enumerator used to cross-check the strict priority
//    solve (it enumerates active sets and solves each sub-problem separately).

#ifndef DIFFERENTIAL_IK_TEST_SUPPORT_H
#define DIFFERENTIAL_IK_TEST_SUPPORT_H

#include "SkeletalAnimation/SkeletalModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace dik_test
{
	using SA::SkeletalModel;
	using SA::sAnimation;
	using SA::sBone;
	using SA::sIKHinge;
	using SA::sIKReport;
	using SA::sIKTask;
	using SA::sNodeAnimation;
	using SA::sNodeAnimationKey;
	using SA::sSkeleton;

	inline bool CloseF(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

	inline bool CloseV(const glm::vec3& a, const glm::vec3& b, float tol)
	{
		return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol;
	}

	// ---------------------------------------------------------------------
	// Scene construction.
	// ---------------------------------------------------------------------

	inline void SetChildren(sSkeleton& a_Skeleton, unsigned int a_Bone, const std::vector<unsigned int>& a_Children)
	{
		delete[] a_Skeleton.Bones[a_Bone].pChildren;
		a_Skeleton.Bones[a_Bone].NumChildren = static_cast<unsigned int>(a_Children.size());
		a_Skeleton.Bones[a_Bone].pChildren = a_Children.empty() ? NULL : new unsigned int[a_Children.size()];
		for (std::size_t i = 0; i < a_Children.size(); ++i)
			a_Skeleton.Bones[a_Bone].pChildren[i] = a_Children[i];
	}

	inline void InitBone(sBone& a_Bone, const char* a_Name, const glm::mat4x4& a_NodeTransform)
	{
		a_Bone.Name = a_Name;
		a_Bone.NodeTransform = a_NodeTransform;
		a_Bone.OffsetMatrix = glm::mat4x4(1);
		a_Bone.FinalTransformation = glm::mat4x4(1);
		a_Bone.NumWeights = 0;
		a_Bone.pWeights = NULL;
		a_Bone.NumChildren = 0;
		a_Bone.pChildren = NULL;
	}

	// Three-bone arm on +x: root -> upper -> lower, joints one unit apart.
	inline void BuildArm(SkeletalModel& a_Model)
	{
		sSkeleton& skeleton = a_Model.GetSkeleton();
		skeleton.Bones.resize(3);
		InitBone(skeleton.Bones[0], "root", glm::mat4x4(1));
		InitBone(skeleton.Bones[1], "upper", glm::translate(glm::vec3(1, 0, 0)));
		InitBone(skeleton.Bones[2], "lower", glm::translate(glm::vec3(1, 0, 0)));
		SetChildren(skeleton, 0, { 1 });
		SetChildren(skeleton, 1, { 2 });
		SetChildren(skeleton, 2, {});

		sAnimation& anim = a_Model.GetAnimation();
		anim.Duration = 100.0f;
		anim.TicksPerSecond = 0.0f;
		sNodeAnimation channel;
		channel.Name = "root";
		channel.PositionKeys.push_back({ glm::vec3(0), 0.0f });
		channel.RotationKeys.push_back({ glm::quat(1, 0, 0, 0), 0.0f });
		anim.NodeAnimations.push_back(channel);
	}

	// ---------------------------------------------------------------------
	// Independent scalar FK in double precision.
	// ---------------------------------------------------------------------

	struct ScalarModel
	{
		struct Node
		{
			int             Parent = -1;
			bool            HasChannel = false;
			glm::dmat4      FixedLocal = glm::dmat4(1.0);
			std::vector<sNodeAnimationKey<glm::vec3> > Position;
			std::vector<sNodeAnimationKey<glm::quat> > Rotation;
			glm::dvec3      Axis = glm::dvec3(0.0);
			int             Hinge = -1;
		};

		std::vector<Node> Nodes;
		glm::dmat4        GlobalInverse = glm::dmat4(1.0);

		void Build(const SkeletalModel& a_Model)
		{
			const sSkeleton& skeleton = a_Model.GetSkeleton();
			const sAnimation& anim = a_Model.GetAnimation();
			Nodes.assign(skeleton.Bones.size(), Node());
			for (std::size_t i = 0; i < skeleton.Bones.size(); ++i)
			{
				const sBone& bone = skeleton.Bones[i];
				Node& node = Nodes[i];
				for (std::size_t c = 0; c < anim.NodeAnimations.size(); ++c)
				{
					if (anim.NodeAnimations[c].Name == bone.Name)
					{
						node.HasChannel = true;
						node.Position = anim.NodeAnimations[c].PositionKeys;
						node.Rotation = anim.NodeAnimations[c].RotationKeys;
					}
				}
				if (!node.HasChannel) node.FixedLocal = glm::dmat4(bone.NodeTransform);
				for (unsigned int c = 0; c < bone.NumChildren; ++c)
					Nodes[bone.pChildren[c]].Parent = static_cast<int>(i);
			}
			GlobalInverse = glm::dmat4(a_Model.GetGlobalInverseTransform());
		}

		void SetHinge(unsigned int a_Bone, int a_Index, const glm::dvec3& a_Axis, const std::vector<double>& a_Theta)
		{
			Nodes[a_Bone].Hinge = a_Index;
			Nodes[a_Bone].Axis = a_Axis;
			(void)a_Theta;
		}

		static glm::dquat AxisAngle(double angle, const glm::dvec3& axis)
		{
			double h = 0.5 * angle;
			return glm::dquat(std::cos(h), axis.x * std::sin(h), axis.y * std::sin(h), axis.z * std::sin(h));
		}

		static glm::dquat Slerp(glm::dquat a, glm::dquat b, double f)
		{
			auto norm = [](const glm::dquat& q) {
				double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
				return l > 0.0 ? glm::dquat(q.w / l, q.x / l, q.y / l, q.z / l) : glm::dquat(1.0, 0.0, 0.0, 0.0);
			};
			a = norm(a);
			b = norm(b);
			double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
			if (d < 0.0) { b = glm::dquat(-b.w, -b.x, -b.y, -b.z); d = -d; }
			if (d > 0.9995)
				return norm(glm::dquat(a.w + (b.w - a.w) * f, a.x + (b.x - a.x) * f,
				                       a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f));
			double theta = std::acos(std::min(1.0, std::max(-1.0, d)));
			double s = std::sin(theta);
			double wa = std::sin((1.0 - f) * theta) / s;
			double wb = std::sin(f * theta) / s;
			return glm::dquat(a.w * wa + b.w * wb, a.x * wa + b.x * wb,
			                  a.y * wa + b.y * wb, a.z * wa + b.z * wb);
		}

		static glm::dvec3 SamplePosition(const std::vector<sNodeAnimationKey<glm::vec3> >& keys, double t)
		{
			if (keys.empty()) return glm::dvec3(0.0);
			if (keys.size() == 1 || t <= double(keys.front().Time)) return glm::dvec3(keys.front().Value);
			if (t >= double(keys.back().Time)) return glm::dvec3(keys.back().Value);
			for (std::size_t i = 0; i + 1 < keys.size(); ++i)
			{
				double t0 = keys[i].Time, t1 = keys[i + 1].Time;
				if (t <= t1)
				{
					double f = (t1 - t0) > 0.0 ? (t - t0) / (t1 - t0) : 0.0;
					return glm::dvec3(keys[i].Value) +
					       (glm::dvec3(keys[i + 1].Value) - glm::dvec3(keys[i].Value)) * f;
				}
			}
			return glm::dvec3(keys.back().Value);
		}

		static glm::dquat SampleRotation(const std::vector<sNodeAnimationKey<glm::quat> >& keys, double t)
		{
			if (keys.empty()) return glm::dquat(1.0, 0.0, 0.0, 0.0);
			if (keys.size() == 1 || t <= double(keys.front().Time))
			{
				glm::quat q = keys.front().Value;
				return glm::dquat(q.w, q.x, q.y, q.z);
			}
			if (t >= double(keys.back().Time))
			{
				glm::quat q = keys.back().Value;
				return glm::dquat(q.w, q.x, q.y, q.z);
			}
			for (std::size_t i = 0; i + 1 < keys.size(); ++i)
			{
				double t0 = keys[i].Time, t1 = keys[i + 1].Time;
				if (t <= t1)
				{
					double f = (t1 - t0) > 0.0 ? (t - t0) / (t1 - t0) : 0.0;
					glm::quat qa = keys[i].Value, qb = keys[i + 1].Value;
					return Slerp(glm::dquat(qa.w, qa.x, qa.y, qa.z),
					             glm::dquat(qb.w, qb.x, qb.y, qb.z), f);
				}
			}
			glm::quat q = keys.back().Value;
			return glm::dquat(q.w, q.x, q.y, q.z);
		}

		glm::dmat4 Local(std::size_t i, double t, const std::vector<double>& theta) const
		{
			const Node& node = Nodes[i];
			glm::dmat4 local = node.FixedLocal;
			glm::dquat rotation(1.0, 0.0, 0.0, 0.0);
			if (node.HasChannel)
			{
				glm::dvec3 pos = SamplePosition(node.Position, t);
				rotation = SampleRotation(node.Rotation, t);
				local = glm::dmat4(glm::mat4_cast(rotation));
				local[3] = glm::dvec4(pos, 1.0);
			}
			if (node.Hinge >= 0 && node.Hinge < static_cast<int>(theta.size()))
				local = local * glm::dmat4(glm::mat4_cast(AxisAngle(theta[node.Hinge], node.Axis)));
			return local;
		}

		glm::dmat4 Globals(std::size_t i, double t, const std::vector<double>& theta) const
		{
			glm::dmat4 local = Local(i, t, theta);
			int parent = Nodes[i].Parent;
			return parent < 0 ? local : Globals(static_cast<std::size_t>(parent), t, theta) * local;
		}

		// Model-space point (globalInverse applied once) with theta.
		glm::dvec3 Point(std::size_t a_Bone, const glm::dvec3& a_LocalPoint, double t,
		                 const std::vector<double>& theta) const
		{
			return glm::dvec3(GlobalInverse * Globals(a_Bone, t, theta) * glm::dvec4(a_LocalPoint, 1.0));
		}

		// Central-difference Jacobian (3 x hinges) of a task point at time t.
		std::vector<std::vector<double> > FiniteDifferenceJacobian(const sIKTask& a_Task,
		                                                          const std::vector<sIKHinge>& a_Hinges,
		                                                          double a_Time,
		                                                          const std::vector<double>& a_Theta) const
		{
			const std::size_t h = a_Hinges.size();
			std::vector<std::vector<double> > jac(3, std::vector<double>(h, 0.0));
			const double eps = 1e-6;
			glm::dvec3 x(a_Task.LocalPoint);
			for (std::size_t j = 0; j < h; ++j)
			{
				std::vector<double> plus = a_Theta, minus = a_Theta;
				plus[j] += eps;
				minus[j] -= eps;
				glm::dvec3 pPlus = Point(a_Task.BoneIndex, x, a_Time, plus);
				glm::dvec3 pMinus = Point(a_Task.BoneIndex, x, a_Time, minus);
				glm::dvec3 d = (pPlus - pMinus) / (2.0 * eps);
				jac[0][j] = d.x;
				jac[1][j] = d.y;
				jac[2][j] = d.z;
			}
			return jac;
		}

		// Feedforward v_anim: exact same definition as the controller.
		glm::dvec3 Feedforward(const sIKTask& a_Task, double a_OldTime, double a_NewTime,
		                       double a_Dt, const std::vector<double>& a_Theta) const
		{
			glm::dvec3 x(a_Task.LocalPoint);
			glm::dvec3 a = Point(a_Task.BoneIndex, x, a_OldTime, a_Theta);
			glm::dvec3 b = Point(a_Task.BoneIndex, x, a_NewTime, a_Theta);
			return (b - a) / a_Dt;
		}
	};

	// ---------------------------------------------------------------------
	// Brute-force reference: enumerate every active set per layer, solve the
	// equality-constrained least squares separately, keep the feasible minimum.
	// The optimal face after a layer is carried forward as the constraint
	// (A Z) x = (A Z) x*, where Z spans the current feasible tangent space.
	// ---------------------------------------------------------------------
	namespace ref
	{
		typedef std::vector<double> Vec;
		typedef std::vector<Vec> Mat;

		inline double Dot(const Vec& a, const Vec& b)
		{
			double s = 0.0;
			for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
			return s;
		}

		// Orthonormal row basis via modified Gram-Schmidt.
		inline Mat OrthonormalRows(const Mat& a, double tol = 1e-10)
		{
			Mat basis;
			for (std::size_t r = 0; r < a.size(); ++r)
			{
				Vec v = a[r];
				for (std::size_t i = 0; i < basis.size(); ++i)
				{
					double d = Dot(v, basis[i]);
					for (std::size_t j = 0; j < v.size(); ++j) v[j] -= d * basis[i][j];
				}
				double len = std::sqrt(Dot(v, v));
				if (len > tol)
				{
					for (std::size_t j = 0; j < v.size(); ++j) v[j] /= len;
					basis.push_back(v);
				}
			}
			return basis;
		}

		// Orthonormal basis of the null space of the row set (n x k).
		inline Mat NullBasis(const Mat& a, std::size_t n)
		{
			Mat rows = OrthonormalRows(a);
			Mat basis;
			for (std::size_t i = 0; i < n && basis.size() < n - rows.size(); ++i)
			{
				Vec v(n, 0.0);
				v[i] = 1.0;
				for (std::size_t r = 0; r < rows.size(); ++r)
				{
					double d = Dot(v, rows[r]);
					for (std::size_t j = 0; j < n; ++j) v[j] -= d * rows[r][j];
				}
				double len = std::sqrt(Dot(v, v));
				if (len > 1e-10)
				{
					for (std::size_t j = 0; j < n; ++j) v[j] /= len;
					basis.push_back(v);
				}
			}
			Mat out(n, Vec(basis.size(), 0.0));
			for (std::size_t j = 0; j < basis.size(); ++j)
				for (std::size_t i = 0; i < n; ++i) out[i][j] = basis[j][i];
			return out;
		}

		// Symmetric Jacobi eigen-decomposition; G = V diag(lambda) V^T.
		inline void SymmetricEigen(Mat g, Mat& vOut, Vec& lambdaOut)
		{
			const std::size_t n = g.size();
			Mat v(n, Vec(n, 0.0));
			for (std::size_t i = 0; i < n; ++i) v[i][i] = 1.0;
			if (n == 0) { vOut = v; lambdaOut.clear(); return; }

			for (int sweep = 0; sweep < 64; ++sweep)
			{
				bool changed = false;
				for (std::size_t p = 0; p < n; ++p)
				{
					for (std::size_t q = p + 1; q < n; ++q)
					{
						if (std::fabs(g[p][q]) <= 1e-18) continue;
						changed = true;
						double theta = 0.5 * std::atan2(2.0 * g[p][q], g[q][q] - g[p][p]);
						double c = std::cos(theta), s = std::sin(theta);
						for (std::size_t k = 0; k < n; ++k)
						{
							double gpk = g[p][k], gqk = g[q][k];
							g[p][k] = c * gpk - s * gqk;
							g[q][k] = s * gpk + c * gqk;
						}
						for (std::size_t k = 0; k < n; ++k)
						{
							double gkp = g[k][p], gkq = g[k][q];
							g[k][p] = c * gkp - s * gkq;
							g[k][q] = s * gkp + c * gkq;
						}
						for (std::size_t k = 0; k < n; ++k)
						{
							double vkp = v[k][p], vkq = v[k][q];
							v[k][p] = c * vkp - s * vkq;
							v[k][q] = s * vkp + c * vkq;
						}
					}
				}
				if (!changed) break;
			}

			lambdaOut.assign(n, 0.0);
			for (std::size_t i = 0; i < n; ++i) lambdaOut[i] = g[i][i];
			vOut = v;
		}

		// Minimum-norm least squares of min ||A x - b||, with the same relative
		// singular-value cutoff (1e-9) applied on the eigen-decomposition of
		// A^T A.
		inline Vec LsMinNorm(const Mat& a, const Vec& b, std::size_t n)
		{
			Vec x(n, 0.0);
			if (n == 0 || a.empty()) return x;

			Mat g(n, Vec(n, 0.0));
			Vec h(n, 0.0);
			for (std::size_t i = 0; i < a.size(); ++i)
				for (std::size_t r = 0; r < n; ++r)
				{
					h[r] += a[i][r] * b[i];
					for (std::size_t c = 0; c < n; ++c) g[r][c] += a[i][r] * a[i][c];
				}

			Mat v;
			Vec lambda;
			SymmetricEigen(g, v, lambda);
			double lmax = 0.0;
			for (std::size_t i = 0; i < n; ++i) lmax = std::max(lmax, lambda[i]);
			if (lmax <= 0.0) return x;
			// (1e-9)^2 relative cutoff on eigenvalues.
			double cutoff = lmax * 1e-18;

			Vec vth(n, 0.0);
			for (std::size_t j = 0; j < n; ++j)
			{
				double s = 0.0;
				for (std::size_t i = 0; i < n; ++i) s += v[i][j] * h[i];
				vth[j] = s;
			}
			Vec y(n, 0.0);
			for (std::size_t j = 0; j < n; ++j)
				if (lambda[j] > cutoff) y[j] = vth[j] / lambda[j];
			for (std::size_t i = 0; i < n; ++i)
			{
				double s = 0.0;
				for (std::size_t j = 0; j < n; ++j) s += v[i][j] * y[j];
				x[i] = s;
			}
			return x;
		}

		// Solve min ||A x - b|| subject to E x = d.
		inline Vec SolveEqualityConstrained(const Mat& a, const Vec& b, const Mat& eq,
		                                    const Vec& rhs, std::size_t n)
		{
			Mat z = NullBasis(eq, n);
			const std::size_t k = z.empty() ? 0 : z[0].size();

			Vec x0(n, 0.0);
			if (!eq.empty()) x0 = LsMinNorm(eq, rhs, n);

			if (k == 0) return x0;

			Mat az(a.size(), Vec(k, 0.0));
			Vec bz(a.size(), 0.0);
			for (std::size_t r = 0; r < a.size(); ++r)
			{
				bz[r] = b[r];
				for (std::size_t c = 0; c < n; ++c) bz[r] -= a[r][c] * x0[c];
				for (std::size_t j = 0; j < k; ++j)
					for (std::size_t c = 0; c < n; ++c) az[r][j] += a[r][c] * z[c][j];
			}
			Vec y = LsMinNorm(az, bz, k);
			Vec x = x0;
			for (std::size_t i = 0; i < n; ++i)
				for (std::size_t j = 0; j < k; ++j) x[i] += z[i][j] * y[j];
			return x;
		}

		inline double Residual(const Mat& a, const Vec& b, const Vec& x)
		{
			double s = 0.0;
			for (std::size_t r = 0; r < a.size(); ++r)
			{
				double v = -b[r];
				for (std::size_t c = 0; c < x.size(); ++c) v += a[r][c] * x[c];
				s += v * v;
			}
			return s;
		}

		struct HierarchyResult
		{
			Vec              Nu;
			std::vector<double> Residual;
			bool             Ok = false;
		};

		// Strictly prioritized hierarchy. Each layer's full optimal face is
		// described by (A Z) x = (A Z) x* with Z spanning the previous face.
		inline HierarchyResult SolveHierarchy(const std::vector<Mat>& layers,
		                                      const std::vector<Vec>& rhs,
		                                      const Vec& lo, const Vec& hi)
		{
			HierarchyResult result;
			const std::size_t n = lo.size();
			Mat faceEq;
			Vec faceRhs;
			Vec x(n, 0.0);

			for (std::size_t layer = 0; layer < layers.size(); ++layer)
			{
				const Mat& a = layers[layer];
				const Vec& b = rhs[layer];

				double best = 1e300;
				Vec bestX;
				std::size_t combos = 1;
				for (std::size_t i = 0; i < n; ++i) combos *= 3;
				for (std::size_t combo = 0; combo < combos; ++combo)
				{
					std::vector<int> state(n, 0);
					std::size_t c = combo;
					for (std::size_t i = 0; i < n; ++i) { state[i] = int(c % 3); c /= 3; }

					Mat eq = faceEq;
					Vec r = faceRhs;
					for (std::size_t i = 0; i < n; ++i)
					{
						if (state[i] == 0) continue;
						Vec row(n, 0.0);
						row[i] = 1.0;
						eq.push_back(row);
						r.push_back(state[i] == 1 ? lo[i] : hi[i]);
					}

					Vec xc = SolveEqualityConstrained(a, b, eq, r, n);
					bool feasible = true;
					for (std::size_t i = 0; i < n && feasible; ++i)
					{
						if (xc[i] < lo[i] - 1e-9 || xc[i] > hi[i] + 1e-9) feasible = false;
						if (state[i] == 1 && std::fabs(xc[i] - lo[i]) > 1e-7) feasible = false;
						if (state[i] == 2 && std::fabs(xc[i] - hi[i]) > 1e-7) feasible = false;
					}
					if (!feasible) continue;
					for (std::size_t i = 0; i < n; ++i)
						xc[i] = std::min(hi[i], std::max(lo[i], xc[i]));

					double value = Residual(a, b, xc);
					double normSq = Dot(xc, xc);
					if (value < best - 1e-10 ||
					    (std::fabs(value - best) <= 1e-10 && (bestX.empty() || normSq < Dot(bestX, bestX))))
					{
						best = value;
						bestX = xc;
					}
				}
				if (bestX.empty()) return result;

				x = bestX;
				result.Residual.push_back(best);

				// Carry the optimal face forward: (A Z) x = (A Z) x*.
				Mat z = NullBasis(faceEq, n);
				const std::size_t k = z.empty() ? 0 : z[0].size();
				if (k == 0 || a.empty()) continue;
				Mat az(a.size(), Vec(k, 0.0));
				for (std::size_t r2 = 0; r2 < a.size(); ++r2)
					for (std::size_t j = 0; j < k; ++j)
						for (std::size_t cc = 0; cc < n; ++cc) az[r2][j] += a[r2][cc] * z[cc][j];
				// Add the orthonormal rows of AZ expressed back in x space.
				Mat ortho = OrthonormalRows(az);
				for (std::size_t j = 0; j < ortho.size(); ++j)
				{
					Vec row(n, 0.0);
					for (std::size_t i = 0; i < n; ++i)
						for (std::size_t t = 0; t < k; ++t) row[i] += z[i][t] * ortho[j][t];
					faceEq.push_back(row);
					faceRhs.push_back(Dot(row, x));
				}
			}

			// Minimum Euclidean norm inside the common optimal face.
			double bestNorm = 1e300;
			Vec bestX;
			std::size_t combos = 1;
			for (std::size_t i = 0; i < n; ++i) combos *= 3;
			for (std::size_t combo = 0; combo < combos; ++combo)
			{
				std::vector<int> state(n, 0);
				std::size_t c = combo;
				for (std::size_t i = 0; i < n; ++i) { state[i] = int(c % 3); c /= 3; }
				Mat eq = faceEq;
				Vec r = faceRhs;
				for (std::size_t i = 0; i < n; ++i)
				{
					if (state[i] == 0) continue;
					Vec row(n, 0.0);
					row[i] = 1.0;
					eq.push_back(row);
					r.push_back(state[i] == 1 ? lo[i] : hi[i]);
				}
				Vec xc = SolveEqualityConstrained(Mat(), Vec(), eq, r, n);
				bool feasible = true;
				for (std::size_t i = 0; i < n && feasible; ++i)
				{
					if (xc[i] < lo[i] - 1e-9 || xc[i] > hi[i] + 1e-9) feasible = false;
					if (state[i] == 1 && std::fabs(xc[i] - lo[i]) > 1e-7) feasible = false;
					if (state[i] == 2 && std::fabs(xc[i] - hi[i]) > 1e-7) feasible = false;
				}
				if (!feasible) continue;
				for (std::size_t i = 0; i < n; ++i) xc[i] = std::min(hi[i], std::max(lo[i], xc[i]));
				double normSq = Dot(xc, xc);
				if (normSq < bestNorm) { bestNorm = normSq; bestX = xc; }
			}
			if (!bestX.empty()) x = bestX;

			result.Nu = x;
			result.Ok = true;
			return result;
		}
	} // namespace ref

	struct LayerSystem
	{
		std::vector<ref::Mat> Layers;
		std::vector<ref::Vec> Rhs;
	};

	// Build the weighted layer matrices used by both the reference and the
	// controller: rows are sqrt(weight) * J, rhs sqrt(weight) * (desired -
	// v_anim). Rows are appended in the order tasks appear in a_Tasks.
	inline LayerSystem BuildLayerSystem(const std::vector<sIKTask>& a_Tasks,
	                                    const ScalarModel& a_Scalar,
	                                    const std::vector<sIKHinge>& a_Hinges,
	                                    double a_NewTime,
	                                    const std::vector<double>& a_Theta,
	                                    double a_Dt,
	                                    double a_OldTime)
	{
			std::vector<unsigned int> priorities;
			for (std::size_t i = 0; i < a_Tasks.size(); ++i)
			{
				if (std::find(priorities.begin(), priorities.end(), a_Tasks[i].Priority) == priorities.end())
					priorities.push_back(a_Tasks[i].Priority);
			}
			std::sort(priorities.begin(), priorities.end());

			LayerSystem system;
			system.Layers.assign(priorities.size(), ref::Mat());
			system.Rhs.assign(priorities.size(), ref::Vec());
			const std::size_t h = a_Hinges.size();

			for (std::size_t li = 0; li < priorities.size(); ++li)
			{
				for (std::size_t ti = 0; ti < a_Tasks.size(); ++ti)
				{
					if (a_Tasks[ti].Priority != priorities[li]) continue;
					std::vector<std::vector<double> > jac =
						a_Scalar.FiniteDifferenceJacobian(a_Tasks[ti], a_Hinges, a_NewTime, a_Theta);
					glm::dvec3 ff = a_Scalar.Feedforward(a_Tasks[ti], a_OldTime, a_NewTime, a_Dt, a_Theta);
					double sw = std::sqrt(double(a_Tasks[ti].Weight));
					for (int r = 0; r < 3; ++r)
					{
						ref::Vec row(h, 0.0);
						for (std::size_t j = 0; j < h; ++j) row[j] = sw * jac[r][j];
						system.Layers[li].push_back(row);
						double target = double(a_Tasks[ti].DesiredVelocity[r]) - ff[r];
						system.Rhs[li].push_back(sw * target);
					}
				}
			}
			return system;
		}

	inline std::vector<double> ComputeBoundsLo(const std::vector<sIKHinge>& a_Hinges,
	                                           const std::vector<double>& a_Theta, double a_Dt)
	{
		std::vector<double> lo(a_Hinges.size(), 0.0);
		for (std::size_t j = 0; j < a_Hinges.size(); ++j)
		{
			double maxSpeed = double(a_Hinges[j].MaxSpeed);
			double l = std::max(-maxSpeed, (double(a_Hinges[j].MinAngle) - a_Theta[j]) / a_Dt);
			double u = std::min(maxSpeed, (double(a_Hinges[j].MaxAngle) - a_Theta[j]) / a_Dt);
			lo[j] = l > u ? 0.0 : l;
		}
		return lo;
	}

	inline std::vector<double> ComputeBoundsHi(const std::vector<sIKHinge>& a_Hinges,
	                                           const std::vector<double>& a_Theta, double a_Dt)
	{
		std::vector<double> hi(a_Hinges.size(), 0.0);
		for (std::size_t j = 0; j < a_Hinges.size(); ++j)
		{
			double maxSpeed = double(a_Hinges[j].MaxSpeed);
			double l = std::max(-maxSpeed, (double(a_Hinges[j].MinAngle) - a_Theta[j]) / a_Dt);
			double u = std::min(maxSpeed, (double(a_Hinges[j].MaxAngle) - a_Theta[j]) / a_Dt);
			hi[j] = l > u ? 0.0 : u;
		}
		return hi;
	}
} // namespace dik_test

#endif
