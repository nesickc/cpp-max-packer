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

## Native validation qualification

Run the fixed 91-workload matrix with the Release benchmark and retain raw native
streams:

```text
python benchmarks/run_validation.py --executable out/build/windows-ninja-release/bin/spectrapack_validation_benchmark.exe --build-metadata out/build/windows-ninja-release/build-metadata.json --output .local/t004/validation.json
```

Qualification requires three to twenty retained samples and one to ten warmups;
defaults are three and one. The per-process watchdog defaults to 300 seconds and
the overall deadline to 600 seconds. The overall deadline covers source checks,
native execution, independent payload validation, provenance collection,
serialization, and publication.

The runner derives all 91 workload identities, poses, verdicts, affected copy IDs,
check states, and expected AABB pair counts from the reviewed import bounds and the
fixed analytic construction. It accepts only the complete native report shape with
finite non-boolean numbers, exact enum values, deterministic work counts, complete
valid snapshots, and no snapshot for invalid results. It computes each workload's
median and nearest-rank p95 from retained sample times; native cached statistics are
not accepted as input.

All ten manifest sources are checked against their pinned byte sizes and SHA-256
values before execution, including the rejected simplified Pryanik. The import
expectations must bind the manifest hash. The executable, build metadata, manifest,
expectations, and sources retain their filesystem identities, sizes, and hashes
through publication. Output aliases, including hardlinks, are rejected. Raw stdout
and stderr remain in a unique artifact directory even when the child times out.
The qualified report records the complete native payload, per-workload statistics,
host and Release build metadata, Git revision and dirty state, timeout parameters,
and hashes for every protected input. It is serialized to an exclusive temporary
file with non-finite JSON disabled, flushed, deadline-checked, and atomically
published; an older output remains unchanged on failure.
