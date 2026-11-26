// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/stack.h"
#include "opentimelineio/version.h"

#include <utility>
#include <vector>

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {

class Clip;

/// @brief A stack whose output time is mapped non-linearly to internal time.
///
/// The time map is a sequence of (output time, internal source time) control
/// points.  Output control points are strictly increasing; source control
/// points may run forwards, backwards, or hold on one time.
class OTIO_API_TYPE TimeMappedStack : public Stack
{
public:
    /// @brief This struct provides the TimeMappedStack schema.
    struct Schema
    {
        static auto constexpr name   = "TimeMappedStack";
        static int constexpr version = 1;
    };

    using Parent = Stack;

    /// @brief A leaf clip and the clip-local source time sampled at an output.
    struct Sample
    {
        Clip const*  clip;
        RationalTime source_time;
    };

    OTIO_API TimeMappedStack(
        std::string const&              name         = std::string(),
        std::optional<TimeRange> const& source_range = std::nullopt,
        AnyDictionary const&            metadata     = AnyDictionary(),
        std::vector<Effect*> const&     effects      = {},
        std::vector<Marker*> const&     markers      = {});

    OTIO_API ~TimeMappedStack() override;

    /// @brief Replace the output-to-source control map.
    ///
    /// On validation failure the previous map is retained.
    OTIO_API bool set_time_map(
        std::vector<std::pair<RationalTime, RationalTime>> const& time_map,
        ErrorStatus* error_status = nullptr);

    /// @brief Return the current output-to-source control map.
    std::vector<std::pair<RationalTime, RationalTime>> const& time_map()
        const noexcept
    {
        return _time_map;
    }

    /// @brief Return whether a non-empty time map is installed.
    bool has_time_map() const noexcept { return !_time_map.empty(); }

    /// @brief Sample enabled leaf clips at visible elapsed time.
    OTIO_API std::vector<Sample> sample_at(
        RationalTime elapsed,
        ErrorStatus* error_status = nullptr) const;

    TimeRange
    available_range(ErrorStatus* error_status = nullptr) const override;

protected:
    bool read_from(Reader&) override;
    void write_to(Writer&) const override;

private:
    std::vector<std::pair<RationalTime, RationalTime>> _time_map;
};

/// @brief Clip search with TimeMappedStack range splitting support.
///
/// Returns true when the tree contains an active TimeMappedStack and the
/// caller should use the mapped implementation.  Otherwise classic_result is
/// left untouched.
OTIO_API bool time_mapped_find_children_clip(
    Composition const* root,
    ErrorStatus* error_status,
    std::optional<TimeRange> const& search_range,
    bool shallow_search,
    std::vector<SerializableObject::Retainer<Clip>>* result);

/// @brief Return whether the enabled tree contains an active time map.
OTIO_API bool tree_has_active_time_map(Composition const* root);

}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS
