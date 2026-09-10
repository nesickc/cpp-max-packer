#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "spectrapack/geometry/import.hpp"
#include "../src/solid_analysis.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace geo = spectrapack::geometry;
namespace {
std::vector<std::byte> ascii(std::string_view body) {
  std::vector<std::byte> out(body.size()); std::memcpy(out.data(), body.data(), body.size()); return out;
}
std::vector<std::byte> triangle_ascii() { return ascii("solid test\nfacet normal 9 8 7\nouter loop\nvertex -1 2 3\nvertex 1 2 3\nvertex -1 4 3\nendloop\nendfacet\nendsolid test\n"); }
void put32(std::vector<std::byte>& b, std::size_t at, std::uint32_t n) { for (int i=0;i<4;++i) b[at+i]=std::byte(n >> (8*i)); }
void putf(std::vector<std::byte>& b, std::size_t at, float n) { put32(b, at, std::bit_cast<std::uint32_t>(n)); }
std::vector<std::byte> binary_triangle(bool solid_header=false) {
  std::vector<std::byte> b(134); if (solid_header) { constexpr char h[]="solid binary"; std::memcpy(b.data(),h,sizeof(h)-1); } put32(b,80,1);
  for (int i=0;i<3;++i) putf(b,84+i*4,0); const float p[9]={-1,2,3,1,2,3,-1,4,3}; for(int i=0;i<9;++i) putf(b,96+i*4,p[i]); return b;
}
const geo::AssetDraft& draft(const geo::ImportOutcome<geo::AssetDraft>& o) { REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(o)); return *std::get<std::shared_ptr<const geo::AssetDraft>>(o); }

