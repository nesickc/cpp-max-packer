import type { Desktop } from './generated/contracts';

export const defaultSettings: Desktop.Settings = { desktop_version: 1, box_dimensions_mm: [100, 100, 100], clearance_mm: { pair: 1, wall: 1 }, orientation: { mode: 'fixed', quaternion_xyzw: [0, 0, 0, 1] }, pitch_mm: 2, budget_seconds: 60, seed: '0', thread_count: 1, budget_scope: 'total_start' };
export function acceptState(current: Desktop.State | null, incoming: Desktop.State): Desktop.State {
  if (current && (current.session_id !== incoming.session_id || incoming.revision <= current.revision)) return current;
  return incoming;
}
export function settingsError(settings: Desktop.Settings): string | null {
  const positive = (value: number) => Number.isFinite(value) && value > 0;
  if (!settings.box_dimensions_mm.every(positive)) return 'Box dimensions must be positive millimeters.';
  if (!Object.values(settings.clearance_mm).every(v => Number.isFinite(v) && v >= 0)) return 'Clearances must be zero or positive.';
  if (!positive(settings.pitch_mm)) return 'Pitch must be positive millimeters.';
  if (!positive(settings.budget_seconds)) return 'Search budget must be positive seconds.';
  if (!Number.isInteger(settings.thread_count ?? 1) || (settings.thread_count ?? 1) < 1 || (settings.thread_count ?? 1) > 8) return 'CPU threads must be an integer from 1 through 8.';
  if (!/^(0|[1-9][0-9]{0,19})$/.test(settings.seed) || BigInt(settings.seed) > 18446744073709551615n) return 'Seed must be a decimal uint64 (0–18446744073709551615).';
  if (settings.orientation.mode === 'fixed' && (!settings.orientation.quaternion_xyzw.every(Number.isFinite) || Math.hypot(...settings.orientation.quaternion_xyzw) < 1e-12)) return 'Fixed quaternion must be finite and nonzero.';
  return null;
}
export function activeOperation(state: Desktop.State | null): boolean { return !!state?.operation && !state.operation.finished_at; }
export function snapshotSettings(result: Desktop.ResultSnapshot): Desktop.Settings {
  const s = result.document.search.resolved_settings;
  return { desktop_version: 1, box_dimensions_mm: result.document.container.kind === 'box' ? result.document.container.dimensions_mm : [100,100,100], clearance_mm: { ...s.clearance_mm }, orientation: s.orientation.mode === 'cube' ? { mode: 'cube' } : { mode: 'fixed', quaternion_xyzw: s.orientation.mode === 'fixed' ? [...s.orientation.quaternion_xyzw] : [0,0,0,1] }, pitch_mm: s.resolved.pitch_mm, budget_seconds: s.search.budget_seconds ?? 60, seed: s.search.seed, thread_count: s.resolved.thread_count, budget_scope: s.search.budget_scope ?? 'search_only' };
}
