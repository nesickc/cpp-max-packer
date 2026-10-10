#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../engine/pack_compute/src/correlation_profile.hpp"
#include "../../engine/pack_geometry/src/field_kernel.hpp"
#include "../../engine/pack_geometry/src/field_profile.hpp"
#include "../../engine/pack_geometry/src/placed_field_test_support.hpp"
#include "../../engine/pack_geometry/src/raster_execution_internal.hpp"
#include "../../engine/pack_geometry/tests/validation_fixtures.hpp"
#include "../../engine/pack_solver/src/pipeline_profile.hpp"
#include "../../engine/pack_solver/src/spectral_pipeline.hpp"
#include "../../engine/pack_solver/src/storage_accounting.hpp"
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
    geo::detail::RasterEvidence raster;
    Clock::time_point start;
    std::array<double, 24> orientation_ms {}, validation_ms {}, scoring_ms {};
    std::array<std::uint64_t, 24> orientation_end_candidates {}, orientation_copies {};
    double baseline_validation_ms {}, baseline_scoring_ms {}, dominated_scoring_ms {};
    std::uint64_t validation_calls {}, scoring_calls {}, first36_seed {}, first36_candidate {};
    std::optional<double> first_valid36_ms, first_published36_ms;
    std::array<std::array<std::array<double, 3>, 3>, 2> fft_axis_ms {};
};
void baseline_sample(void* context, const sol::detail::BaselineProfileSample& sample) noexcept
{
    auto& profile = *static_cast<Profile*>(context);
    using Phase = sol::detail::BaselineProfilePhase;
    if (sample.phase == Phase::validation) {
        profile.baseline_validation_ms += sample.elapsed_ms;
        ++profile.validation_calls;
        if (sample.native_valid && sample.copy_count == 36 && !profile.first_valid36_ms) {
            profile.first_valid36_ms = elapsed(profile.start);
            profile.first36_seed = sample.seed;
            profile.first36_candidate = sample.candidate_evaluations;
        }
    }
    else if (sample.phase == Phase::scoring) {
        profile.baseline_scoring_ms += sample.elapsed_ms;
        ++profile.scoring_calls;
        if (sample.copy_count < sample.incumbent_count) {
            profile.dominated_scoring_ms += sample.elapsed_ms;
        }
    }
    if (sample.seed < profile.orientation_ms.size()) {
        if (sample.phase == Phase::orientation) {
            profile.orientation_ms[sample.seed] += sample.elapsed_ms;
            profile.orientation_end_candidates[sample.seed] = sample.candidate_evaluations;
            profile.orientation_copies[sample.seed] = sample.copy_count;
        }
        else if (sample.phase == Phase::validation) {
            profile.validation_ms[sample.seed] += sample.elapsed_ms;
        }
        else {
            profile.scoring_ms[sample.seed] += sample.elapsed_ms;
        }
    }
}
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
    bench::Json output {
        { "pipeline_ms",         pipeline      },
        { "nested_field_phases", fields        },
        { "pipeline_calls",      profile.pages }
    };
    output["nested_fft_axis_ms"] = profile.fft_axis_ms;
    output["fft_axis_order"] = "binary/proximity; environment-forward/kernel-forward/inverse; X/Y/Z";
    if (profile.validation_calls) {
        bench::Json orientations = bench::Json::array();
        std::uint64_t previous {};
        for (std::size_t seed = 0; seed != profile.orientation_ms.size(); ++seed) {
            orientations.push_back({
                { "seed",                  seed                                              },
                { "inclusive_ms",          profile.orientation_ms[seed]                      },
                { "validation_ms",         profile.validation_ms[seed]                       },
                { "scoring_ms",            profile.scoring_ms[seed]                          },
                { "candidate_evaluations", profile.orientation_end_candidates[seed] >= previous
                                               ? profile.orientation_end_candidates[seed] - previous
                                               : 0 },
                { "validated_copies",      profile.orientation_copies[seed]                  }
            });
            previous = profile.orientation_end_candidates[seed];
        }
        output["baseline"] = {
            { "exclusive_validation_ms",         profile.baseline_validation_ms                               },
            { "exclusive_scoring_ms",            profile.baseline_scoring_ms                                  },
            { "strictly_lower_count_scoring_ms", profile.dominated_scoring_ms                                 },
            { "validation_calls",                profile.validation_calls                                     },
            { "scoring_calls",                   profile.scoring_calls                                        },
            { "first_native_valid36_ms",
             profile.first_valid36_ms ? bench::Json(*profile.first_valid36_ms) : bench::Json(nullptr)         },
            { "first36_seed",                    profile.first36_seed                                         },
            { "first36_candidate_evaluations",   profile.first36_candidate                                    },
            { "first_central_published36_ms",
             profile.first_published36_ms ? bench::Json(*profile.first_published36_ms) : bench::Json(nullptr) },
            { "orientations",                    orientations                                                 }
        };
    }
    return output;
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
        { "geometry_kernel_work",       run.run.stats.geometry_kernel_work            },
        { "geometry_vertex_visits",     run.run.stats.geometry_vertex_visits          },
        { "invalid_candidates",         run.run.stats.invalid_candidates              },
        { "indeterminate_candidates",   run.run.stats.indeterminate_candidates        },
        { "refinement_evaluations",     run.spectral_stats.refinement_evaluations     },
        { "tracked_working_bytes_peak", run.run.stats.tracked_working_bytes_peak      }
    };
}

