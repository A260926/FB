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

#include <microgl/clippers/homo_triangle_clipper.h>
#include <microgl/micro-tess/include/micro-tess/triangles.h>
#include <microgl/math.h>
#include <microgl/math/vertex2.h>
#include <microgl/math/vertex3.h>
#include <microgl/math/vertex4.h>
#include <microgl/z_buffer.h>
#include <microgl/functions/orient2d.h>
#include <microgl/functions/minmax.h>
#include <microgl/functions/swap.h>
#include <microgl/rsm/rsm_math.h>

namespace microgl {
    namespace rsm {

        /**
         * A re-targeting of the canvas 3d triangle pipeline.
         *
         * It runs the exact same stages the canvas runs for a shaded triangle:
         *   1. six-plane homogeneous clipping (homo_triangle_clipper), producing a
         *      fan of clip-space triangles with barycentric weights into the
         *      original triangle;
         *   2. perspective divide and the canvas viewport transform;
         *   3. fixed-point top-left barycentric coverage with the canvas fill rule;
         *   4. nearest-surface-wins depth test against a uint16 depth buffer.
         *
         * Instead of blending byte colors, every covered fragment is handed to a
         * template sink that stores full precision world position, normalized world
         * normal and floating point RGB flux. This is a triangle rasterizer that
         * shares the canvas clipper/coverage/depth machinery, not a per-pixel ray
         * caster.
         */
        template<typename number>
        class rsm_rasterizer {
        public:
            using v2 = vertex2<int>;
            using v3 = vertex3<number>;
            using v4 = vertex4<number>;
            using clipper = microgl::clipping::homo_triangle_clipper<number>;

            /**
             * everything a covered fragment needs. world position and normal travel
             * with the triangle through homogeneous clipping; for the orthographic
             * light every clip vertex shares one constant w, so plain screen-space
             * barycentric weights are identical to perspective-correct weights.
             */
            struct fragment {
                v3 world_position;
                v3 world_normal;       // normalized
                number depth;          // viewport depth in [0,1]
                int x;
                int y;
            };

            /**
             * @param width  light map width in texels (square maps only)
             * @param height light map height
             */
            rsm_rasterizer(int width, int height)
                    : _w(width), _h(height), _depth(width, height) {
            }

            int width() const { return _w; }
            int height() const { return _h; }

            void clear_depth() {
                _depth.clear();
            }

            /**
             * Draw one triangle already expressed in light clip space.
             *
             * @param clip0/1/2      clip-space (post view*projection) vertices
             * @param world0/1/2     world-space positions of the original vertices
             * @param normal0/1/2    world-space normals of the original vertices
             * @param sink           callback invoked for every covered, depth-passing
             *                       fragment; it owns the physical storage
             */
            template<typename Sink>
            void draw_triangle(const v4 &clip0, const v4 &clip1, const v4 &clip2,
                               const v3 &world0, const v3 &world1, const v3 &world2,
                               const v3 &normal0, const v3 &normal1, const v3 &normal2,
                               Sink &sink) {
                typename clipper::vertices_list clipped;
                const bool outside = !clipper::compute(clip0, clip1, clip2, clipped);
                if (outside) return;

                const unsigned triangles = clipped.size() - 2;
                for (unsigned t = 0; t < triangles; ++t) {
                    const v4 &p0 = clipped[0].point;
                    const v4 &p1 = clipped[t + 1].point;
                    const v4 &p2 = clipped[t + 2].point;
                    const vertex4<number> &q0 = clipped[0].bary;
                    const vertex4<number> &q1 = clipped[t + 1].bary;
                    const vertex4<number> &q2 = clipped[t + 2].bary;

                    // world position / normal at each clipped vertex, via the clip
                    // barycentric weights into the original triangle.
                    v3 w0 = bary_world(world0, world1, world2, q0);
                    v3 w1 = bary_world(world0, world1, world2, q1);
                    v3 w2 = bary_world(world0, world1, world2, q2);
                    v3 n0 = bary_normal(normal0, normal1, normal2, q0);
                    v3 n1 = bary_normal(normal0, normal1, normal2, q1);
                    v3 n2 = bary_normal(normal0, normal1, normal2, q2);

                    raster_clipped(p0, p1, p2, w0, w1, w2, n0, n1, n2, sink);
                }
            }

        private:
            using depth_buffer = microgl::z_buffer<16>;
            static constexpr unsigned short depth_max =
                    static_cast<unsigned short>(depth_buffer::max_value);
            static constexpr unsigned char sub_pixel_precision = 8;

