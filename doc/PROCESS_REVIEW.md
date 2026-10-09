# Scope checkpoints and process review

Use two distinct reviews: existing code reviewers assess implementation and its
complexity; `process_reviewer` assesses how the work was organized. The primary
remains responsible for scope control. Reuse existing tickets, evidence and review
results; this procedure adds no recurring monitor or extra review hierarchy.

## During implementation

Before substantial work, put a short scope statement in the existing ticket/plan:
deliverable, simplest plausible approach, exclusions, first observable result,
and rough expected change size/subsystems. An estimate is a review aid, not a quota.

Trigger a bounded reassessment at approximately 1,000 added handwritten production
lines, a substantial scope overrun, an unplanned subsystem/second execution path,
or two failed repairs. Count the cumulative branch change, including uncommitted
work, from its merge base; report additions/deletions separately from tests,
generated files and documentation. Formatting and moves can inflate counts;
size alone does not establish unnecessary complexity.

Use the already assigned code reviewer, architect or bounded repair diagnosis
under AGENTS.md's existing risk rules. Combine coincident triggers in one review.
Compare the original requirement with observable progress and the simpler feasible
alternative. Record one decision in the existing ticket/evidence: continue,
simplify, split, or redesign, with reasons and the next bounded result. Resolve
material design findings before adding more mechanisms. Do not relax acceptance
requirements or seek user permission for routine internal design choices.

Record the reviewed revision and dirty scope. Do not retrigger on every commit
while the same finding is being resolved; review again for a new scope/repair
trigger or roughly another 1,000 added production lines beyond that checkpoint.
At PR handoff, use the normal code review to confirm delivery and proportionate
implementation; no additional mandatory pre-PR reviewer is introduced.

## After the ready PR is opened

Dispatch one fresh `process_reviewer` (`gpt-6.1-sol` / `high`) at PR handoff, while
the work is still in context. This is an advisory retrospective, not a merge or
acceptance gate. Run once per delivery, not per push, repair or commit. Revisit
only on an explicit user request. Do not run reviews of reviews automatically.
Use the existing two-worker cap and leaf policy. If the named role is unavailable,
use the exact model/effort and role instructions with fresh context as described
in [agent setup](AGENT_SETUP.md); report that fallback.

Supply a compact brief with the PR URL/head, original scope and scope changes,
applicable orchestration instructions, worker assignments/handoffs, review and
check summaries, repeated attempts and reasons, and available timing/usage data.
Link existing records and include known contrary evidence. Do not reconstruct the
entire conversation or build a new telemetry system. Missing data is a limitation;
commit timestamps and summed parallel durations do not prove active elapsed work.

Review delegation, duplicated investigation, unnecessary handoffs/escalations,
context loss, repeated checks, delayed decisions, and reporting overhead. Inspect
workflow/configuration files and bounded execution metadata if needed; do not
read product source, test implementations, code patches or full raw build logs,
run builds/tests, or reassess code correctness. A necessary regression rerun is
not waste merely because it repeats. Distinguish observed facts from inferred
causes; never treat line count or the primary's summary alone as proof.

Return at most three numbered recommendation bullets, at most 250 words total:
**evidence -> process change -> expected benefit/tradeoff -> how to check it on
the next comparable PR**. Reference the relevant record. State material evidence
limits briefly. If no worthwhile change is supported, say so; do not invent work.

The primary presents these bullets to the user, who may approve, deny or defer
each. The reviewer makes no edits, launches no agents, and posts no PR comments.
Only explicit human approval authorizes applying a recommendation; silence and
approval of this review procedure do not approve future recommendations. Keep
decisions in the existing task record, apply only approved changes, and consider
their effect at the next comparable review. Do not repeat rejected proposals
without new evidence or automatically expand rules after every PR.
