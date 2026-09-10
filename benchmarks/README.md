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

## STL import qualification runner

`run_import.py` exercises the public `inspect` command against all ten files pinned
by `tests/fixtures/rc-manifest.json`. It independently verifies each source hash,
frame, retained source artifact, binary f64/u32 PLY, triangle provenance, and the
reviewed outcome. The six container fixtures also require the exact eight corners,
twelve triangles, complete checks, one shell, and the bounds-product volume.

Qualification requires Release build metadata and the checked-in reviewed
expectations. Discovery records current observations without claiming qualification:

```text
python benchmarks/run_import.py --executable out/build/windows-ninja-release/bin/spectrapack-engine.exe --build-metadata out/build/windows-ninja-release/build-metadata.json --output .local/t003/import-discovery.json --discover
python benchmarks/run_import.py --executable out/build/windows-ninja-release/bin/spectrapack-engine.exe --build-metadata out/build/windows-ninja-release/build-metadata.json --output .local/t003/import-qualification.json
```

Defaults are one validated pass over every fixture, then one warmup and three timed
imports of `pryanik_2.STL`, with 120 seconds per process and 600 seconds overall.
Every timed result must reproduce the reviewed outcome, geometry hash, and work
counts. Median and nearest-rank p95 are computed from raw milliseconds. Each run
uses a new artifact directory beside the output and retains stdout, stderr, reports,
and content-addressed artifacts. Output paths may not alias the executable, build
metadata, manifest, expectations, or any source, including by hardlink. An
indeterminate or resource-capped result cannot qualify; a reviewed invalid result
may qualify only when it has a completed decisive diagnostic path.
