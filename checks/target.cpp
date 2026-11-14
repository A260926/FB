// Regression tests for ImDrawList channel split/merge, without any graphics backend.
//
// The oracle is an independent operation tree recorded while driving the public
// ImDrawList/ImDrawListSplitter/draw APIs. It never inspects pre-merge state
// (_Channels, _Splitter, _CmdHeader are off-limits) and never treats the
// pre-merge CmdBuffer as the expected answer. Verification is performed against
// the real, post-merge CmdBuffer/IdxBuffer/VtxBuffer by replaying triangles
// through each command's IdxOffset/VtxOffset, plus an independent CPU
// source-over rasterizer used as a pixel reference.
//
// Exported entry point: int unit_run_group(const char* group_name). No main().

#include "suite.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

//-----------------------------------------------------------------------------
// Small helpers
//-----------------------------------------------------------------------------

static inline ImTextureRef make_tex_ref(int id) { return ImTextureRef((ImTextureID)id); }
static inline unsigned long long tex_id_of(const ImDrawCmd& c) { return (unsigned long long)c.GetTexID(); }
static inline ImVec4 clip4(const ImVec2& a, const ImVec2& b) { return ImVec4(a.x, a.y, b.x, b.y); }
static inline bool clip_equal(const ImVec4& a, const ImVec4& b)
{ return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
static inline bool v2_equal(const ImVec2& a, const ImVec2& b) { return a.x == b.x && a.y == b.y; }

//-----------------------------------------------------------------------------
// Procedural textures (2x2 and 4x4), generated in code so the suite needs no
// font atlas, image files or platform services.
//-----------------------------------------------------------------------------

struct ProcTexture
{
    int             W, H;
    ImU32           Px[16];
};

static const int kTexCount = 4;
static ProcTexture g_tex[kTexCount];

static ImU32 tex_pixel(int tex, int x, int y)
{
    const ProcTexture& t = g_tex[tex - 1];
    if (x < 0) x = 0; if (x >= t.W) x = t.W - 1;
    if (y < 0) y = 0; if (y >= t.H) y = t.H - 1;
    return t.Px[y * t.W + x];
}

static void init_textures()
{
    // Texture 1: 2x2, four distinct opaque colors (asymmetric layout).
    g_tex[0].W = g_tex[0].H = 2;
    g_tex[0].Px[0] = IM_COL32(250,  10,  20, 255);
    g_tex[0].Px[1] = IM_COL32( 20, 240,  30, 255);
    g_tex[0].Px[2] = IM_COL32( 30,  40, 235, 255);
    g_tex[0].Px[3] = IM_COL32(240, 230,  10, 255);

    // Texture 2: 4x4 gradient-ish, every texel unique.
    g_tex[1].W = g_tex[1].H = 4;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            g_tex[1].Px[y * 4 + x] = IM_COL32(40 + x * 50, 60 + y * 45, 120 + x * 17 + y * 13, 255);

    // Texture 3: 2x2 with semi-transparent texels.
    g_tex[2].W = g_tex[2].H = 2;
    g_tex[2].Px[0] = IM_COL32(200,  50,  50, 160);
    g_tex[2].Px[1] = IM_COL32( 50, 200,  50, 200);
    g_tex[2].Px[2] = IM_COL32( 50,  50, 200, 120);
    g_tex[2].Px[3] = IM_COL32(220, 220,  60, 230);

    // Texture 4: 4x4 mostly opaque, used by the large-mesh markers is not needed;
    // keep a fourth distinct texture for boundary scenarios.
    g_tex[3].W = g_tex[3].H = 4;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            g_tex[3].Px[y * 4 + x] = IM_COL32(10 + y * 60, 200 - x * 30, 90 + x * 25 + y * 11, 255);
}

static ImU32 sample_texture(int tex, float u, float v)
{
    const ProcTexture& t = g_tex[tex - 1];
    int x = (int)(u * t.W);
    int y = (int)(v * t.H);
    return tex_pixel(tex, x, y);
}

//-----------------------------------------------------------------------------
// Draw callback: records an ordered token and never touches the draw list.
//-----------------------------------------------------------------------------

static std::vector<intptr_t> g_cb_tokens;
static void record_callback(const ImDrawList*, const ImDrawCmd* cmd)
{
    g_cb_tokens.push_back((intptr_t)cmd->UserCallbackData);
}

//-----------------------------------------------------------------------------
// Independent input operation tree.
//
// A ModelGroup mirrors one splitter lifetime: one vector of items per channel.
// The root group has a single channel and holds top-level items; every
// Split()/Merge() cycle becomes a subgroup item (nested splitters use a
// separate ImDrawListSplitter instance, exactly as the library requires).
// Flattening walks channels in numeric order and expands subgroups the same
// way, so temporal call order can never be mistaken for final draw order.
//-----------------------------------------------------------------------------

enum ModelItemKind { MODEL_RECT, MODEL_CALLBACK, MODEL_GROUP };
enum ModelRectKind { RECT_FLAT, RECT_MULTICOLOR, RECT_IMAGE };

struct ModelRect
{
    ModelRectKind Kind;
    int           Tex;
    ImVec4        Clip;
    ImVec2        PMin, PMax;
    ImVec2        UVMin, UVMax;
    ImU32         Col[4];   // vertex order mirrors PrimRect/PrimRectUV
};

struct ModelItem
{
    ModelItemKind Kind;
    ModelRect     Rect;     // Kind == MODEL_RECT
    intptr_t      Token;    // Kind == MODEL_CALLBACK
    int           SubGroup; // Kind == MODEL_GROUP, index into pool
};

struct ModelGroup
{
    std::vector<std::vector<ModelItem> > Channels;
};

struct ExpectedVert
{
    ImVec2 Pos;
    ImVec2 UV;
    ImU32  Col;
};

struct TriEvent
{
    bool          IsCallback;
    bool          UseTex;     // image quad samples a texture; flat/multicolor use the white pixel
    intptr_t      Token;      // callback
    int           Tex;        // triangle
    ImVec4        Clip;       // triangle
    ExpectedVert  V[3];
};

// Recorder drives a real ImDrawList while mirroring every operation in the tree.
struct Recorder
{
    ImDrawList*                 List;
    ModelGroup                  Root;
    std::vector<int>            StackId;   // -1 = Root, otherwise index into Pool
    std::vector<int>            StackCh;   // current channel within each group
    std::vector<ModelGroup>     Pool;      // stable storage for subgroups
    std::vector<ImVec4>         ClipStack;
    std::vector<int>            TexStack;

    Recorder(ImDrawList* list) : List(list)
    {
        Root.Channels.resize(1);
        StackId.push_back(-1);
        StackCh.push_back(0);
    }

    ModelGroup& cur_group()
    {
        int id = StackId.back();
        return id < 0 ? Root : Pool[id];
    }

    ImVec4 cur_clip() const { return ClipStack.empty() ? ImVec4(0, 0, 0, 0) : ClipStack.back(); }
    int     cur_tex() const  { return TexStack.empty() ? 0 : TexStack.back(); }

    void add_item(const ModelItem& it) { cur_group().Channels[StackCh.back()].push_back(it); }

    void push_clip(const ImVec2& a, const ImVec2& b)
    {
        List->PushClipRect(a, b, false);
        ClipStack.push_back(clip4(a, b));
    }
    void pop_clip()
    {
        List->PopClipRect();
        ClipStack.pop_back();
    }
    void push_tex(int tex)
    {
        List->PushTexture(make_tex_ref(tex));
        TexStack.push_back(tex);
    }
    void pop_tex()
    {
        List->PopTexture();
        TexStack.pop_back();
    }

    ModelItem make_rect_item(ModelRectKind kind, const ImVec2& pmin, const ImVec2& pmax,
                             const ImVec2& uvmin, const ImVec2& uvmax, const ImU32 col[4])
    {
        ModelItem it;
        memset(&it, 0, sizeof(it));
        it.Kind = MODEL_RECT;
        it.Rect.Kind = kind;
        it.Rect.Tex = cur_tex();
        it.Rect.Clip = cur_clip();
        it.Rect.PMin = pmin; it.Rect.PMax = pmax;
        it.Rect.UVMin = uvmin; it.Rect.UVMax = uvmax;
        for (int i = 0; i < 4; i++) it.Rect.Col[i] = col[i];
        return it;
    }
    void rect_filled(const ImVec2& pmin, const ImVec2& pmax, ImU32 col)
    {
        ImU32 c[4] = { col, col, col, col };
        add_item(make_rect_item(RECT_FLAT, pmin, pmax, ImVec2(0, 0), ImVec2(0, 0), c));
        List->AddRectFilled(pmin, pmax, col);
    }
    void rect_multicolor(const ImVec2& pmin, const ImVec2& pmax,
                         ImU32 c0, ImU32 c1, ImU32 c2, ImU32 c3)
    {
        ImU32 c[4] = { c0, c1, c2, c3 };
        add_item(make_rect_item(RECT_MULTICOLOR, pmin, pmax, ImVec2(0, 0), ImVec2(0, 0), c));
        List->AddRectFilledMultiColor(pmin, pmax, c0, c1, c2, c3);
    }
    void image(int tex, const ImVec2& pmin, const ImVec2& pmax, const ImVec2& uvmin, const ImVec2& uvmax, ImU32 tint)
    {
        ImU32 c[4] = { tint, tint, tint, tint };
        ModelItem it = make_rect_item(RECT_IMAGE, pmin, pmax, uvmin, uvmax, c);
        // AddImage binds its own texture argument for the duration of the quad,
        // independently of the texture stack, so the model records that texture.
        it.Rect.Tex = tex;
        add_item(it);
        List->AddImage(make_tex_ref(tex), pmin, pmax, uvmin, uvmax, tint);
    }
    void callback(intptr_t token)
    {
        ModelItem it;
        memset(&it, 0, sizeof(it));
        it.Kind = MODEL_CALLBACK;
        it.Token = token;
        add_item(it);
        List->AddCallback(record_callback, (void*)token);
    }

    int split(ImDrawListSplitter* splitter, int count)
    {
        Pool.push_back(ModelGroup());
        int id = (int)Pool.size() - 1;
        Pool.back().Channels.resize(count);

        ModelItem it;
        memset(&it, 0, sizeof(it));
        it.Kind = MODEL_GROUP;
        it.SubGroup = id;
        add_item(it);

        splitter->Split(List, count);
        StackId.push_back(id);
        StackCh.push_back(0);
        return id;
    }
    void set_channel(ImDrawListSplitter* splitter, int ch)
    {
        splitter->SetCurrentChannel(List, ch);
        StackCh.back() = ch;
    }
    void merge(ImDrawListSplitter* splitter)
    {
        splitter->Merge(List);
        StackId.pop_back();
        StackCh.pop_back();
    }

    void flatten_into(const ModelGroup& g, std::vector<const ModelItem*>& out) const
    {
        for (int ch = 0; ch < (int)g.Channels.size(); ch++)
            for (const ModelItem& it : g.Channels[ch])
            {
                if (it.Kind == MODEL_GROUP)
                    flatten_into(Pool[it.SubGroup], out);
                else
                    out.push_back(&it);
                }
    }
    std::vector<const ModelItem*> flatten() const
    {
        std::vector<const ModelItem*> out;
        flatten_into(Root, out);
        return out;
    }
};

// Emit the two triangles of a model rect using the exact vertex/index ordering
// of ImDrawList::PrimRect / PrimRectUV / AddRectFilledMultiColor.
static void emit_rect_tris(const ModelRect& r, const ImVec2& white_uv,
                           std::vector<TriEvent>& out)
{
    ImVec2 uv0 = r.Kind == RECT_IMAGE ? r.UVMin : white_uv;
    ImVec2 uv1 = r.Kind == RECT_IMAGE ? r.UVMax : white_uv;
    ExpectedVert v[4];
    v[0].Pos = r.PMin;
    v[1].Pos = ImVec2(r.PMax.x, r.PMin.y);
    v[2].Pos = r.PMax;
    v[3].Pos = ImVec2(r.PMin.x, r.PMax.y);
    v[0].UV = uv0;
    v[1].UV = ImVec2(uv1.x, uv0.y);
    v[2].UV = uv1;
    v[3].UV = ImVec2(uv0.x, uv1.y);
    for (int i = 0; i < 4; i++) v[i].Col = r.Col[i];

    static const int kIdx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int t = 0; t < 2; t++)
    {
        TriEvent e;
        e.IsCallback = false;
        e.UseTex = r.Kind == RECT_IMAGE;
        e.Token = 0;
        e.Tex = r.Tex;
        e.Clip = r.Clip;
        for (int k = 0; k < 3; k++) e.V[k] = v[kIdx[t * 3 + k]];
        out.push_back(e);
    }
}

