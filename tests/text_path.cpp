#include "check.h"
#include "font_fixture.hpp"
#include "graphics.h"
#include "pathmeasure.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace lunasvg;

namespace {

// Independent reference geometry: high precision adaptive Simpson integration
// of a cubic's speed function and bisection-based arc length inversion. These
// never call PathMeasure.
struct RefCubic {
    double p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
};

void refDerivative(const RefCubic& c, double t, double& dx, double& dy)
{
    double u = 1.0 - t;
    dx = 3.0 * u * u * (c.p1x - c.p0x) + 6.0 * u * t * (c.p2x - c.p1x) + 3.0 * t * t * (c.p3x - c.p2x);
    dy = 3.0 * u * u * (c.p1y - c.p0y) + 6.0 * u * t * (c.p2y - c.p1y) + 3.0 * t * t * (c.p3y - c.p2y);
}

void refPoint(const RefCubic& c, double t, double& x, double& y)
{
    double u = 1.0 - t;
    double a = u * u * u, b = 3.0 * u * u * t, d = 3.0 * u * t * t, e = t * t * t;
    x = a * c.p0x + b * c.p1x + d * c.p2x + e * c.p3x;
    y = a * c.p0y + b * c.p1y + d * c.p2y + e * c.p3y;
}

double refSpeed(const RefCubic& c, double t)
{
    double dx, dy;
    refDerivative(c, t, dx, dy);
    return std::sqrt(dx * dx + dy * dy);
}

double refSimpson(const RefCubic& c, double a, double b)
{
    double m = (a + b) * 0.5;
    return (b - a) / 6.0 * (refSpeed(c, a) + 4.0 * refSpeed(c, m) + refSpeed(c, b));
}

double refLengthRange(const RefCubic& c, double a, double b, double whole, int depth)
{
    double m = (a + b) * 0.5;
    double left = refSimpson(c, a, m);
    double right = refSimpson(c, m, b);
    if(depth < 40 && std::fabs(left + right - whole) > 1e-11)
        return refLengthRange(c, a, m, left, depth + 1) + refLengthRange(c, m, b, right, depth + 1);
    return left + right;
}

double refLength(const RefCubic& c)
{
    return refLengthRange(c, 0.0, 1.0, refSimpson(c, 0.0, 1.0), 0);
}

double refParameterAtDistance(const RefCubic& c, double distance)
{
    double lo = 0.0, hi = 1.0;
    for(int i = 0; i < 80; ++i) {
        double t = (lo + hi) * 0.5;
        double current = refLengthRange(c, 0.0, t, refSimpson(c, 0.0, t), 0);
        if(current < distance) lo = t; else hi = t;
    }
    return (lo + hi) * 0.5;
}

// Fixture font metrics, units/em = 1000 (see support/font_fixture.hpp).
constexpr double kAdvanceA = 0.6;
constexpr double kAdvanceB = 0.8;
constexpr double kAdvanceSpace = 0.25;

std::unique_ptr<Document> load(const std::string& svg)
{
    UNIT_CHECK(text_path_fixture::install());
    auto document = Document::loadFromData(svg);
    if(document)
        document->updateLayout();
    return document;
}

bool near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

// Rotated glyphs shift their ink relative to the baseline midpoint, so check
// coverage within a small radius instead of a single exact pixel.
bool coveredNear(const Bitmap& bitmap, int cx, int cy, int radius)
{
    for(int y = cy - radius; y <= cy + radius; ++y) {
        for(int x = cx - radius; x <= cx + radius; ++x) {
            if(x >= 0 && y >= 0 && x < bitmap.width() && y < bitmap.height()
                && text_path_fixture::alpha(bitmap, x, y) != 0) {
                return true;
            }
        }
    }
    return false;
}

// A deliberately non-uniform cubic: its asymmetric speed function makes the
// arc length midpoint differ from the parameter midpoint.
Path nonUniformPath()
{
    Path path;
    path.moveTo(0.f, 0.f);
    path.cubicTo(120.f, 0.f, 10.f, 100.f, 100.f, 100.f);
    return path;
}

RefCubic nonUniformCubic()
{
    return RefCubic{0, 0, 120, 0, 10, 100, 100, 100};
}

void case_measure_length_and_samples()
{
    auto path = nonUniformPath();
    PathMeasure measure(path, Transform::Identity);
    UNIT_CHECK(measure.valid());
    UNIT_CHECK(near(measure.length(), refLength(nonUniformCubic()), 0.01));

    double half = measure.length() * 0.5;
    Point position, tangent;
    UNIT_CHECK(measure.sample(0.0, position, tangent));
    UNIT_CHECK(near(position.x, 0.0, 0.01) && near(position.y, 0.0, 0.01));
    UNIT_CHECK(measure.sample(measure.length(), position, tangent));
    UNIT_CHECK(near(position.x, 100.0, 0.01) && near(position.y, 100.0, 0.01));

    UNIT_CHECK(measure.sample(half, position, tangent));
    double t = refParameterAtDistance(nonUniformCubic(), half);
    double rx, ry;
    refPoint(nonUniformCubic(), t, rx, ry);
    UNIT_CHECK(near(position.x, rx, 0.01) && near(position.y, ry, 0.01));
    double dx, dy;
    refDerivative(nonUniformCubic(), t, dx, dy);
    double speed = std::sqrt(dx * dx + dy * dy);
    UNIT_CHECK(near(std::atan2(tangent.y, tangent.x), std::atan2(dy / speed, dx / speed), 0.001));

    // Arc length midpoint must not coincide with the parameter midpoint.
    double px, py;
    refPoint(nonUniformCubic(), 0.5, px, py);
    UNIT_CHECK(std::hypot(position.x - px, position.y - py) > 2.0);

    // Out of range and non-finite distances fail and leave outputs untouched.
    Point sentry(position), stangent(tangent);
    UNIT_CHECK(!measure.sample(-0.1, position, tangent));
    UNIT_CHECK(!measure.sample(measure.length() + 0.1, position, tangent));
    UNIT_CHECK(!measure.sample(std::numeric_limits<double>::infinity(), position, tangent));
    UNIT_CHECK(!measure.sample(std::numeric_limits<double>::quiet_NaN(), position, tangent));
    UNIT_CHECK(position.x == sentry.x && position.y == sentry.y);
    UNIT_CHECK(tangent.x == stangent.x && tangent.y == stangent.y);
}

void case_transformed_path_measurement()
{
    auto path = nonUniformPath();
    auto transform = Transform::translated(20.f, -10.f) * Transform::scaled(2.f, 3.f);
    PathMeasure measure(path, transform);
    UNIT_CHECK(measure.valid());

    auto map = [](const RefCubic& c, double x, double y) {
        return std::pair<double, double>(20.0 + 2.0 * x, -10.0 + 3.0 * y);
    };
    const auto& source = nonUniformCubic();
    auto p0 = map(source, source.p0x, source.p0y);
    auto p1 = map(source, source.p1x, source.p1y);
    auto p2 = map(source, source.p2x, source.p2y);
    auto p3 = map(source, source.p3x, source.p3y);
    RefCubic transformed{p0.first, p0.second, p1.first, p1.second, p2.first, p2.second, p3.first, p3.second};
    UNIT_CHECK(near(measure.length(), refLength(transformed), 0.01));

    Point position, tangent;
    double distance = measure.length() * 0.25;
    UNIT_CHECK(measure.sample(distance, position, tangent));
    double t = refParameterAtDistance(transformed, distance);
    double rx, ry;
    refPoint(transformed, t, rx, ry);
    UNIT_CHECK(near(position.x, rx, 0.01) && near(position.y, ry, 0.01));

    // An empty path is not a valid open subpath.
    Path empty;
    PathMeasure emptyMeasure(empty, Transform::Identity);
    UNIT_CHECK(!emptyMeasure.valid() && emptyMeasure.length() == 0.0);
}

void case_mixed_segments_and_invalid_measure()
{
    // Line plus quadratic: the parser lowers the quad to cubics, so this also
    // checks that round trip. Reference length integrates each piece directly.
    Path path;
    path.parse("M0 0 L40 0 Q80 0 80 40", std::strlen("M0 0 L40 0 Q80 0 80 40"));
    PathMeasure measure(path, Transform::Identity);
    UNIT_CHECK(measure.valid());

    // Reference: 40 unit line plus the exact length of the quadratic
    // (40,0)->(80,0)->(80,40). Compute it with dense numeric integration.
    auto quadSpeed = [](double t) {
        double dx = 2.0 * ((1.0 - t) * 40.0 + t * 0.0);
        double dy = 2.0 * ((1.0 - t) * 0.0 + t * 40.0);
        return std::sqrt(dx * dx + dy * dy);
    };
    double quadLength = 0.0;
    int n = 100000;
    for(int i = 0; i < n; ++i)
        quadLength += quadSpeed((i + 0.5) / n) / n;
    UNIT_CHECK(near(measure.length(), 40.0 + quadLength, 0.01));

    Point position, tangent;
    UNIT_CHECK(measure.sample(20.0, position, tangent));  // inside the line
    UNIT_CHECK(near(position.y, 0.0, 0.01));
    UNIT_CHECK(measure.sample(measure.length(), position, tangent));
    UNIT_CHECK(near(position.x, 80.0, 0.01) && near(position.y, 40.0, 0.01));

    Path closed;
    closed.parse("M0 0 H40 V40 Z", std::strlen("M0 0 H40 V40 Z"));
    UNIT_CHECK(!PathMeasure(closed, Transform::Identity).valid());

    Path multi;
    multi.parse("M0 0 H20 M30 0 H50", std::strlen("M0 0 H20 M30 0 H50"));
    UNIT_CHECK(!PathMeasure(multi, Transform::Identity).valid());

    // A second, empty move still means more than one subpath.
    Path multiEmpty;
    multiEmpty.parse("M0 0 M10 10 H30", std::strlen("M0 0 M10 10 H30"));
    UNIT_CHECK(!PathMeasure(multiEmpty, Transform::Identity).valid());
}

void case_horizontal_layout_continuity()
{
    // Straight horizontal baseline; glyph positions follow fixture advances.
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="100">
<path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" fill="black">
<textPath href="#p"><tspan font-size="20">A</tspan><tspan font-size="10">B</tspan></textPath>
</text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    auto box = document->getElementById("t").getBoundingBox();
    // A: size 20, advance 12, midpoint at distance 6 -> center (16, 50),
    // glyph x range [10, 18]. B: size 10 starts at distance 12, midpoint 16
    // -> center (26, 50), x range [22.5, 28.5]. The tspan size change must
    // not restart the one dimensional distance axis.
    UNIT_CHECK(near(box.x, 10.0, 0.5));
    UNIT_CHECK(near(box.x + box.w, 28.5, 0.5));
    UNIT_CHECK(near(box.y, 36.0, 0.5));
    UNIT_CHECK(near(box.y + box.h, 50.0, 0.5));

    auto bitmap = document->renderToBitmap(220, 100);
    UNIT_CHECK(!bitmap.isNull());
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 14, 45) != 0);  // inside A
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 26, 46) != 0);  // inside B
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 2, 2) == 0);    // outside both
}

