#include "plutovg-private.h"
#include "plutovg-utils.h"

#include <assert.h>
#include <limits.h>

#define COLOR_TABLE_SIZE 1024
typedef struct {
    plutovg_matrix_t matrix;
    plutovg_spread_method_t spread;
    uint32_t colortable[COLOR_TABLE_SIZE];
    union {
        struct {
            float x1, y1;
            float x2, y2;
        } linear;
        struct {
            float cx, cy, cr;
            float fx, fy, fr;
        } radial;
    } values;
} gradient_data_t;

typedef struct {
    plutovg_matrix_t matrix;
    uint8_t* data;
    int width;
    int height;
    int stride;
    int const_alpha;
} texture_data_t;

typedef struct {
    float dx;
    float dy;
    float l;
    float off;
} linear_gradient_values_t;

typedef struct {
    float dx;
    float dy;
    float dr;
    float sqrfr;
    float a;
    bool extended;
} radial_gradient_values_t;

static inline uint32_t premultiply_color_with_opacity(const plutovg_color_t* color, float opacity)
{
    uint32_t alpha = lroundf(color->a * opacity * 255);
    uint32_t pr = lroundf(color->r * alpha);
    uint32_t pg = lroundf(color->g * alpha);
    uint32_t pb = lroundf(color->b * alpha);
    return (alpha << 24) | (pr << 16) | (pg << 8) | (pb);
}

static inline uint32_t INTERPOLATE_PIXEL_255(uint32_t x, uint32_t a, uint32_t y, uint32_t b)
{
    uint32_t t = (x & 0xff00ff) * a + (y & 0xff00ff) * b;
    t = (t + ((t >> 8) & 0xff00ff) + 0x800080) >> 8;
    t &= 0xff00ff;

    x = ((x >> 8) & 0xff00ff) * a + ((y >> 8) & 0xff00ff) * b;
    x = (x + ((x >> 8) & 0xff00ff) + 0x800080);
    x &= 0xff00ff00;
    x |= t;
    return x;
}

static inline uint32_t INTERPOLATE_PIXEL_256(uint32_t x, uint32_t a, uint32_t y, uint32_t b)
{
    uint32_t t = (x & 0xff00ff) * a + (y & 0xff00ff) * b;
    t >>= 8;
    t &= 0xff00ff;

    x = ((x >> 8) & 0xff00ff) * a + ((y >> 8) & 0xff00ff) * b;
    x &= 0xff00ff00;
    x |= t;
    return x;
}

static inline uint32_t BYTE_MUL(uint32_t x, uint32_t a)
{
    uint32_t t = (x & 0xff00ff) * a;
    t = (t + ((t >> 8) & 0xff00ff) + 0x800080) >> 8;
    t &= 0xff00ff;

    x = ((x >> 8) & 0xff00ff) * a;
    x = (x + ((x >> 8) & 0xff00ff) + 0x800080);
    x &= 0xff00ff00;
    x |= t;
    return x;
}

#ifdef __SSE2__

#include <emmintrin.h>

void plutovg_memfill32(unsigned int* dest, int length, unsigned int value)
{
    __m128i vector_data = _mm_set_epi32(value, value, value, value);
    while(length && ((uintptr_t)dest & 0xf)) {
        *dest++ = value;
        length--;
    }

    while(length >= 32) {
        _mm_store_si128((__m128i*)(dest), vector_data);
        _mm_store_si128((__m128i*)(dest + 4), vector_data);
        _mm_store_si128((__m128i*)(dest + 8), vector_data);
        _mm_store_si128((__m128i*)(dest + 12), vector_data);
        _mm_store_si128((__m128i*)(dest + 16), vector_data);
        _mm_store_si128((__m128i*)(dest + 20), vector_data);
        _mm_store_si128((__m128i*)(dest + 24), vector_data);
        _mm_store_si128((__m128i*)(dest + 28), vector_data);

        dest += 32;
        length -= 32;
    }

    if(length >= 16) {
        _mm_store_si128((__m128i*)(dest), vector_data);
        _mm_store_si128((__m128i*)(dest + 4), vector_data);
        _mm_store_si128((__m128i*)(dest + 8), vector_data);
        _mm_store_si128((__m128i*)(dest + 12), vector_data);

        dest += 16;
        length -= 16;
    }

    if(length >= 8) {
        _mm_store_si128((__m128i*)(dest), vector_data);
        _mm_store_si128((__m128i*)(dest + 4), vector_data);

        dest += 8;
        length -= 8;
    }

    if(length >= 4) {
        _mm_store_si128((__m128i*)(dest), vector_data);

        dest += 4;
        length -= 4;
    }

    while(length) {
        *dest++ = value;
        length--;
    }
}

#else

void plutovg_memfill32(unsigned int* dest, int length, unsigned int value)
{
    while(length--) {
        *dest++ = value;
    }
}

#endif // __SSE2__

static inline int gradient_clamp(const gradient_data_t* gradient, int ipos)
{
    if(gradient->spread == PLUTOVG_SPREAD_METHOD_REPEAT) {
        ipos = ipos % COLOR_TABLE_SIZE;
        ipos = ipos < 0 ? COLOR_TABLE_SIZE + ipos : ipos;
    } else if(gradient->spread == PLUTOVG_SPREAD_METHOD_REFLECT) {
        const int limit = COLOR_TABLE_SIZE * 2;
        ipos = ipos % limit;
        ipos = ipos < 0 ? limit + ipos : ipos;
        ipos = ipos >= COLOR_TABLE_SIZE ? limit - 1 - ipos : ipos;
    } else {
        if(ipos < 0) {
            ipos = 0;
        } else if(ipos >= COLOR_TABLE_SIZE) {
            ipos = COLOR_TABLE_SIZE - 1;
        }
    }

    return ipos;
}

#define FIXPT_BITS 8
#define FIXPT_SIZE (1 << FIXPT_BITS)
static inline uint32_t gradient_pixel_fixed(const gradient_data_t* gradient, int fixed_pos)
{
    int ipos = (fixed_pos + (FIXPT_SIZE / 2)) >> FIXPT_BITS;
    return gradient->colortable[gradient_clamp(gradient, ipos)];
}

static inline uint32_t gradient_pixel(const gradient_data_t* gradient, float pos)
{
    int ipos = (int)(pos * (COLOR_TABLE_SIZE - 1) + 0.5f);
    return gradient->colortable[gradient_clamp(gradient, ipos)];
}