static std::vector<TriEvent> expected_stream(const Recorder& rec)
{
    std::vector<const ModelItem*> items = rec.flatten();
    std::vector<TriEvent> out;
    const ImVec2 white_uv = rec.List->_Data->TexUvWhitePixel;
    for (const ModelItem* it : items)
    {
        if (it->Kind == MODEL_CALLBACK)
        {
            TriEvent e;
            memset(&e, 0, sizeof(e));
            e.IsCallback = true;
            e.Token = it->Token;
            out.push_back(e);
        }
        else
        {
            emit_rect_tris(it->Rect, white_uv, out);
        }
    }
    return out;
}

//-----------------------------------------------------------------------------
// Extraction of the actual post-merge triangle/callback stream.
//-----------------------------------------------------------------------------

struct ActualStream
{
    std::vector<TriEvent> Events;
    int                   DrawCmdCount;
    int                   CallbackCmdCount;
};

static int extract_actual_stream(ImDrawList* list, ActualStream& out)
{
    out.Events.clear();
    out.DrawCmdCount = 0;
    out.CallbackCmdCount = 0;

    const int vtx_size = list->VtxBuffer.Size;
    const int idx_size = list->IdxBuffer.Size;
    unsigned running_idx_offset = 0;

    for (int ci = 0; ci < list->CmdBuffer.Size; ci++)
    {
        const ImDrawCmd& cmd = list->CmdBuffer[ci];

        if (cmd.UserCallback != NULL)
        {
            // A callback is an ordered boundary: ElemCount must be zero and the
            // command must survive merging (it is not a zero-element draw cmd).
            if (cmd.ElemCount != 0)
            {
                std::fprintf(stderr, "index: callback cmd %d has ElemCount=%u\n", ci, cmd.ElemCount);
                return -1;
            }
            // Simulate the renderer invoking the callback in stream order. The
            // test callback only records its token and never touches the list.
            cmd.UserCallback(list, &cmd);
            TriEvent e;
            memset(&e, 0, sizeof(e));
            e.IsCallback = true;
            e.Token = (intptr_t)cmd.UserCallbackData;
            out.Events.push_back(e);
            out.CallbackCmdCount++;
            continue;
        }
        if (cmd.ElemCount == 0)
            continue; // trailing/empty draw command, legal
        out.DrawCmdCount++;

        // IdxOffset bookkeeping: draw commands must tile the index buffer.
        if (cmd.IdxOffset != running_idx_offset)
        {
            std::fprintf(stderr, "index: cmd %d IdxOffset=%u expected %u\n",
                         ci, cmd.IdxOffset, running_idx_offset);
            return -1;
        }
        if ((int)(cmd.IdxOffset + cmd.ElemCount) > idx_size)
        {
            std::fprintf(stderr, "index: cmd %d range %u+%u exceeds IdxBuffer %d\n",
                         ci, cmd.IdxOffset, cmd.ElemCount, idx_size);
            return -1;
        }
        if (cmd.ElemCount % 3 != 0)
        {
            std::fprintf(stderr, "index: cmd %d ElemCount %u not divisible by 3\n", ci, cmd.ElemCount);
            return -1;
        }

        for (unsigned k = 0; k < cmd.ElemCount; k++)
        {
            unsigned raw = list->IdxBuffer[cmd.IdxOffset + k];
            unsigned abs_idx = cmd.VtxOffset + raw;
            if (abs_idx >= (unsigned)vtx_size)
            {
                std::fprintf(stderr, "index: cmd %d VtxOffset=%u raw=%u out of VtxBuffer(%d)\n",
                             ci, cmd.VtxOffset, raw, vtx_size);
                return -1;
            }
        }

        for (unsigned t = 0; t < cmd.ElemCount / 3; t++)
        {
            TriEvent e;
            memset(&e, 0, sizeof(e));
            e.IsCallback = false;
            e.Tex = (int)tex_id_of(cmd);
            e.Clip = cmd.ClipRect;
            const ImVec2 white_uv = list->_Data->TexUvWhitePixel;
            bool any_nonwhite_uv = false;
            for (int k = 0; k < 3; k++)
            {
                unsigned raw = list->IdxBuffer[cmd.IdxOffset + t * 3 + k];
                const ImDrawVert& v = list->VtxBuffer[cmd.VtxOffset + raw];
                e.V[k].Pos = v.pos;
                e.V[k].UV = v.uv;
                e.V[k].Col = v.col;
                if (!v2_equal(v.uv, white_uv)) any_nonwhite_uv = true;
            }
            e.UseTex = any_nonwhite_uv;
            out.Events.push_back(e);
        }
        running_idx_offset = cmd.IdxOffset + cmd.ElemCount;
    }

    if (running_idx_offset != (unsigned)idx_size)
    {
        std::fprintf(stderr, "index: consumed %u indices but IdxBuffer has %d\n",
                     running_idx_offset, idx_size);
        return -1;
    }
    return 0;
}

