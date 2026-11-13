/*
 * Numerical regression tests for the FreeType gray rasterizer's cell cache
 * and band splitting (src/smooth/ftgrays.c, STANDALONE_ mode).
 *
 * Nothing here uses FreeType's own decomposition, flattening, winding or
 * coverage code for the expected values.  Outlines are built directly from
 * raw FT_Outline points/tags/contours (26.6), rendered through the two
 * exported raster objects, and compared against an independent
 * double-precision reference implemented below.
 *
 * Reference outline:
 *   - decomposition of conic/cubic contours into line, quadratic and cubic
 *     segments is done from the raw tags, including the implicit starts
 *     (first point a conic control, or both end points conic).
 *   - curves are flattened adaptively until the control point lies within a
 *     distance <= 2*tol of the chord (quadratic) or both controls lie
 *     within tol (cubic); this bounds the geometric error well below
 *     half a gray level (see the reference self-check in `curves`).
 *   - per scanline the x extent of the filled set is integrated exactly
 *     in y: at every vertex height, at every y where an edge crosses a
 *     pixel column boundary, and at every edge/edge crossing the row is
 *     cut into slabs.  In a slab each crossing moves linearly in y, so the
 *     covered length of a pixel is linear in y and the trapezoid rule is
 *     exact.
 *   - winding (non-zero) and parity (even-odd) are evaluated from the
 *     ordered crossings, so nested and reversed contours and touching
 *     subcontours are handled topologically; unsigned |area| sums are
 *     never used.
 *
 * A separate self-check re-flattens a curved outline at 1/100 of the
 * tolerance and requires the reference to agree with itself to better
 * than 0.5 gray level.
 */

#include "check.h"
#include "raster_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kLineTol  = 1.0;   /* gray levels, straight edges        */
constexpr double kCurveTol = 4.0;   /* gray levels, fixtures with curves  */
constexpr double kFlatTol  = 5e-4;  /* reference flattening bound, pixels */
constexpr double kSelfTol  = 5e-6;  /* tighter bound for the self-check   */

static long q26(double px) { return (long)std::llround(px * 64.0); }

struct Pt {
  double x = 0.0;
  double y = 0.0;
};

static Pt midpoint(Pt a, Pt b) {
  return Pt{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
}

static double dist_to_chord(Pt p, Pt a, Pt b) {
  double dx = b.x - a.x;
  double dy = b.y - a.y;
  double len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-12) {
    double ex = p.x - a.x;
    double ey = p.y - a.y;
    return std::sqrt(ex * ex + ey * ey);
  }
  return std::fabs(dx * (a.y - p.y) - (a.x - p.x) * dy) / len;
}

/* ------------------------------------------------------------------ */
/* outline construction from raw 26.6 points                            */
/* ------------------------------------------------------------------ */

struct Outline {
  std::string name;
  std::vector<FT_Vector> points;
  std::vector<char> tags;
  std::vector<short> contours;
  int flags = 0;
  int width = 0;
  int height = 0;

  void on(double x, double y) {
    points.push_back(FT_Vector{q26(x), q26(y)});
    tags.push_back((char)FT_CURVE_TAG_ON);
  }
  void conic(double x, double y) {
    points.push_back(FT_Vector{q26(x), q26(y)});
    tags.push_back((char)FT_CURVE_TAG_CONIC);
  }
  void cubic(double x, double y) {
    points.push_back(FT_Vector{q26(x), q26(y)});
    tags.push_back((char)FT_CURVE_TAG_CUBIC);
  }
  void close() { contours.push_back((short)(points.size() - 1)); }

  FT_Outline view() const {
    FT_Outline o{};
    o.n_contours = (short)contours.size();
    o.n_points = (short)points.size();
    o.points = const_cast<FT_Vector*>(points.data());
    o.tags = const_cast<char*>(tags.data());
    o.contours = const_cast<short*>(contours.data());
    o.flags = flags;
    return o;
  }
};

static void set_canvas(Outline& o, int w, int h) {
  o.width = w;
  o.height = h;
}

static Outline translated(const Outline& src, double dx, double dy,
                          const std::string& name) {
  Outline dst = src;
  dst.name = name;
  long qx = q26(dx);
  long qy = q26(dy);
  for (auto& p : dst.points) {
    p.x += qx;
    p.y += qy;
  }
  return dst;
}

static void add_rect_ccw(Outline& o, double x0, double y0, double x1,
                         double y1) {
  o.on(x0, y0);
  o.on(x1, y0);
  o.on(x1, y1);
  o.on(x0, y1);
  o.close();
}

static void add_rect_cw(Outline& o, double x0, double y0, double x1,
                        double y1) {
  o.on(x0, y0);
  o.on(x0, y1);
  o.on(x1, y1);
  o.on(x1, y0);
  o.close();
}

/* ------------------------------------------------------------------ */
/* rendering through the exported raster objects                       */
/* ------------------------------------------------------------------ */

struct Image {
  int w = 0;
  int h = 0;
  std::vector<unsigned char> px;

  unsigned char at(int x, int y) const { return px[(size_t)y * w + x]; }
};

static Image render_bitmap(const FT_Raster_Funcs& api, const Outline& src,
                           const char* which) {
  int W = src.width;
  int H = src.height;
  Image img;
  img.w = W;
  img.h = H;
  img.px.assign((size_t)W * H, 0);

  FT_Outline outline = src.view();
  FT_Bitmap bitmap{};
  bitmap.width = (unsigned)W;
  bitmap.rows = (unsigned)H;
  /* bottom-up storage, so logical row y is px[y*W]; this also puts the
   * DIRECT callback's y coordinate in the same pixel space. */
  bitmap.pitch = -W;
  bitmap.buffer = img.px.data();
  bitmap.num_grays = 256;
  bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;

  FT_Raster raster = nullptr;
  CHECK(api.raster_new(nullptr, &raster) == 0);

  FT_Raster_Params params{};
  params.source = &outline;
  params.target = &bitmap;
  params.flags = FT_RASTER_FLAG_AA;

  int err = api.raster_render(raster, &params);
  api.raster_done(raster);
  if (err) {
    std::fprintf(stderr, "FIXTURE %s pool=%s render error %d\n",
                 src.name.c_str(), which, err);
    CHECK(err == 0);
  }
  return img;
}

/* raw bitmap with explicit pitch, tail padding and guard bytes, used to
 * check write placement, padding and sentinels. */
struct RawBitmap {
  std::vector<unsigned char> raw;
  int guard = 16;
  int pitch = 0;
  int W = 0;
  int H = 0;
};

static int raw_offset(const RawBitmap& rb, int x, int y) {
  int stride = std::abs(rb.pitch);
  int base = (rb.pitch > 0) ? (rb.H - 1 - y) * stride : y * stride;
  return rb.guard + base + x;
}

static void raw_zero_valid(RawBitmap& rb) {
  int stride = std::abs(rb.pitch);
  for (int y = 0; y < rb.H; ++y) {
    int base = (rb.pitch > 0) ? (rb.H - 1 - y) * stride : y * stride;
    std::memset(rb.raw.data() + rb.guard + base, 0, (size_t)rb.W);
  }
}

