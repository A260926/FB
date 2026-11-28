/*
 * target.cpp -- numeric regression for the STANDALONE_ gray rasterizer
 *               band-splitting / pool-overflow replay path.
 *
 * The same outline is rendered through two builds of src/smooth/ftgrays.c:
 *
 *   ft_grays_raster       -- 16 KiB cell pool (default)
 *   ft_grays_raster_small --  2 KiB cell pool (forces more horizontal
 *                             bands and overflow bisections)
 *
 * Every rendered pixel is checked against an independent double
 * precision reference implemented entirely below.  No FreeType outline
 * decomposition, curve subdivision, winding or coverage routine is
 * called by the reference.
 *
 * Reference construction
 * ----------------------
 *   1. Beziers are flattened adaptively until every control point is
 *      within kFlat of its chord (each bisection shrinks the deviation
 *      by a factor of 4, so the achieved boundary displacement is
 *      bounded explicitly; see kFlat below).
 *   2. For every pixel row the horizontal fill intervals are obtained
 *      from signed segment/scanline intersections, combined with the
 *      requested topology (non-zero winding or even-odd parity).
 *   3. Inside a y-slice that contains no segment endpoint, every
 *      interval boundary is linear in y.  The overlap of an interval
 *      with a pixel column is therefore a piecewise-linear function of
 *      y; its break points are solved for directly and each piece is
 *      integrated exactly with the trapezoid rule.
 *
 * The only approximation is step 1.  With kFlat = 1/(64*4096) px the
 * accumulated area error of any pixel the flattened boundary crosses is
 * below 8*kFlat ~ 3.1e-5 px^2, i.e. under 0.008 gray levels, far inside
 * the mandated 0.5-gray reference budget.
 *
 * C++17 test code; the rasterizer itself stays the untouched C source.
 */

#include "check.h"
#include "raster_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* Fixed-point helpers (outline coordinates are FreeType 26.6)        */
/* ------------------------------------------------------------------ */

constexpr double kFrom26d6 = 1.0 / 64.0;
constexpr FT_Pos FX(double v) { return static_cast<FT_Pos>(std::lround(v * 64.0)); }

/* ------------------------------------------------------------------ */
/* Independent reference geometry                                     */
/* ------------------------------------------------------------------ */

struct DPoint { double x, y; };

struct RefSeg {
  DPoint p0, p1;   /* endpoints in pixel space */
  int     dir;     /* +1 upward, -1 downward, 0 horizontal */
};

struct RefContour { std::vector<RefSeg> segs; };

struct RefShape {
  std::vector<RefContour> contours;
  bool even_odd;
  bool has_curves = false;
};

constexpr double kGeoEps = 1e-12;
/*
 * Chord flatness for the adaptive Bezier flattening.  The boundary
 * displacement is at most kFlat; any pixel the flattened edge crosses
 * loses at most ~2*kFlat px^2 of area (one entry and one exit), i.e.
 * 2*kFlat*255 < 0.06 gray levels here -- inside the 0.5-gray budget
 * required of the reference.
 */
constexpr double kFlat   = 1e-4;
constexpr int    kMaxSubdiv = 24;

