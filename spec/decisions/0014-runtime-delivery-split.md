# ADR 0014: separate runtime foundations from CPU threading

Date: 2026-10-02. Status: accepted by the user.

The combined T-010/T-011 branch grew across serial geometry fixes, retained
preparation, transport, resource accounting and threading prerequisites before
implementing a parallel kernel. The user approved splitting the delivery and
requested a concrete complexity audit. This supersedes the combined-delivery
ordering in ADR 0011; ADR 0013's technical requirements remain subject to explicit
design revisions, not silent removal.

1. **T-011 first:** deliver serial runtime/preparation/deadlines and the required
   T009-A2/A3/A6 serial packing/export prerequisites. Reuse the existing
   `feature/SOL-08-cpu-runtime` branch and its preserved evidence; its historical
   name does not imply threading is implemented. Require T011-A1–A7 plus those
   carried practical gates and a successful serial baseline. No parallel kernel,
   multiworker scheduler, durable service or live viewer is added here.
2. **T-010 second:** after the user merges T-011, start
   `feature/SOL-08-cpu-threading` from updated `main`. Deliver one measured parallel
   kernel, thread configuration, aggregate worker resources and T010-A1–A7.
   Preserve the merged runtime behavior and run affected regressions. Do not
   rebuild a second retained session or start a dependent branch before the merge.
3. T-012 and later prerequisites continue through the merged T-010. No requirement,
   acceptance ID, frozen performance target or geometry/resource constraint is
   retired. A split is not acceptance evidence; every gate remains open today.

Before further feature implementation, audit the existing handwritten changes by
user benefit, added/deleted/net size, duplicated execution paths and ownership
complexity. Prefer removing avoidable serialization round trips and consolidating
failure/resource boundaries over extending local workarounds. Record any technical
contract change and keep its observable regressions; neither code size alone nor
an existing test makes a design necessary. Do not invent a deletion quota.

Keep the existing cross-ticket evidence ledger and raw artifacts in place. Each
delivery gets its own ready PR and user merge. Historical combined-scope records
remain historical; no source reset, history rewrite or automatic merge is needed.
