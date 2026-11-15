#pragma once

#include "util/Utilities.h"

namespace Microfacet {
	
	inline Vector3 F(Vector3 f0, double vdoth) {
		// Schlick Fresnel approximation
		// F(v, h) = f₀ + (1 - f₀) * (1 - v·h)⁵

		return f0 + (Vector3(1.0,1.0,1.0) - f0) * pow(1 - vdoth, 5);
	}

	inline double F(double f0, double vdoth) {
		// Schlick Fresnel approximation
		// F(v, h) = f₀ + (1 - f₀) * (1 - v·h)⁵

		return f0 + (1 - f0) * pow(1 - vdoth, 5);
	}

	inline double G1(double alpha, double ndotx) {
		// Smith GGX geometry factor for either shadowing or masking
		// G₁(n, x) = (2 n·x) / (n·x + sqrt(α² + (1 - α²)(n·x)²))

		const double aa = alpha * alpha;

		return 2 * ndotx / (ndotx + sqrt(aa + (1 - aa) * ndotx * ndotx));
	}

	inline double G(double alpha, double ndotl, double ndotv) {
		// Smith GGX geometry factor for both shadowing and masking
		// G = G₁(n, l) G₁(n, v)

		return G1(alpha, ndotl) * G1(alpha, ndotv);
	}

	inline double V1(double alpha, double ndotx) {
		// Smith GGX visibility factor for either shadowing or masking
		// V₁(n, x) = 1 / (n·x + sqrt(α² + (1 - α²)(n·x)²)

		const double aa = alpha * alpha;

		return 1 / (ndotx + sqrt(aa + (1 - aa) * ndotx * ndotx));
	}

	inline double V(double alpha, double ndotl, double ndotv) {
		// Smith GGX visibility factor for both shadowing and masking
		// V = V₁(n, l) V₁(n, v)

		return V1(alpha, ndotl) * V1(alpha, ndotv);
	}

	inline double D(double alpha, double ndoth) {
		// Smith GGX normal distribution
		// D(h) = α² / (π ((α² - 1) (n·h)² + 1)²)

		const double aa = alpha * alpha;
		const double q = (aa - 1) * ndoth * ndoth + 1;

		return aa / (pi * q * q);
	}

	// Calculate the microfacet normal from the geometry normal, view vector and random sample
	inline Vector3 CalculateNormal(const Vector3& v, const Vector3& n, const Vector3& sample, double alpha) {
		// Generate rotation basis
		auto make_rotation_basis = [](const Vector3& z) {
			Vector3 reference = fabs(dot(z, Vector3(0, 1, 0))) > 0.99 ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
			Vector3 tangent = unit_vector(cross(reference, z));
			Vector3 bitangent = cross(z, tangent);
			return std::make_pair(tangent, bitangent);
		};

		auto [tangent, bitangent] = make_rotation_basis(n);

		// Transform view to geometry (stretched) space
		Vector3 vg = Vector3(dot(v, tangent), dot(v, bitangent), dot(v, n));
		double alpha_vg_x = alpha * vg.x();
		double alpha_vg_y = alpha * vg.y();
		double vg_z = vg.z();
		Vector3 vs = unit_vector(Vector3(alpha_vg_x, alpha_vg_y, vg_z));

		// Sampling probabilities
		double area_blue = 1.0;
		double area_green = vs.z();
		double prob_blue = 1.0 / (area_blue + area_green);
		double prob_green = 1.0 - prob_blue;

		// Polar coordinates
		double ra = sample.x();
		double rb = sample.y();

		double phi;
		if (rb < prob_blue) {
			phi = rb / prob_blue * pi;
		} else {
			phi = pi + (rb - prob_blue) / prob_green * pi;
		}

		double radius = sqrt(ra);
		double x = radius * cos(phi);
		double y = radius * sin(phi) * (rb < prob_blue ? area_blue : area_green);
		double z = sqrt(std::max(0.0, 1.0 - x * x - y * y));

		// Undo stretch and transform to world space
		Vector3 m_unstretched(alpha * x, alpha * y, z);
		m_unstretched = unit_vector(m_unstretched);
		return m_unstretched.x() * tangent + m_unstretched.y() * bitangent + m_unstretched.z() * n;
	}
}
