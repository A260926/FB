/* Task tests for the fused ball field groups (metaballs) of c-ray-fast.
 * Registered through task_support/check.h in three groups: roots,
 * acceleration, render. No main() here - task_support/driver.c drives it.
 *
 * Every geometric expectation is cross-checked against an independently
 * written reference field/gradient (plus finite differences) and analytic
 * single-kernel solutions, never against the production code itself.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c-ray-fast/scene.h"
#include "c-ray-fast/metaball.h"
#include "task_support/check.h"

/* ---- independent reference implementation, written separately from the
 * production code, used to derive expected values and normals. ---- */
static double ref_field(const struct metaball *mb, double x, double y, double z)
{
	int i;
	double f = 0.0;
	for(i=0; i<mb->num_comp; i++) {
		const struct mb_component *cp = mb->comp + i;
		double dx = x - cp->c.x, dy = y - cp->c.y, dz = z - cp->c.z;
		double d2 = dx*dx + dy*dy + dz*dz, r2 = cp->r * cp->r;
		double u;
		if(d2 >= r2) continue;
		u = 1.0 - d2 / r2;
		f += cp->w * u * u;
	}
	return f;
}

static void ref_grad(const struct metaball *mb, double x, double y, double z, double g[3])
{
	int i;
	g[0] = g[1] = g[2] = 0.0;
	for(i=0; i<mb->num_comp; i++) {
		const struct mb_component *cp = mb->comp + i;
		double dx = x - cp->c.x, dy = y - cp->c.y, dz = z - cp->c.z;
		double d2 = dx*dx + dy*dy + dz*dz, r2 = cp->r * cp->r;
		double u, k;
		if(d2 >= r2) continue;
		u = 1.0 - d2 / r2;
		k = -4.0 * cp->w * u / r2;
		g[0] += k * dx; g[1] += k * dy; g[2] += k * dz;
	}
}

static void hit_point(const struct ray *r, double t, double p[3])
{
	p[0] = r->orig.x + r->dir.x * t;
	p[1] = r->orig.y + r->dir.y * t;
	p[2] = r->orig.z + r->dir.z * t;
}

static void reset_scene(void)
{
	objects = 0; infobj = 0; materials = 0; lights = 0; metaballs = 0;
	num_obj = num_inf = num_mtl = num_lt = num_mb = 0;
	octroot = 0;
}

static struct material make_mtl(double r, double g, double b, double refl, double refr, double ior)
{
	struct material m = def_mtl;
	m.kd.r = (float)r; m.kd.g = (float)g; m.kd.b = (float)b;
	m.ks.r = m.ks.g = m.ks.b = 0.0f;
	m.spow = 0.0;
	m.refl = refl;
	m.refr = refr;
	m.ior = ior;
	return m;
}

/* dense numeric scan along a ray, used only to corroborate the analytic root.
 * One million steps give a ~1e-6 parametric grid; agreement within a few grid
 * cells confirms the analytic finder, while exact precision is checked
 * separately against analytic solutions. */
static double scan_first(const struct metaball *mb, const struct ray *r)
{
	int k;
	double prev = ref_field(mb, r->orig.x, r->orig.y, r->orig.z) - mb->tau;
	for(k=1; k<=1000000; k++) {
		double t = ERR_MARGIN + (1.0 - ERR_MARGIN) * k / 1000000.0;
		double p[3], cur;
		hit_point(r, t, p);
		cur = ref_field(mb, p[0], p[1], p[2]) - mb->tau;
		if(cur == 0.0 || ((prev < 0.0) != (cur < 0.0))) return t;
		prev = cur;
	}
	return -1.0;
}

/* ============================ roots group ============================ */

static int case_single_kernel(void)
{
	/* one off-center kernel, non-unit skewed ray; exact analytic solution */
	struct mb_component comp = {{1.0, -2.0, 0.5}, 1.5, 2.0};
	struct metaball mb = {0};
	struct ray r = {{-3.0, 1.0, -2.0}, {6.0, -4.0, 5.0}};
	struct spoint sp;
	double p[3], g[3], glen, nref[3], nerr, dlen, texp;

	mb.tau = 0.7;
	mb.comp = &comp;
	mb.num_comp = 1;

	TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);

	/* analytic: u(t) quadratic; field 2 u^2 = tau on entry, u = +sqrt(tau/2) */
	{
		double ex = r.orig.x - comp.c.x, ey = r.orig.y - comp.c.y, ez = r.orig.z - comp.c.z;
		double r2 = comp.r * comp.r;
		double A = -(r.dir.x*r.dir.x + r.dir.y*r.dir.y + r.dir.z*r.dir.z) / r2;
		double B = -2.0 * (ex*r.dir.x + ey*r.dir.y + ez*r.dir.z) / r2;
		double C = 1.0 - (ex*ex + ey*ey + ez*ez) / r2;
		double target = sqrt(mb.tau / comp.w);
		double disc = B*B - 4.0*A*(C - target);
		double t1 = (-B - sqrt(disc)) / (2.0 * A);
		double t2 = (-B + sqrt(disc)) / (2.0 * A);
		texp = DBL_MAX;
		if(t1 >= ERR_MARGIN && t1 <= 1.0 && t1 < texp) texp = t1;
		if(t2 >= ERR_MARGIN && t2 <= 1.0 && t2 < texp) texp = t2;
	}
	TASK_CHECK(fabs(sp.dist - texp) < 1e-10);
	TASK_CHECK(sp.dist >= ERR_MARGIN && sp.dist <= 1.0);

	hit_point(&r, sp.dist, p);
	TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
	TASK_CHECK(fabs(mb_field(&mb, &(struct vec3){p[0], p[1], p[2]}) - mb.tau) < 1e-10);

	/* outward normal = -grad F / |grad F|, against the independent reference */
	ref_grad(&mb, p[0], p[1], p[2], g);
	glen = sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
	nref[0] = -g[0]/glen; nref[1] = -g[1]/glen; nref[2] = -g[2]/glen;
	nerr = sqrt(SQ(sp.normal.x-nref[0]) + SQ(sp.normal.y-nref[1]) + SQ(sp.normal.z-nref[2]));
	TASK_CHECK(nerr < 1e-12);
	TASK_CHECK(fabs(sqrt(DOT(sp.normal, sp.normal)) - 1.0) < 1e-14);
	/* entering surface: outward normal opposes travel direction */
	TASK_CHECK(DOT(sp.normal, r.dir) < 0.0);

	/* D keeps its original length */
	dlen = sqrt(DOT(r.dir, r.dir));
	TASK_CHECK(fabs(dlen - sqrt(77.0)) < 1e-12);
	return 0;
}