static int compare_streams(const std::vector<TriEvent>& expect, const std::vector<TriEvent>& actual)
{
    if (expect.size() != actual.size())
    {
        std::fprintf(stderr, "stream: event count mismatch expected=%zu actual=%zu\n",
                     expect.size(), actual.size());
        return -1;
    }
    for (size_t i = 0; i < expect.size(); i++)
    {
        const TriEvent& e = expect[i];
        const TriEvent& a = actual[i];
        if (e.IsCallback != a.IsCallback)
        {
            std::fprintf(stderr, "callback boundary lost or misplaced at event %zu\n", i);
            return -1;
        }
        if (e.IsCallback)
        {
            if (e.Token != a.Token)
            {
                std::fprintf(stderr, "callback token mismatch at event %zu expected=%lld actual=%lld\n",
                             i, (long long)e.Token, (long long)a.Token);
                return -1;
            }
            continue;
        }
        if (e.Tex != a.Tex)
        {
            std::fprintf(stderr, "texture mismatch at tri event %zu expected tex=%d actual tex=%d\n",
                         i, e.Tex, a.Tex);
            return -1;
        }
        if (!clip_equal(e.Clip, a.Clip))
        {
            std::fprintf(stderr, "clip mismatch at tri event %zu expected=(%.1f,%.1f,%.1f,%.1f) actual=(%.1f,%.1f,%.1f,%.1f)\n",
                         i, e.Clip.x, e.Clip.y, e.Clip.z, e.Clip.w,
                         a.Clip.x, a.Clip.y, a.Clip.z, a.Clip.w);
            return -1;
        }
        for (int k = 0; k < 3; k++)
        {
            if (!v2_equal(e.V[k].Pos, a.V[k].Pos))
            {
                std::fprintf(stderr, "position mismatch event %zu vert %d\n", i, k);
                return -1;
            }
            if (!v2_equal(e.V[k].UV, a.V[k].UV))
            {
                std::fprintf(stderr, "uv mismatch event %zu vert %d\n", i, k);
                return -1;
            }
            if (e.V[k].Col != a.V[k].Col)
            {
                std::fprintf(stderr, "color mismatch event %zu vert %d expected=%08x actual=%08x\n",
                             i, k, e.V[k].Col, a.V[k].Col);
                return -1;
            }
        }
    }
    return 0;
}

//-----------------------------------------------------------------------------
// Independent CPU rasterizer (source-over), shared by actual and reference paths.
// Axis-aligned rects arrive as two triangles; pixel centers are tested with
// barycentric coordinates and vertex RGBA/UV are interpolated.
//-----------------------------------------------------------------------------

struct Canvas
{
    int W, H;
    std::vector<float> RGBA; // premultiplied-free float storage, 4 channels per pixel

    void init(int w, int h)
    {
        W = w; H = h;
        RGBA.assign((size_t)w * h * 4, 0.0f);
    }
    float* px(int x, int y) { return &RGBA[((size_t)y * W + x) * 4]; }
};

static inline unsigned char round_byte(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (unsigned char)(v + 0.5f);
}

static inline void col_unpack(ImU32 c, float out[4])
{
    out[0] = (float)((c >> IM_COL32_R_SHIFT) & 0xFF);
    out[1] = (float)((c >> IM_COL32_G_SHIFT) & 0xFF);
    out[2] = (float)((c >> IM_COL32_B_SHIFT) & 0xFF);
    out[3] = (float)((c >> IM_COL32_A_SHIFT) & 0xFF);
}

static bool barycentric(const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c,
                        float& l0, float& l1, float& l2)
{
    float den = (b.y - c.y) * (a.x - c.x) + (b.x - c.x) * (c.y - a.y);
    if (den == 0.0f) return false;
    l0 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / den;
    l1 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / den;
    l2 = 1.0f - l0 - l1;
    const float eps = -1e-6f;
    return l0 >= eps && l1 >= eps && l2 >= eps;
}

static void raster_triangle(Canvas& canvas, const TriEvent& tri)
{
    int minx = (int)floorf(fminf(fminf(tri.V[0].Pos.x, tri.V[1].Pos.x), tri.V[2].Pos.x));
    int maxx = (int)ceilf(fmaxf(fmaxf(tri.V[0].Pos.x, tri.V[1].Pos.x), tri.V[2].Pos.x));
    int miny = (int)floorf(fminf(fminf(tri.V[0].Pos.y, tri.V[1].Pos.y), tri.V[2].Pos.y));
    int maxy = (int)ceilf(fmaxf(fmaxf(tri.V[0].Pos.y, tri.V[1].Pos.y), tri.V[2].Pos.y));
    if (minx < 0) minx = 0; if (maxx > canvas.W) maxx = canvas.W;
    if (miny < 0) miny = 0; if (maxy > canvas.H) maxy = canvas.H;

    for (int y = miny; y < maxy; y++)
        for (int x = minx; x < maxx; x++)
        {
            ImVec2 p((float)x + 0.5f, (float)y + 0.5f);
            float l0, l1, l2;
            if (!barycentric(p, tri.V[0].Pos, tri.V[1].Pos, tri.V[2].Pos, l0, l1, l2))
                continue;
            // Dear ImGui clip rect: min inclusive, max exclusive.
            if (!(p.x >= tri.Clip.x && p.x < tri.Clip.z && p.y >= tri.Clip.y && p.y < tri.Clip.w))
                continue;

            float col[3][4], uv[3][2];
            for (int k = 0; k < 3; k++)
            {
                col_unpack(tri.V[k].Col, col[k]);
                uv[k][0] = tri.V[k].UV.x;
                uv[k][1] = tri.V[k].UV.y;
            }
            float r = l0 * col[0][0] + l1 * col[1][0] + l2 * col[2][0];
            float g = l0 * col[0][1] + l1 * col[1][1] + l2 * col[2][1];
            float b = l0 * col[0][2] + l1 * col[1][2] + l2 * col[2][2];
            float a = l0 * col[0][3] + l1 * col[1][3] + l2 * col[2][3];

            // Image quads sample the procedural texture; every other primitive
            // uses the font-atlas white pixel, which must resolve to white.
            float tr = 255.0f, tg = 255.0f, tb = 255.0f, ta = 255.0f;
            if (tri.UseTex && tri.Tex >= 1 && tri.Tex <= kTexCount)
            {
                float u = l0 * uv[0][0] + l1 * uv[1][0] + l2 * uv[2][0];
                float v = l0 * uv[0][1] + l1 * uv[1][1] + l2 * uv[2][1];
                float trgb[4];
                col_unpack(sample_texture(tri.Tex, u, v), trgb);
                tr = trgb[0]; tg = trgb[1]; tb = trgb[2]; ta = trgb[3];
            }
            // Tint modulation (vertex color) followed by source-over blending.
            float sr = r * tr / 255.0f;
            float sg = g * tg / 255.0f;
            float sb = b * tb / 255.0f;
            float sa = a * ta / (255.0f * 255.0f);

            float* dst = canvas.px(x, y);
            dst[0] = sr * sa + dst[0] * (1.0f - sa);
            dst[1] = sg * sa + dst[1] * (1.0f - sa);
            dst[2] = sb * sa + dst[2] * (1.0f - sa);
            dst[3] = sa * 255.0f + dst[3] * (1.0f - sa);
        }
}

