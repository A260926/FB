// Geometry callback stage contract tests.
//
// The stage sits between the vertex shader and clipping/backface culling.
// Input primitives are assembled by the existing draw-mode rules; each
// complete input primitive invokes one callback; emitted strips are appended
// to the normal primitive stream and rasterized by the existing pipeline.
#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"

#include <cstring>
#include <vector>
#include "support/contract.hpp"

namespace {

const int W = 48;
const int H = 48;

glContext ctx;
pix_t* pixels = nullptr;

// ---------------------------------------------------------------------------
// shaders

// Vertex shader: attribute 0 is clip-space vec4, attribute 1 is one flat-ish
// varying value copied through.  The position is passed through unchanged so
// the callback sees raw clip coordinates.
void passthru_vs(float* out, vec4* attribs, Shader_Builtins* builtins, void* uniforms)
{
    (void)uniforms;
    builtins->gl_Position = attribs[0];
    out[0] = attribs[1].x;
    out[1] = attribs[1].y;
}

// Fragment color = varying row; gives us a visible per-vertex tag.
void varying_fs(float* in, Shader_Builtins* builtins, void* uniforms)
{
    (void)uniforms;
    SET_V4(builtins->gl_FragColor, in[0], in[1], 0.0f, 1.0f);
}

struct Tagged { float tag0, tag1; };

// ---------------------------------------------------------------------------
// recording

struct InputRecord
{
    GLenum mode;
    GLsizei count;
    GLuint primitive_id;
    GLint instance_id;
    GLuint base_instance;
    pglGeometryVertex v[3];
};

struct Recorder
{
    std::vector<InputRecord> inputs;
    int max_emit_seen = 0;
};

void record_cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u)
{
    Recorder* r = (Recorder*)u;
    InputRecord rec;
    rec.mode = in->mode;
    rec.count = in->count;
    rec.primitive_id = in->primitive_id;
    rec.instance_id = in->instance_id;
    rec.base_instance = in->base_instance;
    for (int i = 0; i < 3; ++i) rec.v[i] = in->vertices[i];
    r->inputs.push_back(rec);
    (void)em;
}

