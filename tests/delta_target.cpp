// Delta (discrete) specular tests for OpenPBR.
//
// These tests verify the discrete reflection/refraction path that is taken when both effective
// specular alphas are exactly zero, including its interaction with the coat and diffuse lobes.
// All reference values are computed independently here (plane reflection, Snell refraction,
// and the unpolarized Fresnel equations) rather than by calling the implementation.

#include <glm/glm.hpp>
#include "openpbr.h"

#include <cmath>
#include <cstdio>

#include "../task_support/check.h"

namespace {

constexpr float kDirectionTolerance = 2.0e-6f;

// ---------------------------------------------------------------------------------------------------
// Independent reference math
// ---------------------------------------------------------------------------------------------------

// Reflect an incident direction about a unit plane normal (both directions point away from the surface).
glm::vec3 ref_reflect(glm::vec3 incident, glm::vec3 normal)
{
    return 2.0f * glm::dot(incident, normal) * normal - incident;
}

// Snell refraction through a plane. eta = n_transmitted / n_incident (the same convention as the
// reflection/transmission coefficients). Both vectors point away from the surface.
// Returns false on total internal reflection.
bool ref_refract(glm::vec3 incident, glm::vec3 normal, float eta, glm::vec3 &transmitted)
{
    const float cos_i = glm::dot(incident, normal);
    const float eta_i_over_eta_t = 1.0f / eta;
    const float sin2_t = (1.0f - cos_i * cos_i) * eta_i_over_eta_t * eta_i_over_eta_t;
    if (sin2_t >= 1.0f)
        return false;
    const float cos_t = std::sqrt(1.0f - sin2_t);
    transmitted = glm::normalize(-incident * eta_i_over_eta_t + normal * (eta_i_over_eta_t * cos_i - cos_t));
    return true;
}

// Unpolarized Fresnel reflectance for eta = n_transmitted / n_incident and a non-negative cosine.
float ref_fresnel(float eta, float cos_i)
{
    if (eta == 1.0f)
        return 0.0f;
    const float sin2_t = (1.0f - cos_i * cos_i) / (eta * eta);
    if (sin2_t >= 1.0f)
        return 1.0f;
    const float cos_t = std::sqrt(1.0f - sin2_t);
    const float rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    const float rp = (cos_t - eta * cos_i) / (cos_t + eta * cos_i);
    return 0.5f * (rs * rs + rp * rp);
}

// Metal reflection color used by the OpenPBR F82-tint model, evaluated at a given cosine.
glm::vec3 ref_metal_fresnel(glm::vec3 f0, glm::vec3 f82_tint, float cos_theta)
{
    constexpr float cos_theta_max = 1.0f / 7.0f;
    const float om = 1.0f - cos_theta_max;
    const float om5 = om * om * om * om * om;
    const float om6 = om5 * om;
    const glm::vec3 r = f0;
    const glm::vec3 t = f82_tint;
    const glm::vec3 b = (r + (glm::vec3(1.0f) - r) * om5) * (glm::vec3(1.0f) - t) / (cos_theta_max * om6);
    const float u = 1.0f - cos_theta;
    const glm::vec3 f = r + ((glm::vec3(1.0f) - r) - b * cos_theta * u) * (u * u * u * u * u);
    return glm::clamp(f, 0.0f, 1.0f);
}

const glm::vec3 kBaseColor(0.8f, 0.3f, 0.1f);
const glm::vec3 kSpecularColor(1.0f);
const glm::vec3 kTransmissionColor(1.0f);
const glm::vec3 kView = glm::normalize(glm::vec3(0.75f, 0.0f, 0.66f));

// ---------------------------------------------------------------------------------------------------
// Material construction
// ---------------------------------------------------------------------------------------------------

struct MaterialInputs
{
    float base_metalness = 0.0f;
    float transmission_weight = 0.0f;
    float coat_weight = 0.0f;
    float coat_roughness = 0.0f;
    float coat_ior = 1.6f;
    float specular_ior = 1.5f;
    glm::vec3 base_color = kBaseColor;
    glm::vec3 specular_color = kSpecularColor;
    glm::vec3 coat_color = glm::vec3(1.0f);
    glm::vec3 transmission_color = kTransmissionColor;
    glm::vec3 geometry_normal = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 coat_normal = glm::vec3(0.0f, 0.0f, 1.0f);
};

OpenPBR_ResolvedInputs make_inputs(const MaterialInputs &m, float roughness)
{
    OpenPBR_ResolvedInputs in = openpbr_make_default_resolved_inputs();
    in.base_color = m.base_color;
    in.specular_color = m.specular_color;
    in.coat_color = m.coat_color;
    in.transmission_color = m.transmission_color;
    in.base_metalness = m.base_metalness;
    in.transmission_weight = m.transmission_weight;
    in.coat_weight = m.coat_weight;
    in.coat_roughness = m.coat_roughness;
    in.coat_ior = m.coat_ior;
    in.specular_ior = m.specular_ior;
    in.specular_roughness = roughness;
    in.transmission_dispersion_scale = 0.0f;
    in.geometry_basis = openpbr_make_basis(m.geometry_normal);
    in.geometry_coat_basis = openpbr_make_basis(m.coat_normal);
    return in;
}

OpenPBR_PreparedBsdf prepare(const MaterialInputs &m, float roughness, glm::vec3 path_throughput = glm::vec3(1.0f),
                             glm::vec3 view = kView)
{
    return openpbr_prepare(make_inputs(m, roughness), path_throughput, OpenPBR_BaseRgbWavelengths_nm, OpenPBR_VacuumIor, view);
}

// Relative error metric requested by the task: |a - b| <= max(1, |reference|) * 2e-5.
bool close_to_reference(float actual, float reference)
{
    const float tolerance = std::fmax(1.0f, std::fabs(reference)) * 2.0e-5f;
    return std::fabs(actual - reference) <= tolerance;
}

bool vec_close_to_reference(glm::vec3 actual, glm::vec3 reference)
{
    return close_to_reference(actual.x, reference.x) && close_to_reference(actual.y, reference.y) &&
           close_to_reference(actual.z, reference.z);
}

bool direction_close(glm::vec3 a, glm::vec3 b)
{
    const float distance_squared = glm::dot(a - b, a - b);
    return std::sqrt(distance_squared) <= kDirectionTolerance;
}

glm::vec3 eval_discrete_sum(const OpenPBR_PreparedBsdf &p, glm::vec3 direction)
{
    return openpbr_get_sum_of_diffuse_specular(openpbr_eval_discrete(p, direction));
}

// ---------------------------------------------------------------------------------------------------
// Reference material models
// ---------------------------------------------------------------------------------------------------

// A smooth dielectric interface: reflection is Fresnel-weighted, transmission is (1 - Fresnel)-weighted
// and scaled by the base transmission color. Metal and opaque components are not present.
struct DielectricReference
{
    float eta;  // n_transmitted / n_incident as seen from the current side
    glm::vec3 reflection_color;
    glm::vec3 transmission_color;
    float reflection_probability;
    float transmission_probability;
};

DielectricReference dielectric_at_view(const MaterialInputs &m)
{
    const float incidence_cos = std::fabs(glm::dot(kView, m.geometry_normal));
    const bool entering = glm::dot(kView, m.geometry_normal) > 0.0f;
    const float eta = entering ? m.specular_ior : 1.0f / m.specular_ior;

    const float fresnel = ref_fresnel(eta, incidence_cos);
    const glm::vec3 refl = m.specular_color * fresnel;
    const glm::vec3 trans = m.transmission_color * (1.0f - fresnel);

    const float refl_max = glm::max(refl.x, glm::max(refl.y, refl.z));
    const float trans_max = glm::max(trans.x, glm::max(trans.y, trans.z));
    const float sum = refl_max + trans_max;
    DielectricReference r;
    r.eta = eta;
    r.reflection_color = refl;
    r.transmission_color = trans;
    r.reflection_probability = sum > 0.0f ? refl_max / sum : 0.0f;
    r.transmission_probability = sum > 0.0f ? trans_max / sum : 0.0f;
    return r;
}

// ---------------------------------------------------------------------------------------------------
// Case groups
// ---------------------------------------------------------------------------------------------------

// Pure glass from the front: reflection follows the mirror direction about the face-forward normal,
// transmission follows Snell, and the discrete contribution matches the Fresnel split.
int case_glass_front()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.specular_ior = 1.5f;
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);

    const DielectricReference ref = dielectric_at_view(m);
    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);
    glm::vec3 transmitted;
    TASK_CHECK(ref_refract(kView, m.geometry_normal, ref.eta, transmitted));

    // Discrete state exists in both directions, and the continuous eval/pdf do not report it.
    TASK_CHECK(openpbr_discrete_pdf(p, reflected) > 0.0f);
    TASK_CHECK(openpbr_discrete_pdf(p, transmitted) > 0.0f);
    TASK_CHECK(openpbr_pdf(p, reflected) == 0.0f);
    TASK_CHECK(openpbr_pdf(p, transmitted) == 0.0f);
    // The continuous eval at an ideal direction is zero (no finite-width peak leaks through).
    TASK_CHECK(openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, reflected)) == glm::vec3(0.0f));
    TASK_CHECK(openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, transmitted)) == glm::vec3(0.0f));

    // Analytic contributions.
    const glm::vec3 expected_refl = ref.reflection_color;
    const glm::vec3 expected_trans = ref.transmission_color;
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), expected_refl));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, transmitted), expected_trans));

    // Probability masses.
    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, reflected), ref.reflection_probability));
    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, transmitted), ref.transmission_probability));

    // A nearby but distinct direction carries no discrete mass.
    const glm::vec3 off_reflected = glm::normalize(reflected + glm::vec3(1.0e-4f, 0.0f, 0.0f));
    TASK_CHECK(openpbr_discrete_pdf(p, off_reflected) == 0.0f);
    TASK_CHECK(eval_discrete_sum(p, off_reflected) == glm::vec3(0.0f));

    // Sampling is consistent: pdf is the discrete mass and weight * pdf is the discrete contribution.
    vec3 direction;
    OpenPBR_DiffuseSpecular weight;
    float pdf;
    OpenPBR_BsdfLobeType type;
    openpbr_sample(p, glm::vec3(0.9f, 0.5f, 0.7f), direction, weight, pdf, type);
    TASK_CHECK(bool(type & OpenPBR_BsdfLobeTypeSpecular));
    TASK_CHECK(!bool(type & OpenPBR_BsdfLobeTypeGlossy));
    TASK_CHECK(close_to_reference(pdf, openpbr_discrete_pdf(p, direction)));
    TASK_CHECK(vec_close_to_reference(openpbr_get_sum_of_diffuse_specular(weight) * pdf, eval_discrete_sum(p, direction)));

    return 0;
}

