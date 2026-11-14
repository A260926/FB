/*
 * cgltf_retime_example.c - minimal example for the cgltf_retime add-on.
 *
 * Build (from the repository root):
 *
 *   cc -std=c99 -I. examples/cgltf_retime_example.c \
 *      examples/cgltf_retime.c task_support/cgltf_impl.c -lm -o retime_example
 *
 * The file is not part of the target test; the implementation macros live in
 * task_support/cgltf_impl.c so that both can share one definition.
 */

#include "cgltf.h"
#include "cgltf_write.h"
#include "examples/cgltf_retime.h"

#include <stdio.h>

/* Read the first and last time of an animation's first sampler. */
static int animation_span(const cgltf_animation* animation, float* first, float* last)
{
	const cgltf_accessor* input;
	if (!animation || animation->channels_count == 0 ||
			!animation->channels[0].sampler ||
			!animation->channels[0].sampler->input)
	{
		return 0;
	}
	input = animation->channels[0].sampler->input;
	if (input->count == 0 ||
			!cgltf_accessor_read_float(input, 0, first, 1) ||
			!cgltf_accessor_read_float(input, input->count - 1, last, 1))
	{
		return 0;
	}
	return 1;
}

int main(int argc, char** argv)
{
	const char* input_path = argc > 1 ? argv[1] : "in.gltf";
	const char* output_path = argc > 2 ? argv[2] : "out.gltf";
	cgltf_options options;
	cgltf_data* source = NULL;
	cgltf_data* result = NULL;
	cgltf_result status;
	cgltf_float source_times[3];
	cgltf_float output_times[3];
	float first, last, middle, out_middle;

	options = (cgltf_options){0};
	status = cgltf_parse_file(&options, input_path, &source);
	if (status != cgltf_result_success)
	{
		fprintf(stderr, "parse failed: %d\n", (int)status);
		return 1;
	}
	status = cgltf_load_buffers(&options, source, input_path);
	if (status != cgltf_result_success)
	{
		fprintf(stderr, "load_buffers failed: %d\n", (int)status);
		cgltf_free(source);
		return 1;
	}

	if (!animation_span(&source->animations[0], &first, &last) || !(last > first))
	{
		fprintf(stderr, "animation 0 has no usable time range\n");
		cgltf_free(source);
		return 1;
	}

	/*
	 * Split the source span in half.  The first half is played faster (it
	 * occupies 25% of the output), the second half slower.  Any finite,
	 * strictly increasing arrays work as long as the first output time is
	 * zero and the source span stays within the animation's time range.
	 */
	middle = (float)(((double)first + (double)last) / 2.0);
	out_middle = (float)(((double)last - (double)first) / 4.0);
	source_times[0] = first;
	source_times[1] = middle;
	source_times[2] = last;
	output_times[0] = 0.0f;
	output_times[1] = out_middle;
	output_times[2] = (float)((double)last - (double)first);

	/* Rewrite animation 0; other animations and all scene data are kept. */
	status = cgltf_retime_animation(source, 0, source_times, output_times, 3, &result);
	if (status != cgltf_result_success)
	{
		fprintf(stderr, "retime failed: %d\n", (int)status);
		cgltf_free(source);
		return 1;
	}

	/* The result owns its buffer bytes; the source can be dropped now. */
	cgltf_free(source);

	status = cgltf_validate(result);
	if (status != cgltf_result_success)
	{
		fprintf(stderr, "validate failed: %d\n", (int)status);
		cgltf_free(result);
		return 1;
	}

	/* All buffers are embedded as data URIs, so this file is portable. */
	status = cgltf_write_file(&options, output_path, result);
	if (status != cgltf_result_success)
	{
		fprintf(stderr, "write failed: %d\n", (int)status);
		cgltf_free(result);
		return 1;
	}

	cgltf_free(result);
	printf("wrote %s\n", output_path);
	return 0;
}
