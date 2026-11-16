#include "Hittables/Mesh.hpp"
#include "Utilities.hpp"
#include "Materials/Lambertian.hpp"
#include "Defaults.hpp"
#include "Sampler.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>

namespace
{
	constexpr std::size_t	MESH_BVH_LEAF_SIZE = 8;
	constexpr std::size_t	MESH_BVH_STACK_SIZE = 128;
	constexpr std::size_t	MESH_BVH_SAH_BINS = 12;
	constexpr double			MESH_BVH_CENTROID_EPSILON = 1e-12;

	struct	TraversalNode
	{
		std::size_t	index;
		double		near;
	};

	struct	BinnedSplit
	{
		bool		found = false;
		int			axis = 0;
		std::size_t	splitBin = 0;
		double		centroidMin = 0.0;
		double		binScale = 0.0;
	};

	struct	Bin
	{
		AABB		boundingBox;
		std::size_t	count = 0;
		bool		hasBox = false;
	};

	double	boxMinimumAxis(const AABB& box, int axis)
	{
		return (box.getMinimum()[axis]);
	}

	Vector3	boxCentroid(const AABB& box)
	{
		return ((box.getMinimum() + box.getMaximum()) / 2.0);
	}

	AABB	mergeBoxes(const AABB& first, const AABB& second)
	{
		return (AABB(
			Vector3(
				std::min(first.getMinimum().getX(), second.getMinimum().getX()),
				std::min(first.getMinimum().getY(), second.getMinimum().getY()),
				std::min(first.getMinimum().getZ(), second.getMinimum().getZ())
			),
			Vector3(
				std::max(first.getMaximum().getX(), second.getMaximum().getX()),
				std::max(first.getMaximum().getY(), second.getMaximum().getY()),
				std::max(first.getMaximum().getZ(), second.getMaximum().getZ())
			)
		));
	}

	void	addBoxToBin(Bin& bin, const AABB& box)
	{
		bin.boundingBox = bin.hasBox ? mergeBoxes(bin.boundingBox, box) : box;
		bin.count++;
		bin.hasBox = true;
	}

	void	mergeBin(Bin& target, const Bin& source)
	{
		if (!source.hasBox)
		{
			return;
		}
		target.boundingBox = target.hasBox
			? mergeBoxes(target.boundingBox, source.boundingBox)
			: source.boundingBox;
		target.count += source.count;
		target.hasBox = true;
	}

	double	boxSurfaceArea(const AABB& box)
	{
		const Vector3 extent = box.getMaximum() - box.getMinimum();
		const double x = std::max(0.0, extent.getX());
		const double y = std::max(0.0, extent.getY());
		const double z = std::max(0.0, extent.getZ());

		return (2.0 * ((x * y) + (x * z) + (y * z)));
	}

	bool	boxHitDistance(const AABB& box, Ray& ray, double tMax, double& tNear)
	{
		double tMin = T_MIN;
		const Vector3& minimum = box.getMinimum();
		const Vector3& maximum = box.getMaximum();
		const Vector3& origin = ray.getOrigin();
		const Vector3& direction = ray.getDirection();
		const Vector3& inverseDirection = ray.getInverseDirection();

		if (direction[0] == 0.0 && direction[1] == 0.0 && direction[2] == 0.0)
		{
			return (false);
		}
		for (int axis = 0; axis < 3; axis++)
		{
			const double invD = inverseDirection[axis];
			if (invD == 0.0)
			{
				if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
				{
					return (false);
				}
				continue;
			}
			double t0 = (minimum[axis] - origin[axis]) * invD;
			double t1 = (maximum[axis] - origin[axis]) * invD;
			if (invD < 0.0)
			{
				std::swap(t0, t1);
			}

			tMin = t0 > tMin ? t0 : tMin;
			tMax = t1 < tMax ? t1 : tMax;
			if (tMax <= tMin)
			{
				return (false);
			}
		}

		tNear = tMin;
		return (true);
	}

	AABB	boundingBoxForRange(
		const std::vector<AABB>& boxes,
		const std::vector<std::size_t>& indices,
		std::size_t start,
		std::size_t end
	)
	{
		AABB result;
		bool hasBox = false;

		for (std::size_t i = start; i < end; i++)
		{
			const AABB& box = boxes[indices[i]];

			result = hasBox ? mergeBoxes(result, box) : box;
			hasBox = true;
		}

		return (result);
	}

	int	largestExtentAxis(const AABB& box)
	{
		const Vector3 extent = box.getMaximum() - box.getMinimum();

		if (extent.getX() >= extent.getY() && extent.getX() >= extent.getZ())
		{
			return (0);
		}
		if (extent.getY() >= extent.getZ())
		{
			return (1);
		}
		return (2);
	}

	std::size_t	binForCentroid(double centroid, double minimum, double scale)
	{
		std::size_t bin = static_cast<std::size_t>((centroid - minimum) * scale);

		if (bin >= MESH_BVH_SAH_BINS)
		{
			bin = MESH_BVH_SAH_BINS - 1;
		}
		return (bin);
	}

