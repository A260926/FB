// Skin weight fitting for ufbx.
//
// The fitter reduces the number of bone influences per vertex while preserving
// the animated deformation. For every vertex it solves, in double precision, a
// constrained least squares problem over the discrete animation samples:
//
//   minimize   sum_s || sum_j w_j * C_j(s) - b(s) ||^2
//   subject to w_j >= 0, sum_j w_j = 1, |{j : w_j != 0}| <= max_weights
//
// C_j(s) is the world position of the morphed local vertex transformed by
// cluster j's geometry_to_world at sample s, and b(s) is the world position the
// original complete skinning produces at that sample. The same constant weights
// are used for every sample.
//
// The solver enumerates every candidate support of size 1..max_weights (at most
// 162 supports as a vertex has at most 8 positive influences and max_weights is
// at most 4) and solves the equality-constrained problem exactly, which also
// handles duplicate or linearly dependent cluster trajectories without any
// regularization.

#include "ufbx.h"
#include "ufbx_skin_fit_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UFBX_SKIN_FIT_MAX_SAMPLES 64
#define UFBX_SKIN_FIT_MAX_INFLUENCES 8
#define UFBX_SKIN_FIT_MAX_WEIGHTS 4

// Negligible negative weight below which a coordinate is treated as numerical
// noise on an otherwise feasible simplex face rather than a real violation.
#define UFBX_SKIN_FIT_NEG_EPS 1e-12
// Rank threshold for the normal-equation solves, relative to the largest entry.
#define UFBX_SKIN_FIT_RANK_EPS 1e-14

// Mirrors UFBX_EPSILON from ufbx.c, the library threshold below which a total
// skin weight is treated as zero by the skinning code.
#define UFBX_SKIN_FIT_NORMALIZE_THRESHOLD \
	(sizeof(ufbx_real) == sizeof(float) ? 1.0842021795674597e-19 : 1.4916681462400413e-154)

typedef struct {
	double x, y, z;
} fit_vec3;

typedef struct {
	double v[12]; // column-major 4x3, matches ufbx_matrix.v
} fit_matrix;

static fit_vec3 fit_vec3_from(ufbx_vec3 v)
{
	fit_vec3 r = { (double)v.x, (double)v.y, (double)v.z };
	return r;
}

static fit_matrix fit_matrix_from(const ufbx_matrix *m)
{
	fit_matrix r;
	for (size_t i = 0; i < 12; i++) r.v[i] = (double)m->v[i];
	return r;
}

static fit_vec3 fit_transform(const fit_matrix *m, fit_vec3 p)
{
	fit_vec3 r;
	r.x = m->v[0]*p.x + m->v[3]*p.y + m->v[6]*p.z + m->v[9];
	r.y = m->v[1]*p.x + m->v[4]*p.y + m->v[7]*p.z + m->v[10];
	r.z = m->v[2]*p.x + m->v[5]*p.y + m->v[8]*p.z + m->v[11];
	return r;
}

