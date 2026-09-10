import type {
  BenchmarkSummary,
  Common,
  Protocol,
  Results,
  Settings,
} from '../../desktop/src/generated/contracts.js';

const hash = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
const identity: Common.Matrix4 = [
  [1, 0, 0, 0],
  [0, 1, 0, 0],
  [0, 0, 1, 0],
  [0, 0, 0, 1],
];
const point: Common.Vec3 = [1, 2, 3];
const rotation: Common.Quaternion = [0, 0, 0, 1];

// @ts-expect-error vec3 has exactly three values
const malformedPoint: Common.Vec3 = [1, 2];
// @ts-expect-error quaternion has exactly four values
const malformedRotation: Common.Quaternion = [0, 0, 1];

const requested: Settings.Requested = {
  settings_version: 1,
  object_asset_id: 'asset-0123456789abcdef0123456789abcdef-1',
  container: { kind: 'box', dimensions_mm: [10, 10, 10] },
  clearance_mm: { pair: 0, wall: 0 },
  orientation: { mode: 'fixed' },
  search: { preset: 'test-v1', seed: '0', deterministic: false, budget_seconds: 1 },
  resolution: { mode: 'manual', pitch_mm: 1 },
  compute: { backend: 'auto' },
};

const resolved: Settings.Resolved = {
  settings_version: 1,
  object_asset: { source_sha256: hash, accepted_solid_sha256: hash },
  container: { kind: 'box', dimensions_mm: [10, 10, 10] },
  clearance_mm: { pair: 0, wall: 0 },
  orientation: { mode: 'fixed', quaternion_xyzw: rotation },
  search: {
    preset: 'test-v1',
    seed: '0',
    deterministic: true,
    work_budget: { max_candidate_evaluations: 1, max_search_passes: 1 },
  },
  resolution: { mode: 'manual', pitch_mm: 1 },
  compute: { backend: 'cpu' },
  resolved: {
    pitch_mm: 1,
    orientation_catalog_sha256: hash,
    orientation_catalog_version: 1,
    backend: 'cpu',
    thread_count: 1,
  },
};

const { object_asset_id: omittedRequestedAsset, ...requestedWithoutAsset } = requested;
// @ts-expect-error requested settings require object_asset_id
const invalidRequested: Settings.Requested = requestedWithoutAsset;

const resolvedWithMissingQuaternion = {
  ...resolved,
  orientation: { mode: 'fixed' as const },
};
// @ts-expect-error resolved fixed orientation requires quaternion_xyzw
const invalidResolved: Settings.Resolved = resolvedWithMissingQuaternion;

const protocolSuccess: Protocol.Success = {
  protocol_version: 1,
  request_id: 'request-1',
  ok: true,
  result: {},
};
const protocolFailure: Protocol.Failure = {
  protocol_version: 1,
  request_id: null,
  ok: false,
  error: { code: 'INVALID_JSON', message: 'invalid JSON', details: {}, recoverable: true },
};
const successWithoutResult = {
  protocol_version: 1 as const,
  request_id: 'request-2',
  ok: true as const,
};
// @ts-expect-error successful protocol responses require result metadata
const invalidSuccess: Protocol.Success = successWithoutResult;
const failureWithoutError = {
  protocol_version: 1 as const,
  request_id: null,
  ok: false as const,
};
// @ts-expect-error failed protocol responses require structured error metadata
const invalidFailure: Protocol.Failure = failureWithoutError;

const acceptedObject: Results.HttpsSpectrapackInvalidSchemasV1ResultsSchemaJson['assets']['object'] = {
  schema_version: 1,
  role: 'object',
  state: 'accepted',
  source: { sha256: hash, path: 'assets/object.stl', units: 'mm', unit_scale_mm: 1 },
  frame: {
    source_bounds: { min: [0, 0, 0], max: [1, 1, 1] },
    source_to_local: identity,
  },
  dimensions_mm: [1, 1, 1],
  diagnostics: { status: 'valid', messages: [] },
  accepted_solid: {
    sha256: hash,
    path: 'assets/object.ply',
    format: 'binary_little_endian_ply_f64_u32',
    vertex_count: 8,
    triangle_count: 12,
  },
  repair_record: null,
};