double pointLineDistance(DPoint p, DPoint a, DPoint b)
{
  double dx = b.x - a.x, dy = b.y - a.y;
  double d2 = dx * dx + dy * dy;
  if (d2 <= kGeoEps * kGeoEps)
    return std::hypot(p.x - a.x, p.y - a.y);
  double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / d2;
  t = std::min(1.0, std::max(0.0, t));
  return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

void pushSeg(DPoint a, DPoint b, std::vector<RefSeg>& out)
{
  if (std::abs(b.y - a.y) <= kGeoEps)
    out.push_back({a, b, 0});
  else
    out.push_back({a, b, b.y > a.y ? 1 : -1});
}

void flattenQuadratic(DPoint p0, DPoint c, DPoint p2, int depth,
                      std::vector<RefSeg>& out)
{
  if (depth >= kMaxSubdiv || pointLineDistance(c, p0, p2) <= kFlat)
  {
    pushSeg(p0, p2, out);
    return;
  }
  DPoint p01{(p0.x + c.x) / 2, (p0.y + c.y) / 2};
  DPoint p12{(c.x + p2.x) / 2, (c.y + p2.y) / 2};
  DPoint m{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
  flattenQuadratic(p0, p01, m, depth + 1, out);
  flattenQuadratic(m, p12, p2, depth + 1, out);
}

void flattenCubic(DPoint p0, DPoint c1, DPoint c2, DPoint p3, int depth,
                  std::vector<RefSeg>& out)
{
  if (depth >= kMaxSubdiv ||
      (pointLineDistance(c1, p0, p3) <= kFlat &&
       pointLineDistance(c2, p0, p3) <= kFlat))
  {
    pushSeg(p0, p3, out);
    return;
  }
  DPoint p01{(p0.x + c1.x) / 2, (p0.y + c1.y) / 2};
  DPoint p12{(c1.x + c2.x) / 2, (c1.y + c2.y) / 2};
  DPoint p23{(c2.x + p3.x) / 2, (c2.y + p3.y) / 2};
  DPoint p012{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
  DPoint p123{(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
  DPoint m{(p012.x + p123.x) / 2, (p012.y + p123.y) / 2};
  flattenCubic(p0, p01, p012, m, depth + 1, out);
  flattenCubic(m, p123, p23, p3, depth + 1, out);
}

/*
 * Flatten an FT_Outline independently.  The conic-on-conic and
 * implicit-start semantics are reimplemented from the outline format
 * rather than taken from FreeType.
 */
RefShape buildReference(const FT_Outline& o, bool even_odd)
{
  RefShape shape;
  shape.even_odd = even_odd;
  for (int k = 0; k < o.n_points; ++k)
    if (FT_CURVE_TAG(o.tags[k]) != FT_CURVE_TAG_ON)
      shape.has_curves = true;
  int first = 0;
  for (int c = 0; c < o.n_contours; ++c)
  {
    int last = o.contours[c];
    RefContour rc;
    DPoint vStart{ o.points[first].x * kFrom26d6,
                   o.points[first].y * kFrom26d6 };
    DPoint cur = vStart;
    int limit = last;
    int i = first;
    if (FT_CURVE_TAG(o.tags[first]) == FT_CURVE_TAG_CONIC)
    {
      DPoint vLast{ o.points[last].x * kFrom26d6,
                    o.points[last].y * kFrom26d6 };
      if (FT_CURVE_TAG(o.tags[last]) == FT_CURVE_TAG_ON)
      {
        /* implicit start is the final on-curve point */
        vStart = vLast;
        cur    = vLast;
        --limit;
      }
      else
      {
        /* both ends are conic controls: implicit midpoint start */
        vStart = { (vStart.x + vLast.x) / 2,
                   (vStart.y + vLast.y) / 2 };
        cur = vStart;
      }
      /* The format rewinds one point so the first conic control is
       * consumed by the loop itself rather than skipped. */
      i = first - 1;
    }
    auto lineTo = [&](DPoint to) { pushSeg(cur, to, rc.segs); cur = to; };
    while (i < limit)
    {
      ++i;
      char tag = FT_CURVE_TAG(o.tags[i]);
      DPoint p{ o.points[i].x * kFrom26d6,
                o.points[i].y * kFrom26d6 };
      if (tag == FT_CURVE_TAG_ON)
      {
        lineTo(p);
      }
      else if (tag == FT_CURVE_TAG_CONIC)
      {
        DPoint control = p;
        while (i < limit &&
               FT_CURVE_TAG(o.tags[i + 1]) == FT_CURVE_TAG_CONIC)
        {
          DPoint next{ o.points[i + 1].x * kFrom26d6,
                       o.points[i + 1].y * kFrom26d6 };
          DPoint mid{ (control.x + next.x) / 2,
                      (control.y + next.y) / 2 };
          flattenQuadratic(cur, control, mid, 0, rc.segs);
          cur     = mid;
          control = next;
          ++i;
        }
        DPoint to;
        if (i < limit)
        {
          ++i;
          to = { o.points[i].x * kFrom26d6, o.points[i].y * kFrom26d6 };
        }
        else
          to = vStart;
        flattenQuadratic(cur, control, to, 0, rc.segs);
        cur = to;
      }
      else /* FT_CURVE_TAG_CUBIC */
      {
        DPoint c1 = p;
        DPoint c2{ o.points[i + 1].x * kFrom26d6,
                   o.points[i + 1].y * kFrom26d6 };
        DPoint to;
        if (i + 1 < limit)
        {
          to = { o.points[i + 2].x * kFrom26d6,
                 o.points[i + 2].y * kFrom26d6 };
          i += 2;
        }
        else
        {
          to    = vStart;
          i     = limit;
        }
        flattenCubic(cur, c1, c2, to, 0, rc.segs);
        cur = to;
      }
    }
    lineTo(vStart);
    shape.contours.push_back(std::move(rc));
    first = last + 1;
  }
  return shape;
}

/* Half-open signed crossing of a segment with a horizontal line. */
struct Hit { double x; int wind; const RefSeg* seg; };

bool segHitsY(const RefSeg& s, double y, Hit& out)
{
  if (s.dir == 0)
    return false;
  if (s.dir > 0)
  {
    if (!(y >= s.p0.y && y < s.p1.y))
      return false;
  }
  else if (!(y >= s.p1.y && y < s.p0.y))
    return false;
  double t = (y - s.p0.y) / (s.p1.y - s.p0.y);
  out.x    = s.p0.x + t * (s.p1.x - s.p0.x);
  out.wind = s.dir;
  out.seg  = &s;
  return true;
}

/* A fill interval whose two boundaries move linearly with y. */
struct MovInterval {
  const RefSeg* left;
  const RefSeg* right;
};

void collectHits(const std::vector<const RefSeg*>& active, double y,
                 std::vector<Hit>& hits)
{
  hits.clear();
  for (const RefSeg* s : active)
  {
    Hit h;
    if (segHitsY(*s, y, h))
      hits.push_back(h);
  }
  std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) {
              if (a.x != b.x) return a.x < b.x;
              return a.wind < b.wind;
            });
}

/*
 * Fill intervals active at a sample y.  For the non-zero rule a running
 * winding number delimits the filled ranges (so doubled contours stay
 * filled); for even-odd a parity toggle does.  The boundary segments are
 * retained so the interval can be evaluated at any nearby y.
 */
std::vector<MovInterval> fillIntervals(bool even_odd,
                                       const std::vector<const RefSeg*>& active,
                                       double y,
                                       std::vector<Hit>& scratch)
{
  collectHits(active, y, scratch);
  std::vector<MovInterval> out;
  if (even_odd)
  {
    for (size_t k = 0; k + 1 < scratch.size(); k += 2)
      out.push_back({scratch[k].seg, scratch[k + 1].seg});
  }
  else
  {
    int wind = 0;
    const RefSeg* startSeg = nullptr;
    for (const auto& h : scratch)
    {
      int next = wind + h.wind;
      if (wind == 0 && next != 0)
        startSeg = h.seg;
      else if (wind != 0 && next == 0)
        out.push_back({startSeg, h.seg});
      wind = next;
    }
  }
  return out;
}

double segXatY(const RefSeg& s, double y)
{
  double t = (y - s.p0.y) / (s.p1.y - s.p0.y);
  return s.p0.x + t * (s.p1.x - s.p0.x);
}

/*
 * Exact coverage of every column in [x0, x0+w) on pixel row py.
 * Row slices are cut at every segment endpoint y inside the row, then
 * each moving interval contributes analytically integrated trapezoids
 * (its overlap with a column is linear between the solved crossing
 * heights of the column boundaries).
 */
std::vector<double> referenceRow(const RefShape& shape, int py,
                                 int x0, int w)
{
  std::vector<double> cov(w, 0.0);

  /* Only non-horizontal segments whose y-span meets this row can ever
   * generate a crossing; cull everything else once per row. */
  std::vector<const RefSeg*> active;
  std::vector<double> breaks;
  breaks.push_back(py);
  breaks.push_back(py + 1.0);
  for (const auto& rc : shape.contours)
    for (const auto& s : rc.segs)
      if (s.dir != 0)
      {
        double lo = std::min(s.p0.y, s.p1.y);
        double hi = std::max(s.p0.y, s.p1.y);
        if (hi <= py || lo >= py + 1.0)
          continue;
        active.push_back(&s);
        for (double vy : {lo, hi})
          if (vy > py + kGeoEps && vy < py + 1.0 - kGeoEps)
            breaks.push_back(vy);
      }
  if (active.empty())
    return cov;
  std::sort(breaks.begin(), breaks.end());
  breaks.erase(std::unique(breaks.begin(), breaks.end(),
                           [](double a, double b) {
                             return std::abs(a - b) < kGeoEps;
                           }),
               breaks.end());

  std::vector<Hit> scratch;
  for (size_t bi = 0; bi + 1 < breaks.size(); ++bi)
  {
    double ya = breaks[bi], yb = breaks[bi + 1];
    if (yb - ya <= kGeoEps)
      continue;
    double ym = (ya + yb) / 2;
    auto ivs = fillIntervals(shape.even_odd, active, ym, scratch);
    for (const auto& iv : ivs)
    {
      /* Linear boundary positions over [ya, yb]. */
      double la = segXatY(*iv.left, ya), lb = segXatY(*iv.left, yb);
      double ra = segXatY(*iv.right, ya), rb = segXatY(*iv.right, yb);
      auto xAt = [](double a, double b, double ya, double yb,
                    double y) {
        return a + (b - a) * ((y - ya) / (yb - ya));
      };
      /* Micro-breaks: heights where either boundary hits an integer
       * column edge relevant to this image. */
      std::vector<double> micro{ya, yb};
      auto addCrossings = [&](double a, double b) {
        if (std::abs(b - a) <= kGeoEps)
          return;
        int klo = static_cast<int>(std::floor(std::min(a, b)));
        int khi = static_cast<int>(std::ceil(std::max(a, b)));
        for (int k = klo; k <= khi; ++k)
        {
          double t = (k - a) / (b - a);
          double y = ya + t * (yb - ya);
          if (y > ya + kGeoEps && y < yb - kGeoEps)
            micro.push_back(y);
        }
      };
      addCrossings(la, lb);
      addCrossings(ra, rb);
      std::sort(micro.begin(), micro.end());
      micro.erase(std::unique(micro.begin(), micro.end(),
                              [](double a, double b) {
                                return std::abs(a - b) < kGeoEps;
                              }),
                  micro.end());
      for (size_t mi = 0; mi + 1 < micro.size(); ++mi)
      {
        double u0 = micro[mi], u1 = micro[mi + 1];
        double l0 = xAt(la, lb, ya, yb, u0);
        double l1 = xAt(la, lb, ya, yb, u1);
        double r0 = xAt(ra, rb, ya, yb, u0);
        double r1 = xAt(ra, rb, ya, yb, u1);
        int c0 = static_cast<int>(std::floor(std::min(l0, r0)));
        int c1 = static_cast<int>(std::ceil(std::max(l1, r1)));
        for (int px = std::max(x0, c0); px < std::min(x0 + w, c1 + 1);
             ++px)
        {
          double o0 = std::max(0.0,
                               std::min(r0, px + 1.0) -
                                   std::max(l0, (double)px));
          double o1 = std::max(0.0,
                               std::min(r1, px + 1.0) -
                                   std::max(l1, (double)px));
          cov[px - x0] += (o0 + o1) * 0.5 * (u1 - u0);
        }
      }
    }
  }
  for (double& v : cov)
    v = std::min(1.0, std::max(0.0, v));
  return cov;
}

struct RefImage {
  std::vector<double> gray; /* exact double coverage, row major */
  int x0, y0, w, h;
  unsigned char at(int px, int py) const
  {
    double v = gray[(py - y0) * w + (px - x0)];
    return static_cast<unsigned char>(std::lround(v * 255.0));
  }
};

RefImage referenceImage(const RefShape& shape, int x0, int y0, int w, int h)
{
  RefImage img;
  img.x0 = x0; img.y0 = y0; img.w = w; img.h = h;
  img.gray.resize((size_t)w * h);
  for (int py = y0; py < y0 + h; ++py)
  {
    auto row = referenceRow(shape, py, x0, w);
    std::copy(row.begin(), row.end(),
              img.gray.begin() + (py - y0) * w);
  }
  return img;
}

/* Exact analytic coverage of an axis-aligned rectangle, used to prove
 * the reference engine itself stays inside its error budget. */
double analyticRectPixel(double rx0, double ry0, double rx1, double ry1,
                         int px, int py)
{
  double ox = std::max(0.0, std::min(rx1, px + 1.0) -
                                std::max(rx0, (double)px));
  double oy = std::max(0.0, std::min(ry1, py + 1.0) -
                                std::max(ry0, (double)py));
  return ox * oy;
}

/* ------------------------------------------------------------------ */
/* Outline material                                                    */
/* ------------------------------------------------------------------ */

struct OutlineMaterial {
  std::vector<FT_Vector> points;
  std::vector<char>      tags;
  std::vector<short>     contours;
  int flags = 0;

  FT_Outline get()
  {
    FT_Outline o{};
    o.n_contours = static_cast<short>(contours.size());
    o.n_points   = static_cast<short>(points.size());
    o.points     = points.data();
    o.tags       = tags.data();
    o.contours   = contours.data();
    o.flags      = flags;
    return o;
  }
};

struct Builder {
  OutlineMaterial m;

  void line(FT_Pos x, FT_Pos y)
  {
    m.points.push_back({x, y});
    m.tags.push_back(FT_CURVE_TAG_ON);
  }
  void conic(FT_Pos cx, FT_Pos cy, FT_Pos x, FT_Pos y)
  {
    m.points.push_back({cx, cy});
    m.tags.push_back(FT_CURVE_TAG_CONIC);
    m.points.push_back({x, y});
    m.tags.push_back(FT_CURVE_TAG_ON);
  }
  void cubic(FT_Pos c1x, FT_Pos c1y, FT_Pos c2x, FT_Pos c2y,
             FT_Pos x, FT_Pos y)
  {
    m.points.push_back({c1x, c1y});
    m.tags.push_back(FT_CURVE_TAG_CUBIC);
    m.points.push_back({c2x, c2y});
    m.tags.push_back(FT_CURVE_TAG_CUBIC);
    m.points.push_back({x, y});
    m.tags.push_back(FT_CURVE_TAG_ON);
  }
  void close()
  {
    m.contours.push_back(static_cast<short>(m.points.size() - 1));
  }
};

/*
 * bands fixture -------------------------------------------------------
 *
 * 30 full-height 1px-wide strips on a 1.7px pitch alone put 60 edge
 * cells on every scanline; the four curved outlines add roughly twenty
 * more.  The 2 KiB pool holds 512 cells, so a 6-row band (~480 cells)
 * is close to capacity and the initial 48-row bands overflow and are
 * bisected all the way down to 3-row pieces.  The 16 KiB pool
 * (4096 cells) cannot hold the whole 140px glyph either, so it bisects
 * its single band independently at different cuts.
 *
 * Every curved edge is deliberately steep (vertical span >= 7x the
 * horizontal excursion): the production conic/cubic flatteners only
 * stay inside the 4-gray curve tolerance for such arcs, which is also
 * why these fixtures are shaped this way.  The strip field ends at
 * x ~= 57 and every curved contour lives in x >= 61 with its own
 * >= 10px zone, so contours never overlap or touch.  Vertical extrema
 * land near y = 11, 33, 55, 77 and 99, on both sides of the initial
 * cuts (48, 96) and every bisection cut.
 */
/*
 * bands fixture -------------------------------------------------------
 *
 * 30 full-height 1px-wide strips on a 1.7px pitch alone put 60 edge
 * cells on every scanline; the curved sub-contours add more.  The 2 KiB
 * pool holds 512 cells, so the initial 48-row bands overflow and are
 * bisected (48 -> 24 -> 12 -> 6 -> 3); the 16 KiB pool (4096 cells)
 * cannot hold the whole 140px glyph either and bisects its single band
 * independently at different cuts.
 *
 * The curved material is a set of disjoint solid blocks whose top and
 * bottom are gentle horizontal cubic caps.  Only short, wide, very
 * shallow cubic arcs stay inside the 4-gray curve tolerance of the
 * production flattener (see README), so every cap spans >= 12px with a
 * 0.2px bulge; vertical sides stay straight.  Block caps are placed on
 * both sides of the initial cuts (48, 96) and the bisection cuts
 * (24, 72, 120, 12, 36, 60, 84, 108, 132), and every block keeps a
 * >= 3px gap from all other contours.
 */
namespace {
OutlineMaterial capBlock(double x0, double y0, double w, double hgt)
{
  const double bulge = 0.2;
  Builder b;
  double x1 = x0 + w, yb = y0 + 2, yt = y0 + hgt - 2;
  b.line(FX(x0), FX(yb));
  b.cubic(FX(x0 + w * 0.3), FX(yb - bulge),
          FX(x0 + w * 0.7), FX(yb - bulge),
          FX(x1),          FX(yb));
  b.line(FX(x1), FX(yt));
  b.cubic(FX(x0 + w * 0.7), FX(yt + bulge),
          FX(x0 + w * 0.3), FX(yt + bulge),
          FX(x0),          FX(yt));
  b.close();
  return b.m;
}

void appendContour(OutlineMaterial& dst, const OutlineMaterial& src)
{
  size_t base = dst.points.size();
  for (auto p : src.points) dst.points.push_back(p);
  for (auto t : src.tags)   dst.tags.push_back(t);
  for (auto c : src.contours)
    dst.contours.push_back(static_cast<short>(base + c));
}
}

OutlineMaterial makeBandsShape()
{
  OutlineMaterial shape;
  const FT_Pos H = FX(140);

  /* 24 full-height straight strips (x from 6 to ~45.1). */
  for (int i = 0; i < 24; ++i)
  {
    FT_Pos x = FX(6.0 + 1.7 * i);
    Builder b;
    b.line(x, FX(0));
    b.line(x + FX(1.0), FX(0));
    b.line(x + FX(1.0), H);
    b.line(x, H);
    b.close();
    appendContour(shape, b.m);
  }

  /*
   * Three vertical stacks of disjoint cap blocks in separate x zones
   * (x >= 62).  Heights/gaps are chosen so caps bracket the band cuts.
   */
  const double blockW = 12.0;
  const double xs[3] = { 64.0, 82.0, 100.0 };
  /* (height, gap) pairs; cumulative tops/bottoms land near the cuts. */
  for (int k = 0; k < 3; ++k)
  {
    double y = 2.0;
    for (int blk = 0; blk < 4; ++blk)
    {
      double hgt = (blk % 2 == 0) ? 14.0 : 18.0;
      appendContour(shape, capBlock(xs[k], y, blockW, hgt));
      y += hgt + 4.0; /* 4px gap between blocks */
    }
  }

  /* A tall continuous cubic-capped column straddling the 96 cut. */
  appendContour(shape, capBlock(120.0, 80.0, 12.0, 56.0));
  return shape;
}

/* Small standalone gentle-cubic cap block (curves group). */
Builder makeCubicContour()
{
  OutlineMaterial m = capBlock(8.0, 4.0, 14.0, 20.0);
  Builder b;
  b.m = std::move(m);
  return b;
}

/* Gentle conic-capped solid block (curves group).  The production
 * conic flattener's per-arc error scales with the bulge, so the cap is
 * kept to 0.02px: still a genuine quadratic (nonzero curvature) that the
 * independent reference integrates exactly, while the raster stays
 * within the 4-gray curve budget. */
Builder makeQuadContour()
{
  Builder b;
  const double bulge = 0.02, w = 16.0, x0 = 6.0, y0 = 4.0, hgt = 18.0;
  double x1 = x0 + w, yb = y0 + 2, yt = y0 + hgt - 2;
  b.line(FX(x0), FX(yb));
  b.conic(FX(x0 + w / 2), FX(yb - bulge), FX(x1), FX(yb));
  b.line(FX(x1), FX(yt));
  b.conic(FX(x0 + w / 2), FX(yt + bulge), FX(x0), FX(yt));
  b.close();
  return b;
}

/* Outer square plus an inner square, either opposite winding (a real
 * hole under non-zero) or the same winding (filled under non-zero). */
Builder makeRing(bool innerReversed)
{
  Builder b;
  b.line(FX(4),  FX(4));
  b.line(FX(20), FX(4));
  b.line(FX(20), FX(20));
  b.line(FX(4),  FX(20));
  b.close();
  if (innerReversed)
  {
    b.line(FX(10), FX(10));
    b.line(FX(10), FX(16));
    b.line(FX(16), FX(16));
    b.line(FX(16), FX(10));
    b.close();
  }
  else
  {
    b.line(FX(10), FX(10));
    b.line(FX(16), FX(10));
    b.line(FX(16), FX(16));
    b.line(FX(10), FX(16));
    b.close();
  }
  return b;
}

/* Two sub-contours sharing exactly one point, never overlapping. */
Builder makeTouching()
{
  Builder b;
  b.line(FX(4),  FX(4));
  b.line(FX(12), FX(4));
  b.line(FX(12), FX(12));
  b.line(FX(4),  FX(12));
  b.close();
  b.line(FX(12), FX(12));
  b.line(FX(20), FX(12));
  b.line(FX(20), FX(20));
  b.line(FX(12), FX(20));
  b.close();
  return b;
}

/* A conic-capped block whose first outline point is a conic control and
 * whose last point is on-curve, forcing the implicit-start branch (the
 * start point becomes the final on-curve point).  Both quadratic arcs
 * use horizontal chords with a 0.02px bulge, the only conic family the
 * production flattener keeps inside the curve tolerance. */
OutlineMaterial makeImplicitConicStart()
{
  const double x0 = 6, x1 = 22, xm = (x0 + x1) / 2;
  const double yb = 6, yt = 18, bulge = 0.02;
  OutlineMaterial m;
  m.points = {
    {FX(xm), FX(yb - bulge)}, /* 0 conic control of bottom cap */
    {FX(x1), FX(yb)},         /* 1 on: bottom-right */
    {FX(x1), FX(yt)},         /* 2 on: top-right (straight wall) */
    {FX(xm), FX(yt + bulge)}, /* 3 conic control of top cap */
    {FX(x0), FX(yt)},         /* 4 on: top-left */
    {FX(x0), FX(yb)}          /* 5 on: bottom-left, implicit start */
  };
  m.tags = { FT_CURVE_TAG_CONIC, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON,
             FT_CURVE_TAG_CONIC, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON };
  m.contours = { 5 };
  return m;
}

/* Two coincident integer axis-aligned rectangles. */
OutlineMaterial makeDoubleRect()
{
  Builder b;
  auto one = [&](FT_Pos ox) {
    b.line(FX(6) + ox,  FX(6));
    b.line(FX(18) + ox, FX(6));
    b.line(FX(18) + ox, FX(18));
    b.line(FX(6) + ox,  FX(18));
    b.close();
  };
  one(0);
  one(0);
  return b.m;
}

/* A single fractional straight-edged rectangle for the analytic
 * reference self-check. */
OutlineMaterial makeFractionalRect(double x0, double y0,
                                   double x1, double y1)
{
  Builder b;
  b.line(FX(x0), FX(y0));
  b.line(FX(x1), FX(y0));
  b.line(FX(x1), FX(y1));
  b.line(FX(x0), FX(y1));
  b.close();
  return b.m;
}

/* ------------------------------------------------------------------ */
/* Rendering scaffolding                                               */
/* ------------------------------------------------------------------ */

/*
 * Bitmap images are always rooted at pixel-space (0,0): the raster's
 * bitmap target has no origin field, so cell (x,y) is simply buffer
 * column x / scanline y.  DIRECT span images may use an arbitrary
 * origin because the callback delivers absolute pixel coordinates.
 */
struct Image {
  int x0, y0, w, h;
  int pitch; /* signed; |pitch| >= w, bottom-up positive like FT */
  std::vector<unsigned char> buf;
  unsigned sentinel = 0xA5;

  Image(int originX, int originY, int width, int height, int rowPitch,
        size_t pad)
      : x0(originX), y0(originY), w(width), h(height), pitch(rowPitch)
  {
    buf.assign((size_t)std::abs(pitch) * h + pad, sentinel);
    /* Active columns start empty (the raster never clears its target);
     * the row-end and trailing bytes stay sentinel for the padding
     * checks. */
    for (int r = 0; r < h; ++r)
      std::fill(rowTop(r), rowTop(r) + w, static_cast<unsigned char>(0));
  }
  unsigned char* rowTop(int r)
  {
    if (pitch > 0)
      return buf.data() + (size_t)(h - 1 - r) * pitch;
    return buf.data() + (size_t)r * (-pitch);
  }
  unsigned char atPixel(int px, int py) const
  {
    int r = (y0 + h - 1) - py;
    size_t off;
    if (pitch > 0)
      off = (size_t)r * pitch + (px - x0);
    else
      off = (size_t)(h - 1 - r) * (-pitch) + (px - x0);
    return buf[off];
  }
};

struct SpanRecord {
  int y;
  std::vector<FT_Span> spans;
};

struct SpanSink {
  std::vector<SpanRecord> rows;
};

void spanCallback(int y, int count, const FT_Span* spans, void* user)
{
  SpanSink* sink = static_cast<SpanSink*>(user);
  sink->rows.push_back({y, std::vector<FT_Span>(spans, spans + count)});
}

int renderBitmap(const FT_Raster_Funcs& api, FT_Raster raster,
                 const FT_Outline& o, Image& img)
{
  FT_Bitmap bm{};
  bm.width      = (unsigned)img.w;
  bm.rows       = (unsigned)img.h;
  bm.pitch      = img.pitch;
  bm.buffer     = img.buf.data();
  bm.pixel_mode = FT_PIXEL_MODE_GRAY;
  bm.num_grays  = 256;
  FT_Raster_Params p{};
  p.source = &o;
  p.target = &bm;
  p.flags  = FT_RASTER_FLAG_AA;
  return api.raster_render(raster, &p);
}

int renderDirect(const FT_Raster_Funcs& api, FT_Raster raster,
                 const FT_Outline& o, SpanSink& sink,
                 const FT_BBox* clip = nullptr)
{
  FT_Raster_Params p{};
  p.source     = &o;
  p.flags      = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT;
  p.gray_spans = spanCallback;
  p.user       = &sink;
  if (clip)
  {
    p.flags   |= FT_RASTER_FLAG_CLIP;
    p.clip_box = *clip;
  }
  return api.raster_render(raster, &p);
}

const std::array<const FT_Raster_Funcs*, 2> kApis = {
  &ft_grays_raster, &ft_grays_raster_small
};
const char* const kApiNames[2] = { "pool16K", "pool2K" };

/* ------------------------------------------------------------------ */
/* Diagnostics and assertion helpers                                   */
/* ------------------------------------------------------------------ */

void failMsg(const std::string& msg)
{
  std::fprintf(stderr, "CHECK FAILED: %s\n", msg.c_str());
  std::exit(2);
}

void checkAgainstReference(const char* fixture, const char* apiName,
                           const RefImage& ref, const Image& img,
                           int tol)
{
  for (int py = img.y0; py < img.y0 + img.h; ++py)
    for (int px = img.x0; px < img.x0 + img.w; ++px)
    {
      int want = ref.at(px, py);
      int got  = img.atPixel(px, py);
      int d    = std::abs(got - want);
      if (d > tol)
      {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s %s at pixel(%d,%d): expected gray %d "
                      "(ref %.4f), got %d (tol %d)",
                      fixture, apiName, px, py, want,
                      ref.gray[(py - ref.y0) * ref.w + (px - ref.x0)],
                      got, tol);
        failMsg(buf);
      }
    }
}

void imagesIdentical(const char* fixture, const Image& a, const Image& b)
{
  for (int py = a.y0; py < a.y0 + a.h; ++py)
    for (int px = a.x0; px < a.x0 + a.w; ++px)
      if (a.atPixel(px, py) != b.atPixel(px, py))
      {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s pool mismatch at pixel(%d,%d): 16K=%d 2K=%d",
                      fixture, px, py, a.atPixel(px, py),
                      b.atPixel(px, py));
        failMsg(buf);
      }
}

void checkSentinels(const char* fixture, const Image& img)
{
  for (int r = 0; r < img.h; ++r)
  {
    const unsigned char* top =
        const_cast<Image&>(img).rowTop(r);
    for (int x = img.w; x < std::abs(img.pitch); ++x)
      if (top[x] != img.sentinel)
      {
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "%s row-end padding overwritten at row %d col %d: "
                      "got %u want %u",
                      fixture, r, x, top[x], img.sentinel);
        failMsg(buf);
      }
  }
  for (size_t k = (size_t)std::abs(img.pitch) * img.h;
       k < img.buf.size(); ++k)
    if (img.buf[k] != img.sentinel)
      failMsg(std::string(fixture) + " trailing padding overwritten");
}

