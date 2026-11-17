#ifndef MATERIAL_H
#define MATERIAL_H

#include "util/Radiance.h"
#include "brdf/FancyBRDF.h"

#include <vector>
#include <fstream>


// Define the custom clamp function here
template <typename T>
constexpr T clamp(const T& value, const T& min, const T& max) {
    return (value < min) ? min : (value > max ? max : value);
}

class Material {
    public:
        double ks = 0.1; // Specular reflection coefficient
        double kd = 0.9; // Diffuse reflection coefficient
        double specularexponent = 10; // Specular exponent
        Radiance diffusecolor = Radiance(0.5, 0.5, 0.5); // Diffuse color
        Radiance specularcolor = Radiance(1, 1, 1); // Specular color

        bool is_reflective = false; // Reflective material
        double reflectivity = 1.0; // Reflectivity coefficient

        double ka = 0.5; // Ambient reflection coefficient

        bool is_refractive = false; // Refractive material
        double refractiveindex = 1.0; // Refractivity coefficient

        // TEXTURE MAPPING
        bool has_texture = false ; // flag for texture mapping
        // std::vector<Radiance> texture_data; // texture pixel data
        std::vector<uint8_t> texture_data; // Compact texture data (R, G, B as uint8_t)
        int texture_width = 0; // texture width
        int texture_height = 0; // texture height
        double tile_factor_u = 1.0; // tiling factor for U coordinate
        double tile_factor_v = 1.0; // tiling factor for V coordinate

        // MICROFACET BRDF
        bool has_brdf = false;
        double roughness = 0.5; // Roughness for NDF
        double metalness = 0.0; // 0 for dielectric, 1 for metal
        Radiance base_color = Radiance(0.8, 0.8, 0.8); // Base color for non-metals


        FancyBRDF build_brdf(const Vector3& n, double u, double v) const
        {
            // Sample material properties
            Radiance kx_0 = base_color;
            if (has_texture) {
                kx_0 = get_texture_color(u, v); // Use texture color
            }
            const Radiance kx = kx_0;
            const Radiance kd = Lerp(kx, Vector3(0.0, 0.0, 0.0), metalness);
            const Radiance ks = Lerp(Vector3(0.04, 0.04, 0.04), kx, metalness);

            // Vector3 h(dot(normalize(dpdu), Vector3(1,1,1)),
            //        dot(normalize(dpdv), Vector3(1,1,1)),
            //        dot(n, Vector3(1,1,1)));

            // Return the constructed BRDF
            return FancyBRDF(n, kd, ks, roughness);
        }



        Radiance get_texture_color(double u, double v) const {
            if (!has_texture) return diffusecolor; // Return default color if no texture

            // Map (u, v) to texture pixel indices
            int i = clamp(static_cast<int>(u * texture_width), 0, texture_width - 1);
            int j = clamp(static_cast<int>(v * texture_height), 0, texture_height - 1);

            // Calculate the index in the 1D texture data array
            int index = (j * texture_width + i) * 3; // 3 channels (R, G, B)

            // Convert from uint8_t to double and return the color
            double r = texture_data[index] / 255.0f;
            double g = texture_data[index + 1] / 255.0f;
            double b = texture_data[index + 2] / 255.0f;

            return Radiance(r, g, b);
        }

        void load_texture(const std::string& filename) {
            std::ifstream file(filename);
            if (!file) {
                std::cerr << "Failed to open texture file: " << filename << std::endl;
                return;
            }

            // Read PPM header
            std::string header;
            file >> header;
            if (header != "P3") {
                std::cerr << "Unsupported PPM format (only P3 supported): " << header << std::endl;
                return;
            }

            // Read texture dimensions and max value
            int max_val;
            file >> texture_width >> texture_height >> max_val;

            if (texture_width <= 0 || texture_height <= 0 || max_val != 255) {
                std::cerr << "Invalid PPM header in file: " << filename << std::endl;
                return;
            }

            // Resize the texture data array
            texture_data.resize(texture_width * texture_height * 3);

            // Read pixel data
            for (int i = 0; i < texture_data.size(); i++) {
                int pixel_value;
                if (!(file >> pixel_value)) {
                    std::cerr << "Warning: truncated or invalid texture file: " << filename << std::endl;
                    has_texture = false;
                    texture_data.clear();
                    texture_width = 0;
                    texture_height = 0;
                    return;
                }
                pixel_value = std::max(0, std::min(pixel_value, max_val));
                texture_data[i] = static_cast<uint8_t>(pixel_value);
            }

            has_texture = true; // Mark texture as loaded
            std::cout << "Texture loaded: " << filename 
                      << " (" << texture_width << "x" << texture_height << ")" << std::endl;
        }