// Pure glass from the back: the face-forward normal flips, so reflection still mirrors about it and
// refraction leaves the object (eta is inverted).
int case_glass_back()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.specular_ior = 1.5f;
    // Below the critical angle (48.6 degrees here is above it), so refraction is real.
    const glm::vec3 view = glm::normalize(glm::vec3(0.30f, 0.0f, -0.95f));  // below the surface
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f, glm::vec3(1.0f), view);

    const glm::vec3 normal_ff = glm::vec3(0.0f, 0.0f, -1.0f);
    const float cos_i = glm::dot(view, normal_ff);
    TASK_CHECK(cos_i > 0.0f);

    const float eta = 1.0f / m.specular_ior;  // leaving the object
    const glm::vec3 reflected = ref_reflect(view, normal_ff);
    glm::vec3 transmitted;
    TASK_CHECK(ref_refract(view, normal_ff, eta, transmitted));

    const float fresnel = ref_fresnel(eta, cos_i);
    const glm::vec3 expected_refl = m.specular_color * fresnel;
    const glm::vec3 expected_trans = m.transmission_color * (1.0f - fresnel);

    TASK_CHECK(direction_close(reflected, glm::normalize(reflected)));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), expected_refl));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, transmitted), expected_trans));
    TASK_CHECK(openpbr_pdf(p, reflected) == 0.0f);
    TASK_CHECK(openpbr_pdf(p, transmitted) == 0.0f);

    return 0;
}

