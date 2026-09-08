# SpectraPack project guidance

## Read only what the task needs

- Start with [project status](doc/PROJECT_STATUS.md) and `git status --short`.
- [spectrapack-spec.md](doc/spectrapack-spec.md) is the authoritative product baseline. Preserve requirement IDs. Read the relevant sections and acceptance cases before changing behavior; this file is a router, not a replacement spec.
- Use `./tools/Read-Spec.ps1` for a heading index, or `./tools/Read-Spec.ps1 -Section 5.2,5.3 -Id GEO-06,AT-09` for selected text. Avoid loading the full spec, setup guide, or logs on routine tasks.
- Consult status for the current tranche and available checks; do not assume planned build commands or product modules exist. Read [architecture](doc/ARCHITECTURE.md) when changing module boundaries.

| Task | Spec sections | Load skill when relevant |
| --- | --- | --- |
| Build, dependencies, module boundaries | 3, 4, 11 | None required |
| STL, units, solids, containment, clearance | 5 | `spectrapack-geometry` |
| FFT, voxelization, orientations, count search, Vulkan | 6 | `spectrapack-spectral` |
| Desktop, protocol, persistence, export | 7, 8 | `spectrapack-contracts` |
| Fixtures, regression evidence, milestone/release claims | 9, 10, 11 | `spectrapack-acceptance` |

Skills live in `.agents/skills/<name>/SKILL.md`; open only the applicable skill. Keep the spec at `doc/spectrapack-spec.md` until an explicit migration updates all references. Planned product directories are in §11.2.

## Model allocation and delegation

Primary: **Astra Ultra** handles coordination, final interface decisions, integration acceptance, and user communication. Delegate initial architecture to `architect`, code review to `code_reviewer`, and implementation/integration edits to workers. The primary may maintain specifications/configuration/status and run targeted checks.

| Role | Model / effort | Assignment |
| --- | --- | --- |
| `implementation_worker` | `gpt-5.6-terra` / `medium` | Bounded feature, module, integration, or repair |
| `light_worker` | `gpt-5.6-luna` / `low` | Low-risk/difficulty edits, fixtures, focused tests, inspection, documentation |
| `log_reviewer` | `gpt-5.6-luna` / `low` | Notable log lines with a short evidence-based summary |
| `architect` | `gpt-6-astra` / `xhigh` | Initial architecture, module contracts, test seams |
| `code_reviewer` | `gpt-6-astra` / `xhigh` | Focused review of code, tests, and contract consistency |
| `expert_worker` | `gpt-5.6-sol` / `high` | A reproduced hard issue or focused expert review |

- Keep at most **two open workers**, excluding the primary; queue additional roles. Astra workers are authorized only as `architect` and `code_reviewer` at **Extra High (`xhigh`)**, never Ultra. Workers are leaves: no delegation, nested Codex, or external model sessions.
- Settle shared interfaces before parallel edits. Give each worker: deliverable, relevant requirement IDs, exact inputs, exclusive write scope, dependencies, acceptance checks, and stop condition. Use fresh/limited context when sufficient.
- Do useful independent work while a worker runs; do not duplicate its implementation. Review the diff and evidence. Reuse workers for related corrections, close completed workers where supported, and avoid repeated status polling.
- After two failed repair attempts, obtain one bounded expert diagnosis and reassess. Do not grow an escalating agent tree.
- Offload low-risk work to Luna. Send raw logs to `log_reviewer` with paths and a question; return a few notable lines with file/line references, failure counts, and a short summary. The primary/reviewer reads deeper only for an unresolved issue. A truncated or filtered log cannot establish success.
- Where named roles are unavailable, explicitly pass the role's exact model **and** effort with fresh/limited context, and include its role-file instructions. See [setup status](doc/AGENT_SETUP.md) for verified capabilities. Report unavailable settings; never silently promote or substitute models.
- Preserve existing ChatGPT sign-in and permissions. No API billing or Fast-speed changes. Leaf behavior is policy, not a claimed permission boundary.

## Shared product rules

- C++20 owns geometry/packing; Tauri/Rust owns desktop integration; React/Three.js presents state. CPU pocketfft and Vulkan VkFFT are the selected compute paths. No CUDA/HIP/ROCm dependency.
- Authoritative accepted solids determine validity/export. LODs, voxels, FFT scores, and screenshots cannot authorize a placement. Only `valid` may replace the incumbent; `indeterminate` is rejected.
- Preserve rigid physical dimensions, millimeters, Z-up frames, separate pair/wall clearances, and STL interior-volume semantics. Results are `best_found`, never an optimality claim.
- Follow §11.3: identify requirements and acceptance first; update contracts/spec before behavior changes. Semantic changes require a decision record and compatibility assessment. No speculative dependency versions or invented build/test commands.
- Use test-driven development for behavior: add a practical spec-linked failing test, observe the intended failure, implement the smallest passing change, then refactor and rerun affected gates. Fix bugs with a reproducer first. Avoid tests that mirror the implementation or assert only stubs/mocks; record red/green evidence. Documentation-only edits need relevant validation, not artificial unit tests.
- [Test plan](tests/TEST_PLAN.md) maps AT-01–AT-17 to observable checks. `rc/` assets are user-confirmed millimeters; containers represent closed usable interior volumes. Preserve source bytes and hashes. Import/solid validity still needs validation; file names and a closed-edge check do not prove it. Use analytic fixtures for exact outcomes and `rc/` for practical regressions.
- Run relevant checks and required gates; broaden only for an unresolved risk. Keep long logs in `.local/`, return actual results/failures with artifact paths, and update `doc/PROJECT_STATUS.md` when scope or verified status changes. Distinguish configured, implemented, and verified.
