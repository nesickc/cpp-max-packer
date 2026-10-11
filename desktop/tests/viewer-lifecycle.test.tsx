import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Camera, Scene } from 'three';
import { Box3, Color, InstancedMesh, Matrix4, Mesh, MeshBasicMaterial } from 'three';
import type { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import type { Desktop, Results } from '../src/generated/contracts';
import fixtures from '../../tests/contracts/schema-fixtures.json';
import nativePreview from './fixtures/cube10mm-double.json';

const device = vi.hoisted(() => ({ created: vi.fn(), disposed: vi.fn(), render: vi.fn(), loop: null as (() => void) | null, controls: [] as OrbitControls[] }));
const native = vi.hoisted(() => ({ state: vi.fn(), preview: vi.fn() }));
vi.mock('../src/bridge', () => ({ nativeAvailable: () => true, bridge: native }));
vi.mock('three', async importOriginal => {
  const actual = await importOriginal<typeof import('three')>();
  return { ...actual, WebGLRenderer: class {
    domElement = document.createElement('canvas');
    constructor() { device.created(); }
    setPixelRatio() {}
    setSize() {}
    setAnimationLoop(callback: (() => void) | null) { device.loop = callback; }
    render = device.render;
    dispose = device.disposed;
  } };
});
vi.mock('three/addons/controls/OrbitControls.js', async importOriginal => {
  const actual = await importOriginal<typeof import('three/addons/controls/OrbitControls.js')>();
  return { OrbitControls: class extends actual.OrbitControls {
    constructor(...args: ConstructorParameters<typeof actual.OrbitControls>) { super(...args); device.controls.push(this); }
  } };
});
import { App } from '../src/App';
import { Viewer } from '../src/Viewer';

const bytes = Uint8Array.from(atob(nativePreview.ply_base64), c => c.charCodeAt(0)).buffer;
const preview: NonNullable<Desktop.ImportedObject['preview']> = { preview_id: 'cube', format: 'ply', sha256: nativePreview.sha256, byte_length: bytes.byteLength, triangle_count: 12, coordinate_frame: 'object_local_mm' };
const pose: Results.Placement = { copy_id: 'cube-1', translation_mm: [5,5,5], quaternion_xyzw: [0,0,0,1], local_to_world: [[1,0,0,5],[0,1,0,5],[0,0,1,5],[0,0,0,1]] };
let frames: Map<number, FrameRequestCallback>;
let nextFrame: number;
async function settle() { await act(async () => { await Promise.resolve(); }); }
function frame() {
  const callbacks = [...frames.values()]; frames.clear();
  act(() => { callbacks.forEach(callback => callback(0)); device.loop?.(); });
}
function drain() {
  for (let i = 0; i < 500 && (frames.size || device.loop); i++) frame();
}
function visibleMesh() {
  const scene = device.render.mock.lastCall![0] as Scene;
  return scene.children.find(child => child instanceof InstancedMesh) as InstancedMesh | undefined;
}
beforeEach(() => {
  vi.useFakeTimers(); frames = new Map(); nextFrame = 0; device.controls = []; device.loop = null;
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
  vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => { frames.set(++nextFrame, callback); return nextFrame; });
  vi.stubGlobal('cancelAnimationFrame', (id: number) => frames.delete(id));
  native.preview.mockResolvedValue(bytes);
});
afterEach(() => { cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); vi.clearAllMocks(); });

