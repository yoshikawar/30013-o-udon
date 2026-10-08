#pragma once

#include "hexa_udon/core/result.hpp"

#include <cstdint>
#include <optional>

namespace hexa_udon::core {

using GridSize = std::int32_t;
using Quantity = std::int32_t;
using UnixTimestamp = std::int64_t;

struct CellIndex {
    std::int32_t value;

    [[nodiscard]] bool operator==(const CellIndex&) const = default;
};

struct HexCoord {
    GridSize row;
    GridSize col;

    [[nodiscard]] bool operator==(const HexCoord&) const = default;
};

enum class Direction : std::int32_t {
    UpperLeft = 0,
    UpperRight = 1,
    Right = 2,
    LowerRight = 3,
    LowerLeft = 4,
    Left = 5,
};

enum class Terrain : std::int32_t {
    Plain = 0,
    Road = 1,
    Mountain = 2,
    Pond = 3,
};

enum class NonRoadTerrain : std::int32_t {
    Plain,
    Mountain,
    Pond,
};

enum class RoadStatus : std::int32_t {
    Smooth = 0,
    Busy = 1,
    Jammed = 2,
};

enum class AgentKind : std::int32_t {
    Patrol = 0,
    Supply = 1,
};

struct MovementCost {
    Quantity steps;
    Quantity fuel;

    [[nodiscard]] bool operator==(const MovementCost&) const = default;
};

[[nodiscard]] std::optional<Direction> direction_from_int(std::int32_t value) noexcept;
[[nodiscard]] std::optional<Terrain> terrain_from_int(std::int32_t value) noexcept;
[[nodiscard]] std::optional<RoadStatus> road_status_from_int(std::int32_t value) noexcept;
[[nodiscard]] std::optional<AgentKind> agent_kind_from_int(std::int32_t value) noexcept;

[[nodiscard]] constexpr std::int32_t to_int(Direction value) noexcept
{
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] constexpr std::int32_t to_int(Terrain value) noexcept
{
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] constexpr std::int32_t to_int(RoadStatus value) noexcept
{
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] constexpr std::int32_t to_int(AgentKind value) noexcept
{
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] std::optional<NonRoadTerrain> as_non_road(Terrain terrain) noexcept;
[[nodiscard]] std::optional<MovementCost> movement_cost(NonRoadTerrain terrain) noexcept;
[[nodiscard]] std::optional<MovementCost> movement_cost(RoadStatus status) noexcept;

}  // namespace hexa_udon::core