static void fetch_linear_gradient(uint32_t* buffer, const linear_gradient_values_t* v, const gradient_data_t* gradient, int y, int x, int length)
{
    float t, inc;
    float rx = 0, ry = 0;

    if(v->l == 0.f) {
        t = inc = 0;
    } else {
        rx = gradient->matrix.c * (y + 0.5f) + gradient->matrix.a * (x + 0.5f) + gradient->matrix.e;
        ry = gradient->matrix.d * (y + 0.5f) + gradient->matrix.b * (x + 0.5f) + gradient->matrix.f;
        t = v->dx * rx + v->dy * ry + v->off;
        inc = v->dx * gradient->matrix.a + v->dy * gradient->matrix.b;
        t *= (COLOR_TABLE_SIZE - 1);
        inc *= (COLOR_TABLE_SIZE - 1);
    }

    const uint32_t* end = buffer + length;
    if(inc > -1e-5f && inc < 1e-5f) {
        plutovg_memfill32(buffer, length, gradient_pixel_fixed(gradient, (int)(t * FIXPT_SIZE)));
    } else {
        if(t + inc * length < (float)(INT_MAX >> (FIXPT_BITS + 1)) && t + inc * length > (float)(INT_MIN >> (FIXPT_BITS + 1))) {
            int t_fixed = (int)(t * FIXPT_SIZE);
            int inc_fixed = (int)(inc * FIXPT_SIZE);
            while(buffer < end) {
                *buffer = gradient_pixel_fixed(gradient, t_fixed);
                t_fixed += inc_fixed;
                ++buffer;
            }
        } else {
            while(buffer < end) {
                *buffer = gradient_pixel(gradient, t / COLOR_TABLE_SIZE);
                t += inc;
                ++buffer;
            }
        }
    }
}

static void fetch_radial_gradient(uint32_t* buffer, const radial_gradient_values_t* v, const gradient_data_t* gradient, int y, int x, int length)
{
    if(v->a == 0.f) {
        plutovg_memfill32(buffer, length, 0);
        return;
    }

    float rx = gradient->matrix.c * (y + 0.5f) + gradient->matrix.e + gradient->matrix.a * (x + 0.5f);
    float ry = gradient->matrix.d * (y + 0.5f) + gradient->matrix.f + gradient->matrix.b * (x + 0.5f);

    rx -= gradient->values.radial.fx;
    ry -= gradient->values.radial.fy;

    float inv_a = 1.f / (2.f * v->a);
    float delta_rx = gradient->matrix.a;
    float delta_ry = gradient->matrix.b;

    float b = 2 * (v->dr * gradient->values.radial.fr + rx * v->dx + ry * v->dy);
    float delta_b = 2 * (delta_rx * v->dx + delta_ry * v->dy);
    float b_delta_b = 2 * b * delta_b;
    float delta_b_delta_b = 2 * delta_b * delta_b;

    float bb = b * b;
    float delta_bb = delta_b * delta_b;

    b *= inv_a;
    delta_b *= inv_a;

    float rxrxryry = rx * rx + ry * ry;
    float delta_rxrxryry = delta_rx * delta_rx + delta_ry * delta_ry;
    float rx_plus_ry = 2 * (rx * delta_rx + ry * delta_ry);
    float delta_rx_plus_ry = 2 * delta_rxrxryry;

    inv_a *= inv_a;

    float det = (bb - 4 * v->a * (v->sqrfr - rxrxryry)) * inv_a;
    float delta_det = (b_delta_b + delta_bb + 4 * v->a * (rx_plus_ry + delta_rxrxryry)) * inv_a;
    float delta_delta_det = (delta_b_delta_b + 4 * v->a * delta_rx_plus_ry) * inv_a;

    const uint32_t* end = buffer + length;
    if(v->extended) {
        while(buffer < end) {
            uint32_t result = 0;
            if(det >= 0) {
                float w = sqrtf(det) - b;
                if(gradient->values.radial.fr + v->dr * w >= 0) {
                    result = gradient_pixel(gradient, w);
                }
            }

            *buffer = result;
            det += delta_det;
            delta_det += delta_delta_det;
            b += delta_b;
            ++buffer;
        }
    } else {
        while(buffer < end) {
            *buffer++ = gradient_pixel(gradient, sqrtf(det) - b);
            det += delta_det;
            delta_det += delta_delta_det;
            b += delta_b;
        }
    }
}

static void composition_solid_clear(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        plutovg_memfill32(dest, length, 0);
    } else {
        uint32_t ialpha = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(dest[i], ialpha);
        }
    }
}

static void composition_solid_source(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        plutovg_memfill32(dest, length, color);
    } else {
        uint32_t ialpha = 255 - const_alpha;
        color = BYTE_MUL(color, const_alpha);
        for(int i = 0; i < length; i++) {
            dest[i] = color + BYTE_MUL(dest[i], ialpha);
        }
    }
}

static void composition_solid_destination(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
}

static void composition_solid_source_over(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha != 255)
        color = BYTE_MUL(color, const_alpha);
    uint32_t ialpha = 255 - plutovg_alpha(color);
    for(int i = 0; i < length; i++) {
        dest[i] = color + BYTE_MUL(dest[i], ialpha);
    }
}

static void composition_solid_destination_over(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha != 255)
        color = BYTE_MUL(color, const_alpha);
    for(int i = 0; i < length; i++) {
        uint32_t d = dest[i];
        dest[i] = d + BYTE_MUL(color, plutovg_alpha(~d));
    }
}

static void composition_solid_source_in(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(color, plutovg_alpha(dest[i]));
        }
    } else {
        color = BYTE_MUL(color, const_alpha);
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(color, plutovg_alpha(d), d, cia);
        }
    }
}

static void composition_solid_destination_in(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    uint32_t a = plutovg_alpha(color);
    if(const_alpha != 255)
        a = BYTE_MUL(a, const_alpha) + 255 - const_alpha;
    for(int i = 0; i < length; i++) {
        dest[i] = BYTE_MUL(dest[i], a);
    }
}

static void composition_solid_source_out(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(color, plutovg_alpha(~dest[i]));
        }
    } else {
        color = BYTE_MUL(color, const_alpha);
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(color, plutovg_alpha(~d), d, cia);
        }
    }
}

static void composition_solid_destination_out(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    uint32_t a = plutovg_alpha(~color);
    if(const_alpha != 255)
        a = BYTE_MUL(a, const_alpha) + 255 - const_alpha;
    for(int i = 0; i < length; i++) {
        dest[i] = BYTE_MUL(dest[i], a);
    }
}

static void composition_solid_source_atop(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha != 255)
        color = BYTE_MUL(color, const_alpha);
    uint32_t sia = plutovg_alpha(~color);
    for(int i = 0; i < length; i++) {
        uint32_t d = dest[i];
        dest[i] = INTERPOLATE_PIXEL_255(color, plutovg_alpha(d), d, sia);
    }
}