std::vector<std::byte> cube_ascii(
    bool seam = false, bool add_zero_area_outlier = false,
    geo::Vec3 low = {-1, -1, -1}, geo::Vec3 high = {1, 1, 1}) {
  std::array<geo::Vec3, 8> vertices{{
      {low[0], low[1], low[2]}, {high[0], low[1], low[2]},
      {high[0], high[1], low[2]}, {low[0], high[1], low[2]},
      {low[0], low[1], high[2]}, {high[0], low[1], high[2]},
      {high[0], high[1], high[2]}, {low[0], high[1], high[2]}}};
  const std::array<geo::Triangle, 12> faces{{
      {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
      {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},
      {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}}};
  std::ostringstream text;
  text << "solid cube\n";
  for (std::size_t face_index = 0; face_index != faces.size(); ++face_index) {
    text << "facet normal 0 0 0\nouter loop\n";
    for (int corner = 0; corner != 3; ++corner) {
      auto point = vertices[faces[face_index][corner]];
      if (seam && face_index == 0 && corner == 0) point[0] += 0.05;
      text << "vertex " << point[0] << ' ' << point[1] << ' ' << point[2] << "\n";
    }
    text << "endloop\nendfacet\n";
  }
  if (add_zero_area_outlier) {
    text << "facet normal 0 0 0\nouter loop\n"
            "vertex 100 0 0\nvertex 101 0 0\nvertex 102 0 0\n"
            "endloop\nendfacet\n";
  }
  text << "endsolid cube\n";
  return ascii(text.str());
}

std::vector<std::byte> cube_binary() {
  const std::array<geo::Vec3, 8> vertices{{
      {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
      {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}};
  const std::array<geo::Triangle, 12> faces{{
      {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
      {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},
      {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}}};
  std::vector<std::byte> bytes(84 + 50 * faces.size());
  constexpr char header[] = "solid binary cube";
  std::memcpy(bytes.data(), header, sizeof(header) - 1);
  put32(bytes, 80, static_cast<std::uint32_t>(faces.size()));
  for (std::size_t face = 0; face != faces.size(); ++face) {
    const auto base = 84 + face * 50;
    for (int coordinate = 0; coordinate != 3; ++coordinate) putf(bytes, base + coordinate * 4, 0);
    for (int corner = 0; corner != 3; ++corner) {
      const auto& point = vertices[faces[face][corner]];
      for (int coordinate = 0; coordinate != 3; ++coordinate) {
        putf(bytes, base + 12 + corner * 12 + coordinate * 4,
             static_cast<float>(point[coordinate]));
      }
    }
  }
  return bytes;
}
}
TEST_CASE("AT-03 parses ASCII and binary independent encodings with a solid binary header", "[import][AT-03]") {
  auto a=geo::inspect_stl(triangle_ascii(), {geo::AssetRole::object,geo::Units::inch}); auto b=geo::inspect_stl(binary_triangle(true), {geo::AssetRole::object,geo::Units::inch});
  const auto& ad=draft(a); const auto& bd=draft(b); CHECK(ad.report().encoding==geo::StlEncoding::ascii); CHECK(bd.report().encoding==geo::StlEncoding::binary); REQUIRE(ad.mesh().vertices.size()==bd.mesh().vertices.size()); for (std::size_t i=0;i<ad.mesh().vertices.size();++i) CHECK(ad.mesh().vertices[i]==bd.mesh().vertices[i]); CHECK(ad.frame().unit_scale_mm==Catch::Approx(25.4)); CHECK(ad.frame().source_bounds.min==geo::Vec3{-1,2,3}); CHECK(ad.frame().anchor_mm[0]==Catch::Approx(0)); CHECK(ad.frame().anchor_mm[1]==Catch::Approx(76.2)); CHECK(ad.frame().anchor_mm[2]==Catch::Approx(76.2));
}
TEST_CASE("AT-03 keeps container minimum source frame and rejects malformed or nonfinite input", "[import][AT-03]") {
  auto container=geo::inspect_stl(triangle_ascii(), {geo::AssetRole::container,geo::Units::custom,2.0}); CHECK(draft(container).frame().anchor_mm==geo::Vec3{-2,4,6});
  auto truncated=geo::inspect_stl(ascii("solid t"), {geo::AssetRole::object,geo::Units::mm}); REQUIRE(std::holds_alternative<geo::ImportFailure>(truncated)); CHECK(std::get<geo::ImportFailure>(truncated).reason=="TRUNCATED_STL");
  auto bad=geo::inspect_stl(ascii("solid x\nfacet normal 0 0 0\nouter loop\nvertex nan 0 0\n"), {geo::AssetRole::object,geo::Units::mm}); REQUIRE(std::holds_alternative<geo::ImportFailure>(bad)); CHECK(std::get<geo::ImportFailure>(bad).reason=="NONFINITE_COORDINATE");
  auto scale=geo::inspect_stl(triangle_ascii(), {geo::AssetRole::object,geo::Units::custom,0}); REQUIRE(std::holds_alternative<geo::ImportFailure>(scale)); CHECK(std::get<geo::ImportFailure>(scale).reason=="INVALID_SCALE");
}
TEST_CASE("AT-04 exact cleanup keeps positive-area faces while removing exact duplicates and zero area", "[import][AT-04]") {
  auto bytes=ascii("solid x\nfacet normal 0 0 -1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 2 0 0\nendloop\nendfacet\nendsolid x\n");
  auto outcome=geo::inspect_stl(bytes,{geo::AssetRole::object,geo::Units::mm}); const auto& d=draft(outcome); CHECK(d.mesh().triangles.size()==1); CHECK(d.report().cleanup.exact_vertices_merged==5); CHECK(d.report().duplicate_faces==1); CHECK(d.report().zero_area_faces==1); CHECK(d.report().validity==geo::Validity::invalid);
}
TEST_CASE("AT-03 accepts locale-independent leading plus and reports resource caps structurally", "[import][AT-03]") {
  auto plus=ascii("solid x\nfacet normal +0 +0 +1\nouter loop\nvertex +0 +0 +0\nvertex +1 +0 +0\nvertex +0 +1 +0\nendloop\nendfacet\nendsolid x\n");
  CHECK(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(geo::inspect_stl(plus,{geo::AssetRole::object,geo::Units::mm})));
  geo::ImportLimits limits{}; limits.max_source_bytes=8;
  auto capped=geo::inspect_stl(triangle_ascii(),{geo::AssetRole::object,geo::Units::mm,1.0,limits});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(capped)); CHECK(std::get<geo::ImportFailure>(capped).code=="MEMORY_LIMIT");

  auto ambiguous_sign=ascii("solid x\nfacet normal +-0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid x\n");
  auto malformed=geo::inspect_stl(ambiguous_sign,{geo::AssetRole::object,geo::Units::mm});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(malformed));
  CHECK(std::get<geo::ImportFailure>(malformed).reason=="MALFORMED_ASCII");
}
TEST_CASE("AT-03 keeps inspected mesh owned after caller destroys source bytes", "[import][AT-03]") {
  auto bytes=triangle_ascii(); auto outcome=geo::inspect_stl(bytes,{geo::AssetRole::object,geo::Units::mm}); bytes.clear(); bytes.shrink_to_fit();
  const auto& inspected=draft(outcome); REQUIRE(inspected.mesh().vertices.size()==3); CHECK(inspected.mesh().vertices[0]==geo::Vec3{-1,-1,0});
}
TEST_CASE("AT-04 refuses null or indeterminate acceptance and never creates an implicit repair", "[import][AT-04]") {
  auto null_accept=geo::accept_asset({}); REQUIRE(std::holds_alternative<geo::ImportFailure>(null_accept));
  auto inspected=geo::inspect_stl(triangle_ascii(),{geo::AssetRole::object,geo::Units::mm}); auto handle=std::get<std::shared_ptr<const geo::AssetDraft>>(inspected);
  CHECK(handle->report().validity==geo::Validity::invalid);
  auto rejected=geo::accept_asset(handle); REQUIRE(std::holds_alternative<geo::ImportFailure>(rejected));
  auto repair=geo::propose_weld(handle,{0.1,10});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::RepairProposal>>(repair));
  CHECK(std::get<std::shared_ptr<const geo::RepairProposal>>(repair)->candidate()->report().validity == geo::Validity::invalid);
}

