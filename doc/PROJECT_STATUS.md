# Project status

Updated 2026-10-27. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import, independent overlap/containment and pair/wall
  clearance validation, mixed-orientation packing, CPU FFT search, checked export.
- **Desktop:** settings survive STL replacement, including edits made while import
  runs. Start/Stop, live packing, portable Save/Open and export retain provenance.
- **Automatic packing envelopes:** every desktop import creates and accepts an
  envelope, including valid inputs. CGAL builds a cavity-free enclosing solid;
  native validation independently checks the solid and source enclosure. Exact-valid
  source solids remain available through CLI. Source-only preparation avoids a
  redundant topology analysis. Welding/reconstruction remain advanced tools.
- **Inspection:** Original, Packing envelope and Overlay views share the same
  poses, clipping and selection. Original bytes and cleaned source geometry remain
  separate from packing geometry through Save/Open. STL export retains the source
  and verifies enclosure after float32 quantization.
- **Packing refinement:** paired layers use relative sideways and height offsets;
  finite layouts screen only existing row/layer neighbors. Validator v7 resolves
  separated sloping boundaries after rounded rigid transforms using outward interval
  proofs. Spatial triangle bounds accelerate fresh validation; finalization has a
  bounded 30-second allowance to avoid load-dependent five-second save failures.
  Whole-arrangement axis sweeps compact validated layouts and refill freed space;
  native-validated contraction recovers diagonal slack missed by coarse bins.
  Equal-count compactness improvements now reach the desktop and saved result.
- **Interlocking layers:** timed searches try layered double lattices of a
  rotation and its flipped twin (FFT no-fit maps per rotation pair). The
  simplified Pryanik at 0/0 mm in 400 x 340 x 285 mm goes from 324 to **336**
  in 120 s (330 in 60 s): 14 slabs of 12 + 12 nested twins. Bunny: 45 (was 40).
- **Box tolerance:** boxes accept an allowed oversize (desktop default 0.1 mm,
  ADR 0022); results record the validated interior. Envelopes now use
  diagonal/2000 offset, so projects saved with older envelopes need re-import.
- **Bulba:** automatic envelope generation takes about 24 seconds, versus 54 with
  redundant source analysis, with byte-identical geometry. The unchanged search
  now saves **24 validated copies in 400 x 340 x 285 mm**, at 0/0 mm clearance,
  cube rotations, 2 mm pitch and 60 seconds. The expanded 408 x 348 x 293 mm box
  also returns 24; 32 remains unverified. A later diagonal-gap reproduction
  reached 30 in 410 x 350 x 290 mm but only 24 in 410 x 350 x 285 mm.
  General proposal contraction now saves **30 in the 285 mm box** within
  90 seconds (2 mm pitch, four threads, zero clearance, cube rotations).
  The desktop result occupies 281.605 mm in Z and survives Save/Open/export.
  Desktop controls now use cube rotations exclusively; old results keep their
  recorded orientation permissions. Evidence: `.local/bulba/`, `.local/diagonal/`.

Native and desktop checks cover independent validity, source preservation, recipe
replay, Stop, portable projects and export. Benchy/matrice qualification remains
under `.local/envelope/`; packing validity does not establish source printability.

Not yet: Vulkan, desktop STL containers, upright/free-3D search, crash recovery
or installer. These remain **Later** in spec v0.3.

## Scoreboard

Release, eight threads; unchanged 10-second analytic and 60-second STL budgets.
Validator v7 retains these counts; zero-clearance Pryanik reached 450 in an
isolated repeat after returning 447 alongside other test jobs.

| Case | Count | Floor | Target |
| --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 |
| lprism2 | 1 | 1 | 2 |
| lprism-gap (0.125/0.125 mm) | **2** | 2 | 2 |
| pryanik2 (1/1 mm) | **435** | 425 | 426 |
| pryanik2-0 (0/1 mm) | **450** | 416 | 416 |
| ulamok | 36 | 36 | 36 |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Conservative envelopes can close recesses/cavities and reduce nesting. They
   preserve source shape, but do not repair its defects or recover intended volume.
   Refused or indeterminate enclosure cannot enable packing. No shrinking to fit.
2. Reconstruction remains approximate and can erase features; its resolution is
   independent of packing pitch. Use the normal envelope action to retain source.
3. The exact-contact L-prism pair remains unfound; the positive-clearance case
   now reaches two. The spacing revision verified 60 bracket copies at 0/0 mm
   and 56 at 1/1 mm clearance in 400 x 340 x 285 mm. Its known 50-copy physical
   layout in the smaller box remains a target: zero-clearance packing improved from 36 to 42
   in its exterior-size upper-bound box. Compaction reduces occupied width from
   210 to about 186.5 mm while retaining 42. Actual interior dimensions are unknown.
   Final larger-box compaction rechecks were interrupted/refused under low RAM.
   Results remain `best_found`.
4. `10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post box is
   400 x 340 x 285 mm. Use box dimensions for that comparison.

Diagonal compaction is stacked on PR #29 (automatic envelopes), which includes #28
(compaction), which includes #27, #26, #25 and #24. See
[ADR 0021](../spec/decisions/0021-packing-envelopes.md), [ALGORITHM.md](../docs/ALGORITHM.md),
[BUILDING.md](../docs/BUILDING.md) and [AGENTS.md](../AGENTS.md).