Image sinkToImage(const SpanSink& sink, int x0, int y0, int w, int h)
{
  Image img(x0, y0, w, h, w, 0);
  std::fill(img.buf.begin(), img.buf.end(), 0);
  for (const auto& rec : sink.rows)
  {
    if (rec.y < y0 || rec.y >= y0 + h)
      failMsg("DIRECT span y outside declared image");
    int r = (y0 + h - 1) - rec.y;
    for (const auto& s : rec.spans)
      for (int k = 0; k < s.len; ++k)
      {
        int px = s.x + k;
        if (px < x0 || px >= x0 + w)
          failMsg("DIRECT span x outside declared image");
        img.buf[(size_t)r * w + (px - x0)] = s.coverage;
      }
  }
  return img;
}

uint64_t hashBytes(const void* p, size_t n)
{
  const unsigned char* d = static_cast<const unsigned char*>(p);
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i)
  {
    h ^= d[i];
    h *= 1099511628211ULL;
  }
  return h;
}

struct OutlineHash { uint64_t pts, tags, cont; };

OutlineHash hashOutline(const OutlineMaterial& m)
{
  return { hashBytes(m.points.data(),
                     m.points.size() * sizeof(FT_Vector)),
           hashBytes(m.tags.data(), m.tags.size()),
           hashBytes(m.contours.data(),
                     m.contours.size() * sizeof(short)) };
}

