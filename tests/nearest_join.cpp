/******************************************************************************
 * Project:  libspatialindex - A C++ library for spatial indexing
 * Purpose:  Tests for RTree::nearestNeighborJoin (batch k-NN join).
 *
 * The expected answer of every left record is computed here from scratch by
 * evaluating the pairwise minimum Euclidean distance between independent
 * rectangles; neither the join traversal under test nor the legacy
 * nearestNeighborQuery() entry point is used to produce expected sets. The
 * legacy single-shape query appears only in the pruning group to count the
 * reads of the application-level per-record alternative.
 *
 * All data is generated in this file, only the C++ standard library and
 * libspatialindex are used, and the entry point is:
 *
 *     int join_case(const char *group);
 *
 * with group in {"results", "storage", "pruning"}.
 ******************************************************************************/

#include <spatialindex/SpatialIndex.h>

#include "cases.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SpatialIndex;

namespace
{
	struct Box
	{
		double lo[2];
		double hi[2];
	};

	struct Rec
	{
		id_type id;
		Box box;
	};

	// One captured callback. Everything is copied while the IData references
	// are still valid; nothing from the callback is retained as a pointer.
	struct PairRec
	{
		id_type leftId = -1;
		id_type rightId = -1;
		double distance = 0.0;
		uint8_t leftTag = 0;
		uint8_t rightTag = 0;
		Box leftBox = {{0.0, 0.0}, {0.0, 0.0}};
		Box rightBox = {{0.0, 0.0}, {0.0, 0.0}};
	};

	uint8_t payloadTag(id_type id)
	{
		// Deliberately distinct from the id so that payload preservation is
		// actually exercised. Zero is avoided so an uninitialized byte would
		// stand out.
		return static_cast<uint8_t>((static_cast<uint64_t>(id) * 31u + 7u) & 0xFFu);
	}

	// Independent reference implementation of the minimum Euclidean distance
	// between two axis-aligned rectangles. Overlapping or touching
	// projections contribute zero per axis, hence intersecting/contacting
	// rectangles have distance zero. Written independently of the library
	// traversal and of Region::getMinimumDistance().
	double referenceDistance(const Box& a, const Box& b)
	{
		double squared = 0.0;

		for (int cDim = 0; cDim < 2; ++cDim)
		{
			double gap = 0.0;
			const double alo = a.lo[cDim];
			const double ahi = a.hi[cDim];
			const double blo = b.lo[cDim];
			const double bhi = b.hi[cDim];

			if (bhi < alo) gap = alo - bhi;
			else if (ahi < blo) gap = blo - ahi;

			squared += gap * gap;
		}

		return std::sqrt(squared);
	}

	bool nearlyEqual(double a, double b)
	{
		const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
		return std::fabs(a - b) <= 1e-12 * scale;
	}

	Region toRegion(const Box& b)
	{
		return Region(b.lo, b.hi, 2);
	}

	ISpatialIndex* buildTree(
		IStorageManager& sm, const std::vector<Rec>& recs, uint32_t capacity,
		id_type& indexId)
	{
		ISpatialIndex* tree = RTree::createNewRTree(
			sm, 0.7, capacity, capacity, 2, RTree::RV_RSTAR, indexId);

		for (size_t i = 0; i < recs.size(); ++i)
		{
			const Rec& r = recs[i];
			Region region = toRegion(r.box);
			uint8_t tag = payloadTag(r.id);
			tree->insertData(1, &tag, region, r.id);
		}

		return tree;
	}

	const Rec* findRec(const std::vector<Rec>& recs, id_type id)
	{
		for (size_t i = 0; i < recs.size(); ++i)
			if (recs[i].id == id) return &recs[i];
		return nullptr;
	}