static void raster_stream(Canvas& canvas, const std::vector<TriEvent>& stream)
{
    for (const TriEvent& e : stream)
        if (!e.IsCallback)
            raster_triangle(canvas, e);
}

static int compare_canvas(const Canvas& a, const Canvas& b)
{
    if (a.W != b.W || a.H != b.H) return -1;
    for (size_t i = 0; i < a.RGBA.size(); i++)
        if (round_byte(a.RGBA[i]) != round_byte(b.RGBA[i]))
        {
            size_t pix = i / 4, ch = i % 4;
            std::fprintf(stderr, "pixel mismatch at (%zu,%zu) ch=%zu reference=%u actual=%u\n",
                         pix % a.W, pix / a.W, ch, round_byte(a.RGBA[i]), round_byte(b.RGBA[i]));
            return -1;
        }
    return 0;
}

//-----------------------------------------------------------------------------
// Shared list construction
//-----------------------------------------------------------------------------

static ImDrawListSharedData g_shared;

static void reset_list(ImDrawList& list)
{
    list._ResetForNewFrame();
    list._SetPixelDensity(1.0f); // integer fringe scale: plain rects take the 6/4 fast path
}

//-----------------------------------------------------------------------------
// replay group
//-----------------------------------------------------------------------------

// Scenario 1: cross-channel state relations, empty channel, merge while parked
// on a non-zero channel, primitives appended after merge.
static int replay_cross_channel_states()
{
    g_cb_tokens.clear();
    ImDrawList list(&g_shared);
    reset_list(list);
    Recorder rec(&list);
    ImDrawListSplitter sp;

    // All channels share one clip: this produces boundaries that differ ONLY
    // in texture, which the texture-ignoring splitter mutant must collapse.
    const ImVec2 clip_lo(0, 0), clip_hi(48, 48);
    const ImVec4 clip_full(0, 0, 48, 48);
    const ImVec4 clip_small(0, 0, 40, 40);

    rec.split(&sp, 5);

    // Build out of temporal order on purpose: touch channel 3 first, then 1, 0, 2.
    rec.set_channel(&sp, 3); // remains empty: an empty channel must emit nothing

    rec.set_channel(&sp, 1);
    // Same clip, different texture than channel 0 (texture-only boundary).
    rec.push_clip(clip_lo, clip_hi);
    rec.push_tex(2);
    rec.image(2, ImVec2(20, 2), ImVec2(24, 6), ImVec2(0.1f, 0.2f), ImVec2(0.8f, 0.9f), IM_COL32_WHITE);
    rec.pop_tex();
    rec.pop_clip();

    rec.set_channel(&sp, 0);
    rec.push_clip(clip_lo, clip_hi);
    rec.push_tex(1);
    rec.rect_filled(ImVec2(2, 2), ImVec2(6, 6), IM_COL32(210, 30, 30, 255));
    rec.pop_tex();
    rec.pop_clip();

    rec.set_channel(&sp, 2);
    // Same texture as channel 0, different clip (clip-only boundary).
    rec.push_clip(ImVec2(0, 0), ImVec2(40, 40));
    rec.push_tex(1);
    rec.rect_filled(ImVec2(8, 8), ImVec2(12, 12), IM_COL32(30, 210, 30, 255));
    // Identical header (tex+clip+vtxoffset) to the previous rect, separated by a callback.
    rec.callback(1001);
    rec.rect_filled(ImVec2(13, 8), ImVec2(17, 12), IM_COL32(30, 30, 210, 255));
    rec.pop_tex();
    rec.pop_clip();

    rec.set_channel(&sp, 4);
    // Same clip as channel 0, different texture: another texture-only boundary.
    rec.push_clip(clip_lo, clip_hi);
    rec.push_tex(2);
    rec.image(2, ImVec2(26, 2), ImVec2(30, 6), ImVec2(0.0f, 0.25f), ImVec2(0.75f, 1.0f), IM_COL32(255, 240, 220, 255));
    // Same texture, different clip: a clip boundary within the same channel.
    rec.push_clip(ImVec2(0, 0), ImVec2(48, 32));
    rec.rect_multicolor(ImVec2(30, 20), ImVec2(36, 26),
                        IM_COL32(255, 0, 0, 255), IM_COL32(0, 255, 0, 255),
                        IM_COL32(0, 0, 255, 255), IM_COL32(255, 255, 0, 255));
    rec.pop_clip();
    rec.pop_tex();
    rec.pop_clip();

    // Merge while the current channel is non-zero (4).
    rec.merge(&sp);

    // Keep appending after the merge: the trailing command setup must work.
    rec.push_tex(3);
    rec.push_clip(clip_lo, clip_hi);
    rec.rect_filled(ImVec2(40, 40), ImVec2(44, 44), IM_COL32(123, 45, 67, 255));
    rec.pop_clip();
    rec.pop_tex();

    std::vector<TriEvent> expect = expected_stream(rec);
    ActualStream actual;
    UNIT_CHECK(extract_actual_stream(&list, actual) == 0);
    UNIT_CHECK(compare_streams(expect, actual.Events) == 0);

    // Explicit relation checks on the real post-merge events.
    UNIT_CHECK(actual.CallbackCmdCount == 1);
    // Locate the callback event and prove it is an ordered boundary between two
    // triangle streams with identical texture/clip header fields.
    int cb_index = -1;
    for (size_t i = 0; i < actual.Events.size(); i++)
        if (actual.Events[i].IsCallback) { cb_index = (int)i; break; }
    UNIT_CHECK(cb_index > 0 && cb_index + 1 < (int)actual.Events.size());
    const TriEvent& before_cb = actual.Events[cb_index - 1];
    const TriEvent& after_cb = actual.Events[cb_index + 1];
    UNIT_CHECK(!before_cb.IsCallback && !after_cb.IsCallback);
    UNIT_CHECK(before_cb.Tex == after_cb.Tex);
    UNIT_CHECK(clip_equal(before_cb.Clip, after_cb.Clip));

    // Texture-only boundary: find the first tex-2 triangle and verify the
    // preceding draw event uses a different texture but the same clip.
    int first_tex2 = -1;
    for (size_t i = 0; i < actual.Events.size(); i++)
        if (!actual.Events[i].IsCallback && actual.Events[i].Tex == 2) { first_tex2 = (int)i; break; }
    UNIT_CHECK(first_tex2 > 0);
    for (int i = first_tex2 - 1; i >= 0; i--)
        if (!actual.Events[i].IsCallback)
        {
            UNIT_CHECK(actual.Events[i].Tex != 2);
            UNIT_CHECK(clip_equal(actual.Events[i].Clip, actual.Events[first_tex2].Clip));
            break;
        }

    // Clip-only boundary: a tex-1 triangle carrying the smaller clip exists.
    bool found_clip_small = false, found_clip_full = false;
    for (const TriEvent& e : actual.Events)
    {
        if (e.IsCallback || e.Tex != 1) continue;
        if (clip_equal(e.Clip, clip_small)) found_clip_small = true;
        if (clip_equal(e.Clip, clip_full)) found_clip_full = true;
    }
    UNIT_CHECK(found_clip_small && found_clip_full);

    // Post-merge append is the final triangle event and uses tex 3.
    UNIT_CHECK(!actual.Events.back().IsCallback);
    UNIT_CHECK(actual.Events.back().Tex == 3);

    // Empty channel produced no geometry: total tri count equals the model.
    size_t expect_tris = 0;
    for (const TriEvent& e : expect) if (!e.IsCallback) expect_tris++;
    size_t actual_tris = 0;
    for (const TriEvent& e : actual.Events) if (!e.IsCallback) actual_tris++;
    UNIT_CHECK(expect_tris == actual_tris);

    // The recorded callback really fired in order with the right token.
    UNIT_CHECK(g_cb_tokens.size() == 1 && g_cb_tokens[0] == 1001);
    return 0;
}

