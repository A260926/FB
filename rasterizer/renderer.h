#pragma once

#include "buffer.h"
#include "program.h"
#include "framebuffer.h"
#include "multisample_framebuffer.h"

#include "math/utility.h"
#include "math/rectangle.h"

#include "detail/test_member.h"
#include "detail/tuple_helper.h"

#include <cassert>
#include <stdexcept>

struct Renderer
{
    /* rasterizer options */
    struct Options
    {
        Rectf viewport;
        bool culling;
        bool wireframe;
    };


    Renderer(unsigned int width, unsigned int height);

    DefaultFramebuffer& framebuffer();
    Options& options();



    template<typename Vertex, typename Varying, typename Uniforms>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct Buffer<Vertex>& buffer)
    {
        draw(program, buffer, m_framebuffer, m_options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct BufferIndexed<Vertex, Indx>& buffer)
    {
        draw(program, buffer, m_framebuffer, m_options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw(const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, const struct Buffer<Vertex>& buffer, Framebuffer<Targets...>& fb)
    {
        draw(program, buffer, fb, m_options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx, typename... Targets>
    void draw(const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, const struct BufferIndexed<Vertex, Indx>& buffer, Framebuffer<Targets...>& fb)
    {
        draw(program, buffer, fb, m_options);
    }

    /* fixed 4-sample target: only the default Vec4 fragment output is supported */
    template<typename Vertex, typename Varying, typename Uniforms>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct Buffer<Vertex>& buffer, MultisampleFramebuffer& fb)
    {
        draw(program, buffer, fb, m_options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct BufferIndexed<Vertex, Indx>& buffer, MultisampleFramebuffer& fb)
    {
        draw(program, buffer, fb, m_options);
    }

    template<typename Vertex, typename Varying, typename Uniforms>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct Buffer<Vertex>& buffer, MultisampleFramebuffer& fb, const Options& options)
    {
        assert(program.m_vertShader);
        assert(program.m_fragShader);

        /* lines and wireframe have no multisample path; reject before any sample is touched */
        if(buffer.primitive == ePrimitive::LINES || options.wireframe)
            throw std::invalid_argument("MultisampleFramebuffer only supports filled triangles");

        std::vector<Varying> pipeline_data(buffer.vertices.size());
        process_vertices(buffer.vertices, pipeline_data, program, options);
        draw_triangles_ms(pipeline_data, program, fb, options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx>
    void draw(const Program<Vertex, Varying, Uniforms>& program, const struct BufferIndexed<Vertex, Indx>& buffer, MultisampleFramebuffer& fb, const Options& options)
    {
        assert(program.m_vertShader);
        assert(program.m_fragShader);

        /* lines and wireframe have no multisample path; reject before any sample is touched */
        if(buffer.primitive == ePrimitive::LINES || options.wireframe)
            throw std::invalid_argument("MultisampleFramebuffer only supports filled triangles");

        std::vector<Varying> pipeline_data(buffer.vertices.size());
        process_vertices(buffer.vertices, pipeline_data, program, options);
        draw_triangles_ms(pipeline_data, buffer.indices, program, fb, options);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw(const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, const struct Buffer<Vertex>& buffer, Framebuffer<Targets...>& fb, const Options& options)
    {
        assert(program.m_vertShader);
        assert(program.m_fragShader);

        std::vector<Varying> pipeline_data(buffer.vertices.size());
        process_vertices(buffer.vertices, pipeline_data, program, options);

        switch(buffer.primitive)
        {
        case ePrimitive::TRIANGLES: options.wireframe ? draw_triangles_wireframe(pipeline_data, program, fb, options)
                                                      : draw_triangles(pipeline_data, program, fb, options);
            break;
        case ePrimitive::LINES: draw_lines(pipeline_data, program, fb, options);
            break;
        default: break;
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx, typename... Targets>
    void draw(const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, const struct BufferIndexed<Vertex, Indx>& buffer, Framebuffer<Targets...>& fb, const Options& options)
    {
        assert(program.m_vertShader);
        assert(program.m_fragShader);

        std::vector<Varying> pipeline_data(buffer.vertices.size());
        process_vertices(buffer.vertices, pipeline_data, program, options);

        switch(buffer.primitive)
        {
        case ePrimitive::TRIANGLES: options.wireframe ? draw_triangles_wireframe(pipeline_data, buffer.indices, program, fb, options)
                                                      : draw_triangles(pipeline_data, buffer.indices, program, fb, options);
            break;
        case ePrimitive::LINES: draw_lines(pipeline_data, buffer.indices, program, fb, options);
            break;
        default: break;
        }
    }


private:
    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void process_vertices(const std::vector<Vertex>& vertices, std::vector<Varying>& out, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, const Options& options)
    {
        for(unsigned int i = 0; i < vertices.size(); i++)
        {
            program.m_vertShader(program.m_uniforms, vertices[i], out[i]);
            post_process_vertices(out[i], options);
        }
    }

    template<typename Varying>
    void post_process_vertices(Varying& out, const Options& options)
    {
        // perspective divide
        out.position.w = 1.0f / out.position.w;
        out.position.x *= out.position.w;
        out.position.y *= out.position.w;
        out.position.z *= out.position.w;

        // screen space (viewport mapping)
        out.position.x = options.viewport.min.x + (out.position.x + 1.0f) / 2.0f * (options.viewport.max.x - options.viewport.min.x);
        out.position.y = options.viewport.min.y + (out.position.y + 1.0f) / 2.0f * (options.viewport.max.y - options.viewport.min.y);
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw_triangles(const std::vector<Varying>& in, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < in.size() / 3; i++)
        {
            draw_triangle(in[i*3 + 0], in[i*3 + 1], in[i*3 + 2], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms,  typename... Targets>
    void draw_triangles_wireframe(const std::vector<Varying>& in, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < in.size() / 3; i++)
        {
            draw_line(in[i*3 + 0], in[i*3 + 1], program, fb, options);
            draw_line(in[i*3 + 1], in[i*3 + 2], program, fb, options);
            draw_line(in[i*3 + 2], in[i*3 + 0], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx,  typename... Targets>
    void draw_triangles(const std::vector<Varying>& in, const std::vector<Indx>& indices, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < indices.size() / 3; i++)
        {
            draw_triangle(in[ indices[i*3 + 0] ], in[ indices[i*3 + 1] ], in[ indices[i*3 + 2] ], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx,  typename... Targets>
    void draw_triangles_wireframe(const std::vector<Varying>& in, const std::vector<Indx>& indices, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < indices.size() / 3; i++)
        {
            draw_line(in[ indices[i*3 + 0] ], in[ indices[i*3 + 1] ], program, fb, options);
            draw_line(in[ indices[i*3 + 1] ], in[ indices[i*3 + 2] ], program, fb, options);
            draw_line(in[ indices[i*3 + 2] ], in[ indices[i*3 + 0] ], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw_triangle(const Varying& v_0, const Varying& v_1, const Varying& v_2, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        if(options.culling)
        {
            auto normal = cross(Vec3(v_1.position) - Vec3(v_0.position), Vec3(v_2.position) - Vec3(v_0.position));
            if(normal.z < 0.0f) return;
        }

        Recti bbox(v_0.position, v_1.position, v_2.position);
        bbox.clamp(options.viewport, 0, -1);

        for(int x = bbox.min.x; x <= bbox.max.x; x++)
        {
            for(int y = bbox.min.y; y <= bbox.max.y; y++)
            {
                Vec2 fragCoord = Vec2(x + 0.5f, y + 0.5f);
                auto bc = barycentric(Vec2(v_0.position), Vec2(v_1.position), Vec2(v_2.position), fragCoord);
                if(bc.x < 0 || bc.y < 0 || bc.z < 0 || std::isnan(bc.x)) continue;

                /* linear interpolate in screen space */
                float w = bc.x * v_0.position.w + bc.y * v_1.position.w + bc.z * v_2.position.w;
                float z = bc.x * v_0.position.z + bc.y * v_1.position.z + bc.z * v_2.position.z;
                z = z * 0.5f + 0.5f;

                /* TODO: clipping should happen earlier */
                if(0.0f > z || z > 1.0f) continue;

                /* early depth test */
                if constexpr (Framebuffer<Targets...>::has_depth)
                {
                    auto& depth = fb.depth()(x, y);
                    if(z > depth) continue;
                    depth = z;
                }

                /* perspective correction of barycentric coordinates */
                bc.x = 1.0f / w * bc.x * v_0.position.w;
                bc.y = 1.0f / w * bc.y * v_1.position.w;
                bc.z = 1.0f / w * bc.z * v_2.position.w;

                /* interpolate fragment data */
                Varying inter;
                interpolate_frag_data(bc, v_0, v_1, v_2, inter);

                /* call fragment shader, TODO: unecessary complicated to have two different function definitions? */
                if constexpr (std::is_same_v<Framebuffer<Targets...>, DefaultFramebuffer>)
                {
                    Vec4 fragColor(0, 0, 0, 0);
                    program.m_fragShader(program.m_uniforms, inter, fragColor);

                    fb.color()(x, y) = RGBA8( max( min(fragColor, 1.0), 0.0) * 255 );
                }
                else
                {
                    auto targets = fb.targets(x, y);
                    program.m_fragShader(program.m_uniforms, inter, targets);
                }
            }
        }
    }

    /* ============================================================
       fixed 4-sample rasterization path (MultisampleFramebuffer)
       ============================================================ */

    template<typename Vertex, typename Varying, typename Uniforms>
    void draw_triangles_ms(const std::vector<Varying>& in, const Program<Vertex, Varying, Uniforms>& program, MultisampleFramebuffer& fb, const Options& options)
    {
        for(unsigned int i = 0; i < in.size() / 3; i++)
        {
            draw_triangle_ms(in[i*3 + 0], in[i*3 + 1], in[i*3 + 2], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx>
    void draw_triangles_ms(const std::vector<Varying>& in, const std::vector<Indx>& indices, const Program<Vertex, Varying, Uniforms>& program, MultisampleFramebuffer& fb, const Options& options)
    {
        for(unsigned int i = 0; i < indices.size() / 3; i++)
        {
            draw_triangle_ms(in[ indices[i*3 + 0] ], in[ indices[i*3 + 1] ], in[ indices[i*3 + 2] ], program, fb, options);
        }
    }

    /* half-plane test with the winding-consistent top/left fill convention:
       the strict interior follows the triangle orientation, while a tied
       sample is included when the directed edge goes down or is a
       leftward horizontal edge (y points up). */
    static bool edge_inside(const Vec2& a, const Vec2& b, const Vec2& p, bool ccw)
    {
        const float side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);

        if(side > 0.0f) return ccw;
        if(side < 0.0f) return !ccw;

        const bool down = b.y < a.y;
        const bool horizontal_left = (b.y == a.y) && (b.x < a.x);
        return down || horizontal_left;
    }

    /* orientation-independent barycentrics: weights stay non-negative on a
       covered sample for both windings (edge functions and area share the sign) */
    static Vec3 sample_barycentric(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& p)
    {
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        const float w0 = (b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y);
        const float w1 = (c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y);
        const float w2 = (a.x - p.x) * (b.y - p.y) - (b.x - p.x) * (a.y - p.y);
        return { w0 / area, w1 / area, w2 / area };
    }

    template<typename Vertex, typename Varying, typename Uniforms>
    void draw_triangle_ms(const Varying& v_0, const Varying& v_1, const Varying& v_2, const Program<Vertex, Varying, Uniforms>& program, MultisampleFramebuffer& fb, const Options& options)
    {
        if(options.culling)
        {
            auto normal = cross(Vec3(v_1.position) - Vec3(v_0.position), Vec3(v_2.position) - Vec3(v_0.position));
            if(normal.z < 0.0f) return;
        }

        Recti bbox(v_0.position, v_1.position, v_2.position);

        /* clip against both the viewport and the target dimensions */
        Recti target_rect(0, 0, fb.width(), fb.height());
        bbox.clamp(options.viewport, 0, -1);
        bbox.clamp(target_rect, 0, -1);

        const Vec2 p0(v_0.position);
        const Vec2 p1(v_1.position);
        const Vec2 p2(v_2.position);

        /* orientation decides the strict-interior half-plane; boundary ties
           still follow the directed down-edge / left-horizontal rule */
        const float orientation = cross(Vec3(v_1.position) - Vec3(v_0.position),
                                        Vec3(v_2.position) - Vec3(v_0.position)).z;
        const bool ccw = orientation >= 0.0f;

        for(int x = bbox.min.x; x <= bbox.max.x; x++)
        {
            for(int y = bbox.min.y; y <= bbox.max.y; y++)
            {
                for(std::size_t s = 0; s < MultisampleFramebuffer::sample_count; s++)
                {
                    const Vec2 offset = MultisampleFramebuffer::sample_position(s);
                    const Vec2 sample_point(static_cast<float>(x) + offset.x, static_cast<float>(y) + offset.y);

                    /* coverage is evaluated at the sample's own position */
                    if(!edge_inside(p0, p1, sample_point, ccw) ||
                       !edge_inside(p1, p2, sample_point, ccw) ||
                       !edge_inside(p2, p0, sample_point, ccw))
                        continue;

                    auto bc = sample_barycentric(p0, p1, p2, sample_point);
                    if(std::isnan(bc.x)) continue;

                    /* linear screen-space interpolation, mapped to [0, 1] (per-sample depth) */
                    float w = bc.x * v_0.position.w + bc.y * v_1.position.w + bc.z * v_2.position.w;
                    float z = bc.x * v_0.position.z + bc.y * v_1.position.z + bc.z * v_2.position.z;
                    z = z * 0.5f + 0.5f;

                    /* TODO: clipping should happen earlier */
                    if(0.0f > z || z > 1.0f) continue;

                    /* per-sample early depth test, keeping the library's equal-value pass rule */
                    float& depth = fb.depth(x, y, s);
                    if(z > depth) continue;
                    depth = z;

                    /* perspective correction of barycentric coordinates */
                    bc.x = 1.0f / w * bc.x * v_0.position.w;
                    bc.y = 1.0f / w * bc.y * v_1.position.w;
                    bc.z = 1.0f / w * bc.z * v_2.position.w;

                    /* interpolate fragment data at this sample */
                    Varying inter;
                    interpolate_frag_data(bc, v_0, v_1, v_2, inter);

                    /* default program: Vec4 color output */
                    Vec4 fragColor(0, 0, 0, 0);
                    program.m_fragShader(program.m_uniforms, inter, fragColor);

                    fb.color(x, y, s) = RGBA8(max(min(fragColor, 1.0), 0.0) * 255);
                }
            }
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw_lines(const std::vector<Varying>& in, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < in.size() / 2; i++)
        {
            draw_line(in[i*2 + 0], in[i*2 + 1], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename Indx, typename... Targets>
    void draw_lines(const std::vector<Varying>& in, const std::vector<Indx>& indices, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        for(unsigned int i = 0; i < indices.size() / 2; i++)
        {
            draw_line(in[ indices[i*2 + 0] ], in[ indices[i*2 + 1] ], program, fb, options);
        }
    }

    template<typename Vertex, typename Varying, typename Uniforms, typename... Targets>
    void draw_line(const Varying& v_0, const Varying& v_1, const Program<Vertex, Varying, Uniforms, Framebuffer<Targets...>>& program, Framebuffer<Targets...>& fb, const Options& options)
    {
        /* clamp to viewport */
        auto v0 = clamp(options.viewport, Vec2(v_0.position), 0.0f, -1.0f);
        auto v1 = clamp(options.viewport, Vec2(v_1.position), 0.0f, -1.0f);

        /* rasterize line and iterate fragments */
        Vec2i r0 = v0;
        Vec2i r1 = v1;

        Vec2i step( r0.x < r1.x ? 1 : -1, r0.y < r1.y ? 1 : -1 );
        Vec2i delta( std::abs(r1.x - r0.x), -std::abs(r1.y - r0.y) );
        int err = delta.x + delta.y;
        int err2 = 2 * err;

        for(;r0 != r1; err2 = 2 * err)
        {
            Vec2i pixelCoord = r0;

            /* step to next fragment position */
            if(err2 > delta.y) { err += delta.y; r0.x += step.x; }
            if(err2 < delta.x) { err += delta.x; r0.y += step.y; }

            /* linear interpolate in screen space */
            Vec2 fragCoord = Vec2(pixelCoord.x + 0.5f, pixelCoord.y + 0.5f);
            auto ic = linear(Vec2(v_0.position), Vec2(v_1.position), fragCoord);

            float w = ic.x * v_0.position.w + ic.y * v_1.position.w;
            float z = ic.x * v_0.position.z + ic.y * v_1.position.z;
            z = z * 0.5f + 0.5f;

            /* TODO: clipping should happen earlier */
            if(0.0f > z || z > 1.0f) continue;

            /* early depth test */
            if constexpr (Framebuffer<Targets...>::has_depth)
            {
                auto& depth = fb.depth()(pixelCoord.x, pixelCoord.y);
                if(z > depth) continue;
                depth = z;
            }

            /* perspective correction of linear coordinates */
            ic.x = 1.0f / w * ic.x * v_0.position.w;
            ic.y = 1.0f / w * ic.y * v_1.position.w;

            /* interpolate fragment data */
            Varying inter;
            interpolate_frag_data(ic, v_0, v_1, inter);

            /* call fragment shader, TODO: unecessary complicated to have two different function definitions? */
            if constexpr (std::is_same_v<Framebuffer<Targets...>, DefaultFramebuffer>)
            {
                Vec4 fragColor(0, 0, 0, 0);
                program.m_fragShader(program.m_uniforms, inter, fragColor);

                fb.color()(pixelCoord.x, pixelCoord.y) = RGBA8( max( min(fragColor, 1.0), 0.0) * 255 );
            }
            else
            {
                auto targets = fb.targets(pixelCoord.x, pixelCoord.y);
                program.m_fragShader(program.m_uniforms, inter, targets);
            }
        }
    }

    template<typename Varying>
    void interpolate_frag_data(const Vec3& bc, const Varying& v_0, const Varying& v_1, const Varying& v_2, Varying& result)
    {
        #define VARYING(...) decltype(std::tie( __VA_ARGS__ )) _reflect = std::tie( __VA_ARGS__ );

        auto interpolate = [bc](const auto& x0, const auto& x1, const auto& x2, auto& res)
        {
            res = bc.x * x0 + bc.y * x1 + bc.z * x2;
        };

        detail::tuple_iter(interpolate, v_0._reflect, v_1._reflect, v_2._reflect, result._reflect);
    }

    template<typename Varying>
    void interpolate_frag_data(const Vec2& ic, const Varying& v_0, const Varying& v_1, Varying& result)
    {
        auto interpolate = [ic](const auto& x0, const auto& x1, auto& res)
        {
            res = ic.x * x0 + ic.y * x1;
        };

        detail::tuple_iter(interpolate, v_0._reflect, v_1._reflect, result._reflect);
    }

private:
    DefaultFramebuffer m_framebuffer;
    Options m_options;
};