static void composition_solid_destination_atop(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    uint32_t a = plutovg_alpha(color);
    if(const_alpha != 255) {
        color = BYTE_MUL(color, const_alpha);
        a = plutovg_alpha(color) + 255 - const_alpha;
    }

    for(int i = 0; i < length; i++) {
        uint32_t d = dest[i];
        dest[i] = INTERPOLATE_PIXEL_255(d, a, color, plutovg_alpha(~d));
    }
}

static void composition_solid_xor(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha)
{
    if(const_alpha != 255)
        color = BYTE_MUL(color, const_alpha);
    uint32_t sia = plutovg_alpha(~color);
    for(int i = 0; i < length; i++) {
        uint32_t d = dest[i];
        dest[i] = INTERPOLATE_PIXEL_255(color, plutovg_alpha(~d), d, sia);
    }
}

typedef void(*composition_solid_function_t)(uint32_t* dest, int length, uint32_t color, uint32_t const_alpha);

static const composition_solid_function_t composition_solid_table[] = {
    composition_solid_clear,
    composition_solid_source,
    composition_solid_destination,
    composition_solid_source_over,
    composition_solid_destination_over,
    composition_solid_source_in,
    composition_solid_destination_in,
    composition_solid_source_out,
    composition_solid_destination_out,
    composition_solid_source_atop,
    composition_solid_destination_atop,
    composition_solid_xor
};

static void composition_clear(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        plutovg_memfill32(dest, length, 0);
    } else {
        uint32_t ialpha = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(dest[i], ialpha);
        }
    }
}

static void composition_source(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        memcpy(dest, src, length * sizeof(uint32_t));
    } else {
        uint32_t ialpha = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            dest[i] = INTERPOLATE_PIXEL_255(src[i], const_alpha, dest[i], ialpha);
        }
    }
}

static void composition_destination(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
}

static void composition_source_over(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            uint32_t s = src[i];
            if(s >= 0xff000000) {
                dest[i] = s;
            } else if(s != 0) {
                dest[i] = s + BYTE_MUL(dest[i], plutovg_alpha(~s));
            }
        }
    } else {
        for(int i = 0; i < length; i++) {
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            dest[i] = s + BYTE_MUL(dest[i], plutovg_alpha(~s));
        }
    }
}

static void composition_destination_over(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            dest[i] = d + BYTE_MUL(src[i], plutovg_alpha(~d));
        }
    } else {
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            dest[i] = d + BYTE_MUL(s, plutovg_alpha(~d));
        }
    }
}

static void composition_source_in(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(src[i], plutovg_alpha(dest[i]));
        }
    } else {
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(d), d, cia);
        }
    }
}

static void composition_destination_in(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(dest[i], plutovg_alpha(src[i]));
        }
    } else {
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t a = BYTE_MUL(plutovg_alpha(src[i]), const_alpha) + cia;
            dest[i] = BYTE_MUL(dest[i], a);
        }
    }
}

static void composition_source_out(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(src[i], plutovg_alpha(~dest[i]));
        }
    } else {
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(~d), d, cia);
        }
    }
}

static void composition_destination_out(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            dest[i] = BYTE_MUL(dest[i], plutovg_alpha(~src[i]));
        }
    } else {
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t sia = BYTE_MUL(plutovg_alpha(~src[i]), const_alpha) + cia;
            dest[i] = BYTE_MUL(dest[i], sia);
        }
    }
}

static void composition_source_atop(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            uint32_t s = src[i];
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(d), d, plutovg_alpha(~s));
        }
    } else {
        for(int i = 0; i < length; i++) {
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(d), d, plutovg_alpha(~s));
        }
    }
}

static void composition_destination_atop(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            uint32_t s = src[i];
            uint32_t d = dest[i];
            dest[i] = INTERPOLATE_PIXEL_255(d, plutovg_alpha(s), s, plutovg_alpha(~d));
        }
    } else {
        uint32_t cia = 255 - const_alpha;
        for(int i = 0; i < length; i++) {
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            uint32_t d = dest[i];
            uint32_t a = plutovg_alpha(s) + cia;
            dest[i] = INTERPOLATE_PIXEL_255(d, a, s, plutovg_alpha(~d));
        }
    }
}

static void composition_xor(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha)
{
    if(const_alpha == 255) {
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            uint32_t s = src[i];
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(~d), d, plutovg_alpha(~s));
        }
    } else {
        for(int i = 0; i < length; i++) {
            uint32_t d = dest[i];
            uint32_t s = BYTE_MUL(src[i], const_alpha);
            dest[i] = INTERPOLATE_PIXEL_255(s, plutovg_alpha(~d), d, plutovg_alpha(~s));
        }
    }
}

typedef void(*composition_function_t)(uint32_t* dest, int length, const uint32_t* src, uint32_t const_alpha);

static const composition_function_t composition_table[] = {
    composition_clear,
    composition_source,
    composition_destination,
    composition_source_over,
    composition_destination_over,
    composition_source_in,
    composition_destination_in,
    composition_source_out,
    composition_destination_out,
    composition_source_atop,
    composition_destination_atop,
    composition_xor
};

static void blend_solid(plutovg_surface_t* surface, plutovg_operator_t op, uint32_t solid, const plutovg_span_buffer_t* span_buffer)
{
    composition_solid_function_t func = composition_solid_table[op];
    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + spans->x;
        func(target, spans->len, solid, spans->coverage);
        ++spans;
    }
}

#define BUFFER_SIZE 1024
static void blend_linear_gradient(plutovg_surface_t* surface, plutovg_operator_t op, const gradient_data_t* gradient, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    unsigned int buffer[BUFFER_SIZE];

    linear_gradient_values_t v;
    v.dx = gradient->values.linear.x2 - gradient->values.linear.x1;
    v.dy = gradient->values.linear.y2 - gradient->values.linear.y1;
    v.l = v.dx * v.dx + v.dy * v.dy;
    v.off = 0.f;
    if(v.l != 0.f) {
        v.dx /= v.l;
        v.dy /= v.l;
        v.off = -v.dx * gradient->values.linear.x1 - v.dy * gradient->values.linear.y1;
    }

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        int length = spans->len;
        int x = spans->x;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            fetch_linear_gradient(buffer, &v, gradient, spans->y, x, l);
            uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + x;
            func(target, l, buffer, spans->coverage);
            x += l;
            length -= l;
        }

        ++spans;
    }
}

static void blend_radial_gradient(plutovg_surface_t* surface, plutovg_operator_t op, const gradient_data_t* gradient, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    unsigned int buffer[BUFFER_SIZE];

    radial_gradient_values_t v;
    v.dx = gradient->values.radial.cx - gradient->values.radial.fx;
    v.dy = gradient->values.radial.cy - gradient->values.radial.fy;
    v.dr = gradient->values.radial.cr - gradient->values.radial.fr;
    v.sqrfr = gradient->values.radial.fr * gradient->values.radial.fr;
    v.a = v.dr * v.dr - v.dx * v.dx - v.dy * v.dy;
    v.extended = gradient->values.radial.fr != 0.f || v.a <= 0.f;

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        int length = spans->len;
        int x = spans->x;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            fetch_radial_gradient(buffer, &v, gradient, spans->y, x, l);
            uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + x;
            func(target, l, buffer, spans->coverage);
            x += l;
            length -= l;
        }

        ++spans;
    }
}