static int case_field_gradient_values(void)
{
	struct mb_component comp[2] = {
		{{0.3, -0.7, 1.2}, 2.0, 1.5},
		{{2.0, 2.0, 2.0}, 0.5, -0.8}	/* point below sits outside this support */
	};
	struct metaball mb = {0};
	struct vec3 p = {0.7, -0.3, 1.1}, grad, fd;
	double h = 1e-7, fprod, fref;

	mb.tau = 0.4;
	mb.comp = comp;
	mb.num_comp = 2;

	fprod = mb_field(&mb, &p);
	fref = ref_field(&mb, p.x, p.y, p.z);
	TASK_CHECK(fabs(fprod - fref) < 1e-12);

	/* hand value for the single active kernel */
	{
		double dx = p.x-comp[0].c.x, dy = p.y-comp[0].c.y, dz = p.z-comp[0].c.z;
		double u = 1.0 - (dx*dx+dy*dy+dz*dz)/(comp[0].r*comp[0].r);
		TASK_CHECK(fabs(fprod - comp[0].w*u*u) < 1e-12);
	}

	/* outside every support: field exactly zero */
	{
		struct vec3 far = {100.0, 100.0, 100.0};
		TASK_CHECK(mb_field(&mb, &far) == 0.0);
		mb_gradient(&mb, &far, &grad);
		TASK_CHECK(grad.x == 0.0 && grad.y == 0.0 && grad.z == 0.0);
	}

	/* analytic gradient vs independent reference vs central finite differences */
	mb_gradient(&mb, &p, &grad);
	{
		double g[3];
		ref_grad(&mb, p.x, p.y, p.z, g);
		TASK_CHECK(fabs(grad.x-g[0]) < 1e-12 && fabs(grad.y-g[1]) < 1e-12 &&
				fabs(grad.z-g[2]) < 1e-12);
	}
	fd.x = (ref_field(&mb, p.x+h, p.y, p.z) - ref_field(&mb, p.x-h, p.y, p.z)) / (2*h);
	fd.y = (ref_field(&mb, p.x, p.y+h, p.z) - ref_field(&mb, p.x, p.y-h, p.z)) / (2*h);
	fd.z = (ref_field(&mb, p.x, p.y, p.z+h) - ref_field(&mb, p.x, p.y, p.z-h)) / (2*h);
	TASK_CHECK(fabs(grad.x-fd.x) < 1e-8 && fabs(grad.y-fd.y) < 1e-8 &&
			fabs(grad.z-fd.z) < 1e-8);
	return 0;
}

static int case_overlap_summed_first(void)
{
	/* two symmetric overlapping kernels, each too weak to reach tau alone
	 * but their sum on the axis does; exact analytic solution on the z axis */
	struct mb_component comp[2] = {{{-1,0,0}, 2.0, 0.4}, {{1,0,0}, 2.0, 0.4}};
	struct metaball mb = {0};
	struct ray r = {{0,0,-6}, {0,0,12}};
	struct spoint sp;
	double p[3], texp;

	mb.tau = 0.3;
	mb.comp = comp;
	mb.num_comp = 2;

	/* d^2 = 1 + z^2, u = 0.75 - z^2/4, F = 2 w u^2 = tau -> u = sqrt(tau/2w) */
	{
		double u = sqrt(mb.tau / (2.0 * comp[0].w));
		double z = sqrt(4.0 * (0.75 - u));
		texp = (6.0 - z) / 12.0;
	}

	TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);
	TASK_CHECK(fabs(sp.dist - texp) < 1e-11);
	hit_point(&r, sp.dist, p);
	TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
	TASK_CHECK(fabs(sp.normal.z + 1.0) < 1e-12);

	/* each kernel alone never reaches tau anywhere on the ray: the surface
	 * exists only because the fields are summed before thresholding. */
	{
		struct metaball one = {0};
		one.tau = mb.tau;
		one.comp = comp;
		one.num_comp = 1;
		TASK_CHECK(ray_metaball(&one, &r, 0) == 0);
		/* and its peak field along the axis is provably below tau */
		TASK_CHECK(comp[0].w * 0.75 * 0.75 < mb.tau);
	}
	return 0;
}

