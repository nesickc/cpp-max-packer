# Agent setup and verification

Updated 2026-09-08 for the user's architecture, review, log-triage, and TDD additions. This is the current policy; the [original setup template](codex-local-agent-setup-guide.md) is historical where it conflicts.

## Allocation

| Role | Configured model / effort | Use |
| --- | --- | --- |
| Primary | `gpt-6-astra` / `ultra` | Coordination, final decisions, integration acceptance |
| `architect` | `gpt-6-astra` / `xhigh` | Initial architecture and shared testable contracts |
| `code_reviewer` | `gpt-6-astra` / `xhigh` | Focused code/test and contract review |
| `implementation_worker` | `gpt-5.6-terra` / `medium` | Bounded product implementation and substantive fixes |
| `light_worker` | `gpt-5.6-luna` / `low` | Low-risk edits, fixture metadata, focused tests, documentation |
| `log_reviewer` | `gpt-5.6-luna` / `low` | Notable log lines, locations, counts, and a short summary |
| `expert_worker` | `gpt-5.6-sol` / `high` | Reproduced difficult numerical/C++/Vulkan uncertainty |

Extra High is **`xhigh`**, not Ultra. The user authorized these two Astra worker roles. Ordinary implementation stays on lower tiers; the unqualified default remains Terra Medium. The cap remains two workers excluding the primary, so roles are queued. Workers are leaves by policy. No measured usage saving is claimed.

## Files and scope

- [AGENTS.md](../AGENTS.md): routing, allocation, TDD, and shared invariants.
- [.codex/config.toml](../.codex/config.toml) and [.codex/agents/](../.codex/agents/): primary/default settings and six roles, each with model **and** effort.
- [.agents/skills/](../.agents/skills/): four focused project skills; acceptance now includes test-first work and `rc/` policy.
- [ARCHITECTURE.md](ARCHITECTURE.md), [ADR 0001](../spec/decisions/0001-initial-architecture.md): boundaries and baseline decisions from the Astra architect.
- [TEST_PLAN.md](../tests/TEST_PLAN.md), [fixture checks](../tests/README.md), and [manifest](../tests/fixtures/rc-manifest.json): AT-01–AT-17 coverage plan and executable fixture-tool groundwork.

All changes are repository-local. Spec §11.3 now records TDD and confirmed fixture conventions. Source STL bytes are retained. No global configuration, sign-in, permissions, provider, speed, or billing settings were changed.

The first-tranche creation manifest is `.local/agent-setup/install-20260908-194541.json`. This addition backs up changed existing role/skill files under `.local/agent-additions/backups/20260908-211000/`; existence flags and hashes are in `.local/agent-additions/install.json`. The previous setup report is `.local/agent-additions/AGENT_SETUP.previous.md`. New roles have no earlier version. Preserve subsequent edits when undoing individual settings; do not reset the repository.

## Observed evidence

- Installed desktop package `OpenAI.Codex 26.901.6511.0`; CLI and independently observed desktop engine `0.153.4` (first-tranche inspection).
- Desktop configuration home is `C:\Users\Nes\.codex`; the project was already trusted. A fresh read-only `app-server --strict-config` check under that account still reports Astra Ultra, Terra Medium defaults, and cap `2`. All seven project TOML files parse; all six role model/effort pairs match the allocation.
- Native `skills/list` discovers four enabled repository skills without project errors; `debug prompt-input` confirms root guidance loads. Diagnostics: `.local/agent-additions/client-check.json`. The optional bundled skill validator lacked PyYAML; native loading and direct metadata/link checks provide the available validation without installing packages.
- Separate architect and reviewer tasks ran on **Astra `xhigh`**, confirmed from session `turn_context` metadata. Luna `low` performed inventory, initial tests, and log triage; Terra `medium` took over the substantive structural-parser repair after narrow fixes proved insufficient. These are observations, not self-identification. Records: `.local/agent-additions/runtime-check.json`.
- Astra identified malformed ASCII and ambiguous binary-header defects. Reproducing tests drove repairs, and final review closed every finding, including one regression-test gap. All **10 fixture-tool tests** and **six focused reviewer reproductions** pass. Red/green history and Luna's notable-line summary are under `.local/fixture-inspection/`; final repro evidence is `.local/review/final-parser-check-results.json`.

Architect session: `01a0822e-b3c1-7163-95ee-d16b3752d301`; reviewer: `01a08234-a4d4-74a3-9e3a-e44790762187`; Luna worker: `01a0822f-0b98-7b61-9e3b-308f389a4cfb`; Terra repair: `01a0823a-5579-73f0-8820-609f80c72a53`. Sol runtime remains unprobed. Spark is optional and disabled.

## Dispatch and evidence limits

This task exposes explicit model/effort overrides and no named-role selector or close-worker control. Tasks used fresh context, exact model/effort, and scoped role instructions. Reuse completed workers for related follow-ups and close them where supported. At most two workers ran simultaneously; the configured cap is confirmed without a stress test. Leaf-only behavior is policy, not a verified permission boundary.

When a future surface supports native named-role dispatch, select the role and inspect metadata on first use. Otherwise use the tested explicit-override fallback. A role name in a prompt alone does not select a model. Native role-file and unqualified-worker dispatch are not claimed runtime-verified. No restart is needed merely to repeat probes; confirm a fresh project task retains Astra Ultra.

## Test-first operation

Spec §11.3 and the acceptance skill require a practical failing test/reproducer, the intended red result, the smallest passing change, and refactoring with affected gates passing. Failed test construction or unrelated build errors do not demonstrate missing product behavior. Logs go to Luna for notable excerpts and a short summary; higher tiers inspect deeper only for an unresolved issue.

Fixture-tool checks establish inventory/parser behavior only. They do not pass the unimplemented C++ or product AT cases. [PROJECT_STATUS.md](PROJECT_STATUS.md) records the current boundary and next work.

The layout follows official [project configuration](https://learn.chatgpt.com/docs/config-file/config-reference), [custom agents](https://learn.chatgpt.com/docs/agent-configuration/subagents#custom-agents), and [repository skill discovery](https://learn.chatgpt.com/docs/build-skills#where-codex-loads-local-skills). Local observations above define what is actually verified.
