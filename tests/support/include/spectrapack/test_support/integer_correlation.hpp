#pragma once

#include <cstddef>
#include <vector>

namespace spectrapack::test_support {

struct Extent3 {
  int x{};
  int y{};
  int z{};

  [[nodiscard]] std::size_t cell_count() const;
};

struct IntField3 {
  Extent3 extent;
  std::vector<int> cells;

  IntField3(Extent3 extent, std::vector<int> cells);
  [[nodiscard]] int at(int x, int y, int z) const;
};

struct TranslationRange3 {
  int min_x{};
  int max_x{};
  int min_y{};
  int max_y{};
  int min_z{};
  int max_z{};
};

struct CorrelationResult {
  TranslationRange3 translations;
  Extent3 extent;
  std::vector<int> values;

  [[nodiscard]] int at_translation(int tx, int ty, int tz) const;
};

// Storage is X-fastest: index = x + extent.x * (y + extent.y * z).
// `kernel_origin` is the local integer coordinate of kernel cell (0, 0, 0).
// A kernel cell i occupies blocked-field coordinate t + kernel_origin + i.
// The returned field covers every translation with at least one overlapping
// cell; its value is sum B(x) * O(x - t), evaluated exactly as integers.
[[nodiscard]] CorrelationResult direct_linear_cross_correlation(
    const IntField3& blocked,
    const IntField3& kernel,
    Extent3 kernel_origin);

}  // namespace spectrapack::test_support