static int case_tangent(void)
{
	/* ray grazing the isosurface: even-multiplicity root, no sign change */
	struct mb_component comp = {{0,0,0}, 2.0, 1.0};
	struct metaball mb = {0};
	struct ray r = {{-5, 0, sqrt(2.0)}, {10, 0, 0}};
	struct spoint sp;
	double p[3], g[3];

	mb.tau = 0.25;
	mb.comp = &comp;
	mb.num_comp = 1;

	TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);
	TASK_CHECK(fabs(sp.dist - 0.5) < 1e-10);
	hit_point(&r, sp.dist, p);
	TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
	ref_grad(&mb, p[0], p[1], p[2], g);
	TASK_CHECK(fabs(sp.normal.z - (-g[2]/sqrt(g[0]*g[0]+g[1]*g[1]+g[2]*g[2]))) < 1e-9);
	/* field stays <= tau on both sides of the contact */
	{
		double pa[3], pb[3];
		hit_point(&r, sp.dist - 1e-4, pa);
		hit_point(&r, sp.dist + 1e-4, pb);
		TASK_CHECK(ref_field(&mb, pa[0], pa[1], pa[2]) <= mb.tau + 1e-12);
		TASK_CHECK(ref_field(&mb, pb[0], pb[1], pb[2]) <= mb.tau + 1e-12);
	}
	return 0;
}

static int case_internal_start(void)
{
	/* origin inside the entity: the earliest valid root is the exit. */
	struct mb_component comp = {{0,0,0}, 2.0, 1.0};
	struct metaball mb = {0};
	struct ray r = {{0,0,0}, {0,0,8}};
	struct spoint sp;

	mb.tau = 0.25;
	mb.comp = &comp;
	mb.num_comp = 1;

	TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);
	TASK_CHECK(fabs(sp.dist - sqrt(2.0)/8.0) < 1e-11);
	TASK_CHECK(DOT(sp.normal, r.dir) > 0.0);	/* exit: normal along travel */

	/* origin deeper inside, skewed direction, corroborated by dense scan */
	{
		struct ray r2 = {{0.2, -0.3, 0.1}, {3.0, 5.0, 4.0}};
		struct spoint sp2;
		double ts;
		TASK_CHECK(ray_metaball(&mb, &r2, &sp2) == 1);
		ts = scan_first(&mb, &r2);
		TASK_CHECK(ts > 0.0 && fabs(sp2.dist - ts) < 1e-5);
	}
	return 0;
}

static int case_negative_notch_layers(void)
{
	/* co-centered positive kernel (R=2,w=1) and negative kernel (R=1,w=-0.5):
	 * along z, F(z) = (1-z^2/4)_+^2 - 0.5 (1-z^2)_+^2. With tau just above
	 * 0.5 the center is carved out and two independent inner surface layers
	 * sit exactly 1e-5 apart (in world z), plus the outer pair where only
	 * the positive kernel contributes. */
	struct mb_component comp[2] = {{{0,0,0}, 2.0, 1.0}, {{0,0,0}, 1.0, -0.5}};
	struct metaball mb = {0};
	struct spoint sp;
	const double tau = 0.5 + 1.25e-11;
	double z_inner = 5.0e-6, z_outer, p[3];

	mb.tau = tau;
	mb.comp = comp;
	mb.num_comp = 2;

	/* analytic outer root: |z| in (1,2) where only the negative kernel acts */
	z_outer = 2.0 * sqrt(1.0 - sqrt(tau));

	/* ray from outside: outer entry first */
	{
		struct ray rout = {{0,0,-3}, {0,0,6}};
		TASK_CHECK(ray_metaball(&mb, &rout, &sp) == 1);
		hit_point(&rout, sp.dist, p);
		TASK_CHECK(fabs(p[2] + z_outer) < 1e-9);
		TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - tau) < 1e-11);
	}

	/* start in the carved center going +z: inner layer at +5e-6 */
	{
		struct ray rin = {{0,0,0}, {0,0,3}};
		TASK_CHECK(ray_metaball(&mb, &rin, &sp) == 1);
		TASK_CHECK(sp.dist > ERR_MARGIN);
		hit_point(&rin, sp.dist, p);
		TASK_CHECK(fabs(p[2] - z_inner) < 1e-10);
	}

	/* same center going -z: inner layer at -5e-6; the two layers are
	 * distinct roots 1e-5 apart and must not be merged. */
	{
		struct ray rneg = {{0,0,0}, {0,0,-3}};
		struct spoint spn;
		double pp[3];
		TASK_CHECK(ray_metaball(&mb, &rneg, &spn) == 1);
		hit_point(&rneg, spn.dist, pp);
		TASK_CHECK(fabs(pp[2] + z_inner) < 1e-10);
		TASK_CHECK(fabs(spn.dist - sp.dist) < 1e-10);	/* symmetric t */
		TASK_CHECK(fabs(pp[2] - p[2]) > 9.0e-6);		/* layers separated */
	}

	/* field topology: below tau in the core, above tau between layers */
	TASK_CHECK(ref_field(&mb, 0, 0, 0) < tau);
	TASK_CHECK(ref_field(&mb, 0, 0, 1e-4) > tau);
	TASK_CHECK(ref_field(&mb, 0, 0, 0.5) > tau);
	TASK_CHECK(ref_field(&mb, 0, 0, 2.5) < tau);

	/* enumerate every root along the full line by restarting past each hit */
	{
		struct ray walk = {{0,0,-3}, {0,0,6}};
		double bases[4], base = 0.0;
		int k, count = 0;
		for(k=0; k<6; k++) {
			struct spoint s;
			if(!ray_metaball(&mb, &walk, &s)) break;
			bases[count++] = base + s.dist;
			base += s.dist + 1e-9;
			walk.orig.z = -3.0 + 6.0 * base;
		}
		TASK_CHECK(count == 4);
		if(count == 4) {
			double zs[4], j;
			for(k=0; k<4; k++) zs[k] = -3.0 + 6.0 * bases[k];
			TASK_CHECK(fabs(zs[0] + z_outer) < 1e-8);
			TASK_CHECK(fabs(zs[1] + z_inner) < 1e-9);
			TASK_CHECK(fabs(zs[2] - z_inner) < 1e-9);
			TASK_CHECK(fabs(zs[3] - z_outer) < 1e-8);
			for(j=0; j<4; j++) {
				double pp[3] = {0,0,zs[(int)j]};
				TASK_CHECK(fabs(ref_field(&mb, pp[0], pp[1], pp[2]) - tau) < 1e-10);
			}
		}
	}
	return 0;
}