void case_href_priority()
{
    // Both references present: href wins over xlink:href.
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="220" height="120">
<path id="p1" d="M10 90 H200" fill="none"/>
<path id="p2" d="M10 30 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#p2" xlink:href="#p1">A</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    auto bitmap = document->renderToBitmap(220, 120);
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 14, 25) != 0);  // on p2 baseline
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 14, 85) == 0);  // not on p1
}

void case_spacing_and_tspan_paint()
{
    auto plain = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="100"><path id="p" d="M10 50 H230" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p">A B</textPath></text></svg>)svg");
    auto spaced = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="100"><path id="p" d="M10 50 H230" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black" letter-spacing="4" word-spacing="10"><textPath href="#p">A B</textPath></text></svg>)svg");
    UNIT_CHECK(plain && spaced);
    auto pb = plain->getElementById("t").getBoundingBox();
    auto sb = spaced->getElementById("t").getBoundingBox();
    // A + space + B natural span; spacing widens it: two letter gaps plus the
    // extra word gap on the space.
    UNIT_CHECK(sb.w > pb.w + 8.f);

    // Different fill and stroke across nested tspans still render, and both
    // contribute to the same shared layout without restarting the axis.
    auto painted = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120"><path id="p" d="M10 60 H230" fill="none"/>
<text id="t" font-family="Fixture" font-size="20">
<textPath href="#p"><tspan fill="red">A</tspan><tspan fill="blue" stroke="black" stroke-width="1">B</tspan></textPath>
</text></svg>)svg");
    UNIT_CHECK(painted != nullptr);
    auto bitmap = painted->renderToBitmap(240, 120);
    // First glyph ink near distance 6 -> around x=16; second starts after A.
    UNIT_CHECK(coveredNear(bitmap, 16, 54, 7));
    UNIT_CHECK(coveredNear(bitmap, 30, 54, 9));
    auto box = painted->getElementById("t").getBoundingBox();
    UNIT_CHECK(box.w > 0.f && box.h > 0.f);
}

