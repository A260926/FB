// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/audioRender.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/composition.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/item.h"
#include "opentimelineio/mediaReference.h"
#include "opentimelineio/pcmWave.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

// Boundary tolerance, in seconds, for half-open interval tests.  Layout
// boundaries are rational values converted to seconds, so they carry a few
// ulp of error; this is far below one sample interval at any practical rate
// and far above that noise.
constexpr double kBoundaryEpsilon = 1e-12;

// Tolerance used to snap a media frame position that is mathematically an
// integer (but arrived through double conversion) onto that frame.
constexpr double kFrameSnapEpsilon = 1e-6;

constexpr double kPi = 3.14159265358979323846264338327950288;

void
set_error(
    AudioRenderStatus* status,
    AudioRenderError   error,
    std::string        message)
{
    if (status)
    {
        status->error   = error;
        status->message = std::move(message);
    }
}

// Half-open containment: start <= t < end.
bool
contains_time(double t, double start, double end) noexcept
{
    return t >= start - kBoundaryEpsilon && t < end - kBoundaryEpsilon;
}

bool
ranges_overlap(double a0, double a1, double b0, double b1) noexcept
{
    return std::min(a1, b1) - std::max(a0, b0) > kBoundaryEpsilon;
}

bool
valid_time(RationalTime const& t) noexcept
{
    return t.is_valid_time() && std::isfinite(t.value());
}

bool
valid_range(TimeRange const& r) noexcept
{
    return valid_time(r.start_time()) && valid_time(r.duration())
           && std::isfinite(r.duration().value()) && r.duration().value() >= 0;
}

// Immutable snapshot of one media file.  Nothing here refers back to the
// Timeline supplied to the renderer.
struct MediaSource
{
    std::string   path;
    PcmWaveReader reader;
    bool          opened = false;

    bool open_reader(std::string& error)
    {
        if (opened)
        {
            return true;
        }
        PcmWaveStatus status;
        if (!reader.open(path, &status))
        {
            error = status.message.empty()
                        ? ("cannot open media '" + path + "'")
                        : status.message;
            return false;
        }
        opened = true;
        return true;
    }

    bool probe(std::string& error)
    {
        PcmWaveStatus status;
        if (!reader.open(path, &status))
        {
            error = status.message.empty()
                        ? ("cannot open media '" + path + "'")
                        : status.message;
            return false;
        }
        reader.close();
        opened = false;
        return true;
    }
};

enum class NodeKind
{
    Muted,
    Clip,
    Track,
    Stack,
};

struct CompiledNode;

struct TrackEntry
{
    CompiledNode* node       = nullptr;
    bool          transition = false;
    double        start      = 0;
    double        end        = 0;
};

struct CompiledTransition
{
    size_t left_entry  = 0;
    size_t right_entry = 0;
    double start       = 0;
    double end         = 0;
};

struct CompiledNode
{
    NodeKind kind = NodeKind::Muted;

    // Duration of this node in its own local coordinate system (seconds).
    double duration = 0;

    // Local source coordinate that corresponds to the start of this node in
    // its parent.  For a Clip this is the trimmed_range start; for a
    // composition it is the source_range start.
    double source_start = 0;

    // Whether the underlying composable is a Clip.  Transitions require
    // clip-like neighbours even when those clips are disabled (muted).
    bool is_clip = false;

    std::shared_ptr<MediaSource> media;

    // Child nodes (Stack children, and the ordinary children of a Track so
    // that the compiled graph owns them).
    std::vector<std::unique_ptr<CompiledNode>> children;

    // Track layout in the track's local coordinate system.  Includes one entry
    // per enabled non-transition child, plus one entry per enabled transition.
    std::vector<TrackEntry>         entries;
    std::vector<CompiledTransition> transitions;
};

struct EvalError
{
    AudioRenderError code = AudioRenderError::InvalidTimeline;
    std::string      message;
};

bool eval_node(
    CompiledNode* node,
    double        t,
    double&       left,
    double&       right,
    EvalError&    error);

