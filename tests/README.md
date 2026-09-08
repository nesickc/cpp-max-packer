# Test foundation

Run `python -m unittest discover -s tests -p 'test_*.py'` with Python 3.12 or later. These tests check the STL inventory helper, pinned reference manifest, and performance-report harness. They use only the Python standard library.

The C++20 Catch2/CTest tests cover analytic fixture construction and a direct integer linear cross-correlation oracle. See [native build instructions](../docs/BUILDING.md) for the Debug and Release presets. Run [bounded reference timings](../benchmarks/README.md) separately from correctness tests.

These executable checks establish reusable testing tools. Product implementations and their acceptance gates remain planned in [TEST_PLAN.md](TEST_PLAN.md); no skipped placeholder tests count as acceptance. The supplied `rc/` source files remain unchanged.
