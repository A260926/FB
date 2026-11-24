Compiler::TwoAxisSource Compiler::CombinedImageSamplerHandler::resolve_axis_source(uint32_t id, bool is_sampler)
{
	auto &sources = is_sampler ? compiler.two_axis_sampler_sources : compiler.two_axis_image_sources;

	auto itr = sources.find(id);
	if (itr != sources.end())
		return itr->second;

	// A global UniformConstant variable used directly (e.g. passed as a call argument).
	if (auto *global_var = compiler.maybe_get<SPIRVariable>(id))
	{
		if (global_var->storage == StorageClassUniformConstant)
		{
			TwoAxisSource src;
			src.root = global_var->self;
			src.index_id = 0;
			src.is_array = !compiler.get<SPIRType>(global_var->basetype).array.empty();
			return src;
		}
	}

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
			if (auto *arg_var = compiler.maybe_get<SPIRVariable>(current))
			{
				if (arg_var->storage == StorageClassUniformConstant)
				{
					TwoAxisSource src;
					src.root = arg_var->self;
					src.index_id = 0;
					src.is_array = !compiler.get<SPIRType>(arg_var->basetype).array.empty();
					return src;
				}
			}
		}
	}

	SPIRV_CROSS_THROW("Could not resolve separate image/sampler element selection for combined image sampler "
	                  "flattening. Resources used through Phi/Select or unsupported control flow are not allowed.");
}

void Compiler::CombinedImageSamplerHandler::track_load(uint32_t result_id, uint32_t ptr_id, bool is_sampler)
{
	auto &sources = is_sampler ? compiler.two_axis_sampler_sources : compiler.two_axis_image_sources;

	// Array element load: the pointer is an access chain already tracked.
	auto chain_itr = sources.find(ptr_id);
	if (chain_itr != sources.end())
	{
		sources[result_id] = chain_itr->second;
		return;
	}

	// Scalar global variable or function parameter carrying a scalar handle.
	sources[result_id] = resolve_axis_source(ptr_id, is_sampler);
}

void Compiler::CombinedImageSamplerHandler::track_access_chain(uint32_t result_id, uint32_t base_id,
                                                               uint32_t index_id, bool is_sampler)
{
	// Resolve the base (a global resource or a parameter carrying a resource pointer)
	// to its root, then attach the selector used at this access chain.
	TwoAxisSource src = resolve_axis_source(base_id, is_sampler);
	src.index_id = index_id;
	src.is_array = true;

	auto &sources = is_sampler ? compiler.two_axis_sampler_sources : compiler.two_axis_image_sources;
	sources[result_id] = src;
}
