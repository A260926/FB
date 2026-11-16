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

#include <microgl/shaders/shader.h>
#include <microgl/math/matrix_4x4.h>
#include <microgl/functions/clamp.h>
#include <microgl/rsm/rsm_math.h>

namespace microgl {
    namespace rsm {

        template<typename number>
        class Rsm;

        template<typename number>
        struct rsm_rgb;

        /**
         * Canvas shader that shades receiver triangles with one-bounce RSM
         * irradiance. It is a normal micro-gl shader and therefore participates in
         * the ordinary six-plane clipping, perspective-correct varying interpolation
         * and canvas depth test/update; it only reads the captured RSM, never writes
         * it.
         *
         * World position and world normal are vertex varyings, so they travel the
         * same clipping / perspective varying path as any other shaded attribute.
         * The normal is normalized again per fragment before transport. The result
         * stays linear until the final byte conversion done here, which is the only
         * place values are truncated.
         *
         * Shaders without RSM (e.g. microgl::shading::color_shader) are untouched and
         * keep their original interface and output.
         */
        template<typename number_, typename rgba_>
        struct rsm_receiver_shader {
            using number = number_;
            using rgba = rgba_;
            using gl_position = vertex4<number>;

            struct vertex_attributes {
                /** local-space position. */
                vertex3<number> point;
                /** local-space normal (non zero). */
                vertex3<number> normal;
            };

            struct varying {
                vertex3<number> world_position;
                vertex3<number> world_normal;

                void interpolate(const varying &a, const varying &b, const varying &c,
                                 const vertex4<int> &bary) {
                    world_position.x = (a.world_position.x * bary.x +
                                        b.world_position.x * bary.y +
                                        c.world_position.x * bary.z) / bary.w;
                    world_position.y = (a.world_position.y * bary.x +
                                        b.world_position.y * bary.y +
                                        c.world_position.y * bary.z) / bary.w;
                    world_position.z = (a.world_position.z * bary.x +
                                        b.world_position.z * bary.y +
                                        c.world_position.z * bary.z) / bary.w;
                    world_normal.x = (a.world_normal.x * bary.x +
                                      b.world_normal.x * bary.y +
                                      c.world_normal.x * bary.z) / bary.w;
                    world_normal.y = (a.world_normal.y * bary.x +
                                      b.world_normal.y * bary.y +
                                      c.world_normal.y * bary.z) / bary.w;
                    world_normal.z = (a.world_normal.z * bary.x +
                                      b.world_normal.z * bary.y +
                                      c.world_normal.z * bary.z) / bary.w;
                }
            };

            /** full camera transform projection * view * model (local -> clip). */
            matrix_4x4<number> camera_matrix;
            /** local -> world model matrix. */
            matrix_4x4<number> model;
            /** inverse-transpose normal matrix of the model. */
            matrix_4x4<number> normal_matrix;

            /** captured snapshot, read only; may be null when RSM is disabled. */
            const Rsm<number> *rsm = nullptr;
            /** linear receiver reflectance, channels in [0,1]. */
            rsm_rgb<number> receiver_reflectance{number(1), number(1), number(1)};
            number ambient = number(0);
            number gain = number(1);
            number epsilon = number(1e-4);

            inline gl_position
            vertex(const vertex_attributes &attributes, varying &output) {
                const vertex4<number> world4 =
                        model * vertex4<number>{attributes.point.x,
                                                attributes.point.y,
                                                attributes.point.z, number(1)};
                output.world_position = vertex3<number>{world4.x, world4.y, world4.z};
                const vertex4<number> n4 =
                        normal_matrix * vertex4<number>{attributes.normal.x,
                                                        attributes.normal.y,
                                                        attributes.normal.z, number(0)};
                output.world_normal =
                        rsm_math::normalize3(vertex3<number>{n4.x, n4.y, n4.z});
                return camera_matrix *
                       vertex4<number>{attributes.point.x, attributes.point.y,
                                       attributes.point.z, number(1)};
            }

            inline microgl::color_t
            fragment(const varying &input) {
                microgl::color_t out{0, 0, 0, 255};
                if (rsm == nullptr) return out;

                const vertex3<number> n = rsm_math::normalize3(input.world_normal);
                const rsm_rgb<number> linear = rsm->shade(
                        input.world_position, n, receiver_reflectance,
                        ambient, gain, epsilon);
                out.r = to_byte(linear.r);
                out.g = to_byte(linear.g);
                out.b = to_byte(linear.b);
                out.a = 255;
                return out;
            }

        private:
            static microgl::channel_t to_byte(number v) {
                if (v < number(0)) v = number(0);
                int q = int(v * number(255) + number(0.5));
                q = microgl::functions::clamp<int>(q, 0, 255);
                return static_cast<microgl::channel_t>(q);
            }
        };
    }
}
