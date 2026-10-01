#include <bit>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "../../engine/pack_geometry/tests/validation_fixtures.hpp"
#include "../../engine/pack_solver/src/spectral_pipeline.hpp"
#include "baseline_support.hpp"
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
std::uint64_t field_checksum;
geo::CellShape environment_shape, padded_shape;
void capture(const sol::detail::BinaryObservation& observation) noexcept
{
    environment_shape = observation.environment_window.shape;
    padded_shape = observation.output_shape;
    if (observation.proximity_values.empty()) {
        return;
    }
    field_checksum = 0;
    for (const auto value : observation.proximity) {
        field_checksum = field_checksum * 1315423911ULL + std::bit_cast<std::uint64_t>(value);
    }
    for (const auto value : observation.values) {
        field_checksum = field_checksum * 1315423911ULL + std::bit_cast<std::uint64_t>(value);
    }
    for (const auto value : observation.proximity_values) {
        field_checksum = field_checksum * 1315423911ULL + std::bit_cast<std::uint64_t>(value);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    try {
        const bool larger = argc == 2 && std::string_view(argv[1]) == "--larger";
        if (argc > 2 || (argc == 2 && !larger)) {
            throw std::runtime_error("usage: spectrapack_t009_benchmark [--larger]");
        }
        bench::Json samples = bench::Json::array();
        bool passed = true;
        for (const auto dimensions : {
                 geo::Vec3 { 10, 8,  6  },
                  geo::Vec3 { 22, 18, 12 },
                  geo::Vec3 { 38, 28, 20 }
        }) {
            if (dimensions[0] == 38 && !larger) {
                continue;
            }
            geo::Constraints constraints;
            constraints.pair_clearance_mm = .125;
            constraints.wall_clearance_mm = .25;
            constraints.orientations.mode = geo::OrientationMode::fixed;
            const auto made = geo::make_validation_context(
                geo::test_support::accepted(geo::test_support::cuboid({ -1, -1.5, -.5 }, { 1, 1.5, .5 }),
                                            geo::AssetRole::object),
                geo::BoxDimensions { dimensions[0], dimensions[1], dimensions[2] }, constraints);
            if (!std::holds_alternative<std::shared_ptr<const geo::ValidationContext>>(made)) {
                throw std::runtime_error("context failed");
            }
            const auto context = std::get<std::shared_ptr<const geo::ValidationContext>>(made);
            const sol::OrientationCatalog catalog { 1, { { 0, 0, 0, 1 } } };
            const geo::GridLattice lattice {
                { -.25, .25, -.5 },
                1
            };
            sol::SpectralLimits limits;
            limits.baseline.max_candidate_evaluations = 1;
            limits.spectral.max_candidate_evaluations = 1;
            limits.max_refinement_evaluations = 0;
            for (int ordinal = -1; ordinal != 5; ++ordinal) {
                const auto start = std::chrono::steady_clock::now();
                field_checksum = 0;
                const auto field =
                    sol::detail::build_spectral_pipeline(context, lattice, limits, {}, catalog, &capture);
                const double field_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const auto fixed_start = std::chrono::steady_clock::now();
                const auto run = sol::run_cpu_spectral(context, lattice, catalog, limits, {});
                const double fixed_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fixed_start).count();
                std::uint64_t process_peak {};
#ifdef _WIN32
                PROCESS_MEMORY_COUNTERS counters {};
                counters.cb = sizeof(counters);
                if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
                    process_peak = counters.PeakWorkingSetSize;
                }
#endif
                auto entry = bench::Json {
                    { "box_mm",                         { dimensions[0], dimensions[1], dimensions[2] } },
                    { "pitch_mm",                       1                                               },
                    { "lattice_origin_mm",              { -.25, .25, -.5 }                              },
                    { "ordinal",                        ordinal                                         },
                    { "warmup",                         ordinal < 0                                     },
                    { "proximity_ms",                   field.proximity_ms                              },
                    { "field_ms",                       field_ms                                        },
                    { "fixed_work_ms",                  fixed_ms                                        },
                    { "complete",                       field.complete                                  },
                    { "diagnostic",                     field.diagnostic                                },
                    { "proximity_terms",                field.stats.proximity_terms                     },
                    { "field_checksum",                 std::to_string(field_checksum)                  },
                    { "environment_shape",              environment_shape                               },
                    { "padded_shape",                   padded_shape                                    },
                    { "direct_terms",                   field.stats.direct_terms                        },
                    { "correlations",                   field.stats.correlations                        },
                    { "tracked_peak_bytes",             field.working_bytes_peak                        },
                    { "process_peak_bytes",             process_peak                                    },
                    { "candidate_evaluations",          run.run.stats.candidate_evaluations             },
                    { "baseline_candidate_evaluations", run.baseline_stats.candidate_evaluations        },
                    { "run_proximity_terms",            run.spectral_stats.proximity_terms              },
                    { "termination",                    static_cast<int>(run.run.termination_reason)    },
                    { "run_diagnostic",                 run.run.diagnostic_code                         },
                    { "best_found",                     bench::snapshot_json(run.run.best)              }
                };
                samples.push_back(std::move(entry));
                passed = passed && field.complete && run.run.best && !run.run.best->solution->copies().empty();
            }
        }
        std::cout << bench::Json {
            { "ticket",         "T-009" },
            { "warmups",        1       },
            { "serial_samples", 5       },
            { "samples",        samples }
        }.dump(2) << '\n';
        return passed ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