// Critical angle: below it refraction is real, above it total internal reflection leaves no
// transmitted discrete mass.
int case_critical_angle()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.specular_ior = 1.5f;

    // Viewing from inside (back side) at angles around asin(1/1.5) ~= 41.8 degrees.
    const float eta = 1.0f / 1.5f;
    const float critical_sin = 1.0f / 1.5f;
    const float critical_cos = std::sqrt(1.0f - critical_sin * critical_sin);

    // Slightly below the critical angle: transmission exists.
    {
        const float cos_i = critical_cos + 1.0e-3f;
        const float sin_i = std::sqrt(std::fmax(0.0f, 1.0f - cos_i * cos_i));
        // Build a view direction in the plane spanned by +z and +x, pointing into the back hemisphere.
        const glm::vec3 view = glm::vec3(sin_i, 0.0f, -cos_i);
        const OpenPBR_PreparedBsdf p = prepare(m, 0.0f, glm::vec3(1.0f), view);

        glm::vec3 transmitted;
        TASK_CHECK(ref_refract(view, glm::vec3(0.0f, 0.0f, -1.0f), eta, transmitted));
        TASK_CHECK(openpbr_discrete_pdf(p, transmitted) > 0.0f);

        const float fresnel = ref_fresnel(eta, cos_i);
        TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, transmitted), m.transmission_color * (1.0f - fresnel)));
    }

    // Slightly above the critical angle: total internal reflection, no transmitted discrete mass.
    {
        const float cos_i = critical_cos - 1.0e-3f;
        const float sin_i = std::sqrt(std::fmax(0.0f, 1.0f - cos_i * cos_i));
        const glm::vec3 view = glm::vec3(sin_i, 0.0f, -cos_i);
        const OpenPBR_PreparedBsdf p = prepare(m, 0.0f, glm::vec3(1.0f), view);

        glm::vec3 transmitted;
        TASK_CHECK(!ref_refract(view, glm::vec3(0.0f, 0.0f, -1.0f), eta, transmitted));

        // The reflected direction must be the only discrete event, carrying full reflectance.
        const glm::vec3 reflected = ref_reflect(view, glm::vec3(0.0f, 0.0f, -1.0f));
        TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, reflected), 1.0f));
        TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), m.specular_color));

        // Sweep the full lower hemisphere for any transmitted discrete mass.
        for (int i = 0; i < 32; ++i)
        {
            const float phi = (2.0f * 3.14159265358979f * float(i)) / 32.0f;
            const glm::vec3 sample_dir(std::cos(phi) * 0.5f, std::sin(phi) * 0.5f, -std::sqrt(0.75f));
            TASK_CHECK(openpbr_discrete_pdf(p, glm::normalize(sample_dir)) == 0.0f);
        }
    }

    return 0;
}