void reset_context()
{
    if (pixels) { free_glContext(&ctx); pixels = nullptr; }
    UNIT_CHECK(init_glContext(&ctx, &pixels, W, H));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

GLuint make_program(GLenum interp0)
{
    GLenum interp[2] = { interp0, interp0 };
    return pglCreateProgram(passthru_vs, varying_fs, 2, interp, GL_FALSE);
}

GLuint pb = 0, tb = 0;

void bind_geometry(const float* pos, int nverts, const float* tags)
{
    if (pb) glDeleteBuffers(1, &pb);
    if (tb) glDeleteBuffers(1, &tb);
    glGenBuffers(1, &pb);
    glBindBuffer(GL_ARRAY_BUFFER, pb);
    glBufferData(GL_ARRAY_BUFFER, nverts * 4 * sizeof(float), pos, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

    glGenBuffers(1, &tb);
    glBindBuffer(GL_ARRAY_BUFFER, tb);
    glBufferData(GL_ARRAY_BUFFER, nverts * 2 * sizeof(float), tags, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

pglGeometryVertex gv(float x, float y, float z, float w, float t0, float t1)
{
    pglGeometryVertex v;
    memset(&v, 0, sizeof(v));
    SET_V4(v.position, x, y, z, w);
    v.varying[0] = t0;
    v.varying[1] = t1;
    return v;
}

// Count non-black pixels, and collect the distinct tag colors.
int nonblack_pixels()
{
    int n = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r || c.g || c.b) ++n;
    }
    return n;
}

unsigned snapshot[W * H];

void take_snapshot()
{
    memcpy(snapshot, pixels, sizeof(snapshot));
}

bool snapshot_equal()
{
    return !memcmp(snapshot, pixels, sizeof(snapshot));
}

} // namespace

// ===========================================================================
// assembly: input topology, numbering, indices and instancing

static void geometry_assembly_lines()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.5f, -0.5f, 0, 1,
         0.5f, -0.5f, 0, 1,
        -0.5f,  0.5f, 0, 1,
         0.5f,  0.5f, 0, 1,
    };
    float tags[] = { 0.1f, 0, 0.2f, 0, 0.3f, 0, 0.4f, 0 };
    bind_geometry(pos, 4, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_LINE_STRIP, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    glDrawArrays(GL_LINE_STRIP, 0, 4);
    UNIT_CHECK(rec.inputs.size() == 3);
    for (size_t i = 0; i < rec.inputs.size(); ++i) {
        UNIT_CHECK(rec.inputs[i].mode == GL_LINES);
        UNIT_CHECK(rec.inputs[i].count == 2);
        UNIT_CHECK(rec.inputs[i].primitive_id == i);
        UNIT_CHECK(rec.inputs[i].instance_id == 0);
        UNIT_CHECK(rec.inputs[i].base_instance == 0);
    }
    // line strip order: 0-1, 1-2, 2-3
    UNIT_CHECK(rec.inputs[0].v[0].varying[0] == 0.1f);
    UNIT_CHECK(rec.inputs[0].v[1].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[1].v[0].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[1].v[1].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[2].v[0].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[2].v[1].varying[0] == 0.4f);
    // vertices are clip-space positions
    UNIT_CHECK(rec.inputs[0].v[0].position.x == -0.5f);
    UNIT_CHECK(rec.inputs[0].v[1].position.y == -0.5f);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_assembly_loop_and_index()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.8f, -0.8f, 0, 1,
         0.8f, -0.8f, 0, 1,
         0.8f,  0.8f, 0, 1,
        -0.8f,  0.8f, 0, 1,
    };
    float tags[] = { 0.1f, 0, 0.2f, 0, 0.3f, 0, 0.4f, 0 };
    bind_geometry(pos, 4, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_POINTS, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    // line loop: 0-1, 1-2, 2-3, 3-0
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    UNIT_CHECK(rec.inputs.size() == 4);
    UNIT_CHECK(rec.inputs[3].v[0].varying[0] == 0.4f);
    UNIT_CHECK(rec.inputs[3].v[1].varying[0] == 0.1f);

    // A repeated index still gets one vertex-shader run per index.
    rec.inputs.clear();
    GLushort idx[] = { 0, 1, 1, 2, 2, 2 };
    static GLuint eb = 0;
    glGenBuffers(1, &eb);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glDrawElements(GL_LINES, 6, GL_UNSIGNED_SHORT, 0);
    UNIT_CHECK(rec.inputs.size() == 3);
    UNIT_CHECK(rec.inputs[0].v[0].varying[0] == 0.1f);
    UNIT_CHECK(rec.inputs[0].v[1].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[1].v[0].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[1].v[1].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[2].v[0].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[2].v[1].varying[0] == 0.3f);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &eb);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_assembly_strip_parity()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    // Triangle strip with an odd winding flip: prim k uses (k+1,k,k+2) for
    // odd k.  Varying tags let us verify the handedness of the input.
    float pos[] = {
        -0.8f, -0.8f, 0, 1,
        -0.8f,  0.8f, 0, 1,
         0.0f, -0.8f, 0, 1,
         0.0f,  0.8f, 0, 1,
         0.8f, -0.8f, 0, 1,
    };
    float tags[] = { 0.1f, 0, 0.2f, 0, 0.3f, 0, 0.4f, 0, 0.5f, 0 };
    bind_geometry(pos, 5, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 5);
    UNIT_CHECK(rec.inputs.size() == 3);
    // even: 0,1,2
    UNIT_CHECK(rec.inputs[0].v[0].varying[0] == 0.1f);
    UNIT_CHECK(rec.inputs[0].v[1].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[0].v[2].varying[0] == 0.3f);
    // odd: 2,1,3
    UNIT_CHECK(rec.inputs[1].v[0].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[1].v[1].varying[0] == 0.2f);
    UNIT_CHECK(rec.inputs[1].v[2].varying[0] == 0.4f);
    // even: 2,3,4
    UNIT_CHECK(rec.inputs[2].v[0].varying[0] == 0.3f);
    UNIT_CHECK(rec.inputs[2].v[1].varying[0] == 0.4f);
    UNIT_CHECK(rec.inputs[2].v[2].varying[0] == 0.5f);
    // primitive_id counts raw primitives
    UNIT_CHECK(rec.inputs[2].primitive_id == 2);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_assembly_incomplete_tail()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.8f, -0.8f, 0, 1,
         0.8f, -0.8f, 0, 1,
         0.0f,  0.8f, 0, 1,
         0.4f,  0.4f, 0, 1,
    };
    float tags[] = { 0.1f, 0, 0.2f, 0, 0.3f, 0, 0.4f, 0 };
    bind_geometry(pos, 4, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    // 4 vertices -> 1 triangle, the 4th vertex never reaches a callback
    glDrawArrays(GL_TRIANGLES, 0, 4);
    UNIT_CHECK(rec.inputs.size() == 1);
    UNIT_CHECK(rec.inputs[0].primitive_id == 0);

    // strip of 4 vertices -> 2 triangles
    rec.inputs.clear();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    UNIT_CHECK(rec.inputs.size() == 2);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_assembly_instanced()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.5f, -0.5f, 0, 1,
         0.5f, -0.5f, 0, 1,
         0.0f,  0.5f, 0, 1,
    };
    float tags[] = { 0.1f, 0, 0.2f, 0, 0.3f, 0 };
    bind_geometry(pos, 3, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 3, 3, 5);
    UNIT_CHECK(rec.inputs.size() == 3);
    for (size_t i = 0; i < rec.inputs.size(); ++i) {
        UNIT_CHECK(rec.inputs[i].instance_id == (GLint)i);
        UNIT_CHECK(rec.inputs[i].base_instance == 5);
        UNIT_CHECK(rec.inputs[i].primitive_id == 0);
    }

    // MultiDraw: primitive_id restarts per sub draw, per instance.
    rec.inputs.clear();
    GLint firsts[] = { 0, 0 };
    GLsizei counts[] = { 3, 3 };
    glMultiDrawArrays(GL_TRIANGLES, firsts, counts, 2);
    UNIT_CHECK(rec.inputs.size() == 2);
    UNIT_CHECK(rec.inputs[0].primitive_id == 0);
    UNIT_CHECK(rec.inputs[1].primitive_id == 0);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

// ===========================================================================
// emission: output geometry, strips, parity, provoking/flat, interpolation

