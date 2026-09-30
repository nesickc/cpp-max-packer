# SpectraPack

SpectraPack packs rigid copies of one STL solid into a fixed box or STL interior
volume. Its C++20 CPU engine preserves physical dimensions and independently
validates placements. The specification selects Tauri/React for the Windows
desktop and Vulkan for later GPU support.

The repository includes source-preserving STL import, accepted-solid validation,
CPU spectral placement, checked JSON/STL export and spec-linked test evidence.
T-008 adds the first desktop journey; its implementation and qualification status
are tracked separately from the remaining full-product acceptance gates.

- [Product specification](doc/spectrapack-spec.md)
- [Current status and next milestone](doc/PROJECT_STATUS.md)
- [Project instructions](AGENTS.md)
- [Agent configuration and verification](doc/AGENT_SETUP.md)
- [Initial architecture](doc/ARCHITECTURE.md) and [architecture decision](spec/decisions/0001-initial-architecture.md)
- [Executable fixture checks](tests/README.md) and [spec-linked TDD plan](tests/TEST_PLAN.md)
- [Native build instructions](docs/BUILDING.md), [CPU dependency inventory](docs/DEPENDENCIES.md), [reference timings](benchmarks/README.md), and [feature delivery queue](doc/DELIVERY.md)
- [Desktop workflow](docs/DESKTOP.md) and [T-008 scope/evidence](doc/T-008.md)
- [Original setup guide](doc/codex-local-agent-setup-guide.md)

From PowerShell, retrieve only the relevant spec text:

```powershell
./tools/Read-Spec.ps1                     # Heading index
./tools/Read-Spec.ps1 -Section 4,11.1     # Build boundaries and milestones
./tools/Read-Spec.ps1 -Id GEO-06,AT-09    # Requirement and acceptance rows
```

Project skills in `.agents/skills/` cover geometry, spectral search, desktop/data contracts, and acceptance evidence. They link back to the specification and load only when relevant.
