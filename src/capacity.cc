// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (LBL / UC Berkeley)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file capacity.cc
 * \brief Function implementations for the capacity-constrained power
 * diagram solver. */

#include <cmath>
#include <limits>

#include "voro++.hh"
#include "capacity.hh"

namespace voro {

/** The maximum number of step halvings tried by the line search before it
 * reports that no acceptable progress can be made. */
const int capacity_max_halvings=40;

/** Tests whether a floating point number is finite, without relying on
 * library extensions.
 * \param[in] x the value to test.
 * \return True if x is finite, false for NaN and infinities. */
static inline bool capacity_finite(double x) {
	double m=std::numeric_limits<double>::max();
	return x>=-m&&x<=m;
}

/** Validates the common problem description before any geometry is built.
 * \param[in] (ax,bx) the x extents of the box.
 * \param[in] (ay,by) the y extents of the box.
 * \param[in] (az,bz) the z extents of the box.
 * \param[in] ids the seed IDs.
 * \param[in] positions the seed positions as consecutive (x,y,z) triples.
 * \param[in] target_volumes the target volumes, or NULL if not applicable.
 * \param[in] weights the power weights, or NULL if not applicable.
 * \return True if the input is valid, false otherwise. */
static bool capacity_validate(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> *target_volumes,const std::vector<double> *weights) {
	int n=(int) ids.size(),i,j;
	if(n<2||n>64) return false;
	if(positions.size()!=(size_t) 3*n) return false;
	if(target_volumes!=0&&target_volumes->size()!=(size_t) n) return false;
	if(weights!=0&&weights->size()!=(size_t) n) return false;

	// The box must have finite, positive side lengths
	if(!capacity_finite(ax)||!capacity_finite(bx)||
	   !capacity_finite(ay)||!capacity_finite(by)||
	   !capacity_finite(az)||!capacity_finite(bz)) return false;
	if(!(bx>ax)||!(by>ay)||!(bz>az)) return false;

	// IDs must be unique and non-negative, so that they cannot be
	// confused with the negative wall IDs
	for(i=0;i<n;i++) {
		if(ids[i]<0) return false;
		for(j=i+1;j<n;j++) if(ids[i]==ids[j]) return false;
	}

	// Positions must be finite, inside the box, and distinct
	for(i=0;i<n;i++) {
		double x=positions[3*i],y=positions[3*i+1],z=positions[3*i+2];
		if(!capacity_finite(x)||!capacity_finite(y)||!capacity_finite(z)) return false;
		if(x<ax||x>bx||y<ay||y>by||z<az||z>bz) return false;
		for(j=i+1;j<n;j++)
			if(positions[3*j]==x&&positions[3*j+1]==y&&positions[3*j+2]==z) return false;
	}

	// Target volumes must be finite and positive, and their sum must
	// match the box volume; they are used exactly as supplied
	if(target_volumes!=0) {
		double vbox=(bx-ax)*(by-ay)*(bz-az),sum=0;
		for(i=0;i<n;i++) {
			double t=(*target_volumes)[i];
			if(!capacity_finite(t)||t<=0) return false;
			sum+=t;
		}
		if(fabs(sum-vbox)>1e-10*vbox) return false;
	}

	// Weights may have arbitrary signs but must be finite
	if(weights!=0) for(i=0;i<n;i++) if(!capacity_finite((*weights)[i])) return false;
	return true;
}

/** Finds the index of a seed ID in the ID list.
 * \param[in] ids the ID list.
 * \param[in] id the ID to find.
 * \return The index, or -1 if the ID is not present. */
static int capacity_index_of(const std::vector<int> &ids,int id) {
	for(int i=0;i<(int) ids.size();i++) if(ids[i]==id) return i;
	return -1;
}

/** Builds the full power diagram geometry for a weight vector from
 * scratch. This is the single geometry path shared by capacity_evaluate()
 * and the iteration inside capacity_solve(); every accepted or trial state
 * of the solver corresponds to one call of this routine, so adjacency
 * information always reflects the current tessellation.
 * \param[in] (ax,bx) the x extents of the box.
 * \param[in] (ay,by) the y extents of the box.
 * \param[in] (az,bz) the z extents of the box.
 * \param[in] ids the seed IDs.
 * \param[in] positions the seed positions as consecutive (x,y,z) triples.
 * \param[in] weights the power weights.
 * \param[out] out the geometry and derivative matrix.
 * \return True if every seed has a non-empty cell, false otherwise. */
static bool capacity_build(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_eval &out) {
	int n=(int) ids.size(),i;

	// Shift the weights so that the smallest is zero. The geometry only
	// depends on weight differences, and the shifted weights are valid
	// squared radii for the radical tessellation.
	double wmin=weights[0];
	for(i=1;i<n;i++) if(weights[i]<wmin) wmin=weights[i];

	// Set up a non-periodic container with a reasonable block grid
	double dx=bx-ax,dy=by-ay,dz=bz-az;
	double ilscale=pow(n/(optimal_particles*dx*dy*dz),1/3.0);
	int nx=int(dx*ilscale+1),ny=int(dy*ilscale+1),nz=int(dz*ilscale+1);
	container_poly con(ax,bx,ay,by,az,bz,nx,ny,nz,false,false,false,8);
	particle_order vo;
	for(i=0;i<n;i++) {
		double r=sqrt(weights[i]-wmin);
		con.put(vo,ids[i],positions[3*i],positions[3*i+1],positions[3*i+2],r);
	}
	if(con.total_particles()!=n) return false;

	out.ids=ids;
	out.volumes.assign(n,0);
	out.centroids.assign(3*n,0);
	out.dvolume.assign((size_t) n*n,0);
	out.cells.clear();
	out.cells.resize(n);

	bool all_filled=true;
	voronoicell_neighbor c(con);
	c_loop_order vl(con,vo);
	std::vector<int> fv,nb;
	std::vector<double> vv,fa;
	i=0;
	if(vl.start()) do {
		capacity_cell &cell=out.cells[i];
		cell.id=ids[i];
		if(!con.compute_cell(c,vl)) {
			// The cell vanished completely under the plane cuts
			all_filled=false;
			cell.volume=0;
			cell.centroid[0]=positions[3*i];
			cell.centroid[1]=positions[3*i+1];
			cell.centroid[2]=positions[3*i+2];
			out.centroids[3*i]=cell.centroid[0];
			out.centroids[3*i+1]=cell.centroid[1];
			out.centroids[3*i+2]=cell.centroid[2];
			i++;
			continue;
		}
		double vol=c.volume();
		if(!(vol>0)) all_filled=false;
		cell.volume=vol;
		out.volumes[i]=vol;

		// The cell centroid is computed relative to the seed position;
		// shift it into world coordinates
		double cx,cy,cz;
		c.centroid(cx,cy,cz);
		cell.centroid[0]=positions[3*i]+cx;
		cell.centroid[1]=positions[3*i+1]+cy;
		cell.centroid[2]=positions[3*i+2]+cz;
		out.centroids[3*i]=cell.centroid[0];
		out.centroids[3*i+1]=cell.centroid[1];
		out.centroids[3*i+2]=cell.centroid[2];

		// Vertices in world coordinates
		c.vertices(positions[3*i],positions[3*i+1],positions[3*i+2],vv);
		cell.vertices=vv;

		// Ordered face vertex loops, the matching neighbor IDs, and the
		// face areas; the three routines traverse the faces in the same
		// order
		c.face_vertices(fv);
		c.neighbors(nb);
		c.face_areas(fa);
		cell.faces.clear();
		cell.face_neighbors.clear();
		int f=0;
		for(size_t k=0;k<fv.size();) {
			int cnt=fv[k++];
			cell.faces.push_back(std::vector<int>(fv.begin()+k,fv.begin()+k+cnt));
			k+=cnt;
			cell.face_neighbors.push_back(f<(int) nb.size()?nb[f]:0);
			f++;
		}

		// Assemble the volume derivative row of this cell from the real
		// shared faces. Moving w_j shifts the face between i and j by
		// 1/(2|p_i-p_j|) along their connecting line, so the face of
		// area A contributes -A/(2|p_i-p_j|) to dV_i/dw_j. Faces on the
		// boundary box walls have negative IDs and are not particles, so
		// they contribute nothing. The diagonal is the negative sum of
		// the row, which makes every row sum to zero.
		for(size_t g=0;g<cell.face_neighbors.size();g++) {
			int nid=cell.face_neighbors[g];
			if(nid<0) continue;
			int j=capacity_index_of(ids,nid);
			if(j<0) continue;
			double px=positions[3*i]-positions[3*j],
			       py=positions[3*i+1]-positions[3*j+1],
			       pz=positions[3*i+2]-positions[3*j+2],
			       d=sqrt(px*px+py*py+pz*pz);
			if(d<=0) continue;
			double coeff=(g<fa.size()?fa[g]:0)/(2*d);
			out.dvolume[(size_t) i*n+j]-=coeff;
			out.dvolume[(size_t) i*n+i]+=coeff;
		}
		i++;
	} while(vl.inc());
	return all_filled;
}

/** Computes the maximum relative volume error of a geometry with respect
 * to the target volumes.
 * \param[in] ev the evaluated geometry.
 * \param[in] target_volumes the target volumes.
 * \return The maximum of |V_i-t_i|/t_i over all cells. */
static double capacity_rel_error(const capacity_eval &ev,const std::vector<double> &target_volumes) {
	double err=0;
	for(int i=0;i<(int) target_volumes.size();i++) {
		double e=fabs(ev.volumes[i]-target_volumes[i])/target_volumes[i];
		if(e>err) err=e;
	}
	return err;
}

/** Computes the smallest cell volume of a geometry.
 * \param[in] ev the evaluated geometry.
 * \return The minimum cell volume. */
static double capacity_min_volume(const capacity_eval &ev) {
	double mv=ev.volumes.empty()?0:ev.volumes[0];
	for(size_t i=1;i<ev.volumes.size();i++) if(ev.volumes[i]<mv) mv=ev.volumes[i];
	return mv;
}

/** Normalizes a weight vector in place so that its smallest entry is
 * zero.
 * \param[in,out] w the weight vector. */
static void capacity_normalize(std::vector<double> &w) {
	double wmin=w[0];
	for(size_t i=1;i<w.size();i++) if(w[i]<wmin) wmin=w[i];
	for(size_t i=0;i<w.size();i++) w[i]-=wmin;
}

/** Solves a dense linear system A x = b by Gaussian elimination with
 * partial pivoting.
 * \param[in,out] A the matrix in row-major order; destroyed on return.
 * \param[in,out] b the right-hand side; holds the solution on return.
 * \param[in] m the dimension of the system.
 * \return True if the system was nonsingular, false otherwise. */
static bool capacity_dense_solve(std::vector<double> &A,std::vector<double> &b,int m) {
	int col,row,c;
	double amax=0;
	for(col=0;col<m*m;col++) if(fabs(A[col])>amax) amax=fabs(A[col]);
	if(!(amax>0)) return false;
	double tiny=1e-13*amax;
	for(col=0;col<m;col++) {
		int piv=col;
		double best=fabs(A[(size_t) col*m+col]);
		for(row=col+1;row<m;row++) {
			double v=fabs(A[(size_t) row*m+col]);
			if(v>best) {best=v;piv=row;}
		}
		if(best<=tiny) return false;
		if(piv!=col) {
			for(c=col;c<m;c++) {
				double t=A[(size_t) col*m+c];
				A[(size_t) col*m+c]=A[(size_t) piv*m+c];
				A[(size_t) piv*m+c]=t;
			}
			double t=b[col];b[col]=b[piv];b[piv]=t;
		}
		double inv=1/A[(size_t) col*m+col];
		for(row=col+1;row<m;row++) {
			double fac=A[(size_t) row*m+col]*inv;
			if(fac==0) continue;
			for(c=col;c<m;c++) A[(size_t) row*m+c]-=fac*A[(size_t) col*m+c];
			b[row]-=fac*b[col];
		}
	}
	for(row=m-1;row>=0;row--) {
		double s=b[row];
		for(c=row+1;c<m;c++) s-=A[(size_t) row*m+c]*b[c];
		b[row]=s/A[(size_t) row*m+row];
	}
	return true;
}

bool capacity_evaluate(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_eval &out) {
	if(!capacity_validate(ax,bx,ay,by,az,bz,ids,positions,0,&weights)) return false;
	return capacity_build(ax,bx,ay,by,az,bz,ids,positions,weights,out);
}

capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &opts,const std::vector<double> *initial_weights) {
	out=capacity_result();
	int n=(int) ids.size(),i;

	// Validate the problem description and the solver options before
	// doing any work
	if(!capacity_validate(ax,bx,ay,by,az,bz,ids,positions,&target_volumes,initial_weights))
		return out.status=capacity_invalid_input;
	if(!capacity_finite(opts.rel_tol)||opts.rel_tol<=0||opts.max_iterations<0)
		return out.status=capacity_invalid_input;

	// Start from the caller's weights if given, otherwise from equal
	// weights; the global additive constant is removed by normalization
	std::vector<double> w(n,0);
	if(initial_weights!=0) w=*initial_weights;
	capacity_normalize(w);

	// Evaluate the initial state
	capacity_eval cur;
	out.geometry_builds=1;
	if(!capacity_build(ax,bx,ay,by,az,bz,ids,positions,w,cur)) {
		out.status=capacity_empty_initial;
		out.weights=w;
		return out.status;
	}
	double cur_err=capacity_rel_error(cur,target_volumes);

	// Record the initial accepted state
	capacity_step st0;
	st0.iteration=0;st0.max_rel_error=cur_err;
	st0.min_volume=capacity_min_volume(cur);
	st0.geometry_builds=out.geometry_builds;
	out.history.push_back(st0);

	capacity_status status;
	if(cur_err<=opts.rel_tol) {
		status=capacity_converged;
	} else {
		status=capacity_max_iterations;
		for(int it=1;it<=opts.max_iterations;it++) {

			// Newton step for V(w)=t: solve M delta = -(r-mean(r))
			// where M is the derivative matrix of the current state and
			// r the volume residual. Projecting the right-hand side
			// removes the component along the weight-shift direction,
			// which is the gauge freedom of the problem. The system is
			// solved with the first weight pinned at zero change.
			std::vector<double> r(n);
			double rmean=0;
			for(i=0;i<n;i++) {
				r[i]=cur.volumes[i]-target_volumes[i];
				rmean+=r[i];
			}
			rmean/=n;
			std::vector<double> A((size_t) (n-1)*(n-1)),b(n-1);
			for(i=1;i<n;i++) {
				for(int j=1;j<n;j++)
					A[(size_t) (i-1)*(n-1)+(j-1)]=cur.dvolume[(size_t) i*n+j];
				b[i-1]=-(r[i]-rmean);
			}
			if(!capacity_dense_solve(A,b,n-1)) {status=capacity_no_progress;break;}
			std::vector<double> delta(n);
			delta[0]=0;
			for(i=1;i<n;i++) delta[i]=b[i-1];

			// Line search: halve the step until the trial state has no
			// empty cell and lowers the residual. A vanished cell is a
			// rejected trial, never a converged state, and every trial
			// triggers a full geometry rebuild.
			double t=1;
			bool accepted=false;
			for(int half=0;half<capacity_max_halvings;half++) {
				std::vector<double> wt(n);
				for(i=0;i<n;i++) wt[i]=w[i]+t*delta[i];
				capacity_eval trial;
				out.geometry_builds++;
				if(!capacity_build(ax,bx,ay,by,az,bz,ids,positions,wt,trial)) {t*=0.5;continue;}
				double terr=capacity_rel_error(trial,target_volumes);
				if(terr<cur_err) {
					w=wt;
					capacity_normalize(w);
					cur=trial;
					cur_err=terr;
					accepted=true;
					break;
				}
				t*=0.5;
			}
			if(!accepted) {status=capacity_no_progress;break;}

			// Record the newly accepted state
			capacity_step st;
			st.iteration=it;st.max_rel_error=cur_err;
			st.min_volume=capacity_min_volume(cur);
			st.geometry_builds=out.geometry_builds;
			out.history.push_back(st);
			out.iterations=it;
			if(cur_err<=opts.rel_tol) {status=capacity_converged;break;}
		}
	}

	out.status=status;
	out.max_rel_error=cur_err;
	out.weights=w;
	out.volumes=cur.volumes;
	out.cells=cur.cells;
	return status;
}

}