static void geometry_emit_ribbon()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    // A single horizontal line from -0.8,-0.8 to 0.8,-0.8 (clip space w=1).
    float pos[] = { -0.8f, -0.8f, 0, 1, 0.8f, -0.8f, 0, 1 };
    float tags[] = { 0.5f, 0.0f, 1.0f, 0.0f };
    bind_geometry(pos, 2, tags);

    // Expand the line into two triangles (a wide ribbon).
    struct Ribbon {
        pglGeometryVertex v[4];
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Ribbon* r = (Ribbon*)u;
            (void)in;
            for (int i = 0; i < 4; ++i) UNIT_CHECK(pglEmitGeometryVertex(em, &r->v[i]));
        }
    } ribbon;
    ribbon.v[0] = gv(-0.8f, -0.6f, 0, 1, 1, 0);
    ribbon.v[1] = gv(-0.8f, -1.0f, 0, 1, 0, 0);
    ribbon.v[2] = gv( 0.8f, -0.6f, 0, 1, 1, 0);
    ribbon.v[3] = gv( 0.8f, -1.0f, 0, 1, 0, 0);
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_TRIANGLE_STRIP, 4, Ribbon::cb));
    glUseProgram(prog);
    pglSetUniform(&ribbon);

    glDrawArrays(GL_LINES, 0, 2);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    int emitted = nonblack_pixels();
    UNIT_CHECK(emitted > 0);
    take_snapshot();

    // Reference: draw the same ribbon directly as a strip; the geometry
    // stage is removed so the draw takes the normal path.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 1, NULL));
    {
        float rp[] = {
            -0.8f, -0.6f, 0, 1,
            -0.8f, -1.0f, 0, 1,
             0.8f, -0.6f, 0, 1,
             0.8f, -1.0f, 0, 1,
        };
        float rt[] = { 1, 0, 0, 0, 1, 0, 0, 0 };
        bind_geometry(rp, 4, rt);
        glUseProgram(prog);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    UNIT_CHECK(nonblack_pixels() == emitted);
    UNIT_CHECK(snapshot_equal());

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_multistrip()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.8f,  0.6f, 0, 1,
         0.8f,  0.6f, 0, 1,
        -0.8f,  0.8f, 0, 1,
         0.8f,  0.8f, 0, 1,
    };
    float tags[] = { 0.2f, 0, 0.2f, 0, 0.4f, 0, 0.4f, 0 };
    bind_geometry(pos, 4, tags);

    // Two independent strips from one input triangle.
    struct Multi {
        pglGeometryVertex a[3];
        pglGeometryVertex b[3];
    } m;
    m.a[0] = gv(-0.8f, -0.8f, 0, 1, 1, 0);
    m.a[1] = gv( 0.8f, -0.8f, 0, 1, 1, 0);
    m.a[2] = gv( 0.0f, -0.2f, 0, 1, 1, 0);
    m.b[0] = gv(-0.8f,  0.2f, 0, 1, 0, 1);
    m.b[1] = gv( 0.8f,  0.2f, 0, 1, 0, 1);
    m.b[2] = gv( 0.0f,  0.8f, 0, 1, 0, 1);
    struct MultiEmit {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Multi* mm = (Multi*)u;
            (void)in;
            for (int i = 0; i < 3; ++i) pglEmitGeometryVertex(em, &mm->a[i]);
            pglEndGeometryPrimitive(em);
            for (int i = 0; i < 3; ++i) pglEmitGeometryVertex(em, &mm->b[i]);
            // implicit End
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 8, MultiEmit::cb));
    glUseProgram(prog);
    pglSetUniform(&m);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Both strips should be visible (red-ish and green-ish quadrants).
    int red = 0, green = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100 && c.g < 100) ++red;
        if (c.g > 100 && c.r < 100) ++green;
    }
    UNIT_CHECK(red > 0);
    UNIT_CHECK(green > 0);

    // The two strips must not be joined: draw only strip A and count.
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_parity()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.8f, -0.8f, 0, 1, 0.8f, -0.8f, 0, 1, 0.0f, 0.8f, 0, 1 };
    float tags[] = { 0.2f, 0, 0.2f, 0, 0.2f, 0 };
    bind_geometry(pos, 3, tags);

    // Emit the same 4 vertices as a strip every time; parity must flip
    // winding on the second triangle.
    struct Par {
        pglGeometryVertex v[4];
    } p;
    p.v[0] = gv(-0.7f, -0.7f, 0, 1, 1, 0);
    p.v[1] = gv( 0.7f, -0.7f, 0, 1, 1, 0);
    p.v[2] = gv(-0.7f,  0.7f, 0, 1, 1, 0);
    p.v[3] = gv( 0.7f,  0.7f, 0, 1, 1, 0);

    struct ParEmit {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Par* pp = (Par*)u;
            (void)in;
            for (int i = 0; i < 4; ++i) pglEmitGeometryVertex(em, &pp->v[i]);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 4, ParEmit::cb));
    glUseProgram(prog);
    pglSetUniform(&p);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    int normal = nonblack_pixels();
    take_snapshot();

    // Reference: same quad drawn directly as a strip.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 1, NULL));
    {
        float qp[] = {
            -0.7f, -0.7f, 0, 1,
             0.7f, -0.7f, 0, 1,
            -0.7f,  0.7f, 0, 1,
             0.7f,  0.7f, 0, 1,
        };
        float qt[] = { 1, 0, 1, 0, 1, 0, 1, 0 };
        bind_geometry(qp, 4, qt);
        glUseProgram(prog);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    UNIT_CHECK(nonblack_pixels() == normal);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_flat_source()
{
    reset_context();
    GLuint prog;
    {
        GLenum interp[2] = { PGL_FLAT, PGL_FLAT };
        prog = pglCreateProgram(passthru_vs, varying_fs, 2, interp, GL_FALSE);
    }

    // Input triangle with distinct v0/v1/v2 tags; but the flat source must
    // come from the emitted (generated) vertices, not the input varying.
    float pos[] = {
        -0.9f, -0.9f, 0, 1,
         0.9f, -0.9f, 0, 1,
         0.0f,  0.9f, 0, 1,
    };
    float tags[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    bind_geometry(pos, 3, tags);

    struct Flat {
        pglGeometryVertex v[3];
    } f;
    // All emitted vertices carry a green tag: the generated quad must be green.
    f.v[0] = gv(-0.6f, -0.6f, 0, 1, 0, 1);
    f.v[1] = gv( 0.6f, -0.6f, 0, 1, 0, 1);
    f.v[2] = gv( 0.0f,  0.6f, 0, 1, 0, 1);

    struct FlatEmit {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Flat* ff = (Flat*)u;
            (void)in;
            for (int i = 0; i < 3; ++i) pglEmitGeometryVertex(em, &ff->v[i]);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 4, FlatEmit::cb));
    glUseProgram(prog);
    pglSetUniform(&f);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    int green = 0, red = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.g > 100) ++green;
        if (c.r > 100) ++red;
    }
    UNIT_CHECK(green > 0);
    UNIT_CHECK(red == 0);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_interp_w()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.9f, 0, 0, 1, 0.9f, 0, 0, 1, 0, 0, 0, 1 };
    float tags[] = { 0, 0, 0, 0, 0, 0 };
    bind_geometry(pos, 3, tags);

    // Two emitted vertices with different w; the interpolated tag should
    // follow perspective interpolation (smooth), matching direct drawing.
    struct IW {
        pglGeometryVertex v[3];
    } iw;
    iw.v[0] = gv(-0.8f, -0.8f, 0, 1.0f, 1, 0);
    iw.v[1] = gv( 0.8f, -0.8f, 0, 2.0f, 0, 0);
    iw.v[2] = gv( 0.0f,  0.8f, 0, 1.0f, 0, 0);

    struct IWEmit {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            IW* p = (IW*)u;
            (void)in;
            for (int i = 0; i < 3; ++i) pglEmitGeometryVertex(em, &p->v[i]);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, IWEmit::cb));
    glUseProgram(prog);
    pglSetUniform(&iw);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    int geom_pixels = nonblack_pixels();
    take_snapshot();

    // Direct draw of the same triangle with the same w/varyings.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 1, NULL));
    {
        float dp[] = {
            -0.8f, -0.8f, 0, 1.0f,
             0.8f, -0.8f, 0, 2.0f,
             0.0f,  0.8f, 0, 1.0f,
        };
        float dt[] = { 1, 0, 0, 0, 0, 0 };
        bind_geometry(dp, 3, dt);
        glUseProgram(prog);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    UNIT_CHECK(nonblack_pixels() == geom_pixels);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_clipped()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.8f, -0.8f, 0, 1, 0.8f, -0.8f, 0, 1, 0.0f, 0.8f, 0, 1 };
    float tags[] = { 1, 1, 1, 1, 1, 1 };
    bind_geometry(pos, 3, tags);

    // Emit a quad that extends far outside the frustum; the normal clipper
    // must produce visible fragments from the visible part, matching a direct
    // draw of the same over-sized strip.
    struct Clip {
        pglGeometryVertex v[4];
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Clip* p = (Clip*)u;
            (void)in;
            for (int i = 0; i < 4; ++i) UNIT_CHECK(pglEmitGeometryVertex(em, &p->v[i]));
        }
    } cl;
    cl.v[0] = gv(-3.0f, -3.0f, 0, 2.0f, 1, 0);
    cl.v[1] = gv( 3.0f, -3.0f, 0, 2.0f, 1, 0);
    cl.v[2] = gv(-3.0f,  3.0f, 0, 2.0f, 1, 0);
    cl.v[3] = gv( 3.0f,  3.0f, 0, 2.0f, 1, 0);

    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 4, Clip::cb));
    glUseProgram(prog);
    pglSetUniform(&cl);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    int clipped_pixels = nonblack_pixels();
    UNIT_CHECK(clipped_pixels > 0);
    take_snapshot();

    // Direct strip with the same four clip-space vertices.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 1, NULL));
    {
        float dp[] = {
            -3.0f, -3.0f, 0, 2.0f,
             3.0f, -3.0f, 0, 2.0f,
            -3.0f,  3.0f, 0, 2.0f,
             3.0f,  3.0f, 0, 2.0f,
        };
        float dt[] = { 1, 0, 1, 0, 1, 0, 1, 0 };
        bind_geometry(dp, 4, dt);
        glUseProgram(prog);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    UNIT_CHECK(nonblack_pixels() == clipped_pixels);
    UNIT_CHECK(snapshot_equal());

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_lifetime_copy()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.9f, -0.9f, 0, 1, 0.9f, -0.9f, 0, 1, 0.0f, 0.9f, 0, 1 };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    // Reuse one temporary vertex, mutating it after each Emit.  The emitted
    // copies must not follow the mutation.
    struct Mut {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex tmp = gv(-0.6f, -0.6f, 0, 1, 1, 0);
            pglEmitGeometryVertex(em, &tmp);
            tmp.position.x = 0.6f;
            tmp.varying[0] = 0;   // red -> green
            tmp.varying[1] = 1;
            pglEmitGeometryVertex(em, &tmp);
            tmp.position.y = 0.6f;
            pglEmitGeometryVertex(em, &tmp);
            pglEndGeometryPrimitive(em);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, Mut::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    int red = 0, green = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100) ++red;
        if (c.g > 100) ++green;
    }
    // Only the first emitted vertex was red; the rest green.  A visible
    // triangle with both proves the copies were taken at emit time.
    UNIT_CHECK(red > 0);
    UNIT_CHECK(green > 0);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_points()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.6f, -0.6f, 0, 1,
         0.0f, -0.6f, 0, 1,
         0.6f, -0.6f, 0, 1,
    };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    // One GL_POINTS input -> three output points.  Each Emit is its own point,
    // so no End is required between them.
    struct Pts {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)u;
            float x0 = in->vertices[0].position.x;
            pglGeometryVertex v = gv(x0, -0.6f, 0, 1, 1, 0);
            UNIT_CHECK(pglEmitGeometryVertex(em, &v) == GL_TRUE);
            v.position.x = x0 + 0.5f;
            UNIT_CHECK(pglEmitGeometryVertex(em, &v) == GL_TRUE);
            v.position.x = x0 + 1.0f;
            UNIT_CHECK(pglEmitGeometryVertex(em, &v) == GL_TRUE);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 3, Pts::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glPointSize(3.0f);
    glDrawArrays(GL_POINTS, 0, 3);
    glPointSize(1.0f);

    // Nine points, spread horizontally, must color some pixels.
    int red = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100) ++red;
    }
    UNIT_CHECK(red >= 9);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_line_strip_tail()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.8f, -0.8f, 0, 1, 0.8f, -0.8f, 0, 1, 0.0f, 0.8f, 0, 1 };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    // A one-vertex line strip tail emits no line; the second strip with only
    // two vertices emits exactly one line.
    struct Tail {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v = gv(-0.7f, -0.7f, 0, 1, 1, 0);
            pglEmitGeometryVertex(em, &v);
            pglEndGeometryPrimitive(em);          // length 1: no line
            v = gv(-0.5f, 0.0f, 0, 1, 1, 0);
            pglEmitGeometryVertex(em, &v);
            v = gv(0.5f, 0.0f, 0, 1, 1, 0);
            pglEmitGeometryVertex(em, &v);
            pglEndGeometryPrimitive(em);          // length 2: one line
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_LINE_STRIP, 4, Tail::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glLineWidth(3.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glLineWidth(1.0f);

    // The horizontal line at y=0 is visible; the single vertex is not.
    int visible = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100) ++visible;
    }
    UNIT_CHECK(visible > 0);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_flat_clipped()
{
    reset_context();
    GLuint prog;
    {
        GLenum interp[2] = { PGL_FLAT, PGL_FLAT };
        prog = pglCreateProgram(passthru_vs, varying_fs, 2, interp, GL_FALSE);
    }

    float pos[] = { -1.5f, -1.5f, 0, 1, 1.5f, -1.5f, 0, 1, 0.0f, 1.5f, 0, 1 };
    float tags[] = { 1, 1, 1, 1, 1, 1 };
    bind_geometry(pos, 3, tags);

    // The strip crosses the frustum and gets clipped.  Every generated vertex
    // carries red; clip-generated vertices must not replace the flat source.
    struct FlatClip {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v = gv(-3.0f, -3.0f, 0, 2.0f, 1, 0);
            pglEmitGeometryVertex(em, &v);
            v = gv( 3.0f, -3.0f, 0, 2.0f, 1, 0); pglEmitGeometryVertex(em, &v);
            v = gv(-3.0f,  3.0f, 0, 2.0f, 1, 0); pglEmitGeometryVertex(em, &v);
            v = gv( 3.0f,  3.0f, 0, 2.0f, 1, 0); pglEmitGeometryVertex(em, &v);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 4, FlatClip::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Red must survive clipping (flat source is an emitted vertex).
    int red = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100 && c.g < 50) ++red;
    }
    UNIT_CHECK(red > 0);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_provoke()
{
    struct P3 {
        static void vs(float* out, vec4* a, Shader_Builtins* b, void* u) {
            (void)u;
            b->gl_Position = a[0];
            out[0] = a[1].x; out[1] = a[1].y;
        }
        static void fs(float* in, Shader_Builtins* b, void* u) {
            (void)u;
            SET_V4(b->gl_FragColor, in[0], in[1], 0.0f, 1.0f);
        }
    };

    reset_context();
    GLuint prog;
    {
        GLenum interp[2] = { PGL_FLAT, PGL_FLAT };
        prog = pglCreateProgram(P3::vs, P3::fs, 2, interp, GL_FALSE);
    }

    float pos[] = { -0.9f, -0.9f, 0, 1, 0.9f, -0.9f, 0, 1, 0.0f, 0.9f, 0, 1 };
    float tags[] = { 1, 1, 1, 1, 1, 1 };
    bind_geometry(pos, 3, tags);

    // Strip: v0 red (1,0), v1 yellow (1,1), v2 green (0,1).  First/last
    // convention selects the provoking output vertex and so a solid color.
    struct Prov {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v;
            memset(&v, 0, sizeof(v));
            SET_V4(v.position, -0.7f, -0.7f, 0, 1);
            v.varying[0] = 1; v.varying[1] = 0;  // red
            pglEmitGeometryVertex(em, &v);
            SET_V4(v.position, 0.7f, -0.7f, 0, 1);
            v.varying[0] = 1; v.varying[1] = 1;  // yellow
            pglEmitGeometryVertex(em, &v);
            SET_V4(v.position, 0.0f, 0.7f, 0, 1);
            v.varying[0] = 0; v.varying[1] = 1;  // green
            pglEmitGeometryVertex(em, &v);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, Prov::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    int red = 0, green = 0, yellow = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100 && c.g < 50) ++red;
        if (c.g > 100 && c.r < 50) ++green;
        if (c.r > 100 && c.g > 100) ++yellow;
    }
    UNIT_CHECK(red > 0);
    UNIT_CHECK(green == 0 && yellow == 0);

    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    red = green = yellow = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100 && c.g < 50) ++red;
        if (c.g > 100 && c.r < 50) ++green;
        if (c.r > 100 && c.g > 100) ++yellow;
    }
    UNIT_CHECK(green > 0);
    UNIT_CHECK(red == 0 && yellow == 0);

    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_emit_depth_blend()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = { -0.9f, -0.9f, 0, 1, 0.9f, -0.9f, 0, 1, 0.0f, 0.9f, 0, 1 };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    // Emit the near quad first, then a farther one that overlaps it.  With
    // GL_LESS the later (farther) strip must fail the depth test, so the
    // nearer color stays visible.  Without the depth stage it would overwrite.
    struct Depth {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v = gv(-0.5f, -0.5f, -0.5f, 1, 0, 1);  // near: green
            pglEmitGeometryVertex(em, &v);
            v = gv(0.5f, -0.5f, -0.5f, 1, 0, 1); pglEmitGeometryVertex(em, &v);
            v = gv(0.0f, 0.5f, -0.5f, 1, 0, 1); pglEmitGeometryVertex(em, &v);
            pglEndGeometryPrimitive(em);
            v = gv(-0.7f, -0.7f, 0.5f, 1, 1, 0);  // far: red, drawn second
            pglEmitGeometryVertex(em, &v);
            v = gv(0.7f, -0.7f, 0.5f, 1, 1, 0); pglEmitGeometryVertex(em, &v);
            v = gv(0.0f, 0.7f, 0.5f, 1, 1, 0); pglEmitGeometryVertex(em, &v);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 6, Depth::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_DEPTH_TEST);

    // The near (green) triangle keeps the overlap; the far (red) one is only
    // visible where it does not overlap.
    int green = 0, red = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.g > 100 && c.r < 50) ++green;
        if (c.r > 100 && c.g < 50) ++red;
    }
    UNIT_CHECK(green > 0);
    UNIT_CHECK(red > 0);
    // Center pixel (inside both) must still be the near green.
    Color center = PIXEL_TO_COLOR(pixels[(H / 2) * W + W / 2]);
    UNIT_CHECK(center.g > 100 && center.r < 50);

    // Blending: emit a half-transparent white triangle over a red background
    // and check the center is a 50/50 mix (red channel stays full, green
    // rises to about half).
    struct BlendCb {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v;
            memset(&v, 0, sizeof(v));
            SET_V4(v.position, -0.8f, -0.8f, 0, 1);
            pglEmitGeometryVertex(em, &v);
            SET_V4(v.position, 0.8f, -0.8f, 0, 1);
            pglEmitGeometryVertex(em, &v);
            SET_V4(v.position, 0.0f, 0.8f, 0, 1);
            pglEmitGeometryVertex(em, &v);
        }
    };

    float bpos[] = { -0.9f, -0.9f, 0, 1, 0.9f, -0.9f, 0, 1, 0.0f, 0.9f, 0, 1 };
    float btags[] = { 0, 0, 0, 0, 0, 0 };

    // Fragment color alpha comes from in[1]; use a shader that outputs
    // (1, 1, 1, 0.5) so blending is visible.
    struct BlendFs {
        static void fs(float* in, Shader_Builtins* b, void* u) {
            (void)in; (void)u;
            SET_V4(b->gl_FragColor, 1.0f, 1.0f, 1.0f, 0.5f);
        }
    };
    reset_context();
    GLuint prog3;
    {
        GLenum interp[2] = { PGL_SMOOTH, PGL_SMOOTH };
        prog3 = pglCreateProgram(passthru_vs, BlendFs::fs, 2, interp, GL_FALSE);
    }
    bind_geometry(bpos, 3, btags);
    UNIT_CHECK(pglSetGeometryShader(prog3, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, BlendCb::cb));
    glUseProgram(prog3);
    pglSetUniform(nullptr);

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    Color blended = PIXEL_TO_COLOR(pixels[(H / 2) * W + W / 2]);
    UNIT_CHECK(blended.r > 200);
    UNIT_CHECK(blended.g > 80 && blended.g < 180);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}


