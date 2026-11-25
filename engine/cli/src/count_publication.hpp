#pragma once
#include <cmath>

#include "solve.hpp"

namespace spectrapack::cli {
class CountPublication final {
public:
    // A continued run numbers its snapshots after the result it continues.
    explicit CountPublication(std::uint64_t base_revision = 0) noexcept : revision_(base_revision) {}
    bool offer(solver::SnapshotHandle native, double seconds) noexcept
    {
        try {
            if (!native || !native->solution || native->solution->copies().empty()) {
                return false;
            }
            const auto& solution = native->solution;
            bool same_count = false;
            if (latest_) {
                const auto count = solution->copies().size();
                const auto previous_count = latest_->native->solution->copies().size();
                if (solution->context() != latest_->native->solution->context() || count < previous_count) {
                    return false;
                }
                same_count = count == previous_count;
                if (same_count) {
                    const auto& next = native->score;
                    const auto& prior = latest_->native->score;
                    const auto scored = [count](const auto& score) {
                        return score.count == count && score.enclosing_z_span_mm && score.enclosing_xy_span_sum_mm &&
                               std::isfinite(*score.enclosing_z_span_mm) &&
                               std::isfinite(*score.enclosing_xy_span_sum_mm);
                    };
                    if (!scored(next) || !scored(prior) ||
                        !(*next.enclosing_z_span_mm < *prior.enclosing_z_span_mm ||
                          (*next.enclosing_z_span_mm == *prior.enclosing_z_span_mm &&
                           *next.enclosing_xy_span_sum_mm < *prior.enclosing_xy_span_sum_mm))) {
                        return false;
                    }
                }
            }
            latest_ = std::make_shared<const PublishedSnapshot>(PublishedSnapshot {
                revision_ + 1, same_count ? latest_->time_to_count_seconds : seconds, std::move(native) });
            ++revision_;
            return true;
        }
        catch (...) {
            failed_ = true;
            return false;
        }
    }
    const PublishedSnapshotHandle& latest() const noexcept { return latest_; }
    bool failed() const noexcept { return failed_; }

private:
    PublishedSnapshotHandle latest_;
    std::uint64_t revision_ {};
    bool failed_ {};
};
}  // namespace spectrapack::cli
