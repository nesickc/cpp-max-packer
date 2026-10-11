#pragma once
#include "solve.hpp"

namespace spectrapack::cli {
inline constexpr std::uint64_t publication_slot_bytes = 1ULL << 20;
// Capacity proof (pinned MSVC/STL + nlohmann 3.12): the solver already charges its
// current solution/context; this reserve admits TWO additional obsolete solution
// payloads (public/pending and writer-in-flight), each checked by resident capacity.
// During replacement the new solution is the solver's charged current owner; the
// pending old public owner and an older in-flight owner are the only extra payloads.
// All owners share the same already-charged immutable context/accepted solids.
// The writer drops its native handle before pipe I/O, and allocates only ONE wire
// string, capacity <= 1 MiB+64, after a count-only serialization pass. Its old string
// has died before the next iteration. Envelope + copy (11 bounded map nodes, five
// <=128-byte identities), one placement DOM (4 keys, 7+16 numeric values, <=4096-byte
// ID), serializer's fixed 512-byte buffers/indent string, adapters and native/shared
// wrappers fit below 64 KiB on these pinned types. The fourth 1 MiB pool covers that
// scratch plus the wire's 64-byte capacity excess, with >900 KiB slack; +4096 is
// additional control-block headroom, not an allowance to omit capacity accounting.
// Existing request/terminal queues, prior-run result and display owners remain in
// retained_other_bytes; this full reserve is charged during solve AND final export.
inline constexpr std::uint64_t publication_reserve_bytes = 4ULL * publication_slot_bytes + 4096;
class CountPublication final {
public:
    bool offer(solver::SnapshotHandle native, double seconds) noexcept
    {
        try {
            if (!native || !native->solution || native->solution->copies().empty()) {
                return false;
            }
            const auto& solution = native->solution;
            if (latest_ && (solution->context() != latest_->native->solution->context() ||
                            solution->copies().size() <= latest_->native->solution->copies().size())) {
                return false;
            }
            const auto bytes = solution->resident_buffer_bytes();
            if (!bytes || *bytes > publication_slot_bytes) {
                failed_ = true;
                return false;
            }
            for (const auto& pose : solution->copies()) {
                if (pose.copy_id.size() > 4096) {
                    failed_ = true;
                    return false;
                }
            }
            latest_ = std::make_shared<const PublishedSnapshot>(
                PublishedSnapshot { revision_ + 1, seconds, std::move(native) });
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
