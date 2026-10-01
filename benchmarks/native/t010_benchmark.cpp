#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "../../engine/pack_geometry/src/field_kernel.hpp"
#include "../../engine/pack_geometry/src/field_profile.hpp"
#include "../../engine/pack_geometry/tests/validation_fixtures.hpp"
#include "../../engine/pack_solver/src/pipeline_profile.hpp"
#include "baseline_support.hpp"
#include "spectrapack/solver/spectral.hpp"
#ifdef _WIN32
#define NOMINMAX
// Windows declarations must precede the process-memory API header.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

namespace geo = spectrapack::geometry;
namespace sol = spectrapack::solver;
namespace bench = spectrapack::benchmark;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Profile {
    sol::detail::PipelineProfileSample pipeline;
    std::array<double, 3> field_ms {};
    std::array<std::uint64_t, 3> field_work {}, field_visits {}, field_calls {};
    std::uint64_t pages {};
};
void pipeline_sample(void* context, const sol::detail::PipelineProfileSample& sample) noexcept
{
    auto& profile = *static_cast<Profile*>(context);
    ++profile.pages;
    for (std::size_t index = 0; index != sample.elapsed_ms.size(); ++index) {
        profile.pipeline.elapsed_ms[index] += sample.elapsed_ms[index];
    }
}
void field_sample(void* context, const geo::detail::FieldProfileSample& sample) noexcept
{
    auto& profile = *static_cast<Profile*>(context);
    const auto index = static_cast<std::size_t>(sample.phase);
    profile.field_ms[index] += sample.elapsed_ms;
    profile.field_work[index] += sample.kernel_work;
    profile.field_visits[index] += sample.cell_visits;
    ++profile.field_calls[index];
}
bench::Json profile_json(const Profile& profile)
{
    constexpr std::array pipeline_names { "admission", "prepare",   "container",  "placed",        "object",
                                          "occupancy", "proximity", "binary_fft", "proximity_fft", "ranking" };
    constexpr std::array field_names { "raster", "material_fill", "clearance" };
    bench::Json pipeline, fields;
    for (std::size_t index = 0; index != pipeline_names.size(); ++index) {
        pipeline[pipeline_names[index]] = profile.pipeline.elapsed_ms[index];
    }
    for (std::size_t index = 0; index != field_names.size(); ++index) {
        fields[field_names[index]] = {
            { "elapsed_ms",  profile.field_ms[index]     },
            { "kernel_work", profile.field_work[index]   },
            { "cell_visits", profile.field_visits[index] },
            { "calls",       profile.field_calls[index]  }
        };
    }
    return {
        { "pipeline_ms",         pipeline      },
        { "nested_field_phases", fields        },
        { "pipeline_calls",      profile.pages }
    };
}
std::shared_ptr<const geo::AcceptedSolid> load(std::string_view path)
{
    std::ifstream file(std::string(SPECTRAPACK_TEST_ROOT) + "/" + std::string(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("source open failed");
    }
    file.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("source read failed");
    }
    const auto inspected = geo::inspect_stl(bytes, { geo::AssetRole::object, geo::Units::mm });
    if (const auto* failure = std::get_if<geo::ImportFailure>(&inspected)) {
        throw std::runtime_error(failure->code);
    }
    const auto accepted = geo::accept_asset(std::get<std::shared_ptr<const geo::AssetDraft>>(inspected));
    if (const auto* failure = std::get_if<geo::ImportFailure>(&accepted)) {
        throw std::runtime_error(failure->code);
    }
    return std::get<std::shared_ptr<const geo::AcceptedSolid>>(accepted);
}
bench::Json counters(const sol::SpectralOutcome& run)
{
    return {
        { "candidate_evaluations",      run.run.stats.candidate_evaluations           },
        { "search_passes",              run.run.stats.search_passes                   },
        { "correlations",               run.spectral_stats.correlations               },
        { "representation_kernel_work", run.spectral_stats.representation_kernel_work },
        { "representation_cell_visits", run.spectral_stats.representation_cell_visits },
        { "direct_terms",               run.spectral_stats.direct_terms               },
        { "proximity_terms",            run.spectral_stats.proximity_terms            },
        { "validation_kernel_work",     run.run.stats.validation_kernel_work          },
        { "validation_aabb_pair_tests", run.run.stats.validation_aabb_pair_tests      },
        { "tracked_working_bytes_peak", run.run.stats.tracked_working_bytes_peak      }
    };
}
}  // namespace

