# SpectraPack desktop frontend

This React/Three.js application uses only the narrow Rust commands in
ADR 0010. Run `pnpm desktop:dev` from the repository root for the browser
preview at `http://127.0.0.1:1420`. Native import, solve and file actions
require the Tauri application; the browser preview never fabricates results.
Run `pnpm desktop:native dev` or `pnpm desktop:native build` after the native
engine staging steps are complete.

Checks: `pnpm desktop:test`, `pnpm desktop:check`, `pnpm desktop:build`, and
`pnpm contracts:check`. On the sandbox host, use `$env:CI='true'` and
`pnpm --config.verifyDepsBeforeRun=false <script>` to avoid pnpm's implicit
dependency-store purge prompt. Registry-pinned dependencies are locked in
the root pnpm lockfile.

Frontend evidence (2026-09-30): the first `desktop:test` run failed four
behavior assertions for missing pose conversion, hiding, stale-state rejection,
and uint64/pitch validation. The current run passes eight tests, including a
pending-settings Save test with a mocked command bridge. The latter establishes
frontend presentation only, not native geometry or file acceptance.
The off-origin inch-source coordinate oracle is hand-computed independently;
instance rendering consumes already-local millimeter PLY without recentering.
An actual native PLY fixture also exercises Three.js's upload adapter: native
Float64 coordinates become finite float32 display attributes without changing
poses or source data. Overflow and render-loop failures show a viewer error.

The final build includes TypeScript checking and passes. Contract checking
passes the original 31 fixtures, 25 desktop-definition fixtures, generated
types and the native schema embedding check. Complete logs are under
`.local/t008/frontend/` (`red.log`, `green-final.log`, `build-final.log`,
`contracts.log`). Vite reports an approximately 817 kB bundle warning for the
combined React/Three.js application. Native workflow and qualified GPU
performance acceptance are separate gates tracked in `doc/T-008.md`.
