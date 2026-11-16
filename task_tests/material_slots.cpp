// Regression tests for OBJ material slots, per-instance slot bindings and
// mixed-material mesh light transport.
//
// Groups:
//   binding      - OBJ parsing, slot preservation, instance binding isolation
//   distribution - per-face emissive mass, PDF and emission power
//   transport    - real Scene::getLights and direct-light production paths

#include "task_support/check.h"

#include "OBJReader.hpp"
#include "Hittables/Cube.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/Mesh.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Lambertian.hpp"
#include "Materials/MaterialBindings.hpp"
#include "Sampler.hpp"
#include "Scene/Scene.hpp"
#include "SceneFile/SceneFile.hpp"
#include "Utilities.hpp"
#include "Vector3.hpp"
#include "../src/renderer/RendererInternal.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
	bool	near(double actual, double expected, double tolerance = 1e-6)
	{
		return (std::fabs(actual - expected) <= tolerance);
	}

	double	luminance(const Color& color)
	{
		return (Utilities::luminance(color));
	}

	std::string	writeTextFile(const std::string& name, const std::string& contents)
	{
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / name;
		std::ofstream stream(path, std::ios::trunc | std::ios::binary);

		if (!stream)
		{
			return (std::string());
		}
		stream << contents;
		stream.close();
		return (path.string());
	}

	void	removeFile(const std::string& path)
	{
		if (!path.empty())
		{
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	}

	ObjReadOptions	quietOptions(void)
	{
		ObjReadOptions options;

		options.quiet = true;
		return (options);
	}

	// A small OBJ with a horizontal quad (fan triangulation), two degenerate
	// faces and a negative index, split between three named slots. Faces live at
	// y=0 so a downward ray selects a specific slot by x/z position.
	std::string	multiSlotObjContents(void)
	{
		return (
			"v -1 0 -1\n"  // 1  Shade quad
			"v  1 0 -1\n"  // 2
			"v  1 0  1\n"  // 3
			"v -1 0  1\n"  // 4
			"v  3 0 -1\n"  // 5  Bulb
			"v  5 0 -1\n"  // 6
			"v  3 0  1\n"  // 7
			"v  5 0  1\n"  // 8
			"v  3 0  0\n"  // 9  degenerate helper
			"v  7 0 -1\n"  // 10 Frame
			"v  9 0 -1\n"  // 11
			"v  7 0  1\n"  // 12
			"usemtl Shade\n"
			"f 1 2 3 4\n"
			"usemtl Bulb\n"
			"f 5 6 7\n"
			"f 5 7 -5\n"
			"f 9 9 5\n"
			"usemtl Frame\n"
			"f 10 11 12\n"
			"f 10 12 10\n"
		);
	}

	Color	emissiveLuminanceColor(double luminanceValue)
	{
		// Pure-ish white keeps the luminance math easy to inspect.
		return (Color(luminanceValue, luminanceValue, luminanceValue));
	}

	std::shared_ptr<Material>	fallbackMaterial(void)
	{
		return (std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5)));
	}

	struct BuiltMultiSlotMesh
	{
		ObjMeshData	data;
		MaterialBindings	bindings;
		std::shared_ptr<Material>	shade;
		std::shared_ptr<Material>	bulb;
		std::shared_ptr<Material>	frame;
		std::shared_ptr<Material>	fallback;
		std::shared_ptr<Mesh>	mesh;
	};

	BuiltMultiSlotMesh	buildMultiSlotMesh(Vector3 rotationDegrees, Vector3 scale)
	{
		BuiltMultiSlotMesh built;

		const std::string path = writeTextFile("luz_material_slots.obj", multiSlotObjContents());
		built.data = readObjMeshData(path, quietOptions());
		removeFile(path);

		built.shade = std::make_shared<Lambertian>(Color(0.1, 0.2, 0.3));
		built.bulb = std::make_shared<Emissive>(Color(3.0, 2.0, 1.0));
		built.frame = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
		built.fallback = std::make_shared<Lambertian>(Color(0.5, 0.5, 0.5));

		built.bindings["Shade"] = built.shade;
		built.bindings["Bulb"] = built.bulb;
		built.bindings["Frame"] = built.frame;

		built.mesh = std::make_shared<Mesh>(buildObjMesh(
			built.data,
			Vector3(),
			rotationDegrees,
			scale,
			built.bindings,
			built.fallback
		));
		return (built);
	}

	// Total world/local area of every face using the given slot, in mesh-local
	// units. Independent of any instance-specific bindings.
	double	slotArea(const BuiltMultiSlotMesh& built, const std::string& slotName)
	{
		double area = 0.0;

		for (std::size_t index = 0; index < built.mesh->surfaceElementCount(); index++)
		{
			const int slot = built.mesh->getTriangleMaterialSlot(index);
			const std::vector<std::string>& names = built.mesh->getMaterialSlotNames();

			if (slot < 0 || static_cast<std::size_t>(slot) >= names.size() || names[slot] != slotName)
			{
				continue;
			}
			area += built.mesh->transformedSurfaceElementArea(
				index,
				[](const Vector3& point) { return (point); }
			);
		}
		return (area);
	}

	// Sum of Ai * Yi for every face using the given slot, evaluated with the
	// given per-slot binding table and the shared mesh's local face areas.
	double	slotMassWithBindings(
		const BuiltMultiSlotMesh& built,
		const MaterialBindings& bindings,
		const std::string& slotName
	)
	{
		double mass = 0.0;

		for (std::size_t index = 0; index < built.mesh->surfaceElementCount(); index++)
		{
			const int slot = built.mesh->getTriangleMaterialSlot(index);
			const std::vector<std::string>& names = built.mesh->getMaterialSlotNames();

			if (slot < 0 || static_cast<std::size_t>(slot) >= names.size() || names[slot] != slotName)
			{
				continue;
			}
			const auto binding = bindings.find(slotName);
			Material* material = binding != bindings.end() && binding->second
				? binding->second.get()
				: built.fallback.get();
			const double area = built.mesh->transformedSurfaceElementArea(
				index,
				[](const Vector3& point) { return (point); }
			);
			mass += area * luminance(material->emitted());
		}
		return (mass);
	}
}

// ---------------------------------------------------------------------------
// binding
// ---------------------------------------------------------------------------