bool
eval_clip(
    CompiledNode* node,
    double        t,
    double&       left,
    double&       right,
    EvalError&    error)
{
    if (!node->media)
    {
        error = { AudioRenderError::InvalidTimeline,
                  "clip has no readable media" };
        return false;
    }

    std::string open_error;
    if (!node->media->open_reader(open_error))
    {
        error = { AudioRenderError::MediaReadError, open_error };
        return false;
    }

    PcmWaveReader& media     = node->media->reader;
    double         frame_pos = t * media.sample_rate();
    if (!std::isfinite(frame_pos))
    {
        error = { AudioRenderError::InvalidTimeline,
                  "clip source time is not finite" };
        return false;
    }

    double const nearest = std::nearbyint(frame_pos);
    if (std::fabs(frame_pos - nearest) < kFrameSnapEpsilon)
    {
        frame_pos = nearest;
    }

    PcmWaveStatus status;
    float         sample_left = 0, sample_right = 0;
    if (!media.sample_at(frame_pos, sample_left, sample_right, &status))
    {
        error = { AudioRenderError::MediaReadError,
                  status.message.empty() ? "failed to read media"
                                         : status.message };
        return false;
    }
    left  = sample_left;
    right = sample_right;
    return true;
}

bool
eval_track(
    CompiledNode* node,
    double        t,
    double&       left,
    double&       right,
    EvalError&    error)
{
    for (auto const& transition: node->transitions)
    {
        if (!contains_time(t, transition.start, transition.end))
        {
            continue;
        }

        double const span = transition.end - transition.start;
        double       u    = span > 0 ? (t - transition.start) / span : 0.0;
        u                 = std::clamp(u, 0.0, 1.0);

        TrackEntry const& left_entry  = node->entries[transition.left_entry];
        TrackEntry const& right_entry = node->entries[transition.right_entry];
        if (!left_entry.node || !right_entry.node)
        {
            error = { AudioRenderError::InvalidTimeline,
                      "transition does not have two adjacent clips" };
            return false;
        }

        double const out_gain = std::cos(kPi * u * 0.5);
        double const in_gain  = std::sin(kPi * u * 0.5);

        double ol = 0, orr = 0, il = 0, irr = 0;
        if (!eval_node(
                left_entry.node,
                t - left_entry.start + left_entry.node->source_start,
                ol,
                orr,
                error)
            || !eval_node(
                right_entry.node,
                t - right_entry.start + right_entry.node->source_start,
                il,
                irr,
                error))
        {
            return false;
        }

        left  = ol * out_gain + il * in_gain;
        right = orr * out_gain + irr * in_gain;
        return true;
    }

    TrackEntry const* covering = nullptr;
    for (auto const& entry: node->entries)
    {
        if (entry.transition)
        {
            continue;
        }
        if (contains_time(t, entry.start, entry.end))
        {
            if (covering)
            {
                error = { AudioRenderError::InvalidTimeline,
                          "overlapping clips in track" };
                return false;
            }
            covering = &entry;
        }
    }

    if (!covering)
    {
        left = right = 0.0;
        return true;
    }

    return eval_node(
        covering->node,
        t - covering->start + covering->node->source_start,
        left,
        right,
        error);
}

bool
eval_stack(
    CompiledNode* node,
    double        t,
    double&       left,
    double&       right,
    EvalError&    error)
{
    left = right = 0.0;
    for (auto const& child: node->children)
    {
        if (!contains_time(t, 0.0, child->duration))
        {
            continue;
        }

        double cl = 0, cr = 0;
        if (!eval_node(child.get(), t + child->source_start, cl, cr, error))
        {
            return false;
        }
        left += cl;
        right += cr;
    }
    return true;
}

bool
eval_node(
    CompiledNode* node,
    double        t,
    double&       left,
    double&       right,
    EvalError&    error)
{
    switch (node->kind)
    {
        case NodeKind::Muted:
            left = right = 0.0;
            return true;
        case NodeKind::Clip:
            return eval_clip(node, t, left, right, error);
        case NodeKind::Track:
            return eval_track(node, t, left, right, error);
        case NodeKind::Stack:
            return eval_stack(node, t, left, right, error);
    }
    left = right = 0.0;
    return true;
}

bool
item_geometry(
    Item const*        item,
    double&            duration,
    double&            source_start,
    AudioRenderStatus* status)
{
    ErrorStatus error;
    TimeRange   trimmed = item->trimmed_range(&error);
    if (is_error(error) || !valid_range(trimmed))
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "item '" + item->name() + "' has an invalid trimmed range");
        return false;
    }

    if (item->source_range() && !valid_range(*item->source_range()))
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "item '" + item->name() + "' has an invalid source_range");
        return false;
    }

    duration     = trimmed.duration().to_seconds();
    source_start = trimmed.start_time().to_seconds();
    if (!std::isfinite(duration) || !std::isfinite(source_start)
        || duration < 0)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "item '" + item->name() + "' has a non-finite or negative duration");
        return false;
    }
    return true;
}

