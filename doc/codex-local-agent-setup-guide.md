# Local Codex setup: Astra orchestration with economical workers

> Historical setup template. The user's subsequent additions authorize Astra Extra High (`xhigh`) `architect` and `code_reviewer` workers, Luna log review, and spec-linked TDD. Current project policy is in `AGENTS.md` and `doc/AGENT_SETUP.md`; it supersedes this template's blanket ban on Astra workers. Keep the two-worker cap and economical implementation defaults.

**Version:** 1.0 · **Prepared:** 8 September 2026

## Instructions to the local setup agent

Implement this configuration in the user's active local project on Windows. The user wants **GPT-6 Astra Ultra to orchestrate**, with cheaper models doing implementation. Use Codex's built-in subagents and the existing ChatGPT Pro sign-in. This task is configuration and a small verification run; do not start implementing the application or build another orchestration framework.

Inspect the installed client before changing files. Preserve existing project instructions and unrelated configuration. Apply compatible changes, verify the effective setup, and report any remaining client interaction. Do not merely describe what could be configured. If no target project is identified, prepare the proposed configuration and ask for the target directory before installing it globally or into an unrelated repository.

The templates below follow the official documentation available on the preparation date. Installed versions and model access can differ. Adapt to verified local capabilities, and distinguish **configured**, **observed working**, and **unavailable** in your completion report.

## 1. Required model division

| Role | Model ID | Effort | Responsibility |
| --- | --- | --- | --- |
| Primary orchestrator | `gpt-6-astra` | **Ultra**, selected in the app | Architecture, decomposition, interface decisions, assignment, integration decisions, critical review, user communication |
| `implementation_worker` | `gpt-5.6-terra` | `medium` | Bounded C++ modules, ordinary UI features, integration work and fixes |
| `light_worker` | `gpt-5.6-luna` | `low` | Mechanical edits, simple fixtures/tests, focused inspection and documentation |
| `expert_worker` | `gpt-5.6-sol` | `high` | Difficult C++, numerical or Vulkan issues, targeted expert review |
| Optional `spark_worker` | `gpt-5.3-codex-spark` | `low` **if supported locally** | Small text-only coding iterations where latency matters |

Luna Medium is an optional intermediate setting for tasks that outgrow Low but remain narrow. Use a separately configured role or a verified explicit model/effort override; do not assume an override can defeat a role file's fixed effort. The required baseline uses Luna Low.

Keep **at most two concurrently open subagent threads**, excluding the primary. Workers are leaves and must not delegate. Do not spawn Astra workers. Routine implementation belongs to workers; the primary may inspect relevant code, review changes, run targeted verification, and maintain coordination documents.

The allocation is the user's chosen operating policy, not a guarantee of savings. OpenAI positions Terra for everyday work, Luna for focused inexpensive tasks, and Sol for harder work. Ultra uses maximum reasoning and supports proactive delegation. [Model guidance](https://learn.chatgpt.com/docs/models).

## 2. Inspect and preserve the current installation

Do this setup serially. Do not create a team to configure the team.

1. Identify the active repository, applicable `AGENTS.md`/override files, existing `.codex` configuration, custom roles, and uncommitted changes.
2. Record the desktop app version and, if installed, the CLI version using `codex --version`. Do not assume the desktop app uses the same engine build as the CLI.
3. Inspect the effective config location and precedence. On a normal native Windows installation, user configuration is under `%USERPROFILE%\.codex`; honor an existing `CODEX_HOME` override. A WSL or remote session can have a different home and installation. Configure the environment actually used by the project.
4. Verify model availability and supported efforts from the installed client, its model picker, schema/help, or session metadata. Keep the existing ChatGPT sign-in; do not print credentials or introduce API-key billing.
5. Make timestamped local backups of each existing file you will change. Record paths and original existence. Merge TOML tables without duplicate keys and preserve unrelated settings and comments where practical.

Prefer **project-scoped configuration**. Use personal/global settings only when the user has explicitly chosen that scope. Project configuration is loaded only for trusted projects, and normal precedence or managed settings can affect it. Do not change sandbox, approval, authentication, provider, or organizational policy settings to make model selection work. [Configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference).

If a version does not support the documented configuration, do not guess substitute keys or silently install a different client. Prepare the compatible portion and state the specific missing capability. If only explicit per-spawn overrides are supported, use those together with the project policy and record the limitation.

## 3. Configure the primary and default workers