	// Expected right identifiers for one left record: all right records up to
	// and including every tie at the k-th distance.
	std::vector<id_type> expectedRights(
		const Rec& left, const std::vector<Rec>& rights, uint32_t k)
	{
		struct Scored
		{
			double distance;
			id_type id;
		};

		std::vector<Scored> all;
		all.reserve(rights.size());

		for (size_t i = 0; i < rights.size(); ++i)
		{
			Scored s;
			s.distance = referenceDistance(left.box, rights[i].box);
			s.id = rights[i].id;
			all.push_back(s);
		}

		std::sort(all.begin(), all.end(), [](const Scored& a, const Scored& b) {
			if (a.distance != b.distance) return a.distance < b.distance;
			return a.id < b.id;
		});

		std::vector<id_type> out;
		if (all.empty()) return out;

		const size_t cut = std::min(static_cast<size_t>(k), all.size());
		const double threshold = all[cut - 1].distance;

		for (size_t i = 0; i < all.size(); ++i)
		{
			if (all[i].distance > threshold) break;
			out.push_back(all[i].id);
		}
		return out;
	}

	// Collecting join visitor: copies identifiers, distances, payloads and
	// MBRs during each callback, while the IData objects are alive.
	class CollectingVisitor : public RTree::IJoinVisitor
	{
	public:
		void visitPair(const IData& left, const IData& right, double distance) override
		{
			PairRec p;
			p.leftId = left.getIdentifier();
			p.rightId = right.getIdentifier();
			p.distance = distance;
			p.leftTag = copyTag(left);
			p.rightTag = copyTag(right);
			p.leftBox = copyBox(left);
			p.rightBox = copyBox(right);
			m_pairs.push_back(p);
		}

		static uint8_t copyTag(const IData& d)
		{
			uint32_t len = 0;
			uint8_t* bytes = nullptr;
			d.getData(len, &bytes);
			uint8_t tag = 0;
			if (len == 1 && bytes != nullptr) tag = bytes[0];
			delete[] bytes;
			return tag;
		}

		static Box copyBox(const IData& d)
		{
			IShape* shape = nullptr;
			d.getShape(&shape);
			Region* region = dynamic_cast<Region*>(shape);
			Box b;
			b.lo[0] = region->m_pLow[0];
			b.lo[1] = region->m_pLow[1];
			b.hi[0] = region->m_pHigh[0];
			b.hi[1] = region->m_pHigh[1];
			delete shape;
			return b;
		}

		std::vector<PairRec> m_pairs;
	};

	bool sameBox(const Box& a, const Box& b)
	{
		return a.lo[0] == b.lo[0] && a.lo[1] == b.lo[1] &&
			a.hi[0] == b.hi[0] && a.hi[1] == b.hi[1];
	}

	// Full content check of one join run against the independent reference.
	// The join runs carry copied payloads and MBRs (checkPayload == true);
	// the read-count baseline only carries identifiers and distances.
	int checkAgainstReference(
		const std::vector<Rec>& lefts, const std::vector<Rec>& rights,
		uint32_t k, const std::vector<PairRec>& got, bool checkPayload = true)
	{
		std::set<std::pair<id_type, id_type> > seen;

		for (size_t i = 0; i < got.size(); ++i)
		{
			const PairRec& p = got[i];

			// Every ordered pair is reported exactly once.
			std::pair<std::set<std::pair<id_type, id_type> >::iterator, bool> ins =
				seen.insert(std::make_pair(p.leftId, p.rightId));
			CHECK_CHECK(ins.second);

			const Rec* l = findRec(lefts, p.leftId);
			const Rec* r = findRec(rights, p.rightId);
			CHECK_CHECK(l != nullptr && r != nullptr);

			// The callback distance matches the independent rectangle
			// distance, and for join runs the payloads round-trip and the
			// exposed MBR is the one stored for the record.
			CHECK_CHECK(nearlyEqual(p.distance, referenceDistance(l->box, r->box)));
			if (checkPayload)
			{
				CHECK_CHECK(p.leftTag == payloadTag(l->id));
				CHECK_CHECK(p.rightTag == payloadTag(r->id));
				CHECK_CHECK(sameBox(p.leftBox, l->box));
				CHECK_CHECK(sameBox(p.rightBox, r->box));
			}
		}

		size_t expectedTotal = 0;

		for (size_t i = 0; i < lefts.size(); ++i)
		{
			std::vector<id_type> want = expectedRights(lefts[i], rights, k);
			expectedTotal += want.size();

			std::set<id_type> gotForLeft;
			for (size_t j = 0; j < got.size(); ++j)
				if (got[j].leftId == lefts[i].id) gotForLeft.insert(got[j].rightId);

			std::set<id_type> wantSet(want.begin(), want.end());
			CHECK_CHECK(gotForLeft == wantSet);
		}

		CHECK_CHECK(got.size() == expectedTotal);
		return 0;
	}