static double fit_dot(fit_vec3 a, fit_vec3 b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

static int fit_finite_vec3(fit_vec3 v)
{
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static int fit_finite_matrix(const fit_matrix *m)
{
	for (size_t i = 0; i < 12; i++) {
		if (!isfinite(m->v[i])) return 0;
	}
	return 1;
}

static void fit_set_error(ufbx_error *error, const char *message)
{
	if (!error) return;
	memset(error, 0, sizeof(*error));
	error->type = UFBX_ERROR_UNKNOWN;
	error->description.data = (char*)message;
	error->description.length = strlen(message);
	error->info[0] = '\0';
	error->info_length = 0;
}

// Solve A * y = c for a symmetric positive semi-definite A (normal equations
// of a least squares problem) using Gauss-Jordan elimination with threshold
// pivoting. Rank deficient systems (duplicate or linearly dependent cluster
// trajectories make A singular) get the minimum particular solution with free
// variables set to zero; the system is always consistent. Returns 0 if A has no
// usable pivot at all.
static int fit_solve_psd(size_t n, double A[][UFBX_SKIN_FIT_MAX_WEIGHTS], double *c, double *y)
{
	double scale = 0.0;
	for (size_t i = 0; i < n; i++) {
		for (size_t j = 0; j < n; j++) {
			double a = fabs(A[i][j]);
			if (a > scale) scale = a;
		}
	}
	double pivot_eps = UFBX_SKIN_FIT_RANK_EPS * (scale > 0.0 ? scale : 1.0);

	size_t pivot_col[UFBX_SKIN_FIT_MAX_WEIGHTS];
	size_t num_pivots = 0;
	size_t row = 0;

	for (size_t col = 0; col < n && row < n; col++) {
		size_t best = row;
		double best_abs = fabs(A[row][col]);
		for (size_t i = row + 1; i < n; i++) {
			double a = fabs(A[i][col]);
			if (a > best_abs) { best_abs = a; best = i; }
		}

		if (best_abs <= pivot_eps) {
			y[col] = 0.0; // dependent variable, choose the zero particular solution
			continue;
		}

		if (best != row) {
			for (size_t j = 0; j < n; j++) {
				double t = A[row][j]; A[row][j] = A[best][j]; A[best][j] = t;
			}
			double t = c[row]; c[row] = c[best]; c[best] = t;
		}

		double inv = 1.0 / A[row][col];
		for (size_t j = 0; j < n; j++) A[row][j] *= inv;
		c[row] *= inv;

		for (size_t i = 0; i < n; i++) {
			if (i == row) continue;
			double factor = A[i][col];
			if (factor == 0.0) continue;
			for (size_t j = 0; j < n; j++) A[i][j] -= factor * A[row][j];
			c[i] -= factor * c[row];
		}

		pivot_col[num_pivots++] = col;
		row++;
	}

	for (size_t i = 0; i < num_pivots; i++) {
		// After Gauss-Jordan each pivot row has a single unit coefficient.
		y[pivot_col[i]] = c[i];
	}

	return num_pivots > 0;
}

typedef struct {
	// Original positive influences of the vertex, indices into skin->clusters.
	uint32_t cluster[UFBX_SKIN_FIT_MAX_INFLUENCES];
	size_t num_candidates;

	// Fitted/stored weights, parallel to `cluster[]`.
	double weight[UFBX_SKIN_FIT_MAX_INFLUENCES];
	size_t num_weights;

	int is_zero;    // vertex without any positive influence, kept untouched
	int is_simple;  // only de-zeroed and normalized, no least squares
} fit_vertex;

typedef struct {
	size_t num_samples;
	size_t num_vertices;
	size_t num_clusters;

	// cluster_to_world[s][c]
	fit_matrix *cluster_to_world;
	// instance_to_world[s], the single mesh instance's geometry_to_world
	fit_matrix *instance_to_world;
	// morphed[s][v] local vertex after blend shapes
	fit_vec3 *morphed;
	// reference[s][v] world position from the original complete skinning
	fit_vec3 *reference;
} fit_sample_data;

static const fit_matrix *fit_mat_cell(const fit_sample_data *data, size_t sample, size_t cluster)
{
	return &data->cluster_to_world[sample * data->num_clusters + cluster];
}

static const fit_vec3 *fit_morph_cell(const fit_sample_data *data, size_t sample, size_t vertex)
{
	return &data->morphed[sample * data->num_vertices + vertex];
}

static const fit_vec3 *fit_ref_cell(const fit_sample_data *data, size_t sample, size_t vertex)
{
	return &data->reference[sample * data->num_vertices + vertex];
}

// World position of vertex v's morphed position driven by cluster c at sample s.
static fit_vec3 fit_cluster_position(const fit_sample_data *data, size_t sample, size_t cluster, size_t vertex)
{
	return fit_transform(fit_mat_cell(data, sample, cluster), *fit_morph_cell(data, sample, vertex));
}

// Summed squared objective for one vertex given candidate-local weights.
static double fit_objective(fit_sample_data *data, const fit_vertex *fv, size_t vertex, const double *w)
{
	double obj = 0.0;
	for (size_t s = 0; s < data->num_samples; s++) {
		fit_vec3 pred = { 0.0, 0.0, 0.0 };
		for (size_t c = 0; c < fv->num_candidates; c++) {
			fit_vec3 pos = fit_cluster_position(data, s, fv->cluster[c], vertex);
			pred.x += w[c]*pos.x;
			pred.y += w[c]*pos.y;
			pred.z += w[c]*pos.z;
		}
		fit_vec3 ref = *fit_ref_cell(data, s, vertex);
		fit_vec3 d = { pred.x - ref.x, pred.y - ref.y, pred.z - ref.z };
		obj += fit_dot(d, d);
	}
	return obj;
}

// Solve the simplex-constrained least squares for one vertex by enumerating all
// supports up to max_weights and taking the best feasible one.
static int fit_solve_vertex(fit_sample_data *data, fit_vertex *fv, size_t vertex, size_t max_weights)
{
	size_t k = fv->num_candidates;
	double best_w[UFBX_SKIN_FIT_MAX_INFLUENCES] = { 0 };
	double best_obj = INFINITY;
	int found = 0;

	// Enumerate all non-empty supports of at most max_weights candidates.
	for (uint32_t mask = 1; mask < (1u << k); mask++) {
		size_t r = 0;
		uint32_t m = mask;
		while (m) { r += (size_t)(m & 1u); m >>= 1; }
		if (r > max_weights) continue;

		uint32_t support[UFBX_SKIN_FIT_MAX_WEIGHTS];
		size_t num_support = 0;
		for (size_t c = 0; c < k; c++) {
			if (mask & (1u << c)) support[num_support++] = (uint32_t)c;
		}

		double x[UFBX_SKIN_FIT_MAX_WEIGHTS];

		if (r == 1) {
			x[0] = 1.0;
		} else {
			// Parameterize the sum-to-one constraint using the last support
			// member as the pivot: x_last = 1 - sum(y), turning the problem
			// into an unconstrained PSD least squares in r-1 variables.
			size_t pivot = support[r - 1];
			double A[UFBX_SKIN_FIT_MAX_WEIGHTS][UFBX_SKIN_FIT_MAX_WEIGHTS];
			double c[UFBX_SKIN_FIT_MAX_WEIGHTS];
			size_t n = r - 1;
			memset(A, 0, sizeof(A));
			memset(c, 0, sizeof(c));

			for (size_t s = 0; s < data->num_samples; s++) {
				fit_vec3 cp = fit_cluster_position(data, s, fv->cluster[pivot], vertex);
				fit_vec3 d[UFBX_SKIN_FIT_MAX_WEIGHTS];
				for (size_t a = 0; a < n; a++) {
					fit_vec3 ap = fit_cluster_position(data, s, fv->cluster[support[a]], vertex);
					d[a].x = ap.x - cp.x;
					d[a].y = ap.y - cp.y;
					d[a].z = ap.z - cp.z;
				}
				fit_vec3 rr;
				fit_vec3 ref = *fit_ref_cell(data, s, vertex);
				rr.x = ref.x - cp.x;
				rr.y = ref.y - cp.y;
				rr.z = ref.z - cp.z;

				for (size_t a = 0; a < n; a++) {
					c[a] += fit_dot(d[a], rr);
					for (size_t b = 0; b < n; b++) A[a][b] += fit_dot(d[a], d[b]);
				}
			}

			double y[UFBX_SKIN_FIT_MAX_WEIGHTS];
			if (!fit_solve_psd(n, A, c, y)) continue;

			double sum_y = 0.0;
			for (size_t a = 0; a < n; a++) sum_y += y[a];
			for (size_t a = 0; a < n; a++) x[a] = y[a];
			x[r - 1] = 1.0 - sum_y;
		}

		// Feasibility: the face is only usable if no weight is really negative.
		double max_abs = 0.0;
		for (size_t a = 0; a < r; a++) {
			if (fabs(x[a]) > max_abs) max_abs = fabs(x[a]);
		}
		double neg_eps = UFBX_SKIN_FIT_NEG_EPS * (max_abs > 1.0 ? max_abs : 1.0);
		int feasible = 1;
		for (size_t a = 0; a < r; a++) {
			if (x[a] < -neg_eps) { feasible = 0; break; }
		}
		if (!feasible) continue;

		// Build the candidate-local weight vector, clamping numerical-noise
		// negatives to zero, then renormalize to an exact unit sum.
		double full_w[UFBX_SKIN_FIT_MAX_INFLUENCES];
		for (size_t c = 0; c < k; c++) full_w[c] = 0.0;
		for (size_t a = 0; a < r; a++) {
			double val = x[a] > 0.0 ? x[a] : 0.0;
			full_w[support[a]] = val;
		}
		double full_sum = 0.0;
		for (size_t c = 0; c < k; c++) full_sum += full_w[c];
		if (!(full_sum > 0.0)) continue;
		for (size_t c = 0; c < k; c++) full_w[c] /= full_sum;

		double obj = fit_objective(data, fv, vertex, full_w);
		if (!isfinite(obj)) continue;
		if (!found || obj < best_obj) {
			found = 1;
			best_obj = obj;
			for (size_t c = 0; c < k; c++) best_w[c] = full_w[c];
		}
	}

	if (!found) return 0;

	size_t n = 0;
	for (size_t c = 0; c < k; c++) {
		if (best_w[c] > 0.0) {
			fv->weight[n] = best_w[c];
			fv->cluster[n] = fv->cluster[c];
			n++;
		}
	}
	if (n == 0) return 0;
	// Re-normalize the compacted weights to an exact unit sum.
	double sum = 0.0;
	for (size_t i = 0; i < n; i++) sum += fv->weight[i];
	for (size_t i = 0; i < n; i++) fv->weight[i] /= sum;
	fv->num_weights = n;
	return 1;
}

// Recompute the worst Euclidean error using the final stored ufbx_real weight
// tables. The result scene is re-evaluated at every sample (which also proves
// the installed weights survive `ufbx_evaluate_scene()`); the prediction is
// formed from the evaluated frame's own stored weights and cluster matrices and
// compared against the original full-skinning reference held in `data`.
static int fit_recompute_error(const ufbx_scene *result, size_t mesh_index,
	const fit_sample_data *data, const fit_vertex *vertices, const double *sample_times,
	ufbx_skin_weight_fit_report *report_out)
{
	const ufbx_mesh *base_mesh = result->meshes.data[mesh_index];
	const ufbx_skin_deformer *base_skin = base_mesh->skin_deformers.data[0];
	size_t nv = data->num_vertices;

	ufbx_evaluate_opts eval_opts = { 0 };
	eval_opts.evaluate_skinning = true;

	double worst = -1.0;
	size_t worst_vertex = 0, worst_sample = 0;

	// One evaluation per sample. Exact ties resolve to the smallest vertex and
	// then the smallest sample (structurally identical trajectories produce
	// bit-identical errors, so an equality comparison is well defined here).
	for (size_t s = 0; s < data->num_samples; s++) {
		ufbx_error eval_error;
		ufbx_scene *frame = ufbx_evaluate_scene(result, NULL, sample_times[s], &eval_opts, &eval_error);
		if (!frame) return 0;

		const ufbx_mesh *mesh = frame->meshes.data[mesh_index];
		const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];

		for (size_t v = 0; v < nv; v++) {
			// The fitted tables must carry through evaluation unchanged.
			ufbx_skin_vertex sv = skin->vertices.data[v];
			if (sv.num_weights != base_skin->vertices.data[v].num_weights) {
				ufbx_free_scene(frame); return 0;
			}

			// Morphed local vertex from the evaluated frame itself.
			ufbx_vec3 local = mesh->vertices.data[v];
			for (size_t b = 0; b < mesh->blend_deformers.count; b++) {
				ufbx_add_blend_vertex_offsets(mesh->blend_deformers.data[b], &local, 1, (ufbx_real)1.0);
			}
			fit_vec3 mp = fit_vec3_from(local);

			fit_vec3 pred = { 0.0, 0.0, 0.0 };
			double wsum = 0.0;

			if (vertices[v].is_zero || sv.num_weights == 0) {
				// Zero-influence vertices follow the instance fallback, matching
				// ufbx_get_skin_vertex_matrix() for an unweighted vertex.
				fit_matrix im = fit_matrix_from(&mesh->instances.data[0]->geometry_to_world);
				pred = fit_transform(&im, mp);
			} else {
				for (uint32_t j = 0; j < sv.num_weights; j++) {
					ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + j];
					fit_matrix fm = fit_matrix_from(&skin->clusters.data[sw.cluster_index]->geometry_to_world);
					fit_vec3 col = fit_transform(&fm, mp);
					double w = (double)sw.weight;
					pred.x += w * col.x;
					pred.y += w * col.y;
					pred.z += w * col.z;
					wsum += w;
				}
				if (wsum > 0.0) {
					pred.x /= wsum;
					pred.y /= wsum;
					pred.z /= wsum;
				}
			}

			fit_vec3 ref = *fit_ref_cell(data, s, v);
			fit_vec3 d = { pred.x - ref.x, pred.y - ref.y, pred.z - ref.z };
			double err = sqrt(fit_dot(d, d));
			if (!isfinite(err)) { ufbx_free_scene(frame); return 0; }
			if (err > worst || (err == worst && (v < worst_vertex || (v == worst_vertex && s < worst_sample)))) {
				worst = err;
				worst_vertex = v;
				worst_sample = s;
			}
		}

		ufbx_free_scene(frame);
	}

	if (report_out) {
		report_out->max_error = worst;
		report_out->worst_vertex = worst_vertex;
		report_out->worst_sample = worst_sample;
	}
	return 1;
}

