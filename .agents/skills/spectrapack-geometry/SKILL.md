---
name: spectrapack-geometry
description: Implement or review SpectraPack STL import, coordinate transforms, authoritative solids, containment, clearance, or display LOD isolation.
---

# Geometry work

From the repository root, use `./tools/Read-Spec.ps1 -Section 5 -Id GEO-01,GEO-02,GEO-03,GEO-04,GEO-05,GEO-06` for the contract; narrow the sections when the brief already identifies them. The canonical source is [spec §5](../../../doc/spectrapack-spec.md#5-geometry-contract). Read only relevant AT-03–AT-09 rows with `-Id`.

Before implementation, identify source/accepted-solid ownership and the exact frame of each input/output. Keep original bytes and accepted repairs distinct. Preserve cavities and rigid multi-component assets. Watertightness alone does not settle valid solid semantics.

- Frames (§5.2): millimeters, right-handed Z-up; `p_local = s*p_source - a`, then `p_world = R(q)*p_local + t`. XYZW active quaternions; matrices operate on column vectors. Unit conversion is recorded, never scaling to fit.
- Validation (§5.3): accept only `valid`. Surface intersection/distance alone misses full enclosure and coincidence. STL containment requires solid difference from the permitted volume, including excluded cavities; vertex inclusion is sufficient only for an analytic convex box.
- Clearance: pair and wall values are separate Euclidean distances. Tolerance classifies uncertainty; it cannot reduce the requested gap or permit positive overlap. Resolve borderline cases robustly or return `indeterminate`.
- Representation (§5.1): display LOD changes cannot alter authoritative hashes, voxel fields, validation, or export. Voxelize authoritative geometry; ordinary simplification is not an enclosing proxy.

Verify the affected invariant with analytic/adversarial cases, including the failure mode a surface-only or voxel-only test would miss. Report unresolved kernel behavior to the primary; the Manifold/FCL adapter is gated by M1, not presumed correct from library import success.