// Pure metal: the discrete reflection carries the F82-tint metal Fresnel color.
int case_pure_metal()
{
    MaterialInputs m;
    m.base_metalness = 1.0f;
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);

    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);
    const float cos_i = glm::dot(kView, m.geometry_normal);

    const glm::vec3 weighted_base_color = m.base_color;  // base_weight defaults to 1
    const glm::vec3 expected = ref_metal_fresnel(weighted_base_color, m.specular_color, cos_i);

    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, reflected), 1.0f));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), expected));
    TASK_CHECK(openpbr_pdf(p, reflected) == 0.0f);
    TASK_CHECK(eval_discrete_sum(p, ref_reflect(kView, glm::vec3(1.0f, 0.0f, 0.0f))) == glm::vec3(0.0f));

    // A pure metal has no transmission event at all.
    for (int i = 0; i < 16; ++i)
    {
        const float phi = (2.0f * 3.14159265358979f * float(i)) / 16.0f;
        const glm::vec3 sample_dir = glm::normalize(glm::vec3(std::cos(phi), std::sin(phi), -1.0f));
        TASK_CHECK(openpbr_discrete_pdf(p, sample_dir) == 0.0f);
    }

    return 0;
}

// Colored throughput: the channel sampling probabilities must follow path_throughput, and the
// returned weight must remain the ratio of discrete contribution to discrete mass.
int case_colored_throughput()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.specular_ior = 1.5f;

    const glm::vec3 throughput(1.0f, 0.25f, 0.05f);
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f, throughput);

    const DielectricReference ref = dielectric_at_view(m);
    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);
    glm::vec3 transmitted;
    TASK_CHECK(ref_refract(kView, m.geometry_normal, ref.eta, transmitted));

    // The discrete contributions are still the (untinted) reflection/transmission coefficients.
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), ref.reflection_color));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, transmitted), ref.transmission_color));

    // The probability masses must be throughput-weighted.
    const glm::vec3 refl = ref.reflection_color * throughput;
    const glm::vec3 trans = ref.transmission_color * throughput;
    const float refl_max = glm::max(refl.x, glm::max(refl.y, refl.z));
    const float trans_max = glm::max(trans.x, glm::max(trans.y, trans.z));
    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, reflected), refl_max / (refl_max + trans_max)));
    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, transmitted), trans_max / (refl_max + trans_max)));

    // Sampling agrees with the queries.
    vec3 direction;
    OpenPBR_DiffuseSpecular weight;
    float pdf;
    OpenPBR_BsdfLobeType type;
    openpbr_sample(p, glm::vec3(0.6f, 0.2f, 0.8f), direction, weight, pdf, type);
    TASK_CHECK(bool(type & OpenPBR_BsdfLobeTypeSpecular));
    TASK_CHECK(close_to_reference(pdf, openpbr_discrete_pdf(p, direction)));
    TASK_CHECK(vec_close_to_reference(openpbr_get_sum_of_diffuse_specular(weight) * pdf, eval_discrete_sum(p, direction)));

    return 0;
}

