// Windowless, dependency-free example for ufbx_fit_skin_weights().
//
// Loads an FBX scene, fits the skin of the first mesh to at most two bone
// influences per vertex over a few animation samples, and prints the worst
// world-space fitting error together with the number of stored influences.
//
// This only uses the portable C API: there is no window, platform or UI code.
// Build (single translation unit form):
//   cc -std=c99 -I. -Isrc examples/skin_fit_example.c ufbx.c src/ufbx_skin_fit.c -lm
// or link the separately compiled objects produced by your build system.
//
// Run with an FBX path as the first argument.

#include "ufbx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s model.fbx\n", argv[0]);
		return 2;
	}

	ufbx_error error;
	ufbx_load_opts load_opts = { 0 };
	ufbx_scene *scene = ufbx_load_file(argv[1], &load_opts, &error);
	if (!scene) {
		char buffer[512];
		ufbx_format_error(buffer, sizeof buffer, &error);
		fprintf(stderr, "failed to load scene: %s\n", buffer);
		return 1;
	}

	if (scene->meshes.count == 0) {
		fprintf(stderr, "scene contains no meshes\n");
		ufbx_free_scene(scene);
		return 1;
	}

	// Discrete sampling contract: the fitter only guarantees its result at the
	// exact sample times supplied here. Choosing more, well-spread samples gives
	// a closer match over the whole animation but never changes the semantics,
	// which remain a least-squares fit over precisely these time values.
	static const double sample_times[] = { 0.0, 0.25, 0.5, 0.75, 1.0 };

	ufbx_skin_weight_fit_opts opts = { 0 };
	opts.sample_times = sample_times;
	opts.num_samples = sizeof(sample_times) / sizeof(sample_times[0]);
	opts.max_weights = 2;
	// Reject the result unless every vertex stays within half a world unit of
	// the original deformation at every sample. Set a large value to accept the
	// global optimum unconditionally.
	opts.max_error = 0.5;

	ufbx_skin_weight_fit_report report;
	ufbx_scene *fitted = ufbx_fit_skin_weights(scene, NULL, 0, &opts, &report, &error);
	if (!fitted) {
		char buffer[512];
		ufbx_format_error(buffer, sizeof buffer, &error);
		fprintf(stderr, "skin fitting failed or exceeded max_error: %s\n", buffer);
		ufbx_free_scene(scene);
		return 1;
	}

	printf("fitted skin: max_error=%.6g at vertex %zu / sample %zu, total influences=%zu\n",
		report.max_error, report.worst_vertex, report.worst_sample, report.total_weights);

	// The returned scene is a normal, independently owned ufbx scene. Driving it
	// with the usual evaluator reuses the fitted weights through the existing
	// linear blend skinning path.
	ufbx_evaluate_opts eval_opts = { 0 };
	eval_opts.evaluate_skinning = true;
	for (size_t i = 0; i < opts.num_samples; i++) {
		ufbx_scene *frame = ufbx_evaluate_scene(fitted, NULL, sample_times[i], &eval_opts, &error);
		if (!frame) {
			fprintf(stderr, "evaluation failed\n");
			ufbx_free_scene(fitted);
			ufbx_free_scene(scene);
			return 1;
		}
		// Frame is a pose of the fitted scene at sample_times[i].
		(void)frame->meshes.data[0]->skinned_position.values.count;
		ufbx_free_scene(frame);
	}

	ufbx_free_scene(fitted);
	ufbx_free_scene(scene);
	return 0;
}
