/*========================================================================================
 Copyright (2021), Tomer Shalev (tomer.shalev@gmail.com, https://github.com/HendrixString).
 All Rights Reserved.
 License is a custom open source semi-permissive license with the following guidelines:
 1. unless otherwise stated, derivative work and usage of this file is permitted and
    should be credited to the project and the author of this project.
 2. Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
========================================================================================*/
#pragma once

/**
 * @file Rsm.h
 *
 * Reflective Shadow Map (RSM) capture and one-bounce diffuse color bleeding on top
 * of the micro-gl CPU triangle pipeline. This header is self contained and is the
 * single public entry point of the RSM module.
 *
 * The approximation is deterministic and discrete:
 *  - the light view renders the scene into a square light map using the same
 *    six-plane homogeneous clipping, top-left barycentric coverage and
 *    nearest-wins uint16 depth path as canvas::drawTriangle;
 *  - every light-map texel stores a validity bit, a world position, a normalized
 *    world normal and a floating point RGB flux (never byte encoded);
 *  - receivers sum the contribution of every valid texel. No random sampling, no
 *    renormalization by the number of valid texels, and no secondary occlusion
 *    between the reflective surface and the receiver.
 */

#include <microgl/rsm/rsm_math.h>
#include <microgl/rsm/rsm_rasterizer.h>

#include <vector>
#include <cstddef>
#include <cmath>

namespace microgl {
    namespace rsm {

        /** linear, unclamped RGB triplet. */
        template<typename number>
        struct rsm_rgb {
            number r;
            number g;
            number b;
        };

        /**
         * Orthographic directional light description.
         * view/projection transform world points into light clip space; the
         * aperture dimensions measure the world footprint of the light map
         * perpendicular to the propagation direction.
         */
        template<typename number>
        struct rsm_light {
            matrix_4x4<number> view;
            matrix_4x4<number> projection;
            /** unit direction in which the light propagates. */
            vertex3<number> light_direction;
            /** world width of the light aperture. */
            number world_width;
            /** world height of the light aperture. */
            number world_height;
            /** incident RGB irradiance E. */
            rsm_rgb<number> irradiance;
        };

        /**
         * One capture surface: local-space triangles sharing one invertible
         * affine model matrix.
         */
        template<typename number>
        struct rsm_surface {
            /** 3 * triangle_count local-space positions. */
            const vertex3<number> *positions;
            /** 3 * triangle_count local-space normals (non zero). */
            const vertex3<number> *normals;
            /** triangle_count per-face linear RGB reflectance, each channel in [0,1]. */
            const rsm_rgb<number> *reflectance;
            unsigned triangle_count;
            /** invertible affine model matrix (local -> world). */
            matrix_4x4<number> model;
        };

        /**
         * One light-map texel. All physical quantities keep their native floating
         * point precision; the validity bit replaces any byte color encoding.
         */
        template<typename number>
        struct rsm_texel {
            bool valid = false;
            vertex3<number> world_position{number(0), number(0), number(0)};
            vertex3<number> normal{number(0), number(0), number(1)};
            rsm_rgb<number> flux{number(0), number(0), number(0)};
        };

        /**
         * Owns one reflective shadow map.
         *
         * The snapshot stores its own data: clearing or reusing any external canvas,
         * mutating the input arrays after capture or drawing another camera never
         * changes it. A successful capture replaces the previous snapshot; a failed
         * capture leaves the previous snapshot untouched.
         */
        template<typename number>
        class Rsm {
        public:
            using rgb = rsm_rgb<number>;
            using vec3 = vertex3<number>;
            using mat4 = matrix_4x4<number>;

            Rsm() = default;

            /** square light map side length, 0 before the first successful capture. */
            unsigned mapSize() const { return _map_size; }

            /** whether at least one successful capture exists. */
            bool hasCapture() const { return _map_size != 0; }

