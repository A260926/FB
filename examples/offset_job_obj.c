/*
 * Example: approximate the offset surface of a NURBS surface with a
 * resumable session, driving it one evaluation at a time, then write the
 * result to an OBJ file.
 *
 * Usage: offset_job_obj <output.obj>
 *
 * The example builds a small bicubic saddle, creates a session, advances it
 * with a budget of exactly one evaluation per step, takes the finished
 * surface and writes a non-empty Wavefront OBJ.  It returns non-zero if the
 * session fails or the file cannot be written.
 */

#include "sisl_offset_job.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* A 4x4 control net over a non-unit parameter domain [-2,4]^2. */

static SISLSurf *build_source(void)
{
  double k[] = {-2, -2, -2, -2, 4, 4, 4, 4};
  double c[4 * 4 * 3];
  int i, j;

  for (j = 0; j < 4; ++j)
    for (i = 0; i < 4; ++i)
    {
      int n = 3 * (4 * j + i);
      c[n + 0] = i;
      c[n + 1] = j;
      c[n + 2] = 0.12 * i * j + 0.03 * i * i;
    }

  return newSurf(4, 4, 4, 4, k, k, c, 1, 3, 1);
}

/* Evaluate the surface, honouring a rational description, at (u,v). */

static void eval_point(SISLSurf *ps, double u, double v, double out[3])
{
  int il1 = 0, il2 = 0, stat = 0;
  double par[2];
  double der[27];

  par[0] = u;
  par[1] = v;
  s1421(ps, 0, par, &il1, &il2, der, NULL, &stat);
  if (stat < 0)
  {
    out[0] = out[1] = out[2] = 0.0;
    return;
  }
  out[0] = der[0];
  out[1] = der[1];
  out[2] = der[2];
}

static int write_obj(const char *path, SISLSurf *s, int nu, int nv)
{
  FILE *f;
  int i, j;
  double u0, u1, v0, v1;

  f = fopen(path, "w");
  if (f == NULL) return 0;

  u0 = s->et1[s->ik1 - 1];
  u1 = s->et1[s->in1];
  v0 = s->et2[s->ik2 - 1];
  v1 = s->et2[s->in2];

  fprintf(f, "# SISL offset surface\n");
  fprintf(f, "# %dx%d control net, degree %d x %d\n",
          s->in1, s->in2, s->ik1 - 1, s->ik2 - 1);
  fprintf(f, "# %d x %d sample grid\n", nu, nv);

  for (j = 0; j <= nv; ++j)
    for (i = 0; i <= nu; ++i)
    {
      double u = u0 + (u1 - u0) * (double)i / (double)nu;
      double v = v0 + (v1 - v0) * (double)j / (double)nv;
      double p[3];
      eval_point(s, u, v, p);
      fprintf(f, "v %.*g %.*g %.*g\n", 15, p[0], 15, p[1], 15, p[2]);
    }

  for (j = 0; j < nv; ++j)
    for (i = 0; i < nu; ++i)
    {
      int a = j * (nu + 1) + i + 1;
      int b = a + 1;
      int c = a + (nu + 1);
      int d = c + 1;
      fprintf(f, "f %d %d %d\n", a, b, d);
      fprintf(f, "f %d %d %d\n", a, d, c);
    }

  if (ferror(f))
  {
    fclose(f);
    return 0;
  }

  if (fclose(f) != 0) return 0;
  return 1;
}

int main(int argc, char **argv)
{
  SISLSurf *source;
  SISLOffsetJob *job;
  SISLOffsetJobStats stats;
  SISLSurf *offset;
  int jstat = 0;
  int rc;
  long steps = 0;

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <output.obj>\n", argv[0]);
    return 2;
  }

  source = build_source();
  if (source == NULL)
  {
    fprintf(stderr, "could not create the source surface\n");
    return 1;
  }

  job = sisl_offset_job_create(source, 0.1, 0.001, 0.0, 3, &jstat);
  if (job == NULL)
  {
    fprintf(stderr, "could not create the offset session (status %d)\n",
            jstat);
    freeSurf(source);
    return 1;
  }

  /* Drive the session with a budget of one evaluation per call. */

  do
  {
    rc = sisl_offset_job_step(job, 1, &stats);
    ++steps;
    if (rc < 0 && rc != SISL_OFFSET_JOB_CANCELLED)
    {
      fprintf(stderr, "offset session failed with status %d\n", rc);
      sisl_offset_job_free(job);
      freeSurf(source);
      return 1;
    }
  } while (rc == SISL_OFFSET_JOB_MORE);

  if (rc != SISL_OFFSET_JOB_DONE)
  {
    fprintf(stderr, "offset session did not finish (status %d)\n", rc);
    sisl_offset_job_free(job);
    freeSurf(source);
    return 1;
  }

  offset = sisl_offset_job_take_result(job, &jstat);
  if (offset == NULL)
  {
    fprintf(stderr, "no offset surface available (status %d)\n", jstat);
    sisl_offset_job_free(job);
    freeSurf(source);
    return 1;
  }

  fprintf(stderr,
          "offset surface ready after %ld steps and %d evaluations\n",
          steps, stats.evaluations_total);

  if (!write_obj(argv[1], offset, 16, 16))
  {
    fprintf(stderr, "failed to write %s\n", argv[1]);
    freeSurf(offset);
    sisl_offset_job_free(job);
    freeSurf(source);
    return 1;
  }

  fprintf(stderr, "wrote %s\n", argv[1]);

  freeSurf(offset);
  sisl_offset_job_free(job);
  freeSurf(source);
  return 0;
}
