#ifndef LIGHTLIST_H
#define LIGHTLIST_H

#include "util/Utilities.h"
#include "light/Light.h"

#include <vector>

// child class of Light
// Contains multiple lights of any light classes
class LightList : public Light {
    public:
        std::vector<shared_ptr<Light>> lights; // List of lights

        LightList() {}
        LightList(shared_ptr<Light> light) { add(light); }

        void clear() { lights.clear(); }

        void add(shared_ptr<Light> light) {
            lights.push_back(light);
        }

        Radiance phong_shading(
            const Point3& p, 
            const Vector3& normal, 
            const Vector3& view_dir, 
            const IntersectionRecord& rec,
            const Object& world,
            const Radiance& ambient_light
        ) const override {
            // Accumulate the shading results from all lights

            Radiance result(0, 0, 0);

            for (const auto& l : lights) {
                // Average the ambient light contribution
                Radiance normaliased_ambient = ambient_light / lights.size();
                result += l->phong_shading(p, normal, view_dir, rec, world, normaliased_ambient);
            }

            return result;
        }

        Radiance brdf_shading(            
            const Point3& p,
            const Vector3& normal,
            const Vector3& view_dir,
            const FancyBRDF& brdf,
            const Object& world,
            const Vector3& coefficient
        ) const override  {
            // Accumulate the shading results from all lights
            Radiance result(0, 0, 0);

            for (const auto& l : lights) {
                result += l->brdf_shading(p, normal, view_dir, brdf, world, coefficient);
            }

            return result;
        }

};


#endif // LIGHT_LIST_H