	// Hand-built dataset exercising containment, contact, zero-area boxes,
	// repeated distances, cross-table identifier reuse and a left record
	// whose nearest neighbors are all far away (independent per-record bound).
	void buildMixedDataset(std::vector<Rec>& lefts, std::vector<Rec>& rights)
	{
		lefts.clear();
		rights.clear();

		lefts.push_back({1, {{0.0, 0.0}, {2.0, 2.0}}});
		lefts.push_back({2, {{10.0, 10.0}, {10.0, 10.0}}}); // zero-area point
		lefts.push_back({3, {{20.0, 20.0}, {22.0, 22.0}}});
		lefts.push_back({4, {{-5.0, -5.0}, {-4.0, -4.0}}});
		lefts.push_back({5, {{100.0, 100.0}, {101.0, 101.0}}}); // isolated far record

		rights.push_back({10, {{0.0, 0.0}, {2.0, 2.0}}});    // equal MBR, distance 0
		rights.push_back({11, {{2.0, 1.0}, {4.0, 2.0}}});    // edge contact, distance 0
		rights.push_back({12, {{1.0, 1.0}, {1.5, 1.5}}});    // contained in left 1
		rights.push_back({13, {{10.0, 10.0}, {10.0, 10.0}}}); // zero-area point on left 2
		rights.push_back({14, {{12.0, 10.0}, {12.0, 10.0}}}); // distance 2 from left 2
		rights.push_back({15, {{8.0, 10.0}, {8.0, 10.0}}});  // distance 2 (tie)
		rights.push_back({16, {{10.0, 12.0}, {10.0, 12.0}}}); // distance 2 (tie)
		rights.push_back({5, {{20.5, 20.5}, {21.0, 21.0}}}); // same id as left 5, contained in left 3
		rights.push_back({17, {{30.0, 30.0}, {31.0, 31.0}}});
		rights.push_back({18, {{-100.0, -100.0}, {-99.0, -99.0}}});
	}

	// Deterministic grid dataset used for height and pruning cases.
	void buildGridDataset(
		std::vector<Rec>& lefts, std::vector<Rec>& rights,
		uint32_t leftSide, uint32_t rightNear, uint32_t rightFar)
	{
		lefts.clear();
		rights.clear();

		id_type nextId = 1;

		for (uint32_t x = 0; x < leftSide; ++x)
		{
			for (uint32_t y = 0; y < leftSide; ++y)
			{
				const double c = static_cast<double>(x);
				const double d = static_cast<double>(y);
				Rec r;
				r.id = nextId++;
				r.box = {{c, d}, {c, d}}; // dense zero-area points
				lefts.push_back(r);
			}
		}

		// A few close reference regions interleaved with the dense left grid.
		for (uint32_t i = 0; i < rightNear; ++i)
		{
			const double c = static_cast<double>(i % leftSide);
			const double d = static_cast<double>(i / leftSide);
			Rec r;
			r.id = nextId++;
			r.box = {{c + 0.25, d + 0.25}, {c + 0.75, d + 0.75}};
			rights.push_back(r);
		}

		// Many far-away regions, arranged deterministically well outside the
		// left grid, so that a shared traversal can prune whole right subtrees.
		for (uint32_t i = 0; i < rightFar; ++i)
		{
			const double c = 1000.0 + static_cast<double>(i % 20) * 5.0;
			const double d = 1000.0 + static_cast<double>(i / 20) * 5.0;
			Rec r;
			r.id = nextId++;
			r.box = {{c, d}, {c + 1.0, d + 1.0}};
			rights.push_back(r);
		}
	}