static int case_support_event_switch(void)
{
	/* small kernel B only contributes after the ray enters its support;
	 * neither kernel alone reaches tau along this line, the sum does. */
	struct mb_component comp[2] = {{{0,0,0}, 2.0, 0.6}, {{1.5,0,0}, 0.8, 0.6}};
	struct metaball mb = {0};
	struct ray r = {{1.5, 0, -4}, {0,0,8}};
	struct spoint sp;
	double p[3], ts;

	mb.tau = 0.7;
	mb.comp = comp;
	mb.num_comp = 2;

	TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);
	hit_point(&r, sp.dist, p);
	TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
	/* hit must lie inside B's support, where both kernels are active */
	TASK_CHECK(fabs(p[2]) < 0.8);
	{
		double da = p[0]*p[0] + p[1]*p[1] + p[2]*p[2];
		double db = (p[0]-1.5)*(p[0]-1.5) + p[1]*p[1] + p[2]*p[2];
		TASK_CHECK(da < 4.0 && db < 0.64);
	}
	ts = scan_first(&mb, &r);
	TASK_CHECK(ts > 0.0 && fabs(sp.dist - ts) < 3e-6);

	/* same line with B removed: no surface at all */
	{
		struct metaball one = {0};
		one.tau = mb.tau;
		one.comp = comp;
		one.num_comp = 1;
		TASK_CHECK(ray_metaball(&one, &r, 0) == 0);
	}
	return 0;
}

static int case_range_clamping(void)
{
	struct mb_component comp = {{0,0,0}, 2.0, 1.0};
	struct metaball mb = {0};
	struct spoint sp;

	mb.tau = 0.25;
	mb.comp = &comp;
	mb.num_comp = 1;

	/* root before ERR_MARGIN is rejected */
	{
		struct ray r = {{0,0,-1.41421356237}, {0,0,1e-7}};
		TASK_CHECK(ray_metaball(&mb, &r, 0) == 0);
	}
	/* root after t=1 is rejected */
	{
		struct ray r = {{0,0,-5}, {0,0,3.0}};
		TASK_CHECK(ray_metaball(&mb, &r, 0) == 0);
	}
	/* a root exactly reachable inside the range hits */
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		TASK_CHECK(ray_metaball(&mb, &r, &sp) == 1);
		TASK_CHECK(sp.dist >= ERR_MARGIN && sp.dist <= 1.0);
	}
	return 0;
}

