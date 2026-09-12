# Feature delivery

The user authorizes incremental commits and pushes to feature branches. Each task has its own ticket and branch. Once a feature is ready, the agent may open its pull/merge request targeting `main`; the user reviews/tests it and merges it. Dependent work waits for that merge; branches are not stacked and the agent does not merge them automatically.

The spec's `T-001`–`T-008` are initial implementation tasks; `AT-01`–`AT-17` are acceptance gates that often span several tasks. A branch name links the work to its primary gate without claiming that entire gate is complete. The first branch delivers a foundation slice of QA-01; its remaining product fixtures and benchmark obligations stay in the test plan.

| Task | Branch | Prerequisite / boundary |
| --- | --- | --- |
| QA-01 test foundation | `feature/QA-01-test-foundation` | Merged through PR #1 at `067f1e52`; native tests, analytic/reference helpers, bounded performance reporting |
| [T-001 CPU build and dependency lock](T-001.md) | `feature/AT-01-windows-cpu-build` | Merged through PR #2 at `2568c38`; local/hosted checks pass |
| [T-002 schemas and service envelopes](T-002.md) | `feature/AT-13-protocol-contracts` | Merged through PR #3 at `6af862e`; local and hosted checks pass |
| [T-003 STL/units/diagnostics](T-003.md) | `feature/AT-03-stl-import` | Merged through PR #4 at `08d094b`; local and hosted qualification pass |
| [T-004 independent solid validator](T-004.md) | `feature/AT-06-solid-validator` | In progress from merged T-003; adversarial overlap, containment, cavity and clearance gates |
| T-005 LOD and conservative fields | `feature/AT-05-geometry-representations` | Relevant import/validator contracts merged; representation isolation |
| T-006 physical AABB baseline | `feature/AT-10-aabb-baseline` | Validator merged; exact physical fixtures and retained incumbent; reuse independent correlation test oracle |
| T-007 CPU spectral placement | `feature/AT-12-cpu-spectral-placement` | Geometry fields/baseline merged; compare production FFT against independent reference |
| T-008 desktop workflow | `feature/AT-15-desktop-workflow` | Required engine/protocol/result contracts merged; first desktop vertical slice |

Later M4–M6 tasks will be bounded and ticketed from the spec when their prerequisites are available. No future task is marked implemented by this queue.

For each feature: identify requirements and observable cases; add/observe failing tests before behavior changes; implement and refactor; run affected correctness and performance checks; obtain the configured code review; push incremental commits; open the ready pull/merge request and return its link; report the final branch/head, checks and remaining limitations. Preserve real failing evidence and clearly distinguish reference-tool tests from production acceptance.

The initial performance foundation measures bounded test-reference workloads. Default timings are informational; explicit regression comparisons require compatible host/build/workload metadata. Production FFT/solver, GPU, responsiveness and release benchmark gates are added alongside the relevant features, following spec §§9–10.