int main(int argc, char** argv)
{
    try {
        std::string profile_name = "analytic";
        int samples = 5, warmups = 1;
        std::string scope = "field";
        for (int index = 1; index < argc; index += 2) {
            if (index + 1 == argc) {
                throw std::runtime_error("option requires a value");
            }
            const std::string_view option = argv[index];
            if (option == "--profile") {
                profile_name = argv[index + 1];
            }
            else if (option == "--samples") {
                samples = std::stoi(argv[index + 1]);
            }
            else if (option == "--warmups") {
                warmups = std::stoi(argv[index + 1]);
            }
            else if (option == "--scope") {
                scope = argv[index + 1];
            }
            else {
                throw std::runtime_error("unknown option");
            }
        }
        if (samples < 1 || samples > 100 || warmups < 0 || warmups > 10) {
            throw std::runtime_error("invalid sample count");
        }
        if (scope != "field" && scope != "prepared-start") {
            throw std::runtime_error("scope must be field or prepared-start");
        }
        if (profile_name != "analytic" && profile_name != "ulamok" && profile_name != "pryanik1" &&
            profile_name != "pryanik2") {
            throw std::runtime_error("profile must be analytic, ulamok, pryanik1 or pryanik2");
        }
        const bool analytic = profile_name == "analytic", ulamok = profile_name == "ulamok";
        const std::string source = analytic                     ? "analytic-cuboid"
                                   : ulamok                     ? "rc/items/ulamok_2kg_simplified.stl"
                                   : profile_name == "pryanik1" ? "rc/items/pryanik_1.STL"
                                                                : "rc/items/pryanik_2.STL";
        const auto preparation_start = Clock::now();
        const auto object =
            analytic ? geo::test_support::accepted(geo::test_support::cuboid({ -1, -1.5, -.5 }, { 1, 1.5, .5 }),
                                                   geo::AssetRole::object)
                     : load(source);
        const double preparation_ms = elapsed(preparation_start);
        geo::Constraints constraints;
        constraints.pair_clearance_mm = analytic ? .125 : .1;
        constraints.wall_clearance_mm = analytic ? .25 : 1;
        constraints.orientations.mode = ulamok ? geo::OrientationMode::cube : geo::OrientationMode::fixed;
        const geo::BoxDimensions box = analytic ? geo::BoxDimensions { 38, 28, 20 }
                                       : ulamok ? geo::BoxDimensions { 400, 350, 285 }
                                                : geo::BoxDimensions { 100, 100, 50 };
        const auto made = geo::make_validation_context(object, box, constraints);
        if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made)) {
            throw std::runtime_error("context failed");
        }
        const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
        sol::BaselineLimits baseline_limits;
        if (analytic) {
            baseline_limits.max_candidate_evaluations = 2;
        }
        const auto baseline_start = Clock::now();
        const auto baseline = sol::run_aabb_baseline(context, baseline_limits, {});
        const double baseline_ms = elapsed(baseline_start);
        if (!baseline.best) {
            throw std::runtime_error("baseline failed");
        }
        const geo::GridLattice lattice {
            { 0, 0, 0 },
            analytic ? 1.
            : ulamok ? 16.
                     : 4.
        };
        sol::SpectralLimits limits;
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        if (scope == "prepared-start") {
            limits.baseline = baseline_limits;
        }
        limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
        limits.max_refinement_evaluations = 0;
        bool passed = true;
        bench::Json records = bench::Json::array();
        for (int ordinal = -warmups; ordinal != samples; ++ordinal) {
            Profile profile;
            sol::detail::pipeline_profile_sink = &pipeline_sample;
            sol::detail::pipeline_profile_context = &profile;
            geo::detail::field_profile_sink = &field_sample;
            geo::detail::field_profile_context = &profile;
            const auto start = Clock::now();
            const auto run = sol::run_cpu_spectral(
                context, lattice, limits, {}, {},
                scope == "field" ? baseline.best->solution : std::shared_ptr<const geo::ValidatedSolution> {});
            const double solver_ms = elapsed(start);
            sol::detail::pipeline_profile_sink = nullptr;
            geo::detail::field_profile_sink = nullptr;
            if (!run.run.best) {
                throw std::runtime_error("retained snapshot missing");
            }
            const auto validation_start = Clock::now();
            const auto checked = geo::revalidate(run.run.best->solution);
            const double revalidation_ms = elapsed(validation_start);
            const double fixed_ms = scope == "prepared-start" ? elapsed(start) : solver_ms;
            std::uint64_t process_peak {};
#ifdef _WIN32
            PROCESS_MEMORY_COUNTERS process_counters {};
            process_counters.cb = sizeof(process_counters);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &process_counters, sizeof(process_counters))) {
                process_peak = process_counters.PeakWorkingSetSize;
            }
