import { describe, expect, it } from 'vitest';
import { BoxGeometry, Vector3 } from 'three';
import type { Desktop, Results } from '../src/generated/contracts';
import { acceptState, defaultSettings, settingsError } from '../src/state';
import { packingMesh, poseMatrix } from '../src/transforms';

const pose: Results.Placement = { copy_id: 'copy-quarter-turn', translation_mm: [13, 17, 23], quaternion_xyzw: [0, 0, Math.SQRT1_2, Math.SQRT1_2], local_to_world: [[0, -1, 0, 13], [1, 0, 0, 17], [0, 0, 1, 23], [0, 0, 0, 1]] };
const state = (session_id: string, revision: number): Desktop.State => ({ desktop_version: 1, session_id, revision, object: null, draft_settings: null, result: null, operation: null, last_error: null });

describe('UI-02 / AT-14 viewer independent coordinate oracle', () => {
  it('rotates an already-local, asymmetric point once with unit scale', () => {
    // Source in inches (5,9,13), source center (4,7,10), scale 25.4:
    // PLY local point (25.4,50.8,76.2); Z quarter turn + translation:
    // (-50.8+13,25.4+17,76.2+23) = (-37.8,42.4,99.2).
    const matrix = poseMatrix(pose);
    const p = new Vector3(25.4, 50.8, 76.2).applyMatrix4(matrix);
    expect(p.x).toBeCloseTo(-37.8, 10); expect(p.y).toBeCloseTo(42.4, 10); expect(p.z).toBeCloseTo(99.2, 10);
    expect(matrix.determinant()).toBeCloseTo(1, 12);
    new Vector3().setFromMatrixScale(matrix).toArray().forEach(scale => expect(scale).toBeCloseTo(1, 12));
  });
  it('shares geometry and hides by stable ID without changing export poses/count', () => {
    const poses = [pose, { ...pose, copy_id: 'copy-other' }];
    const before = JSON.stringify(poses); const geometry = new BoxGeometry(2, 4, 6);
    const { mesh, ids } = packingMesh(geometry, poses, new Set(['copy-quarter-turn']));
    expect(mesh.geometry).toBe(geometry); expect(mesh.count).toBe(1); expect(ids).toEqual(['copy-other']);
    expect(JSON.stringify(poses)).toBe(before); expect(poses.length).toBe(2);
    const scaled = packingMesh(geometry, Array.from({ length: 1000 }, (_, i) => ({ ...pose, copy_id: `id-${i}` })), new Set());
    expect(scaled.mesh.geometry).toBe(geometry); expect(scaled.mesh.count).toBe(1000);
  });
});
describe('UI-01/UI-03 immutable routing and settings', () => {
  it('rejects older revisions and another session without explicit New', () => {
    const current = state('session-new', 7);
    expect(acceptState(current, state('session-new', 6))).toBe(current);
    expect(acceptState(current, state('session-old', 99))).toBe(current);
    expect(acceptState(current, state('session-new', 8)).revision).toBe(8);
  });
  it('retains independent default clearances and exact uint64 seeds', () => {
    expect(defaultSettings.clearance_mm).toEqual({ pair: 1, wall: 1 });
    expect(settingsError({ ...defaultSettings, seed: '18446744073709551615' })).toBeNull();
    expect(settingsError({ ...defaultSettings, seed: '18446744073709551616' })).toMatch(/seed/i);
    expect(settingsError({ ...defaultSettings, pitch_mm: 0 })).toMatch(/pitch/i);
    expect(settingsError({ ...defaultSettings, box_dimensions_mm: [1, NaN, 3] })).toMatch(/box/i);
    expect(settingsError({ ...defaultSettings, clearance_mm: { pair: 0, wall: 0 } })).toBeNull();
  });
});