void case_xlink_href_fallback()
{
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="220" height="100">
<path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath xlink:href="#p">A</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    auto box = document->getElementById("t").getBoundingBox();
    UNIT_CHECK(near(box.x, 10.0, 0.5) && near(box.y + box.h, 50.0, 0.5));
    auto bitmap = document->renderToBitmap(220, 100);
    UNIT_CHECK(text_path_fixture::alpha(bitmap, 14, 45) != 0);
}

void case_invalid_paths_are_hidden()
{
    const std::string cases[] = {
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120"><path id="p" d="M10 10 H90 V80 Z"/><text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p">AB</textPath></text><rect id="r" x="2" y="100" width="20" height="16" fill="red"/></svg>)svg",
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120"><path id="p" d="M0 0 H50 M60 0 H100"/><text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p">AB</textPath></text><rect id="r" x="2" y="100" width="20" height="16" fill="red"/></svg>)svg",
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120"><path id="p" d="M10 10 L10 10"/><text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p">AB</textPath></text><rect id="r" x="2" y="100" width="20" height="16" fill="red"/></svg>)svg",
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120"><text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#missing">AB</textPath></text><rect id="r" x="2" y="100" width="20" height="16" fill="red"/></svg>)svg"
    };

    for(const auto& svg : cases) {
        auto document = load(svg);
        UNIT_CHECK(document != nullptr);
        auto text = document->getElementById("t");
        auto box = text.getBoundingBox();
        UNIT_CHECK(box.w == 0.f && box.h == 0.f);
        auto bitmap = document->renderToBitmap(120, 120);
        // No horizontal fallback text near the origin; sibling rect still paints.
        UNIT_CHECK(text_path_fixture::alpha(bitmap, 5, 5) == 0);
        UNIT_CHECK(text_path_fixture::alpha(bitmap, 10, 108) != 0);
    }
}

