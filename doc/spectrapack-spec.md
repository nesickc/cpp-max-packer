# SpectraPack — application specification

**Version:** 0.1 • **Date:** 8 September 2026 • **Status:** proposed implementation baseline

This document specifies a local Windows application that packs as many rigid copies of one STL model as it can find into a fixed container, displays the packing, and exports the count and placements. The algorithm is based on spectral packing and the psacking reference implementation. The full v1 includes a C++ backend, CPU operation, AMD-compatible GPU acceleration, rectangular and STL containers, mesh preparation, a desktop UI, and practical benchmarks.

This is a development specification, not an implementation or a claim of measured performance. Requirements, algorithms, interfaces, and tests below define the work to build. “MUST” is a release requirement; “SHOULD” permits a documented exception. Numerical defaults are initial, versioned engineering choices rather than values established as best by the paper.

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

### 2.3 Full v1 and later work

Full v1 includes every MUST requirement below, including the Vulkan path and STL containers. Intermediate milestones are usable subsets, not substitutes for the complete application.

Later work may add multiple distinct object types, quantities or values per type, physical assembly constraints, distributed search, Linux/macOS installers, interactive manual placement editing, and 3MF export. These are not prerequisites for v1. An installer, CLI, JSON placement export, and packed binary STL export are prerequisites.

## 3. Requirements and traceability

Every requirement in these tables is a MUST. Acceptance case identifiers refer to section 10. Detailed rules in subsequent sections refine these requirements.

| ID | Requirement | Acceptance |
| --- | --- | --- |
| SYS-01 | Ship a native Windows 10 22H2 / Windows 11 x64 application and standalone CLI; use C++20 for the geometry and packing backend. | AT-01 |
| SYS-02 | Build and run the default distribution without CUDA, cuFFT, NVCC, HIP, or ROCm. End users need no compiler, Python, Node.js, or Vulkan SDK. | AT-01, AT-02 |
| SYS-03 | Support CPU-only solving and Vulkan solving on a qualified Windows AMD Radeon device, with device detection and recoverable fallback. | AT-02, AT-16 |
| SYS-04 | Perform import, solving, viewing, project persistence, and export locally without network access. | AT-01, AT-15 |
| GEO-01 | Import ASCII and binary STL, including files with multiple solid components; detect content without relying on the extension or the word `solid` alone. | AT-03 |
| GEO-02 | Require an explicit unit selection, preserve source coordinates through recorded transforms, and never resize to fit automatically. | AT-03, AT-14 |
| GEO-03 | Diagnose mesh validity, offer bounded cleanup, and preserve the original file and any accepted repaired version separately. | AT-04 |
| GEO-04 | Maintain distinct authoritative geometry, display LODs, and search representations. Simplification must not weaken final acceptance. | AT-05, AT-06 |
| GEO-05 | Support analytic boxes and closed STL interior volumes, including concave containers and excluded cavities. | AT-07, AT-08 |
| GEO-06 | Validate original-solid overlap, containment, and both clearances; use a documented numerical tolerance independent of voxel pitch. | AT-06, AT-07, AT-09 |
| SOL-01 | Maximize identical-copy count within an immutable container and retain the best validated solution throughout a job. | AT-10, AT-11 |
| SOL-02 | Implement spectral placement with correct linear correlation, physical coordinate mapping, and independent discrete checks. | AT-12 |
| SOL-03 | Support fixed orientation, upright yaw, cube rotations, sampled full 3D rotations, and explicit quaternion catalogs. | AT-08 |
| SOL-04 | Support multiple starts, continuous local pose refinement where allowed, and removal/reinsertion of neighborhoods to seek another copy. | AT-11, AT-17 |
| SOL-05 | Reuse identical-object preprocessing and bounded caches; preflight host/device memory before allocating large fields. | AT-05, AT-16 |
| SOL-06 | Support stop-and-keep, continued search, and recoverable checkpoints without ever exposing an invalid trial as the best packing. | AT-13, AT-16 |
| SOL-07 | Record reproducible settings and seeds; provide a deterministic CPU mode with a fixed work budget. | AT-12, AT-17 |
| UI-01 | Provide import, diagnostics, container editing, search controls, progress, results, project actions, and export in one desktop workflow. | AT-15 |
| UI-02 | Display an interactive, correctly scaled packing with instanced copies, container transparency, clipping, selection, and dimensions. | AT-14, AT-15 |
| UI-03 | Keep the UI responsive during preprocessing, solving, stopping, and export; show the actual backend and resolution. | AT-15, AT-16 |
| DATA-01 | Provide a versioned C++ CLI and local request/event protocol with structured errors. | AT-13, AT-14 |
| DATA-02 | Save portable projects containing source assets, geometry decisions, settings, placements, and provenance; recover the last complete checkpoint after interruption. | AT-13, AT-14 |
| DATA-03 | Export an authoritative JSON result and optional full-resolution binary STL assembly; validate the exported coordinates. | AT-14 |
| QA-01 | Ship analytic correctness fixtures, geometry regressions, a pinned Benchy fixture, and repeatable benchmark commands. | AT-06–AT-12, AT-17 |
| QA-02 | Release against Windows CPU tests and an actual Windows AMD GPU test run, with recorded build, driver, and hardware information. | AT-01, AT-02, AT-17 |

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

