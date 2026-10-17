# SpectraPack project guidance

SpectraPack packs as many rigid copies of one STL solid as it can into a fixed
container. The goal of development is **more validated copies, faster, in a
usable desktop app**. Process exists only to serve that.

## Start here

- [Project status](doc/PROJECT_STATUS.md) (one page) and `git status --short`.
- [Spec](doc/spectrapack-spec.md): product contract. Read only the sections the task
  touches (`./tools/Read-Spec.ps1 -Section 5.3`). Requirement IDs are stable.
- [Architecture](doc/ARCHITECTURE.md) when changing module boundaries.
- [Build](docs/BUILDING.md). Skills in `.agents/skills/` hold domain notes for geometry,
  spectral search and contracts; open the one that applies.

## Non-negotiable product rules

- The independent full-resolution validator alone decides validity. Voxels, FFT
  scores, LODs and screenshots never authorize a placement. Only `valid` replaces the
  incumbent; `indeterminate` is rejected.
- Preserve rigid physical dimensions, millimetres, Z-up frames, separate pair/wall
  clearances and STL interior-volume semantics. Never shrink geometry, clearance or
  the container to make something fit. Results are `best_found`, never "optimal".
- C++20 owns geometry/packing, Tauri/Rust owns desktop integration, React/Three.js
  presents state. CPU pocketfft first; Vulkan/VkFFT later. No CUDA/HIP/ROCm.
- `rc/` assets are millimetres and must stay byte-identical.

## How work is done

- **One agent, full context.** Do the work yourself. Spawn a helper only for a
  clearly separate, parallel job (e.g. an unrelated script) or when the user asks.
- **Independent review** (`code_reviewer`) only when the user asks, or when a change
  touches the validator, clearance semantics, transforms or result contracts.
- **Measure with the scoreboard.** `python tools/scoreboard.py` runs the fixed cases
  and prints count, time and validity. A packing change is done when no case loses
  count or validity and the targeted case improves. Record notable results with
  `--record` (appends to `benchmarks/SCOREBOARD.md`).
- **Time-box.** Before starting, write in the PR description: the goal (ideally a
  scoreboard number), the simplest approach, and what is out of scope. If a few days
  pass without moving the scoreboard or the user-visible goal, stop and tell the user
  rather than adding more mechanism.
- **Prefer deleting to adding.** Simplify an existing path before adding a second
  one. Do not add configuration, compatibility shims or accounting layers "just in
  case"; there are no external users before v1.

## Tests

- Test observable behaviour at public boundaries: validator verdicts, geometry and
  import results, contract round-trips, FFT correlation against the direct oracle,
  packing counts on analytic fixtures. Fix bugs with a reproducer first.
- Do not write tests that assert internal counters, work units, allocation
  sequences, owner lifetimes or ticket-specific recovery paths. When such an existing
  test blocks a simplification, delete it instead of preserving the mechanism.
- Run the affected tests and the scoreboard. Full suites before opening a PR; long
  hosted qualification only before a release.

## Docs

- Keep [PROJECT_STATUS.md](doc/PROJECT_STATUS.md) to one readable page: what works,
  current goal, known problems, scoreboard snapshot. No evidence ledgers.
- Change the spec only when product behaviour or a contract changes. Write a decision
  record only for choices that are hard to reverse (dependencies, file formats, the
  validity contract). Algorithm details live in code comments and `docs/ALGORITHM.md`.
- Logs and run artifacts stay in `.local/`; link them, do not copy them into docs.
- `doc/T-*.md`, `doc/MILESTONES.md`, `doc/NEXT_WORK.md`, `doc/TEST_FOUNDATION_EVIDENCE.md`,
  `tests/TEST_PLAN.md` and `spec/decisions/` up to 0019 are frozen history. Do not
  update them; broken links inside them are acceptable.

## Delivery

- Short-lived `feature/<short-description>` branches, one goal each. Commits and
  pushes to feature branches are authorized; open a ready PR to `main`. The user
  reviews and merges; never merge automatically.
- Stacking a branch on an unmerged one is fine when it saves waiting; say so in the PR.
- Preserve existing ChatGPT sign-in and permissions. No API billing or Fast-speed
  changes.