void case_out_of_range_glyphs()
{
    // Natural layout at size 20: A 12, B 16 each. Path length 100.
    auto negative = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="160" height="100">
<path id="p" d="M10 50 H110" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#p" startOffset="-30">ABBB</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(negative != nullptr);
    auto box = negative->getElementById("t").getBoundingBox();
    // A and the first B fall off the start and must not be clamped onto it.
    UNIT_CHECK(box.x > 7.f);
    // Later in-range glyphs still advance and render near distance 22.
    UNIT_CHECK(near(box.x + box.w, 37.0, 1.0));
    auto bitmap = negative->renderToBitmap(160, 100);
    UNIT_CHECK(text_path_fixture::covered(bitmap) > 0);

    auto beyond = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="160" height="100">
<path id="p" d="M10 50 H110" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#p" startOffset="150%">AB</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(beyond != nullptr);
    auto hiddenBox = beyond->getElementById("t").getBoundingBox();
    UNIT_CHECK(hiddenBox.w == 0.f && hiddenBox.h == 0.f);
    auto hiddenBitmap = beyond->renderToBitmap(160, 100);
    UNIT_CHECK(text_path_fixture::covered(hiddenBitmap) == 0);
}

void case_curved_bitmap_and_bbox()
{
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="180" height="120">
<path id="p" d="M10 90 C40 10 120 10 150 90" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#p">AB</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);

    RefCubic curve{10, 90, 40, 10, 120, 10, 150, 90};
    double length = refLength(curve);
    // A midpoint at distance 6, B start 12, midpoint 20.
    double centers[2];
    centers[0] = 6.0;
    centers[1] = 20.0;
    Point expected[2];
    for(int i = 0; i < 2; ++i) {
        double t = refParameterAtDistance(curve, centers[i]);
        double x, y;
        refPoint(curve, t, x, y);
        expected[i] = Point(float(x), float(y));
    }

    auto box = document->getElementById("t").getBoundingBox();
    for(const auto& point : expected) {
        UNIT_CHECK(box.x <= point.x && point.x <= box.x + box.w);
        UNIT_CHECK(box.y <= point.y && point.y <= box.y + box.h);
    }
    // Rotated glyphs produce a taller, slanted box than a horizontal line.
    UNIT_CHECK(box.h > 16.f);

    auto bitmap = document->renderToBitmap(180, 120);
    for(const auto& point : expected) {
        int x = int(std::lround(point.x));
        int y = int(std::lround(point.y)) - 6;
        UNIT_CHECK(coveredNear(bitmap, x, y, 7));
    }
    UNIT_CHECK(text_path_fixture::covered(bitmap) > 0);
}