// Scenario 2: a separate splitter instance nested inside a channel, plus reuse
// of an outer splitter across several split/merge cycles.
static int replay_nested_and_reuse()
{
    ImDrawList list(&g_shared);
    reset_list(list);
    Recorder rec(&list);
    ImDrawListSplitter outer;
    ImDrawListSplitter inner; // nested splitting needs another instance

    rec.split(&outer, 3);

    rec.set_channel(&outer, 0);
    rec.push_tex(1);
    rec.push_clip(ImVec2(0, 0), ImVec2(64, 64));
    rec.rect_filled(ImVec2(1, 1), ImVec2(4, 4), IM_COL32(11, 22, 33, 255));

    // Nested splitter while parked in outer channel 0: its subtree must be
    // expanded by inner channel number, not by call order. We draw inner 2 first.
    rec.split(&inner, 2);
    rec.set_channel(&inner, 1);
    rec.rect_filled(ImVec2(6, 1), ImVec2(9, 4), IM_COL32(44, 55, 66, 255));
    rec.set_channel(&inner, 0);
    rec.rect_filled(ImVec2(11, 1), ImVec2(14, 4), IM_COL32(77, 88, 99, 255));
    rec.merge(&inner);

    // More geometry in outer channel 0 after the inner merge.
    rec.rect_filled(ImVec2(16, 1), ImVec2(19, 4), IM_COL32(111, 122, 133, 255));

    rec.set_channel(&outer, 1); // empty outer channel
    rec.set_channel(&outer, 2);
    rec.image(2, ImVec2(21, 1), ImVec2(25, 5), ImVec2(0.2f, 0.1f), ImVec2(0.9f, 0.7f), IM_COL32_WHITE);
    rec.pop_clip();
    rec.pop_tex();

    rec.merge(&outer);

    // Reuse the same outer splitter for another cycle.
    rec.split(&outer, 2);
    rec.set_channel(&outer, 1);
    rec.push_tex(3);
    rec.push_clip(ImVec2(0, 0), ImVec2(64, 64));
    rec.rect_filled(ImVec2(1, 8), ImVec2(4, 11), IM_COL32(144, 155, 166, 255));
    rec.set_channel(&outer, 0);
    rec.rect_filled(ImVec2(6, 8), ImVec2(9, 11), IM_COL32(177, 188, 199, 255));
    rec.pop_clip();
    rec.pop_tex();
    rec.merge(&outer);

    std::vector<TriEvent> expect = expected_stream(rec);
    ActualStream actual;
    UNIT_CHECK(extract_actual_stream(&list, actual) == 0);
    UNIT_CHECK(compare_streams(expect, actual.Events) == 0);

    // Explicit inner ordering proof: the inner channel-0 color (77,88,99) must
    // appear before the inner channel-1 color (44,55,66) even though channel 1
    // was drawn first.
    int pos_c0 = -1, pos_c1 = -1;
    for (size_t i = 0; i < actual.Events.size(); i++)
    {
        const TriEvent& e = actual.Events[i];
        if (e.IsCallback) continue;
        if (pos_c0 < 0 && e.V[0].Col == IM_COL32(77, 88, 99, 255)) pos_c0 = (int)i;
        if (pos_c1 < 0 && e.V[0].Col == IM_COL32(44, 55, 66, 255)) pos_c1 = (int)i;
    }
    UNIT_CHECK(pos_c0 >= 0 && pos_c1 >= 0 && pos_c0 < pos_c1);

    return 0;
}

// Scenario 3: reset / fresh draw list must never see stale index content.
static int replay_reset_isolation()
{
    ImDrawList list(&g_shared);
    reset_list(list);
    list.PushTexture(make_tex_ref(1));
    for (int i = 0; i < 50; i++)
        list.AddRectFilled(ImVec2((float)i, 0), ImVec2((float)i + 1, 2), IM_COL32(100, 100, 100, 255));
    UNIT_CHECK(list.IdxBuffer.Size == 50 * 6);
    UNIT_CHECK(list.VtxBuffer.Size == 50 * 4);

    // Reset for a new frame: buffers must logically start empty again.
    reset_list(list);
    UNIT_CHECK(list.IdxBuffer.Size == 0);
    UNIT_CHECK(list.VtxBuffer.Size == 0);
    UNIT_CHECK(list.CmdBuffer.Size == 1);

    list.PushTexture(make_tex_ref(2));
    list.AddRectFilled(ImVec2(3, 3), ImVec2(7, 7), IM_COL32(1, 2, 3, 255));
    UNIT_CHECK(list.IdxBuffer.Size == 6);
    UNIT_CHECK(list.VtxBuffer.Size == 4);
    for (int k = 0; k < 6; k++)
    {
        unsigned raw = list.IdxBuffer[k];
        UNIT_CHECK(raw < 4); // indices can only address the four new vertices
    }
    const ImDrawCmd& cmd = list.CmdBuffer.back();
    UNIT_CHECK(cmd.IdxOffset == 0 && cmd.VtxOffset == 0);
    UNIT_CHECK(tex_id_of(cmd) == 2);
    list.PopTexture();

    // A brand new draw list sharing the same shared data starts independent.
    ImDrawList fresh(&g_shared);
    reset_list(fresh);
    UNIT_CHECK(fresh.IdxBuffer.Size == 0);
    UNIT_CHECK(fresh.VtxBuffer.Size == 0);
    fresh.PushTexture(make_tex_ref(4));
    fresh.AddRectFilled(ImVec2(0, 0), ImVec2(2, 2), IM_COL32(9, 8, 7, 255));
    UNIT_CHECK(fresh.IdxBuffer.Size == 6);
    for (int k = 0; k < 6; k++) UNIT_CHECK(fresh.IdxBuffer[k] < 4);
    fresh.PopTexture();
    return 0;
}

// Scenario 4: pixel-level source-over reference on a small canvas, including
// overlapping translucent rects, images with asymmetric UV, clips and channels.
static int replay_pixels()
{
    g_cb_tokens.clear();
    const int W = 32, H = 32;
    ImDrawList list(&g_shared);
    reset_list(list);
    Recorder rec(&list);
    ImDrawListSplitter sp;
    rec.split(&sp, 3);

    // Channel 0: translucent overlapping rects.
    rec.set_channel(&sp, 0);
    rec.push_clip(ImVec2(0, 0), ImVec2(32, 32));
    rec.push_tex(1);
    rec.rect_filled(ImVec2(2, 2), ImVec2(14, 14), IM_COL32(255, 0, 0, 128));
    rec.rect_filled(ImVec2(8, 6), ImVec2(20, 18), IM_COL32(0, 255, 0, 160));
    rec.pop_tex();

    // Channel 1: image sampled with asymmetric UV, clipped to a sub-rect.
    rec.set_channel(&sp, 1);
    rec.push_clip(ImVec2(4, 4), ImVec2(28, 24));
    rec.push_tex(3);
    rec.image(3, ImVec2(5, 5), ImVec2(18, 17), ImVec2(0.25f, 0.1f), ImVec2(0.9f, 0.85f), IM_COL32(255, 255, 255, 220));
    rec.pop_tex();
    rec.pop_clip();

    // Channel 2: a multicolor rect overlapping everything, and a callback boundary.
    rec.set_channel(&sp, 2);
    rec.push_clip(ImVec2(0, 0), ImVec2(32, 32));
    rec.push_tex(1);
    rec.callback(2002);
    rec.rect_multicolor(ImVec2(10, 10), ImVec2(26, 28),
                        IM_COL32(255, 255, 255, 100), IM_COL32(255, 0, 255, 100),
                        IM_COL32(0, 255, 255, 100), IM_COL32(255, 255, 0, 100));
    rec.pop_tex();
    rec.pop_clip();

    rec.merge(&sp);
    rec.pop_clip(); // balance channel 0's push (stacks are shared across channels)

    std::vector<TriEvent> expect = expected_stream(rec);
    ActualStream actual;
    UNIT_CHECK(extract_actual_stream(&list, actual) == 0);
    UNIT_CHECK(compare_streams(expect, actual.Events) == 0);
    UNIT_CHECK(g_cb_tokens.size() == 1 && g_cb_tokens[0] == 2002);

    Canvas ref_canvas, actual_canvas;
    ref_canvas.init(W, H);
    actual_canvas.init(W, H);
    raster_stream(ref_canvas, expect);
    raster_stream(actual_canvas, actual.Events);
    UNIT_CHECK(compare_canvas(ref_canvas, actual_canvas) == 0);
    return 0;
}