// A smooth coat and a smooth base with the same normal share the mirror direction: the discrete
// mass and the layer-attenuated contributions must be merged rather than reporting only one lobe.
int case_coincident_coat_and_base()
{
    MaterialInputs m;
    m.coat_weight = 1.0f;
    m.coat_roughness = 0.0f;
    m.coat_ior = 1.6f;
    m.specular_ior = 1.5f;
    m.base_metalness = 0.0f;
    m.transmission_weight = 0.0f;

    OpenPBR_ResolvedInputs in = make_inputs(m, 0.0f);
    in.base_weight = 0.0f;  // remove the diffuse lobe from selection so both events cover all mass
    const OpenPBR_PreparedBsdf p =
        openpbr_prepare(in, glm::vec3(1.0f), OpenPBR_BaseRgbWavelengths_nm, OpenPBR_VacuumIor, kView);

    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);
    const float cos_i = glm::dot(kView, m.geometry_normal);

    const float coat_fresnel = ref_fresnel(m.coat_ior, cos_i);
    TASK_CHECK(direction_close(ref_reflect(kView, m.coat_normal), reflected));

    // Both lobes produce exactly the same direction and each covers it with mass 1, so the merged
    // discrete probability mass must be the full 1. Reporting only one lobe's value would be < 1.
    TASK_CHECK(close_to_reference(openpbr_discrete_pdf(p, reflected), 1.0f));
    TASK_CHECK(openpbr_pdf(p, reflected) == 0.0f);
    TASK_CHECK(openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, reflected)) == glm::vec3(0.0f));

    // The merged contribution contains the coat reflection plus the attenuated base reflection.
    const glm::vec3 merged = eval_discrete_sum(p, reflected);
    const glm::vec3 coat_term = m.specular_color * coat_fresnel;
    TASK_CHECK(merged.x > coat_term.x);  // the base adds energy here
    TASK_CHECK(merged.x <= 1.0f + 1.0e-5f);

    // Sampling through either branch (coat or base) must report the same merged mass and the same
    // merged contribution for this shared direction. A per-lobe report would differ between branches.
    float observed_pdf = -1.0f;
    glm::vec3 observed_contribution(-1.0f);
    for (int i = 0; i < 400; ++i)
    {
        const float u0 = (float(i) + 0.5f) / 400.0f;
        vec3 direction;
        OpenPBR_DiffuseSpecular weight;
        float pdf;
        OpenPBR_BsdfLobeType type;
        openpbr_sample(p, glm::vec3(u0, 0.29f, 0.63f), direction, weight, pdf, type);
        if (!direction_close(direction, reflected))
            continue;
        TASK_CHECK(bool(type & OpenPBR_BsdfLobeTypeSpecular));
        const glm::vec3 contribution = openpbr_get_sum_of_diffuse_specular(weight) * pdf;
        if (observed_pdf < 0.0f)
        {
            observed_pdf = pdf;
            observed_contribution = contribution;
        }
        else
        {
            TASK_CHECK(close_to_reference(pdf, observed_pdf));
            TASK_CHECK(vec_close_to_reference(contribution, observed_contribution));
        }
    }
    TASK_CHECK(observed_pdf > 0.0f);
    TASK_CHECK(close_to_reference(observed_pdf, openpbr_discrete_pdf(p, reflected)));
    TASK_CHECK(vec_close_to_reference(observed_contribution, merged));

    return 0;
}

// A smooth coat with a different normal produces a different mirror direction; each direction keeps
// only the contribution of the lobe that actually reaches it, and the coat/base disjointness must not
// bleed their discrete mass into each other.
int case_separated_coat_and_base()
{
    MaterialInputs m;
    m.coat_weight = 1.0f;
    m.coat_roughness = 0.0f;
    m.coat_ior = 1.6f;
    m.specular_ior = 1.5f;
    m.coat_normal = glm::normalize(glm::vec3(0.35f, 0.0f, 1.0f));
    m.geometry_normal = glm::vec3(0.0f, 0.0f, 1.0f);

    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);

    const glm::vec3 coat_dir = ref_reflect(kView, m.coat_normal);
    const glm::vec3 base_dir = ref_reflect(kView, m.geometry_normal);
    TASK_CHECK(!direction_close(coat_dir, base_dir));

    const glm::vec3 coat_value = eval_discrete_sum(p, coat_dir);
    const glm::vec3 base_value = eval_discrete_sum(p, base_dir);
    TASK_CHECK(coat_value != glm::vec3(0.0f));
    TASK_CHECK(base_value != glm::vec3(0.0f));

    const float coat_mass = openpbr_discrete_pdf(p, coat_dir);
    const float base_mass = openpbr_discrete_pdf(p, base_dir);
    TASK_CHECK(coat_mass > 0.0f);
    TASK_CHECK(base_mass > 0.0f);

    // The two ideal directions are distinct, so each mass comes from a single lobe and their sum is
    // the total discrete probability of the material (the split between the lobes).
    TASK_CHECK(coat_mass + base_mass <= 1.0f + 1.0e-5f);

    // The coat reflection in the discrete eval is exactly presence * Fresnel at the coat IOR.
    const float coat_fresnel = ref_fresnel(m.coat_ior, glm::dot(kView, m.coat_normal));
    TASK_CHECK(vec_close_to_reference(coat_value, m.specular_color * (coat_fresnel * m.coat_weight)));

    // The base reflection is the base Fresnel (relative to the weighted coat IOR) identically scaled by
    // the coat passage attenuation; it must be strictly attenuated but still present.
    const float weighted_coat_ior = glm::mix(1.0f, m.coat_ior, m.coat_weight);
    const float relative_base_ior = m.specular_ior / weighted_coat_ior;
    const float base_fresnel = ref_fresnel(relative_base_ior, glm::dot(kView, m.geometry_normal));
    TASK_CHECK(base_fresnel > 0.0f);
    TASK_CHECK(base_value.x > 0.0f);
    TASK_CHECK(base_value.x <= base_fresnel * (1.0f - coat_fresnel) + 1.0e-6f);

    // The base ideal direction must not receive the coat's contribution (and vice versa).
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, base_dir), base_value));
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, coat_dir), coat_value));

    // Both directions are sampled with matching discrete mass when the coat is selected.
    for (int i = 0; i < 200; ++i)
    {
        const float u0 = (float(i) + 0.5f) / 200.0f;
        vec3 direction;
        OpenPBR_DiffuseSpecular weight;
        float pdf;
        OpenPBR_BsdfLobeType type;
        openpbr_sample(p, glm::vec3(u0, 0.37f, 0.81f), direction, weight, pdf, type);
        if (!bool(type & OpenPBR_BsdfLobeTypeSpecular))
            continue;
        TASK_CHECK(close_to_reference(pdf, openpbr_discrete_pdf(p, direction)));
        TASK_CHECK(vec_close_to_reference(openpbr_get_sum_of_diffuse_specular(weight) * pdf, eval_discrete_sum(p, direction)));
    }

    return 0;
}

