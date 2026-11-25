# 0023 — Settings and results version 2: one timed mode, no resource record

Accepted 2026-11-25.

## Context

Version 1 settings carried a fixed-work mode (`search.deterministic`, `search.work_budget`) and,
after a desktop Start, a `resolved.resource_policy` that wrote every internal cap of the solver as
a string. Version 1 results carried `work_counts` for the search and each run segment and a
`diagnostics.resource_recovery` record of up to sixteen resource transitions. The desktop never
used fixed work, deterministic replay is Later (spec §2, SOL-07), and nobody read the cap list,
the work counts or the transition record; they tied the contracts to the solver's internals.

## Decision

Settings version 2: `search` requires `budget_seconds`; `deterministic`, `work_budget` and
`resolved.resource_policy` are gone. Every search is timed. Its host memory ceiling is half of the
free RAM at Start, or the lower `compute.host_memory_limit_bytes`, and the engine reports it in the
desktop's `resource_status` with its estimate and threads.

Results version 2: `search.work_counts`, `run_segments[].work_counts` and
`diagnostics.resource_recovery` are gone; elapsed times still add up across run segments. The
desktop `resource_status` loses `recovery_count`, `last_cause` and `last_action`.

## Consequences

Version 1 settings, results and projects are refused (`SCHEMA_UNSUPPORTED`); before v1.0 there is
no migration (spec §11). A Later deterministic mode returns as a new settings field with its own
version. Process tests that needed a partial packing take a subset of a full one.