bench::Json work_limits(const sol::SpectralLimits& limits)
{
    const auto phase = [](const sol::BaselineLimits& value) {
        return bench::Json {
            { "candidates",             value.max_candidate_evaluations                   },
            { "passes",                 value.max_search_passes                           },
            { "copies",                 value.max_copies                                  },
            { "orientations",           value.max_orientations                            },
            { "axis_cells",             value.max_axis_cells                              },
            { "working_bytes",          value.max_working_bytes                           },
            { "reserved_bytes",         value.reserved_bytes                              },
            { "geometry_work",          value.max_geometry_kernel_work                    },
            { "geometry_vertex_visits", value.max_geometry_vertex_visits                  },
            { "validation_work",        value.max_validation_kernel_work                  },
            { "validation_pair_tests",  value.max_validation_aabb_pair_tests              },
            { "per_query",
             { { "working_bytes", value.per_query.max_working_bytes },
                { "work", value.per_query.max_kernel_work },
                { "vertex_visits", value.per_query.max_vertex_visits } }                  },
            { "per_validation",
             { { "working_bytes", value.per_validation.max_working_bytes },
                { "work", value.per_validation.max_kernel_work },
                { "pair_tests", value.per_validation.max_aabb_pair_tests },
                { "copies", value.per_validation.max_copy_count },
                { "diagnostic_examples", value.per_validation.max_diagnostic_examples } } }
        };
    };
    return {
        { "baseline",                   phase(limits.baseline)                     },
        { "spectral",                   phase(limits.spectral)                     },
        { "working_bytes",              limits.max_working_bytes                   },
        { "caller_reserved_bytes",      limits.reserved_bytes                      },
        { "representation_work",        limits.max_representation_kernel_work      },
        { "representation_cell_visits", limits.max_representation_cell_visits      },
        { "direct_terms",               limits.max_direct_terms                    },
        { "proximity_terms",            limits.max_proximity_terms                 },
        { "refinement_evaluations",     limits.max_refinement_evaluations          },
        { "per_representation",
         { { "working_bytes", limits.per_representation.max_working_bytes },
            { "work", limits.per_representation.max_kernel_work },
            { "cell_visits", limits.per_representation.max_cell_visits },
            { "cells", limits.per_representation.max_cells },
            { "reserved_bytes", limits.per_representation.reserved_bytes },
            { "input_triangles", limits.per_representation.max_input_triangles } } },
        { "per_correlation",
         { { "working_bytes", limits.per_correlation.max_working_bytes },
            { "reserved_bytes", limits.per_correlation.reserved_bytes },
            { "padded_cells", limits.per_correlation.max_padded_cells },
            { "direct_terms", limits.per_correlation.max_direct_terms } }          }
    };
}