bool
check_effects(Item const* item, AudioRenderStatus* status)
{
    for (auto const& effect: item->effects())
    {
        if (!effect.value)
        {
            continue;
        }
        if (dynamic_cast<TimeEffect const*>(effect.value))
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "time effects are not supported by the audio renderer");
            return false;
        }
    }
    return true;
}

std::string
resolve_media_path(std::string root, std::string const& target)
{
    std::filesystem::path target_path(target);
    if (target_path.is_absolute() || root.empty())
    {
        return target_path.lexically_normal().string();
    }
    return (std::filesystem::path(root) / target_path)
        .lexically_normal()
        .string();
}

} // namespace

// ---------------------------------------------------------------------------
// Renderer implementation
// ---------------------------------------------------------------------------

struct AudioRenderer::Impl
{
    bool          valid       = false;
    double        output_rate = 0;
    RationalTime  duration_rt;
    double        duration_s  = 0;
    int64_t       frame_count = 0;
    int64_t       position    = 0;
    std::string   media_root;
    std::unique_ptr<CompiledNode> root;

    // One shared reader per resolved media path, so repeated references to
    // the same file do not duplicate handles or caches.
    std::map<std::string, std::shared_ptr<MediaSource>> media_sources;

    std::shared_ptr<MediaSource> source_for(std::string const& path)
    {
        auto found = media_sources.find(path);
        if (found != media_sources.end())
        {
            return found->second;
        }
        auto source  = std::make_shared<MediaSource>();
        source->path = path;
        media_sources.emplace(path, source);
        return source;
    }

    bool compile(
        Timeline const*    timeline,
        std::string        root_path,
        AudioRenderStatus* status);
    bool build(
        Composable const*             composable,
        bool                          muted,
        std::unique_ptr<CompiledNode>& out,
        AudioRenderStatus*            status);
    bool build_children(
        Composition const*            composition,
        bool                          muted,
        CompiledNode&                 out,
        AudioRenderStatus*            status);
    void build_track(
        Track const*                  track,
        double                        duration,
        double                        source_start,
        std::unique_ptr<CompiledNode>& out,
        AudioRenderStatus*            status,
        bool&                         ok);
    bool probe_media(AudioRenderStatus* status);
    bool read_block(
        int64_t            frames,
        float*             out,
        int64_t&           written,
        AudioRenderStatus* status);
};

bool
AudioRenderer::Impl::build_children(
    Composition const* composition,
    bool               muted,
    CompiledNode&      out,
    AudioRenderStatus* status)
{
    for (auto const& child: composition->children())
    {
        std::unique_ptr<CompiledNode> compiled;
        if (!build(child.value, muted, compiled, status))
        {
            return false;
        }
        out.children.push_back(std::move(compiled));
    }
    return true;
}