            /**
             * Capture a scene made of several surfaces (each with its own model
             * matrix) into a fresh light map.
             *
             * @return true on success. On invalid arguments the previous snapshot is
             *         preserved and false is returned. Degenerate triangles are
             *         silently ignored.
             */
            bool capture(const rsm_surface<number> *surfaces, unsigned surface_count,
                         const rsm_light<number> &light, unsigned map_size) {
                if (map_size < 8 || map_size > 64) return false;
                if (surfaces == nullptr || surface_count == 0) return false;
                for (unsigned s = 0; s < surface_count; ++s) {
                    const auto &surface = surfaces[s];
                    if (surface.positions == nullptr || surface.normals == nullptr ||
                        surface.reflectance == nullptr || surface.triangle_count == 0)
                        return false;
                }
                if (!(light.world_width > number(0)) ||
                    !(light.world_height > number(0)))
                    return false;

                const unsigned texel_count = map_size * map_size;
                std::vector<rsm_texel<number> > next(texel_count);
                const int map_side = int(map_size);
                rsm_rasterizer<number> raster(map_side, map_side);

                const vec3 light_dir = rsm_math::normalize3(light.light_direction);
                const mat4 view_projection = light.projection * light.view;
                const number pi = number(3.14159265358979323846);
                // projected world area carried by every light-map texel.
                const number texel_area =
                        (light.world_width * light.world_height) / number(texel_count);

                for (unsigned s = 0; s < surface_count; ++s) {
                    const auto &surface = surfaces[s];
                    const mat4 normal_mat =
                            rsm_math::normal_matrix(surface.model);

                    for (unsigned t = 0; t < surface.triangle_count; ++t) {
                        const unsigned base = t * 3u;
                        const vec3 local[3] = {
                                surface.positions[base + 0],
                                surface.positions[base + 1],
                                surface.positions[base + 2]};
                        const vec3 local_n[3] = {
                                surface.normals[base + 0],
                                surface.normals[base + 1],
                                surface.normals[base + 2]};

                        vec3 world[3];
                        vec3 world_n[3];
                        for (int k = 0; k < 3; ++k) {
                            world[k] = rsm_math::transform_point(surface.model, local[k]);
                            // normals use the inverse-transpose of the model matrix.
                            world_n[k] = rsm_math::normalize3(rsm_math::transform_direction(
                                    normal_mat, local_n[k]));
                        }

                        // degenerate triangles contribute nothing and must not reach
                        // the rasterizer.
                        const vec3 e1 = rsm_math::sub3(world[1], world[0]);
                        const vec3 e2 = rsm_math::sub3(world[2], world[0]);
                        if (rsm_math::length3(rsm_math::cross3(e1, e2)) == number(0))
                            continue;

                        vertex4<number> clip[3];
                        for (int k = 0; k < 3; ++k)
                            clip[k] = view_projection *
                                      vertex4<number>{world[k].x, world[k].y,
                                                      world[k].z, number(1)};

                        capture_sink sink{next.data(), map_size, light_dir,
                                          surface.reflectance[t], light.irradiance,
                                          texel_area, pi};
                        raster.draw_triangle(clip[0], clip[1], clip[2],
                                             world[0], world[1], world[2],
                                             world_n[0], world_n[1], world_n[2], sink);
                    }
                }

                // commit only once the whole capture succeeded.
                _texels.swap(next);
                _map_size = map_size;
                return true;
            }

            /** convenience capture for a single surface. */
            bool capture(const rsm_surface<number> &surface,
                         const rsm_light<number> &light, unsigned map_size) {
                const rsm_surface<number> *one = &surface;
                return capture(one, 1, light, map_size);
            }

            // ------------------------------------------------------------------
            // read-only texel queries
            // ------------------------------------------------------------------

            bool inBounds(int x, int y) const {
                const int n = int(_map_size);
                return _map_size != 0 && x >= 0 && y >= 0 && x < n && y < n;
            }

            /** whether the texel holds a nearest-surface sample. */
            bool texelValid(int x, int y) const {
                return inBounds(x, y) && _texels[index(x, y)].valid;
            }

            /** world position of the nearest surface at this texel. */
            vec3 texelPosition(int x, int y) const {
                if (!inBounds(x, y)) return vec3{number(0), number(0), number(0)};
                return _texels[index(x, y)].world_position;
            }

            /** normalized world normal of the nearest surface at this texel. */
            vec3 texelNormal(int x, int y) const {
                if (!inBounds(x, y)) return vec3{number(0), number(0), number(1)};
                return _texels[index(x, y)].normal;
            }