static int case_load_validation(void)
{
	struct metaball *mb;
	struct object *obj;
	FILE *fp;

	/* duplicate group name rejected */
	fp = tmpfile();
	TASK_CHECK(fp != 0);
	fputs("M \"m\" 1 1 1 0 0 0 0 0 1\n", fp);
	fputs("B \"g\" 0.5 \"m\"\nB \"g\" 0.25 \"m\"\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	/* non-positive tau rejected */
	fp = tmpfile();
	fputs("B \"g\" 0 \"m\"\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	/* undeclared material rejected */
	fp = tmpfile();
	fputs("B \"g\" 0.5 \"ghost\"\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	/* undeclared group in b rejected */
	fp = tmpfile();
	fputs("M \"m\" 1 1 1 0 0 0 0 0 1\n", fp);
	fputs("B \"g\" 0.5 \"m\"\nb \"nog\" 0 0 0 1 1\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	/* non-positive radius and zero/non-finite weight rejected */
	fp = tmpfile();
	fputs("M \"m\" 1 1 1 0 0 0 0 0 1\nB \"g\" 0.5 \"m\"\n", fp);
	fputs("b \"g\" 0 0 0 0 1\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	fp = tmpfile();
	fputs("M \"m\" 1 1 1 0 0 0 0 0 1\nB \"g\" 0.5 \"m\"\n", fp);
	fputs("b \"g\" 0 0 0 1 0\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == -1);
	fclose(fp);

	/* valid file with an empty group: accepted, no surface produced */
	fp = tmpfile();
	fputs("M \"m\" 0.7 0.7 0.7 0 0 0 0 0 1\n", fp);
	fputs("B \"empty\" 0.5 \"m\"\n", fp);
	fputs("s 0 0 0 1 \"m\"\n", fp);
	rewind(fp);
	reset_scene();
	TASK_CHECK(load_scene(fp) == 0);
	fclose(fp);
	TASK_CHECK(num_mb == 1 && num_obj == 2);
	mb = find_metaball("empty");
	TASK_CHECK(mb != 0 && mb->num_comp == 0);
	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		struct spoint sp;
		TASK_CHECK(ray_scene(&r, &sp) == 1);
		TASK_CHECK(sp.obj->type == OBJ_SPHERE);	/* empty group added nothing */
	}
	obj = objects;
	TASK_CHECK(obj[1].type == OBJ_METABALL && obj[1].o.mb == mb);
	return 0;
}

/* ======================== acceleration group ======================== */

static struct metaball *build_basic_scene(void)
{
	static struct material mtls[2];
	static struct metaball mbs[1];
	static struct mb_component comps[2];
	static struct object objs[3];

	reset_scene();
	materials = mtls; num_mtl = 2;
	mtls[0] = make_mtl(0.8, 0.4, 0.2, 0.0, 0.0, 1.0);
	mtls[0].name = "clay";
	mtls[1] = make_mtl(0.1, 0.1, 0.9, 0.0, 0.0, 1.0);
	mtls[1].name = "blue";

	metaballs = mbs; num_mb = 1;
	memset(mbs, 0, sizeof mbs);
	mbs[0].name = "g";
	mbs[0].tau = 0.5;
	mbs[0].mat = mtls[0];
	mbs[0].comp = comps;
	mbs[0].max_comp = 2;
	comps[0].c = (struct vec3){-1, 0, 0}; comps[0].r = 2.0; comps[0].w = 1.0;
	comps[1].c = (struct vec3){1, 0, 0}; comps[1].r = 2.0; comps[1].w = 1.0;
	mbs[0].num_comp = 2;

	objects = objs; num_obj = 0;
	objs[num_obj].type = OBJ_SPHERE;
	objs[num_obj].o.sph.pos = (struct vec3){0, 3, 0};
	objs[num_obj].o.sph.rad = 0.5;
	objs[num_obj].mat = mtls[1];
	num_obj++;
	objs[num_obj].type = OBJ_METABALL;
	objs[num_obj].o.mb = &mbs[0];
	objs[num_obj].mat = mbs[0].mat;
	num_obj++;

	return &mbs[0];
}

static int case_prepare_and_query(void)
{
	struct metaball *mb = build_basic_scene();
	struct ray r = {{0,0,-6}, {0,0,12}};
	struct spoint sp, direct;
	struct aabox bb;

	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(octroot != 0);

	/* conservative group bounds: union of the two R=2 support spheres */
	metaball_bounds(mb, &bb);
	TASK_CHECK(fabs(bb.vmin.x + 3.0) < 1e-12 && fabs(bb.vmax.x - 3.0) < 1e-12);
	TASK_CHECK(fabs(bb.vmin.y + 2.0) < 1e-12 && fabs(bb.vmax.y - 2.0) < 1e-12);
	TASK_CHECK(fabs(bb.vmin.z + 2.0) < 1e-12 && fabs(bb.vmax.z - 2.0) < 1e-12);
	TASK_CHECK(octroot->aabb.vmin.x <= bb.vmin.x && octroot->aabb.vmax.x >= bb.vmax.x);

	TASK_CHECK(ray_scene(&r, &sp) == 1);
	TASK_CHECK(sp.obj->type == OBJ_METABALL);
	TASK_CHECK(ray_metaball(mb, &r, &direct) == 1);
	TASK_CHECK(fabs(sp.dist - direct.dist) < 1e-14);
	TASK_CHECK(fabs(sp.normal.x - direct.normal.x) < 1e-14);

	/* occlusion query shares the same intersection */
	TASK_CHECK(ray_scene(&r, 0) == 1);
	{
		struct ray miss = {{20, 20, 20}, {1, 0, 0}};
		TASK_CHECK(ray_scene(&miss, 0) == 0);
		TASK_CHECK(ray_scene(&miss, &sp) == 0);
	}
	return 0;
}

static int case_reprepare_edits(void)
{
	struct metaball *mb = build_basic_scene();
	struct ray r = {{0,0,-6}, {0,0,12}};
	struct spoint sp_before, sp_after;
	struct aabox bb;

	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(ray_scene(&r, &sp_before) == 1);

	/* move a component, grow a radius, change a weight, then re-prepare */
	mb->comp[0].c.x = -4.0;
	mb->comp[0].r = 3.0;
	mb->comp[1].w = 1.7;
	TASK_CHECK(prepare_scene() == 0);	/* tears down and rebuilds the octree */

	metaball_bounds(mb, &bb);
	TASK_CHECK(fabs(bb.vmin.x + 7.0) < 1e-12);	/* -4 - 3 */
	TASK_CHECK(fabs(bb.vmax.x - 3.0) < 1e-12);
	TASK_CHECK(octroot->aabb.vmin.x <= -7.0);

	TASK_CHECK(ray_scene(&r, &sp_after) == 1);
	TASK_CHECK(fabs(sp_after.dist - sp_before.dist) > 1e-6);
	{
		double p[3];
		hit_point(&r, sp_after.dist, p);
		TASK_CHECK(fabs(ref_field(mb, p[0], p[1], p[2]) - mb->tau) < 1e-10);
	}

	/* a third prepare still gives consistent results (no leaked old nodes) */
	TASK_CHECK(prepare_scene() == 0);
	{
		struct spoint sp3;
		TASK_CHECK(ray_scene(&r, &sp3) == 1);
		TASK_CHECK(fabs(sp3.dist - sp_after.dist) < 1e-14);
	}
	return 0;
}

static int count_group_refs(struct octnode *node, struct metaball *mb, int *count)
{
	int i;
	if(node->obj) {
		for(i=0; i<node->num_obj; i++) {
			if(node->obj[i]->type == OBJ_METABALL && node->obj[i]->o.mb == mb) {
				(*count)++;
			}
		}
	}
	for(i=0; i<8; i++) {
		if(node->sub[i]) count_group_refs(node->sub[i], mb, count);
	}
	return *count;
}

static int case_split_plane_dedup(void)
{
	/* >64 finite objects force real subdivision; place a group straddling
	 * the root split plane so several leaves reference the same group. */
	static struct material mtl;
	static struct metaball mbs[1];
	static struct mb_component comps[2];
	static struct sphere sph_store[70];
	static struct object objs[71];
	int i, refs = 0;

	reset_scene();
	mtl = make_mtl(0.5, 0.5, 0.5, 0, 0, 1);
	materials = &mtl; num_mtl = 1;

	metaballs = mbs; num_mb = 1;
	memset(mbs, 0, sizeof mbs);
	mbs[0].name = "straddle";
	mbs[0].tau = 0.4;
	mbs[0].mat = mtl;
	mbs[0].comp = comps;
	comps[0].c = (struct vec3){-0.5, 0, 0}; comps[0].r = 1.5; comps[0].w = 1.0;
	comps[1].c = (struct vec3){0.5, 0, 0}; comps[1].r = 1.5; comps[1].w = 1.0;
	mbs[0].num_comp = 2;

	objects = objs; num_obj = 0;
	for(i=0; i<70; i++) {
		objs[num_obj].type = OBJ_SPHERE;
		sph_store[i].pos = (struct vec3){-34.5 + i, 10.0, 0};
		sph_store[i].rad = 0.25;
		objs[num_obj].o.sph = sph_store[i];
		objs[num_obj].mat = mtl;
		num_obj++;
	}
	objs[num_obj].type = OBJ_METABALL;
	objs[num_obj].o.mb = &mbs[0];
	objs[num_obj].mat = mtl;
	num_obj++;

	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(octroot->obj == 0);	/* root was subdivided */
	count_group_refs(octroot, &mbs[0], &refs);
	TASK_CHECK(refs >= 2);			/* duplicated across leaves ... */

	{
		struct ray r = {{0,0,-6}, {0,0,12}};
		struct spoint via_tree, direct;
		TASK_CHECK(ray_scene(&r, &via_tree) == 1);
		TASK_CHECK(via_tree.obj->type == OBJ_METABALL);
		TASK_CHECK(ray_metaball(&mbs[0], &r, &direct) == 1);
		/* repeated leaf references must neither drop nor double-mix the group */
		TASK_CHECK(fabs(via_tree.dist - direct.dist) < 1e-14);
		TASK_CHECK(ray_scene(&r, 0) == ray_metaball(&mbs[0], &r, 0));
	}

	/* a ray crossing several leaves and missing the group only hits spheres */
	{
		struct ray r = {{0, 0, 20}, {0, 0, 8}};
		TASK_CHECK(ray_scene(&r, 0) == 0);
	}
	return 0;
}

static int case_reentrant_queries(void)
{
	/* group larger than the per-call stack capacity exercises the heap path */
	static struct mb_component comps[40];
	static struct metaball mb;
	struct ray ra = {{-3, 0.5, -4}, {7, -4, 9}};
	/* rb runs straight through the center of positive kernel comps[1]
	 * at (-2,-2,-1), reached at t=0.8, where the field equals its weight
	 * 0.7 (no neighboring support reaches that center). */
	struct ray rb = {{4.4, -2, -1}, {-8, 0, 0}};
	struct spoint sa1, sa2, sb1, sb2;
	int i;

	reset_scene();
	memset(&mb, 0, sizeof mb);
	mb.tau = 0.3;
	mb.comp = comps;
	mb.num_comp = 40;
	for(i=0; i<40; i++) {
		comps[i].c = (struct vec3){(i % 7) - 3.0, (i / 7) - 2.0, (i % 5) - 2.0};
		comps[i].r = 1.0 + 0.05 * (i % 3);
		comps[i].w = (i & 1) ? 0.7 : -0.35;
	}

	TASK_CHECK(ray_metaball(&mb, &ra, &sa1) == 1);
	TASK_CHECK(ray_metaball(&mb, &rb, &sb1) == 1);
	/* interleave the same queries: per-call state must not leak between them */
	TASK_CHECK(ray_metaball(&mb, &rb, &sb2) == 1);
	TASK_CHECK(ray_metaball(&mb, &ra, &sa2) == 1);
	TASK_CHECK(fabs(sa1.dist - sa2.dist) < 1e-15);
	TASK_CHECK(fabs(sb1.dist - sb2.dist) < 1e-15);
	{
		double p[3];
		hit_point(&ra, sa2.dist, p);
		TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
		hit_point(&rb, sb2.dist, p);
		TASK_CHECK(fabs(ref_field(&mb, p[0], p[1], p[2]) - mb.tau) < 1e-10);
	}
	return 0;
}

static int case_finite_shadow_line(void)
{
	struct spoint sp;

	build_basic_scene();
	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-6}, {0,0,12}};
		TASK_CHECK(ray_scene(&r, &sp) == 1);
	}

	/* shadow ray from the surface toward +z passes through the group body */
	{
		struct ray shadow = {sp.pos, {0, 0, 5}};
		TASK_CHECK(ray_scene(&shadow, 0) == 1);
	}
	/* shadow ray toward -z leaves the entity immediately and sees clear space */
	{
		struct ray clear = {sp.pos, {0, 0, -5}};
		TASK_CHECK(ray_scene(&clear, 0) == 0);
	}
	/* short ray that stops before the surface reports no occlusion */
	{
		struct ray shortr = {{0, 0, -6}, {0, 0, 4.0}};	/* ends at z=-2 */
		TASK_CHECK(ray_scene(&shortr, 0) == 0);
	}
	return 0;
}

/* ============================ render group ============================ */

static struct light one_light(double x, double y, double z)
{
	struct light lt;
	lt.pos = (struct vec3){x, y, z};
	lt.col = (struct color){1, 1, 1, 1};
	return lt;
}

static int case_trace_diffuse(void)
{
	static struct material mtl;
	static struct metaball mb;
	static struct mb_component comp;
	static struct object obj;
	static struct light lt;

	reset_scene();
	mtl = make_mtl(0.8, 0.4, 0.2, 0, 0, 1);
	materials = &mtl; num_mtl = 1;
	mb.tau = 0.25;
	mb.mat = mtl;
	mb.comp = &comp;
	comp.c = (struct vec3){0,0,0}; comp.r = 2.0; comp.w = 1.0;
	mb.num_comp = 1;
	metaballs = &mb; num_mb = 1;

	objects = &obj; num_obj = 1;
	obj.type = OBJ_METABALL;
	obj.o.mb = &mb;
	obj.mat = mtl;

	ambcol = (struct color){0.01f, 0.01f, 0.01f, 0};
	bgcol = (struct color){0, 0, 0, 0};
	lights = &lt; num_lt = 1;
	lt = one_light(0, 0, -10);

	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		struct color col;
		trace(&r, 0, &col);
		/* front-lit: ambient (0.01 * kd) plus full diffuse (1.0 * kd),
		 * and the outward shadow ray reaches the light unobstructed. */
		TASK_CHECK(fabs(col.r - (0.01f * 0.8f + 0.8f)) < 1e-5);
		TASK_CHECK(fabs(col.g - (0.01f * 0.4f + 0.4f)) < 1e-5);
		TASK_CHECK(fabs(col.b - (0.01f * 0.2f + 0.2f)) < 1e-5);
		TASK_CHECK(col.a == 1.0f);
	}
	return 0;
}

static int case_trace_shadow(void)
{
	static struct material mtls[2];
	static struct metaball mb;
	static struct mb_component comp;
	static struct object objs[2];
	static struct sphere blocker;
	static struct light lt;

	reset_scene();
	mtls[0] = make_mtl(0.8, 0.4, 0.2, 0, 0, 1);
	mtls[1] = make_mtl(0.2, 0.2, 0.2, 0, 0, 1);
	materials = mtls; num_mtl = 2;

	mb.tau = 0.25; mb.mat = mtls[0];
	mb.comp = &comp;
	comp.c = (struct vec3){0,0,0}; comp.r = 2.0; comp.w = 1.0;
	mb.num_comp = 1;
	metaballs = &mb; num_mb = 1;

	objects = objs; num_obj = 0;
	objs[num_obj].type = OBJ_METABALL;
	objs[num_obj].o.mb = &mb;
	objs[num_obj].mat = mtls[0];
	num_obj++;
	objs[num_obj].type = OBJ_SPHERE;
	blocker.pos = (struct vec3){0, 0, -8};
	blocker.rad = 0.75;
	objs[num_obj].o.sph = blocker;
	objs[num_obj].mat = mtls[1];
	num_obj++;

	ambcol = (struct color){0.01f, 0.01f, 0.01f, 0};
	bgcol = (struct color){0, 0, 0, 0};
	lights = &lt; num_lt = 1;
	lt = one_light(0, 0, -10);	/* blocker sits between surface and light */

	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		struct color col;
		trace(&r, 0, &col);
		/* fully shadowed: ambient term only */
		TASK_CHECK(fabs(col.r - 0.01f * 0.8f) < 1e-5);
		TASK_CHECK(fabs(col.g - 0.01f * 0.4f) < 1e-5);
		TASK_CHECK(fabs(col.b - 0.01f * 0.2f) < 1e-5);
	}
	return 0;
}

