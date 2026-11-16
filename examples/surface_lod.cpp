// Surface error preserving LOD example
//
// Generates a small grid mesh with a raised bump, runs meshopt_simplifyWithSurfaceError with a small
// object-space surface distance limit and writes the resulting mesh as an OBJ file. The two-sided
// certified distance bound is printed to stdout.
//
// Usage: surface_lod [output.obj] [max_surface_error] [target_triangles]
#include "src/meshoptimizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

namespace
{

struct Vertex
{
	float x, y, z;
	float u, v;
};

static void buildMesh(unsigned int n, float bump, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices)
{
	for (unsigned int j = 0; j <= n; ++j)
		for (unsigned int i = 0; i <= n; ++i)
		{
			Vertex v = {float(i) / n, float(j) / n, 0.f, float(i) / n, float(j) / n};

			// a narrow raised bump in the middle; flattening it would exceed a small surface error limit
			if (bump != 0.f && i == n / 2 && j == n / 2)
				v.z = bump;

			vertices.push_back(v);
		}

	for (unsigned int j = 0; j < n; ++j)
		for (unsigned int i = 0; i < n; ++i)
		{
			unsigned int a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
			unsigned int f[] = {a, b, d, a, d, c};
			indices.insert(indices.end(), f, f + 6);
		}
}

} // namespace

// runs the example and writes the OBJ file; returns the number of output triangles, or 0 on failure
// this is separated from main() so that tests can execute the same code path in-process
int surface_lod_run(const char* output_path, float max_surface_error, size_t target_triangles)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
	buildMesh(24, 0.35f, vertices, indices);

	std::vector<unsigned int> result(indices.size());
	const float attribute_weights[2] = {0.1f, 0.1f};
	float result_error = -1;
	meshopt_SurfaceErrorStats stats = {};

	size_t count = meshopt_simplifyWithSurfaceError(result.data(), indices.data(), indices.size(), &vertices[0].x, vertices.size(), sizeof(Vertex), &vertices[0].u, sizeof(Vertex), attribute_weights, 2, NULL, target_triangles * 3, 1.f, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &result_error, max_surface_error, 16, &stats);

	if (count == 0)
		return 0;

	FILE* file = fopen(output_path, "wb");

	if (!file)
		return 0;

	fprintf(file, "# meshoptimizer surface error LOD example\n");
	fprintf(file, "# input triangles: %d\n", int(indices.size() / 3));
	fprintf(file, "# output triangles: %d\n", int(count / 3));
	fprintf(file, "# certified two-sided surface bound: %.9g\n", stats.certified_error);
	fprintf(file, "# qem error: %.9g\n", result_error);

	for (size_t i = 0; i < vertices.size(); ++i)
		fprintf(file, "v %.9g %.9g %.9g\n", vertices[i].x, vertices[i].y, vertices[i].z);
	for (size_t i = 0; i < vertices.size(); ++i)
		fprintf(file, "vt %.9g %.9g\n", vertices[i].u, vertices[i].v);

	// OBJ indices are 1-based; vertex references stay in the original vertex buffer
	for (size_t i = 0; i < count; i += 3)
		fprintf(file, "f %u/%u %u/%u %u/%u\n", result[i + 0] + 1, result[i + 0] + 1, result[i + 1] + 1, result[i + 1] + 1, result[i + 2] + 1, result[i + 2] + 1);

	fclose(file);

	printf("SURFACE_LOD input_triangles=%d output_triangles=%d certified_error=%.9g qem_error=%.9g\n", int(indices.size() / 3), int(count / 3), stats.certified_error, result_error);

	return int(count / 3);
}

#ifndef MESHOPT_EXAMPLE_NO_MAIN
int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "surface_lod.obj";
	float max_surface_error = argc > 2 ? float(atof(argv[2])) : 0.01f;
	size_t target_triangles = argc > 3 ? size_t(atoi(argv[3])) : 24 * 3;

	int triangles = surface_lod_run(path, max_surface_error, target_triangles);

	if (triangles == 0)
	{
		fprintf(stderr, "surface_lod: failed to write %s\n", path);
		return 1;
	}

	return 0;
}
#endif
