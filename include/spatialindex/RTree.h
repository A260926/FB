/******************************************************************************
 * Project:  libspatialindex - A C++ library for spatial indexing
 * Author:   Marios Hadjieleftheriou, mhadji@gmail.com
 ******************************************************************************
 * Copyright (c) 2004, Marios Hadjieleftheriou
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

#pragma once

namespace SpatialIndex
{
	namespace RTree
	{
		enum RTreeVariant
		{
			RV_LINEAR = 0x0,
			RV_QUADRATIC,
			RV_RSTAR
		};

		enum BulkLoadMethod
		{
			BLM_STR = 0x0
		};

		enum PersistenObjectIdentifier
		{
			PersistentIndex = 0x1,
			PersistentLeaf = 0x2
		};

		enum RangeQueryType
		{
			ContainmentQuery = 0x1,
			IntersectionQuery = 0x2
		};

		class SIDX_DLL Data : public IData, public Tools::ISerializable
		{
		public:
			Data(uint32_t len, uint8_t* pData, Region& r, id_type id);
			~Data() override;

			Data* clone() override;
			id_type getIdentifier() const override;
			void getShape(IShape** out) const override;
			void getData(uint32_t& len, uint8_t** data) const override;
			uint32_t getByteArraySize() override;
			void loadFromByteArray(const uint8_t* data) override;
			void storeToByteArray(uint8_t** data, uint32_t& len) override;

			id_type m_id;
			Region m_region;
			uint8_t* m_pData;
			uint32_t m_dataLength;
		}; // Data

		// Receives every ordered (left record, right record) pair produced by
		// nearestNeighborJoin(). The IData references and any payload obtained
		// from getData() are only valid for the duration of the visitPair()
		// call; a visitor that needs to keep them must copy the identifiers,
		// shapes and payload bytes itself. An exception thrown out of
		// visitPair() propagates out of nearestNeighborJoin() unchanged, after
		// all temporary objects of the join have been released.
		class SIDX_DLL IJoinVisitor
		{
		public:
			virtual void visitPair(const IData& left, const IData& right, double distance) = 0;
			virtual ~IJoinVisitor() = default;
		}; // IJoinVisitor

		SIDX_DLL ISpatialIndex* returnRTree(IStorageManager& ind, Tools::PropertySet& in);
		SIDX_DLL ISpatialIndex* createNewRTree(
			IStorageManager& sm,
			double fillFactor,
			uint32_t indexCapacity,
			uint32_t leafCapacity,
			uint32_t dimension,
			RTreeVariant rv,
			id_type& indexIdentifier
		);
		SIDX_DLL ISpatialIndex* createAndBulkLoadNewRTree(
			BulkLoadMethod m,
			IDataStream& stream,
			IStorageManager& sm,
			double fillFactor,
			uint32_t indexCapacity,
			uint32_t leafCapacity,
			uint32_t dimension,
			RTreeVariant rv,
			id_type& indexIdentifier
		);
		SIDX_DLL ISpatialIndex* createAndBulkLoadNewRTree(
			BulkLoadMethod m,
			IDataStream& stream,
			IStorageManager& sm,
			Tools::PropertySet& ps,
			id_type& indexIdentifier
		);
		SIDX_DLL ISpatialIndex* loadRTree(IStorageManager& in, id_type indexIdentifier);

		// Batch nearest-neighbor join between two plain R-Trees. For every
		// data entry in left, the k nearest data entries of right are
		// reported to visitor, together with the minimum Euclidean distance
		// between the two stored MBRs (zero when the MBRs intersect or
		// touch). Ties at the k-th distance are all reported, so a left
		// record may receive more than k partners; fewer are reported when
		// right holds less than k records. The association is directed, so
		// passing the same tree for both arguments pairs every record with
		// itself as well. Both trees must have the same dimensionality and
		// coordinate units. Throws Tools::IllegalArgumentException before any
		// callback when an argument is not an RTree, when the
		// dimensionalities differ or when k is zero. Neither tree is fully
		// materialized: the traversal prunes node pairs on the lower-bound
		// distance between their MBRs and keeps a separate pruning bound per
		// left record.
		SIDX_DLL void nearestNeighborJoin(
			ISpatialIndex& left,
			ISpatialIndex& right,
			uint32_t k,
			IJoinVisitor& visitor
		);
	}
}