static int case_trace_reflection(void)
{
	static struct material mtls[2];
	static struct metaball mb;
	static struct mb_component comp;
	static struct object objs[2];
	static struct sphere redball;

	reset_scene();
	mtls[0] = make_mtl(0.3, 0.3, 0.3, 0.5, 0.0, 1.0);	/* half reflective */
	mtls[1] = make_mtl(1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
	materials = mtls; num_mtl = 2;

	mb.tau = 0.25; mb.mat = mtls[0];
	mb.comp = &comp;
	comp.c = (struct vec3){0,0,0}; comp.r = 2.0; comp.w = 1.0;
	mb.num_comp = 1;
	metaballs = &mb; num_mb = 1;

	objects = objs; num_obj = 0;
	objs[num_obj].type = OBJ_METABALL;
	objs[num_obj].o.mb = &mb;
	objs[num_obj].mat = mtls[0];
	num_obj++;
	/* reflection ray at the front cap travels toward -z; put red there */
	objs[num_obj].type = OBJ_SPHERE;
	redball.pos = (struct vec3){0, 0, -6};
	redball.rad = 1.0;
	objs[num_obj].o.sph = redball;
	objs[num_obj].mat = mtls[1];
	num_obj++;

	/* no lights: direct terms vanish, isolating ambient plus the reflection
	 * bounce. The reflected red ball then shades to its ambient red only. */
	ambcol = (struct color){0.05f, 0.05f, 0.05f, 0};
	bgcol = (struct color){0, 0, 0, 0};
	lights = 0; num_lt = 0;

	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		struct color col;
		trace(&r, 0, &col);
		/* group ambient 0.05 * 0.3 = 0.015 on every channel; the reflection
		 * adds refl 0.5 times the red ball's ambient red 0.05 = 0.025 to red. */
		TASK_CHECK(fabs(col.r - (0.05f * 0.3f + 0.5f * 0.05f)) < 1e-5);
		TASK_CHECK(fabs(col.g - 0.05f * 0.3f) < 1e-5);
		TASK_CHECK(fabs(col.b - 0.05f * 0.3f) < 1e-5);
		TASK_CHECK(col.r > col.g + 0.01f);
	}
	return 0;
}