	std::size_t	packedBVHNodeReserve(std::size_t triangleCount)
	{
		if (triangleCount == 0)
		{
			return (0);
		}

		const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
		const std::size_t leafCount = (triangleCount + MESH_BVH_LEAF_SIZE - 1) / MESH_BVH_LEAF_SIZE;
		std::size_t estimatedNodeCount = maxSize;
		if (leafCount <= maxSize / 2)
		{
			estimatedNodeCount = (leafCount * 2) - 1;
		}

		const std::size_t slack = std::max<std::size_t>(1024, estimatedNodeCount / 4);
		std::size_t reserve = maxSize;
		if (estimatedNodeCount <= maxSize - slack)
		{
			reserve = estimatedNodeCount + slack;
		}

		const std::size_t fullBinaryUpperBound = triangleCount <= maxSize / 2
			? triangleCount * 2
			: maxSize;
		return (std::min(reserve, fullBinaryUpperBound));
	}

	BinnedSplit	findBinnedSAHSplit(
		const std::vector<AABB>& boxes,
		const std::vector<Vector3>& centroids,
		const std::vector<std::size_t>& indices,
		std::size_t start,
		std::size_t end
	)
	{
		const std::size_t triangleCount = end - start;
		const std::size_t minimumSideCount = std::max<std::size_t>(1, triangleCount / 16);
		BinnedSplit bestSplit;
		double bestCost = std::numeric_limits<double>::infinity();

		for (int axis = 0; axis < 3; axis++)
		{
			double centroidMin = std::numeric_limits<double>::infinity();
			double centroidMax = -std::numeric_limits<double>::infinity();

			for (std::size_t i = start; i < end; i++)
			{
				const double centroid = centroids[indices[i]][axis];
				centroidMin = std::min(centroidMin, centroid);
				centroidMax = std::max(centroidMax, centroid);
			}
			const double centroidExtent = centroidMax - centroidMin;
			if (centroidExtent <= MESH_BVH_CENTROID_EPSILON)
			{
				continue;
			}

			const double binScale = static_cast<double>(MESH_BVH_SAH_BINS) / centroidExtent;
			std::array<Bin, MESH_BVH_SAH_BINS> bins;
			for (std::size_t i = start; i < end; i++)
			{
				const std::size_t triangleIndex = indices[i];
				const std::size_t bin = binForCentroid(centroids[triangleIndex][axis], centroidMin, binScale);

				addBoxToBin(bins[bin], boxes[triangleIndex]);
			}

			std::array<Bin, MESH_BVH_SAH_BINS> leftBins;
			std::array<Bin, MESH_BVH_SAH_BINS> rightBins;
			Bin runningLeft;
			Bin runningRight;

			for (std::size_t i = 0; i < MESH_BVH_SAH_BINS; i++)
			{
				mergeBin(runningLeft, bins[i]);
				leftBins[i] = runningLeft;
			}
			for (std::size_t i = MESH_BVH_SAH_BINS; i > 0; i--)
			{
				const std::size_t binIndex = i - 1;

				mergeBin(runningRight, bins[binIndex]);
				rightBins[binIndex] = runningRight;
			}

			for (std::size_t splitBin = 0; splitBin + 1 < MESH_BVH_SAH_BINS; splitBin++)
			{
				const Bin& left = leftBins[splitBin];
				const Bin& right = rightBins[splitBin + 1];

				if (
					!left.hasBox
					|| !right.hasBox
					|| left.count < minimumSideCount
					|| right.count < minimumSideCount
				)
				{
					continue;
				}

				const double cost =
					(boxSurfaceArea(left.boundingBox) * static_cast<double>(left.count))
					+ (boxSurfaceArea(right.boundingBox) * static_cast<double>(right.count));
				if (cost < bestCost)
				{
					bestCost = cost;
					bestSplit.found = true;
					bestSplit.axis = axis;
					bestSplit.splitBin = splitBin;
					bestSplit.centroidMin = centroidMin;
					bestSplit.binScale = binScale;
				}
			}
		}

		return (bestSplit);
	}
}

/*
	Constructors
*/

// Constructs the Mesh with default values
Mesh::Mesh(void)
{
	this->_position = Vector3();
	this->_material = std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6));
	this->_legacyBVH = BVHNode();
	this->_totalArea = 0.0;
	this->_usesPackedTriangles = false;
	this->_usesMaterialSlots = false;
	this->_totalLightMass = 0.0;
}

// Constructs the Mesh with custom values
Mesh::Mesh(Vector3 position, std::shared_ptr<Material> material, BVHNode bvh)
{
	this->_position = position;
	this->_material = material;
	this->_legacyBVH = bvh;
	this->_totalArea = 0.0;
	this->_usesPackedTriangles = false;
	this->_usesMaterialSlots = false;
	this->_totalLightMass = 0.0;
}

