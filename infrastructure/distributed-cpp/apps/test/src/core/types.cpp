#include "hexa_udon/core/types.hpp"

namespace hexa_udon::core {

std::optional<Direction> direction_from_int(const std::int32_t value) noexcept
{
    switch (value) {
    case 0: return Direction::UpperLeft;
    case 1: return Direction::UpperRight;
    case 2: return Direction::Right;
    case 3: return Direction::LowerRight;
    case 4: return Direction::LowerLeft;
    case 5: return Direction::Left;
    default: return std::nullopt;
    }
}

std::optional<Terrain> terrain_from_int(const std::int32_t value) noexcept
{
    switch (value) {
    case 0: return Terrain::Plain;
    case 1: return Terrain::Road;
    case 2: return Terrain::Mountain;
    case 3: return Terrain::Pond;
    default: return std::nullopt;
    }
}

std::optional<RoadStatus> road_status_from_int(const std::int32_t value) noexcept
{
    switch (value) {
    case 0: return RoadStatus::Smooth;
    case 1: return RoadStatus::Busy;
    case 2: return RoadStatus::Jammed;
    default: return std::nullopt;
    }
}

std::optional<AgentKind> agent_kind_from_int(const std::int32_t value) noexcept
{
    switch (value) {
    case 0: return AgentKind::Patrol;
    case 1: return AgentKind::Supply;
    default: return std::nullopt;
    }
}

std::optional<NonRoadTerrain> as_non_road(const Terrain terrain) noexcept
{
    switch (terrain) {
    case Terrain::Plain: return NonRoadTerrain::Plain;
    case Terrain::Mountain: return NonRoadTerrain::Mountain;
    case Terrain::Pond: return NonRoadTerrain::Pond;
    case Terrain::Road: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<MovementCost> movement_cost(const NonRoadTerrain terrain) noexcept
{
    switch (terrain) {
    case NonRoadTerrain::Plain: return MovementCost{2, 1};
    case NonRoadTerrain::Mountain: return MovementCost{3, 2};
    case NonRoadTerrain::Pond: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<MovementCost> movement_cost(const RoadStatus status) noexcept
{
    switch (status) {
    case RoadStatus::Smooth: return MovementCost{1, 2};
    case RoadStatus::Busy: return MovementCost{2, 2};
    case RoadStatus::Jammed: return MovementCost{4, 2};
    }
    return std::nullopt;
}

}  // namespace hexa_udon::core