void
AudioRenderer::Impl::build_track(
    Track const*                  track,
    double                        duration,
    double                        source_start,
    std::unique_ptr<CompiledNode>& out,
    AudioRenderStatus*            status,
    bool&                         ok)
{
    ok = false;

    struct ChildInfo
    {
        bool                          is_transition = false;
        bool                          skip          = false;
        bool                          is_clip       = false;
        std::unique_ptr<CompiledNode> node;
        double                        in_offset  = 0;
        double                        out_offset = 0;
    };

    std::vector<ChildInfo> infos;
    infos.reserve(track->children().size());

    for (auto const& child: track->children())
    {
        ChildInfo info;

        if (auto transition = dynamic_cast<Transition const*>(child.value))
        {
            if (!transition->enabled())
            {
                info.skip = true;
                infos.push_back(std::move(info));
                continue;
            }

            info.is_transition = true;
            if (transition->transition_type()
                != Transition::Type::SMPTE_Dissolve)
            {
                set_error(
                    status,
                    AudioRenderError::UnsupportedFeature,
                    "only SMPTE_Dissolve transitions are supported");
                return;
            }
            if (!valid_time(transition->in_offset())
                || !valid_time(transition->out_offset())
                || transition->in_offset().value() < 0
                || transition->out_offset().value() < 0)
            {
                set_error(
                    status,
                    AudioRenderError::InvalidTimeline,
                    "transition has invalid offsets");
                return;
            }
            info.in_offset  = transition->in_offset().to_seconds();
            info.out_offset = transition->out_offset().to_seconds();
            if (!(info.in_offset + info.out_offset > 0))
            {
                set_error(
                    status,
                    AudioRenderError::InvalidTimeline,
                    "transition has zero duration");
                return;
            }
            infos.push_back(std::move(info));
            continue;
        }

        std::unique_ptr<CompiledNode> compiled;
        if (!build(child.value, false, compiled, status))
        {
            return;
        }
        info.is_clip = dynamic_cast<Clip const*>(child.value) != nullptr;
        info.node    = std::move(compiled);
        infos.push_back(std::move(info));
    }

    // A transition needs an immediately adjacent clip on each side.  Gaps,
    // nested compositions and other transitions do not qualify.
    for (size_t i = 0; i < infos.size(); ++i)
    {
        if (!infos[i].is_transition)
        {
            continue;
        }
        if (i == 0 || i + 1 >= infos.size())
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "transition does not have two adjacent clips");
            return;
        }
        ChildInfo const& left_info  = infos[i - 1];
        ChildInfo const& right_info = infos[i + 1];
        if (left_info.is_transition || right_info.is_transition
            || !left_info.is_clip || !right_info.is_clip || !left_info.node
            || !right_info.node)
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "transition does not have two adjacent clips");
            return;
        }
    }

    out.reset(new CompiledNode);
    out->kind         = NodeKind::Track;
    out->duration     = duration;
    out->source_start = source_start;

    double cursor = 0;
    for (size_t i = 0; i < infos.size(); ++i)
    {
        ChildInfo& info = infos[i];
        if (info.skip)
        {
            continue;
        }

        if (info.is_transition)
        {
            TrackEntry entry;
            entry.transition = true;
            entry.start      = cursor - info.in_offset;
            entry.end        = entry.start + info.in_offset + info.out_offset;

            CompiledTransition compiled;
            compiled.left_entry  = out->entries.size() - 1;
            compiled.right_entry = out->entries.size() + 1;
            compiled.start       = entry.start;
            compiled.end         = entry.end;

            out->entries.push_back(entry);
            out->transitions.push_back(compiled);
            continue;
        }

        TrackEntry entry;
        entry.node  = info.node.get();
        entry.start = cursor;
        entry.end   = cursor + info.node->duration;
        cursor      = entry.end;
        out->entries.push_back(entry);
        out->children.push_back(std::move(info.node));
    }

    for (size_t i = 0; i < out->transitions.size(); ++i)
    {
        CompiledTransition const& transition = out->transitions[i];
        if (transition.left_entry >= out->entries.size()
            || transition.right_entry >= out->entries.size())
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "transition does not have two adjacent clips");
            return;
        }

        TrackEntry const& left  = out->entries[transition.left_entry];
        TrackEntry const& right = out->entries[transition.right_entry];
        if (left.transition || right.transition || !left.node || !right.node)
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "transition does not have two adjacent clips");
            return;
        }

        // The offsets must fit inside the adjacent clips.
        if (transition.start < left.start - kBoundaryEpsilon
            || transition.end > right.end + kBoundaryEpsilon)
        {
            set_error(
                status,
                AudioRenderError::InvalidTimeline,
                "transition extends beyond its adjacent clips");
            return;
        }

        // The transition interval may overlap only its two neighbours, so no
        // ordinary clip is additionally mixed inside the transition.
        for (size_t e = 0; e < out->entries.size(); ++e)
        {
            if (e == transition.left_entry || e == transition.right_entry)
            {
                continue;
            }
            TrackEntry const& entry = out->entries[e];
            if (entry.transition)
            {
                continue;
            }
            if (ranges_overlap(
                    transition.start,
                    transition.end,
                    entry.start,
                    entry.end))
            {
                set_error(
                    status,
                    AudioRenderError::UnsupportedFeature,
                    "transition interval overlaps another item");
                return;
            }
        }

        // Transition coverage intervals may not intersect each other.
        for (size_t j = i + 1; j < out->transitions.size(); ++j)
        {
            CompiledTransition const& other = out->transitions[j];
            if (ranges_overlap(
                    transition.start,
                    transition.end,
                    other.start,
                    other.end))
            {
                set_error(
                    status,
                    AudioRenderError::UnsupportedFeature,
                    "transition intervals overlap");
                return;
            }
        }
    }

    ok = true;
}

