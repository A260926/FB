/******************************************************************************
 * Project:  libspatialindex - A C++ library for spatial indexing
 * Author:   Marios Hadjieleftheriou, mhadji@gmail.com
 ******************************************************************************
 * Copyright (c) 2002, Marios Hadjieleftheriou
 *
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
******************************************************************************/

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include <spatialindex/SpatialIndex.h>
#include "RTree.h"
#include "Node.h"
#include "Leaf.h"

using namespace SpatialIndex;
using namespace SpatialIndex::RTree;

namespace
{
	// Minimum Euclidean distance between two axis-aligned boxes. It matches
	// Region::getMinimumDistance(Region): per dimension the gap is zero when
	// the projections overlap or touch, so intersecting/contacting boxes
	// report a distance of zero. Only the stored MBRs are used; payload
	// geometry is never interpreted.
	double mbrMinimumDistance(const Region& a, const Region& b)
	{
		double ret = 0.0;

		for (uint32_t cDim = 0; cDim < a.m_dimension; ++cDim)
		{
			double gap = 0.0;

			if (b.m_pHigh[cDim] < a.m_pLow[cDim])
			{
				gap = a.m_pLow[cDim] - b.m_pHigh[cDim];
			}
			else if (a.m_pHigh[cDim] < b.m_pLow[cDim])
			{
				gap = b.m_pLow[cDim] - a.m_pHigh[cDim];
			}

			ret += gap * gap;
		}

		return std::sqrt(ret);
	}
}

// Shared batch k-nearest-neighbor join.
//
// The left tree is descended node by node. For every left leaf, one
// best-first traversal of the right tree is shared by all data records held
// in that leaf: right node MBRs are pruned on their lower-bound distance,
// and each left record keeps an independent candidate set and upper bound.
// A right entry is skipped only when every left record's own bound rejects
// it, so one record's tight bound can never truncate another record's
// answers. When the two trees have different heights the two descents are
// independent: a left leaf can be reached while the right traversal still
// has index levels to refine. Neither tree is ever fully materialized and
// the legacy single-shape nearestNeighborQuery() is never called.
class SpatialIndex::RTree::RTree::NearestNeighborJoinEngine
{
private:
	struct Candidate
	{
		std::shared_ptr<Data> m_data;
		double m_distance;

		Candidate(std::shared_ptr<Data> d, double dist)
			: m_data(std::move(d)), m_distance(dist) {}
	};

	struct CandidateCompare
	{
		// Ascending order on distance, identifier as a deterministic tie
		// break.
		bool operator()(const Candidate& a, const Candidate& b) const
		{
			if (a.m_distance != b.m_distance) return a.m_distance < b.m_distance;
			return a.m_data->getIdentifier() < b.m_data->getIdentifier();
		}
	};

	// Per-left-record state: the k nearest right records seen so far, all
	// ties at the k-th distance retained, and an independent pruning bound.
	// The shared best-first queue is ordered across all records of a leaf, so
	// a far candidate can arrive before this record's true neighbors; the
	// threshold is therefore recomputed (and can only decrease) whenever a
	// better candidate arrives, rather than being fixed on the k-th insert.
	class LeftBound
	{
	public:
		LeftBound(std::shared_ptr<Data> leftData, uint32_t k)
			: m_leftData(std::move(leftData)), m_k(k),
			  m_threshold(std::numeric_limits<double>::infinity()) {}

		bool canPrune(double distance) const
		{
			return m_items.size() >= m_k && distance > m_threshold;
		}

		void add(const std::shared_ptr<Data>& d, double distance)
		{
			// Once k candidates are held, anything strictly farther than the
			// k-th distance can never be part of the answer.
			if (m_items.size() >= m_k && distance > m_threshold) return;

			m_items.push_back(Candidate(d, distance));

			if (m_items.size() >= m_k)
			{
				// Recompute the current k-th smallest distance and discard
				// only entries strictly farther than it; entries tied at the
				// k-th distance are all retained.
				std::nth_element(m_items.begin(),
					m_items.begin() + (m_k - 1), m_items.end(), CandidateCompare());
				m_threshold = m_items[m_k - 1].m_distance;

				m_items.erase(
					std::remove_if(m_items.begin(), m_items.end(),
						[this](const Candidate& c) { return c.m_distance > m_threshold; }),
					m_items.end());
			}
		}

		std::shared_ptr<Data> m_leftData;
		std::vector<Candidate> m_items;

	private:
		uint32_t m_k;
		double m_threshold;
	};

