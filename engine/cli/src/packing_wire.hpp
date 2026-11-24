#pragma once
#include <spectrapack/geometry/rigid_transform.hpp>

#include "count_publication.hpp"
#include "runtime_wire.hpp"

namespace spectrapack::cli {
// A packing record lists every copy, so it is not held to the 1 MiB limit of
// the other runtime records.
inline std::string encode_packing_record(io::Json envelope, const PublishedSnapshot& snapshot)
{
    const auto& copies = snapshot.native->solution->copies();
    auto placements = io::Json::array();
    for (const auto& pose : copies) {
        const auto transform = geometry::RigidTransform::make(pose.rotation_xyzw, pose.translation_mm);
        if (!transform) {
            throw std::runtime_error("Publication rigid pose unavailable.");
        }
        placements.push_back({
            { "copy_id",         pose.copy_id                },
            { "translation_mm",  pose.translation_mm         },
            { "quaternion_xyzw", pose.rotation_xyzw          },
            { "local_to_world",  transform->local_to_world() }
        });
    }
    envelope["packing"] = {
        { "validation_status", "valid"             },
        { "solution_revision", snapshot.revision   },
        { "count",             copies.size()       },
        { "placements",        std::move(placements) }
    };
    return envelope.dump() + '\n';
}
}  // namespace spectrapack::cli
