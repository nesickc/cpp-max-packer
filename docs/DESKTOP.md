# Desktop workflow

T-008 introduces the first Windows desktop journey. Its scope and verification
are tracked in [T-008](../doc/T-008.md). The C++ engine owns import acceptance,
packing, placement validation and export; the desktop presents those results.

## Pack an object

1. Choose the STL's units, then import it. Inspect dimensions and diagnostics.
   Invalid geometry cannot start a packing job. Import does not alter the file.
2. Enter the box's internal width, depth and height in millimeters. Pair and
   wall clearances are independent; both default to 1 mm and can be zero.
3. Choose fixed or cube rotations, pitch, budget and CPU thread count from the
   engine's supported range (up to eight on Windows). New drafts default to at
   most four threads. Pitch controls the search representation;
   it never changes the object's physical size.
4. Start the job. Stop requests a safe termination and keeps the engine's
   validated result. Completion can follow the acknowledgement because a
   native operation may need to reach a safe boundary.
5. Inspect the complete result in the viewer. Selection, hiding and clipping
   affect the view only. The displayed count always belongs to the full result.

New drafts measure the budget from Start, including runtime preparation and
search; initial import, Open and export are separate operations. Restored results
retain their original budget scope and thread settings. [T-010](../doc/T-010.md)
records measured CPU scaling and the supported limits.
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

Desktop admission limits are 256 MiB per imported STL, 64 MiB for a result or
preview, and 2 GiB compressed/expanded per project with at most 1,024 entries.
Projects use ordinary ZIP32 stored/deflated entries; ZIP64 and encrypted
archives are rejected. These are size limits, not performance guarantees.

The local development checks are `pnpm desktop:check`, `pnpm desktop:test`,
`pnpm desktop:build` and `pnpm contracts:check`. `pnpm desktop:dev` starts the
frontend on port 1420; a browser preview alone cannot run the native workflow.
See [CPU build instructions](BUILDING.md) for the fixed engine. The native shell
uses Rust 1.98.1 with the Windows MSVC target and the committed Cargo lockfile.
With the Release engine built and frontend dependencies installed, run:

```powershell
./tools/desktop/Invoke-Desktop.ps1 -Mode Build
```

The wrapper stages the engine and its dependency notices, builds the frontend
and shell, then places the fixed engine beside
`desktop/src-tauri/target/release/spectrapack-desktop.exe`. Launch that executable
with its sibling engine and `engine-resources` directory present. It verifies
the engine's build-time hash; replacing that binary requires rebuilding the
shell. WebView2 must be installed. This is a local build, not a qualified installer.

License notices are generated during staging under `engine-resources/third-party/`.
The index links packages to complete license/NOTICE files, with identical texts
stored once. The generated bundle is shipped with the app and is not committed
to Git. Frontend notices follow production dependencies plus recorded build-generated
runtime code (Vite's preload helpers use its core license); Rust notices retain the
Windows normal/build closure to cover potential generated code. Existing C++
dependency notices stay in `engine-resources/share/licenses/`. Keep installed
locked frontend dependencies available when staging. Cargo uses the selected
toolchain and downloads missing locked sources during build preparation as needed;
this adds no runtime network access.

`-Mode Dev` starts the Tauri development window after staging. Test mode with
`-Mode Test -TestStlPath <file>` runs the Rust core integration suite against an actual
10 mm analytic cube and native engine. `-EngineBuildDirectory` selects an
existing native build; `-DebugBuild` selects a Debug shell. Measured acceptance
evidence and remaining qualification are recorded in [the ticket](../doc/T-008.md).
