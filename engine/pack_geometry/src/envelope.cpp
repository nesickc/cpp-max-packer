#include "envelope.hpp"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Random.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/alpha_wrap_3.h>

#include <cmath>
#include <span>

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

// The refinement limits above bound how many searches the wrap makes, not what
// one search costs: where many source pieces overlap, each search visits all of
// them. Counting every visited piece bounds the work itself, and lets a stop
// request end a long search.
struct SearchWork {
    const runtime::OperationControl* control;
    std::uint64_t limit;
    std::uint64_t visits {};
};
thread_local SearchWork* search_work {};
struct SearchWorkScope {
    explicit SearchWorkScope(SearchWork& work) { search_work = &work; }
    ~SearchWorkScope() { search_work = nullptr; }
};
inline void count_search_visit()
{
    auto& work = *search_work;
    if ((++work.visits & 0xfff) == 0) {
        operation_checkpoint(*work.control);
        if (work.visits > work.limit) {
            throw LimitReached {};
        }
    }
}

namespace wrap = CGAL::Alpha_wraps_3::internal;
using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using OracleTypes = wrap::TS_oracle_traits<Kernel>;

// CGAL's traversals of the split source triangles, with each visit counted.
struct CountedTraversal {
    using Split = wrap::Splitter_traversal_traits<OracleTypes::AABB_traits>;
    using Piece = OracleTypes::AABB_traits::Primitive;
    struct Projection_traits : Split::Projection_traits {
        using Split::Projection_traits::Projection_traits;
        void intersection(const Kernel::Point_3& query, const Piece& piece)
        {
            count_search_visit();
            Split::Projection_traits::intersection(query, piece);
        }
    };
    template <class Query>
    struct Do_intersect_traits : Split::Do_intersect_traits<Query> {
        using Split::Do_intersect_traits<Query>::Do_intersect_traits;
        void intersection(const Query& query, const Piece& piece)
        {
            count_search_visit();
            Split::Do_intersect_traits<Query>::intersection(query, piece);
        }
    };
    template <class Query>
    struct First_intersection_traits : Split::First_intersection_traits<Query> {
        using Split::First_intersection_traits<Query>::First_intersection_traits;
        void intersection(const Query& query, const Piece& piece)
        {
            count_search_visit();
            Split::First_intersection_traits<Query>::intersection(query, piece);
        }
    };
};

// CGAL's Triangle_soup_oracle with the counted traversals and a build that a
// stop request can end. Pieces, trees and query results are unchanged.
using SearchTree =
    wrap::AABB_tree_oracle<OracleTypes::Geom_traits, OracleTypes::AABB_tree, CountedTraversal, int>;
using Splitter = wrap::AABB_tree_oracle_splitter<true, OracleTypes::Point_3, OracleTypes::Geom_traits>;
struct SourceOracle : SearchTree, Splitter {
    using Geom_traits = OracleTypes::Geom_traits;
    explicit SourceOracle(double alpha) : SearchTree(0, Geom_traits(Kernel())), Splitter(alpha)
    {
        initialize_tree_property_maps(tree());
    }
    void add(const std::vector<Kernel::Point_3>& points, std::span<const Triangle> faces,
             const runtime::OperationControl& control)
    {
        reserve(faces.size());
        const auto degenerate = geom_traits().is_degenerate_3_object();
        for (const auto& face : faces) {
            operation_checkpoint(control);
            const Geom_traits::Triangle_3 triangle(points[face[0]], points[face[1]], points[face[2]]);
            if (!degenerate(triangle)) {
                split_and_insert_datum(triangle, tree(), geom_traits());
            }
        }
        tree().accelerate_distance_queries();
    }
};
}  // namespace
std::optional<EnvelopeParameters> envelope_parameters(Bounds bounds)
{
    const double diagonal =
        std::hypot(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1], bounds.max[2] - bounds.min[2]);
    const double alpha = diagonal / 60.0, offset = diagonal / 2000.0;
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
std::optional<std::uint64_t> envelope_search_pieces(MeshView source, double alpha_mm, std::uint64_t limit,
                                                    const runtime::OperationControl& control)
{
    // CGAL (splitting_helper.h) bisects the longest edge while its squared
    // length exceeds nine squared alphas, in interval arithmetic. The slightly
    // lower bound here splits at least as often.
    const double squared_bound = 9.0 * alpha_mm * alpha_mm * (1.0 - 1e-9);
    if (!(squared_bound > 0.0)) {
        return source.triangles.size() <= limit ? std::optional<std::uint64_t>(source.triangles.size())
                                                : std::nullopt;
    }
    const auto squared_length = [](const Vec3& a, const Vec3& b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
    };
    std::uint64_t pieces = 0;
    std::vector<std::array<Vec3, 3>> pending;
    for (const auto& face : source.triangles) {
        operation_checkpoint(control);
        pending.push_back({ source.vertices[face[0]], source.vertices[face[1]], source.vertices[face[2]] });
        while (!pending.empty()) {
            const auto triangle = pending.back();
            pending.pop_back();
            const double lengths[3] = { squared_length(triangle[1], triangle[2]),
                                        squared_length(triangle[2], triangle[0]),
                                        squared_length(triangle[0], triangle[1]) };
            const int apex = lengths[0] >= lengths[1] ? (lengths[0] >= lengths[2] ? 0 : 2)
                                                      : (lengths[1] >= lengths[2] ? 1 : 2);
            const auto& first = triangle[(apex + 1) % 3];
            const auto& second = triangle[(apex + 2) % 3];
            const Vec3 middle { (first[0] + second[0]) / 2, (first[1] + second[1]) / 2,
                                (first[2] + second[2]) / 2 };
            if (!(lengths[apex] > squared_bound)) {
                if (++pieces > limit) {
                    return {};
                }
                continue;
            }
            // A midpoint that is an endpoint can never shorten the edge enough.
            if (middle == first || middle == second) {
                return {};
            }
            pending.push_back({ triangle[apex], first, middle });
            pending.push_back({ triangle[apex], middle, second });
        }
    }
    return pieces;
}
std::variant<ReconstructedMesh, ImportFailure> build_packing_envelope(MeshView source, Bounds bounds,
                                                                      const runtime::OperationControl& control,
                                                                      std::uint64_t search_visit_limit)
{
    const auto parameters = envelope_parameters(bounds);
    if (!parameters || source.triangles.empty()) {
        return ImportFailure {
            "INVALID_SOLID", "ENVELOPE_BOUNDS", "A packing envelope requires finite nonempty geometry.", {}
        };
    }
    try {
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
        SearchWork work { &control, search_visit_limit };
        const SearchWorkScope work_scope(work);
        // What CGAL::alpha_wrap_3 does, with the counted oracle.
        SourceOracle oracle(parameters->alpha_mm);
        oracle.add(points, source.triangles, control);
        wrap::Alpha_wrapper_3<SourceOracle> wrapper(oracle);
        wrapper(parameters->alpha_mm, parameters->offset_mm, shell, CGAL::parameters::visitor(visitor));
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
