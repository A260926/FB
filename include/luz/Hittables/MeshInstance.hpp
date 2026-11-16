#pragma once

#include "AABB.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/Mesh.hpp"
#include "Materials/Material.hpp"
#include "Materials/MaterialBindings.hpp"
#include "Vector3.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class	MeshInstance : public Hittable
{
	public:
		MeshInstance(
			std::shared_ptr<const Mesh> geometry,
			Vector3 position,
			Vector3 rotationDegrees,
			Vector3 scale,
			std::shared_ptr<Material> material
		);
		// Binds material slot names to materials. Unbound slots use
		// 'fallbackMaterial'. The geometry and its BVH are shared with other
		// instances; the bindings live in this instance only. All scale
		// components must be finite and non-zero.
		MeshInstance(
			std::shared_ptr<const Mesh> geometry,
			Vector3 position,
			Vector3 rotationDegrees,
			Vector3 scale,
			const MaterialBindings& materialBindings,
			std::shared_ptr<Material> fallbackMaterial
		);

		const std::shared_ptr<const Mesh>&	getGeometry(void) const;
		const MaterialBindings&	getMaterialBindings(void) const;
		std::shared_ptr<Material>	getSlotMaterial(const std::string& slotName) const;
		virtual Material*	getMaterial(void) const override;
		virtual bool		hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		virtual bool		hitAny(Ray& ray, double t_min, double t_max) const override;
		virtual bool		createBoundingBox(AABB& outputBoundingBox) const override;
		virtual double	pdfValue(const Vector3& origin, const Vector3& vec) const override;
		virtual Vector3	random(const Vector3& origin) const override;
		virtual bool		sampleLight(const Vector3& origin, HittableLightSample& sample) const override;
		virtual bool		sampleEmission(HittableEmissionSample& sample) const override;
		virtual double	lightSelectionWeight(void) const override;

	private:
		struct	InstanceTransform
		{
			Vector3	position;
			Vector3	scale;
			double	cosX = 1.0;
			double	sinX = 0.0;
			double	cosY = 1.0;
			double	sinY = 0.0;
			double	cosZ = 1.0;
			double	sinZ = 0.0;
			double	uniformAreaScale = 1.0;
			bool	invertible = true;
			bool	uniformArea = true;

			InstanceTransform(Vector3 position, Vector3 rotationDegrees, Vector3 scale);
			Vector3	rotate(Vector3 vector) const;
			Vector3	inverseRotate(Vector3 vector) const;
			Vector3	transformPoint(const Vector3& point) const;
			Vector3	inverseTransformPoint(const Vector3& point) const;
			Vector3	inverseTransformVector(const Vector3& vector) const;
			Vector3	transformNormal(const Vector3& normal) const;
		};

		double	_surfaceArea(void) const;
		const std::vector<double>&	_transformedAreaPrefixSums(void) const;
		bool	_sampleSurface(Vector3& position, Vector3& normal, double& area) const;
		bool	_computeBoundingBox(void);
		bool	_localRay(const Ray& ray, Ray& localRay, double& localDistanceScale) const;
		void	_transformHitRecord(
			const Ray& worldRay,
			const HitRecord& localHitRecord,
			double localDistanceScale,
			HitRecord& worldHitRecord
		) const;

		// Slot-aware light distribution helpers.
		bool	_usesSlotMaterials(void) const;
		void	_buildSlotMaterialTable(void);
		void	_buildWorldLightMass(void) const;
		void	_ensureWorldLightMass(void) const;
		Material*	_resolvedMaterial(int slotIndex) const;
		double	_worldElementArea(std::size_t index) const;
		bool	_selectWorldLightElement(
			double& selectedWeight,
			double& totalWeight,
			std::size_t& index
		) const;

		std::shared_ptr<const Mesh>	_geometry;
		std::shared_ptr<Material>	_material;
		MaterialBindings	_materialBindings;
		std::vector<std::shared_ptr<Material>>	_resolvedSlotMaterials;
		InstanceTransform	_transform;
		AABB	_boundingBox;
		bool	_hasBoundingBox = false;
		mutable double	_surfaceAreaCache = 0.0;
		mutable std::once_flag	_surfaceAreaOnce;
		mutable std::vector<double>	_transformedAreaPrefixCache;
		mutable std::once_flag	_transformedAreaPrefixOnce;
		mutable std::vector<double>	_worldElementAreaCache;
		mutable std::vector<double>	_worldLightMassPrefixCache;
		mutable double	_totalWorldLightMass = 0.0;
		mutable std::once_flag	_worldLightMassOnce;
};