	// Walks every node reachable from the root and reports the tree height
	// (leaves are level 0, so a single-leaf tree has height 1).
	class HeightStrategy : public IQueryStrategy
	{
	public:
		void getNextEntry(const IEntry& entry, id_type& nextEntry, bool& hasNext) override
		{
			const INode* node = dynamic_cast<const INode*>(&entry);
			if (node != nullptr)
			{
				++m_nodes;
				if (node->getLevel() + 1 > m_height) m_height = node->getLevel() + 1;

				// Leaf children are data identifiers, not node pages, so only
				// index nodes can be descended into via queryStrategy().
				if (node->isIndex())
				{
					for (uint32_t i = 0; i < node->getChildrenCount(); ++i)
						m_pending.push_back(node->getChildIdentifier(i));
				}
			}

			if (m_pending.empty())
			{
				hasNext = false;
			}
			else
			{
				nextEntry = m_pending.back();
				m_pending.pop_back();
				hasNext = true;
			}
		}

		uint32_t m_height = 0;
		uint32_t m_nodes = 0;

	private:
		std::vector<id_type> m_pending;
	};

	uint32_t treeHeight(ISpatialIndex& tree)
	{
		HeightStrategy strategy;
		tree.queryStrategy(strategy);
		return strategy.m_height;
	}

	uint64_t readsOf(ISpatialIndex& tree)
	{
		IStatistics* stats = nullptr;
		tree.getStatistics(&stats);
		const uint64_t reads = stats->getReads();
		delete stats;
		return reads;
	}

	// Legacy single-shape k-NN visitor, used only as the read-count baseline.
	class IdCollector : public IVisitor
	{
	public:
		void visitNode(const INode&) override {}
		void visitData(const IData& d) override { m_ids.push_back(d.getIdentifier()); }
		void visitData(std::vector<const IData*>&) override {}

		std::vector<id_type> m_ids;
	};

	class FlagVisitor : public RTree::IJoinVisitor
	{
	public:
		void visitPair(const IData&, const IData&, double) override { m_called = true; }
		bool m_called = false;
	};

	class ThrowingVisitor : public RTree::IJoinVisitor
	{
	public:
		void visitPair(const IData&, const IData&, double) override
		{
			++m_calls;
			throw std::runtime_error("join visitor failure");
		}

		int m_calls = 0;
	};

	int runMixedCase(uint32_t k, int& cases)
	{
		std::vector<Rec> lefts;
		std::vector<Rec> rights;
		buildMixedDataset(lefts, rights);

		std::unique_ptr<IStorageManager> sm(StorageManager::createNewMemoryStorageManager());
		id_type leftIndexId = -1;
		id_type rightIndexId = -1;
		std::unique_ptr<ISpatialIndex> left(
			buildTree(*sm, lefts, 8, leftIndexId));
		std::unique_ptr<ISpatialIndex> right(
			buildTree(*sm, rights, 8, rightIndexId));

		CollectingVisitor visitor;
		RTree::nearestNeighborJoin(*left, *right, k, visitor);

		if (checkAgainstReference(lefts, rights, k, visitor.m_pairs) < 0) return -1;
		++cases;
		return 0;
	}

	int runEmptyCase(bool emptyLeft, int& cases)
	{
		std::vector<Rec> lefts;
		std::vector<Rec> rights;
		buildMixedDataset(lefts, rights);

		std::unique_ptr<IStorageManager> smL(StorageManager::createNewMemoryStorageManager());
		std::unique_ptr<IStorageManager> smR(StorageManager::createNewMemoryStorageManager());
		id_type dummy = -1;

		std::unique_ptr<ISpatialIndex> left(buildTree(*smL, emptyLeft ? std::vector<Rec>() : lefts, 8, dummy));
		std::unique_ptr<ISpatialIndex> right(buildTree(*smR, emptyLeft ? rights : std::vector<Rec>(), 8, dummy));

		CollectingVisitor visitor;
		RTree::nearestNeighborJoin(*left, *right, 3, visitor);
		CHECK_CHECK(visitor.m_pairs.empty());
		++cases;
		return 0;
	}

