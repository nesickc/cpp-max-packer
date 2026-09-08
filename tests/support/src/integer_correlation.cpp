#include "spectrapack/test_support/integer_correlation.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace spectrapack::test_support {
namespace {

void validate_extent(Extent3 extent) {
  if (extent.x <= 0 || extent.y <= 0 || extent.z <= 0) {
    throw std::invalid_argument("field extents must be positive");
  }
  std::size_t count = static_cast<std::size_t>(extent.x);
  for (const int dimension : {extent.y, extent.z}) {
    if (count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(dimension)) {
      throw std::invalid_argument("field extent is too large");
    }
    count *= static_cast<std::size_t>(dimension);
  }
  if (count == 0) {
    throw std::invalid_argument("field extent is too large");
  }
}

int coordinate_index(int coordinate, int minimum, int extent) {
  const auto index = static_cast<long long>(coordinate) - static_cast<long long>(minimum);
  if (index < 0 || index >= extent) {
    throw std::out_of_range("translation is outside full linear correlation range");
  }
  return index;
}

int checked_int(long long value, const char* message) {
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(message);
  }
  return static_cast<int>(value);
}

int checked_extent_sum(int first, int second) {
  return checked_int(static_cast<long long>(first) + static_cast<long long>(second) - 1,
                     "correlation output extent is too large");
}

}  // namespace

std::size_t Extent3::cell_count() const {
  validate_extent(*this);
  return static_cast<std::size_t>(x) * static_cast<std::size_t>(y) * static_cast<std::size_t>(z);
}

IntField3::IntField3(Extent3 input_extent, std::vector<int> input_cells)
    : extent(input_extent), cells(std::move(input_cells)) {
  if (cells.size() != extent.cell_count()) {
    throw std::invalid_argument("field cell count does not match extent");
  }
}

int IntField3::at(int x, int y, int z) const {
  if (x < 0 || x >= extent.x || y < 0 || y >= extent.y || z < 0 || z >= extent.z) {
    throw std::out_of_range("field cell coordinate is outside extent");
  }
  return cells[static_cast<std::size_t>(x) + static_cast<std::size_t>(extent.x) *
      (static_cast<std::size_t>(y) + static_cast<std::size_t>(extent.y) * static_cast<std::size_t>(z))];
}

int CorrelationResult::at_translation(int tx, int ty, int tz) const {
  const auto x = coordinate_index(tx, translations.min_x, extent.x);
  const auto y = coordinate_index(ty, translations.min_y, extent.y);
  const auto z = coordinate_index(tz, translations.min_z, extent.z);
  return values[static_cast<std::size_t>(x) + static_cast<std::size_t>(extent.x) *
      (static_cast<std::size_t>(y) + static_cast<std::size_t>(extent.y) * static_cast<std::size_t>(z))];
}

CorrelationResult direct_linear_cross_correlation(
    const IntField3& blocked, const IntField3& kernel, Extent3 kernel_origin) {
  const auto minimum = [](int kernel_extent, int origin) {
    return -((static_cast<long long>(kernel_extent) - 1) + static_cast<long long>(origin));
  };
  const auto maximum = [](int blocked_extent, int origin) {
    return (static_cast<long long>(blocked_extent) - 1) - static_cast<long long>(origin);
  };
  const TranslationRange3 range{
      checked_int(minimum(kernel.extent.x, kernel_origin.x), "translation range is too large"),
      checked_int(maximum(blocked.extent.x, kernel_origin.x), "translation range is too large"),
      checked_int(minimum(kernel.extent.y, kernel_origin.y), "translation range is too large"),
      checked_int(maximum(blocked.extent.y, kernel_origin.y), "translation range is too large"),
      checked_int(minimum(kernel.extent.z, kernel_origin.z), "translation range is too large"),
      checked_int(maximum(blocked.extent.z, kernel_origin.z), "translation range is too large")};
  const Extent3 output_extent{checked_extent_sum(blocked.extent.x, kernel.extent.x),
                              checked_extent_sum(blocked.extent.y, kernel.extent.y),
                              checked_extent_sum(blocked.extent.z, kernel.extent.z)};
  std::vector<int> output(output_extent.cell_count(), 0);

  for (long long tz = range.min_z; tz <= range.max_z; ++tz) {
    for (long long ty = range.min_y; ty <= range.max_y; ++ty) {
      for (long long tx = range.min_x; tx <= range.max_x; ++tx) {
        long long sum = 0;
        for (int kz = 0; kz < kernel.extent.z; ++kz) {
          for (int ky = 0; ky < kernel.extent.y; ++ky) {
            for (int kx = 0; kx < kernel.extent.x; ++kx) {
              const long long bx = tx + kernel_origin.x + kx;
              const long long by = ty + kernel_origin.y + ky;
              const long long bz = tz + kernel_origin.z + kz;
              if (bx >= 0 && bx < blocked.extent.x && by >= 0 && by < blocked.extent.y &&
                  bz >= 0 && bz < blocked.extent.z) {
                const long long term = static_cast<long long>(blocked.at(static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bz))) * kernel.at(kx, ky, kz);
                if ((term > 0 && sum > std::numeric_limits<long long>::max() - term) ||
                    (term < 0 && sum < std::numeric_limits<long long>::min() - term)) {
                  throw std::overflow_error("integer correlation accumulation overflowed");
                }
                sum += term;
              }
            }
          }
        }
        if (sum < std::numeric_limits<int>::min() || sum > std::numeric_limits<int>::max()) {
          throw std::overflow_error("integer correlation result is outside the int domain");
        }
        const auto index = static_cast<std::size_t>(tx - range.min_x) +
            static_cast<std::size_t>(output_extent.x) *
            (static_cast<std::size_t>(ty - range.min_y) +
             static_cast<std::size_t>(output_extent.y) * static_cast<std::size_t>(tz - range.min_z));
        output[index] = static_cast<int>(sum);
      }
    }
  }
  return {range, output_extent, std::move(output)};
}

}  // namespace spectrapack::test_support
