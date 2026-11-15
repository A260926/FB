/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#include "beam_volume.h"

#include "utility.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace Cubiquity
{
	using namespace Internals;

	namespace
	{
		constexpr u32 NoMaterial = 0xffffffffu;
		constexpr u32 RootHeight = 32;
		constexpr i64 RootLower = -(i64(1) << 31);

		// A node of the persistent column-transfer representation. A node always describes a
		// square region of the transverse plane. Transmittance is stored as the absorbed
		// fraction 'd = 1 - T' because this keeps tiny extinctions accurate when summing.
		struct TransferNode
		{
			bool uniform = true;

			// Aggregate over the valid columns of the region.
			double count = 0.0;
			vec3d  sumC{ 0.0, 0.0, 0.0 };
			double sumD = 0.0;

			// Valid when 'uniform' is true: a constant transfer over every valid column.
			vec3d  uniformC{ 0.0, 0.0, 0.0 };
			double uniformD = 0.0;

			// The four transverse quadrants, low first. Ordering matches the octree child
			// bit ordering for the transverse axes, so compositions stay aligned.
			std::array<std::shared_ptr<const TransferNode>, 4> child{};
		};

		bool isIdentity(const TransferNode& n)
		{
			return n.uniform && n.uniformD == 0.0 &&
				n.uniformC.x == 0.0 && n.uniformC.y == 0.0 && n.uniformC.z == 0.0;
		}

		double canonical(double v)
		{
			return v == 0.0 ? 0.0 : v;
		}

		vec3d canonical(const vec3d& v)
		{
			return vec3d(canonical(v.x), canonical(v.y), canonical(v.z));
		}

		struct TransferKey
		{
			u32 node = 0;
			u32 height = 0;
			u32 axis = 0;
			u32 reverse = 0;

			bool operator==(const TransferKey& rhs) const
			{
				return node == rhs.node && height == rhs.height &&
					axis == rhs.axis && reverse == rhs.reverse;
			}
		};

		struct TransferKeyHasher
		{
			std::size_t operator()(const TransferKey& key) const noexcept
			{
				u64 hash = hash_value(key.node);
				hash = hash_combine(hash, hash_value(key.height));
				hash = hash_combine(hash, hash_value(key.axis));
				hash = hash_combine(hash, hash_value(key.reverse));
				return static_cast<std::size_t>(hash);
			}
		};

		struct UniformIdKey
		{
			u32 node = 0;
			u32 height = 0;

			bool operator==(const UniformIdKey& rhs) const
			{
				return node == rhs.node && height == rhs.height;
			}
		};

		struct UniformIdKeyHasher
		{
			std::size_t operator()(const UniformIdKey& key) const noexcept
			{
				u64 hash = hash_value(key.node);
				hash = hash_combine(hash, hash_value(key.height));
				return static_cast<std::size_t>(hash);
			}
		};

		u64 doubleBits(double v)
		{
			return std::bit_cast<u64>(canonical(v));
		}

		struct UniformPoolKey
		{
			u64 count = 0;
			u64 cx = 0, cy = 0, cz = 0;
			u64 d = 0;

			bool operator==(const UniformPoolKey& rhs) const
			{
				return count == rhs.count && cx == rhs.cx && cy == rhs.cy &&
					cz == rhs.cz && d == rhs.d;
			}
		};

		struct UniformPoolHasher
		{
			std::size_t operator()(const UniformPoolKey& key) const noexcept
			{
				u64 hash = bit_mix(key.count);
				hash = hash_combine(hash, bit_mix(key.cx));
				hash = hash_combine(hash, bit_mix(key.cy));
				hash = hash_combine(hash, bit_mix(key.cz));
				hash = hash_combine(hash, bit_mix(key.d));
				return static_cast<std::size_t>(hash);
			}
		};

		struct InternalPoolKey
		{
			std::array<const TransferNode*, 4> child{};

			bool operator==(const InternalPoolKey& rhs) const { return child == rhs.child; }
		};

		struct InternalPoolHasher
		{
			std::size_t operator()(const InternalPoolKey& key) const noexcept
			{
				u64 hash = hash_value(reinterpret_cast<std::uintptr_t>(key.child[0]));
				hash = hash_combine(hash, hash_value(reinterpret_cast<std::uintptr_t>(key.child[1])));
				hash = hash_combine(hash, hash_value(reinterpret_cast<std::uintptr_t>(key.child[2])));
				hash = hash_combine(hash, hash_value(reinterpret_cast<std::uintptr_t>(key.child[3])));
				return static_cast<std::size_t>(hash);
			}
		};

		bool validPalette(const std::array<OpticalMaterial, MaterialCount>& palette)
		{
			const OpticalMaterial& zero = palette[0];
			if (zero.extinction != 0.0 ||
				zero.emission.x != 0.0 || zero.emission.y != 0.0 || zero.emission.z != 0.0)
			{
				return false;
			}

			auto inRange = [](double v) {
				return std::isfinite(v) && v >= 0.0 && v <= 1.0e6;
			};

			for (u32 i = 0; i < MaterialCount; ++i)
			{
				const OpticalMaterial& m = palette[i];
				if (!inRange(m.extinction)) return false;
				if (!inRange(m.emission.x) || !inRange(m.emission.y) || !inRange(m.emission.z)) return false;
			}
			return true;
		}

		bool samePalette(const std::array<OpticalMaterial, MaterialCount>& a,
			const std::array<OpticalMaterial, MaterialCount>& b)
		{
			for (u32 i = 0; i < MaterialCount; ++i)
			{
				if (a[i].extinction != b[i].extinction) return false;
				if (a[i].emission.x != b[i].emission.x ||
					a[i].emission.y != b[i].emission.y ||
					a[i].emission.z != b[i].emission.z) return false;
			}
			return true;
		}

		void transverseAxes(int axis, int& t0, int& t1)
		{
			switch (axis)
			{
			case 0: t0 = 1; t1 = 2; break;
			case 1: t0 = 0; t1 = 2; break;
			default: t0 = 0; t1 = 1; break;
			}
		}
	}

	struct BeamVolume::Impl
	{
		const Volume* volume = nullptr;
		std::array<OpticalMaterial, MaterialCount> palette{};

		std::unordered_map<UniformIdKey, u32, UniformIdKeyHasher> uniformIds;
		std::unordered_map<TransferKey, std::shared_ptr<const TransferNode>, TransferKeyHasher> cache;

		std::unordered_map<UniformPoolKey, std::shared_ptr<const TransferNode>, UniformPoolHasher> uniformPool;
		std::unordered_map<InternalPoolKey, std::shared_ptr<const TransferNode>, InternalPoolHasher> internalPool;

		BeamPrepareStats stats{};

		Impl(const Volume* v, const std::array<OpticalMaterial, MaterialCount>& p)
			: volume(v), palette(p) {}

		const NodeStore& nodes() const { return getNodes(*volume); }
		u32 root() const { return getRootNodeIndex(*volume); }

		// A single homogeneous unit-length slab. Returns the absorbed fraction and the
		// premultiplied emitted radiance. Uses expm1 to retain accuracy at small extinction.
		vec3d segment(u32 material, double length, double& outD) const
		{
			const OpticalMaterial& m = palette[material];
			if (m.extinction == 0.0)
			{
				outD = 0.0;
				return m.emission * length;
			}

			const double x = m.extinction * length;
			if (!(x > 0.0))
			{
				// Underflow of a denormal extinction: fall back to the exact zero-extinction form.
				outD = 0.0;
				return m.emission * length;
			}

			if (x < 1e-4)
			{
				// Series form avoids dividing a denormal absorbed fraction by a tiny
				// extinction; 1 - T = x - x^2/2 + x^3/6, so (1 - T)/extinction is
				// length * (1 - x/2 + x^2/6).
				outD = x * (1.0 - 0.5 * x);
				return m.emission * (length * (1.0 - 0.5 * x + (x * x) / 6.0));
			}

			outD = -std::expm1(-x);
			return m.emission * (outD / m.extinction);
		}

		std::shared_ptr<const TransferNode> makeUniform(const vec3d& c, double d, double count,
			bool persistent)
		{
			const vec3d cc = canonical(c);
			const double cd = canonical(d);
			const double ccount = canonical(count);

			if (persistent)
			{
				UniformPoolKey key;
				key.count = doubleBits(ccount);
				key.cx = doubleBits(cc.x);
				key.cy = doubleBits(cc.y);
				key.cz = doubleBits(cc.z);
				key.d = doubleBits(cd);

				auto it = uniformPool.find(key);
				if (it != uniformPool.end()) return it->second;

				auto node = std::make_shared<TransferNode>();
				node->uniform = true;
				node->count = ccount;
				node->uniformC = cc;
				node->uniformD = cd;
				node->sumC = cc * ccount;
				node->sumD = cd * ccount;
				uniformPool.emplace(key, node);
				++stats.transferNodes;
				return node;
			}

			auto node = std::make_shared<TransferNode>();
			node->uniform = true;
			node->count = ccount;
			node->uniformC = cc;
			node->uniformD = cd;
			node->sumC = cc * ccount;
			node->sumD = cd * ccount;
			return node;
		}

		std::shared_ptr<const TransferNode> makeEmpty(bool persistent)
		{
			return makeUniform(vec3d(0.0, 0.0, 0.0), 0.0, 0.0, persistent);
		}

		std::shared_ptr<const TransferNode> makeInternal(
			const std::array<std::shared_ptr<const TransferNode>, 4>& children,
			double /*regionCount*/, bool persistent)
		{
			bool allUniform = true;
			for (const auto& child : children)
			{
				if (!child || !child->uniform) { allUniform = false; break; }
			}

			if (allUniform)
			{
				bool same = true;
				for (int i = 1; i < 4; ++i)
				{
					if (children[i]->uniformC.x != children[0]->uniformC.x ||
						children[i]->uniformC.y != children[0]->uniformC.y ||
						children[i]->uniformC.z != children[0]->uniformC.z ||
						children[i]->uniformD != children[0]->uniformD)
					{
						same = false;
						break;
					}
				}

				if (same)
				{
					double count = 0.0;
					for (const auto& child : children) { count += child->count; }
					return makeUniform(children[0]->uniformC, children[0]->uniformD, count, persistent);
				}
			}

			if (persistent)
			{
				InternalPoolKey key;
				for (int i = 0; i < 4; ++i) { key.child[i] = children[i].get(); }

				auto it = internalPool.find(key);
				if (it != internalPool.end()) return it->second;

				auto node = std::make_shared<TransferNode>();
				node->uniform = false;
				node->child = children;
				for (const auto& child : children)
				{
					node->count += child->count;
					node->sumC += child->sumC;
					node->sumD += child->sumD;
				}
				internalPool.emplace(key, node);
				++stats.transferNodes;
				return node;
			}

			auto node = std::make_shared<TransferNode>();
			node->uniform = false;
			node->child = children;
			for (const auto& child : children)
			{
				node->count += child->count;
				node->sumC += child->sumC;
				node->sumD += child->sumD;
			}
			return node;
		}

		// Compose A (nearer) then B (farther) for every column of a region. 'regionCount'
		// is the number of valid columns in the region; both maps share the same valid mask.
		std::shared_ptr<const TransferNode> compose(
			const std::shared_ptr<const TransferNode>& a,
			const std::shared_ptr<const TransferNode>& b,
			double regionCount, bool persistent)
		{
			if (regionCount <= 0.0) return makeEmpty(persistent);

			if (a->uniform && isIdentity(*a)) return b;
			if (b->uniform && isIdentity(*b)) return a;

			if (a->uniform && b->uniform)
			{
				const double d = a->uniformD + b->uniformD - a->uniformD * b->uniformD;
				const vec3d c = a->uniformC + b->uniformC * (1.0 - a->uniformD);
				return makeUniform(c, d, regionCount, persistent);
			}

			std::array<std::shared_ptr<const TransferNode>, 4> children;

			if (a->uniform)
			{
				for (int i = 0; i < 4; ++i)
				{
					children[i] = compose(a, b->child[i], b->child[i]->count, persistent);
				}
			}
			else if (b->uniform)
			{
				for (int i = 0; i < 4; ++i)
				{
					children[i] = compose(a->child[i], b, a->child[i]->count, persistent);
				}
			}
			else
			{
				for (int i = 0; i < 4; ++i)
				{
					children[i] = compose(a->child[i], b->child[i], a->child[i]->count, persistent);
				}
			}

			return makeInternal(children, regionCount, persistent);
		}

		// Returns the material id if every leaf in this node is the same material.
		// 'memo' caches results; preparation uses the persistent map whereas queries use a
		// private map so that they never modify the persistent representation.
		//
		// The octree is uniform in space but not necessarily in structure: a volume filled
		// through repeated setVoxel() calls can contain many distinct nodes which all contain
		// the same material. If every child of every node is a uniform material with the same
		// id then the whole node is that material, including when the node happens to be an
		// internal node at leaf height. This keeps the prepared representation independent of
		// whatever hierarchy the source DAG happens to use.
		template <typename Memo>
		u32 uniformIdImpl(u32 node, int height, Memo& memo)
		{
			if (isMaterialNode(node)) return node;

			UniformIdKey key{ node, static_cast<u32>(height) };
			auto it = memo.find(key);
			if (it != memo.end()) return it->second;

			const NodeStore& store = nodes();
			u32 first = NoMaterial;
			for (u32 childId = 0; childId < 8; ++childId)
			{
				const u32 childNode = store[node][childId];

				// A repeated child which has already been classified can be reused without
				// descending through it again.
				u32 id = NoMaterial;
				if (height > 0)
				{
					id = uniformIdImpl(childNode, height - 1, memo);
				}
				else if (isMaterialNode(childNode))
				{
					id = childNode;
				}

				if (id == NoMaterial) { memo.emplace(key, NoMaterial); return NoMaterial; }
				if (childId == 0) first = id;
				else if (id != first) { memo.emplace(key, NoMaterial); return NoMaterial; }
			}

			memo.emplace(key, first);
			return first;
		}

		u32 uniformId(u32 node, int height)
		{
			return uniformIdImpl(node, height, uniformIds);
		}

		// Read-only lookup of a preparation-time result, if it exists.
		bool lookupUniformId(u32 node, int height, u32& result) const
		{
			if (isMaterialNode(node)) { result = node; return true; }

			auto it = uniformIds.find(UniformIdKey{ node, static_cast<u32>(height) });
			if (it == uniformIds.end()) return false;
			result = it->second;
			return true;
		}

		std::shared_ptr<const TransferNode> materialMap(u32 material, int height, bool persistent)
		{
			const double length = std::ldexp(1.0, height);
			const double count = std::ldexp(1.0, 2 * height);
			double d = 0.0;
			const vec3d c = segment(material, length, d);
			return makeUniform(c, d, count, persistent);
		}

		u32 childIndex(int axis, int t0, int t1, int t0Bit, int t1Bit, int depthBit) const
		{
			int bits[3] = { 0, 0, 0 };
			bits[t0] = t0Bit;
			bits[t1] = t1Bit;
			bits[axis] = depthBit;
			return static_cast<u32>(bits[0] | (bits[1] << 1) | (bits[2] << 2));
		}

		std::shared_ptr<const TransferNode> prepareRec(u32 node, int height, int axis, bool reverse)
		{
			TransferKey key{ node, static_cast<u32>(height), static_cast<u32>(axis), static_cast<u32>(reverse ? 1 : 0) };
			auto cached = cache.find(key);
			if (cached != cache.end()) return cached->second;

			++stats.sourceStates;

			std::shared_ptr<const TransferNode> result;
			const u32 material = uniformId(node, height);

			if (material != NoMaterial)
			{
				result = materialMap(material, height, true);
			}
			else
			{
				if (height <= 0)
				{
					throw std::logic_error("BeamVolume: invalid DAG (internal node at height zero)");
				}

				int t0 = 0, t1 = 0;
				transverseAxes(axis, t0, t1);

				const NodeStore& store = nodes();
				std::array<std::shared_ptr<const TransferNode>, 4> quadrants;

				for (int q = 0; q < 4; ++q)
				{
					const int t0Bit = q & 1;
					const int t1Bit = (q >> 1) & 1;

					const u32 nearId = childIndex(axis, t0, t1, t0Bit, t1Bit, reverse ? 1 : 0);
					const u32 farId = childIndex(axis, t0, t1, t0Bit, t1Bit, reverse ? 0 : 1);

					auto nearMap = prepareRec(store[node][nearId], height - 1, axis, reverse);
					auto farMap = prepareRec(store[node][farId], height - 1, axis, reverse);
					quadrants[q] = compose(nearMap, farMap, nearMap->count, true);
				}

				result = makeInternal(quadrants, std::ldexp(1.0, 2 * height), true);
			}

			cache.emplace(key, result);
			return result;
		}

		void build()
		{
			stats = BeamPrepareStats{};
			uniformIds.clear();
			cache.clear();
			uniformPool.clear();
			internalPool.clear();

			const u32 rootNode = root();
			uniformId(rootNode, RootHeight);

			for (int axis = 0; axis < 3; ++axis)
			{
				for (int reverse = 0; reverse < 2; ++reverse)
				{
					prepareRec(rootNode, RootHeight, axis, reverse != 0);
				}
			}
		}
	};

	BeamVolume::BeamVolume(const Volume& volume,
		const std::array<OpticalMaterial, Internals::MaterialCount>& palette)
		: mVolume(&volume)
		, mPalette(palette)
	{
		if (!validPalette(palette))
		{
			throw std::invalid_argument("BeamVolume: invalid optical palette");
		}

		mRep = std::make_shared<Impl>(&volume, palette);
		mRep->build();
	}

	bool BeamVolume::setPalette(const std::array<OpticalMaterial, Internals::MaterialCount>& palette)
	{
		if (!validPalette(palette)) return false;
		if (samePalette(palette, mPalette)) return true;

		auto rep = std::make_shared<Impl>(mVolume, palette);
		rep->build();

		mPalette = palette;
		mRep = std::move(rep);
		return true;
	}

	BeamPrepareStats BeamVolume::prepareStats() const
	{
		if (mRep == nullptr) { return BeamPrepareStats{}; }
		return mRep->stats;
	}

	BeamResult BeamVolume::query(int axis, bool reverse,
		const std::array<i64, 2>& lo, const std::array<i64, 2>& hi,
		i64 depthLo, i64 depthHi, BeamStats* stats) const
	{
		if (stats) { *stats = BeamStats{}; }
		if (mRep == nullptr)
		{
			throw std::invalid_argument("BeamVolume: no prepared representation");
		}

		if (axis < 0 || axis > 2)
		{
			throw std::invalid_argument("BeamVolume: axis must be 0, 1 or 2");
		}

		auto inRange = [](i64 value) {
			return value >= -(i64(1) << 31) && value <= (i64(1) << 31);
		};

		if (!inRange(lo[0]) || !inRange(hi[0]) || !inRange(lo[1]) || !inRange(hi[1]) ||
			!inRange(depthLo) || !inRange(depthHi))
		{
			throw std::invalid_argument("BeamVolume: endpoints outside [-2^31, 2^31]");
		}

		if (lo[0] > hi[0] || lo[1] > hi[1] || depthLo > depthHi)
		{
			throw std::invalid_argument("BeamVolume: invalid half-open range");
		}

		if (lo[0] == hi[0] || lo[1] == hi[1] || depthLo == depthHi)
		{
			return BeamResult{};
		}

		int t0 = 0, t1 = 0;
		transverseAxes(axis, t0, t1);

		struct Query
		{
			BeamVolume::Impl* impl = nullptr;
			const NodeStore* storePtr = nullptr;
			int axis = 0, t0 = 0, t1 = 0;
			bool reverse = false;
			i64 lo[2] = { 0, 0 };
			i64 hi[2] = { 0, 0 };
			i64 depthLo = 0;
			i64 depthHi = 0;
			BeamStats* stats = nullptr;
			BeamStats localStats{};
			std::unordered_map<UniformIdKey, u32, UniformIdKeyHasher> queryUniformIds;

			std::shared_ptr<const TransferNode> eval(u32 node, int height, const i64 lower[3])
			{
				const i64 size = i64(1) << height;
				const i64 t0Lo = std::max(lower[t0], lo[0]);
				const i64 t0Hi = std::min(lower[t0] + size, hi[0]);
				const i64 t1Lo = std::max(lower[t1], lo[1]);
				const i64 t1Hi = std::min(lower[t1] + size, hi[1]);
				if (t0Lo >= t0Hi || t1Lo >= t1Hi)
				{
					return impl->makeEmpty(false);
				}

				++stats->sourceVisits;

				const double area = static_cast<double>(t0Hi - t0Lo) * static_cast<double>(t1Hi - t1Lo);

				const i64 dLo = std::max(lower[axis], depthLo);
				const i64 dHi = std::min(lower[axis] + size, depthHi);
				if (dLo >= dHi)
				{
					// The column exists but this node contributes nothing in the window.
					return impl->makeUniform(vec3d(0.0, 0.0, 0.0), 0.0, area, false);
				}

				u32 material = NoMaterial;
				if (!impl->lookupUniformId(node, height, material))
				{
					// Never extend the persistent representation from a query: use a local memo.
					material = impl->uniformIdImpl(node, height, queryUniformIds);
				}


				const bool fullyInside =
					lower[t0] >= lo[0] && lower[t0] + size <= hi[0] &&
					lower[t1] >= lo[1] && lower[t1] + size <= hi[1] &&
					lower[axis] >= depthLo && lower[axis] + size <= depthHi;

				if (fullyInside)
				{
					TransferKey key{ node, static_cast<u32>(height),
						static_cast<u32>(axis), static_cast<u32>(reverse ? 1 : 0) };
					auto it = impl->cache.find(key);
					if (it == impl->cache.end())
					{
						throw std::logic_error("BeamVolume: missing prepared transfer node");
					}
					++stats->transferVisits;
					return it->second;
				}

				if (material != NoMaterial)
				{
					double d = 0.0;
					const vec3d c = impl->segment(material, static_cast<double>(dHi - dLo), d);
					return impl->makeUniform(c, d, area, false);
				}

				if (height <= 0)
				{
					return impl->makeEmpty(false);
				}

				const i64 childSize = size / 2;
				std::array<std::shared_ptr<const TransferNode>, 4> quadrants;

				for (int q = 0; q < 4; ++q)
				{
					const int t0Bit = q & 1;
					const int t1Bit = (q >> 1) & 1;

					i64 lowerNear[3] = { lower[0], lower[1], lower[2] };
					i64 lowerFar[3] = { lower[0], lower[1], lower[2] };

					lowerNear[t0] += t0Bit ? childSize : 0;
					lowerNear[t1] += t1Bit ? childSize : 0;
					lowerFar[t0] += t0Bit ? childSize : 0;
					lowerFar[t1] += t1Bit ? childSize : 0;

					const int nearBit = reverse ? 1 : 0;
					const int farBit = reverse ? 0 : 1;
					lowerNear[axis] += nearBit ? childSize : 0;
					lowerFar[axis] += farBit ? childSize : 0;

					const u32 nearId = impl->childIndex(axis, t0, t1, t0Bit, t1Bit, nearBit);
					const u32 farId = impl->childIndex(axis, t0, t1, t0Bit, t1Bit, farBit);

					auto nearMap = eval((*storePtr)[node][nearId], height - 1, lowerNear);
					auto farMap = eval((*storePtr)[node][farId], height - 1, lowerFar);
					quadrants[q] = impl->compose(nearMap, farMap, nearMap->count, false);
				}

				return impl->makeInternal(quadrants, area, false);
			}
		};

		Query q;
		q.impl = mRep.get();
		q.storePtr = &getNodes(*mVolume);
		q.axis = axis;
		q.t0 = t0;
		q.t1 = t1;
		q.reverse = reverse;
		q.lo[0] = lo[0]; q.lo[1] = lo[1];
		q.hi[0] = hi[0]; q.hi[1] = hi[1];
		q.depthLo = depthLo;
		q.depthHi = depthHi;
		q.stats = stats;
		if (q.stats == nullptr)
		{
			q.localStats = BeamStats{};
			q.stats = &q.localStats;
		}

		const i64 lower[3] = { RootLower, RootLower, RootLower };
		auto map = q.eval(getRootNodeIndex(*mVolume), RootHeight, lower);

		BeamResult result;
		if (!map || map->count <= 0.0)
		{
			return result;
		}

		const vec3d radiance = map->sumC / map->count;
		double d = map->sumD / map->count;
		d = std::clamp(d, 0.0, 1.0);

		result.radiance = vec3d(std::max(0.0, radiance.x), std::max(0.0, radiance.y), std::max(0.0, radiance.z));
		result.transmittance = 1.0 - d;
		return result;
	}
}