#endif
            const bool complete = (run.run.termination_reason == sol::TerminationReason::budget_exhausted ||
                                   run.run.termination_reason == sol::TerminationReason::search_stalled) &&
                                  run.run.diagnostic_code != "PHYSICAL_DEADLINE" &&
                                  run.spectral_stats.correlations >= (ulamok ? 48 : 2) && checked.validated_solution;
            passed &= complete;
            records.push_back({
                { "ordinal",                                 ordinal                                      },
                { "warmup",                                  ordinal < 0                                  },
                { "complete",                                complete                                     },
                { "fixed_work_ms",                           fixed_ms                                     },
                { "native_solver_ms",                        solver_ms                                    },
                { "independent_revalidation_ms",             revalidation_ms                              },
                { "independent_validity",                    static_cast<int>(checked.report.validity)    },
                { "termination",                             static_cast<int>(run.run.termination_reason) },
                { "diagnostic",                              std::string(run.run.diagnostic_code)         },
                { "counters",                                counters(run)                                },
                { "profile",                                 profile_json(profile)                        },
                { "process_lifetime_peak_working_set_bytes", process_peak                                 },
                { "best_found",                              bench::snapshot_json(run.run.best)           }
            });
        }
        const char* included_phases =
            scope == "field" ? "spectral-search" : "initial-baseline,spectral-search,final-independent-validation";
        const char* excluded_phases = scope == "field"
                                          ? "accepted-preparation,initial-baseline,independent-revalidation"
                                          : "accepted-preparation";
        std::cout << bench::Json {
            { "ticket",                     "T-010"                                                   },
            { "profile",                    profile_name                                              },
            { "source",                     source                                                    },
            { "triangles",                  object->mesh().triangles.size()                           },
            { "cpu_thread_count",           1                                                         },
            { "scope",                      scope                                                     },
            { "field_work_revision",        geo::detail::validation_kernel::kRasterWorkRevision       },
            { "preparation_state",
             scope == "field" ? "accepted-input-and-retained-baseline-fixed" : "accepted-input-fixed" },
            { "included_phases",            included_phases                                           },
            { "excluded_phases",            excluded_phases                                           },
            { "cold_native_preparation_ms", preparation_ms                                            },
            { "initial_baseline_ms",        baseline_ms                                               },
            { "initial_baseline",           bench::snapshot_json(baseline.best)                       },
            { "pitch_mm",                   lattice.pitch_mm                                          },
            { "box_mm",                     { box.width_mm, box.depth_mm, box.height_mm }             },
            { "warmups",                    warmups                                                   },
            { "serial_samples",             samples                                                   },
            { "samples",                    records                                                   }
        }.dump(2) << '\n';
        return passed ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
