#pragma once

#include "Hittables/Hittable.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/Triangle.hpp"
#include "Materials/MaterialBindings.hpp"
#include <cstddef>
#include <functional>
#include <vector>

class	Mesh : public Hittable
{
	public:
		Mesh(void);
		Mesh(Vector3 position, std::shared_ptr<Material> material, BVHNode bvh);
		Mesh(Vector3 position, std::shared_ptr<Material> material, std::vector<std::shared_ptr<Hittable>> triangles);
		Mesh(Vector3 position, std::shared_ptr<Material> material, std::vector<Triangle> triangles);
		// Constructs a mesh that keeps its per-face material slot association.
		// 'slotMaterials' must be parallel to 'slotNames' and
		// 'triangleSlots' parallel to 'triangles' (indices into 'slotNames').
		Mesh(
			Vector3 position,
			std::shared_ptr<Material> material,
			std::vector<Triangle> triangles,
			std::vector<std::string> slotNames,
			std::vector<std::shared_ptr<Material>> slotMaterials,
			std::vector<int> triangleSlots
		);
		virtual Material*	getMaterial(void) const override;
		virtual bool		hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		virtual bool		hitAny(Ray& ray, double t_min, double t_max) const override;
		virtual bool		createBoundingBox(AABB& outputBoundingBox) const override;
		virtual double	pdfValue(const Vector3& origin, const Vector3& vec) const override;
		virtual Vector3	random(const Vector3& origin) const override;
		virtual bool		sampleLight(const Vector3& origin, HittableLightSample& sample) const override;
		virtual bool		sampleEmission(HittableEmissionSample& sample) const override;
		virtual double	lightSelectionWeight(void) const override;
		double			surfaceArea(void) const;
		std::size_t		surfaceElementCount(void) const;
		double			transformedSurfaceArea(const std::function<Vector3(const Vector3&)>& transformPoint) const;
		double			transformedSurfaceElementArea(
			std::size_t index,
			const std::function<Vector3(const Vector3&)>& transformPoint
		) const;
		bool			sampleSurface(Vector3& position, Vector3& normal) const;
		bool			sampleSurfaceElement(std::size_t index, Vector3& position, Vector3& normal) const;

		// Material slot metadata. Slots are named by the OBJ "usemtl"
		// declarations (case-sensitive). Faces without a declaration use the
		// empty-name slot.
		bool			usesMaterialSlots(void) const;
		const std::vector<std::string>&	getMaterialSlotNames(void) const;
		const std::vector<std::shared_ptr<Material>>&	getMaterialSlotMaterials(void) const;
		const std::vector<int>&	getTriangleMaterialSlots(void) const;
		int				getTriangleMaterialSlot(std::size_t triangleIndex) const;
		Material*		getSurfaceMaterial(std::size_t triangleIndex) const;

	private:
		struct	PackedBVHNode
		{
			AABB		boundingBox;
			std::size_t	left = 0;
			std::size_t	right = 0;
			std::size_t	start = 0;
			std::size_t	count = 0;
			bool		isLeaf = false;
		};

		void	_setPackedTriangles(std::vector<Triangle> triangles);
		bool	_trySetPackedTriangles(const std::vector<std::shared_ptr<Hittable>>& triangles);
		void	_buildPackedBVH(void);
		std::size_t	_buildPackedBVHNode(std::size_t start, std::size_t end);
		void	_computePackedTriangleAreas(void);
		void	_computeLegacyTriangleAreas(void);
		void	_computeLightMassDistribution(void);
		const Triangle*	_surfaceTriangle(std::size_t index) const;
		bool	_selectLightElement(double& selectedWeight, double& totalWeight, std::size_t& index) const;
		void	_assignLegacyHitSurfaceIndex(const Ray& ray, HitRecord& hitRecord) const;

		Vector3 _position;
		std::shared_ptr<Material>	_material;
		BVHNode	_legacyBVH;
		std::vector<std::shared_ptr<Hittable>>	_legacyTriangles;
		std::vector<Triangle>	_triangles;
		std::vector<AABB>	_triangleBoundingBoxes;
		std::vector<Vector3>	_triangleCentroids;
		std::vector<std::size_t>	_triangleIndices;
		std::vector<PackedBVHNode>	_packedBVHNodes;
		std::vector<double>	_triangleAreaPrefixSums;
		AABB	_boundingBox;
		double	_totalArea;
		bool	_usesPackedTriangles;

		bool	_usesMaterialSlots;
		std::vector<std::string>	_materialSlotNames;
		std::vector<std::shared_ptr<Material>>	_materialSlotMaterials;
		std::vector<int>	_triangleMaterialSlots;
		std::vector<double>	_triangleLightMasses;
		std::vector<double>	_lightMassPrefixSums;
		double	_totalLightMass;
};