// A non-metal opaque base adds a diffuse lobe. The diffuse component must stay continuous: it must
// not be folded into the discrete mass or the discrete eval.
int case_diffuse_mixture()
{
    MaterialInputs m;
    m.base_color = glm::vec3(0.6f, 0.5f, 0.4f);
    m.specular_ior = 1.5f;

    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);
    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);

    const float fresnel = ref_fresnel(m.specular_ior, glm::dot(kView, m.geometry_normal));
    const glm::vec3 expected_specular = m.specular_color * fresnel;

    // Discrete specular contribution only.
    TASK_CHECK(vec_close_to_reference(eval_discrete_sum(p, reflected), expected_specular));
    TASK_CHECK(openpbr_discrete_pdf(p, reflected) > 0.0f);
    TASK_CHECK(openpbr_discrete_pdf(p, reflected) < 1.0f);  // the diffuse lobe takes the rest of the mass

    // A direction in the same hemisphere that is not the mirror direction has only continuous energy.
    const glm::vec3 diffuse_dir = glm::normalize(glm::vec3(0.2f, 0.3f, 0.9f));
    TASK_CHECK(openpbr_discrete_pdf(p, diffuse_dir) == 0.0f);
    TASK_CHECK(eval_discrete_sum(p, diffuse_dir) == glm::vec3(0.0f));
    TASK_CHECK(openpbr_pdf(p, diffuse_dir) > 0.0f);

    // Total discrete specular mass must not swallow the diffuse lobe's probability.
    TASK_CHECK(openpbr_discrete_pdf(p, reflected) < 1.0f);

    return 0;
}

// Positive roughness keeps the previous clamped GGX path: sample types stay Glossy, the continuous
// pdf/eval reproduce the original numbers, and no discrete mass appears anywhere.
int case_positive_roughness_unchanged()
{
    struct Expected
    {
        float roughness;
        float metalness;
        float transmission;
        float eval_x;    // at direction normalize(-0.5, 0.3, 0.8)
        float pdf;       // at the same direction
        uint32_t type;   // from the fixed-seed sample
    };
    const Expected cases[] = {
        {0.4f, 0.0f, 0.0f, 0.217488557f, 0.265002996f, 5u},
        {0.2f, 1.0f, 0.0f, 0.0340626203f, 0.0426682979f, 9u},
        {0.3f, 0.0f, 1.0f, 0.00831719395f, 0.00831998419f, 10u},
    };

    for (const Expected &expected : cases)
    {
        MaterialInputs m;
        m.base_metalness = expected.metalness;
        m.transmission_weight = expected.transmission;
        const OpenPBR_PreparedBsdf p = prepare(m, expected.roughness);

        const glm::vec3 direction = glm::normalize(glm::vec3(-0.5f, 0.3f, 0.8f));
        TASK_CHECK(close_to_reference(openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, direction)).x, expected.eval_x));
        TASK_CHECK(close_to_reference(openpbr_pdf(p, direction), expected.pdf));

        vec3 sampled;
        OpenPBR_DiffuseSpecular weight;
        float pdf;
        OpenPBR_BsdfLobeType type;
        openpbr_sample(p, glm::vec3(0.85f, 0.65f, 0.95f), sampled, weight, pdf, type);
        TASK_CHECK(unsigned(type) == unsigned(expected.type));
        // Positive roughness must never take the discrete (Specular-without-Glossy) path.
        TASK_CHECK(!(bool(type & OpenPBR_BsdfLobeTypeSpecular) && !bool(type & OpenPBR_BsdfLobeTypeGlossy)));

        TASK_CHECK(openpbr_discrete_pdf(p, direction) == 0.0f);
        TASK_CHECK(openpbr_discrete_pdf(p, sampled) == 0.0f);
        TASK_CHECK(eval_discrete_sum(p, direction) == glm::vec3(0.0f));
        TASK_CHECK(eval_discrete_sum(p, sampled) == glm::vec3(0.0f));
    }

    return 0;
}