// Constructs the Mesh with custom values
Mesh::Mesh(Vector3 position, std::shared_ptr<Material> material, std::vector<std::shared_ptr<Hittable>> triangles)
{
	this->_position = position;
	this->_material = material;
	this->_totalArea = 0.0;
	this->_usesPackedTriangles = false;
	this->_usesMaterialSlots = false;
	this->_totalLightMass = 0.0;

	if (this->_trySetPackedTriangles(triangles))
	{
		return;
	}

	this->_legacyTriangles = std::move(triangles);
	this->_legacyBVH = BVHNode(this->_legacyTriangles);
	this->_computeLegacyTriangleAreas();
}

Mesh::Mesh(Vector3 position, std::shared_ptr<Material> material, std::vector<Triangle> triangles)
{
	this->_position = position;
	this->_material = material;
	this->_totalArea = 0.0;
	this->_usesPackedTriangles = false;
	this->_usesMaterialSlots = false;
	this->_totalLightMass = 0.0;
	this->_setPackedTriangles(std::move(triangles));
}

Mesh::Mesh(
	Vector3 position,
	std::shared_ptr<Material> material,
	std::vector<Triangle> triangles,
	std::vector<std::string> slotNames,
	std::vector<std::shared_ptr<Material>> slotMaterials,
	std::vector<int> triangleSlots
)
{
	this->_position = position;
	this->_material = material;
	this->_totalArea = 0.0;
	this->_usesPackedTriangles = false;
	this->_usesMaterialSlots = true;
	this->_totalLightMass = 0.0;

	// Degenerate faces are dropped by the packed-triangle path. Apply the same
	// filter here so the slot association stays parallel to the stored faces.
	std::vector<Triangle> keptTriangles;
	std::vector<int> keptSlots;

	keptTriangles.reserve(triangles.size());
	keptSlots.reserve(triangles.size());
	for (std::size_t index = 0; index < triangles.size(); index++)
	{
		if (triangles[index].area() <= 0.0)
		{
			continue;
		}
		keptTriangles.push_back(std::move(triangles[index]));
		keptSlots.push_back(
			index < triangleSlots.size() ? triangleSlots[index] : -1
		);
	}

	this->_setPackedTriangles(std::move(keptTriangles));
	this->_materialSlotNames = std::move(slotNames);
	this->_materialSlotMaterials = std::move(slotMaterials);
	this->_triangleMaterialSlots = std::move(keptSlots);
	if (this->_materialSlotMaterials.size() < this->_materialSlotNames.size())
	{
		this->_materialSlotMaterials.resize(this->_materialSlotNames.size(), material);
	}
	// Ensure each stored face points at its slot's resolved material as well,
	// so plain hit records and per-face sampling agree.
	for (std::size_t index = 0; index < this->_triangleMaterialSlots.size(); index++)
	{
		const int slot = this->_triangleMaterialSlots[index];
		if (
			slot >= 0
			&& static_cast<std::size_t>(slot) < this->_materialSlotMaterials.size()
			&& this->_materialSlotMaterials[static_cast<std::size_t>(slot)]
		)
		{
			this->_triangles[index].setMaterial(
				this->_materialSlotMaterials[static_cast<std::size_t>(slot)]
			);
		}
	}
	this->_computeLightMassDistribution();
}

void	Mesh::_setPackedTriangles(std::vector<Triangle> triangles)
{
	this->_legacyTriangles.clear();
	this->_triangleBoundingBoxes.clear();
	this->_triangleCentroids.clear();
	this->_triangleIndices.clear();
	this->_packedBVHNodes.clear();
	this->_triangleAreaPrefixSums.clear();
	this->_triangles.clear();
	triangles.erase(
		std::remove_if(
			triangles.begin(),
			triangles.end(),
			[](const Triangle& triangle)
			{
				return (triangle.area() <= 0.0);
			}
		),
		triangles.end()
	);
	this->_triangles = std::move(triangles);

	this->_usesPackedTriangles = true;
	this->_computePackedTriangleAreas();
	this->_buildPackedBVH();
	this->_computeLightMassDistribution();
}

bool	Mesh::_trySetPackedTriangles(const std::vector<std::shared_ptr<Hittable>>& triangles)
{
	std::vector<Triangle> packedTriangles;

	packedTriangles.reserve(triangles.size());
	for (const std::shared_ptr<Hittable>& hittable : triangles)
	{
		const std::shared_ptr<Triangle> triangle = std::dynamic_pointer_cast<Triangle>(hittable);

		if (!triangle)
		{
			return (false);
		}
		packedTriangles.push_back(*triangle);
	}

	this->_setPackedTriangles(std::move(packedTriangles));
	return (true);
}

void	Mesh::_buildPackedBVH(void)
{
	this->_triangleBoundingBoxes.clear();
	this->_triangleCentroids.clear();
	this->_triangleIndices.clear();
	this->_packedBVHNodes.clear();

	if (this->_triangles.empty())
	{
		return;
	}

	this->_triangleBoundingBoxes.reserve(this->_triangles.size());
	this->_triangleCentroids.reserve(this->_triangles.size());
	this->_triangleIndices.resize(this->_triangles.size());
	std::iota(this->_triangleIndices.begin(), this->_triangleIndices.end(), 0);

	for (const Triangle& triangle : this->_triangles)
	{
		AABB boundingBox;

		triangle.createBoundingBox(boundingBox);
		this->_triangleBoundingBoxes.push_back(boundingBox);
		this->_triangleCentroids.push_back(boxCentroid(boundingBox));
	}

	this->_packedBVHNodes.reserve(packedBVHNodeReserve(this->_triangles.size()));
	this->_buildPackedBVHNode(0, this->_triangleIndices.size());
	std::vector<AABB>().swap(this->_triangleBoundingBoxes);
	std::vector<Vector3>().swap(this->_triangleCentroids);
	this->_packedBVHNodes.shrink_to_fit();
}