static void blend_untransformed_argb(plutovg_surface_t* surface, plutovg_operator_t op, const texture_data_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];

    const int image_width = texture->width;
    const int image_height = texture->height;

    int xoff = (int)(texture->matrix.e);
    int yoff = (int)(texture->matrix.f);

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        int x = spans->x;
        int length = spans->len;
        int sx = xoff + x;
        int sy = yoff + spans->y;
        if(sy >= 0 && sy < image_height && sx < image_width) {
            if(sx < 0) {
                x -= sx;
                length += sx;
                sx = 0;
            }

            if(sx + length > image_width)
                length = image_width - sx;
            if(length > 0) {
                const int coverage = (spans->coverage * texture->const_alpha) >> 8;
                const uint32_t* src = (const uint32_t*)(texture->data + sy * texture->stride) + sx;
                uint32_t* dest = (uint32_t*)(surface->data + spans->y * surface->stride) + x;
                func(dest, length, src, coverage);
            }
        }

        ++spans;
    }
}

#define FIXED_SCALE (1 << 16)
static void blend_transformed_argb(plutovg_surface_t* surface, plutovg_operator_t op, const texture_data_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    uint32_t buffer[BUFFER_SIZE];

    int image_width = texture->width;
    int image_height = texture->height;

    int fdx = (int)(texture->matrix.a * FIXED_SCALE);
    int fdy = (int)(texture->matrix.b * FIXED_SCALE);

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + spans->x;

        const float cx = spans->x + 0.5f;
        const float cy = spans->y + 0.5f;

        int x = (int)((texture->matrix.c * cy + texture->matrix.a * cx + texture->matrix.e) * FIXED_SCALE);
        int y = (int)((texture->matrix.d * cy + texture->matrix.b * cx + texture->matrix.f) * FIXED_SCALE);

        int length = spans->len;
        const int coverage = (spans->coverage * texture->const_alpha) >> 8;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            const uint32_t* end = buffer + l;
            uint32_t* b = buffer;
            while(b < end) {
                int px = x >> 16;
                int py = y >> 16;
                if((px < 0) || (px >= image_width) || (py < 0) || (py >= image_height)) {
                    *b = 0x00000000;
                } else {
                    *b = ((const uint32_t*)(texture->data + py * texture->stride))[px];
                }

                x += fdx;
                y += fdy;
                ++b;
            }

            func(target, l, buffer, coverage);
            target += l;
            length -= l;
        }

        ++spans;
    }
}

static void blend_untransformed_tiled_argb(plutovg_surface_t* surface, plutovg_operator_t op, const texture_data_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];

    int image_width = texture->width;
    int image_height = texture->height;

    int xoff = (int)(texture->matrix.e) % image_width;
    int yoff = (int)(texture->matrix.f) % image_height;

    if(xoff < 0)
        xoff += image_width;
    if(yoff < 0) {
        yoff += image_height;
    }

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        int x = spans->x;
        int length = spans->len;
        int sx = (xoff + spans->x) % image_width;
        int sy = (spans->y + yoff) % image_height;
        if(sx < 0)
            sx += image_width;
        if(sy < 0) {
            sy += image_height;
        }

        const int coverage = (spans->coverage * texture->const_alpha) >> 8;
        while(length) {
            int l = plutovg_min(image_width - sx, length);
            if(BUFFER_SIZE < l)
                l = BUFFER_SIZE;
            const uint32_t* src = (const uint32_t*)(texture->data + sy * texture->stride) + sx;
            uint32_t* dest = (uint32_t*)(surface->data + spans->y * surface->stride) + x;
            func(dest, l, src, coverage);
            x += l;
            sx += l;
            length -= l;
            if(sx >= image_width) {
                sx = 0;
            }
        }

        ++spans;
    }
}

static void blend_transformed_tiled_argb(plutovg_surface_t* surface, plutovg_operator_t op, const texture_data_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    uint32_t buffer[BUFFER_SIZE];

    int image_width = texture->width;
    int image_height = texture->height;
    const int scanline_offset = texture->stride / 4;

    int fdx = (int)(texture->matrix.a * FIXED_SCALE);
    int fdy = (int)(texture->matrix.b * FIXED_SCALE);

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + spans->x;
        const uint32_t* image_bits = (const uint32_t*)texture->data;

        const float cx = spans->x + 0.5f;
        const float cy = spans->y + 0.5f;

        int x = (int)((texture->matrix.c * cy + texture->matrix.a * cx + texture->matrix.e) * FIXED_SCALE);
        int y = (int)((texture->matrix.d * cy + texture->matrix.b * cx + texture->matrix.f) * FIXED_SCALE);

        const int coverage = (spans->coverage * texture->const_alpha) >> 8;
        int length = spans->len;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            const uint32_t* end = buffer + l;
            uint32_t* b = buffer;
            while(b < end) {
                int px = x >> 16;
                int py = y >> 16;
                px %= image_width;
                py %= image_height;
                if(px < 0) px += image_width;
                if(py < 0) py += image_height;
                int y_offset = py * scanline_offset;

                assert(px >= 0 && px < image_width);
                assert(py >= 0 && py < image_height);

                *b = image_bits[y_offset + px];
                x += fdx;
                y += fdy;
                ++b;
            }

            func(target, l, buffer, coverage);
            target += l;
            length -= l;
        }

        ++spans;
    }
}

static inline uint32_t interpolate_4_pixels(uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br, uint32_t distx, uint32_t disty)
{
    uint32_t idistx = 256 - distx;
    uint32_t idisty = 256 - disty;
    uint32_t xtop = INTERPOLATE_PIXEL_256(tl, idistx, tr, distx);
    uint32_t xbot = INTERPOLATE_PIXEL_256(bl, idistx, br, distx);
    return INTERPOLATE_PIXEL_256(xtop, idisty, xbot, disty);
}