describe('UI-02/UI-03 / AT-15 persistent viewer (device adapter, real PLY and controls)', () => {
  it.each(['object', 'result'] as const)('keeps a deferred %s preview installed across cloned progress revisions', async mode => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    document.container = { kind: 'box', dimensions_mm: [40,40,40] }; document.placements = [pose]; document.count = 1;
    const initial: Desktop.State = { desktop_version: 1, session_id: 'viewer-progress', revision: 1, object: { id: 'object', display_name: 'Cube.stl', report: document.assets.object, preview }, draft_settings: { desktop_version: 1, box_dimensions_mm: [40,40,40], clearance_mm: { pair: 0, wall: 0 }, orientation: { mode: 'fixed', quaternion_xyzw: [0,0,0,1] }, pitch_mm: 2, budget_seconds: 60, seed: '0' }, result: mode === 'result' ? { id: 'result', document, preview, origin: 'solve' } : null, operation: null, last_error: null };
    native.state.mockResolvedValue(initial);
    let resolve!: (value: ArrayBuffer) => void;
    native.preview.mockImplementation(() => new Promise<ArrayBuffer>(done => { resolve = done; }));
    render(<App />); await settle();
    expect(native.preview).toHaveBeenCalled();
    for (let revision = 2; revision <= 5; revision++) {
      const progress = structuredClone(initial); progress.revision = revision;
      progress.operation = { id: 'solve', kind: 'solve', phase: 'placing', started_at: '2026-10-10T00:00:00Z', finished_at: null, result_id: null, detail: `Progress ${revision}` };
      native.state.mockResolvedValue(progress);
      await act(async () => { await vi.advanceTimersByTimeAsync(250); });
    }
    expect(native.preview).toHaveBeenCalledOnce(); expect(device.created).toHaveBeenCalledOnce();
    await act(async () => { resolve(bytes); }); drain();
    expect(screen.queryByText('Loading shared display mesh…')).toBeNull();
    expect(visibleMesh()?.count).toBe(1);
    const installed = visibleMesh(); const camera = device.render.mock.lastCall![1] as Camera;
    const position = camera.position.clone();
    native.state.mockResolvedValue({ ...structuredClone(initial), revision: 6 });
    await act(async () => { await vi.advanceTimersByTimeAsync(250); }); drain();
    expect(visibleMesh()).toBe(installed); expect(camera.position.equals(position)).toBe(true);
    expect(screen.queryByText('Loading shared display mesh…')).toBeNull();
    expect(native.preview).toHaveBeenCalledOnce(); expect(device.disposed).not.toHaveBeenCalled();
  });

  it('updates poses, visibility, selection, clipping, dimensions and projection without refetching geometry', async () => {
    const props = { mode: 'result' as const, preview, placements: [pose], dimensions: [40,40,40] as [number,number,number], hidden: new Set<string>(), selected: null as string | null, onSelect: vi.fn() };
    const view = render(<Viewer {...props} />); await settle(); drain();
    const geometry = visibleMesh()!.geometry;
    const source = JSON.stringify(props.placements);
    const camera = device.render.mock.lastCall![1] as Camera; const position = camera.position.clone();
    const moved = { ...pose, translation_mm: [15,5,5] as [number,number,number] };
    view.rerender(<Viewer {...props} placements={[moved]} selected="cube-1" dimensions={[50,40,40]} onSelect={vi.fn()} />); drain();
    const matrix = new Matrix4(); visibleMesh()!.getMatrixAt(0, matrix);
    expect(matrix.elements[12]).toBe(15); expect(visibleMesh()!.geometry).toBe(geometry);
    const color = new Color(); visibleMesh()!.getColorAt(0, color);
    expect(color.getHexString()).toBe('ffbb6b');
    const scene = device.render.mock.lastCall![0] as Scene;
    const box = scene.children.find(child => child instanceof Mesh && child.material instanceof MeshBasicMaterial) as Mesh;
    expect(new Box3().setFromObject(box).max.toArray()).toEqual([50,40,40]);
    expect(camera.position.distanceTo(position)).toBeLessThan(1e-9);
    fireEvent.change(screen.getByLabelText('Clipping height'), { target: { value: '50' } });
    fireEvent.change(screen.getByLabelText('Container transparency'), { target: { value: '25' } }); drain();
    const material = visibleMesh()!.material as import('three').MeshStandardMaterial;
    expect(material.clippingPlanes![0].constant).toBe(20);
    expect((box.material as MeshBasicMaterial).opacity).toBe(.25);
    view.rerender(<Viewer {...props} hidden={new Set(['cube-1'])} />); drain();
    expect(visibleMesh()!.count).toBe(0);
    fireEvent.click(screen.getByRole('button', { name: 'Perspective' })); drain();
    expect((device.render.mock.lastCall![1] as import('three').OrthographicCamera).isOrthographicCamera).toBe(true);
    expect(native.preview).toHaveBeenCalledOnce(); expect(device.created).toHaveBeenCalledOnce();
    expect(JSON.stringify(props.placements)).toBe(source);
    view.unmount(); expect(device.disposed).toHaveBeenCalledOnce(); expect(frames.size).toBe(0);
  });

  it('loads a changed preview once and ignores an obsolete response', async () => {
    const pending = new Map<string, (data: ArrayBuffer) => void>();
    native.preview.mockImplementation(({ preview_id }: { preview_id: string }) => new Promise<ArrayBuffer>(resolve => pending.set(preview_id, resolve)));
    const props = { mode: 'result' as const, preview, placements: [pose], dimensions: [40,40,40] as [number,number,number], hidden: new Set<string>(), selected: null, onSelect: vi.fn() };
    const view = render(<Viewer {...props} />);
    view.rerender(<Viewer {...props} preview={{ ...preview, preview_id: 'new-cube' }} />);
    await act(async () => { pending.get('new-cube')!(bytes); }); drain();
    const installed = visibleMesh(); expect(installed?.count).toBe(1);
    await act(async () => { pending.get('cube')!(new ArrayBuffer(0)); }); drain();
    expect(visibleMesh()).toBe(installed); expect(screen.queryByRole('alert')).toBeNull();
    expect(native.preview).toHaveBeenCalledTimes(2);
    view.rerender(<Viewer {...props} preview={{ ...preview, preview_id: 'unmounted-cube' }} />);
    view.unmount();
    const renders = device.render.mock.calls.length;
    await act(async () => { pending.get('unmounted-cube')!(bytes); }); drain();
    expect(device.render).toHaveBeenCalledTimes(renders); expect(frames.size).toBe(0);
  });

  it('renders changes and real orbit damping until settled, then stops scheduling idle frames', async () => {
    render(<Viewer mode="result" preview={preview} placements={[pose]} dimensions={[40,40,40]} hidden={new Set()} selected={null} onSelect={() => {}} />);
    await settle(); drain();
    const idle = device.render.mock.calls.length; frame(); frame();
    expect(device.render).toHaveBeenCalledTimes(idle);
    const controls = device.controls[0]; const before = controls.getAzimuthalAngle();
    act(() => controls.rotateLeft(.5)); frame();
    const first = controls.getAzimuthalAngle(); frame();
    expect(controls.getAzimuthalAngle()).not.toBe(first); expect(first).not.toBe(before);
    drain(); expect(frames.size).toBe(0); expect(device.loop).toBeNull();
    const settled = device.render.mock.calls.length; frame(); frame();
    expect(device.render).toHaveBeenCalledTimes(settled); expect(settled).toBeGreaterThan(idle + 2);
  });
});
