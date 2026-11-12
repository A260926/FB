// Voro++, a 3D cell-based Voronoi library
//
// Author   : Chris H. Rycroft (Harvard University / LBL)
// Email    : chr@alum.mit.edu
// Date     : August 30th 2011

/** \file capacity.cc
 * \brief Function implementations for the capacity-constrained power
 * diagram solver. */

#include "capacity.hh"

#include <cmath>

#include "voro++.hh"

namespace voro {

namespace {

/** The minimum number of seeds. */
const int min_seeds=2;
/** The maximum number of seeds. */
const int max_seeds=64;
/** The maximum number of step-halving attempts within one iteration
 * before the solve declares that no acceptable progress can be made. */
const int max_backtracks=50;
/** The relative accuracy to which the target volumes must match the box
 * volume. */
const double target_sum_tolerance=1e-10;

/** Chooses the number of computational blocks per direction for the
 * container, aiming for roughly one block per seed. */
inline int grid_size(int n) {
	int g=1;
	while(g<8&&g*g*g<n) g++;
	return g;
}

/** Shifts a weight vector so that its minimum entry is zero. */
inline void normalize_weights(std::vector<double> &w) {
	double m=w[0];
	for(size_t i=1;i<w.size();i++) if(w[i]<m) m=w[i];
	for(size_t i=0;i<w.size();i++) w[i]-=m;
}

/** Checks the inputs shared by the evaluation and solve routines.
 * \return True if the input is valid. */
bool valid_common(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights) {
	if(!std::isfinite(ax)||!std::isfinite(bx)||!std::isfinite(ay)||
	   !std::isfinite(by)||!std::isfinite(az)||!std::isfinite(bz)) return false;
	if(!(bx>ax)||!(by>ay)||!(bz>az)) return false;
	size_t n=ids.size();
	if(n<(size_t) min_seeds||n>(size_t) max_seeds) return false;
	if(positions.size()!=3*n||weights.size()!=n) return false;
	for(size_t i=0;i<n;i++) {
		if(ids[i]<0) return false;
		for(size_t j=i+1;j<n;j++) if(ids[j]==ids[i]) return false;
		double x=positions[3*i],y=positions[3*i+1],z=positions[3*i+2];
		if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) return false;
		if(!(x>ax&&x<bx&&y>ay&&y<by&&z>az&&z<bz)) return false;
		if(!std::isfinite(weights[i])) return false;
		for(size_t j=i+1;j<n;j++)
			if(positions[3*j]==x&&positions[3*j+1]==y&&positions[3*j+2]==z) return false;
	}
	return true;
}

/** Computes the complete power diagram for one weight vector: every cell
 * is rebuilt from scratch, so any change in the adjacency structure is
 * reflected in the output. Also assembles the derivative of the cell
 * volumes with respect to the power weights from the current shared-face
 * geometry.
 * \return True if every cell is non-empty. */
bool build_geometry(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_geometry &out) {
	const int n=(int) ids.size();
	double wmin=weights[0];
	for(int i=1;i<n;i++) if(weights[i]<wmin) wmin=weights[i];
	int g=grid_size(n);
	container_poly con(ax,bx,ay,by,az,bz,g,g,g,false,false,false,8);
	for(int i=0;i<n;i++)
		con.put(ids[i],positions[3*i],positions[3*i+1],positions[3*i+2],
				sqrt(weights[i]-wmin));
	out.cells.clear();
	out.cells.resize(n);
	out.derivative.assign((size_t) n*n,0.0);
	bool ok=true;
	voronoicell_neighbor c(con);
	std::vector<double> fa,verts;
	std::vector<int> nb,fv;
	c_loop_all vl(con);
	if(vl.start()) do {
		int pid=vl.pid(),idx=-1;
		for(int i=0;i<n;i++) if(ids[i]==pid) {idx=i;break;}
		if(idx<0) continue;
		capacity_cell &cell=out.cells[idx];
		cell.id=pid;
		if(!con.compute_cell(c,vl)) {ok=false;cell.volume=0;continue;}
		double x=positions[3*idx],y=positions[3*idx+1],z=positions[3*idx+2];
		cell.volume=c.volume();
		if(!(cell.volume>0)) {ok=false;continue;}
		double cx,cy,cz;
		c.centroid(cx,cy,cz);
		cell.centroid[0]=cx+x;cell.centroid[1]=cy+y;cell.centroid[2]=cz+z;
		c.vertices(x,y,z,verts);
		cell.vertices=verts;
		c.face_vertices(fv);
		cell.faces.clear();
		for(size_t p=0;p<fv.size();) {
			int k=fv[p];
			cell.faces.push_back(std::vector<int>(fv.begin()+p+1,fv.begin()+p+1+k));
			p+=1+(size_t) k;
		}
		c.neighbors(nb);
		cell.face_neighbors=nb;
		c.face_areas(fa);
		double *row=&out.derivative[(size_t) idx*n];
		for(size_t f=0;f<nb.size();f++) {
			int nid=nb[f];
			if(nid<0) continue;
			int j=-1;
			for(int i2=0;i2<n;i2++) if(ids[i2]==nid) {j=i2;break;}
			if(j<0||j==idx) continue;
			double dx=positions[3*j]-x,dy=positions[3*j+1]-y,dz=positions[3*j+2]-z;
			double d=sqrt(dx*dx+dy*dy+dz*dz);
			if(!(d>0)) continue;
			double contrib=fa[f]/(2*d);
			row[j]-=contrib;
			row[idx]+=contrib;
		}
	} while(vl.inc());
	out.ok=ok;
	return ok;
}

/** Computes the maximum relative volume error max_i |V_i-t_i|/t_i. */
double max_relative_residual(const capacity_geometry &geo,const std::vector<double> &target) {
	double r=0;
	for(size_t i=0;i<target.size();i++) {
		double e=fabs(geo.cells[i].volume-target[i])/target[i];
		if(e>r) r=e;
	}
	return r;
}

/** Computes the smallest cell volume of a geometry. */
double min_cell_volume(const capacity_geometry &geo) {
	double m=geo.cells.empty()?0:geo.cells[0].volume;
	for(size_t i=1;i<geo.cells.size();i++) if(geo.cells[i].volume<m) m=geo.cells[i].volume;
	return m;
}

/** Solves a dense linear system Ax=b by Gaussian elimination with partial
 * pivoting. On success the solution is returned in b.
 * \return False if the matrix is singular or a non-finite value appears. */
bool solve_linear_system(std::vector<double> &a,std::vector<double> &b,int n) {
	for(int col=0;col<n;col++) {
		int piv=col;
		double best=fabs(a[(size_t) col*n+col]);
		for(int r=col+1;r<n;r++) {
			double v=fabs(a[(size_t) r*n+col]);
			if(v>best) {best=v;piv=r;}
		}
		if(!(best>0)) return false;
		if(piv!=col) {
			for(int c2=col;c2<n;c2++) {
				double t=a[(size_t) col*n+c2];
				a[(size_t) col*n+c2]=a[(size_t) piv*n+c2];
				a[(size_t) piv*n+c2]=t;
			}
			double t=b[col];b[col]=b[piv];b[piv]=t;
		}
		double inv=1.0/a[(size_t) col*n+col];
		for(int r=col+1;r<n;r++) {
			double f=a[(size_t) r*n+col]*inv;
			if(f==0) continue;
			a[(size_t) r*n+col]=0;
			for(int c2=col+1;c2<n;c2++) a[(size_t) r*n+c2]-=f*a[(size_t) col*n+c2];
			b[r]-=f*b[col];
		}
	}
	for(int r=n-1;r>=0;r--) {
		double s=b[r];
		for(int c2=r+1;c2<n;c2++) s-=a[(size_t) r*n+c2]*b[c2];
		double d=a[(size_t) r*n+r];
		if(!(d!=0)||!std::isfinite(s)) return false;
		b[r]=s/d;
	}
	return true;
}

/** Computes the Newton step for the volume equations. The global weight
 * shift freedom is handled by fixing the step of the last seed to zero,
 * reducing the system to the leading (n-1) by (n-1) block of the
 * derivative matrix, which is positive definite whenever every cell is
 * non-empty.
 * \return False if the system could not be solved. */
bool newton_step(const capacity_geometry &geo,const std::vector<double> &target,int n,
		std::vector<double> &delta) {
	delta.assign(n,0.0);
	int m=n-1;
	std::vector<double> a((size_t) m*m),b(m);
	for(int i=0;i<m;i++) {
		for(int j=0;j<m;j++) a[(size_t) i*m+j]=geo.derivative[(size_t) i*n+j];
		b[i]=target[i]-geo.cells[i].volume;
	}
	if(!solve_linear_system(a,b,m)) return false;
	for(int i=0;i<m;i++) {
		if(!std::isfinite(b[i])) return false;
		delta[i]=b[i];
	}
	return true;
}

}