std::size_t	Mesh::_buildPackedBVHNode(std::size_t start, std::size_t end)
{
	const std::size_t nodeIndex = this->_packedBVHNodes.size();
	const std::size_t triangleCount = end - start;
	const AABB nodeBox = boundingBoxForRange(
		this->_triangleBoundingBoxes,
		this->_triangleIndices,
		start,
		end
	);

	this->_packedBVHNodes.push_back(PackedBVHNode());
	this->_packedBVHNodes[nodeIndex].boundingBox = nodeBox;
	if (nodeIndex == 0)
	{
		this->_boundingBox = nodeBox;
	}

	if (triangleCount <= MESH_BVH_LEAF_SIZE)
	{
		this->_packedBVHNodes[nodeIndex].start = start;
		this->_packedBVHNodes[nodeIndex].count = triangleCount;
		this->_packedBVHNodes[nodeIndex].isLeaf = true;
		return (nodeIndex);
	}

	const BinnedSplit split = findBinnedSAHSplit(
		this->_triangleBoundingBoxes,
		this->_triangleCentroids,
		this->_triangleIndices,
		start,
		end
	);
	std::size_t mid = start;
	if (split.found)
	{
		auto splitIt = std::partition(
			this->_triangleIndices.begin() + start,
			this->_triangleIndices.begin() + end,
			[this, split](std::size_t triangleIndex)
			{
				const std::size_t bin = binForCentroid(
					this->_triangleCentroids[triangleIndex][split.axis],
					split.centroidMin,
					split.binScale
				);

				return (bin <= split.splitBin);
			}
		);
		mid = static_cast<std::size_t>(splitIt - this->_triangleIndices.begin());
	}

	if (mid == start || mid == end)
	{
		const int axis = largestExtentAxis(nodeBox);

		mid = start + triangleCount / 2;
		std::nth_element(
			this->_triangleIndices.begin() + start,
			this->_triangleIndices.begin() + mid,
			this->_triangleIndices.begin() + end,
			[this, axis](std::size_t first, std::size_t second)
			{
				const double firstCentroid = this->_triangleCentroids[first][axis];
				const double secondCentroid = this->_triangleCentroids[second][axis];

				if (firstCentroid == secondCentroid)
				{
					return (boxMinimumAxis(this->_triangleBoundingBoxes[first], axis)
						< boxMinimumAxis(this->_triangleBoundingBoxes[second], axis));
				}
				return (firstCentroid < secondCentroid);
			}
		);
	}

	this->_packedBVHNodes[nodeIndex].left = this->_buildPackedBVHNode(start, mid);
	this->_packedBVHNodes[nodeIndex].right = this->_buildPackedBVHNode(mid, end);

	return (nodeIndex);
}

void	Mesh::_computePackedTriangleAreas(void)
{
	this->_triangleAreaPrefixSums.clear();
	this->_triangleAreaPrefixSums.reserve(this->_triangles.size());
	this->_totalArea = 0.0;

	for (const Triangle& triangle : this->_triangles)
	{
		this->_totalArea += triangle.area();
		this->_triangleAreaPrefixSums.push_back(this->_totalArea);
	}
}

/*
	Light mass distribution

	Each non-degenerate face i contributes A_i * Y_i where A_i is its area and
	Y_i the luminance of the material resolved for its slot. The distribution is
	built once, after the packed BVH has fixed the triangle order, so it can be
	read concurrently by parallel render workers.
*/
void	Mesh::_computeLightMassDistribution(void)
{
	this->_triangleLightMasses.clear();
	this->_lightMassPrefixSums.clear();
	this->_totalLightMass = 0.0;

	const std::size_t elementCount = this->_usesPackedTriangles
		? this->_triangles.size()
		: this->_legacyTriangles.size();
	if (elementCount == 0)
	{
		return;
	}

	this->_triangleLightMasses.reserve(elementCount);
	this->_lightMassPrefixSums.reserve(elementCount);
	for (std::size_t index = 0; index < elementCount; index++)
	{
		double mass = 0.0;
		const Triangle* const triangle = this->_surfaceTriangle(index);

		if (triangle)
		{
			Material* material = triangle->getMaterial();
			const double area = triangle->area();
			const double luminance = material ? Utilities::luminance(material->emitted()) : 0.0;

			if (
				std::isfinite(area)
				&& area > 0.0
				&& std::isfinite(luminance)
				&& luminance > 0.0
			)
			{
				mass = area * luminance;
			}
		}
		this->_totalLightMass += mass;
		this->_triangleLightMasses.push_back(mass);
		this->_lightMassPrefixSums.push_back(this->_totalLightMass);
	}
	if (!std::isfinite(this->_totalLightMass) || this->_totalLightMass <= 0.0)
	{
		this->_totalLightMass = 0.0;
		this->_lightMassPrefixSums.clear();
	}
}

