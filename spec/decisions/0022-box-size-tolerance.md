# 0022 — Box size tolerance

Accepted 2026-10-27.

## Context

Physical boxes are made to a dimensional tolerance, typically several millimetres for corrugated
cartons. Requiring every copy to fit the nominal interior exactly can lose a whole layer of copies
to a fraction of a millimetre, as with ten 28.4 mm layers of the simplified Pryanik in a 285 mm box.

## Decision

A box container may carry `tolerance_mm >= 0` in the engine settings (`box_tolerance_mm` in desktop
settings, default 0.1 mm for new desktop jobs; absent means 0). The validated interior is
`[0, W+t] x [0, D+t] x [0, H+t]`. The independent validator, solver and exporter see only that
interior; nothing about the object, the clearances or the validator changes.

Results record the validated interior as `container.dimensions_mm` and keep the requested
dimensions and tolerance in `search.resolved_settings.container`. Contract checks require each
recorded dimension to equal the requested dimension plus the tolerance exactly, in double
arithmetic, so a result cannot claim a box it was not validated against. Volume and utilization use
the validated interior.

## Consequences

A result may exceed the nominal box by up to the tolerance in each dimension, and it says so. A zero
tolerance reproduces the previous contract, and existing results without the field replay
unchanged.
