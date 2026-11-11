#include <renderer.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

/* ============================================================
   Test types: vertex / fragment data and uniforms
   ============================================================ */

struct Vertex
{
    Vec4 p;
    Vec2 uv;
};

struct Frag
{
    Vec4 position;
    Vec2 uv;
    VARYING(position, uv);
};

struct Uniforms
{
    Vec4 color;
};

/* fragment shaders receive const uniforms, so callbacks are counted globally */
static int frag_call_count = 0;

static void check(bool condition, const std::string& message)
{
    if(!condition)
        throw std::runtime_error("FAILED: " + message);
}

static RGBA8 to_byte(const Vec4& c)
{
    return RGBA8(max(min(c, 1.0f), 0.0f) * 255);
}

static bool near_color(const RGBA8& a, const RGBA8& b, int tol)
{
    return std::abs(int(a.x) - int(b.x)) <= tol &&
           std::abs(int(a.y) - int(b.y)) <= tol &&
           std::abs(int(a.z) - int(b.z)) <= tol &&
           std::abs(int(a.w) - int(b.w)) <= tol;
}

/* ============================================================
   Independent per-sample reference
   ============================================================ */

/* mirror of the renderer's winding-consistent fill convention (reimplemented) */
static bool ref_edge_inside(const Vec2& a, const Vec2& b, const Vec2& p, bool ccw)
{
    const float side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
    if(side > 0.0f) return ccw;
    if(side < 0.0f) return !ccw;
    const bool down = b.y < a.y;
    const bool horizontal_left = (b.y == a.y) && (b.x < a.x);
    return down || horizontal_left;
}

/* orientation-independent barycentrics (independent reimplementation) */
static Vec3 ref_barycentric(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& p)
{
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    const float w0 = (b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y);
    const float w1 = (c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y);
    const float w2 = (a.x - p.x) * (b.y - p.y) - (b.x - p.x) * (a.y - p.y);
    return { w0 / area, w1 / area, w2 / area };
}

struct RefTarget
{
    int W, H;
    Rectf vp;
    std::vector<RGBA8> color;
    std::vector<float> depth;

    RefTarget(int w, int h, const Rectf& viewport, const RGBA8& clear_color)
        : W(w), H(h), vp(viewport),
          color(w * h * 4, clear_color),
          depth(w * h * 4, std::numeric_limits<float>::max())
    {

    }

    std::size_t index(int x, int y, int s) const { return (static_cast<std::size_t>(y) * W + x) * 4 + s; }

    /* same pixel-region truncation as Recti::clamp against the (integer) viewport / target */
    int x0() const { return std::max(0, static_cast<int>(vp.min.x)); }
    int y0() const { return std::max(0, static_cast<int>(vp.min.y)); }
    int x1() const { return std::min(W - 1, static_cast<int>(vp.max.x) - 1); }
    int y1() const { return std::min(H - 1, static_cast<int>(vp.max.y) - 1); }

    void draw_triangle(const Vec4& q0, const Vec4& q1, const Vec4& q2,
                       const Vec2& uv0, const Vec2& uv1, const Vec2& uv2,
                       const std::function<Vec4(const Vec2&)>& shade, int& calls)
    {
        const Vec2 p0(q0), p1(q1), p2(q2);
        const float area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        const bool ccw = area2 >= 0.0f;

        for(int x = x0(); x <= x1(); x++)
        {
            for(int y = y0(); y <= y1(); y++)
            {
                for(int s = 0; s < 4; s++)
                {
                    const Vec2 off = MultisampleFramebuffer::sample_position(s);
                    const Vec2 sp(static_cast<float>(x) + off.x, static_cast<float>(y) + off.y);

                    if(!ref_edge_inside(p0, p1, sp, ccw) ||
                       !ref_edge_inside(p1, p2, sp, ccw) ||
                       !ref_edge_inside(p2, p0, sp, ccw))
                        continue;

                    const Vec3 bc = ref_barycentric(p0, p1, p2, sp);
                    if(std::isnan(bc.x)) continue;

                    const float wlin = bc.x * q0.w + bc.y * q1.w + bc.z * q2.w;
                    float z = bc.x * q0.z + bc.y * q1.z + bc.z * q2.z;
                    z = z * 0.5f + 0.5f;
                    if(0.0f > z || z > 1.0f) continue;

                    float& d = depth[index(x, y, s)];
                    if(z > d) continue;
                    d = z;

                    Vec3 corrected(
                        (1.0f / wlin) * bc.x * q0.w,
                        (1.0f / wlin) * bc.y * q1.w,
                        (1.0f / wlin) * bc.z * q2.w);

                    const Vec2 uv = corrected.x * uv0 + corrected.y * uv1 + corrected.z * uv2;
                    color[index(x, y, s)] = to_byte(shade(uv));
                    calls++;
                }
            }
        }
    }
};