static int	binding_obj_reader_preserves_slots_and_triangles(void)
{
	const std::string path = writeTextFile("luz_material_slots_reader.obj", multiSlotObjContents());
	TASK_CHECK(!path.empty());

	const ObjMeshData data = readObjMeshData(path, quietOptions());

	removeFile(path);

	// Quad -> two triangles, Bulb -> three triangles (one degenerate is kept in
	// the raw data until Mesh filtering), Frame -> two triangles.
	TASK_CHECK(data.triangles.size() == 7);
	TASK_CHECK(data.triangleMaterialSlots.size() == data.triangles.size());

	// Names are case-sensitive and stored in first-seen order.
	TASK_CHECK(data.materialSlotNames.size() == 3);
	TASK_CHECK(data.materialSlotNames[0] == "Shade");
	TASK_CHECK(data.materialSlotNames[1] == "Bulb");
	TASK_CHECK(data.materialSlotNames[2] == "Frame");

	// Faces before any usemtl use the empty-name slot; this file has none, so
	// the empty slot must not appear.
	for (const std::string& name : data.materialSlotNames)
	{
		TASK_CHECK(name != "");
	}

	// Fan triangulation keeps the quad's slot on both triangles.
	TASK_CHECK(data.materialSlotForTriangle(0) == 0);
	TASK_CHECK(data.materialSlotForTriangle(1) == 0);
	// Bulb block covers the negative-index face and the two degenerate ones.
	TASK_CHECK(data.materialSlotForTriangle(2) == 1);
	TASK_CHECK(data.materialSlotForTriangle(3) == 1);
	TASK_CHECK(data.materialSlotForTriangle(4) == 1);
	// Frame block covers the last two faces.
	TASK_CHECK(data.materialSlotForTriangle(5) == 2);
	TASK_CHECK(data.materialSlotForTriangle(6) == 2);

	// The negative index resolves to vertex 8 (0-based 7) in the Bulb block.
	TASK_CHECK(data.triangles[3][2].vertexIndex == 7);

	return (0);
}

static int	binding_obj_reader_assigns_unlabelled_faces_to_empty_slot(void)
{
	const std::string contents =
		"v 0 0 0\n"
		"v 1 0 0\n"
		"v 0 1 0\n"
		"f 1 2 3\n"
		"usemtl Lamp\n"
		"v 2 0 0\n"
		"v 3 0 0\n"
		"v 2 1 0\n"
		"f 4 5 6\n";
	const std::string path = writeTextFile("luz_material_slots_empty.obj", contents);
	const ObjMeshData data = readObjMeshData(path, quietOptions());

	removeFile(path);

	TASK_CHECK(data.triangles.size() == 2);
	TASK_CHECK(data.materialSlotNames.size() == 2);
	// Empty slot comes from the unlabelled face, before Lamp was declared. It
	// must be the empty-name slot and the first triangle must use it.
	const int firstSlot = data.materialSlotForTriangle(0);
	const int secondSlot = data.materialSlotForTriangle(1);

	TASK_CHECK(firstSlot >= 0);
	TASK_CHECK(secondSlot >= 0);
	TASK_CHECK(*data.materialSlotName(firstSlot) == "");
	TASK_CHECK(*data.materialSlotName(secondSlot) == "Lamp");
	return (0);
}

static int	binding_build_mesh_filters_degenerate_and_keeps_slots(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));

	// 7 raw faces, two degenerate (f 9 9 5 and f 10 12 10) => 5 stored faces.
	TASK_CHECK(built.data.triangles.size() == 7);
	TASK_CHECK(built.mesh->surfaceElementCount() == 5);
	TASK_CHECK(built.mesh->getTriangleMaterialSlots().size() == 5);
	TASK_CHECK(built.mesh->usesMaterialSlots());
	TASK_CHECK(built.mesh->getMaterialSlotNames().size() == 3);

	// Slot order survives the packed BVH reorder: query by triangle slot.
	std::size_t shadeFaces = 0;
	std::size_t bulbFaces = 0;
	std::size_t frameFaces = 0;
	for (std::size_t index = 0; index < built.mesh->surfaceElementCount(); index++)
	{
		const int slot = built.mesh->getTriangleMaterialSlot(index);
		const std::string& name = built.mesh->getMaterialSlotNames()[static_cast<std::size_t>(slot)];

		if (name == "Shade")
			shadeFaces++;
		else if (name == "Bulb")
			bulbFaces++;
		else if (name == "Frame")
			frameFaces++;
	}
	TASK_CHECK(shadeFaces == 2);
	TASK_CHECK(bulbFaces == 2);
	TASK_CHECK(frameFaces == 1);

	// The resolved material for each slot is the bound one.
	TASK_CHECK(built.mesh->getMaterialSlotMaterials()[0].get() == built.shade.get());
	TASK_CHECK(built.mesh->getMaterialSlotMaterials()[1].get() == built.bulb.get());
	TASK_CHECK(built.mesh->getMaterialSlotMaterials()[2].get() == built.frame.get());
	return (0);
}

static int	binding_two_instances_share_geometry_with_opposite_bindings(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto red = std::make_shared<Emissive>(emissiveLuminanceColor(2.0));
	auto blue = std::make_shared<Emissive>(emissiveLuminanceColor(5.0));
	MaterialBindings redBindings;
	MaterialBindings blueBindings;

	redBindings["Bulb"] = red;
	redBindings["Shade"] = built.shade;
	blueBindings["Bulb"] = blue;
	blueBindings["Shade"] = built.shade;

	auto instanceA = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(0.0, 0.0, 0.0),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		redBindings,
		built.fallback
	);
	auto instanceB = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(0.0, 0.0, 0.0),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		blueBindings,
		built.fallback
	);

	TASK_CHECK(instanceA->getGeometry().get() == built.mesh.get());
	TASK_CHECK(instanceB->getGeometry().get() == built.mesh.get());
	TASK_CHECK(instanceA->getGeometry().get() == instanceB->getGeometry().get());

	// Bindings are held by each instance and do not mutate the source mesh.
	TASK_CHECK(instanceA->getSlotMaterial("Bulb").get() == red.get());
	TASK_CHECK(instanceB->getSlotMaterial("Bulb").get() == blue.get());
	TASK_CHECK(built.mesh->getMaterialSlotMaterials()[1].get() == built.bulb.get());

	// Slot lookups are case-sensitive, unknown slots fall back.
	TASK_CHECK(instanceA->getSlotMaterial("bulb").get() == built.fallback.get());
	TASK_CHECK(instanceA->getSlotMaterial("Frame").get() == built.fallback.get());

	// The apparent bulb luminance differs per instance.
	TASK_CHECK(near(instanceA->lightSelectionWeight(), slotMassWithBindings(built, redBindings, "Bulb")));
	TASK_CHECK(near(instanceB->lightSelectionWeight(), slotMassWithBindings(built, blueBindings, "Bulb")));

	// A hit on a Bulb face reports the instance's own material.
	Ray ray(Vector3(4.0, 2.0, 0.0), Vector3(0.0, -1.0, 0.0));
	HitRecord hitRecord;

	TASK_CHECK(instanceA->hit(ray, hitRecord, 0.001, 100.0));
	TASK_CHECK(hitRecord.materialSlot == 1);
	TASK_CHECK(hitRecord.material == red.get());
	TASK_CHECK(instanceB->hit(ray, hitRecord, 0.001, 100.0));
	TASK_CHECK(hitRecord.materialSlot == 1);
	TASK_CHECK(hitRecord.material == blue.get());

	// The shared source mesh still resolves to its own slot materials.
	HitRecord sourceHit;
	TASK_CHECK(built.mesh->hit(ray, sourceHit, 0.001, 100.0));
	TASK_CHECK(sourceHit.material == built.bulb.get());
	return (0);
}

