#pragma once

#include "hexa_udon/core/result.hpp"
#include "hexa_udon/core/types.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace hexa_udon::core {

class MapDefinition {
public:
    using Neighbors = std::array<std::optional<CellIndex>, 6>;

    [[nodiscard]] static Result<MapDefinition> create(
        GridSize height, GridSize width, std::vector<Terrain> cells);

    [[nodiscard]] GridSize height() const noexcept { return height_; }
    [[nodiscard]] GridSize width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t cell_count() const noexcept;
    [[nodiscard]] std::span<const Terrain> cells() const noexcept { return cells_; }

    [[nodiscard]] bool contains(HexCoord coord) const noexcept;
    [[nodiscard]] bool contains(CellIndex index) const noexcept;
    [[nodiscard]] std::optional<CellIndex> cell_index(HexCoord coord) const noexcept;
    [[nodiscard]] std::optional<HexCoord> coordinate(CellIndex index) const noexcept;
    [[nodiscard]] std::optional<Terrain> terrain_at(CellIndex index) const noexcept;
    [[nodiscard]] std::optional<CellIndex> neighbor(
        CellIndex origin, Direction direction) const noexcept;
    [[nodiscard]] Neighbors neighbors(CellIndex origin) const noexcept;

private:
    MapDefinition(GridSize height, GridSize width, std::vector<Terrain> cells);

    GridSize height_;
    GridSize width_;
    std::vector<Terrain> cells_;
};

}  // namespace hexa_udon::core
