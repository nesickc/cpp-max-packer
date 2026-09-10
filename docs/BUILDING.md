# Windows CPU build

[T-001](../doc/T-001.md) supplies the C++20 reference tests and CPU dependency diagnostic.
[T-002](../doc/T-002.md) adds versioned contracts and the capability CLI/stdio service.
Production geometry adapters, packing and the desktop application follow in later
tasks. See [ADR 0004](../spec/decisions/0004-versioned-contracts.md) for this boundary.

## Locked prerequisites

Use a vcpkg checkout at the manifest's `builtin-baseline`, with its matching bootstrapped executable. [dependencies.lock.json](../cmake/dependencies.lock.json) records the resolved target and host packages, source archives, port identities and tool hash. The target triplet is `x64-windows-static`; Debug uses `/MTd`, Release uses `/MT`. Vulkan is disabled, and configuring it on currently fails explicitly.

[toolchains.lock.json](../cmake/toolchains.lock.json) defines exact local and hosted profiles. Install the specified Visual Studio, MSVC toolset, Windows SDK, CMake and Ninja versions. The runner rejects drift; a new environment requires a reviewed profile update backed by measured versions and passing checks. Python 3.12 or later runs the standard-library tooling. The hosted profile is tied to the recorded Windows VS 2026 image; a runner image update can require deliberate requalification.

During hosted image rollouts, CI selects a unique locked profile using `ImageOS`
and `ImageVersion`. Both observed images have separate exact entries; unknown
images are rejected. Retained tool inventories and the selected build metadata
identify which profile a successful run actually qualifies.

## Build and test

From the repository root in PowerShell:

```powershell
.\tools\Invoke-NativeTests.ps1 -Preset windows-ninja-debug -ToolchainProfile local-windows-2026 -VcpkgRoot D:\path\to\vcpkg -PythonPath python -Fresh
.\tools\Invoke-NativeTests.ps1 -Preset windows-ninja-release -ToolchainProfile local-windows-2026 -VcpkgRoot D:\path\to\vcpkg -PythonPath python -Fresh
python -m unittest discover -s tests -p 'test_*.py'
```

The wrapper selects the locked Visual Studio installation and x64 developer environment, checks tools, configures, checks the installed dependency graph and static runtime flags, builds and runs CTest. Pass `-VsWherePath`, `-CmakePath`, `-CtestPath` or `-PythonPath` for non-default executable locations. `-Fresh` discards only that preset's CMake configuration cache. Outputs are in `out/build/<preset>/`; `build-metadata.json` records the measured build environment. Direct CMake commands alone do not run all admission checks.

The wrapper verifies and seeds the original locked validator patch before CMake.
Set `VCPKG_DOWNLOADS` to use a separate writable asset cache. The same Python
interpreter supplied to the wrapper runs CMake's process-level protocol checks.
Node 24.19.0 and pnpm 11.19.0 provide development-only schema/type tooling;
install with `pnpm install --frozen-lockfile --ignore-scripts`.

Run `pnpm contracts:check` for embedded-schema and TypeScript freshness, the
independent Ajv fixture corpus, and positive/negative TypeScript checks. After
an intentional schema change, use `pnpm contracts:generate`, review the generated
diff, and rebuild native tests. Native tests exercise the same checked-in schemas.

Native checks exercise exact analytic geometry through Manifold and FCL, meshoptimizer triangle preservation, a non-power-of-two pocketfft transform and explicit inverse normalization, and typed JSON round trips. Foundation cases retain analytic fixtures and direct integer correlation. These are library characterization checks, not completed product acceptance cases.

## Stage and inspect the diagnostic

After the Release wrapper succeeds:

```powershell
$packageRoot = Join-Path $env:TEMP 'spectrapack-cpu-package'
cmake --install out/build/windows-ninja-release --prefix $packageRoot --component cpu-diagnostics
```

The `cpu-diagnostics` component contains `diagnostics/spectrapack_cpu_dependency_smoke.exe`, both lock files, measured build metadata and upstream copyright notices under `share/licenses/<port>/copyright`. The diagnostic runs all five integration cases before writing an optional runtime-module report:

```powershell
$report = Join-Path $PWD '.local/cpu-runtime.json'
& "$packageRoot/diagnostics/spectrapack_cpu_dependency_smoke.exe" --runtime-report $report
```

For runtime evidence, run from the staged `diagnostics` directory outside the checkout with developer paths removed from `PATH`, retain the full report and `dumpbin /DEPENDENTS` output, then check them:

```powershell
python tools/Inspect-CpuBuild.py audit-runtime --package-root $packageRoot --runtime-report .local/cpu-runtime.json --pe-imports .local/cpu-imports.log
```

The [Windows workflow](../.github/workflows/test-foundation.yml) performs those steps and uploads the package, licenses and evidence. This staged check does not establish clean Windows 10/11 installation or the full AT-01 import/solve/export workflow.

Stage the capability-only engine with its runtime-library notices separately:

```powershell
cmake --install out/build/windows-ninja-release --prefix $packageRoot --component engine
& "$packageRoot/bin/spectrapack-engine.exe" capabilities --json
python tests/protocol/service_subprocess_test.py "$packageRoot/bin/spectrapack-engine.exe"
```

The process test runs the engine outside the checkout and checks that it responds
before stdin closes. The executable embeds its contract catalog. There is no
installer or packing/import implementation in this component yet.

## Bounded performance checks

```powershell
python benchmarks/run_reference.py --executable out/build/windows-ninja-release/bin/spectrapack_reference_benchmark.exe --output .local/reference-release.json --samples 5 --warmup 1 --timeout-seconds 60
```

These timings measure deterministic direct correlation and analytic fixture construction. They are not solver or GPU benchmarks. Keep a new baseline when changing the CRT/toolchain; do not compare to the prior dynamic-CRT build. See [benchmark instructions](../benchmarks/README.md) for compatible-baseline comparisons.

T-002 also measures bounded process-level capability replay:

```powershell
python tests/protocol/service_replay_perf.py out/build/windows-ninja-release/bin/spectrapack-engine.exe --samples 5 --requests 1000
```

One warmup precedes five fixed-work samples. The watchdog rejects a stalled run;
times are informational and include process/transport overhead. This does not
measure solver performance or qualify stop latency.