Merge into **`<project>/.codex/config.toml`**:

```toml
# Main thread default. Confirm Astra + Ultra in the desktop model picker.
model = "gpt-6-astra"

[agents]
enabled = true
default_subagent_model = "gpt-5.6-terra"
default_subagent_reasoning_effort = "medium"
max_concurrent_threads_per_session = 2
```

Select **Astra → Ultra** in the desktop app for the primary project task. If the installed version supports persisting Ultra as a TOML effort value, it is acceptable to add `model_reasoning_effort = "ultra"` at the top level, **before `[agents]`**. Verify support first: published pages and client versions can differ in the effort values they enumerate. Do not substitute `xhigh` or `max` and report it as Ultra.

An existing app selection or launch override can supersede the default model. Verify the primary session's actual selection after reloading configuration or starting a new task. Prepare all file changes first if a user click in the app is required to finish selection.

The four `[agents]` keys are documented configuration controls; the concurrency cap excludes the primary. Model and effort defaults prevent ordinary workers from inheriting Astra's expensive configuration. These defaults are not a model allowlist. [Configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference).

## 4. Install named worker roles

Current Codex documentation supports standalone project agents under **`.codex/agents/`**, with `name`, `description`, and `developer_instructions`. Set **both model and effort** in every role. Role-file values override the model/effort resolved before that file is applied, so select the correct role rather than attempting to turn a Terra role into Sol through a conflicting spawn argument. [Custom agent configuration](https://learn.chatgpt.com/docs/agent-configuration/subagents#custom-agents).

Use the following three files. If an existing role has the same name, inspect and merge intentionally or choose a consistent project-specific name and update the policy. Do not overwrite unrelated instructions.

### `.codex/agents/implementation_worker.toml`

```toml
name = "implementation_worker"
description = "Terra Medium. Implement one bounded feature or fix in assigned files."
model = "gpt-5.6-terra"
model_reasoning_effort = "medium"
developer_instructions = """
You are an implementation worker, not the primary orchestrator.
Implement the assigned deliverable and run its relevant verification.
Follow the task brief, repository instructions, and agreed interfaces.
Edit only assigned files. Do not revert or overwrite another worker's changes.
Do not spawn subagents or launch nested Codex/model sessions.
If a shared interface must change, return the proposal to the primary.
After two unsuccessful implementation/repair attempts, return a concise blocker
with the evidence and the smallest useful next step. Do not restart the task tree.
Avoid unrelated cleanup and broad tests without a concrete reason.
Return status, changed files, checks/results, and remaining risks in <=200 words;
include essential failure details or artifact paths when needed.
"""
```

### `.codex/agents/light_worker.toml`

```toml
name = "light_worker"
description = "Luna Low. Handle small, clear edits, fixtures, inspection, or documentation."
model = "gpt-5.6-luna"
model_reasoning_effort = "low"
developer_instructions = """
You are a lightweight worker, not the primary orchestrator.
Complete only the narrow task in the brief and obey its read/write scope.
Do not spawn subagents or launch nested Codex/model sessions.
Use the supplied files, constraints, and acceptance checks.
If the task requires significant redesign or ambiguous reasoning, report that
early with evidence so the primary can route it to Terra or Sol.
Run only relevant checks and preserve concurrent changes.
Do not invent verification results or claim a model identity from your prompt.
Return status, changed files or findings, checks/results, and blockers in
<=150 words, with file references for supporting detail.
"""
```

### `.codex/agents/expert_worker.toml`

```toml
name = "expert_worker"
description = "Sol High. Resolve a specific difficult issue or conduct targeted expert review."
model = "gpt-5.6-sol"
model_reasoning_effort = "high"
developer_instructions = """
You are a targeted expert worker, not the primary orchestrator.
Address the stated uncertainty using the supplied evidence and relevant code.
Do not redo completed exploration without identifying missing or unreliable evidence.
Do not spawn subagents or launch nested Codex/model sessions.
For review-only tasks, do not modify files. For repair tasks, edit only assigned files.
Focus on correctness, numerical edge cases, integration, and the named risk.
Run the smallest verification that resolves the issue; obey required repository gates.
Return findings or changes, supporting evidence, checks/results, and unresolved
questions in <=250 words. Do not expand the task into a general audit.
"""
```

Keep permission settings inherited. “No further delegation” and the model routing policy are instructions; do not present them as a hard permission boundary unless the installed client supplies and verifies such enforcement. Do not invent a `max_depth` key. The documented concurrency setting provides the separate thread cap.

For clients that require explicit `[agents.<role>]` declarations instead of standalone discovery, consult that version's schema and register these roles with supported `config_file` paths. Paths resolve relative to the declaring config. Do not register roles twice through competing discovery mechanisms or assume the older format accepts every standalone metadata field.

## 5. Add a concise project policy

Merge the following section into the applicable root **`AGENTS.md`**. Keep existing project guidance. If an override file shadows it, place or reconcile the section in the effective instruction source rather than leaving an inactive copy.

```markdown
## Model allocation and delegation

These rules distinguish the primary thread from spawned workers.

Primary thread:
- Use GPT-6 Astra Ultra for architecture, task decomposition, interface decisions,
  coordination, critical review, and user communication.
- Delegate product implementation to implementation_worker (Terra Medium).
- Use light_worker (Luna Low) for clear, small tasks. Use expert_worker (Sol High)
  for a specific difficult issue or targeted review, not for every change.
- Do not spawn Astra workers. Do not promote a worker to Ultra implicitly.
- Keep at most two open worker threads. Use one when work is sequential.
- Spawn workers only for concrete deliverables with useful independent work.
  Prefer one implementation worker plus a targeted reader/reviewer where useful.
- Define shared interfaces before parallel edits and assign distinct file ownership.
- Give each worker a focused brief: objective, relevant requirements, paths,
  constraints, allowed edits, and acceptance checks. Use fresh or limited context
  when supported and sufficient; preserve all applicable constraints.
- Do not implement the same task alongside the assigned worker. Inspect its diff
  and evidence; request a focused correction when necessary.
- Reuse a worker for a closely related follow-up; start fresh when context is stale
  or the task is unrelated. Close completed threads when the client supports it.
- After two failed implementation/repair attempts on a subtask, request one bounded
  expert diagnosis. Then reassess the approach; do not start an escalating agent tree.
- Integration edits belong to an assigned worker. The primary may maintain plans,
  specifications, and configuration, review code, and run targeted verification.

Spawned workers:
- Implement or inspect the assigned scope; the primary-only restrictions on product
  implementation do not apply to workers.
- Do not spawn agents, run nested Codex sessions, or call external models.
- Preserve other workers' changes and escalate shared-interface changes to the primary.
- Keep raw logs in local artifacts. Return a concise result with changed files,
  actual checks/results, blockers, and material risks. Never hide a failed check.
- Run checks appropriate to the change and required project gates. Broaden testing
  only to resolve a concrete remaining risk.

For all threads:
- Do not silently substitute unavailable models or efforts. Report the problem and
  use the documented fallback only when that fallback is available.
- Use the existing ChatGPT sign-in. Do not introduce paid API calls or enable Fast
  speed as a supposed cost-saving measure.
- Honor existing permissions and repository requirements.
```

The word limits in worker templates apply to summaries, not implementation completeness or essential evidence. They are not reasoning-token caps. Higher effort can still consume substantial usage even when the visible answer is short. [Model and effort guidance](https://learn.chatgpt.com/docs/models).

## 6. Optional Spark setup

Do not make Spark the default or a prerequisite. First verify **`gpt-5.3-codex-spark`** is available to the actual desktop/CLI session and is accepted for subagent use. Availability in the main model picker alone is insufficient evidence of worker support.

If supported, create `.codex/agents/spark_worker.toml` using the `light_worker` template with these replacements:

```toml
name = "spark_worker"
description = "Optional Spark worker for small text-only coding iterations."
model = "gpt-5.3-codex-spark"
model_reasoning_effort = "low"
```

Retain the complete `developer_instructions` from the light-worker template. Validate the effort locally; if Low is unsupported, record the supported setting actually chosen before enabling the role. Do not let it inherit Ultra. Add the role to the project policy only after verification.

Use Spark for mechanical code changes and tight text-based iterations. Give it a text-only brief; do not rely on inherited images. Keep geometry, FFT, and Vulkan correctness decisions with the stronger roles. If Spark is missing, rejects worker invocation, or hits its limit, fall back to `light_worker` and report the substitution once.

OpenAI documents Spark as a text-only Pro research preview, with availability depending on the client and rollout and a separate demand-dependent usage limit. Its behavior in the local installation must be checked. [Models](https://learn.chatgpt.com/docs/models), [Pricing and Spark usage](https://learn.chatgpt.com/docs/pricing).

## 7. Verify the actual setup with minimal work

### Static checks

- Parse changed TOML with an available TOML parser and validate against the installed client's supported schema. Parsing alone does not establish that Codex recognizes a key.
- Confirm role discovery, both model and effort in each file, the primary model, and a worker thread cap of two.
- Inspect effective configuration/launch overrides and applicable instructions. Keep evidence restricted to relevant non-secret fields.
- Start a fresh project task or reload through the supported client mechanism. Do not restart active work without preserving it.

### Small runtime probes

Use **three tiny read-only worker tasks**, one per required role, sequentially or with no more than two open at once. Assign each a concrete harmless task, such as checking that one named configuration file parses or locating one named requirement in the specification. Limit the result to a few lines. No repository-wide review, benchmark, or application implementation is needed to test routing.

For each probe, inspect client/session metadata or supported diagnostics and record **requested and observed model/effort**. The worker saying “I am Terra” is not verification. If metadata is unavailable, label model/effort selection unverified instead of claiming success.

Verify unqualified workers resolve to Terra Medium through effective configuration. Run one additional tiny default-worker probe only if needed to resolve uncertainty about precedence. Verify the concurrent-thread cap using supported diagnostics when available; do not launch a flood of agents as a test. A Spark probe is optional and happens only after local availability is established.

Check that the primary remains Astra Ultra and workers did not inherit Ultra. Close finished probes. If a role loads with the wrong settings, correct the cause and retry only that probe once. Do not continue with an unexpectedly expensive configuration.

### Required completion report

Return a concise report with:

1. Changed files and backup locations.
2. Desktop/CLI versions and the configured project/config scope.
3. Primary model/effort and each role's requested versus observed settings.
4. Evidence for the thread cap and a clear note that leaf-only behavior is policy unless enforced by the client.
5. Probe results, Spark enabled/unavailable status, and any remaining user action.

Finish all configuration work before requesting a necessary final app interaction. Never claim that a hosted chat changed the user's local app configuration.

## 8. Task briefs and economical operation

For normal implementation, use this brief structure:

```text
Role: implementation_worker
Deliverable: one observable feature or fix
Requirements: only relevant spec IDs/sections, plus shared invariants
Inputs: exact files and the current agreed interfaces
Write scope: explicit files/directories; other files are read-only
Dependencies: what is already settled; what must wait
Acceptance: specific tests/checks and expected outcomes
Stop/escalate: completion condition or a concrete unresolved blocker
Return: status, changed files, checks/results, risks; link lengthy evidence
```

For SpectraPack, allocate architecture and review of **coordinate transforms, clearance, authoritative-solid validation, FFT indexing, and count semantics** to Astra. Have Terra implement a bounded backend module or UI feature. Use Luna for simple fixture manifests, settings wiring, or documentation. Use Sol for a reproduced numerical/collision/Vulkan issue with a focused question. Run independent UI and backend work concurrently only after their contract is fixed.

The orchestrator should wait for meaningful progress using the client's normal completion/wait mechanisms. Avoid repeated status polling, requests for restated summaries, unnecessary agent replacement, and duplicate full test runs. Maintain a short task/decision note in the repository when needed; do not construct a second project-management system.

Measure usage per accepted change over the next few real tasks, along with elapsed time, retries, and defects. Do not spend a second implementation of every feature just to benchmark the workflow. Pro allowance consumption depends on model, reasoning, context, tools, and other factors; no fixed saving percentage is promised. Standard and Fast speed are separate from model/effort choice, and Fast can consume allowance faster. [Usage guidance](https://learn.chatgpt.com/docs/pricing).

Keep Astra Ultra as requested. A later switch to lower effort for routine orchestration is a separate optimization decision, not a silent part of this setup.

## 9. Recovery and completion criteria

On an unsupported key or role format, revert only the affected edits and keep the validated configuration. Preserve backups and show the specific limitation. To undo the setup later, remove the added roles/policy section and restore only the changed settings, preserving any subsequent user edits. Do not use a repository-wide reset.

Setup is complete when the local client loads the configuration, the primary is Astra Ultra, required roles resolve to Terra Medium / Luna Low / Sol High, the two-worker cap is established, project policy is active, and the small probes are recorded. Spark can remain unavailable without blocking the baseline. Anything not verified is explicitly listed in the report.