#define HALF_POINT (1 << 15)
static void blend_transformed_bilinear_tiled_argb(plutovg_surface_t* surface, plutovg_operator_t op, const texture_data_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    uint32_t buffer[BUFFER_SIZE];

    int image_width = texture->width;
    int image_height = texture->height;

    int fdx = (int)(texture->matrix.a * FIXED_SCALE);
    int fdy = (int)(texture->matrix.b * FIXED_SCALE);

    int count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(count--) {
        uint32_t* target = (uint32_t*)(surface->data + spans->y * surface->stride) + spans->x;

        const float cx = spans->x + 0.5f;
        const float cy = spans->y + 0.5f;

        int fx = (int)((texture->matrix.c * cy + texture->matrix.a * cx + texture->matrix.e) * FIXED_SCALE);
        int fy = (int)((texture->matrix.d * cy + texture->matrix.b * cx + texture->matrix.f) * FIXED_SCALE);

        fx -= HALF_POINT;
        fy -= HALF_POINT;

        const int coverage = (spans->coverage * texture->const_alpha) >> 8;
        int length = spans->len;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            const uint32_t* end = buffer + l;
            uint32_t* b = buffer;
            while (b < end) {
                int x1 = (fx >> 16) % image_width;
                int y1 = (fy >> 16) % image_height;

                if(x1 < 0) x1 += image_width;
                if(y1 < 0) y1 += image_height;

                int x2 = (x1 + 1) % image_width;
                int y2 = (y1 + 1) % image_height;

                const uint32_t* s1 = (const uint32_t*)(texture->data + y1 * texture->stride);
                const uint32_t* s2 = (const uint32_t*)(texture->data + y2 * texture->stride);

                uint32_t tl = s1[x1];
                uint32_t tr = s1[x2];
                uint32_t bl = s2[x1];
                uint32_t br = s2[x2];

                int distx = (fx & 0x0000ffff) >> 8;
                int disty = (fy & 0x0000ffff) >> 8;
                *b = interpolate_4_pixels(tl, tr, bl, br, distx, disty);

                fx += fdx;
                fy += fdy;
                ++b;
            }

            func(target, l, buffer, coverage);
            target += l;
            length -= l;
        }

        ++spans;
    }
}

/*
 * Mesh paint rendering.
 *
 * A tensor-product cubic Bézier patch is defined over the parameter square
 * [0, 1] x [0, 1]. For a device position p we need the parameter (u, v) with
 * S(u, v) = p. The patch is assumed injective with a non-zero Jacobian, so
 * Newton's method converges to the unique solution from a starting point near
 * the correct parameter. Once the parameter is known the colors are
 * interpolated bilinearly from the four premultiplied corner colors.
 *
 * Coverage is decided per device pixel by sampling a regular 8x8 grid of
 * subpixel centers. The colors found at all covered subpixels are averaged.
 * When a sample position is covered by more than one patch, the last patch in
 * the input array wins.
 */

#define MESH_SUBPIXELS 8
#define MESH_SUBPIXEL_COUNT (MESH_SUBPIXELS * MESH_SUBPIXELS)
#define MESH_SUBPIXEL_SCALE (1.0 / MESH_SUBPIXELS)
#define MESH_SEED_GRID 9
#define MESH_SEED_NODES (MESH_SEED_GRID * MESH_SEED_GRID)
#define MESH_MAX_NEWTON_ITERATIONS 64
#define MESH_RESIDUAL_TOLERANCE 1e-9
#define MESH_PARAMETER_EPSILON 1e-9

typedef struct {
    /* Power-basis coefficients of the patch position. */
    double x[4][4];
    double y[4][4];
    /* Bilinear coefficients of each premultiplied color channel. */
    double color[4][4];
    double min_x, min_y, max_x, max_y;
    /*
     * True when the position map reduces to an affine map. Such patches are
     * inverted with a single 2x2 solve instead of Newton iteration.
     */
    bool affine;
    double origin_x, origin_y;   /* Value at (u, v) = (0, 0). */
    double linear_u_x, linear_u_y; /* Coefficient of u. */
    double linear_v_x, linear_v_y; /* Coefficient of v. */
    double inverse_det;          /* 1 / determinant of the linear part. */
    /*
     * Seeds for the parameter search. These are an implementation detail of
     * the inversion, not part of the interpolation.
     */
    float seed_x[MESH_SEED_NODES];
    float seed_y[MESH_SEED_NODES];
    float seed_u[MESH_SEED_NODES];
    float seed_v[MESH_SEED_NODES];
} mesh_patch_data_t;

/* Power-basis coefficients of the cubic Bernstein basis functions. */
static const double mesh_bernstein_to_power[4][4] = {
    { 1.0, -3.0,  3.0, -1.0 },
    { 0.0,  3.0, -6.0,  3.0 },
    { 0.0,  0.0,  3.0, -3.0 },
    { 0.0,  0.0,  0.0,  1.0 }
};

/*
 * Evaluates the patch and its two partial derivatives using a separable
 * Horner scheme. For each v coefficient the polynomial in u is evaluated
 * first, which keeps the inner loop short and numerically stable.
 */
static void mesh_eval(const mesh_patch_data_t* patch, double u, double v,
    double* x, double* y, double* dxdu, double* dydu, double* dxdv, double* dydv)
{
    double row_x[4], row_y[4];
    double drow_x[4], drow_y[4];
    for(int b = 0; b < 4; ++b) {
        double px = patch->x[3][b];
        double py = patch->y[3][b];
        double dpx = 0.0, dpy = 0.0;
        for(int a = 2; a >= 0; --a) {
            dpx = dpx * u + px;
            dpy = dpy * u + py;
            px = px * u + patch->x[a][b];
            py = py * u + patch->y[a][b];
        }

        row_x[b] = px;
        row_y[b] = py;
        drow_x[b] = dpx;
        drow_y[b] = dpy;
    }

    double sx = row_x[3], sy = row_y[3];
    double sux = drow_x[3], suy = drow_y[3];
    double svx = 0.0, svy = 0.0;
    for(int b = 2; b >= 0; --b) {
        svx = svx * v + sx;
        svy = svy * v + sy;
        sx = sx * v + row_x[b];
        sy = sy * v + row_y[b];
        sux = sux * v + drow_x[b];
        suy = suy * v + drow_y[b];
    }

    *x = sx; *y = sy;
    *dxdu = sux; *dydu = suy;
    *dxdv = svx; *dydv = svy;
}

static void mesh_eval_color(const mesh_patch_data_t* patch, double u, double v, double color[4])
{
    for(int channel = 0; channel < 4; ++channel) {
        const double* c = patch->color[channel];
        double value = c[0] + c[1] * u + c[2] * v + c[3] * u * v;
        color[channel] = plutovg_clamp(value, 0.0, 1.0);
    }
}