static int	binding_legacy_single_material_overload_is_unchanged(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto legacyMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(7.0));
	Mesh legacy = buildObjMesh(
		built.data,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		legacyMaterial
	);

	// The legacy overload still forces one material over the whole mesh and
	// does not expose slot metadata.
	TASK_CHECK(!legacy.usesMaterialSlots());
	TASK_CHECK(legacy.surfaceElementCount() == 5);
	TASK_CHECK(legacy.getMaterial() == legacyMaterial.get());
	TASK_CHECK(near(legacy.lightSelectionWeight(), legacy.surfaceArea() * 7.0));

	// The legacy MeshInstance overload over a slot-aware shared mesh keeps its
	// original meaning: every face reports the instance's single material.
	const std::vector<std::string>& slotNames = built.mesh->getMaterialSlotNames();
	TASK_CHECK(slotNames.size() == 3);

	MeshInstance legacyInstance(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		legacyMaterial
	);
	for (const Vector3& origin : {
		Vector3(0.0, 2.0, 0.0),
		Vector3(4.0, 2.0, 0.0),
		Vector3(8.0, 2.0, 0.0)
	})
	{
		Ray ray(origin, Vector3(0.0, -1.0, 0.0));
		HitRecord hitRecord;

		TASK_CHECK(legacyInstance.hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(hitRecord.material == legacyMaterial.get());
	}
	TASK_CHECK(near(
		legacyInstance.lightSelectionWeight(),
		built.mesh->surfaceArea() * 7.0,
		1e-6
	));
	return (0);
}

