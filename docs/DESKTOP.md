# Desktop workflow

T-008 introduces the first Windows desktop journey. Its scope and verification
are tracked in [T-008](../doc/T-008.md). The C++ engine owns import acceptance,
packing, placement validation and export; the desktop presents those results.

## Pack an object

1. Choose the STL's units, then import it. Inspect dimensions and diagnostics.
   Invalid geometry cannot start a packing job. Import does not alter the file.
2. Enter the box's internal width, depth and height in millimeters. Pair and
   wall clearances are independent; both default to 1 mm and can be zero.
3. Choose fixed or cube rotations, pitch and search budget. This first journey
   runs on the CPU with one thread. Pitch controls the search representation;
   it never changes the object's physical size.
4. Start the job. Stop requests a safe termination and keeps the engine's
   validated result. Completion can follow the acknowledgement because a
   native operation may need to reach a safe boundary.
5. Inspect the complete result in the viewer. Selection, hiding and clipping
   affect the view only. The displayed count always belongs to the full result.

The budget currently applies to native search. Import and desktop preparation
are separate; this slice does not implement the full application budget policy.
Results are **best found**, including valid empty results. They do not prove
optimality or that a larger packing is impossible.

## Preserve and export a result

Save writes a portable `.spectrapack` archive containing source and accepted
geometry, import decisions, settings and the complete result when present.
Pending settings are saved separately from the settings that produced the
result. An accepted object can also be saved before its first solve.

Open verifies the archive and geometry and revalidates saved placements before
activating them. The archive can be moved to another directory. A failed Open
keeps the current result. Saving after replacing the object while retaining a
result for the old object is rejected; finish a new solve or start a new session.

JSON export includes the authoritative assets and pose/provenance data. STL
export uses full-resolution accepted geometry and includes a JSON companion
with copy identities and triangle ranges. The engine checks the actual rounded
STL coordinates. If rounding violates the geometry constraints, STL export
fails while valid JSON remains available; changing the view cannot fix it.

## Current boundaries

This journey uses box containers, fixed/cube rotations, explicit pitch and CPU
execution. STL containers, repair-acceptance controls, free/upright search,
Continue, live incumbent updates, periodic checkpoints, viewport screenshots
and full-resolution inspection controls remain tracked follow-up work.
Saving a complete project is distinct from recovering an interrupted search.

The local development checks are `pnpm desktop:check`, `pnpm desktop:test`,
`pnpm desktop:build` and `pnpm contracts:check`. `pnpm desktop:dev` starts the
frontend on port 1420; a browser preview alone cannot run the native workflow.
Native build/staging instructions and measured acceptance evidence are recorded
with the ticket. See [CPU build instructions](BUILDING.md) for the fixed engine.
