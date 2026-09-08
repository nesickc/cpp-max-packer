#include "spectrapack/test_support/analytic_fixtures.hpp"
#include "spectrapack/test_support/integer_correlation.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options { int samples = 5; int warmup = 1; };
struct Workload { std::string name; std::uint64_t work_units; std::vector<double> samples_ms; std::uint64_t checksum; };

int parse_bounded_positive(const char* text, bool allow_zero) {
  std::size_t parsed{};
  const auto value = std::stoi(text, &parsed);
  if (text[parsed] != '\0' || value < (allow_zero ? 0 : 1) || value > 1000) {
    throw std::invalid_argument("value must be within the bounded benchmark range");
  }
  return value;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if ((argument == "--samples" || argument == "--warmup") && i + 1 < argc) {
      const auto value = parse_bounded_positive(argv[++i], argument == "--warmup");
      if (argument == "--samples") options.samples = value; else options.warmup = value;
    } else {
      throw std::invalid_argument("usage: spectrapack_reference_benchmark [--samples N] [--warmup N]");
    }
  }
  return options;
}

template <typename Function>
Workload measure(std::string name, std::uint64_t work_units, const Options& options, Function&& operation) {
  const std::uint64_t checksum = operation();
  for (int i = 0; i < options.warmup; ++i) {
    if (operation() != checksum) throw std::runtime_error("reference workload is not deterministic");
  }
  Workload workload{std::move(name), work_units, {}, checksum};
  workload.samples_ms.reserve(static_cast<std::size_t>(options.samples));
  for (int i = 0; i < options.samples; ++i) {
    const auto start = std::chrono::steady_clock::now();
    if (operation() != checksum) throw std::runtime_error("reference workload is not deterministic");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    workload.samples_ms.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
  }
  workload.checksum = checksum;
  return workload;
}

std::uint64_t correlation_workload() {
  const spectrapack::test_support::IntField3 small_blocked{{3, 2, 1}, {1, 2, 3, 4, 5, 6}};
  const spectrapack::test_support::IntField3 small_kernel{{2, 2, 1}, {2, 0, 1, 3}};
  const auto known = spectrapack::test_support::direct_linear_cross_correlation(small_blocked, small_kernel, {-1, 1, 0});
  if (known.extent.x != 4 || known.extent.y != 3 || known.at_translation(1, -1, 0) != 21) {
    throw std::runtime_error("integer correlation analytic precheck failed");
  }
  const spectrapack::test_support::IntField3 blocked{{23, 19, 3}, std::vector<int>(23 * 19 * 3, 0)};
  auto mutable_blocked = blocked;
  for (int z = 0; z < 3; ++z) for (int y = 0; y < 19; ++y) for (int x = 0; x < 23; ++x)
    mutable_blocked.cells[static_cast<std::size_t>(x + 23 * (y + 19 * z))] = (3 * x + 5 * y + 7 * z) % 11;
  std::vector<int> cells(7 * 5 * 2);
  for (std::size_t i = 0; i < cells.size(); ++i) cells[i] = static_cast<int>((i * 7 + 3) % 5);
  const spectrapack::test_support::IntField3 kernel{{7, 5, 2}, std::move(cells)};
  const auto result = spectrapack::test_support::direct_linear_cross_correlation(mutable_blocked, kernel, {-2, 3, -1});
  std::uint64_t checksum{};
  for (const int value : result.values) checksum = checksum * 1315423911u + static_cast<unsigned int>(value);
  return checksum;
}

std::uint64_t fixture_workload() {
  constexpr int iterations = 1000;
  std::uint64_t checksum{};
  for (int i = 0; i < iterations; ++i) {
    const auto cuboid = spectrapack::test_support::make_cuboid(2.5, 3.0, 4.0);
    const auto tetrahedron = spectrapack::test_support::make_tetrahedron(3.0);
    const auto prism = spectrapack::test_support::make_l_prism(1.0, 2.0);
    const double combined = spectrapack::test_support::signed_volume(cuboid) +
        spectrapack::test_support::signed_volume(tetrahedron) + spectrapack::test_support::signed_volume(prism);
    if (combined != 40.5) throw std::runtime_error("analytic fixture precheck failed");
    checksum = checksum * 1315423911u +
        static_cast<std::uint64_t>(cuboid.triangles.size() * 10000 + tetrahedron.triangles.size() * 100 + prism.triangles.size());
  }
  return checksum;
}

void write_json(const std::vector<Workload>& workloads) {
  std::cout << "{\"schema_version\":1,\"benchmark_kind\":\"test_reference_oracle\",\"workloads\":[";
  for (std::size_t i = 0; i < workloads.size(); ++i) {
    const auto& workload = workloads[i];
    if (i) std::cout << ',';
    std::cout << "{\"name\":\"" << workload.name << "\",\"work_units\":" << workload.work_units << ",\"samples_ms\":[";
    for (std::size_t j = 0; j < workload.samples_ms.size(); ++j) {
      if (j) std::cout << ',';
      std::cout << std::fixed << std::setprecision(6) << workload.samples_ms[j];
    }
    std::cout << "],\"checksum\":\"" << workload.checksum << "\"}";
  }
#if defined(_MSC_VER)
  const std::string compiler = "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
  const std::string compiler = "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#else
  const std::string compiler = "C++";
#endif
#ifdef NDEBUG
  constexpr const char* build_type = "Release";
#else
  constexpr const char* build_type = "Debug";
#endif
  std::cout << "],\"compiler\":\"" << compiler << "\",\"build_type\":\"" << build_type << "\"}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    // These fixed checks make a benchmark failure visible rather than timing invalid reference work.
    if (correlation_workload() == 0 || fixture_workload() == 0) throw std::runtime_error("reference checksum precheck failed");
    const std::vector<Workload> workloads{
        measure("integer_linear_cross_correlation_23x19x3__7x5x2", 23u * 19u * 3u * 7u * 5u * 2u, options, correlation_workload),
        measure("analytic_fixture_construction", 3000, options, fixture_workload)};
    write_json(workloads);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "spectrapack_reference_benchmark: " << error.what() << '\n';
    return 2;
  }
}