static int	binding_many_faces_survive_bvh_reorder(void)
{
	// 40 triangles in two interleaved slots, forcing the packed BVH to split
	// and reorder its traversal index without losing the slot association.
	std::string contents;
	const std::size_t faceCount = 40;

	for (std::size_t index = 0; index < faceCount; index++)
	{
		const double x = static_cast<double>(index) * 2.0;
		contents +=
			"v " + std::to_string(x) + " 0 0\n"
			+ "v " + std::to_string(x + 1.0) + " 0 0\n"
			+ "v " + std::to_string(x) + " 1 0\n";
	}
	for (std::size_t index = 0; index < faceCount; index++)
	{
		contents += index % 2 == 0 ? "usemtl Even\n" : "usemtl Odd\n";
		contents += "f " + std::to_string(index * 3 + 1) + " "
			+ std::to_string(index * 3 + 2) + " "
			+ std::to_string(index * 3 + 3) + "\n";
	}
	const std::string path = writeTextFile("luz_material_slots_many.obj", contents);
	const ObjMeshData data = readObjMeshData(path, quietOptions());
	removeFile(path);

	auto evenMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(2.0));
	auto oddMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(7.0));
	MaterialBindings bindings;

	bindings["Even"] = evenMaterial;
	bindings["Odd"] = oddMaterial;

	auto mesh = std::make_shared<Mesh>(buildObjMesh(
		data,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0))
	));

	TASK_CHECK(mesh->surfaceElementCount() == faceCount);

	// The packed BVH reorders its traversal index but not the stored faces;
	// each face's material remains the one resolved from its slot.
	for (std::size_t index = 0; index < faceCount; index++)
	{
		TASK_CHECK(mesh->getSurfaceMaterial(index) == (index % 2 == 0
			? evenMaterial.get()
			: oddMaterial.get()));
	}

	// Every triangle has area 0.5; total mass = 20*0.5*2 + 20*0.5*7 = 90.
	TASK_CHECK(near(mesh->lightSelectionWeight(), 90.0, 1e-6));

	// Hit each face straight down and verify the exact source-face material.
	for (std::size_t index = 0; index < faceCount; index++)
	{
		const double x = static_cast<double>(index) * 2.0 + 0.25;
		Ray ray(Vector3(x, 0.25, 2.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		TASK_CHECK(mesh->hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(hitRecord.material == (index % 2 == 0
			? evenMaterial.get()
			: oddMaterial.get()));
	}
	return (0);
}

static int	binding_slot_bindings_reject_zero_scale(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto material = std::make_shared<Emissive>(emissiveLuminanceColor(1.0));
	MaterialBindings bindings;
	bool threw = false;

	bindings["Bulb"] = material;
	try
	{
		MeshInstance rejected(
			built.mesh,
			Vector3(),
			Vector3(),
			Vector3(1.0, 0.0, 1.0),
			bindings,
			built.fallback
		);
		(void)rejected;
	}
	catch (const std::exception&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// Non-finite components are rejected as well.
	threw = false;
	try
	{
		MeshInstance rejected(
			built.mesh,
			Vector3(),
			Vector3(),
			Vector3(1.0, 1.0, std::numeric_limits<double>::infinity()),
			bindings,
			built.fallback
		);
		(void)rejected;
	}
	catch (const std::exception&)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// Zero scale keeps the legacy MeshInstance behaviour (no exception; the
	// instance simply cannot be inverted).
	MeshInstance legacyDegenerate(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 0.0, 1.0),
		built.fallback
	);
	Ray ray(Vector3(0.0, 2.0, 0.0), Vector3(0.0, -1.0, 0.0));
	HitRecord hitRecord;

	TASK_CHECK(!legacyDegenerate.hit(ray, hitRecord, 0.001, 100.0));
	return (0);
}

static int	binding_rotated_and_nonuniform_scaled_instances_keep_slots(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(0.0, 90.0, 0.0), Vector3(2.0, 1.0, 3.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(4.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = built.shade;
	bindings["Frame"] = built.frame;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.0, 90.0, 0.0),
		Vector3(2.0, 1.0, 3.0),
		bindings,
		built.fallback
	);

	// The instance's sampled light must always report the Bulb material and a
	// positive pdf; per-face world areas must be recomputed after scaling.
	Sampler::setRenderSeed(7);
	for (std::uint32_t sample = 0; sample < 64; sample++)
	{
		Sampler::beginPixelSample(sample, 0, sample);
		HittableLightSample lightSample;

		TASK_CHECK(instance->sampleLight(Vector3(0.0, 5.0, 0.0), lightSample));
		TASK_CHECK(lightSample.valid);
		TASK_CHECK(lightSample.material == bulbMaterial.get());
		TASK_CHECK(lightSample.pdf > 0.0);
		TASK_CHECK(std::isfinite(lightSample.pdf));
		Sampler::endPixelSample();
	}
	TASK_CHECK(instance->lightSelectionWeight() > 0.0);
	return (0);
}

static int	binding_hit_uv_normals_and_scale(void)
{
	// Smooth normals deliberately differ from the geometric normal so the
	// inverse-transpose path is observable.
	const std::string contents =
		"v 0 0 0\n"
		"v 1 0 1\n"
		"v 0 1 1\n"
		"vn -0.5 0 0.5\n"
		"vt 0 0\n"
		"vt 1 0\n"
		"vt 0 1\n"
		"usemtl Bulb\n"
		"f 1/1/1 2/2/1 3/3/1\n";
	const std::string path = writeTextFile("luz_material_slots_uv.obj", contents);
	const ObjMeshData data = readObjMeshData(path, quietOptions());
	removeFile(path);

	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(1.0));
	auto fallback = std::make_shared<Lambertian>(Color(0.2, 0.2, 0.2));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;

	auto mesh = std::make_shared<Mesh>(buildObjMesh(
		data,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		fallback
	));

	// Identity instance: UVs interpolate from the source face.
	auto identity = std::make_shared<MeshInstance>(
		mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		fallback
	);
	Ray uvRay(Vector3(0.25, 0.25, 5.0), Vector3(0.0, 0.0, -1.0));
	HitRecord uvHit;

	TASK_CHECK(identity->hit(uvRay, uvHit, 0.001, 100.0));
	TASK_CHECK(near(uvHit.u, 0.25, 1e-6));
	TASK_CHECK(near(uvHit.v, 0.25, 1e-6));
	TASK_CHECK(near(uvHit.t0, 4.5, 1e-6));
	TASK_CHECK(uvHit.material == bulbMaterial.get());
	// Smooth normal points mostly along -x/+z in local space, not along the
	// geometric normal (-1,-1,1)/sqrt(3).
	TASK_CHECK(std::fabs(uvHit.normal.getX()) > 0.4);
	TASK_CHECK(std::fabs(uvHit.normal.getY()) < 1e-6);

	// Non-uniform scale: geometric normal must be the inverse-transpose of the
	// local face normal, and the hit distance must be scaled accordingly.
	const Vector3 scale(2.0, 1.0, 3.0);
	auto stretched = std::make_shared<MeshInstance>(
		mesh,
		Vector3(),
		Vector3(),
		scale,
		bindings,
		fallback
	);
	Ray stretchedRay(Vector3(0.5, 0.25, 10.0), Vector3(0.0, 0.0, -1.0));
	HitRecord stretchedHit;

	TASK_CHECK(stretched->hit(stretchedRay, stretchedHit, 0.001, 100.0));
	TASK_CHECK(stretchedHit.material == bulbMaterial.get());

	const Vector3 v0(0.0, 0.0, 0.0);
	const Vector3 v1(1.0 * scale.getX(), 0.0, 1.0 * scale.getZ());
	const Vector3 v2(0.0, 1.0 * scale.getY(), 1.0 * scale.getZ());
	Vector3 worldGeometric = Utilities::cross(v1 - v0, v2 - v0);
	worldGeometric = Utilities::normalize(worldGeometric);
	TASK_CHECK(
		std::fabs(Utilities::dot(worldGeometric, stretchedHit.geometricNormal)) > 1.0 - 1e-6
	);
	// Smooth normal is not parallel to the transformed geometric normal.
	TASK_CHECK(
		std::fabs(Utilities::dot(worldGeometric, stretchedHit.normal)) < 1.0 - 1e-3
	);
	TASK_CHECK(near(stretchedRay.pointAtRay(stretchedHit.t0).getZ(), 1.5, 1e-6));
	return (0);
}

// ---------------------------------------------------------------------------
// distribution
// ---------------------------------------------------------------------------

static int	distribution_light_mass_uses_area_times_luminance(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = built.shade;
	bindings["Frame"] = built.frame;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);

	const double expectedMass = slotArea(built, "Bulb") * 6.0;

	TASK_CHECK(expectedMass > 0.0);
	TASK_CHECK(near(instance->lightSelectionWeight(), expectedMass, 1e-6));
	return (0);
}

