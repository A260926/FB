void Compiler::CombinedImageSamplerHandler::enable_two_axis_support()
{
	two_axis_active = true;
	auto *glsl = dynamic_cast<CompilerGLSL *>(&compiler);
	if (glsl)
	{
		auto &opts = glsl->get_common_options();
		two_axis_glsl430 = !opts.es && !opts.vulkan_semantics && opts.version >= 430;
	}
}

void Compiler::CombinedImageSamplerHandler::validate_two_axis_resource(uint32_t var_id, bool is_sampler,
                                                                        uint32_t &length) const
{
	length = 1;
	auto *var = compiler.maybe_get<SPIRVariable>(var_id);
	if (!var)
		SPIRV_CROSS_THROW("Two-axis combined image samplers require separate images/samplers backed by global "
		                  "UniformConstant variables.");

	if (var->storage != StorageClassUniformConstant)
		SPIRV_CROSS_THROW("Two-axis combined image samplers only support UniformConstant separate image and "
		                  "sampler resources.");

	auto &type = compiler.get<SPIRType>(var->basetype);
	auto pointee = compiler.get_pointee_type(type);
	if (is_sampler)
	{
		if (pointee.basetype != SPIRType::Sampler)
			SPIRV_CROSS_THROW("Two-axis combined image samplers expect a separate sampler resource.");
	}
	else
	{
		if (pointee.basetype != SPIRType::Image || pointee.image.sampled != 1)
			SPIRV_CROSS_THROW("Two-axis combined image samplers expect a separate sampled image resource.");
		if (pointee.image.dim != Dim2D || pointee.image.depth || pointee.image.ms || pointee.image.arrayed)
			SPIRV_CROSS_THROW("Two-axis combined image samplers only support 2D, non-depth, non-multisampled, "
			                  "non-arrayed floating-point images.");
	}

	// The variable itself may be scalar or a single-level constant-length array.
	if (type.array.size() > 1)
		SPIRV_CROSS_THROW("Multi-dimensional separate image/sampler arrays cannot be flattened into combined "
		                  "image samplers.");
	if (type.array.size() == 1)
	{
		if (!type.array_size_literal.front())
			SPIRV_CROSS_THROW("Two-axis combined image samplers require constant-length resource arrays.");
		length = type.array.front();
		if (length == 0)
			SPIRV_CROSS_THROW("Separate image/sampler arrays must have a positive length.");
	}
}

Compiler::TwoAxisSource Compiler::CombinedImageSamplerHandler::resolve_axis_source(uint32_t id, bool is_sampler)
{
	auto &sources = is_sampler ? sampler_sources : image_sources;
	auto itr = sources.find(id);
	if (itr != sources.end())
		return itr->second;

	// A function parameter: walk the call-site remapping stack to find the
	// argument it was bound to, then resolve that argument as an axis source.
	auto *var = compiler.maybe_get<SPIRVariable>(id);
	if (var && var->parameter)
	{
		uint32_t current = id;
		for (auto frame = parameter_argument_remap.rbegin();
		     frame != parameter_argument_remap.rend(); ++frame)
		{
			auto arg_itr = frame->find(current);
			if (arg_itr == frame->end())
				break;
			current = arg_itr->second;
			auto jtr = sources.find(current);
			if (jtr != sources.end())
				return jtr->second;
		}
	}

	SPIRV_CROSS_THROW("Could not resolve separate image/sampler element selection for combined image sampler "
	                  "flattening. Resources used through Phi/Select or unsupported control flow are not allowed.");
}

void Compiler::CombinedImageSamplerHandler::track_load(uint32_t result_id, uint32_t ptr_id, bool is_sampler)
{
	auto &sources = is_sampler ? sampler_sources : image_sources;

	// Scalar global load: root variable, no index.
	auto *var = compiler.maybe_get<SPIRVariable>(ptr_id);
	if (var)
	{
		TwoAxisSource src;
		src.root = var->self;
		src.index_id = 0;
		src.is_array = false;
		sources[result_id] = src;
		return;
	}

	// Array element load: the pointer is an access chain already tracked.
	auto chain_itr = sources.find(ptr_id);
	if (chain_itr != sources.end())
	{
		sources[result_id] = chain_itr->second;
		return;
	}

	// A function parameter carrying an already-selected scalar handle.
	auto *param_var = compiler.maybe_get<SPIRVariable>(ptr_id);
	if (param_var && param_var->parameter)
	{
		sources[result_id] = resolve_axis_source(ptr_id, is_sampler);
		return;
	}
}

