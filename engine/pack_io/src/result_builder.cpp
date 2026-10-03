#include <spectrapack/geometry/rigid_transform.hpp>
#include <spectrapack/io/result_export.hpp>

#include "operation_guard.hpp"
#include "result_builder_accounting.hpp"
#include "result_export_test_seam.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#endif
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <iomanip>
#include <new>
#include <set>
#include <sstream>

namespace spectrapack::io {
namespace {
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
std::atomic_bool result_build_fail_post_validation_allocation {};
#endif
Error bad(std::string c, std::string m) { return { std::move(c), std::move(m), Json::object(), false }; }
bool canonical(const geometry::Quaternion& q)
{
    double n = 0;
    for (double x : q) {
        if (!std::isfinite(x) || (x == 0.0 && std::signbit(x))) {
            return false;
        }
        n += x * x;
    }
    if (std::abs(std::sqrt(n) - 1) > 1e-12 || q[3] < 0) {
        return false;
    }
    if (q[3] == 0) {
        for (int i = 0; i < 3; ++i) {
            if (q[i] != 0) {
                return q[i] > 0;
            }
        }
    }
    return true;
}
Json jq(const geometry::Quaternion& q)
{
    Json r = Json::array();
    for (double x : q) {
        r.push_back(x == 0 ? 0.0 : x);
    }
    return r;
}
bool equal(const geometry::Quaternion& a, const geometry::Quaternion& b) { return a == b; }
bool same_bits(const geometry::Quaternion& left, const geometry::Quaternion& right)
{
    for (std::size_t index = 0; index != left.size(); ++index) {
        if (std::bit_cast<std::uint64_t>(left[index]) != std::bit_cast<std::uint64_t>(right[index])) {
            return false;
        }
    }
    return true;
}
geometry::Quaternion normalized(geometry::Quaternion q)
{
    const double n = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
    if (!std::isfinite(n) || n == 0.0) {
        return {};
    }
    for (double& value : q) {
        value /= n;
    }
    bool negate = q[3] < 0;
    if (q[3] == 0) {
        for (unsigned i = 0; i != 3; ++i) {
            if (q[i] != 0) {
                negate = q[i] < 0;
                break;
            }
        }
    }
    if (negate) {
        for (double& value : q) {
            value = -value;
        }
    }
    for (double& value : q) {
        if (value == 0) {
            value = 0;
        }
    }
    return q;
}
geometry::Quaternion signed_positive_zero(geometry::Quaternion q)
{
    bool negate = q[3] < 0;
    if (q[3] == 0) {
        for (unsigned i = 0; i != 3; ++i) {
            if (q[i] != 0) {
                negate = q[i] < 0;
                break;
            }
        }
    }
    if (negate) {
        for (double& value : q) {
            value = -value;
        }
    }
    for (double& value : q) {
        if (value == 0) {
            value = 0;
        }
    }
    return q;
}
std::vector<geometry::Quaternion> cube_catalog()
{
    using Matrix = std::array<int, 9>;
    std::array<Matrix, 24> matrices {};
    std::size_t count {};
    std::array<int, 3> permutation { 0, 1, 2 };
    const auto determinant = [](const Matrix& m) {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    };
    do {
        for (int sx : { -1, 1 }) {
            for (int sy : { -1, 1 }) {
                for (int sz : { -1, 1 }) {
                    Matrix m {};
                    m[permutation[0]] = sx;
                    m[3 + permutation[1]] = sy;
                    m[6 + permutation[2]] = sz;
                    if (determinant(m) == 1) {
                        matrices[count++] = m;
                    }
                }
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    std::sort(matrices.begin(), matrices.end());
    std::vector<geometry::Quaternion> out;
    out.reserve(24);
    for (const auto& m : matrices) {
        const int trace = m[0] + m[4] + m[8];
        std::array<int, 4> n {};
        if (trace > 0) {
            n = { m[7] - m[5], m[2] - m[6], m[3] - m[1], trace + 1 };
        }
        else if (m[0] > m[4] && m[0] > m[8]) {
            n = { 1 + m[0] - m[4] - m[8], m[1] + m[3], m[2] + m[6], m[7] - m[5] };
        }
        else if (m[4] > m[8]) {
            n = { m[1] + m[3], 1 + m[4] - m[0] - m[8], m[5] + m[7], m[2] - m[6] };
        }
        else {
            n = { m[2] + m[6], m[5] + m[7], 1 + m[8] - m[0] - m[4], m[3] - m[1] };
        }
        const auto nz = std::count_if(n.begin(), n.end(), [](int v) {
            return v != 0;
        });
        const double s = nz == 1 ? 1.0 : nz == 2 ? 0.7071067811865475244 : 0.5;
        geometry::Quaternion q {};
        for (unsigned i = 0; i < 4; ++i) {
            q[i] = n[i] < 0 ? -s : n[i] ? s : 0;
        }
        out.push_back(signed_positive_zero(q));
    }
    return out;
}
std::string mode(geometry::OrientationMode x)
{
    switch (x) {
    case geometry::OrientationMode::fixed:
        return "fixed";
    case geometry::OrientationMode::cube:
        return "cube";
    case geometry::OrientationMode::catalog:
        return "custom";
    case geometry::OrientationMode::upright:
        return "upright";
    case geometry::OrientationMode::free:
        return "free";
    }
    return {};
}
std::variant<std::string, Error> catalog_digest(const ResultCatalog& c)
{
    if (c.version != 1 || c.quaternions.empty()) {
        return bad("RESULT_CATALOG_INVALID", "Catalog must be nonempty version 1.");
    }
    nlohmann::ordered_json x;
    x["version"] = 1;
    x["quaternions_xyzw"] = nlohmann::ordered_json::array();
    std::set<geometry::Quaternion> seen;
    for (auto& q : c.quaternions) {
        if (!canonical(q) || !seen.insert(q).second) {
            return bad("RESULT_CATALOG_INVALID", "Catalog is not canonical and duplicate-free.");
        }
        nlohmann::ordered_json row = nlohmann::ordered_json::array();
        for (double v : q) {
            row.push_back(v == 0 ? 0.0 : v);
        }
        x["quaternions_xyzw"].push_back(row);
    }
#ifdef _WIN32
    std::string text = x.dump();
    BCRYPT_ALG_HANDLE a {};
    BCRYPT_HASH_HANDLE h {};
    DWORD n {}, got {}, d {};
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return bad("INTERNAL_ERROR", "SHA-256 provider unavailable.");
    }
    auto close = [&] {
        if (h) {
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(a, 0);
    };
    if (BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&n), sizeof(n), &got, 0) < 0 ||
        BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&d), sizeof(d), &got, 0) < 0) {
        close();
        return bad("INTERNAL_ERROR", "SHA-256 provider unavailable.");
    }
    std::vector<UCHAR> obj, digest;
    try {
        obj.resize(n);
        digest.resize(d);
    }
    catch (const std::bad_alloc&) {
        close();
        return bad("MEMORY_LIMIT", "Catalog hashing exhausted memory.");
    }
    if (BCryptCreateHash(a, &h, obj.data(), n, nullptr, 0, 0) < 0 ||
        BCryptHashData(h, reinterpret_cast<PUCHAR>(text.data()), static_cast<ULONG>(text.size()), 0) < 0 ||
        BCryptFinishHash(h, digest.data(), d, 0) < 0) {
        close();
        return bad("INTERNAL_ERROR", "SHA-256 provider unavailable.");
    }
    close();
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : digest) {
        out << std::setw(2) << unsigned(b);
    }
    return out.str();
#else
    return bad("INTERNAL_ERROR", "SHA-256 unavailable.");
#endif
}
bool matches(const geometry::OrientationPolicy& p, const ResultCatalog& c)
{
    if (p.mode == geometry::OrientationMode::fixed) {
        geometry::Quaternion q = p.catalog_xyzw.empty() ? geometry::Quaternion { 0, 0, 0, 1 } : p.catalog_xyzw[0];
        return p.catalog_xyzw.size() <= 1 && c.quaternions.size() == 1 && equal(normalized(q), c.quaternions[0]);
    }
    if (p.mode == geometry::OrientationMode::catalog) {
        std::vector<geometry::Quaternion> expected;
        for (const auto& q : p.catalog_xyzw) {
            const auto stable = normalized(q);
            if (std::find(expected.begin(), expected.end(), stable) == expected.end()) {
                expected.push_back(stable);
            }
        }
        return expected == c.quaternions;
    }
    return p.mode == geometry::OrientationMode::cube && cube_catalog() == c.quaternions;
}
bool matches_resolved(const geometry::OrientationPolicy& p, const ResultCatalog& c)
{
    if (p.mode == geometry::OrientationMode::fixed) {
        const geometry::Quaternion identity { 0, 0, 0, 1 };
        const auto& expected = p.catalog_xyzw.empty() ? identity : p.catalog_xyzw.front();
        return p.catalog_xyzw.size() <= 1 && c.quaternions.size() == 1 && same_bits(c.quaternions.front(), expected);
    }
    if (p.mode == geometry::OrientationMode::catalog) {
        return p.catalog_xyzw.size() == c.quaternions.size() &&
               std::equal(p.catalog_xyzw.begin(), p.catalog_xyzw.end(), c.quaternions.begin(), same_bits);
    }
    return p.mode == geometry::OrientationMode::cube && cube_catalog() == c.quaternions;
}
bool exact_quaternion_json(const Json& value, const geometry::Quaternion& expected)
{
    if (!value.is_array() || value.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index != expected.size(); ++index) {
        if (!value[index].is_number() ||
            std::bit_cast<std::uint64_t>(value[index].get<double>()) != std::bit_cast<std::uint64_t>(expected[index])) {
            return false;
        }
    }
    return true;
}
bool exact_resolved_orientation(const Json& value, const geometry::OrientationPolicy& policy,
                                const ResultCatalog& catalog)
{
    if (!value.is_object() || !value.contains("mode") || value.at("mode") != mode(policy.mode)) {
        return false;
    }
    if (policy.mode == geometry::OrientationMode::fixed) {
        return value.contains("quaternion_xyzw") && catalog.quaternions.size() == 1 &&
               exact_quaternion_json(value.at("quaternion_xyzw"), catalog.quaternions.front());
    }
    if (policy.mode == geometry::OrientationMode::catalog) {
        if (!value.contains("quaternions_xyzw") || !value.at("quaternions_xyzw").is_array() ||
            value.at("quaternions_xyzw").size() != catalog.quaternions.size()) {
            return false;
        }
        for (std::size_t index = 0; index != catalog.quaternions.size(); ++index) {
            if (!exact_quaternion_json(value.at("quaternions_xyzw").at(index), catalog.quaternions[index])) {
                return false;
            }
        }
    }
    return policy.mode == geometry::OrientationMode::catalog || policy.mode == geometry::OrientationMode::cube;
}
bool exact_resolved_metadata(const Json& metadata, const geometry::OrientationPolicy& policy,
                             const ResultCatalog& catalog)
{
    if (!metadata.contains("search") || !metadata.at("search").is_object()) {
        return false;
    }
    const auto& search = metadata.at("search");
    if (!search.contains("resolved_settings") || !search.contains("run_segments") ||
        !search.at("run_segments").is_array() ||
        !exact_resolved_orientation(search.at("resolved_settings").at("orientation"), policy, catalog)) {
        return false;
    }
    for (const auto& segment : search.at("run_segments")) {
        if (!segment.is_object() || !segment.contains("resolved_settings") ||
            !exact_resolved_orientation(segment.at("resolved_settings").at("orientation"), policy, catalog)) {
            return false;
        }
    }
    return true;
}
Json portable(const VerifiedAsset& a)
{
    Json r = a.record();
    auto s = r["source"]["sha256"].get<std::string>();
    auto p = r["accepted_solid"]["sha256"].get<std::string>();
    r["source"]["path"] = "assets/" + s + ".stl";
    r["accepted_solid"]["path"] = "assets/" + p + ".ply";
    return r;
}
Json jm(const geometry::Matrix4& m)
{
    Json a = Json::array();
    for (auto& r : m) {
        Json b = Json::array();
        for (double x : r) {
            b.push_back(x == 0 ? 0.0 : x);
        }
        a.push_back(b);
    }
    return a;
}
}  // namespace
namespace test {
void fail_result_build_post_validation_allocation_for_test(bool enabled) noexcept
{
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
    result_build_fail_post_validation_allocation.store(enabled);
#else
    (void)enabled;
#endif
}
}  // namespace test
std::variant<std::string, Error> result_catalog_sha256(const ResultCatalog& catalog,
                                                       const geometry::OrientationPolicy& policy)
{
    if (catalog.binding == ResultCatalogBinding::normalized_policy) {
        if (!matches(policy, catalog)) {
            return bad("RESULT_CATALOG_INVALID", "Catalog does not match context policy.");
        }
    }
    else if (catalog.binding == ResultCatalogBinding::resolved_policy) {
        if (!matches_resolved(policy, catalog)) {
            return bad("RESULT_CATALOG_INVALID", "Catalog does not match resolved context policy.");
        }
    }
    else {
        return bad("RESULT_CATALOG_INVALID", "Catalog binding is unsupported.");
    }
    return catalog_digest(catalog);
}
detail::ResultBuildAttempt detail::build_result_with_report(const ResultRequest& r,
                                                            const runtime::OperationControl& control)
{
    const OperationGuard operation(control);
    geometry::ValidationReport validation_report;
    bool validation_attempted {};
    const auto finish = [&validation_report, &validation_attempted](ResultOutcome result) {
        return ResultBuildAttempt { std::move(result), std::move(validation_report), validation_attempted };
    };
    try {
        poll_operation();
        if (!r.solution || !r.object_asset) {
            return finish(bad("RESULT_INPUT_INVALID", "Solution and verified object asset are required."));
        }
        auto ctx = r.solution->context();
        if (!ctx || ctx->object().get() != r.object_asset->solid().get()) {
            return finish(bad("RESULT_ASSET_MISMATCH", "Object must be the exact context asset."));
        }
        bool box = std::holds_alternative<geometry::BoxDimensions>(ctx->container());
        if (box ? bool(r.container_asset) : !r.container_asset) {
            return finish(bad("RESULT_ASSET_MISMATCH", "Container asset presence does not match context."));
        }
        if (!box && std::get<std::shared_ptr<const geometry::AcceptedSolid>>(ctx->container()).get() !=
                        r.container_asset->solid().get()) {
            return finish(bad("RESULT_ASSET_MISMATCH", "Container must be the exact context asset."));
        }
        const std::set<std::string> keys = { "schema_version", "job_id", "solution_revision", "created_at", "engine",
                                             "search",         "metrics" };
        if (!r.metadata.is_object() || r.metadata.size() != keys.size()) {
            return finish(bad("RESULT_METADATA_INVALID", "Metadata has unsupported fields."));
        }
        for (auto& k : keys) {
            if (!r.metadata.contains(k)) {
                return finish(bad("RESULT_METADATA_INVALID", "Metadata is incomplete."));
            }
        }
        const std::set<std::string> measures = { "time_to_best_seconds", "peak_host_bytes", "peak_device_bytes",
                                                 "termination_reason" };
        if (!r.metadata["metrics"].is_object() || r.metadata["metrics"].size() != measures.size()) {
            return finish(bad("RESULT_METADATA_INVALID", "Metrics may contain only measured values."));
        }
        for (auto& k : measures) {
            if (!r.metadata["metrics"].contains(k)) {
                return finish(bad("RESULT_METADATA_INVALID", "Measured metrics are incomplete."));
            }
        }
        auto hash = result_catalog_sha256(r.catalog, ctx->constraints().orientations);
        if (auto* e = std::get_if<Error>(&hash)) {
            return finish(*e);
        }
        if (r.catalog.binding == ResultCatalogBinding::resolved_policy &&
            !exact_resolved_metadata(r.metadata, ctx->constraints().orientations, r.catalog)) {
            return finish(bad("RESULT_CATALOG_INVALID",
                              "Resolved settings do not preserve the catalog's exact representatives."));
        }
        for (auto& p : r.solution->copies()) {
            bool found = false;
            for (auto& q : r.catalog.quaternions) {
                found =
                    found || (r.catalog.binding == ResultCatalogBinding::resolved_policy ? same_bits(p.rotation_xyzw, q)
                                                                                         : equal(p.rotation_xyzw, q));
            }
            if (!found) {
                return finish(bad("RESULT_CATALOG_INVALID", "Pose is not in catalog."));
            }
        }
        validation_attempted = true;
        control.phase(runtime::Phase::validating);
        auto fresh = geometry::revalidate(r.solution, r.validation_limits, control);
        poll_operation();
        validation_report = fresh.report;
        if (!fresh.validated_solution || validation_report.validity != geometry::Validity::valid) {
            auto error = bad("RESULT_VALIDATION_FAILED", "Fresh native validation did not certify solution.");
            error.details = {
                { "validation_code",        validation_report.code               },
                { "validation_kernel_work", validation_report.kernel_work        },
                { "validation_peak_bytes",  validation_report.working_bytes_peak },
                { "validation_pair_tests",  validation_report.aabb_pair_tests    }
            };
            return finish(std::move(error));
        }
#ifdef SPECTRAPACK_ASSET_LOADER_TESTING
        if (result_build_fail_post_validation_allocation.exchange(false)) {
            throw std::bad_alloc {};
        }
#endif
        Json object = portable(*r.object_asset);
        Json result = {
            { "schema_version",    r.metadata["schema_version"]    },
            { "label",             "best_found"                    },
            { "job_id",            r.metadata["job_id"]            },
            { "solution_revision", r.metadata["solution_revision"] },
            { "created_at",        r.metadata["created_at"]        },
            { "engine",            r.metadata["engine"]            },
            { "assets",            { { "object", object } }        }
        };
        if (box) {
            auto d = std::get<geometry::BoxDimensions>(ctx->container());
            result["container"] = {
                { "kind",          "box"                                   },
                { "dimensions_mm", { d.width_mm, d.depth_mm, d.height_mm } }
            };
        }
        else {
            Json c = portable(*r.container_asset);
            result["assets"]["container"] = c;
            result["container"] = {
                { "kind",            "stl_volume"                                                              },
                { "asset",
                 { { "source_sha256", c["source"]["sha256"] },
                    { "accepted_solid_sha256", c["accepted_solid"]["sha256"] } }                               },
                { "source_to_world", jm(geometry::source_to_local_matrix(r.container_asset->solid()->frame())) },
                { "semantics",       "interior_volume"                                                         }
            };
        }
        Json orientation = {
            { "mode", mode(ctx->constraints().orientations.mode) }
        };
        if (ctx->constraints().orientations.mode == geometry::OrientationMode::fixed) {
            orientation["quaternion_xyzw"] = jq(r.catalog.quaternions[0]);
        }
        if (ctx->constraints().orientations.mode == geometry::OrientationMode::catalog) {
            orientation["quaternions_xyzw"] = Json::array();
            for (const auto& q : r.catalog.quaternions) {
                orientation["quaternions_xyzw"].push_back(jq(q));
            }
        }
        result["constraints"] = {
            { "clearance_mm",
             { { "pair", ctx->constraints().pair_clearance_mm }, { "wall", ctx->constraints().wall_clearance_mm } } },
            { "orientation",                orientation                                                             },
            { "orientation_catalog_sha256", std::get<std::string>(hash)                                             }
        };
        result["search"] = r.metadata["search"];
        Json placements = Json::array();
        for (auto& p : r.solution->copies()) {
            auto t = geometry::RigidTransform::make(p.rotation_xyzw, p.translation_mm);
            if (!t) {
                return finish(bad("RESULT_VALIDATION_FAILED", "Pose transform invalid."));
            }
            placements.push_back({
                { "copy_id",         p.copy_id               },
                { "translation_mm",  p.translation_mm        },
                { "quaternion_xyzw", jq(p.rotation_xyzw)     },
                { "local_to_world",  jm(t->local_to_world()) }
            });
        }
        result["count"] = placements.size();
        result["placements"] = placements;
        Json ah = Json::array();
        ah.push_back(object["accepted_solid"]["sha256"]);
        if (!box) {
            ah.push_back(result["assets"]["container"]["accepted_solid"]["sha256"]);
        }
        result["validation"] = {
            { "status",                        "valid"                                                                       },
            { "tolerance_mm",                  fresh.report.epsilon_mm                                                       },
            { "authoritative_geometry_sha256", ah                                                                            },
            { "validator_version",             "native-validation"                                                           },
            { "kernel_version",                fresh.report.kernel_revision                                                  },
            { "checks",                        { { "pair", "valid" }, { "containment", "valid" }, { "clearance", "valid" } } }
        };
        Json metrics = r.metadata["metrics"];
        auto solid = r.object_asset->solid()->report().volume_mm3;
        std::optional<double> cv;
        if (box) {
            auto d = std::get<geometry::BoxDimensions>(ctx->container());
            double v = d.width_mm * d.depth_mm * d.height_mm;
            if (std::isfinite(v) && v > 0) {
                cv = v;
            }
        }
        else {
            cv = r.container_asset->solid()->report().volume_mm3;
        }
        if (solid && cv && std::isfinite(*solid) && std::isfinite(*cv) && *solid > 0 && *cv > 0) {
            metrics["solid_volume_mm3"] = *solid;
            metrics["container_volume_mm3"] = *cv;
            metrics["utilization"] = result["count"].get<double>() * *solid / *cv;
        }
        else {
            metrics["solid_volume_mm3"] = nullptr;
            metrics["container_volume_mm3"] = nullptr;
            metrics["utilization"] = nullptr;
        }
        result["metrics"] = metrics;
        ContractValidator v(ContractKind::results);
        auto checked = v.validate(ContractKind::results, result, r.diagnostic_limits);
        if (auto* x = std::get_if<ValidatedDocument>(&checked)) {
            return finish(std::move(*x));
        }
        return finish(contract_error(std::get<ContractFailure>(checked)));
    }
    catch (const Interrupted& interruption) {
        return finish(interrupted_error(interruption));
    }
    catch (const std::bad_alloc&) {
        return finish(bad("MEMORY_LIMIT", "Result construction exhausted memory."));
    }
    catch (const std::exception&) {
        return finish(bad("RESULT_BUILD_FAILED", "Result construction failed."));
    }
}

ResultOutcome build_result(const ResultRequest& request, const runtime::OperationControl& control)
{
    return detail::build_result_with_report(request, control).result;
}
}  // namespace spectrapack::io