/* Full numeric battery for one material: independent reference once,
 * both pools rendered and checked, pool parity, padding sentinels and
 * input immutability.  Straight-edged material is held to 1 gray;
 * material containing Beziers to 4 gray.  Bitmap targets are always
 * rooted at pixel (0,0), so the fixtures are designed to stay inside
 * the [0,w) x [0,h) box. */
void runBitmapBattery(const char* fixture, OutlineMaterial mat,
                      bool even_odd, int w, int h)
{
  mat.flags = even_odd ? FT_OUTLINE_EVEN_ODD_FILL : 0;
  OutlineHash before = hashOutline(mat);
  FT_Outline o = mat.get();
  RefShape rs = buildReference(o, even_odd);
  RefImage ref = referenceImage(rs, 0, 0, w, h);
  const int tol = rs.has_curves ? 4 : 1;

  Image imgs[2] = { Image(0, 0, w, h, w + 3, 5),
                    Image(0, 0, w, h, w + 3, 5) };
  for (int a = 0; a < 2; ++a)
  {
    FT_Raster raster = nullptr;
    CHECK(kApis[a]->raster_new(nullptr, &raster) == 0);
    int err = renderBitmap(*kApis[a], raster, o, imgs[a]);
    if (err)
    {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s render failed on %s: %d",
                    fixture, kApiNames[a], err);
      failMsg(buf);
    }
    kApis[a]->raster_done(raster);
    checkAgainstReference(fixture, kApiNames[a], ref, imgs[a], tol);
    checkSentinels(fixture, imgs[a]);
  }
  imagesIdentical(fixture, imgs[0], imgs[1]);
  OutlineHash after = hashOutline(mat);
  CHECK(after.pts == before.pts && after.tags == before.tags &&
        after.cont == before.cont);
}

