// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/audioRender.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/composition.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/freezeFrame.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/generatorReference.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/missingReference.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_map>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

namespace {

constexpr double k_pi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Error helper: map the renderer-specific error taxonomy onto an OTIO
// ErrorStatus outcome while keeping the precise error name in the details.
// ---------------------------------------------------------------------------
ErrorStatus::Outcome
outcome_for(AudioRenderError error)
{
    switch (error)
    {
        case AudioRenderError::invalid_argument:
            return ErrorStatus::INTERNAL_ERROR;
        case AudioRenderError::unsupported_timeline:
            return ErrorStatus::NOT_IMPLEMENTED;
        case AudioRenderError::invalid_time:
        case AudioRenderError::overlapping_transitions:
            return ErrorStatus::INVALID_TIME_RANGE;
        case AudioRenderError::transition_without_neighbors:
            return ErrorStatus::CANNOT_TRIM_TRANSITION;
        case AudioRenderError::invalid_media_reference:
            return ErrorStatus::MALFORMED_SCHEMA;
        case AudioRenderError::unreadable_media:
            return ErrorStatus::FILE_OPEN_FAILED;
        case AudioRenderError::seek_out_of_range:
            return ErrorStatus::ILLEGAL_INDEX;
        case AudioRenderError::io_failure:
            return ErrorStatus::INTERNAL_ERROR;
        case AudioRenderError::none:
            break;
    }
    return ErrorStatus::INTERNAL_ERROR;
}

void
fail(ErrorStatus* error_status, AudioRenderError error, std::string message)
{
    if (error_status)
    {
        error_status->outcome = outcome_for(error);
        error_status->details =
            audio_render_error_name(error) + ": " + message;
        error_status->full_description =
            ErrorStatus::outcome_to_string(error_status->outcome) + ": "
            + error_status->details;
    }
}

// ---------------------------------------------------------------------------
// Media: one open PCM source plus a small sliding read window so sample reads
// are on demand and resident memory is bounded by the window, not the media.
// ---------------------------------------------------------------------------
struct MediaEntry
{
    std::unique_ptr<PcmWaveSource> source;
    int                            rate        = 0;
    int                            channels    = 0;
    int64_t                        frame_count = 0;

    static constexpr int64_t window_frames = 512;
    std::vector<float>       window;
    int64_t                  window_start = -1;

    // Fetch one normalized source sample. Out-of-range frames are silence.
    float
    frame_sample(int64_t frame, int channel, ErrorStatus* error_status)
    {
        if (frame < 0 || frame >= frame_count)
        {
            return 0.0f;
        }
        int64_t bucket = frame / window_frames;
        int64_t start  = bucket * window_frames;
        int64_t covered =
            window.empty() ? -1 : int64_t(window.size() / channels);
        if (frame < window_start || frame >= window_start + covered)
        {
            int64_t count =
                std::min<int64_t>(window_frames, frame_count - start);
            if (!source->read_frames(start, count, window, error_status))
            {
                window.clear();
                window_start = -1;
                return 0.0f;
            }
            window_start = start;
        }
        return window[(frame - window_start) * channels + channel];
    }