	int runSelfJoinCase(int& cases)
	{
		std::vector<Rec> recs;
		for (id_type i = 1; i <= 8; ++i)
		{
			const double c = static_cast<double>(i) * 3.0;
			recs.push_back({i, {{c, c}, {c + 1.0, c + 1.0}}});
		}

		std::unique_ptr<IStorageManager> sm(StorageManager::createNewMemoryStorageManager());
		id_type indexId = -1;
		std::unique_ptr<ISpatialIndex> tree(buildTree(*sm, recs, 4, indexId));

		CollectingVisitor visitor;
		RTree::nearestNeighborJoin(*tree, *tree, 2, visitor);

		if (checkAgainstReference(recs, recs, 2, visitor.m_pairs) < 0) return -1;

		// Directed self join: every record pairs with itself at distance 0.
		std::set<std::pair<id_type, id_type> > pairs;
		bool selfSeen = false;
		for (size_t i = 0; i < visitor.m_pairs.size(); ++i)
		{
			const PairRec& p = visitor.m_pairs[i];
			pairs.insert(std::make_pair(p.leftId, p.rightId));
			if (p.leftId == p.rightId)
			{
				CHECK_CHECK(nearlyEqual(p.distance, 0.0));
				selfSeen = true;
			}
		}
		CHECK_CHECK(selfSeen);

		// Both directions of an ordered pair survive as distinct entries.
		CHECK_CHECK(pairs.count(std::make_pair(1, 2)) == 1);
		CHECK_CHECK(pairs.count(std::make_pair(2, 1)) == 1);
		++cases;
		return 0;
	}

	int runHeightCase(bool tallLeft, int& cases)
	{
		std::vector<Rec> small;
		std::vector<Rec> big;
		std::vector<Rec> other;
		buildGridDataset(big, other, 8, 4, 40); // 64 dense records versus 44 rights

		for (id_type i = 1000; i < 1003; ++i)
			small.push_back({i, {{static_cast<double>(i), 0.0}, {static_cast<double>(i) + 1.0, 1.0}}});

		std::unique_ptr<IStorageManager> smL(StorageManager::createNewMemoryStorageManager());
		std::unique_ptr<IStorageManager> smR(StorageManager::createNewMemoryStorageManager());
		id_type dummy = -1;

		const std::vector<Rec>& leftRecs = tallLeft ? big : small;
		const std::vector<Rec>& rightRecs = tallLeft ? small : big;

		std::unique_ptr<ISpatialIndex> left(buildTree(*smL, leftRecs, 4, dummy));
		std::unique_ptr<ISpatialIndex> right(buildTree(*smR, rightRecs, 4, dummy));

		const uint32_t hLeft = treeHeight(*left);
		const uint32_t hRight = treeHeight(*right);
		CHECK_CHECK(hLeft != hRight);
		if (tallLeft) CHECK_CHECK(hLeft > hRight);
		else CHECK_CHECK(hRight > hLeft);

		CollectingVisitor visitor;
		RTree::nearestNeighborJoin(*left, *right, 2, visitor);
		if (checkAgainstReference(leftRecs, rightRecs, 2, visitor.m_pairs) < 0) return -1;
		++cases;
		return 0;
	}

	int runExceptionCase(int& cases)
	{
		std::vector<Rec> lefts;
		std::vector<Rec> rights;
		buildMixedDataset(lefts, rights);

		std::unique_ptr<IStorageManager> sm(StorageManager::createNewMemoryStorageManager());
		id_type dummy = -1;
		std::unique_ptr<ISpatialIndex> left(buildTree(*sm, lefts, 8, dummy));
		std::unique_ptr<ISpatialIndex> right(buildTree(*sm, rights, 8, dummy));

		ThrowingVisitor thrower;
		bool propagated = false;
		try
		{
			RTree::nearestNeighborJoin(*left, *right, 2, thrower);
		}
		catch (const std::runtime_error& e)
		{
			propagated = (std::string(e.what()) == "join visitor failure");
		}
		CHECK_CHECK(propagated);
		CHECK_CHECK(thrower.m_calls > 0);

		// Temporary objects were released: both trees remain queryable.
		CHECK_CHECK(left->isIndexValid());
		CHECK_CHECK(right->isIndexValid());

		CollectingVisitor visitor;
		RTree::nearestNeighborJoin(*left, *right, 2, visitor);
		if (checkAgainstReference(lefts, rights, 2, visitor.m_pairs) < 0) return -1;

		IdCollector legacy;
		Region query = toRegion(lefts[0].box);
		right->nearestNeighborQuery(1, query, legacy);
		CHECK_CHECK(legacy.m_ids.size() >= 1);
		++cases;
		return 0;
	}

