#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "spectrapack/geometry/import.hpp"

namespace geo = spectrapack::geometry;

namespace {
std::vector<std::byte> practical_bytes(const char* filename)
{
    std::ifstream input(std::string(SPECTRAPACK_TEST_ROOT) + "/rc/items/" + filename, std::ios::binary);
    REQUIRE(input);
    input.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(input);
    return bytes;
}
}  // namespace

TEST_CASE("T011 native practical import rejects unadmitted replacement before mesh work", "[import][T011]")
{
    const auto bytes = practical_bytes("ulamok_2kg_simplified.stl");
    geo::ImportLimits limits;
    limits.max_working_bytes = 0;
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm, 1.0, limits });
    REQUIRE(std::holds_alternative<geo::ImportFailure>(inspected));
    CHECK(std::get<geo::ImportFailure>(inspected).code == "MEMORY_LIMIT");
    CHECK(std::get<geo::ImportFailure>(inspected).reason == "IMPORT_WORKING_BYTES");
}

TEST_CASE("T011 practical import admission retains exact accepted geometry and report", "[import][T011]")
{
    const auto bytes = practical_bytes("ulamok_2kg_simplified.stl");
    const auto estimate = geo::estimate_import_admission(bytes);
    REQUIRE(std::holds_alternative<geo::ImportAdmission>(estimate));
    const auto admitted = std::get<geo::ImportAdmission>(estimate);
    REQUIRE(admitted.triangle_upper_bound == 768);
    geo::ImportLimits limits;
    limits.max_working_bytes = admitted.working_bytes_upper_bound - 1;
    const auto rejected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm, 1.0, limits });
    REQUIRE(std::holds_alternative<geo::ImportFailure>(rejected));
    CHECK(std::get<geo::ImportFailure>(rejected).reason == "IMPORT_WORKING_BYTES");
    limits.max_working_bytes = admitted.working_bytes_upper_bound;
    const auto actual = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm, 1.0, limits });
    const auto legacy = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(actual));
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(legacy));
    const auto a = std::get<std::shared_ptr<const geo::AssetDraft>>(actual);
    const auto b = std::get<std::shared_ptr<const geo::AssetDraft>>(legacy);
    REQUIRE(a->report().validity == geo::Validity::valid);
    CHECK(std::equal(a->mesh().vertices.begin(), a->mesh().vertices.end(), b->mesh().vertices.begin(),
                     b->mesh().vertices.end()));
    CHECK(std::equal(a->mesh().triangles.begin(), a->mesh().triangles.end(), b->mesh().triangles.begin(),
                     b->mesh().triangles.end()));
    CHECK(a->frame().anchor_mm == b->frame().anchor_mm);
    CHECK(a->frame().dimensions_mm == b->frame().dimensions_mm);
    CHECK(a->report().volume_mm3 == b->report().volume_mm3);
    CHECK(a->report().predicate_work == b->report().predicate_work);
    CHECK(a->report().candidate_pair_tests == b->report().candidate_pair_tests);
    CHECK(a->report().source_triangle_count == b->report().source_triangle_count);
    CHECK(a->report().cleanup.faces_reoriented == b->report().cleanup.faces_reoriented);
    CHECK(a->report().issues.size() == b->report().issues.size());
    CHECK(a->report().shells.size() == b->report().shells.size());
}