static int run_replay()
{
    g_cb_tokens.clear();
    if (replay_cross_channel_states() < 0) return -1;
    if (replay_nested_and_reuse() < 0)     return -1;
    if (replay_reset_isolation() < 0)      return -1;
    if (replay_pixels() < 0)               return -1;
    return 0;
}

//-----------------------------------------------------------------------------
// large_mesh group
//-----------------------------------------------------------------------------

struct LargeRect
{
    ImVec2 PMin, PMax;
    ImU32  Col;
    bool   Marker;
};

static int run_large_mesh()
{
    ImDrawList list(&g_shared);
    reset_list(list);
    list.Flags |= ImDrawFlags_UseVtxOffset;

    const int N0 = 17000; // 68000 vertices: crosses 65536 inside channel 0
    const int N2 = 16500; // 66000 more vertices, further segments

    // Independent per-channel reference records.
    std::vector<LargeRect> ref_ch0, ref_ch1, ref_ch2;
    ImDrawListSplitter sp;
    sp.Split(&list, 3);

    sp.SetCurrentChannel(&list, 0);
    list.PushTexture(make_tex_ref(1));
    list.PushClipRect(ImVec2(0, 0), ImVec2(300, 300), false);
    for (int i = 0; i < N0; i++)
    {
        int gx = i % 100, gy = i / 100;
        ImVec2 p0((float)gx, (float)(16 + gy));
        ImVec2 p1(p0.x + 1.0f, p0.y + 1.0f);
        bool marker = (i == 100 || i == 16500 || i == 16999);
        ImU32 col = marker ? IM_COL32(200 + i % 50, 10 + i % 90, 5 + i % 70, 255)
                           : IM_COL32(60, 70, 80, 255);
        list.AddRectFilled(p0, p1, col);
        ref_ch0.push_back(LargeRect{ p0, p1, col, marker });
    }

    sp.SetCurrentChannel(&list, 1);
    // Marker rects live in a small region so a tiny pixel canvas can cover them.
    // A dedicated clip also proves the markers survive with their own header.
    list.PushClipRect(ImVec2(0, 0), ImVec2(16, 8), false);
    const ImVec2 m1p0(1, 1), m1p1(7, 5);
    const ImVec2 m2p0(9, 2), m2p1(15, 6);
    const ImU32 m1c = IM_COL32(12, 200, 240, 255), m2c = IM_COL32(240, 120, 12, 255);
    list.AddRectFilled(m1p0, m1p1, m1c);
    list.AddRectFilled(m2p0, m2p1, m2c);
    ref_ch1.push_back(LargeRect{ m1p0, m1p1, m1c, true });
    ref_ch1.push_back(LargeRect{ m2p0, m2p1, m2c, true });
    list.PopClipRect();

    sp.SetCurrentChannel(&list, 2);
    for (int i = 0; i < N2; i++)
    {
        int gx = i % 100, gy = i / 100;
        ImVec2 p0((float)(gx + 100), (float)(16 + gy));
        ImVec2 p1(p0.x + 1.0f, p0.y + 1.0f);
        bool marker = (i == 50 || i == 16499);
        ImU32 col = marker ? IM_COL32(15 + i % 60, 210 - i % 80, 90 + i % 40, 255)
                           : IM_COL32(20, 30, 40, 255);
        list.AddRectFilled(p0, p1, col);
        ref_ch2.push_back(LargeRect{ p0, p1, col, marker });
    }
    list.PopClipRect();
    list.PopTexture();

    sp.Merge(&list);

    const int total_rects = N0 + 2 + N2;
    UNIT_CHECK(list.VtxBuffer.Size == total_rects * 4);
    UNIT_CHECK(list.VtxBuffer.Size > 65536);
    UNIT_CHECK(list.IdxBuffer.Size == total_rects * 6);

    // 16-bit indices must have segmented through VtxOffset; raw indices stay < 64K.
    const bool is_16 = sizeof(ImDrawIdx) == 2;
    int segmented_cmds = 0;
    for (int i = 0; i < list.CmdBuffer.Size; i++)
        if (list.CmdBuffer[i].VtxOffset != 0) segmented_cmds++;
    if (is_16)
        UNIT_CHECK(segmented_cmds > 0);

    // Structural index validation for every command.
    ActualStream actual;
    UNIT_CHECK(extract_actual_stream(&list, actual) == 0);
    if (is_16)
    {
        for (int i = 0; i < list.IdxBuffer.Size; i++)
            UNIT_CHECK((unsigned)list.IdxBuffer[i] < 65536u);
    }

    // Build the normalized reference triangle stream: absolute vertex indices
    // (4r..4r+3) and the exact PrimRect winding, independent of ImDrawIdx width.
    std::vector<std::array<unsigned, 3> > ref_abs;
    std::vector<LargeRect> ref_all = ref_ch0;
    ref_all.insert(ref_all.end(), ref_ch1.begin(), ref_ch1.end());
    ref_all.insert(ref_all.end(), ref_ch2.begin(), ref_ch2.end());
    UNIT_CHECK((int)ref_all.size() == total_rects);
    ref_abs.reserve((size_t)total_rects * 2);
    for (int r = 0; r < total_rects; r++)
    {
        unsigned base = (unsigned)r * 4;
        ref_abs.push_back({ base, base + 1, base + 2 });
        ref_abs.push_back({ base, base + 2, base + 3 });
    }

    // Re-walk commands and compare normalized absolute indices directly.
    std::vector<std::array<unsigned, 3> > act_abs;
    act_abs.reserve(ref_abs.size());
    for (int ci = 0; ci < list.CmdBuffer.Size; ci++)
    {
        const ImDrawCmd& cmd = list.CmdBuffer[ci];
        if (cmd.UserCallback != NULL || cmd.ElemCount == 0) continue;
        for (unsigned t = 0; t < cmd.ElemCount / 3; t++)
        {
            std::array<unsigned, 3> tri;
            for (int k = 0; k < 3; k++)
            {
                unsigned raw = list.IdxBuffer[cmd.IdxOffset + t * 3 + k];
                tri[k] = cmd.VtxOffset + raw;
            }
            act_abs.push_back(tri);
        }
    }
    UNIT_CHECK(act_abs.size() == ref_abs.size());
    for (size_t i = 0; i < ref_abs.size(); i++)
        for (int k = 0; k < 3; k++)
            if (act_abs[i][k] != ref_abs[i][k])
            {
                std::fprintf(stderr, "index: normalized tri %zu vert %d actual=%u expected=%u\n",
                             i, k, act_abs[i][k], ref_abs[i][k]);
                return -1;
            }

    // Every marked rectangle must still reference its own position/UV/color
    // through the real buffers (not just an element-count statistic).
    const ImVec2 white_uv = g_shared.TexUvWhitePixel;
    int markers_checked = 0;
    for (int r = 0; r < total_rects; r++)
    {
        const LargeRect& mr = ref_all[r];
        if (!mr.Marker) continue;
        unsigned base = (unsigned)r * 4;
        const ImDrawVert& v0 = list.VtxBuffer[base + 0];
        const ImDrawVert& v1 = list.VtxBuffer[base + 1];
        const ImDrawVert& v2 = list.VtxBuffer[base + 2];
        const ImDrawVert& v3 = list.VtxBuffer[base + 3];
        UNIT_CHECK(v2_equal(v0.pos, mr.PMin));
        UNIT_CHECK(v2_equal(v1.pos, ImVec2(mr.PMax.x, mr.PMin.y)));
        UNIT_CHECK(v2_equal(v2.pos, mr.PMax));
        UNIT_CHECK(v2_equal(v3.pos, ImVec2(mr.PMin.x, mr.PMax.y)));
        UNIT_CHECK(v0.col == mr.Col && v1.col == mr.Col && v2.col == mr.Col && v3.col == mr.Col);
        UNIT_CHECK(v2_equal(v0.uv, white_uv) && v2_equal(v1.uv, white_uv)
                   && v2_equal(v2.uv, white_uv) && v2_equal(v3.uv, white_uv));
        markers_checked++;
    }
    UNIT_CHECK(markers_checked == 7); // 3 + 2 + 2

    // Pixel reference over the small marker region (16 x 8 canvas). Channel 0/2
    // geometry starts at y=16, so only the two channel-1 markers cover it.
    const int CW = 16, CH = 8;
    Canvas ref_canvas, act_canvas;
    ref_canvas.init(CW, CH);
    act_canvas.init(CW, CH);

    for (const LargeRect& mr : ref_ch1)
    {
        ModelRect model;
        memset(&model, 0, sizeof(model));
        model.Kind = RECT_FLAT;
        model.Tex = 1;
        model.Clip = ImVec4(0, 0, (float)CW, (float)CH);
        model.PMin = mr.PMin; model.PMax = mr.PMax;
        model.Col[0] = model.Col[1] = model.Col[2] = model.Col[3] = mr.Col;
        std::vector<TriEvent> tris;
        emit_rect_tris(model, white_uv, tris);
        for (const TriEvent& t : tris) raster_triangle(ref_canvas, t);
    }
    // Actual: rasterize the real stream; out-of-region geometry is dropped by
    // the triangle bounds and the clip test.
    for (const TriEvent& e : actual.Events)
        if (!e.IsCallback)
            raster_triangle(act_canvas, e);
    UNIT_CHECK(compare_canvas(ref_canvas, act_canvas) == 0);
    return 0;
}