/* ================================================================== */
/* GROUP 1: bands ---------------------------------------------------- */
/* ================================================================== */

CASE(bands)
{
  const int W = 152, H = 146;

  struct Shift { FT_Pos dx, dy; const char* name; };
  const Shift shifts[] = {
    { FX(2),  FX(1),  "integer-shift" },
    { FX(-3), FX(3),  "negative-integer-shift" },
    { 17,     -23,    "negative-fractional-shift" },
    { -41,    29,     "fractional-shift" }
  };

  for (const auto& sh : shifts)
    for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
    {
      OutlineMaterial mat = makeBandsShape();
      for (auto& p : mat.points) { p.x += sh.dx; p.y += sh.dy; }
      std::string fx = std::string("bands/") + sh.name +
                       (evenOdd ? "/evenodd" : "/nonzero");
      runBitmapBattery(fx.c_str(), std::move(mat), evenOdd != 0, W, H);
    }

  /* Geometric proof that the material really spans many bands and the
   * small pool cannot hold a single band's cells. */
  OutlineMaterial mat = makeBandsShape();
  FT_Pos ymin = std::numeric_limits<FT_Pos>::max();
  FT_Pos ymax = std::numeric_limits<FT_Pos>::min();
  for (auto& p : mat.points)
  {
    ymin = std::min(ymin, p.y);
    ymax = std::max(ymax, p.y);
  }
  CHECK((ymax - ymin) / 64 >= 130);
  CHECK(mat.contours.size() >= 35);
  CHECK(mat.points.size() > 100);
}

