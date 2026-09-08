# QA-01 test foundation evidence

Branch: `feature/QA-01-test-foundation`. Base: `14094439ff89259aff98ab67c1987022a29b95e5`.

This records testing-tool evidence only. No production geometry, packing, FFT, GPU, desktop, installer or product acceptance gate is complete. It is the foundation slice of requirement QA-01; the pinned official Benchy, product geometry regressions and full benchmark suite remain later feature work.

## Test-first evidence

The first native configure succeeded but compilation failed on a Catch2 macro expression containing initializer-list commas. That compiler error is setup evidence, not the intended TDD failure (`.local/test-foundation/native-red.log`).

After correcting the expression, the Debug build succeeded. CTest ran seven cases: four analytic-fixture cases passed; three direct-correlation cases failed with `direct integer cross-correlation is not implemented`. CTest returned 8 (`.local/test-foundation/native-red-tests.log`). The worker implemented the oracle only after that failure was observed. Incorrect expected values found in review are recalculated independently; passing an incorrect oracle test is not accepted as evidence.

The existing ten STL inventory tests and pinned manifest predate this feature. They check structural inspection of six container and four item files, not authoritative solid validity. Their source bytes are unchanged.

Review regressions were then added before repairs. Nine native cases ran; three failed, exposing integer extent/range/value overflow, L-prism edge closure, and unrepresentable derived coordinates (`.local/test-foundation/native-review-red.log`). The repaired fixture shares identical vertices, and independent face/edge checks detect a deliberately reversed face. Both full correlation arrays also match the reviewer's independent pair-scatter calculation (`.local/test-foundation/review-array-checks.json`).

Performance-wrapper test methods use case matrices for malformed reports, host/build/parameter/workload compatibility, timeout handling and regression behavior. Against the earlier wrapper, six methods produced 12 failures (`.local/evidence/reference-wrapper-red.log`). Repairs recompute baseline statistics from samples, reject baseline/output aliases before writing, preserve reports on measured regressions, and validate input types before access. Earlier test code that accidentally generated a syntax error was replaced with structured payloads.

The final eight-method suite added compatibility diagnostics with distinct output paths, oversized-sample rejection and executable resolution from another working directory. It recorded three failing cases before the last repairs and then passed eight methods (`.local/evidence/reference-wrapper-final-red.log` and `reference-wrapper-final-green.log`). Reports include executable SHA-256 and repository revision/dirty state when available; those provenance fields are allowed to differ across performance baselines.

## Local passing checks

| Check | Result | Artifact |
| --- | --- | --- |
| Fresh Debug configure, build and CTest | 9/9 passed | `.local/test-foundation/native-final-debug.log` |
| Fresh Release configure, build and CTest | 9/9 passed | `.local/test-foundation/native-final-release.log` |
| Python discovery (fixture tools and performance wrapper) | 18/18 passed | `.local/test-foundation/python-final.log` |
| Real Release reference executable through report wrapper | Two workloads, five samples and one warmup; successful bounded run | `.local/test-foundation/reference-final.json` |
| Real compatible-baseline comparison | Passed the explicit default 20% median-regression limit on this machine | `.local/test-foundation/reference-comparison.json` |

Astra Extra High completed the code review and targeted closure checks with no remaining actionable findings in this scope. Four final wrapper closure tests passed (`.local/test-foundation/review-wrapper-final-closure.log`); prior native/config findings remain closed. Luna summarized test logs separately. Default timing reports are informational, not a throughput promise or a product performance gate.

## Reproducibility and remaining gates

Local native configuration uses Windows x64, CMake 4.2.3, MSVC 19.51.36252.0 and Catch2 3.11.0 through the recorded vcpkg baseline. Commands are in [BUILDING.md](../docs/BUILDING.md); performance-report commands and comparison rules are in [benchmarks/README.md](../benchmarks/README.md).

The Windows CI workflow runs Python checks, native Release checks and bounded reference timings, retaining named logs and JSON reports. Hosted success must be reported from an actual run, separately from local results.

After user review/testing and merge, continue with T-001 from the merged default branch. [DELIVERY.md](DELIVERY.md) records the task queue and prerequisite rule.