// Validate the input scene/mesh/skin and gather the per-vertex candidate sets.
static int fit_validate_and_prepare(const ufbx_scene *scene, size_t mesh_index,
	const ufbx_skin_weight_fit_opts *opts, fit_vertex **out_vertices,
	const ufbx_mesh **out_mesh, const ufbx_skin_deformer **out_skin)
{
	if (mesh_index >= scene->meshes.count) return 0;
	const ufbx_mesh *mesh = scene->meshes.data[mesh_index];
	if (!mesh || mesh->num_vertices == 0) return 0;
	if (mesh->instances.count != 1) return 0;
	if (mesh->skin_deformers.count != 1) return 0;
	if (mesh->cache_deformers.count != 0) return 0;

	const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
	if (skin->skinning_method != UFBX_SKINNING_METHOD_LINEAR) return 0;
	if (!skin->vertices.data || !skin->weights.data) return 0;
	if (skin->vertices.count != mesh->num_vertices) return 0;
	if (skin->clusters.count == 0) return 0;

	// The skin must not be shared with another mesh.
	for (size_t mi = 0; mi < scene->meshes.count; mi++) {
		const ufbx_mesh *other = scene->meshes.data[mi];
		for (size_t si = 0; si < other->skin_deformers.count; si++) {
			if (other->skin_deformers.data[si] == skin && mi != mesh_index) return 0;
		}
	}

	for (size_t c = 0; c < skin->clusters.count; c++) {
		const ufbx_skin_cluster *cluster = skin->clusters.data[c];
		if (!cluster->bone_node) return 0;
		for (size_t k = 0; k < 12; k++) {
			if (!isfinite((double)cluster->geometry_to_bone.v[k])) return 0;
			if (!isfinite((double)cluster->bind_to_world.v[k])) return 0;
		}
	}

	for (size_t v = 0; v < mesh->num_vertices; v++) {
		if (!isfinite((double)mesh->vertices.data[v].x) ||
			!isfinite((double)mesh->vertices.data[v].y) ||
			!isfinite((double)mesh->vertices.data[v].z)) return 0;
	}

	fit_vertex *vertices = (fit_vertex*)calloc(mesh->num_vertices, sizeof(fit_vertex));
	if (!vertices) return 0;

	double threshold = UFBX_SKIN_FIT_NORMALIZE_THRESHOLD;

	for (size_t v = 0; v < mesh->num_vertices; v++) {
		ufbx_skin_vertex sv = skin->vertices.data[v];
		if ((size_t)sv.weight_begin + (size_t)sv.num_weights > skin->weights.count) goto fail;

		double sum = 0.0;
		size_t positive = 0;
		for (uint32_t j = 0; j < sv.num_weights; j++) {
			ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + j];
			double w = (double)sw.weight;
			if (!isfinite(w) || w < 0.0) goto fail;
			if (sw.cluster_index >= skin->clusters.count) goto fail;
			// Clusters must not repeat within a vertex.
			for (size_t q = 0; q < positive; q++) {
				if (vertices[v].cluster[q] == sw.cluster_index) goto fail;
			}
			if (w > 0.0) {
				if (positive >= UFBX_SKIN_FIT_MAX_INFLUENCES) goto fail;
				vertices[v].cluster[positive] = sw.cluster_index;
				vertices[v].weight[positive] = w;
				positive++;
				sum += w;
			}
		}

		vertices[v].num_candidates = positive;
		vertices[v].num_weights = positive;
		if (positive == 0) {
			vertices[v].is_zero = 1;
			continue;
		}
		if (!isfinite(sum) || !(sum > threshold)) goto fail;

		if (positive <= opts->max_weights) {
			// De-zero and normalize only, no least squares.
			vertices[v].is_simple = 1;
			for (size_t c = 0; c < positive; c++) vertices[v].weight[c] /= sum;
		}
	}

	*out_vertices = vertices;
	*out_mesh = mesh;
	*out_skin = skin;
	return 1;