void case_ancestor_transform_excluded()
{
    // The path's own transform places the baseline; the ancestor group's
    // transform must not participate in the measurement.
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="120">
<g transform="translate(50,50)">
<path id="p" transform="translate(20,10) scale(2,1)" d="M0 0 C10 40 60 40 80 0" fill="none"/>
</g>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#p">A</textPath>
</text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    RefCubic local{0, 0, 10, 40, 60, 40, 80, 0};
    double t = refParameterAtDistance(local, 6.0 / 2.0); // x scaled by 2
    double x, y;
    refPoint(local, t, x, y);
    Point expected(float(20.0 + 2.0 * x), float(10.0 + y));
    auto bitmap = document->renderToBitmap(220, 120);
    int px = int(std::lround(expected.x));
    int py = int(std::lround(expected.y)) - 6;
    UNIT_CHECK(coveredNear(bitmap, px, py, 7));
    // A point shifted by the ancestor group translate must stay empty.
    UNIT_CHECK(!coveredNear(bitmap, px + 50, py + 50, 3));
}

void case_textlength_modes_and_offsets()
{
    auto natural = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="100"><path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p">AB</textPath></text></svg>)svg");
    auto spacing = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="100"><path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p" textLength="40" lengthAdjust="spacing">AB</textPath></text></svg>)svg");
    auto glyphs = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="100"><path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p" textLength="40" lengthAdjust="spacingAndGlyphs">AB</textPath></text></svg>)svg");
    UNIT_CHECK(natural && spacing && glyphs);

    auto nb = natural->getElementById("t").getBoundingBox();
    auto sb = spacing->getElementById("t").getBoundingBox();
    auto gb = glyphs->getElementById("t").getBoundingBox();
    UNIT_CHECK(sb.w > nb.w + 1.f);
    UNIT_CHECK(gb.w > nb.w + 1.f);
    UNIT_CHECK(std::fabs(sb.w - gb.w) > 1.f);
    // spacingAndGlyphs never scales the glyph height.
    UNIT_CHECK(near(gb.h, nb.h, 0.01));

    // dx accumulates on the distance axis, dy along the downward normal and
    // rotate tilts the glyph, growing the axis aligned box height.
    auto offsets = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="100"><path id="p" d="M10 50 H200" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black"><textPath href="#p"><tspan dx="8" dy="6" rotate="30">A</tspan></textPath></text></svg>)svg");
    UNIT_CHECK(offsets != nullptr);
    auto ob = offsets->getElementById("t").getBoundingBox();
    UNIT_CHECK(ob.x > nb.x + 5.f);
    UNIT_CHECK(ob.h > nb.h + 1.f);
}

void case_path_declared_after_text()
{
    // The referenced path appears later in document order; its first layout
    // pass has not run when the text is laid out.
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="120">
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath href="#late">A</textPath></text>
<path id="late" d="M12 80 H200" fill="none"/>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    auto box = document->getElementById("t").getBoundingBox();
    // A midpoint at distance 6 lands on (18, 80); glyph x range [12, 20].
    UNIT_CHECK(near(box.x, 12.0, 0.5) && near(box.y + box.h, 80.0, 0.5));
    auto bitmap = document->renderToBitmap(220, 120);
    UNIT_CHECK(coveredNear(bitmap, 18, 74, 7));

    // Mutating the still-referenced later path moves the glyph on the next pass.
    document->getElementById("late").setAttribute("d", "M12 30 H200");
    document->updateLayout();
    auto moved = document->getElementById("t").getBoundingBox();
    UNIT_CHECK(near(moved.y + moved.h, 30.0, 0.5));
}

void case_text_anchor_whole_span()
{
    // Natural size-20 span: A 12 then B 16 = 28 units. With startOffset at
    // distance 50, middle anchoring centers the 28-wide span on that point.
    auto make = [](const char* anchor) {
        std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
            "<path id=\"p\" d=\"M0 50 H190\" fill=\"none\"/>"
            "<text id=\"t\" font-family=\"Fixture\" font-size=\"20\" text-anchor=\"";
        svg += anchor;
        svg += "\"><textPath href=\"#p\" startOffset=\"50\">"
               "<tspan>A</tspan><tspan>B</tspan></textPath></text></svg>";
        return load(svg);
    };

    auto start = make("start");
    auto middle = make("middle");
    auto end = make("end");
    UNIT_CHECK(start && middle && end);
    auto sb = start->getElementById("t").getBoundingBox();
    auto mb = middle->getElementById("t").getBoundingBox();
    auto eb = end->getElementById("t").getBoundingBox();

    // Start: first glyph begins at distance 50 -> left edge near x=50.
    UNIT_CHECK(near(sb.x, 50.0, 0.5));
    // Middle: span centered on 50 -> left edge near 50 - 14 = 36.
    UNIT_CHECK(near(mb.x, 36.0, 0.5));
    // End: span ends on 50 -> left edge near 50 - 28 = 22.
    UNIT_CHECK(near(eb.x, 22.0, 0.5));
    // Anchoring never restarts inside the tspan boundary: the total width is
    // preserved across all three modes.
    UNIT_CHECK(near(sb.w, mb.w, 0.01) && near(mb.w, eb.w, 0.01));
}