static int	distribution_sample_light_pdf_matches_hit_density(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = built.shade;
	bindings["Frame"] = built.frame;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	const Vector3 origin(0.0, 4.0, 0.0);

	// Straight down at x=0 hits the non-emissive Shade quad.
	TASK_CHECK(near(instance->pdfValue(origin, Vector3(0.0, -1.0, 0.0)), 0.0, 1e-12));
	// Straight down over the Bulb face has a positive emission density: face
	// area cancels, leaving (Y / sum(Aj*Yj)) * d^2 / |cos|.
	const double bulbLuminance = luminance(bulbMaterial->emitted());
	const double bulbArea = slotArea(built, "Bulb");
	const double totalMass = bulbArea * bulbLuminance;
	// A downward ray from y=4 to the bulb at y=0 has d=4 and |cos|=1.
	const double expectedBulbPdf = (bulbLuminance / totalMass) * 16.0;
	TASK_CHECK(near(
		instance->pdfValue(Vector3(4.0, 4.0, 0.0), Vector3(0.0, -1.0, 0.0)),
		expectedBulbPdf,
		1e-9
	));
	// The density depends on the *other* emissive faces through sum(Aj*Yj),
	// not on the whole mesh area nor on the hit face's own area. Making the
	// Frame slot emissive increases the sum, so the density at the same Bulb
	// face must drop by exactly that ratio while Yi and Ai are unchanged.
	{
		auto frameLight = std::make_shared<Emissive>(emissiveLuminanceColor(21.0));
		MaterialBindings mixedBindings = bindings;
		mixedBindings["Frame"] = frameLight;
		auto mixedInstance = std::make_shared<MeshInstance>(
			built.mesh,
			Vector3(),
			Vector3(),
			Vector3(1.0, 1.0, 1.0),
			mixedBindings,
			built.fallback
		);
		const double frameMass = slotArea(built, "Frame") * 21.0;
		const double mixedSum = totalMass + frameMass;
		const double mixedPdf = mixedInstance->pdfValue(
			Vector3(4.0, 4.0, 0.0),
			Vector3(0.0, -1.0, 0.0)
		);
		// (Y/sumMixed)*d^2 vs (Y/sumBulb)*d^2 => ratio = sumBulb/sumMixed.
		TASK_CHECK(near(
			mixedPdf,
			expectedBulbPdf * (totalMass / mixedSum),
			1e-9
		));
		// Uniformly scaling every emissive face's luminance leaves Yi/sum
		// unchanged.
		TASK_CHECK(near(mixedInstance->lightSelectionWeight(), mixedSum, 1e-6));
	}

	Sampler::setRenderSeed(11);
	for (std::uint32_t sample = 0; sample < 32; sample++)
	{
		Sampler::beginPixelSample(sample, 0, sample);
		HittableLightSample lightSample;

		TASK_CHECK(instance->sampleLight(origin, lightSample));
		TASK_CHECK(lightSample.material == bulbMaterial.get());
		const double hitPdf = instance->pdfValue(origin, lightSample.direction);
		TASK_CHECK(near(hitPdf, lightSample.pdf, 1e-6));
		Sampler::endPixelSample();
	}
	return (0);
}

static int	distribution_sample_frequency_follows_mass(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(9.0));
	auto frameMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(3.0));
	auto shadeMaterial = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Frame"] = frameMaterial;
	bindings["Shade"] = shadeMaterial;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);

	const double bulbMass = slotArea(built, "Bulb") * 9.0;
	const double frameMass = slotArea(built, "Frame") * 3.0;
	const double totalMass = bulbMass + frameMass;
	const double expectedBulbFraction = bulbMass / totalMass;

	TASK_CHECK(bulbMass > 0.0);
	TASK_CHECK(frameMass > 0.0);
	TASK_CHECK(near(instance->lightSelectionWeight(), totalMass, 1e-6));

	const std::size_t sampleCount = 2000;
	std::size_t bulbSamples = 0;
	const Vector3 origin(0.0, 4.0, 0.0);

	Sampler::setRenderSeed(23);
	for (std::uint32_t sample = 0; sample < sampleCount; sample++)
	{
		Sampler::beginPixelSample(sample, 0, sample);
		HittableLightSample lightSample;

		TASK_CHECK(instance->sampleLight(origin, lightSample));
		TASK_CHECK(lightSample.material == bulbMaterial.get()
			|| lightSample.material == frameMaterial.get());
		TASK_CHECK(lightSample.material != shadeMaterial.get());
		TASK_CHECK(lightSample.pdf > 0.0);
		if (lightSample.material == bulbMaterial.get())
		{
			bulbSamples++;
		}
		Sampler::endPixelSample();
	}
	const double observedBulbFraction = static_cast<double>(bulbSamples) / static_cast<double>(sampleCount);

	TASK_CHECK(std::fabs(observedBulbFraction - expectedBulbFraction) < 0.05);
	return (0);
}

static int	distribution_emission_power_uses_face_mass(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = built.shade;
	bindings["Frame"] = built.frame;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	const double totalMass = slotArea(built, "Bulb") * 6.0;

	TASK_CHECK(near(totalMass, instance->lightSelectionWeight(), 1e-6));

	// Emitted power. The direction distribution stays the existing two-sided
	// cosine lobe, but the sample now represents face i under the mass
	// distribution, so powerScale = 2*pi/(Yi/sum(Aj*Yj)). Therefore
	// powerScale*Y/(2*pi) is sum(Aj*Yj) for every sample, regardless of which
	// face was selected — this is the discriminating check that would fail if
	// the whole-mesh area or a single face's area were used instead.
	Sampler::setRenderSeed(31);
	for (std::uint32_t sample = 0; sample < 64; sample++)
	{
		Sampler::beginPixelSample(sample, 0, sample);
		HittableEmissionSample emissionSample;

		TASK_CHECK(instance->sampleEmission(emissionSample));
		TASK_CHECK(emissionSample.valid);
		TASK_CHECK(near(
			Utilities::luminance(emissionSample.emitted),
			luminance(bulbMaterial->emitted()),
			1e-9
		));
		TASK_CHECK(emissionSample.powerScale > 0.0);
		TASK_CHECK(near(
			emissionSample.powerScale * luminance(emissionSample.emitted)
				/ (2.0 * 3.14159265358979323846),
			totalMass,
			1e-6
		));
		Sampler::endPixelSample();
	}

	// For a single-face bulb the emission power is exactly
	// 2*pi / (Y / (A*Y)) = 2*pi*A.
	const std::string singleFace =
		"v 0 0 0\nv 2 0 0\nv 0 2 0\n"
		"usemtl Emit\n"
		"f 1 2 3\n";
	const std::string singlePath = writeTextFile("luz_material_slots_single.obj", singleFace);
	const ObjMeshData singleData = readObjMeshData(singlePath, quietOptions());
	removeFile(singlePath);
	auto emitter = std::make_shared<Emissive>(emissiveLuminanceColor(5.0));
	MaterialBindings singleBindings;
	singleBindings["Emit"] = emitter;
	auto singleMesh = std::make_shared<Mesh>(buildObjMesh(
		singleData,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		singleBindings,
		fallbackMaterial()
	));
	auto singleInstance = std::make_shared<MeshInstance>(
		singleMesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		singleBindings,
		fallbackMaterial()
	);

	Sampler::setRenderSeed(37);
	Sampler::beginPixelSample(0, 0, 0);
	HittableEmissionSample singleSample;
	TASK_CHECK(singleInstance->sampleEmission(singleSample));
	TASK_CHECK(near(singleSample.powerScale, 2.0 * 3.14159265358979323846 * 2.0, 1e-6));
	Sampler::endPixelSample();
	return (0);
}

