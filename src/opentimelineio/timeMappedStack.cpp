// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/timeMappedStack.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/item.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_set>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

// Exact rational seconds for the integer frame/time-base combinations used by
// the task.  __int128 keeps intermediate frame products exact.
struct Frac
{
    __int128 n;
    __int128 d; // always positive
};

__int128
abs128(__int128 x)
{
    return x < 0 ? -x : x;
}

__int128
gcd128(__int128 a, __int128 b)
{
    a = abs128(a);
    b = abs128(b);
    while (b != 0)
    {
        __int128 r = a % b;
        a          = b;
        b          = r;
    }
    return a ? a : 1;
}

Frac
norm_frac(__int128 n, __int128 d)
{
    if (d < 0)
    {
        n = -n;
        d = -d;
    }
    if (n == 0)
    {
        return { 0, 1 };
    }
    const __int128 g = gcd128(n, d);
    return { n / g, d / g };
}

Frac
add_frac(Frac a, Frac b)
{
    return norm_frac(a.n * b.d + b.n * a.d, a.d * b.d);
}

Frac
sub_frac(Frac a, Frac b)
{
    b.n = -b.n;
    return add_frac(a, b);
}

Frac
mul_frac(Frac a, Frac b)
{
    return norm_frac(a.n * b.n, a.d * b.d);
}

Frac
div_frac(Frac a, Frac b)
{
    return norm_frac(a.n * b.d, a.d * b.n);
}