static void mesh_prepare_patch(mesh_patch_data_t* data, const plutovg_mesh_patch_t* patch, const plutovg_matrix_t* matrix)
{
    /* Transform the control points into device space. */
    double points[16][2];
    for(int k = 0; k < 16; ++k) {
        double x = patch->points[k].x;
        double y = patch->points[k].y;
        points[k][0] = (double)matrix->a * x + (double)matrix->c * y + matrix->e;
        points[k][1] = (double)matrix->b * x + (double)matrix->d * y + matrix->f;
    }

    for(int a = 0; a < 4; ++a) {
        for(int b = 0; b < 4; ++b) {
            double cx = 0.0, cy = 0.0;
            for(int i = 0; i < 4; ++i) {
                for(int j = 0; j < 4; ++j) {
                    double weight = mesh_bernstein_to_power[i][a] * mesh_bernstein_to_power[j][b];
                    const double* point = points[4 * j + i];
                    cx += weight * point[0];
                    cy += weight * point[1];
                }
            }

            data->x[a][b] = cx;
            data->y[a][b] = cy;
        }
    }

    double min_x = points[0][0], max_x = points[0][0];
    double min_y = points[0][1], max_y = points[0][1];
    for(int k = 1; k < 16; ++k) {
        min_x = plutovg_min(min_x, points[k][0]);
        max_x = plutovg_max(max_x, points[k][0]);
        min_y = plutovg_min(min_y, points[k][1]);
        max_y = plutovg_max(max_y, points[k][1]);
    }

    data->min_x = min_x;
    data->min_y = min_y;
    data->max_x = max_x;
    data->max_y = max_y;

    /*
     * Detect a patch whose position map is affine: all u^a v^b terms with
     * a > 1, b > 1, or both non-zero must vanish. Such patches can be
     * inverted directly, which is both faster and more accurate than
     * iterating. The tolerance is relative to the coordinate magnitude so
     * that float rounding in an otherwise flat control net still qualifies.
     */
    double scale = 1.0;
    for(int k = 0; k < 16; ++k) {
        scale = plutovg_max(scale, fabs(points[k][0]));
        scale = plutovg_max(scale, fabs(points[k][1]));
    }

    /*
     * The control points are stored as float, so an exactly affine control
     * net still produces tiny higher-order coefficients. Use a tolerance
     * just above float rounding relative to the coordinate magnitude.
     */
    const double affine_tolerance = 1e-6 * scale;
    bool affine = true;
    for(int a = 0; a < 4 && affine; ++a) {
        for(int b = 0; b < 4; ++b) {
            if(a == 0 && b == 0)
                continue;
            if(a > 1 || b > 1 || (a > 0 && b > 0)) {
                if(fabs(data->x[a][b]) > affine_tolerance || fabs(data->y[a][b]) > affine_tolerance) {
                    affine = false;
                    break;
                }
            }
        }
    }

    data->affine = affine;
    data->origin_x = data->x[0][0];
    data->origin_y = data->y[0][0];
    data->linear_u_x = data->x[1][0];
    data->linear_u_y = data->y[1][0];
    data->linear_v_x = data->x[0][1];
    data->linear_v_y = data->y[0][1];
    double det = data->linear_u_x * data->linear_v_y - data->linear_u_y * data->linear_v_x;
    data->inverse_det = det != 0.0 ? 1.0 / det : 0.0;

    /*
     * Colors are premultiplied before interpolation. The corners are ordered
     * as (0, 0), (1, 0), (1, 1), (0, 1); the result is stored as the bilinear
     * coefficients in u and v.
     */
    double premultiplied[4][4];
    for(int k = 0; k < 4; ++k) {
        const plutovg_color_t* color = patch->colors + k;
        premultiplied[k][0] = (double)color->r * color->a;
        premultiplied[k][1] = (double)color->g * color->a;
        premultiplied[k][2] = (double)color->b * color->a;
        premultiplied[k][3] = color->a;
    }

    for(int channel = 0; channel < 4; ++channel) {
        double c00 = premultiplied[0][channel];
        double c10 = premultiplied[1][channel];
        double c11 = premultiplied[2][channel];
        double c01 = premultiplied[3][channel];
        data->color[channel][0] = c00;
        data->color[channel][1] = c10 - c00;
        data->color[channel][2] = c01 - c00;
        data->color[channel][3] = c11 - c10 - c01 + c00;
    }

    /*
     * A regular grid of sampled device positions gives the starting point for
     * Newton's method. This makes the inversion robust for strongly bent
     * patches; it does not affect the interpolated color.
     */
    for(int j = 0; j < MESH_SEED_GRID; ++j) {
        for(int i = 0; i < MESH_SEED_GRID; ++i) {
            int index = j * MESH_SEED_GRID + i;
            double u = (double)i / (MESH_SEED_GRID - 1);
            double v = (double)j / (MESH_SEED_GRID - 1);
            double x, y, dxdu, dydu, dxdv, dydv;
            mesh_eval(data, u, v, &x, &y, &dxdu, &dydu, &dxdv, &dydv);
            data->seed_x[index] = (float)x;
            data->seed_y[index] = (float)y;
            data->seed_u[index] = (float)u;
            data->seed_v[index] = (float)v;
        }
    }
}

static void mesh_find_seed(const mesh_patch_data_t* patch, double x, double y, double* u, double* v)
{
    int index = 0;
    double best = DBL_MAX;
    for(int i = 0; i < MESH_SEED_NODES; ++i) {
        double dx = patch->seed_x[i] - x;
        double dy = patch->seed_y[i] - y;
        double distance = dx * dx + dy * dy;
        if(distance < best) {
            best = distance;
            index = i;
        }
    }

    *u = patch->seed_u[index];
    *v = patch->seed_v[index];
}

/*
 * Inverts the patch at the device position (x, y). Returns true when the
 * position lies inside the parameter square. On return (u, v) holds the
 * approximate parameter, which is also useful as the seed for a nearby query.
 */
static bool mesh_invert(const mesh_patch_data_t* patch, double x, double y, double* u, double* v)
{
    if(patch->affine) {
        if(patch->inverse_det == 0.0)
            return false;
        double rx = x - patch->origin_x;
        double ry = y - patch->origin_y;
        double su = (rx * patch->linear_v_y - ry * patch->linear_v_x) * patch->inverse_det;
        double sv = (patch->linear_u_x * ry - patch->linear_u_y * rx) * patch->inverse_det;
        *u = su;
        *v = sv;
    } else {
        bool converged = false;
        for(int iteration = 0; iteration < MESH_MAX_NEWTON_ITERATIONS; ++iteration) {
            double px, py, dxdu, dydu, dxdv, dydv;
            mesh_eval(patch, *u, *v, &px, &py, &dxdu, &dydu, &dxdv, &dydv);
            double rx = px - x;
            double ry = py - y;
            double det = dxdu * dydv - dydu * dxdv;
            if(!isfinite(det) || fabs(det) < 1e-18)
                return false;

            double du = (rx * dydv - ry * dxdv) / det;
            double dv = (dxdu * ry - dydu * rx) / det;
            *u -= du;
            *v -= dv;
            if(!isfinite(*u) || !isfinite(*v) || *u < -4.0 || *u > 5.0 || *v < -4.0 || *v > 5.0)
                return false;

            if(fabs(rx) <= MESH_RESIDUAL_TOLERANCE && fabs(ry) <= MESH_RESIDUAL_TOLERANCE
                && fabs(du) <= 1e-9 && fabs(dv) <= 1e-9) {
                converged = true;
                break;
            }
        }

        if(!converged) {
            double px, py, dxdu, dydu, dxdv, dydv;
            mesh_eval(patch, *u, *v, &px, &py, &dxdu, &dydu, &dxdv, &dydv);
            if(fabs(px - x) > 1e-6 || fabs(py - y) > 1e-6)
                return false;
        }
    }

    return *u >= -MESH_PARAMETER_EPSILON && *u <= 1.0 + MESH_PARAMETER_EPSILON
        && *v >= -MESH_PARAMETER_EPSILON && *v <= 1.0 + MESH_PARAMETER_EPSILON;
}

