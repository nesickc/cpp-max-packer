#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
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
    std::optional<RunFailureDetails> failure_details;
    bool complete {};
    // Private benchmark evidence; elapsed proximity phase, without correlation.
    double proximity_ms {};
    struct RankedCandidate {
        geometry::CellIndex translation {};
        double score {};
        bool direct_zero_overlap {};
        std::uint64_t orientation_index {};
        double binary_overlap {};
    };
    // One owned, bounded page derived from the checked correlation arrays. The
    // caller consumes this as a proposal hint and never treats FFT as authority.
    // Failure results have no eager vector proxy. Construct only after admission
    // and transfer the pointer without moving a vector (ADR 0015).
    using RankedPage = std::vector<RankedCandidate>;
    std::unique_ptr<RankedPage> ranked_candidates;
    geometry::PhysicalQueryStats ranking_bounds_stats {};
    std::optional<CpuFieldAdmissionEstimate> field_admission;
    // Checked pre-allocation maxima while this generation is still private.
    std::uint64_t admitted_bytes_upper_bound {};
    bool allocation_refused {};
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

class SpectralWorkspace {
public:
    SpectralWorkspace() noexcept;
    ~SpectralWorkspace();
    SpectralWorkspace(SpectralWorkspace&&) noexcept;
    SpectralWorkspace& operator=(SpectralWorkspace&&) noexcept;
    SpectralWorkspace(const SpectralWorkspace&) = delete;
    SpectralWorkspace& operator=(const SpectralWorkspace&) = delete;
    // Additional persistent payload, excluding accepted/context/solution owners
    // already held by the coordinator. Overflow/missing ownership fails closed.
    [[nodiscard]] std::optional<std::uint64_t> resident_bytes() const noexcept;
    [[nodiscard]] std::uint64_t layout_revision() const noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
    friend SpectralPipelineResult build_spectral_pipeline(SpectralWorkspace&,
                                                          const std::shared_ptr<const geometry::ValidationContext>&,
                                                          geometry::GridLattice, const SpectralLimits&,
                                                          const std::shared_ptr<const geometry::ValidatedSolution>&,
                                                          const OrientationCatalog&, const CandidatePageQuery&,
                                                          BinaryObservationSink, const RunControl&);
};

[[nodiscard]] SpectralPipelineResult build_spectral_pipeline(SpectralWorkspace&,
                                                             const std::shared_ptr<const geometry::ValidationContext>&,
                                                             geometry::GridLattice, const SpectralLimits&,
                                                             const std::shared_ptr<const geometry::ValidatedSolution>&,
                                                             const OrientationCatalog&, const CandidatePageQuery&,
                                                             BinaryObservationSink = nullptr, const RunControl& = {});

[[nodiscard]] std::optional<RunFailureDetails> spectral_admission(
    const std::shared_ptr<const geometry::ValidationContext>&, geometry::GridLattice, const SpectralLimits&,
    const std::shared_ptr<const geometry::ValidatedSolution>& = {}, const OrientationCatalog* actual_catalog = nullptr,
    std::uint64_t* admitted_field_bytes = nullptr);

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
    // At most 16 distinct representation tokens: source/container payloads(4),
    // prepared geometry(1), container mask(2), old+staged blockers(2), temporary
    // placed field(2), two LRU kernels(2), and a replacement kernel(2). Arrays,
    // correlation values and PMR pose identities are charged separately.
    // 32 leaves bounded headroom; malformed snapshots/overflow fail closed.
    std::array<geometry::RepresentationResidentBlock, 32> blocks_ {};
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

}  // namespace spectrapack::solver::detail