static int	distribution_random_selects_emissive_faces(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	auto dark = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = dark;
	bindings["Frame"] = dark;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	const Vector3 origin(0.0, -3.0, 0.0);

	Sampler::setRenderSeed(71);
	std::size_t emissiveHits = 0;
	for (std::uint32_t sample = 0; sample < 64; sample++)
	{
		Sampler::beginPixelSample(sample, 0, sample);
		const Vector3 direction = instance->random(origin);
		Ray ray(origin, direction);
		HitRecord hitRecord;

		TASK_CHECK(instance->hit(ray, hitRecord, 0.001, 100.0));
		TASK_CHECK(hitRecord.material == bulbMaterial.get());
		emissiveHits++;
		Sampler::endPixelSample();
	}
	TASK_CHECK(emissiveHits == 64);
	return (0);
}

static int	distribution_nonuniform_scale_recomputes_face_mass(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = built.shade;
	bindings["Frame"] = built.frame;

	auto uniform = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	// Non-uniform scale changes each face's world area independently.
	auto stretched = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(4.0, 1.0, 2.0),
		bindings,
		built.fallback
	);

	TASK_CHECK(stretched->lightSelectionWeight() > uniform->lightSelectionWeight());

	// Recompute the expected stretched mass per bulb face from the world
	// vertices to make sure the per-face areas are not uniform-scaled.
	double expectedMass = 0.0;
	for (std::size_t index = 0; index < built.mesh->surfaceElementCount(); index++)
	{
		const int slot = built.mesh->getTriangleMaterialSlot(index);
		const std::string& name = built.mesh->getMaterialSlotNames()[static_cast<std::size_t>(slot)];

		if (name != "Bulb")
		{
			continue;
		}
		const double area = built.mesh->transformedSurfaceElementArea(
			index,
			[](const Vector3& point) {
				return (Vector3(point.getX() * 4.0, point.getY(), point.getZ() * 2.0));
			}
		);
		expectedMass += area * 6.0;
	}
	TASK_CHECK(expectedMass > 0.0);
	TASK_CHECK(near(stretched->lightSelectionWeight(), expectedMass, 1e-5));
	return (0);
}

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------

static int	transport_dark_mesh_is_not_registered_as_light(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto dark = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = dark;
	bindings["Shade"] = dark;
	bindings["Frame"] = dark;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	Scene scene;

	scene.addHittable(instance);
	scene.updateLights();
	TASK_CHECK(scene.getLights().empty());
	TASK_CHECK(near(instance->lightSelectionWeight(), 0.0));

	HittableEmissionSample emissionSample;
	TASK_CHECK(!instance->sampleEmission(emissionSample));
	HittableLightSample lightSample;
	TASK_CHECK(!instance->sampleLight(Vector3(0.0, 4.0, 0.0), lightSample));
	return (0);
}

static int	transport_scene_lights_use_per_face_material(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	auto dark = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = dark;
	bindings["Frame"] = dark;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	Scene scene;

	scene.addHittable(instance);
	scene.updateLights();

	TASK_CHECK(scene.getLights().size() == 1);
	TASK_CHECK(scene.getLightSelectionTotalWeight() > 0.0);
	TASK_CHECK(near(
		scene.getLightSelectionTotalWeight(),
		slotArea(built, "Bulb") * 6.0,
		1e-6
	));

	Sampler::setRenderSeed(41);
	Sampler::beginPixelSample(0, 0, 0);
	HittableLightSample lightSample;

	TASK_CHECK(scene.getLights()[0]->sampleLight(Vector3(0.0, 4.0, 0.0), lightSample));
	TASK_CHECK(lightSample.material == bulbMaterial.get());
	TASK_CHECK(lightSample.pdf > 0.0);
	Sampler::endPixelSample();
	return (0);
}

static int	transport_direct_lighting_uses_mixed_mesh_material(void)
{
	// Two floors and one shadowing setup:
	//  - The emissive mesh is a real scene light (scene.getLights()).
	//  - A white diffuse floor is lit only by that mesh, so the resulting color
	//    must carry the bulb slot's emitted color (never the fallback and never
	//    one of the dark slots).
	//  - A large opaque dark triangle between the floor and the light must make
	//    the same direct-light estimate zero; the shadow ray is not allowed to
	//    pass through it.
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(Color(3.0, 1.5, 0.75));
	auto dark = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	auto whiteFloor = std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0));
	auto blackOccluder = std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = dark;
	bindings["Frame"] = dark;

	auto makeLightInstance = [&]() {
		return (std::make_shared<MeshInstance>(
			built.mesh,
			Vector3(0.0, 3.0, 0.0),
			Vector3(),
			Vector3(1.0, 1.0, 1.0),
			bindings,
			built.fallback
		));
	};
	auto makeFloor = [&]() {
		return (std::make_shared<Triangle>(
			Vector3(-4.0, 0.0, -4.0),
			Vector3(4.0, 0.0, -4.0),
			Vector3(-4.0, 0.0, 4.0),
			whiteFloor
		));
	};
	const Ray cameraRay(Vector3(0.5, 1.0, 0.5), Vector3(-0.5, -1.0, -0.5));

	auto renderOnce = [&](Scene& scene) {
		Color accumulated(0.0, 0.0, 0.0);

		Sampler::beginPixelSample(0, 0, 0);
		accumulated = Renderer::internal::_calculateLightRaysColor(cameraRay, scene);
		Sampler::endPixelSample();
		return (accumulated);
	};

	// Unoccluded: the floor is lit by the bulb slot.
	Scene openScene;
	openScene.setRenderSky(SKY_NONE);
	openScene.setBackgroundColor(Color(0.0, 0.0, 0.0));
	openScene.setMaxLightBounces(1);
	openScene.addHittable(makeLightInstance());
	openScene.addHittable(makeFloor());
	openScene.updateLights();
	openScene.updateAccelerationStructure();

	TASK_CHECK(openScene.getLights().size() == 1);
	const double openWeight = openScene.getLightSelectionTotalWeight();
	TASK_CHECK(openWeight > 0.0);

	Sampler::setRenderSeed(47);
	const Color litColor = renderOnce(openScene);
	const double litLuminance = Utilities::luminance(litColor);

	TASK_CHECK(litLuminance > 0.0);
	// The emitted color is (3, 1.5, 0.75): the lit result must keep that warm
	// ordering, which proves the bulb slot's material was used.
	TASK_CHECK(litColor.getRed() > litColor.getGreen());
	TASK_CHECK(litColor.getGreen() > litColor.getBlue());
	TASK_CHECK(litColor.getRed() > 0.0);

	// Occluded: a large dark triangle at y=2 blocks every shadow ray from the
	// floor to the light. The scene is otherwise identical.
	Scene shadowedScene;
	shadowedScene.setRenderSky(SKY_NONE);
	shadowedScene.setBackgroundColor(Color(0.0, 0.0, 0.0));
	shadowedScene.setMaxLightBounces(1);
	shadowedScene.addHittable(makeLightInstance());
	shadowedScene.addHittable(makeFloor());
	shadowedScene.addHittable(std::make_shared<Cube>(
		Transform(Vector3(0.0, 2.0, 0.0), Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)),
		8.0,
		0.2,
		8.0,
		blackOccluder
	));
	shadowedScene.updateLights();
	shadowedScene.updateAccelerationStructure();

	Sampler::setRenderSeed(47);
	const Color shadowedColor = renderOnce(shadowedScene);

	TASK_CHECK(near(Utilities::luminance(shadowedColor), 0.0, 1e-9));
	return (0);
}