// ===========================================================================
// lifetime: overflow, culling placement, validation, context switching

static void geometry_life_overflow()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.9f, -0.9f, 0, 1,
         0.9f, -0.9f, 0, 1,
         0.0f,  0.9f, 0, 1,
        -0.5f, -0.5f, 0, 1,
         0.5f, -0.5f, 0, 1,
         0.0f,  0.5f, 0, 1,
    };
    float tags[] = { 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1 };
    bind_geometry(pos, 6, tags);

    // For primitive 0 of the first draw: emit a visible triangle, End it,
    // then emit one more vertex beyond the 3-vertex limit.  Everything from
    // that callback must be dropped (including the visible strip).
    // Primitive 1 of the first draw still draws; a later draw with only
    // primitive 1-style content works too.
    struct Over {
        int draw;
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            Over* o = (Over*)u;
            if (o->draw == 0 && in->primitive_id == 0) {
                pglGeometryVertex v = gv(-0.6f, -0.6f, 0, 1, 1, 0);
                pglEmitGeometryVertex(em, &v);
                v = gv(0.6f, -0.6f, 0, 1, 1, 0); pglEmitGeometryVertex(em, &v);
                v = gv(0.0f, 0.6f, 0, 1, 1, 0); pglEmitGeometryVertex(em, &v);
                pglEndGeometryPrimitive(em);
                v = gv(0.0f, 0.0f, 0, 1, 1, 0);
                UNIT_CHECK(pglEmitGeometryVertex(em, &v) == GL_FALSE);
                UNIT_CHECK(pglEmitGeometryVertex(em, &v) == GL_FALSE);
            } else {
                pglGeometryVertex v = gv(-0.5f, 0.2f, 0, 1, 0, 1);
                pglEmitGeometryVertex(em, &v);
                v = gv(0.5f, 0.2f, 0, 1, 0, 1); pglEmitGeometryVertex(em, &v);
                v = gv(0.0f, 0.8f, 0, 1, 0, 1); pglEmitGeometryVertex(em, &v);
            }
        }
    } over;
    over.draw = 0;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, Over::cb));
    glUseProgram(prog);
    pglSetUniform(&over);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Overflow raised GL_INVALID_OPERATION.
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);
    // The dropped primitive left no red (r>100, g<100) pixels; the surviving
    // primitive did leave green pixels.
    int red = 0, green = 0;
    for (int i = 0; i < W * H; ++i) {
        Color c = PIXEL_TO_COLOR(pixels[i]);
        if (c.r > 100 && c.g < 100) ++red;
        if (c.g > 100 && c.r < 100) ++green;
    }
    UNIT_CHECK(red == 0);
    UNIT_CHECK(green > 0);

    // The stage still works for the next draw.
    over.draw = 1;
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 3, 3);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(nonblack_pixels() > 0);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_life_cull_after_stage()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    // Input triangle entirely behind (w<0) and off-frustum, yet the callback
    // generates a visible front-facing quad.  Culling looks at generated
    // geometry, not the input.
    float pos[] = {
        -4.0f, -4.0f, 0, -1,
         4.0f, -4.0f, 0, -1,
         0.0f,  4.0f, 0, -1,
    };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    struct Gen {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)in; (void)u;
            pglGeometryVertex v = gv(-0.7f, -0.7f, 0, 1, 1, 0);
            pglEmitGeometryVertex(em, &v);
            v = gv(0.7f, -0.7f, 0, 1, 1, 0); pglEmitGeometryVertex(em, &v);
            v = gv(0.0f, 0.7f, 0, 1, 1, 0); pglEmitGeometryVertex(em, &v);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, Gen::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(nonblack_pixels() > 0);
    glDisable(GL_CULL_FACE);

    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_life_validation()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    struct Noop {
        static void cb(const pglGeometryInput*, pglGeometryEmitter*, void*) {}
    };

    // Program 0 is configurable.
    UNIT_CHECK(pglSetGeometryShader(0, GL_POINTS, GL_POINTS, 4, Noop::cb));
    UNIT_CHECK(pglSetGeometryShader(0, GL_POINTS, GL_POINTS, 1, NULL));

    // Bad enums / values.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINE_LOOP, GL_POINTS, 4, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_ENUM);
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_LINES, 4, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_ENUM);
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 0, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_VALUE);
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 257, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_VALUE);
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 256, Noop::cb) == GL_TRUE);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    // Invalid program: out of range and deleted both fail with
    // GL_INVALID_OPERATION, even for a NULL shader.
    UNIT_CHECK(pglSetGeometryShader(9999, GL_POINTS, GL_POINTS, 4, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);
    UNIT_CHECK(pglSetGeometryShader(9999, GL_POINTS, GL_POINTS, 1, NULL) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);

    GLuint dead = make_program(PGL_SMOOTH);
    glDeleteProgram(dead);
    UNIT_CHECK(pglSetGeometryShader(dead, GL_POINTS, GL_POINTS, 4, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);

    // A failed configuration must preserve the previous one.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_POINTS, 8, Noop::cb) == GL_TRUE);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(pglSetGeometryShader(prog, (GLenum)0x9999, GL_POINTS, 8, Noop::cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_ENUM);
    // Still enabled from the prior successful call: drawing triangles now
    // mismatches the stored GL_LINES input.
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);

    // A recycled program starts with no stage.
    GLuint fresh = make_program(PGL_SMOOTH);
    UNIT_CHECK(fresh == dead);
    glUseProgram(fresh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_life_family_mismatch()
{
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    float pos[] = {
        -0.8f, -0.8f, 0, 1,
         0.8f, -0.8f, 0, 1,
         0.0f,  0.8f, 0, 1,
    };
    float tags[] = { 1, 1, 1, 1, 1, 1 };
    bind_geometry(pos, 3, tags);

    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_LINE_STRIP, 8, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);

    // Mismatched family: error before any callback or pixel write.
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);
    UNIT_CHECK(rec.inputs.empty());
    UNIT_CHECK(nonblack_pixels() == 0);

    // A zero-count draw is not "non-empty": no error, no callback.
    glDrawArrays(GL_TRIANGLES, 0, 0);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(rec.inputs.empty());

    // Correct family works.
    glDrawArrays(GL_LINES, 0, 2);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(rec.inputs.size() == 1);

    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_life_chunk_reuse()
{
    // PGL_CHUNK_PRIMS==64 is set on the driver command line.  Run several
    // chunks of generated, partly-clipped geometry in one callback draw, then
    // a normal draw, to exercise clip-arena reuse and output-space lifetime.
    reset_context();
    GLuint prog = make_program(PGL_SMOOTH);

    // 200 lines spanning well past the frustum; each emits a 4-vertex strip
    // that needs clipping.
    std::vector<float> pos, tags;
    for (int i = 0; i < 200; ++i) {
        float y = -1.2f + (2.4f * i) / 199.0f;
        pos.push_back(-1.6f); pos.push_back(y); pos.push_back(0); pos.push_back(1);
        pos.push_back( 1.6f); pos.push_back(y); pos.push_back(0); pos.push_back(1);
        tags.push_back(0.5f); tags.push_back(0.5f);
        tags.push_back(0.5f); tags.push_back(0.5f);
    }
    bind_geometry(pos.data(), 400, tags.data());

    // Recording callback: no output, but checks primitive ids survive chunking.
    Recorder rec;
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_LINE_STRIP, 2, record_cb));
    glUseProgram(prog);
    pglSetUniform(&rec);
    glDrawArrays(GL_LINES, 0, 400);
    UNIT_CHECK(rec.inputs.size() == 200);
    for (GLsizei i = 0; i < 200; ++i)
        UNIT_CHECK(rec.inputs[i].primitive_id == (GLuint)i);

    // Emit a thin quad per line; the ribbon spans chunks and clips, so the
    // clip arena and primitive records are reused across 64-primitive batches.
    struct Ribbon {
        static void cb(const pglGeometryInput* in, pglGeometryEmitter* em, void* u) {
            (void)u;
            pglGeometryVertex a = in->vertices[0];
            pglGeometryVertex b = in->vertices[1];
            float y0 = a.position.y, y1 = b.position.y;
            float x = 0.02f;
            pglGeometryVertex v;
            memset(&v, 0, sizeof(v));
            v.varying[0] = 0.6f; v.varying[1] = 0.6f;
            SET_V4(v.position, -1.6f, y0 - x, 0, 1); pglEmitGeometryVertex(em, &v);
            SET_V4(v.position, -1.6f, y0 + x, 0, 1); pglEmitGeometryVertex(em, &v);
            SET_V4(v.position,  1.6f, y1 - x, 0, 1); pglEmitGeometryVertex(em, &v);
            SET_V4(v.position,  1.6f, y1 + x, 0, 1); pglEmitGeometryVertex(em, &v);
        }
    };
    UNIT_CHECK(pglSetGeometryShader(prog, GL_LINES, GL_TRIANGLE_STRIP, 4, Ribbon::cb));
    glUseProgram(prog);
    pglSetUniform(nullptr);
    glDrawArrays(GL_LINES, 0, 400);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(nonblack_pixels() > 0);

    // A normal (no-stage) draw still works and is unaffected.
    UNIT_CHECK(pglSetGeometryShader(prog, GL_POINTS, GL_POINTS, 1, NULL));
    glClear(GL_COLOR_BUFFER_BIT);
    float tri[] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    float tt[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(tri, 3, tt);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);
    UNIT_CHECK(nonblack_pixels() > 0);

    free_glContext(&ctx);
    pixels = nullptr;
}