bool	Mesh::_selectLightElement(double& selectedWeight, double& totalWeight, std::size_t& index) const
{
	selectedWeight = 0.0;
	totalWeight = 0.0;
	index = 0;

	const bool useLightMass = this->_totalLightMass > 0.0;
	const std::vector<double>& prefixSums = useLightMass
		? this->_lightMassPrefixSums
		: this->_triangleAreaPrefixSums;
	const double total = useLightMass ? this->_totalLightMass : this->_totalArea;

	if (prefixSums.empty() || !std::isfinite(total) || total <= 0.0)
	{
		return (false);
	}
	totalWeight = total;

	const double target = Sampler::sample1D(Sampler::DIM_LIGHT_SURFACE_SELECTION) * total;
	const auto it = std::lower_bound(prefixSums.begin(), prefixSums.end(), target);
	std::size_t selectedIndex = std::min<std::size_t>(
		static_cast<std::size_t>(it - prefixSums.begin()),
		prefixSums.size() - 1
	);

	// Prefix sums contain zero-weight (dark or degenerate) faces. Skip them
	// forward (wrapping once) so only positive-mass faces are selected.
	for (std::size_t attempt = 0; attempt <= prefixSums.size(); attempt++)
	{
		const double previous = selectedIndex == 0 ? 0.0 : prefixSums[selectedIndex - 1];
		const double weight = prefixSums[selectedIndex] - previous;

		if (weight > 0.0 && std::isfinite(weight))
		{
			selectedWeight = weight;
			index = selectedIndex;
			return (true);
		}
		selectedIndex = (selectedIndex + 1) % prefixSums.size();
	}

	return (false);
}

void	Mesh::_assignLegacyHitSurfaceIndex(const Ray& ray, HitRecord& hitRecord) const
{
	// BVHNode reports the closest hit but not which element produced it.
	// Resolve it lazily against the stored triangle list (used only by the
	// light-density query, never on the camera/shadow hot path).
	double closestT = T_MAX;
	std::size_t closestIndex = static_cast<std::size_t>(-1);

	for (std::size_t index = 0; index < this->_legacyTriangles.size(); index++)
	{
		const Triangle* triangle = dynamic_cast<const Triangle*>(this->_legacyTriangles[index].get());

		if (!triangle)
		{
			continue;
		}
		Ray localRay = ray;
		HitRecord triangleHit;
		if (triangle->hit(localRay, triangleHit, T_MIN, closestT))
		{
			closestT = triangleHit.t0;
			closestIndex = index;
		}
	}
	if (closestIndex != static_cast<std::size_t>(-1))
	{
		hitRecord.surfaceIndex = closestIndex;
		hitRecord.materialSlot = this->getTriangleMaterialSlot(closestIndex);
	}
}

void	Mesh::_computeLegacyTriangleAreas(void)
{
	this->_triangleAreaPrefixSums.clear();
	this->_triangleAreaPrefixSums.reserve(this->_legacyTriangles.size());
	this->_totalArea = 0.0;

	for (const std::shared_ptr<Hittable>& hittable : this->_legacyTriangles)
	{
		const std::shared_ptr<Triangle> triangle = std::dynamic_pointer_cast<Triangle>(hittable);

		if (triangle)
		{
			this->_totalArea += triangle->area();
		}
		this->_triangleAreaPrefixSums.push_back(this->_totalArea);
	}
	this->_computeLightMassDistribution();
}

const Triangle*	Mesh::_surfaceTriangle(std::size_t index) const
{
	if (this->_usesPackedTriangles)
	{
		if (index >= this->_triangles.size())
		{
			return (nullptr);
		}
		return (&this->_triangles[index]);
	}
	if (index >= this->_legacyTriangles.size())
	{
		return (nullptr);
	}
	return (dynamic_cast<const Triangle*>(this->_legacyTriangles[index].get()));
}

// Returns the Mesh's material
Material*	Mesh::getMaterial(void) const
{
	return (this->_material.get());
}

bool	Mesh::usesMaterialSlots(void) const
{
	return (this->_usesMaterialSlots);
}

const std::vector<std::string>&	Mesh::getMaterialSlotNames(void) const
{
	return (this->_materialSlotNames);
}

const std::vector<std::shared_ptr<Material>>&	Mesh::getMaterialSlotMaterials(void) const
{
	return (this->_materialSlotMaterials);
}

const std::vector<int>&	Mesh::getTriangleMaterialSlots(void) const
{
	return (this->_triangleMaterialSlots);
}

int	Mesh::getTriangleMaterialSlot(std::size_t triangleIndex) const
{
	if (triangleIndex >= this->_triangleMaterialSlots.size())
	{
		return (-1);
	}
	return (this->_triangleMaterialSlots[triangleIndex]);
}

