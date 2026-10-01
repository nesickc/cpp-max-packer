# Feature delivery

The user authorizes incremental commits and pushes to feature branches. Each delivery task has its own ticket and branch. An explicitly included work package shares its owning ticket's branch, PR and acceptance ledger. Once a feature is ready, the agent may open its pull/merge request targeting `main`; the user reviews/tests it and merges it. Dependent work waits for that merge; branches are not stacked and the agent does not merge them automatically.

The spec's `T-001`–`T-008` are initial implementation tasks; `AT-01`–`AT-17` are acceptance gates that often span several tasks. A branch name links the work to its primary gate without claiming that entire gate is complete. The first branch delivers a foundation slice of QA-01; its remaining product fixtures and benchmark obligations stay in the test plan.

| Task | Branch | Prerequisite / boundary |
| --- | --- | --- |
| QA-01 test foundation | `feature/QA-01-test-foundation` | Merged through PR #1 at `067f1e52`; native tests, analytic/reference helpers, bounded performance reporting |
| [T-001 CPU build and dependency lock](T-001.md) | `feature/AT-01-windows-cpu-build` | Merged through PR #2 at `2568c38`; local/hosted checks pass |
| [T-002 schemas and service envelopes](T-002.md) | `feature/AT-13-protocol-contracts` | Merged through PR #3 at `6af862e`; local and hosted checks pass |
| [T-003 STL/units/diagnostics](T-003.md) | `feature/AT-03-stl-import` | Merged through PR #4 at `08d094b`; local and hosted qualification pass |
| [T-004 independent solid validator](T-004.md) | `feature/AT-06-solid-validator` | Merged through PR #5 at `407be83`; local and hosted qualification pass |
| [T-005 LOD and conservative fields](T-005.md) | `feature/AT-05-geometry-representations` | Merged through PR #6 at `02f62d0`; local and hosted qualification pass |
| [T-006 physical AABB baseline](T-006.md) | `feature/AT-10-aabb-baseline` | Merged through PR #7 at `bb4a0ef`; local gates, configured review and hosted checks at `a1032bb` pass |
| [T-007 CPU spectral placement and transform export](T-007.md) | `feature/AT-12-cpu-spectral-placement` | Merged through PR #8 at `1014b39`; local qualification is recorded in the ticket |
| [T-008 desktop workflow](T-008.md) | `feature/AT-15-desktop-workflow` | Merged through PR #10 at `a05fa2c`; bounded first-journey evidence and remaining full-v1 gates are recorded in the ticket |
| [T-009 CPU field scalability](T-009.md) | `feature/SOL-05-cpu-field-scalability` | Ready for review from merged `ebb0e07`; bounded local gates and three retained-result desktop journeys pass. Reviewed scope revision carries successful practical spectral/STL support into T-010 without raising caps |

[Delivery milestones](MILESTONES.md) define T-009–T-023 and their merged
prerequisites. Each delivery ticket has a planned feature branch, requirements
and observable acceptance. T-009 is the current implementation; later dependent
branches wait for the preceding user merge.
The first four specifications form three deliveries: T-009 CPU scalability and
the Ulamok regression; T-010 CPU runtime including T-011 preparation/deadlines;
then T-012 library repair. T-011 retains every acceptance ID as an included work
package with no separate branch/PR. Two workers share each delivery branch under
the [native/integration ownership rules](MILESTONES.md#parallel-agent-execution).
Explicit library repair and practical
geometry workflows follow. Live result rendering is low priority after CPU,
repair and core packing/usability work; Vulkan follows measured CPU parallelism.
[Motivating evidence](NEXT_WORK.md) and
[ADR 0011](../spec/decisions/0011-cpu-first-follow-up.md) retain the findings and
compatibility boundaries. The planning change is merged; dependent feature
branches still wait for prerequisite merges.
Freeze shared interfaces before concurrent implementation. Reuse one integrated
gate run/review across included work packages when it proves each acceptance;
performance measurements remain serial and preserve attributable baselines.

For each feature: identify requirements and observable cases; add/observe failing tests before behavior changes; implement and refactor; run affected correctness and performance checks; obtain the configured code review; push incremental commits; open the ready pull/merge request and return its link; report the final branch/head, checks and remaining limitations. Preserve real failing evidence and clearly distinguish reference-tool tests from production acceptance.

The initial performance foundation measures bounded test-reference workloads. Default timings are informational; explicit regression comparisons require compatible host/build/workload metadata. Production FFT/solver, GPU, responsiveness and release benchmark gates are added alongside the relevant features, following spec §§9–10.