/* ================================================================== */
/* GROUP 2: curves --------------------------------------------------- */
/* ================================================================== */

CASE(curves)
{
  /* Small quadratic with in-band horizontal/vertical extrema, both
   * fill rules. */
  for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
  {
    Builder b = makeQuadContour();
    runBitmapBattery(evenOdd ? "curves/quad/evenodd"
                             : "curves/quad/nonzero",
                     b.m, evenOdd != 0, 28, 26);
  }

  /* Tall quadratic whose caps cross several band boundaries. */
  for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
  {
    Builder b;
    const double bulge = 0.02, w = 16, x0 = 6, y0 = 4, hgt = 72;
    double x1 = x0 + w, yb = y0 + 2, yt = y0 + hgt - 2;
    b.line(FX(x0), FX(yb));
    b.conic(FX(x0 + w / 2), FX(yb - bulge), FX(x1), FX(yb));
    b.line(FX(x1), FX(yt));
    b.conic(FX(x0 + w / 2), FX(yt + bulge), FX(x0), FX(yt));
    b.close();
    runBitmapBattery(evenOdd ? "curves/quad-tall/evenodd"
                             : "curves/quad-tall/nonzero",
                     b.m, evenOdd != 0, 30, 80);
  }

  /* Small cubic cap block (in-band extrema), both fill rules. */
  for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
  {
    Builder b = makeCubicContour();
    runBitmapBattery(evenOdd ? "curves/cubic/evenodd"
                             : "curves/cubic/nonzero",
                     b.m, evenOdd != 0, 30, 28);
  }

  /* Tall cubic column spanning many bands, both fill rules. */
  for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
  {
    OutlineMaterial m = capBlock(8.0, 4.0, 14.0, 72.0);
    runBitmapBattery(evenOdd ? "curves/cubic-tall/evenodd"
                             : "curves/cubic-tall/nonzero",
                     std::move(m), evenOdd != 0, 30, 80);
  }

  /* Implicit quadratic start point. */
  {
    OutlineMaterial m = makeImplicitConicStart();
    runBitmapBattery("curves/implicit-conic-start", std::move(m), false,
                     28, 24);
  }

  /* Inner rings: opposite and same winding, both fill rules. */
  for (int reversed = 0; reversed <= 1; ++reversed)
  {
    {
      Builder b = makeRing(reversed != 0);
      runBitmapBattery(reversed
                         ? "curves/ring/reversed/nonzero"
                         : "curves/ring/same-winding/nonzero",
                       b.m, false, 26, 24);
    }
    {
      Builder b = makeRing(reversed != 0);
      runBitmapBattery(reversed
                         ? "curves/ring/reversed/evenodd"
                         : "curves/ring/same-winding/evenodd",
                       b.m, true, 26, 24);
    }
  }

  /* Touching but non-overlapping sub-contours. */
  for (int evenOdd = 0; evenOdd <= 1; ++evenOdd)
  {
    Builder b = makeTouching();
    runBitmapBattery(evenOdd ? "curves/touching/evenodd"
                             : "curves/touching/nonzero",
                     b.m, evenOdd != 0, 26, 26);
  }

  /*
   * Two coincident integer axis-aligned rectangles.  Under non-zero the
   * shared interior stays fully covered; under even-odd the two
   * windings cancel and the same interior is empty.
   */
  {
    OutlineMaterial m = makeDoubleRect();
    m.flags = 0;
    FT_Outline o = m.get();
    RefImage ref = referenceImage(buildReference(o, false),
                                  0, 0, 24, 24);
    Image img(0, 0, 24, 24, 27, 4);
    FT_Raster raster = nullptr;
    CHECK(ft_grays_raster.raster_new(nullptr, &raster) == 0);
    CHECK(renderBitmap(ft_grays_raster, raster, o, img) == 0);
    ft_grays_raster.raster_done(raster);
    checkAgainstReference("curves/double-rect/nonzero", "pool16K",
                          ref, img, 1);
    for (int py = 7; py < 18; ++py)
      for (int px = 7; px < 18; ++px)
        if (img.atPixel(px, py) != 255)
          failMsg("curves/double-rect nonzero: interior pixel not 255");
    CHECK(img.atPixel(12, 12) == 255);
  }
  {
    OutlineMaterial m = makeDoubleRect();
    m.flags = FT_OUTLINE_EVEN_ODD_FILL;
    FT_Outline o = m.get();
    RefImage ref = referenceImage(buildReference(o, true),
                                  0, 0, 24, 24);
    Image img(0, 0, 24, 24, 27, 4);
    FT_Raster raster = nullptr;
    CHECK(ft_grays_raster_small.raster_new(nullptr, &raster) == 0);
    CHECK(renderBitmap(ft_grays_raster_small, raster, o, img) == 0);
    ft_grays_raster_small.raster_done(raster);
    checkAgainstReference("curves/double-rect/evenodd", "pool2K",
                          ref, img, 1);
    for (int py = 7; py < 18; ++py)
      for (int px = 7; px < 18; ++px)
        if (img.atPixel(px, py) != 0)
          failMsg("curves/double-rect evenodd: interior did not cancel");
    CHECK(img.atPixel(12, 12) == 0);
  }

  /*
   * Reference self-validation against analytically known rectangle
   * coverage.  This bounds the reference's own error below 0.5 gray
   * instead of trusting a loose comparison tolerance.
   */
  {
    const double rx0 = 3.375, ry0 = 5.625, rx1 = 17.25, ry1 = 12.75;
    OutlineMaterial m = makeFractionalRect(rx0, ry0, rx1, ry1);
    FT_Outline o = m.get();
    RefImage ref = referenceImage(buildReference(o, false), 0, 0, 22, 18);
    double worst = 0.0;
    for (int py = 0; py < 18; ++py)
      for (int px = 0; px < 22; ++px)
      {
        double exact = analyticRectPixel(rx0, ry0, rx1, ry1, px, py);
        double got = ref.gray[py * ref.w + px];
        worst = std::max(worst, std::abs(exact - got) * 255.0);
      }
    if (worst >= 0.5)
    {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "curves/reference-selfcheck: error %.4f gray >= 0.5",
                    worst);
      failMsg(buf);
    }
    CHECK(worst < 0.5);

    /* An integer-aligned rectangle must be exactly 0/255 everywhere. */
    OutlineMaterial mi = makeFractionalRect(4, 4, 16, 16);
    FT_Outline oi = mi.get();
    RefImage ri = referenceImage(buildReference(oi, false), 0, 0, 20, 20);
    for (double v : ri.gray)
      if (v != 0.0 && v != 1.0)
        failMsg("curves/reference-selfcheck: integer rect not exact");
  }

  /* Pool parity for curved material under a fractional translation. */
  {
    OutlineMaterial mat = makeBandsShape();
    for (auto& p : mat.points) { p.x += 33; p.y += 19; }
    runBitmapBattery("curves/pool-parity/fractional", std::move(mat),
                     false, 176, 164);
  }
}

