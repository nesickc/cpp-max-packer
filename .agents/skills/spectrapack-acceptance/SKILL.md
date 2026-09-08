---
name: spectrapack-acceptance
description: Plan and verify test-first SpectraPack changes, practical correctness fixtures, acceptance gates, or milestone/release evidence.
---

# Test-first acceptance and evidence

Start with [project status](../../../doc/PROJECT_STATUS.md) and the relevant row of [TEST_PLAN.md](../../../tests/TEST_PLAN.md). Read governing requirement/AT rows with `./tools/Read-Spec.ps1 -Id SOL-02,AT-12`; use §11.1 for milestones and §9 for benchmarks. The [spec](../../../doc/spectrapack-spec.md) remains authoritative.

For behavior changes, write a practical spec-linked test, run it to observe the intended missing behavior, implement the smallest passing change, then refactor while rerunning affected gates. Reproduce bugs before fixing them. Record actual red/green commands and outcomes. An unrelated build failure, skipped test, self-confirming mock, or test mirroring implementation is not acceptance evidence. Tests for existing behavior may be characterization checks; never fabricate a red run. Documentation-only edits need relevant validation rather than artificial unit tests.

Inspect existing presets/runners before naming commands. Missing tooling or hardware is a limitation, not a passing stub. Prefer public module/CLI contracts and independent checks:

- Geometry/search: small analytic solids and direct integer correlation supply exact answers; include relevant enclosure, cavity, clearance, contact, and indexing adversaries.
- Practical fixtures: `rc/` source coordinates are user-confirmed millimeters; containers are intended closed usable interior volumes. Pin hashes via [rc-manifest.json](../../../tests/fixtures/rc-manifest.json), preserve originals, and validate actual solids before accepted packing tests. Closed edges alone are insufficient. A simplified filename does not establish an LOD relationship. Heavy asset cases are integration tests; small analytic cases form fast unit tests.
- Benchmarks: pin provenance/hash/settings and keep count **and poses**. Independently validate results/re-read exports, retain failed seeds, and follow §9 budgets/baselines. User assets complement the required pinned Benchy; do not invent counts or public redistribution rights.
- Determinism: fixed work, same build/configuration, recorded RNG/thread settings; watchdog aborts are not completed comparisons.
- Release: actual Windows Radeon results gate M5; full v1 gates, including clean Windows CPU workflows, gate M6. Respect only optional benchmarks identified by the spec.

Run narrow tests and required gates once; repeat/broaden for changes, failures, or unresolved risks. Route raw logs to the Luna `log_reviewer` with paths/question. Return exact commands, red/green or characterization results, scope/IDs, relevant build/hardware, and evidence paths. Keep full logs in `.local/`; update status only to the demonstrated level. Fixture-tool tests do not prove product AT cases.
