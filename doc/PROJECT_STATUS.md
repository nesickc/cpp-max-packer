# Project status

Updated 2026-10-19. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import, independent overlap/containment and pair/wall
  clearance validation, mixed-orientation packing, CPU FFT search, checked export.
- **Desktop:** settings survive STL replacement, including edits made while import
  runs. Start/Stop, live packing, portable Save/Open and export retain provenance.
- **Automatic packing envelopes:** one action prepares defective object inputs,
  with explicit preview/acceptance. CGAL builds a cavity-free enclosing solid;
  native validation independently checks the solid and source enclosure. Exact-valid
  models keep their original path. Welding/reconstruction remain advanced tools.
- **Inspection:** Original, Packing envelope and Overlay views share the same
  poses, clipping and selection. Original bytes and cleaned source geometry remain
  separate from packing geometry through Save/Open. STL export retains the source
  and verifies enclosure after float32 quantization.
- **Packing refinement:** paired layers use relative sideways and height offsets;
  finite layouts screen only existing row/layer neighbors. Validator v6 resolves
  inexact shell witnesses without loosening containment or clearance. Spatial
  triangle bounds keep dense results within the fresh-validation save deadline.
- **Actual models:** Benchy and matrice both produce valid envelopes in about six
  seconds (11,030 and 7,486 triangles), pack one copy in a fitted test box, survive
  Save/Open and export their 225,102 and 336,126 source triangles. Evidence remains
  under `.local/envelope/`; this does not establish source printability.

Verified in Release/Debug native tests, desktop/contract/frontend checks and both
supplied-model journeys; the rebuilt viewer was checked in all three geometry views.
The replay test shares setup and has a measured Debug watchdog; details are in the PR.

Not yet: Vulkan, desktop STL containers, upright/free-3D search, crash recovery
or installer. These remain **Later** in spec v0.3.

## Scoreboard

Release, eight threads; unchanged 10-second analytic and 60-second STL budgets.

| Case | Count | Floor | Target |
| --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 |
| lprism2 | 1 | 1 | 2 |
| pryanik2 (1/1 mm) | **435** | 425 | 426 |
| pryanik2-0 (0/1 mm) | **447** | 416 | 416 |
| ulamok | 36 | 36 | 36 |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Conservative envelopes can close recesses/cavities and reduce nesting. They
   preserve source shape, but do not repair its defects or recover intended volume.
   Refused or indeterminate enclosure cannot enable packing. No shrinking to fit.
2. Reconstruction remains approximate and can erase features; its resolution is
   independent of packing pitch. Use the normal envelope action to retain source.
3. The L-prism pair remains unfound. The bracket reaches 50 copies at 1/1 mm
   clearance in 400 x 340 x 285 mm. Its known 50-copy physical layout in the
   smaller box remains a target; actual interior dimensions are unknown.
   Results remain `best_found`.
4. `10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post box is
   400 x 340 x 285 mm. Use box dimensions for that comparison.

Packing refinement is stacked on open PR #25 (envelopes), itself based on #24
(`feature/import-repair`). See
[ADR 0021](../spec/decisions/0021-packing-envelopes.md), [ALGORITHM.md](../docs/ALGORITHM.md),
[BUILDING.md](../docs/BUILDING.md) and [AGENTS.md](../AGENTS.md).
