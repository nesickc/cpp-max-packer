# T011 retained-session measurement

`t011_retained_session.py` uses Python's standard library and Windows QPC/process
memory APIs. Coordinate the single CPU slot before launching an engine. Supply a
held Release executable and its exact SHA-256; a fresh output directory is required.

```powershell
& $pythonExe -W error::ResourceWarning benchmarks/runtime/t011_retained_session.py `
  --engine $heldReleaseExe --engine-sha256 $heldSha256 `
  --output .local/t010/runtime-benchmark/new-series --check-inputs
```

Remove `--check-inputs` for the four-profile series, after the integrated executable
is pinned and CPU handoff is granted. `--smoke` runs one small analytic cold/retained
pair without target verdicts; `--profile ID` selects a subset. An incomplete profile
set cannot close the four-profile gate. `--report ID=PATH` and `--old-root PATH`
override immutable input locations. Missing inputs fail; no reports are rewritten.

Each full profile has six fresh-process cold runs (one warmup, five samples), then
an excluded bootstrap prepare and six repeated prepares/Starts in one retained
process. Cold means fresh native context; **OS file cache is not reset**. Analytic
T011 uses the frozen 22×18×12 mm box, distinct from native T010's 38×28×20 field box.
Work stays CPU thread 1, one candidate, one pass, frozen pitch/catalog/clearances.

Preparation total includes native prepare, required old-token release and harness
overhead through terminal receipt; cold also includes process startup. Command and
release durations are recorded separately. Combined fixed-work Start includes
preparation and run through receipt of the complete terminal NDJSON record. QPC
is captured before run-envelope logging/sending. Host receipt, native search-only,
saved pre-commit runtime and native pre-response/completion times are distinct.
Successful session terminals may expose completion fields without a nested runtime
record; the harness retains that absence explicitly rather than borrowing saved
pre-commit timings. Completion fields must still be finite and nonnegative.
Result readback/checking and shutdown are outside Start. The old frozen total runs
through process exit and includes its polling/reclamation overhead; preserve that
boundary distinction when interpreting a ratio.

Native valid status, exact source/accepted/catalog identities, work counts and
ordered poses must match the old baseline in every repetition. Python verifies
these records and hashes actual artifact bytes; it does not validate geometry.

`memory.jsonl` retains 10 ms observations and operation boundaries. Windows
PeakWorkingSetSize and PeakPagefileUsage are historical process high-water marks,
including earlier operations, not resettable request peaks. Profile maxima include
warmups/bootstrap/shutdown and are compared with frozen old maxima plus 64 MiB.
At least one valid memory observation is required; an unavailable zero measurement
cannot pass. Unavailable observations after process exit remain in the log.
Native tracked payload estimates remain separate from measured process memory.

Evidence includes invocation/exit, raw wire/stderr, receive timestamps, every
request/response/phase, timings, saved result path/hash, sample failures, input and
engine hashes before/after, raw samples and medians. Existing evidence is refused;
rerun into a new directory. Watchdog expiry or forced kill is failure, never
cooperative Stop success. A full series exits nonzero on any missed frozen target;
completed measurements remain available with an explicit verdict. This script
does not qualify full24 Ulamok, parallel speed, Stop phases or the desktop journey.