fail:
	free(vertices);
	return 0;
}

// Collect cluster matrices, morphed local vertices and reference world
// positions for every sample.
static int fit_collect_samples(const ufbx_scene *scene, const ufbx_anim *anim,
	const ufbx_mesh *mesh, const ufbx_skin_weight_fit_opts *opts,
	const double *sample_times, fit_sample_data *data)
{
	memset(data, 0, sizeof(*data));
	data->num_samples = opts->num_samples;
	data->num_vertices = mesh->num_vertices;
	data->num_clusters = mesh->skin_deformers.data[0]->clusters.count;

	size_t ns = data->num_samples, nv = data->num_vertices, nc = data->num_clusters;
	data->cluster_to_world = (fit_matrix*)malloc(ns * nc * sizeof(fit_matrix));
	data->instance_to_world = (fit_matrix*)malloc(ns * sizeof(fit_matrix));
	data->morphed = (fit_vec3*)malloc(ns * nv * sizeof(fit_vec3));
	data->reference = (fit_vec3*)malloc(ns * nv * sizeof(fit_vec3));
	if (!data->cluster_to_world || !data->instance_to_world || !data->morphed || !data->reference) goto fail;

	ufbx_evaluate_opts eval_opts = { 0 };
	eval_opts.evaluate_skinning = true;

	for (size_t s = 0; s < ns; s++) {
		ufbx_error error;
		ufbx_scene *frame = ufbx_evaluate_scene(scene, anim, sample_times[s], &eval_opts, &error);
		if (!frame) goto fail;

		const ufbx_mesh *fmesh = frame->meshes.data[mesh->typed_id];
		if (!fmesh || fmesh->num_vertices != nv) { ufbx_free_scene(frame); goto fail; }
		const ufbx_skin_deformer *fskin = fmesh->skin_deformers.data[0];
		if (fskin->clusters.count != nc) { ufbx_free_scene(frame); goto fail; }
		if (fmesh->skinned_position.values.count != nv) { ufbx_free_scene(frame); goto fail; }

		ufbx_vec3 *local = (ufbx_vec3*)malloc(nv * sizeof(ufbx_vec3));
		if (!local) { ufbx_free_scene(frame); goto fail; }
		memcpy(local, fmesh->vertices.data, nv * sizeof(ufbx_vec3));
		for (size_t b = 0; b < fmesh->blend_deformers.count; b++) {
			ufbx_add_blend_vertex_offsets(fmesh->blend_deformers.data[b], local, nv, (ufbx_real)1.0);
		}

		for (size_t c = 0; c < nc; c++) {
			fit_matrix fm = fit_matrix_from(&fskin->clusters.data[c]->geometry_to_world);
			if (!fit_finite_matrix(&fm)) { free(local); ufbx_free_scene(frame); goto fail; }
			data->cluster_to_world[s * nc + c] = fm;
		}

		if (fmesh->instances.count != 1) { free(local); ufbx_free_scene(frame); goto fail; }
		data->instance_to_world[s] = fit_matrix_from(&fmesh->instances.data[0]->geometry_to_world);
		if (!fit_finite_matrix(&data->instance_to_world[s])) {
			free(local); ufbx_free_scene(frame); goto fail;
		}

		for (size_t v = 0; v < nv; v++) {
			fit_vec3 mp = fit_vec3_from(local[v]);
			if (!fit_finite_vec3(mp)) { free(local); ufbx_free_scene(frame); goto fail; }
			data->morphed[s * nv + v] = mp;

			ufbx_vec3 ref = fmesh->skinned_position.values.data[v];
			fit_vec3 rv = fit_vec3_from(ref);
			if (fmesh->skinned_is_local && fmesh->instances.count > 0) {
				fit_matrix im = fit_matrix_from(&fmesh->instances.data[0]->geometry_to_world);
				rv = fit_transform(&im, rv);
			}
			if (!fit_finite_vec3(rv)) { free(local); ufbx_free_scene(frame); goto fail; }
			data->reference[s * nv + v] = rv;
		}

		free(local);
		ufbx_free_scene(frame);
	}

	return 1;

fail:
	free(data->cluster_to_world);
	free(data->instance_to_world);
	free(data->morphed);
	free(data->reference);
	memset(data, 0, sizeof(*data));
	return 0;
}