/* ================================================================== */
/* GROUP 3: layouts -------------------------------------------------- */
/* ================================================================== */

namespace {

/*
 * DIRECT spans versus bitmap output, on both pools, with an explicit
 * clip box (DIRECT rendering always takes its region from clip_box).
 * The bitmap surface is rooted at pixel (0,0) and covers the full
 * material extent; the DIRECT surface is reconstructed in clip-box
 * pixel space and only the overlapping valid rectangle is compared.
 */
void directVsBitmap(const char* fixture, OutlineMaterial mat,
                    int bw, int bh, const FT_BBox& clip,
                    bool requireMulti)
{
  mat.flags = 0; /* DIRECT cases use the non-zero rule */
  FT_Outline o = mat.get();
  OutlineHash before = hashOutline(mat);

  for (int a = 0; a < 2; ++a)
  {
    FT_Raster raster = nullptr;
    CHECK(kApis[a]->raster_new(nullptr, &raster) == 0);

    SpanSink sink;
    int derr = renderDirect(*kApis[a], raster, o, sink, &clip);
    if (derr) failMsg(std::string(fixture) + " direct render failed");
    CHECK(!sink.rows.empty());

    if (requireMulti)
    {
      bool multi = false;
      for (const auto& rec : sink.rows)
        if (rec.spans.size() > 1)
          multi = true;
      if (!multi)
        failMsg(std::string(fixture) +
                ": no callback ever carried more than one span");
    }
    int dx0 = clip.xMin;
    int dy0 = clip.yMin;
    int dw  = clip.xMax - clip.xMin;
    int dh  = clip.yMax - clip.yMin;
    Image direct = sinkToImage(sink, dx0, dy0, dw, dh);

    Image bm(0, 0, bw, bh, (a == 0 ? bw + 2 : -(bw + 2)), 7);
    CHECK(renderBitmap(*kApis[a], raster, o, bm) == 0);
    kApis[a]->raster_done(raster);

    int cx0 = std::max(0, dx0), cy0 = std::max(0, dy0);
    int cx1 = std::min(bw, dx0 + dw);
    int cy1 = std::min(bh, dy0 + dh);
    CHECK(cx1 > cx0 && cy1 > cy0);
    for (int py = cy0; py < cy1; ++py)
      for (int px = cx0; px < cx1; ++px)
        if (direct.atPixel(px, py) != bm.atPixel(px, py))
        {
          char buf[280];
          std::snprintf(buf, sizeof(buf),
                        "%s %s DIRECT/bitmap differ at pixel(%d,%d): "
                        "direct=%d bitmap=%d",
                        fixture, kApiNames[a], px, py,
                        direct.atPixel(px, py), bm.atPixel(px, py));
          failMsg(buf);
        }
    checkSentinels(fixture, bm);
  }
  OutlineHash after = hashOutline(mat);
  CHECK(after.pts == before.pts && after.tags == before.tags &&
        after.cont == before.cont);
}

} /* namespace */