// Diagnostic only: expose the existing preflight components without allocating
// any fields or running an FFT. The saved poses are freshly native-validated.
bench::Json admission_probe(const std::shared_ptr<const geo::ValidationContext>& context, geo::GridLattice lattice,
                            sol::SpectralLimits limits, const sol::OrientationCatalog& catalog,
                            const std::string& result_path)
{
    std::ifstream saved(result_path);
    const auto result = bench::Json::parse(saved);
    std::vector<geo::CopyPose> poses;
    for (const auto& pose : result.at("placements")) {
        poses.push_back({ pose.at("copy_id").get<std::string>(), pose.at("translation_mm").get<geo::Vec3>(),
                          pose.at("quaternion_xyzw").get<geo::Quaternion>() });
    }
    const auto made = geo::make_candidate(context, std::move(poses));
    const auto checked = geo::validate(context, std::get<std::shared_ptr<const geo::Candidate>>(made));
    if (!checked.validated_solution || checked.validated_solution->copies().size() != 306) {
        throw std::runtime_error("saved306 failed fresh validation");
    }
    sol::Incumbent incumbent(context);
    const auto offered = incumbent.offer(checked.validated_solution, {});
    if (!offered.best) {
        throw std::runtime_error("saved306 scoring failed");
    }
    const auto solution_bytes =
        checked.validated_solution->resident_buffer_bytes().value() + 4 * sol::detail::kSharedOwnerControlBytes;
    const auto snapshot_bytes = offered.retained_storage_bytes;
    // Match the retained exact user's caller reserve, not this small probe's RSS.
    constexpr std::uint64_t caller_bytes = 96'252'289;
    limits.reserved_bytes = caller_bytes + snapshot_bytes;
    geo::CellShape environment {}, kernel {};
    const std::array<double, 3> dimensions { 400, 340, 285 };
    for (std::size_t axis = 0; axis != 3; ++axis) {
        environment[axis] = static_cast<std::uint32_t>(std::ceil(dimensions[axis] / lattice.pitch_mm)) + 2;
    }
    for (const auto& q : catalog.quaternions) {
        const auto window = geo::estimate_object_window(*context->object(), lattice, q).value();
        for (std::size_t axis = 0; axis != 3; ++axis) {
            kernel[axis] = std::max(kernel[axis], window.shape[axis]);
        }
    }
    const auto fft =
        std::get<spectrapack::compute::CorrelationEstimate>(spectrapack::compute::estimate_correlation_cpu({
            environment, kernel, { -1, -1, -1 },
              {}
    }));
    const auto cells = [](geo::CellShape shape) {
        return static_cast<std::uint64_t>(shape[0]) * shape[1] * shape[2];
    };
    auto expanded = environment;
    const auto halo = static_cast<std::uint32_t>(std::ceil(1 / lattice.pitch_mm)) + 2;
    for (auto& axis : expanded) {
        axis += 2 * halo;
    }
    const auto environment_bytes = cells(environment) * 15;
    const auto expanded_bytes = cells(expanded) * 10;
    const auto kernel_bytes = cells(kernel) * 10;
    const auto width = 2 * halo + 1;
    const auto stencil_bytes = static_cast<std::uint64_t>(width) * width * width * sizeof(geo::CellIndex);
    const auto proximity_bytes =
        static_cast<std::uint64_t>(*std::max_element(environment.begin(), environment.end())) * 24;
    const auto fft_retained_bytes = fft.padded_cells * sizeof(double);
    const auto geometry_bytes = geo::estimate_field_geometry_bytes(*context->object()).value();
    const auto worker_bytes = geo::estimate_raster_execution_bytes(limits.cpu_thread_count).value();
    const auto fixed_bytes = environment_bytes + expanded_bytes + kernel_bytes + stencil_bytes + proximity_bytes +
                             fft.working_bytes + fft_retained_bytes;
    bench::Json scenarios = bench::Json::array();
    for (const bool active306 : { false, true }) {
        auto scenario_limits = limits;
        if (!active306) {
            scenario_limits.reserved_bytes += solution_bytes;
        }
        const auto failure = sol::detail::spectral_admission(
            context, lattice, scenario_limits, active306 ? checked.validated_solution : nullptr, &catalog);
        auto required = failure;
        if (!required) {
            scenario_limits.max_working_bytes = 0;
            required = sol::detail::spectral_admission(context, lattice, scenario_limits,
                                                       active306 ? checked.validated_solution : nullptr, &catalog);
        }
        scenarios.push_back({
            { "active_copies",                      active306 ? 306 : 0                                                              },
            { "resident_baseline_copies",           306                                                                              },
            { "failure",                            failure ? bench::Json(std::string(failure->cause_code())) : bench::Json(nullptr) },
            { "required_bytes",
             required && required->resource ? bench::Json(required->resource->required) : bench::Json(nullptr)                       },
            { "active_footprint_upper_bound_bytes", active306 ? cells(environment) * 306 * sizeof(std::size_t) : 0                   }
        });
    }
    return {
        { "measurement",                           "native conservative admission estimates; no fields or FFT executed" },
        { "profile",                               "pryanik2-user"                                                      },
        { "source",                                "rc/items/pryanik_2.STL"                                             },
        { "pitch_mm",                              lattice.pitch_mm                                                     },
        { "box_mm",                                { 400, 340, 285 }                                                    },
        { "orientation",                           "cube"                                                               },
        { "clearance_mm",                          { { "pair", 1 }, { "wall", 1 } }                                     },
        { "cpu_thread_count",                      limits.cpu_thread_count                                              },
        { "host_cap_bytes",                        limits.max_working_bytes                                             },
        { "environment_shape",                     environment                                                          },
        { "maximum_catalog_kernel_shape",          kernel                                                               },
        { "padded_fft_shape",                      fft.padded_shape                                                     },
        { "padded_fft_cells",                      fft.padded_cells                                                     },
        { "caller_reserved_bytes",                 caller_bytes                                                         },
        { "accepted_geometry_bytes",               context->object()->resident_buffer_bytes().value()                   },
        { "resident_solution_bytes",               solution_bytes                                                       },
        { "resident_snapshot_and_score_key_bytes", snapshot_bytes                                                       },
        { "raster_prepared_geometry_bytes",        geometry_bytes                                                       },
        { "raster_worker_reserve_bytes",           worker_bytes                                                         },
        { "fixed_buffer_bytes",                    fixed_bytes                                                          },
        { "fixed_buffer_components",
         { { "environment_arrays", environment_bytes },
            { "expanded_field", expanded_bytes },
            { "kernel_field", kernel_bytes },
            { "stencil", stencil_bytes },
            { "proximity_scratch", proximity_bytes },
            { "correlation_workspace", fft.working_bytes },
            { "retained_correlation_array", fft_retained_bytes } }                                                      },
        { "fresh_validation_work",                 checked.report.kernel_work                                           },
        { "fresh_validation_pair_tests",           checked.report.aabb_pair_tests                                       },
        { "scenarios",                             scenarios                                                            }
    };
}
}  // namespace

