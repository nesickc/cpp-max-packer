# Executable tests

Run `python -m unittest discover -s tests -p 'test_*.py'` with Python 3.12 or later. These tests check the STL inventory helper, pinned reference manifest, CPU build/dependency/runtime guards, and performance-report harness. They use only the Python standard library.

The C++20 Catch2/CTest tests cover analytic fixture construction, a direct integer linear cross-correlation oracle, and real Manifold/FCL/meshoptimizer/pocketfft/JSON calls. See [native build instructions](../docs/BUILDING.md) for the locked Debug and Release presets and staged CPU diagnostic. Run [bounded reference timings](../benchmarks/README.md) separately from correctness tests.

These executable checks establish reusable testing tools. Product implementations and their acceptance gates remain planned in [TEST_PLAN.md](TEST_PLAN.md); no skipped placeholder tests count as acceptance. The supplied `rc/` source files remain unchanged.