//-----------------------------------------------------------------------------
// mutations group
//-----------------------------------------------------------------------------

struct MutCaseResult
{
    std::string Name;
    int         Checks;
    bool        Entered;
};

struct MutRun
{
    std::string Name;
    std::string CopyDir;
    std::string CompileCmd;
    int         CompileExit = -1;
    std::string RunCmd;
    int         RunExit = -1;
    bool        RunSignaled = false;
    int         RunSignal = -1;
    std::vector<MutCaseResult> Cases;
    std::string FirstDiff;
    std::string Verdict;
};

static std::string slurp_file(const std::string& path)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& content)
{
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    f.write(content.data(), (std::streamsize)content.size());
    return (bool)f;
}

static bool replace_once(std::string& haystack, const std::string& needle, const std::string& repl)
{
    size_t p = haystack.find(needle);
    if (p == std::string::npos) return false;
    if (haystack.find(needle, p + 1) != std::string::npos) return false;
    haystack.replace(p, needle.size(), repl);
    return true;
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            }
            else out += c;
        }
    }
    return out;
}

static const char* kCoreFiles[] =
{
    "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "imgui.h", "imgui_internal.h", "imconfig.h",
    "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h"
};

// Exact anchors from the unmodified library source.
static const char* kAnchorHeaderCompare =
    "            if (ImDrawCmd_HeaderCompare(last_cmd, next_cmd) == 0 && last_cmd->UserCallback == NULL && next_cmd->UserCallback == NULL)";
static const char* kMutHeaderCompare =
    "            if ((memcmp(last_cmd, next_cmd, sizeof(ImVec4)) == 0 && last_cmd->VtxOffset == next_cmd->VtxOffset) && last_cmd->UserCallback == NULL && next_cmd->UserCallback == NULL)";
static const char* kAnchorIdxOffset =
    "            ch._CmdBuffer.Data[cmd_n].IdxOffset = idx_offset;";
static const char* kMutIdxOffset =
    "            ch._CmdBuffer.Data[cmd_n].IdxOffset = 0;";

static std::string repo_root()
{
    // Prefer the path injected by checks/target.sh; fall back to the working
    // directory the fixed runner starts from (the repository root).
    const char* env = std::getenv("MUT_REPO_ROOT");
    if (env && *env) return std::string(env);
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) return std::string(cwd);
    return std::string(".");
}

