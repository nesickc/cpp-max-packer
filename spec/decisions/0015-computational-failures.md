# ADR 0015: allocation-free computational diagnostics

Date: 2026-10-02. Status: accepted for the T-011 simplification.

Actual MSVC Debug allocation-failure runs terminate while moving string-owning
computational diagnostics. Repairing each copy/return adds recovery machinery
around descriptors that are predominantly literals. This revises ADR 0013's
requirement to preserve those native DTO types/layouts and use allocating copies
and outcome finalizers. SOL-05/SOL-06, resource caps and incumbent validity remain.

1. Carry static-lifetime `std::string_view` descriptors through `KernelFailure`
   (code/method), `RepresentationFailure` (code/message plus optional static method),
   `CorrelationFailure` and `CatalogFailure` (code/message), `ResourceLimitDetails` (resource), and
   `RunFailureDetails` (phase/cause). Numeric fields and failure classification
   remain. Never borrow temporary strings, input buffers, IDs or `error.what()`.
   Correlation's native-only arbitrary exception prose becomes the stable message
   `Correlation operation failed.`; its code and stats remain. Dynamic import,
   repair and validation reports retain owned payloads.
2. These failure values and empty solver outcomes require no heap allocation.
   Remove diagnostic-copy recovery layers and their string/proxy accounting.
   Ordinary value returns must retain the exact latest validated handles and
   counters under transient and persistent allocation failure; no trial publishes.
3. Vector transfer remains a separate concern on pinned MSVC Debug. Make the
   private ranked page lazy owned storage, transferring its pointer. Admit its
   wrapper, proxy and capacity before its throwing construction. Preserve the
   public correlation and orientation-catalog vector payloads using narrow throwing
   empty construction plus swap transfer, including variant/optional assembly;
   account simultaneous proxies. Use the existing cube seed array instead of an
   intermediate vector for catalog construction/verification. Remove the private
   implicit-catalog pipeline overload, which has no production callers; preserve
   both public solver APIs. Do not introduce a generic container/allocator framework.
4. Admit the fixed simultaneous control/result metadata before dynamic work.
   Below-bound and early failure outcomes must themselves remain reportable without
   allocation. Charge actual remaining owners, including IDs and buffers; remove
   only eliminated diagnostic allocations. Planned admission and observed peaks
   remain distinct, and all caps stay unchanged. Admit raw catalog capacity and
   simultaneous owners before construction, including calls without an initial
   Stop. A refused page reservation must not raise an observed working peak;
   preserve actual partial-construction observations separately from admission.
5. Existing CLI/I/O adapters materialize the current structured wire diagnostics
   from these descriptors and numeric values. Preserve wire fields/codes, schema
   versions and termination mapping, including catalog allocation as a resource
   failure. Formatting failure cannot mutate the native
   outcome or authorize publication. A new terminal serializer is outside this
   checkpoint; full adapter-memory and lifecycle qualification remains required.

This is an intentional native C++ type/layout/ownership revision. Rebuild and
migrate repository consumers together; dynamic-string assignments and correlation/
catalog aggregate construction may require changes. No ABI compatibility is claimed for
these native types. Physical geometry, serialized project compatibility and
authority are unchanged. Keep genuine allocation catches and transactional state
rollback; do not implement the superseded baseline/spectral diagnostic finalizers.

Use the existing real field diagnostic-ordinal abort as red evidence. Verify the
field hook is reached, specific codes/numbers and exact retained handles survive,
non-elided failure transfers allocate nothing, and real correlation/page transfers
handle allocation failure. Restore fault hooks before test reporting. Run affected
Debug/Release field, compute, solver and adapter diagnostic regressions. This
decision does not close those gates or the separate practical/performance gates.

Focused critical review found the remaining catalog allocation/transfer and page
peak gaps after the field-failure regressions passed. The catalog additions above
are the bounded response to that evidence; they do not authorize migration of
unrelated diagnostic types or a general container rewrite. Retain causal catalog
and refused-page reproducers, then rerun affected checks and review the correction.