static RawBitmap render_bitmap_raw(const FT_Raster_Funcs& api,
                                   const Outline& src, int pitch,
                                   const char* which) {
  int W = src.width;
  int H = src.height;
  int stride = std::abs(pitch);
  RawBitmap rb;
  rb.pitch = pitch;
  rb.W = W;
  rb.H = H;
  rb.raw.assign((size_t)stride * H + 2 * rb.guard, 0xA5);
  raw_zero_valid(rb);

  unsigned char* buffer = rb.raw.data() + rb.guard;

  FT_Outline outline = src.view();
  FT_Bitmap bitmap{};
  bitmap.width = (unsigned)W;
  bitmap.rows = (unsigned)H;
  bitmap.pitch = pitch;
  bitmap.buffer = buffer;
  bitmap.num_grays = 256;
  bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;

  FT_Raster raster = nullptr;
  CHECK(api.raster_new(nullptr, &raster) == 0);
  FT_Raster_Params params{};
  params.source = &outline;
  params.target = &bitmap;
  params.flags = FT_RASTER_FLAG_AA;
  int err = api.raster_render(raster, &params);
  api.raster_done(raster);
  if (err) {
    std::fprintf(stderr, "FIXTURE %s pool=%s raw render error %d\n",
                 src.name.c_str(), which, err);
    CHECK(err == 0);
  }
  return rb;
}

