#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <spectrapack/io/contracts.hpp>
#include <spectrapack/io/result_export.hpp>
#include <string>
#include <variant>

int quantization_fixture(const std::filesystem::path& report, const std::filesystem::path& original,
                         const std::filesystem::path& output)
{
    namespace io = spectrapack::io;
    namespace geo = spectrapack::geometry;
    const auto loaded = io::load_accepted_asset(report);
    if (!std::holds_alternative<std::shared_ptr<const io::VerifiedAsset>>(loaded)) {
        return 1;
    }
    const auto asset = std::get<std::shared_ptr<const io::VerifiedAsset>>(loaded);
    const auto gap = std::ldexp(1.0, -25);
    geo::Constraints constraints;
    constraints.pair_clearance_mm = gap;
    const auto made = geo::make_validation_context(asset->solid(), geo::BoxDimensions { 35, 35, 35 }, constraints);
    if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made)) {
        return 1;
    }
    const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
    const auto candidate = geo::make_candidate(
        context, {
                     { "left",  { 10, 10, 10 },       { 0, 0, 0, 1 } },
                     { "right", { 20 + gap, 10, 10 }, { 0, 0, 0, 1 } }
    });
    if (!std::holds_alternative<std::shared_ptr<const geo::Candidate>>(candidate)) {
        return 1;
    }
    const auto fresh = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(candidate));
    if (!fresh.validated_solution) {
        return 1;
    }
    const auto stored = io::Json::parse(std::ifstream(original));
    io::Json metadata;
    for (const auto* key : { "schema_version", "job_id", "solution_revision", "created_at", "engine", "search" }) {
        metadata[key] = stored.at(key);
    }
    for (const auto* key : { "time_to_best_seconds", "peak_host_bytes", "peak_device_bytes", "termination_reason" }) {
        metadata["metrics"][key] = stored.at("metrics").at(key);
    }
    auto& settings = metadata["search"]["resolved_settings"];
    settings["container"]["dimensions_mm"] = { 35, 35, 35 };
    settings["clearance_mm"]["pair"] = gap;
    for (auto& segment : metadata["search"]["run_segments"]) {
        segment["resolved_settings"] = settings;
    }
    const auto built = io::build_result({
        fresh.validated_solution,
        asset,
        {},
        { 1, { { 0, 0, 0, 1 } }, io::ResultCatalogBinding::resolved_policy },
        metadata
    });
    if (!std::holds_alternative<io::ValidatedDocument>(built)) {
        return 1;
    }
    std::ofstream stream(output, std::ios::binary);
    stream << std::get<io::ValidatedDocument>(built).value().dump();
    stream.close();
    return stream ? 0 : 1;
}

template <class Char>
int run(int argc, Char** argv)
{
    if (argc == 5 && std::filesystem::path(argv[1]) == "--quantization-fixture") {
        return quantization_fixture(argv[2], argv[3], argv[4]);
    }
    if (argc != 1) {
        return 2;
    }
    const std::string input { std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>() };

    spectrapack::io::ContractValidator validator;
    const auto outcome = validator.parse(spectrapack::io::ContractKind::results, input);
    if (std::holds_alternative<spectrapack::io::ValidatedDocument>(outcome)) {
        return 0;
    }

    const auto& failure = std::get<spectrapack::io::ContractFailure>(outcome);
    std::cerr << "results contract validation failed with " << failure.issues.size() << " issue(s)\n";
    for (const auto& issue : failure.issues) {
        std::cerr << issue.path << ": " << issue.code << ": " << issue.message << '\n';
    }
    return 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
