#pragma once
#include <spectrapack/geometry/rigid_transform.hpp>

#include "count_publication.hpp"
#include "runtime_wire.hpp"

namespace spectrapack::cli {
// Only one fixed-size pose DOM is ever allocated, independently of packing count.
// Copy IDs are checked before pinning so scratch stays inside the reserved pool.
inline std::string encode_packing_record(io::Json envelope, const PublishedSnapshot& snapshot)
{
    const auto emit = [&](const std::shared_ptr<detail::RuntimeOutput>& out) {
        const auto literal = [&](std::string_view text) {
            out->write_characters(text.data(), text.size());
        };
        out->write_character('{');
        bool first = true;
        for (const auto& item : envelope.items()) {
            if (!first) {
                out->write_character(',');
            }
            first = false;
            detail::serialize(io::Json(item.key()), out);
            out->write_character(':');
            detail::serialize(item.value(), out);
        }
        literal(",\"packing\":{\"validation_status\":\"valid\",\"solution_revision\":");
        detail::serialize(io::Json(snapshot.revision), out);
        literal(",\"count\":");
        detail::serialize(io::Json(snapshot.native->solution->copies().size()), out);
        literal(",\"placements\":[");
        first = true;
        for (const auto& pose : snapshot.native->solution->copies()) {
            if (!first) {
                out->write_character(',');
            }
            first = false;
            const auto transform = geometry::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
            if (!transform) {
                throw std::runtime_error("Publication rigid pose unavailable.");
            }
            const io::Json placement {
                { "copy_id",         pose.copy_id                },
                { "translation_mm",  pose.translation_mm         },
                { "quaternion_xyzw", pose.rotation_xyzw          },
                { "local_to_world",  transform->local_to_world() }
            };
            detail::serialize(placement, out);
        }
        literal("]}}");
    };
    const auto count = std::make_shared<detail::RuntimeOutput>(runtime_wire_limit);
    emit(count);
    std::string bytes;
    bytes.reserve(count->size() + 1);
    if (bytes.capacity() > runtime_wire_capacity_limit) {
        throw std::length_error("Publication capacity overflow.");
    }
    emit(std::make_shared<detail::RuntimeOutput>(count->size(), &bytes));
    bytes += '\n';
    return bytes;
}
}  // namespace spectrapack::cli