Material*	Mesh::getSurfaceMaterial(std::size_t triangleIndex) const
{
	const Triangle* const triangle = this->_surfaceTriangle(triangleIndex);

	return (triangle ? triangle->getMaterial() : nullptr);
}

// Calculates if the Mesh's BVH is hit by 'ray', is closer than 't_max' and farther than T_MIN
bool	Mesh::hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const
{
	if (!this->_usesPackedTriangles)
	{
		return (this->_legacyBVH.hit(ray, hitRecord, t_min, t_max));
	}
	if (this->_packedBVHNodes.empty())
	{
		return (false);
	}

	double rootNear = 0.0;

	if (!boxHitDistance(this->_packedBVHNodes[0].boundingBox, ray, t_max, rootNear))
	{
		return (false);
	}

	std::array<TraversalNode, MESH_BVH_STACK_SIZE> stack;
	std::size_t stackSize = 0;
	bool hitAnything = false;
	double closestHit = t_max;
	HitRecord triangleHitRecord;

	stack[stackSize++] = {0, rootNear};
	while (stackSize > 0)
	{
		const TraversalNode stackNode = stack[--stackSize];
		if (stackNode.near >= closestHit)
		{
			continue;
		}
		const std::size_t nodeIndex = stackNode.index;
		const PackedBVHNode& node = this->_packedBVHNodes[nodeIndex];

		if (node.isLeaf)
		{
			for (std::size_t i = node.start; i < node.start + node.count; i++)
			{
				const std::size_t triangleIndex = this->_triangleIndices[i];
				const Triangle& triangle = this->_triangles[triangleIndex];

				if (triangle.hit(ray, triangleHitRecord, t_min, closestHit))
				{
					triangleHitRecord.surfaceIndex = triangleIndex;
					triangleHitRecord.materialSlot = this->getTriangleMaterialSlot(triangleIndex);
					hitRecord = triangleHitRecord;
					closestHit = triangleHitRecord.t0;
					hitAnything = true;
				}
			}
			continue;
		}

		const PackedBVHNode& leftNode = this->_packedBVHNodes[node.left];
		const PackedBVHNode& rightNode = this->_packedBVHNodes[node.right];
		double leftNear = 0.0;
		double rightNear = 0.0;
		const bool hitLeft = boxHitDistance(leftNode.boundingBox, ray, closestHit, leftNear);
		const bool hitRight = boxHitDistance(rightNode.boundingBox, ray, closestHit, rightNear);

		if (hitLeft && hitRight)
		{
			if (leftNear < rightNear)
			{
				stack[stackSize++] = {node.right, rightNear};
				stack[stackSize++] = {node.left, leftNear};
			}
			else
			{
				stack[stackSize++] = {node.left, leftNear};
				stack[stackSize++] = {node.right, rightNear};
			}
		}
		else if (hitLeft)
		{
			stack[stackSize++] = {node.left, leftNear};
		}
		else if (hitRight)
		{
			stack[stackSize++] = {node.right, rightNear};
		}
	}

	return (hitAnything);
}

bool	Mesh::hitAny(Ray& ray, double t_min, double t_max) const
{
	if (!this->_usesPackedTriangles)
	{
		return (this->_legacyBVH.hitAny(ray, t_min, t_max));
	}
	if (this->_packedBVHNodes.empty())
	{
		return (false);
	}

	double rootNear = 0.0;

	if (!boxHitDistance(this->_packedBVHNodes[0].boundingBox, ray, t_max, rootNear))
	{
		return (false);
	}

	std::array<TraversalNode, MESH_BVH_STACK_SIZE> stack;
	std::size_t stackSize = 0;

	stack[stackSize++] = {0, rootNear};
	while (stackSize > 0)
	{
		const TraversalNode stackNode = stack[--stackSize];
		const std::size_t nodeIndex = stackNode.index;
		const PackedBVHNode& node = this->_packedBVHNodes[nodeIndex];

		if (node.isLeaf)
		{
			for (std::size_t i = node.start; i < node.start + node.count; i++)
			{
				const Triangle& triangle = this->_triangles[this->_triangleIndices[i]];

				if (triangle.hitAny(ray, t_min, t_max))
				{
					return (true);
				}
			}
			continue;
		}

		const PackedBVHNode& leftNode = this->_packedBVHNodes[node.left];
		const PackedBVHNode& rightNode = this->_packedBVHNodes[node.right];
		double leftNear = 0.0;
		double rightNear = 0.0;
		const bool hitLeft = boxHitDistance(leftNode.boundingBox, ray, t_max, leftNear);
		const bool hitRight = boxHitDistance(rightNode.boundingBox, ray, t_max, rightNear);

		if (hitLeft && hitRight)
		{
			if (leftNear < rightNear)
			{
				stack[stackSize++] = {node.right, rightNear};
				stack[stackSize++] = {node.left, leftNear};
			}
			else
			{
				stack[stackSize++] = {node.left, leftNear};
				stack[stackSize++] = {node.right, rightNear};
			}
		}
		else if (hitLeft)
		{
			stack[stackSize++] = {node.left, leftNear};
		}
		else if (hitRight)
		{
			stack[stackSize++] = {node.right, rightNear};
		}
	}

	return (false);
}

