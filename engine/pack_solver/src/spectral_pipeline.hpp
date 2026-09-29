#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "spectrapack/solver/spectral.hpp"

namespace spectrapack::solver::detail {

struct SpectralPipelineResult {
    SpectralStats stats {};
    // Global tracked peak. Representation attempts exclude their reserve, so the
    // pipeline adds it; correlation peaks already include their reserve.
    std::uint64_t working_bytes_peak {};
    std::string_view diagnostic { "SPECTRAL_PIPELINE_PENDING" };
    bool complete {};
    struct RankedCandidate {
        geometry::CellIndex translation {};
        double score {};
        bool direct_zero_overlap {};
        std::uint64_t orientation_index {};
        double binary_overlap {};
    };
    // One owned, bounded page derived from the checked correlation arrays. The
    // caller consumes this as a proposal hint and never treats FFT as authority.
    std::vector<RankedCandidate> ranked_candidates;
    geometry::PhysicalQueryStats ranking_bounds_stats {};
};

enum class CandidatePageMode { ordinary, low_overlap };

struct CandidatePageQuery {
    std::uint64_t orientation_index {};
    std::optional<SpectralPipelineResult::RankedCandidate> exclusive_cursor;
    CandidatePageMode mode { CandidatePageMode::ordinary };
};

struct BinaryObservation {
    geometry::GridWindow environment_window {};
    geometry::GridWindow kernel_window {};
    std::span<const std::uint8_t> environment;
    std::span<const std::uint8_t> kernel;
    std::span<const double> values;
    std::span<const double> proximity;
    std::span<const double> proximity_values;
    compute::Index3 translation_first {};
    compute::Shape3 output_shape {};
};

using BinaryObservationSink = void (*)(const BinaryObservation&) noexcept;

// Fixed-size, allocation-free union of the public ownership snapshots.  The
// tokens borrow their owners, which the pipeline keeps alive through a call.
class ResidencyLedger {
public:
    [[nodiscard]] bool include(const geometry::RepresentationResidency&) noexcept;
    [[nodiscard]] bool include(const std::optional<geometry::RepresentationResidency>&) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> bytes() const noexcept;
    // The representation factory charges I itself: reserve external + G\I.
    [[nodiscard]] std::optional<std::uint64_t> reserve_for(const geometry::RepresentationResidency&,
                                                           std::uint64_t external) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> reserve_for(std::span<const geometry::RepresentationResidency>,
                                                           std::uint64_t external) const noexcept;

private:
    std::array<geometry::RepresentationResidentBlock, 24> blocks_ {};
    std::uint8_t count_ {};
};

[[nodiscard]] SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>&,
                                                             geometry::GridLattice, const SpectralLimits&,
                                                             const std::shared_ptr<const geometry::ValidatedSolution>&,
                                                             const OrientationCatalog&,
                                                             BinaryObservationSink = nullptr);
[[nodiscard]] SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>&,
                                                             geometry::GridLattice, const SpectralLimits&,
                                                             const std::shared_ptr<const geometry::ValidatedSolution>&,
                                                             const OrientationCatalog&, const CandidatePageQuery&,
                                                             BinaryObservationSink = nullptr);
[[nodiscard]] SpectralPipelineResult build_spectral_pipeline(const std::shared_ptr<const geometry::ValidationContext>&,
                                                             geometry::GridLattice, const SpectralLimits&,
                                                             const std::shared_ptr<const geometry::ValidatedSolution>&,
                                                             BinaryObservationSink = nullptr);

}  // namespace spectrapack::solver::detail
