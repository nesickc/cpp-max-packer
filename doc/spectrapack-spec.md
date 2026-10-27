# SpectraPack — application specification

**Version:** 0.3 • **Date:** 18 October 2026 • **Status:** development baseline; implementation status is tracked separately

This document specifies a local Windows application that packs as many rigid copies of one STL model as it can find into a fixed container, displays the packing, and exports the count and placements. The algorithm is based on spectral packing and the psacking reference implementation. Version 1.0 (**Core**) is a C++ CPU engine, analytic box containers, mesh diagnostics, a desktop UI and a fixed quality scoreboard. AMD-compatible GPU acceleration, STL containers and crash recovery are **Later** (v1.x).

This is a development specification, not an implementation or a claim of measured performance. It fixes what the product must do and the validity contract. It does not fix how the search works: algorithm parameters and strategies live in [docs/ALGORITHM.md](../docs/ALGORITHM.md) and may change whenever the scoreboard improves. “MUST” is a requirement for the tier it belongs to; “SHOULD” permits a documented exception.

Version 0.3 splits requirements into Core and Later tiers, moves algorithm details out of §6, replaces the §6.6 accounting policy with a simpler degrade-and-continue rule, makes the §9 scoreboard the quality gate and narrows the §11.3 change procedure. Requirement, AT and M0–M6 identifiers are preserved; protocol, settings, result and project schema versions are unchanged. Earlier rationale is in `spec/decisions/` and the frozen `doc/T-*.md` tickets; see [current status](PROJECT_STATUS.md).

## 1. Product decision

Build a **C++20 packing engine with a spectral placement kernel, a maximum-count search controller, and an independent solid validator**. Use **pocketfft on CPU** and **VkFFT through Vulkan on GPU**. Present it through **Tauri 2, React/TypeScript, and Three.js**. The small Rust shell handles desktop integration and communication; all geometry and packing decisions remain in C++.

| Decision | Reason |
| --- | --- |
| Port the spectral method and consult psacking source | psacking is a useful algorithm reference, but its current build includes CUDA. It is not a ready Windows/AMD backend. |
| Keep a complete CPU implementation | Enables development, automated tests, and operation without a compatible compute GPU. |
| Add Vulkan acceleration | VkFFT documents Windows and AMD support. The application selects its Vulkan backend only. |
| Optimize count directly | Greedy placement alone can leave arrangements that prevent an additional copy. Rearrangement and restarts are required. |
| Preserve a full-resolution authoritative solid | Display simplification and voxel resolution must not redefine the object being packed. |
| Represent boxes analytically | Avoid unnecessary triangle processing for the main use case. STL containers use a separate containment implementation. |

