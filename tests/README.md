# Executable tests

Run `python -m unittest discover -s tests -p 'test_*.py'` with Python 3.12 or later. These tests check the STL inventory helper, pinned reference manifest, CPU build/dependency/runtime guards, and performance-report harness. They use only the Python standard library.

The C++20 Catch2/CTest tests cover analytic fixture construction, a direct integer
linear cross-correlation oracle, real dependency calls, versioned contracts,
STL/frame parsing, exact geometric predicates, shell analysis and immutable repair
acceptance. Process tests exercise the service handshake and source-preserving
inspection CLI. See [native build instructions](../docs/BUILDING.md) for the locked
Debug and Release presets and staged package checks. Run
[bounded timings](../benchmarks/README.md) separately from correctness tests.

Foundation checks establish reusable testing tools; the product slices they
support are tracked separately in [TEST_PLAN.md](TEST_PLAN.md) and the linked
delivery tickets. No skipped placeholder counts as acceptance. The supplied
`rc/` files remain unchanged; practical import qualification requires reviewed
diagnostic outcomes and full-resolution artifact checks.