	// One entry of the shared right-tree best-first queue. Node entries
	// (m_isData == false) carry the child/node MBR and its page id; data
	// entries additionally reference a copied RTree::Data with its payload.
	struct RightEntry
	{
		Region m_mbr;
		id_type m_id;
		bool m_isData;
		std::shared_ptr<Data> m_data;
		double m_lowerBound;

		RightEntry(const Region& r, id_type id, bool isData,
			std::shared_ptr<Data> d, double lowerBound)
			: m_mbr(r), m_id(id), m_isData(isData), m_data(std::move(d)),
			  m_lowerBound(lowerBound) {}
	};

	struct RightCompare
	{
		// min-heap on the lower-bound distance for best-first traversal.
		bool operator()(const RightEntry* a, const RightEntry* b) const
		{
			return a->m_lowerBound > b->m_lowerBound;
		}
	};

	struct LeftRecord
	{
		std::shared_ptr<Data> m_data;
		Region m_mbr;
		id_type m_id;
		LeftBound* m_bound;

		LeftRecord(std::shared_ptr<Data> d, const Region& r, id_type id, LeftBound* b)
			: m_data(std::move(d)), m_mbr(r), m_id(id), m_bound(b) {}
	};

public:
	NearestNeighborJoinEngine(RTree& left, RTree& right, uint32_t k, IJoinVisitor& visitor)
		: m_left(left), m_right(right), m_k(k), m_visitor(visitor) {}

	void run()
	{
		NodePtr rootL = m_left.readNode(m_left.m_rootID);
		NodePtr rootR = m_right.readNode(m_right.m_rootID);

		// An empty tree cannot produce any pair.
		if (rootL->m_children == 0 || rootR->m_children == 0) return;

		// Descend the left tree once; every reached leaf is served by a
		// single shared traversal of the right tree. m_bounds owns every
		// per-record state, so an exception at any point (including a
		// visitPair() throw in report()) releases all temporary objects.
		std::vector<id_type> leftStack;
		leftStack.push_back(m_left.m_rootID);

		while (! leftStack.empty())
		{
			id_type page = leftStack.back();
			leftStack.pop_back();

			NodePtr node = m_left.readNode(page);

			if (node->m_level == 0)
			{
				processLeftLeaf(*node);
			}
			else
			{
				for (uint32_t cChild = 0; cChild < node->m_children; ++cChild)
					leftStack.push_back(node->m_pIdentifier[cChild]);
			}
		}

		report();
	}

private:
	RTree& m_left;
	RTree& m_right;
	uint32_t m_k;
	IJoinVisitor& m_visitor;
	std::unordered_map<id_type, LeftBound*> m_boundsIndex;
	std::vector<std::unique_ptr<LeftBound> > m_bounds;

	LeftBound& boundFor(id_type leftId, const std::shared_ptr<Data>& leftData)
	{
		std::unordered_map<id_type, LeftBound*>::iterator it = m_boundsIndex.find(leftId);
		if (it != m_boundsIndex.end()) return *(it->second);

		LeftBound* b = new LeftBound(leftData, m_k);
		m_boundsIndex.insert(std::make_pair(leftId, b));
		m_bounds.push_back(std::unique_ptr<LeftBound>(b));
		return *b;
	}

	// Smallest lower bound from a right MBR to any record of this leaf. The
	// per-record distances are also filled in so callers can apply each
	// record's own bound.
	double minLowerBound(
		const std::vector<LeftRecord>& records, const Region& rightMBR,
		std::vector<double>& perRecordDistance)
	{
		double best = std::numeric_limits<double>::infinity();
		perRecordDistance.clear();
		perRecordDistance.reserve(records.size());

		for (size_t i = 0; i < records.size(); ++i)
		{
			double d = mbrMinimumDistance(records[i].m_mbr, rightMBR);
			perRecordDistance.push_back(d);
			if (d < best) best = d;
		}
		return best;
	}

	// True only when every left record's independent bound rejects this
	// distance. There is no global cutoff: as long as one record can still
	// accept an entry at this distance, the entry is refined.
	bool allRecordsPrune(
		const std::vector<LeftRecord>& records, const std::vector<double>& distances)
	{
		for (size_t i = 0; i < records.size(); ++i)
		{
			if (! records[i].m_bound->canPrune(distances[i])) return false;
		}
		return true;
	}

