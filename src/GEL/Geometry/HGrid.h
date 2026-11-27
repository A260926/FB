/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file HGrid.h
 * @brief Hierarchical voxel grid - space saving.
 */

#ifndef __GEOMETRY_HGRID_H
#define __GEOMETRY_HGRID_H
// Author: J. Andreas B�rentzen,
// Created: Wed Jan 24 18:29:0

#include <vector>
#include <GEL/CGLA/ArithVec.h>
#include <GEL/Geometry/AncestorGrid.h>
#include <GEL/Geometry/Cell.h>

namespace Geometry 
{
	/** \brief Hierarchical voxel grid. 

			In many cases we wish to save on the storage requirements of volumes.
			A hierarchical voxel grid is a volume representation where the volume 
			is divided into box shaped regions, and each region is represented only 
			if it contains voxels. This class template is for such a grid.
	*/
	template<class T, class CellT=DefaultCell<T,8> >
	class HGrid: public AncestorGrid<T,HGrid<T,CellT> > 
	{
	public:
		typedef T DataType;		
		typedef CellT CellType;

	private:

		/// Dimensions of top level grid.
		const CGLA::Vec3i top_dims;

		/// Top level grid. I.e. vector of sub grids.
		std::vector<CellT> top_grid;

		/// The default grid value, used to clear grid. 
		DataType default_val;
		
		/// Size of the top grid (number of cells x*y*z)
		int top_grid_size;

	public:

		const CGLA::Vec3i& get_top_dims() const {return top_dims;}
		
		int get_bottom_dim() const {return CellT::get_dim();}

		
	private:
 
		/// Get index into top level grid from int vector position.
		int get_top_index(const CGLA::Vec3i& idx) const
		{
			const CGLA::Vec3i top_idx = idx/get_bottom_dim();
			return (top_idx[2]*top_dims[1]+top_idx[1])*top_dims[0]+top_idx[0];
		}

	public:

		/// Construct grid of specified dimensions
		HGrid(const CGLA::Vec3i& dims, const T& val = T()):
			AncestorGrid<T,HGrid<T,CellT> >(dims),
			top_dims(dims/CellT::get_dim()+
							 CGLA::Vec3i(dims[0]%CellT::get_dim()?1:0,
													 dims[1]%CellT::get_dim()?1:0,
													 dims[2]%CellT::get_dim()?1:0)),
			top_grid(top_dims[0]*top_dims[1]*top_dims[2],CellT(val)),
			default_val(val), top_grid_size(top_grid.size())
		{}

		/** Store a voxel vox at position p in grid. The Cell will
				automatically subdivide if it is not already subdivided. */
 		void store(const CGLA::Vec3i& p, const T& vox)
		{
			assert(this->in_domain(p));
			top_grid[get_top_index(p)].store(p, vox);
		}

		/** Fill the half open voxel box [lo,hi) with a single value.

				The box is clipped against the grid domain; if the intersection
				is empty the grid is left unchanged. Bricks whose entire logical
				(voxel domain) extent is covered are coalesced directly into a
				single value without being expanded first. For partially covered
				bricks only the affected voxels are written, and afterwards the
				brick is coalesced again if all of its valid voxels (padding of
				the final bricks excluded) ended up with the same value. */
		void fill_box(const CGLA::Vec3i& lo, const CGLA::Vec3i& hi, const T& value)
		{
			const CGLA::Vec3i& dims = this->get_dims();
			const CGLA::Vec3i box_lo = CGLA::v_max(lo, CGLA::Vec3i(0));
			const CGLA::Vec3i box_hi = CGLA::v_min(hi, dims);
			if(box_hi[0] <= box_lo[0] || box_hi[1] <= box_lo[1] || box_hi[2] <= box_lo[2])
				return;
			const int bdim = get_bottom_dim();
			const CGLA::Vec3i bt_lo = box_lo/bdim;
			const CGLA::Vec3i bt_hi = (box_hi-CGLA::Vec3i(1))/bdim;
			for(CGLA::Vec3i bt(bt_lo); bt[2]<=bt_hi[2]; ++bt[2])
				for(bt[1]=bt_lo[1]; bt[1]<=bt_hi[1]; ++bt[1])
					for(bt[0]=bt_lo[0]; bt[0]<=bt_hi[0]; ++bt[0])
					{
						const CGLA::Vec3i org = bt*bdim;
						const CGLA::Vec3i valid_hi = CGLA::v_min(org+CGLA::Vec3i(bdim), dims);
						const CGLA::Vec3i lo_c = CGLA::v_max(box_lo, org);
						const CGLA::Vec3i hi_c = CGLA::v_min(box_hi, valid_hi);
						CellT& cell = get_cell(bt);
						if(lo_c==org && hi_c==valid_hi)
						{
							// The whole logical brick is covered: coalesce
							// directly instead of expanding and writing.
							cell.coalesce(value);
							continue;
						}
						if(cell.is_coalesced())
						{
							if(cell[org]==value)
								continue; // Already uniformly the fill value.
							cell.split();
						}
						for(CGLA::Vec3i p(lo_c); p[2]<hi_c[2]; ++p[2])
							for(p[1]=lo_c[1]; p[1]<hi_c[1]; ++p[1])
								for(p[0]=lo_c[0]; p[0]<hi_c[0]; ++p[0])
									cell.store(p, value);
						// Coalesce again if every valid voxel now holds
						// the same value.
						const T v0 = cell[org];
						bool uniform = true;
						for(CGLA::Vec3i p(org); uniform && p[2]<valid_hi[2]; ++p[2])
							for(p[1]=org[1]; uniform && p[1]<valid_hi[1]; ++p[1])
								for(p[0]=org[0]; p[0]<valid_hi[0]; ++p[0])
									if(!(cell[p]==v0))
									{
										uniform = false;
										break;
									}
						if(uniform)
							cell.coalesce(v0);
					}
		}


		/** Read only access to a voxel in the grid. */
 		const T& operator[](const CGLA::Vec3i& p) const
		{
			assert(this->in_domain(p));
			return top_grid[get_top_index(p)][p];
		}


//  		bool get(const CGLA::Vec3i& p, T* voxel)
// 		{
// 			assert(in_domain(p));
// 			CellT* cell = top_grid[get_top_index(p)];
// 			if(cell->is_coalesced()) return false;
// 			voxel = cell[p];
// 		}

		CellT& get_cell(const CGLA::Vec3i& p)
		{
			return top_grid[(p[2]*top_dims[1]+p[1])*top_dims[0]+p[0]];
		}

		const CellT& get_cell(const CGLA::Vec3i& p) const
		{
			return top_grid[(p[2]*top_dims[1]+p[1])*top_dims[0]+p[0]];
		}

		CellT& get_cell(int i) 
		{
			return top_grid[i];
		}

		const CellT& get_cell(int i) const
		{
			return top_grid[i];
		}

		void clear()
		{
			int N = top_grid.size();
			for(int i=0;i<N;++i)
				top_grid[i].coalesce(default_val);
		}
		
	};

}
#endif
