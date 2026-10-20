# 0021 — Conservative packing envelopes

Accepted 2026-10-19; default-import policy updated 2026-10-20. Supersedes the exact-source requirement only for certified object envelopes.

## Decision

Use CGAL 6.0.1 Alpha Wrapping with its supported Boost.Multiprecision arithmetic backend (`CGAL_DISABLE_GMP`). Pin the source archive by SHA-512 in `cmake/CGAL.cmake`; retain the existing vcpkg baseline and pin Boost 1.89.0 through the dependency lock. CGAL Alpha Wrapping is GPL-3.0-or-later (commercial licensing is also available upstream). Distributions must comply with that license; stage CGAL and Boost notices with the engine. This adds no online repair service.

The normal preparation action has no numeric settings. Build a candidate around the cleaned, unit-converted source using diagonal/60 alpha and diagonal/1000 offset, deterministic seed 0. Independently validate its closed solid, reject excluded cavity shells, then certify source surface enclosure. Desktop Import selects and accepts this certified envelope route for every object, with the policy disclosed before import. Explicit repair previews still require acceptance. CLI exact inspection remains available. Reconstruction and welding remain advanced operations that change export geometry.

Keep two meshes in the same source-to-local frame: the retained cleaned source and the full-resolution packing envelope. Search and the independent placement validator use the envelope. Viewer Original/Envelope/Overlay modes share exactly the same instance poses. Cavities and recesses may close, reducing nesting; this is visible and disclosed. Container interior semantics and pair/wall clearances are unchanged.

## Persistence and export

Recipe schema 3 identifies `cgal-alpha-wrap-envelope-v1`, CGAL version, seed, alpha, offset, role, units, source hash and both mesh hashes. `packing_envelope` in the asset report binds the source PLY and parameters; replay must match the recipe and both meshes. Metadata without its verified recipe is rejected. Original STL bytes remain retained and unchanged. New envelopes record `options.source_analysis: "surface_only"`: their source draft is deliberately indeterminate, with only exact cleanup and frame mapping performed. Replay uses that same preparation policy; older recipes without the marker retain full source inspection. Envelope validation and enclosure certification are unchanged.

STL export uses the cleaned source triangles, not the enclosing mesh. After checking the immutable staged STL against the requested transforms, validate separately quantized envelope copies and certify each staged source surface remains inside its cavity-free envelope. A failed enclosure, quantization, clearance or resource check rejects STL publication while retaining valid JSON. This establishes packing validity, not printability of a defective source.

## Consequences

Envelopes may be larger and close narrow openings; counts can decrease relative to a hypothetical exact repair. They never shrink source dimensions or requested clearances. Runtime refinement is bounded and cancellable; inability to construct or certify an envelope remains a diagnostic. Library generation is not itself evidence of validity.

References: [CGAL Alpha Wrapping](https://doc.cgal.org/6.0.1/Alpha_wrap_3/index.html), [CGAL licensing](https://www.cgal.org/license.html). Algorithm details belong in `docs/ALGORITHM.md`; local model qualification logs remain under `.local/envelope/`.