void Compiler::CombinedImageSamplerHandler::track_access_chain(uint32_t result_id, uint32_t base_id,
                                                               uint32_t index_id, bool is_sampler)
{
	auto &sources = is_sampler ? sampler_sources : image_sources;

	TwoAxisSource src;
	auto *var = compiler.maybe_get<SPIRVariable>(base_id);
	if (var)
	{
		src.root = var->self;
	}
	else
	{
		// The base is a function parameter carrying a resource pointer.
		src = resolve_axis_source(base_id, is_sampler);
	}
	src.index_id = index_id;
	src.is_array = true;
	sources[result_id] = src;
}

uint32_t Compiler::CombinedImageSamplerHandler::get_or_create_two_axis_combined(VariableID image_root,
                                                                                uint32_t image_count,
                                                                                VariableID sampler_root,
                                                                                uint32_t sampler_count)
{
	auto existing = compiler.find_two_axis_combined_for_roots(image_root, sampler_root);
	if (existing)
		return existing->combined_id;

	auto itr = find_if(begin(compiler.combined_image_samplers), end(compiler.combined_image_samplers),
	                   [image_root, sampler_root](const CombinedImageSampler &combined) {
		                   return combined.image_id == image_root && combined.sampler_id == sampler_root;
	                   });
	if (itr != end(compiler.combined_image_samplers))
		return itr->combined_id;

	// Create the sampler2D[N * M] global combined resource.
	uint32_t id = compiler.ir.increase_bound_by(3);
	auto type_id = id + 0;
	auto ptr_type_id = id + 1;
	auto combined_id = id + 2;

	auto &image_type = compiler.get_type_from_variable(image_root);
	auto &type = compiler.set<SPIRType>(type_id, OpTypeSampledImage);
	type = image_type;
	type.self = type_id;
	type.basetype = SPIRType::SampledImage;
	type.pointer = false;
	type.storage = StorageClassGeneric;
	type.array.clear();
	type.array_size_literal.clear();
	if (image_count * sampler_count > 1)
	{
		type.array.push_back(image_count * sampler_count);
		type.array_size_literal.push_back(true);
	}

	auto &ptr_type = compiler.set<SPIRType>(ptr_type_id, OpTypePointer);
	ptr_type = type;
	ptr_type.pointer = true;
	ptr_type.storage = StorageClassUniformConstant;
	ptr_type.parent_type = type_id;

	compiler.set<SPIRVariable>(combined_id, ptr_type_id, StorageClassUniformConstant, 0);
	compiler.set_name(combined_id,
	                  join("SPIRV_Cross_Combined", compiler.to_name(image_root), compiler.to_name(sampler_root)));

	compiler.combined_image_samplers.push_back({ combined_id, image_root, sampler_root });
	compiler.combined_image_sampler_axes.push_back(
	    { combined_id, image_root, sampler_root, image_count, sampler_count });
	return combined_id;
}

void Compiler::CombinedImageSamplerHandler::handle_two_axis_sampled_image(uint32_t result_id, uint32_t image_id,
                                                                          uint32_t sampler_id, bool is_fetch)
{
	if (is_fetch)
		SPIRV_CROSS_THROW("Two-axis combined image samplers do not support texelFetch-style image operations.");

	if (!two_axis_glsl430)
		SPIRV_CROSS_THROW("Flattening separate sampler arrays into combined image samplers is only supported for "
		                  "non-Vulkan desktop GLSL 430 or newer.");

	auto image_src = resolve_axis_source(image_id, false);
	auto sampler_src = resolve_axis_source(sampler_id, true);

	uint32_t image_count = 1, sampler_count = 1;
	validate_two_axis_resource(image_src.root, false, image_count);
	validate_two_axis_resource(sampler_src.root, true, sampler_count);

	if (image_src.is_array && image_count == 1)
		SPIRV_CROSS_THROW("Separate image array reported scalar length during combined image sampler flattening.");
	if (sampler_src.is_array && sampler_count == 1)
		SPIRV_CROSS_THROW("Separate sampler array reported scalar length during combined image sampler flattening.");

	uint32_t combined_id =
	    get_or_create_two_axis_combined(image_src.root, image_count, sampler_src.root, sampler_count);

	// Record the resolved selection for the GLSL backend. Index IDs are the selector
	// SSA IDs valid in the function performing the OpSampledImage; 0 means scalar.
	TwoAxisSelection selection;
	selection.combined_id = combined_id;
	selection.image_index_id = image_src.is_array ? image_src.index_id : 0u;
	selection.sampler_index_id = sampler_src.is_array ? sampler_src.index_id : 0u;
	selection.sampler_count = sampler_count;
	compiler.two_axis_selections[result_id] = selection;
}
