#pragma once

#include "util/Utilities.h"

#include "brdf/BRDF.h"
#include "brdf/Lambert.h"
#include "brdf/Microfacet.h"

// FancyBRDF is a combination of a Lambert diffuse layer and a Microfacet specular layer.
struct FancyBRDF : BRDF {

    FancyBRDF(Vector3 normal, Vector3 albedo, Vector3 reflectance, double roughness) : 
        normal(normal), albedo(albedo),
        reflectance(reflectance), alpha(roughness * roughness){}

    // Evaluate the cosine-weighted brdf
    Vector3 Evaluate(Vector3 l, Vector3 v) const override {

        const Vector3 n = normal;
        const Vector3 h = normalize(l + v, n);

        const double vdoth = saturate(dot(v, h));
        const double ndotl = saturate(dot(n, l));
        const double ndotv = saturate(dot(n, v));
        const double ndoth = saturate(dot(n, h));

        // Fresnel term for reflectance
        Vector3 fresnel_reflected = Microfacet::F(reflectance, vdoth);
        Vector3 fresnel_transmitted = Vector3(1.0, 1.0, 1.0) - fresnel_reflected;

        // Combine Lambert and Microfacet layers
        Vector3 lambert_layer = fresnel_transmitted * Lambert::Evaluate(ndotl, albedo);
        Vector3 microfacet_layer = fresnel_reflected * Microfacet::V(alpha, ndotl, ndotv) * Microfacet::D(alpha, ndoth) * ndotl;

        return lambert_layer + microfacet_layer;
    }

    Vector3 Sample(Vector3& l, Vector3 v, Vector3 sample) const override {
        // Half-vector calculation
        Vector3 h = Microfacet::CalculateNormal(v, normal, sample, alpha);

        // Fresnel term
        Vector3 fresnel = Microfacet::F(reflectance, std::max(0.0, dot(v, h)));

        // Probability densities
        double ps = (fresnel.x() + fresnel.y() + fresnel.z()) / 3.0;
        double pd = (albedo.x() + albedo.y() + albedo.z()) / 3.0;
        double pdf = ps / (ps + pd);

        // Random decision for sampling
        if (random_double() < pdf) {


            // Specular reflection
            l = reflect_pm(-v, h);
            double g_term = Microfacet::G1(alpha, std::max(0.0, dot(normal, l)));

            return fresnel / pdf * g_term;
        } else {

            // Diffuse reflection
            l = Lambert::CalculateLightDirection(normal, sample);

            Vector3 transmitted = Vector3(1.0, 1.0, 1.0) - fresnel;
            return transmitted / (1 - pdf) * albedo;
        }
    }

    // World-space normal
    Vector3 normal = Vector3(0,0,1);

    // Diffuse  albedo
    Vector3 albedo = Vector3(0,0,0);

    // Specular normal reflectance
    Vector3 reflectance = Vector3(0,0,0);

    // Specular alpha
    double alpha = 0;
};