int main(int argc, char** argv)
{
    try {
        std::string profile_name = "analytic";
        int samples = 5, warmups = 1;
        std::uint32_t threads = 1;
        std::string scope = "field";
        std::string retained_result;
        double diagnostic_pitch = 2;
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
            else if (option == "--threads") {
                const auto value = std::stoll(argv[index + 1]);
                if (value < 1 || value > sol::cpu_supported_thread_count()) {
                    throw std::runtime_error("unsupported thread count");
                }
                threads = static_cast<std::uint32_t>(value);
            }
            else if (option == "--scope") {
                scope = argv[index + 1];
            }
            else if (option == "--retained-result") {
                retained_result = argv[index + 1];
            }
            else if (option == "--pitch-mm") {
                diagnostic_pitch = std::stod(argv[index + 1]);
                if (diagnostic_pitch != 2 && diagnostic_pitch != 3 && diagnostic_pitch != 4) {
                    throw std::runtime_error("diagnostic pitch must explicitly be 2, 3 or 4mm");
                }
            }
            else {
                throw std::runtime_error("unknown option");
            }
        }
        if (samples < 1 || samples > 100 || warmups < 0 || warmups > 10) {
            throw std::runtime_error("invalid sample count");
        }
        if (scope != "field" && scope != "prepared-start" && scope != "admission") {
            throw std::runtime_error("scope must be field, prepared-start or admission");
        }
        if (profile_name != "analytic" && profile_name != "ulamok" && profile_name != "ulamok4-user" &&
            profile_name != "ulamok4-limited" && profile_name != "pryanik2-many4-fixed" && profile_name != "pryanik1" &&
            profile_name != "pryanik2" && profile_name != "pryanik2-user") {
            throw std::runtime_error("unknown profile");
        }
        const bool pryanik_user = profile_name == "pryanik2-user";
        if (pryanik_user && scope != "admission") {
            throw std::runtime_error("pryanik2-user currently exposes admission scope only");
        }
        if (scope == "admission" && (!pryanik_user || retained_result.empty())) {
            throw std::runtime_error("admission requires pryanik2-user and --retained-result");
        }
        const bool user_profile = profile_name == "ulamok4-user";
        const bool ulamok_limited = profile_name == "ulamok4-limited";
        const bool pryanik_many = profile_name == "pryanik2-many4-fixed";
        const bool limited_profile = ulamok_limited || pryanik_many;
        const bool prepared_profile = user_profile || limited_profile;
        if (prepared_profile && scope != "prepared-start") {
            throw std::runtime_error("this profile requires prepared-start scope");
        }
        const bool analytic = profile_name == "analytic",
                   ulamok = profile_name == "ulamok" || user_profile || ulamok_limited;
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
        constraints.pair_clearance_mm = analytic ? .125 : prepared_profile || pryanik_user ? 1 : .1;
        constraints.wall_clearance_mm = analytic ? .25 : 1;
        constraints.orientations.mode =
            ulamok || pryanik_user ? geo::OrientationMode::cube : geo::OrientationMode::fixed;
        const geo::BoxDimensions box = analytic                       ? geo::BoxDimensions { 38, 28, 20 }
                                       : pryanik_user || pryanik_many ? geo::BoxDimensions { 400, 340, 285 }
                                       : ulamok ? geo::BoxDimensions { 400, prepared_profile ? 340. : 350., 285 }
                                                : geo::BoxDimensions { 100, 100, 50 };
        const auto make_context = [&]() {
            auto made = geo::make_validation_context(object, box, constraints);
            if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made)) {
                throw std::runtime_error("context failed");
            }
            return std::get<std::shared_ptr<const geo::ValidationContext>>(std::move(made));
        };
        sol::BaselineLimits baseline_limits;
        if (analytic) {
            baseline_limits.max_candidate_evaluations = 2;
        }
        std::shared_ptr<const geo::ValidationContext> field_context;
        std::shared_ptr<const geo::ValidatedSolution> field_seed;
        bench::Json initial_baseline;
        double baseline_ms {};
        if (scope == "field") {
            field_context = make_context();
            const auto baseline_start = Clock::now();
            const auto baseline = sol::run_aabb_baseline(field_context, baseline_limits, {});
            baseline_ms = elapsed(baseline_start);
            if (!baseline.best) {
                throw std::runtime_error("baseline failed");
            }
            initial_baseline = bench::snapshot_json(baseline.best);
            field_seed = baseline.best->solution;
        }
        const geo::GridLattice lattice {
            { 0, 0, 0 },
            pryanik_user                  ? diagnostic_pitch
            : analytic                    ? 1.
            : ulamok && !prepared_profile ? 16.
                                          : 4.
        };
        sol::SpectralLimits limits;
        limits.cpu_thread_count = threads;
        if (pryanik_many) {
            // Preserve the exact user's caller reserve; this is a full-mesh,
            // many-copy fixed-identity workload at explicitly supported 4mm.
            limits.reserved_bytes = 96'252'289;
        }
        if (scope == "admission") {
            const auto catalog = std::get<sol::OrientationCatalog>(
                sol::make_orientation_catalog(constraints.orientations, limits.baseline.max_orientations));
            std::cout << admission_probe(make_context(), lattice, limits, catalog, retained_result).dump(2) << '\n';
            return 0;
        }
        limits.baseline.max_candidate_evaluations = limits.baseline.max_search_passes = 0;
        if (scope == "prepared-start") {
            limits.baseline = baseline_limits;
        }
        if (!user_profile) {
            limits.spectral.max_candidate_evaluations = limits.spectral.max_search_passes = 1;
            limits.max_refinement_evaluations = 0;
        }
        bool passed = true;
        bench::Json records = bench::Json::array();
        for (int ordinal = -warmups; ordinal != samples; ++ordinal) {
            Profile profile;
            geo::detail::set_raster_evidence_sink(
                [](void* context, const geo::detail::RasterEvidence& evidence) noexcept {
                static_cast<Profile*>(context)->raster = evidence;
            }, &profile);
            sol::detail::pipeline_profile_sink = &pipeline_sample;
            sol::detail::pipeline_profile_context = &profile;
            geo::detail::field_profile_sink = &field_sample;
            geo::detail::field_profile_context = &profile;
            spectrapack::compute::detail::fft_axis_profile_context = &profile;
            spectrapack::compute::detail::fft_axis_profile_sink =
                [](void* context, const spectrapack::compute::detail::FftAxisProfileSample& sample) noexcept {
                static_cast<Profile*>(context)->fft_axis_ms[sample.binary ? 0 : 1][sample.transform][sample.axis] +=
                    sample.elapsed_ms;
            };
            const auto start = Clock::now();
            profile.start = start;
            double catalog_ms {};
            std::optional<sol::OrientationCatalog> resolved_catalog;
            if (prepared_profile) {
                sol::detail::baseline_profile_sink = &baseline_sample;
                sol::detail::baseline_profile_context = &profile;
                const auto catalog_start = Clock::now();
                auto made = sol::make_orientation_catalog(constraints.orientations, limits.baseline.max_orientations);
                if (!std::holds_alternative<sol::OrientationCatalog>(made)) {
                    throw std::runtime_error("diagnostic catalog failed");
                }
                resolved_catalog.emplace(std::get<sol::OrientationCatalog>(std::move(made)));
                catalog_ms = elapsed(catalog_start);
            }
            // A prepared Start owns a fresh job context and baseline. Only the
            // accepted input stays fixed; no unrelated reference solution is
            // retained while observing its cumulative process high-water.
            const auto context = scope == "field" ? field_context : make_context();
            const auto run = prepared_profile ? sol::run_cpu_spectral(context, lattice, *resolved_catalog, limits, {},
                                                                      [&](const sol::SnapshotHandle& snapshot) {
                if (snapshot && snapshot->score.count == 36 && !profile.first_published36_ms) {
                    profile.first_published36_ms = elapsed(start);
                }
            })
                                              : sol::run_cpu_spectral(context, lattice, limits, {}, {}, field_seed);
            const double solver_ms = elapsed(start);
            sol::detail::baseline_profile_sink = nullptr;
            sol::detail::pipeline_profile_sink = nullptr;
            geo::detail::field_profile_sink = nullptr;
            spectrapack::compute::detail::fft_axis_profile_sink = nullptr;
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
            if (!GetProcessMemoryInfo(GetCurrentProcess(), &process_counters, sizeof(process_counters))) {
                const auto error = GetLastError();
                throw std::runtime_error("GetProcessMemoryInfo failed (Win32 error " + std::to_string(error) + ").");
            }
            process_peak = process_counters.PeakWorkingSetSize;
#endif
            const bool complete =
                (run.run.termination_reason == sol::TerminationReason::budget_exhausted ||
                 run.run.termination_reason == sol::TerminationReason::search_stalled) &&
                run.run.diagnostic_code != "PHYSICAL_DEADLINE" &&
                run.spectral_stats.correlations >= (ulamok ? 48 : 2) && checked.validated_solution &&
                run.run.best->solution->copies().size() >= (ulamok         ? 36
                                                            : pryanik_many ? 280
                                                                           : 2) &&
                (!user_profile ||
                 (profile.first_valid36_ms && profile.first_published36_ms && run.baseline_stats.search_passes == 24 &&
                  run.spectral_stats.refinement_evaluations == 128)) &&
                (!limited_profile ||
                 (run.spectral_stats.correlations == (ulamok_limited ? 48 : 2) &&
                  run.baseline_stats.search_passes == (ulamok_limited ? 24 : 1) &&
                  run.run.stats.candidate_evaluations - run.baseline_stats.candidate_evaluations == 1 &&
                  run.spectral_stats.refinement_evaluations == 0 && run.field_admission &&
                  run.field_admission->footprint_copy_count == 0));
            passed &= complete;
            records.push_back({
                { "ordinal",                                 ordinal                                      },
                { "warmup",                                  ordinal < 0                                  },
                { "complete",                                complete                                     },
                { "fixed_work_ms",                           fixed_ms                                     },
                { "native_solver_ms",                        solver_ms                                    },
                { "catalog_ms",                              catalog_ms                                   },
                { "independent_revalidation_ms",             revalidation_ms                              },
                { "independent_validity",                    static_cast<int>(checked.report.validity)    },
                { "termination",                             static_cast<int>(run.run.termination_reason) },
                { "diagnostic",                              std::string(run.run.diagnostic_code)         },
                { "counters",                                counters(run)                                },
                { "field_admission",
                 run.field_admission
                      ? bench::Json { { "working_bytes_upper_bound", run.field_admission->working_bytes_upper_bound },
                                      { "footprint_copy_count", run.field_admission->footprint_copy_count },
                                      { "effective_host_cap_bytes", run.field_admission->effective_host_cap_bytes },
                                      { "cpu_thread_count", run.field_admission->cpu_thread_count },
                                      { "scheduling_policy", std::string(run.field_admission->scheduling_policy) } }
                      : bench::Json(nullptr)                                                              },
                { "parallel_raster_evidence",
                 { { "batches", profile.raster.batches },
                    { "descriptor_inspections", profile.raster.inspections },
                    { "grants", profile.raster.grants },
                    { "refunded_work_units", profile.raster.refunds },
                    { "thread_ids", profile.raster.thread_ids },
                    { "primitive_attempts", profile.raster.primitive_attempts },
                    { "actual_worker_stack_bytes", profile.raster.actual_stack_bytes },
                    { "background_stack_reservation_bytes", profile.raster.stack_reservation_bytes } }    },
                { "profile",                                 profile_json(profile)                        },
                { "process_lifetime_peak_working_set_bytes", process_peak                                 },
                { "best_found",                              bench::snapshot_json(run.run.best)           }
            });
            if (prepared_profile) {
                records.back()["diagnostic_counters"] = {
                    { "baseline_candidates",      run.baseline_stats.candidate_evaluations  },
                    { "baseline_passes",          run.baseline_stats.search_passes          },
                    { "baseline_validation_work", run.baseline_stats.validation_kernel_work },
                    { "baseline_geometry_work",   run.baseline_stats.geometry_kernel_work   },
                    { "refinement_evaluations",   run.spectral_stats.refinement_evaluations }
                };
            }
        }
        geo::detail::set_raster_evidence_sink(nullptr, nullptr);
        const char* included_phases = scope == "field"
                                          ? "spectral-search"
                                          : "job-context,initial-baseline,spectral-search,final-independent-validation";
        const char* excluded_phases = scope == "field"
                                          ? "accepted-preparation,initial-baseline,independent-revalidation"
                                          : "accepted-preparation";
        std::cout << bench::Json {
            { "seed",                          prepared_profile ? bench::Json("42") : bench::Json(nullptr)  },
            { "seed_note",
             "Native API has no RNG seed; recorded user seed is unused, catalog entries are deterministic." },
            { "measurement",
             limited_profile
                  ? "limited fixed work: one spectral candidate/pass, zero refinement; not full solve quality"
              : user_profile ? "instrumented exact-geometry full work, no wall deadline; not qualified speed"
                             : "qualified-profile fixed work"                                               },
            { "ticket",                        "T-010"                                                      },
            { "profile",                       profile_name                                                 },
            { "source",                        source                                                       },
            { "triangles",                     object->mesh().triangles.size()                              },
            { "cpu_thread_count",              threads                                                      },
            { "cpu_scheduling_policy",         std::string(sol::cpu_scheduling_policy(threads))             },
            { "accepted_input_resident_bytes", object->resident_buffer_bytes().value()                      },
            { "host_cap_bytes",                limits.max_working_bytes                                     },
            { "representation_work_cap",       limits.max_representation_kernel_work                        },
            { "work_limits",                   work_limits(limits)                                          },
            { "scope",                         scope                                                        },
            { "field_work_revision",           geo::detail::validation_kernel::kRasterWorkRevision          },
            { "placed_support_policy",         geo::detail::kPlacedSupportPolicy                            },
            { "preparation_state",
             scope == "field" ? "accepted-input-and-retained-baseline-fixed" : "accepted-input-fixed"       },
            { "included_phases",               included_phases                                              },
            { "excluded_phases",               excluded_phases                                              },
            { "cold_native_preparation_ms",    preparation_ms                                               },
            { "initial_baseline_ms",           baseline_ms                                                  },
            { "initial_baseline",              initial_baseline                                             },
            { "process_peak_basis",            "cumulative-process-high-water-including-preparation"        },
            { "pitch_mm",                      lattice.pitch_mm                                             },
            { "box_mm",                        { box.width_mm, box.depth_mm, box.height_mm }                },
            { "clearance_mm",
             { { "pair", constraints.pair_clearance_mm }, { "wall", constraints.wall_clearance_mm } }       },
            { "orientation",                   ulamok ? "cube" : "fixed"                                    },
            { "warmups",                       warmups                                                      },
            { "serial_samples",                samples                                                      },
            { "samples",                       records                                                      }
        }.dump(2) << '\n';
        return passed ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
