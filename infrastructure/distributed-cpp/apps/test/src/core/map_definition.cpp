#include "hexa_udon/core/map_definition.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace hexa_udon::core {

namespace {

constexpr std::array<Direction, 6> kDirections{
    Direction::UpperLeft,
    Direction::UpperRight,
    Direction::Right,
    Direction::LowerRight,
    Direction::LowerLeft,
    Direction::Left,
};

}  // namespace

Result<MapDefinition> MapDefinition::create(
    const GridSize height, const GridSize width, std::vector<Terrain> cells)
{
    ValidationErrors errors;
    if (height <= 0) {
        errors.push_back({ValidationErrorCode::InvalidDimension, "height", "height must be positive"});
    }
    if (width <= 0) {
        errors.push_back({ValidationErrorCode::InvalidDimension, "width", "width must be positive"});
    }

    const auto expected = static_cast<std::int64_t>(height) * static_cast<std::int64_t>(width);
    if (expected <= 0 || expected != static_cast<std::int64_t>(cells.size())) {
        errors.push_back({ValidationErrorCode::CellCountMismatch, "cells", "cell count must equal height * width"});
    }
    if (expected > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        errors.push_back({ValidationErrorCode::InvalidDimension, "height/width", "cell count exceeds CellIndex capacity"});
    }

    if (!errors.empty()) {
        return Result<MapDefinition>::failure(std::move(errors));
    }
    return Result<MapDefinition>::success(MapDefinition(height, width, std::move(cells)));
}

MapDefinition::MapDefinition(
    const GridSize height, const GridSize width, std::vector<Terrain> cells)
    : height_(height), width_(width), cells_(std::move(cells))
{
}

std::int32_t MapDefinition::cell_count() const noexcept
{
    return static_cast<std::int32_t>(cells_.size());
}

bool MapDefinition::contains(const HexCoord coord) const noexcept
{
    return coord.row >= 0 && coord.row < height_ && coord.col >= 0 && coord.col < width_;
}

bool MapDefinition::contains(const CellIndex index) const noexcept
{
    return index.value >= 0 && index.value < cell_count();
}

std::optional<CellIndex> MapDefinition::cell_index(const HexCoord coord) const noexcept
{
    if (!contains(coord)) {
        return std::nullopt;
    }
    return CellIndex{coord.row * width_ + coord.col};
}

std::optional<HexCoord> MapDefinition::coordinate(const CellIndex index) const noexcept
{
    if (!contains(index)) {
        return std::nullopt;
    }
    return HexCoord{index.value / width_, index.value % width_};
}

std::optional<Terrain> MapDefinition::terrain_at(const CellIndex index) const noexcept
{
    if (!contains(index)) {
        return std::nullopt;
    }
    return cells_[static_cast<std::size_t>(index.value)];
}

std::optional<CellIndex> MapDefinition::neighbor(
    const CellIndex origin, const Direction direction) const noexcept
{
    const auto coord = coordinate(origin);
    if (!coord.has_value()) {
        return std::nullopt;
    }

    HexCoord candidate = *coord;
    const bool even_row = coord->row % 2 == 0;
    switch (direction) {
    case Direction::UpperLeft:
        --candidate.row;
        candidate.col += even_row ? 0 : -1;
        break;
    case Direction::UpperRight:
        --candidate.row;
        candidate.col += even_row ? 1 : 0;
        break;
    case Direction::Right:
        ++candidate.col;
        break;
    case Direction::LowerRight:
        ++candidate.row;
        candidate.col += even_row ? 1 : 0;
        break;
    case Direction::LowerLeft:
        ++candidate.row;
        candidate.col += even_row ? 0 : -1;
        break;
    case Direction::Left:
        --candidate.col;
        break;
    default:
        return std::nullopt;
    }
    return cell_index(candidate);
}

MapDefinition::Neighbors MapDefinition::neighbors(const CellIndex origin) const noexcept
{
    Neighbors result{};
    for (std::size_t i = 0; i < kDirections.size(); ++i) {
        result[i] = neighbor(origin, kDirections[i]);
    }
    return result;
}

}  // namespace hexa_udon::core
