# Agent setup and verification

Updated 2026-10-02 with the user-approved process review workflow; existing model allocations are retained. This is the current policy; the [original setup template](codex-local-agent-setup-guide.md) and runtime records below describe earlier configurations where they conflict.

## Allocation

| Role | Configured model / effort | Use |
| --- | --- | --- |
| Primary | `gpt-6-astra` / `ultra` | Coordination, final decisions, integration acceptance |
| `implementation_worker` | `gpt-6.1-sol` / `high` | Default product implementation and substantive fixes |
| `expert_worker` | `gpt-6.1-sol` / `xhigh` | Known difficult features or bounded numerical/C++/Vulkan diagnosis |
| `code_reviewer` | `gpt-6.1-sol` / `xhigh` | Routine independent code/test and contract review |
| `process_reviewer` | `gpt-6.1-sol` / `high` | Advisory after-PR orchestration review; at most three recommendations for human approval |
| `architect` | `gpt-6-astra` / `xhigh` | Selective architecture, shared contracts, and critical validity decisions |
| `critical_reviewer` | `gpt-6-astra` / `xhigh` | Selective high-risk review or uncertainty unresolved by Sol |
| `light_worker` | `gpt-6-luna` / `low` | Low-risk edits, fixture metadata, focused tests, documentation |
| `log_reviewer` | `gpt-6-luna` / `low` | Notable log lines, locations, counts, and a short summary |

The unqualified worker default is **6.1 Sol High**. Route known difficult features directly to `expert_worker`; no failed High attempts are required. Routine review starts on Sol Extra High as a trial. Use Astra `critical_reviewer` for authoritative validity, clearance semantics, numerical uncertainty, difficult concurrency, or consequential unresolved concerns. Choose one appropriate reviewer; do not append an automatic Astra pass. Use `architect` when shared boundaries or critical decisions need design. After two failed repairs, obtain bounded expert diagnosis; if Sol Extra High has already failed, use selective Astra diagnosis and reassess.

Extra High is **`xhigh`**, not Ultra. Astra workers are limited to `architect` and `critical_reviewer`. The cap remains two workers excluding the primary, so roles are queued. Workers are leaves by policy. Test-first and acceptance requirements are unchanged. No measured project quality, latency, or usage improvement is claimed.

## Files and scope

- [AGENTS.md](../AGENTS.md): routing, allocation, TDD, and shared invariants.
- [.codex/config.toml](../.codex/config.toml) and [.codex/agents/](../.codex/agents/): primary/default settings and eight roles, each with model **and** effort.
- [PROCESS_REVIEW.md](PROCESS_REVIEW.md): scope checkpoints within existing reviews and one advisory process retrospective after PR creation; no automatic application of recommendations.
- [.agents/skills/](../.agents/skills/): four focused project skills; acceptance now includes test-first work and `rc/` policy.
- [ARCHITECTURE.md](ARCHITECTURE.md), [ADR 0001](../spec/decisions/0001-initial-architecture.md): boundaries and baseline decisions from the Astra architect.
- [TEST_PLAN.md](../tests/TEST_PLAN.md), [fixture checks](../tests/README.md), and [manifest](../tests/fixtures/rc-manifest.json): AT-01–AT-17 coverage plan and executable fixture-tool groundwork.

All changes are repository-local. Spec §11.3 now records TDD and confirmed fixture conventions. Source STL bytes are retained. No global configuration, sign-in, permissions, provider, speed, or billing settings were changed.

The first-tranche creation manifest is `.local/agent-setup/install-20260908-194541.json`. The original additions backed up changed existing role/skill files under `.local/agent-additions/backups/20260908-211000/`; existence flags and hashes are in `.local/agent-additions/install.json`. The previous setup report is `.local/agent-additions/AGENT_SETUP.previous.md`. These are historical backups, not snapshots of this allocation change. Preserve subsequent edits when undoing individual settings; do not reset the repository.

## Current verification and dispatch

On 2026-10-02, all nine project TOML files parsed; all eight role allocations,
the unchanged primary/default settings and two-worker cap, and 57 local links
passed validation. A bounded Luna configuration/documentation check found no
actionable inconsistency. Evidence: `.local/process-review-setup/validation.json`.
The current chat does not expose the new named `process_reviewer`; its runtime
dispatch remains unverified. Use the documented explicit fallback if needed.

On 2026-09-30, Python `tomllib` parsed all eight project TOML files. Assertions verified all seven role names/model/effort pairs, matching allocation tables in `AGENTS.md` and this document, leaf instructions, the Astra Ultra primary, Sol High default, and worker cap of two. `git diff --check` passed. This was configuration/documentation validation; no product tests or new model benchmark runs were needed.

Prior runtime evidence below does not verify the new allocation. On first use in a fresh project chat, inspect the exposed role settings and session metadata. Existing chats may retain earlier role definitions; when a named role is absent or stale, use a fresh/limited-context worker with the exact model, effort, and role-file instructions. A role name in a prompt alone does not select a model. Report unavailable settings rather than silently substituting.

