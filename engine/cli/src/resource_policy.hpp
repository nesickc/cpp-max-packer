#pragma once

#include <charconv>
#include <limits>
#include <optional>
#include <spectrapack/solver/spectral.hpp>
#include <type_traits>

#include "host_admission.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace spectrapack::cli {
inline std::optional<std::uint64_t> sample_available_ram(void* = nullptr) noexcept
{
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return {};
    }
    return status.ullAvailPhys;
}

inline std::optional<std::uint64_t> resolve_host_cap(std::optional<std::uint64_t> available,
                                                     std::optional<std::uint64_t> user = {}) noexcept
{
    if (!available || *available < 2 || (user && *user == 0)) {
        return {};
    }
    return user ? std::min(*available / 2, *user) : *available / 2;
}

inline std::optional<std::uint64_t> decimal_bytes(const io::Json& value) noexcept
{
    if (!value.is_string()) {
        return {};
    }
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result {};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc {} && end == text.data() + text.size() ? std::optional(result) : std::nullopt;
}

template <class F>
void visit_resource_limits(solver::SpectralLimits& value, F&& visit)
{
    visit("max_working_bytes", value.max_working_bytes);
    visit("max_representation_kernel_work", value.max_representation_kernel_work);
    visit("max_representation_cell_visits", value.max_representation_cell_visits);
    visit("max_direct_terms", value.max_direct_terms);
    visit("max_proximity_terms", value.max_proximity_terms);
    visit("max_refinement_evaluations", value.max_refinement_evaluations);
    for (const auto baseline : { true, false }) {
        auto& part = baseline ? value.baseline : value.spectral;
        const std::string prefix = baseline ? "baseline_" : "spectral_";
#define VISIT_LIMIT(name) visit(prefix + #name, part.name)
        VISIT_LIMIT(max_candidate_evaluations);
        VISIT_LIMIT(max_search_passes);
        VISIT_LIMIT(max_orientations);
        VISIT_LIMIT(max_copies);
        VISIT_LIMIT(max_axis_cells);
        VISIT_LIMIT(max_working_bytes);
        VISIT_LIMIT(max_geometry_kernel_work);
        VISIT_LIMIT(max_geometry_vertex_visits);
        VISIT_LIMIT(max_validation_kernel_work);
        VISIT_LIMIT(max_validation_aabb_pair_tests);
#undef VISIT_LIMIT
#define VISIT_NESTED(group, name) visit(prefix + #group "_" #name, part.group.name)
        VISIT_NESTED(per_query, max_working_bytes);
        VISIT_NESTED(per_query, max_kernel_work);
        VISIT_NESTED(per_query, max_vertex_visits);
        VISIT_NESTED(per_validation, max_copy_count);
        VISIT_NESTED(per_validation, max_working_bytes);
        VISIT_NESTED(per_validation, max_aabb_pair_tests);
        VISIT_NESTED(per_validation, max_kernel_work);
        VISIT_NESTED(per_validation, max_diagnostic_examples);
#undef VISIT_NESTED
    }
#define VISIT_PART(group, name) visit(#group "_" #name, value.group.name)
    VISIT_PART(per_representation, max_working_bytes);
    VISIT_PART(per_representation, max_cells);
    VISIT_PART(per_representation, max_kernel_work);
    VISIT_PART(per_representation, max_cell_visits);
    VISIT_PART(per_representation, max_input_triangles);
    VISIT_PART(per_correlation, max_working_bytes);
    VISIT_PART(per_correlation, max_padded_cells);
    VISIT_PART(per_correlation, max_direct_terms);
#undef VISIT_PART
}

inline io::Json resolve_resource_policy(std::uint64_t available, std::uint64_t cap, bool fixed,
                                        solver::SpectralLimits limits = {})
{
    io::Json record = io::Json::object();
    visit_resource_limits(limits, [&](const std::string& name, auto& value) {
        if (name.ends_with("max_working_bytes")) {
            value = cap;
        }
        record[name] = std::to_string(value);
    });
    return {
        { "version",             1                                                            },
        { "available_ram_bytes", std::to_string(available)                                    },
        { "host_cap_bytes",      std::to_string(cap)                                          },
        { "work_mode",           fixed ? "fixed_work" : "timed"                               },
        { "search_order",        fixed ? "catalog-merge-v1" : "completed-page-round-robin-v1" },
        { "limits",              std::move(record)                                            }
    };
}

inline bool apply_resource_policy(const io::Json& record, solver::SpectralLimits& limits)
{
    if (record.at("version") != 1) {
        return false;
    }
    const auto cap = decimal_bytes(record.at("host_cap_bytes"));
    bool valid = cap && *cap > 0;
    visit_resource_limits(limits, [&](const std::string& name, auto& value) {
        const auto member = record.at("limits").find(name);
        const auto parsed = member == record.at("limits").end() ? std::nullopt : decimal_bytes(*member);
        if (!parsed || *parsed > std::numeric_limits<std::remove_reference_t<decltype(value)>>::max() ||
            (name.ends_with("max_working_bytes") && parsed != cap)) {
            valid = false;
        }
        else {
            value = static_cast<std::remove_reference_t<decltype(value)>>(*parsed);
        }
    });
    limits.resource_mode =
        record.at("work_mode") == "timed" ? solver::ResourceMode::timed : solver::ResourceMode::fixed_work;
    limits.available_ram = limits.resource_mode == solver::ResourceMode::timed ? sample_available_ram : nullptr;
    return valid;
}
}  // namespace spectrapack::cli