static uint32_t mesh_sample_pixel(const mesh_patch_data_t* patches, int count, int px, int py, float opacity, double* seed_u, double* seed_v)
{
    /*
     * Each subpixel is painted by at most one patch: patches are visited from
     * last to first and already covered subpixels are skipped, so the last
     * patch in the array wins where several patches overlap. `covered` is a
     * bitmask over the 8x8 subpixel grid.
     */
    uint64_t covered = 0;
    double red = 0.0, green = 0.0, blue = 0.0, alpha = 0.0;

    const double pixel_x = px;
    const double pixel_y = py;
    for(int index = count - 1; index >= 0 && covered != ~(uint64_t)0; --index) {
        const mesh_patch_data_t* patch = patches + index;
        if(pixel_x + 1.0 < patch->min_x || pixel_x > patch->max_x
            || pixel_y + 1.0 < patch->min_y || pixel_y > patch->max_y) {
            continue;
        }

        double u = 0.5, v = 0.5;
        if(!patch->affine) {
            /* Reuse the parameter found for the previous pixel when possible. */
            if(seed_u[index] >= 0.0) {
                u = seed_u[index];
                v = seed_v[index];
            } else {
                mesh_find_seed(patch, pixel_x + 0.5, pixel_y + 0.5, &u, &v);
            }
        }

        for(int j = 0; j < MESH_SUBPIXELS; ++j) {
            for(int i = 0; i < MESH_SUBPIXELS; ++i) {
                int sample = j * MESH_SUBPIXELS + i;
                uint64_t bit = (uint64_t)1 << sample;
                if(covered & bit)
                    continue;

                double x = pixel_x + ((double)i + 0.5) * MESH_SUBPIXEL_SCALE;
                double y = pixel_y + ((double)j + 0.5) * MESH_SUBPIXEL_SCALE;
                double su = u, sv = v;
                bool found = mesh_invert(patch, x, y, &su, &sv);
                if(!found && !patch->affine) {
                    /* Retry from the nearest pre-sampled parameter. */
                    double tu, tv;
                    mesh_find_seed(patch, x, y, &tu, &tv);
                    su = tu; sv = tv;
                    found = mesh_invert(patch, x, y, &su, &sv);
                }

                if(found) {
                    double color[4];
                    mesh_eval_color(patch, plutovg_clamp(su, 0.0, 1.0), plutovg_clamp(sv, 0.0, 1.0), color);
                    red += color[0];
                    green += color[1];
                    blue += color[2];
                    alpha += color[3];
                    covered |= bit;
                    /* A successful inverse is a good seed for the neighbour. */
                    u = su;
                    v = sv;
                    seed_u[index] = su;
                    seed_v[index] = sv;
                }
            }
        }
    }

    red = red * opacity / MESH_SUBPIXEL_COUNT;
    green = green * opacity / MESH_SUBPIXEL_COUNT;
    blue = blue * opacity / MESH_SUBPIXEL_COUNT;
    alpha = alpha * opacity / MESH_SUBPIXEL_COUNT;

    uint32_t a = (uint32_t)lround(plutovg_clamp(alpha, 0.0, 1.0) * 255.0);
    uint32_t r = (uint32_t)lround(plutovg_clamp(red, 0.0, 1.0) * 255.0);
    uint32_t g = (uint32_t)lround(plutovg_clamp(green, 0.0, 1.0) * 255.0);
    uint32_t b = (uint32_t)lround(plutovg_clamp(blue, 0.0, 1.0) * 255.0);
    if(r > a) r = a;
    if(g > a) g = a;
    if(b > a) b = a;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void blend_mesh(plutovg_surface_t* surface, plutovg_operator_t op, const plutovg_mesh_paint_t* mesh, const plutovg_matrix_t* matrix, float opacity, const plutovg_span_buffer_t* span_buffer)
{
    composition_function_t func = composition_table[op];
    uint32_t buffer[BUFFER_SIZE];
    int count = mesh->count;
    mesh_patch_data_t* patches = malloc((size_t)count * sizeof(mesh_patch_data_t));
    double* seed_u = malloc((size_t)count * sizeof(double));
    double* seed_v = malloc((size_t)count * sizeof(double));
    if(patches == NULL || seed_u == NULL || seed_v == NULL) {
        free(patches);
        free(seed_u);
        free(seed_v);
        return;
    }

    for(int i = 0; i < count; ++i) {
        mesh_prepare_patch(patches + i, mesh->patches + i, matrix);
        seed_u[i] = -1.0;
        seed_v[i] = -1.0;
    }

    int spans_count = span_buffer->spans.size;
    const plutovg_span_t* spans = span_buffer->spans.data;
    while(spans_count--) {
        int x = spans->x;
        int length = spans->len;
        const int y = spans->y;
        while(length) {
            int l = plutovg_min(length, BUFFER_SIZE);
            for(int i = 0; i < l; ++i) {
                buffer[i] = mesh_sample_pixel(patches, count, x + i, y, opacity, seed_u, seed_v);
            }

            uint32_t* target = (uint32_t*)(surface->data + y * surface->stride) + x;
            func(target, l, buffer, spans->coverage);
            x += l;
            length -= l;
        }

        ++spans;
    }

    free(seed_v);
    free(seed_u);
    free(patches);
}

static void plutovg_blend_mesh(plutovg_canvas_t* canvas, const plutovg_mesh_paint_t* mesh, const plutovg_span_buffer_t* span_buffer)
{
    if(mesh->count <= 0)
        return;
    plutovg_state_t* state = canvas->state;
    /*
     * Control points are mapped by the paint matrix first and then by the
     * canvas matrix. plutovg_matrix_multiply(dst, left, right) produces
     * right(left(p)), so the paint matrix is passed first.
     */
    plutovg_matrix_t matrix;
    plutovg_matrix_multiply(&matrix, &mesh->matrix, &state->matrix);
    double det = (double)matrix.a * matrix.d - (double)matrix.b * matrix.c;
    if(!isfinite(det) || det == 0.0)
        return;
    blend_mesh(canvas->surface, state->op, mesh, &matrix, state->opacity, span_buffer);
}
static void plutovg_blend_color(plutovg_canvas_t* canvas, const plutovg_color_t* color, const plutovg_span_buffer_t* span_buffer)
{
    plutovg_state_t* state = canvas->state;
    uint32_t solid = premultiply_color_with_opacity(color, state->opacity);
    uint32_t alpha = plutovg_alpha(solid);

    if(alpha == 255 && state->op == PLUTOVG_OPERATOR_SRC_OVER) {
        blend_solid(canvas->surface, PLUTOVG_OPERATOR_SRC, solid, span_buffer);
    } else {
        blend_solid(canvas->surface, state->op, solid, span_buffer);
    }
}

static void plutovg_blend_gradient(plutovg_canvas_t* canvas, const plutovg_gradient_paint_t* gradient, const plutovg_span_buffer_t* span_buffer)
{
    if(gradient->nstops == 0)
        return;
    plutovg_state_t* state = canvas->state;
    gradient_data_t data;
    data.spread = gradient->spread;
    data.matrix = gradient->matrix;
    plutovg_matrix_multiply(&data.matrix, &data.matrix, &state->matrix);
    if(!plutovg_matrix_invert(&data.matrix, &data.matrix))
        return;
    int i, pos = 0, nstops = gradient->nstops;
    const plutovg_gradient_stop_t *curr, *next, *start, *last;
    uint32_t curr_color, next_color, last_color;
    uint32_t dist, idist;
    float delta, t, incr, fpos;
    float opacity = state->opacity;

    start = gradient->stops;
    curr = start;
    curr_color = premultiply_color_with_opacity(&curr->color, opacity);

    data.colortable[pos++] = curr_color;
    incr = 1.0f / COLOR_TABLE_SIZE;
    fpos = 1.5f * incr;

    while(fpos <= curr->offset) {
        data.colortable[pos] = data.colortable[pos - 1];
        ++pos;
        fpos += incr;
    }

    for(i = 0; i < nstops - 1; i++) {
        curr = (start + i);
        next = (start + i + 1);
        if(curr->offset == next->offset)
            continue;
        delta = 1.f / (next->offset - curr->offset);
        next_color = premultiply_color_with_opacity(&next->color, opacity);
        while(fpos < next->offset && pos < COLOR_TABLE_SIZE) {
            t = (fpos - curr->offset) * delta;
            dist = (uint32_t)(255 * t);
            idist = 255 - dist;
            data.colortable[pos] = INTERPOLATE_PIXEL_255(curr_color, idist, next_color, dist);
            ++pos;
            fpos += incr;
        }

        curr_color = next_color;
    }

    last = start + nstops - 1;
    last_color = premultiply_color_with_opacity(&last->color, opacity);
    for(; pos < COLOR_TABLE_SIZE; ++pos) {
        data.colortable[pos] = last_color;
    }

    if(gradient->type == PLUTOVG_GRADIENT_TYPE_LINEAR) {
        data.values.linear.x1 = gradient->values[0];
        data.values.linear.y1 = gradient->values[1];
        data.values.linear.x2 = gradient->values[2];
        data.values.linear.y2 = gradient->values[3];
        blend_linear_gradient(canvas->surface, state->op, &data, span_buffer);
    } else {
        data.values.radial.cx = gradient->values[0];
        data.values.radial.cy = gradient->values[1];
        data.values.radial.cr = gradient->values[2];
        data.values.radial.fx = gradient->values[3];
        data.values.radial.fy = gradient->values[4];
        data.values.radial.fr = gradient->values[5];
        blend_radial_gradient(canvas->surface, state->op, &data, span_buffer);
    }
}

static void plutovg_blend_texture(plutovg_canvas_t* canvas, const plutovg_texture_paint_t* texture, const plutovg_span_buffer_t* span_buffer)
{
    if(texture->surface == NULL)
        return;
    plutovg_state_t* state = canvas->state;
    texture_data_t data;
    data.matrix = texture->matrix;
    data.data = texture->surface->data;
    data.width = texture->surface->width;
    data.height = texture->surface->height;
    data.stride = texture->surface->stride;
    data.const_alpha = lroundf(state->opacity * texture->opacity * 256);

    plutovg_matrix_multiply(&data.matrix, &data.matrix, &state->matrix);
    if(!plutovg_matrix_invert(&data.matrix, &data.matrix))
        return;
    const plutovg_matrix_t* matrix = &data.matrix;
    if(matrix->a == 1 && matrix->b == 0 && matrix->c == 0 && matrix->d == 1) {
        if(texture->type == PLUTOVG_TEXTURE_TYPE_PLAIN) {
            blend_untransformed_argb(canvas->surface, state->op, &data, span_buffer);
        } else {
            blend_untransformed_tiled_argb(canvas->surface, state->op, &data, span_buffer);
        }
    } else {
        if(texture->type == PLUTOVG_TEXTURE_TYPE_PLAIN) {
            blend_transformed_argb(canvas->surface, state->op, &data, span_buffer);
        } else if(fabsf(matrix->b) > 1e-6f || fabsf(matrix->c) > 1e-6f) {
            blend_transformed_bilinear_tiled_argb(canvas->surface, state->op, &data, span_buffer);
        } else {
            blend_transformed_tiled_argb(canvas->surface, state->op, &data, span_buffer);
        }
    }
}

void plutovg_blend(plutovg_canvas_t* canvas, const plutovg_span_buffer_t* span_buffer)
{
    if(span_buffer->spans.size == 0)
        return;
    if(canvas->state->paint == NULL) {
        plutovg_blend_color(canvas, &canvas->state->color, span_buffer);
        return;
    }

    plutovg_paint_t* paint = canvas->state->paint;
    if(paint->type == PLUTOVG_PAINT_TYPE_COLOR) {
        plutovg_solid_paint_t* solid = (plutovg_solid_paint_t*)(paint);
        plutovg_blend_color(canvas, &solid->color, span_buffer);
    } else if(paint->type == PLUTOVG_PAINT_TYPE_GRADIENT) {
        plutovg_gradient_paint_t* gradient = (plutovg_gradient_paint_t*)(paint);
        plutovg_blend_gradient(canvas, gradient, span_buffer);
    } else if(paint->type == PLUTOVG_PAINT_TYPE_TEXTURE) {
        plutovg_texture_paint_t* texture = (plutovg_texture_paint_t*)(paint);
        plutovg_blend_texture(canvas, texture, span_buffer);
    } else {
        plutovg_mesh_paint_t* mesh = (plutovg_mesh_paint_t*)(paint);
        plutovg_blend_mesh(canvas, mesh, span_buffer);
    }
}