T-008's 2026-09-30 chat exposes all seven named roles with the configured model
and effort pairs. Named `architect`, `light_worker`, and `expert_worker` dispatch
has succeeded with fresh context. Current `turn_context` session metadata was
not accessible during the tooling inventory, so this confirms exposed settings
and successful dispatch, not independent runtime model verification. Inventory:
`.local/t008/inventory.md`. The task retains the two-worker cap and leaf policy.

## Historical observed evidence (before this allocation)

- Installed desktop package `OpenAI.Codex 26.901.6511.0`; CLI and independently observed desktop engine `0.153.4` (first-tranche inspection).
- Desktop configuration home is `C:\Users\Nes\.codex`; the project was already trusted. A fresh read-only `app-server --strict-config` check under that account still reports Astra Ultra, Terra Medium defaults, and cap `2`. All seven project TOML files parse; all six role model/effort pairs match the allocation.
- Native `skills/list` discovers four enabled repository skills without project errors; `debug prompt-input` confirms root guidance loads. Diagnostics: `.local/agent-additions/client-check.json`. The optional bundled skill validator lacked PyYAML; native loading and direct metadata/link checks provide the available validation without installing packages.
- Separate architect and reviewer tasks ran on **Astra `xhigh`**, confirmed from session `turn_context` metadata. Luna `low` performed inventory, initial tests, and log triage; Terra `medium` took over the substantive structural-parser repair after narrow fixes proved insufficient. These are observations, not self-identification. Records: `.local/agent-additions/runtime-check.json`.
- Astra identified malformed ASCII and ambiguous binary-header defects. Reproducing tests drove repairs, and final review closed every finding, including one regression-test gap. All **10 fixture-tool tests** and **six focused reviewer reproductions** pass. Red/green history and Luna's notable-line summary are under `.local/fixture-inspection/`; final repro evidence is `.local/review/final-parser-check-results.json`.

Architect session: `01a0822e-b3c1-7163-95ee-d16b3752d301`; reviewer: `01a08234-a4d4-74a3-9e3a-e44790762187`; Luna worker: `01a0822f-0b98-7b61-9e3b-308f389a4cfb`; Terra repair: `01a0823a-5579-73f0-8820-609f80c72a53`. Sol runtime remains unprobed. Spark is optional and disabled.

## Historical dispatch and evidence limits

The original setup task exposed explicit model/effort overrides and no named-role selector or close-worker control. Tasks used fresh context, exact model/effort, and scoped role instructions. Reuse completed workers for related follow-ups and close them where supported. At most two workers ran simultaneously; the configured cap is confirmed without a stress test. Leaf-only behavior is policy, not a verified permission boundary.

At initial setup, native role-file and unqualified-worker dispatch had not been runtime-verified. Subsequent named-role checks below verified the earlier allocation; they do not establish runtime use of the 2026-09-30 settings.

On 2026-09-21, the T-006 task exposes native named-role dispatch. Local session
`turn_context` records confirm primary `gpt-6-astra` / `ultra`, named `architect`
and `code_reviewer` `gpt-6-astra` / `xhigh`, `implementation_worker`
`gpt-5.6-terra` / `medium`, and `light_worker` / `log_reviewer`
`gpt-5.6-luna` / `low`. All seven repository TOML files parse successfully;
metadata evidence is retained in `.local/t006/agent-runtime-check.json`.
The task uses fresh worker context and keeps at most two workers active; no
close-worker control is exposed. Historical account paths above are not current
machine prerequisites. Sign-in, billing, speed and global permissions remain
unchanged.

Before the 2026-09-30 allocation change, T-007 repeated the metadata check in a fresh primary task and verified all six
named roles, including `expert_worker` on `gpt-5.6-sol` / `high` for a reproduced
CPU correlation issue. At that time primary used Astra Ultra; architecture/review used
Astra Extra High, implementation used Terra Medium, and focused tests/log triage
used Luna Low. Evidence: `.local/t007/current-agent-metadata.json`. Workers remained
leaves and at most two run concurrently; this does not assert an enforced
permission boundary. No model, billing, sign-in or global permission setting was
changed by the check.

## Test-first operation

Spec §11.3 and the acceptance skill require a practical failing test/reproducer, the intended red result, the smallest passing change, and refactoring with affected gates passing. Failed test construction or unrelated build errors do not demonstrate missing product behavior. Logs go to Luna for notable excerpts and a short summary; higher tiers inspect deeper only for an unresolved issue.

Fixture-tool checks establish inventory/parser behavior only. They do not pass the unimplemented C++ or product AT cases. [PROJECT_STATUS.md](PROJECT_STATUS.md) records the current boundary and next work.

The layout follows official [project configuration](https://learn.chatgpt.com/docs/config-file/config-reference), [custom agents](https://learn.chatgpt.com/docs/agent-configuration/subagents#custom-agents), and [repository skill discovery](https://learn.chatgpt.com/docs/build-skills#where-codex-loads-local-skills). Local observations above define what is actually verified.
