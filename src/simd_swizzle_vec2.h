/*
 * MIT License
 *
 * Copyright 2026 Pierre GEISSLER
 *
 * Permission is hereby granted, free of charge, to any person obtaining _a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to USE, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR _a PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
#ifndef SIMD_SWIZZLE_VEC2_H
#define SIMD_SWIZZLE_VEC2_H

#include "simd_vec2.h"
#include "simd_vec3.h"
#include "simd_vec4.h"
#include "simd_ivec2.h"

#pragma region vec2 swizzle implementations
// vec2 swizzle implementations
inline vec2 vec2::gg() const { return { g, g }; }
inline vec3 vec2::ggg() const { return { g, g, g }; }
inline vec4 vec2::gggg() const { return { g, g, g, g }; }
inline vec4 vec2::gggr() const { return { g, g, g, r }; }
inline vec3 vec2::ggr() const { return { g, g, r }; }
inline vec4 vec2::ggrg() const { return { g, g, r, g }; }
inline vec4 vec2::ggrr() const { return { g, g, r, r }; }
inline vec2 vec2::gr() const { return { g, r }; }
inline vec3 vec2::grg() const { return { g, r, g }; }
inline vec4 vec2::grgg() const { return { g, r, g, g }; }
inline vec4 vec2::grgr() const { return { g, r, g, r }; }
inline vec3 vec2::grr() const { return { g, r, r }; }
inline vec4 vec2::grrg() const { return { g, r, r, g }; }
inline vec4 vec2::grrr() const { return { g, r, r, r }; }
inline vec2 vec2::rg() const { return { r, g }; }
inline vec3 vec2::rgg() const { return { r, g, g }; }
inline vec4 vec2::rggg() const { return { r, g, g, g }; }
inline vec4 vec2::rggr() const { return { r, g, g, r }; }
inline vec3 vec2::rgr() const { return { r, g, r }; }
inline vec4 vec2::rgrg() const { return { r, g, r, g }; }
inline vec4 vec2::rgrr() const { return { r, g, r, r }; }
inline vec2 vec2::rr() const { return { r, r }; }
inline vec3 vec2::rrg() const { return { r, r, g }; }
inline vec4 vec2::rrgg() const { return { r, r, g, g }; }
inline vec4 vec2::rrgr() const { return { r, r, g, r }; }
inline vec3 vec2::rrr() const { return { r, r, r }; }
inline vec4 vec2::rrrg() const { return { r, r, r, g }; }
inline vec4 vec2::rrrr() const { return { r, r, r, r }; }
inline vec2 vec2::ss() const { return { s, s }; }
inline vec3 vec2::sss() const { return { s, s, s }; }
inline vec4 vec2::ssss() const { return { s, s, s, s }; }
inline vec4 vec2::ssst() const { return { s, s, s, t }; }
inline vec3 vec2::sst() const { return { s, s, t }; }
inline vec4 vec2::ssts() const { return { s, s, t, s }; }
inline vec4 vec2::sstt() const { return { s, s, t, t }; }
inline vec2 vec2::st() const { return { s, t }; }
inline vec3 vec2::sts() const { return { s, t, s }; }
inline vec4 vec2::stss() const { return { s, t, s, s }; }
inline vec4 vec2::stst() const { return { s, t, s, t }; }
inline vec3 vec2::stt() const { return { s, t, t }; }
inline vec4 vec2::stts() const { return { s, t, t, s }; }
inline vec4 vec2::sttt() const { return { s, t, t, t }; }
inline vec2 vec2::ts() const { return { t, s }; }
inline vec3 vec2::tss() const { return { t, s, s }; }
inline vec4 vec2::tsss() const { return { t, s, s, s }; }
inline vec4 vec2::tsst() const { return { t, s, s, t }; }
inline vec3 vec2::tst() const { return { t, s, t }; }
inline vec4 vec2::tsts() const { return { t, s, t, s }; }
inline vec4 vec2::tstt() const { return { t, s, t, t }; }
inline vec2 vec2::tt() const { return { t, t }; }
inline vec3 vec2::tts() const { return { t, t, s }; }
inline vec4 vec2::ttss() const { return { t, t, s, s }; }
inline vec4 vec2::ttst() const { return { t, t, s, t }; }
inline vec3 vec2::ttt() const { return { t, t, t }; }
inline vec4 vec2::ttts() const { return { t, t, t, s }; }
inline vec4 vec2::tttt() const { return { t, t, t, t }; }
inline vec2 vec2::xx() const { return { x, x }; }
inline vec3 vec2::xxx() const { return { x, x, x }; }
inline vec4 vec2::xxxx() const { return { x, x, x, x }; }
inline vec4 vec2::xxxy() const { return { x, x, x, y }; }
inline vec3 vec2::xxy() const { return { x, x, y }; }
inline vec4 vec2::xxyx() const { return { x, x, y, x }; }
inline vec4 vec2::xxyy() const { return { x, x, y, y }; }
inline vec2 vec2::xy() const { return { x, y }; }
inline vec3 vec2::xyx() const { return { x, y, x }; }
inline vec4 vec2::xyxx() const { return { x, y, x, x }; }
inline vec4 vec2::xyxy() const { return { x, y, x, y }; }
inline vec3 vec2::xyy() const { return { x, y, y }; }
inline vec4 vec2::xyyx() const { return { x, y, y, x }; }
inline vec4 vec2::xyyy() const { return { x, y, y, y }; }
inline vec2 vec2::yx() const { return { y, x }; }
inline vec3 vec2::yxx() const { return { y, x, x }; }
inline vec4 vec2::yxxx() const { return { y, x, x, x }; }
inline vec4 vec2::yxxy() const { return { y, x, x, y }; }
inline vec3 vec2::yxy() const { return { y, x, y }; }
inline vec4 vec2::yxyx() const { return { y, x, y, x }; }
inline vec4 vec2::yxyy() const { return { y, x, y, y }; }
inline vec2 vec2::yy() const { return { y, y }; }
inline vec3 vec2::yyx() const { return { y, y, x }; }
inline vec4 vec2::yyxx() const { return { y, y, x, x }; }
inline vec4 vec2::yyxy() const { return { y, y, x, y }; }
inline vec3 vec2::yyy() const { return { y, y, y }; }
inline vec4 vec2::yyyx() const { return { y, y, y, x }; }
inline vec4 vec2::yyyy() const { return { y, y, y, y }; }
inline void vec2::gr(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; g = s0_; r = s1_; }
inline void vec2::gr(const vec2& _val) {  const simdfloat s0_ = _val.x; const simdfloat s1_ = _val.y; g = s0_; r = s1_; }
inline void vec2::gr(const simdfloat& _val) {  const simdfloat s0_ = _val; g = s0_; r = s0_; }
inline void vec2::rg(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; r = s0_; g = s1_; }
inline void vec2::rg(const vec2& _val) {  const simdfloat s0_ = _val.x; const simdfloat s1_ = _val.y; r = s0_; g = s1_; }
inline void vec2::rg(const simdfloat& _val) {  const simdfloat s0_ = _val; r = s0_; g = s0_; }
inline void vec2::st(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; s = s0_; t = s1_; }
inline void vec2::st(const vec2& _val) {  const simdfloat s0_ = _val.s; const simdfloat s1_ = _val.t; s = s0_; t = s1_; }
inline void vec2::st(const simdfloat& _val) {  const simdfloat s0_ = _val; s = s0_; t = s0_; }
inline void vec2::ts(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; t = s0_; s = s1_; }
inline void vec2::ts(const vec2& _val) {  const simdfloat s0_ = _val.s; const simdfloat s1_ = _val.t; t = s0_; s = s1_; }
inline void vec2::ts(const simdfloat& _val) {  const simdfloat s0_ = _val; t = s0_; s = s0_; }
inline void vec2::xy(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; x = s0_; y = s1_; }
inline void vec2::xy(const vec2& _val) {  const simdfloat s0_ = _val.x; const simdfloat s1_ = _val.y; x = s0_; y = s1_; }
inline void vec2::xy(const simdfloat& _val) {  const simdfloat s0_ = _val; x = s0_; y = s0_; }
inline void vec2::yx(const simdfloat& _val0, const simdfloat& _val1) {  const simdfloat s0_ = _val0; const simdfloat s1_ = _val1; y = s0_; x = s1_; }
inline void vec2::yx(const vec2& _val) {  const simdfloat s0_ = _val.x; const simdfloat s1_ = _val.y; y = s0_; x = s1_; }
inline void vec2::yx(const simdfloat& _val) {  const simdfloat s0_ = _val; y = s0_; x = s0_; }

#pragma endregion

// cross-type assignment from ivec2 (components use simdfloat's own conversion operator)
inline vec2 vec2::operator=(const ivec2& _v) {  const simdint s0_ = _v.x; const simdint s1_ = _v.y; x = s0_; y = s1_; return *this; }

inline vec2 vec2::operator=(const vec3& _v) {  const simdfloat s0_ = _v.x; const simdfloat s1_ = _v.y; x = s0_; y = s1_; return *this; }

inline vec2 vec2::operator=(const vec4& _v) {  const simdfloat s0_ = _v.x; const simdfloat s1_ = _v.y; x = s0_; y = s1_; return *this; }

// constructors for vec2
inline vec2::vec2(const float& _f) { shader::detail::full_store(x, _f); shader::detail::full_store(y, _f); }

inline vec2::vec2(const simdfloat& _f) { shader::detail::full_store(x, _f); shader::detail::full_store(y, _f); }

inline vec2::vec2(const ivec2& _v) { shader::detail::full_store(x, _v.x); shader::detail::full_store(y, _v.y); }

inline vec2::vec2(const simdfloat& _x, const simdfloat& _y) { shader::detail::full_store(x, _x); shader::detail::full_store(y, _y); }

template<typename V, std::enable_if_t<std::is_same_v<V, vec3>, int>>
vec2::vec2(const simdfloat& _x, const V& _v) { shader::detail::full_store(x, _x); shader::detail::full_store(y, _v.x); }

template<typename V, std::enable_if_t<std::is_same_v<V, vec4>, int>>
vec2::vec2(const simdfloat& _x, const V& _v) { shader::detail::full_store(x, _x); shader::detail::full_store(y, _v.x); }

#endif