static std::string mutation_work_dir()
{
    const char* override_dir = std::getenv("MUT_WORK_DIR");
    std::string dir = override_dir && *override_dir
        ? std::string(override_dir) + "/imgui_target_mutations"
        : (std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/imgui_target_mutations");
    mkdir(dir.c_str(), 0755);
    return dir;
}

static int parse_sub_output(MutRun& run)
{
    std::string out = slurp_file(run.CopyDir + "/run.out");
    std::string err = slurp_file(run.CopyDir + "/run.err");

    std::istringstream outs(out);
    std::string line;
    while (std::getline(outs, line))
    {
        if (line.rfind("CASE_ENTER ", 0) == 0)
        {
            MutCaseResult c;
            c.Name = line.substr(11);
            c.Checks = -1;
            c.Entered = true;
            run.Cases.push_back(c);
        }
        else if (line.rfind("CASE_DONE ", 0) == 0)
        {
            // CASE_DONE <name> checks=<n>
            std::istringstream ls(line.substr(9));
            std::string name, tok;
            int checks = 0;
            ls >> name >> tok;
            size_t eq = tok.find('=');
            if (eq != std::string::npos) checks = std::atoi(tok.c_str() + eq + 1);
            for (MutCaseResult& c : run.Cases)
                if (c.Name == name) c.Checks = checks;
        }
    }
    // First diff: first content-dimension line emitted by a failed UNIT_CHECK.
    std::istringstream es(err);
    while (std::getline(es, line))
    {
        if (line.find("CHECK_FAIL") != std::string::npos ||
            line.find("texture") != std::string::npos ||
            line.find("index") != std::string::npos ||
            line.find("clip") != std::string::npos ||
            line.find("stream") != std::string::npos ||
            line.find("callback") != std::string::npos ||
            line.find("pixel") != std::string::npos)
        {
            run.FirstDiff = line;
            break;
        }
    }
    if (run.FirstDiff.empty() && !err.empty())
    {
        std::istringstream es2(err);
        std::getline(es2, run.FirstDiff);
    }
    return 0;
}

static int run_mutation_build(const std::string& name, const std::string& root, const std::string& work,
                              bool apply_tex_mutation, bool apply_idx_mutation, MutRun& run)
{
    run.Name = name;
    run.CopyDir = work + "/" + name;
    // Start from a clean directory so stale per-run logs can never be mistaken
    // for the results of this compilation/execution.
    std::string clean_cmd = "rm -rf '" + run.CopyDir + "' && mkdir -p '" + run.CopyDir + "'";
    if (std::system(clean_cmd.c_str()) != 0)
    {
        run.Verdict = "copy_failed";
        run.FirstDiff = "could not prepare clean copy directory";
        return -1;
    }

    // Independent temporary source copy.
    std::string cp_cmd = "cp -f";
    for (const char* f : kCoreFiles) cp_cmd += " '" + root + "/" + f + "'";
    cp_cmd += " '" + run.CopyDir + "' 2> '" + work + "/" + name + ".cp.err'";
    int cp_status = std::system(cp_cmd.c_str());
    if (cp_status != 0)
    {
        run.Verdict = "copy_failed";
        run.FirstDiff = slurp_file(work + "/" + name + ".cp.err");
        return -1;
    }

    // Apply exactly one compilable degradation to the copy.
    if (apply_tex_mutation || apply_idx_mutation)
    {
        std::string draw_path = run.CopyDir + "/imgui_draw.cpp";
        std::string src = slurp_file(draw_path);
        bool ok = false;
        if (apply_tex_mutation)
            ok = replace_once(src, kAnchorHeaderCompare, kMutHeaderCompare);
        else
            ok = replace_once(src, kAnchorIdxOffset, kMutIdxOffset);
        if (!ok)
        {
            run.Verdict = "patch_anchor_missing";
            run.FirstDiff = "mutation anchor not found in copied imgui_draw.cpp";
            return -1;
        }
        UNIT_CHECK(write_file(draw_path, src));
    }

    const char* abi_flag = (sizeof(ImDrawIdx) == 4) ? " '-DImDrawIdx=unsigned int'" : "";

    // Temporary sub-driver: only replay/large_mesh, never recurses into mutations.
    std::string driver_path = run.CopyDir + "/mut_driver.cpp";
    const char* driver_src =
        "#include \"suite.h\"\n"
        "#include <cstdio>\n"
        "int unit_checks = 0;\n"
        "int main() {\n"
        "    const char* groups[2] = { \"replay\", \"large_mesh\" };\n"
        "    for (int g = 0; g < 2; g++) {\n"
        "        std::printf(\"CASE_ENTER %s\\n\", groups[g]); std::fflush(stdout);\n"
        "        unit_checks = 0;\n"
        "        int n = unit_run_group(groups[g]);\n"
        "        if (n <= 0 || n != unit_checks) { std::fprintf(stderr, \"CASE_FAIL %s\\n\", groups[g]); return 3; }\n"
        "        std::printf(\"CASE_DONE %s checks=%d\\n\", groups[g], n); std::fflush(stdout);\n"
        "    }\n"
        "    std::puts(\"SUB_OK\");\n"
        "    return 0;\n"
        "}\n";
    UNIT_CHECK(write_file(driver_path, driver_src));

    std::string bin_path = run.CopyDir + "/sub";
    run.CompileCmd = "c++ -std=c++17 -O0 -I'" + run.CopyDir + "' -I'" + root + "/checks' "
        + run.CopyDir + "/imgui.cpp " + run.CopyDir + "/imgui_draw.cpp " + run.CopyDir + "/imgui_tables.cpp "
        + run.CopyDir + "/imgui_widgets.cpp '" + root + "/checks/target.cpp' '" + driver_path + "'"
        + abi_flag + " -o '" + bin_path + "' 2> '" + run.CopyDir + "/compile.err'";
    int cp_raw = std::system(run.CompileCmd.c_str());
    run.CompileExit = WIFEXITED(cp_raw) ? WEXITSTATUS(cp_raw) : -1;
    if (run.CompileExit != 0)
    {
        run.Verdict = "compile_failed";
        run.FirstDiff = slurp_file(run.CopyDir + "/compile.err");
        return 0; // not a valid capture; caller asserts against this
    }

    run.RunCmd = "'" + bin_path + "' > '" + run.CopyDir + "/run.out' 2> '" + run.CopyDir + "/run.err'";
    int run_raw = std::system(run.RunCmd.c_str());
    if (WIFEXITED(run_raw))
    {
        run.RunExit = WEXITSTATUS(run_raw);
        run.RunSignaled = false;
    }
    else
    {
        run.RunSignaled = true;
        run.RunSignal = WTERMSIG(run_raw);
        run.RunExit = -1;
    }
    parse_sub_output(run);
    return 0;
}

static int run_mutations()
{
    const std::string root = repo_root();
    const std::string work = mutation_work_dir();
    const char* abi = (sizeof(ImDrawIdx) == 4) ? "32bit" : "16bit";

    std::vector<MutRun> runs(3);
    // Control: unmodified copy must pass the same normal-behavior assertions.
    UNIT_CHECK(run_mutation_build("control", root, work, false, false, runs[0]) == 0);
    // Degradation 1: Merge ignores texture when comparing adjacent channel headers.
    UNIT_CHECK(run_mutation_build("ignore_tex", root, work, true, false, runs[1]) == 0);
    // Degradation 2: post-merge IdxOffset fixup pinned to zero.
    UNIT_CHECK(run_mutation_build("zero_idxoffset", root, work, false, true, runs[2]) == 0);

    for (MutRun& r : runs)
    {
        const bool compiled = r.CompileExit == 0;
        const int entered = (int)r.Cases.size();
        if (r.Name == "control")
        {
            r.Verdict = (compiled && !r.RunSignaled && r.RunExit == 0 && entered == 2)
                        ? "pass_expected" : "invalid";
        }
        else if (!compiled)
        {
            r.Verdict = "compile_failed"; // compile failure cannot count as capture
        }
        else if (r.RunSignaled)
        {
            // Crash / IM_ASSERT abort (SIGABRT) is not an assertion-based capture.
            r.Verdict = (r.RunSignal == SIGABRT) ? "assert_abort" : "crashed";
        }
        else if (entered == 0)
        {
            r.Verdict = "zero_cases";
        }
        else if (r.RunExit == 0)
        {
            r.Verdict = "not_caught";
        }
        else
        {
            r.Verdict = "caught"; // clean non-zero exit after entered cases failed
        }
    }

    // Hard assertions: control passes, both degradations are genuinely caught.
    UNIT_CHECK(runs[0].Verdict == "pass_expected");
    UNIT_CHECK(runs[1].Verdict == "caught");
    UNIT_CHECK(runs[2].Verdict == "caught");

    // The failure must come from the content dimension corresponding to the mutation.
    UNIT_CHECK(runs[1].FirstDiff.find("texture") != std::string::npos);
    UNIT_CHECK(runs[2].FirstDiff.find("index") != std::string::npos);

    // Persist every command, exit code, entered case and first diff as JSON.
    std::ostringstream json;
    json << "{\n  \"abi\": \"" << abi << "\",\n  \"work_dir\": \"" << json_escape(work) << "\",\n  \"runs\": [\n";
    for (size_t i = 0; i < runs.size(); i++)
    {
        const MutRun& r = runs[i];
        json << "    {\n";
        json << "      \"name\": \"" << json_escape(r.Name) << "\",\n";
        json << "      \"copy_dir\": \"" << json_escape(r.CopyDir) << "\",\n";
        json << "      \"compile_cmd\": \"" << json_escape(r.CompileCmd) << "\",\n";
        json << "      \"compile_exit\": " << r.CompileExit << ",\n";
        json << "      \"run_cmd\": \"" << json_escape(r.RunCmd) << "\",\n";
        json << "      \"run_exit\": " << r.RunExit << ",\n";
        json << "      \"run_signaled\": " << (r.RunSignaled ? "true" : "false") << ",\n";
        json << "      \"run_signal\": " << r.RunSignal << ",\n";
        json << "      \"cases\": [";
        for (size_t c = 0; c < r.Cases.size(); c++)
        {
            if (c) json << ", ";
            json << "{\"name\": \"" << json_escape(r.Cases[c].Name)
                 << "\", \"entered\": " << (r.Cases[c].Entered ? "true" : "false")
                 << ", \"checks\": " << r.Cases[c].Checks << "}";
        }
        json << "],\n";
        json << "      \"first_diff\": \"" << json_escape(r.FirstDiff) << "\",\n";
        json << "      \"verdict\": \"" << json_escape(r.Verdict) << "\"\n";
        json << "    }" << (i + 1 < runs.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    std::string json_path = work + "/mutations-" + abi + ".json";
    UNIT_CHECK(write_file(json_path, json.str()));
    std::printf("MUTATION_JSON %s\n", json_path.c_str());
    return 0;
}

//-----------------------------------------------------------------------------
// Entry point
//-----------------------------------------------------------------------------

int unit_run_group(const char* group)
{
    init_textures();
    if (std::strcmp(group, "replay") == 0)
    {
        int r = run_replay();
        if (r < 0) return -1;
        return unit_checks;
    }
    if (std::strcmp(group, "large_mesh") == 0)
    {
        int r = run_large_mesh();
        if (r < 0) return -1;
        return unit_checks;
    }
    if (std::strcmp(group, "mutations") == 0)
    {
        int r = run_mutations();
        if (r < 0) return -1;
        return unit_checks;
    }
    std::fprintf(stderr, "unknown group: %s\n", group);
    return -1;
}