void case_mutation_and_shared_path()
{
    auto document = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="220" height="120">
<path id="p" d="M10 60 H120" fill="none"/>
<text id="t" font-family="Fixture" font-size="20" fill="black">
<textPath id="tp" href="#p">AB</textPath></text>
</svg>)svg");
    UNIT_CHECK(document != nullptr);
    auto before = document->renderToBitmap(220, 120);
    auto beforeCoverage = text_path_fixture::covered(before);
    auto beforeBox = document->getElementById("t").getBoundingBox();

    document->getElementById("p").setAttribute("d", "M10 100 H200");
    document->getElementById("tp").setAttribute("startOffset", "40");
    document->updateLayout();
    auto after = document->renderToBitmap(220, 120);
    auto afterCoverage = text_path_fixture::covered(after);
    auto afterBox = document->getElementById("t").getBoundingBox();
    UNIT_CHECK(afterBox.y != beforeBox.y || afterBox.x != beforeBox.x);
    UNIT_CHECK(afterCoverage != beforeCoverage || afterBox.y > beforeBox.y + 1.f);

    document->getElementById("tp").setAttribute("textLength", "60");
    document->updateLayout();
    auto stretchedBox = document->getElementById("t").getBoundingBox();
    UNIT_CHECK(stretchedBox.w > afterBox.w + 1.f);

    // Changing the referenced path's own transform re-measures and relocates.
    document->getElementById("p").setAttribute("transform", "translate(0,20)");
    document->updateLayout();
    auto movedByTransform = document->getElementById("t").getBoundingBox();
    UNIT_CHECK(near(movedByTransform.y + movedByTransform.h, 120.0, 0.5));

    // One path referenced by two text elements with different startOffsets:
    // their layouts must remain independent.
    auto shared = load(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120">
<path id="p" d="M10 60 H220" fill="none"/>
<text id="t1" font-family="Fixture" font-size="20" fill="black"><textPath href="#p" startOffset="0">AB</textPath></text>
<text id="t2" font-family="Fixture" font-size="20" fill="black"><textPath href="#p" startOffset="50">AB</textPath></text>
</svg>)svg");
    UNIT_CHECK(shared != nullptr);
    auto b1 = shared->getElementById("t1").getBoundingBox();
    auto b2 = shared->getElementById("t2").getBoundingBox();
    UNIT_CHECK(std::fabs(b2.x - b1.x) > 40.f);
    auto sharedBitmap = shared->renderToBitmap(240, 120);
    UNIT_CHECK(text_path_fixture::covered(sharedBitmap) > 0);
}

} // namespace

const unit_case unit_cases[] = {
    {"measure_length_and_samples", "normal", case_measure_length_and_samples},
    {"transformed_path_measurement", "normal", case_transformed_path_measurement},
    {"mixed_segments_and_invalid_measure", "normal", case_mixed_segments_and_invalid_measure},
    {"horizontal_layout_continuity", "normal", case_horizontal_layout_continuity},
    {"spacing_and_tspan_paint", "normal", case_spacing_and_tspan_paint},
    {"href_priority", "normal", case_href_priority},
    {"xlink_href_fallback", "normal", case_xlink_href_fallback},
    {"invalid_paths_are_hidden", "boundary", case_invalid_paths_are_hidden},
    {"out_of_range_glyphs", "boundary", case_out_of_range_glyphs},
    {"text_anchor_whole_span", "boundary", case_text_anchor_whole_span},
    {"curved_bitmap_and_bbox", "integration", case_curved_bitmap_and_bbox},
    {"ancestor_transform_excluded", "integration", case_ancestor_transform_excluded},
    {"path_declared_after_text", "integration", case_path_declared_after_text},
    {"textlength_modes_and_offsets", "integration", case_textlength_modes_and_offsets},
    {"mutation_and_shared_path", "integration", case_mutation_and_shared_path},
};

const unsigned unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);