/* full 8x8 viewport: build a clip-space vertex (positive w) landing at screen position (sx, sy) */
static Vec4 clip_from_screen(float sx, float sy, float z_ndc, float w)
{
    return Vec4((sx / 4.0f - 1.0f) * w, (sy / 4.0f - 1.0f) * w, z_ndc * w, w);
}

/* run the renderer vertex stage expectation: perspective divide + viewport map */
static Vec4 screen_space(const Vec4& clip, const Rectf& vp)
{
    const float iw = 1.0f / clip.w;
    Vec4 q(clip.x * iw, clip.y * iw, clip.z * iw, iw);
    q.x = vp.min.x + (q.x + 1.0f) * 0.5f * (vp.max.x - vp.min.x);
    q.y = vp.min.y + (q.y + 1.0f) * 0.5f * (vp.max.y - vp.min.y);
    return q;
}

static const Rectf full_viewport(0, 0, 8, 8);
static const RGBA8 clear_color(0, 0, 0, 255);
static const RGBA8 red(255, 0, 0, 255);
static const RGBA8 green(0, 255, 0, 255);
static const RGBA8 blue(0, 0, 255, 255);

static Program<Vertex, Frag, Uniforms> make_program()
{
    Program<Vertex, Frag, Uniforms> p;
    p.onVertex([](const Uniforms&, const Vertex& v, Frag& f)
    {
        f.position = v.p;
        f.uv = v.uv;
    });
    p.onFragment([](const Uniforms& u, const Frag& f, Vec4& c)
    {
        frag_call_count++;
        c = u.color;
        if(u.color.w < 0.0f) c = Vec4(f.uv.x, f.uv.y, 0.25f, 1.0f); /* uv probe channel */
    });
    return p;
}

/* ============================================================
   Tests
   ============================================================ */