	int runInvalidArgumentsCase(int& cases)
	{
		std::vector<Rec> recs2d;
		buildMixedDataset(recs2d, recs2d);

		std::unique_ptr<IStorageManager> smA(StorageManager::createNewMemoryStorageManager());
		std::unique_ptr<IStorageManager> smB(StorageManager::createNewMemoryStorageManager());
		std::unique_ptr<IStorageManager> smC(StorageManager::createNewMemoryStorageManager());

		id_type dummy = -1;
		std::unique_ptr<ISpatialIndex> rtree2d(buildTree(*smA, recs2d, 8, dummy));

		id_type mvrId = -1;
		std::unique_ptr<ISpatialIndex> mvr(
			MVRTree::createNewMVRTree(*smB, 0.7, 10, 10, 2, MVRTree::RV_RSTAR, mvrId));

		id_type id3d = -1;
		std::unique_ptr<ISpatialIndex> rtree3d(
			RTree::createNewRTree(*smC, 0.7, 8, 8, 3, RTree::RV_RSTAR, id3d));
		double low3[3] = {0.0, 0.0, 0.0};
		double high3[3] = {1.0, 1.0, 1.0};
		Region region3(low3, high3, 3);
		rtree3d->insertData(0, nullptr, region3, 1);

		FlagVisitor flag;

		bool caught = false;
		try { RTree::nearestNeighborJoin(*mvr, *rtree2d, 1, flag); }
		catch (const Tools::IllegalArgumentException&) { caught = true; }
		CHECK_CHECK(caught);
		CHECK_CHECK(!flag.m_called);

		caught = false;
		try { RTree::nearestNeighborJoin(*rtree2d, *mvr, 1, flag); }
		catch (const Tools::IllegalArgumentException&) { caught = true; }
		CHECK_CHECK(caught);
		CHECK_CHECK(!flag.m_called);

		caught = false;
		try { RTree::nearestNeighborJoin(*rtree2d, *rtree3d, 1, flag); }
		catch (const Tools::IllegalArgumentException&) { caught = true; }
		CHECK_CHECK(caught);
		CHECK_CHECK(!flag.m_called);

		caught = false;
		try { RTree::nearestNeighborJoin(*rtree2d, *rtree2d, 0, flag); }
		catch (const Tools::IllegalArgumentException&) { caught = true; }
		CHECK_CHECK(caught);
		CHECK_CHECK(!flag.m_called);
		++cases;
		return 0;
	}

	int resultsGroup(int& cases)
	{
		// k below the right count, including ties at the k-th distance.
		if (runMixedCase(3, cases) < 0) return -1;

		// k larger than the whole right table: every right record is returned.
		if (runMixedCase(50, cases) < 0) return -1;

		if (runEmptyCase(true, cases) < 0) return -1;
		if (runEmptyCase(false, cases) < 0) return -1;
		if (runSelfJoinCase(cases) < 0) return -1;
		if (runHeightCase(false, cases) < 0) return -1;
		if (runHeightCase(true, cases) < 0) return -1;
		if (runExceptionCase(cases) < 0) return -1;
		if (runInvalidArgumentsCase(cases) < 0) return -1;
		return 0;
	}

	std::string tempBase(const char* label)
	{
		static unsigned counter = 0;
		const char* dir = std::getenv("TMPDIR");
		if (dir == nullptr || *dir == '\0') dir = "/tmp";
		std::string base = std::string(dir) + "/sidx_join_" + label + "_" +
			std::to_string(static_cast<long long>(std::time(nullptr))) + "_" +
			std::to_string(counter++);
		return base;
	}

	void removeFile(const std::string& path)
	{
		std::remove(path.c_str());
	}