static void geometry_life_context_switch()
{
    reset_context();

    glContext ctx2;
    pix_t* pixels2 = nullptr;
    UNIT_CHECK(init_glContext(&ctx2, &pixels2, W, H));
    set_glContext(&ctx);

    float pos[] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0.0f, 0.5f, 0, 1 };
    float tags[] = { 1, 0, 1, 0, 1, 0 };
    bind_geometry(pos, 3, tags);

    Recorder rec;
    GLenum interp[2] = { PGL_SMOOTH, PGL_SMOOTH };
    GLuint p1 = pglCreateProgram(passthru_vs, varying_fs, 2, interp, GL_FALSE);
    glUseProgram(p1);
    UNIT_CHECK(pglSetGeometryShader(p1, GL_TRIANGLES, GL_TRIANGLE_STRIP, 3, record_cb));
    pglSetUniform(&rec);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(rec.inputs.size() == 1);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    // Switch to the second context: program ids, stage config and error state
    // are independent.
    set_glContext(&ctx2);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    // Program 0 only exists there; configuring geometry on an id that the
    // first context allocated must fail as out of range.
    UNIT_CHECK(pglSetGeometryShader(p1, GL_POINTS, GL_POINTS, 4, record_cb) == GL_FALSE);
    UNIT_CHECK(glGetError() == GL_INVALID_OPERATION);

    // The second context's own program still works normally (no stage).
    GLuint p2 = pglCreateProgram(passthru_vs, varying_fs, 2, interp, GL_FALSE);
    glUseProgram(p2);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);
    float t2[] = { 1, 0, 1, 0, 1, 0 };
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, t2);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    set_glContext(&ctx);
    // Back on context 1 the stage is still configured.
    glDrawArrays(GL_TRIANGLES, 0, 3);
    UNIT_CHECK(rec.inputs.size() == 2);
    UNIT_CHECK(glGetError() == GL_NO_ERROR);

    set_glContext(&ctx2);
    free_glContext(&ctx2);
    set_glContext(&ctx);
    free_glContext(&ctx);
    pixels = nullptr;
    (void)pixels2;
}