TEST_CASE("AT-03 imports and accepts an analytic ASCII cube with authoritative owned bounds",
          "[import][AT-03][AT-04]") {
  auto source = cube_ascii();
  auto outcome = geo::inspect_stl(source, {geo::AssetRole::object, geo::Units::inch});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(outcome));
  auto inspected = std::get<std::shared_ptr<const geo::AssetDraft>>(std::move(outcome));
  CHECK(inspected->report().validity == geo::Validity::valid);
  CHECK(inspected->frame().dimensions_mm == geo::Vec3{50.8, 50.8, 50.8});
  REQUIRE(inspected->report().volume_mm3);
  CHECK(*inspected->report().volume_mm3 == Catch::Approx(50.8 * 50.8 * 50.8));
  CHECK(inspected->mesh().vertices.size() == 8);
  CHECK(inspected->mesh().triangles.size() == 12);

  auto accepted_result = geo::accept_asset(inspected);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted_result));
  auto accepted = std::get<std::shared_ptr<const geo::AcceptedSolid>>(std::move(accepted_result));
  inspected.reset();
  source.clear();
  REQUIRE(accepted->mesh().vertices.size() == 8);
  CHECK(accepted->report().validity == geo::Validity::valid);
  const auto bounds = accepted->bounds_mm();
  CHECK(bounds.min == geo::Vec3{-25.4, -25.4, -25.4});
  CHECK(bounds.max == geo::Vec3{25.4, 25.4, 25.4});
}