        void print_material() const {
            std::cout << "Material:" << std::endl;
            std::cout << "  Diffuse Color: [" << diffusecolor.x() << ", " << diffusecolor.y() << ", " << diffusecolor.z() << "]" << std::endl;
            std::cout << "  Specular Color: [" << specularcolor.x() << ", " << specularcolor.y() << ", " << specularcolor.z() << "]" << std::endl;
            std::cout << "  Reflectivity: " << reflectivity << std::endl;
            std::cout << "  Refractive Index: " << refractiveindex << std::endl;
            std::cout << "  Is Reflective: " << is_reflective << std::endl;
            std::cout << "  Is Refractive: " << is_refractive << std::endl;
            std::cout << "  Is Microfacet: " << has_brdf << std::endl;
            if (has_brdf) {
                std::cout << "    Roughness: " << roughness << std::endl;
                std::cout << "    Metalness: " << metalness << std::endl;
                std::cout << "    Base Color: [" << base_color.x() << ", " << base_color.y() << ", " << base_color.z() << "]" << std::endl;
            }
            if (has_texture) {
                std::cout << "  Has Texture: Yes (Size: " << texture_width << "x" << texture_height << ")" << std::endl;
            } else {
                std::cout << "  Has Texture: No" << std::endl;
            }
        }        

};

inline double GGX_D(const Vector3& h, const Vector3& n, double roughness) {
    double alpha = std::max((roughness * roughness), 0.0001);
    double alpha2 = alpha * alpha;
    double NdotH = std::max(dot(n, h), 0.0);
    double denom = std::max((NdotH * NdotH * (alpha2 - 1) + 1), static_cast<double> (std::numeric_limits<double>::epsilon()));
    return alpha2 / (pi * denom * denom);
}

inline Radiance fresnel_schlick(double cosTheta, const Radiance& F0) {
    return F0 + (Radiance(1.0, 1.0, 1.0) - F0) * pow(1.0 - cosTheta, 5);
}

inline double smith_G1(const Vector3& v, const Vector3& n, double roughness) {
    double NdotV = std::max(dot(n, v), 0.0001);
    double k = (roughness + 1) * (roughness + 1) / 8.0;
    return NdotV / (NdotV * (1 - k) + k);
}

inline double smith_G(const Vector3& wi, const Vector3& wo, const Vector3& n, double roughness) {
    return smith_G1(wi, n, roughness) * smith_G1(wo, n, roughness);
}

inline Vector3 sample_ggx_halfway(const Vector3& normal, double roughness) {
    // Generate two random numbers for sampling
    double r1 = random_double(); // Random value in [0, 1]
    double r2 = random_double(); // Random value in [0, 1]

    // Convert roughness to alpha (GGX scale parameter)
    double alpha = std::max(roughness * roughness, 0.0001);

    // Compute spherical coordinates
    double phi = 2.0 * pi * r1; // Azimuthal angle
    double cos_theta = std::sqrt((1.0 - r2) / (1.0 + (alpha * alpha - 1.0) * r2));
    double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);

    // Convert to Cartesian coordinates (local space)
    Vector3 local_h(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // Transform from local to world space
    Vector3 tangent = unit_vector(perpendicular(normal));
    Vector3 bitangent = cross(normal, tangent);
    return local_h.x() * tangent + local_h.y() * bitangent + local_h.z() * normal;
}

#endif // MATERIAL_H