#pragma once

#include <chrono>
#include <cstdint>

#include "exact_predicates.hpp"

namespace spectrapack::geometry::detail {

enum class ImportProfilePhase { topology, intersection, volume, containment, material, total };
struct ImportProfileSample {
    ImportProfilePhase phase;
    double elapsed_ms {};
    std::uint64_t predicate_work {}, orient2_calls {}, orient3_calls {}, interval_hits {}, structural_zeros {},
        exact_fallbacks {};
};
using ImportProfileSink = void (*)(void*, const ImportProfileSample&) noexcept;
// Private, operation-thread-only evidence seam. Production leaves it empty.
inline thread_local ImportProfileSink import_profile_sink {};
inline thread_local void* import_profile_context {};

class ImportProfileBinding {
public:
    ImportProfileBinding(ImportProfileSink sink, void* context) noexcept :
        prior_(import_profile_sink),
        context_(import_profile_context)
    {
        import_profile_sink = sink;
        import_profile_context = context;
    }
    ~ImportProfileBinding()
    {
        import_profile_sink = prior_;
        import_profile_context = context_;
    }
    ImportProfileBinding(const ImportProfileBinding&) = delete;
    ImportProfileBinding& operator=(const ImportProfileBinding&) = delete;

private:
    ImportProfileSink prior_;
    void* context_;
};

class ImportProfileTimer {
public:
    ImportProfileTimer(ImportProfilePhase phase, const exact::WorkBudget& budget) noexcept :
        budget_(budget),
        phase_(phase),
        sink_(import_profile_sink),
        context_(import_profile_context)
    {
        mark();
    }
    ~ImportProfileTimer() { emit(); }
    ImportProfileTimer(const ImportProfileTimer&) = delete;
    ImportProfileTimer& operator=(const ImportProfileTimer&) = delete;
    void phase(ImportProfilePhase phase) noexcept
    {
        emit();
        phase_ = phase;
        mark();
    }

private:
    void mark() noexcept
    {
        before_ = { phase_,
                    0,
                    budget_.used(),
                    budget_.orient2_calls(),
                    budget_.orient3_calls(),
                    budget_.interval_hits(),
                    budget_.structural_zeros(),
                    budget_.exact_fallbacks() };
        if (sink_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    void emit() noexcept
    {
        if (!sink_) {
            return;
        }
        sink_(context_,
              { phase_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(),
                budget_.used() - before_.predicate_work, budget_.orient2_calls() - before_.orient2_calls,
                budget_.orient3_calls() - before_.orient3_calls, budget_.interval_hits() - before_.interval_hits,
                budget_.structural_zeros() - before_.structural_zeros,
                budget_.exact_fallbacks() - before_.exact_fallbacks });
    }
    const exact::WorkBudget& budget_;
    ImportProfilePhase phase_;
    ImportProfileSink sink_;
    void* context_;
    ImportProfileSample before_ {};
    std::chrono::steady_clock::time_point start_ {};
};

}  // namespace spectrapack::geometry::detail
