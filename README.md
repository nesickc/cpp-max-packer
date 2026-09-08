# SpectraPack

A planned Windows application for packing rigid copies of one STL solid into a fixed box or STL interior volume. The specification selects a C++20 engine with CPU/Vulkan compute and a Tauri/React desktop UI.

This repository contains the specification, agent setup, initial architecture, reference-fixture inventory, and executable fixture-tool tests. There is no packing application, C++ build system, or measured packing performance yet.

- [Product specification](doc/spectrapack-spec.md)
- [Current status and next milestone](doc/PROJECT_STATUS.md)
- [Project instructions](AGENTS.md)
- [Agent configuration and verification](doc/AGENT_SETUP.md)
- [Initial architecture](doc/ARCHITECTURE.md) and [architecture decision](spec/decisions/0001-initial-architecture.md)
- [Executable fixture checks](tests/README.md) and [spec-linked TDD plan](tests/TEST_PLAN.md)
- [Original setup guide](doc/codex-local-agent-setup-guide.md)

From PowerShell, retrieve only the relevant spec text:

```powershell
./tools/Read-Spec.ps1                     # Heading index
./tools/Read-Spec.ps1 -Section 4,11.1     # Build boundaries and milestones
./tools/Read-Spec.ps1 -Id GEO-06,AT-09    # Requirement and acceptance rows
```

Project skills in `.agents/skills/` cover geometry, spectral search, desktop/data contracts, and acceptance evidence. They link back to the specification and load only when relevant.
