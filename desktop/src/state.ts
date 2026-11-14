import type { Desktop } from './generated/contracts';

// Native version-1 cube45 catalog; also checked at the Rust publication boundary.
export const cube45CatalogHash = '7434d4e2eb8cd12e6a67af3a5d636a7435a4e50da043777586ff0fbff9eae4ef';

// No thread_count: until one is chosen, Start uses the shell's default_thread_count.
export const defaultSettings: Desktop.Settings = { desktop_version: 1, box_dimensions_mm: [100, 100, 100], box_tolerance_mm: 0.1, clearance_mm: { pair: 1, wall: 1 }, orientation: { mode: 'cube' }, pitch_mm: 2, budget_seconds: 60, seed: '0', budget_scope: 'total_start' };
// Field-by-field equality; the shell returns settings with its keys sorted.
const sortedKeys = (_: string, value: unknown) => value && typeof value === 'object' && !Array.isArray(value) ? Object.fromEntries(Object.entries(value).sort(([a], [b]) => a < b ? -1 : 1)) : value;
export function sameSettings(a: Desktop.Settings, b: Desktop.Settings): boolean { return JSON.stringify(a, sortedKeys) === JSON.stringify(b, sortedKeys); }
// Nova Post boxes, width x depth x height in millimetres. Only the 10 kg interior is
// measured; the others are the carrier's published sizes.
export const boxPresets: readonly { name: string; dimensions_mm: readonly [number, number, number] }[] = [
  { name: 'Nova Post 0.5 kg', dimensions_mm: [170, 120, 90] },
  { name: 'Nova Post 1 kg', dimensions_mm: [240, 170, 90] },
  { name: 'Nova Post 2 kg', dimensions_mm: [340, 240, 90] },
  { name: 'Nova Post 3 kg', dimensions_mm: [240, 240, 200] },
  { name: 'Nova Post 5 kg', dimensions_mm: [400, 240, 200] },
  { name: 'Nova Post 10 kg', dimensions_mm: [400, 340, 285] },
  { name: 'Nova Post 15 kg', dimensions_mm: [600, 350, 280] },
  { name: 'Nova Post 20 kg', dimensions_mm: [470, 400, 420] },
  { name: 'Nova Post 30 kg', dimensions_mm: [700, 400, 420] },
];
// The preset whose size the box currently has, or '' for a custom size.
export function presetName(dimensions: readonly number[]): string {
  return boxPresets.find(p => p.dimensions_mm.every((v, i) => v === dimensions[i]))?.name ?? '';
}
export function acceptState(current: Desktop.State | null, incoming: Desktop.State): Desktop.State {
  if (current && (current.session_id !== incoming.session_id || incoming.revision <= current.revision)) return current;
  const old = current?.live_result, next = incoming.live_result;
  if (old && next && old.operation_id === next.operation_id) {
    if (old.settings_id !== next.settings_id || old.native_epoch !== next.native_epoch ||
        next.count < old.count || next.sequence <= old.sequence || next.solution_revision <= old.solution_revision)
      return { ...incoming, live_result: old };
  }
  if (next && next.operation_id !== old?.operation_id && next.operation_id !== incoming.operation?.id)
    return { ...incoming, live_result: old ?? null };
  return incoming;
}
export function settingsError(settings: Desktop.Settings): string | null {
  const positive = (value: number) => Number.isFinite(value) && value > 0;
  if (!settings.box_dimensions_mm.every(positive)) return 'Box dimensions must be positive millimeters.';
  if (settings.box_tolerance_mm !== undefined && !(Number.isFinite(settings.box_tolerance_mm) && settings.box_tolerance_mm >= 0)) return 'Box tolerance must be zero or positive millimeters.';
  if (!Object.values(settings.clearance_mm).every(v => Number.isFinite(v) && v >= 0)) return 'Clearances must be zero or positive.';
  if (!positive(settings.pitch_mm)) return 'Pitch must be positive millimeters.';
  if (!positive(settings.budget_seconds)) return 'Search budget must be positive seconds.';
  if (!Number.isInteger(settings.thread_count ?? 1) || (settings.thread_count ?? 1) < 1 || (settings.thread_count ?? 1) > 8) return 'CPU threads must be an integer from 1 through 8.';
  if (!/^(0|[1-9][0-9]{0,19})$/.test(settings.seed) || BigInt(settings.seed) > 18446744073709551615n) return 'Seed must be a decimal uint64 (0–18446744073709551615).';
  if (settings.host_memory_limit_bytes !== undefined && (!/^[1-9][0-9]{0,19}$/.test(settings.host_memory_limit_bytes) || BigInt(settings.host_memory_limit_bytes) > 18446744073709551615n)) return 'Host memory ceiling must be a positive uint64 byte count.';
  if (settings.orientation.mode === 'fixed' && (!settings.orientation.quaternion_xyzw.every(Number.isFinite) || Math.hypot(...settings.orientation.quaternion_xyzw) < 1e-12)) return 'Fixed quaternion must be finite and nonzero.';
  return null;
}
export function activeOperation(state: Desktop.State | null): boolean { return !!state?.operation && !state.operation.finished_at; }
// The validated interior: each nominal dimension plus the allowed tolerance.
export function effectiveDimensions(settings: Desktop.Settings): [number, number, number] {
  const tolerance = Number.isFinite(settings.box_tolerance_mm) && settings.box_tolerance_mm! > 0 ? settings.box_tolerance_mm! : 0;
  return settings.box_dimensions_mm.map(v => v + tolerance) as [number, number, number];
}
export interface SearchFacts { pitch_mm: number; coarsened_to_mm?: number; threads: number; estimated_bytes?: string; host_cap_bytes?: string; recoveries?: string; remaining_seconds?: number; since_improvement_seconds?: number }
// What the search panel shows. A running solve, or one that ended without a result,
// is described by its Start settings, the engine's live resource status and the
// desktop's clock (`improvedAt`: operation seconds when the count last rose). A
// result is described by its own document: resolved pitch and threads, budget
// left at its end and time since its best count. The live memory status belongs
// to the last solve, so a result shows it only if that solve produced it
// (`solvedResultId`).
export function searchFacts(state: Desktop.State | null, elapsed: number, improvedAt?: number, solvedResultId?: string | null): SearchFacts | null {
  const operation = state?.operation, result = state?.result, sent = state?.draft_settings, resource = state?.cpu_runtime?.resource_status;
  const memory = { estimated_bytes: resource?.estimated_working_bytes, host_cap_bytes: resource?.admitted_host_cap_bytes,
    recoveries: resource && resource.recovery_count !== '0' ? `${resource.recovery_count}${resource.last_action ? `, last ${resource.last_action.replaceAll('_', ' ')} (${resource.last_cause})` : ''}` : undefined };
  if (operation?.kind === 'solve' && sent && (!operation.finished_at || !result))
    return { pitch_mm: sent.pitch_mm, threads: resource?.actual_threads ?? sent.thread_count ?? 1, ...memory, remaining_seconds: Math.max(0, sent.budget_seconds - elapsed), since_improvement_seconds: improvedAt === undefined ? undefined : Math.max(0, elapsed - improvedAt) };
  if (!result) return null;
  const search = result.document.search, resolved = search.resolved_settings.resolved, budget = search.resolved_settings.search.budget_seconds;
  const coarsest = Math.max(...search.pitch_levels_mm);
  const used = search.resolved_settings.search.budget_scope === 'total_start' ? search.runtime?.total_elapsed_seconds ?? search.elapsed_seconds : search.elapsed_seconds;
  return { pitch_mm: resolved.pitch_mm, ...(coarsest > resolved.pitch_mm ? { coarsened_to_mm: coarsest } : {}), threads: search.diagnostics?.resource_recovery?.actual_final_threads ?? resolved.thread_count,
    ...(result.id === solvedResultId ? memory : {}), remaining_seconds: budget === undefined ? undefined : Math.max(0, budget - used),
    since_improvement_seconds: Math.max(0, search.elapsed_seconds - result.document.metrics.time_to_best_seconds) };
}
// Memory in MiB below 1 GiB, else GiB.
export function memoryText(bytes: string): string {
  const value = Number(bytes);
  return value < 2 ** 30 ? `${(value / 2 ** 20).toFixed(1)} MiB` : `${(value / 2 ** 30).toFixed(2)} GiB`;
}
export function snapshotSettings(result: Desktop.ResultSnapshot): Desktop.Settings {
  const s = result.document.search.resolved_settings;
  return { desktop_version: 1, box_dimensions_mm: s.container.kind === 'box' ? [...s.container.dimensions_mm] : [100,100,100], ...(s.container.kind === 'box' && s.container.tolerance_mm !== undefined ? { box_tolerance_mm: s.container.tolerance_mm } : {}), clearance_mm: { ...s.clearance_mm }, orientation: s.orientation.mode === 'custom' && s.resolved.orientation_catalog_sha256 === cube45CatalogHash ? { mode: 'cube45' } : s.orientation.mode === 'cube' ? { mode: 'cube' } : { mode: 'fixed', quaternion_xyzw: s.orientation.mode === 'fixed' ? [...s.orientation.quaternion_xyzw] : [0,0,0,1] }, pitch_mm: s.resolved.pitch_mm, budget_seconds: s.search.budget_seconds ?? 60, seed: s.search.seed, thread_count: s.resolved.thread_count, budget_scope: s.search.budget_scope ?? 'search_only', ...(s.compute.host_memory_limit_bytes ? {host_memory_limit_bytes:s.compute.host_memory_limit_bytes} : {}) };
}