int main(int argc, char** argv)
{
    const std::string ppm_path = (argc > 1) ? argv[1] : "";

    /* ---------- Test A: partial coverage against the per-sample reference ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(1, 0, 0, 1);

        Buffer<Vertex> buf;
        buf.vertices = {
            { Vec4(-0.8f, -0.7f, 0.0f, 1.0f), Vec2(0, 0) },
            { Vec4( 0.65f, -0.7f, 0.0f, 1.0f), Vec2(1, 0) },
            { Vec4(-0.1f,  0.8f, 0.0f, 1.0f), Vec2(0, 1) }
        };

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        frag_call_count = 0;
        renderer.draw(program, buf, fb);

        RefTarget ref(8, 8, full_viewport, clear_color);
        int ref_calls = 0;
        const Vec2 uv0(0, 0), uv1(1, 0), uv2(0, 1);
        ref.draw_triangle(screen_space(buf.vertices[0].p, full_viewport),
                          screen_space(buf.vertices[1].p, full_viewport),
                          screen_space(buf.vertices[2].p, full_viewport),
                          uv0, uv1, uv2,
                          [](const Vec2&) { return Vec4(1, 0, 0, 1); }, ref_calls);

        int covered_samples = 0;
        int partial_pixels = 0;
        int quarter_pixels = 0;
        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                int covered = 0;
                for(int s = 0; s < 4; s++)
                {
                    const RGBA8 got = fb.color(x, y, s);
                    const RGBA8 want = ref.color[ref.index(x, y, s)];
                    check(got == want, "Test A sample color mismatch against reference");
                    if(want == red) covered++;
                }
                covered_samples += covered;
                if(covered > 0 && covered < 4) partial_pixels++;
                if(covered == 1) quarter_pixels++;
            }
        }
        check(covered_samples == ref_calls, "Test A covered sample count differs from reference");
        check(covered_samples > 0, "Test A triangle covers nothing");
        check(partial_pixels > 0, "Test A expected partially covered pixels (anti-aliased edge)");
        check(quarter_pixels > 0, "Test A expected at least one quarter-covered pixel");
        check(frag_call_count == ref_calls, "Test A shader callback count mismatch");

        /* resolve: (sum + 2) / 4 per channel, independent of depth */
        const Texture<RGBA8> resolved = fb.resolve();
        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                int sr = 0, sg = 0, sb = 0, sa = 0;
                for(int s = 0; s < 4; s++)
                {
                    const RGBA8 c = fb.color(x, y, s);
                    sr += c.x; sg += c.y; sb += c.z; sa += c.w;
                }
                const RGBA8 want((sr + 2) / 4, (sg + 2) / 4, (sb + 2) / 4, (sa + 2) / 4);
                check(resolved(x, y) == want, "Test A resolve mismatch");
            }
        }

        /* small PPM showing the quarter-coverage edge */
        if(!ppm_path.empty())
        {
            std::ofstream out(ppm_path, std::ios::binary);
            check(out.good(), "cannot open PPM output path");
            out << "P6\n" << 8 << " " << 8 << "\n255\n";
            for(int y = 7; y >= 0; y--) /* flip into top-left image rows */
            {
                for(int x = 0; x < 8; x++)
                {
                    const RGBA8 c = resolved(x, y);
                    const char rgb[3] = { char(c.x), char(c.y), char(c.z) };
                    out.write(rgb, 3);
                }
            }
            check(out.good(), "failed writing PPM");
        }

        std::cout << "A partial-coverage OK: covered_samples=" << covered_samples
                  << " partial_pixels=" << partial_pixels
                  << " quarter_pixels=" << quarter_pixels << "\n";
    }

    /* ---------- Test B: repeated front/back coverage, per-sample depth ---------- */
    {
        auto program = make_program();
        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        RefTarget ref(8, 8, full_viewport, clear_color);
        int ref_calls = 0;
        frag_call_count = 0;

        const Vec2 uv0(0, 0), uv1(1, 0), uv2(0, 1);
        struct Tri { Vec4 c[3]; float z; Vec4 color; };
        const Tri triangles[3] = {
            { { clip_from_screen(0.5f, 0.5f, 0.6f, 1), clip_from_screen(7.5f, 0.5f, 0.6f, 1), clip_from_screen(0.5f, 7.5f, 0.6f, 1) }, 0.6f, Vec4(0, 0, 1, 1) },
            { { clip_from_screen(1.0f, 1.0f, 0.9f, 1), clip_from_screen(7.0f, 1.0f, 0.9f, 1), clip_from_screen(1.0f, 7.0f, 0.9f, 1) }, 0.9f, Vec4(0, 1, 0, 1) },
            { { clip_from_screen(2.0f, 2.0f, 0.2f, 1), clip_from_screen(6.5f, 2.0f, 0.2f, 1), clip_from_screen(2.0f, 6.0f, 0.2f, 1) }, 0.2f, Vec4(1, 0, 0, 1) }
        };

        for(const Tri& tri : triangles)
        {
            Buffer<Vertex> buf;
            for(const Vec4& c : tri.c) buf.vertices.push_back({ c, Vec2() });
            program.uniforms().color = tri.color;
            renderer.draw(program, buf, fb);

            const Vec4 q0 = screen_space(tri.c[0], full_viewport);
            const Vec4 q1 = screen_space(tri.c[1], full_viewport);
            const Vec4 q2 = screen_space(tri.c[2], full_viewport);
            const Vec4 color = tri.color;
            ref.draw_triangle(q0, q1, q2, uv0, uv1, uv2,
                              [color](const Vec2&) { return color; }, ref_calls);
        }

        int mixed_depth_pixels = 0;
        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                bool different = false;
                for(int s = 1; s < 4; s++)
                    different = different || (fb.depth(x, y, s) != fb.depth(x, y, 0));
                mixed_depth_pixels += different;

                for(int s = 0; s < 4; s++)
                {
                    check(fb.color(x, y, s) == ref.color[ref.index(x, y, s)], "Test B color winner mismatch");
                    check(fb.depth(x, y, s) == ref.depth[ref.index(x, y, s)], "Test B per-sample depth mismatch");
                }
            }
        }
        check(mixed_depth_pixels > 0, "Test B expected pixels holding different depths per sample");
        check(frag_call_count == ref_calls, "Test B callback count mismatch");

        std::cout << "B layered depth OK: mixed-depth pixels=" << mixed_depth_pixels << "\n";
    }

    /* ---------- Test C: perspective-corrected UV with different w ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(0, 0, 0, -1.0f); /* switch fragment shader into uv mode */

        Buffer<Vertex> buf;
        buf.vertices = {
            { Vec4(-0.8f, -0.7f, 0.0f, 1.0f), Vec2(0, 0) },
            { Vec4( 0.65f * 2.0f, -0.7f * 2.0f, 0.0f, 2.0f), Vec2(1, 0) },
            { Vec4(-0.1f * 3.0f,  0.8f * 3.0f, 0.0f, 3.0f), Vec2(0, 1) }
        };

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        frag_call_count = 0;
        renderer.draw(program, buf, fb);

        RefTarget ref(8, 8, full_viewport, clear_color);
        int ref_calls = 0;
        ref.draw_triangle(screen_space(buf.vertices[0].p, full_viewport),
                          screen_space(buf.vertices[1].p, full_viewport),
                          screen_space(buf.vertices[2].p, full_viewport),
                          Vec2(0, 0), Vec2(1, 0), Vec2(0, 1),
                          [](const Vec2& uv) { return Vec4(uv.x, uv.y, 0.25f, 1.0f); }, ref_calls);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                for(int s = 0; s < 4; s++)
                {
                    check(near_color(fb.color(x, y, s), ref.color[ref.index(x, y, s)], 1),
                          "Test C perspective-corrected UV mismatch");
                }
            }
        }
        check(frag_call_count == ref_calls, "Test C callback count mismatch");

        std::cout << "C perspective UV (w=1,2,3) OK: calls=" << ref_calls << "\n";
    }

    /* ---------- Test D: indexed / non-indexed consistency ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(1, 0, 0, 1);

        const std::vector<Vertex> vertices = {
            { Vec4(-0.8f, -0.7f, 0.0f, 1.0f), Vec2(0, 0) },
            { Vec4( 0.65f, -0.7f, 0.0f, 1.0f), Vec2(1, 0) },
            { Vec4(-0.1f,  0.8f, 0.0f, 1.0f), Vec2(0, 1) },
            { Vec4( 0.8f,  0.8f, 0.3f, 1.0f), Vec2(1, 1) }
        };

        Buffer<Vertex> plain;
        plain.vertices = { vertices[0], vertices[1], vertices[2], vertices[1], vertices[3], vertices[2] };

        BufferIndexed<Vertex, unsigned> indexed;
        indexed.vertices = vertices;
        indexed.indices = { 0, 1, 2, 1, 3, 2 };

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb_a(8, 8);
        fb_a.clear(clear_color);
        renderer.draw(program, plain, fb_a);

        MultisampleFramebuffer fb_b(8, 8);
        fb_b.clear(clear_color);
        renderer.draw(program, indexed, fb_b);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                for(int s = 0; s < 4; s++)
                {
                    check(fb_a.color(x, y, s) == fb_b.color(x, y, s), "Test D indexed color mismatch");
                    check(fb_a.depth(x, y, s) == fb_b.depth(x, y, s), "Test D indexed depth mismatch");
                }
            }
        }

        const Texture<RGBA8> ra = fb_a.resolve();
        const Texture<RGBA8> rb = fb_b.resolve();
        check(ra.data() == rb.data(), "Test D resolved images differ");

        std::cout << "D indexed/non-indexed consistency OK\n";
    }

    /* ---------- Test E: shared edges, callback counts, reverse winding ---------- */
    {
        auto program = make_program();
        Renderer renderer(8, 8);
        renderer.options().culling = false;

        /* E1: quad split along a diagonal, both triangles CCW, shared edge opposite directions */
        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        RefTarget ref(8, 8, full_viewport, clear_color);
        int ref_calls = 0;
        frag_call_count = 0;
        const Vec2 uv0(0, 0), uv1(1, 0), uv2(0, 1);

        const Vec4 a = clip_from_screen(1, 1, 0.0f, 1);
        const Vec4 b = clip_from_screen(6, 1, 0.0f, 1);
        const Vec4 c = clip_from_screen(6, 6, 0.0f, 1);
        const Vec4 d = clip_from_screen(1, 6, 0.0f, 1);

        Buffer<Vertex> t1;
        t1.vertices = { { a, Vec2() }, { b, Vec2() }, { c, Vec2() } };
        Buffer<Vertex> t2;
        t2.vertices = { { a, Vec2() }, { c, Vec2() }, { d, Vec2() } };

        program.uniforms().color = Vec4(1, 0, 0, 1);
        renderer.draw(program, t1, fb);
        ref.draw_triangle(screen_space(a, full_viewport), screen_space(b, full_viewport),
                          screen_space(c, full_viewport), uv0, uv1, uv2,
                          [](const Vec2&) { return Vec4(1, 0, 0, 1); }, ref_calls);

        program.uniforms().color = Vec4(0, 1, 0, 1);
        renderer.draw(program, t2, fb);
        ref.draw_triangle(screen_space(a, full_viewport), screen_space(c, full_viewport),
                          screen_space(d, full_viewport), uv0, uv1, uv2,
                          [](const Vec2&) { return Vec4(0, 1, 0, 1); }, ref_calls);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                for(int s = 0; s < 4; s++)
                    check(fb.color(x, y, s) == ref.color[ref.index(x, y, s)], "E1 shared-edge color mismatch");
            }
        }
        check(frag_call_count == ref_calls, "E1 shared edge must not generate duplicate callbacks");

        /* E2: horizontal shared edge at y = 2.625, row-2 sample 2 lands exactly on it.
           The edge runs rightward in the lower triangle (excluded) and leftward in the
           upper one (included): tied samples belong to exactly one triangle. */
        const Vec4 ea = clip_from_screen(1, 2.625f, 0.0f, 1);
        const Vec4 eb = clip_from_screen(6, 2.625f, 0.0f, 1);
        const Vec4 ec = clip_from_screen(3.5f, 0.5f, 0.0f, 1);
        const Vec4 ed = clip_from_screen(3.5f, 5.5f, 0.0f, 1);

        Buffer<Vertex> lower;
        lower.vertices = { { ea, Vec2() }, { eb, Vec2() }, { ec, Vec2() } };
        Buffer<Vertex> upper;
        upper.vertices = { { eb, Vec2() }, { ea, Vec2() }, { ed, Vec2() } };

        MultisampleFramebuffer fb2(8, 8);
        fb2.clear(clear_color);
        RefTarget ref2(8, 8, full_viewport, clear_color);
        int ref_calls2 = 0;
        frag_call_count = 0;

        program.uniforms().color = Vec4(0, 0, 1, 1);
        renderer.draw(program, lower, fb2);
        ref2.draw_triangle(screen_space(ea, full_viewport), screen_space(eb, full_viewport),
                           screen_space(ec, full_viewport), uv0, uv1, uv2,
                           [](const Vec2&) { return Vec4(0, 0, 1, 1); }, ref_calls2);

        program.uniforms().color = Vec4(0, 1, 0, 1);
        renderer.draw(program, upper, fb2);
        ref2.draw_triangle(screen_space(eb, full_viewport), screen_space(ea, full_viewport),
                           screen_space(ed, full_viewport), uv0, uv1, uv2,
                           [](const Vec2&) { return Vec4(0, 1, 0, 1); }, ref_calls2);

        int tied = 0;
        for(int x = 0; x < 8; x++)
        {
            const int y = 2;
            const int s = 2; /* offset (1/8, 5/8): 2 + 5/8 = 2.625 */
            const Vec2 sp(x + 1.0f / 8.0f, 2.625f);
            const bool on_segment = sp.x >= 1.0f && sp.x < 6.0f;
            if(on_segment)
            {
                tied++;
                check(fb2.color(x, y, s) == green, "E2 tied sample must belong to the upper triangle only");
                check(fb2.color(x, y, s) == ref2.color[ref2.index(x, y, s)], "E2 reference mismatch");
            }
        }
        check(tied == 5, "E2 expected five samples exactly on the shared horizontal edge");
        check(frag_call_count == ref_calls2, "E2 tied samples must be shaded exactly once");

        /* E3: reversed winding covers the same region with culling disabled */
        Buffer<Vertex> forward;
        forward.vertices = { { a, Vec2() }, { b, Vec2() }, { c, Vec2() } };
        Buffer<Vertex> reversed;
        reversed.vertices = { { a, Vec2() }, { c, Vec2() }, { b, Vec2() } };

        MultisampleFramebuffer fb_f(8, 8);
        fb_f.clear(clear_color);
        MultisampleFramebuffer fb_r(8, 8);
        fb_r.clear(clear_color);

        program.uniforms().color = Vec4(1, 0, 0, 1);
        renderer.draw(program, forward, fb_f);
        renderer.draw(program, reversed, fb_r);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                for(int s = 0; s < 4; s++)
                    check(fb_f.color(x, y, s) == fb_r.color(x, y, s), "E3 reverse winding coverage differs");
            }
        }

        std::cout << "E shared edges / winding OK: diagonal calls=" << ref_calls
                  << " tied=" << tied << "\n";
    }

    /* ---------- Test F: clipping to viewport and target dimensions ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(1, 0, 0, 1);

        Buffer<Vertex> big;
        big.vertices = {
            { clip_from_screen(-3, -3, 0, 1), Vec2() },
            { clip_from_screen(11, -3, 0, 1), Vec2() },
            { clip_from_screen(-3, 11, 0, 1), Vec2() }
        };

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        /* F1: viewport (2,2)-(6,6) */
        const Rectf small_vp(2, 2, 6, 6);
        Renderer::Options options = renderer.options();
        options.viewport = small_vp;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        renderer.draw(program, big, fb, options);

        RefTarget ref(8, 8, small_vp, clear_color);
        int ref_calls = 0;
        ref.draw_triangle(screen_space(big.vertices[0].p, small_vp),
                          screen_space(big.vertices[1].p, small_vp),
                          screen_space(big.vertices[2].p, small_vp),
                          Vec2(), Vec2(), Vec2(),
                          [](const Vec2&) { return Vec4(1, 0, 0, 1); }, ref_calls);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                const bool inside_vp = (x >= 2 && x < 6 && y >= 2 && y < 6);
                for(int s = 0; s < 4; s++)
                {
                    check(fb.color(x, y, s) == ref.color[ref.index(x, y, s)], "F1 viewport clip mismatch");
                    if(!inside_vp)
                        check(fb.color(x, y, s) == clear_color, "F1 sample outside viewport was written");
                }
            }
        }

        /* F2: viewport larger than the target: bbox clamps to the target grid */
        const Rectf huge_vp(-4, -4, 12, 12);
        options.viewport = huge_vp;
        MultisampleFramebuffer fb2(8, 8);
        fb2.clear(clear_color);
        renderer.draw(program, big, fb2, options);

        RefTarget ref2(8, 8, huge_vp, clear_color);
        int ref_calls2 = 0;
        ref2.draw_triangle(screen_space(big.vertices[0].p, huge_vp),
                           screen_space(big.vertices[1].p, huge_vp),
                           screen_space(big.vertices[2].p, huge_vp),
                           Vec2(), Vec2(), Vec2(),
                           [](const Vec2&) { return Vec4(1, 0, 0, 1); }, ref_calls2);

        for(int y = 0; y < 8; y++)
        {
            for(int x = 0; x < 8; x++)
            {
                for(int s = 0; s < 4; s++)
                    check(fb2.color(x, y, s) == ref2.color[ref2.index(x, y, s)], "F2 target clip mismatch");
            }
        }

        std::cout << "F viewport/target clipping OK\n";
    }

    /* ---------- Test G: resolve returns an independent snapshot ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(1, 0, 0, 1);

        Buffer<Vertex> buf;
        buf.vertices = {
            { Vec4(-0.8f, -0.7f, 0, 1), Vec2() },
            { Vec4( 0.65f, -0.7f, 0, 1), Vec2() },
            { Vec4(-0.1f,  0.8f, 0, 1), Vec2() }
        };

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        renderer.draw(program, buf, fb);

        const Texture<RGBA8> snapshot = fb.resolve();
        const std::vector<RGBA8> saved = snapshot.data();

        /* mutate the target afterwards: old texture must not change */
        fb.clear(RGBA8(10, 20, 30, 40));
        program.uniforms().color = Vec4(0, 0, 1, 1);
        renderer.draw(program, buf, fb);

        check(snapshot.data() == saved, "G resolve snapshot changed after redraw/clear");

        const Texture<RGBA8> after = fb.resolve();
        check(after.data() != saved, "G expected the new resolve to differ");

        /* mutating the returned texture leaves the target's samples alone */
        Texture<RGBA8> editable = fb.resolve();
        const RGBA8 before = fb.color(0, 0, 0);
        editable(0, 0) = RGBA8(99, 99, 99, 99);
        check(fb.color(0, 0, 0) == before, "G resolved texture aliases target storage");

        std::cout << "G resolve snapshot independence OK\n";
    }

    /* ---------- Test H: unsupported modes throw before any sample is written ---------- */
    {
        auto program = make_program();
        program.uniforms().color = Vec4(1, 0, 0, 1);

        Renderer renderer(8, 8);
        renderer.options().culling = false;

        MultisampleFramebuffer fb(8, 8);
        fb.clear(clear_color);
        const std::vector<RGBA8> saved_colors = [&]
        {
            std::vector<RGBA8> v;
            for(int y = 0; y < 8; y++)
                for(int x = 0; x < 8; x++)
                    for(int s = 0; s < 4; s++)
                        v.push_back(fb.color(x, y, s));
            return v;
        }();
        const std::vector<float> saved_depths = [&]
        {
            std::vector<float> v;
            for(int y = 0; y < 8; y++)
                for(int x = 0; x < 8; x++)
                    for(int s = 0; s < 4; s++)
                        v.push_back(fb.depth(x, y, s));
            return v;
        }();

        auto untouched = [&]()
        {
            std::size_t i = 0;
            for(int y = 0; y < 8; y++)
                for(int x = 0; x < 8; x++)
                    for(int s = 0; s < 4; s++, i++)
                        if(fb.color(x, y, s) != saved_colors[i] || fb.depth(x, y, s) != saved_depths[i])
                            return false;
            return true;
        };

        Buffer<Vertex> lines;
        lines.primitive = ePrimitive::LINES;
        lines.vertices = {
            { Vec4(-0.8f, -0.7f, 0, 1), Vec2() },
            { Vec4( 0.65f,  0.8f, 0, 1), Vec2() }
        };

        bool threw = false;
        try { renderer.draw(program, lines, fb); }
        catch(const std::invalid_argument&) { threw = true; }
        check(threw, "H LINES must throw invalid_argument");
        check(untouched(), "H LINES wrote samples before throwing");

        BufferIndexed<Vertex, unsigned> lines_idx;
        lines_idx.primitive = ePrimitive::LINES;
        lines_idx.vertices = lines.vertices;
        lines_idx.indices = { 0, 1 };
        threw = false;
        try { renderer.draw(program, lines_idx, fb); }
        catch(const std::invalid_argument&) { threw = true; }
        check(threw, "H indexed LINES must throw invalid_argument");
        check(untouched(), "H indexed LINES wrote samples before throwing");

        Buffer<Vertex> tris;
        tris.vertices = {
            { Vec4(-0.8f, -0.7f, 0, 1), Vec2() },
            { Vec4( 0.65f, -0.7f, 0, 1), Vec2() },
            { Vec4(-0.1f,  0.8f, 0, 1), Vec2() }
        };
        Renderer::Options wire = renderer.options();
        wire.wireframe = true;
        threw = false;
        try { renderer.draw(program, tris, fb, wire); }
        catch(const std::invalid_argument&) { threw = true; }
        check(threw, "H wireframe must throw invalid_argument");
        check(untouched(), "H wireframe wrote samples before throwing");

        BufferIndexed<Vertex, unsigned> tris_idx;
        tris_idx.vertices = tris.vertices;
        tris_idx.indices = { 0, 1, 2 };
        threw = false;
        try { renderer.draw(program, tris_idx, fb, wire); }
        catch(const std::invalid_argument&) { threw = true; }
        check(threw, "H indexed wireframe must throw invalid_argument");
        check(untouched(), "H indexed wireframe wrote samples before throwing");

        /* ordinary targets keep LINES and wireframe support */
        DefaultFramebuffer& df = renderer.framebuffer();
        df.clear(Vec4(0, 0, 0, 1));
        renderer.draw(program, lines);
        const Renderer::Options saved_options = renderer.options();
        renderer.options().wireframe = true;
        renderer.draw(program, tris);
        renderer.options() = saved_options;
        bool any_written = false;
        for(const RGBA8& c : df.color().data()) any_written = any_written || (c.x != 0);
        check(any_written, "H default framebuffer lost LINES/wireframe support");

        std::cout << "H unsupported modes zero-write OK\n";
    }

    std::cout << "MSAA4_OK" << std::endl;
    return 0;
}
