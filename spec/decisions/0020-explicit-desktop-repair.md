# ADR 0020: Explicit desktop repair candidates

Accepted for implementation, 2026-10-18.

Use the already locked Manifold 3.2.1 dependency for an explicitly approximate
closed-surface reconstruction candidate alongside deterministic tolerance
welding. Rasterize source boundaries with half-open scanline parity; nested shells
alternate material and disconnected regions remain in one candidate asset.
Reconstruct the trilinearly interpolated +/-1 field with Manifold LevelSet.
The fixed isovalue 0.01 avoids exact binary-field saddles; simplify at
resolution * 1e-6 before independent full-resolution analysis. All parameters
are retained. No hull, fit scaling or automatic replacement is allowed.

Admission caps the grid at one million samples, scanline tests at fifty million
and output at one million triangles. Open boundaries and unmatched scanlines are rejected. Unresolved topology,
intersections or indeterminate analysis block acceptance. These are proposals
for a new solid, not proof of the source's intended shape. Sub-resolution
features, components and cavities may be lost; this limitation is explicit in
the acceptance UI. The independent full-resolution analysis remains the gate.

The resolution (or weld tolerance) is an explicit repair parameter in millimetres, independent of
packing pitch. Preserve the original source and source-to-local frame. Display
both meshes, actual bounds, surface areas, components and cavity counts (unknown
when source analysis is incomplete). Do not present a simplification tolerance
or vertex displacement as a certified surface bound.

Keep schema-1 weld recipes replayable. Reconstruction uses a schema-2 recipe
with algorithm identifier `manifold-even-odd-level-set-v1`, library version `3.2.1`,
resolution, isovalue, simplification tolerance, original source hash, before/after mesh hashes and diagnostics.
The accepted asset retains the content-addressed recipe and explicit acceptance.
Loading replays only the recorded algorithm/version and checks exact canonical
recipe and mesh hashes; unknown recipes fail rather than using a newer algorithm.
Original weld projects do not enter the reconstruction path. Project ZIP and
accepted float64 PLY formats remain unchanged.

Desktop import and repair replace asset-dependent state transactionally.
Settings are owned by the editor until Start/Save; import and repair do not read
or rewrite them. New clears the project; Open restores its saved settings.
Failed proposals and rejected candidates retain the previous object. An invalid
source or preview cannot start a packing job. Accept requires the exact current
proposal token and prepares the accepted geometry before publication.

Supplied Benchy succeeds at 0.5 mm through the real desktop/native acceptance,
packing, Save/Open and checked STL export path. Tested matrice reconstructions
remain invalid and blocked. These observations do not guarantee other resolutions
or source meshes; library construction success alone makes no validity claim.
