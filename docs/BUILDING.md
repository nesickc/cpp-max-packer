# Native test foundation

QA-01 pins Catch2 3.11.0 through the vcpkg manifest baseline and provides CMake/Ninja presets for the native reference-test scaffold. It does not build a production engine, FFT implementation, or geometry adapter.

Set `VCPKG_ROOT` to a vcpkg checkout with the pinned baseline available, then run the portable wrapper from a Developer PowerShell:

```powershell
.\tools\Invoke-NativeTests.ps1 -VcpkgRoot D:\path\to\vcpkg -Preset windows-ninja-debug
```

The wrapper locates Visual Studio through `vswhere`, imports its x64 developer environment, configures, builds, and runs CTest. To use non-default executable locations, pass `-VsWherePath`, `-CmakePath`, or `-CtestPath`. Equivalent steps are:

```powershell
$env:VCPKG_ROOT = 'D:\path\to\vcpkg'
cmake --preset windows-ninja-debug
cmake --build --preset windows-ninja-debug
ctest --preset windows-ninja-debug
```

Pass `-Fresh` to have CMake discard only the selected preset's configured cache before configuring. This is useful for a clean configuration check; it does not delete other build directories.

`out/build/<preset>/bin/spectrapack_reference_benchmark.exe` is a bounded reference harness for QA development. It checks its own analytic/reference work before measuring and prints exactly one JSON object on stdout:

```powershell
.\out\build\windows-ninja-release\bin\spectrapack_reference_benchmark.exe --samples 5 --warmup 1
```

`--samples` accepts 1 through 1000 and `--warmup` accepts 0 through 1000. The workloads are deterministic CPU reference work: direct integer linear cross-correlation and analytic fixture construction. They are not packing measurements or a product performance gate.

The CTest suite covers QA groundwork for AT-12: asymmetric non-power-of-two fields, every full-linear translation range, nonzero kernel origins, boundaries, and input rejection. Analytic cuboid, tetrahedron, and L-prism fixtures provide known dimensions, winding/volume, and count checks for later geometry work. CPU/GPU agreement, FFT fallback behavior, and complete AT-12 acceptance remain future engine work.