	void processLeftLeaf(Node& leaf)
	{
		std::vector<LeftRecord> records;
		records.reserve(leaf.m_children);

		for (uint32_t cChild = 0; cChild < leaf.m_children; ++cChild)
		{
			std::shared_ptr<Data> d(new Data(
				leaf.m_pDataLength[cChild],
				leaf.m_pData[cChild],
				*(leaf.m_ptrMBR[cChild]),
				leaf.m_pIdentifier[cChild]));
			LeftBound& bound = boundFor(leaf.m_pIdentifier[cChild], d);
			records.push_back(LeftRecord(d, *(leaf.m_ptrMBR[cChild]),
				leaf.m_pIdentifier[cChild], &bound));
		}

		std::priority_queue<RightEntry*, std::vector<RightEntry*>, RightCompare> queue;
		std::vector<double> distances;

		try
		{
			NodePtr root = m_right.readNode(m_right.m_rootID);
			double rootLB = minLowerBound(records, root->m_nodeMBR, distances);
			queue.push(new RightEntry(root->m_nodeMBR, m_right.m_rootID,
				false, nullptr, rootLB));

			while (! queue.empty())
			{
				std::unique_ptr<RightEntry> entry(queue.top());
				queue.pop();

				minLowerBound(records, entry->m_mbr, distances);
				if (allRecordsPrune(records, distances))
				{
					// A pruned node is not refined, so whole right subtrees
					// can be discarded together; a pruned data entry is
					// simply not offered to any record.
					continue;
				}

				if (entry->m_isData)
				{
					for (size_t i = 0; i < records.size(); ++i)
					{
						if (! records[i].m_bound->canPrune(distances[i]))
							records[i].m_bound->add(entry->m_data, distances[i]);
					}
					continue;
				}

				NodePtr node = m_right.readNode(entry->m_id);

				for (uint32_t cChild = 0; cChild < node->m_children; ++cChild)
				{
					std::vector<double> childDistances;
					double lb = minLowerBound(records,
						*(node->m_ptrMBR[cChild]), childDistances);

					if (allRecordsPrune(records, childDistances)) continue;

					if (node->m_level == 0)
					{
						std::shared_ptr<Data> d(new Data(
							node->m_pDataLength[cChild],
							node->m_pData[cChild],
							*(node->m_ptrMBR[cChild]),
							node->m_pIdentifier[cChild]));
						queue.push(new RightEntry(*(node->m_ptrMBR[cChild]),
							node->m_pIdentifier[cChild], true, std::move(d), lb));
					}
					else
					{
						queue.push(new RightEntry(*(node->m_ptrMBR[cChild]),
							node->m_pIdentifier[cChild], false, nullptr, lb));
					}
				}
			}
		}
		catch (...)
		{
			while (! queue.empty())
			{
				delete queue.top();
				queue.pop();
			}
			throw;
		}
	}

	void report()
	{
		// Every surviving candidate belongs to exactly one left record, so
		// every ordered pair is reported exactly once. The bounds (and the
		// Data they reference) are owned by m_bounds and are released when
		// run() returns, even if visitPair() throws.
		for (size_t i = 0; i < m_bounds.size(); ++i)
		{
			LeftBound* bound = m_bounds[i].get();

			// Report in ascending distance (identifier breaks ties); no Data
			// outlives the join call.
			std::sort(bound->m_items.begin(), bound->m_items.end(), CandidateCompare());

			for (size_t j = 0; j < bound->m_items.size(); ++j)
			{
				const Candidate& c = bound->m_items[j];
				m_visitor.visitPair(*(bound->m_leftData), *(c.m_data), c.m_distance);
			}
		}
	}
};

void SpatialIndex::RTree::RTree::nearestNeighborJoinImpl(RTree& right, uint32_t k, IJoinVisitor& visitor)
{
	NearestNeighborJoinEngine engine(*this, right, k, visitor);
	engine.run();
}

void SpatialIndex::RTree::nearestNeighborJoin(
	ISpatialIndex& left,
	ISpatialIndex& right,
	uint32_t k,
	IJoinVisitor& visitor)
{
	// Everything is validated before any node is read and before any
	// visitPair() callback can happen.
	RTree* pLeft = dynamic_cast<RTree*>(&left);
	RTree* pRight = dynamic_cast<RTree*>(&right);

	if (pLeft == nullptr || pRight == nullptr)
		throw Tools::IllegalArgumentException(
			"nearestNeighborJoin: both spatial indexes must be RTree instances.");

	if (pLeft->m_dimension != pRight->m_dimension)
		throw Tools::IllegalArgumentException(
			"nearestNeighborJoin: the two RTrees have different dimensionalities.");

	if (k == 0)
		throw Tools::IllegalArgumentException(
			"nearestNeighborJoin: k must be greater than zero.");

	pLeft->nearestNeighborJoinImpl(*pRight, k, visitor);
}