static int	transport_emission_path_carries_face_value(void)
{
	BuiltMultiSlotMesh built = buildMultiSlotMesh(Vector3(), Vector3(1.0, 1.0, 1.0));
	auto bulbMaterial = std::make_shared<Emissive>(emissiveLuminanceColor(6.0));
	auto dark = std::make_shared<Emissive>(Color(0.0, 0.0, 0.0));
	MaterialBindings bindings;

	bindings["Bulb"] = bulbMaterial;
	bindings["Shade"] = dark;
	bindings["Frame"] = dark;

	auto instance = std::make_shared<MeshInstance>(
		built.mesh,
		Vector3(),
		Vector3(),
		Vector3(1.0, 1.0, 1.0),
		bindings,
		built.fallback
	);
	Scene scene;

	scene.addHittable(instance);
	scene.updateLights();
	TASK_CHECK(scene.getLights().size() == 1);

	Sampler::setRenderSeed(53);
	Sampler::beginPixelSample(0, 0, 0);
	HittableEmissionSample emissionSample;

	TASK_CHECK(scene.getLights()[0]->sampleEmission(emissionSample));
	TASK_CHECK(emissionSample.valid);
	TASK_CHECK(emissionSample.emitted.getRed() > 0.0);
	TASK_CHECK(near(
		Utilities::luminance(emissionSample.emitted),
		luminance(bulbMaterial->emitted()),
		1e-9
	));
	TASK_CHECK(emissionSample.powerScale > 0.0);
	Sampler::endPixelSample();
	return (0);
}

// ---------------------------------------------------------------------------
// .luz object blocks
// ---------------------------------------------------------------------------

static int	transport_luz_object_block_slot_bindings_load(void)
{
	const std::filesystem::path directory = std::filesystem::temp_directory_path();
	const std::string objectPath = (directory / "luz_material_slots_scene.obj").string();
	const std::string scenePath = (directory / "luz_material_slots_scene.luz").string();
	{
		std::ofstream objectStream(objectPath);
		objectStream << multiSlotObjContents();
	}
	{
		std::ofstream sceneStream(scenePath);
		sceneStream
			<< "[materials]\n"
			<< "material shade_mat {\n"
			<< "type=lambertian\n"
			<< "color=(0.1,0.2,0.3)\n"
			<< "}\n"
			<< "material bulb_mat {\n"
			<< "type=emissive\n"
			<< "color=(1,1,1)\n"
			<< "radiance=4\n"
			<< "}\n"
			<< "material frame_mat {\n"
			<< "type=emissive\n"
			<< "color=(0,0,0)\n"
			<< "radiance=0\n"
			<< "}\n"
			<< "material fallback_mat {\n"
			<< "type=lambertian\n"
			<< "color=(0.5,0.5,0.5)\n"
			<< "}\n\n"
			<< "[meshes]\n"
			<< "mesh lamp_source {\n"
			<< "file=" << std::filesystem::path(objectPath).filename().string() << "\n"
			<< "}\n\n"
			<< "[scene]\n"
			<< "object lamp_a {\n"
			<< "mesh=lamp_source\n"
			<< "position=(0,0,0)\n"
			<< "material=fallback_mat\n"
			<< "material.Bulb=bulb_mat\n"
			<< "material.Shade=shade_mat\n"
			<< "}\n"
			<< "object lamp_b {\n"
			<< "file=" << std::filesystem::path(objectPath).filename().string() << "\n"
			<< "position=(4,0,0)\n"
			<< "material=fallback_mat\n"
			<< "material.bulb=frame_mat\n"
			<< "}\n";
	}

	Scene scene;
	SceneFile::read(scene, scenePath);
	removeFile(objectPath);
	removeFile(scenePath);

	TASK_CHECK(scene.getHittables().size() == 2);
	scene.updateLights();

	// lamp_a emits through its Bulb slot; lamp_b binds a dark material to a
	// case-different slot name that does not exist, so its Bulb slot uses the
	// fallback (non emissive) and contributes no light.
	TASK_CHECK(scene.getLights().size() == 1);
	TASK_CHECK(scene.getLights()[0].get() == scene.getHittables()[0].get());

	// lamp_a's Bulb face resolves to the emissive bulb material.
	Ray bulbRay(Vector3(4.0, 2.0, 0.0), Vector3(0.0, -1.0, 0.0));
	HitRecord lampAHit;
	TASK_CHECK(scene.getHittables()[0]->hit(bulbRay, lampAHit, 0.001, 100.0));
	TASK_CHECK(lampAHit.material != nullptr);
	TASK_CHECK(lampAHit.material->getType() == EMISSIVE);
	TASK_CHECK(lampAHit.materialSlot >= 0);

	// lamp_b keeps its own fallback for the same slot; the shared geometry did
	// not inherit lamp_a's bindings.
	Ray lampBRay(Vector3(8.0, 2.0, 0.0), Vector3(0.0, -1.0, 0.0));
	HitRecord lampBHit;
	TASK_CHECK(scene.getHittables()[1]->hit(lampBRay, lampBHit, 0.001, 100.0));
	TASK_CHECK(lampBHit.material != nullptr);
	TASK_CHECK(lampBHit.material->getType() != EMISSIVE);
	TASK_CHECK(lampBHit.material != lampAHit.material);

	// Shade slot bound to a lambertian in lamp_a.
	Ray shadeRay(Vector3(0.0, 2.0, 0.0), Vector3(0.0, -1.0, 0.0));
	HitRecord shadeHit;
	TASK_CHECK(scene.getHittables()[0]->hit(shadeRay, shadeHit, 0.001, 100.0));
	TASK_CHECK(shadeHit.material != nullptr);
	TASK_CHECK(shadeHit.material->getType() == LAMBERTIAN);

	// Both instances resolve their own light sample; the emissive one never
	// reports the fallback.
	Sampler::setRenderSeed(59);
	Sampler::beginPixelSample(0, 0, 0);
	HittableLightSample lightSample;
	TASK_CHECK(scene.getLights()[0]->sampleLight(Vector3(0.0, 4.0, 0.0), lightSample));
	TASK_CHECK(lightSample.valid);
	TASK_CHECK(lightSample.material->getType() == EMISSIVE);
	TASK_CHECK(near(Utilities::luminance(lightSample.material->emitted()), 4.0, 1e-6));
	Sampler::endPixelSample();
	return (0);
}