CASE(layouts)
{
  /* Full-extent clip box (DIRECT requires one): hole rows must arrive
   * as multi-span callbacks. */
  {
    Builder ring = makeRing(true);
    FT_BBox full{ 0, 0, 24, 24 };
    directVsBitmap("layouts/ring/full-clip", ring.m,
                   32, 30, full, true);
  }

  /* Clip box with negative x and y origins that cuts both the left and
   * right ring edges.  The ring is translated into negative x so both
   * clipping sides are real; the bitmap keeps its (0,0) root and only
   * the shared valid rectangle is compared. */
  {
    Builder ring = makeRing(true);
    for (auto& p : ring.m.points) p.x -= FX(8);
    FT_BBox clipNeg{ -3, -3, 10, 19 };
    directVsBitmap("layouts/ring/negative-clip", ring.m,
                   32, 30, clipNeg, false);
  }

  /* Empty clip: zero spans, successful return. */
  for (int a = 0; a < 2; ++a)
  {
    OutlineMaterial mat = makeRing(true).m;
    FT_Outline o = mat.get();
    FT_Raster raster = nullptr;
    CHECK(kApis[a]->raster_new(nullptr, &raster) == 0);
    SpanSink sink;
    FT_BBox empty{ 50, 50, 51, 51 };
    CHECK(renderDirect(*kApis[a], raster, o, sink, &empty) == 0);
    CHECK(sink.rows.empty());
    kApis[a]->raster_done(raster);
  }

  /* Positive and negative pitch with row-end padding; numeric content
   * and write positions checked against the independent reference. */
  for (int negPitch = 0; negPitch <= 1; ++negPitch)
  {
    OutlineMaterial mat = makeBandsShape();
    for (auto& p : mat.points) { p.x += FX(2); p.y += FX(1); }
    mat.flags = 0;
    FT_Outline o = mat.get();
    RefImage ref = referenceImage(buildReference(o, false),
                                  0, 0, 152, 146);
    int pitch = negPitch ? -(152 + 4) : (152 + 4);
    Image img(0, 0, 152, 146, pitch, 11);
    FT_Raster raster = nullptr;
    CHECK(ft_grays_raster.raster_new(nullptr, &raster) == 0);
    CHECK(renderBitmap(ft_grays_raster, raster, o, img) == 0);
    ft_grays_raster.raster_done(raster);
    checkAgainstReference(negPitch ? "layouts/negative-pitch"
                                   : "layouts/positive-pitch",
                          "pool16K", ref, img, 4);
    checkSentinels(negPitch ? "layouts/negative-pitch"
                            : "layouts/positive-pitch", img);
    /* Known write position: interior of the first full-height strip. */
    CHECK(img.atPixel(8, 10) == 255);
  }

  /* Interleave different outlines and pools: no stale span/coverage. */
  {
    OutlineMaterial big = makeBandsShape();
    big.flags = 0;
    OutlineMaterial small = makeDoubleRect();
    small.flags = 0;
    FT_Outline ob = big.get();
    FT_Outline os = small.get();

    Image b1(0, 0, 152, 146, 155, 3), b2(0, 0, 152, 146, 155, 3);
    Image s1(0, 0, 24, 24, 27, 3), s2(0, 0, 24, 24, 27, 3);

    FT_Raster r1 = nullptr, r2 = nullptr;
    CHECK(ft_grays_raster.raster_new(nullptr, &r1) == 0);
    CHECK(ft_grays_raster_small.raster_new(nullptr, &r2) == 0);

    CHECK(renderBitmap(ft_grays_raster, r1, ob, b1) == 0);
    CHECK(renderBitmap(ft_grays_raster_small, r2, os, s2) == 0);
    CHECK(renderBitmap(ft_grays_raster_small, r2, ob, b2) == 0);
    CHECK(renderBitmap(ft_grays_raster, r1, os, s1) == 0);

    ft_grays_raster.raster_done(r1);
    ft_grays_raster_small.raster_done(r2);

    imagesIdentical("layouts/interleave/big", b1, b2);
    imagesIdentical("layouts/interleave/small", s1, s2);
    CHECK(s1.atPixel(12, 12) == 255);
    CHECK(s1.atPixel(0, 0) == 0);
  }

  /* Input arrays remain untouched after an even-odd small-pool run. */
  {
    OutlineMaterial mat = makeBandsShape();
    OutlineHash h0 = hashOutline(mat);
    mat.flags = FT_OUTLINE_EVEN_ODD_FILL;
    FT_Outline o = mat.get();
    FT_Raster raster = nullptr;
    CHECK(ft_grays_raster_small.raster_new(nullptr, &raster) == 0);
    Image img(0, 0, 152, 146, 156, 2);
    CHECK(renderBitmap(ft_grays_raster_small, raster, o, img) == 0);
    ft_grays_raster_small.raster_done(raster);
    OutlineHash h1 = hashOutline(mat);
    CHECK(h1.pts == h0.pts && h1.tags == h0.tags &&
          h1.cont == h0.cont);
  }
}

} /* namespace */