// Only one anisotropic effective alpha is zero: the lobe must keep the clamped GGX behavior.
int case_single_axis_zero_alpha_keeps_ggx()
{
    // With a positive isotropic alpha and full anisotropy, one alpha component is exactly zero
    // while the other stays positive.
    OpenPBR_ResolvedInputs in = make_inputs(MaterialInputs(), 0.3f);
    in.specular_roughness_anisotropy = 1.0f;
    in.base_weight = 0.0f;  // isolate the specular lobe
    const OpenPBR_PreparedBsdf p = openpbr_prepare(in, glm::vec3(1.0f), OpenPBR_BaseRgbWavelengths_nm, OpenPBR_VacuumIor, kView);

    const vec2 alpha = p.fuzz_lobe.coating_lobe.base_lobe.specular_lobe.microfacet_distr.alpha;
    // The zero-on-one-axis anisotropy formula would yield a zero component, but the previous
    // clamping behavior must keep both components usable instead of treating it as a delta.
    TASK_CHECK(alpha.x > 0.0f && alpha.y > 0.0f);

    vec3 direction;
    OpenPBR_DiffuseSpecular weight;
    float pdf;
    OpenPBR_BsdfLobeType type;
    openpbr_sample(p, glm::vec3(0.85f, 0.65f, 0.95f), direction, weight, pdf, type);
    TASK_CHECK(bool(type & OpenPBR_BsdfLobeTypeGlossy));
    TASK_CHECK(!bool(type & OpenPBR_BsdfLobeTypeSpecular));
    TASK_CHECK(pdf > 0.0f);  // still a finite continuous density

    return 0;
}

// Repeated sampling must reproduce the discrete mass by frequency, for each event.
int case_sampling_frequency()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.specular_ior = 1.5f;
    OpenPBR_ResolvedInputs in = make_inputs(m, 0.0f);
    in.base_weight = 0.0f;  // remove the diffuse lobe so every sample is a discrete specular event
    const OpenPBR_PreparedBsdf p =
        openpbr_prepare(in, glm::vec3(1.0f), OpenPBR_BaseRgbWavelengths_nm, OpenPBR_VacuumIor, kView);

    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);
    glm::vec3 transmitted;
    TASK_CHECK(ref_refract(kView, m.geometry_normal, dielectric_at_view(m).eta, transmitted));

    const float reflected_mass = openpbr_discrete_pdf(p, reflected);
    const float transmitted_mass = openpbr_discrete_pdf(p, transmitted);
    TASK_CHECK(std::fabs(reflected_mass + transmitted_mass - 1.0f) < 1.0e-5f);

    constexpr int num_samples = 40000;
    int reflected_count = 0;
    int transmitted_count = 0;
    for (int i = 0; i < num_samples; ++i)
    {
        const float u0 = (float(i) + 0.5f) / float(num_samples);
        const float u1 = std::fmod(float(i) * 0.61803398875f, 1.0f);
        const float u2 = std::fmod(float(i) * 0.75487766625f, 1.0f);
        vec3 direction;
        OpenPBR_DiffuseSpecular weight;
        float pdf;
        OpenPBR_BsdfLobeType type;
        openpbr_sample(p, glm::vec3(u0, u1, u2), direction, weight, pdf, type);
        TASK_CHECK(bool(type & OpenPBR_BsdfLobeTypeSpecular));
        if (direction_close(direction, reflected))
            ++reflected_count;
        else if (direction_close(direction, transmitted))
            ++transmitted_count;
    }

    const double reflected_frequency = double(reflected_count) / double(num_samples);
    const double transmitted_frequency = double(transmitted_count) / double(num_samples);
    TASK_CHECK(std::fabs(reflected_frequency - double(reflected_mass)) < 2.0e-3);
    TASK_CHECK(std::fabs(transmitted_frequency - double(transmitted_mass)) < 2.0e-3);
    TASK_CHECK(reflected_count + transmitted_count == num_samples);

    return 0;
}

