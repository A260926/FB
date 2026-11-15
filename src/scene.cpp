#include "scene.h"
#include "intersection.h"

#include <cmath>
#include <cstdio>

void Scene::EnableCausal(float c)
{
	if (!(c > 0.0f) || !std::isfinite(c))
	{
		std::fprintf(stderr, "Causal mode requires a finite positive light speed\n");
		return;
	}

	causalEnabled = true;
	lightSpeed = c;
}

void Scene::Clear()
{
	for (int i=0; i < int(meshes.size()); ++i)
		delete meshes[i];

	meshes.resize(0);
	primitives.resize(0);

	delete bvh.nodes;
	bvh.nodes = NULL;

	if (causal)
	{
		const causal_detail::Hooks* hooks = causal_detail::GetCausalHooks();
		if (hooks)
			hooks->destroy(causal);
		causal = NULL;
	}
	causalEnabled = false;
}

namespace
{
	bool SameTransform(const Transform& a, const Transform& b)	{
		const float eps = 1.0e-6f;
		return Length(a.p - b.p) < eps &&
		       fabsf(a.s - b.s) < eps &&
		       fabsf(a.r.x - b.r.x) < eps && fabsf(a.r.y - b.r.y) < eps &&
		       fabsf(a.r.z - b.r.z) < eps && fabsf(a.r.w - b.r.w) < eps;
	}
}

void Scene::Build()
{
	// build scene bvh
	std::vector<Bounds> primitiveBounds;
	for (int i=0; i < primitives.size(); ++i)
	{
		Bounds r = PrimitiveBounds(primitives[i]);
		primitiveBounds.push_back(r);
	}

	BVHBuilder builder;
	bvh = builder.Build(&primitiveBounds[0], primitiveBounds.size());

	if (!causalEnabled)
		return;

	bool valid = true;

	// environment lighting is not part of the causal model
	if (sky.probe.valid ||
	    Luminance(Color(sky.horizon, 1.0f)) != 0.0f ||
	    Luminance(Color(sky.zenith, 1.0f)) != 0.0f)
	{
		std::fprintf(stderr, "Causal mode rejects environment lighting\n");
		valid = false;
	}

	for (int i=0; i < primitives.size(); ++i)
	{
		const Primitive& p = primitives[i];

		// only triangle meshes
		if (p.type != eMesh || !p.mesh.positions)
		{
			std::fprintf(stderr, "Causal mode only supports triangle mesh primitives\n");
			valid = false;
			continue;
		}

		// instance transforms must be static and reversible
		if (!SameTransform(p.startTransform, p.endTransform) || p.startTransform.s == 0.0f)
		{
			std::fprintf(stderr, "Causal mode rejects moving or singular instance transforms\n");
			valid = false;
		}

		// no transmission
		if (p.material.transmission > 0.0f)
		{
			std::fprintf(stderr, "Causal mode rejects transmissive materials\n");
			valid = false;
		}

		const Mesh* mesh = NULL;
		for (Mesh* m : meshes)
		{
			if ((unsigned long)m == p.mesh.id)
			{
				mesh = m;
				break;
			}
		}

		if (!mesh)
		{
			std::fprintf(stderr, "Causal mode could not resolve a primitive's mesh\n");
			valid = false;
			continue;
		}

		const bool moving = !mesh->endPositions.empty();
		if (moving && mesh->endPositions.size() != mesh->positions.size())
		{
			std::fprintf(stderr, "Causal mode requires endPositions to match positions topology\n");
			valid = false;
			continue;
		}

		if (moving)
		{
			// world-space vertex speeds must stay below c/4
			const float speedLimit = lightSpeed*0.25f;
			for (int v=0; v < int(mesh->positions.size()); ++v)
			{
				const Vec3 world0 = TransformPoint(p.startTransform, mesh->positions[v]);
				const Vec3 world1 = TransformPoint(p.startTransform, mesh->endPositions[v]);
				const float speed = Length(world1 - world0);
				if (speed >= speedLimit)
				{
					std::fprintf(stderr, "Causal mode rejects vertex speed %f >= c/4 (%f)\n",
					             speed, speedLimit);
					valid = false;
					break;
				}
			}
		}
	}

	const causal_detail::Hooks* hooks = causal_detail::GetCausalHooks();

	if (!valid)
	{
		// keep the normal renderer path available; causal queries return black
		if (hooks)
			causal = hooks->createInvalid(lightSpeed);
		return;
	}

	for (Mesh* m : meshes)
		m->RebuildCausalBVH();

	if (causal)
	{
		if (hooks)
			hooks->destroy(causal);
		causal = NULL;
	}
	if (hooks)
		hooks->build(*this);
}