// Returns the AABB / bounding box for this Mesh's BVH
bool	Mesh::createBoundingBox(AABB& outputBoundingBox) const
{
	if (!this->_usesPackedTriangles)
	{
		return (this->_legacyBVH.createBoundingBox(outputBoundingBox));
	}
	if (this->_packedBVHNodes.empty())
	{
		return (false);
	}

	outputBoundingBox = this->_boundingBox;
	return (true);
}

double	Mesh::surfaceArea(void) const
{
	return (this->_totalArea);
}

std::size_t	Mesh::surfaceElementCount(void) const
{
	if (this->_usesPackedTriangles)
	{
		return (this->_triangles.size());
	}
	return (this->_legacyTriangles.size());
}

double	Mesh::transformedSurfaceElementArea(
	std::size_t index,
	const std::function<Vector3(const Vector3&)>& transformPoint
) const
{
	const Triangle* const triangle = this->_surfaceTriangle(index);
	if (!triangle)
	{
		return (0.0);
	}

	const Vector3 vertex0 = transformPoint(triangle->getVertex0());
	const Vector3 vertex1 = transformPoint(triangle->getVertex1());
	const Vector3 vertex2 = transformPoint(triangle->getVertex2());
	const double area = Utilities::vectorLength(Utilities::cross(vertex1 - vertex0, vertex2 - vertex0)) / 2.0;
	if (!std::isfinite(area) || area <= 0.0)
	{
		return (0.0);
	}
	return (area);
}

double	Mesh::transformedSurfaceArea(const std::function<Vector3(const Vector3&)>& transformPoint) const
{
	double area = 0.0;
	const std::size_t elementCount = this->surfaceElementCount();

	for (std::size_t index = 0; index < elementCount; index++)
	{
		area += this->transformedSurfaceElementArea(index, transformPoint);
	}
	if (!std::isfinite(area) || area <= 0.0)
	{
		return (0.0);
	}
	return (area);
}

bool	Mesh::sampleSurface(Vector3& position, Vector3& normal) const
{
	if (this->_triangleAreaPrefixSums.empty() || this->_totalArea <= 0.0)
	{
		return (false);
	}

	const double targetArea = Sampler::sample1D(Sampler::DIM_LIGHT_SURFACE_SELECTION) * this->_totalArea;
	const auto areaIt = std::lower_bound(
		this->_triangleAreaPrefixSums.begin(),
		this->_triangleAreaPrefixSums.end(),
		targetArea
	);
	const std::size_t randomIndex = std::min<std::size_t>(
		static_cast<std::size_t>(areaIt - this->_triangleAreaPrefixSums.begin()),
		this->_triangleAreaPrefixSums.size() - 1
	);
	const double previousArea = randomIndex == 0
		? 0.0
		: this->_triangleAreaPrefixSums[randomIndex - 1];
	const double selectedArea = this->_triangleAreaPrefixSums[randomIndex] - previousArea;
	if (selectedArea <= 0.0)
	{
		return (false);
	}

	return (this->sampleSurfaceElement(randomIndex, position, normal));
}

bool	Mesh::sampleSurfaceElement(std::size_t index, Vector3& position, Vector3& normal) const
{
	const Triangle* const triangle = this->_surfaceTriangle(index);
	if (!triangle)
	{
		return (false);
	}
	return (triangle->sampleSurface(position, normal));
}

double	Mesh::pdfValue(const Vector3& origin, const Vector3& vec) const
{
	if (this->_triangleAreaPrefixSums.empty() || this->_totalArea <= 0.0)
	{
		return (0.0);
	}
	if (this->_usesMaterialSlots && this->_totalLightMass <= 0.0)
	{
		return (0.0);
	}

	Ray ray(origin, vec);
	HitRecord hitRecord;

	if (!this->hit(ray, hitRecord, T_MIN, T_MAX))
	{
		return (0.0);
	}

	const double vecLength = Utilities::vectorLength(vec);
	if (!std::isfinite(vecLength) || vecLength <= 0.0)
	{
		return (0.0);
	}
	const double distanceSquared = hitRecord.t0 * hitRecord.t0 * Utilities::vectorLengthSquared(vec);
	const double cosine = std::fabs(Utilities::dot(vec, hitRecord.geometricNormal) / vecLength);
	if (!std::isfinite(distanceSquared) || !std::isfinite(cosine) || cosine <= 0.0)
	{
		return (0.0);
	}

	if (this->_usesMaterialSlots && this->_totalLightMass <= 0.0)
	{
		// A slot-aware mesh whose resolved faces are all dark emits nothing and
		// has no light density at all.
		return (0.0);
	}

	if (this->_totalLightMass > 0.0)
	{
		// Emission-weighted density: Yi / sum(Aj*Yj). A hit on a face whose
		// resolved material is dark has zero density under this distribution.
		if (!this->_usesPackedTriangles && hitRecord.surfaceIndex == static_cast<std::size_t>(-1))
		{
			// Legacy triangle lists do not report which element was hit; the
			// light-density query is not on the camera/shadow hot path, so
			// resolve it here.
			this->_assignLegacyHitSurfaceIndex(ray, hitRecord);
		}
		if (hitRecord.surfaceIndex >= this->_triangleLightMasses.size())
		{
			return (0.0);
		}
		const double hitWeight = this->_triangleLightMasses[hitRecord.surfaceIndex];
		const Triangle* const hitTriangle = this->_surfaceTriangle(hitRecord.surfaceIndex);
		const double hitArea = hitTriangle ? hitTriangle->area() : 0.0;
		if (!(hitWeight > 0.0) || !(hitArea > 0.0))
		{
			return (0.0);
		}
		return (distanceSquared * hitWeight / (cosine * this->_totalLightMass * hitArea));
	}

	// Legacy, non-emitting mesh: keep the original uniform-area density.
	return (distanceSquared / (cosine * this->_totalArea));
}