TEST_CASE("T011 full Pryanik preparation fits actual session reserve and pinned source",
          "[import][T011][qualification]")
{
    const auto bytes = practical_bytes("pryanik_2.STL");
    const auto estimate = geo::estimate_import_admission(bytes);
    REQUIRE(std::holds_alternative<geo::ImportAdmission>(estimate));
    const auto admitted = std::get<geo::ImportAdmission>(estimate);
    CHECK(admitted.triangle_upper_bound == 139212);
    constexpr std::uint64_t combined_session_and_adapter_reserve = 72ULL << 20;
    constexpr std::uint64_t previous_bundle_allowance = 32ULL << 20;
    constexpr std::uint64_t report_and_preview_allowance = 16ULL << 20;
    constexpr std::uint64_t cap = 512ULL << 20;
    const auto remaining = cap - combined_session_and_adapter_reserve - previous_bundle_allowance -
                           report_and_preview_allowance - bytes.capacity();
    REQUIRE(admitted.working_bytes_upper_bound <= remaining);
    geo::ImportLimits limits;
    limits.max_working_bytes = admitted.working_bytes_upper_bound;
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm, 1.0, limits });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(inspected));
    const auto& report = std::get<std::shared_ptr<const geo::AssetDraft>>(inspected)->report();
    REQUIRE(report.validity == geo::Validity::valid);
    // Frozen pre-lifetime native report, from the T010 original Start baseline.
    CHECK(report.triangle_count == 139212);
    CHECK(report.vertex_count == 69602);
    CHECK(report.predicate_work == 1009948927);
    CHECK(report.candidate_pair_tests == 1606744);
    REQUIRE(report.volume_mm3);
    CHECK(*report.volume_mm3 == 8430.004424364888);
    REQUIRE(report.shells.size() == 2);
    CHECK(report.shells[0].depth == 0);
    CHECK(report.shells[0].triangle_count == 123372);
    CHECK(report.shells[1].depth == 1);
    CHECK(report.shells[1].parent_id == 0);
    CHECK(report.shells[1].final_orientation == geo::ShellOrientation::inward);
    CHECK(report.shells[1].triangle_count == 15840);
}

TEST_CASE("T011 ASCII admission handles malformed giant tokens without triangle undercount", "[import][T011]")
{
    std::string text = "solid name\nfacet normal 0 0 1\n";
    text.append(100000, 'x');
    text += "\nfacet\nendsolid name\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    const auto estimate = geo::estimate_import_admission(bytes);
    REQUIRE(std::holds_alternative<geo::ImportAdmission>(estimate));
    const auto admitted = std::get<geo::ImportAdmission>(estimate);
    CHECK(admitted.triangle_upper_bound == 2);
    CHECK(admitted.working_bytes_upper_bound >= bytes.size() * 5);
    geo::ImportLimits limits;
    limits.max_working_bytes = admitted.working_bytes_upper_bound;
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm, 1.0, limits });
    REQUIRE(std::holds_alternative<geo::ImportFailure>(inspected));
    CHECK(std::get<geo::ImportFailure>(inspected).code == "INVALID_STL");
}

TEST_CASE("T011 repair uses the caller remaining allowance before candidate allocation", "[import][T011]")
{
    const auto bytes = practical_bytes("ulamok_2kg_simplified.stl");
    const auto draft = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::AssetDraft>>(draft));
    geo::WeldOptions options { .01 };
    options.max_working_bytes = 0;
    const auto proposal = geo::propose_weld(std::get<std::shared_ptr<const geo::AssetDraft>>(draft), options);
    REQUIRE(std::holds_alternative<geo::ImportFailure>(proposal));
    CHECK(std::get<geo::ImportFailure>(proposal).code == "MEMORY_LIMIT");
    CHECK(std::get<geo::ImportFailure>(proposal).reason == "WELD_WORKING_BYTES");
    const auto source = std::get<std::shared_ptr<const geo::AssetDraft>>(draft);
    const auto estimate = geo::estimate_weld_admission(*source, options);
    REQUIRE(std::holds_alternative<geo::ImportAdmission>(estimate));
    options.max_working_bytes = std::get<geo::ImportAdmission>(estimate).working_bytes_upper_bound - 1;
    const auto too_small = geo::propose_weld(source, options);
    REQUIRE(std::holds_alternative<geo::ImportFailure>(too_small));
    options.max_working_bytes += 1;
    const auto accepted = geo::propose_weld(source, options);
    REQUIRE(std::holds_alternative<std::shared_ptr<const geo::RepairProposal>>(accepted));
    CHECK(std::get<std::shared_ptr<const geo::RepairProposal>>(accepted)->original() == source);
    CHECK(std::get<std::shared_ptr<const geo::RepairProposal>>(accepted)->candidate()->report().validity ==
          geo::Validity::valid);
}
