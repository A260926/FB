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

#include <microgl/math/matrix_4x4.h>
#include <microgl/math/vertex3.h>
#include <microgl/math/vertex4.h>

#include <cmath>

namespace microgl {
    namespace rsm {

        /**
         * small, self contained floating point helpers used by the RSM pipeline.
         * the whole RSM module works with float/double CPU math and never encodes
         * its physical quantities into byte colors.
         */
        namespace rsm_math {

            template<typename number>
            inline vertex3<number> sub3(const vertex3<number> &a, const vertex3<number> &b) {
                return vertex3<number>{a.x - b.x, a.y - b.y, a.z - b.z};
            }

            template<typename number>
            inline vertex3<number> add3(const vertex3<number> &a, const vertex3<number> &b) {
                return vertex3<number>{a.x + b.x, a.y + b.y, a.z + b.z};
            }

            template<typename number>
            inline vertex3<number> mul3(const vertex3<number> &a, number s) {
                return vertex3<number>{a.x * s, a.y * s, a.z * s};
            }

            template<typename number>
            inline number dot3(const vertex3<number> &a, const vertex3<number> &b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            }

            template<typename number>
            inline number length3(const vertex3<number> &a) {
                return number(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z));
            }

            template<typename number>
            inline vertex3<number> normalize3(const vertex3<number> &a) {
                const number len = length3(a);
                if (len == number(0)) return a;
                return mul3(a, number(1) / len);
            }

            template<typename number>
            inline vertex3<number> cross3(const vertex3<number> &a, const vertex3<number> &b) {
                return vertex3<number>{
                        a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x};
            }

            /**
             * transform a point (w = 1) by an affine 4x4 matrix.
             */
            template<typename number>
            inline vertex3<number> transform_point(const matrix_4x4<number> &m,
                                                   const vertex3<number> &p) {
                const vertex4<number> out = m * vertex4<number>{p.x, p.y, p.z, number(1)};
                return vertex3<number>{out.x, out.y, out.z};
            }

            /**
             * transform a direction (w = 0) by a 4x4 matrix.
             */
            template<typename number>
            inline vertex3<number> transform_direction(const matrix_4x4<number> &m,
                                                       const vertex3<number> &d) {
                const vertex4<number> out = m * vertex4<number>{d.x, d.y, d.z, number(0)};
                return vertex3<number>{out.x, out.y, out.z};
            }

            /**
             * invert a general (affine) 4x4 matrix using cofactors. the caller
             * guarantees the model matrix is invertible, so no failure branch is
             * required, but a singular matrix simply yields an identity result.
             */
            template<typename number>
            inline matrix_4x4<number> invert4(const matrix_4x4<number> &m) {
                number a[16];
                for (unsigned i = 0; i < 16; ++i) a[i] = m[i];
                number inv[16];

                inv[0] =  a[5] * a[10] * a[15] - a[5] * a[11] * a[14] -
                          a[9] * a[6]  * a[15] + a[9] * a[7]  * a[14] +
                          a[13] * a[6] * a[11] - a[13] * a[7] * a[10];

                inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] +
                          a[8] * a[6]  * a[15] - a[8] * a[7]  * a[14] -
                          a[12] * a[6] * a[11] + a[12] * a[7] * a[10];

                inv[8] =  a[4] * a[9] * a[15] - a[4] * a[11] * a[13] -
                          a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
                          a[12] * a[5] * a[11] - a[12] * a[7] * a[9];

                inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] +
                           a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
                           a[12] * a[5] * a[10] + a[12] * a[6] * a[9];

                inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] +
                          a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
                          a[13] * a[2] * a[11] + a[13] * a[3] * a[10];

                inv[5] =  a[0] * a[10] * a[15] - a[0] * a[11] * a[14] -
                          a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
                          a[12] * a[2] * a[11] - a[12] * a[3] * a[10];

                inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] +
                          a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
                          a[12] * a[1] * a[11] + a[12] * a[3] * a[9];

                inv[13] =  a[0] * a[9] * a[14] - a[0] * a[10] * a[13] -
                           a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
                           a[12] * a[1] * a[10] - a[12] * a[2] * a[9];

                inv[2] =  a[1] * a[6] * a[15] - a[1] * a[7] * a[14] -
                          a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
                          a[13] * a[2] * a[7] - a[13] * a[3] * a[6];

                inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] +
                          a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
                          a[12] * a[2] * a[7] + a[12] * a[3] * a[6];

                inv[10] =  a[0] * a[5] * a[15] - a[0] * a[7] * a[13] -
                           a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
                           a[12] * a[1] * a[7] - a[12] * a[3] * a[5];

                inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] +
                           a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
                           a[12] * a[1] * a[6] + a[12] * a[2] * a[5];

                inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] +
                          a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
                          a[9] * a[2] * a[7] + a[9] * a[3] * a[6];

                inv[7] =  a[0] * a[6] * a[11] - a[0] * a[7] * a[10] -
                          a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
                          a[8] * a[2] * a[7] - a[8] * a[3] * a[6];

                inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] +
                           a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
                           a[8] * a[1] * a[7] + a[8] * a[3] * a[5];

                inv[15] =  a[0] * a[5] * a[10] - a[0] * a[6] * a[9] +
                           a[4] * a[1] * a[10] - a[4] * a[2] * a[9] +
                           a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

                number det = a[0] * inv[0] + a[1] * inv[4] +
                             a[2] * inv[8] + a[3] * inv[12];

                matrix_4x4<number> result;
                if (det == number(0)) {
                    result.identity();
                    return result;
                }
                const number inv_det = number(1) / det;
                for (unsigned i = 0; i < 16; ++i) result[i] = inv[i] * inv_det;
                return result;
            }

            /**
             * normal matrix = transpose of the inverse of the upper 3x3 of the
             * model matrix. normals are transformed as (M^-1)^T * n and the result
             * is normalized later at every sample.
             */
            template<typename number>
            inline matrix_4x4<number> normal_matrix(const matrix_4x4<number> &model) {
                const matrix_4x4<number> inv = invert4(model);
                matrix_4x4<number> tr;
                tr.identity();
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        tr(r, c) = inv(c, r);
                return tr;
            }

        }
    }
}