TEST_CASE("AT-03 equivalent accepted ASCII and binary cubes have identical geometry",
          "[import][AT-03]") {
  const auto ascii_result = geo::inspect_stl(
      cube_ascii(), {geo::AssetRole::object, geo::Units::mm});
  const auto binary_result = geo::inspect_stl(
      cube_binary(), {geo::AssetRole::object, geo::Units::mm});
  const auto& ascii_draft = draft(ascii_result);
  const auto& binary_draft = draft(binary_result);
  REQUIRE(ascii_draft.report().validity == geo::Validity::valid);
  REQUIRE(binary_draft.report().validity == geo::Validity::valid);
  CHECK(ascii_draft.mesh().vertices.size() == binary_draft.mesh().vertices.size());
  CHECK(ascii_draft.mesh().triangles.size() == binary_draft.mesh().triangles.size());
  CHECK(ascii_draft.report().volume_mm3 == binary_draft.report().volume_mm3);
  CHECK(ascii_draft.report().mesh_bounds_mm->min == binary_draft.report().mesh_bounds_mm->min);
  CHECK(ascii_draft.report().mesh_bounds_mm->max == binary_draft.report().mesh_bounds_mm->max);
}

TEST_CASE("AT-03 one-inch cube preserves physical dimensions and volume",
          "[import][AT-03][units]") {
  const auto result = geo::inspect_stl(
      cube_ascii(false, false, {0, 0, 0}, {1, 1, 1}),
      {geo::AssetRole::object, geo::Units::inch});
  const auto& inspected = draft(result);
  CHECK(inspected.frame().dimensions_mm == geo::Vec3{25.4, 25.4, 25.4});
  REQUIRE(inspected.report().mesh_bounds_mm);
  CHECK(inspected.report().mesh_bounds_mm->min == geo::Vec3{-12.7, -12.7, -12.7});
  CHECK(inspected.report().mesh_bounds_mm->max == geo::Vec3{12.7, 12.7, 12.7});
  REQUIRE(inspected.report().volume_mm3);
  CHECK(*inspected.report().volume_mm3 == Catch::Approx(25.4 * 25.4 * 25.4));
}

TEST_CASE("AT-04 weld proposal closes a bounded seam without mutating the original draft",
          "[import][AT-04][weld]") {
  auto outcome = geo::inspect_stl(
      cube_ascii(true), {geo::AssetRole::object, geo::Units::mm});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(outcome));
  auto original = std::get<std::shared_ptr<const geo::AssetDraft>>(std::move(outcome));
  CHECK(original->report().validity == geo::Validity::invalid);
  const auto original_vertices = original->mesh().vertices.size();

  auto proposal_result = geo::propose_weld(original, {0.1, 1'000});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::RepairProposal>>(proposal_result));
  auto proposal = std::get<std::shared_ptr<const geo::RepairProposal>>(std::move(proposal_result));
  REQUIRE(proposal->original() == original);
  REQUIRE(proposal->candidate());
  CHECK(proposal->candidate()->report().validity == geo::Validity::valid);
  CHECK(proposal->max_displacement_mm() > 0.0);
  CHECK(proposal->max_displacement_mm() <= 0.1);
  CHECK(original->mesh().vertices.size() == original_vertices);
  CHECK(original->report().validity == geo::Validity::invalid);

  auto bypass = geo::accept_asset(proposal->candidate());
  REQUIRE(std::holds_alternative<geo::ImportFailure>(bypass));
  CHECK(std::get<geo::ImportFailure>(bypass).reason == "REPAIR_PROVENANCE_REQUIRED");

  auto chained = geo::propose_weld(proposal->candidate(), {0.05, 1'000});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(chained));
  CHECK(std::get<geo::ImportFailure>(chained).reason == "REPAIR_PROVENANCE_REQUIRED");

  auto accepted_result = geo::accept_repair(proposal);
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AcceptedSolid>>(accepted_result));
  const auto accepted = std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted_result);
  CHECK(accepted->report().validity == geo::Validity::valid);
  CHECK(accepted->mesh().vertices.size() == 8);
}

TEST_CASE("AT-04 import cleanup and solid analysis share one predicate budget",
          "[import][AT-04][limits]") {
  const auto generous = geo::inspect_stl(
      cube_ascii(), {geo::AssetRole::object, geo::Units::mm});
  const auto& inspected = draft(generous);
  const auto analysis_only = geo::detail::analyze_solid(inspected.mesh(), {});
  REQUIRE(analysis_only.report.validity == geo::Validity::valid);
  REQUIRE(inspected.report().validity == geo::Validity::valid);
  REQUIRE(inspected.report().predicate_work > analysis_only.report.predicate_work);

  geo::ImportLimits limits;
  limits.max_predicate_work = analysis_only.report.predicate_work;
  const auto capped = geo::inspect_stl(
      cube_ascii(), {geo::AssetRole::object, geo::Units::mm, 1.0, limits});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(capped));
  CHECK(std::get<std::shared_ptr<const geo::AssetDraft>>(capped)->report().validity !=
        geo::Validity::valid);
}

