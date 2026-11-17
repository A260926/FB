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
// Author: J. Andreas Bærentzen,
// Created: Wed Jan 24 18:29:0

#include <cassert>
#include <cmath>
#include <type_traits>
#include <vector>
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
		CGLA::Vec3i top_dims;

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

		/** \brief Fill an axis aligned, half-open box of voxels with a value.

			The box spans the voxels @c p with @c lo[i] <= p[i] < @c hi[i] for
			all three axes. A precondition is that @c lo[i] <= @c hi[i] on each
			axis. An empty box, either as given or after clipping against the
			grid, leaves the grid unchanged.

			The box is clipped to the grid. Parts outside the grid are ignored,
			and values outside the box are preserved.

			Top level cells which are completely covered by the box are merged
			into a single value directly - they are never split and written
			voxel by voxel. Cells which are only partially covered are modified
			voxel by voxel, and afterwards such a cell is merged again if all
			its in-domain voxels became equal. The padding of a partially
			covered last cell is not part of the voxel domain and is never
			examined or modified.

			The value must be finite. This is only enforced for floating point
			types (by an assertion). */
 		void fill_box(const CGLA::Vec3i& lo, const CGLA::Vec3i& hi, const T& value)
		{
			assert(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]);
			if constexpr (std::is_floating_point_v<T>)
				assert(std::isfinite(value));

			const CGLA::Vec3i dims = this->get_dims();
			const CGLA::Vec3i lo_c = CGLA::v_max(lo, CGLA::Vec3i(0));
			const CGLA::Vec3i hi_c = CGLA::v_min(hi, dims);
			if(lo_c[0] >= hi_c[0] || lo_c[1] >= hi_c[1] || lo_c[2] >= hi_c[2])
				return;

			const int bd = CellT::get_dim();
			const CGLA::Vec3i t_lo = lo_c / bd;
			const CGLA::Vec3i t_hi = (hi_c - CGLA::Vec3i(1)) / bd + CGLA::Vec3i(1);

			for(int tz = t_lo[2]; tz < t_hi[2]; ++tz)
				for(int ty = t_lo[1]; ty < t_hi[1]; ++ty)
					for(int tx = t_lo[0]; tx < t_hi[0]; ++tx)
					{
						const CGLA::Vec3i toff(tx*bd, ty*bd, tz*bd);
						// In-domain extent of this top level cell. The
						// remainder (padding) of a last cell is not part of
						// the voxel domain.
						const CGLA::Vec3i ext = CGLA::v_min(CGLA::Vec3i(bd), dims - toff);

						// Box clipped to this cell's in-domain extent.
						const CGLA::Vec3i clo = CGLA::v_max(lo_c - toff, CGLA::Vec3i(0));
						const CGLA::Vec3i chi = CGLA::v_min(hi_c - toff, ext);
						if(clo[0] >= chi[0] || clo[1] >= chi[1] || clo[2] >= chi[2])
							continue;

						CellT& cell = get_cell(CGLA::Vec3i(tx, ty, tz));

						// The cell is completely covered: merge directly.
						if(clo == CGLA::Vec3i(0) && chi == ext)
						{
							cell.coalesce(value);
							continue;
						}

						// Partial coverage.
						if(cell.is_coalesced())
						{
							// Nothing changes if the cell already holds the value.
							if(cell[CGLA::Vec3i(0)] == value)
								continue;
							cell.split();
						}

						for(int z = clo[2]; z < chi[2]; ++z)
							for(int y = clo[1]; y < chi[1]; ++y)
								for(int x = clo[0]; x < chi[0]; ++x)
									cell.store(CGLA::Vec3i(x, y, z), value);

						// If every in-domain voxel of the cell is now equal,
						// merge the cell again to regain the compressed form.
						const T first = cell[CGLA::Vec3i(0)];
						bool uniform = true;
						for(int z = 0; z < ext[2] && uniform; ++z)
							for(int y = 0; y < ext[1] && uniform; ++y)
								for(int x = 0; x < ext[0]; ++x)
									if(!(cell[CGLA::Vec3i(x, y, z)] == first))
									{
										uniform = false;
										break;
									}
						if(uniform)
							cell.coalesce(first);
					}
		}

	};

}
#endif