            int _w, _h;
            // the exact uint16 nearest-wins depth storage used by canvas triangles.
            depth_buffer _depth;

            // one vertex of a clipped fan triangle, carrying every attribute in
            // parallel so that orientation swaps keep position/normal/depth aligned.
            struct record {
                int sx, sy;           // fixed-point screen position
                v3 world;
                v3 normal;            // normalized
                number depth;         // viewport depth in [0,1]
            };

            static v3 bary_world(const v3 &a, const v3 &b, const v3 &c,
                                 const vertex4<number> &w) {
                return v3{a.x * w.x + b.x * w.y + c.x * w.z,
                          a.y * w.x + b.y * w.y + c.y * w.z,
                          a.z * w.x + b.z * w.y + c.z * w.z};
            }

            static v3 bary_normal(const v3 &a, const v3 &b, const v3 &c,
                                  const vertex4<number> &w) {
                v3 n{a.x * w.x + b.x * w.y + c.x * w.z,
                     a.y * w.x + b.y * w.y + c.y * w.z,
                     a.z * w.x + b.z * w.y + c.z * w.z};
                // accurate (std sqrt) normalization, not the library fixed-point
                // approximation in vec3::normalize.
                return rsm_math::normalize3(n);
            }

            template<typename Sink>
            void raster_clipped(const v4 &p0, const v4 &p1, const v4 &p2,
                                v3 w0, v3 w1, v3 w2, v3 n0, v3 n1, v3 n2,
                                Sink &sink) {
                if (p0.w == number(0) || p1.w == number(0) || p2.w == number(0)) return;

                const v4 ndc0 = p0 / p0.w;
                const v4 ndc1 = p1 / p1.w;
                const v4 ndc2 = p2 / p2.w;

                const number wv = number(_w), hv = number(_h);

                // identical viewport transform to the canvas (y flipped, z in [0,1])
                record r[3];
                r[0] = make_record(ndc0, w0, n0, wv, hv);
                r[1] = make_record(ndc1, w1, n1, wv, hv);
                r[2] = make_record(ndc2, w2, n2, wv, hv);

                long long area = microgl::functions::orient2d<int, long long>(
                        r[0].sx, r[0].sy, r[1].sx, r[1].sy, r[2].sx, r[2].sy,
                        sub_pixel_precision);
                if (area == 0) return; // degenerate, matches the canvas discard

                // the canvas only rasterizes the CW ordering (orient2d > 0); swap a
                // whole record so every attribute follows its screen position.
                if (area < 0) {
                    swap_record(r[1], r[2]);
                    area = -area;
                }

                // bounding box, floor/ceil fixed point exactly like the canvas, then
                // intersected with the light map rectangle.
                const int mask = ~((int(1) << sub_pixel_precision) - 1);
                int min_x = (microgl::functions::min<int>(r[0].sx, r[1].sx, r[2].sx) & mask)
                        >> sub_pixel_precision;
                int min_y = (microgl::functions::min<int>(r[0].sy, r[1].sy, r[2].sy) & mask)
                        >> sub_pixel_precision;
                int max_x = ceil_fixed(
                        microgl::functions::max<int>(r[0].sx, r[1].sx, r[2].sx));
                int max_y = ceil_fixed(
                        microgl::functions::max<int>(r[0].sy, r[1].sy, r[2].sy));
                if (min_x < 0) min_x = 0;
                if (min_y < 0) min_y = 0;
                if (max_x > _w - 1) max_x = _w - 1;
                if (max_y > _h - 1) max_y = _h - 1;
                if (min_x > max_x || min_y > max_y) return;

                // top-left fill rule for a CW triangle (CCW=false), same as canvas.
                microtess::triangles::top_left_t top_left =
                        microtess::triangles::classifyTopLeftEdges(
                                false, r[0].sx, r[0].sy, r[1].sx, r[1].sy,
                                r[2].sx, r[2].sy);
                const int bias_w0 = top_left.first ? 0 : -1;
                const int bias_w1 = top_left.second ? 0 : -1;
                const int bias_w2 = top_left.third ? 0 : -1;

                v2 p_fixed{min_x << sub_pixel_precision,
                           min_y << sub_pixel_precision};
                const int half = (int(1) << sub_pixel_precision) >> 1;
                p_fixed = p_fixed + v2{half, half};

                // edge functions. E01 is opposite r2, E12 opposite r0, E20
                // opposite r1; the top-left bias belongs to the edge itself.
                long long e01_row = (microgl::functions::orient2d<int, long long>(
                        r[0].sx, r[0].sy, r[1].sx, r[1].sy,
                        p_fixed.x, p_fixed.y, 0) + bias_w0) >> sub_pixel_precision;
                long long e12_row = (microgl::functions::orient2d<int, long long>(
                        r[1].sx, r[1].sy, r[2].sx, r[2].sy,
                        p_fixed.x, p_fixed.y, 0) + bias_w1) >> sub_pixel_precision;
                long long e20_row = (microgl::functions::orient2d<int, long long>(
                        r[2].sx, r[2].sy, r[0].sx, r[0].sy,
                        p_fixed.x, p_fixed.y, 0) + bias_w2) >> sub_pixel_precision;

                const long long A01 = r[0].sy - r[1].sy;
                const long long A12 = r[1].sy - r[2].sy;
                const long long A20 = r[2].sy - r[0].sy;
                const long long B01 = r[1].sx - r[0].sx;
                const long long B12 = r[2].sx - r[1].sx;
                const long long B20 = r[0].sx - r[2].sx;

                int index = min_y * _w;
                for (int y = min_y; y <= max_y;
                     ++y, index += _w, e01_row += B01, e12_row += B12, e20_row += B20) {
                    long long e01 = e01_row, e12 = e12_row, e20 = e20_row;
                    for (int x = min_x; x <= max_x;
                         ++x, e01 += A01, e12 += A12, e20 += A20) {
                        const bool in_closure = (e01 | e12 | e20) >= 0;
                        if (!in_closure) continue;
                        const long long e01c = e01 >> sub_pixel_precision;
                        const long long e12c = e12 >> sub_pixel_precision;
                        const long long e20c = e20 >> sub_pixel_precision;
                        const long long area_c = e01c + e12c + e20c;
                        if (area_c == 0) continue;

                        // weight of each record is the function of the edge
                        // opposite it: r0<-E12, r1<-E20, r2<-E01.
                        const number f0 = number(e12c) / number(area_c);
                        const number f1 = number(e20c) / number(area_c);
                        const number f2 = number(e01c) / number(area_c);

                        // linear across a planar triangle; the orthographic light has
                        // a constant clip w so screen weights are already correct.
                        v3 world{r[0].world.x * f0 + r[1].world.x * f1 + r[2].world.x * f2,
                                 r[0].world.y * f0 + r[1].world.y * f1 + r[2].world.y * f2,
                                 r[0].world.z * f0 + r[1].world.z * f1 + r[2].world.z * f2};
                        v3 normal{r[0].normal.x * f0 + r[1].normal.x * f1 + r[2].normal.x * f2,
                                  r[0].normal.y * f0 + r[1].normal.y * f1 + r[2].normal.y * f2,
                                  r[0].normal.z * f0 + r[1].normal.z * f1 + r[2].normal.z * f2};
                        normal = rsm_math::normalize3(normal);
                        const number depth =
                                r[0].depth * f0 + r[1].depth * f1 + r[2].depth * f2;

                        const unsigned short z = static_cast<unsigned short>(
                                depth * number(depth_max));
                        const int z_index = index + x;
                        if (z > _depth[z_index]) continue; // nearest surface wins
                        _depth[z_index] = z;

                        fragment frag;
                        frag.world_position = world;
                        frag.world_normal = normal;
                        frag.depth = depth;
                        frag.x = x;
                        frag.y = y;
                        sink(frag);
                    }
                }
            }

            record make_record(const v4 &ndc, const v3 &world, const v3 &normal,
                               number viewport_w, number viewport_h) const {
                record rec;
                rec.sx = microgl::math::to_fixed(
                        ((ndc.x + number(1)) * viewport_w) / number(2),
                        sub_pixel_precision);
                rec.sy = microgl::math::to_fixed(
                        viewport_h - ((ndc.y + number(1)) * viewport_h) / number(2),
                        sub_pixel_precision);
                rec.depth = (ndc.z + number(1)) / number(2);
                rec.world = world;
                rec.normal = normal;
                return rec;
            }

            static void swap_record(record &a, record &b) {
                record tmp = a;
                a = b;
                b = tmp;
            }

            static int ceil_fixed(int value) {
                const int m = (int(1) << sub_pixel_precision) - 1;
                return (value & m) ? ((value >> sub_pixel_precision) + 1)
                                   : (value >> sub_pixel_precision);
            }
        };
    }
}
