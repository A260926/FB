#pragma once

#include "Hittables/Mesh.hpp"
#include "Materials/MaterialBindings.hpp"
#include "TerminalProgress.hpp"
#include "Vector3.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct	ObjMeshFaceVertex
{
	int	vertexIndex = -1;
	int	textureIndex = -1;
	int	normalIndex = -1;
};

struct	ObjMeshData
{
	std::vector<Vector3>	vertices;
	std::vector<Vector3>	textureCoordinates;
	std::vector<Vector3>	normals;
	std::vector<std::array<ObjMeshFaceVertex, 3>>	triangles;
	// Slot names in first-seen order. Faces without a preceding "usemtl" use the
	// empty-name slot.
	std::vector<std::string>	materialSlotNames;
	// Parallel to 'triangles': index into 'materialSlotNames'.
	std::vector<int>	triangleMaterialSlots;

	int	materialSlotForTriangle(std::size_t triangleIndex) const
	{
		if (triangleIndex >= this->triangleMaterialSlots.size())
		{
			return (-1);
		}
		return (this->triangleMaterialSlots[triangleIndex]);
	}

	const std::string*	materialSlotName(int slotIndex) const
	{
		if (slotIndex < 0 || static_cast<std::size_t>(slotIndex) >= this->materialSlotNames.size())
		{
			return (nullptr);
		}
		return (&this->materialSlotNames[static_cast<std::size_t>(slotIndex)]);
	}
};

struct	ObjLoadProgress
{
	std::size_t	total = 0;
	std::size_t	loaded = 0;
	std::size_t	skippedDegenerateTriangles = 0;
	bool		started = false;
	bool		outputEnabled = true;
	double		elapsedMS = 0.0;
	std::chrono::steady_clock::time_point	startTime;
	std::unique_ptr<TerminalProgress::PhaseProgress>	reporter;
	std::mutex	mutex;
};

struct	ObjReadOptions
{
	bool	quiet = false;
	ObjLoadProgress*	progress = nullptr;
};

ObjMeshData	readObjMeshData(std::string fileName);
ObjMeshData	readObjMeshData(std::string fileName, const ObjReadOptions& options);
Mesh	buildObjMesh(
	const ObjMeshData& data,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material
);
// Resolves every triangle's material slot through 'materialBindings'. Slots
// that are not present in the table use 'fallbackMaterial'. The returned Mesh
// keeps the slot names and per-triangle slot association.
Mesh	buildObjMesh(
	const ObjMeshData& data,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	const MaterialBindings& materialBindings,
	std::shared_ptr<Material> fallbackMaterial
);
Mesh	readObj(std::string fileName);
Mesh	readObj(std::string fileName, Vector3 positionOffset, std::shared_ptr<Material> material);
Mesh	readObj(std::string fileName, Vector3 positionOffset, Vector3 rotationDegrees, Vector3 scale, std::shared_ptr<Material> material);
Mesh	readObj(
	std::string fileName,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material,
	const ObjReadOptions& options
);