    // Linearly interpolate at a fractional source position (in frames).
    bool
    interpolated(double position_frames, float out[2], ErrorStatus* error_status)
    {
        if (!(position_frames >= 0.0))
        {
            out[0] = out[1] = 0.0f;
            return true;
        }
        int64_t k0   = int64_t(std::floor(position_frames));
        double  frac = position_frames - double(k0);

        if (channels == 1)
        {
            float v0 = frame_sample(k0, 0, error_status);
            if (is_error(error_status))
            {
                return false;
            }
            float v1 = frame_sample(k0 + 1, 0, error_status);
            if (is_error(error_status))
            {
                return false;
            }
            float value =
                float(double(v0) + (double(v1) - double(v0)) * frac);
            out[0] = out[1] = value; // mono duplicates to both speakers
        }
        else
        {
            for (int c = 0; c < 2; ++c)
            {
                float v0 = frame_sample(k0, c, error_status);
                if (is_error(error_status))
                {
                    return false;
                }
                float v1 = frame_sample(k0 + 1, c, error_status);
                if (is_error(error_status))
                {
                    return false;
                }
                out[c] =
                    float(double(v0) + (double(v1) - double(v0)) * frac);
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Render node tree: a flattened, pre-validated mirror of the audio portion of
// the timeline snapshot. All times stored in seconds (double) so fractional
// program and source times are preserved.
// ---------------------------------------------------------------------------
struct Node
{
    virtual ~Node() = default;
    // Evaluate at local time t (seconds in this node's local coordinate).
    virtual bool evaluate(double t, float out[2], ErrorStatus*) = 0;
};

struct SilenceNode final : Node
{
    bool evaluate(double, float out[2], ErrorStatus*) override
    {
        out[0] = out[1] = 0.0f;
        return true;
    }
};

struct ClipNode final : Node
{
    MediaEntry* media = nullptr;
    // Time (in seconds) at which the file's first frame sits in the media
    // reference's source coordinate (the available_range start).
    double media_start_seconds = 0.0;

    bool evaluate(double t, float out[2], ErrorStatus* error_status) override
    {
        // A clip's local coordinate is the media reference's source
        // coordinate. Subtract the available range origin to reach the file's
        // own time, where file frame k sits at k / file_rate. Reads outside
        // the physical media (including the far side of handles) are silence;
        // interpolation handles the boundary frame itself.
        double file_time = t - media_start_seconds;
        double position  = file_time * double(media->rate);
        return media->interpolated(position, out, error_status);
    }
};

struct CompositionNode : Node
{
    // A placed child occupies [start, end) in this composition's seconds.
    struct Child
    {
        Node*  node         = nullptr;
        double start        = 0.0;
        double end          = 0.0;
        // Start of the child's own source coordinate (trimmed range start).
        double source_start = 0.0;
    };
    std::vector<Child> children;

    // Map a parent-local time into the child's local/source coordinate.
    double to_child_time(Child const& child, double t) const
    {
        return child.source_start + (t - child.start);
    }
};

struct TrackNode final : CompositionNode
{
    struct Transition
    {
        double    start;      // half-open coverage in track-local seconds
        double    end;
        ClipNode* outgoing;   // neighbor before the transition
        ClipNode* incoming;   // neighbor after the transition
        double    outgoing_source_start;
        double    incoming_source_start;
        double    outgoing_placement_start;
        double    incoming_placement_start;
    };
    std::vector<Transition> transitions;

    bool evaluate(double t, float out[2], ErrorStatus* error_status) override
    {
        out[0] = out[1] = 0.0f;

        // Transitions own their whole coverage interval; ordinary segments
        // are never added on top of them.
        for (Transition const& tr: transitions)
        {
            if (t >= tr.start && t < tr.end)
            {
                // u is the full, un-rebased transition progress, even when a
                // nested crop only reveals part of the transition.
                double u        = (t - tr.start) / (tr.end - tr.start);
                double gain_out = std::cos(k_pi * u / 2.0);
                double gain_in  = std::sin(k_pi * u / 2.0);

                double src_out = tr.outgoing_source_start
                                 + (t - tr.outgoing_placement_start);
                double src_in = tr.incoming_source_start
                                + (t - tr.incoming_placement_start);

                float a[2], b[2];
                if (!tr.outgoing->evaluate(src_out, a, error_status)
                    || !tr.incoming->evaluate(src_in, b, error_status))
                {
                    return false;
                }
                out[0] = float(gain_out * a[0] + gain_in * b[0]);
                out[1] = float(gain_out * a[1] + gain_in * b[1]);
                return true;
            }
        }

        for (Child const& child: children)
        {
            if (t >= child.start && t < child.end)
            {
                return child.node->evaluate(
                    to_child_time(child, t), out, error_status);
            }
        }
        // Gaps and the regions past short tracks are silence.
        return true;
    }
};

struct StackNode final : CompositionNode
{
    bool evaluate(double t, float out[2], ErrorStatus* error_status) override
    {
        out[0] = out[1] = 0.0f;
        // Stack children all start at local zero and are summed.
        for (Child const& child: children)
        {
            if (t < child.start || t >= child.end)
            {
                continue;
            }
            float value[2];
            if (!child.node->evaluate(
                    to_child_time(child, t), value, error_status))
            {
                return false;
            }
            out[0] += value[0];
            out[1] += value[1];
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Builder: walks the snapshot, validates every rule and opens the media.
// ---------------------------------------------------------------------------
class Builder
{
public:
    std::string media_root;
    std::map<std::string, std::unique_ptr<MediaEntry>> media;
    ErrorStatus* error_status;

    Builder(std::string root, ErrorStatus* es)
        : media_root(std::move(root))
        , error_status(es)
    {}

    static bool
    valid_time(RationalTime const& t)
    {
        return t.is_valid_time() && std::isfinite(t.to_seconds());
    }

    static bool
    valid_range(TimeRange const& r)
    {
        return valid_time(r.start_time()) && valid_time(r.duration())
               && r.duration().value() >= 0.0;
    }

    // Reject any time effect attached to an item.
    bool
    check_effects(Item const* item)
    {
        for (auto const& effect: item->effects())
        {
            if (dynamic_cast<TimeEffect const*>(effect.value))
            {
                fail(
                    error_status,
                    AudioRenderError::unsupported_timeline,
                    "time effects are not supported (item: " + item->name()
                        + ")");
                return false;
            }
        }
        return true;
    }

    std::string
    resolve_path(std::string const& target_url)
    {
        std::string url = target_url;
        // Only local file URLs are accepted.
        if (url.rfind("file://", 0) == 0)
        {
            url.erase(0, 7);
        }
        else if (url.rfind("file:", 0) == 0)
        {
            url.erase(0, 5);
        }
        else if (url.find("://") != std::string::npos)
        {
            return std::string(); // signal unsupported scheme
        }

        // Minimal percent-decoding for local path characters.
        std::string decoded;
        decoded.reserve(url.size());
        for (size_t i = 0; i < url.size(); ++i)
        {
            if (url[i] == '%' && i + 2 < url.size())
            {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hex(url[i + 1]);
                int lo = hex(url[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    decoded.push_back(char(hi * 16 + lo));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(url[i]);
        }

        if (!decoded.empty() && decoded[0] == '/')
        {
            return decoded;
        }
        if (media_root.empty())
        {
            return decoded;
        }
        if (media_root.back() == '/')
        {
            return media_root + decoded;
        }
        return media_root + "/" + decoded;
    }

    MediaEntry*
    open_media(Clip const* clip)
    {
        auto ref =
            dynamic_cast<ExternalReference const*>(clip->media_reference());
        if (!ref)
        {
            fail(
                error_status,
                AudioRenderError::invalid_media_reference,
                "clip '" + clip->name()
                    + "' does not use a local ExternalReference");
            return nullptr;
        }
        std::string path = resolve_path(ref->target_url());
        if (path.empty())
        {
            fail(
                error_status,
                AudioRenderError::invalid_media_reference,
                "clip '" + clip->name()
                    + "' references a non-local URL: " + ref->target_url());
            return nullptr;
        }

        auto found = media.find(path);
        if (found != media.end())
        {
            return found->second.get();
        }

        auto entry    = std::make_unique<MediaEntry>();
        entry->source = std::make_unique<PcmWaveSource>(path, error_status);
        if (is_error(error_status))
        {
            fail(
                error_status,
                AudioRenderError::unreadable_media,
                "cannot read PCM media for clip '" + clip->name() + "': "
                    + path);
            return nullptr;
        }
        entry->rate        = entry->source->sample_rate();
        entry->channels    = entry->source->channels();
        entry->frame_count = entry->source->frame_count();
        MediaEntry* raw    = entry.get();
        media.emplace(path, std::move(entry));
        return raw;
    }

    double
    item_duration_seconds(Item const* item)
    {
        RationalTime duration = item->duration(error_status);
        if (is_error(error_status) || !valid_time(duration)
            || duration.value() < 0.0)
        {
            fail(
                error_status,
                AudioRenderError::invalid_time,
                "invalid duration on item: " + item->name());
            return -1.0;
        }
        return duration.to_seconds();
    }

    double
    trimmed_start_seconds(Item const* item)
    {
        TimeRange trimmed = item->trimmed_range(error_status);
        if (is_error(error_status) || !valid_range(trimmed))
        {
            fail(
                error_status,
                AudioRenderError::invalid_time,
                "invalid trimmed range on item: " + item->name());
            return std::nan("");
        }
        return trimmed.start_time().to_seconds();
    }

    // Build a node for an item that is known to be enabled.
    Node*
    build_item(Item const* item)
    {
        if (!check_effects(item))
        {
            return nullptr;
        }
        if (dynamic_cast<Gap const*>(item))
        {
            return owned_silence();
        }
        if (auto clip = dynamic_cast<Clip const*>(item))
        {
            MediaEntry* entry = open_media(clip);
            if (!entry)
            {
                return nullptr;
            }
            auto node    = std::make_unique<ClipNode>();
            node->media  = entry;
            // The file's first frame is located at the media reference's
            // available_range start in the source coordinate.
            auto const* ref =
                dynamic_cast<ExternalReference const*>(clip->media_reference());
            if (ref && ref->available_range())
            {
                RationalTime const& media_start =
                    ref->available_range()->start_time();
                if (!valid_time(media_start))
                {
                    fail(
                        error_status,
                        AudioRenderError::invalid_time,
                        "invalid available_range on clip: " + clip->name());
                    return nullptr;
                }
                node->media_start_seconds = media_start.to_seconds();
            }
            ClipNode* raw = node.get();
            pending_owned.push_back(std::move(node));
            return raw;
        }
        if (auto composition = dynamic_cast<Composition const*>(item))
        {
            return build_composition(composition);
        }
        fail(
            error_status,
            AudioRenderError::unsupported_timeline,
            "unsupported item type in audio hierarchy: " + item->name());
        return nullptr;
    }

    Node*
    build_composition(Composition const* composition)
    {
        if (auto track = dynamic_cast<Track const*>(composition))
        {
            return build_track(track);
        }
        if (auto stack = dynamic_cast<Stack const*>(composition))
        {
            return build_stack(stack);
        }
        fail(
            error_status,
            AudioRenderError::unsupported_timeline,
            "unsupported composition type: " + composition->name());
        return nullptr;
    }

    Node*
    build_stack(Stack const* stack)
    {
        auto node    = std::make_unique<StackNode>();
        StackNode* raw = node.get();
        pending_owned.push_back(std::move(node));

        for (auto const& child_retainer: stack->children())
        {
            Composable* child = child_retainer.value;
            if (auto child_track = dynamic_cast<Track const*>(child))
            {
                // Video tracks are ignored wherever they appear in the audio
                // hierarchy; audio tracks recurse like any other item.
                if (child_track->kind() != Track::Kind::audio)
                {
                    continue;
                }
            }
            else if (dynamic_cast<Transition const*>(child))
            {
                fail(
                    error_status,
                    AudioRenderError::unsupported_timeline,
                    "transitions are only supported inside tracks (stack: "
                        + stack->name() + ")");
                return nullptr;
            }
            auto item = dynamic_cast<Item const*>(child);
            if (!item)
            {
                fail(
                    error_status,
                    AudioRenderError::unsupported_timeline,
                    "non-item child in stack: " + stack->name());
                return nullptr;
            }
            if (!item->enabled())
            {
                continue; // disabled branch is muted
            }

            double duration = item_duration_seconds(item);
            if (duration < 0.0)
            {
                return nullptr;
            }
            double src_start = trimmed_start_seconds(item);
            if (std::isnan(src_start))
            {
                return nullptr;
            }
            Node* child_node = build_item(item);
            if (!child_node)
            {
                return nullptr;
            }
            raw->children.push_back({ child_node, 0.0, duration, src_start });
        }
        return raw;
    }

    Node*
    build_track(Track const* track)
    {
        auto node     = std::make_unique<TrackNode>();
        TrackNode* raw = node.get();
        pending_owned.push_back(std::move(node));

        auto const& kids = track->children();

        // Pass 1: lay out and build every item. Transitions are overlapping
        // and do not move item placements, so the cursor only advances by the
        // duration of ordinary items.
        double cursor = 0.0;
        for (auto const& child_retainer: kids)
        {
            Composable* child = child_retainer.value;
            if (dynamic_cast<Transition const*>(child))
            {
                continue;
            }
            auto item = dynamic_cast<Item const*>(child);
            if (!item)
            {
                fail(
                    error_status,
                    AudioRenderError::unsupported_timeline,
                    "unsupported child in track: " + track->name());
                return nullptr;
            }

            double duration = item_duration_seconds(item);
            if (duration < 0.0)
            {
                return nullptr;
            }

            if (!item->enabled())
            {
                // A disabled item still occupies its track time as silence.
                Node* silent = owned_silence();
                raw->children.push_back(
                    { silent, cursor, cursor + duration, 0.0 });
                item_nodes[item] = { silent, cursor, duration };
                cursor += duration;
                continue;
            }

            double src_start = trimmed_start_seconds(item);
            if (std::isnan(src_start))
            {
                return nullptr;
            }
            Node* child_node = build_item(item);
            if (!child_node)
            {
                return nullptr;
            }
            raw->children.push_back(
                { child_node, cursor, cursor + duration, src_start });
            item_nodes[item] = { child_node, cursor, duration };
            cursor += duration;
        }

        // Pass 2: validate and register transitions between ordinary clips.
        for (size_t i = 0; i < kids.size(); ++i)
        {
            auto transition =
                dynamic_cast<Transition const*>(kids[i].value);
            if (!transition)
            {
                continue;
            }
            if (!transition->enabled())
            {
                continue; // disabled transition: the clips simply abut
            }
            if (transition->transition_type()
                != Transition::Type::SMPTE_Dissolve)
            {
                fail(
                    error_status,
                    AudioRenderError::unsupported_timeline,
                    "only SMPTE_Dissolve transitions are supported: "
                        + transition->name());
                return nullptr;
            }
            RationalTime in  = transition->in_offset();
            RationalTime out = transition->out_offset();
            if (!valid_time(in) || !valid_time(out) || in.value() <= 0.0
                || out.value() <= 0.0)
            {
                fail(
                    error_status,
                    AudioRenderError::invalid_time,
                    "invalid transition offsets: " + transition->name());
                return nullptr;
            }

            Clip const* out_clip =
                (i > 0) ? dynamic_cast<Clip const*>(kids[i - 1].value)
                        : nullptr;
            Clip const* in_clip =
                (i + 1 < kids.size())
                    ? dynamic_cast<Clip const*>(kids[i + 1].value)
                    : nullptr;
            if (!out_clip || !out_clip->enabled() || !in_clip
                || !in_clip->enabled())
            {
                fail(
                    error_status,
                    AudioRenderError::transition_without_neighbors,
                    "transition '" + transition->name()
                        + "' requires two adjacent enabled clips");
                return nullptr;
            }

            auto out_info = item_nodes.find(out_clip);
            auto in_info  = item_nodes.find(in_clip);
            if (out_info == item_nodes.end() || in_info == item_nodes.end())
            {
                fail(
                    error_status,
                    AudioRenderError::transition_without_neighbors,
                    "transition '" + transition->name()
                        + "' neighbors could not be resolved");
                return nullptr;
            }
            auto* out_node = dynamic_cast<ClipNode*>(out_info->second.node);
            auto* in_node  = dynamic_cast<ClipNode*>(in_info->second.node);
            if (!out_node || !in_node)
            {
                fail(
                    error_status,
                    AudioRenderError::transition_without_neighbors,
                    "transition '" + transition->name()
                        + "' neighbors must be ordinary clips");
                return nullptr;
            }

            double in_s  = in.to_seconds();
            double out_s = out.to_seconds();
            TrackNode::Transition slot;
            slot.outgoing                 = out_node;
            slot.incoming                 = in_node;
            slot.outgoing_placement_start = out_info->second.placement;
            slot.incoming_placement_start = in_info->second.placement;
            slot.start =
                out_info->second.placement + out_info->second.duration - in_s;
            slot.end = in_info->second.placement + out_s;
            slot.outgoing_source_start =
                trimmed_start_seconds(out_clip);
            slot.incoming_source_start =
                trimmed_start_seconds(in_clip);
            if (std::isnan(slot.outgoing_source_start)
                || std::isnan(slot.incoming_source_start))
            {
                return nullptr;
            }

            // Transitions may not overlap each other's coverage.
            for (auto const& other: raw->transitions)
            {
                if (slot.start < other.end && other.start < slot.end)
                {
                    fail(
                        error_status,
                        AudioRenderError::overlapping_transitions,
                        "overlapping transition coverage in track: "
                            + track->name());
                    return nullptr;
                }
            }
            raw->transitions.push_back(slot);
        }
        return raw;
    }

    Node*
    owned_silence()
    {
        auto node = std::make_unique<SilenceNode>();
        Node* raw = node.get();
        pending_owned.push_back(std::move(node));
        return raw;
    }

    struct Placement
    {
        Node*  node;
        double placement;
        double duration;
    };

    // Flat node ownership for the whole render tree.
    std::vector<std::unique_ptr<Node>>           pending_owned;
    std::unordered_map<Item const*, Placement>   item_nodes;

    std::vector<std::unique_ptr<Node>>
    take_owned()
    {
        return std::move(pending_owned);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// AudioRenderer::Impl
// ---------------------------------------------------------------------------
struct AudioRenderer::Impl
{
    SerializableObject::Retainer<Timeline>              snapshot;
    std::vector<std::unique_ptr<Node>>                   nodes;
    std::map<std::string, std::unique_ptr<MediaEntry>>   media;
    std::unique_ptr<StackNode>                           root;
    int                                                  output_rate = 0;
    int64_t                                              total_frames = 0;
    int64_t                                              position = 0;
    double                                               duration_seconds = 0.0;
};

AudioRenderer::AudioRenderer(
    Timeline const&    timeline,
    std::string const& media_root,
    int                output_sample_rate,
    ErrorStatus*       error_status)
    : _impl(std::make_unique<Impl>())
{
    if (output_sample_rate <= 0)
    {
        fail(
            error_status,
            AudioRenderError::invalid_argument,
            "output sample rate must be a positive integer");
        return;
    }
    _impl->output_rate = output_sample_rate;

    // Independent deep snapshot: later edits to the input timeline cannot
    // reach this renderer.
    SerializableObject* cloned = timeline.clone(error_status);
    if (is_error(error_status) || !cloned)
    {
        fail(
            error_status,
            AudioRenderError::unsupported_timeline,
            "could not snapshot the timeline");
        return;
    }
    auto* snapshot_timeline = dynamic_cast<Timeline*>(cloned);
    if (!snapshot_timeline)
    {
        cloned->possibly_delete();
        fail(
            error_status,
            AudioRenderError::invalid_argument,
            "snapshot was not a Timeline");
        return;
    }
    _impl->snapshot = SerializableObject::Retainer<Timeline>(snapshot_timeline);

    RationalTime timeline_duration = _impl->snapshot->duration(error_status);
    if (is_error(error_status) || !timeline_duration.is_valid_time()
        || timeline_duration.value() < 0.0
        || !std::isfinite(timeline_duration.to_seconds()))
    {
        fail(
            error_status,
            AudioRenderError::invalid_time,
            "timeline has an invalid duration");
        return;
    }
    _impl->duration_seconds = timeline_duration.to_seconds();

    Builder builder(media_root, error_status);

    // The timeline's tracks Stack is the render root: its children are summed
    // (audio tracks, nested Tracks/Stacks and clips), video tracks are dropped
    // and global_start_time is deliberately never used to offset output.
    Node* root_untyped = builder.build_stack(_impl->snapshot->tracks());
    if (!root_untyped || is_error(error_status))
    {
        return;
    }
    auto* root = dynamic_cast<StackNode*>(root_untyped);
    if (!root)
    {
        fail(
            error_status,
            AudioRenderError::unsupported_timeline,
            "internal error: timeline root was not a stack");
        return;
    }

    if (is_error(error_status))
    {
        return;
    }

    // The builder retains every node (including the root) in its flat
    // ownership list. Pull the root out into its own holder; all other nodes
    // stay alive in _impl->nodes for the renderer's lifetime.
    _impl->nodes = builder.take_owned();
    for (auto& held: _impl->nodes)
    {
        if (held.get() == root)
        {
            _impl->root.reset(static_cast<StackNode*>(held.release()));
            break;
        }
    }
    if (!_impl->root)
    {
        fail(
            error_status,
            AudioRenderError::unsupported_timeline,
            "internal error: render root ownership could not be transferred");
        return;
    }
    _impl->media = std::move(builder.media);

    // Whole-timeline frame count, rounded once (never per segment). Nudge away
    // floating point overshoot on exact integer products.
    double exact   = _impl->duration_seconds * double(output_sample_rate);
    double rounded = std::ceil(exact - 1e-9 * std::max(1.0, exact));
    _impl->total_frames = int64_t(rounded);
    if (_impl->total_frames < 0)
    {
        _impl->total_frames = 0;
    }
}

AudioRenderer::~AudioRenderer() = default;

int
AudioRenderer::output_sample_rate() const noexcept
{
    return _impl->output_rate;
}

int64_t
AudioRenderer::frame_count() const noexcept
{
    return _impl->total_frames;
}

int64_t
AudioRenderer::position() const noexcept
{
    return _impl->position;
}

bool
AudioRenderer::seek(int64_t frame, ErrorStatus* error_status)
{
    if (frame < 0 || frame > _impl->total_frames)
    {
        fail(
            error_status,
            AudioRenderError::seek_out_of_range,
            "seek frame " + std::to_string(frame) + " is outside [0, "
                + std::to_string(_impl->total_frames) + "]");
        return false;
    }
    _impl->position = frame;
    return true;
    }

bool
AudioRenderer::read(
    int64_t             requested_frame_count,
    std::vector<float>& out,
    ErrorStatus*        error_status)
{
    out.clear();
    if (requested_frame_count < 0)
    {
        fail(
            error_status,
            AudioRenderError::invalid_argument,
            "requested frame count must be non-negative");
        return false;
    }
    if (_impl->position >= _impl->total_frames || requested_frame_count == 0)
    {
        return true; // clean end of stream (or empty request)
    }

    int64_t count =
        std::min(requested_frame_count, _impl->total_frames - _impl->position);
    out.reserve(size_t(count) * 2);

    for (int64_t i = 0; i < count; ++i)
    {
        int64_t frame = _impl->position + i;
        // Program time t = n / R, kept fractional.
        double t = double(frame) / double(_impl->output_rate);
        float  value[2];
        if (!_impl->root->evaluate(t, value, error_status))
        {
            return false;
        }
        out.push_back(value[0]);
        out.push_back(value[1]);
    }

    _impl->position += count;
    return true;
}

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