static void fit_free_samples(fit_sample_data *data)
{
	free(data->cluster_to_world);
	free(data->instance_to_world);
	free(data->morphed);
	free(data->reference);
	memset(data, 0, sizeof(*data));
}

// Descending weight sort for the stored per-vertex weights, matching ufbx's
// convention that skin weights are ordered largest first.
static void fit_sort_vertex_weights(uint32_t *clusters, ufbx_real *weights, size_t count)
{
	for (size_t i = 1; i < count; i++) {
		ufbx_real w = weights[i];
		uint32_t c = clusters[i];
		size_t j = i;
		while (j > 0 && weights[j - 1] < w) {
			weights[j] = weights[j - 1];
			clusters[j] = clusters[j - 1];
			j--;
		}
		weights[j] = w;
		clusters[j] = c;
	}
}

// Install the fitted tables into the time-zero result scene and recompute its
// skinned positions and normals.
static int fit_install(ufbx_scene *result, size_t mesh_index, const fit_vertex *vertices,
	size_t *out_total_weights)
{
	ufbx_mesh *mesh = result->meshes.data[mesh_index];
	ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
	size_t nv = mesh->num_vertices;
	size_t nc = skin->clusters.count;

	size_t total = 0;
	size_t positive_total = 0;
	size_t max_per_vertex = 0;
	for (size_t v = 0; v < nv; v++) {
		ufbx_skin_vertex old = skin->vertices.data[v];
		size_t cnt = vertices[v].is_zero ? (size_t)old.num_weights : vertices[v].num_weights;
		total += cnt;
		if (!vertices[v].is_zero) positive_total += cnt;
		if (cnt > max_per_vertex) max_per_vertex = cnt;
	}

	ufbx_skin_vertex *new_vertices = (ufbx_skin_vertex*)ufbxi_skin_fit_scene_push(result, sizeof(ufbx_skin_vertex), nv);
	ufbx_skin_weight *new_weights = (ufbx_skin_weight*)ufbxi_skin_fit_scene_push(result, sizeof(ufbx_skin_weight), total);
	if (!new_vertices || !new_weights) return 0;

	uint32_t *cluster_vertices = (uint32_t*)ufbxi_skin_fit_scene_push(result, sizeof(uint32_t), total);
	ufbx_real *cluster_weights = (ufbx_real*)ufbxi_skin_fit_scene_push(result, sizeof(ufbx_real), total);
	if (!cluster_vertices || !cluster_weights) return 0;

	size_t *per_cluster_count = (size_t*)calloc(nc, sizeof(size_t));
	size_t *per_cluster_begin = (size_t*)calloc(nc, sizeof(size_t));
	size_t *per_cluster_cursor = (size_t*)calloc(nc, sizeof(size_t));
	if (!per_cluster_count || !per_cluster_begin || !per_cluster_cursor) {
		free(per_cluster_count); free(per_cluster_begin); free(per_cluster_cursor);
		return 0;
	}

	// Fill the per-vertex tables. Zero-influence vertices keep their original
	// weight entries verbatim; the rest get the fitted, descending-sorted weights.
	uint32_t offset = 0;
	for (size_t v = 0; v < nv; v++) {
		ufbx_skin_vertex old = skin->vertices.data[v];
		new_vertices[v].dq_weight = old.dq_weight;
		new_vertices[v].weight_begin = offset;

		size_t cnt;
		if (vertices[v].is_zero) {
			cnt = (size_t)old.num_weights;
			for (uint32_t j = 0; j < old.num_weights; j++) {
				new_weights[offset + j] = skin->weights.data[old.weight_begin + j];
			}
		} else {
			cnt = vertices[v].num_weights;
			uint32_t tmp_cluster[UFBX_SKIN_FIT_MAX_INFLUENCES];
			ufbx_real tmp_weight[UFBX_SKIN_FIT_MAX_INFLUENCES];
			for (size_t j = 0; j < cnt; j++) {
				tmp_cluster[j] = vertices[v].cluster[j];
				tmp_weight[j] = (ufbx_real)vertices[v].weight[j];
			}
			fit_sort_vertex_weights(tmp_cluster, tmp_weight, cnt);
			for (size_t j = 0; j < cnt; j++) {
				new_weights[offset + j].cluster_index = tmp_cluster[j];
				new_weights[offset + j].weight = tmp_weight[j];
			}
		}

		new_vertices[v].num_weights = (uint32_t)cnt;
		for (size_t j = 0; j < cnt; j++) {
			per_cluster_count[new_weights[offset + j].cluster_index]++;
		}
		offset += (uint32_t)cnt;
	}

	// Prefix offsets for contiguous per-cluster reverse tables.
	{
		size_t acc = 0;
		for (size_t c = 0; c < nc; c++) {
			per_cluster_begin[c] = acc;
			per_cluster_cursor[c] = acc;
			acc += per_cluster_count[c];
		}
	}

	// Rebuild every cluster reverse table, keeping the cluster order intact.
	for (size_t v = 0; v < nv; v++) {
		ufbx_skin_vertex sv = new_vertices[v];
		for (uint32_t j = 0; j < sv.num_weights; j++) {
			ufbx_skin_weight sw = new_weights[sv.weight_begin + j];
			size_t at = per_cluster_cursor[sw.cluster_index]++;
			cluster_vertices[at] = (uint32_t)v;
			cluster_weights[at] = sw.weight;
		}
	}
	for (size_t c = 0; c < nc; c++) {
		ufbx_skin_cluster *cluster = skin->clusters.data[c];
		cluster->num_weights = per_cluster_count[c];
		cluster->vertices.data = cluster_vertices + per_cluster_begin[c];
		cluster->vertices.count = per_cluster_count[c];
		cluster->weights.data = cluster_weights + per_cluster_begin[c];
		cluster->weights.count = per_cluster_count[c];
	}
	free(per_cluster_count);
	free(per_cluster_begin);
	free(per_cluster_cursor);

	skin->vertices.data = new_vertices;
	skin->vertices.count = nv;
	skin->weights.data = new_weights;
	skin->weights.count = total;
	skin->max_weights_per_vertex = max_per_vertex;
	// The report counts positive influences only; zero-influence vertices and
	// any retained zero-weight entries do not contribute.
	if (out_total_weights) *out_total_weights = positive_total;

	// Recompute the time-zero skinned positions with the new weights so the
	// returned scene is consistent with its own tables.
	ufbx_vec3 *positions = (ufbx_vec3*)malloc(nv * sizeof(ufbx_vec3));
	ufbx_topo_edge *topo = (ufbx_topo_edge*)malloc(mesh->num_indices * sizeof(ufbx_topo_edge));
	uint32_t *normal_indices = (uint32_t*)malloc((mesh->num_indices ? mesh->num_indices : 1) * sizeof(uint32_t));
	if (!positions || !topo || !normal_indices) {
		free(positions); free(topo); free(normal_indices);
		return 0;
	}

	memcpy(positions, mesh->vertices.data, nv * sizeof(ufbx_vec3));
	for (size_t b = 0; b < mesh->blend_deformers.count; b++) {
		ufbx_add_blend_vertex_offsets(mesh->blend_deformers.data[b], positions, nv, (ufbx_real)1.0);
	}
	for (size_t v = 0; v < nv; v++) {
		ufbx_matrix mat = ufbx_get_skin_vertex_matrix(skin, v,
			mesh->instances.count > 0 ? &mesh->instances.data[0]->geometry_to_world : NULL);
		positions[v] = ufbx_transform_position(&mat, positions[v]);
	}

	ufbx_vec3 *skinned = (ufbx_vec3*)ufbxi_skin_fit_scene_push(result, sizeof(ufbx_vec3), nv);
	if (!skinned) { free(positions); free(topo); free(normal_indices); return 0; }
	memcpy(skinned, positions, nv * sizeof(ufbx_vec3));
	mesh->skinned_position.values.data = skinned;
	mesh->skinned_position.values.count = nv;
	mesh->skinned_is_local = false;

	// Regenerate normals exactly like the library's own skinning evaluation.
	if (mesh->num_indices > 0) {
		ufbx_compute_topology(mesh, topo, mesh->num_indices);
		size_t num_normals = ufbx_generate_normal_mapping(mesh, topo, mesh->num_indices,
			normal_indices, mesh->num_indices, false);
		ufbx_vec3 *normals = (ufbx_vec3*)ufbxi_skin_fit_scene_push(result, sizeof(ufbx_vec3), num_normals);
		uint32_t *stored_indices = (uint32_t*)ufbxi_skin_fit_scene_push(result, sizeof(uint32_t), mesh->num_indices);
		if (!normals || !stored_indices) { free(positions); free(topo); free(normal_indices); return 0; }
		ufbx_compute_normals(mesh, &mesh->skinned_position, normal_indices, mesh->num_indices, normals, num_normals);
		memcpy(stored_indices, normal_indices, mesh->num_indices * sizeof(uint32_t));
		mesh->skinned_normal.exists = true;
		mesh->skinned_normal.values.data = normals;
		mesh->skinned_normal.values.count = num_normals;
		mesh->skinned_normal.indices.data = stored_indices;
		mesh->skinned_normal.indices.count = mesh->num_indices;
		mesh->skinned_normal.value_reals = 3;
		mesh->skinned_normal.unique_per_vertex = (num_normals == nv);
		mesh->generated_normals = true;
	}

	free(positions);
	free(topo);
	free(normal_indices);
	return 1;
}