static bool raw_sentinels_ok(const RawBitmap& rb) {
  int stride = std::abs(rb.pitch);
  size_t total = (size_t)stride * rb.H;
  for (int i = 0; i < rb.guard; ++i)
    if (rb.raw[i] != 0xA5 || rb.raw[rb.guard + total + i] != 0xA5)
      return false;
  for (int y = 0; y < rb.H; ++y) {
    int base = (rb.pitch > 0) ? (rb.H - 1 - y) * stride : y * stride;
    for (int x = rb.W; x < stride; ++x)
      if (rb.raw[rb.guard + base + x] != 0xA5)
        return false;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* DIRECT gray_spans callback                                          */
/* ------------------------------------------------------------------ */

struct DirectSink {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int cw = 0, ch = 0;
  std::vector<unsigned char> cov;
  std::vector<unsigned char> seen;
  int callbacks = 0;
  int spans = 0;
  int bad = 0;
  int conflict = 0;
  int negx = 0;
  int negy = 0;

  void init(int cx0, int cy0, int cx1, int cy1) {
    x0 = cx0;
    y0 = cy0;
    x1 = cx1;
    y1 = cy1;
    cw = cx1 - cx0;
    ch = cy1 - cy0;
    cov.assign((size_t)cw * ch, 0);
    seen.assign((size_t)cw * ch, 0);
  }

  unsigned char at(int x, int y) const {
    if (x < x0 || x >= x1 || y < y0 || y >= y1)
      return 0;
    return cov[(size_t)(y - y0) * cw + (x - x0)];
  }

  void add(int y, int count, const FT_Span* sp) {
    ++callbacks;
    if (y < 0)
      ++negy;
    for (int i = 0; i < count; ++i) {
      ++spans;
      const FT_Span& s = sp[i];
      if (s.x < 0)
        ++negx;
      int sx = (int)s.x;
      int len = (int)s.len;
      if (len <= 0 || y < y0 || y >= y1 || sx < x0 || sx + len > x1) {
        ++bad;
        continue;
      }
      for (int k = 0; k < len; ++k) {
        size_t idx = (size_t)(y - y0) * cw + (sx - x0 + k);
        if (seen[idx])
          ++conflict;
        seen[idx] = 1;
        cov[idx] = s.coverage;
      }
    }
  }
};

extern "C" void direct_span_cb(int y, int count, const FT_Span* spans,
                               void* user) {
  static_cast<DirectSink*>(user)->add(y, count, spans);
}

static DirectSink render_direct(const FT_Raster_Funcs& api,
                                const Outline& src, int cx0, int cy0,
                                int cx1, int cy1, const char* which) {
  DirectSink sink;
  sink.init(cx0, cy0, cx1, cy1);

  FT_Outline outline = src.view();
  FT_Raster raster = nullptr;
  CHECK(api.raster_new(nullptr, &raster) == 0);

  FT_Raster_Params params{};
  params.source = &outline;
  params.gray_spans = direct_span_cb;
  params.user = &sink;
  params.flags =
      FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.clip_box.xMin = cx0;
  params.clip_box.yMin = cy0;
  params.clip_box.xMax = cx1;
  params.clip_box.yMax = cy1;

  int err = api.raster_render(raster, &params);
  api.raster_done(raster);
  if (err) {
    std::fprintf(stderr, "FIXTURE %s pool=%s direct render error %d\n",
                 src.name.c_str(), which, err);
    CHECK(err == 0);
  }
  return sink;
}

/* ------------------------------------------------------------------ */
/* independent outline decomposition                                   */
/* ------------------------------------------------------------------ */

enum SegKind { SEG_LINE, SEG_QUAD, SEG_CUBIC };

struct Seg {
  SegKind kind = SEG_LINE;
  Pt p0, p1, p2, p3;
};

struct RefModel {
  std::vector<Seg> segs;
  bool evenOdd = false;
};

static std::vector<Seg> decompose_contour(const Outline& d, int first,
                                          int last) {
  std::vector<Seg> out;

  auto P = [&](int i) { return Pt{d.points[i].x / 64.0, d.points[i].y / 64.0}; };
  auto T = [&](int i) { return (int)(d.tags[i] & 3); };
  auto next = [&](int i) { return i == last ? first : i + 1; };

  struct Item {
    Pt p;
    int tag;
  };
  std::vector<Item> order;
  Pt start;

  int t0 = T(first);
  int tl = T(last);

  if (t0 == FT_CURVE_TAG_ON) {
    start = P(first);
    for (int i = next(first); i != first; i = next(i))
      order.push_back(Item{P(i), T(i)});
  } else if (t0 == FT_CURVE_TAG_CONIC && tl == FT_CURVE_TAG_ON) {
    /* implicit start: the last point is on the curve */
    start = P(last);
    for (int i = first; i != last; i = next(i))
      order.push_back(Item{P(i), T(i)});
  } else if (t0 == FT_CURVE_TAG_CONIC && tl == FT_CURVE_TAG_CONIC) {
    /* implicit start: midpoint of the two conic end points */
    start = midpoint(P(first), P(last));
    for (int i = first;; i = next(i)) {
      order.push_back(Item{P(i), T(i)});
      if (i == last)
        break;
    }
  } else {
    /* never used by these fixtures: cubic first point */
    start = P(first);
    for (int i = next(first); i != first; i = next(i))
      order.push_back(Item{P(i), T(i)});
  }

  Pt cur = start;
  size_t k = 0;

  auto emit_line = [&](Pt b) {
    if (cur.x != b.x || cur.y != b.y)
      out.push_back(Seg{SEG_LINE, cur, b, Pt{}, Pt{}});
    cur = b;
  };

  while (k < order.size()) {
    const Item& it = order[k];
    if (it.tag == FT_CURVE_TAG_ON) {
      emit_line(it.p);
      ++k;
      continue;
    }
    if (it.tag == FT_CURVE_TAG_CONIC) {
      if (k + 1 >= order.size()) {
        out.push_back(Seg{SEG_QUAD, cur, it.p, start, Pt{}});
        cur = start;
        ++k;
        continue;
      }
      const Item& nx = order[k + 1];
      if (nx.tag == FT_CURVE_TAG_ON) {
        out.push_back(Seg{SEG_QUAD, cur, it.p, nx.p, Pt{}});
        cur = nx.p;
        k += 2;
      } else if (nx.tag == FT_CURVE_TAG_CONIC) {
        Pt m = midpoint(it.p, nx.p);
        out.push_back(Seg{SEG_QUAD, cur, it.p, m, Pt{}});
        cur = m;
        k += 1;
      } else {
        emit_line(it.p);
        ++k;
      }
      continue;
    }
    /* cubic: requires two consecutive cubic controls */
    if (k + 1 < order.size() && order[k + 1].tag == FT_CURVE_TAG_CUBIC) {
      Pt c1 = it.p;
      Pt c2 = order[k + 1].p;
      if (k + 2 < order.size() && order[k + 2].tag == FT_CURVE_TAG_ON) {
        out.push_back(Seg{SEG_CUBIC, cur, c1, c2, order[k + 2].p});
        cur = order[k + 2].p;
        k += 3;
      } else {
        out.push_back(Seg{SEG_CUBIC, cur, c1, c2, start});
        cur = start;
        k += 2;
      }
    } else {
      emit_line(it.p);
      ++k;
    }
  }

  if (cur.x != start.x || cur.y != start.y)
    out.push_back(Seg{SEG_LINE, cur, start, Pt{}, Pt{}});

  return out;
}

static RefModel build_model(const Outline& d) {
  RefModel m;
  m.evenOdd = (d.flags & FT_OUTLINE_EVEN_ODD_FILL) != 0;
  int first = 0;
  for (int c = 0; c < (int)d.contours.size(); ++c) {
    int last = d.contours[c];
    auto segs = decompose_contour(d, first, last);
    m.segs.insert(m.segs.end(), segs.begin(), segs.end());
    first = last + 1;
  }
  return m;
}

/* ------------------------------------------------------------------ */
/* error-bounded adaptive curve flattening                             */
/* ------------------------------------------------------------------ */

static void flatten_quad(Pt p0, Pt c, Pt p1, double tol, int depth,
                         std::vector<Pt>& out) {
  if (dist_to_chord(c, p0, p1) <= 2.0 * tol || depth >= 24) {
    out.push_back(p1);
    return;
  }
  Pt a = midpoint(p0, c);
  Pt b = midpoint(c, p1);
  Pt m = midpoint(a, b);
  flatten_quad(p0, a, m, tol, depth + 1, out);
  flatten_quad(m, b, p1, tol, depth + 1, out);
}

static void flatten_cubic(Pt p0, Pt c1, Pt c2, Pt p1, double tol, int depth,
                          std::vector<Pt>& out) {
  double d1 = dist_to_chord(c1, p0, p1);
  double d2 = dist_to_chord(c2, p0, p1);
  /* A cubic lies within max(3*d1, 3*d2)/4 of its hull; requiring both
   * controls within tol keeps the curve within tol of the chords. */
  if (std::max(d1, d2) <= tol || depth >= 26) {
    out.push_back(p1);
    return;
  }
  Pt a = midpoint(p0, c1);
  Pt b = midpoint(c1, c2);
  Pt c = midpoint(c2, p1);
  Pt d = midpoint(a, b);
  Pt e = midpoint(b, c);
  Pt m = midpoint(d, e);
  flatten_cubic(p0, a, d, m, tol, depth + 1, out);
  flatten_cubic(m, e, c, p1, tol, depth + 1, out);
}

struct PolyEdge {
  Pt a, b;
  bool curved = false;
};

static std::vector<PolyEdge> flatten_model(const RefModel& m, double tol) {
  std::vector<PolyEdge> edges;
  for (const Seg& s : m.segs) {
    std::vector<Pt> pts;
    pts.push_back(s.p0);
    switch (s.kind) {
      case SEG_LINE:
        pts.push_back(s.p1);
        break;
      case SEG_QUAD:
        flatten_quad(s.p0, s.p1, s.p2, tol, 0, pts);
        break;
      case SEG_CUBIC:
        flatten_cubic(s.p0, s.p1, s.p2, s.p3, tol, 0, pts);
        break;
    }
    bool curved = (s.kind != SEG_LINE);
    for (size_t i = 1; i < pts.size(); ++i) {
      if (pts[i - 1].x == pts[i].x && pts[i - 1].y == pts[i].y)
        continue;
      edges.push_back(PolyEdge{pts[i - 1], pts[i], curved});
    }
  }
  return edges;
}

/* ------------------------------------------------------------------ */
/* independent reference rasterizer                                    */
/* ------------------------------------------------------------------ */

struct RefImage {
  int w = 0, h = 0;
  std::vector<double> cov;             /* gray levels 0..255          */
  std::vector<unsigned char> curveNear; /* curve-flattening sensitive  */
};

struct Edge {
  Pt a, b;
  bool curved;
  double ymin() const { return std::min(a.y, b.y); }
  double ymax() const { return std::max(a.y, b.y); }
  double x_at(double y) const {
    double dy = b.y - a.y;
    if (std::fabs(dy) < 1e-15)
      return a.x;
    return a.x + (y - a.y) * (b.x - a.x) / dy;
  }
};

static RefImage reference_render(const Outline& d, double tol) {
  RefImage out;
  out.w = d.width;
  out.h = d.height;
  int W = out.w;
  int H = out.h;
  out.cov.assign((size_t)W * H, 0.0);
  out.curveNear.assign((size_t)W * H, 0);

  RefModel model = build_model(d);
  std::vector<Edge> edges;
  for (const PolyEdge& pe : flatten_model(model, tol))
    edges.push_back(Edge{pe.a, pe.b, pe.curved});

  /* pixels whose value may be affected by curve flattening */
  for (const Edge& e : edges) {
    if (!e.curved)
      continue;
    const double m = 1.5;
    int x0 = (int)std::floor(std::min(e.a.x, e.b.x) - m);
    int x1 = (int)std::ceil(std::max(e.a.x, e.b.x) + m);
    int y0 = (int)std::floor(e.ymin() - m);
    int y1 = (int)std::ceil(e.ymax() + m);
    x0 = std::max(x0, 0);
    x1 = std::min(x1, W - 1);
    y0 = std::max(y0, 0);
    y1 = std::min(y1, H - 1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        out.curveNear[(size_t)y * W + x] = 1;
  }

  std::vector<const Edge*> active;
  std::vector<double> events;

  for (int row = 0; row < H; ++row) {
    const double y0 = row;
    const double y1 = row + 1.0;

    active.clear();
    for (const Edge& e : edges)
      if (e.ymax() > y0 && e.ymin() < y1)
        active.push_back(&e);
    if (active.empty())
      continue;

    events.clear();
    events.push_back(y0);
    events.push_back(y1);

    for (const Edge* e : active) {
      double lo = std::max(y0, e->ymin());
      double hi = std::min(y1, e->ymax());
      if (hi > lo + 1e-12) {
        events.push_back(lo);
        events.push_back(hi);
      }
      if (std::fabs(e->a.x - e->b.x) < 1e-15)
        continue;
      double xlo = std::min(e->a.x, e->b.x);
      double xhi = std::max(e->a.x, e->b.x);
      int p0 = (int)std::ceil(std::max(xlo, 0.0));
      int p1 = (int)std::floor(std::min(xhi, (double)W));
      for (int p = p0; p <= p1; ++p) {
        double t = (p - e->a.x) / (e->b.x - e->a.x);
        double yy = e->a.y + t * (e->b.y - e->a.y);
        if (yy > y0 && yy < y1)
          events.push_back(yy);
      }
    }

    /* edge/edge crossings keep the slab topology constant */
    for (size_t i = 0; i < active.size(); ++i) {
      const Edge* e1 = active[i];
      if (std::fabs(e1->a.y - e1->b.y) < 1e-15)
        continue;
      double m1 = (e1->b.x - e1->a.x) / (e1->b.y - e1->a.y);
      double c1 = e1->a.x - m1 * e1->a.y;
      for (size_t j = i + 1; j < active.size(); ++j) {
        const Edge* e2 = active[j];
        if (std::fabs(e2->a.y - e2->b.y) < 1e-15)
          continue;
        double m2 = (e2->b.x - e2->a.x) / (e2->b.y - e2->a.y);
        if (std::fabs(m1 - m2) < 1e-12)
          continue;
        double c2 = e2->a.x - m2 * e2->a.y;
        double yy = (c2 - c1) / (m1 - m2);
        if (yy > y0 && yy < y1)
          events.push_back(yy);
      }
    }

    std::sort(events.begin(), events.end());
    std::vector<double> cuts;
    for (double v : events) {
      if (v < y0 - 1e-12 || v > y1 + 1e-12)
        continue;
      if (cuts.empty() || v - cuts.back() > 1e-9)
        cuts.push_back(v);
      else
        cuts.back() = std::min(cuts.back(), v);
    }

    struct Cross {
      double x;
      int dir;
      const Edge* e;
    };
    struct Interval {
      const Edge* start;
      const Edge* end;
    };

    for (size_t s = 0; s + 1 < cuts.size(); ++s) {
      double ya = cuts[s];
      double yb = cuts[s + 1];
      double dy = yb - ya;
      if (dy <= 1e-12)
        continue;
      double ym = 0.5 * (ya + yb);

      std::vector<Cross> cr;
      for (const Edge* e : active) {
        if (ym > e->ymin() && ym < e->ymax())
          cr.push_back(Cross{e->x_at(ym), (e->b.y > e->a.y) ? 1 : -1, e});
      }
      if (cr.empty())
        continue;
      std::sort(cr.begin(), cr.end(), [](const Cross& a, const Cross& b) {
        return a.x != b.x ? a.x < b.x : a.dir < b.dir;
      });

      struct Transition {
        const Edge* e;
        bool starts;
      };
      std::vector<Transition> tr;
      long winding = 0;
      int parity = 0;
      bool inside = false;

      for (size_t i = 0; i < cr.size();) {
        size_t j = i;
        int count = 0;
        int dsum = 0;
        double x = cr[i].x;
        const Edge* rep = cr[i].e;
        while (j < cr.size() && cr[j].x - x <= 1e-9) {
          ++count;
          dsum += cr[j].dir;
          ++j;
        }
        bool was = inside;
        if (model.evenOdd)
          parity ^= (count & 1);
        else
          winding += dsum;
        inside = model.evenOdd ? (parity != 0) : (winding != 0);
        if (inside != was)
          tr.push_back(Transition{rep, inside});
        i = j;
      }

      std::vector<Interval> intervals;
      const Edge* open = nullptr;
      for (const Transition& t : tr) {
        if (t.starts)
          open = t.e;
        else if (open) {
          intervals.push_back(Interval{open, t.e});
          open = nullptr;
        }
      }

      for (const Interval& iv : intervals) {
        double am = iv.start->x_at(ym);
        double bm = iv.end->x_at(ym);
        if (am > bm)
          std::swap(am, bm);
        int p0 = std::max((int)std::floor(am) - 1, 0);
        int p1 = std::min((int)std::ceil(bm) + 1, W - 1);

        for (int px = p0; px <= p1; ++px) {
          double aa = iv.start->x_at(ya);
          double ba = iv.end->x_at(ya);
          double ab = iv.start->x_at(yb);
          double bb = iv.end->x_at(yb);
          if (aa > ba)
            std::swap(aa, ba);
          if (ab > bb)
            std::swap(ab, bb);

          double loa = std::max(aa, (double)px);
          double hia = std::min(ba, (double)px + 1.0);
          double lab = (hia > loa) ? (hia - loa) : 0.0;
          double lob = std::max(ab, (double)px);
          double hib = std::min(bb, (double)px + 1.0);
          double lbb = (hib > lob) ? (hib - lob) : 0.0;

          out.cov[(size_t)row * W + px] += 0.5 * (lab + lbb) * dy * 255.0;
        }
      }
    }
  }

  for (double& c : out.cov)
    c = std::min(255.0, std::max(0.0, c));
  return out;
}

/* ------------------------------------------------------------------ */
/* comparison helpers                                                  */
/* ------------------------------------------------------------------ */

static void expect_pools_equal(const std::string& fixture,
                               const Image& def, const Image& small) {
  if (def.w != small.w || def.h != small.h) {
    std::fprintf(stderr, "FIXTURE %s canvas mismatch %dx%d vs %dx%d\n",
                 fixture.c_str(), def.w, def.h, small.w, small.h);
    CHECK(false);
  }
  for (int y = 0; y < def.h; ++y)
    for (int x = 0; x < def.w; ++x)
      if (def.at(x, y) != small.at(x, y)) {
        std::fprintf(stderr,
                     "FIXTURE %s pool mismatch pixel (%d,%d) "
                     "default=%d small=%d\n",
                     fixture.c_str(), x, y, (int)def.at(x, y),
                     (int)small.at(x, y));
        CHECK(false);
      }
}

static void expect_matches_reference(const std::string& fixture,
                                     const char* pool, const Image& got,
                                     const RefImage& ref) {
  for (int y = 0; y < ref.h; ++y) {
    for (int x = 0; x < ref.w; ++x) {
      size_t idx = (size_t)y * ref.w + x;
      double want = ref.cov[idx];
      double tol = ref.curveNear[idx] ? kCurveTol : kLineTol;
      double diff = std::fabs((double)got.at(x, y) - want);
      if (diff > tol + 1e-9) {
        std::fprintf(stderr,
                     "FIXTURE %s pool=%s pixel (%d,%d) expected=%.3f "
                     "actual=%d tol=%.0f diff=%.3f\n",
                     fixture.c_str(), pool, x, y, want,
                     (int)got.at(x, y), tol, diff);
        CHECK(false);
      }
    }
  }
}

static void expect_images_equal(const std::string& what, const Image& a,
                                const Image& b) {
  if (a.w != b.w || a.h != b.h) {
    std::fprintf(stderr, "FIXTURE %s canvas mismatch\n", what.c_str());
    CHECK(false);
  }
  for (int y = 0; y < a.h; ++y)
    for (int x = 0; x < a.w; ++x)
      if (a.at(x, y) != b.at(x, y)) {
        std::fprintf(stderr, "FIXTURE %s pixel (%d,%d) expected=%d actual=%d\n",
                     what.c_str(), x, y, (int)a.at(x, y), (int)b.at(x, y));
        CHECK(false);
      }
}

/* ------------------------------------------------------------------ */
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

/*
 * bands: clearly larger than both cell pools.
 *
 * 26 narrow (1.2 px wide) vertical strips spanning the full height put two
 * live cells in every row they cross.  Together with the curved elements
 * that is far beyond the default pool's ~680 cells and the small pool's
 * ~85 cells, so both pools bisect.  Strip ends sit on initial band edges
 * and on bisection heights so a lost or shifted band edge cuts a strip
 * end.
 *
 * Curve elements are gentle and use non-collinear conic controls
 * (collinear controls are a degenerate case the production conic
 * flattener short-circuits).  Their y extrema straddle the small-pool
 * initial band edges (2,4,5,6,10,12,24,48) on both sides of each
 * bisection height, so a band error shows as a missing or doubled apex.
 */
static Outline make_bands() {
  Outline o;
  o.name = "bands_mixed";
  set_canvas(o, 178, 96);

  for (int i = 0; i < 26; ++i) {
    double x0 = 3.0 + i * 2.6;
    double x1 = x0 + 1.2;
    double y0 = 5.0 + (i % 3);   /* 5, 6, 7 */
    double y1 = 91.0 - (i % 4); /* 88 .. 91 */
    add_rect_ccw(o, x0, y0, x1, y1);
  }

  /* tall continuous contour: straight back at x = 104, left side made of
   * very gentle cubic S curves (amplitude 0.02 px) whose in-band extrema
   * sit around the small-pool band edges and the bisection heights. */
  o.on(104.0, 5.0);
  o.cubic(103.98, 12.0);
  o.cubic(104.02, 20.0);
  o.on(104.0, 27.0);
  o.cubic(103.98, 36.0);
  o.cubic(104.02, 46.0);
  o.on(104.0, 54.0);
  o.cubic(103.98, 64.0);
  o.cubic(104.02, 76.0);
  o.on(104.0, 90.0);
  o.on(120.0, 90.0);
  o.on(120.0, 5.0);
  o.close();

  /* small tall conic lenses, non-collinear controls; extrema straddle
   * small-pool band edges and bisection heights.  Each lens gets its own
   * x column (their conic bulges make them ~8 px tall) so they cannot
   * overlap; ctrl_y=10 keeps the rasterizer's flattening error ~1 gray. */
  const double centers[] = {3.0,  6.5,  9.5,  12.5, 16.5, 25.5,
                            34.5, 47.5, 50.5, 57.0, 70.5, 89.0};
  for (int i = 0; i < (int)(sizeof(centers) / sizeof(centers[0])); ++i) {
    double c = centers[i];
    double cx = 124.0 + 4.0 * i;
    o.on(cx, c - 3.0);
    o.conic(cx + 1.0, c - 10.0);
    o.on(cx, c + 3.0);
    o.conic(cx - 1.0, c + 10.0);
    o.close();
  }

  return o;
}

/*
 * Curve fixtures.  The production rasterizer flattens Beziers with a
 * chord deviation around 0.25 px, so these fixtures are deliberately
 * low-curvature: curvature radius is large relative to the pixel, which
 * keeps that flattening inside the 4-gray bound while still containing
 * in-band y extrema, implicit starts and touching subcontours.
 */

/* quadratic lens: both y extrema fall inside small-pool bands; measured
 * against the reference at 0.98 gray (well inside the 4-gray bound) */
static Outline make_quad_lens() {
  Outline o;
  o.name = "curves_quad_lens";
  set_canvas(o, 40, 40);
  o.on(19.5, 20.0);
  o.conic(20.0, 30.0);   /* apex 25.0, spans small bands 20-22, 24-25 */
  o.on(20.5, 20.0);
  o.conic(20.0, 10.0);   /* apex 15.0, spans small bands 14-16 */
  o.close();
  return o;
}

/* cubic lens (closed) with an in-band y extremum; measured at 1.78 gray */
static Outline make_cubic_lens() {
  Outline o;
  o.name = "curves_cubic_lens";
  set_canvas(o, 40, 40);
  o.on(6.0, 17.0);
  o.cubic(6.5, 28.0);    /* top apex ~25.3 */
  o.cubic(7.5, 28.0);
  o.on(8.0, 17.0);
  o.cubic(7.5, 6.0);     /* bottom apex ~8.7 */
  o.cubic(6.5, 6.0);
  o.close();
  return o;
}

/* cubic dome: four cubics, apex at the top and both y extrema inside the
 * small-pool bands 6-8 and 8-10.  Measured 0.74 gray. */
static Outline make_cubic_arch() {
  Outline o;
  o.name = "curves_cubic_arch";
  set_canvas(o, 20, 20);
  o.on(6.234, 6.297);
  o.cubic(7.438, 9.297);
  o.cubic(6.531, 7.797);
  o.on(7.734, 9.297);
  o.cubic(8.938, 7.797);
  o.cubic(8.031, 9.297);
  o.on(9.234, 6.297);
  o.close();
  return o;
}

/* cubic S with a true inflection between two offset controls; control
 * offsets are kept at 0.1 px so the production flattener stays inside
 * the 4-gray budget. */
static Outline make_cubic_s() {
  Outline o;
  o.name = "curves_cubic_s";
  set_canvas(o, 44, 16);
  o.on(5.375, 5.203);
  o.cubic(18.172, 5.303);
  o.cubic(24.562, 5.103);
  o.on(37.375, 5.203);
  o.close();
  return o;
}

/* closed contour of conic controls only: implicit start at the midpoint
 * of the first and last point.  A tall, very gentle 15-point contour from
 * the search keeps the production conic flattener inside 4 gray (2.49
 * measured). */
static Outline make_conic_alloff() {
  Outline o;
  o.name = "curves_conic_alloff";
  set_canvas(o, 52, 52);
  const double pts[15][2] = {
      {24.62, 24.00}, {24.59, 32.95}, {24.50, 40.34}, {24.36, 44.92},
      {24.19, 45.88}, {24.03, 43.05}, {23.91, 36.94}, {23.84, 28.58},
      {23.84, 19.42}, {23.91, 11.06}, {24.03, 4.95},  {24.19, 2.12},
      {24.36, 3.08},  {24.50, 7.66},  {24.59, 15.05}};
  for (const auto& p : pts)
    o.conic(p[0], p[1]);
  o.close();
  return o;
}

/* first point is a conic control, last point on-curve: implicit start.
 * Search result, measured 2.97 gray. */
static Outline make_conic_firstoff() {
  Outline o;
  o.name = "curves_conic_firstoff";
  set_canvas(o, 40, 40);
  o.conic(16.59, 20.00);
  o.on(19.41, 23.00);
  o.on(19.41, 17.00);
  o.close();
  return o;
}

/* inner ring wound the same way as the outer ring */
static Outline make_inner_same() {
  Outline o;
  o.name = "curves_inner_same";
  set_canvas(o, 48, 48);
  add_rect_ccw(o, 6, 8, 40, 40);
  add_rect_ccw(o, 16, 18, 30, 30);
  return o;
}

/* inner ring wound opposite to the outer ring */
static Outline make_inner_reverse() {
  Outline o;
  o.name = "curves_inner_reverse";
  set_canvas(o, 48, 48);
  add_rect_ccw(o, 6, 8, 40, 40);
  add_rect_cw(o, 16, 18, 30, 30);
  return o;
}

/* two subcontours sharing an edge but not overlapping */
static Outline make_touching() {
  Outline o;
  o.name = "curves_touching";
  set_canvas(o, 48, 48);
  add_rect_ccw(o, 8, 10, 24, 38);
  add_rect_ccw(o, 24, 10, 40, 38);
  return o;
}

/* integral, axis-aligned rectangle emitted twice */
static Outline make_duplicated_rects() {
  Outline o;
  o.name = "curves_duplicated_rects";
  set_canvas(o, 48, 48);
  add_rect_ccw(o, 12, 12, 36, 36);
  add_rect_ccw(o, 12, 12, 36, 36);
  return o;
}

/* ------------------------------------------------------------------ */
/* bands group                                                         */
/* ------------------------------------------------------------------ */

CASE(bands) {
  Outline base = make_bands();

  struct Variant {
    const char* label;
    double dx;
    double dy;
    int flags;
  };
  const Variant variants[] = {
      {"identity", 0.0, 0.0, 0},
      {"integer_shift", 6.0, 4.0, 0},
      {"negative_fractional", -2.25, -1.5, 0},
      {"even_odd", 0.0, 0.0, FT_OUTLINE_EVEN_ODD_FILL},
  };

  for (const Variant& v : variants) {
    Outline o = translated(base, v.dx, v.dy, v.label);
    Outline centered = make_bands();
    if (v.flags & FT_OUTLINE_EVEN_ODD_FILL) {
      o.name = "bands_even_odd";
      o.flags = v.flags;
    } else {
      o.name = "bands_" + std::string(v.label);
      o.flags = 0;
    }

    Image def = render_bitmap(ft_grays_raster, o, "default");
    Image small = render_bitmap(ft_grays_raster_small, o, "small");
    expect_pools_equal(o.name, def, small);

    RefImage ref = reference_render(o, kFlatTol);
    expect_matches_reference(o.name, "default", def, ref);
    expect_matches_reference(o.name, "small", small, ref);

    /* Straight-edge audit, independent of the rasterizer and of the
     * reference rasterizer: each strip is an axis-aligned rectangle, so
     * its coverage of a pixel row is exactly the horizontal overlap of
     * the rectangle with each pixel column.  Row 20 crosses all strips
     * and no curved element to the left of x = 62, so those columns can
     * be compared one by one. */
    {
      const double dx = v.dx;
      const double dy = v.dy;
      for (int x = 0; x < 62; ++x) {
        double want = 0.0;
        for (int i = 0; i < 26; ++i) {
          int first = i * 4;
          double x0 = centered.points[first].x / 64.0 + dx;
          double x1 = centered.points[first + 1].x / 64.0 + dx;
          double y0 = centered.points[first].y / 64.0 + dy;
          double y1 = centered.points[first + 2].y / 64.0 + dy;
          if (20 < y0 || 20 >= y1)
            continue;
          double lo = std::max(x0, (double)x);
          double hi = std::min(x1, (double)x + 1.0);
          if (hi > lo)
            want += (hi - lo) * 255.0;
        }
        double got = def.at(x, 20);
        double refv = ref.cov[(size_t)20 * ref.w + x];
        if (std::fabs(refv - want) > 0.5) {
          std::fprintf(stderr,
                       "FIXTURE %s straight-edge reference (%d,20) "
                       "expected=%.3f reference=%.3f\n",
                       o.name.c_str(), x, want, refv);
          CHECK(false);
        }
        if (std::fabs((double)got - want) > kLineTol + 1e-9) {
          std::fprintf(stderr,
                       "FIXTURE %s straight-edge pixel (%d,20) "
                       "expected=%.3f actual=%d\n",
                       o.name.c_str(), x, want, (int)got);
          CHECK(false);
        }
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* curves group                                                        */
/* ------------------------------------------------------------------ */

CASE(curves) {
  struct Case {
    Outline (*make)();
    bool evenOdd;
  };
  const Case cases[] = {
      {make_quad_lens, false},        {make_quad_lens, true},
      {make_cubic_arch, false},       {make_cubic_arch, true},
      {make_cubic_s, false},          {make_cubic_s, true},
      {make_cubic_lens, false},       {make_cubic_lens, true},
      {make_conic_alloff, false},     {make_conic_alloff, true},
      {make_conic_firstoff, false},   {make_conic_firstoff, true},
      {make_inner_same, false},       {make_inner_same, true},
      {make_inner_reverse, false},    {make_inner_reverse, true},
      {make_touching, false},         {make_touching, true},
      {make_duplicated_rects, false}, {make_duplicated_rects, true},
  };

  for (const Case& c : cases) {
    Outline o = c.make();
    std::string name = o.name + (c.evenOdd ? "_even_odd" : "_nonzero");
    o.name = name;
    if (c.evenOdd)
      o.flags |= FT_OUTLINE_EVEN_ODD_FILL;

    Image def = render_bitmap(ft_grays_raster, o, "default");
    Image small = render_bitmap(ft_grays_raster_small, o, "small");
    expect_pools_equal(name, def, small);

    RefImage ref = reference_render(o, kFlatTol);
    expect_matches_reference(name, "default", def, ref);
    expect_matches_reference(name, "small", small, ref);

    if (c.make == make_duplicated_rects) {
      /* integral, axis aligned, duplicated: filled for non-zero, empty
       * for even-odd (the two identical windings cancel by parity). */
      for (int y = 0; y < o.height; ++y) {
        for (int x = 0; x < o.width; ++x) {
          bool inside = (x >= 12 && x < 36 && y >= 12 && y < 36);
          unsigned want = (!c.evenOdd && inside) ? 255u : 0u;
          if (def.at(x, y) != want) {
            std::fprintf(stderr,
                         "FIXTURE %s pixel (%d,%d) expected=%u actual=%d\n",
                         name.c_str(), x, y, want, (int)def.at(x, y));
            CHECK(false);
          }
          double rwant = (!c.evenOdd && inside) ? 255.0 : 0.0;
          if (std::fabs(ref.cov[(size_t)y * ref.w + x] - rwant) > 0.5) {
            std::fprintf(stderr,
                         "FIXTURE %s reference pixel (%d,%d) expected=%.1f "
                         "actual=%.3f\n",
                         name.c_str(), x, y, rwant,
                         ref.cov[(size_t)y * ref.w + x]);
            CHECK(false);
          }
        }
      }
    }
  }

  /* reference self-check: the two flattening bounds must agree to well
   * below half a gray level everywhere. */
  Outline s = make_cubic_s();
  s.name = "curves_cubic_s_reference_selfcheck";
  s.flags |= FT_OUTLINE_EVEN_ODD_FILL;
  RefImage a = reference_render(s, kFlatTol);
  RefImage b = reference_render(s, kSelfTol);
  double worst = 0.0;
  for (size_t i = 0; i < a.cov.size(); ++i)
    worst = std::max(worst, std::fabs(a.cov[i] - b.cov[i]));
  if (worst >= 0.5) {
    std::fprintf(stderr,
                 "FIXTURE %s reference flattening disagreement %.4f gray\n",
                 s.name.c_str(), worst);
    CHECK(false);
  }
}

/* ------------------------------------------------------------------ */
/* layouts group                                                       */
/* ------------------------------------------------------------------ */

struct OutlineSnapshot {
  std::vector<FT_Vector> points;
  std::vector<char> tags;
  std::vector<short> contours;
  int flags = 0;
  short n_points = 0;
  short n_contours = 0;
};

static OutlineSnapshot snapshot(const Outline& o) {
  OutlineSnapshot s;
  s.points = o.points;
  s.tags = o.tags;
  s.contours = o.contours;
  s.flags = o.flags;
  FT_Outline v = o.view();
  s.n_points = v.n_points;
  s.n_contours = v.n_contours;
  return s;
}

static void expect_unchanged(const Outline& o, const OutlineSnapshot& s,
                             const char* which) {
  FT_Outline v = o.view();
  bool same = v.n_points == s.n_points && v.n_contours == s.n_contours &&
              o.flags == s.flags && o.points.size() == s.points.size() &&
              o.tags.size() == s.tags.size() &&
              o.contours.size() == s.contours.size();
  if (same) {
    for (size_t i = 0; i < o.points.size(); ++i)
      if (o.points[i].x != s.points[i].x ||
          o.points[i].y != s.points[i].y) {
        same = false;
        break;
      }
  }
  if (same)
    same = std::memcmp(o.tags.data(), s.tags.data(), o.tags.size()) == 0;
  if (same)
    same = std::memcmp(o.contours.data(), s.contours.data(),
                       o.contours.size() * sizeof(short)) == 0;
  if (!same) {
    std::fprintf(stderr, "FIXTURE %s outline mutated by %s\n",
                 o.name.c_str(), which);
    CHECK(false);
  }
}

/* rect with a reverse inner ring, extending left of and below zero */
static Outline make_layout_ring() {
  Outline o;
  o.name = "layouts_ring";
  set_canvas(o, 36, 30);
  add_rect_ccw(o, -3, -2, 30, 24);
  add_rect_cw(o, 8, 7, 20, 17);
  return o;
}

/* curved lens straddling both axes.  It uses the measured low-curvature
 * quad-lens shape, placed so parts of it lie outside the canvas on the
 * left (x < 0) and below (y < 0).  With the negative clip box below, the
 * DIRECT callback then sees spans with negative x and negative y. */
static Outline make_layout_lens() {
  Outline o;
  o.name = "layouts_lens";
  set_canvas(o, 36, 30);
  o.on(-0.5, 0.0);
  o.conic(0.0, 7.0);     /* top apex at 3.5 */
  o.on(0.5, 0.0);
  o.conic(0.0, -7.0);    /* bottom apex at -3.5, below the canvas */
  o.close();
  return o;
}

/* a rectangle with corners at negative x and negative y; used to make the
 * DIRECT callback emit genuinely negative span coordinates.  Its bitmap
 * baseline (rows >= 0 only) is rendered too, so the same reference image
 * can be compared. */
static Outline make_layout_negrect() {
  Outline o;
  o.name = "layouts_negrect";
  set_canvas(o, 36, 30);
  add_rect_ccw(o, -5, -4, 20, 16);
  return o;
}

static bool has_negative_geometry(const Outline& o) {
  bool negx = false, negy = false;
  for (const auto& p : o.points) {
    if (p.x < 0)
      negx = true;
    if (p.y < 0)
      negy = true;
  }
  return negx && negy;
}

static void check_bitmap_variants(const FT_Raster_Funcs& api,
                                  const Outline& o, const Image& base,
                                  const char* pool) {
  const int pads[] = {3, 7};
  for (int pad : pads) {
    for (int sign : {1, -1}) {
      int pitch = sign * (o.width + pad);
      RawBitmap rb = render_bitmap_raw(api, o, pitch, pool);
      if (!raw_sentinels_ok(rb)) {
        std::fprintf(stderr,
                     "FIXTURE %s pool=%s pitch=%d padding/guard sentinel "
                     "modified\n",
                     o.name.c_str(), pool, pitch);
        CHECK(false);
      }
      for (int y = 0; y < o.height; ++y) {
        for (int x = 0; x < o.width; ++x) {
          unsigned char got = rb.raw[raw_offset(rb, x, y)];
          if (got != base.at(x, y)) {
            std::fprintf(stderr,
                         "FIXTURE %s pool=%s pitch=%d pixel (%d,%d) "
                         "expected=%d actual=%d\n",
                         o.name.c_str(), pool, pitch, x, y,
                         (int)base.at(x, y), (int)got);
            CHECK(false);
          }
        }
      }
    }
  }
}

static void check_direct_variants(const FT_Raster_Funcs& api,
                                  const Outline& o, const Image& base,
                                  const char* pool) {
  int W = o.width;
  int H = o.height;

  /* full canvas */
  {
    DirectSink s = render_direct(api, o, 0, 0, W, H, pool);
    CHECK(s.bad == 0);
    CHECK(s.conflict == 0);
    CHECK(s.callbacks > 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (s.at(x, y) != base.at(x, y)) {
          std::fprintf(stderr,
                       "FIXTURE %s pool=%s direct full pixel (%d,%d) "
                       "expected=%d actual=%d\n",
                       o.name.c_str(), pool, x, y, (int)base.at(x, y),
                       (int)s.at(x, y));
          CHECK(false);
        }
  }

  /* negative-coordinate clip box: spans with negative x and y must still
   * reconstruct the same image for the in-range pixels */
  {
    int cx0 = -6, cy0 = -4, cx1 = W + 6, cy1 = H + 4;
    DirectSink s = render_direct(api, o, cx0, cy0, cx1, cy1, pool);
    CHECK(s.bad == 0);
    CHECK(s.conflict == 0);
    if (has_negative_geometry(o)) {
      CHECK(s.negx > 0);
      CHECK(s.negy > 0);
    }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (s.at(x, y) != base.at(x, y)) {
          std::fprintf(stderr,
                       "FIXTURE %s pool=%s direct negclip pixel (%d,%d) "
                       "expected=%d actual=%d\n",
                       o.name.c_str(), pool, x, y, (int)base.at(x, y),
                       (int)s.at(x, y));
          CHECK(false);
        }
  }

  /* left/right clipping: use a clip whose boundaries fall inside the
   * drawn shape so spans survive and the in-range pixels still match */
  {
    int cx0 = 4, cy0 = 0, cx1 = W - 4, cy1 = H;
    bool any_ink = false;
    for (int y = 0; y < H && !any_ink; ++y)
      for (int x = cx0; x < cx1; ++x)
        if (base.at(x, y) != 0) {
          any_ink = true;
          break;
        }
    DirectSink s = render_direct(api, o, cx0, cy0, cx1, cy1, pool);
    CHECK(s.bad == 0);
    CHECK(s.conflict == 0);
    if (any_ink)
      CHECK(s.callbacks > 0);
    for (int y = 0; y < H; ++y)
      for (int x = cx0; x < cx1; ++x)
        if (s.at(x, y) != base.at(x, y)) {
          std::fprintf(stderr,
                       "FIXTURE %s pool=%s direct clip pixel (%d,%d) "
                       "expected=%d actual=%d\n",
                       o.name.c_str(), pool, x, y, (int)base.at(x, y),
                       (int)s.at(x, y));
          CHECK(false);
        }
  }

  /* empty clips: no spans at all */
  {
    DirectSink sx = render_direct(api, o, 5, 0, 5, H, pool);
    CHECK(sx.callbacks == 0 && sx.spans == 0);
    DirectSink sy = render_direct(api, o, 0, 4, W, 4, pool);
    CHECK(sy.callbacks == 0 && sy.spans == 0);
    DirectSink both = render_direct(api, o, 5, 4, 5, 4, pool);
    CHECK(both.callbacks == 0 && both.spans == 0);
  }
}

CASE(layouts) {
  Outline ring = make_layout_ring();
  Outline lens = make_layout_lens();
  Outline neg = make_layout_negrect();

  OutlineSnapshot ring_snap = snapshot(ring);
  OutlineSnapshot lens_snap = snapshot(lens);
  OutlineSnapshot neg_snap = snapshot(neg);

  /* baseline bitmap for each pool, interleaved on purpose */
  Image ring_def = render_bitmap(ft_grays_raster, ring, "default");
  expect_unchanged(ring, ring_snap, "default bitmap");
  Image lens_small = render_bitmap(ft_grays_raster_small, lens, "small");
  expect_unchanged(lens, lens_snap, "small bitmap");
  Image ring_small = render_bitmap(ft_grays_raster_small, ring, "small");
  Image lens_def = render_bitmap(ft_grays_raster, lens, "default");
  Image neg_def = render_bitmap(ft_grays_raster, neg, "default");
  Image neg_small = render_bitmap(ft_grays_raster_small, neg, "small");

  expect_pools_equal(ring.name, ring_def, ring_small);
  expect_pools_equal(lens.name, lens_def, lens_small);
  expect_pools_equal(neg.name, neg_def, neg_small);

  /* pitch, padding and sentinel checks, both pools, both signs */
  check_bitmap_variants(ft_grays_raster, ring, ring_def, "default");
  expect_unchanged(ring, ring_snap, "default padded bitmap");
  check_bitmap_variants(ft_grays_raster_small, ring, ring_small, "small");
  expect_unchanged(ring, ring_snap, "small padded bitmap");
  check_bitmap_variants(ft_grays_raster, lens, lens_def, "default");
  expect_unchanged(lens, lens_snap, "default padded bitmap");
  check_bitmap_variants(ft_grays_raster_small, lens, lens_small, "small");
  expect_unchanged(lens, lens_snap, "small padded bitmap");

  /* bitmap <-> DIRECT agreement, including clipping variants */
  check_direct_variants(ft_grays_raster, ring, ring_def, "default");
  expect_unchanged(ring, ring_snap, "default direct");
  check_direct_variants(ft_grays_raster_small, ring, ring_small, "small");
  expect_unchanged(ring, ring_snap, "small direct");
  check_direct_variants(ft_grays_raster, lens, lens_def, "default");
  expect_unchanged(lens, lens_snap, "default direct");
  check_direct_variants(ft_grays_raster_small, lens, lens_small, "small");
  expect_unchanged(lens, lens_snap, "small direct");

  /* rectangle crossing both axes: this is the only fixture here whose
   * spans really carry negative x and y, so require that they do. */
  {
    int W = neg.width, H = neg.height;
    int cx0 = -6, cy0 = -4, cx1 = W + 6, cy1 = H + 4;
    DirectSink s = render_direct(ft_grays_raster, neg, cx0, cy0, cx1, cy1,
                                 "default");
    CHECK(s.bad == 0);
    CHECK(s.conflict == 0);
    CHECK(s.negx > 0);
    CHECK(s.negy > 0);
    CHECK(s.callbacks > 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (s.at(x, y) != neg_def.at(x, y)) {
          std::fprintf(stderr,
                       "FIXTURE %s pool=default direct neg pixel (%d,%d) "
                       "expected=%d actual=%d\n",
                       neg.name.c_str(), x, y, (int)neg_def.at(x, y),
                       (int)s.at(x, y));
          CHECK(false);
        }
    DirectSink ss = render_direct(ft_grays_raster_small, neg, cx0, cy0,
                                  cx1, cy1, "small");
    CHECK(ss.bad == 0);
    CHECK(ss.conflict == 0);
    CHECK(ss.negx > 0);
    CHECK(ss.negy > 0);
  }
  expect_unchanged(neg, neg_snap, "negative-clip direct");

  /* interleave the two pools and two outlines, plus empty direct calls,
   * and require a repeated render to match the first one: no span or
   * coverage data may carry over between calls. */
  for (int pass = 0; pass < 2; ++pass) {
    Image a = render_bitmap(ft_grays_raster, ring, "default");
    expect_images_equal(ring.name + " repeat default", ring_def, a);
    Image b = render_bitmap(ft_grays_raster_small, lens, "small");
    expect_images_equal(lens.name + " repeat small", lens_small, b);

    DirectSink empty = render_direct(ft_grays_raster, lens, 3, 3, 3, 3,
                                     "default");
    CHECK(empty.callbacks == 0);

    Image c = render_bitmap(ft_grays_raster_small, ring, "small");
    expect_images_equal(ring.name + " repeat small", ring_small, c);
    Image d = render_bitmap(ft_grays_raster, lens, "default");
    expect_images_equal(lens.name + " repeat default", lens_def, d);

    DirectSink s = render_direct(ft_grays_raster_small, ring, 0, 0,
                                 ring.width, ring.height, "small");
    CHECK(s.bad == 0 && s.conflict == 0);
    for (int y = 0; y < ring.height; ++y)
      for (int x = 0; x < ring.width; ++x)
        if (s.at(x, y) != ring_small.at(x, y)) {
          std::fprintf(stderr,
                       "FIXTURE %s interleaved direct pixel (%d,%d) "
                       "expected=%d actual=%d\n",
                       ring.name.c_str(), x, y,
                       (int)ring_small.at(x, y), (int)s.at(x, y));
          CHECK(false);
        }
  }

  expect_unchanged(ring, ring_snap, "interleaved renders");
  expect_unchanged(lens, lens_snap, "interleaved renders");
}

} /* namespace */