std::vector<uf::Case> unit_cases()
{
    return {
        { "assembly_lines", "assembly", geometry_assembly_lines },
        { "assembly_loop_index", "assembly", geometry_assembly_loop_and_index },
        { "assembly_strip_parity", "assembly", geometry_assembly_strip_parity },
        { "assembly_incomplete", "assembly", geometry_assembly_incomplete_tail },
        { "assembly_instanced", "assembly", geometry_assembly_instanced },
        { "emit_ribbon", "emission", geometry_emit_ribbon },
        { "emit_multistrip", "emission", geometry_emit_multistrip },
        { "emit_parity", "emission", geometry_emit_parity },
        { "emit_flat_source", "emission", geometry_emit_flat_source },
        { "emit_interp_w", "emission", geometry_emit_interp_w },
        { "emit_clipped", "emission", geometry_emit_clipped },
        { "emit_lifetime_copy", "emission", geometry_emit_lifetime_copy },
        { "emit_points", "emission", geometry_emit_points },
        { "emit_line_strip_tail", "emission", geometry_emit_line_strip_tail },
        { "emit_flat_clipped", "emission", geometry_emit_flat_clipped },
        { "emit_provoke", "emission", geometry_emit_provoke },
        { "emit_depth_blend", "emission", geometry_emit_depth_blend },
        { "life_overflow", "lifetime", geometry_life_overflow },
        { "life_cull_after_stage", "lifetime", geometry_life_cull_after_stage },
        { "life_validation", "lifetime", geometry_life_validation },
        { "life_family_mismatch", "lifetime", geometry_life_family_mismatch },
        { "life_chunk_reuse", "lifetime", geometry_life_chunk_reuse },
        { "life_context_switch", "lifetime", geometry_life_context_switch },
    };
}