	int storageGroup(int& cases)
	{
		std::vector<Rec> lefts;
		std::vector<Rec> rights;
		buildMixedDataset(lefts, rights);

		const std::string leftBase = tempBase("left");
		const std::string rightBase = tempBase("right");
		id_type leftIndexId = -1;
		id_type rightIndexId = -1;

		{
			std::unique_ptr<IStorageManager> diskL(
				StorageManager::createNewDiskStorageManager(const_cast<std::string&>(leftBase), 4096));
			std::unique_ptr<StorageManager::IBuffer> bufL(
				StorageManager::createNewRandomEvictionsBuffer(*diskL, 8, false));
			std::unique_ptr<IStorageManager> diskR(
				StorageManager::createNewDiskStorageManager(const_cast<std::string&>(rightBase), 4096));
			std::unique_ptr<StorageManager::IBuffer> bufR(
				StorageManager::createNewRandomEvictionsBuffer(*diskR, 8, false));

			std::unique_ptr<ISpatialIndex> left(buildTree(*bufL, lefts, 4, leftIndexId));
			std::unique_ptr<ISpatialIndex> right(buildTree(*bufR, rights, 4, rightIndexId));

			// The two storages hand out the same page numbers independently.
			CHECK_CHECK(leftIndexId == rightIndexId);

			CollectingVisitor before;
			RTree::nearestNeighborJoin(*left, *right, 3, before);
			if (checkAgainstReference(lefts, rights, 3, before.m_pairs) < 0) return -1;

			left->flush();
			right->flush();
		}

		// Reopen both trees from disk and rerun every check against the
		// independent reference, including k larger than the right table.
		{
			std::unique_ptr<IStorageManager> diskL(
				StorageManager::loadDiskStorageManager(const_cast<std::string&>(leftBase)));
			std::unique_ptr<StorageManager::IBuffer> bufL(
				StorageManager::createNewRandomEvictionsBuffer(*diskL, 8, false));
			std::unique_ptr<IStorageManager> diskR(
				StorageManager::loadDiskStorageManager(const_cast<std::string&>(rightBase)));
			std::unique_ptr<StorageManager::IBuffer> bufR(
				StorageManager::createNewRandomEvictionsBuffer(*diskR, 8, false));

			std::unique_ptr<ISpatialIndex> left(RTree::loadRTree(*bufL, leftIndexId));
			std::unique_ptr<ISpatialIndex> right(RTree::loadRTree(*bufR, rightIndexId));

			CHECK_CHECK(left->isIndexValid());
			CHECK_CHECK(right->isIndexValid());

			CollectingVisitor reopened;
			RTree::nearestNeighborJoin(*left, *right, 3, reopened);
			if (checkAgainstReference(lefts, rights, 3, reopened.m_pairs) < 0) return -1;

			CollectingVisitor allRights;
			RTree::nearestNeighborJoin(*left, *right, 50, allRights);
			if (checkAgainstReference(lefts, rights, 50, allRights.m_pairs) < 0) return -1;
		}
		++cases;

		// A visitor throwing against disk-backed trees must leave the trees
		// usable, including after closing and reopening the storage.
		{
			std::unique_ptr<IStorageManager> diskL(
				StorageManager::loadDiskStorageManager(const_cast<std::string&>(leftBase)));
			std::unique_ptr<StorageManager::IBuffer> bufL(
				StorageManager::createNewRandomEvictionsBuffer(*diskL, 4, false));
			std::unique_ptr<IStorageManager> diskR(
				StorageManager::loadDiskStorageManager(const_cast<std::string&>(rightBase)));
			std::unique_ptr<StorageManager::IBuffer> bufR(
				StorageManager::createNewRandomEvictionsBuffer(*diskR, 4, false));

			std::unique_ptr<ISpatialIndex> left(RTree::loadRTree(*bufL, leftIndexId));
			std::unique_ptr<ISpatialIndex> right(RTree::loadRTree(*bufR, rightIndexId));

			ThrowingVisitor thrower;
			bool propagated = false;
			try
			{
				RTree::nearestNeighborJoin(*left, *right, 2, thrower);
			}
			catch (const std::runtime_error&)
			{
				propagated = true;
			}
			CHECK_CHECK(propagated);
			CHECK_CHECK(left->isIndexValid());
			CHECK_CHECK(right->isIndexValid());

			CollectingVisitor after;
			RTree::nearestNeighborJoin(*left, *right, 2, after);
			if (checkAgainstReference(lefts, rights, 2, after.m_pairs) < 0) return -1;
		}
		++cases;

		removeFile(leftBase + ".dat");
		removeFile(leftBase + ".idx");
		removeFile(rightBase + ".dat");
		removeFile(rightBase + ".idx");
		return 0;
	}