The paper uses voxel geometry and FFT-based correlation to search translations across sampled orientations. psacking is an unofficial implementation with a restricted orientation catalog. The count controller, broader rotation search, portable compute backends, application UI, and validation contract below are proposed additions. [Spectral packing paper](https://doi.org/10.1145/3592126), [psacking](https://github.com/Vrroom/psacking), [psacking build](https://github.com/Vrroom/psacking/blob/main/CMakeLists.txt).

## 2. Scope and interpretation

### 2.1 Required user journey

1. Import an object STL, choose its units, and inspect its dimensions and mesh diagnostics.
2. Enter a box's **internal** dimensions, or import an STL that encloses the permitted packing volume.
3. Set part clearance, wall clearance, allowed rotations, and an effort preset or time budget.
4. Inspect the resolved resolution, compute device, and memory estimate; start packing.
5. Watch the best validated count and inspect its arrangement in a 3D viewer.
6. Stop with the best result retained, or continue searching from it.
7. Save the project and export placements and a packed STL assembly.

### 2.2 Geometric meaning

- **Object:** a bounded, positive-volume solid, potentially concave, with multiple components or modeled cavities. All components form one rigid copy.
- **Container:** the closed set of positions occupied by the usable interior volume. A container STL describes this volume's boundary. An open shell or a mesh of the physical walls requires conversion to an interior-volume model before packing.
- **Allowed movement:** translation and proper rotation only. Copies retain their dimensions. Unit conversion is not a packing degree of freedom; reflection, deformation, and scaling to fit are excluded.
- **Clearance:** minimum Euclidean distance in millimeters, specified separately between copies and between a copy and the container boundary. Zero permits boundary contact but never interior overlap.
- **Best count:** the largest validated count found under the selected constraints and computation budget. Failing to place another copy does not establish impossibility.

“Any STL” means format support for ASCII and binary STL and geometry support for valid solids of arbitrary shape within the resource budget. Missing faces, self-intersections, ambiguous shells, non-finite coordinates, and numerically unusable inputs require diagnostics or an explicitly accepted repair. The application cannot infer every malformed mesh's intended solid.

The solution is **geometric packing**. v1 does not certify gravity stability, support-free printing, insertion through an opening, or the ability to separate interlocked parts. The paper's interlocking work does not by itself provide all these physical guarantees. The results view states this scope once, with an explanation available beside it. [Paper](https://doi.org/10.1145/3592126).

### 2.3 Core v1.0 and Later work

**Core (v1.0)** is every requirement marked Core in §3: a usable Windows desktop application and CLI that imports an STL, packs it into a box on the CPU with independently validated placements, shows the result, and saves/exports it. Packing quality is measured on the §9 scoreboard.

**Later (v1.x)** covers requirements marked Later: Vulkan/AMD acceleration and its qualification, STL containers, upright/free-3D rotation search, checkpoint recovery after a crash, fault-injection suites, deterministic fixed-work replay and Windows 10 qualification. Their text stays in this spec as the design target; they do not block v1.0.

Beyond v1.x: multiple distinct object types, quantities or values per type, physical assembly constraints, distributed search, Linux/macOS installers, interactive manual placement editing and 3MF export.

## 3. Requirements and traceability

Each requirement is a MUST for its tier. Acceptance case identifiers refer to section 10. Detailed rules in subsequent sections refine these requirements. Where a row is split, the Later part is named explicitly.

| ID | Tier | Requirement | Acceptance |
| --- | --- | --- | --- |
| SYS-01 | Core | Ship a native Windows 11 x64 application and standalone CLI; use C++20 for the geometry and packing backend. Windows 10 22H2 qualification is Later. | AT-01 |
| SYS-02 | Core | Build and run the default distribution without CUDA, cuFFT, NVCC, HIP, or ROCm. End users need no compiler, Python, Node.js, or Vulkan SDK. | AT-01, AT-02 |
| SYS-03 | Core / Later | Core: CPU-only solving. Later: Vulkan solving on a qualified Windows AMD Radeon device, with device detection and recoverable fallback. | AT-02, AT-16 |
| SYS-04 | Core | Perform import, solving, viewing, project persistence, and export locally without network access. | AT-01, AT-15 |
| GEO-01 | Core | Import ASCII and binary STL, including files with multiple solid components; detect content without relying on the extension or the word `solid` alone. | AT-03 |
| GEO-02 | Core | Require an explicit unit selection, preserve source coordinates through recorded transforms, and never resize to fit automatically. | AT-03, AT-14 |
| GEO-03 | Core | Diagnose mesh validity, apply safe cleanup, preserve the original file, and offer bounded tolerance welding and library-backed reconstruction as explicitly accepted repairs. Unsupported or ambiguous geometry remains rejected. | AT-04 |
| GEO-04 | Core | Maintain distinct authoritative geometry, display LODs, and search representations. Simplification must not weaken final acceptance. | AT-05, AT-06 |
| GEO-05 | Core / Later | Core: analytic boxes. Later: closed STL interior volumes, including concave containers and excluded cavities. | AT-07, AT-08 |
| GEO-06 | Core | Validate original-solid overlap, containment, and both clearances, including zero-clearance contact, within the job budget; use a documented numerical tolerance independent of voxel pitch. | AT-06, AT-07, AT-09 |
| SOL-01 | Core | Maximize identical-copy count within an immutable container and retain the best validated solution throughout a job. | AT-10, AT-11 |
| SOL-02 | Core | Implement spectral placement with correct linear correlation, physical coordinate mapping, and independent discrete checks. | AT-12 |
| SOL-03 | Core / Later | Core: fixed orientation, cube rotations and explicit quaternion catalogs. Later: upright yaw and sampled full 3D rotations. | AT-08 |
| SOL-04 | Core | Improve on the physical baseline with search (for example restarts, local refinement, removal/reinsertion); the methods are free and the scoreboard measures the result. | AT-11, AT-17 |
| SOL-05 | Core | Reuse identical-object preprocessing; estimate the dominant memory before allocating large fields and degrade (smaller batches, coarser pitch with notice) rather than end the search. | AT-05, AT-16 |
| SOL-06 | Core / Later | Core: stop-and-keep and continued search without ever exposing an invalid trial as the best packing. Later: recoverable checkpoints after a crash. | AT-13, AT-16 |
| SOL-07 | Core / Later | Core: record settings and seeds with every result. Later: a deterministic CPU mode with a fixed work budget. | AT-12, AT-17 |
| SOL-08 | Core | Provide bounded CPU multithreading with an explicit resolved thread count that improves the scoreboard over the single-thread path. | AT-12, AT-16, AT-17 |
| UI-01 | Core | Provide import, diagnostics, container editing, search controls, progress, results, project actions, and export in one desktop workflow. | AT-15 |
| UI-02 | Core | Display an interactive, correctly scaled packing with instanced copies, container transparency, clipping, selection, and dimensions. | AT-14, AT-15 |
| UI-03 | Core | Keep the UI responsive during preprocessing, solving, stopping, and export; show the actual backend and resolution. | AT-15, AT-16 |
| DATA-01 | Core | Provide a versioned C++ CLI and local request/event protocol with structured errors. | AT-13, AT-14 |
| DATA-02 | Core / Later | Core: save portable projects containing source assets, geometry decisions, settings, placements, and provenance. Later: recover the last complete checkpoint after interruption. | AT-13, AT-14 |
| DATA-03 | Core | Export an authoritative JSON result and optional full-resolution binary STL assembly; validate the exported coordinates. | AT-14 |
| QA-01 | Core | Ship analytic correctness fixtures, geometry regressions and the §9 scoreboard with repeatable commands. A pinned Benchy fixture is Later. | AT-06–AT-12, AT-17 |
| QA-02 | Later | Release against Windows CPU tests and an actual Windows AMD GPU test run, with recorded build, driver, and hardware information. | AT-01, AT-02, AT-17 |

## 4. Architecture and dependencies

### 4.1 Components

| Component | Responsibility | Boundary |
| --- | --- | --- |
| `pack_geometry` | STL parsing, units, topology, authoritative solids, BVHs, voxelization, distances, validation, display simplification | C++ library; no UI or FFT dependency |
| `pack_compute` | FFT plans, correlation, score reduction, device capabilities, memory accounting | C++ interface with CPU and Vulkan implementations |
| `pack_solver` | Orientation catalogs, greedy placement, incumbents, neighborhood search, budgets, checkpoints | C++ library depending on geometry and compute interfaces |
| `pack_io` | Versioned settings/results, project archives, mesh and assembly export | C++ library; no search policy |
| `spectrapack-engine` | Headless commands and a persistent stdio service; job scheduling | C++ executable; one active solver job per process in v1 |
| Desktop shell | File dialogs, sidecar lifecycle, scoped file access, window management | Tauri/Rust; no geometry or optimization logic |
| Desktop frontend | Controls, state presentation, 3D inspection | React/TypeScript + Three.js; no authoritative feasibility decisions |

The engine can run entirely without the desktop application. The viewer receives a shared display mesh and per-copy transforms; it does not require a separate mesh allocation for every instance. Three.js supplies an instanced mesh abstraction. [Three.js InstancedMesh](https://threejs.org/docs/pages/InstancedMesh.html).

### 4.2 Selected starting stack

| Area | Selection | Integration rule |
| --- | --- | --- |
| Native build | MSVC, CMake presets, vcpkg manifest/baseline | Pin dependencies and toolchain versions in the first build milestone. |
| CPU FFT | pocketfft C++ | Use multidimensional real transforms where practical; normalize inverse transforms explicitly. |
| GPU FFT | VkFFT, Vulkan backend | Compile with `VKFFT_BACKEND=0`; bundle required shader compilation support. |
| Solid operations | Manifold using its double-precision mesh interface | Boolean operations support containment and overlap classification; validate inputs first. |
| Proximity | FCL using double precision | BVH broad phase and mesh distance/collision queries; surface queries alone are insufficient for solid validity. |
| Display simplification | meshoptimizer | Error-limited LOD generation, with the authoritative mesh retained. |
| Desktop | Tauri 2, React, TypeScript, Three.js | Bundle the C++ sidecar and all UI assets. |
| Contracts/testing | JSON Schema, nlohmann/json, a C++ test framework, frontend integration tests | Select exact versions at M0; commit lockfiles and schema fixtures. |

pocketfft provides multidimensional C++ transforms under BSD-3-Clause. VkFFT documents Windows, AMD GPUs, and a Vulkan integration using glslang; neither CUDA nor HIP is needed for that backend. Shader/device compatibility still requires testing on the target Radeon card. [pocketfft](https://github.com/mreineck/pocketfft), [VkFFT](https://github.com/dtolm/VkFFT).

Manifold provides C++ solid booleans and MSVC support but requires suitable manifold input; it is not a general repair guarantee. Its API exposes `MeshGL64`. FCL provides collision and distance facilities. The application-specific validator composes these capabilities under the contract in section 5; passing a library import operation alone does not validate a solid. [Manifold](https://github.com/elalish/manifold), [Manifold API](https://github.com/elalish/manifold/blob/master/include/manifold/manifold.h), [FCL](https://github.com/flexible-collision-library/fcl).

At M0, record exact source revisions and third-party notices. Preserve psacking's attribution for any reused code; avoid inheriting its build wholesale. No package is permitted to pull in NVIDIA dependencies implicitly. The chosen geometry adapter must pass M1's adversarial cases before it becomes the production validation gate.

### 4.3 Windows distribution

- Support Windows 11 x64 (Core) and Windows 10 22H2 (Later). Windows 11 is the primary performance reference. GPU acceleration additionally requires a working compatible driver on the selected OS.
- Provide an installer and a standalone engine package. Bundle native runtimes and shader compiler components as needed. Load Vulkan dynamically so CPU operation works without a Vulkan loader or device.
- Use WebView2 for the desktop shell and offer an offline installer variant containing its prerequisite installer. Users install normal AMD drivers, not development SDKs. Tauri documents sidecar bundling and offline WebView2 installation. [Sidecars](https://v2.tauri.app/develop/sidecar/), [Windows installers](https://v2.tauri.app/distribute/windows-installer/).
- Detect compute capabilities by feature queries and a startup FFT self-test, not GPU vendor name. Support Vulkan single precision without requiring device FP64 support. Geometry validation always uses the CPU double-precision path.
- Test on the user's exact Radeon model when available; publish the qualified model/driver list with each release. CPU fallback is required even on unqualified GPUs.

## 5. Geometry contract

### 5.1 Import and authoritative geometry

Import produces a diagnostic report containing file SHA-256, byte size, triangle/component counts, bounds, units, volume if meaningful, boundary/non-manifold edges, degeneracies, duplicate faces, shell orientation, and self-intersection findings. STL facet normals are hints; derive geometric normals from vertex order.

The initial unit selection defaults visibly to millimeters; imports must record that choice. Inches and custom positive scale-to-millimeters are supported. Display physical bounds before the first run. Recenter internally to improve numerical conditioning while retaining the exact source-to-local mapping.

Automatic cleanup may merge exactly equal vertices, remove exact duplicate faces and zero-area faces, and correct an unambiguous shell orientation without moving vertices. Record every change and revalidate. Near-vertex welding, hole filling, shell union, and topology-changing reconstruction are separate, explicit repair proposals with before/after previews. v1 must offer bounded tolerance welding and a bounded library-backed reconstruction route for the pinned simplified Pryanik repair case in T-012. Other meshes needing unsupported repair may be rejected with an explanation of what must be fixed externally. Never replace a missing cavity with solid material silently.

Reconstruction creates a proposed new authoritative solid; it is not permission to pack an invalid source using an approximate search field. Repair resolution is independent of packing pitch. Preserve original bytes and retain the repair algorithm/library version, parameters, source and repaired hashes, source-frame mapping and explicit acceptance. Revalidate the reconstructed solid before accepting it and use that same accepted geometry for validation, search and export. Preview changes in dimensions, surface, components and cavities; report volume only where meaningful, and distinguish sampled deviation from a certified bound. Changing packing pitch or display LOD cannot rerun or alter an accepted repair. T-012 must demonstrate the repaired practical fixture through Save/Open, packing and export; a welding UI or an external-repair instruction alone does not complete that case. [ADR 0011](../spec/decisions/0011-cpu-first-follow-up.md) records the scope extension; library selection and project/report compatibility require the implementation decision before edits.

Desktop object import automatically creates and accepts a certified **packing envelope**, including for valid sources. Disclose this route beside Import: it retains the original cleaned source for display/export, while closed recesses may reduce nesting. Offer Original/Envelope/Overlay inspection after import. Source preparation may stop at finite, unit-converted, exactly cleaned surfaces; such a draft remains indeterminate and cannot itself authorize packing. Validate the envelope as a solid, reject excluded cavity shells, and independently certify every retained source triangle (including its interior) lies strictly inside it. Failed certification cannot enable packing. Containers retain exact interior-volume semantics. Explicit CLI inspection can still select the exact source or preview an envelope. [ADR 0021](../spec/decisions/0021-packing-envelopes.md) defines the dependency and persisted distinction.

Require closed, consistently oriented, non-self-intersecting shell geometry with an unambiguous inside. Nested boundaries represent cavities through a recorded shell containment tree and alternating orientation; disjoint components remain one rigid asset. Intersecting or touching shells with ambiguous volume semantics require repair or rejection. Do not treat “watertight” as sufficient evidence of validity.

Keep three representations:

| Representation | Purpose | Authority |
| --- | --- | --- |
| Source file and accepted full-resolution packing solid | Validation; source or accepted repair for export | Exact solid or explicitly accepted enclosing envelope; original source stays distinct |
| Display LOD | Responsive viewer | Visual approximation only |
| Conservative voxel fields | Candidate generation and fast rejection | Search approximation; cannot authorize a packing |

Use meshoptimizer to target, initially, 20,000 display triangles per shared object with a displayed error budget of 0.1 mm; allow a higher count if the budget prevents further reduction. Its simplifier reports an approximate error and can stop before reaching a target count. This is not a conservative containment guarantee. [meshoptimizer simplification](https://github.com/zeux/meshoptimizer#simplification).

Search speed in v1 comes primarily from voxel resolution, BVHs, and repeated-shape caching. Voxelize the authoritative solid directly. A later simplified search mesh may replace it only with a verified enclosing representation or an independently bounded outward offset. Ordinary decimation followed by voxelization is insufficient. Display LOD changes must not change the candidate fields, validator, or exported triangles.

### 5.2 Frames and transforms

Use a right-handed world frame with Z up and millimeters throughout the engine. A box occupies `[0,W] × [0,D] × [0,H]`. A box may carry a size tolerance `t ≥ 0` (default 0.1 mm in the desktop, absent means 0): real boxes are made to a dimensional tolerance, so the validated interior is `[0,W+t] × [0,D+t] × [0,H+t]` and results record that interior beside the requested dimensions and tolerance. Preserve an STL container's original axes and translate its bounding-box minimum to the world origin after unit conversion; record that mapping. Do not rotate the container automatically.

For the object, let `s` be the selected source-unit scale and `a` the source bounding-box center expressed in millimeters:

`p_local_mm = s * p_source - a`

`p_world_mm = R(q) * p_local_mm + t_mm`

Store quaternions as `[x,y,z,w]`, normalized, with a deterministic sign convention. They encode active rotations applied to column vectors. JSON matrices are nested row-major 4×4 arrays operating on column vectors. Thus `source_to_world = local_to_world * source_to_local`. Export both the source mapping and placement transforms; no consumer should need voxel pitch to recover a pose. All packing transforms have unit scale and determinant +1 within numerical tolerance.

Canonical quaternion sign: make `w` positive; if `w` is exactly zero, make the first nonzero component in X/Y/Z positive. Serialize doubles with round-trip precision. Compare orientation permissions using angular distance, initially with a 1e-7 rad numerical tolerance; this does not authorize a meaningful extra rotation outside a fixed/catalog mode.

### 5.3 Validation

`validate(solution, authoritative_assets, constraints)` is independent of the search algorithm and returns `valid`, `invalid`, or `indeterminate`, with affected copy IDs and diagnostics. Only `valid` may become an incumbent or be labeled a validated export.

1. Check finite rigid transforms, asset hashes, units, allowed orientations, and the unchanged container/clearance constraints.
2. Use enlarged world AABBs to eliminate distant copy pairs. Perform accepted packing-solid checks on remaining pairs. Combine surface intersection/distance queries with solid boolean or containment classification so fully enclosed objects and coincident solids are detected.
3. For boxes, check all transformed authoritative vertices against the six inward-offset planes. Convexity makes these vertex tests sufficient for containment of triangular solids in a box.
4. For STL containers, require the object solid's difference from the permitted container volume to be empty, then check the required distance to its boundary. Vertex inclusion alone cannot detect a triangle crossing a concavity, or an object enclosing an excluded cavity.
5. Reject any positive interior overlap. For positive clearances, also reject an insufficient surface gap. Do not accept a small overlap merely because its volume is below a chosen threshold.
6. Treat unresolved classification near numerical degeneracies as `indeterminate`; retry with the adapter's robust path or reject that candidate. Kernel disagreement cannot produce `valid` by majority vote.

Geometry computations use double precision and documented scale conditioning. Initial validation tolerance is `epsilon_mm = max(1e-6, 1e-9 * D)`, where `D` is the larger object/container bounding-box diagonal in mm after unit conversion. It is a classification tolerance, **not permission to subtract epsilon from requested clearance**. For positive clearance, a result whose numerical uncertainty crosses the threshold is indeterminate unless resolved; the search should leave a small margin. Exact axis-aligned contact fixtures at zero clearance must be accepted. Features the kernel cannot resolve at this tolerance cause a diagnostic rather than deletion.

Validation may be incremental during search: reuse checks only for unchanged geometry, constraints, and poses, and validate every changed copy against all relevant neighbors and the container. A complete final/export pass bypasses search-derived certificates. Avoid recomputing expensive solid booleans for pairs already separated by a sound bounding-volume test.

For the bounded T015-P1 stagger path, [ADR 0018](../spec/decisions/0018-cardinal-support-stagger.md)
permits a validation-local outward support-interval certificate over all accepted
vertices for exact cardinal poses. A positive projected gap must independently
prove the requested Euclidean pair clearance with guarded interval arithmetic.
The table, work and scratch remain within existing budgets; inconclusive cases
retain the existing validator path. Final/export validation rebuilds this
authoritative-geometry certificate independently of search state.

Report the tolerance, kernel revision, and test method with results. “Validated” means passing this numerical contract; it is not a formal real-arithmetic proof. The implementation must not invent certified error bounds for a third-party operation that supplies none: uncertain cases stay indeterminate, and the M1 tests gate the chosen adapter.

## 6. Packing algorithm

### 6.1 Objective and incumbent

For object solid `S`, container `C`, and poses `T_i`, maximize integer `N` subject to containment, disjoint interiors, allowed rotations, and the requested distances. `C` and object scale stay fixed.

Compare layouts lexicographically: larger validated count first, then smaller occupied Z extent, then a deterministic compactness/tie-break score. Secondary scores guide search; they never compensate for losing a copy in the reported incumbent. Internal trials may temporarily contain fewer copies. Never publish them as the best result.

The desktop shows the first complete valid nonempty packing, then replaces it with a higher validated count or a same-count improvement in occupied Z extent, then XY span sum. Native validation and ranking authorize each replacement; revisions and event sequences increase. Save/export use the displayed poses.

Compute utilization as `N * volume(S) / volume(C)` from validated solids, not the occupied cluster's bounding box or voxel count. A hollow object's volume excludes its modeled voids. If volume cannot be established reliably, omit utilization. Do not label a failed insertion as an optimum. An optional volume upper bound is displayable only when conservatively bounded volume calculations support it; otherwise omit the bound.

### 6.2 Baseline before advanced search

For a box, start from deterministic physical layouts of oriented object AABBs for the allowed orientations, validated with authoritative geometry. For dimensions `b_x,b_y,b_z`, pair gap `c_p` and wall gap `c_w`, a uniform grid fits `n_j = max(0, floor((L_j - 2*c_w + c_p) / (b_j + c_p)))` copies along axis `j`. Block layouts that fill leftover space with other orientations are encouraged; the scoreboard reports the best simple block layout as a reference. Account conservatively for numerical uncertainty at exact fits. The baseline works in physical coordinates, so a coarse voxel grid cannot prevent an obvious box tiling. Preserve the best baseline as the starting incumbent.

For an STL container (Later), create the same enclosing-box candidates, filter each through true containment, and retain a validated subset. A bounding box by itself never defines feasibility for an STL container.

### 6.3 Search invariants

The search design is in [docs/ALGORITHM.md](../docs/ALGORITHM.md) and is tunable. Whatever it does, these invariants hold:

- Search representations (voxel fields, FFT scores, caches) only **propose** poses. Every accepted pose passes the authoritative validator of §5.3; floating-point thresholds affect proposal quality only and never establish feasibility or impossibility.
- FFT correlation is linear, with a documented reversal/indexing convention, padding to at least `environment_size + kernel_size - 1` per axis, and no circular aliases creating apparent free placements. It is tested against a direct integer correlation oracle (AT-12).
- Search fields use one physical pitch per level and are derived from the authoritative solid, conservatively: uncertain cells count as occupied. Overlapping clearance regions of placed copies are reference-counted so removing one copy cannot clear another's blocked cells.
- Off-grid poses are never silently rounded back to the grid.

### 6.4 Orientations and refinement

| Mode | Meaning |
| --- | --- |
| Fixed | One supplied quaternion; identity by default |
| Upright | Source +Z remains world +Z; yaw sampled initially every 15 degrees, with continuous yaw refinement |
| Cube rotations | The 24 proper cube symmetries; no reflection or arbitrary tilted refinement |
| Free 3D | A finite catalog covering SO(3), followed by local continuous rotation refinement |
| Custom catalog | User-provided quaternions; discrete by default; reject invalid entries and deduplicate equivalents |

Catalog construction (Halton sampling, ordering, deduplication) and refinement step schedules are in [docs/ALGORITHM.md](../docs/ALGORITHM.md). Sampling is not exhaustive coverage of continuous rotations. Fixed and discrete catalogs cannot drift into forbidden orientations.

### 6.5 Search budget and termination

The controller's strategy is in [docs/ALGORITHM.md](../docs/ALGORITHM.md). Its externally visible rules:

- One monotonic budget begins at Start and includes job-specific loading, preparation, search and validation. At expiry, begin no new search work, return the best validated incumbent after bounded cleanup and report any overrun.
- The best published count never decreases within a job, and continuing a job cannot lower it. Internal trials may temporarily hold fewer copies; they are never published.
- Termination reasons are `budget_exhausted`, `user_stopped`, `search_stalled`, `resource_limit`, or `error`; none means globally optimal. `resource_limit` is reported only when no affordable search work remains (§6.6), not when one operation hits a local limit.
- Record seeds and resolved settings with each result. A deterministic fixed-work mode (Later) replaces time-dependent decisions with candidate/pass caps on the same build and thread configuration.

### 6.6 Compute and memory policy

CPU correctness and useful CPU multithreading precede Vulkan work. Resolve and display the CPU thread count; keep a single-thread path. Authoritative incumbent admission stays ordered and thread-safe.

Memory policy is deliberately simple:

- Before allocating large fields, estimate the dominant buffers (padded real/complex FFT fields times batch size, plus cached spectra) and choose a batch size that fits in a share of available RAM. A 512³ float field is 512 MiB; a full complex-float field is 1 GiB.
- On allocation failure or an exceeded estimate, shrink the batch, evict caches, or move to a coarser pitch, and keep searching from the incumbent. An explicitly chosen manual pitch is coarsened only with a visible notice in the result and UI.
- Report `resource_limit` only when no affordable work remains. Never shrink geometry, clearance, or the container to solve a memory problem, and never relabel a resource problem as malformed geometry.

Vulkan (Later): `Auto` selects a qualified device if its startup self-test and memory preflight succeed, otherwise CPU. Explicit `CPU` never initializes Vulkan. Explicit `Vulkan` with an unavailable device returns a clear error and offers a CPU retry. Device loss discards the unfinished trial and continues from the last validated incumbent on CPU.

## 7. Desktop behavior

### 7.1 Layout and controls

Use a single main window: configuration panel on the left, 3D viewer in the center, and a compact progress/result panel below or to the right. Advanced controls are collapsible. A first successful packing should require only import, container dimensions, and Start.

| Area | Required controls and feedback |
| --- | --- |
| Object | Import/replace STL; units; dimensions; triangle count; diagnostics; repair proposal preview/accept/reject; original/LOD viewer toggle |
| Container | Box or STL interior volume; internal W/D/H and allowed size tolerance; independent STL units; bounds; diagnostic status; container transparency |
| Constraints | Pair clearance and wall clearance in mm; desktop search uses the 24 right-angle cube rotations |
| Search | Fast/Balanced/Thorough; total run budget; Start; Stop and keep best; Continue; seed and maximum additional work in advanced controls |
| Resources | Auto/CPU/Vulkan; selected device; detected capability; resolved pitch; estimated RAM/VRAM; advanced pitch, thread, and memory limits |
| Results | Best validated count; elapsed and remaining budget; time since improvement; utilization if available; termination reason; geometry validation status |
| Project/export | New/Open/Save/Save As; export result JSON; export packed STL; export a viewport PNG; open output folder |

Constraints are immutable for a running job. Editing them creates pending settings for a new job and visibly separates previous results from those settings. A result is always labeled with the settings that produced it. Continue permits additional budget and changes to search effort/backend, but preserves object geometry, scale, container, clearances, and orientation permissions. Broader/narrower orientation permissions create a new job, optionally seeded with revalidated compatible poses.

Progress reports phases (`preparing`, `voxelizing`, `planning_fft`, `placing`, `improving`, `validating`, `saving`) and elapsed budget. Show progress within a bounded phase when meaningful; do not invent a percentage of “optimal packing found.” Best count never decreases on the active job's timeline.

### 7.2 Initial presets

| Preset | Default total job budget | Orientations when unrestricted | Target pitch before memory adjustment |
| --- | --- | --- | --- |
| Fast | 60 seconds | 24 cube rotations | Object longest dimension / 64 |
| Balanced, default | 600 seconds | Free 3D, 256 orientations | Object longest dimension / 96 |
| Thorough | 3,600 seconds | Free 3D, 2,048 orientations | Object longest dimension / 160 |

An explicitly selected orientation restriction overrides the preset's orientation choice. Show that Fast restricts rotations; changing it to Free 3D is permitted. The preset resolves to concrete settings stored in the job, so later preset changes cannot change old results. Clearance defaults are visibly **1.0 mm between copies and 1.0 mm to walls**, both editable down to zero. These are application defaults, not a manufacturing recommendation.

The job budget covers job-specific preparation, FFT planning, search, and validation from Start. Previously completed import diagnostics are reported separately. At the deadline, stop beginning new search work and return the latest validated incumbent after bounded cleanup; report any overrun. Heavy preprocessing may consume the budget before finding a nonempty solution, which must be stated explicitly. Benchmark reporting additionally includes a cold end-to-end time from file import through export.

### 7.3 Viewer

Provide orbit, pan, zoom, fit-to-container, reset view, orthographic/perspective toggle, axis indicator, and scale/grid. Draw the container as transparent surfaces plus edges. Provide a movable clipping plane to inspect interior copies. Allow copy selection, isolation/hiding for inspection, stable copy IDs, and translation/quaternion display. Hiding or clipping affects only the view and never the solution count.

Offer Original, Envelope and Overlay views using the same per-copy transforms, selection, hiding and clipping. For exact assets the full accepted mesh is the packing boundary; for envelope assets show the retained source and the full envelope separately. These controls never change acceptance or search.

Render a shared object mesh with instance transforms. Use display LODs during interaction; offer full-resolution inspection of a selected copy. Do not load `N` full meshes into GPU memory. The visible best packing must correspond to one complete validated solution revision. A separately enabled candidate/debug preview is clearly identified as unvalidated and cannot be exported as the best result.

UI events run independently of engine computation. Throttle progress/preview delivery to at most four updates per second and coalesce obsolete preview revisions. Keyboard focus, resize, window movement, and Stop remain usable during long native work. Missing graphics acceleration may degrade rendering quality, but CPU solving and result export remain available.

For the count-only live desktop path, ignore lower/equal-count, invalid, duplicate,
stale-operation and out-of-order packing offers. Coalesce to the latest higher
count in bounded storage, including a final higher-count flush within the same
4 Hz ceiling. Progress/elapsed/Stop may update independently. Reuse the shared
mesh and atomically replace instance transforms without camera reset or rendering
continuously solely because search is active. A final result with equal count
preserves the displayed canonical poses and their Save/export association.

## 8. Interfaces, persistence, and export

### 8.1 CLI and service

Required command surface, with detailed flags specified in the generated CLI help:

```text
spectrapack-engine capabilities --json
spectrapack-engine inspect --stl <path> --units mm --report <path>
spectrapack-engine pack --job <job.json> --output <directory>
spectrapack-engine validate --result <result.json> --project <project> --report <path>
spectrapack-engine benchmark --suite <suite.json> --output <directory>
spectrapack-engine serve --stdio
```

The T-007 CPU implementation slice also provides a synchronous diagnostic entry
point, `spectrapack-engine solve --settings FILE --object-report FILE
[--container-report FILE] --result FILE [--stl FILE]`, specified in
[ADR 0009](../spec/decisions/0009-cpu-spectral-placement-and-export.md). It consumes
resolved settings and existing accepted inspection reports. The required `pack`
job-file interface and asynchronous job methods remain their own integration
gates. Capabilities distinguish implemented commands from implemented methods.

The T-008 first desktop journey uses a Rust-owned asynchronous adapter around
these auxiliary CLI operations, as specified in
[ADR 0010](../spec/decisions/0010-desktop-workflow.md). Its additive
`desktop-prepare` and `desktop-restore` commands resolve supported CPU settings
and independently validate restored results; `solve --stop-file FILE` requests
safe-boundary stop. The desktop DTO/project manifest is a separate versioned
schema. This bounded box-container journey does not replace the required stdio
job lifecycle, Continue, or periodic checkpoint acceptance gates below.

Headless `pack` and UI `job.start` use the same settings schema, solver, and output contract. CLI Ctrl+C maps to stop-and-keep. Exit codes: `0` for a valid terminal result, including zero copies, budget exhaustion, or a requested stop; `2` for bad input/settings; `3` for unsupported capabilities/resource preflight; `4` for internal failure; `5` for failed or indeterminate validation in the standalone validator. A fatal error may also preserve a prior valid incumbent; the nonzero exit and error record remain.

Use UTF-8 newline-delimited JSON over the sidecar's stdin/stdout. stdout contains protocol records only; human-readable logs use stderr. Each record includes `protocol_version: 1`. Requests have a unique string `request_id`, a `method`, and `params`. Responses carry the request ID, `ok`, and either `result` or a structured `error`. Events carry `job_id`, a monotonically increasing `sequence`, `type`, and `payload`. Use a maximum record size of 1 MiB; transfer geometry through scoped local asset files, not JSON arrays of millions of vertices.

Required methods: `capabilities.get`, `asset.import`, `asset.accept_repair`, `job.preflight`, `job.start`, `job.stop`, `job.continue`, `job.status`, `project.open`, `project.save`, `result.validate`, and `result.export`. Long operations acknowledge a task/job ID promptly and subsequently emit events. Only one solver job runs per engine process in v1. Duplicate request IDs must not start duplicate jobs.

The desktop repair command proposes a method and millimetre parameter, or accepts/rejects the exact current proposal token. A pending proposal carries separate before/after preview handles; it cannot be packed. Import and repair preserve editor-owned settings, including unsaved and in-flight edits. Successful asset replacement clears old results; failure/cancellation retains the previous state. New deliberately restores defaults and Open restores saved settings. Reconstruction recipes are versioned separately from weld recipes; [ADR 0020](../spec/decisions/0020-explicit-desktop-repair.md) defines deterministic replay and compatibility.

Version-1 schema, framing, replay, null-ID transport errors, asset-handle grammar and compatibility rules are fixed in [ADR 0004](../spec/decisions/0004-versioned-contracts.md). A record limit counts bytes before LF, including an optional CR. Malformed complete records are recoverable; oversized records and nonempty EOF fragments close with exit 2. Requests with trusted envelopes retain correlation IDs; errors before a trustworthy envelope exists use `request_id: null`. The bounded per-session replay cache never evicts IDs; exhaustion closes with a nonrecoverable resource error. Unimplemented required methods return `METHOD_UNSUPPORTED` and must not fabricate successful work. The authoritative schemas and generated types implement these initial contracts; contract consistency cannot establish physical validity.

An asset import task completes with its asset ID, source/accepted hashes, frame and dimension metadata, diagnostic report, and a scoped shared preview-mesh reference. The shell grants access only to user-selected/project-owned files and launches the fixed bundled sidecar; protocol arguments are data, never shell command fragments.

Illustrative request after a successful object import; the asset ID is supplied by the running engine. This example is pretty-printed for reading; each wire record occupies one line:

```json
{
  "protocol_version": 1,
  "request_id": "request-42",
  "method": "job.start",
  "params": {
    "settings_version": 1,
    "object_asset_id": "asset-0123456789abcdef0123456789abcdef-1",
    "container": {
      "kind": "box",
      "dimensions_mm": [165.0, 165.0, 320.0]
    },
    "clearance_mm": { "pair": 1.0, "wall": 1.0 },
    "orientation": { "mode": "free", "catalog_size": 256 },
    "search": {
      "preset": "balanced-v1",
      "budget_seconds": 600,
      "seed": "42",
      "deterministic": false
    },
    "resolution": { "mode": "auto", "longest_object_axis_cells": 96 },
    "compute": { "backend": "auto" }
  }
}
```

An STL container substitutes `{"kind":"stl_volume","asset_id":"asset-0123456789abcdef0123456789abcdef-2"}`. These illustrative handles must be replaced by IDs from the running process. Imports carry independent units, which resolve before job creation. Seeds are decimal uint64 strings to avoid JavaScript integer precision loss.

Event types include `task_progress`, `job_state`, `best_solution`, `backend_changed`, `warning`, `error`, and `job_finished`. A `best_solution` event identifies an immutable revision and result file; it does not send an unvalidated mutable buffer. The frontend ignores stale events by job ID and revision. The shell reads records asynchronously and prevents progress output from blocking the solver.

Errors contain `code`, `message`, `details`, and `recoverable`. Stable initial codes include `INVALID_STL`, `INVALID_SOLID`, `AMBIGUOUS_CONTAINER`, `UNITS_REQUIRED`, `INVALID_SETTINGS`, `GEOMETRY_INDETERMINATE`, `MEMORY_LIMIT`, `GPU_UNAVAILABLE`, `GPU_DEVICE_LOST`, `JOB_BUSY`, `ASSET_MISMATCH`, `SCHEMA_UNSUPPORTED`, and `EXPORT_VALIDATION_FAILED`. Messages explain a concrete corrective action where one exists.

### 8.2 Job lifecycle

States are `created`, `preparing`, `running`, `stopping`, `finished`, and `failed`. `finished` includes a termination reason; it does not imply optimality. Stop is acknowledged without waiting for a long FFT or validation operation to complete. At a safe boundary, discard unfinished trials, flush the last validated checkpoint, and finish. A terminated process leaves the last complete checkpoint usable.

Continue creates a new run segment within the same logical job and adds budget. Record each segment's settings, backend, seed/RNG state, times, and parent solution. Exact continuation requires compatible engine/cache versions. Otherwise load the valid incumbent as a warm start and report that search state was reset. Do not imply bit-for-bit continuation after a backend change.

### 8.3 Authoritative result contract

Implement versioned JSON Schemas for settings, assets, results, protocol records, and benchmark summaries. Generate TypeScript types from those schemas; validate both CLI and service inputs against the same contracts. JSON contains no NaN/Infinity. Schema validation is followed by semantic validation of units, normalized quaternions, hashes, bounds, and array lengths.

| Result field | Required content |
| --- | --- |
| Identity | `schema_version`, `job_id`, `solution_revision`, creation time, engine version/commit |
| Assets | Source SHA-256, accepted-solid SHA-256, repair record reference, original unit scale, source-to-local mapping, portable asset path |
| Container | Validated box interior (requested dimensions plus tolerance, both kept in resolved settings) or container asset reference and its source-to-world transform; interior-volume semantics |
| Constraints | Pair/wall clearance, allowed orientation policy and catalog hash, unchanged physical dimensions |
| Search | Fully resolved settings, pitch levels, seed, work counts, run segments, backend/device/driver transitions, elapsed times |
| Packing | `count` and exactly that many `{copy_id, translation_mm, quaternion_xyzw, local_to_world}` records |
| Validation | `status`, tolerance, authoritative geometry hashes, validator/kernel version, pair/containment/clearance check summaries |
| Metrics | Solid/container volume and utilization when valid, time to best count, peak host/device memory, termination reason |
| Artifacts | Optional assembled STL, preview image, and benchmark/log paths with checksums |

The quaternion/translation pair is canonical. Matrices are derived and checked for consistency. Copy IDs remain stable across surviving copies and are never interpreted as placement order. `count` must match the placement array length; a valid empty packing is represented explicitly. A `best_found` label is required; `optimal` is not a v1 result status.

### 8.4 Portable project and checkpoints

Use `.spectrapack` as a versioned ZIP archive containing `project.json`, source STL assets, accepted full-resolution meshes, import/repair reports, resolved job settings, the best result, and optional checkpoint state. Store each asset once by content hash; asset paths inside the archive are relative. Preserve accepted geometry losslessly as indexed binary little-endian PLY with float64 XYZ and uint32 triangle indices; optional display GLB and search caches are disposable derived data. A source or accepted mesh change creates a new asset hash.

Checkpoints (Later): during a run, use a local working directory with immutable result revisions and atomic replacement of the checkpoint pointer. Save the project through a temporary archive and atomic rename. Keep the previous complete version until replacement succeeds. Checkpoint at each new best and at least every 30 seconds at a safe boundary; include RNG/search state where supported. Never serialize GPU buffers as the only recoverable state.

Opening verifies schema versions and asset checksums and revalidates restored placements before they become an active incumbent. Unsupported newer schemas produce a clear error. Moving the archive to another directory or Windows user profile cannot break internal references. Archive extraction rejects absolute paths, traversal, and excessive declared sizes. Native paths support spaces, Unicode, and long-path-capable Windows APIs.

### 8.5 Export

JSON placements plus the referenced authoritative asset are the primary output. Export the full-resolution accepted object transformed once per copy into a binary STL assembly in millimeters. For packing-envelope assets, export the retained cleaned source mesh instead: validate a separately quantized envelope at each pose and independently certify the actual staged source triangles remain inside it. The envelope must remain cavity-free. Source defects are retained; enclosure establishes packing safety, not printability or a repaired source volume. Do not export voxel cubes or a display LOD as the normal packed STL. Preserve per-copy identity in JSON because STL does not encode it or units reliably.

At export, verify the latest accepted geometry and constraints, stream triangles to avoid an `N × mesh_size` memory spike, and estimate output size before writing. Re-read the STL's actual quantized coordinates and check them under the same geometry contract. Binary STL stores finite-precision coordinates: if quantization violates clearance, fail the STL export while retaining the valid JSON/project, with advice to increase clearance. Do not silently nudge placements or relax constraints. The application may additionally emit one STL per copy using the same checks.

Exporting an assembly is not a boolean union operation. At zero clearance, touching copies may form a non-manifold combined surface; validate the per-copy solids using recorded triangle ranges, not by assuming the entire STL is one manifold solid. Include a small JSON companion with units, source hashes, and copy ranges. A screenshot export uses the current view and carries the count/settings in its companion metadata.

## 9. Scoreboard and quality goals

### 9.1 Fixture policy

Analytic fixtures are generated from checked-in parameters with exact dimensions; they expose containment, indexing and clearance errors that practical models cannot. Practical fixtures are the user-supplied `rc/` assets, pinned by hash in `tests/fixtures/rc-manifest.json` and never modified. A pinned Benchy fixture (official download, CC0, hash frozen on acquisition) is Later.

### 9.2 Scoreboard

`python tools/scoreboard.py` runs a fixed set of cases on the Release engine and prints count, floor, target, a simple block-layout reference, time to best, total time, validation status and termination reason. Each case has a **floor** (the count the current solver reliably reaches) and a **target** (a known layout or goal).

| Case | Object and container | Purpose |
| --- | --- | --- |
| `cube64` | 10 mm cube; 40×40×40 mm box; zero gaps; cube rotations | Exact tiling; must find 64. |
| `lprism2` | L of three 1 mm unit cubes; 3×2×1 mm box; zero gaps | Concave pair fills the box; an AABB grid fits one. |
| `pryanik2` | `rc/items/pryanik_2.STL`; 400×340×285 mm (Nova Post 10 kg box); pair/wall 1/1 mm; cube rotations; 4 mm pitch; 60 s | Main practical quality case. |
| `pryanik2-0` | As above with pair 0 mm, wall 1 mm | Touching copies, as packed by hand. |
| `ulamok` | `rc/items/ulamok_2kg_simplified.stl`; 400×350×285 mm; pair/wall 0.1/1 mm; 4 mm pitch; 60 s | Must retain at least 36. |

Rules: a change must not drop any case below its floor or produce a non-`valid` result. Raise a floor when an improvement is reliable. Add a case when a new user scenario matters; do not remove one to make a change pass. Record notable runs with `--record` in `benchmarks/SCOREBOARD.md`. Counts are `best_found`, never optimality claims.

### 9.3 Measurement

Timed runs on one machine vary; compare repeated runs before claiming an improvement of one or two copies. CPU and later Vulkan paths must satisfy the same geometry contract; bit-identical packings across devices are not required. Report GPU speedups separately for FFT work and end-to-end runs. Long benchmark suites (five-seed Benchy summaries, host-frozen baselines) are Later and run on release candidates, not on every change.

## 10. Acceptance tests

These are required implementation tests, not results obtained while writing this document. Test names and fixtures become permanent regression identifiers. Each test must check an observable contract, not merely repeat an internal function's implementation.

Tiers: AT-02 and the GPU parts of AT-12 and AT-16 are Later, as are AT-13's kill-during-checkpoint recovery, AT-15's AMD-host frame-time measurement and STL-container journey, AT-01's Windows 10 run and AT-17's Benchy suite. For Core, AT-17 is satisfied by the §9 scoreboard with every result validated.

| ID | Scenario and passing condition |
| --- | --- |
| AT-01 | On clean Windows 10 22H2 and Windows 11 x64 installations without NVIDIA tools, Python, Node.js, or a development SDK, install the offline package, import a fixture, solve on CPU, view it, save/open the project, and export JSON/STL with networking disabled. The standalone CLI completes the same job. Record loaded native dependencies. |
| AT-02 | On a recorded AMD Radeon/driver configuration, run Vulkan capability detection, FFT self-tests, and `benchy_small`. Validation passes. Force CPU and repeat. Simulate a missing Vulkan loader or unsuitable device: Auto selects CPU; explicit Vulkan returns `GPU_UNAVAILABLE`. |
| AT-03 | Import equivalent ASCII/binary STLs, including a binary header starting with `solid`, negative source coordinates, Unicode paths, and separate components. Equivalent physical models produce equivalent accepted bounds/volume. A 1-inch cube resolves to 25.4 mm, and source-to-world export reconstructs the same vertices. Truncated files and non-finite coordinates produce structured errors. |
| AT-04 | Exercise reversed normals, duplicate vertices/faces, a repairable small seam, an open surface, a non-manifold edge, a self-intersecting closed mesh, and ambiguous overlapping shells. Safe cleanup is reported. Tolerance welding requires accepted repair; source bytes stay unchanged. The pinned simplified Pryanik remains rejected as originally supplied; its library-reconstructed derivative must be explicitly accepted, independently valid, and retain repair provenance through project/packing/export. Unresolved invalid geometry cannot start a solver job. |
| AT-05 | Use a subdivided valid mesh with at least one million triangles and a thin-feature fixture. Changing display LOD does not change authoritative hashes, voxel fields, validation, or deterministic solver results. Geometry is shared across copies. If simplification cannot reach its target within the display error setting, report the actual count; do not force it. Large jobs either pass preflight or fail without exhausting system memory. |
| AT-06 | Validate separated, touching, intersecting, coincident, and wholly enclosed primitive solids, plus a solid in a legitimate modeled cavity. The validator distinguishes material overlap from empty space. A pose that passes a deliberately undersized proxy but intersects the authoritative solid fails. Both `invalid` and `indeterminate` are rejected by incumbent publication. |
| AT-07 | For boxes, test all six walls, fractional dimensions/pitch, and gaps at, above, and below the threshold. For a U-shaped STL volume, test a bridge whose vertices lie in the two arms while its middle lies outside. Also test an object that surrounds an excluded internal cavity without crossing its boundary. Both STL cases fail containment. Valid concave-container poses pass. |
| AT-08 | Run `tilted_bar`: cube rotations yield zero; an explicit 45° Z quaternion and Free 3D yield at least one validated copy. Upright mode never tilts +Z. Custom catalogs reject non-finite/zero quaternions, normalize valid inputs, deduplicate q/−q, and prevent continuous drift outside permitted rotations. |
| AT-09 | Check `cube_clearance` and pair distances at `c−10*epsilon`, `c`, and `c+10*epsilon`. The short gap fails; the larger gap passes; the exact analytic case resolves correctly. Cover near-coplanar faces, translated large coordinates after conditioning, and tiny features. Report uncertainty rather than widening the allowable gap tolerance. |
| AT-10 | `cube_exact` returns 64 with original-solid validation and export round-trip. `ulamok_box_36` establishes at least 36 through the physical baseline and a supported packing/export profile as defined in §9.2. A run given a model too large to fit returns a valid empty packing with `best_found` and a termination reason, not “proved impossible.” The best-count event sequence is monotone and utilization uses fixed container volume. |
| AT-11 | `interlocking_l_prisms` reaches two, exceeding its AABB baseline of one. For rearrangement, start with one L translated by +0.5 mm in X from its lower-left placement: a second cannot fit while it stays fixed. With fresh restarts disabled, a fixed 10,000 candidate-evaluation / 100-pass budget and 0.25 mm pitch, removal/reinsertion reaches the known two-copy layout. Log the operator used. Removing a copy whose clearance field overlaps another's does not erase the other's blocked cells. |
| AT-12 | Compare every translation of small asymmetric binary fields with a direct integer cross-correlation oracle, including non-power-of-two extents, nonzero kernel origins, rotations, and boundary placements. No circular aliases. CPU/GPU values on the small oracle suite differ from integers by less than 0.25 and round identically; candidate validity always uses discrete/solid checks. Inject bad normalization and an FFT error to verify rejection/fallback. Exercise supported CPU thread counts; fixed-work runs reproduce counts and transforms on the same build/configuration, including resolved thread count. |
| AT-13 | Start, query, stop, continue, and interrupt a job through CLI and service. Duplicate requests do not duplicate work. Kill the process during a trial and during checkpoint replacement; reopening preserves the last complete validated result. Drop/coalesce progress events and restart the UI without corrupting the engine's best state. Errors and terminal states match the protocol. |
| AT-14 | Save/move/open a project with repaired and unrepaired fixtures, nontrivial source origins, and rotations. JSON transforms, viewer transforms, and exported vertices agree. Result count matches placement records. Tampered hashes, inconsistent matrices, unsupported schemas, truncated archives, path traversal, and STL quantization violations are rejected clearly. Export streams within the memory cap and includes the per-copy triangle map. |
| AT-15 | Complete the full desktop journey using box and STL containers. Verify controls, settings/result association, copy selection, clipping, LOD switching, keyboard operation, screenshots, Save As, and error recovery. On the qualified AMD host, orbit a 1,000-copy display scene using a 20,000-triangle shared LOD at 1080p with median frame time ≤33 ms and p95 ≤50 ms over a recorded 10-second interaction. Count/placements are not changed by hiding copies. |
| AT-16 | Inject host/device allocation failure, GPU device loss, an unresponsive native operation, and a full output disk. Memory remains within declared caps or the operation terminates gracefully; no invalid trial replaces the incumbent. Auto resumes from its valid checkpoint on CPU when feasible. The UI acknowledges Stop within 250 ms and remains interactive even if the engine must be terminated. |
| AT-17 | Run the pinned Benchy and synthetic suites, save counts plus poses, and independently invoke the standalone validator on every result and STL export. Benchy counts meet the baseline rules in section 9. Record five-seed summaries for the standard case and CPU/AMD release evidence. Fail the suite on any invalid published packing; do not discard failed seeds from reports. |

Additional executable properties: rigid global translation of both object/container frames preserves a transformed solution; changes only to display settings preserve solver inputs; a continued fixed-work run preserves or improves its incumbent; removing a placement cannot create a new geometric overlap among the unchanged copies. Do not assert that a finer grid, more orientations, or a larger independent time budget necessarily produces a better heuristic result.

The standalone validator reuses the geometry contract but bypasses all search occupancy and cached feasibility. Analytic fixtures and the direct correlation oracle provide separate checks so a shared bug is not hidden by comparing the solver with itself.

### 10.1 Responsiveness and operational limits

- Protocol/UI acknowledgments target ≤250 ms on the qualified hosts, independently of job completion. Show a stopping state immediately.
- During ordinary operation, reach a safe stop within 5 seconds on the qualification suite. A longer non-preemptible native call must not freeze the UI; offer termination after 10 seconds while retaining the last checkpoint. Vulkan device hangs are handled through normal driver/device-loss behavior, not unsafe buffer reuse.
- Write progress at no more than 4 Hz; saving/export have their own bounded progress phases. Do not claim a total job deadline was met if cleanup overruns it.
- Packing-quality regressions are caught by the §9 scoreboard floors. Correctness failures always block release.
- Fuzz STL/archive/protocol parsers with bounded memory and time. Reject hostile or corrupt inputs without corrupting unrelated project files. Local operation requires no account, telemetry, or model upload.

## 11. Implementation sequence and development rules

### 11.1 Milestones

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| M0 — contracts and build | Repository skeleton; pinned Windows C++ toolchain/dependencies; schemas; CLI/service handshake; FFT and geometry adapters; fixture manifests | Clean CPU build without NVIDIA tooling; schema examples validate; dependency and source revisions recorded. |
| M1 — trusted geometry | STL/units pipeline; repair reports; authoritative assets; box/STL containment; pair distances; standalone validator; display LOD | AT-03–AT-09 geometry cases pass. Resolve the Manifold/FCL adapter's ambiguity handling here; do not defer geometry correctness to the end. |
| M2 — CPU packing vertical slice | AABB baseline; conservative voxelizer; CPU spectral placement; fixed/cube/custom catalogs; JSON result; initial packed STL export | AT-10 and CPU AT-12 pass. Headless Benchy produces a validated packing at least as good as its AABB baseline. |
| M3 — usable desktop | Imports, settings, shared-mesh viewer, progress, stop, projects, export, offline installer prototype | End-to-end AT-13–AT-15 functional paths pass on Windows CPU. This is the first usable application, with advanced count search and GPU work still pending. |
| M4 — count and orientation search | Free/upright catalogs, local refinement, multiple starts, neighborhood reinsertion, coarse-to-fine passes, continued search | AT-08 and AT-11 solver gates pass; initial spectral incumbents are preserved; practical quality comparison is committed. |
| M5 — AMD acceleration | Vulkan/VkFFT fields and reductions; bounded caching; resource preflight; device loss and CPU fallback | AT-02, GPU AT-12, and AT-16 pass on a real Windows Radeon device. Same geometry validity contract as CPU. |
| M6 — full v1 qualification | Benchy release suite; high-detail imports; installer/runtime verification; export recovery; user/developer documentation | All requirements and AT-01–AT-17 pass or their documented optional benchmark status applies; publish count-and-placement artifacts with hardware provenance. |

M0–M6 remain as history labels. From v0.3, progress is tracked by Core requirements and the §9 scoreboard, not milestone gates; M5 (AMD) and the AMD parts of M6 are Later. No milestone requires running the original CUDA psacking application; its source and paper are references, and direct oracle tests establish this port's behavior.

### 11.2 Repository organization

Use `spec/` for this document, extracted schemas, acceptance scenarios, and decision records; `engine/` for the C++ libraries/executable; `desktop/` for the Tauri frontend/shell; `tests/` for unit/integration/property fixtures; `benchmarks/` for pinned manifests and runners; and `docs/` for build, user, algorithm, and troubleshooting guides. Generated caches and large benchmark outputs do not belong in source control; small golden fixtures and result summaries do.

Suggested first implementation tasks, each linked to requirements:

1. `T-001`: Windows CPU build preset and dependency lock — SYS-01, SYS-02.
2. `T-002`: schema package, CLI/service envelopes, and asset IDs — DATA-01.
3. `T-003`: STL import, unit/frame conversion, diagnostics, and accepted geometry — GEO-01–GEO-03.
4. `T-004`: independent validator with adversarial solid/container fixtures — GEO-05, GEO-06.
5. `T-005`: shared mesh LOD and conservative voxelization — GEO-04, SOL-02, SOL-05.
6. `T-006`: physical AABB baseline and direct-correlation oracle — SOL-01, SOL-02, QA-01.
7. `T-007`: CPU FFT placement and transform export — SOL-02, SOL-03, DATA-03.
8. `T-008`: first box-to-viewer-to-export desktop journey — UI-01–UI-03, DATA-02.

Tickets T-001–T-023 in `doc/` are frozen history. New work is described in the PR that delivers it (goal, approach, exclusions); see `AGENTS.md`.

### 11.3 Spec-driven change procedure

For each change, name the requirement or scoreboard case it serves and how success will be observed. Update this spec only when product behaviour or a contract changes; algorithm changes update [docs/ALGORITHM.md](../docs/ALGORITHM.md) instead.

Test observable behaviour at public boundaries: validator verdicts, geometry and import results, contract round-trips, FFT correlation against the direct oracle, and packing counts. Fix bugs with a reproducer first. Do not add tests that assert internal counters, work units, allocation sequences or object lifetimes; they freeze the design without protecting the product. A requirement is complete when its acceptance case passes; a screenshot alone does not establish packing validity.

Use analytic fixtures for known geometric answers and the user-supplied `rc/items/` and `rc/containers/` assets for practical regressions. The user confirms that all `rc/` coordinates are millimeters and that the closed container shapes denote usable interior volumes; `rc/containers/10_kg_np.stl` is known to be 10 mm wider than the real 400×340×285 mm box. Preserve source bytes and pin their hashes; these declarations do not replace mesh/solid validation.

Write a decision record only for choices that are hard to reverse: the validity contract and clearance semantics, coordinate conventions, persisted file and protocol formats, and compute dependencies. Before v1.0 there are no external users: no migration path or legacy-interpretation rules are required for settings, results or projects, but bump schema versions when formats change. Preserve IDs when wording improves; retire rather than reuse IDs when requirements are removed. Generate protocol types from the schemas to prevent UI/backend drift.

### 11.4 Release definition of done

**Core v1.0:** a user on Windows 11 can install the application, import a valid STL, enter a box, run a bounded CPU search, inspect the best packing, stop/continue it, save the project and export a revalidated count and arrangement. Every placement respects the original accepted geometry, selected rotations, fixed physical container and clearances. Results are labeled best found, and the scoreboard is published with the release.

**v1.x:** adds the Later tier, including Vulkan acceleration qualified on an actual Windows AMD desktop, STL containers and crash recovery.
