#pragma once

#include "util/Utilities.h"


namespace Lambert {
        
        inline Vector3 Evaluate(double ndotl, Vector3 albedo) {
                return albedo * ndotl / pi;
        }

                // Calculate the light direction for importance sampling
        inline Vector3 CalculateLightDirection(Vector3 n, Vector3 sample) {	

                const double phi = 2 * pi * sample.y();
                const double cos_theta = sqrt(sample.x());
                const double sin_theta = sqrt(1.0 - sample.x());

                // Create an orthonormal basis around the normal `n`
                Vector3 tangent = unit_vector(perpendicular(n));
                Vector3 bitangent = cross(n, tangent);

                // Transform the sampled vector to world space
                Vector3 local_dir(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
                Vector3 world_dir = local_dir.x() * tangent + local_dir.y() * bitangent + local_dir.z() * n;

                return unit_vector(world_dir);
        }

}