int
cmp_frac(Frac a, Frac b)
{
    const __int128 lhs = a.n * b.d;
    const __int128 rhs = b.n * a.d;
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

double
frac_to_double(Frac f)
{
    return static_cast<double>(static_cast<long double>(f.n)
                               / static_cast<long double>(f.d));
}

// Convert an ordinary finite double to an exact binary rational.  Production
// times are small enough that the shifted denominator fits in __int128.
Frac
double_to_frac(double x)
{
    if (x == 0.0)
    {
        return { 0, 1 };
    }

    int exponent = 0;
    const double mantissa = std::frexp(x, &exponent);
    const long long integer_mantissa =
        std::llround(std::ldexp(mantissa, 53));
    int power = exponent - 53;

    __int128 n = integer_mantissa;
    __int128 d = 1;
    if (power >= 0)
    {
        n <<= power;
    }
    else
    {
        d <<= -power;
    }
    return norm_frac(n, d);
}

Frac
time_frac(RationalTime t)
{
    return div_frac(double_to_frac(t.value()), double_to_frac(t.rate()));
}

RationalTime
frac_to_time(Frac f, double rate)
{
    if (std::isfinite(rate) && rate > 0.0 && rate == std::floor(rate))
    {
        Frac values = mul_frac(f, { static_cast<__int128>(rate), 1 });
        return RationalTime(frac_to_double(values), rate);
    }
    return RationalTime(frac_to_double(f) * rate, rate);
}

enum class Bias
{
    half_open,
    left
};

struct MappedPoint
{
    Frac source;
    Bias bias;
    int  segment;
};

void
set_error(
    ErrorStatus& e,
    ErrorStatus::Outcome outcome,
    std::string const&    details,
    SerializableObject const* object = nullptr)
{
    e = ErrorStatus(outcome, details, object);
}

bool
is_active_time_map(Composition const* composition)
{
    auto const* tms = dynamic_cast<TimeMappedStack const*>(composition);
    return tms && tms->has_time_map();
}

bool
validate_item_effects(Item const* item, ErrorStatus& e)
{
    if (!item)
    {
        return true;
    }

    for (auto const& effect_retainer: item->effects())
    {
        Effect const* effect = effect_retainer.value;
        if (effect && effect->enabled()
            && dynamic_cast<TimeEffect const*>(effect))
        {
            set_error(
                e,
                ErrorStatus::NOT_IMPLEMENTED,
                "TimeMappedStack sampling does not support TimeEffect",
                item);
            return false;
        }
    }
    return true;
}

bool
check_hit_child_support(Composable const* child, ErrorStatus& e)
{
    if (auto const* transition = dynamic_cast<Transition const*>(child))
    {
        if (transition->enabled())
        {
            set_error(
                e,
                ErrorStatus::NOT_IMPLEMENTED,
                "TimeMappedStack sampling does not support transitions",
                child);
            return false;
        }
    }

    if (auto const* item = dynamic_cast<Item const*>(child))
    {
        if (item->enabled() && !validate_item_effects(item, e))
        {
            return false;
        }
    }
    return true;
}

bool
map_output_point(
    TimeMappedStack const* tms,
    Frac                   output_time,
    Bias                   output_bias,
    MappedPoint&           mapped,
    ErrorStatus&           e)
{
    auto const& map = tms->time_map();
    if (map.size() < 2)
    {
        mapped = { output_time, Bias::half_open, -1 };
        return true;
    }

    std::vector<Frac> outputs;
    outputs.reserve(map.size());
    for (auto const& p: map)
    {
        outputs.push_back(time_frac(p.first));
    }

    int segment = -1;
    if (output_bias == Bias::half_open)
    {
        for (int i = 0; i + 1 < int(map.size()); ++i)
        {
            if (cmp_frac(outputs[i], output_time) <= 0
                && cmp_frac(output_time, outputs[i + 1]) < 0)
            {
                segment = i;
                break;
            }
        }
    }
    else
    {
        for (int i = 0; i + 1 < int(map.size()); ++i)
        {
            if (cmp_frac(outputs[i], output_time) < 0
                && cmp_frac(output_time, outputs[i + 1]) <= 0)
            {
                segment = i;
                break;
            }
        }
    }

    if (segment < 0)
    {
        set_error(
            e,
            ErrorStatus::INVALID_TIME_RANGE,
            "TimeMappedStack sample time is outside the time map",
            tms);
        return false;
    }

    Frac out0   = outputs[segment];
    Frac out1   = outputs[segment + 1];
    Frac src0   = time_frac(map[segment].second);
    Frac src1   = time_frac(map[segment + 1].second);
    Frac span   = sub_frac(out1, out0);
    Frac factor = div_frac(sub_frac(output_time, out0), span);
    Frac source = add_frac(src0, mul_frac(sub_frac(src1, src0), factor));

    Bias source_bias = Bias::half_open;
    if (cmp_frac(src0, src1) != 0)
    {
        bool const forward  = cmp_frac(src0, src1) < 0;
        bool const out_right = output_bias == Bias::half_open;
        // Forward output motion preserves side; reverse output motion flips
        // it so an exclusive source end is attributed to the left clip.
        source_bias = (forward == out_right) ? Bias::half_open : Bias::left;
    }

    mapped = { source, source_bias, segment };
    return true;
}

bool sample_children(
    Composition const* comp,
    Frac               t,
    Bias               bias,
    std::vector<TimeMappedStack::Sample>& samples,
    ErrorStatus&       e);

bool
sample_time_mapped(
    TimeMappedStack const* tms,
    Frac                   output_time,
    Bias                   output_bias,
    std::vector<TimeMappedStack::Sample>& samples,
    ErrorStatus&           e)
{
    if (!validate_item_effects(tms, e))
    {
        return false;
    }

    if (!tms->has_time_map())
    {
        return sample_children(tms, output_time, output_bias, samples, e);
    }

    MappedPoint mapped;
    if (!map_output_point(tms, output_time, output_bias, mapped, e))
    {
        return false;
    }
    return sample_children(tms, mapped.source, mapped.bias, samples, e);
}

bool
sample_children(
    Composition const* comp,
    Frac               t,
    Bias               bias,
    std::vector<TimeMappedStack::Sample>& samples,
    ErrorStatus&       e)
{
    if (!validate_item_effects(comp, e))
    {
        return false;
    }

    auto const& children = comp->children();
    for (int i = 0; i < int(children.size()); ++i)
    {
        Composable const* child = children[i].value;
        if (!child)
        {
            continue;
        }

        auto const* item = dynamic_cast<Item const*>(child);
        if (item && !item->enabled())
        {
            continue;
        }

        ErrorStatus child_error;
        TimeRange   layout =
            comp->range_of_child_at_index(i, &child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }

        Frac start = time_frac(layout.start_time());
        Frac finish = add_frac(start, time_frac(layout.duration()));

        bool hit = false;
        if (bias == Bias::half_open)
        {
            hit = cmp_frac(start, t) <= 0 && cmp_frac(t, finish) < 0;
        }
        else
        {
            hit = cmp_frac(start, t) < 0 && cmp_frac(t, finish) <= 0;
        }
        if (!hit)
        {
            continue;
        }

        if (!check_hit_child_support(child, e))
        {
            return false;
        }

        if (!item)
        {
            continue;
        }

        TimeRange trimmed = item->trimmed_range(&child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }

        Frac child_time = add_frac(
            time_frac(trimmed.start_time()),
            sub_frac(t, start));

        if (auto const* clip = dynamic_cast<Clip const*>(item))
        {
            double rate = trimmed.start_time().rate();
            if (!(rate > 0.0))
            {
                rate = trimmed.duration().rate();
            }
            samples.push_back(
                { clip, frac_to_time(child_time, rate > 0 ? rate : 1.0) });
        }
        else if (auto const* child_comp =
                     dynamic_cast<Composition const*>(item))
        {
            if (auto const* tms =
                    dynamic_cast<TimeMappedStack const*>(child_comp))
            {
                if (!sample_time_mapped(
                        tms,
                        child_time,
                        bias,
                        samples,
                        e))
                {
                    return false;
                }
            }
            else if (!sample_children(
                         child_comp,
                         child_time,
                         bias,
                         samples,
                         e))
            {
                return false;
            }
        }
    }

    return true;
}

bool collect_point_hits(
    Composition const* comp,
    Frac               t,
    Bias               bias,
    bool               recurse,
    std::set<Clip const*>&   hits,
    ErrorStatus&       e);

bool collect_output_range(
    Composition const* comp,
    Frac               start,
    Frac               finish,
    bool               recurse,
    std::set<Clip const*>&   hits,
    ErrorStatus&       e);

bool
collect_point_hits(
    Composition const* comp,
    Frac               t,
    Bias               bias,
    bool               recurse,
    std::set<Clip const*>&   hits,
    ErrorStatus&       e)
{
    if (!validate_item_effects(comp, e))
    {
        return false;
    }

    auto const& children = comp->children();
    for (int i = 0; i < int(children.size()); ++i)
    {
        Composable const* child = children[i].value;
        if (!child)
        {
            continue;
        }
        auto const* item = dynamic_cast<Item const*>(child);
        if (item && !item->enabled())
        {
            continue;
        }

        ErrorStatus child_error;
        TimeRange   layout = comp->range_of_child_at_index(i, &child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }

        Frac start  = time_frac(layout.start_time());
        Frac finish = add_frac(start, time_frac(layout.duration()));
        bool hit    = false;
        if (bias == Bias::half_open)
        {
            hit = cmp_frac(start, t) <= 0 && cmp_frac(t, finish) < 0;
        }
        else
        {
            hit = cmp_frac(start, t) < 0 && cmp_frac(t, finish) <= 0;
        }
        if (!hit)
        {
            continue;
        }
        if (!check_hit_child_support(child, e))
        {
            return false;
        }
        if (!item)
        {
            continue;
        }

        if (auto const* clip = dynamic_cast<Clip const*>(child))
        {
            hits.insert(clip);
            continue;
        }

        if (!recurse)
        {
            continue;
        }

        TimeRange trimmed = item->trimmed_range(&child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }
        Frac child_time = add_frac(
            time_frac(trimmed.start_time()),
            sub_frac(t, start));

        if (auto const* tms = dynamic_cast<TimeMappedStack const*>(item))
        {
            if (!tms->has_time_map())
            {
                if (!collect_point_hits(
                        tms,
                        child_time,
                        bias,
                        true,
                        hits,
                        e))
                {
                    return false;
                }
                continue;
            }

            MappedPoint mapped;
            if (!map_output_point(tms, child_time, bias, mapped, e))
            {
                return false;
            }
            if (!collect_point_hits(
                    tms,
                    mapped.source,
                    mapped.bias,
                    true,
                    hits,
                    e))
            {
                return false;
            }
        }
        else if (auto const* child_comp =
                     dynamic_cast<Composition const*>(item))
        {
            if (!collect_point_hits(
                    child_comp,
                    child_time,
                    bias,
                    true,
                    hits,
                    e))
            {
                return false;
            }
        }
    }
    return true;
}

bool
collect_internal_range(
    Composition const* comp,
    Frac               range_start,
    Frac               range_finish,
    bool               recurse,
    std::set<Clip const*>&   hits,
    ErrorStatus&       e)
{
    if (cmp_frac(range_start, range_finish) >= 0)
    {
        return true;
    }
    if (!validate_item_effects(comp, e))
    {
        return false;
    }

    auto const& children = comp->children();
    for (int i = 0; i < int(children.size()); ++i)
    {
        Composable const* child = children[i].value;
        if (!child)
        {
            continue;
        }
        auto const* item = dynamic_cast<Item const*>(child);
        if (item && !item->enabled())
        {
            continue;
        }

        ErrorStatus child_error;
        TimeRange   layout = comp->range_of_child_at_index(i, &child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }

        Frac child_start  = time_frac(layout.start_time());
        Frac child_finish =
            add_frac(child_start, time_frac(layout.duration()));
        Frac overlap_start =
            cmp_frac(range_start, child_start) < 0 ? child_start
                                                   : range_start;
        Frac overlap_finish =
            cmp_frac(range_finish, child_finish) > 0 ? child_finish
                                                     : range_finish;

        // A single endpoint of contact has no positive duration contribution.
        if (cmp_frac(overlap_start, overlap_finish) >= 0)
        {
            continue;
        }

        if (!check_hit_child_support(child, e))
        {
            return false;
        }

        if (auto const* clip = dynamic_cast<Clip const*>(child))
        {
            hits.insert(clip);
            continue;
        }

        if (!recurse || !item)
        {
            continue;
        }

        TimeRange trimmed = item->trimmed_range(&child_error);
        if (is_error(child_error))
        {
            e = child_error;
            return false;
        }

        Frac local_start = sub_frac(overlap_start, child_start);
        Frac mapped_start =
            add_frac(time_frac(trimmed.start_time()), local_start);
        Frac mapped_finish = add_frac(
            mapped_start,
            sub_frac(overlap_finish, overlap_start));

        if (auto const* child_comp =
                dynamic_cast<Composition const*>(item))
        {
            if (!collect_output_range(
                    child_comp,
                    mapped_start,
                    mapped_finish,
                    true,
                    hits,
                    e))
            {
                return false;
            }
        }
    }
    return true;
}

bool
collect_output_range(
    Composition const* comp,
    Frac               start,
    Frac               finish,
    bool               recurse,
    std::set<Clip const*>&   hits,
    ErrorStatus&       e)
{
    if (cmp_frac(start, finish) >= 0)
    {
        return true;
    }

    auto const* tms = dynamic_cast<TimeMappedStack const*>(comp);
    if (!tms || !tms->has_time_map())
    {
        return collect_internal_range(
            comp,
            start,
            finish,
            recurse,
            hits,
            e);
    }

    if (!validate_item_effects(comp, e))
    {
        return false;
    }

    auto const& map = tms->time_map();
    std::vector<Frac> outputs;
    outputs.reserve(map.size());
    for (auto const& p: map)
    {
        outputs.push_back(time_frac(p.first));
    }

    Frac domain_start = outputs.front();
    Frac domain_finish = outputs.back();
    if (cmp_frac(start, domain_start) < 0)
    {
        start = domain_start;
    }
    if (cmp_frac(finish, domain_finish) > 0)
    {
        finish = domain_finish;
    }
    if (cmp_frac(start, finish) >= 0)
    {
        return true;
    }

    std::vector<Frac> cuts{ start };
    for (Frac const& knot: outputs)
    {
        if (cmp_frac(start, knot) < 0 && cmp_frac(knot, finish) < 0)
        {
            cuts.push_back(knot);
        }
    }
    cuts.push_back(finish);

    for (size_t cut = 0; cut + 1 < cuts.size(); ++cut)
    {
        Frac a = cuts[cut];
        Frac b = cuts[cut + 1];
        if (cmp_frac(a, b) >= 0)
        {
            continue;
        }

        MappedPoint at_a;
        if (!map_output_point(tms, a, Bias::half_open, at_a, e))
        {
            return false;
        }
        int segment = at_a.segment;
        if (segment < 0)
        {
            set_error(
                e,
                ErrorStatus::INTERNAL_ERROR,
                "failed to locate TimeMappedStack segment",
                tms);
            return false;
        }

        Frac out0 = outputs[segment];
        Frac out1 = outputs[segment + 1];
        Frac src0 = time_frac(map[segment].second);
        Frac src1 = time_frac(map[segment + 1].second);
        Frac factor_a = div_frac(sub_frac(a, out0), sub_frac(out1, out0));
        Frac factor_b = div_frac(sub_frac(b, out0), sub_frac(out1, out0));
        Frac source_a =
            add_frac(src0, mul_frac(sub_frac(src1, src0), factor_a));
        Frac source_b =
            add_frac(src0, mul_frac(sub_frac(src1, src0), factor_b));

        if (cmp_frac(source_a, source_b) == 0)
        {
            // A held source point is owned by the ordinary half-open leaf.
            if (!collect_point_hits(
                    tms,
                    source_a,
                    Bias::half_open,
                    recurse,
                    hits,
                    e))
            {
                return false;
            }
        }
        else
        {
            Frac lo = cmp_frac(source_a, source_b) < 0 ? source_a : source_b;
            Frac hi = cmp_frac(source_a, source_b) < 0 ? source_b : source_a;
            if (!collect_internal_range(
                    tms,
                    lo,
                    hi,
                    recurse,
                    hits,
                    e))
            {
                return false;
            }
        }
    }

    return true;
}

void
tree_clip_order(Composition const* comp, std::vector<Clip const*>& order)
{
    if (!comp)
    {
        return;
    }
    for (auto const& child_retainer: comp->children())
    {
        Composable const* child = child_retainer.value;
        if (auto const* clip = dynamic_cast<Clip const*>(child))
        {
            order.push_back(clip);
        }
        else if (auto const* child_comp =
                     dynamic_cast<Composition const*>(child))
        {
            tree_clip_order(child_comp, order);
        }
    }
}

bool
tree_is_supported(Composition const* comp, ErrorStatus& e)
{
    if (!comp)
    {
        return true;
    }
    if (!validate_item_effects(comp, e))
    {
        return false;
    }

    for (auto const& child_retainer: comp->children())
    {
        Composable const* child = child_retainer.value;
        if (!child)
        {
            continue;
        }

        if (auto const* item = dynamic_cast<Item const*>(child))
        {
            if (!item->enabled())
            {
                continue;
            }
            if (!validate_item_effects(item, e))
            {
                return false;
            }
        }

        if (auto const* transition =
                dynamic_cast<Transition const*>(child))
        {
            if (transition->enabled())
            {
                set_error(
                    e,
                    ErrorStatus::NOT_IMPLEMENTED,
                    "TimeMappedStack traversal does not support transitions",
                    child);
                return false;
            }
        }

        if (auto const* child_comp =
                dynamic_cast<Composition const*>(child))
        {
            if (!tree_is_supported(child_comp, e))
            {
                return false;
            }
        }
    }
    return true;
}

bool
tree_has_map_impl(Composition const* comp)
{
    if (!comp)
    {
        return false;
    }
    if (is_active_time_map(comp))
    {
        return true;
    }
    for (auto const& child_retainer: comp->children())
    {
        Composable const* child = child_retainer.value;
        auto const* item       = dynamic_cast<Item const*>(child);
        if (item && !item->enabled())
        {
            continue;
        }
        if (auto const* child_comp =
                dynamic_cast<Composition const*>(child))
        {
            if (tree_has_map_impl(child_comp))
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TimeMappedStack::TimeMappedStack(
    std::string const&              name,
    std::optional<TimeRange> const& source_range,
    AnyDictionary const&            metadata,
    std::vector<Effect*> const&      effects,
    std::vector<Marker*> const&     markers)
    : Parent(name, source_range, metadata, effects, markers)
{}

TimeMappedStack::~TimeMappedStack() = default;

bool
TimeMappedStack::set_time_map(
    std::vector<std::pair<RationalTime, RationalTime>> const& time_map,
    ErrorStatus* error_status)
{
    ErrorStatus  local_error;
    ErrorStatus& e = error_status ? *error_status : local_error;
    e              = ErrorStatus();

    if (time_map.size() < 2)
    {
        set_error(
            e,
            ErrorStatus::INVALID_TIME_RANGE,
            "TimeMappedStack time map requires at least two points",
            this);
        return false;
    }

    for (auto const& point: time_map)
    {
        RationalTime const& out = point.first;
        RationalTime const& src = point.second;
        if (!std::isfinite(out.value()) || !std::isfinite(out.rate())
            || !std::isfinite(src.value()) || !std::isfinite(src.rate())
            || !(out.rate() > 0.0) || !(src.rate() > 0.0))
        {
            set_error(
                e,
                ErrorStatus::INVALID_TIME_RANGE,
                "TimeMappedStack time map contains an invalid time",
                this);
            return false;
        }
    }

    std::vector<Frac> outputs;
    outputs.reserve(time_map.size());
    for (auto const& point: time_map)
    {
        outputs.push_back(time_frac(point.first));
    }

    if (cmp_frac(outputs.front(), Frac{ 0, 1 }) != 0)
    {
        set_error(
            e,
            ErrorStatus::INVALID_TIME_RANGE,
            "TimeMappedStack output map must start at zero",
            this);
        return false;
    }

    for (int i = 1; i < int(outputs.size()); ++i)
    {
        if (cmp_frac(outputs[i - 1], outputs[i]) >= 0)
        {
            set_error(
                e,
                ErrorStatus::INVALID_TIME_RANGE,
                "TimeMappedStack output times must be strictly increasing",
                this);
            return false;
        }
    }

    ErrorStatus stack_error;
    TimeRange   stack_range = Stack::available_range(&stack_error);
    if (is_error(stack_error))
    {
        e = stack_error;
        return false;
    }
    Frac stack_duration = time_frac(stack_range.duration());

    for (auto const& point: time_map)
    {
        Frac source = time_frac(point.second);
        if (cmp_frac(source, Frac{ 0, 1 }) < 0
            || cmp_frac(source, stack_duration) > 0)
        {
            set_error(
                e,
                ErrorStatus::INVALID_TIME_RANGE,
                "TimeMappedStack source time is outside Stack range",
                this);
            return false;
        }
    }

    _time_map = time_map;
    return true;
}

TimeRange
TimeMappedStack::available_range(ErrorStatus* error_status) const
{
    if (_time_map.empty())
    {
        return Stack::available_range(error_status);
    }
    RationalTime const& end = _time_map.back().first;
    return TimeRange(RationalTime(0, end.rate()), end);
}

std::vector<TimeMappedStack::Sample>
TimeMappedStack::sample_at(
    RationalTime elapsed,
    ErrorStatus* error_status) const
{
    ErrorStatus  local_error;
    ErrorStatus& e = error_status ? *error_status : local_error;
    e              = ErrorStatus();

    if (!std::isfinite(elapsed.value()) || !std::isfinite(elapsed.rate())
        || !(elapsed.rate() > 0.0))
    {
        set_error(
            e,
            ErrorStatus::INVALID_TIME_RANGE,
            "sample_at requires a valid elapsed time",
            this);
        return {};
    }

    if (!tree_is_supported(this, e))
    {
        if (error_status)
        {
            *error_status = e;
        }
        return {};
    }

    ErrorStatus duration_error;
    RationalTime duration = Item::duration(&duration_error);
    if (is_error(duration_error))
    {
        e = duration_error;
        return {};
    }

    Frac elapsed_frac  = time_frac(elapsed);
    Frac duration_frac = time_frac(duration);
    if (cmp_frac(elapsed_frac, Frac{ 0, 1 }) < 0
        || cmp_frac(elapsed_frac, duration_frac) >= 0)
    {
        set_error(
            e,
            ErrorStatus::INVALID_TIME_RANGE,
            "sample_at elapsed time is outside [0, duration)",
            this);
        return {};
    }

    Frac output_time = elapsed_frac;
    if (auto sr = source_range())
    {
        output_time = add_frac(output_time, time_frac(sr->start_time()));
    }

    std::vector<Sample> samples;
    bool ok = true;
    if (_time_map.empty())
    {
        ok = sample_children(this, output_time, Bias::half_open, samples, e);
    }
    else
    {
        ok = sample_time_mapped(
            this,
            output_time,
            Bias::half_open,
            samples,
            e);
    }

    if (!ok || is_error(e))
    {
        if (error_status)
        {
            *error_status = e;
        }
        return {};
    }
    return samples;
}

bool
TimeMappedStack::read_from(Reader& reader)
{
    std::vector<RationalTime> outputs;
    std::vector<RationalTime> sources;

    // Read typed fields before Parent::read_from moves the reader dictionary
    // into dynamic metadata.
    if (!reader.read_if_present("time_map_outputs", &outputs)
        || !reader.read_if_present("time_map_sources", &sources)
        || !Parent::read_from(reader))
    {
        return false;
    }

    if (outputs.empty() && sources.empty())
    {
        _time_map.clear();
        return true;
    }
    if (outputs.size() != sources.size())
    {
        reader.error(ErrorStatus(
            ErrorStatus::MALFORMED_SCHEMA,
            "TimeMappedStack map arrays have different lengths",
            this));
        return false;
    }

    std::vector<std::pair<RationalTime, RationalTime>> loaded;
    loaded.reserve(outputs.size());
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        loaded.emplace_back(outputs[i], sources[i]);
    }

    // Deserialization finalizes objects in an unspecified order, so child
    // durations may not be available for set_time_map() validation yet.
    // Serialized maps are already validated by the public setter; retain the
    // data directly and validate new edits through set_time_map().
    _time_map = loaded;
    return true;
}

void
TimeMappedStack::write_to(Writer& writer) const
{
    Parent::write_to(writer);

    std::vector<RationalTime> outputs;
    std::vector<RationalTime> sources;
    outputs.reserve(_time_map.size());
    sources.reserve(_time_map.size());
    for (auto const& point: _time_map)
    {
        outputs.push_back(point.first);
        sources.push_back(point.second);
    }
    writer.write("time_map_outputs", outputs);
    writer.write("time_map_sources", sources);
}

bool
tree_has_active_time_map(Composition const* root)
{
    return tree_has_map_impl(root);
}

bool
time_mapped_find_children_clip(
    Composition const* root,
    ErrorStatus* error_status,
    std::optional<TimeRange> const& search_range,
    bool shallow_search,
    std::vector<SerializableObject::Retainer<Clip>>* result)
{
    if (!result)
    {
        return false;
    }
    result->clear();

    ErrorStatus  local_error;
    ErrorStatus& e = error_status ? *error_status : local_error;
    e              = ErrorStatus();

    if (!root || !search_range || !tree_has_active_time_map(root))
    {
        return false;
    }

    if (!tree_is_supported(root, e))
    {
        if (error_status)
        {
            *error_status = e;
        }
        return false;
    }

    std::set<Clip const*> hits;
    Frac start  = time_frac(search_range->start_time());
    Frac finish = add_frac(start, time_frac(search_range->duration()));
    if (!collect_output_range(
            root,
            start,
            finish,
            !shallow_search,
            hits,
            e))
    {
        if (error_status)
        {
            *error_status = e;
        }
        return false;
    }

    std::vector<Clip const*> order;
    tree_clip_order(root, order);
    std::unordered_set<Clip const*> emitted;
    for (Clip const* clip: order)
    {
        if (hits.find(clip) != hits.end() && emitted.insert(clip).second)
        {
            result->emplace_back(clip);
        }
    }

    if (error_status)
    {
        *error_status = e;
    }
    return !is_error(e);
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
