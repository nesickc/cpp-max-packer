# ADR 0002: Test foundation before product implementation

Date: 2026-09-08. Scope: QA-01 and test groundwork for AT-03, AT-09, AT-10, AT-12 and AT-17.

The user requested a separate test-foundation feature, practical tests and reasonable performance checks before implementation. Each dependent feature waits for its prerequisite merge.

Use C++20, CMake 3.25+ with Ninja Debug/Release presets, and Catch2 3.11.0 supplied by vcpkg baseline `271a5b8850aa50f9a40269cbf3cf414b36e333d6`. This baseline and port were verified in the available checkout; the manifest records them instead of relying on a floating dependency. Catch2 provides CTest discovery without a custom test runner. Python tooling remains standard-library-only. Product geometry/FFT/UI dependencies remain T-001 work.

Keep analytic geometry builders and direct integer linear cross-correlation in `tests/support/`, independent of future production algorithms. Pin hand-computed examples so the reference helper itself is checked. Product features must add tests at their public boundaries and observe intended failures before implementing behavior. These helper tests do not complete product acceptance gates.

Measure fixed, bounded reference workloads with observable checksums in Release mode. Default reports are informational; an explicit compatible baseline enables a relative regression limit. Shared CI hosts do not enforce universal timing thresholds. Product FFT/packing, GPU and UI performance cases are added with those features and retain the spec's validation requirements.

Compatibility: this adds developer tooling and CI only. It changes no product file format, geometry semantics, accepted-solid rules or user-visible protocol. Existing `rc/` assets and their manifest remain unchanged.

References: [Catch2 CMake integration](https://catch2-temp.readthedocs.io/en/latest/cmake-integration.html), [vcpkg versioning](https://learn.microsoft.com/en-us/vcpkg/users/versioning), [test plan](../../tests/TEST_PLAN.md), [delivery workflow](../../doc/DELIVERY.md).