const work: Results.Work = { candidate_evaluations: 0, search_passes: 0 };
const segment: Results.Segment = {
  segment_id: 'segment-0',
  parent_solution_revision: null,
  seed: '0',
  backend: 'cpu',
  elapsed_seconds: 0,
  work_counts: work,
  search_state_reset: false,
  rng: { algorithm: 'synthetic', state: 'initial' },
  resolved_settings: resolved,
};

const result: Results.HttpsSpectrapackInvalidSchemasV1ResultsSchemaJson = {
  schema_version: 1,
  label: 'best_found',
  job_id: 'job-1',
  solution_revision: 0,
  created_at: '2026-09-10T00:00:00Z',
  engine: { version: 'test', commit: '0000000' },
  assets: { object: acceptedObject },
  container: { kind: 'box', dimensions_mm: [10, 10, 10] },
  constraints: {
    clearance_mm: { pair: 0, wall: 0 },
    orientation: { mode: 'fixed', quaternion_xyzw: rotation },
    orientation_catalog_sha256: hash,
  },
  search: {
    resolved_settings: resolved,
    pitch_levels_mm: [1],
    seed: '0',
    work_counts: work,
    run_segments: [segment],
    backend_transitions: [],
    elapsed_seconds: 0,
  },
  count: 0,
  placements: [],
  validation: {
    status: 'valid',
    tolerance_mm: 0,
    authoritative_geometry_sha256: [hash],
    validator_version: 'test',
    kernel_version: 'test',
    checks: { pair: 'valid', containment: 'valid', clearance: 'valid' },
  },
  metrics: {
    solid_volume_mm3: 1,
    container_volume_mm3: 1000,
    utilization: 0,
    time_to_best_seconds: 0,
    peak_host_bytes: 0,
    peak_device_bytes: 0,
    termination_reason: 'budget_exhausted',
  },
};

const { metrics: omittedMetrics, ...resultWithoutMetrics } = result;
// @ts-expect-error results require complete metrics metadata
const invalidResult: Results.HttpsSpectrapackInvalidSchemasV1ResultsSchemaJson = resultWithoutMetrics;

const benchmark: BenchmarkSummary.HttpsSpectrapackInvalidSchemasV1BenchmarkSummarySchemaJson = {
  schema_version: 1,
  suite_id: 'synthetic',
  created_at: '2026-09-10T00:00:00Z',
  engine: { version: 'test', commit: '0000000' },
  host: { cpu: 'test', gpu: null, driver: null, os_build: 'test', thread_count: 1 },
  dependency_revisions: { pocketfft: 'test' },
  configuration: { method: 'full_search', settings: resolved, cache_state: 'cold' },
  runs: [
    {
      seed: '0',
      status: 'completed',
      settings: resolved,
      count: 0,
      result: { path: 'results/result.json', sha256: hash },
      validation_failures: 0,
      timings: {
        preprocessing_seconds: 0,
        search_seconds: 0,
        validation_seconds: 0,
        export_seconds: 0,
        cold_end_to_end_seconds: 0,
      },
      peak_host_bytes: 0,
      peak_device_bytes: 0,
      improvements: [],
      backend_transitions: [],
    },
  ],
  summary: { successful_runs: 1, failed_runs: 0, best_count: 0, median_count: 0, worst_count: 0 },
};

const { host: omittedHost, ...benchmarkWithoutHost } = benchmark;
// @ts-expect-error benchmark summaries require host provenance metadata
const invalidBenchmark: BenchmarkSummary.HttpsSpectrapackInvalidSchemasV1BenchmarkSummarySchemaJson =
  benchmarkWithoutHost;

void point;
void rotation;
void malformedPoint;
void malformedRotation;
void omittedRequestedAsset;
void invalidRequested;
void invalidResolved;
void protocolSuccess;
void protocolFailure;
void invalidSuccess;
void invalidFailure;
void omittedMetrics;
void invalidResult;
void omittedHost;
void invalidBenchmark;
