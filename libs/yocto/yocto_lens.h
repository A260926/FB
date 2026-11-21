//
// # Yocto/Lens: Compound spherical lens camera
//
// Yocto/Lens implements a simple model of a compound spherical lens used by
// the CPU camera. The lens is described in lens-local coordinates: the optical
// axis is +z and points toward the scene, all lengths are in meters, the film
// plane is at z = -g and the aperture stop is a disk of radius a at z = 0.
//
// Interfaces are listed in strictly increasing vertex z. For an interface
// with curvature radius R, effective radius r and refractive index eta after
// crossing, the sphere center is at z + R and only the half sphere containing
// the vertex is used. R = 0 denotes a plane diaphragm that does not refract,
// so its eta has to be equal to the previous medium. The medium before the
// first interface and after the last one is air.
//
// Acceptance of a traced ray is all-or-nothing: rays that miss the aperture,
// miss an interface, or undergo total internal reflection produce no
// contribution and are never resampled.
//
// This library is standalone and only depends on the math and geometry
// headers. The camera integration helpers at the bottom take a camera_data
// declared in yocto_scene.h.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2022 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//

#ifndef _YOCTO_LENS_H_
#define _YOCTO_LENS_H_

// -----------------------------------------------------------------------------
// INCLUDES
// -----------------------------------------------------------------------------

#include <string>
#include <vector>

#include "yocto_geometry.h"
#include "yocto_math.h"

// -----------------------------------------------------------------------------
// USING DIRECTIVES
// -----------------------------------------------------------------------------
namespace yocto {

// using directives
using std::string;
using std::vector;

}  // namespace yocto

// -----------------------------------------------------------------------------
// COMPOUND LENS DATA
// -----------------------------------------------------------------------------
namespace yocto {

// Forward declaration so that the camera helpers below can be declared here.
struct camera_data;

// One spherical interface of a compound lens, in lens-local coordinates.
// z   -- vertex position along the optical axis, in meters
// R   -- curvature radius; the sphere center is at z + R. R = 0 is a plane
//        diaphragm with no refraction
// r   -- effective (clear) radius
// eta -- refractive index after crossing the interface
struct lens_interface {
  float z = 0;
  union {
    float R = 0;
    float curvature;
  };
  union {
    float r = 0;
    float radius;
  };
  union {
    float eta = 1;
    float ior;
  };
};

// Owning configuration of a compound spherical lens. The aperture stop is a
// disk of radius aperture at z = 0 and the film plane is at z = -film_distance.
// An empty interface list disables the compound lens and leaves the legacy
// camera untouched.
struct lens_config {
  union {
    float aperture = 0;
    float a;
  };
  union {
    float film_distance = 0;
    float g;
  };
  vector<lens_interface> interfaces = {};
};

// Convenience aliases for the public configuration types.
using spherical_lens_interface = lens_interface;
using lens_element             = lens_interface;
using lens_surface             = lens_interface;
using spherical_lens_config    = lens_config;
using lens_group_config        = lens_config;

// Result of tracing a single ray through the lens.
struct lens_trace {
  bool  valid         = false;  // false if blocked by geometry or TIR
  vec3f origin        = {0, 0, 0};  // exit point in lens-local coordinates
  vec3f direction     = {0, 0, 1};  // exit direction, normalized
  float transmittance = 0;          // product of Fresnel transmittances
  int   last_interface = -1;        // index of the last crossed interface
};

// Traces a single ray through the aperture and all interfaces. The ray is
// given in lens-local coordinates and is normalized internally. The aperture
// disk at z = 0 is part of the lens, so rays that miss it are reported invalid.
lens_trace trace_lens_ray(const lens_config& lens, const ray3f& ray);

// Alias spelling of the single-ray lens tracer.
inline lens_trace trace_lens(
    const lens_config& lens, const ray3f& ray) {
  return trace_lens_ray(lens, ray);
}

// Checks a lens configuration. Returns false and fills error on failure.
bool validate_lens(const lens_config& lens, string& error);
bool validate_lens(const lens_config& lens);
inline bool check_lens(const lens_config& lens, string& error) {
  return validate_lens(lens, error);
}
inline bool check_lens(const lens_config& lens) { return validate_lens(lens); }

// Result of a mechanical focus solve. film_distance is only meaningful when ok
// is true.
struct lens_focus_result {
  bool  ok            = false;
  float film_distance = 0;
  string error        = {};
};
using lens_focus_solution = lens_focus_result;
using lens_focus          = lens_focus_result;

// Solves the mechanical focus of the lens for a scene plane at z = focus_z.
// The film distance g is searched in [film_distance_min, film_distance_max],
// which has to be finite, positive and non-empty. The on-axis paraxial ray
// leaving the film center through the aperture point (aperture * 1e-3, 0, 0)
// is traced through the lens and its x intercept at z = focus_z is driven to
// zero. Root finding never falls back to a thin-lens formula.
lens_focus_result solve_lens_focus(const lens_config& lens, float focus_z,
    float film_distance_min, float film_distance_max);

}  // namespace yocto

// -----------------------------------------------------------------------------
// CAMERA INTEGRATION
// -----------------------------------------------------------------------------
namespace yocto {

// Whether the camera has a compound lens configured.
bool camera_has_lens(const camera_data& camera);

// The owning lens configuration of a camera.
const lens_config& camera_lens_config(const camera_data& camera);
lens_config&       camera_lens_config(camera_data& camera);

// Validates the compound lens part of a camera before rendering. Legacy
// cameras without a lens group are always valid. Orthographic cameras are
// rejected in compound lens mode.
bool validate_camera_lens(const camera_data& camera, string& error);
bool validate_camera_lens(const camera_data& camera);

// Sets a compound lens on a camera. The configuration is deep copied, so the
// caller may freely modify its own interface array afterwards.
void set_camera_lens(camera_data& camera, const lens_config& lens);

// Mechanically focuses a camera by solving for the film distance. On success
// the solved distance is stored in the camera's own lens configuration. On
// failure the camera is left completely unchanged and error is filled.
bool focus_camera(camera_data& camera, float focus_z, float film_distance_min,
    float film_distance_max, string* error = nullptr);

// A camera sample with a weight. The weight is the physical exposure weight
// pi * a^2 * cos(theta)^4 / g^2 times the transmittance of all interfaces,
// where theta is the angle of the initial film-to-aperture direction with the
// optical axis. Blocked samples have weight zero and valid = false.
struct camera_lens_sample {
  ray3f ray    = {};
  float weight = 0;
  bool  valid  = false;
};

// Samples the compound lens camera at the given image uv (the same convention
// as eval_camera) and area-uniform aperture uv. image_uv is expected to
// already contain the legacy pixel jitter. The returned ray is in world space,
// with lens-local +z mapped to camera.frame -z.
camera_lens_sample sample_camera_lens(const camera_data& camera,
    const vec2f& image_uv, const vec2f& aperture_uv);

}  // namespace yocto

#endif