bool
AudioRenderer::Impl::build(
    Composable const*             composable,
    bool                          muted,
    std::unique_ptr<CompiledNode>& out,
    AudioRenderStatus*            status)
{
    if (!composable)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "timeline contains a null composable");
        return false;
    }

    if (dynamic_cast<Transition const*>(composable))
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "transition found outside of a track");
        return false;
    }

    auto item = dynamic_cast<Item const*>(composable);
    if (!item)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "unsupported composable in audio timeline");
        return false;
    }

    double duration = 0, source_start = 0;
    if (!item_geometry(item, duration, source_start, status))
    {
        return false;
    }

    bool const is_clip   = dynamic_cast<Clip const*>(composable) != nullptr;
    bool const is_gap    = dynamic_cast<Gap const*>(composable) != nullptr;
    bool const is_video  = dynamic_cast<Track const*>(composable)
                          && static_cast<Track const*>(composable)->kind()
                                 == Track::Kind::video;
    bool const is_muted  = muted || !item->enabled() || is_gap || is_video;

    if (is_muted)
    {
        out.reset(new CompiledNode);
        out->kind     = NodeKind::Muted;
        out->duration = duration;
        out->is_clip  = is_clip;
        return true;
    }

    if (!check_effects(item, status))
    {
        return false;
    }

    if (auto stack = dynamic_cast<Stack const*>(composable))
    {
        out.reset(new CompiledNode);
        out->kind         = NodeKind::Stack;
        out->duration     = duration;
        out->source_start = source_start;
        return build_children(stack, false, *out, status);
    }

    if (auto track = dynamic_cast<Track const*>(composable))
    {
        bool ok = false;
        build_track(track, duration, source_start, out, status, ok);
        return ok;
    }

    if (is_clip)
    {
        auto clip = static_cast<Clip const*>(composable);

        MediaReference* reference = clip->media_reference();
        if (!reference || reference->is_missing_reference())
        {
            set_error(
                status,
                AudioRenderError::MissingMedia,
                "clip '" + clip->name() + "' has a missing media reference");
            return false;
        }

        auto external = dynamic_cast<ExternalReference const*>(reference);
        if (!external)
        {
            set_error(
                status,
                AudioRenderError::UnsupportedFeature,
                "only ExternalReference media is supported");
            return false;
        }
        if (external->target_url().empty())
        {
            set_error(
                status,
                AudioRenderError::MissingMedia,
                "clip '" + clip->name() + "' has an empty media path");
            return false;
        }

        out.reset(new CompiledNode);
        out->kind         = NodeKind::Clip;
        out->duration     = duration;
        out->source_start = source_start;
        out->is_clip      = true;
        out->media        = source_for(
            resolve_media_path(media_root, external->target_url()));
        return true;
    }

    set_error(
        status,
        AudioRenderError::UnsupportedFeature,
        "unsupported audio composable");
    return false;
}

bool
AudioRenderer::Impl::probe_media(AudioRenderStatus* status)
{
    // Every source registered here backs a visible clip, so an unreadable
    // file must fail the render rather than silently render partial audio.
    for (auto const& entry: media_sources)
    {
        std::string error;
        if (!entry.second->probe(error))
        {
            set_error(status, AudioRenderError::MediaReadError, error);
            return false;
        }
    }
    return true;
}

bool
AudioRenderer::Impl::compile(
    Timeline const*    timeline,
    std::string        root_path,
    AudioRenderStatus* status)
{
    media_root = std::move(root_path);

    if (!timeline)
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "timeline must not be null");
        return false;
    }
    if (!std::isfinite(output_rate) || output_rate <= 0)
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "output rate must be a positive finite number");
        return false;
    }

    Stack* tracks = timeline->tracks();
    if (!tracks)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "timeline has no tracks stack");
        return false;
    }

    if (!build(tracks, false, root, status))
    {
        return false;
    }

    // global_start_time is a timecode only and is intentionally ignored.
    ErrorStatus  duration_error;
    RationalTime timeline_duration = timeline->duration(&duration_error);
    if (is_error(duration_error) || !valid_time(timeline_duration)
        || timeline_duration.value() < 0)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "timeline has an invalid duration");
        return false;
    }

    duration_s = timeline_duration.to_seconds();
    if (!std::isfinite(duration_s) || duration_s < 0)
    {
        set_error(
            status,
            AudioRenderError::InvalidTimeline,
            "timeline has an invalid duration");
        return false;
    }
    duration_rt = RationalTime(duration_s, 1);

    long double const exact_frames =
        static_cast<long double>(timeline_duration.value())
        * static_cast<long double>(output_rate)
        / static_cast<long double>(timeline_duration.rate());
    long double const tolerance =
        std::max<long double>(1e-9L, std::fabs(exact_frames) * 1e-12L);
    long double const rounded = std::ceil(exact_frames - tolerance);
    frame_count = rounded <= 0 ? 0 : int64_t(rounded);

    if (!probe_media(status))
    {
        return false;
    }

    valid = true;
    return true;
}

