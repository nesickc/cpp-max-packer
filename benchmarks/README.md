# Reference oracle runner

`run_reference.py` invokes a native test reference oracle and records fixture timing samples:

```text
python benchmarks/run_reference.py --executable out/build/windows-ninja-release/bin/spectrapack_reference_benchmark.exe --output .local/reference-release.json
```

Defaults are 5 samples, 1 warmup, a 60 second watchdog, and a 20% median regression tolerance. The report records Python, OS, CPU, architecture, native compiler/build type, sample/warmup parameters, executable SHA-256, and Git revision/dirty state when available. p95 is the nearest-rank 95th percentile (sorted sample at `ceil(0.95*n)`, one-indexed).

The runner accepts only a strict schema: integer schema/work units, nonempty strings, finite nonnegative samples, and unique workload names. It recomputes statistics from every baseline's raw samples, so cached statistics cannot influence a comparison. A baseline comparison requires two distinct paths, Release reports, and exact native build, host, parameter, and workload identity matches. The new report is retained when a measured regression fails. This is reference-only fixture timing evidence, not a real FFT, solver, GPU, or product-latency benchmark. The timeout only guards runaway processes.

For a comparison, keep the prior report unchanged and choose a new output path:

```text
python benchmarks/run_reference.py --executable out/build/windows-ninja-release/bin/spectrapack_reference_benchmark.exe --output .local/reference-candidate.json --baseline .local/reference-release.json
```
