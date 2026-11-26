#pragma once

#include "AABB.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/Mesh.hpp"
#include "Materials/Material.hpp"
#include "Vector3.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
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

		const std::shared_ptr<const Mesh>&	getGeometry(void) const;
		virtual Material*	getMaterial(void) const override;
		virtual bool		hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		virtual bool		hitAny(Ray& ray, double t_min, double t_max) const override;
		virtual bool		createBoundingBox(AABB& outputBoundingBox) const override;
		virtual double	pdfValue(const Vector3& origin, const Vector3& vec) const override;
		virtual Vector3	random(const Vector3& origin) const override;
		virtual bool		sampleLight(const Vector3& origin, HittableLightSample& sample) const override;
		virtual bool		sampleEmission(HittableEmissionSample& sample) const override;
		virtual double	lightSelectionWeight(void) const override;

		// Read-only diagnostics used by the refit regression tests.
		std::uint64_t	geometryCacheBuildCount(void) const;
		std::uint64_t	cachedGeometryRevision(void) const;

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
			bool	invertible = true;

			InstanceTransform(Vector3 position, Vector3 rotationDegrees, Vector3 scale);
			Vector3	rotate(Vector3 vector) const;
			Vector3	inverseRotate(Vector3 vector) const;
			Vector3	transformPoint(const Vector3& point) const;
			Vector3	inverseTransformPoint(const Vector3& point) const;
			Vector3	inverseTransformVector(const Vector3& vector) const;
			Vector3	transformNormal(const Vector3& normal) const;
		};

		double	_surfaceArea(void) const;
		bool	_sampleSurface(Vector3& position, Vector3& normal, double& area) const;
		bool	_localRay(const Ray& ray, Ray& localRay, double& localDistanceScale) const;
		void	_transformHitRecord(
			const Ray& worldRay,
			const HitRecord& localHitRecord,
			double localDistanceScale,
			HitRecord& worldHitRecord
		) const;

		struct	GeometryCache
		{
			std::uint64_t	revision = 0;
			AABB		boundingBox;
			bool		hasBoundingBox = false;
			double		surfaceArea = 0.0;
			std::vector<double>	areaPrefixSums;
		};

		std::shared_ptr<const GeometryCache>	_geometryCache(void) const;
		std::shared_ptr<const GeometryCache>	_buildGeometryCache(std::uint64_t revision) const;

		std::shared_ptr<const Mesh>	_geometry;
		std::shared_ptr<Material>	_material;
		InstanceTransform	_transform;
		mutable std::mutex	_cacheMutex;
		mutable std::shared_ptr<const GeometryCache>	_cache;
		mutable std::uint64_t	_cachedRevision = 0;
		mutable std::uint64_t	_cacheBuildCount = 0;
		mutable bool		_cacheValid = false;
};
