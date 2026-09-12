#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "spectrapack/geometry/conservative_fields.hpp"
#include "spectrapack/geometry/display_lod.hpp"

namespace spectrapack::representation_benchmark {
namespace geo = spectrapack::geometry;
using Json = nlohmann::json;
inline std::vector<std::byte> read_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  in.seekg(0, std::ios::end);
  const auto n = in.tellg();
  in.seekg(0);
  if (n < 0) throw std::runtime_error("cannot size " + p.string());
  std::vector<std::byte> r(static_cast<std::size_t>(n));
  in.read(reinterpret_cast<char*>(r.data()),
          static_cast<std::streamsize>(r.size()));
  if (!in) throw std::runtime_error("short read " + p.string());
  return r;
}
inline std::uint64_t fingerprint(std::span<const std::byte> b) {
  std::uint64_t h = 1469598103934665603ULL;
  for (auto x : b) {
    h ^= std::to_integer<unsigned char>(x);
    h *= 1099511628211ULL;
  }
  return h;
}
template <class T>
inline std::uint64_t fingerprint(std::span<const T> v) {
  return fingerprint(std::as_bytes(v));
}
inline Json vec(const geo::Vec3& v) { return {v[0], v[1], v[2]}; }
inline Json bounds(const geo::Bounds& b) {
  return {{"min", vec(b.min)}, {"max", vec(b.max)}};
}
inline const char* purpose(geo::FieldPurpose p) {
  return p == geo::FieldPurpose::object_kernel         ? "object_kernel"
         : p == geo::FieldPurpose::placed_pair_blocker ? "placed_pair_blocker"
                                                       : "container_blocker";
}
inline Json field(const geo::CellField& f) {
  std::vector<std::uint8_t> packed((f.cells().size() + 7) / 8);
  for (std::size_t i = 0; i < f.cells().size(); ++i)
    if (f.cells()[i]) packed[i / 8] |= std::uint8_t(1u << (i % 8));
  std::string hex;
  hex.reserve(packed.size() * 2);
  static constexpr char d[] = "0123456789abcdef";
  for (auto b : packed) {
    hex.push_back(d[b >> 4]);
    hex.push_back(d[b & 15]);
  }
  const auto& w = f.window();
  const auto& s = f.stats();
  return {{"window",
           {{"origin_mm", vec(w.lattice.origin_mm)},
            {"pitch_mm", w.lattice.pitch_mm},
            {"first", w.first},
            {"shape", w.shape}}},
          {"purpose", purpose(f.purpose())},
          {"encoding", "bitpacked_x_fast_lsb"},
          {"bits_hex", hex},
          {"fingerprint", fingerprint(f.cells())},
          {"stats",
           {{"working_bytes_peak", s.working_bytes_peak},
            {"kernel_work", s.kernel_work},
            {"cell_visits", s.cell_visits},
            {"occupied_cells", s.occupied_cells},
            {"uncertain_cells", s.uncertain_cells}}}};
}
inline std::shared_ptr<const geo::AcceptedSolid> accepted(
    const std::filesystem::path& p, geo::AssetRole role,
    geo::ImportLimits limits = {}) {
  auto d = geo::inspect_stl(read_bytes(p), {role, geo::Units::mm, 1.0, limits});
  if (!std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(d)) {
    const auto& e = std::get<geo::ImportFailure>(d);
    throw std::runtime_error("inspect " + e.code + ": " + e.message);
  }
  const auto accepted_draft =
      std::get<std::shared_ptr<const geo::AssetDraft>>(d);
  auto a = geo::accept_asset(accepted_draft);
  if (!std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(a)) {
    const auto& e = std::get<geo::ImportFailure>(a);
    const auto& r = accepted_draft->report();
    throw std::runtime_error(
        "accept " + e.code + ": " + e.message +
        " (validity=" + std::to_string(static_cast<int>(r.validity)) +
        ", topology=" + std::to_string(static_cast<int>(r.topology_check)) +
        ", intersection=" +
        std::to_string(static_cast<int>(r.intersection_check)) +
        ", predicate_work=" + std::to_string(r.predicate_work) +
        ", candidate_pairs=" + std::to_string(r.candidate_pair_tests) + ")");
  }
  return std::get<std::shared_ptr<const geo::AcceptedSolid>>(std::move(a));
}
inline Json solid(const std::filesystem::path& p, const geo::AcceptedSolid& a) {
  const auto m = a.mesh();
  const auto& r = a.report();
  return {{"path", p.generic_string()},
          {"source_bytes", std::filesystem::file_size(p)},
          {"source_fingerprint", fingerprint(read_bytes(p))},
          {"frame",
           {{"source_bounds", bounds(a.frame().source_bounds)},
            {"unit_scale_mm", a.frame().unit_scale_mm},
            {"anchor_mm", vec(a.frame().anchor_mm)},
            {"dimensions_mm", vec(a.frame().dimensions_mm)}}},
          {"accepted",
           {{"vertices", m.vertices.size()},
            {"triangles", m.triangles.size()},
            {"bounds_mm", bounds(a.bounds_mm())},
            {"volume_mm3", r.volume_mm3.value()},
            {"vertex_fingerprint", fingerprint(m.vertices)},
            {"triangle_fingerprint", fingerprint(m.triangles)}}},
          {"import_diagnostics",
           {{"validity", static_cast<int>(r.validity)},
            {"source_triangles", r.source_triangle_count},
            {"candidate_pair_tests", r.candidate_pair_tests},
            {"predicate_work", r.predicate_work},
            {"boundary_edges", r.boundary_edges},
            {"nonmanifold_edges", r.nonmanifold_edges},
            {"self_intersection_pairs", r.self_intersection_pairs},
            {"topology_check", static_cast<int>(r.topology_check)},
            {"intersection_check", static_cast<int>(r.intersection_check)}}}};
}
inline double elapsed_ms(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t)
      .count();
}
}  // namespace spectrapack::representation_benchmark
