import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { Desktop } from '../src/generated/contracts';
import fixtures from '../../tests/contracts/schema-fixtures.json';
import { defaultSettings } from '../src/state';

const native = vi.hoisted(() => ({ state: vi.fn(), save: vi.fn(), start: vi.fn(), newProject: vi.fn() }));
vi.mock('../src/bridge', () => ({ nativeAvailable: () => true, bridge: native }));
vi.mock('../src/Viewer', () => ({ Viewer: (props: { dimensions: number[]; preview: { preview_id: string } | null }) => <div data-testid="viewer">{props.dimensions.join(',')} / {props.preview?.preview_id}</div> }));
import { App } from '../src/App';

afterEach(() => { cleanup(); vi.clearAllMocks(); });
describe('UI-01/UI-03 presentation bridge (not native acceptance)', () => {
  it('shows the retained native cause and a pitch suggestion without changing settings', async () => {
    const initial: Desktop.State = { desktop_version: 1, session_id: 'session-failure', revision: 1, object: null, draft_settings: structuredClone(defaultSettings), result: null, operation: null, last_error: { code: 'RESOURCE_LIMIT', message: 'Search stopped; the valid result was retained.', recoverable: true, details: { failure: { phase: 'spectral_preflight', cause_code: 'GRID_CELL_LIMIT', resource: { name: 'grid_cells', required: '40611648', limit: '16777216' }, suggested_pitch_mm: 5 } } } };
    native.state.mockResolvedValue(initial);
    render(<App />);
    await waitFor(() => expect(screen.getByRole('alert').textContent).toContain('GRID_CELL_LIMIT'));
    expect(screen.getByRole('alert').textContent).toContain('40611648');
    expect(screen.getByRole('alert').textContent).toContain('Try a voxel pitch of 5 mm');
    expect((screen.getByLabelText('Voxel pitch') as HTMLInputElement).value).toBe(String(defaultSettings.pitch_mm));
    expect(screen.getByText('Diagnostic details')).toBeDefined();
  });
  it('saves edited pending settings while old result viewer keeps original box/preview', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    document.container = { kind: 'box', dimensions_mm: [10,20,30] };
    const preview = { preview_id: 'result-mesh', format: 'ply' as const, sha256: 'a'.repeat(64), byte_length: 10, triangle_count: 12, coordinate_frame: 'object_local_mm' as const };
    const initial: Desktop.State = { desktop_version: 1, session_id: 'session-ui', revision: 1, object: { id: 'object-current', display_name: 'Object.stl', report: document.assets.object, preview: { ...preview, preview_id: 'new-import-mesh' } }, draft_settings: structuredClone(defaultSettings), result: { id: 'result-previous', document, preview, origin: 'solve' }, operation: null, last_error: null };
    native.state.mockResolvedValue(initial); native.save.mockResolvedValue({ operation_id: 'save-test' });
    render(<App />);
    await waitFor(() => expect(screen.getByRole('button', { name: 'Save project' }).hasAttribute('disabled')).toBe(false));
    fireEvent.change(screen.getByLabelText('Width'), { target: { value: '125' } });
    fireEvent.change(screen.getByLabelText('Between copies'), { target: { value: '2.5' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save project' }));
    await waitFor(() => expect(native.save).toHaveBeenCalledOnce());
    expect(native.save.mock.calls[0][0].settings.box_dimensions_mm).toEqual([125,100,100]);
    expect(native.save.mock.calls[0][0].settings.clearance_mm).toEqual({ pair: 2.5, wall: 1 });
    expect(screen.getByTestId('viewer').textContent).toBe('10,20,30 / result-mesh');
    expect(document.container.dimensions_mm).toEqual([10,20,30]);
  });
});