// Discrete queries must never return an infinite or NaN pdf/eval, even for degenerate inputs.
int case_queries_are_finite()
{
    MaterialInputs m;
    m.transmission_weight = 1.0f;
    m.coat_weight = 1.0f;
    m.coat_roughness = 0.0f;
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);

    for (int i = 0; i < 64; ++i)
    {
        const float theta = 3.14159265358979f * float(i) / 63.0f;
        const float phi = 6.28318530717959f * float(i) / 63.0f;
        const glm::vec3 direction(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
        const float pdf = openpbr_pdf(p, direction);
        const float discrete_pdf = openpbr_discrete_pdf(p, direction);
        const glm::vec3 eval = glm::vec3(openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, direction)));
        const glm::vec3 discrete_eval = eval_discrete_sum(p, direction);
        TASK_CHECK(std::isfinite(pdf) && pdf >= 0.0f);
        TASK_CHECK(std::isfinite(discrete_pdf) && discrete_pdf >= 0.0f);
        for (int c = 0; c < 3; ++c)
        {
            TASK_CHECK(std::isfinite(eval[c]));
            TASK_CHECK(std::isfinite(discrete_eval[c]));
        }
    }

    return 0;
}

// Mixed material: metalness, transmission, and coat weight all active with a smooth coat and base.
int case_mixed_weights()
{
    MaterialInputs m;
    m.base_metalness = 0.4f;
    m.transmission_weight = 0.6f;
    m.coat_weight = 0.5f;
    m.coat_roughness = 0.0f;
    const OpenPBR_PreparedBsdf p = prepare(m, 0.0f);

    const glm::vec3 reflected = ref_reflect(kView, m.geometry_normal);

    // The material has a diffuse lobe as well, so the continuous pdf is nonzero at the mirror
    // direction. The key requirement is that the discrete mass is not folded into it: the continuous
    // pdf must be finite and must not jump between the mirror direction and an infinitesimally
    // nearby direction.
    const float discrete_mass = openpbr_discrete_pdf(p, reflected);
    TASK_CHECK(discrete_mass > 0.0f && discrete_mass <= 1.0f + 1.0e-5f);

    const float pdf_at_event = openpbr_pdf(p, reflected);
    TASK_CHECK(std::isfinite(pdf_at_event) && pdf_at_event >= 0.0f);
    const glm::vec3 nearby = glm::normalize(reflected + glm::vec3(1.0e-4f, 0.0f, 0.0f));
    const float pdf_nearby = openpbr_pdf(p, nearby);
    TASK_CHECK(std::isfinite(pdf_nearby) && pdf_nearby >= 0.0f);
    TASK_CHECK(std::fabs(pdf_at_event - pdf_nearby) < discrete_mass);

    // The continuous eval at the mirror direction contains no delta peak either.
    const glm::vec3 eval_at_event = openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, reflected));
    const glm::vec3 eval_nearby = openpbr_get_sum_of_diffuse_specular(openpbr_eval(p, nearby));
    for (int c = 0; c < 3; ++c)
    {
        TASK_CHECK(std::isfinite(eval_at_event[c]));
        TASK_CHECK(std::isfinite(eval_nearby[c]));
        TASK_CHECK(std::fabs(eval_at_event[c] - eval_nearby[c]) < 1.0f);
    }

    TASK_CHECK(eval_discrete_sum(p, reflected) != glm::vec3(0.0f) || eval_discrete_sum(p, nearby) != glm::vec3(0.0f));

    vec3 direction;
    OpenPBR_DiffuseSpecular weight;
    float pdf;
    OpenPBR_BsdfLobeType type;
    openpbr_sample(p, glm::vec3(0.3f, 0.9f, 0.4f), direction, weight, pdf, type);
    TASK_CHECK(close_to_reference(pdf, openpbr_discrete_pdf(p, direction)));
    TASK_CHECK(vec_close_to_reference(openpbr_get_sum_of_diffuse_specular(weight) * pdf, eval_discrete_sum(p, direction)));

    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------------

const task_case task_cases[] = {
    {"interface", "glass_front", case_glass_front},
    {"interface", "glass_back", case_glass_back},
    {"interface", "critical_angle", case_critical_angle},
    {"interface", "pure_metal", case_pure_metal},
    {"interface", "queries_are_finite", case_queries_are_finite},
    {"mixture", "colored_throughput", case_colored_throughput},
    {"mixture", "coincident_coat_and_base", case_coincident_coat_and_base},
    {"mixture", "separated_coat_and_base", case_separated_coat_and_base},
    {"mixture", "diffuse_mixture", case_diffuse_mixture},
    {"mixture", "mixed_weights", case_mixed_weights},
    {"mixture", "sampling_frequency", case_sampling_frequency},
    {"limits", "positive_roughness_unchanged", case_positive_roughness_unchanged},
    {"limits", "single_axis_zero_alpha_keeps_ggx", case_single_axis_zero_alpha_keeps_ggx},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