static int case_trace_refraction(void)
{
	static struct material mtl;
	static struct metaball mb;
	static struct mb_component comp;
	static struct object obj;

	reset_scene();
	mtl = make_mtl(0.1, 0.1, 0.1, 0.0, 1.0, 1.0);	/* ior 1: straight pass */
	materials = &mtl; num_mtl = 1;

	mb.tau = 0.25; mb.mat = mtl;
	mb.comp = &comp;
	comp.c = (struct vec3){0,0,0}; comp.r = 2.0; comp.w = 1.0;
	mb.num_comp = 1;
	metaballs = &mb; num_mb = 1;

	objects = &obj; num_obj = 1;
	obj.type = OBJ_METABALL;
	obj.o.mb = &mb;
	obj.mat = mtl;

	ambcol = (struct color){0.01f, 0.01f, 0.01f, 0};
	bgcol = (struct color){0, 0, 1, 0};
	lights = 0; num_lt = 0;

	TASK_CHECK(prepare_scene() == 0);
	{
		struct ray r = {{0,0,-5}, {0,0,8}};
		struct color col;
		trace(&r, 0, &col);
		/* two dielectric shade events (entry cap, then the internal-start
		 * query finding the exit cap) each add ambient 0.01 * 0.1 = 0.001;
		 * beyond the exit the blue background (0,0,1) shows through at full
		 * transmission. This exercises the exit-root path end to end. */
		TASK_CHECK(fabs(col.b - (2.0f * 0.01f * 0.1f + 1.0f)) < 1e-5);
		TASK_CHECK(fabs(col.r - 2.0f * 0.01f * 0.1f) < 1e-5);
		TASK_CHECK(fabs(col.g - 2.0f * 0.01f * 0.1f) < 1e-5);
	}
	return 0;
}

