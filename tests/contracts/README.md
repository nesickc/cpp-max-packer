# Version-1 contract fixtures

The shared corpus covers requested/resolved settings, inspected/accepted assets,
zero/one-copy results, protocol envelopes, and completed/failed benchmark records.
These are small synthetic records with explicit expected outcomes. Their hashes
are illustrative identities; no referenced file or authoritative solid is verified.

Each record in `schema-fixtures.json` contains `name`, `kind`, `value`,
`schema_valid`, and `semantic_valid`. Ajv checks structural expectations. The native
contract test reads the same file and checks both stages: a schema-valid semantic
negative must fail semantic validation. Expected outcomes are written explicitly,
not generated from the current validator. `build-fixtures.cjs` constructs the
records from the analytic native zero-result fixture and deliberate mutations.

From the repository root:

```powershell
pnpm install --frozen-lockfile --ignore-scripts
pnpm contracts:check
```

This checks the embedded C++ catalog, generated TypeScript freshness, the Ajv corpus,
and positive/negative TypeScript compile fixtures. The regular native Debug/Release
wrapper runs the shared corpus and additional source-frame, pose, run-history,
parser, registry, and service regressions; see [build instructions](../../docs/BUILDING.md).

After an intentional schema or fixture change:

```powershell
node tests/contracts/build-fixtures.cjs
pnpm contracts:generate
pnpm contracts:check
```

Review changed expectations before rerunning native tests. Contract validity does
not establish geometric feasibility, file integrity, project portability, or full
AT-13/AT-14 acceptance.