TEST_CASE("AT-04 weld work is bounded and distance does not chain or use an AABB corner",
          "[import][AT-04][weld][limits]") {
  const auto chain_source = ascii(
      "solid chain\n"
      "facet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 0 1 0\nvertex 0 0 1\nendloop\nendfacet\n"
      "facet normal 0 0 0\nouter loop\nvertex .09 0 0\nvertex 2 1 0\nvertex 2 0 1\nendloop\nendfacet\n"
      "facet normal 0 0 0\nouter loop\nvertex .18 0 0\nvertex 4 1 0\nvertex 4 0 1\nendloop\nendfacet\n"
      "endsolid chain\n");
  auto chain = std::get<std::shared_ptr<const geo::AssetDraft>>(
      geo::inspect_stl(chain_source, {geo::AssetRole::object, geo::Units::mm}));
  auto chain_result = geo::propose_weld(chain, {0.1, 1'000});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::RepairProposal>>(chain_result));
  const auto chain_proposal = std::get<std::shared_ptr<const geo::RepairProposal>>(chain_result);
  CHECK(chain_proposal->candidate()->mesh().vertices.size() == chain->mesh().vertices.size() - 1);

  const auto corner_source = ascii(
      "solid corner\n"
      "facet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 0 1 0\nvertex 0 0 1\nendloop\nendfacet\n"
      "facet normal 0 0 0\nouter loop\nvertex .09 .09 0\nvertex 2 1 0\nvertex 2 0 1\nendloop\nendfacet\n"
      "endsolid corner\n");
  auto corner = std::get<std::shared_ptr<const geo::AssetDraft>>(
      geo::inspect_stl(corner_source, {geo::AssetRole::object, geo::Units::mm}));
  auto corner_result = geo::propose_weld(corner, {0.1, 1'000});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::RepairProposal>>(corner_result));
  CHECK(std::get<std::shared_ptr<const geo::RepairProposal>>(corner_result)
            ->candidate()->mesh().vertices.size() == corner->mesh().vertices.size());

  auto bounded = geo::propose_weld(corner, {0.1, 1});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(bounded));
  CHECK(std::get<geo::ImportFailure>(bounded).reason == "WELD_CANDIDATE_PAIRS");
}

TEST_CASE("AT-03 frame conversion checks scaled endpoints and rejects precision collapse",
          "[import][AT-03][frame]") {
  const double huge = 1e308;
  std::ostringstream wide;
  wide << std::setprecision(std::numeric_limits<double>::max_digits10);
  wide << "solid wide\nfacet normal 0 0 1\nouter loop\n"
          "vertex " << -huge << " 0 0\nvertex " << huge << " 0 0\nvertex "
       << -huge << ' ' << huge << " 0\nendloop\nendfacet\nendsolid wide\n";
  auto converted = geo::inspect_stl(
      ascii(wide.str()), {geo::AssetRole::object, geo::Units::custom, 1e-300});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(converted));
  const auto& frame = std::get<std::shared_ptr<const geo::AssetDraft>>(converted)->frame();
  CHECK(frame.dimensions_mm[0] == Catch::Approx(2e8));
  CHECK(frame.dimensions_mm[1] == Catch::Approx(1e8));
  CHECK(frame.anchor_mm[0] == 0.0);

  const double next = std::nextafter(1.0, 2.0);
  const double tiny = std::numeric_limits<double>::denorm_min();
  std::ostringstream collapsing;
  collapsing << std::setprecision(std::numeric_limits<double>::max_digits10);
  collapsing << "solid collapse\nfacet normal 0 0 1\nouter loop\n"
                "vertex 1 0 0\nvertex " << next << " 0 0\nvertex 1 " << tiny
             << " 0\nendloop\nendfacet\nendsolid collapse\n";
  auto rejected = geo::inspect_stl(
      ascii(collapsing.str()), {geo::AssetRole::object, geo::Units::custom, tiny});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(rejected));
  CHECK(std::get<geo::ImportFailure>(rejected).reason == "FRAME_PRECISION_LOSS");
}

TEST_CASE("AT-04 removed outliers do not affect authoritative mesh bounds and signed zero is normalized",
          "[import][AT-04]") {
  auto outcome = geo::inspect_stl(
      cube_ascii(false, true), {geo::AssetRole::object, geo::Units::mm});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(outcome));
  const auto inspected = std::get<std::shared_ptr<const geo::AssetDraft>>(outcome);
  REQUIRE(inspected->report().mesh_bounds_mm);
  const auto actual = *inspected->report().mesh_bounds_mm;
  CHECK(actual.max[0] - actual.min[0] == Catch::Approx(2.0));
  CHECK(inspected->frame().dimensions_mm[0] == Catch::Approx(103.0));
  CHECK(inspected->report().zero_area_faces == 1);
  for (const auto& vertex : inspected->mesh().vertices) {
    for (const double coordinate : vertex) {
      if (coordinate == 0.0) CHECK_FALSE(std::signbit(coordinate));
    }
  }
}