static int case_memory_ppm_roundtrip(void)
{
	/* tiny in-memory scene rendered straight into a PPM byte stream,
	 * mirroring the standalone example program. */
	static struct material mtl;
	static struct metaball mb;
	static struct mb_component comp;
	static struct object obj;
	static struct light lt;
	static struct color fb[16];
	FILE *fp;
	char hdr[16];
	int x, y, n;
	long total;
	reset_scene();
	xres = 4; yres = 4; aspect = 1.0;
	mtl = make_mtl(0.9, 0.6, 0.3, 0, 0, 1);
	materials = &mtl; num_mtl = 1;
	mb.tau = 0.25; mb.mat = mtl;
	mb.comp = &comp;
	comp.c = (struct vec3){0,0,0}; comp.r = 2.0; comp.w = 1.0;
	mb.num_comp = 1;
	metaballs = &mb; num_mb = 1;
	objects = &obj; num_obj = 1;
	obj.type = OBJ_METABALL;
	obj.o.mb = &mb;
	obj.mat = mtl;

	ambcol = (struct color){0.02f, 0.02f, 0.02f, 0};
	bgcol = (struct color){0, 0, 0, 0};
	lights = &lt; num_lt = 1;
	lt = one_light(0, 0, -10);
	cam.pos = (struct vec3){0, 0, -5};
	cam.targ = (struct vec3){0, 0, 0};
	cam.fov = M_PI * 30.0 / 180.0;

	TASK_CHECK(prepare_scene() == 0);
	pixels = fb;
	for(y=0; y<yres; y++) {
		for(x=0; x<xres; x++) {
			struct ray ray;
			get_primary_ray(&ray, x, y, 0);
			trace(&ray, 0, &fb[y * xres + x]);
		}
	}

	fp = tmpfile();
	TASK_CHECK(fp != 0);
	fprintf(fp, "P6\n%d %d\n255\n", xres, yres);
	for(n=0; n<xres*yres; n++) {
		fputc((int)(255.0 * fb[n].r), fp);
		fputc((int)(255.0 * fb[n].g), fp);
		fputc((int)(255.0 * fb[n].b), fp);
	}
	rewind(fp);
	TASK_CHECK(fread(hdr, 1, 11, fp) == 11);
	hdr[11] = 0;
	TASK_CHECK(strcmp(hdr, "P6\n4 4\n255\n") == 0);
	fseek(fp, 0, SEEK_END);
	total = ftell(fp);
	/* 11-byte header plus exactly one RGB triple per pixel */
	TASK_CHECK(total == 11 + 16 * 3);
	fclose(fp);
	return 0;
}

const struct task_case task_cases[] = {
	{"roots", "single_kernel_analytic", case_single_kernel},
	{"roots", "field_and_gradient", case_field_gradient_values},
	{"roots", "overlap_summed_before_surface", case_overlap_summed_first},
	{"roots", "tangent_even_root", case_tangent},
	{"roots", "internal_start_exit", case_internal_start},
	{"roots", "negative_weight_layers_thin_shell", case_negative_notch_layers},
	{"roots", "support_boundary_switches_active_set", case_support_event_switch},
	{"roots", "range_and_error_margin_clamping", case_range_clamping},
	{"roots", "load_scene_validation", case_load_validation},
	{"acceleration", "prepare_nearest_and_occlusion", case_prepare_and_query},
	{"acceleration", "reprepare_after_component_edits", case_reprepare_edits},
	{"acceleration", "octree_split_plane_group_dedup", case_split_plane_dedup},
	{"acceleration", "reentrant_isolated_query_state", case_reentrant_queries},
	{"acceleration", "finite_shadow_line", case_finite_shadow_line},
	{"render", "trace_diffuse_color", case_trace_diffuse},
	{"render", "trace_metaball_shadow", case_trace_shadow},
	{"render", "trace_reflection_color", case_trace_reflection},
	{"render", "trace_refraction_exit", case_trace_refraction},
	{"render", "memory_scene_ppm_roundtrip", case_memory_ppm_roundtrip},
};
const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
