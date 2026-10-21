#include "envelope.hpp"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Random.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/alpha_wrap_3.h>

#include <cmath>

#include "operation_checks.hpp"

namespace spectrapack::geometry::detail {
namespace {
struct LimitReached {};
struct Visitor : CGAL::Alpha_wraps_3::internal::Wrapping_default_visitor {
    const runtime::OperationControl* control;
    std::uint64_t steps {}, inserted {};
    template <class Wrapper>
    void check_size(const Wrapper& wrapper)
    {
        if (wrapper.triangulation().number_of_cells() > 100'000 || wrapper.queue().size() > 500'000) {
            throw LimitReached {};
        }
    }
    template <class Wrapper>
    bool go_further(const Wrapper& wrapper)
    {
        operation_checkpoint(*control);
        check_size(wrapper);
        if (++steps > 5'000'000) {
            throw LimitReached {};
        }
        return true;
    }
    template <class Wrapper, class Point>
    void before_Steiner_point_insertion(const Wrapper& wrapper, const Point&)
    {
        operation_checkpoint(*control);
        check_size(wrapper);
        if (++inserted > 25'000) {
            throw LimitReached {};
        }
    }
};
struct RandomScope {
    CGAL::Random previous = CGAL::get_default_random();
    RandomScope() { CGAL::get_default_random() = CGAL::Random(0); }
    ~RandomScope() { CGAL::get_default_random() = previous; }
};
}  // namespace
std::optional<EnvelopeParameters> envelope_parameters(Bounds bounds)
{
    const double diagonal =
        std::hypot(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2]);
    const double alpha = diagonal / 60.0, offset = diagonal / 1000.0;
    if (!std::isfinite(diagonal) || !(offset > 0)) {
        return {};
    }
    for (unsigned a = 0; a < 3; ++a) {
        if (!std::isfinite(bounds.min[a] - 2 * alpha) || !std::isfinite(bounds.max[a] + 2 * alpha) ||
            !(bounds.min[a] - offset < bounds.min[a]) || !(bounds.max[a] + offset > bounds.max[a])) {
            return {};
        }
    }
    return EnvelopeParameters { alpha, offset };
}
std::variant<ReconstructedMesh, ImportFailure> build_packing_envelope(MeshView source, Bounds bounds,
                                                                      const runtime::OperationControl& control)
{
    const auto parameters = envelope_parameters(bounds);
    if (!parameters || source.triangles.empty()) {
        return ImportFailure {
            "INVALID_SOLID", "ENVELOPE_BOUNDS", "A packing envelope requires finite nonempty geometry.", {}
        };
    }
    try {
        using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
        std::vector<Kernel::Point_3> points;
        points.reserve(source.vertices.size());
        for (const auto& p : source.vertices) {
            operation_checkpoint(control);
            points.emplace_back(p[0], p[1], p[2]);
        }
        CGAL::Surface_mesh<Kernel::Point_3> shell;
        RandomScope random;
        Visitor visitor;
        visitor.control = &control;
        CGAL::alpha_wrap_3(points, source.triangles, parameters->alpha_mm, parameters->offset_mm, shell,
                           CGAL::parameters::visitor(visitor));
        operation_checkpoint(control);
        if (shell.number_of_faces() > 500'000) {
            throw LimitReached {};
        }
        shell.collect_garbage();
        ReconstructedMesh result;
        result.vertices.resize(shell.number_of_vertices());
        result.triangles.reserve(shell.number_of_faces());
        for (auto v : shell.vertices()) {
            const auto& p = shell.point(v);
            result.vertices[v.idx()] = { p.x(), p.y(), p.z() };
        }
        for (auto f : shell.faces()) {
            operation_checkpoint(control);
            const auto h = shell.halfedge(f);
            result.triangles.push_back({ shell.target(h).idx(), shell.target(shell.next(h)).idx(),
                                         shell.target(shell.next(shell.next(h))).idx() });
        }
        return result;
    }
    catch (const LimitReached&) {
        return ImportFailure {
            "MEMORY_LIMIT", "ENVELOPE_LIMIT", "Packing envelope exceeded its bounded refinement limit.", {}
        };
    }
}
}  // namespace spectrapack::geometry::detail