static int	transport_luz_unknown_named_material_fails(void)
{
	const std::filesystem::path directory = std::filesystem::temp_directory_path();
	const std::string objectPath = (directory / "luz_material_slots_bad.obj").string();
	const std::string scenePath = (directory / "luz_material_slots_bad.luz").string();
	{
		std::ofstream objectStream(objectPath);
		objectStream
			<< "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
			<< "usemtl Bulb\n"
			<< "f 1 2 3\n";
	}
	{
		std::ofstream sceneStream(scenePath);
		sceneStream
			<< "[scene]\n"
			<< "object broken {\n"
			<< "file=" << std::filesystem::path(objectPath).filename().string() << "\n"
			<< "material.Bulb=missing_material\n"
			<< "}\n";
	}

	Scene scene;
	bool threw = false;
	try
	{
		SceneFile::read(scene, scenePath);
	}
	catch (const std::exception&)
	{
		threw = true;
	}
	removeFile(objectPath);
	removeFile(scenePath);
	TASK_CHECK(threw);
	return (0);
}

static int	transport_luz_compact_obj_slot_bindings(void)
{
	const std::filesystem::path directory = std::filesystem::temp_directory_path();
	const std::string objectPath = (directory / "luz_material_slots_compact.obj").string();
	const std::string scenePath = (directory / "luz_material_slots_compact.luz").string();
	{
		std::ofstream objectStream(objectPath);
		objectStream
			<< "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
			<< "usemtl Bulb\n"
			<< "f 1 2 3\n";
	}
	{
		std::ofstream sceneStream(scenePath);
		sceneStream
			<< "[materials]\n"
			<< "material bulb_mat {\n"
			<< "type=emissive\n"
			<< "color=(1,1,1)\n"
			<< "radiance=2\n"
			<< "}\n"
			<< "material fallback_mat {\n"
			<< "type=lambertian\n"
			<< "color=(0.4,0.4,0.4)\n"
			<< "}\n\n"
			<< "[scene]\n"
			<< "objects{\n"
			<< "obj=" << std::filesystem::path(objectPath).filename().string()
			<< ",(0,0,0),material=fallback_mat,material.Bulb=bulb_mat\n"
			<< "}\n";
	}

	Scene scene;
	SceneFile::read(scene, scenePath);
	removeFile(objectPath);
	removeFile(scenePath);

	TASK_CHECK(scene.getHittables().size() == 1);
	scene.updateLights();
	TASK_CHECK(scene.getLights().size() == 1);

	// The compact form always creates a shared-geometry MeshInstance; verify
	// the bound Bulb slot produced the emissive light.
	Ray bulbRay(Vector3(0.25, 0.25, 2.0), Vector3(0.0, 0.0, -1.0));
	HitRecord hitRecord;
	TASK_CHECK(scene.getHittables()[0]->hit(bulbRay, hitRecord, 0.001, 100.0));
	TASK_CHECK(hitRecord.material != nullptr);
	TASK_CHECK(hitRecord.material->getType() == EMISSIVE);
	TASK_CHECK(near(Utilities::luminance(hitRecord.material->emitted()), 2.0, 1e-6));
	return (0);
}

// ---------------------------------------------------------------------------

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
	{"binding", "obj_reader_preserves_slots", binding_obj_reader_preserves_slots_and_triangles},
	{"binding", "unlabelled_faces_use_empty_slot", binding_obj_reader_assigns_unlabelled_faces_to_empty_slot},
	{"binding", "degenerate_faces_filtered_slots_kept", binding_build_mesh_filters_degenerate_and_keeps_slots},
	{"binding", "instances_share_geometry_opposite_bindings", binding_two_instances_share_geometry_with_opposite_bindings},
	{"binding", "legacy_overload_unchanged", binding_legacy_single_material_overload_is_unchanged},
	{"binding", "many_faces_survive_bvh_reorder", binding_many_faces_survive_bvh_reorder},
	{"binding", "slot_bindings_reject_zero_scale", binding_slot_bindings_reject_zero_scale},
	{"binding", "rotated_nonuniform_slots", binding_rotated_and_nonuniform_scaled_instances_keep_slots},
	{"binding", "hit_uv_normals_and_scale", binding_hit_uv_normals_and_scale},
	{"distribution", "light_mass_area_times_luminance", distribution_light_mass_uses_area_times_luminance},
	{"distribution", "random_selects_emissive_faces", distribution_random_selects_emissive_faces},
	{"distribution", "sample_light_pdf_matches_hit_density", distribution_sample_light_pdf_matches_hit_density},
	{"distribution", "sample_frequency_follows_mass", distribution_sample_frequency_follows_mass},
	{"distribution", "emission_power_uses_face_mass", distribution_emission_power_uses_face_mass},
	{"distribution", "nonuniform_scale_recomputes_mass", distribution_nonuniform_scale_recomputes_face_mass},
	{"transport", "dark_mesh_not_a_light", transport_dark_mesh_is_not_registered_as_light},
	{"transport", "scene_lights_use_face_material", transport_scene_lights_use_per_face_material},
	{"transport", "direct_lighting_uses_mixed_material", transport_direct_lighting_uses_mixed_mesh_material},
	{"transport", "emission_path_carries_face_value", transport_emission_path_carries_face_value},
	{"transport", "luz_object_block_slot_bindings", transport_luz_object_block_slot_bindings_load},
	{"transport", "luz_unknown_named_material_fails", transport_luz_unknown_named_material_fails},
	{"transport", "luz_compact_obj_slot_bindings", transport_luz_compact_obj_slot_bindings},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