ufbx_abi ufbx_scene *ufbx_fit_skin_weights(
	const ufbx_scene *scene, const ufbx_anim *anim, size_t mesh_typed_id,
	const ufbx_skin_weight_fit_opts *opts,
	ufbx_skin_weight_fit_report *report, ufbx_error *error)
{
	if (!scene || !opts) {
		fit_set_error(error, "Invalid arguments");
		return NULL;
	}
	if (opts->_begin_zero | opts->_end_zero) {
		fit_set_error(error, "Uninitialized options");
		return NULL;
	}
	if (!opts->sample_times || opts->num_samples < 1 || opts->num_samples > UFBX_SKIN_FIT_MAX_SAMPLES) {
		fit_set_error(error, "sample_times must contain 1 to 64 samples");
		return NULL;
	}
	for (size_t i = 0; i < opts->num_samples; i++) {
		if (!isfinite(opts->sample_times[i])) {
			fit_set_error(error, "sample_times must be finite");
			return NULL;
		}
		if (i > 0 && !(opts->sample_times[i] > opts->sample_times[i - 1])) {
			fit_set_error(error, "sample_times must be strictly increasing");
			return NULL;
		}
	}
	if (opts->max_weights < 1 || opts->max_weights > UFBX_SKIN_FIT_MAX_WEIGHTS) {
		fit_set_error(error, "max_weights must be between 1 and 4");
		return NULL;
	}
	if (!isfinite(opts->max_error) || opts->max_error < 0.0) {
		fit_set_error(error, "max_error must be finite and non-negative");
		return NULL;
	}

	// Copy the sample times so the caller's array does not need to outlive us.
	double *sample_times = (double*)malloc(opts->num_samples * sizeof(double));
	if (!sample_times) {
		fit_set_error(error, "Out of memory");
		return NULL;
	}
	memcpy(sample_times, opts->sample_times, opts->num_samples * sizeof(double));

	const ufbx_anim *use_anim = anim ? anim : scene->anim;

	const ufbx_mesh *mesh = NULL;
	const ufbx_skin_deformer *skin = NULL;
	fit_vertex *vertices = NULL;
	if (!fit_validate_and_prepare(scene, mesh_typed_id, opts, &vertices, &mesh, &skin)) {
		free(sample_times);
		fit_set_error(error, "Unsupported or invalid mesh/skin configuration");
		return NULL;
	}

	fit_sample_data data;
	if (!fit_collect_samples(scene, use_anim, mesh, opts, sample_times, &data)) {
		free(vertices);
		free(sample_times);
		fit_set_error(error, "Failed to evaluate animation samples");
		return NULL;
	}

	// Solve every vertex that needs fitting.
	for (size_t v = 0; v < mesh->num_vertices; v++) {
		if (vertices[v].is_zero || vertices[v].is_simple) continue;
		if (!fit_solve_vertex(&data, &vertices[v], v, opts->max_weights)) {
			fit_free_samples(&data);
			free(vertices);
			free(sample_times);
			fit_set_error(error, "Failed to solve skin weights");
			return NULL;
		}
	}

	// Build the time-zero result scene only after all fitting has succeeded.
	ufbx_evaluate_opts eval_opts = { 0 };
	eval_opts.evaluate_skinning = true;
	ufbx_scene *result = ufbx_evaluate_scene(scene, use_anim, 0.0, &eval_opts, error);
	if (!result) {
		fit_free_samples(&data);
		free(vertices);
		free(sample_times);
		return NULL;
	}

	size_t total_weights = 0;
	if (!fit_install(result, mesh->typed_id, vertices, &total_weights)) {
		ufbx_free_scene(result);
		fit_free_samples(&data);
		free(vertices);
		free(sample_times);
		fit_set_error(error, "Out of memory");
		return NULL;
	}

	// Recompute the error from the final stored ufbx_real tables at every sample.
	ufbx_skin_weight_fit_report local_report;
	if (!fit_recompute_error(result, mesh->typed_id, &data, vertices, sample_times, &local_report)) {
		ufbx_free_scene(result);
		fit_free_samples(&data);
		free(vertices);
		free(sample_times);
		fit_set_error(error, "Failed to recompute fitting error");
		return NULL;
	}
	local_report.total_weights = total_weights;
	double worst = local_report.max_error;

	if (worst > opts->max_error) {
		ufbx_free_scene(result);
		fit_free_samples(&data);
		free(vertices);
		free(sample_times);
		fit_set_error(error, "Optimal fit exceeds max_error");
		return NULL;
	}

	if (report) *report = local_report;

	fit_free_samples(&data);
	free(vertices);
	free(sample_times);
	return result;
}