Vector3	Mesh::random(const Vector3& origin) const
{
	if (this->_usesMaterialSlots && this->_totalLightMass <= 0.0)
	{
		return (Hittable::random(origin));
	}

	double selectedWeight = 0.0;
	double totalWeight = 0.0;
	std::size_t randomIndex = 0;

	if (!this->_selectLightElement(selectedWeight, totalWeight, randomIndex))
	{
		return (Hittable::random(origin));
	}

	if (this->_usesPackedTriangles)
	{
		return (this->_triangles[randomIndex].random(origin));
	}

	return (this->_legacyTriangles[randomIndex]->random(origin));
}

bool	Mesh::sampleLight(const Vector3& origin, HittableLightSample& sample) const
{
	sample = HittableLightSample();
	if (this->_usesMaterialSlots && this->_totalLightMass <= 0.0)
	{
		return (false);
	}

	double selectedWeight = 0.0;
	double totalWeight = 0.0;
	std::size_t randomIndex = 0;
	if (!this->_selectLightElement(selectedWeight, totalWeight, randomIndex))
	{
		return (false);
	}

	bool sampled = false;
	if (this->_usesPackedTriangles)
	{
		sampled = this->_triangles[randomIndex].sampleLight(origin, sample);
	}
	else
	{
		sampled = this->_legacyTriangles[randomIndex]->sampleLight(origin, sample);
	}
	if (!sampled || !sample.valid)
	{
		sample = HittableLightSample();
		return (false);
	}

	// The triangle uses a uniform area density on the selected face; convert it
	// to the mesh-level density (uniform area when the mesh is non-emitting, or
	// Ai*Yi / sum(Aj*Yj) when it is).
	sample.pdf *= selectedWeight / totalWeight;
	sample.valid = std::isfinite(sample.pdf) && sample.pdf > 0.0;
	sample.surfaceIndex = randomIndex;
	return (sample.valid);
}

bool	Mesh::sampleEmission(HittableEmissionSample& sample) const
{
	sample = HittableEmissionSample();
	if (this->_usesMaterialSlots && this->_totalLightMass <= 0.0)
	{
		return (false);
	}

	double selectedWeight = 0.0;
	double totalWeight = 0.0;
	std::size_t randomIndex = 0;
	if (!this->_selectLightElement(selectedWeight, totalWeight, randomIndex))
	{
		return (false);
	}

	bool sampled = false;
	if (this->_usesPackedTriangles)
	{
		sampled = this->_triangles[randomIndex].sampleEmission(sample);
	}
	else
	{
		sampled = this->_legacyTriangles[randomIndex]->sampleEmission(sample);
	}
	if (!sampled || !sample.valid)
	{
		sample = HittableEmissionSample();
		return (false);
	}

	// The triangle reports 2*pi*Ai. Face i is selected with probability
	// Ai*Yi/sum(Aj*Yj), so the unbiased mesh-level value is
	// 2*pi / (Yi/sum(Aj*Yj)) = 2*pi*Ai*sum/(Ai*Yi).
	sample.powerScale *= totalWeight / selectedWeight;
	sample.valid = std::isfinite(sample.powerScale) && sample.powerScale > 0.0;
	return (sample.valid);
}

double	Mesh::lightSelectionWeight(void) const
{
	const std::size_t elementCount = this->surfaceElementCount();

	if (elementCount > 0 && this->_usesMaterialSlots)
	{
		// Slot-aware meshes distribute light over their resolved faces only.
		// A fully dark slot set (including a dark fallback) contributes nothing.
		if (!std::isfinite(this->_totalLightMass) || this->_totalLightMass <= 0.0)
		{
			return (0.0);
		}
		return (this->_totalLightMass);
	}

	if (elementCount > 0 && this->_totalLightMass > 0.0)
	{
		return (this->_totalLightMass);
	}

	// Meshes built from a pre-existing BVH expose no per-face data; preserve the
	// legacy single-material weight based on the mesh area.
	if (!this->_material || this->_totalArea <= 0.0)
	{
		return (0.0);
	}
	const double luminance = Utilities::luminance(this->_material->emitted());
	if (!std::isfinite(luminance) || luminance <= 0.0)
	{
		return (0.0);
	}
	return (this->_totalArea * luminance);
}