            /**
             * floating point RGB flux at this texel; returns false (and zeroes) for
             * an out-of-range or invalid texel.
             */
            bool texelFlux(int x, int y, rgb &out) const {
                if (!inBounds(x, y)) {
                    out = rgb{number(0), number(0), number(0)};
                    return false;
                }
                out = _texels[index(x, y)].flux;
                return _texels[index(x, y)].valid;
            }

            // ------------------------------------------------------------------
            // transport / receiver shading
            // ------------------------------------------------------------------

            /**
             * Raw discrete transport sum over every valid light-map texel:
             *
             *   sum flux * max(dot(ns,d),0) * max(dot(n,-d),0) / r^2
             *
             * with d = (p - s)/r and sources at distance r <= epsilon skipped.
             * Every texel is visited deterministically; the count of valid texels
             * does not renormalize the result.
             */
            rgb accumulate(const vec3 &p, const vec3 &n, number epsilon) const {
                rgb sum{number(0), number(0), number(0)};
                if (!hasCapture()) return sum;

                const unsigned n_texels = _map_size * _map_size;
                for (unsigned i = 0; i < n_texels; ++i) {
                    const rsm_texel<number> &src = _texels[i];
                    if (!src.valid) continue;

                    const vec3 delta = rsm_math::sub3(p, src.world_position);
                    const number r2 = rsm_math::dot3(delta, delta);
                    const number r = number(std::sqrt(r2));
                    if (!(r > epsilon)) continue; // skip r <= epsilon (and guards r==0)

                    const vec3 d = delta / r;
                    number source_facing = rsm_math::dot3(src.normal, d);
                    if (source_facing < number(0)) source_facing = number(0);
                    number receiver_facing = rsm_math::dot3(n, -d);
                    if (receiver_facing < number(0)) receiver_facing = number(0);
                    if (source_facing == number(0) || receiver_facing == number(0))
                        continue;

                    const number weight =
                            (source_facing * receiver_facing) / r2;
                    sum.r += src.flux.r * weight;
                    sum.g += src.flux.g * weight;
                    sum.b += src.flux.b * weight;
                }
                return sum;
            }

            /**
             * Linear receiver color:
             *
             *   reflectance * (ambient + gain * accumulate(p, n, epsilon) / pi)
             *
             * Nothing is clamped to bytes here; the camera shader truncates the
             * result when it is written to a canvas.
             */
            rgb shade(const vec3 &p, const vec3 &n, const rgb &receiver_reflectance,
                      number ambient, number gain, number epsilon) const {
                const rgb sum = accumulate(p, n, epsilon);
                const number pi = number(3.14159265358979323846);
                const rgb out{
                        receiver_reflectance.r *
                        (ambient + gain * sum.r / pi),
                        receiver_reflectance.g *
                        (ambient + gain * sum.g / pi),
                        receiver_reflectance.b *
                        (ambient + gain * sum.b / pi)};
                return out;
            }

        private:
            struct capture_sink {
                rsm_texel<number> *map;
                unsigned map_size;
                vec3 light_direction;
                rgb reflectance;
                rgb irradiance;
                number texel_area;
                number pi;

                void operator()(const typename rsm_rasterizer<number>::fragment &f) {
                    rsm_texel<number> &texel = map[f.y * map_size + f.x];
                    texel.valid = true;
                    texel.world_position = f.world_position;
                    texel.normal = f.world_normal;

                    // double-sided occlusion: a back-facing nearest surface keeps its
                    // position/depth but stores zero flux.
                    const number facing =
                            rsm_math::dot3(f.world_normal, -light_direction);
                    if (facing <= number(0)) {
                        texel.flux = rgb{number(0), number(0), number(0)};
                    } else {
                        const number scale = texel_area / pi;
                        texel.flux = rgb{
                                reflectance.r * irradiance.r * scale,
                                reflectance.g * irradiance.g * scale,
                                reflectance.b * irradiance.b * scale};
                    }
                }
            };

            unsigned index(int x, int y) const {
                return unsigned(y) * _map_size + unsigned(x);
            }

            unsigned _map_size = 0;
            std::vector<rsm_texel<number> > _texels;
        };
    }
}

// the receiver shader is a canvas shader that reads the fully defined Rsm above;
// include it last so its fragment stage can call Rsm::shade().
#include <microgl/rsm/rsm_receiver_shader.h>