- Support Windows 10 22H2 and Windows 11 x64. Qualify the complete workflow on both; Windows 11 is the primary performance reference. GPU acceleration additionally requires a working compatible driver on the selected OS.
- Provide an installer and a standalone engine package. Bundle native runtimes and shader compiler components as needed. Load Vulkan dynamically so CPU operation works without a Vulkan loader or device.
- Use WebView2 for the desktop shell and offer an offline installer variant containing its prerequisite installer. Users install normal AMD drivers, not development SDKs. Tauri documents sidecar bundling and offline WebView2 installation. [Sidecars](https://v2.tauri.app/develop/sidecar/), [Windows installers](https://v2.tauri.app/distribute/windows-installer/).
- Detect compute capabilities by feature queries and a startup FFT self-test, not GPU vendor name. Support Vulkan single precision without requiring device FP64 support. Geometry validation always uses the CPU double-precision path.
- Test on the user's exact Radeon model when available; publish the qualified model/driver list with each release. CPU fallback is required even on unqualified GPUs.

## 5. Geometry contract

### 5.1 Import and authoritative geometry

Import produces a diagnostic report containing file SHA-256, byte size, triangle/component counts, bounds, units, volume if meaningful, boundary/non-manifold edges, degeneracies, duplicate faces, shell orientation, and self-intersection findings. STL facet normals are hints; derive geometric normals from vertex order.

The initial unit selection defaults visibly to millimeters; imports must record that choice. Inches and custom positive scale-to-millimeters are supported. Display physical bounds before the first run. Recenter internally to improve numerical conditioning while retaining the exact source-to-local mapping.

Automatic cleanup may merge exactly equal vertices, remove exact duplicate faces and zero-area faces, and correct an unambiguous shell orientation without moving vertices. Record every change and revalidate. Near-vertex welding, hole filling, shell union, and topology-changing reconstruction are separate, explicit repair proposals with before/after previews. v1 must offer bounded tolerance welding; it may reject meshes needing more extensive repair and explain what must be fixed externally. Never replace a missing cavity with solid material silently.

Require closed, consistently oriented, non-self-intersecting shell geometry with an unambiguous inside. Nested boundaries represent cavities through a recorded shell containment tree and alternating orientation; disjoint components remain one rigid asset. Intersecting or touching shells with ambiguous volume semantics require repair or rejection. Do not treat “watertight” as sufficient evidence of validity.

Keep three representations:

| Representation | Purpose | Authority |
| --- | --- | --- |
| Source file and accepted full-resolution solid | Validation and export | Authoritative; repaired geometry is authoritative only after explicit acceptance |
| Display LOD | Responsive viewer | Visual approximation only |
| Conservative voxel fields | Candidate generation and fast rejection | Search approximation; cannot authorize a packing |

Use meshoptimizer to target, initially, 20,000 display triangles per shared object with a displayed error budget of 0.1 mm; allow a higher count if the budget prevents further reduction. Its simplifier reports an approximate error and can stop before reaching a target count. This is not a conservative containment guarantee. [meshoptimizer simplification](https://github.com/zeux/meshoptimizer#simplification).

Search speed in v1 comes primarily from voxel resolution, BVHs, and repeated-shape caching. Voxelize the authoritative solid directly. A later simplified search mesh may replace it only with a verified enclosing representation or an independently bounded outward offset. Ordinary decimation followed by voxelization is insufficient. Display LOD changes must not change the candidate fields, validator, or exported triangles.

### 5.2 Frames and transforms

Use a right-handed world frame with Z up and millimeters throughout the engine. A box occupies `[0,W] × [0,D] × [0,H]`. Preserve an STL container's original axes and translate its bounding-box minimum to the world origin after unit conversion; record that mapping. Do not rotate the container automatically.

For the object, let `s` be the selected source-unit scale and `a` the source bounding-box center expressed in millimeters:

`p_local_mm = s * p_source - a`

`p_world_mm = R(q) * p_local_mm + t_mm`

Store quaternions as `[x,y,z,w]`, normalized, with a deterministic sign convention. They encode active rotations applied to column vectors. JSON matrices are nested row-major 4×4 arrays operating on column vectors. Thus `source_to_world = local_to_world * source_to_local`. Export both the source mapping and placement transforms; no consumer should need voxel pitch to recover a pose. All packing transforms have unit scale and determinant +1 within numerical tolerance.

Canonical quaternion sign: make `w` positive; if `w` is exactly zero, make the first nonzero component in X/Y/Z positive. Serialize doubles with round-trip precision. Compare orientation permissions using angular distance, initially with a 1e-7 rad numerical tolerance; this does not authorize a meaningful extra rotation outside a fixed/catalog mode.

### 5.3 Validation

`validate(solution, authoritative_assets, constraints)` is independent of the search algorithm and returns `valid`, `invalid`, or `indeterminate`, with affected copy IDs and diagnostics. Only `valid` may become an incumbent or be labeled a validated export.

1. Check finite rigid transforms, asset hashes, units, allowed orientations, and the unchanged container/clearance constraints.
2. Use enlarged world AABBs to eliminate distant copy pairs. Perform original-solid checks on remaining pairs. Combine surface intersection/distance queries with solid boolean or containment classification so fully enclosed objects and coincident solids are detected.
3. For boxes, check all transformed authoritative vertices against the six inward-offset planes. Convexity makes these vertex tests sufficient for containment of triangular solids in a box.
4. For STL containers, require the object solid's difference from the permitted container volume to be empty, then check the required distance to its boundary. Vertex inclusion alone cannot detect a triangle crossing a concavity, or an object enclosing an excluded cavity.
5. Reject any positive interior overlap. For positive clearances, also reject an insufficient surface gap. Do not accept a small overlap merely because its volume is below a chosen threshold.
6. Treat unresolved classification near numerical degeneracies as `indeterminate`; retry with the adapter's robust path or reject that candidate. Kernel disagreement cannot produce `valid` by majority vote.

Geometry computations use double precision and documented scale conditioning. Initial validation tolerance is `epsilon_mm = max(1e-6, 1e-9 * D)`, where `D` is the larger object/container bounding-box diagonal in mm after unit conversion. It is a classification tolerance, **not permission to subtract epsilon from requested clearance**. For positive clearance, a result whose numerical uncertainty crosses the threshold is indeterminate unless resolved; the search should leave a small margin. Exact axis-aligned contact fixtures at zero clearance must be accepted. Features the kernel cannot resolve at this tolerance cause a diagnostic rather than deletion.

Validation may be incremental during search: reuse checks only for unchanged geometry, constraints, and poses, and validate every changed copy against all relevant neighbors and the container. A complete final/export pass bypasses search-derived certificates. Avoid recomputing expensive solid booleans for pairs already separated by a sound bounding-volume test.

Report the tolerance, kernel revision, and test method with results. “Validated” means passing this numerical contract; it is not a formal real-arithmetic proof. The implementation must not invent certified error bounds for a third-party operation that supplies none: uncertain cases stay indeterminate, and the M1 tests gate the chosen adapter.

## 6. Packing algorithm

### 6.1 Objective and incumbent

For object solid `S`, container `C`, and poses `T_i`, maximize integer `N` subject to containment, disjoint interiors, allowed rotations, and the requested distances. `C` and object scale stay fixed.

Compare layouts lexicographically: larger validated count first, then smaller occupied Z extent, then a deterministic compactness/tie-break score. Secondary scores guide search; they never compensate for losing a copy in the reported incumbent. Internal trials may temporarily contain fewer copies. Never publish them as the best result.

Compute utilization as `N * volume(S) / volume(C)` from validated solids, not the occupied cluster's bounding box or voxel count. A hollow object's volume excludes its modeled voids. If volume cannot be established reliably, omit utilization. Do not label a failed insertion as an optimum. An optional volume upper bound is displayable only when conservatively bounded volume calculations support it; otherwise omit the bound.

### 6.2 Baseline before advanced search

For a box, generate a grid of oriented object AABBs for each allowed baseline orientation. For dimensions `b_x,b_y,b_z`, pair gap `c_p`, and wall gap `c_w`, the candidate count along axis `j` is:

`n_j = max(0, floor((L_j - 2*c_w + c_p) / (b_j + c_p)))`

Place the grid deterministically and validate it using authoritative geometry. Preserve its best result as a starting incumbent. Account conservatively for numerical uncertainty in the floor and check boundary placements. This baseline deliberately works in physical coordinates, so a coarse voxel grid cannot prevent an obvious exact box tiling.

For an STL container, create the same enclosing-box candidates, filter each through true containment, and retain a validated subset. A bounding box by itself never defines feasibility for an STL container.

### 6.3 Conservative fields and FFT placement

Use one physical voxel pitch `h` and a common world grid per search level. Do not independently normalize the object and container into unrelated voxel scales.

- `O_R`: conservative solid occupancy for the authoritative object rotated by catalog orientation `R` about its local anchor. Store the kernel's trimmed grid origin explicitly.
- `B`: blocked environment cells. Block cells not wholly certified inside the container eroded by wall clearance. Also block cells intersecting placed solids dilated by the **full pair clearance**. This one-sided dilation enforces pair distances without coupling them to wall clearance.
- Construct solid occupancy with conservative triangle/cell intersection and inside classification. Preserve cavities; surface-only voxelization is not sufficient. Any uncertain cell is blocked/occupied. Euclidean clearance offsets must be conservatively represented rather than rounded down to whole voxels.

For integer translation `t`, the overlap field is:

`K_R(t) = sum_x B(x) * O_R(x - t)`

Implement it using FFT cross-correlation, with a documented reversal/conjugation and indexing convention. Pad each dimension to at least `environment_size + kernel_size - 1` before selecting an efficient FFT length. Include required geometry/clearance halos; discard all translations outside the legitimate world domain. Circular wraparound must never create an apparent free placement.

Use the FFT to propose low-overlap candidates. Verify shortlisted positions using integer/bitset occupancy and then the authoritative validator. Floating-point near-zero thresholds affect proposal quality only; they cannot establish feasibility. The reference CPU implementation uses double FFTs. The GPU path may use single precision with a measured tolerance envelope and discrete rechecks. If a field fails its error check, recompute on CPU or mark the pass unreliable; do not report a negative feasibility conclusion from it.

Specify cell boundary ownership explicitly and test exact grid alignment. Conservative occupancy can still reject a geometrically valid contact. When ordinary insertion stalls, allow a bounded refinement pass over low-overlap proposals and physical feature/AABB alignment candidates, using authoritative validation directly. Voxel overlap may reject a fast-path candidate; it is not proof that no continuous placement exists. This escape path is especially important for zero-clearance tilings.

Rank collision-free candidates using a height term and a proximity correlation. Initial policy: minimize `0.65 * normalized_top_height - 0.35 * mean_proximity`, with proximity decaying as `exp(-distance / (2*h))` from blocked geometry. Normalize terms so scores are comparable across orientation kernels. These weights are application defaults, not a claim of exact paper reproduction. Tie-break by orientation index and world Z/Y/X. Return candidates in pages of 32 per orientation; expand the shortlist when validation rejects a page and budget remains.

Cache the identical object's orientation kernels and spectra by geometry hash, pitch, orientation catalog/version, padding, numeric precision, and backend. Cache fixed container data separately. A pose change invalidates the affected environment data; changing units, clearances, or container invalidates incompatible fields.

Maintain occupancy **reference counts** for placed, dilated copies, because their clearance regions can overlap. Removing a copy must not clear cells blocked by another copy. Combine these counts with the immutable container mask. Revoxelize after off-grid motion; never round a refined pose back to the grid silently.

### 6.4 Orientations and refinement

| Mode | Meaning |
| --- | --- |
| Fixed | One supplied quaternion; identity by default |
| Upright | Source +Z remains world +Z; yaw sampled initially every 15 degrees, with continuous yaw refinement |
| Cube rotations | The 24 proper cube symmetries; no reflection or arbitrary tilted refinement |
| Free 3D | A finite catalog covering SO(3), followed by local continuous rotation refinement |
| Custom catalog | User-provided quaternions; discrete by default; reject invalid entries and deduplicate equivalents |

The free catalog begins with the 24 cube rotations, then their compositions with a 45-degree local-Z rotation, then deterministic low-discrepancy unit quaternions. Generate the latter from Halton triples in bases 2/3/5 and the standard unit-quaternion map; record the implementation version and resolved catalog hash. Deduplicate antipodal quaternions. Balanced uses 256 catalog orientations and Thorough 2,048. Sampling is not exhaustive coverage of continuous rotations.

For implementation clarity, use Halton indices starting at 1 and map a triple `(u1,u2,u3)` to `[sqrt(1-u1)*sin(2*pi*u2), sqrt(1-u1)*cos(2*pi*u2), sqrt(u1)*sin(2*pi*u3), sqrt(u1)*cos(2*pi*u3)]` in XYZW order. Sort cube rotation matrices lexicographically, use composition `R_cube * R_z45`, canonicalize signs, and continue sampling until the requested number of unique orientations is reached. Orientation ordering/version is part of deterministic replay.

Refine promising layouts by translation steps `h/2`, `h/4`, `h/8`, and, for free mode, local angular steps 5°, 2°, 1°, with decreasing-step improvement passes. Upright refinement only changes yaw. Fixed and discrete catalogs cannot drift into forbidden orientations. Every accepted move is revalidated and its environment field rebuilt or correctly updated. Repeated failures can end local refinement early; log that outcome.

### 6.5 Maximum-count controller

```text
prepare authoritative assets, baseline, fields, and orientation catalogs
best = best validated baseline, or a validated empty packing
while search budget remains:
    choose a reproducible fresh start or a neighborhood of best
    greedily insert identical copies using spectral candidates
    refine feasible poses to expose additional space
    try inserting another copy
    if blocked:
        remove a spatial neighborhood of k copies, k in {1, 2, 4, 8}
        attempt to reinsert those k copies plus at least one extra
        vary poses, candidate ranking, and neighborhood selection
    validate each proposed improvement before replacing best
    checkpoint best and emit its immutable solution revision
return best with the actual termination reason
```

Use a mixture of compact neighborhoods and dispersed removals. Identical-copy ID permutations are not useful restarts; diversify geometry, orientations, grid phases, and candidate choices. Keep a separate feasible working layout for equal-count exploration. Discard a failed neighborhood without damaging the saved incumbent.

Reserve budget for both a fresh greedy spectral run and subsequent improvement. Schedule multiple seeds within the chosen total budget, not one full budget per seed hidden from the user. All modes retain the AABB baseline. The full controller retains its initial spectral result as well. With identical configuration and a fixed work budget, continuing the same job cannot decrease the reported count.

Use coarse-to-fine passes where memory permits: start at `2h`, then `h`, preserving physical poses and rebuilding fields. A coarser pitch may miss feasible arrangements, but it cannot invalidate an already validated incumbent. Stop with `budget_exhausted`, `user_stopped`, `search_stalled`, `resource_limit`, or `error`; none of these means globally optimal.

In deterministic CPU mode, replace time-dependent search decisions with `search.work_budget.max_candidate_evaluations` and `max_search_passes`; stop at the first reached cap. One candidate evaluation is one proposed pose submitted to authoritative feasibility checking, including rejected proposals. A pass is one complete scheduled insertion/refinement/neighborhood attempt. Record both counters, RNG algorithm/state, and thread configuration. A watchdog may abort a hung run but cannot present that abort as a completed deterministic comparison. Wall-clock budgets remain the normal interactive mode.

### 6.6 Compute and memory policy

`Auto` selects a qualified Vulkan device if its startup self-test and memory preflight succeed, otherwise CPU. Explicit `CPU` never initializes compute on Vulkan. Explicit `Vulkan` with an unavailable device returns a clear error and offers a CPU retry. During a running Auto job, allocation failure or device loss discards the unfinished trial and continues from the last validated checkpoint on CPU; record the backend transition.

Preflight includes original meshes/BVHs, environment fields, full FFT padding, all live real/complex buffers, shader/planning workspace, host staging, cached spectra, validation, and the viewer reserve. A 512³ float field alone is 512 MiB; a full complex-float field is 1 GiB. Do not estimate memory from the unpadded occupancy byte array.

Initially cap engine host allocations at 50% of available RAM and compute-device allocations at 60% of the reported available device budget, both user-adjustable downward. Keep orientation spectra in a bounded LRU cache and process batches; never allocate every orientation eagerly. Shared-memory devices require combined host/device accounting.

In automatic resolution mode, increase pitch before starting until the estimated working set fits, and display the resolved value. A manually specified pitch cannot change silently: return a preflight error with a suggested value. Recheck allocations as a job grows; eviction, smaller batches, and CPU fallback precede termination. Do not shrink geometry, clearance, or the container to solve a memory problem.

## 7. Desktop behavior

### 7.1 Layout and controls

Use a single main window: configuration panel on the left, 3D viewer in the center, and a compact progress/result panel below or to the right. Advanced controls are collapsible. A first successful packing should require only import, container dimensions, and Start.

| Area | Required controls and feedback |
| --- | --- |
| Object | Import/replace STL; units; dimensions; triangle count; diagnostics; repair proposal preview/accept/reject; original/LOD viewer toggle |
| Container | Box or STL interior volume; internal W/D/H; independent STL units; bounds; diagnostic status; container transparency |
| Constraints | Pair clearance and wall clearance in mm; orientation mode; fixed/custom quaternion input in advanced controls |
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

Render a shared object mesh with instance transforms. Use display LODs during interaction; offer full-resolution inspection of a selected copy. Do not load `N` full meshes into GPU memory. The visible best packing must correspond to one complete validated solution revision. A separately enabled candidate/debug preview is clearly identified as unvalidated and cannot be exported as the best result.

UI events run independently of engine computation. Throttle progress/preview delivery to at most four updates per second and coalesce obsolete preview revisions. Keyboard focus, resize, window movement, and Stop remain usable during long native work. Missing graphics acceleration may degrade rendering quality, but CPU solving and result export remain available.

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

Headless `pack` and UI `job.start` use the same settings schema, solver, and output contract. CLI Ctrl+C maps to stop-and-keep. Exit codes: `0` for a valid terminal result, including zero copies, budget exhaustion, or a requested stop; `2` for bad input/settings; `3` for unsupported capabilities/resource preflight; `4` for internal failure; `5` for failed or indeterminate validation in the standalone validator. A fatal error may also preserve a prior valid incumbent; the nonzero exit and error record remain.

Use UTF-8 newline-delimited JSON over the sidecar's stdin/stdout. stdout contains protocol records only; human-readable logs use stderr. Each record includes `protocol_version: 1`. Requests have a unique string `request_id`, a `method`, and `params`. Responses carry the request ID, `ok`, and either `result` or a structured `error`. Events carry `job_id`, a monotonically increasing `sequence`, `type`, and `payload`. Use a maximum record size of 1 MiB; transfer geometry through scoped local asset files, not JSON arrays of millions of vertices.

Required methods: `capabilities.get`, `asset.import`, `asset.accept_repair`, `job.preflight`, `job.start`, `job.stop`, `job.continue`, `job.status`, `project.open`, `project.save`, `result.validate`, and `result.export`. Long operations acknowledge a task/job ID promptly and subsequently emit events. Only one solver job runs per engine process in v1. Duplicate request IDs must not start duplicate jobs.

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
| Container | Box dimensions or container asset reference and its source-to-world transform; interior-volume semantics |
| Constraints | Pair/wall clearance, allowed orientation policy and catalog hash, unchanged physical dimensions |
| Search | Fully resolved settings, pitch levels, seed, work counts, run segments, backend/device/driver transitions, elapsed times |
| Packing | `count` and exactly that many `{copy_id, translation_mm, quaternion_xyzw, local_to_world}` records |
| Validation | `status`, tolerance, authoritative geometry hashes, validator/kernel version, pair/containment/clearance check summaries |
| Metrics | Solid/container volume and utilization when valid, time to best count, peak host/device memory, termination reason |
| Artifacts | Optional assembled STL, preview image, and benchmark/log paths with checksums |

The quaternion/translation pair is canonical. Matrices are derived and checked for consistency. Copy IDs remain stable across surviving copies and are never interpreted as placement order. `count` must match the placement array length; a valid empty packing is represented explicitly. A `best_found` label is required; `optimal` is not a v1 result status.

### 8.4 Portable project and checkpoints

Use `.spectrapack` as a versioned ZIP archive containing `project.json`, source STL assets, accepted full-resolution meshes, import/repair reports, resolved job settings, the best result, and optional checkpoint state. Store each asset once by content hash; asset paths inside the archive are relative. Preserve accepted geometry losslessly as indexed binary little-endian PLY with float64 XYZ and uint32 triangle indices; optional display GLB and search caches are disposable derived data. A source or accepted mesh change creates a new asset hash.

During a run, use a local working directory with immutable result revisions and atomic replacement of the checkpoint pointer. Save the project through a temporary archive and atomic rename. Keep the previous complete version until replacement succeeds. Checkpoint at each new best and at least every 30 seconds at a safe boundary; include RNG/search state where supported. Never serialize GPU buffers as the only recoverable state.

Opening verifies schema versions and asset checksums and revalidates restored placements before they become an active incumbent. Unsupported newer schemas produce a clear error. Moving the archive to another directory or Windows user profile cannot break internal references. Archive extraction rejects absolute paths, traversal, and excessive declared sizes. Native paths support spaces, Unicode, and long-path-capable Windows APIs.

### 8.5 Export

JSON placements plus the referenced authoritative asset are the primary output. Export the full-resolution accepted object transformed once per copy into a binary STL assembly in millimeters. Do not export voxel cubes or a display LOD as the normal packed STL. Preserve per-copy identity in JSON because STL does not encode it or units reliably.

At export, verify the latest accepted geometry and constraints, stream triangles to avoid an `N × mesh_size` memory spike, and estimate output size before writing. Re-read the STL's actual quantized coordinates and check them under the same geometry contract. Binary STL stores finite-precision coordinates: if quantization violates clearance, fail the STL export while retaining the valid JSON/project, with advice to increase clearance. Do not silently nudge placements or relax constraints. The application may additionally emit one STL per copy using the same checks.

Exporting an assembly is not a boolean union operation. At zero clearance, touching copies may form a non-manifold combined surface; validate the per-copy solids using recorded triangle ranges, not by assuming the entire STL is one manifold solid. Include a small JSON companion with units, source hashes, and copy ranges. A screenshot export uses the current view and carries the count/settings in its companion metadata.

## 9. Practical benchmarks and quality goals

### 9.1 Fixture policy

Commit fixture manifests containing exact source URL, acquisition date, byte SHA-256, license/provenance, units, authoritative geometry hash, triangle count, physical bounds, validity/repair report, and fixed settings. Fetching “the latest Benchy” during a benchmark is forbidden. The fixture import task freezes the actual file and hash; this spec deliberately does not invent either.

Use the official 3DBenchy model linked from its download page, preserve its intended size, and include its accepted import report. The official license page currently states CC0; record that source with the exact acquired asset. If repair is necessary, retain both files and never compare repaired and unrepaired runs as identical inputs. [Official download](https://www.3dbenchy.com/download/), [Official license](https://www.3dbenchy.com/license/).

Synthetic models must be generated from checked-in parameters and have analytic dimensions. These fixtures are equally important: Benchy alone cannot expose containment, indexing, or clearance errors.

### 9.2 Benchmark matrix

| Fixture | Container and constraints | Purpose and gate |
| --- | --- | --- |
| `cube_exact` | 10 mm cube; 40×40×40 mm box; zero gaps; fixed orientation | Find exactly 64 copies. Their total volume fills the container, establishing the known optimum for this fixture. |
| `cube_clearance` | 10 mm cube; 45×45×45 mm box; pair/wall gaps 1 mm; fixed orientation | Return at least the validated 4×4×4 grid of 64; verify distances independently. No general optimality claim is required. |
| `tilted_bar` | 20×2×2 mm bar; 16×16×4 mm box; 0.1 mm gaps | Cube rotations fit zero. A 45° Z rotation and the free catalog must find at least one. |
| `interlocking_l_prisms` | Union of unit cubes at `(0,0,0)`, `(1,0,0)`, `(0,1,0)`; 3×2×1 mm box; zero gaps; upright 0°/180° catalog | Two complementary L prisms fill the box; an AABB grid fits one. Demonstrates concave packing gains. |
| `concave_container` | Analytic L-shaped permitted volume and an excluded internal cavity, each exported as STL; prescribed valid/invalid poses | Detect concavity crossings and objects enclosing forbidden material. |
| `thin_features` | Parametric hook/channel and a finely tessellated thin fin; feature widths near/below chosen pitch | Coarsening may lose count; simplification must never allow an invalid fit or remove an authoritative feature. |
| `benchy_small` | 128×128×128 mm box; 1 mm gaps; fixed 1 mm pitch; 24 orientations; 120 s | Quick practical CPU/GPU smoke run; count at least the computed, validated AABB baseline. |
| `benchy_standard` | 165×165×320 mm box; 1 mm gaps; fixed 1 mm pitch; free catalog of 256; 600 s | Main quality/runtime comparison, five fixed seeds, original STL validation and exported assembly. |
| `benchy_detail` | Same physical problem; fixed 0.5 mm pitch; free catalog of 2,048; 3,600 s | Optional long quality profile. Preflight may mark unsupported hardware; never silently coarsen this benchmark. |

Benchy acceptance is **not** an invented absolute count. For each benchmark, compute the AABB baseline from the pinned asset and publish the best validated result, settings, and transforms. On the main Benchy benchmark, the full solver must preserve both its baseline and initial spectral incumbent. Benchy quality targets may be tightened after the first measured release baseline is committed. They may not be loosened automatically to make a regression pass.

### 9.3 Measurement and comparison

For `benchy_standard`, run seeds `1,2,3,4,5`. Report best/median/worst count, time to each count improvement, preprocessing/search/validation/export time, cold end-to-end time, peak RAM/VRAM, backend transitions, actual pitch/catalog, and validation failures. Include CPU/GPU model, driver, OS build, thread count, engine commit, dependency revisions, and cold/warm cache state.

Compare three configurations: AABB baseline; this engine's greedy spectral implementation with improvement disabled; full count search. Keep geometry, clearances, pitch, orientation permissions, and total budget identical. The full run preserves its initial greedy solution, but independently timed runs need not produce identical greedy states. Also compare methods using fixed work budgets to isolate changes in search policy from hardware speed.

The baseline CPU and Vulkan paths must satisfy the same geometry contract; bit-identical packings across devices are not required. Report GPU acceleration as a measured ratio on FFT work and end-to-end workloads separately. Do not infer an application-wide speedup from an FFT library benchmark.

Before M6, freeze one Windows CPU reference host and one actual AMD GPU host with exact specifications. Proposed qualification class: 8 CPU cores, 16 GiB RAM, and an 8 GiB Radeon GPU. This is a test target, not a universal minimum for every STL. Do not invent results for the user's unspecified GPU. Long benchmark suites run on release candidates or dedicated hardware, not on every small UI change.

## 10. Acceptance tests

These are required implementation tests, not results obtained while writing this document. Test names and fixtures become permanent regression identifiers. Each test must check an observable contract, not merely repeat an internal function's implementation.

| ID | Scenario and passing condition |
| --- | --- |
| AT-01 | On clean Windows 10 22H2 and Windows 11 x64 installations without NVIDIA tools, Python, Node.js, or a development SDK, install the offline package, import a fixture, solve on CPU, view it, save/open the project, and export JSON/STL with networking disabled. The standalone CLI completes the same job. Record loaded native dependencies. |
| AT-02 | On a recorded AMD Radeon/driver configuration, run Vulkan capability detection, FFT self-tests, and `benchy_small`. Validation passes. Force CPU and repeat. Simulate a missing Vulkan loader or unsuitable device: Auto selects CPU; explicit Vulkan returns `GPU_UNAVAILABLE`. |
| AT-03 | Import equivalent ASCII/binary STLs, including a binary header starting with `solid`, negative source coordinates, Unicode paths, and separate components. Equivalent physical models produce equivalent accepted bounds/volume. A 1-inch cube resolves to 25.4 mm, and source-to-world export reconstructs the same vertices. Truncated files and non-finite coordinates produce structured errors. |
| AT-04 | Exercise reversed normals, duplicate vertices/faces, a repairable small seam, an open surface, a non-manifold edge, a self-intersecting closed mesh, and ambiguous overlapping shells. Safe cleanup is reported. Tolerance welding requires accepted repair; source bytes stay unchanged. Unresolved invalid geometry cannot start a solver job. |
| AT-05 | Use a subdivided valid mesh with at least one million triangles and a thin-feature fixture. Changing display LOD does not change authoritative hashes, voxel fields, validation, or deterministic solver results. Geometry is shared across copies. If simplification cannot reach its target within the display error setting, report the actual count; do not force it. Large jobs either pass preflight or fail without exhausting system memory. |
| AT-06 | Validate separated, touching, intersecting, coincident, and wholly enclosed primitive solids, plus a solid in a legitimate modeled cavity. The validator distinguishes material overlap from empty space. A pose that passes a deliberately undersized proxy but intersects the authoritative solid fails. Both `invalid` and `indeterminate` are rejected by incumbent publication. |
| AT-07 | For boxes, test all six walls, fractional dimensions/pitch, and gaps at, above, and below the threshold. For a U-shaped STL volume, test a bridge whose vertices lie in the two arms while its middle lies outside. Also test an object that surrounds an excluded internal cavity without crossing its boundary. Both STL cases fail containment. Valid concave-container poses pass. |
| AT-08 | Run `tilted_bar`: cube rotations yield zero; an explicit 45° Z quaternion and Free 3D yield at least one validated copy. Upright mode never tilts +Z. Custom catalogs reject non-finite/zero quaternions, normalize valid inputs, deduplicate q/−q, and prevent continuous drift outside permitted rotations. |
| AT-09 | Check `cube_clearance` and pair distances at `c−10*epsilon`, `c`, and `c+10*epsilon`. The short gap fails; the larger gap passes; the exact analytic case resolves correctly. Cover near-coplanar faces, translated large coordinates after conditioning, and tiny features. Report uncertainty rather than widening the allowable gap tolerance. |
| AT-10 | `cube_exact` returns 64 with original-solid validation and export round-trip. A run given a model too large to fit returns a valid empty packing with `best_found` and a termination reason, not “proved impossible.” The best-count event sequence is monotone and utilization uses fixed container volume. |
| AT-11 | `interlocking_l_prisms` reaches two, exceeding its AABB baseline of one. For rearrangement, start with one L translated by +0.5 mm in X from its lower-left placement: a second cannot fit while it stays fixed. With fresh restarts disabled, a fixed 10,000 candidate-evaluation / 100-pass budget and 0.25 mm pitch, removal/reinsertion reaches the known two-copy layout. Log the operator used. Removing a copy whose clearance field overlaps another's does not erase the other's blocked cells. |
| AT-12 | Compare every translation of small asymmetric binary fields with a direct integer cross-correlation oracle, including non-power-of-two extents, nonzero kernel origins, rotations, and boundary placements. No circular aliases. CPU/GPU values on the small oracle suite differ from integers by less than 0.25 and round identically; candidate validity always uses discrete/solid checks. Inject bad normalization and an FFT error to verify rejection/fallback. Fixed-work CPU runs reproduce counts and transforms on the same build/configuration. |
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
- Before M6, establish and commit host-specific runtime/peak-memory baselines for `benchy_standard`. A later median-count regression greater than 5% or median runtime increase greater than 20% triggers investigation and an explicit benchmark-baseline change if accepted. Correctness failures always block release. These are regression gates, not claims about unmeasured performance.
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

CPU correctness precedes GPU tuning. STL containment is designed and tested at M1 so it cannot become a late architectural rewrite. No milestone requires running the original CUDA psacking application on the user's PC; its source and paper are references, and direct oracle tests establish this port's behavior.

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

Subsequent tasks follow M4–M6. This list seeds the backlog; it does not exclude the remaining full-v1 requirements.

### 11.3 Spec-driven change procedure

For each feature, first identify the governing requirement IDs and observable acceptance scenario. Update the spec/contracts before implementing a behavior change. Use test-driven development: add an observable failing test, run it to confirm the intended missing behavior, implement the smallest complete slice that passes, then refactor with the affected tests still passing. For bug fixes, first capture the defect in a regression test. Record the red/green commands and results in the change evidence; do not count an unrelated build failure, skipped test, or mock-only assertion as the required failing behavior. Documentation-only changes use appropriate validation rather than artificial unit tests. A requirement is complete only when its acceptance evidence exists; a screenshot alone does not establish packing validity.

Use analytic fixtures for known geometric answers and the user-supplied `rc/items/` and `rc/containers/` assets for practical import, containment, packing, and export regressions. The user confirms that all `rc/` coordinates are millimeters and the closed container shapes denote usable interior volumes. Preserve source bytes and pin their hashes; these declarations do not replace mesh/solid validation. A simplified file is a separate source asset unless its relationship to another authoritative asset is explicitly established. The executable tests and remaining acceptance coverage are tracked in `tests/TEST_PLAN.md`.

Changes to clearance semantics, coordinate conventions, accepted-solid handling, container meaning, schemas, search objective, or compute dependencies require a decision record and migration/compatibility assessment. Preserve IDs when wording improves; retire rather than reuse IDs when requirements are removed. Generate protocol types from the schemas to prevent UI/backend drift.

Initial decision records are: spectral search with a count controller; C++ engine plus a desktop sidecar UI; CPU plus Vulkan compute; full-resolution validation independent of proxies; STL container as interior volume; and numerical validation without an optimality claim. Dependency versions and qualification hardware are recorded at their stated milestone gates. These implementation details are intentionally not fabricated in this pre-build spec.

### 11.4 Release definition of done

A user on a qualified Windows AMD desktop can install the application, import the pinned Benchy or another valid STL, choose a box or valid STL interior volume, run a bounded search, inspect the best packing, stop/continue it, and export a revalidated count and arrangement. CPU-only operation completes the same workflow. Every placement respects the original accepted geometry, selected rotations, fixed physical container, and clearances. Performance and quality evidence are shipped with reproducible settings; heuristic results are labeled as the best found.