bool
AudioRenderer::Impl::read_block(
    int64_t            frames,
    float*             out,
    int64_t&           written,
    AudioRenderStatus* status)
{
    written = 0;

    if (!valid)
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "audio renderer is not valid");
        return false;
    }
    if (frames < 0 || (frames > 0 && !out))
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "invalid read arguments");
        return false;
    }

    int64_t const available = frame_count - position;
    int64_t const count     = std::min(frames, std::max<int64_t>(available, 0));

    // Render into a scratch buffer so a failure never leaves a partial block
    // in the caller's buffer and never advances the read position.
    std::vector<float> scratch(size_t(count) * 2);
    for (int64_t i = 0; i < count; ++i)
    {
        // Program time is retained as a fractional value in seconds.  Each
        // sample is derived directly from its output frame number, so chunked,
        // seeked and one-shot reads produce identical samples.
        long double const program =
            static_cast<long double>(position + i)
            / static_cast<long double>(output_rate);
        double const program_seconds =
            static_cast<double>(program)
            + static_cast<double>(root->source_start);

        EvalError error;
        double    l = 0, r = 0;
        if (!eval_node(root.get(), program_seconds, l, r, error))
        {
            set_error(status, error.code, std::move(error.message));
            return false;
        }
        scratch[size_t(i) * 2]     = float(l);
        scratch[size_t(i) * 2 + 1] = float(r);
    }

    if (count > 0)
    {
        std::copy(scratch.begin(), scratch.end(), out);
    }
    position += count;
    written = count;
    return true;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

AudioRenderer::AudioRenderer(
    Timeline const*    timeline,
    std::string        media_root,
    double             output_rate,
    AudioRenderStatus* status)
    : _impl(new Impl)
{
    if (status)
    {
        *status = AudioRenderStatus();
    }
    _impl->output_rate = output_rate;
    // compile() reports failures through status; the renderer stays invalid.
    _impl->compile(timeline, std::move(media_root), status);
}

AudioRenderer::~AudioRenderer() = default;

AudioRenderer::AudioRenderer(AudioRenderer&&) noexcept = default;

AudioRenderer&
AudioRenderer::operator=(AudioRenderer&&) noexcept = default;

bool
AudioRenderer::is_valid() const noexcept
{
    return _impl && _impl->valid;
}

double
AudioRenderer::output_rate() const noexcept
{
    return _impl ? _impl->output_rate : 0.0;
}

RationalTime
AudioRenderer::duration() const noexcept
{
    return _impl ? _impl->duration_rt : RationalTime();
}

int64_t
AudioRenderer::frame_count() const noexcept
{
    return _impl ? _impl->frame_count : 0;
}

int64_t
AudioRenderer::tell() const noexcept
{
    return _impl ? _impl->position : 0;
}

bool
AudioRenderer::seek(int64_t frame, AudioRenderStatus* status)
{
    if (status)
    {
        *status = AudioRenderStatus();
    }
    if (!_impl || !_impl->valid)
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "audio renderer is not valid");
        return false;
    }
    if (frame < 0 || frame > _impl->frame_count)
    {
        set_error(
            status,
            AudioRenderError::InvalidSeek,
            "seek position is outside the render interval");
        return false;
    }
    _impl->position = frame;
    return true;
}

int64_t
AudioRenderer::read(
    int64_t            frame_count,
    float*             stereo_out,
    AudioRenderStatus* status)
{
    if (status)
    {
        *status = AudioRenderStatus();
    }
    if (!_impl || !_impl->valid)
    {
        set_error(
            status,
            AudioRenderError::InvalidArgument,
            "audio renderer is not valid");
        return -1;
    }

    int64_t written = 0;
    if (!_impl->read_block(frame_count, stereo_out, written, status))
    {
        return -1;
    }
    return written;
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