const char* capacity_status_name(capacity_status status) {
	switch(status) {
		case capacity_converged: return "converged";
		case capacity_max_iterations: return "max_iterations";
		case capacity_no_progress: return "no_progress";
		case capacity_empty_initial: return "empty_initial";
		case capacity_invalid_input: return "invalid_input";
	}
	return "unknown";
}

capacity_status capacity_evaluate(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &weights,capacity_geometry &out) {
	out=capacity_geometry();
	if(!valid_common(ax,bx,ay,by,az,bz,ids,positions,weights)) return capacity_invalid_input;
	return build_geometry(ax,bx,ay,by,az,bz,ids,positions,weights,out)?
		capacity_converged:capacity_empty_initial;
}

capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out) {
	return capacity_solve(ax,bx,ay,by,az,bz,ids,positions,target_volumes,out,
			capacity_options(),std::vector<double>());
}

capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &options) {
	return capacity_solve(ax,bx,ay,by,az,bz,ids,positions,target_volumes,out,
			options,std::vector<double>());
}

capacity_status capacity_solve(double ax,double bx,double ay,double by,double az,double bz,
		const std::vector<int> &ids,const std::vector<double> &positions,
		const std::vector<double> &target_volumes,capacity_result &out,
		const capacity_options &options,const std::vector<double> &initial_weights) {
	out=capacity_result();
	size_t nn=ids.size();
	int n=(int) nn;
	double boxvol=(bx-ax)*(by-ay)*(bz-az);
	// Validate everything before doing any work. The caller's arrays are
	// only read, never modified, and the targets are never renormalized.
	bool valid=valid_common(ax,bx,ay,by,az,bz,ids,positions,
			initial_weights.empty()?std::vector<double>(nn,0.0):initial_weights);
	if(valid) {
		if(target_volumes.size()!=nn) valid=false;
		if(!std::isfinite(options.tolerance)||!(options.tolerance>0)||
				options.max_iterations<0) valid=false;
		if(!initial_weights.empty()&&initial_weights.size()!=nn) valid=false;
	}
	double tsum=0;
	if(valid) {
		for(size_t i=0;i<nn;i++) {
			if(!std::isfinite(target_volumes[i])||!(target_volumes[i]>0)) {valid=false;break;}
			tsum+=target_volumes[i];
		}
		if(valid&&fabs(tsum-boxvol)>target_sum_tolerance*boxvol) valid=false;
	}
	if(!valid) {out.status=capacity_invalid_input;return out.status;}
	std::vector<double> w=initial_weights.empty()?std::vector<double>(nn,0.0):initial_weights;
	normalize_weights(w);
	int rebuilds=0;
	capacity_geometry geo;
	build_geometry(ax,bx,ay,by,az,bz,ids,positions,w,geo);
	rebuilds++;
	if(!geo.ok) {
		out.status=capacity_empty_initial;
		out.weights=w;
		out.geometry=geo;
		out.total_geometry_rebuilds=rebuilds;
		return out.status;
	}
	double res=max_relative_residual(geo,target_volumes);
	if(res<=options.tolerance) {
		out.status=capacity_converged;
		out.weights=w;
		out.geometry=geo;
		out.total_geometry_rebuilds=rebuilds;
		return out.status;
	}
	for(int iter=0;iter<options.max_iterations;iter++) {
		std::vector<double> delta;
		if(!newton_step(geo,target_volumes,n,delta)) {
			// The Newton system could not be solved; report the last
			// accepted state rather than claiming convergence.
			out.status=capacity_no_progress;
			out.weights=w;
			out.geometry=geo;
			out.total_geometry_rebuilds=rebuilds;
			return out.status;
		}
		double alpha=1.0,trial_res=0;
		int trials=0;
		bool accepted=false;
		capacity_geometry trial_geo;
		std::vector<double> w_trial;
		while(trials<max_backtracks) {
			w_trial=w;
			for(int i=0;i<n;i++) w_trial[i]+=alpha*delta[i];
			normalize_weights(w_trial);
			build_geometry(ax,bx,ay,by,az,bz,ids,positions,w_trial,trial_geo);
			rebuilds++;
			trials++;
			if(trial_geo.ok) {
				trial_res=max_relative_residual(trial_geo,target_volumes);
				if(trial_res<res) {accepted=true;break;}
			}
			// A vanished cell is not a zero error: reject the trial
			// and shorten the step.
			alpha*=0.5;
		}
		if(!accepted) {
			out.status=capacity_no_progress;
			out.weights=w;
			out.geometry=geo;
			out.total_geometry_rebuilds=rebuilds;
			return out.status;
		}
		w.swap(w_trial);
		geo=trial_geo;
		res=trial_res;
		capacity_iteration rec;
		rec.iteration=iter;
		rec.max_residual=res;
		rec.min_cell_volume=min_cell_volume(geo);
		rec.geometry_rebuilds=trials;
		out.history.push_back(rec);
		if(res<=options.tolerance) {
			out.status=capacity_converged;
			out.weights=w;
			out.geometry=geo;
			out.total_geometry_rebuilds=rebuilds;
			return out.status;
		}
	}
	out.status=capacity_max_iterations;
	out.weights=w;
	out.geometry=geo;
	out.total_geometry_rebuilds=rebuilds;
	return out.status;
}

}