	int pruningGroup(int& cases)
	{
		std::vector<Rec> lefts;
		std::vector<Rec> rights;

		// Dense deterministic left side, a few near right regions and a large
		// set of far right regions in tall trees (leaf capacity 4).
		buildGridDataset(lefts, rights, 7, 6, 180);

		// One extra isolated left record whose nearest right record is far
		// away: a shared global cutoff would wrongly leave it without results.
		lefts.push_back({9001, {{-5000.0, -5000.0}, {-5000.0, -5000.0}}});

		std::unique_ptr<IStorageManager> smL(StorageManager::createNewMemoryStorageManager());
		std::unique_ptr<IStorageManager> smR(StorageManager::createNewMemoryStorageManager());
		id_type dummy = -1;
		std::unique_ptr<ISpatialIndex> left(buildTree(*smL, lefts, 4, dummy));
		std::unique_ptr<ISpatialIndex> right(buildTree(*smR, rights, 4, dummy));

		CHECK_CHECK(treeHeight(*left) >= 2);
		CHECK_CHECK(treeHeight(*right) >= 2);

		const uint64_t readsBeforeJoin = readsOf(*right);
		CollectingVisitor joined;
		RTree::nearestNeighborJoin(*left, *right, 3, joined);
		const uint64_t joinReads = readsOf(*right) - readsBeforeJoin;

		if (checkAgainstReference(lefts, rights, 3, joined.m_pairs) < 0) return -1;

		// Application-level alternative: one legacy single-shape k-NN query
		// per left record, counting right-tree reads for the same work.
		const uint64_t readsBeforeNaive = readsOf(*right);
		std::vector<PairRec> naivePairs;

		for (size_t i = 0; i < lefts.size(); ++i)
		{
			IdCollector collector;
			Region query = toRegion(lefts[i].box);
			right->nearestNeighborQuery(3, query, collector);

			for (size_t j = 0; j < collector.m_ids.size(); ++j)
			{
				const Rec* r = findRec(rights, collector.m_ids[j]);
				CHECK_CHECK(r != nullptr);
				PairRec p;
				p.leftId = lefts[i].id;
				p.rightId = r->id;
				p.distance = referenceDistance(lefts[i].box, r->box);
				naivePairs.push_back(p);
			}
		}
		const uint64_t naiveReads = readsOf(*right) - readsBeforeNaive;

		if (checkAgainstReference(lefts, rights, 3, naivePairs, false) < 0) return -1;

		// Counted evidence, not timing: the shared traversal must issue
		// strictly fewer right-tree node reads than per-record queries.
		std::printf(
			"JOIN_PRUNING left=%zu right=%zu join_right_reads=%llu naive_right_reads=%llu\n",
			lefts.size(), rights.size(),
			static_cast<unsigned long long>(joinReads),
			static_cast<unsigned long long>(naiveReads));
		CHECK_CHECK(joinReads < naiveReads);
		CHECK_CHECK(naiveReads >= lefts.size());

		// The isolated left record still received a full answer despite its
		// large nearest distance, proving bounds are per left record.
		size_t isolatedAnswers = 0;
		for (size_t i = 0; i < joined.m_pairs.size(); ++i)
			if (joined.m_pairs[i].leftId == 9001) ++isolatedAnswers;
		const std::vector<id_type> isolatedWant =
			expectedRights(lefts.back(), rights, 3);
		CHECK_CHECK(isolatedAnswers == isolatedWant.size());
		++cases;
		return 0;
	}
}

int join_case(const char* group)
{
	int cases = 0;

	if (group == nullptr) return -1;

	if (std::string(group) == "results")
	{
		if (resultsGroup(cases) < 0) return -1;
	}
	else if (std::string(group) == "storage")
	{
		if (storageGroup(cases) < 0) return -1;
	}
	else if (std::string(group) == "pruning")
	{
		if (pruningGroup(cases) < 0) return -1;
	}
	else
	{
		return -1;
	}

	return cases > 0 ? cases : -1;
}