TEST_CASE("AT-03 parses multiple ASCII solid sections and rejects trailing junk",
          "[import][AT-03]") {
  const auto sections = ascii(
      "solid a\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid a\n"
      "solid b\nfacet normal 0 0 1\nouter loop\nvertex 2 0 0\nvertex 3 0 0\nvertex 2 1 0\nendloop\nendfacet\nendsolid b\n");
  auto parsed = geo::inspect_stl(sections, {geo::AssetRole::object, geo::Units::mm});
  REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(parsed));
  CHECK(std::get<std::shared_ptr<const geo::AssetDraft>>(parsed)->report().source_triangle_count == 2);

  auto trailing = sections;
  const std::string junk = "junk";
  trailing.insert(trailing.end(), reinterpret_cast<const std::byte*>(junk.data()),
                  reinterpret_cast<const std::byte*>(junk.data() + junk.size()));
  auto rejected = geo::inspect_stl(trailing, {geo::AssetRole::object, geo::Units::mm});
  REQUIRE(std::holds_alternative<geo::ImportFailure>(rejected));
  CHECK(std::get<geo::ImportFailure>(rejected).reason == "MALFORMED_ASCII");
}

TEST_CASE("AT-03 imports separate closed ASCII components in one source",
          "[import][AT-03]") {
  auto first = cube_ascii(false, false, {0, 0, 0}, {1, 1, 1});
  auto second = cube_ascii(false, false, {3, 0, 0}, {4, 1, 1});
  first.insert(first.end(), second.begin(), second.end());
  const auto outcome = geo::inspect_stl(
      first, {geo::AssetRole::object, geo::Units::mm});
  const auto& inspected = draft(outcome);
  CHECK(inspected.report().validity == geo::Validity::valid);
  CHECK(inspected.report().component_count == 2);
  REQUIRE(inspected.report().volume_mm3);
  CHECK(*inspected.report().volume_mm3 == Catch::Approx(2.0));
}
