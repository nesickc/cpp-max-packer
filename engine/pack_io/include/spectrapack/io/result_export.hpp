#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <spectrapack/runtime/operation_control.hpp>
#include <variant>
#include <vector>

#include "spectrapack/geometry/import.hpp"
#include "spectrapack/geometry/validation.hpp"
#include "spectrapack/io/contracts.hpp"

namespace spectrapack::io {

struct ExportRequest;
struct ExportSuccess;
using ExportOutcome = std::variant<ExportSuccess, Error>;

class VerifiedAsset;
struct AssetLoadLimits {
    std::uint64_t max_working_bytes { 512ULL << 20 };
    ContractDiagnosticLimits diagnostic_limits {};
};
using AssetLoadOutcome = std::variant<std::shared_ptr<const VerifiedAsset>, Error>;

class VerifiedAsset {
public:
    [[nodiscard]] const std::shared_ptr<const geometry::AcceptedSolid>& solid() const noexcept;
    [[nodiscard]] const Json& record() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> resident_buffer_bytes() const noexcept;

private:
    friend AssetLoadOutcome load_accepted_asset(const std::filesystem::path&, const runtime::OperationControl&,
                                                const AssetLoadLimits&, std::shared_ptr<const VerifiedAsset>);
    friend ExportOutcome export_result(const ExportRequest&, const runtime::OperationControl&);
    struct Storage;
    std::shared_ptr<const Storage> storage_;
    explicit VerifiedAsset(std::shared_ptr<const Storage>) noexcept;
};
[[nodiscard]] AssetLoadOutcome load_accepted_asset(const std::filesystem::path& report_path,
                                                   const runtime::OperationControl& control = {},
                                                   const AssetLoadLimits& limits = {},
                                                   std::shared_ptr<const VerifiedAsset> reuse_candidate = {});

enum class ResultCatalogBinding { normalized_policy, resolved_policy };
struct ResultCatalog {
    std::uint64_t version { 1 };
    std::vector<geometry::Quaternion> quaternions;
    ResultCatalogBinding binding { ResultCatalogBinding::normalized_policy };
};
[[nodiscard]] std::variant<std::string, Error> result_catalog_sha256(const ResultCatalog&,
                                                                     const geometry::OrientationPolicy&);
struct ResultRequest {
    std::shared_ptr<const geometry::ValidatedSolution> solution;
    std::shared_ptr<const VerifiedAsset> object_asset, container_asset;
    ResultCatalog catalog;
    Json metadata;
    geometry::ValidationLimits validation_limits {};
    ContractDiagnosticLimits diagnostic_limits {};
};
using ResultOutcome = std::variant<ValidatedDocument, Error>;
[[nodiscard]] ResultOutcome build_result(const ResultRequest&, const runtime::OperationControl& control = {});

struct ExportRequest {
    std::shared_ptr<const geometry::ValidatedSolution> solution;
    std::shared_ptr<const VerifiedAsset> object_asset, container_asset;
    ResultCatalog catalog;
    ValidatedDocument result;
    std::filesystem::path result_path;
    std::optional<std::filesystem::path> stl_path;
    geometry::ValidationLimits validation_limits {};
    geometry::ImportLimits per_copy_import_limits {};
    std::uint64_t max_working_bytes { 512ULL << 20 };
    std::uint64_t max_output_bytes { 8ULL << 30 };
    // Only versioned timing metadata may be refreshed after checked validation
    // and asset staging, immediately before the primary immutable result commit.
    Json (*runtime_before_commit)(void*) {};
    void* runtime_context {};
    ContractDiagnosticLimits diagnostic_limits {};
};
struct ExportSuccess {
    std::filesystem::path result_path;
    std::optional<std::filesystem::path> stl_path, companion_path;
};
[[nodiscard]] ExportOutcome export_result(const ExportRequest& request, const runtime::OperationControl& control = {});

}  // namespace spectrapack::io
